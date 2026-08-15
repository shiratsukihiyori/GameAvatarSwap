#!/usr/bin/env bash
# 构建 AvatarHook.dll / loader.exe（需要 MinGW-w64 gcc/g++）
set -e
src="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$src")"
mh="$root/third_party/minhook"
stb="$root/third_party/stb"
out="$root/build"
obj="$out/obj"
mkdir -p "$obj"

echo "[1/3] 编译 MinHook 静态库..."
for f in buffer.c hook.c trampoline.c hde/hde64.c hde/hde32.c; do
  gcc -O2 -masm=intel -std=c11 -c "$mh/src/$f" \
      -I "$mh/include" -I "$mh/src" -o "$obj/$(basename "$f" .c).o"
done
ar rcs "$out/libminhook.a" "$obj"/*.o

echo "[2/3] 编译 AvatarHook.dll..."
g++ -shared -O2 -static -o "$out/AvatarHook.dll" "$src/hook_dll.cpp" \
    -I "$mh/include" -I "$stb" -L "$out" -lminhook \
    -ld3d11 -ldxgi -lole32 -luuid -static-libgcc -static-libstdc++

echo "[3/3] 编译 loader.exe..."
g++ -O2 -static -municode -o "$out/loader.exe" "$src/loader.cpp" \
    -static-libgcc -static-libstdc++

echo "完成:"
ls -l "$out"