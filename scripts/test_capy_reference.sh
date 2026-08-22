#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"
base="${BEARER_TEST_HTTP_BASE:-http://127.0.0.1}/examples/capy-reference"
reference_dir=site/examples/capy-reference

[[ -f "$reference_dir/index.capy" && -f "$reference_dir/style.css" ]]

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
[[ "$guide" == *"8. Dynamic values"* && "$guide" == *"Nested assignment creates missing maps"* &&
	"$guide" == *"var profile :="* && "$guide" == *"<strong>Output</strong>"* &&
	"$guide" == *'class="guide-navigation"'* && "$guide" == *'Previous: '* && "$guide" == *'Next: '* &&
	"$guide" != *"Minimal executable example"* && "$guide" != *"Compile-only: WS"* ]] || {
	echo "Capy guide page omitted its content, output, or navigation" >&2
	exit 1
}
constructor=$(curl -fsS --max-time 60 -H "Host: $host" "$doc_base?p=2_DValue_to_string") || {
	echo "Capy constructor API page did not render" >&2
	exit 1
}
[[ "$constructor" == *"function string(value : dval"* && "$constructor" != *"DOC EXAMPLE ERROR"* ]] || {
	echo "Capy constructor API page omitted its Capy signature" >&2
	exit 1
}
contract=$(curl -fsS --max-time 60 -H "Host: $host" "$doc_base?p=http_request") || {
	echo "Structured API contract page did not render" >&2
	exit 1
}
[[ "$contract" == *'<h3>Description</h3>'* && "$contract" == *'<h3>Parameters</h3>'* &&
	"$contract" == *'<h3>Return Values</h3>'* && "$contract" == *'<h3>Errors</h3>'* &&
	"$contract" == *'class="doc-detail-layout"'* &&
	"$contract" == *'class="related-link" href="/doc/?p=http_request_async"'* &&
	"$contract" != *"DOC EXAMPLE ERROR"* ]] || {
	echo "Structured API contract sections or related links are incomplete" >&2
	exit 1
}
legacy=$(curl -fsS --max-time 60 -H "Host: $host" "$doc_base?p=capy-10-dvalues") || {
	echo "Legacy Capy guide URL did not render" >&2
	exit 1
}
[[ "$legacy" == *"8. Dynamic values"* ]] || {
	echo "Legacy Capy guide URL did not reach its canonical content" >&2
	exit 1
}
singlepage=$(curl -fsS --max-time 180 -H "Host: $host" "${doc_base}all/") || {
	echo "Combined documentation route list did not render" >&2
	exit 1
}
[[ "$singlepage" == *'/doc/guide/install-and-first-program/'* &&
	"$singlepage" == *'/doc/api/dvalue/'* && "$singlepage" == *'/doc/api/http-request/'* &&
	"$singlepage" != *"Minimal executable example"* ]] || {
	echo "Combined documentation route list omitted guide or API links" >&2
	exit 1
}
search=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}search/?q=http_request") || {
	echo "Documentation search did not render" >&2
	exit 1
}
[[ "$search" == *'/doc/api/http-request/'* && "$search" == *'http_request'* ]] || {
	echo "Documentation search omitted a structured API page" >&2
	exit 1
}

scripts/test_capy_guide_examples.sh

echo "Capy reference redirects, documentation pages, and guide examples passed for ${#pages[@]} reference pages"
