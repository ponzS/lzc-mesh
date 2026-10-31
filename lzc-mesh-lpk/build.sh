#!/bin/sh
# Builds the hub as a static musl binary and assembles the LPK content dir.
# Client installers are not bundled; the layout page links to GitHub Releases.
set -eu
cd "$(dirname "$0")"
(cd hub && cargo build --release --target x86_64-unknown-linux-musl)
rm -rf content
mkdir -p content/web dist
cp hub/target/x86_64-unknown-linux-musl/release/lzc-mesh-hub content/lzc-mesh-hub
cp web/index.html web/style.css web/app.js content/web/
