#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
umask 077

http_host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"
http_base="${BEARER_TEST_HTTP_BASE:-http://127.0.0.1}"
http_timeout="${BEARER_TEST_HTTP_TIMEOUT:-30}"
abi_version=$(awk '/^#define BEARER_WASM_CORE_ABI_VERSION / {print $3; exit}' src/wasm/abi.h)
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
bin_directory="${BEARER_TEST_BIN_DIRECTORY:-/tmp/bearer/work}"

if [[ -r /etc/bearer/settings.cfg ]]; then
	configured_site=$(awk -F= '/^[[:space:]]*HTTP_DOCUMENT_ROOT[[:space:]]*=/ {sub(/^[^=]*=/, ""); print; exit}' /etc/bearer/settings.cfg)
	configured_bin=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {sub(/^[^=]*=/, ""); print; exit}' /etc/bearer/settings.cfg)
	[[ -n "${BEARER_TEST_SITE_DIRECTORY:-}" ]] || site_directory="${configured_site:-$site_directory}"
	[[ -n "${BEARER_TEST_BIN_DIRECTORY:-}" ]] || bin_directory="${configured_bin:-$bin_directory}"
fi

[[ "$http_timeout" =~ ^[1-9][0-9]*$ ]] || {
	echo "Capy guide test timeout must be a positive integer" >&2
	exit 2
}
python3 - "$http_base" <<'PY'
import sys
from urllib.parse import urlsplit

parsed = urlsplit(sys.argv[1])
if parsed.scheme != "http" or parsed.username or parsed.password or parsed.hostname not in {"127.0.0.1", "::1", "localhost"}:
    raise SystemExit("Capy guide test HTTP base must use localhost over HTTP")
PY

site_directory=$(realpath "$site_directory")
[[ -d "$site_directory/tests" ]] || {
	echo "Capy guide test directory is missing: $site_directory/tests" >&2
	exit 1
}

service_status=$(curl -sS --max-time "$http_timeout" -o /dev/null -w '%{http_code}' -H "Host: $http_host" "$http_base/") || {
	echo "Capy guide test service is unavailable at the local HTTP base" >&2
	exit 1
}
[[ "$service_status" =~ ^[23][0-9][0-9]$ ]] || {
	echo "Capy guide test service returned HTTP $service_status at the local HTTP base" >&2
	exit 1
}

test_directory=$(mktemp -d "$site_directory/tests/capy-guide-examples.XXXXXX")
chmod 711 "$test_directory"
cache_directory=""
cleanup() {
	rm -rf -- "$test_directory"
	[[ -z "$cache_directory" ]] || rm -rf -- "$cache_directory"
}
trap cleanup EXIT HUP INT TERM

python3 - "$test_directory" <<'PY'
import importlib.util
import re
import sys
from pathlib import Path

root = Path.cwd()
renderer = (root / "site/doc/components/doc_page.capy").read_text()
if "function COMPONENT(request : dval)" not in renderer:
    raise SystemExit("Capy documentation renderer must declare request : dval")
checker_path = root / "scripts/check_capy_doc_examples.py"
spec = importlib.util.spec_from_file_location("capy_doc_examples", checker_path)
if spec is None or spec.loader is None:
    raise SystemExit("Capy guide test cannot import the documentation checker")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
guides = root / "site/doc/capy"
errors = checker.check_language_guides(guides, root / "site/doc/pages")
if errors:
    raise SystemExit("Capy guide test rejected guide sources:\n- " + "\n- ".join(errors))
articles = {path.stem: path for path in guides.glob("*.txt")}
if set(articles) != checker.CANONICAL_GUIDES:
    raise SystemExit("Capy guide test found a guide set that differs from the checker canonical set")
snippet_directory = Path(sys.argv[1]) / "snippets"
snippet_directory.mkdir()
unsafe = re.compile(r"\b(?:component_(?:capture|render)|file_[a-z_]+|job_[a-z_]+|mysql_[a-z_]+|server_[a-z_]+|shell_(?:exec|spawn)|sqlite_[a-z_]+|task(?:_[a-z_]+)?|unit_(?:call|compile|load)|ws_[a-z_]+)\s*\(")
for slug in sorted(checker.CANONICAL_GUIDES):
    page = articles[slug]
    sections = checker.parse_sections(page)
    chosen = None
    for index, (line, header, body) in enumerate(sections):
        if header.startswith("example capy render") and index + 1 < len(sections) and sections[index + 1][1] == "output" and sections[index + 1][2]:
            chosen = (line, body, sections[index + 1][2])
            break
    if chosen is None:
        raise SystemExit(f"{page.name}: missing a runnable RENDER example with exact output")
    line, source, expected = chosen
    source_without_strings = re.sub(r'"(?:\\.|[^"\\])*"', '""', source)
    source_without_strings = re.sub(r"//[^\n]*", "", source_without_strings)
    match = unsafe.search(source_without_strings)
    if match:
        raise SystemExit(f"{page.name}:{line}: example requires undeclared external setup or has unsafe side effects: {match.group(0).strip()}")
    source_path = Path(sys.argv[1]) / f"{slug}.capy"
    source_path.write_text(source + "\n")
    source_path.chmod(0o644)
    (Path(sys.argv[1]) / f"{slug}.expected").write_text(expected + "\n")
    snippet_index = 1
    for index, (example_line, header, snippet) in enumerate(sections):
        if not header.startswith("example capy") or snippet == source:
            continue
        snippet_path = snippet_directory / f"{slug}-{snippet_index}.capy"
        snippet_path.write_text(snippet.rstrip() + "\n")
        snippet_index += 1
PY

snippet_build="$test_directory/snippet-build"
mkdir -p "$snippet_build"
snippet_count=0
for snippet in "$test_directory"/snippets/*.capy; do
	snippet_name=${snippet##*/}
	if ! bin/capyc "$snippet" --bearer-unit --abi-version "$abi_version" -o "$snippet_build/$snippet_name.wasm" --source-map "$snippet_build/$snippet_name.wasm.source-map"; then
		echo "Capy guide snippet failed to compile: $snippet_name" >&2
		exit 1
	fi
	((snippet_count += 1))
done

cache_directory="$(scripts/unit_cache_directory "$bin_directory")$test_directory"
test_name=${test_directory##*/}
count=0
page_09_checked=false
for source in "$test_directory"/*.capy; do
	slug=${source##*/}
	slug=${slug%.capy}
	expected="$test_directory/$slug.expected"
	actual="$test_directory/$slug.actual"
	if ! curl -fsS --max-time "$http_timeout" -H "Host: $http_host" "$http_base/tests/$test_name/$slug.capy" -o "$actual"; then
		echo "Capy guide example failed for $slug: the local HTTP request failed" >&2
		exit 1
	fi
	if ! cmp -s "$expected" "$actual"; then
		expected_size=$(wc -c <"$expected")
		actual_size=$(wc -c <"$actual")
		expected_sha=$(sha256sum "$expected" | awk '{print $1}')
		actual_sha=$(sha256sum "$actual" | awk '{print $1}')
		printf 'Capy guide example mismatch for %s: expected %s bytes sha256=%s, got %s bytes sha256=%s\n' \
			"$slug" "$expected_size" "$expected_sha" "$actual_size" "$actual_sha" >&2
		exit 1
	fi
	page="$test_directory/$slug.page"
	page_headers="$test_directory/$slug.headers"
	if ! page_status=$(curl -sS --max-time "$http_timeout" -D "$page_headers" -o "$page" -w '%{http_code}' -H "Host: $http_host" "$http_base/doc/guide/${slug#[0-9][0-9]-}/"); then
		echo "Capy documentation application failed to request $slug" >&2
		exit 1
	fi
	if [[ ! "$page_status" =~ ^2[0-9][0-9]$ ]]; then
		echo "Capy documentation application returned HTTP $page_status for $slug" >&2
		exit 1
	fi
	if ! tr -d '\r' <"$page_headers" | grep -qiE '^Content-Type:[[:space:]]*text/html([[:space:]]*;|[[:space:]]*$)'; then
		if [[ "$slug" == "09-web-handlers-and-requests" ]]; then
			echo "Capy guide page 09 leaked an example response Content-Type" >&2
		else
			echo "Capy documentation application did not return text/html for $slug" >&2
		fi
		exit 1
	fi
	if [[ "$slug" == "09-web-handlers-and-requests" ]]; then
		page_09_checked=true
	fi
	if grep -qE 'DOC EXAMPLE ERROR|Compile-only: WS|guide-example|Minimal executable example|Common variants' "$page" ||
		! grep -q '<pre><code class="language-capy">' "$page" ||
		! grep -q '<strong>Output</strong>' "$page" ||
		! grep -q '<pre><code class="language-text">' "$page"; then
		echo "Capy documentation application misplaced the source or output for $slug" >&2
		exit 1
	fi
	python3 - "$page" <<'PY'
import re
import sys
from pathlib import Path

html = Path(sys.argv[1]).read_text()
pattern = r'<pre><code class="language-capy">.*?</code></pre><p><strong>Output</strong></p><pre><code class="language-text">.*?</code></pre>'
if re.search(pattern, html, re.S) is None:
    raise SystemExit("Capy documentation source and output are not adjacent")
PY
	((count += 1))
done
expected_guides=$(find site/doc/capy -maxdepth 1 -type f -name '*.txt' | wc -l)
[[ "$count" -eq "$expected_guides" ]] || {
	echo "Capy guide test expected $expected_guides canonical examples, ran $count" >&2
	exit 1
}
[[ "$page_09_checked" == true ]] || {
	echo "Capy guide test did not request page 09" >&2
	exit 1
}
echo "Capy guide HTTP examples passed for $count pages; $snippet_count additional snippets compiled"
