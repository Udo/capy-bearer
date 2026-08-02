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
	'capy-01-install-and-first-program'
	'capy-02-source-structure-and-syntax'
	'capy-03-values-and-types'
	'capy-05-functions-and-closures'
	'capy-10-units-components-and-exports'
	'capy-12-errors-testing-and-style'
)
for index in "${!pages[@]}"; do
	path="$base/${pages[$index]}.capy"
	headers=$(curl -fsS --max-time 30 -o /dev/null -D - -H "Host: $host" "$path" | tr -d '\r')
	[[ "$headers" == *"HTTP/1.1 302"* && "$headers" == *"Location: /doc/?p=${targets[$index]}"* ]] || {
		echo "Capy reference redirect mismatch for $path" >&2
		exit 1
	}
done

doc_base="${BEARER_TEST_HTTP_BASE:-http://127.0.0.1}/doc/"
guide=$(curl -fsS --max-time 60 -H "Host: $host" "$doc_base?p=capy-08-dynamic-values") || {
	echo "Capy guide page did not render through the documentation application" >&2
	exit 1
}
[[ "$guide" == *"8. Dynamic values"* && "$guide" == *"Nested assignment updates a declared local DValue root"* ]] || {
	echo "Capy guide page omitted its title or nested-assignment content" >&2
	exit 1
}
constructor=$(curl -fsS --max-time 60 -H "Host: $host" "$doc_base?p=2_DValue_to_string") || {
	echo "Capy constructor API page did not render" >&2
	exit 1
}
[[ "$constructor" == *"DValue::to_string"* && "$constructor" == *"function string(value : dval"* ]] || {
	echo "Capy constructor API page omitted its C++ or Capy signature" >&2
	exit 1
}
legacy_headers=$(curl -fsS --max-time 30 -o /dev/null -D - -H "Host: $host" "$doc_base?p=capy-10-dvalues" | tr -d '\r')
[[ "$legacy_headers" == *"HTTP/1.1 302"* && "$legacy_headers" == *"Location: /doc/?p=capy-08-dynamic-values"* ]] || {
	echo "Legacy Capy guide URL did not redirect to its canonical page" >&2
	exit 1
}

scripts/test_capy_guide_examples.sh

echo "Capy reference redirects, documentation pages, and guide examples passed for ${#pages[@]} reference pages"
