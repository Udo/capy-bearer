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
! grep -q 'component_exists' site/doc/index.capy || { echo "Documentation route probes before rendering" >&2; exit 1; }
python3 - <<'PY'
from pathlib import Path

root = Path.cwd()
if (root / "site/doc/components/doc_page.capy").exists():
    raise SystemExit("Documentation retains the dynamic detail component")
if 'component_render("/doc/components/doc_' in (root / "scripts/generate_capy_docs.py").read_text():
    raise SystemExit("Documentation generator retains dynamic detail components")
for path in (root / "site/doc/content").glob("*/*.capy"):
    lines = [line.strip() for line in path.read_text().splitlines() if line.strip()]
    if not lines or lines[0] != "function COMPONENT(request : dval) {" or lines[-1] != "}":
        raise SystemExit(f"Documentation route has an unexpected shape: {path}")
    if any(not line.startswith("print(") for line in lines[1:-1]):
        raise SystemExit(f"Documentation route does not print static detail HTML directly: {path}")
for component in (root / "site/doc/components/index.capy", root / "site/doc/components/all.capy"):
    text = component.read_text()
    for heading in ("API Functions", "Types", "Handlers", "How-to", "Learn Capy"):
        if f">{heading}<" not in text:
            raise SystemExit(f"Documentation listing lacks {heading}: {component}")
    if "Clear a dynamic value" in text:
        raise SystemExit(f"Documentation listing uses an API prose title: {component}")
search = (root / "site/doc/components/search.capy").read_text()
if 'class=\\\"search-kind\\\"' not in search:
    raise SystemExit("Documentation search does not label result kinds")
PY
status() { curl -sS --max-time 60 -o /dev/null -w '%{http_code}' -H "Host: $host" "$1"; }
for path in api/zzzznotreal/ how-to/zzzznotreal/ api/component-2/ api/string-2/ '?''p=0''_String'; do
    [[ "$(status "$doc_base$path")" == 404 ]] || { echo "Old or unknown documentation URL did not return 404: $path" >&2; exit 1; }
done
for path in api/component/ api/string/ type/string/ type/dvalue/ type/request/ handler/render/ handler/component/ handler/cli/ how-to/read-request-context/; do
    [[ "$(status "$doc_base$path")" == 200 ]] || { echo "Canonical documentation URL did not return 200: $path" >&2; exit 1; }
done
component_page=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}api/component/")
component_exists_page=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}api/component-exists/")
[[ "$component_page" == *">component</h2>"* && "$component_exists_page" == *">component_exists</h2>"* ]] || { echo "Documentation pages with a shared eight-character prefix did not resolve independently" >&2; exit 1; }
route_gate="site/doc/content/how-to/route-file-gate.capy"
trap 'rm -f "$route_gate"' EXIT
cat >"$route_gate" <<'EOF'
function COMPONENT(request : dval) {
    print("<main>route file gate</main>")
}
EOF
route_added=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}how-to/route-file-gate/")
[[ "$route_added" == *"route file gate"* ]] || { echo "A documentation page file did not reach its route" >&2; exit 1; }
rm -f "$route_gate"
[[ "$(status "${doc_base}how-to/route-file-gate/")" == 404 ]] || { echo "A deleted documentation page file did not return 404" >&2; exit 1; }

guide=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}guide/dynamic-values/")
[[ "$guide" == *"8. Dynamic values"* && "$guide" == *"Nested assignment creates missing maps"* && "$guide" == *'class="guide-navigation"'* ]] || { echo "Capy guide page omitted content or navigation" >&2; exit 1; }
constructor=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}api/string/")
[[ "$constructor" == *"function string(value : dval"* && "$constructor" != *"DOC EXAMPLE ERROR"* ]] || { echo "Capy constructor API page omitted its signature" >&2; exit 1; }
contract=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}api/http-request/")
[[ "$contract" == *'<h3>Description</h3>'* && "$contract" == *'class="related-link" href="/doc/api/http-request-async/">http_request_async</a>'* ]] || { echo "Structured API contract sections or related links are incomplete" >&2; exit 1; }
index=$(curl -fsS --max-time 60 -H "Host: $host" "$doc_base")
[[ "$index" != *'>String()<'* && "$index" != *'>COMPONENT()<'* && "$index" != *'>CLI()<'* ]] || { echo "Type or handler index entry rendered as a function" >&2; exit 1; }
[[ "$index" != *"route-file-gate"* ]] || { echo "Documentation index retained a deleted page" >&2; exit 1; }
singlepage=$(curl -fsS --max-time 180 -H "Host: $host" "${doc_base}all/")
[[ "$singlepage" == *'/doc/guide/install-and-first-program/'* && "$singlepage" == *'/doc/how-to/read-request-context/'* && "$singlepage" == *'/doc/type/dvalue/'* && "$singlepage" == *'/doc/api/http-request/'* && "$singlepage" == *'>How-to</h3>'* ]] || { echo "Combined documentation route list omitted canonical links or the how-to section" >&2; exit 1; }
search=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}search/?q=http_request")
[[ "$search" == *'/doc/api/http-request/'* && "$search" == *'http_request'* ]] || { echo "Documentation search omitted a structured API page" >&2; exit 1; }
how_to_search=$(curl -fsS --max-time 60 -H "Host: $host" "${doc_base}search/?q=request")
[[ "$how_to_search" == *'/doc/how-to/read-request-context/'* && "$how_to_search" == *'search-how-to'* && "$how_to_search" == *'>How-to</h3>'* ]] || { echo "Documentation search omitted the how-to section" >&2; exit 1; }
scripts/test_capy_guide_examples.sh
echo "Capy reference pages and canonical documentation routes passed"
