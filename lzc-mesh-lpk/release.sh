#!/bin/sh
# lzc-cli reads package.yml BEFORE buildscript, so sync before starting it.
set -eu
cd "$(dirname "$0")"
python3 ../scripts/version.py
export LZC_VERSION="$(python3 ../scripts/version.py --print)"
exec lzc-cli project release "$PWD" "$@"
