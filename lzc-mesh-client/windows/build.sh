#!/bin/sh
# Cross-builds LZC Mesh.exe (x86_64, static, no runtime DLLs) with zig's mingw toolchain.
set -eu
cd "$(dirname "$0")"
mkdir -p build ../dist
zig rc /fo build/lzc-mesh.res lzc-mesh.rc
zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -DUNICODE -D_UNICODE \
  -Wl,--subsystem,windows src/*.cpp build/lzc-mesh.res \
  -lwinhttp -luser32 -lgdi32 -lshell32 -lole32 -lcomctl32 -ladvapi32 -lshcore \
  -o "build/LZC Mesh.exe"
rm -rf build/pkg && mkdir -p "build/pkg/LZC Mesh"
cp "build/LZC Mesh.exe" install.ps1 "build/pkg/LZC Mesh/"
rm -f ../dist/lzc-mesh-windows.zip && (cd build/pkg && bsdtar -a -cf ../../../dist/lzc-mesh-windows.zip "LZC Mesh")
echo built
