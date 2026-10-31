#!/bin/sh
# Cross-builds 懒猫设备接力 (LZC Mesh) for Windows x86_64 with zig's mingw toolchain (static, no runtime DLLs):
#   ../dist/lzc-mesh-windows-setup.exe  installer (per user, no admin; also the uninstaller)
#   ../dist/lzc-mesh-windows.zip        portable archive: exe + install.ps1
set -eu
cd "$(dirname "$0")"
mkdir -p build ../dist
CXX="zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -Wl,--subsystem,windows"
zig rc /fo build/lzc-mesh.res lzc-mesh.rc
$CXX src/*.cpp build/lzc-mesh.res \
  -lwinhttp -luser32 -lgdi32 -lshell32 -lole32 -lcomctl32 -ladvapi32 -lshcore -lshlwapi -lcomdlg32 \
  -o "build/LZC Mesh.exe"
# The setup embeds the client exe built above as a resource.
zig rc /fo build/setup.res setup/setup.rc
$CXX setup/setup.cpp build/setup.res -luser32 -lshell32 -lole32 -luuid -ladvapi32 -o build/lzc-mesh-windows-setup.exe
cp build/lzc-mesh-windows-setup.exe ../dist/lzc-mesh-windows-setup.exe
rm -rf build/pkg && mkdir -p "build/pkg/LZC Mesh"
cp "build/LZC Mesh.exe" install.ps1 "build/pkg/LZC Mesh/"
rm -f ../dist/lzc-mesh-windows.zip
(cd build/pkg && bsdtar -a -cf ../../../dist/lzc-mesh-windows.zip "LZC Mesh")
echo built
