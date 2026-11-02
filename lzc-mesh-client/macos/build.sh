#!/bin/sh
# Builds "LZC Mesh.app" (universal arm64 + x86_64) and:
#   ../dist/lzc-mesh-macos.dmg  installer: drag-to-Applications disk image
#   ../dist/lzc-mesh-macos.zip  archive: the .app, unzip and move to /Applications
# Signs with the first available "Apple Development" identity so macOS keeps the
# Accessibility / Input Monitoring grants across rebuilds; falls back to ad-hoc.
set -eu
cd "$(dirname "$0")"
rm -rf build && mkdir -p build
export MACOSX_DEPLOYMENT_TARGET=12.0
CARGO="${CARGO:-$HOME/.cargo/bin/cargo}"
for arch in arm64 x86_64; do
  if [ "$arch" = arm64 ]; then rust_arch=aarch64; else rust_arch=x86_64; fi
  "$CARGO" build --manifest-path ../transport/Cargo.toml --release --locked --target "$rust_arch-apple-darwin"
  swiftc -O -target $arch-apple-macos12 -framework IOBluetooth -o build/bluetooth-$arch BluetoothHelper/*.swift
  swiftc -O -target $arch-apple-macos12 -import-objc-header ../transport/include.h \
    -o build/lzc-mesh-$arch Sources/*.swift \
    ../transport/target/$rust_arch-apple-darwin/release/liblzc_mesh_transport.a \
    -framework Security -framework SystemConfiguration -lresolv
 done
APP="build/LZC Mesh.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
lipo -create build/lzc-mesh-arm64 build/lzc-mesh-x86_64 -output "$APP/Contents/MacOS/LZC Mesh"
HELPER="$APP/Contents/Helpers/LZC Mesh Bluetooth.app"
mkdir -p "$HELPER/Contents/MacOS"
lipo -create build/bluetooth-arm64 build/bluetooth-x86_64 -output "$HELPER/Contents/MacOS/LZC Mesh Bluetooth"
cp BluetoothHelper/Info.plist "$HELPER/Contents/Info.plist"
cp Info.plist "$APP/Contents/Info.plist"
[ -f AppIcon.icns ] && cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
IDENTITY=$(security find-identity -v -p codesigning | awk -F'"' '/Apple Development|Developer ID Application/ {print $2; exit}')
codesign --force --sign "${IDENTITY:--}" "$HELPER"
codesign --force --deep --sign "${IDENTITY:--}" "$APP"
echo "signed with: ${IDENTITY:-ad-hoc}"
mkdir -p ../dist
rm -rf build/dmg ../dist/lzc-mesh-macos.dmg
mkdir -p build/dmg
ditto "$APP" "build/dmg/LZC Mesh.app"
ln -s /Applications build/dmg/Applications
hdiutil create -quiet -volname "懒猫设备接力" -srcfolder build/dmg -fs HFS+ -format UDZO -ov ../dist/lzc-mesh-macos.dmg
codesign --force --sign "${IDENTITY:--}" ../dist/lzc-mesh-macos.dmg
hdiutil verify -quiet ../dist/lzc-mesh-macos.dmg
rm -f ../dist/lzc-mesh-macos.zip
(cd build && ditto -c -k --keepParent "LZC Mesh.app" ../../dist/lzc-mesh-macos.zip)
