#!/bin/sh
# Cross-builds 懒猫设备接力 (LZC Mesh) for Windows x86_64 with zig's mingw toolchain (static, no runtime DLLs):
#   ../dist/lzc-mesh-windows-setup.exe  installer (per user, no admin; also the uninstaller)
#   ../dist/lzc-mesh-windows.zip        portable archive: exe + install.ps1
set -eu
cd "$(dirname "$0")"
mkdir -p build ../dist
# Reuse the same authenticated transport and routing as the Mac / Linux clients.
CC_x86_64_pc_windows_gnu="$PWD/zig-cc.sh" \
AR_x86_64_pc_windows_gnu="$PWD/zig-ar.sh" \
CARGO_TARGET_X86_64_PC_WINDOWS_GNU_RUSTFLAGS="-C dlltool=$PWD/zig-dlltool.sh" \
  cargo build --locked --release --manifest-path ../transport/Cargo.toml --target x86_64-pc-windows-gnu
TRANSPORT="../transport/target/x86_64-pc-windows-gnu/release/liblzc_mesh_transport.a"
CXX="zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -Wl,--subsystem,windows"
zig rc /fo build/lzc-mesh.res lzc-mesh.rc
$CXX src/*.cpp build/lzc-mesh.res "$TRANSPORT" \
  -lbcrypt -lntdll -luserenv -lws2_32 -ldbghelp -lcrypt32 -lbthprops \
  -lwinhttp -luser32 -lgdi32 -lshell32 -lole32 -lcomctl32 -ladvapi32 -lshcore -lshlwapi -lcomdlg32 -luxtheme \
  -o "build/LZC Mesh.exe"
# The setup embeds the client exe built above as a resource.
zig rc /fo build/setup.res setup/setup.rc
$CXX setup/setup.cpp build/setup.res -luser32 -lshell32 -lole32 -luuid -ladvapi32 -o build/lzc-mesh-windows-setup.exe
cp build/lzc-mesh-windows-setup.exe ../dist/lzc-mesh-windows-setup.exe
rm -rf build/pkg && mkdir -p "build/pkg/LZC Mesh"
cp "build/LZC Mesh.exe" install.ps1 ../../LICENSE "build/pkg/LZC Mesh/"
rm -f ../dist/lzc-mesh-windows.zip
(cd build/pkg && bsdtar -a -cf ../../../dist/lzc-mesh-windows.zip "LZC Mesh")
echo built
