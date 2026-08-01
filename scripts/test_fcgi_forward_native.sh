#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bin=/tmp/test-fcgi-forward-native
trap 'rm -f "$bin"' EXIT
"${CXX:-g++}" -std=c++20 -O2 -I. scripts/test_fcgi_forward_native.cpp -lpcre2-8 -lcrypto -o "$bin"
timeout --signal=TERM --kill-after=2s 10s "$bin"
echo "bounded FastCGI forwarding passed"
