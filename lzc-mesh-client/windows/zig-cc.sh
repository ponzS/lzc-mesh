#!/usr/bin/env bash
# cc-rs supplies Rust's four-part triple; Zig uses its three-part spelling.
args=()
for arg in "$@"; do
    [[ "$arg" == --target=x86_64-pc-windows-gnu ]] || args+=("$arg")
done
exec zig cc -target x86_64-windows-gnu "${args[@]}"
