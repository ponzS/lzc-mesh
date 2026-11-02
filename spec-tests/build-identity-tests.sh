#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p spec-tests/.run-ac
cargo build --locked --release --manifest-path lzc-mesh-client/transport/Cargo.toml
cc -std=gnu11 -O2 $(pkg-config --cflags glib-2.0) spec-tests/linux-identity.c lzc-mesh-client/linux/src/identity.c \
 lzc-mesh-client/transport/target/release/liblzc_mesh_transport.a $(pkg-config --libs glib-2.0) -ldl -lpthread -lm -o spec-tests/.run-ac/linux-identity
zig c++ -target x86_64-windows-gnu -std=c++17 -O2 spec-tests/windows-identity.cpp lzc-mesh-client/windows/src/identity.cpp \
 lzc-mesh-client/transport/target/x86_64-pc-windows-gnu/release/liblzc_mesh_transport.a \
 -lcrypt32 -lshell32 -lole32 -luser32 -lbcrypt -lntdll -luserenv -lws2_32 -ladvapi32 -o spec-tests/.run-ac/windows-identity.exe
