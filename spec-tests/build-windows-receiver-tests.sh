#!/bin/sh
# Build no-desktop adapter integration test (run explicitly on Windows).
# Requires the Windows transport library from windows/build.sh.
set -eu
cd "$(dirname "$0")/.."
transport=lzc-mesh-client/transport/target/x86_64-pc-windows-gnu/release/liblzc_mesh_transport.a
if [ ! -f "$transport" ]; then
    echo 'Build lzc-mesh-client/windows/build.sh first' >&2
    exit 1
fi
mkdir -p spec-tests/.run-ac
zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -DUNICODE -D_UNICODE \
  spec-tests/windows-direct-receiver.cpp lzc-mesh-client/windows/src/direct.cpp \
  lzc-mesh-client/windows/src/receiver.cpp lzc-mesh-client/windows/src/json.cpp "$transport" \
  -luser32 -lbcrypt -lntdll -luserenv -lws2_32 -ldbghelp -lcrypt32 \
  -o spec-tests/.run-ac/windows-direct-receiver.exe
