#!/usr/bin/env bash
# Ring's cc-rs build uses the Rust target spelling; Zig owns the target here.
args=()
for arg in "$@"; do
    [[ "$arg" == --target=* ]] || args+=("$arg")
done
exec zig cc -target x86_64-linux-musl "${args[@]}"
