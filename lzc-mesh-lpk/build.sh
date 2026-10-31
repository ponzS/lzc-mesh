#!/bin/sh
# Builds the hub as a static musl binary and assembles the LPK content dir.
# Client archives are copied from ../lzc-mesh-client/dist when present so the
# layout page can offer them for download.
set -eu
cd "$(dirname "$0")"
(cd hub && cargo build --release --target x86_64-unknown-linux-musl)
rm -rf content
mkdir -p content/web/downloads dist
cp hub/target/x86_64-unknown-linux-musl/release/lzc-mesh-hub content/lzc-mesh-hub
cp web/index.html web/style.css web/app.js content/web/
for f in ../lzc-mesh-client/dist/*; do
  [ -f "$f" ] && cp "$f" content/web/downloads/
done
true
