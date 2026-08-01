#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"
base="${BEARER_TEST_HTTP_BASE:-http://127.0.0.1}/examples/capy-reference"
reference_dir=site/examples/capy-reference

[[ -f "$reference_dir/index.capy" && -f "$reference_dir/style.css" ]]
if find "$reference_dir" -type f -name '*.uce' -print -quit | grep -q .; then
	echo "Capy reference must not contain C++ .uce units" >&2
	exit 1
fi

pages=(index syntax types ownership interop status)
targets=(
	'capy-01-getting-started'
	'capy-02-basic-syntax'
	'capy-03-types'
	'capy-09-function-values-closures-and-memory'
	'capy-12-components-and-units'
	'capy-14-errors-debugging-and-style'
)
for index in "${!pages[@]}"; do
	path="$base/${pages[$index]}.capy"
	headers=$(curl -fsS --max-time 30 -o /dev/null -D - -H "Host: $host" "$path" | tr -d '\r')
	[[ "$headers" == *"HTTP/1.1 302"* && "$headers" == *"Location: /doc/?p=${targets[$index]}"* ]] || {
		echo "Capy reference redirect mismatch for $path" >&2
		exit 1
	}
done

echo "Legacy Capy reference redirects passed for ${#pages[@]} pages"
