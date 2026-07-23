#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
bin_directory="${BEARER_TEST_BIN_DIRECTORY:-/tmp/bearer/work}"
if [[ -r /etc/bearer/settings.cfg ]]; then
	configured_site=$(awk -F= '/^[[:space:]]*HTTP_DOCUMENT_ROOT[[:space:]]*=/ {sub(/^[^=]*=/, ""); print; exit}' /etc/bearer/settings.cfg)
	configured_bin=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {sub(/^[^=]*=/, ""); print; exit}' /etc/bearer/settings.cfg)
	[[ -n "${BEARER_TEST_SITE_DIRECTORY:-}" ]] || site_directory="${configured_site:-$site_directory}"
	[[ -n "${BEARER_TEST_BIN_DIRECTORY:-}" ]] || bin_directory="${configured_bin:-$bin_directory}"
fi
site_directory=$(realpath "$site_directory")

python3 scripts/test_capy_compiler.py >/dev/null
native_compiler_id=$(sha256sum src/capy/*.cpp src/capy/*.h src/lib/compiler.cpp | sha256sum | awk '{print $1}')
grep -aFq "$native_compiler_id" bin/bearer_fastcgi.linux.bin || {
	echo "Bearer binary does not contain the current native Capy compiler identity" >&2
	exit 1
}

output=$(scripts/bearer-cli /tests/capy-phase1.capy)
[[ "$output" == "capy-direct-ok" ]] || { echo "Capy CLI output mismatch: $output" >&2; exit 1; }

language_output=$(scripts/bearer-cli /tests/capy-language.capy)
[[ "$language_output" == "sum=30;012;ok;01" ]] || {
	echo "Capy functions/locals/control-flow output mismatch: $language_output" >&2
	exit 1
}
operator_output=$(scripts/bearer-cli /tests/capy-operators.capy)
[[ "$operator_output" == "0|1|X1|7|-5|1|00|10" ]] || {
	echo "Capy logical/unary/inferred-declaration output mismatch: $operator_output" >&2
	exit 1
}
dval_return_output=$(scripts/bearer-cli /tests/capy-dval-return.capy)
[[ "$dval_return_output" == "3|0|42|1|0|000|0|9|1|0" ]] || {
	echo "Capy dval-loop early-return ARC mismatch: $dval_return_output" >&2
	exit 1
}
markup_output=$(scripts/bearer-cli /tests/capy-markup.capy)
[[ "$markup_output" == "<p>static</p>|once;<main>&lt;side&gt;&lt;&amp;&gt;&quot;&#39;<strong>&lt;&amp;&gt;&quot;&#39;</strong><em>trusted</em><i>&lt;&amp;&gt;&quot;&#39;</i><aside>-2147483648:0:2147483647:true:false</aside></main>|0" ]] || {
	echo "Capy markup output mismatch: $markup_output" >&2
	exit 1
}
loop_control_output=$(scripts/bearer-cli /tests/capy-loop-control.capy)
[[ "$loop_control_output" == "7|owned-return|0|13|0|023|0|ab|0|0002||2022|0" ]] || {
	echo "Capy break/continue ARC output mismatch: $loop_control_output" >&2
	exit 1
}
closure_output=$(scripts/bearer-cli /tests/capy-closures.capy)
[[ "$closure_output" == "captured:15|2|second:22|2|5|2|second:23|3|temporary:31|3|immediate:42|3|nested:7|3|0" ]] || {
	echo "Capy closure output mismatch: $closure_output" >&2
	exit 1
}
phase3_output=$(scripts/bearer-cli /tests/capy-phase3.capy)
[[ "$phase3_output" == "7|generic|5|fallback|2|name|9tuple|2|tuple|0|tuple|0|tuple|3|innerouter|4|temporary|0|nested|0|5-1tuple|0|0|011|0" ]] || {
	echo "Capy generic specialization output mismatch: $phase3_output" >&2
	exit 1
}
phase3_cache="$(scripts/unit_cache_directory "$bin_directory")$site_directory/tests/capy-phase3.capy"
wasm-validate "$phase3_cache.wasm"
[[ "$(scripts/bearer-cli /tests/capy-cross.capy)" == "cpp-render-ok|roundtripother|1|3|0" ]] || {
	echo "Capy-to-C++ Bearer unit dispatch failed" >&2
	exit 1
}
[[ "$(scripts/bearer-cli /tests/capy-cross-caller.uce)" == "capy-component-ok|capy-named-ok" ]] || {
	echo "C++-to-Capy Bearer component dispatch failed" >&2
	exit 1
}
request_component_output=$(scripts/bearer-cli /tests/capy-request-context-caller.uce)
[[ "$request_component_output" == "component-prop|1" ]] || {
	echo "Capy request props snapshot mismatch: $request_component_output" >&2
	exit 1
}
request_headers=$(mktemp)
request_http_output=$(curl -fsS --max-time 30 -D "$request_headers" -H 'Host: bearer.openfu.com' -d 'answer=42' 'http://127.0.0.1/tests/capy-request-context.capy?name=Ada')
[[ "$request_http_output" == "POST|Ada|42|answer=42|1" ]] || {
	echo "Capy HTTP request snapshot mismatch: $request_http_output" >&2
	exit 1
}
grep -Eq '^HTTP/1\.[01] 201 ' "$request_headers"
grep -Eqi '^X-Capy-Context: yes' "$request_headers"
grep -Eqi '^X-Capy-Clean: first  X-Injected: bad' "$request_headers"
! grep -Eqi '^X-Injected:' "$request_headers"
rm -f "$request_headers"
request_http_second=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' -d 'answer=7' 'http://127.0.0.1/tests/capy-request-context.capy?name=Grace')
[[ "$request_http_second" == "POST|Grace|7|answer=7|1" ]] || {
	echo "Capy request isolation mismatch: $request_http_second" >&2
	exit 1
}
response_trap_body=$(mktemp)
response_trap_status=$(curl -sS --max-time 30 -o "$response_trap_body" -w '%{http_code}' -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-response-header-trap.capy)
rm -f "$response_trap_body"
[[ "$response_trap_status" == "500" ]] || {
	echo "Capy malformed response header did not fail: HTTP $response_trap_status" >&2
	exit 1
}
set +e
status_trap_output=$(scripts/bearer-cli /tests/capy-response-status-trap.capy 2>&1)
status_trap_result=$?
set -e
[[ $status_trap_result -ne 0 && "$status_trap_output" == *"capy-response-status-trap.capy:2:5"* ]] || {
	echo "Capy invalid response status trap/source mismatch: status=$status_trap_result output=$status_trap_output" >&2
	exit 1
}
status_http_body=$(mktemp)
status_http_result=$(curl -sS --max-time 30 -o "$status_http_body" -w '%{http_code}' -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-response-status-trap.capy)
rm -f "$status_http_body"
[[ "$status_http_result" == "500" ]] || {
	echo "Capy invalid HTTP response status did not fail: HTTP $status_http_result" >&2
	exit 1
}
[[ "$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' -d 'answer=8' 'http://127.0.0.1/tests/capy-request-context.capy?name=Reset')" == "POST|Reset|8|answer=8|1" ]] || {
	echo "Capy request workspace did not recover after response trap" >&2
	exit 1
}
rich_dval_output=$(scripts/bearer-cli /tests/capy-dval-rich.capy)
[[ "$rich_dval_output" == "cpp|Ada|9|custom-once;capy|capy|Ada|42|1|logic|10|3|active;age;name;tags;|0=math;1=logic;|2;|3|0|00|2|0" ]] || {
	echo "Capy rich DValue output mismatch: $rich_dval_output" >&2
	exit 1
}
[[ "$(scripts/bearer-cli /tests/capy-dval-rich-caller.uce)" == "custom-once;capy|C++|roundtrip|3" ]] || {
	echo "C++-to-Capy custom DValue export failed" >&2
	exit 1
}
set +e
dval_trap_output=$(scripts/bearer-cli /tests/capy-dval-missing-trap.capy 2>&1)
dval_trap_status=$?
set -e
[[ $dval_trap_status -ne 0 && "$dval_trap_output" == *'wasm `unreachable` instruction executed'* && "$dval_trap_output" == *'capy-dval-missing-trap.capy:3:29'* ]] || {
	echo "Capy missing DValue trap mismatch: status=$dval_trap_status output=$dval_trap_output" >&2
	exit 1
}
[[ "$(scripts/bearer-cli /tests/capy-dval-rich.capy)" == "$rich_dval_output" ]] || {
	echo "Capy DValue workspace did not reset after trap" >&2
	exit 1
}
for fixture_and_location in \
	"capy-dval-negative-trap:3:32" \
	"capy-dval-range-trap:3:30" \
	"capy-dval-scalar-trap:3:29"; do
	fixture=${fixture_and_location%%:*}
	location=${fixture_and_location#*:}
	set +e
	strict_trap_output=$(scripts/bearer-cli "/tests/$fixture.capy" 2>&1)
	strict_trap_status=$?
	set -e
	[[ $strict_trap_status -ne 0 && "$strict_trap_output" == *"$fixture.capy:$location"* ]] || {
		echo "Capy strict DValue trap mismatch: fixture=$fixture output=$strict_trap_output" >&2
		exit 1
	}
	[[ "$(scripts/bearer-cli /tests/capy-dval-rich.capy)" == "$rich_dval_output" ]] || {
		echo "Capy DValue workspace did not reset after $fixture" >&2
		exit 1
	}
done
arc_output=$(scripts/bearer-cli /tests/capy-arc.capy)
expected_arc='first|0|alphaalpha|1|1|1|789|1|8|2|4|1|12|3|ownedtwo|4|temp|1|picked|1|4|2|pair42|3|pair|6|inside|9|field|1|betaalpha|2|betaalphaalphabeta|3|tempz|4|double|5|nested|6|5|0'
[[ "$arc_output" == "$expected_arc" ]] || {
	echo "Capy ARC ownership output mismatch: $arc_output" >&2
	exit 1
}
arc_cache="$(scripts/unit_cache_directory "$bin_directory")$site_directory/tests/capy-arc.capy"
wasm-validate "$arc_cache.wasm"
wasm-objdump -x "$arc_cache.wasm" >"$arc_cache.objdump"
grep -q 'env.bearer_alloc' "$arc_cache.objdump"
grep -q 'env.bearer_free' "$arc_cache.objdump"
grep -Eq 'mem_p2align *: 3' "$arc_cache.objdump"
rm -f "$arc_cache.objdump"
set +e
trap_output=$(scripts/bearer-cli /tests/capy-arc-trap.capy 2>&1)
trap_status=$?
set -e
[[ $trap_status -ne 0 && "$trap_output" == *'wasm `unreachable` instruction executed'* && "$trap_output" == *'capy-arc-trap.capy'* ]] || {
	echo "Capy ARC trap containment did not produce a source-associated trap" >&2
	echo "$trap_output" >&2
	exit 1
}
[[ "$(scripts/bearer-cli /tests/capy-arc.capy)" == "$expected_arc" ]] || {
	echo "Capy ARC workspace did not reset after a trapping request" >&2
	exit 1
}
for array_trap in capy-array-trap capy-array-negative-trap; do
	set +e
	array_trap_output=$(scripts/bearer-cli "/tests/$array_trap.capy" 2>&1)
	array_trap_status=$?
	set -e
	[[ $array_trap_status -ne 0 && "$array_trap_output" == *"$array_trap.capy:2:14"* ]] || {
		echo "Capy array bounds trap/source mapping mismatch: fixture=$array_trap status=$array_trap_status output=$array_trap_output" >&2
		exit 1
	}
	[[ "$(scripts/bearer-cli /tests/capy-arc.capy)" == "$expected_arc" ]] || {
		echo "Capy ARC workspace did not reset after array bounds trap $array_trap" >&2
		exit 1
	}
done
render_output=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-render.capy)
[[ "$render_output" == "capy-render-ok" ]] || {
	echo "Capy HTTP RENDER output mismatch: $render_output" >&2
	exit 1
}

cache="$(scripts/unit_cache_directory "$bin_directory")$site_directory/tests/capy-phase1.capy"
[[ -s "$cache.wasm" && -s "$cache.cwasm" && -s "$cache.wasm.source-map" && -s "$cache.meta.txt" ]]
python3 scripts/check_unit_wasm.py "$cache.wasm" --abi-version "$(awk '/BEARER_WASM_CORE_ABI_VERSION/ {print $3; exit}' src/wasm/abi.h)"
wasm-objdump -x "$cache.wasm" >"$cache.objdump"
grep -q 'env.bearer_print_bytes' "$cache.objdump"
grep -q '__bearer_cli' "$cache.objdump"
grep -Eq 'mem_p2align *: 3' "$cache.objdump"
! grep -q 'wasi_snapshot_preview1' "$cache.objdump"
rm -f "$cache.objdump"

fixture="capy-compile-recovery-$$"
source_dir="$site_directory/$fixture"
artifact_dir="$(scripts/unit_cache_directory "$bin_directory")$source_dir"
cleanup() {
	rm -rf "$source_dir" "$artifact_dir"
}
trap cleanup EXIT

mkdir -p "$source_dir"
for size in 63 64 127 128; do
	payload=$(printf '%*s' "$size" '' | tr ' ' x)
	printf 'function CLI { print("%s") }\n' "$payload" >"$source_dir/entry.capy"
	[[ "$(scripts/bearer-cli "/$fixture/entry.capy")" == "$payload" ]] || {
		echo "Capy signed-LEB output boundary failed at $size bytes" >&2
		exit 1
	}
done
render_prefix=$(printf '%*s' 64 '' | tr ' ' r)
printf 'function RENDER { print("%s") }\nfunction CLI { print("offset-ok") }\n' "$render_prefix" >"$source_dir/entry.capy"
[[ "$(scripts/bearer-cli "/$fixture/entry.capy")" == "offset-ok" ]] || {
	echo "Capy signed-LEB data offset boundary failed" >&2
	exit 1
}

printf '%s\n' 'function CLI { print(not_a_constant) }' >"$source_dir/entry.capy"
set +e
failure=$(scripts/bearer-cli "/$fixture/entry.capy" 2>&1)
status=$?
set -e
[[ $status -ne 0 ]]
[[ "$failure" == *"entry.capy:1:"* && "$failure" == *"unknown local 'not_a_constant'"* ]]
[[ ! -e "$artifact_dir/entry.capy.wasm" ]]

printf '%s\n' 'function CLI { print("capy-recovered") }' >"$source_dir/entry.capy"
[[ "$(scripts/bearer-cli "/$fixture/entry.capy")" == "capy-recovered" ]]
[[ -s "$artifact_dir/entry.capy.wasm" ]]

echo "Capy phase 1 parser/direct-Wasm/CLI smoke passed"
