#include <windows.h>
#include <d3d11.h>
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__GNUC__)
wchar_t target_process_name[64] __attribute__((section(".shared"), shared)) = {0};
int g_state __attribute__((section(".shared"), shared)) = 0;
#else
#pragma data_seg(".shared")
wchar_t target_process_name[64] = {0};
int g_state = 0;
#pragma data_seg()
#pragma comment(linker, "/SECTION:.shared,RWS")
#endif

// All runtime files (config, images, logs) live next to the DLL.
static std::string g_base_dir;
static std::string g_cfg_path, g_marker_path, g_log_path, g_attach_path;
static std::string g_dump_orig512, g_dump_orig128, g_dump_new512, g_dump_new128;

static void init_paths() {
    if (!g_base_dir.empty()) return;
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&init_paths, &hm);
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(hm, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) lstrcpynA(buf, ".\\", MAX_PATH);
    char* slash = strrchr(buf, '\\');
    if (slash) *slash = '\0';
    g_base_dir = std::string(buf) + "\\";
    g_cfg_path = g_base_dir + "hook_config.txt";
    g_marker_path = g_base_dir + "hook_loaded.txt";
    g_log_path = g_base_dir + "hook_sizes.log";
    g_attach_path = g_base_dir + "hook_attach.log";
    g_dump_orig512 = g_base_dir + "hook_orig_512.raw";
    g_dump_orig128 = g_base_dir + "hook_orig_128.raw";
    g_dump_new512 = g_base_dir + "hook_new_512.raw";
    g_dump_new128 = g_base_dir + "hook_new_128.raw";
}

static bool g_hooked = false;
static bool g_flip = false;
static bool g_replace_enabled = true;
static bool g_rep_512 = true;
static bool g_rep_256 = false;
static bool g_rep_128 = true;
static bool g_copylog = false;
static bool g_rtinject = false;
static bool g_srvinject = false;
static bool g_fulllog = false;
static bool g_csinject = false;
static volatile LONG g_tex_version = 0;
static SRWLOCK g_tex_lock = SRWLOCK_INIT;
static SRWLOCK g_rt_lock = SRWLOCK_INIT;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;

struct RtCandidate { void* ptr; LONGLONG tick; };
static RtCandidate g_candidates[128];
static volatile LONG g_dump_done = 0;
static volatile LONG g_dump_new_done = 0;
static std::unordered_map<UINT, std::vector<uint8_t>> g_textures;
static HRESULT(WINAPI* g_orig_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
static void (WINAPI* g_orig_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT) = nullptr;
static HRESULT(WINAPI* g_orig_CopyResource)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
static void (WINAPI* g_orig_CSR)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
static void (WINAPI* g_orig_PSR)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
static void (WINAPI* g_orig_OMRT)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static void (WINAPI* g_orig_OMRTUAV)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
static void (WINAPI* g_orig_CSSetSRV)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
static void (WINAPI* g_orig_CSSetUAV)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
static void (WINAPI* g_orig_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
static void* g_cs_ptr = nullptr;
static volatile LONGLONG g_cs_tick = 0;

struct InjSlot { ID3D11Device* dev; ID3D11Texture2D* tex; UINT version; DXGI_FORMAT fmt; };
static InjSlot g_inj[3];
static int inj_slot(UINT w) { return w == 512 ? 0 : (w == 256 ? 1 : (w == 128 ? 2 : -1)); }

static volatile LONG g_replace_count_512 = 0;
static volatile LONG g_replace_count_256 = 0;
static volatile LONG g_replace_count_128 = 0;
static volatile LONG g_other_square_reads = 0;
static volatile LONGLONG g_last_orig_512 = 0;
static volatile LONGLONG g_last_orig_128 = 0;
static volatile LONGLONG g_last_new_512 = 0;
static volatile LONGLONG g_last_new_128 = 0;

static void dump_staging(const char* path, const D3D11_MAPPED_SUBRESOURCE* m,
                         const D3D11_TEXTURE2D_DESC& desc, volatile LONGLONG* lastp) {
    LONGLONG now = GetTickCount64();
    if (now - *lastp <= 2000) return;
    *lastp = now;
    FILE* df = fopen(path, "wb");
    if (!df) return;
    for (UINT yy = 0; yy < desc.Height; yy++)
        fwrite((const char*)m->pData + (size_t)yy * m->RowPitch, 1, desc.Width * 4, df);
    fclose(df);
}

static FILETIME g_cfg_mtime = {0, 0};
static DWORD g_last_cfg_check = 0;

static void load_config_and_textures();
static void write_marker(const char* text);

static void check_config_reload() {
    DWORD now = GetTickCount();
    if (now - g_last_cfg_check < 2000) return;
    g_last_cfg_check = now;
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExA(g_cfg_path.c_str(), GetFileExInfoStandard, &fd)) return;
    if (fd.ftLastWriteTime.dwLowDateTime == g_cfg_mtime.dwLowDateTime &&
        fd.ftLastWriteTime.dwHighDateTime == g_cfg_mtime.dwHighDateTime) return;
    g_cfg_mtime = fd.ftLastWriteTime;
    AcquireSRWLockExclusive(&g_tex_lock);
    load_config_and_textures();
    ReleaseSRWLockExclusive(&g_tex_lock);
    char buf[256];
    snprintf(buf, sizeof(buf), "RELOADED textures=%zu flip=%d rep512=%d rep256=%d rep128=%d",
             g_textures.size(), g_flip ? 1 : 0, g_rep_512 ? 1 : 0, g_rep_256 ? 1 : 0, g_rep_128 ? 1 : 0);
    write_marker(buf);
}

static bool is_target_process() {
    // The target game's name is intentionally not hardcoded here.
    // You know which one it is.
    if (!target_process_name[0]) return true;  // no restriction configured
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    wchar_t* base = wcsrchr(path, L'\\');
    if (!base) return false;
    return _wcsicmp(base + 1, target_process_name) == 0;
}

static void write_marker(const char* text) {
    FILE* f = fopen(g_marker_path.c_str(), "w");
    if (f) { fprintf(f, "%s\n", text); fclose(f); }
}

static void load_config_and_textures() {
    g_textures.clear();
    FILE* cfg = fopen(g_cfg_path.c_str(), "r");
    if (!cfg) { write_marker("no_config"); return; }
    char line[1024];
    while (fgets(line, sizeof(line), cfg)) {
        char key[64] = {0}, val[960] = {0};
        if (sscanf(line, " %63[^=]=%959s", key, val) != 2) continue;
        if (_stricmp(key, "FLIP") == 0) { g_flip = (atoi(val) != 0); continue; }
        if (_stricmp(key, "REPLACE") == 0) { g_replace_enabled = (atoi(val) != 0); continue; }
        if (_stricmp(key, "REPLACE_512") == 0) { g_rep_512 = (atoi(val) != 0); continue; }
        if (_stricmp(key, "REPLACE_256") == 0) { g_rep_256 = (atoi(val) != 0); continue; }
        if (_stricmp(key, "REPLACE_128") == 0) { g_rep_128 = (atoi(val) != 0); continue; }
        if (_stricmp(key, "COPYLOG") == 0) { g_copylog = (atoi(val) != 0); continue; }
        if (_stricmp(key, "RTINJECT") == 0) { g_rtinject = (atoi(val) != 0); continue; }
        if (_stricmp(key, "SRVINJECT") == 0) { g_srvinject = (atoi(val) != 0); continue; }
        if (_stricmp(key, "FULLLOG") == 0) { g_fulllog = (atoi(val) != 0); continue; }
        if (_stricmp(key, "CSINJECT") == 0) { g_csinject = (atoi(val) != 0); continue; }
        if (_stricmp(key, "TARGET") == 0) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, val, -1, target_process_name, 63);
            if (wlen <= 0) target_process_name[0] = 0;
            else target_process_name[63] = 0;
            continue;
        }
        UINT size = 0;
        if (strcmp(key, "512") == 0) size = 512;
        else if (strcmp(key, "256") == 0) size = 256;
        else if (strcmp(key, "128") == 0) size = 128;
        if (!size) continue;

        int w = 0, h = 0, comp = 0;
        std::string img_path = val;
        if (img_path.find(':') == std::string::npos && img_path[0] != '\\') {
            img_path = g_base_dir + img_path;  // relative to the DLL directory
        }
        stbi_set_flip_vertically_on_load(g_flip ? 1 : 0);
        unsigned char* px = stbi_load(img_path.c_str(), &w, &h, &comp, 4);
        bool ok = false;
        if (px && w == (int)size && h == (int)size) {
            auto& buf = g_textures[size];
            buf.resize((size_t)size * size * 4);
            memcpy(buf.data(), px, (size_t)size * size * 4);
            ok = true;
        }
        stbi_image_free(px);
        if (!ok) write_marker("image_load_failed");
    }
    fclose(cfg);
    InterlockedIncrement(&g_tex_version);
}

static HRESULT WINAPI hooked_Map(ID3D11DeviceContext* self, ID3D11Resource* pResource, UINT sub,
                                 D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* pMapped) {
    check_config_reload();
    HRESULT hr = g_orig_Map(self, pResource, sub, mapType, mapFlags, pMapped);
    if (FAILED(hr)) return hr;
    if (mapType != D3D11_MAP_READ && mapType != D3D11_MAP_READ_WRITE) return hr;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(pResource->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) return hr;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    tex->Release();

    if (desc.Usage != D3D11_USAGE_STAGING) return hr;
    bool bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                 desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    bool rgba = (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                 desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                 desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    if (!bgra && !rgba) return hr;

    bool size_ok = (desc.Width == 512 && g_rep_512) || (desc.Width == 256 && g_rep_256) || (desc.Width == 128 && g_rep_128);
    int replaced = 0;
    bool is_512 = (desc.Width == 512 && desc.Height == 512);
    bool is_128 = (desc.Width == 128 && desc.Height == 128);
    if (is_512) dump_staging(g_dump_orig512.c_str(), pMapped, desc, &g_last_orig_512);
    if (is_128) dump_staging(g_dump_orig128.c_str(), pMapped, desc, &g_last_orig_128);
    AcquireSRWLockShared(&g_tex_lock);
    {
        auto it = g_textures.find(desc.Width);
        bool can_replace = g_replace_enabled && size_ok && (desc.Width == desc.Height) && (it != g_textures.end());
        if (can_replace) {
            const uint8_t* src = it->second.data();
            uint8_t* dst = (uint8_t*)pMapped->pData;
            UINT size = desc.Width;
            for (UINT y = 0; y < size; y++) {
                uint8_t* drow = dst + (size_t)y * pMapped->RowPitch;
                const uint8_t* srow = src + (size_t)y * size * 4;
                if (bgra) {
                    for (UINT x = 0; x < size; x++) {
                        drow[x*4+0] = srow[x*4+2];
                        drow[x*4+1] = srow[x*4+1];
                        drow[x*4+2] = srow[x*4+0];
                        drow[x*4+3] = srow[x*4+3];
                    }
                } else {
                    memcpy(drow, srow, (size_t)size * 4);
                }
            }
            if (size == 512) InterlockedIncrement(&g_replace_count_512);
            else if (size == 256) InterlockedIncrement(&g_replace_count_256);
            else InterlockedIncrement(&g_replace_count_128);
            replaced = 1;
        }
    }
    ReleaseSRWLockShared(&g_tex_lock);
    if (is_512 && replaced) dump_staging(g_dump_new512.c_str(), pMapped, desc, &g_last_new_512);
    if (is_128 && replaced) dump_staging(g_dump_new128.c_str(), pMapped, desc, &g_last_new_128);
    {
        static volatile LONG log_counter = 0;
        static volatile LONG small_skip = 0;
        bool quiet = (desc.Width == 256 && desc.Height == 256);
        long n = InterlockedIncrement(&log_counter);
        bool write_it = false;
        if (g_fulllog) {
            write_it = true;
        } else if (!quiet) {
            write_it = true;
        } else if (replaced) {
            write_it = true;
        } else if ((InterlockedIncrement(&small_skip) % 512) == 0) {
            write_it = true;
        }
        if (write_it) {
            FILE* sf = fopen(g_log_path.c_str(), "a");
            if (sf) {
                fprintf(sf, "[%llu] #%ld map %ux%u fmt=%u type=%u repl=%d c512=%ld c128=%ld\n",
                        (unsigned long long)GetTickCount64(), n, (unsigned)desc.Width, (unsigned)desc.Height,
                        (unsigned)desc.Format, (unsigned)mapType, replaced,
                        g_replace_count_512, g_replace_count_128);
                fclose(sf);
            }
        }
    }
    return hr;
}

static void WINAPI hooked_Unmap(ID3D11DeviceContext* self, ID3D11Resource* pResource, UINT sub) {
    g_orig_Unmap(self, pResource, sub);
}

static void overwrite_rt(ID3D11DeviceContext* self, ID3D11Resource* rt) {
    ID3D11Device* gamedev = nullptr;
    self->GetDevice(&gamedev);
    if (!gamedev) return;
    ID3D11Texture2D* stex = nullptr;
    if (FAILED(rt->QueryInterface(IID_ID3D11Texture2D, (void**)&stex))) { gamedev->Release(); return; }
    D3D11_TEXTURE2D_DESC sd;
    stex->GetDesc(&sd);
    stex->Release();
    if (sd.Width != sd.Height || sd.Usage == D3D11_USAGE_STAGING) { gamedev->Release(); return; }
    int slot = inj_slot(sd.Width);
    if (slot < 0) { gamedev->Release(); return; }
    bool rgba = (sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                 sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                 sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    bool bgra = (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (!rgba && !bgra) { gamedev->Release(); return; }
    AcquireSRWLockShared(&g_tex_lock);
    auto it = g_textures.find(sd.Width);
    const uint8_t* srcpx = (it != g_textures.end()) ? it->second.data() : nullptr;
    UINT ver = (UINT)g_tex_version;
    ReleaseSRWLockShared(&g_tex_lock);
    if (!srcpx) { gamedev->Release(); return; }
    InjSlot& s = g_inj[slot];
    if (s.tex && (s.dev != gamedev || s.version != ver || s.fmt != sd.Format)) {
        s.tex->Release(); s.tex = nullptr;
        s.dev->Release(); s.dev = nullptr;
    }
    if (!s.tex) {
        D3D11_TEXTURE2D_DESC td = sd;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        td.MiscFlags = 0;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        if (FAILED(gamedev->CreateTexture2D(&td, nullptr, &s.tex))) { gamedev->Release(); return; }
        gamedev->AddRef();
        s.dev = gamedev;
        s.version = ver;
        s.fmt = sd.Format;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(self->Map(s.tex, 0, D3D11_MAP_WRITE, 0, &m))) {
            UINT size = sd.Width;
            for (UINT y = 0; y < size; y++) {
                uint8_t* drow = (uint8_t*)m.pData + (size_t)y * m.RowPitch;
                const uint8_t* srow = srcpx + (size_t)y * size * 4;
                if (bgra) {
                    for (UINT x = 0; x < size; x++) {
                        drow[x*4+0] = srow[x*4+2];
                        drow[x*4+1] = srow[x*4+1];
                        drow[x*4+2] = srow[x*4+0];
                        drow[x*4+3] = srow[x*4+3];
                    }
                } else {
                    memcpy(drow, srow, (size_t)size * 4);
                }
            }
            self->Unmap(s.tex, 0);
        }
    }
    if (g_orig_CopyResource) g_orig_CopyResource(self, rt, s.tex);
    else self->CopyResource(rt, s.tex);
    gamedev->Release();
}

static void try_inject_rt(ID3D11DeviceContext* self, ID3D11Resource* pDst, ID3D11Resource* pSrc) {
    ID3D11Texture2D* dtex = nullptr;
    ID3D11Texture2D* stex = nullptr;
    if (FAILED(pDst->QueryInterface(IID_ID3D11Texture2D, (void**)&dtex))) return;
    if (FAILED(pSrc->QueryInterface(IID_ID3D11Texture2D, (void**)&stex))) { dtex->Release(); return; }
    D3D11_TEXTURE2D_DESC dd, sd;
    dtex->GetDesc(&dd);
    stex->GetDesc(&sd);
    dtex->Release();
    stex->Release();
    bool match = (dd.Usage == D3D11_USAGE_STAGING) && (dd.Width == dd.Height) &&
                 (dd.Width == sd.Width) && (dd.Width == 512 || dd.Width == 128) &&
                 (sd.Usage != D3D11_USAGE_STAGING);
    if (match) {
        overwrite_rt(self, pSrc);
        FILE* f = fopen(g_log_path.c_str(), "a");
        if (f) {
            fprintf(f, "[%llu] RTINJECT %ux%u fmt=%u usage=%u src=%p\n",
                    (unsigned long long)GetTickCount64(), (unsigned)sd.Width, (unsigned)sd.Height,
                    (unsigned)sd.Format, (unsigned)sd.Usage, (void*)pSrc);
            fclose(f);
        }
    }
}

static void rt_log(const char* tag, ID3D11Resource* res) {
    static struct { void* ptr; LONGLONG tick; } last[16];
    static int last_n = 0;
    LONGLONG now = GetTickCount64();
    if (!g_fulllog) {
        for (int i = 0; i < last_n; i++) {
            if (last[i].ptr == (void*)res) {
                if (now - last[i].tick < 3000) return;
                last[i].tick = now;
                break;
            }
        }
        if (last_n < 16) { last[last_n].ptr = (void*)res; last[last_n].tick = now; last_n++; }
    }
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    tex->Release();
    FILE* f = fopen(g_log_path.c_str(), "a");
    if (f) {
        fprintf(f, "[%llu] %s rt=%p %ux%u fmt=%u usage=%u\n",
                (unsigned long long)now, tag, (void*)res, (unsigned)d.Width, (unsigned)d.Height,
                (unsigned)d.Format, (unsigned)d.Usage);
        fclose(f);
    }
}

static bool rt_square_ok(const D3D11_TEXTURE2D_DESC& d) {
    if (d.Width != d.Height || d.Usage == D3D11_USAGE_STAGING) return false;
    if (g_fulllog) return d.Width == 512 || d.Width == 256 || d.Width == 128;
    return d.Width == 512 || d.Width == 128;
}

static void rt_candidate_add(void* ptr) {
    LONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_rt_lock);
    for (int i = 0; i < 128; i++) {
        if (g_candidates[i].ptr == ptr) { g_candidates[i].tick = now; break; }
        if (g_candidates[i].ptr == nullptr) { g_candidates[i].ptr = ptr; g_candidates[i].tick = now; break; }
    }
    ReleaseSRWLockExclusive(&g_rt_lock);
}

static bool rt_candidate_check(void* ptr) {
    LONGLONG now = GetTickCount64();
    bool found = false;
    AcquireSRWLockShared(&g_rt_lock);
    for (int i = 0; i < 128; i++) {
        if (g_candidates[i].ptr == ptr) {
            found = (now - g_candidates[i].tick <= 300);
            break;
        }
    }
    ReleaseSRWLockShared(&g_rt_lock);
    return found;
}

static void WINAPI hooked_OMRT(ID3D11DeviceContext* self, UINT num, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* ds) {
    if (g_orig_OMRT) g_orig_OMRT(self, num, views, ds);
    if (!g_copylog && !g_srvinject && !g_fulllog) return;
    if (!views) return;
    for (UINT i = 0; i < num; i++) {
        if (!views[i]) continue;
        ID3D11Resource* res = nullptr;
        views[i]->GetResource(&res);
        if (!res) continue;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) {
            D3D11_TEXTURE2D_DESC d;
            tex->GetDesc(&d);
            tex->Release();
            if (rt_square_ok(d)) {
                rt_log("RTV", res);
                rt_candidate_add(res);
            }
        }
        res->Release();
    }
}

static void WINAPI hooked_PSR(ID3D11DeviceContext* self, UINT start, UINT num, ID3D11ShaderResourceView* const* views) {
    if (g_orig_PSR) g_orig_PSR(self, start, num, views);
    if (!g_copylog && !g_srvinject && !g_fulllog) return;
    if (!views) return;
    for (UINT i = 0; i < num; i++) {
        if (!views[i]) continue;
        ID3D11Resource* res = nullptr;
        views[i]->GetResource(&res);
        if (!res) continue;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) {
            D3D11_TEXTURE2D_DESC d;
            tex->GetDesc(&d);
            tex->Release();
                if (rt_square_ok(d)) {
                    if (g_fulllog || rt_candidate_check(res)) {
                        rt_log("SRV", res);
                        if (g_srvinject && rt_candidate_check(res)) overwrite_rt(self, res);
                    }
            }
        }
        res->Release();
    }
}

static void WINAPI hooked_OMRTUAV(ID3D11DeviceContext* self, UINT numRtv, ID3D11RenderTargetView* const* rtv,
                                  ID3D11DepthStencilView* ds, UINT uavStart, UINT numUav,
                                  ID3D11UnorderedAccessView* const* uav, const UINT* counts) {
    if (g_orig_OMRTUAV) g_orig_OMRTUAV(self, numRtv, rtv, ds, uavStart, numUav, uav, counts);
    if (!g_copylog && !g_srvinject && !g_fulllog) return;
    if (rtv) {
        for (UINT i = 0; i < numRtv; i++) {
            if (!rtv[i]) continue;
            ID3D11Resource* res = nullptr;
            rtv[i]->GetResource(&res);
            if (!res) continue;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) {
                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d); tex->Release();
                if (rt_square_ok(d)) {
                    rt_log("RTV", res);
                    rt_candidate_add(res);
                }
            }
            res->Release();
        }
    }
    if (uav) {
        for (UINT i = 0; i < numUav; i++) {
            if (!uav[i]) continue;
            ID3D11Resource* res = nullptr;
            uav[i]->GetResource(&res);
            if (!res) continue;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) {
                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d); tex->Release();
                if (rt_square_ok(d)) {
                    if (g_fulllog || rt_candidate_check(res)) {
                        rt_log("UAV", res);
                        if (g_srvinject && rt_candidate_check(res)) overwrite_rt(self, res);
                    }
                }
            }
            res->Release();
        }
    }
}


static HRESULT WINAPI hooked_CopyResource(ID3D11DeviceContext* self, ID3D11Resource* pDst, ID3D11Resource* pSrc) {
    if (!g_orig_CopyResource) return E_FAIL;
    if (g_rtinject && pDst && pSrc) try_inject_rt(self, pDst, pSrc);
    HRESULT hr = g_orig_CopyResource(self, pDst, pSrc);
    if (g_copylog && pDst && pSrc) {
        ID3D11Texture2D* dtex = nullptr;
        ID3D11Texture2D* stex = nullptr;
        if (SUCCEEDED(pDst->QueryInterface(IID_ID3D11Texture2D, (void**)&dtex)) &&
            SUCCEEDED(pSrc->QueryInterface(IID_ID3D11Texture2D, (void**)&stex))) {
            D3D11_TEXTURE2D_DESC dd, sd;
            dtex->GetDesc(&dd);
            stex->GetDesc(&sd);
            bool sz_ok = (dd.Width == 512 || dd.Width == 128) ||
                         (g_fulllog && dd.Width == dd.Height && dd.Width >= 128 && dd.Width <= 1024);
            if (dd.Usage == D3D11_USAGE_STAGING && sz_ok) {
                FILE* f = fopen(g_log_path.c_str(), "a");
                if (f) {
                    fprintf(f, "[%llu] COPY dst %ux%u fmt=%u usage=%u <- src %ux%u fmt=%u usage=%u src=%p dst=%p\n",
                            (unsigned long long)GetTickCount64(), (unsigned)dd.Width, (unsigned)dd.Height, (unsigned)dd.Format,
                            (unsigned)dd.Usage, (unsigned)sd.Width, (unsigned)sd.Height, (unsigned)sd.Format,
                            (unsigned)sd.Usage, (void*)pSrc, (void*)pDst);
                    fclose(f);
                }
            }
            dtex->Release();
            stex->Release();
        }
    }
    return hr;
}

static void WINAPI hooked_CSR(ID3D11DeviceContext* self, ID3D11Resource* pDst, UINT dstSub, UINT dstX, UINT dstY, UINT dstZ,
                              ID3D11Resource* pSrc, UINT srcSub, const D3D11_BOX* pBox) {
    if (g_rtinject && pDst && pSrc) try_inject_rt(self, pDst, pSrc);
    if (g_orig_CSR) g_orig_CSR(self, pDst, dstSub, dstX, dstY, dstZ, pSrc, srcSub, pBox);
    if (g_copylog && pDst && pSrc) {
        ID3D11Texture2D* dtex = nullptr;
        ID3D11Texture2D* stex = nullptr;
        if (SUCCEEDED(pDst->QueryInterface(IID_ID3D11Texture2D, (void**)&dtex)) &&
            SUCCEEDED(pSrc->QueryInterface(IID_ID3D11Texture2D, (void**)&stex))) {
            D3D11_TEXTURE2D_DESC dd, sd;
            dtex->GetDesc(&dd);
            stex->GetDesc(&sd);
            bool sz_ok = (dd.Width == 512 || dd.Width == 128) ||
                         (g_fulllog && dd.Width == dd.Height && dd.Width >= 128 && dd.Width <= 1024);
            if (dd.Usage == D3D11_USAGE_STAGING && sz_ok) {
                FILE* f = fopen(g_log_path.c_str(), "a");
                if (f) {
                    fprintf(f, "[%llu] CSR dst %ux%u fmt=%u usage=%u <- src %ux%u fmt=%u usage=%u src=%p dst=%p box=%p\n",
                            (unsigned long long)GetTickCount64(), (unsigned)dd.Width, (unsigned)dd.Height, (unsigned)dd.Format,
                            (unsigned)dd.Usage, (unsigned)sd.Width, (unsigned)sd.Height, (unsigned)sd.Format,
                            (unsigned)sd.Usage, (void*)pSrc, (void*)pDst, (void*)pBox);
                    fclose(f);
                }
            }
            dtex->Release();
            stex->Release();
        }
    }
}

static void WINAPI hooked_CSSetSRV(ID3D11DeviceContext* self, UINT start, UINT num, ID3D11ShaderResourceView* const* views) {
    if ((g_csinject || g_fulllog) && views) {
        for (UINT i = 0; i < num; i++) {
            if (!views[i]) continue;
            ID3D11Resource* res = nullptr;
            views[i]->GetResource(&res);
            if (!res) continue;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) {
                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d); tex->Release();
                if (d.Width == d.Height && (d.Width == 512 || d.Width == 128) && d.Usage != D3D11_USAGE_STAGING &&
                    (g_fulllog || rt_candidate_check(res))) {
                    rt_log("CSRV", res);
                    if (g_csinject && rt_candidate_check(res)) overwrite_rt(self, res);
                    g_cs_ptr = res;
                    g_cs_tick = GetTickCount64();
                }
            }
            res->Release();
        }
    }
    if (g_orig_CSSetSRV) g_orig_CSSetSRV(self, start, num, views);
}

static void WINAPI hooked_CSSetUAV(ID3D11DeviceContext* self, UINT start, UINT num, ID3D11UnorderedAccessView* const* uav, const UINT* counts) {
    if ((g_csinject || g_fulllog) && uav) {
        for (UINT i = 0; i < num; i++) {
            if (!uav[i]) continue;
            ID3D11Resource* res = nullptr;
            uav[i]->GetResource(&res);
            if (!res) continue;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) {
                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d); tex->Release();
                if (d.Width == d.Height && (d.Width == 512 || d.Width == 128) && d.Usage != D3D11_USAGE_STAGING &&
                    (g_fulllog || rt_candidate_check(res))) {
                    rt_log("CUAV", res);
                    if (g_csinject && rt_candidate_check(res)) overwrite_rt(self, res);
                    g_cs_ptr = res;
                    g_cs_tick = GetTickCount64();
                }
            }
            res->Release();
        }
    }
    if (g_orig_CSSetUAV) g_orig_CSSetUAV(self, start, num, uav, counts);
}

static void WINAPI hooked_Dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    if (g_fulllog && g_cs_ptr && (GetTickCount64() - g_cs_tick) <= 100) {
        FILE* f = fopen(g_log_path.c_str(), "a");
        if (f) {
            fprintf(f, "[%llu] DISPATCH x=%u y=%u z=%u cs=rt=%p\n",
                    (unsigned long long)GetTickCount64(), (unsigned)x, (unsigned)y, (unsigned)z, g_cs_ptr);
            fclose(f);
        }
        g_cs_ptr = nullptr;
    }
    if (g_orig_Dispatch) g_orig_Dispatch(self, x, y, z);
}

static DWORD WINAPI init_thread(LPVOID) {
    if (!is_target_process()) return 0;  // re-check in case set_target happened after DllMain
    load_config_and_textures();
    char buf[512];
    snprintf(buf, sizeof(buf), "textures=%zu flip=%d", g_textures.size(), g_flip ? 1 : 0);
    write_marker(buf);

    if (g_textures.empty()) return 0;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) || !dev || !ctx) { write_marker("d3d11create_failed"); return 0; }
    g_dev = dev;
    g_ctx = ctx;
    void** vtable = *(void***)ctx;

    if (MH_Initialize() != MH_OK) { write_marker("mh_init_failed"); return 0; }
    if (MH_CreateHook(vtable[14], (void*)hooked_Map, (void**)&g_orig_Map) != MH_OK) {
        write_marker("mh_create_failed"); return 0;
    }
    if (MH_CreateHook(vtable[15], (void*)hooked_Unmap, (void**)&g_orig_Unmap) != MH_OK) {
        write_marker("mh_unmap_create_failed"); return 0;
    }
    if (MH_CreateHook(vtable[47], (void*)hooked_CopyResource, (void**)&g_orig_CopyResource) != MH_OK) {
        write_marker("mh_copy_create_failed"); return 0;
    }
    if (MH_CreateHook(vtable[46], (void*)hooked_CSR, (void**)&g_orig_CSR) != MH_OK) {
        write_marker("mh_csr_create_failed"); return 0;
    }
    if (MH_CreateHook(vtable[8], (void*)hooked_PSR, (void**)&g_orig_PSR) != MH_OK) {
        write_marker("mh_psr_create_failed"); return 0;
    }
    if (MH_CreateHook(vtable[33], (void*)hooked_OMRT, (void**)&g_orig_OMRT) != MH_OK) {
        write_marker("mh_omrt_create_failed"); return 0;
    }
    if (MH_CreateHook(vtable[34], (void*)hooked_OMRTUAV, (void**)&g_orig_OMRTUAV) != MH_OK) {
        write_marker("mh_omrtuav_create_failed"); return 0;
    }
    if (g_csinject || g_fulllog) {
        if (MH_CreateHook(vtable[67], (void*)hooked_CSSetSRV, (void**)&g_orig_CSSetSRV) != MH_OK) {
            write_marker("mh_cssrv_create_failed"); return 0;
        }
        if (MH_CreateHook(vtable[68], (void*)hooked_CSSetUAV, (void**)&g_orig_CSSetUAV) != MH_OK) {
            write_marker("mh_csuav_create_failed"); return 0;
        }
        if (MH_CreateHook(vtable[41], (void*)hooked_Dispatch, (void**)&g_orig_Dispatch) != MH_OK) {
            write_marker("mh_dispatch_create_failed"); return 0;
        }
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { write_marker("mh_enable_failed"); return 0; }

    g_hooked = true;
    char m2[128];
    snprintf(m2, sizeof(m2), "HOOKED fulllog=%d csinject=%d", g_fulllog ? 1 : 0, g_csinject ? 1 : 0);
    write_marker(m2);
    return 0;
}

static void unhook() {
    if (g_hooked) {
        MH_DisableHook(MH_ALL_HOOKS);
        Sleep(150);
        MH_Uninitialize();
        g_hooked = false;
    }
    for (int i = 0; i < 3; i++) {
        if (g_inj[i].tex) { g_inj[i].tex->Release(); g_inj[i].tex = nullptr; }
        if (g_inj[i].dev) { g_inj[i].dev->Release(); g_inj[i].dev = nullptr; }
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "UNHOOKED counts512=%ld counts256=%ld counts128=%ld other=%ld",
             g_replace_count_512, g_replace_count_256, g_replace_count_128, g_other_square_reads);
    write_marker(buf);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        init_paths();
        // loader.exe sets GAS_TARGET before loading this DLL so every copy
        // (shared section) knows the target name; keeps loader.exe itself clean.
        if (!target_process_name[0]) {
            char env[64] = {0};
            DWORD n = GetEnvironmentVariableA("GAS_TARGET", env, 63);
            if (n > 0 && n < 63) MultiByteToWideChar(CP_UTF8, 0, env, -1, target_process_name, 64);
            target_process_name[63] = 0;
        }
        FILE* _dbg = fopen(g_attach_path.c_str(), "a");
        if (_dbg) {
            wchar_t _p[MAX_PATH];
            GetModuleFileNameW(nullptr, _p, MAX_PATH);
            wchar_t* _b = wcsrchr(_p, L'\\');
            fprintf(_dbg, "attach pid=%lu state=%d name=%ls\n", GetCurrentProcessId(), g_state, _b ? _b + 1 : L"?");
            fclose(_dbg);
        }
        if (g_state == 0) g_state = 1;
        if (is_target_process()) {
            DisableThreadLibraryCalls(h);
            HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
        return TRUE;
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) unhook();
        g_state = 1;
    }
    return TRUE;
}

extern "C" {
__declspec(dllexport) void set_target(const char* name) {
    target_process_name[0] = 0;
    if (name) MultiByteToWideChar(CP_UTF8, 0, name, -1, target_process_name, 63);
    target_process_name[63] = 0;
}
__declspec(dllexport) LRESULT CALLBACK hook_proc(int code, WPARAM w, LPARAM l) {
    return CallNextHookEx(nullptr, code, w, l);
}
}
