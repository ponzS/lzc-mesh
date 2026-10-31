#!/bin/sh
# Installs LZC Mesh for the current user (no root needed) and enables autostart.
set -eu
cd "$(dirname "$0")"
bin="${XDG_BIN_HOME:-$HOME/.local/bin}"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
autostart="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
mkdir -p "$bin" "$apps" "$icons" "$autostart"
install -m 0755 lzc-mesh "$bin/lzc-mesh"
install -m 0644 lzc-mesh.png "$icons/lzc-mesh.png"
sed "s#^Exec=.*#Exec=$bin/lzc-mesh#" lzc-mesh.desktop > "$apps/lzc-mesh.desktop"
cp "$apps/lzc-mesh.desktop" "$autostart/lzc-mesh.desktop"
echo "LZC Mesh installed to $bin/lzc-mesh"
