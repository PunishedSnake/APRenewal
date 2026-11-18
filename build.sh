#!/bin/sh
# Cross-compile the 32-bit d3d9.dll proxy (needs g++-mingw-w64-i686).
set -e
cd "$(dirname "$0")"
mkdir -p out obj
CXX=i686-w64-mingw32-g++
FLAGS="-O2 -std=c++17 -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS"
# Dear ImGui (compiled once, reused)
for f in imgui imgui_draw imgui_tables imgui_widgets imgui_impl_dx9; do
  [ obj/$f.o -nt src/imgui/$f.cpp ] || $CXX $FLAGS -c src/imgui/$f.cpp -o obj/$f.o
done
$CXX $FLAGS -shared -static -static-libgcc -static-libstdc++ \
    -Wall -Wno-unused-function \
    -o out/d3d9.dll src/main.cpp obj/*.o src/d3d9.def \
    -Wl,--kill-at -Wl,--enable-stdcall-fixup -lkernel32 -luser32 -lgdi32 -limm32
cp APRenewal.ini out/
echo "built out/d3d9.dll"
