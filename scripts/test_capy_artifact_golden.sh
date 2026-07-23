#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

capyc=${CAPYC:-bin/capyc}
output_dir=/tmp/capy-artifact-golden
expected=scripts/capy_artifact_golden.sha256
mapfile -t fixtures < <(git ls-files 'site/tests/*.capy' | sort)
rm -rf "$output_dir"
mkdir -p "$output_dir"
cleanup() { rm -rf "$output_dir"; }
trap cleanup EXIT

actual="$output_dir/actual.sha256"
: >"$actual"
for source in "${fixtures[@]}"; do
	name=${source//\//_}
	artifact="$output_dir/$name.wasm"
	"$capyc" "$source" -o "$artifact" --source-map "$artifact.source-map" --abi-version 22
	printf '%s  %s.wasm\n' "$(sha256sum "$artifact" | cut -d' ' -f1)" "$name" >>"$actual"
	printf '%s  %s.wasm.source-map\n' "$(sha256sum "$artifact.source-map" | cut -d' ' -f1)" "$name" >>"$actual"
done

if [[ ${1:-} == "--write" ]]; then
	cp "$actual" "$expected"
	exit 0
fi
if ! diff -u "$expected" "$actual"; then
	echo "Capy artifact bytes changed; inspect ABI/index/section ordering before updating $expected" >&2
	exit 1
fi
echo "Capy Wasm and source-map golden artifacts passed"
