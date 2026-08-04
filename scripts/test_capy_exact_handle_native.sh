#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source="/tmp/capy-exact-handle-$$.cpp"
binary="/tmp/capy-exact-handle-$$"
trap 'rm -f "$source" "$binary"' EXIT
cat >"$source" <<'EOF'
#include "src/lib/types.cpp"
#include "src/lib/dvalue.cpp"
#include "src/lib/functionlib.cpp"
#include <cassert>
int main()
{
	for(const u64 handle : {9007199254740993ull, 18446744073709551615ull})
	{
		DValue request; request["handle"] = std::to_string(handle);
		String encoded = brb_encode(request); DValue decoded; String error;
		assert(brb_decode(encoded, decoded, &error));
		assert(to_u64(decoded["handle"].to_string(), 0) == handle);
	}
}
EOF
"${CXX:-c++}" -std=c++20 -fpermissive -I. "$source" -lpcre2-8 -o "$binary"
"$binary"
echo "exact BRRB u64 decimal round-trip passed"
