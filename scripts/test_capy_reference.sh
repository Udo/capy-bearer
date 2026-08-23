#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"
base="${BEARER_TEST_HTTP_BASE:-http://127.0.0.1}/examples/capy-reference"
reference_dir=site/examples/capy-reference
[[ -f "$reference_dir/index.capy" && -f "$reference_dir/style.css" ]]

pages=(index syntax types ownership interop status)
targets=(install-and-first-program source-structure-and-syntax values-and-types functions-and-closures units-components-and-exports errors-testing-and-style)
for index in "${!pages[@]}"; do
    path="$base/${pages[$index]}.capy"
    headers=$(curl -fsS --max-time 30 -o /dev/null -D - -H "Host: $host" "$path" | tr -d '\r')
    [[ "$headers" == *"HTTP/1.1 302"* && "$headers" == *"Location: /doc/guide/${targets[$index]}/"* ]] || { echo "Capy reference redirect mismatch for $path" >&2; exit 1; }
done

doc_base="${BEARER_TEST_HTTP_BASE:-http://127.0.0.1}/doc/"
status() { curl -sS --max-time 60 -o /dev/null -w '%{http_code}' -H "Host: $host" "$1"; }
for path in api/zzzznotreal/ api/component-2/ api/string-2/ '?''p=0''_String'; do
    [[ "$(status "$doc_base$path")" == 404 ]] || { echo "Old or unknown documentation URL did not return 404: $path" >&2; exit 1; }
done
for path in api/component/ api/string/ type/string/ type/dvalue/ type/request/ handler/render/ handler/component/ handler/cli/; do
    [[ "$(status "$doc_base$path")" == 200 ]] || { echo "Canonical documentation URL did not return 200: $path" >&2; exit 1; }
done

guide=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}guide/dynamic-values/")
[[ "$guide" == *"8. Dynamic values"* && "$guide" == *"Nested assignment creates missing maps"* && "$guide" == *'class="guide-navigation"'* ]] || { echo "Capy guide page omitted content or navigation" >&2; exit 1; }
constructor=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}api/string/")
[[ "$constructor" == *"function string(value : dval"* && "$constructor" != *"DOC EXAMPLE ERROR"* ]] || { echo "Capy constructor API page omitted its signature" >&2; exit 1; }
contract=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}api/http-request/")
[[ "$contract" == *'<h3>Description</h3>'* && "$contract" == *'class="related-link" href="/doc/api/http-request-async/">http_request_async</a>'* ]] || { echo "Structured API contract sections or related links are incomplete" >&2; exit 1; }
index=$(curl -fsS --max-time 60 -H "Host: $host" "$doc_base")
[[ "$index" != *'>String()<'* && "$index" != *'>COMPONENT()<'* && "$index" != *'>CLI()<'* ]] || { echo "Type or handler index entry rendered as a function" >&2; exit 1; }
singlepage=$(curl -fsS --max-time 180 -H "Host: $host" "${doc_base}all/")
[[ "$singlepage" == *'/doc/guide/install-and-first-program/'* && "$singlepage" == *'/doc/type/dvalue/'* && "$singlepage" == *'/doc/api/http-request/'* ]] || { echo "Combined documentation route list omitted canonical links" >&2; exit 1; }
search=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}search/?q=http_request")
[[ "$search" == *'/doc/api/http-request/'* && "$search" == *'http_request'* ]] || { echo "Documentation search omitted a structured API page" >&2; exit 1; }
scripts/test_capy_guide_examples.sh
echo "Capy reference pages and canonical documentation routes passed"
