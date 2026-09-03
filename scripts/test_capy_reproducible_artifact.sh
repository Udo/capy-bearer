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
	(
		cd "$root"
		"$capyc" site/tests/capy-arc.capy -o artifact.wasm \
			--source-map artifact.wasm.source-map --abi-version "$abi_version"
	)
done

cmp "$temporary/short/artifact.wasm" "$temporary/a-different-checkout-directory/artifact.wasm"
cmp "$temporary/short/artifact.wasm.source-map" "$temporary/a-different-checkout-directory/artifact.wasm.source-map"
echo "Capy artifacts are reproducible across source directories"
