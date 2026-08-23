#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=${TMPDIR:-/tmp}/capy-native-tests
mkdir -p "$BUILD_DIR"
COMMON=(-std=c++20 -Wall -Wextra -Werror -pedantic -Isrc/capy)
ABI_VERSION=$(awk '/BEARER_WASM_CORE_ABI_VERSION/ {print $3; exit}' src/wasm/abi.h)
PARITY_MANIFEST="$BUILD_DIR/capy-capability-manifest.md"
needs_rebuild() {
	local output=$1
	shift
	[[ ! -x "$output" ]] || find "$@" -newer "$output" -print -quit | grep -q .
}
if needs_rebuild "$BUILD_DIR/capyc" src/capy src/lib/markup-context.h scripts/build_capy.sh; then
	scripts/build_capy.sh "$BUILD_DIR/capyc" debug
else
	echo "Reusing $BUILD_DIR/capyc"
fi
"$BUILD_DIR/capyc" --check-stdlib src/capy/stdlib.capy src/capy/stdlib.embedded.h
python3 - <<'STATIC_GATE'
from pathlib import Path
import re
handlers = re.compile(r'^\s*function\s+(?:RENDER|COMPONENT(?:\s*:\s*[A-Za-z_][A-Za-z0-9_]*)?|CLI|WS|TASK(?:\s*:\s*[A-Za-z_][A-Za-z0-9_]*)?|INIT|ONCE|SERVE_HTTP(?:\s*:\s*[A-Za-z_][A-Za-z0-9_]*)?)\s*(?:\(([^)]*)\))?\s*\{', re.M)
removed = re.compile(r'\b(?:request_context|request_param|request_get|request_post|request_cookie|request_session|request_body|request_base_url|request_script_url|request_query_path|request_query_route|cli_input|cli_arg|ws_message|ws_connection_id|ws_scope|ws_opcode|ws_is_binary|ws_connections|ws_connection_count|to_bool|to_f64|to_s64|to_u64|to_lower|to_upper|dval_to_json|dval_to_stringmap|request_route_from_raw_path|request_perf)\s*\(')
paths = [path for line in __import__('subprocess').check_output(['git', 'ls-files', '*.capy'], text=True).splitlines() if (path := Path(line)).is_file()]
errors = []
for path in paths:
    source = path.read_text()
    for match in handlers.finditer(source):
        if match.group(1) != 'request : dval':
            errors.append(f'{path}:{source.count(chr(10), 0, match.start()) + 1}: handler must use (request : dval)')
    code = re.sub(r'"(?:\\.|[^"\\])*"', '""', source)
    if re.search(r'\brequest\s*:\s*request\b', code):
        errors.append(f'{path}: public request type is not allowed')
    if removed.search(code):
        errors.append(f'{path}: removed request reader is not allowed')
fixture = Path('site/tests/capy-request-context.capy').read_text()
for field in ('method', 'query', 'form', 'headers', 'cookies', 'body', 'files', 'route', 'url', 'server', 'session', 'props', 'config', 'call', 'connection', 'unit', 'websocket'):
    if f'request.{field}' not in fixture:
        errors.append(f'site/tests/capy-request-context.capy: missing request schema field {field}')
if errors:
    raise SystemExit('\n'.join(errors))
STATIC_GATE
if git grep -nE '[0-9](s32|s64|u64)\b' -- src/capy/stdlib.capy 'site/**/*.capy' 'site/doc/**/*.txt' docs/capy-language.md; then
	echo "Capy source or documentation still uses a removed numeric suffix" >&2
	exit 1
fi
python3 scripts/generate_capy_doc_signatures.py --capyc "$BUILD_DIR/capyc" --check
python3 - <<'PY'
from pathlib import Path
import re
stdlib = Path("src/capy/stdlib.capy").read_text()
core = Path("src/wasm/core.cpp").read_text()
for host in re.findall(r"host function (__bearer_\w+)\(operation : s32", stdlib):
    assert not re.search(re.escape(host) + r"\(\s*\d", stdlib), host
assert not re.search(r"\bcase\s+\d+\s*:\s*|(?:\boperation\s*[=!<>]=?\s*\d|\d\s*[=!<>]=?\s*\boperation)", core)
assert "function request_apply" not in stdlib and not re.search(r"\bglobal\s*>\s*21\b", core)
PY
scripts/test_hardened_http_native.sh
scripts/test_crypto_operation_native.sh
scripts/test_dvalue_none_native.sh
scripts/test_capy_exact_handle_native.sh
"$BUILD_DIR/capyc" --parity-manifest "$PARITY_MANIFEST"
cmp "$PARITY_MANIFEST" docs/capy-capability-manifest.md
expected_manifest_pages=$(find site/doc/pages -maxdepth 1 -type f -name '*.txt' ! -name '3_*' | wc -l)
[[ $(grep -c '^| `' "$PARITY_MANIFEST") -eq $expected_manifest_pages ]]

clang++ "${COMMON[@]}" src/capy/frontend.cpp scripts/test_capy_native_frontend.cpp \
	-o "$BUILD_DIR/frontend"
mapfile -t fixtures < <({ git ls-files 'site/**/*.capy'; printf '%s\n' site/tests/capy-mutable-array-struct.capy site/tests/capy-dval-identity.capy; } | sort -u | while read -r path; do [[ -f "$path" ]] && printf '%s\n' "$path"; done)
"$BUILD_DIR/frontend" "${fixtures[@]}"

clang++ "${COMMON[@]}" src/capy/wasm.cpp scripts/test_capy_native_wasm.cpp \
	-o "$BUILD_DIR/wasm"
"$BUILD_DIR/wasm"

clang++ "${COMMON[@]}" src/capy/frontend.cpp src/capy/wasm.cpp src/capy/compiler.cpp \
	scripts/test_capy_native_compiler.cpp -o "$BUILD_DIR/compiler"
"$BUILD_DIR/compiler"
wasm-validate /tmp/capy-native.wasm
"$BUILD_DIR/capyc" site/tests/capy-phase1.capy \
	-o "$BUILD_DIR/phase1.wasm" --source-map "$BUILD_DIR/phase1.wasm.source-map" --abi-version "$ABI_VERSION"
wasm-validate "$BUILD_DIR/phase1.wasm"
"$BUILD_DIR/capyc" --check-unit "$BUILD_DIR/phase1.wasm" "$ABI_VERSION"
mkdir -p "$BUILD_DIR/repeat"
"$BUILD_DIR/capyc" site/tests/capy-phase1.capy \
	-o "$BUILD_DIR/repeat/phase1.wasm" --source-map "$BUILD_DIR/repeat/phase1.wasm.source-map" --abi-version "$ABI_VERSION"
cmp "$BUILD_DIR/phase1.wasm" "$BUILD_DIR/repeat/phase1.wasm"
cmp "$BUILD_DIR/phase1.wasm.source-map" "$BUILD_DIR/repeat/phase1.wasm.source-map"
cp "$BUILD_DIR/phase1.wasm" "$BUILD_DIR/wrapper.wasm"
wasm-validate "$BUILD_DIR/wrapper.wasm"

for fixture in capy-arc capy-loop-control capy-phase3 capy-closures capy-markup capy-dval-rich capy-cross capy-module-target capy-module-caller capy-methods capy-mutable-array-struct capy-dval-identity; do
	"$BUILD_DIR/capyc" "site/tests/$fixture.capy" \
		-o "$BUILD_DIR/$fixture.wasm" --source-map "$BUILD_DIR/$fixture.wasm.source-map" --abi-version "$ABI_VERSION"
	wasm-validate "$BUILD_DIR/$fixture.wasm"
	"$BUILD_DIR/capyc" --check-unit "$BUILD_DIR/$fixture.wasm" "$ABI_VERSION"
done

for source in "${fixtures[@]}"; do
	artifact="$BUILD_DIR/${source//\//_}.wasm"
	"$BUILD_DIR/capyc" "$source" -o "$artifact" --source-map "$artifact.source-map" --abi-version "$ABI_VERSION"
	wasm-validate "$artifact"
	"$BUILD_DIR/capyc" --check-unit "$artifact" "$ABI_VERSION"
done

wasm-objdump -x "$BUILD_DIR/phase1.wasm" >"$BUILD_DIR/phase1.objdump"
! grep -q 'bearer_request_context_brrb\|bearer_response_set_\|bearer_print_s64\|bearer_print_u64\|bearer_print_f64\|bearer_format_u64\|bearer_format_f64\|bearer_time\|bearer_file_\|bearer_unit_info_brrb\|bearer_units_list_brrb\|bearer_unit_compile\|bearer_codec\|bearer_regex\\|bearer_string_nonblank\|bearer_dv_merge_brrb\|bearer_sqlite_' "$BUILD_DIR/phase1.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-wide-scalars.capy.wasm" >"$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_format_s64' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_format_u64' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_format_f64' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_print_bytes' "$BUILD_DIR/wide-scalars.objdump"
! grep -q 'env.bearer_print_s64\|env.bearer_print_u64\|env.bearer_print_f64' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_time' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_time_precise' "$BUILD_DIR/wide-scalars.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-markup.capy.wasm" >"$BUILD_DIR/markup.objdump"
grep -q 'env.bearer_format_s64' "$BUILD_DIR/markup.objdump"
grep -q 'env.bearer_format_u64' "$BUILD_DIR/markup.objdump"
grep -q 'env.bearer_format_f64' "$BUILD_DIR/markup.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-files.capy.wasm" >"$BUILD_DIR/files.objdump"
for import in file_open file_read file_write file_seek file_tell file_fsync file_close file_temp file_unlink; do
	grep -q "env.bearer_$import" "$BUILD_DIR/files.objdump"
done
"$BUILD_DIR/capyc" site/tests/capy-final-parity.capy -o "$BUILD_DIR/final-parity.wasm" --source-map "$BUILD_DIR/final-parity.wasm.source-map" --abi-version "$ABI_VERSION"
wasm-validate "$BUILD_DIR/final-parity.wasm"
wasm-objdump -x "$BUILD_DIR/final-parity.wasm" >"$BUILD_DIR/final-parity.objdump"
for import in file_pread file_pwrite; do grep -q "env.bearer_$import" "$BUILD_DIR/final-parity.objdump"; done
grep -q 'env.bearer_text_parsing_brrb' "$BUILD_DIR/final-parity.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-strings.capy.wasm" >"$BUILD_DIR/strings.objdump"
grep -q 'env.bearer_string_substr' "$BUILD_DIR/strings.objdump"
grep -q 'env.bearer_string_strpos' "$BUILD_DIR/strings.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-codecs.capy.wasm" >"$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_codec' "$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_dv_f64_to_brrb' "$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_dv_extract_f64' "$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_dv_extract_bool' "$BUILD_DIR/codecs.objdump"
! grep -q 'env.bearer_dv_f64_brrb\|env.bearer_dv_bool_brrb' "$BUILD_DIR/codecs.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-regex.capy.wasm" >"$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex_match' "$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex_dval' "$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex_text' "$BUILD_DIR/regex.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-coreutil.capy.wasm" >"$BUILD_DIR/coreutil.objdump"
for import in text_parsing_brrb route_path_brrb runtime_diagnostics_brrb; do
	grep -q "env.bearer_$import" "$BUILD_DIR/coreutil.objdump"
done
wasm-objdump -x "$BUILD_DIR/site_tests_capy-string-lists.capy.wasm" >"$BUILD_DIR/string-lists.objdump"
grep -q 'env.bearer_text_parsing_brrb' "$BUILD_DIR/string-lists.objdump"
grep -q 'env.bearer_string_nonblank' "$BUILD_DIR/string-lists.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-first-empty.capy.wasm" >"$BUILD_DIR/first-empty.objdump"
! grep -q 'env.bearer_string_nonblank' "$BUILD_DIR/first-empty.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-dval-merge.capy.wasm" >"$BUILD_DIR/dval-merge.objdump"
grep -q 'env.bearer_dv_merge_brrb' "$BUILD_DIR/dval-merge.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-sqlite.capy.wasm" >"$BUILD_DIR/sqlite.objdump"
for import in sqlite_connect sqlite_disconnect sqlite_error sqlite_query sqlite_insert_id sqlite_affected_rows; do
	grep -q "env.bearer_$import" "$BUILD_DIR/sqlite.objdump"
done
wasm-objdump -x "$BUILD_DIR/site_tests_capy-unit-admin.capy.wasm" >"$BUILD_DIR/unit-admin.objdump"
for import in unit_info_brrb units_list_brrb unit_compile; do
	grep -q "env.bearer_$import" "$BUILD_DIR/unit-admin.objdump"
done
"$BUILD_DIR/capyc" site/tests/capy-request-parity.capy -o "$BUILD_DIR/request-parity.wasm" --source-map "$BUILD_DIR/request-parity.wasm.source-map" --abi-version "$ABI_VERSION"
wasm-validate "$BUILD_DIR/request-parity.wasm"
"$BUILD_DIR/capyc" --check-unit "$BUILD_DIR/request-parity.wasm" "$ABI_VERSION"
wasm-objdump -x "$BUILD_DIR/request-parity.wasm" >"$BUILD_DIR/request-parity.objdump"
grep -q 'env.bearer_request_workspace_brrb' "$BUILD_DIR/request-parity.objdump"
! grep -q 'env.bearer_request_context_brrb\|env.bearer_redirect\|env.bearer_session_start' "$BUILD_DIR/request-parity.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-request-context.capy.wasm" >"$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_handler_input_brrb' "$BUILD_DIR/request-context.objdump"
! grep -q 'env.bearer_request_context_brrb\|env.bearer_request_context_for_brrb\|env.bearer_request_value\|env.bearer_request_body' "$BUILD_DIR/request-context.objdump"
! grep -q 'env.bearer_response_set_status' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_response_set_header' "$BUILD_DIR/request-context.objdump"
CAPYC="$BUILD_DIR/capyc" scripts/test_capy_artifact_golden.sh

echo "native Capy frontend, Wasm, compiler, CLI, and tracked fixture checks passed"
