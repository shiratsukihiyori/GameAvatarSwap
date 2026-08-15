/*
 * loader.cpp - inject hook DLL into target process.
 *
 * Preferred:  WH_CBT SetWindowsHookEx -- the system loads the hook DLL
 *             into the target process when CBT events (window create /
 *             activate) fire there.
 * Fallback:   CreateRemoteThread + LoadLibrary, after a 30s timeout.
 *
 * usage: loader.exe <hook.dll> <target.exe>
 */
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

typedef void (*SetTargetFn)(const char*);
typedef LRESULT (CALLBACK *HookProcFn)(int, WPARAM, LPARAM);

static DWORD find_pid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool is_dll_loaded(DWORD pid, const std::wstring& dllName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName.c_str()) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool inject_remotethread(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { printf("[!] OpenProcess failed: %lu\n", GetLastError()); return false; }
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { printf("[!] VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(proc); return false; }
    if (!WriteProcessMemory(proc, mem, dllPath.c_str(), bytes, nullptr)) {
        printf("[!] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryW");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)loadlib, mem, 0, nullptr);
    bool ok = false;
    if (!thread) {
        printf("[!] CreateRemoteThread failed: %lu (anti-cheat may block)\n", GetLastError());
    } else {
        WaitForSingleObject(thread, 10000);
        DWORD exitCode = 0;
        GetExitCodeThread(thread, &exitCode);
        ok = (exitCode != 0);
        printf(ok ? "[+] remote LoadLibrary returned %p\n" : "[!] remote LoadLibrary returned NULL\n",
               (void*)(uintptr_t)exitCode);
        CloseHandle(thread);
    }
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        printf("usage: loader.exe <hook.dll> <target.exe>\n");
        return 1;
    }
    std::wstring dllPath = argv[1];
    std::wstring target = argv[2];

    // Let every future copy of the DLL (shared section) know the target name.
    // Also prevents the DLL from initializing hooks inside loader.exe itself.
    SetEnvironmentVariableW(L"GAS_TARGET", target.c_str());

    printf("[*] Target: %ls\n", target.c_str());
    printf("[*] DLL: %ls\n", dllPath.c_str());

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) { printf("[!] LoadLibrary(self) failed: %lu\n", GetLastError()); return 1; }

    auto set_target = (SetTargetFn)GetProcAddress(mod, "set_target");
    auto hook_proc = (HookProcFn)GetProcAddress(mod, "hook_proc");
    if (!set_target || !hook_proc) { printf("[!] exports missing\n"); return 1; }

    char targetA[64] = {0};
    WideCharToMultiByte(CP_UTF8, 0, target.c_str(), -1, targetA, 63, nullptr, nullptr);
    set_target(targetA);
    printf("[*] set_target(%s)\n", targetA);

    std::wstring dllName = dllPath.substr(dllPath.find_last_of(L"\\/") + 1);

    HHOOK hook = SetWindowsHookExW(WH_CBT, hook_proc, mod, 0);
    if (!hook) {
        printf("[!] SetWindowsHookEx failed: %lu -> trying CreateRemoteThread\n", GetLastError());
        DWORD pid = find_pid(target.c_str());
        if (!pid) { printf("[!] target process not found\n"); return 1; }
        printf("[*] target PID: %lu\n", pid);
        bool ok = inject_remotethread(pid, dllPath);
        printf(ok ? "[+] injected via remote thread\n" : "[!] injection failed\n");
        return ok ? 0 : 1;
    }

    printf("[+] SetWindowsHookEx OK (WH_CBT). Waiting for target: %ls\n", target.c_str());
    DWORD pid = find_pid(target.c_str());
    if (pid && is_dll_loaded(pid, dllName))
        printf("[*] DLL already loaded in target (PID %lu)\n", pid);

    // Pump messages so the CBT hook works; watch for the DLL appearing in
    // the target process. Fall back to a remote thread after 30 seconds.
    DWORD start = GetTickCount();
    bool loaded = false;
    MSG msg;
    while (GetTickCount() - start < 30000) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        pid = find_pid(target.c_str());
        if (pid && is_dll_loaded(pid, dllName)) { loaded = true; break; }
        Sleep(200);
    }

    if (loaded) {
        printf("[+] DLL loaded into target (PID %lu)\n", pid);
        printf("[*] keep this window open to hold the hook; closing it unloads the DLL\n");
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UnhookWindowsHookEx(hook);
        return 0;
    }

    printf("[!] 30s timeout without CBT injection -> trying CreateRemoteThread\n");
    pid = find_pid(target.c_str());
    UnhookWindowsHookEx(hook);
    if (!pid) { printf("[!] target process not found\n"); return 1; }
    printf("[*] target PID: %lu\n", pid);
    bool ok = inject_remotethread(pid, dllPath);
    printf(ok ? "[+] injected via remote thread\n" : "[!] injection failed\n");
    return ok ? 0 : 1;
}
