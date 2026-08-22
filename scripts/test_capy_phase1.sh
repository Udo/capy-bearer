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

expect_equal() {
	local case_name=$1 expected=$2 actual=$3
	if [[ "$actual" != "$expected" ]]; then
		printf '%s mismatch\nexpected: %q\nactual:   %q\n' "$case_name" "$expected" "$actual" >&2
		exit 1
	fi
}

native_compiler_id=$(sha256sum src/capy/*.cpp src/capy/*.h src/lib/compiler.cpp src/lib/markup-context.h | sha256sum | awk '{print $1}')
grep -aFq "$native_compiler_id" bin/bearer_fastcgi.linux.bin || {
	echo "Bearer binary does not contain the current native Capy compiler identity" >&2
	exit 1
}
stdlib_digest=$(sha256sum src/capy/stdlib.capy | awk '{print $1}')
grep -aFq "$stdlib_digest" bin/bearer_fastcgi.linux.bin || {
	echo "Bearer binary does not contain the current embedded Capy standard library" >&2
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
[[ "$operator_output" == "false|true|Xtrue|7|-5|true|false0|true0" ]] || {
	echo "Capy logical/unary/inferred-declaration output mismatch: $operator_output" >&2
	exit 1
}
expect_equal "newline-independent expression parsing" "7|3|9" "$(scripts/bearer-cli /tests/capy-whitespace.capy)"
consistency_expected="1|-2|3|4.5|x|10.5|3|4|left|right|early|late|0"
expect_equal "type aliases, value blocks, conditional values, and wide aggregates" \
	"$consistency_expected" "$(scripts/bearer-cli /tests/capy-language-consistency.capy)"
(
	value_trap_dir=$(mktemp -d "$site_directory/tests/capy-value-trap.XXXXXX")
	trap 'rm -rf -- "$value_trap_dir"' EXIT
	value_trap_name=${value_trap_dir##*/}
	cat >"$value_trap_dir/then.capy" <<'EOF'
function CLI(request : dval) {
    var value := if true { -> [1][1] } else { -> [1][0] }
    print(value)
}
EOF
	cat >"$value_trap_dir/else.capy" <<'EOF'
function CLI(request : dval) {
    var value := if false { -> [1][0] } else { -> [1][1] }
    print(value)
}
EOF
	cat >"$value_trap_dir/block.capy" <<'EOF'
function CLI(request : dval) {
    var value := { var values := [1]; -> values[1] }
    print(value)
}
EOF
	for case in then:2:34 else:2:54 block:2:48; do
		name=${case%%:*}; location=${case#*:}
		set +e
		value_trap_output=$(scripts/bearer-cli "/tests/$value_trap_name/$name.capy" 2>&1)
		value_trap_status=$?
		set -e
		[[ $value_trap_status -ne 0 && "$value_trap_output" == *"$name.capy:$location"* ]] || {
			echo "Capy $name value trap/source mapping mismatch: $value_trap_output" >&2
			exit 1
		}
		expect_equal "$name value trap recovery" "$consistency_expected" "$(scripts/bearer-cli /tests/capy-language-consistency.capy)"
	done
)
coercion_expected="7|8|42|-9|10|4.25|true|false|12|false|1|2|3|9|4|0"
expect_equal "declared parameter coercion and constructors" "$coercion_expected" "$(scripts/bearer-cli /tests/capy-coercion.capy)"
vector_expected="7|token|8|A|Ab|inside3|insideb|x|local|insidebz|0|8|2|1.5|c|12|9|p|a2true|b3|4c|4|4c|imir|function-print6|0"
expect_equal "constructors, shared vector arrays, variadics, splats, function values, and ARC" \
	"$vector_expected" "$(scripts/bearer-cli /tests/capy-vector-variadic.capy)"
mutable_aggregate_expected="98335618|0|thirdfirstsecond1|aBaBctemporaryaB|2|12333|10213245789m0q|12967by|1291233abz0|89000000001parameter6new412|0"
expect_equal "stable mutable arrays and struct fields" \
	"$mutable_aggregate_expected" "$(scripts/bearer-cli /tests/capy-mutable-array-struct.capy)"
expect_equal "managed parameter rebinding, nibble, and aggregate mutation" \
	"go!|go|api:api/length|cc/dd?:cc/dd:zz|0|78|425|9|8" "$(scripts/bearer-cli /tests/capy-managed-parameter-rebind.capy)"
(
	cycle_dir=$(mktemp -d "$site_directory/tests/capy-cycle.XXXXXX")
	trap 'rm -rf -- "$cycle_dir"' EXIT
	cat >"$cycle_dir/entry.capy" <<'EOF'
struct Cycle { children : [Cycle] }
function empty_cycles() [Cycle] { var values : [Cycle] = []; -> values }
function CLI(request : dval) { var cycle := Cycle(empty_cycles()); cycle.children.push(cycle); print(arc_live() > 0) }
EOF
	expect_equal "characterized strong aggregate cycle" "true" "$(scripts/bearer-cli "/tests/${cycle_dir##*/}/entry.capy")"
	expect_equal "clean request after strong aggregate cycle" "$mutable_aggregate_expected" "$(scripts/bearer-cli /tests/capy-mutable-array-struct.capy)"
)
(
	vector_trap_dir=$(mktemp -d "$site_directory/tests/capy-vector-traps.XXXXXX")
	trap 'rm -rf -- "$vector_trap_dir"' EXIT
	for case in \
		'pop|function CLI(request : dval) { var values : [s32] = []; values.pop() }' \
		'insert|function CLI(request : dval) { var values := [1]; values.insert(2, 2) }' \
		'remove|function CLI(request : dval) { var values := [1]; values.remove(-1) }' \
		'reserve|function CLI(request : dval) { var values := [1]; values.reserve(-1) }' \
		'resize|function CLI(request : dval) { var values := [1]; values.resize(-1, 0) }'; do
		name=${case%%|*}; source=${case#*|}
		printf '%s\n' "$source" >"$vector_trap_dir/$name.capy"
		set +e
		output=$(scripts/bearer-cli "/tests/${vector_trap_dir##*/}/$name.capy" 2>&1)
		status=$?
		set -e
		[[ $status -ne 0 && "$output" == *"$name.capy:1:"* && "$output" != *"capy://stdlib.capy"* ]]
		expect_equal "vector trap recovery after $name" "$vector_expected" "$(scripts/bearer-cli /tests/capy-vector-variadic.capy)"
	done
)
(
	coercion_trap_dir=$(mktemp -d "$site_directory/tests/capy-coercion-trap.XXXXXX")
	trap 'rm -rf -- "$coercion_trap_dir"' EXIT
	coercion_trap_name=${coercion_trap_dir##*/}
	cat >"$coercion_trap_dir/entry.capy" <<'EOF'
function narrow(value : as s64) s64 { -> value }
function CLI(request : dval) { print(narrow(1e300)) }
EOF
	set +e
	coercion_trap_output=$(scripts/bearer-cli "/tests/$coercion_trap_name/entry.capy" 2>&1)
	coercion_trap_status=$?
	set -e
	[[ $coercion_trap_status -ne 0 && "$coercion_trap_output" == *"integer overflow"* && "$coercion_trap_output" == *"entry.capy:2:45"* ]] || {
		echo "Capy parameter conversion trap/source mapping mismatch: $coercion_trap_output" >&2
		exit 1
	}
	expect_equal "parameter conversion trap recovery" "$coercion_expected" "$(scripts/bearer-cli /tests/capy-coercion.capy)"
)

variable_expression_output=$(scripts/bearer-cli /tests/capy-variable-expressions.capy)
[[ "$variable_expression_output" == "var;true;typed;true;inferred;true;assigned;true;once;once-value;true;false;visible;equal;passed;call;and;or;2;2;0;2;0;returned;implicit;0" ]] || {
	echo "Capy value-producing declaration output/ARC mismatch: $variable_expression_output" >&2
	exit 1
}
(
	variable_trap_dir=$(mktemp -d "$site_directory/tests/capy-variable-trap.XXXXXX")
	trap 'rm -rf -- "$variable_trap_dir"' EXIT
	variable_trap_name=${variable_trap_dir##*/}
	cat >"$variable_trap_dir/entry.capy" <<'EOF'
function CLI(request : dval) {
    var owned := clone("owned")
    while var again := true { var value := [1][1] }
}
EOF
	set +e
	variable_trap_output=$(scripts/bearer-cli "/tests/$variable_trap_name/entry.capy" 2>&1)
	variable_trap_status=$?
	set -e
	[[ $variable_trap_status -ne 0 && "$variable_trap_output" == *"entry.capy:3:47"* ]] || {
		echo "Capy declaration initializer trap/source mapping mismatch: $variable_trap_output" >&2
		exit 1
	}
	expect_equal "declaration initializer trap recovery" "$variable_expression_output" "$(scripts/bearer-cli /tests/capy-variable-expressions.capy)"
)
backtrace_output=$(scripts/bearer-cli /tests/capy-backtrace.capy)
expected_backtrace=$'#0 __lambda_0 at /Code/capy-bearer/site/tests/capy-backtrace.capy:5:20\n#1 inner at /Code/capy-bearer/site/tests/capy-backtrace.capy:4:1\n#2 outer at /Code/capy-bearer/site/tests/capy-backtrace.capy:1:1\n#3 CLI at /Code/capy-bearer/site/tests/capy-backtrace.capy:24:1\n==\n#0 inner at /Code/capy-bearer/site/tests/capy-backtrace.capy:4:1\n#1 outer at /Code/capy-bearer/site/tests/capy-backtrace.capy:1:1\n--\n|0\n++\n#0 explicit_return_trace at /Code/capy-bearer/site/tests/capy-backtrace.capy:14:1\n#1 CLI at /Code/capy-bearer/site/tests/capy-backtrace.capy:24:1\n~~\n#0 CLI at /Code/capy-bearer/site/tests/capy-backtrace.capy:24:1\n@@\n|#0 bounds at /Code/capy-bearer/site/tests/capy-backtrace.capy:17:1\n#1 CLI at /Code/capy-bearer/site/tests/capy-backtrace.capy:24:1||#0 bounds at /Code/capy-bearer/site/tests/capy-backtrace.capy:17:1\n#1 CLI at /Code/capy-bearer/site/tests/capy-backtrace.capy:24:1'
expect_equal "Capy source-mapped guest backtrace" "$expected_backtrace" "$backtrace_output"
(
	backtrace_test_dir=$(mktemp -d "$site_directory/tests/capy-backtrace-test.XXXXXX")
	trap 'rm -rf -- "$backtrace_test_dir"' EXIT
	backtrace_test_name=${backtrace_test_dir##*/}
	{
		printf 'function tail() string { -> backtrace_get_frames(2147483647, 0) }\n'
		for ((frame = 0; frame < 300; ++frame)); do
			if ((frame == 299)); then
				printf 'function ring_%d() string { -> tail() }\n' "$frame"
			else
				printf 'function ring_%d() string { -> ring_%d() }\n' "$frame" "$((frame + 1))"
			fi
		done
		printf 'function CLI(request : dval) { print(ring_0()) }\n'
	} > "$backtrace_test_dir/ring.capy"
	ring_output=$(scripts/bearer-cli "/tests/$backtrace_test_name/ring.capy")
	mapfile -t ring_frames <<< "$ring_output"
	[[ ${#ring_frames[@]} -eq 256 && ${#ring_output} -gt 4096 ]] || {
		echo "Capy backtrace ring/two-pass output bounds mismatch" >&2
		exit 1
	}
	expect_equal "Capy backtrace ring newest frame" "#0 tail at $site_directory/tests/$backtrace_test_name/ring.capy:1:1" "${ring_frames[0]}"
	expect_equal "Capy backtrace ring oldest retained frame" "#255 ring_45 at $site_directory/tests/$backtrace_test_name/ring.capy:47:1" "${ring_frames[255]}"
)
dval_return_output=$(scripts/bearer-cli /tests/capy-dval-return.capy)
[[ "$dval_return_output" == "3|0|42|1|0|000|0|9|1|0" ]] || {
	echo "Capy dval-loop early-return ARC mismatch: $dval_return_output" >&2
	exit 1
}
wide_output=$(scripts/bearer-cli /tests/capy-wide-scalars.capy)
[[ "$wide_output" == "8|8|9|9|18446744073709551615|-9223372036854775808|-1|-3|-1|true|true|3|2|true|125|-1.5|5|3.5|18446744073709551615|9|-1|true|truetrue|inf|falsetrue|-0|0" ]] || {
	echo "Capy wide scalar operations mismatch: $wide_output" >&2
	exit 1
}
set +e
wide_trap_output=$(scripts/bearer-cli /tests/capy-wide-conversion-trap.capy 2>&1)
wide_trap_status=$?
set -e
[[ $wide_trap_status -ne 0 && "$wide_trap_output" == *"integer overflow"* && "$wide_trap_output" == *"capy-wide-conversion-trap.capy:2:11"* ]]
expect_equal "wide scalar recovery" "$wide_output" "$(scripts/bearer-cli /tests/capy-wide-scalars.capy)"
rm -f /tmp/capy-files-phase-* /tmp/capy-files2-*
file_output=$(scripts/bearer-cli /tests/capy-files.capy)
[[ "$file_output" == "true|5|0|capy!|5|true|2|0|true|true" ]] || {
	echo "Capy file handle/ARC mismatch: $file_output" >&2
	exit 1
}
filesystem_output=$(scripts/bearer-cli /tests/capy-files2.capy)
[[ "$filesystem_output" == "a.txt|true|truetruetruetruetruetruetruetrue|abc|ab|abc|truetrue3|true|a.txt,b.txt,link.txt|truefalsetruetruetruetruetrue|true|true|true|true" ]] || {
	echo "Capy filesystem stdlib/policy mismatch: $filesystem_output" >&2
	exit 1
}
expect_equal "filesystem recovery" "$filesystem_output" "$(scripts/bearer-cli /tests/capy-files2.capy)"
expect_equal "file/archive/split_kv adapters" "3|heXYZ|Ada:admin|archive data|true2truetruetrue|9|0|true" "$(scripts/bearer-cli /tests/capy-final-parity.capy)"
set +e
archive_trap_output=$(scripts/bearer-cli /tests/capy-archive-trap.capy 2>&1)
archive_trap_status=$?
set -e
[[ $archive_trap_status -ne 0 && "$archive_trap_output" == *"capy-archive-trap.capy:1:38"* && "$archive_trap_output" != *"capy://stdlib.capy"* ]]
expect_equal "archive recovery" "3|heXYZ|Ada:admin|archive data|true2truetruetrue|9|0|true" "$(scripts/bearer-cli /tests/capy-final-parity.capy)"
expect_equal "live MySQL stdlib adapters" "truetrue|'a\\'b'|7|true|0:0|truetruetrue|true|truetruetrue|0" "$(scripts/bearer-cli /tests/capy-mysql.capy)"
expect_equal "memcache stdlib adapters" "a_b|line_key|down|live|0" "$(scripts/bearer-cli /tests/capy-memcache.capy)"
expect_equal "job/process stdlib adapters" "shell|spawned|done|done|done|cancel|cancelled|tasks|cwd|server|0" "$(scripts/bearer-cli /tests/capy-jobs.capy)"
expect_equal "named task worker cancellation/failure/recovery and copied props" "canceled|stopped|failed:handler_trap|failed:missing_handler|succeeded:none|succeeded:before" "$(scripts/bearer-cli /tests/capy-task-runtime.capy)"
expect_equal "unit administration" "true|true|true|2|0" "$(scripts/bearer-cli /tests/capy-unit-admin.capy)"
expect_equal "DValue array merge" "rightyesnewright|abcd|valueitem|right|value|z|oddkeep|map|unicode|9|0" "$(scripts/bearer-cli /tests/capy-dval-merge.capy)"
expect_equal "DValue value API" 'truefalseAda42tail|trueone|Bearerfalse|truearray"A"|ftruey|truefalseAdanameengineer|23truefalse|-4242-918446744073709551615|0' "$(scripts/bearer-cli /tests/capy-dval-api.capy)"
set +e
dval_api_trap_output=$(scripts/bearer-cli /tests/capy-dval-api-trap.capy 2>&1)
dval_api_trap_status=$?
set -e
[[ $dval_api_trap_status -ne 0 && "$dval_api_trap_output" == *"capy-dval-api-trap.capy:2:5"* ]] || {
	echo "Capy DValue API trap/source mapping mismatch: $dval_api_trap_output" >&2
	exit 1
}
expect_equal "DValue value API recovery" 'truefalseAda42tail|trueone|Bearerfalse|truearray"A"|ftruey|truefalseAdanameengineer|23truefalse|-4242-918446744073709551615|0' "$(scripts/bearer-cli /tests/capy-dval-api.capy)"
expect_equal "shared mutable DValue identity, live loops, range bindings, codec copies, and ARC" \
	'SR|289secondyesyesyesyesvalue10|newnewtruetruetrue|1falsefalsetrueitemtruearraynewnewvalue1|true|102132|1021|0112|7a|3|sourceresult|0' "$(scripts/bearer-cli /tests/capy-dval-identity.capy)"
expect_equal "nested closure string DValue ARC" "BETA|ALPHA|0" "$(scripts/bearer-cli /tests/capy-map-arc.capy)"
sqlite_output=$(scripts/bearer-cli /tests/capy-sqlite.capy)
expect_equal "SQLite lifecycle/query" "11|1Ada|true|1|0" "$sqlite_output"
expect_equal "SQLite failed-connect diagnostic" "true|0" "$(scripts/bearer-cli /tests/capy-sqlite-failed.capy)"
for sqlite_case in \
	"capy-sqlite-handle-trap|4:11" \
	"capy-sqlite-params-trap|3:5" \
	"capy-sqlite-zero-trap|2:11"; do
	sqlite_fixture=${sqlite_case%%|*}
	sqlite_location=${sqlite_case#*|}
	set +e
	sqlite_trap_output=$(scripts/bearer-cli "/tests/$sqlite_fixture.capy" 2>&1)
	sqlite_trap_status=$?
	set -e
	[[ $sqlite_trap_status -ne 0 && "$sqlite_trap_output" == *"$sqlite_fixture.capy:$sqlite_location"* && "$sqlite_trap_output" != *"capy://stdlib.capy"* ]]
done
set +e
sized_host_trap_output=$(scripts/bearer-cli /tests/capy-sqlite-sized-host-trap.capy 2>&1)
sized_host_trap_status=$?
set -e
[[ $sized_host_trap_status -ne 0 && "$sized_host_trap_output" == *"capy-sqlite-sized-host-trap.capy:4:5"* && "$sized_host_trap_output" != *"capy://stdlib.capy" ]] || {
	echo "Capy generic sized-host failure did not trap at the owned-input callsite: $sized_host_trap_output" >&2
	exit 1
}
expect_equal "generic sized-host failure recovery/ARC" "$sqlite_output" "$(scripts/bearer-cli /tests/capy-sqlite.capy)"
codec_output=$(scripts/bearer-cli /tests/capy-codecs.capy)
[[ "$codec_output" == "<Ada>|1.5true|Q2FweSE=|Capy!|0|a%20b%26|a b&|&lt;&amp;&gt;&quot;&#39;|{}|3|0" ]] || {
	echo "Capy codec/JSON/ARC mismatch: $codec_output" >&2
	exit 1
}
expect_equal "invalid f64 constructor fallback" "0" "$(scripts/bearer-cli /tests/capy-dval-f64-trap.capy)"
expect_equal "codec recovery" "$codec_output" "$(scripts/bearer-cli /tests/capy-codecs.capy)"
expect_equal "codec/text stdlib adapters" 'Ada|Ada|6|Ada|10|document|<p><strong>bold</strong></p>|3|"x"|{"name": "Ada"}|&lt;&amp;&gt;&quot;|8|0' "$(scripts/bearer-cli /tests/capy-codecs-text.capy)"
set +e
codec_text_trap_output=$(scripts/bearer-cli /tests/capy-codecs-text-trap.capy 2>&1)
codec_text_trap_status=$?
set -e
[[ $codec_text_trap_status -ne 0 && "$codec_text_trap_output" == *"capy-codecs-text-trap.capy:1:32"* ]] || {
	echo "Capy malformed XML did not trap at the call site: $codec_text_trap_output" >&2
	exit 1
}
crypto_output=$(scripts/bearer-cli /tests/capy-crypto-random.capy)
[[ "$crypto_output" =~ ^A9993E364706816ABA3E25717850C26C9CD0D89D\|20\|32\|ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\|32\|6e9ef29b75fffc5b7abae527d58fdadb2fe42e7219011976917343065f58ed4a\|truefalse\|16\|truefalsefalse\|1198951227\|9546680768582587775\|true\|7\|true\|true\|true\|[0-9]+\|0$ ]] || {
	echo "Capy crypto/random adapter mismatch: $crypto_output" >&2
	exit 1
}
regex_output=$(scripts/bearer-cli /tests/capy-regex.capy)
[[ "$regex_output" == "true|Capy|4|Capy|3TWO|false|<one> <TWO>|a::b|truetruetruefalse|false|2|:a:b:|0" ]] || {
	echo "Capy regex/ARC mismatch: $regex_output" >&2
	exit 1
}
for regex_case in \
	"capy-regex-trap|could not compile pattern" \
	"capy-regex-flag-trap|unknown regex flag 'q'" \
	"capy-regex-replace-trap|substitution failed: unknown substring"; do
	regex_trap=${regex_case%%|*}
	regex_error=${regex_case#*|}
	set +e
	regex_trap_output=$(scripts/bearer-cli "/tests/$regex_trap.capy" 2>&1)
	regex_trap_status=$?
	set -e
	[[ $regex_trap_status -ne 0 && "$regex_trap_output" == *"$regex_error"* && "$regex_trap_output" == *"$regex_trap.capy:2:11"* ]]
done
expect_equal "regex recovery" "$regex_output" "$(scripts/bearer-cli /tests/capy-regex.capy)"
if compgen -G '/tmp/capy-files-phase-*' >/dev/null || compgen -G '/tmp/capy-files2-*' >/dev/null; then
	echo "Capy filesystem fixture left temporary files" >&2
	rm -f /tmp/capy-files-phase-* /tmp/capy-files2-*
	exit 1
fi
expect_equal "concat-only demand surface" "ab|0" "$(scripts/bearer-cli /tests/capy-string-concat-only.capy)"
expect_equal "zero-argument first" "[]|0" "$(scripts/bearer-cli /tests/capy-first-empty.capy)"
string_list_output=$(scripts/bearer-cli /tests/capy-string-lists.capy)
[[ "$string_list_output" == "a::b:|a,,b,|true|Capy|||Gr:ße||one|value|x-y|[]|[ keep ]|chosen|eplpick|5|0" ]] || {
	echo "Capy split/join/ARC mismatch: $string_list_output" >&2
	exit 1
}
expect_equal "numeric DValue list join conversion" "1" "$(scripts/bearer-cli /tests/capy-string-list-trap.capy)"
set +e
string_list_trap_output=$(scripts/bearer-cli /tests/capy-string-list-scalar-trap.capy 2>&1)
string_list_trap_status=$?
set -e
[[ $string_list_trap_status -ne 0 && "$string_list_trap_output" == *"capy-string-list-scalar-trap.capy:2:11"* ]]
expect_equal "split/join recovery" "$string_list_output" "$(scripts/bearer-cli /tests/capy-string-lists.capy)"
expect_equal "core utility value adapters" "falsetruefalseBETA,ALPHA,BETA|alpha|beta,alpha|truetrue|alpha|0,1,2|bearer:BEARER|Caffile1:Caffile1:core|3.5:255:truetrue|1.5:-9:42|truetrue|a=one&b=last&flag=:last:example.test:docs:two|docs/index:true:|one,two,three:A,é:example.test|alpha,beta,beta:true:SIGSEGV:0:true:40|0" "$(scripts/bearer-cli /tests/capy-coreutil.capy)"
set +e
coreutil_trap_output=$(scripts/bearer-cli /tests/capy-coreutil-trap.capy 2>&1)
coreutil_trap_status=$?
set -e
[[ $coreutil_trap_status -ne 0 && "$coreutil_trap_output" == *"capy-coreutil-trap.capy:1:32"* ]] || {
	echo "Capy negative usleep did not trap at call site: $coreutil_trap_output" >&2
	exit 1
}
string_output=$(scripts/bearer-cli /tests/capy-strings.capy)
[[ "$string_output" == "Capy Bearer|11|truetrue|Bearer|Bearer|Capy:Bearer|5:5:-1|5:-1|Capy Runtime|capy bearer|CAPY BEARER|3|truefalsetrue|3|0" ]] || {
	echo "Capy string operations/ARC mismatch: $string_output" >&2
	exit 1
}
markup_output=$(scripts/bearer-cli /tests/capy-markup.capy)
[[ "$markup_output" == '<p>static</p>|once;<main>&lt;side&gt;&lt;&amp;&gt;&quot;&#39;<strong>&lt;&amp;&gt;&quot;&#39;</strong><em>trusted</em><trust><em>trusted</em></trust><trusted-context title="&lt;em&gt;trusted&lt;/em&gt;"><script>const trusted = "\u003Cem\u003Etrusted\u003C\/em\u003E";</script><style>.trusted::after { content: "\3C em\3E trusted\3C /em\3E "; }</style></trusted-context><i>&lt;&amp;&gt;&quot;&#39;</i><aside>-2147483648:0:2147483647:true:false</aside><wide>-9223372036854775808:18446744073709551615:-1.5</wide></main>|-2147483648|0' ]] || {
	echo "Capy markup output mismatch: $markup_output" >&2
	exit 1
}
context_markup=$(scripts/bearer-cli /tests/capy-markup-context.capy)
[[ "$context_markup" == once\;* && "${context_markup#once;}" != *'once;'* &&
	"$context_markup" == *'&lt;once&gt;<p title="&lt;/script&gt;&lt;style&gt;&amp;&quot;&#39;'* &&
	"$context_markup" == *'\u003C\/script\u003E'* && "$context_markup" == *'\u000A\u2028\u2029'* &&
	"$context_markup" == *'\3C /script\3E '* && "$context_markup" == *'\A \2028 \2029 '* &&
	"$context_markup" == *'|0' ]] || {
	echo "Contextual markup escaping mismatch: $context_markup" >&2
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
[[ "$phase3_output" == "7|generic|5|fallback|2|name|9tuple|2|tuple|0|tuple|0|tuple|3|innerouter|4|temporary|0|nested|0|5-1tuple|0|0|falsetrue1|0" ]] || {
	echo "Capy generic specialization output mismatch: $phase3_output" >&2
	exit 1
}
phase3_cache="$(scripts/unit_cache_directory "$bin_directory")$site_directory/tests/capy-phase3.capy"
wasm-validate "$phase3_cache.wasm"
[[ "$(scripts/bearer-cli /tests/capy-cross.capy)" == "capy-render-ok|counted;roundtripother|1|3|0" ]] || {
	echo "Capy Bearer unit dispatch failed" >&2
	exit 1
}
[[ "$(scripts/bearer-cli /tests/capy-cross-caller.capy)" == "capy-component-ok|capy-named-ok" ]] || {
	echo "Capy component dispatch failed" >&2
	exit 1
}
request_component_output=$(scripts/bearer-cli /tests/capy-request-context-caller.capy)
[[ "$request_component_output" == "component-prop|false|1|handle|1" ]] || {
	echo "Capy request props snapshot mismatch: $request_component_output" >&2
	exit 1
}
component_props_output=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-component-props.capy?name=FromCapy')
[[ "$component_props_output" == "truetruetrue|FromCapy|false|1|FromCapy|false|1|FromCapy|FromCapy|3" ]] || {
	echo "Capy component props dispatch mismatch: $component_props_output" >&2
	exit 1
}
component_trap_body=$(mktemp)
component_trap_status=$(curl -sS --max-time 30 -o "$component_trap_body" -w '%{http_code}' -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-component-props.capy?name=FromCapy&trap=1')
[[ "$component_trap_status" == "500" ]] && ! grep -q 'must-not-leak' "$component_trap_body"
rm -f "$component_trap_body"
[[ "$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-component-props.capy?name=Recovered')" == "truetruetrue|Recovered|false|1|Recovered|false|1|Recovered|Recovered|3" ]]
(
	component_cache_dir=$(mktemp -d "$site_directory/tests/capy-component-cache.XXXXXX")
	chmod 755 "$component_cache_dir"
	trap 'rm -rf -- "$component_cache_dir"' EXIT
	component_cache_name=${component_cache_dir##*/}
	cat >"$component_cache_dir/parent.capy" <<'EOF'
function RENDER(request : dval) {
    print(component_exists("child"))
}
EOF
	printf '%s\n' 'function COMPONENT(request : dval) { print("child") }' >"$component_cache_dir/child.capy"
	expect_equal "Capy warm component resolution" "true" "$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' "http://127.0.0.1/tests/$component_cache_name/parent.capy")"
	rm "$component_cache_dir/child.capy"
	expect_equal "Capy deleted component rejection" "false" "$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' "http://127.0.0.1/tests/$component_cache_name/parent.capy")"
	printf '%s\n' 'function COMPONENT(request : dval) { print("child") }' >"$component_cache_dir/child.capy"
	expect_equal "Capy component resolution recovery" "true" "$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' "http://127.0.0.1/tests/$component_cache_name/parent.capy")"
)
request_headers=$(mktemp)
request_http_output=$(curl -fsS --max-time 30 -D "$request_headers" -H 'Host: bearer.openfu.com' -H 'X-Capy-Schema: yes' -d 'answer=42' 'http://127.0.0.1/tests/capy-request-context.capy?name=Ada')
[[ "$request_http_output" == "POST|Ada|42|answer=42|1" ]] || {
	echo "Capy HTTP request snapshot mismatch: $request_http_output" >&2
	exit 1
}
grep -Eq '^HTTP/1\.[01] 201 ' "$request_headers"
grep -Eqi '^X-Capy-Context: yes' "$request_headers"
grep -Eqi '^X-Capy-Clean: first  X-Injected: bad' "$request_headers"
! grep -Eqi '^X-Injected:' "$request_headers"
rm -f "$request_headers"
upload_file=$(mktemp --suffix=-request-upload.txt)
printf 'data' >"$upload_file"
upload_output=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' -F "upload=@$upload_file;filename=request-upload.txt" 'http://127.0.0.1/tests/capy-request-context.capy?upload=1')
rm -f "$upload_file"
[[ "$upload_output" == "request-upload.txt|true|true" ]] || {
	echo "Capy upload snapshot mismatch: $upload_output" >&2
	exit 1
}
request_http_second=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' -H 'X-Capy-Schema: yes' -d 'answer=7' 'http://127.0.0.1/tests/capy-request-context.capy?name=Grace')
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
[[ "$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' -H 'X-Capy-Schema: yes' -d 'answer=8' 'http://127.0.0.1/tests/capy-request-context.capy?name=Reset')" == "POST|Reset|8|answer=8|1" ]] || {
	echo "Capy request workspace did not recover after response trap" >&2
	exit 1
}
expect_equal "request/response stdlib CLI" "Ada|Ada|inner|outer+inner|index|workspacetruetruetrue|truetruetrue|true|1" "$(scripts/bearer-cli /tests/capy-request-parity.capy name=Ada)"
request_parity_headers=$(mktemp)
request_parity_output=$(curl -fsS --max-time 30 -D "$request_parity_headers" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-request-parity.capy?workspace/projects&name=Grace&status=yes')
[[ "$request_parity_output" == "Grace|Grace|inner|outer+inner|workspace/projects|workspacetruetruetrue|truetruetrue|true|1" ]]
grep -Eq '^HTTP/1\.[01] 202 Capy Accepted' "$request_parity_headers"
rm -f "$request_parity_headers"
session_replacement_headers=$(mktemp)
[[ "$(curl -fsS --max-time 30 -D "$session_replacement_headers" -b 'capy-session=not-a-session' -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=read')" == "missing||2" ]]
grep -Eiq '^Set-Cookie: capy-session=[0-9a-f]{64}.*HttpOnly.*SameSite=Lax' "$session_replacement_headers"
rm -f "$session_replacement_headers"
session_jar=$(mktemp)
session_headers=$(mktemp)
[[ "$(curl -fsS --max-time 30 -D "$session_headers" -c "$session_jar" -b "$session_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=set&value=Ada')" == "set" ]]
grep -Eiq '^Set-Cookie: capy-session=.*HttpOnly.*SameSite=Lax' "$session_headers"
grep -Eiq '^Set-Cookie: capy-extra=yes.*HttpOnly.*SameSite=Lax' "$session_headers"
[[ "$(curl -fsS --max-time 30 -c "$session_jar" -b "$session_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=read')" == "Ada|yes|2" ]]
[[ "$(curl -fsS --max-time 30 -c "$session_jar" -b "$session_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=remove')" == "removed" ]]
[[ "$(curl -fsS --max-time 30 -c "$session_jar" -b "$session_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=read')" == "missing|yes|2" ]]
[[ "$(curl -fsS --max-time 30 -c "$session_jar" -b "$session_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=set&value=Grace')" == "set" ]]
[[ "$(curl -fsS --max-time 30 -c "$session_jar" -b "$session_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=destroy')" == "destroyed" ]]
[[ "$(curl -fsS --max-time 30 -c "$session_jar" -b "$session_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-session.capy?action=read')" == "missing|yes|2" ]]
rm -f "$session_jar" "$session_headers"
redirect_headers=$(mktemp)
redirect_body=$(curl -sS --max-time 30 -D "$redirect_headers" -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-redirect.capy)
grep -Eq '^HTTP/1\.[01] 302 ' "$redirect_headers"
grep -Eqi '^Location: /tests/capy-session.capy\?action=read' "$redirect_headers"
[[ "$redirect_body" == "redirected" ]]
rm -f "$redirect_headers"
csrf_jar=$(mktemp)
csrf_token=$(curl -fsS --max-time 30 -c "$csrf_jar" -b "$csrf_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-csrf.capy?action=token')
[[ "$csrf_token" =~ ^[0-9a-f]{64}$ ]]
[[ "$(curl -fsS --max-time 30 -c "$csrf_jar" -b "$csrf_jar" -H 'Host: bearer.openfu.com' "http://127.0.0.1/tests/capy-csrf.capy?action=valid&submitted=$csrf_token")" == "true" ]]
[[ "$(curl -fsS --max-time 30 -c "$csrf_jar" -b "$csrf_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-csrf.capy?action=valid&submitted=wrong')" == "false" ]]
csrf_rotated=$(curl -fsS --max-time 30 -c "$csrf_jar" -b "$csrf_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-csrf.capy?action=rotate')
[[ "$csrf_rotated" =~ ^[0-9a-f]{64}$ && "$csrf_rotated" != "$csrf_token" ]]
[[ "$(curl -fsS --max-time 30 -c "$csrf_jar" -b "$csrf_jar" -H 'Host: bearer.openfu.com' "http://127.0.0.1/tests/capy-csrf.capy?action=valid&submitted=$csrf_token")" == "false" ]]
[[ "$(curl -fsS --max-time 30 -c "$csrf_jar" -b "$csrf_jar" -H 'Host: bearer.openfu.com' "http://127.0.0.1/tests/capy-csrf.capy?action=valid&submitted=$csrf_rotated")" == "true" ]]
[[ "$(curl -fsS --max-time 30 -c "$csrf_jar" -b "$csrf_jar" -H 'Host: bearer.openfu.com' 'http://127.0.0.1/tests/capy-csrf.capy?action=field')" == "<input type=\"hidden\" name=\"submitted\" value=\"$csrf_rotated\">" ]]
rm -f "$csrf_jar"
websocket_test=$(mktemp)
trap 'rm -f "$websocket_test"' RETURN
clang++ -std=c++20 -Wall -Wextra -Werror -pedantic scripts/test_capy_websocket.cpp -o "$websocket_test"
"$websocket_test"
rm -f "$websocket_test"
expect_equal "SERVE_HTTP caller" "serve-ok" "$(scripts/bearer-cli /tests/capy-serve-http-caller.capy)"
rich_dval_output=$(scripts/bearer-cli /tests/capy-dval-rich.capy)
[[ "$rich_dval_output" == "capy|Ada|9|custom-once;capy|capy|false|Ada|42|true|logic|truefalse|4|active;age;name;tags;|0=math;1=logic;|2;|4|0|falsefalse|2|0" ]] || {
	echo "Capy rich DValue output mismatch: $rich_dval_output" >&2
	exit 1
}
[[ "$(scripts/bearer-cli /tests/capy-dval-rich-caller.capy)" == "custom-once;capy|Capy|roundtrip|3|false" ]] || {
	echo "Capy-to-Capy custom DValue export failed" >&2
	exit 1
}
none_output=$(scripts/bearer-cli /tests/capy-dval-none.capy)
expect_equal "DValue none, safe read, conversion, codec, path update, and ARC contract" \
	"false|true|true|true|true|false|false|false|false|false|Ada|[][string]|false|true|0|32|0|-64|0|64|0|6.5|false|false|false|null|true|false|true|false|false|created|replaced|old|kept|updated|changed|changed|ABR|done|false|0" "$none_output"
set +e
dval_trap_output=$(scripts/bearer-cli /tests/capy-dval-missing-trap.capy 2>&1)
dval_trap_status=$?
set -e
[[ $dval_trap_status -ne 0 && "$dval_trap_output" == *'wasm `unreachable` instruction executed'* && "$dval_trap_output" == *'capy-dval-missing-trap.capy:3:11'* && "$dval_trap_output" != *"capy://stdlib.capy"* ]] || {
	echo "Capy strict missing DValue trap/source mapping mismatch: status=$dval_trap_status output=$dval_trap_output" >&2
	exit 1
}
[[ "$(scripts/bearer-cli /tests/capy-dval-rich.capy)" == "$rich_dval_output" ]] || {
	echo "Capy DValue workspace did not reset after strict trap" >&2
	exit 1
}
for fixture_and_location in \
	"capy-dval-negative-trap:3:11" \
	"capy-dval-range-trap:3:11" \
	"capy-dval-scalar-trap:3:11"; do
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
for fixture_and_location in \
	"capy-dval-assign-list-trap:3:"; do
	fixture=${fixture_and_location%%:*}
	location=${fixture_and_location#*:}
	set +e
	assignment_trap_output=$(scripts/bearer-cli "/tests/$fixture.capy" 2>&1)
	assignment_trap_status=$?
	set -e
	[[ $assignment_trap_status -ne 0 && "$assignment_trap_output" == *"$fixture.capy:$location"* && "$assignment_trap_output" != *"capy://stdlib.capy"* ]] || {
		echo "Capy nested DValue assignment trap/source mapping mismatch: fixture=$fixture output=$assignment_trap_output" >&2
		exit 1
	}
	expect_equal "DValue none recovery after $fixture" "$none_output" "$(scripts/bearer-cli /tests/capy-dval-none.capy)"
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
[[ -s "$cache.wasm" && -s "$cache.cwasm" && -s "$cache.cwasm.manifest" && -s "$cache.wasm.source-map" && -s "$cache.meta.txt" ]]
[[ $(sed -n '1p' "$cache.cwasm.manifest") == "$(sha256sum "$cache.wasm" | awk '{print $1}')" ]]
[[ $(sed -n '2p' "$cache.cwasm.manifest") == "$(sha256sum "$cache.cwasm" | awk '{print $1}')" ]]
grep -qx 'format=bearer-unit-metadata-v3' "$cache.meta.txt"
grep -Eq '^source_map_sha256=[0-9a-f]{64}$' "$cache.meta.txt"
grep -qx "wasm_core_abi_version=$(awk '/BEARER_WASM_CORE_ABI_VERSION/ {print $3; exit}' src/wasm/abi.h)" "$cache.meta.txt"
grep -qx "wasm_sha256=$(sha256sum "$cache.wasm" | awk '{print $1}')" "$cache.meta.txt"
grep -qx "exports_sha256=$(sha256sum "$cache.exports.txt" | awk '{print $1}')" "$cache.meta.txt"
bin/capyc --check-unit "$cache.wasm" "$(awk '/BEARER_WASM_CORE_ABI_VERSION/ {print $3; exit}' src/wasm/abi.h)"
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
	printf 'function CLI(request : dval) { print("%s") }\n' "$payload" >"$source_dir/entry.capy"
	expect_equal "signed-LEB output boundary ($size bytes)" "$payload" "$(scripts/bearer-cli "/$fixture/entry.capy")"
done
render_prefix=$(printf '%*s' 64 '' | tr ' ' r)
printf 'function RENDER(request : dval) { print("%s") }\nfunction CLI(request : dval) { print("offset-ok") }\n' "$render_prefix" >"$source_dir/entry.capy"
expect_equal "source-map offset runtime" "offset-ok" "$(scripts/bearer-cli "/$fixture/entry.capy")"

printf '%s\n' 'function CLI(request : dval) { print(not_a_constant) }' >"$source_dir/entry.capy"
set +e
failure=$(scripts/bearer-cli "/$fixture/entry.capy" 2>&1)
status=$?
set -e
[[ $status -ne 0 ]]
[[ "$failure" == *"entry.capy:1:"* && "$failure" == *"unknown local 'not_a_constant'"* ]]
[[ ! -e "$artifact_dir/entry.capy.wasm" ]]

printf '%s\n' 'function CLI(request : dval) { print("capy-recovered") }' >"$source_dir/entry.capy"
expect_equal "source-map trap recovery" "capy-recovered" "$(scripts/bearer-cli "/$fixture/entry.capy")"
[[ -s "$artifact_dir/entry.capy.wasm" ]]

expect_equal "network socket/hardened HTTP adapters" "true|true|true|200|true:true:200|invalid_request|5" "$(scripts/bearer-cli /tests/capy-network-parity.capy)"
expect_equal "time adapters" "5|0|1970|true|true|0" "$(scripts/bearer-cli /tests/capy-time-parity.capy)"
lifecycle_output=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-lifecycle-parity.capy)
expect_equal "INIT/ONCE lifecycle" "entry-init;entry-once;child-init;child-once;child-component;child-component;render" "$lifecycle_output"
expect_equal "INIT/ONCE lifecycle in a later request workspace" \
	"entry-init;entry-once;child-init;child-once;child-component;child-component;render" \
	"$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-lifecycle-parity.capy)"
component_parity_output=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-component-parity.capy)
expect_equal "component/unit DValue conversions" "Capy|false|1|Capy|false|1|scalar|render|unit|true" "$component_parity_output"
expect_equal "general methods receiver order/overload/generic/local shadow/function fields" "receiver;argument;method;10|s32|string|9|shadow;5|field;5|extension;6" "$(scripts/bearer-cli /tests/capy-methods.capy)"
module_output=$(scripts/bearer-cli /tests/capy-module-caller.capy)
expect_equal "Capy module capability/default input/nested BRRB/legacy/none/copied identity/handler/ARC" "counted;handler-once;handler-render;default|nested|once|capy|legacy|false|inputresult|0" "$module_output"
module_exports="$(scripts/unit_cache_directory "$bin_directory")$site_directory/tests/capy-module-target.capy.exports.txt"
grep -qx 'DValue\* echo(DValue\*);' "$module_exports"
grep -qx 'DValue\* counted(DValue\*);' "$module_exports"
grep -qx 'capy function echo(dval):dval' "$module_exports"
(
	metadata_dir=$(mktemp -d "$site_directory/tests/capy-export-metadata.XXXXXX")
	trap 'rm -rf -- "$metadata_dir"' EXIT
	metadata_name=${metadata_dir##*/}
	cat >"$metadata_dir/provider.capy" <<'EOF'
#exports echo, Shape, ShapeAlias
struct Shape { value : s32 }
type ShapeAlias = Shape
function echo(input : dval) dval { -> input }
function Shape(input : dval) Shape { -> Shape(1) }
EOF
	cat >"$metadata_dir/reader.capy" <<'EOF'
function CLI(request : dval) {
    unit_compile("provider.capy")
    var info := unit_info("provider.capy")
    print(string(info.function_exports["0"]), "|", string(info.function_exports["1"]), "|", string(info.type_exports["0"]), "|", string(info.type_exports["1"]))
}
EOF
	cat >"$metadata_dir/importer.capy" <<'EOF'
#import"provider.capy" as provider

type ImportedShape = provider.Shape
type ImportedAlias = provider.ShapeAlias

function CLI(request : dval) {
    print("imported")
}
EOF
	expect_equal "Capy #exports function and type metadata" "echo|Shape|struct Shape{value:s32}|alias ShapeAlias=struct:Shape" "$(scripts/bearer-cli "/tests/$metadata_name/reader.capy")"
	expect_equal "Capy #import type metadata" "imported" "$(scripts/bearer-cli "/tests/$metadata_name/importer.capy")"
	printf '%s\n' '#exports Shape' 'struct Shape { value : MissingType }' >"$metadata_dir/provider.capy"
	set +e
	import_failure=$(scripts/bearer-cli "/tests/$metadata_name/importer.capy" 2>&1)
	import_status=$?
	set -e
	[[ $import_status -ne 0 && "$import_failure" == *"#import failed to compile"* ]] || {
		echo "Capy #import dependency invalidation did not reject a broken provider: $import_failure" >&2
		exit 1
	}
	cat >"$metadata_dir/provider.capy" <<'EOF'
#exports echo, Shape, ShapeAlias
struct Shape { value : s32 }
type ShapeAlias = Shape
function echo(input : dval) dval { -> input }
function Shape(input : dval) Shape { -> Shape(1) }
EOF
	expect_equal "Capy #import recovery" "imported" "$(scripts/bearer-cli "/tests/$metadata_name/importer.capy")"
	cat >"$metadata_dir/cycle-a.capy" <<'EOF'
#import"cycle-b.capy" as b
type A = b.B
function CLI(request : dval) { print("bad") }
EOF
	cat >"$metadata_dir/cycle-b.capy" <<'EOF'
#import"cycle-a.capy" as a
type B = a.A
EOF
	set +e
	cycle_failure=$(scripts/bearer-cli "/tests/$metadata_name/cycle-a.capy" 2>&1)
	cycle_status=$?
	set -e
	[[ $cycle_status -ne 0 && "$cycle_failure" == *"#import dependency cycle"* ]] || {
		echo "Capy #import cycle was not rejected: $cycle_failure" >&2
		exit 1
	}
)
(
	module_test_dir=$(mktemp -d "$site_directory/tests/capy-module-trap.XXXXXX")
	trap 'rm -rf -- "$module_test_dir"' EXIT
	module_test_name=${module_test_dir##*/}
	check_module_trap() {
		local name=$1 source=$2
		printf '%s\n' "$source" >"$module_test_dir/caller.capy"
		set +e
		local output
		output=$(scripts/bearer-cli "/tests/$module_test_name/caller.capy" 2>&1)
		local status=$?
		set -e
		[[ $status -ne 0 && "$output" == *"BEARER_MODULE_"* && "$output" == *"caller.capy:"* && "$output" != *"capy://stdlib.capy"* ]] || {
			echo "Capy module $name trap/source mapping mismatch: $output" >&2
			exit 1
		}
		expect_equal "Capy module $name recovery" "counted;handler-once;handler-render;default|nested|once|capy|legacy|false|inputresult|0" "$(scripts/bearer-cli /tests/capy-module-caller.capy)"
	}
	check_module_trap missing 'function CLI(request : dval) { var module := unit_load("missing.capy") }'
	check_module_trap unauthorized 'function CLI(request : dval) { var module := unit_load("/etc/passwd") }'
	check_module_trap undeclared $'function CLI(request : dval) {\n    unit_call("/tests/capy-module-target.capy", "hidden", "")\n}'
	check_module_trap wrong_target $'function CLI(request : dval) {\n    var module := unit_load("/tests/capy-module-caller.capy")\n    module.call("CLI")\n}'
	external_unit=$(mktemp /tmp/capy-module-external.XXXXXX.capy)
	trap 'rm -f -- "$external_unit"; rm -rf -- "$module_test_dir"' EXIT
	printf '%s\n' '#exports echo' 'function echo(input : dval) dval { -> dval("external") }' >"$external_unit"
	check_module_trap external_source "function CLI(request : dval) { var module := unit_load(\"$external_unit\"); print(string(module.call(\"echo\"), \"\")) }"
	ln -s "$external_unit" "$module_test_dir/escape.capy"
	check_module_trap source_symlink 'function CLI(request : dval) { var module := unit_load("escape.capy"); print(string(module.call("echo"), "")) }'
	printf '%s\n' 'function CLI(request : dval) { print(not_a_constant) }' >"$module_test_dir/broken.capy"
	check_module_trap failed_compile 'function CLI(request : dval) { var module := unit_load("broken.capy") }'
	printf '%s\n' '#exports echo' 'function echo(input : dval) dval { -> dval("v1") }' >"$module_test_dir/target.capy"
	cat >"$module_test_dir/pinned.capy" <<'EOF'
function CLI(request : dval) {
    var module := unit_load("target.capy")
    file_put_contents("target.capy", "#exports echo\nfunction echo(input : dval) dval { -> dval(\"v2\") }\n")
    print(string(module.call("echo", ""), ""))
}
EOF
	printf '%s\n' 'function CLI(request : dval) { print(string(unit_load("target.capy").call("echo", ""), "")) }' >"$module_test_dir/fresh.capy"
	expect_equal "Capy module request pin" "v1" "$(scripts/bearer-cli "/tests/$module_test_name/pinned.capy")"
	expect_equal "Capy module next-request reload" "v2" "$(scripts/bearer-cli "/tests/$module_test_name/fresh.capy")"
	printf '%s\n' 'function CLI(request : dval) { print(not_a_constant) }' >"$module_test_dir/target.capy"
	check_module_trap failed_reload_compile 'function CLI(request : dval) { var module := unit_load("target.capy") }'
	printf '%s\n' '#exports echo' 'function echo(input : dval) dval { -> dval("v3") }' >"$module_test_dir/target.capy"
	expect_equal "Capy module failed-compile recovery" "v3" "$(scripts/bearer-cli "/tests/$module_test_name/fresh.capy")"
	module_artifacts="$(scripts/unit_cache_directory "$bin_directory")$module_test_dir/target.capy"
	printf '\377' | dd of="$module_artifacts.wasm" bs=1 seek=8 conv=notrunc status=none
	check_module_trap wasm_tamper 'function CLI(request : dval) { unit_call("target.capy", "echo", "") }'
	printf '%s\n' 'DValue* echo(DValue*);' 'not an export declaration' >"$module_artifacts.exports.txt"
	check_module_trap exports_tamper 'function CLI(request : dval) { unit_call("target.capy", "echo", "") }'
	sed -i 's/^wasm_sha256=.*/wasm_sha256=0000000000000000000000000000000000000000000000000000000000000000/' "$module_artifacts.meta.txt"
	check_module_trap metadata_tamper 'function CLI(request : dval) { unit_call("target.capy", "echo", "") }'
	printf '%s\n' '#exports echo, nested, file_relative, boom' \
		'function echo(input : dval) dval { -> dval("v4") }' \
		'function nested(input : dval) dval { -> unit_load("nested.capy").call("echo", input) }' \
		'function file_relative(input : dval) dval { -> dval(file_get_contents("marker.txt")) }' \
		'function boom(input : dval) dval { trap(); -> dval("") }' >"$module_test_dir/target.capy"
	printf '%s\n' '#exports echo' 'function echo(input : dval) dval { -> dval("nested") }' >"$module_test_dir/nested.capy"
	printf '%s' 'target-file' >"$module_test_dir/marker.txt"
	cat >"$module_test_dir/provenance.capy" <<'EOF'
function CLI(request : dval) {
    var target := unit_load("target.capy")
    print(string(target.call("nested", ""), ""), "|", string(target.call("file_relative", ""), ""))
}
EOF
	expect_equal "Capy module target provenance" "nested|target-file" "$(scripts/bearer-cli "/tests/$module_test_name/provenance.capy")"
	printf '%s\n' 'function CLI(request : dval) {' '    unit_load("target.capy").call("boom", "")' '}' >"$module_test_dir/caller.capy"
	set +e
	target_trap_output=$(scripts/bearer-cli "/tests/$module_test_name/caller.capy" 2>&1)
	target_trap_status=$?
	set -e
	[[ $target_trap_status -ne 0 && "$target_trap_output" == *"caller.capy:2:29"* && "$target_trap_output" != *"capy://stdlib.capy"* ]] || {
		echo "Capy module target trap/source map mismatch: $target_trap_output" >&2
		exit 1
	}
	expect_equal "Capy module trap reset recovery" "nested|target-file" "$(scripts/bearer-cli "/tests/$module_test_name/provenance.capy")"
)

echo "Capy phase 1 parser/direct-Wasm/CLI smoke passed"
