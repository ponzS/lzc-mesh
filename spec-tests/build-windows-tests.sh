#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p spec-tests/.run-ac
c++ -std=c++17 -O2 -Wall -Wextra spec-tests/windows-input.cpp lzc-mesh-client/windows/src/json.cpp -o spec-tests/.run-ac/windows-input
spec-tests/.run-ac/windows-input
cargo build --locked --release --manifest-path lzc-mesh-client/transport/Cargo.toml
c++ -std=c++17 -O2 spec-tests/transport-probe.cpp lzc-mesh-client/windows/src/json.cpp \
  lzc-mesh-client/transport/target/release/liblzc_mesh_transport.a -ldl -lpthread -lm -o spec-tests/.run-ac/transport-probe
zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -Wl,--subsystem,windows \
  spec-tests/windows-observer.cpp lzc-mesh-client/windows/src/json.cpp -luser32 -lgdi32 -o spec-tests/.run-ac/windows-observer.exe
# Windows-only regressions are built here but run explicitly on the test host.
# Caption tests never run implicitly: they require an unlocked, idle fixture.
zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -DUNICODE -D_UNICODE \
  spec-tests/windows-receiver.cpp lzc-mesh-client/windows/src/receiver.cpp lzc-mesh-client/windows/src/json.cpp \
  -luser32 -o spec-tests/.run-ac/windows-receiver.exe
zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -Wl,--subsystem,windows \
  spec-tests/windows-caption-os.cpp lzc-mesh-client/windows/src/input.cpp lzc-mesh-client/windows/src/receiver.cpp \
  lzc-mesh-client/windows/src/json.cpp -luser32 -lgdi32 -ladvapi32 -o spec-tests/.run-ac/windows-caption-os.exe
