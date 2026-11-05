#!/bin/sh
# Builds the hub as a static musl binary and assembles the LPK content dir.
# Client installers are not bundled; the layout page links to GitHub Releases.
set -eu
cd "$(dirname "$0")"
if [ -z "${CC_x86_64_unknown_linux_musl:-}" ] && ! command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
  if command -v musl-gcc >/dev/null 2>&1; then
    export CC_x86_64_unknown_linux_musl=musl-gcc
  elif command -v zig >/dev/null 2>&1; then
    export CC_x86_64_unknown_linux_musl="$PWD/zig-musl-cc.sh"
  else
    echo "A musl C compiler or Zig is required to build device cryptography." >&2
    exit 1
  fi
fi
(cd hub && cargo build --release --locked --target x86_64-unknown-linux-musl)
rm -rf content
mkdir -p content/web dist
cp hub/target/x86_64-unknown-linux-musl/release/lzc-mesh-hub content/lzc-mesh-hub
cp web/*.html web/*.css web/*.js content/web/
cp ../LICENSE content/LICENSE
