#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

capyc=${CAPYC:-bin/capyc}
[[ $capyc == /* ]] || capyc="$PWD/$capyc"
abi_version=$(awk '/^#define BEARER_WASM_CORE_ABI_VERSION / {print $3; exit}' src/wasm/abi.h)
temporary=$(mktemp -d)
cleanup() { rm -rf "$temporary"; }
trap cleanup EXIT

for root in "$temporary/short" "$temporary/a-different-checkout-directory"; do
	mkdir -p "$root/site/tests"
	cp site/tests/capy-arc.capy "$root/site/tests/capy-arc.capy"
	{
		printf 'function CLI(request : dval) {\n'
		for ((i = 0; i < 100; ++i)); do
			printf ' var value_%d := dval(%d)\n' "$i" "$i"
		done
		printf '}\n'
	} >"$root/site/tests/capy-dense-dval.capy"
	(
		cd "$root"
		for source in capy-arc capy-dense-dval; do
			"$capyc" "site/tests/$source.capy" -o "$source.wasm" \
				--source-map "$source.wasm.source-map" --abi-version "$abi_version"
		done
	)
done

for artifact in capy-arc.wasm capy-arc.wasm.source-map capy-dense-dval.wasm capy-dense-dval.wasm.source-map; do
	cmp "$temporary/short/$artifact" "$temporary/a-different-checkout-directory/$artifact"
done
echo "Capy artifacts are reproducible across source directories"
