# 构建 AvatarHook.dll / loader.exe
# 需要: MSYS2 MinGW-w64 (gcc/g++/ar) 或等效环境
$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $src
$mh   = Join-Path $root "third_party\minhook"
$stb  = Join-Path $root "third_party\stb"
$out  = Join-Path $root "build"
$obj  = Join-Path $out "obj"
New-Item -ItemType Directory -Force -Path $obj | Out-Null

Write-Host "[1/3] 编译 MinHook 静态库..."
$mhsrc = @("buffer.c", "hook.c", "trampoline.c", "hde\hde64.c", "hde\hde32.c")
foreach ($f in $mhsrc) {
    $base = Split-Path -Leaf $f
    gcc -O2 -masm=intel -std=c11 -c (Join-Path $mh "src\$f") `
        -I (Join-Path $mh "include") -I (Join-Path $mh "src") -o (Join-Path $obj "$base.o")
    if ($LASTEXITCODE -ne 0) { throw "MinHook 编译失败: $f" }
}
$objs = (Get-ChildItem $obj -Filter "*.o" | ForEach-Object { $_.FullName })
ar rcs (Join-Path $out "libminhook.a") $objs
if ($LASTEXITCODE -ne 0) { throw "ar 打包失败" }

Write-Host "[2/3] 编译 AvatarHook.dll..."
g++ -shared -O2 -static -o (Join-Path $out "AvatarHook.dll") (Join-Path $src "hook_dll.cpp") `
    -I (Join-Path $mh "include") -I $stb -L $out -lminhook `
    -ld3d11 -ldxgi -lole32 -luuid -static-libgcc -static-libstdc++
if ($LASTEXITCODE -ne 0) { throw "AvatarHook.dll 编译失败" }

Write-Host "[3/3] 编译 loader.exe..."
g++ -O2 -static -municode -o (Join-Path $out "loader.exe") (Join-Path $src "loader.cpp") `
    -static-libgcc -static-libstdc++
if ($LASTEXITCODE -ne 0) { throw "loader.exe 编译失败" }

Write-Host "完成:"
Get-ChildItem $out -File | Select-Object Name, Length | Format-Table -AutoSize
