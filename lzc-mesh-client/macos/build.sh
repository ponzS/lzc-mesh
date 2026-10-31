#!/bin/sh
# Builds "LZC Mesh.app" (universal arm64 + x86_64) and dist/lzc-mesh-macos.zip.
# Signs with the first available "Apple Development" identity so macOS keeps the
# Accessibility / Input Monitoring grants across rebuilds; falls back to ad-hoc.
set -eu
cd "$(dirname "$0")"
rm -rf build && mkdir -p build
for arch in arm64 x86_64; do
  swiftc -O -target $arch-apple-macos12 -o build/lzc-mesh-$arch Sources/*.swift
done
APP="build/LZC Mesh.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
lipo -create build/lzc-mesh-arm64 build/lzc-mesh-x86_64 -output "$APP/Contents/MacOS/LZC Mesh"
cp Info.plist "$APP/Contents/Info.plist"
[ -f AppIcon.icns ] && cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
IDENTITY=$(security find-identity -v -p codesigning | awk -F'"' '/Apple Development|Developer ID Application/ {print $2; exit}')
codesign --force --deep --sign "${IDENTITY:--}" "$APP"
echo "signed with: ${IDENTITY:-ad-hoc}"
mkdir -p ../dist
(cd build && ditto -c -k --keepParent "LZC Mesh.app" ../../dist/lzc-mesh-macos.zip)
