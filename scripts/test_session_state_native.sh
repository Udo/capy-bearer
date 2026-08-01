#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
bin="${TMPDIR:-/tmp}/test_session_state_native.$$"
trap 'rm -f "$bin"' EXIT
compiler=$(command -v clang++ || command -v g++)
"$compiler" -std=c++20 -fpermissive -I"$repo" "$repo/scripts/test_session_state_native.cpp" -lpcre2-8 -lcrypto -o "$bin"
timeout --signal=TERM --kill-after=2s 20s "$bin"
