#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="dependency-cache-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r /etc/bearer/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
	if [[ -n "${configured_site_directory:-}" ]]; then
		site_directory="$configured_site_directory"
	fi
fi
source_dir="$site_directory/$test_name"
bin_directory="${BIN_DIRECTORY:-}"
if [[ -z "$bin_directory" && -r /etc/bearer/settings.cfg ]]; then
	bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
fi
bin_directory="${bin_directory:-/tmp/bearer/work}"
cache_dir=""
mutation_file="/tmp/bearer-dependency-mutation-$$"
component_mutation_file="/tmp/bearer-component-mutation-$$"
component_post_body="/tmp/bearer-component-post-body-$$"
component_post_headers="/tmp/bearer-component-post-headers-$$"
component_lock_ready_file="/tmp/bearer-component-lock-ready-$$"
post_body="/tmp/bearer-dependency-post-body-$$"
post_headers="/tmp/bearer-dependency-post-headers-$$"
lock_ready_file="/tmp/bearer-dependency-lock-ready-$$"
nested_lock_ready_file="/tmp/bearer-nested-lock-ready-$$"
race_output="/tmp/bearer-source-race-output-$$"
race_error="/tmp/bearer-source-race-error-$$"
race_status="/tmp/bearer-source-race-status-$$"
http_host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"

cleanup() {
	rm -rf "$source_dir"
	if [[ -n "$cache_dir" ]]; then
		rm -rf "$cache_dir"
	fi
	rm -f "$mutation_file" "$component_mutation_file" "$component_post_body" "$component_post_headers" "$component_lock_ready_file" "$post_body" "$post_headers" "$lock_ready_file" "$nested_lock_ready_file" "$race_output" "$race_error" "$race_status"
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

printf '%s\n' \
	'#ifndef BEARER_DEPENDENCY_CACHE_CHILD' \
	'#define BEARER_DEPENDENCY_CACHE_CHILD' \
	'String dependency_cache_marker() { return("dependency-marker-a"); }' \
	'#endif' >"$source_dir/child.uce"
printf '%s\n' \
	'#load "child.uce"' \
	'CLI(Request& context) { print(dependency_cache_marker(), ":", request_perf()["worker_pid"].to_string()); }' \
	"RENDER(Request& context) { if(context.params[\"REQUEST_METHOD\"] == \"POST\") file_put_contents(\"$mutation_file\", dependency_cache_marker()); print(dependency_cache_marker(), \":\", request_perf()[\"worker_pid\"].to_string()); }" >"$source_dir/parent.uce"

assert_marker() {
	local path="$1"
	local expected="$2"
	local output
	output=$(scripts/bearer-cli "/$test_name/$path.uce")
	if [[ "$output" != *"$expected"* ]]; then
		echo "$path returned stale module; expected $expected: $output" >&2
		exit 1
	fi
}

http_marker() {
	curl -fsS -H "Host: $http_host" "http://127.0.0.1/$test_name/parent.uce"
}

assert_marker parent dependency-marker-a
if [[ "$(http_marker)" != *"dependency-marker-a"* ]]; then
	echo "HTTP warm-up did not return dependency-marker-a" >&2
	exit 1
fi

# Compiler source canonicalization must retain symlink-target identity while
# avoiding realpath's repeated per-segment probes on ordinary source graphs.
printf '%s\n' 'String symlink_dependency_marker() { return("symlink-marker-a"); }' >"$source_dir/symlink-target-a.uce"
printf '%s\n' 'String symlink_dependency_marker() { return("symlink-marker-b"); }' >"$source_dir/symlink-target-b.uce"
ln -s "symlink-target-a.uce" "$source_dir/symlink-child.uce"
printf '%s\n' '#load "symlink-child.uce"' 'CLI(Request& context) { print(symlink_dependency_marker()); }' >"$source_dir/symlink-parent.uce"
assert_marker symlink-parent symlink-marker-a
ln -sfn "symlink-target-b.uce" "$source_dir/symlink-child.uce"
assert_marker symlink-parent symlink-marker-b

# A repeated exact transitive load is signature-deduplicated before path
# canonicalization, but the shared dependency must still invalidate the parent.
printf '%s\n' '#ifndef BEARER_DIAMOND_COMMON' '#define BEARER_DIAMOND_COMMON' 'String diamond_marker() { return("diamond-a"); }' '#endif' >"$source_dir/diamond-common.uce"
printf '%s\n' '#load "diamond-common.uce"' >"$source_dir/diamond-left.uce"
printf '%s\n' '#load "diamond-common.uce"' >"$source_dir/diamond-right.uce"
printf '%s\n' '#load "diamond-left.uce"' '#load "diamond-right.uce"' 'CLI(Request& context) { print(diamond_marker()); }' >"$source_dir/diamond-parent.uce"
assert_marker diamond-parent diamond-a
sed -i 's/diamond-a/diamond-b/' "$source_dir/diamond-common.uce"
assert_marker diamond-parent diamond-b

# HTTP entry units can resolve route/components dynamically. A changed dynamic
# component must enter the demand-priority queue just like a changed entry unit;
# otherwise a common dependency rebuild can leave the requested page stale for
# the duration of the full proactive queue.
printf '%s\n' "COMPONENT(Request& context) { if(context.params[\"REQUEST_METHOD\"] == \"POST\") file_put_contents(\"$component_mutation_file\", \"executed\"); print(\"http-component-marker-a\"); }" >"$source_dir/http-component-child.uce"
printf '%s\n' 'RENDER(Request& context) { DValue props; print(component("http-component-child", props, context)); }' >"$source_dir/http-component-parent.uce"
http_component_marker() {
	curl -fsS -H "Host: $http_host" "http://127.0.0.1/$test_name/http-component-parent.uce"
}
if [[ "$(http_component_marker)" != *"http-component-marker-a"* ]]; then
	echo "HTTP component warm-up did not return marker-a" >&2
	exit 1
fi
sed -i 's/http-component-marker-a/http-component-marker-b/' "$source_dir/http-component-child.uce"
started_at=$(date +%s%N)
http_component_output=$(http_component_marker)
elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
if [[ "$http_component_output" != *"http-component-marker-a"* && "$http_component_output" != *"http-component-marker-b"* ]]; then
	echo "stale HTTP component request returned neither complete artifact: $http_component_output" >&2
	exit 1
fi
if (( elapsed_ms >= 2000 )); then
	echo "HTTP component request spent ${elapsed_ms}ms rebuilding a stale artifact" >&2
	exit 1
fi
deadline=$((SECONDS + 15))
while [[ "$http_component_output" != *"http-component-marker-b"* && $SECONDS -lt $deadline ]]; do
	sleep 0.2
	http_component_output=$(http_component_marker)
done
if [[ "$http_component_output" != *"http-component-marker-b"* ]]; then
	echo "requested stale HTTP component did not receive a demand-priority rebuild" >&2
	exit 1
fi

printf '%s\n' 'String http_component_dependency_marker() { return("http-component-dependency-a"); }' >"$source_dir/http-component-dependency.uce"
printf '%s\n' '#load "http-component-dependency.uce"' 'COMPONENT(Request& context) { print(http_component_dependency_marker()); }' >"$source_dir/http-transitive-child.uce"
printf '%s\n' 'RENDER(Request& context) { DValue props; print(component("http-transitive-child", props, context)); }' >"$source_dir/http-transitive-parent.uce"
http_transitive_marker() {
	curl -fsS -H "Host: $http_host" "http://127.0.0.1/$test_name/http-transitive-parent.uce"
}
if [[ "$(http_transitive_marker)" != *"http-component-dependency-a"* ]]; then
	echo "HTTP transitive component warm-up did not return dependency-a" >&2
	exit 1
fi
sed -i 's/http-component-dependency-a/http-component-dependency-b/' "$source_dir/http-component-dependency.uce"
deadline=$((SECONDS + 20))
while [[ "$(http_transitive_marker)" != *"http-component-dependency-b"* && $SECONDS -lt $deadline ]]; do sleep 0.2; done
if [[ "$(http_transitive_marker)" != *"http-component-dependency-b"* ]]; then
	echo "HTTP dynamic component did not converge after its loaded dependency changed" >&2
	exit 1
fi

# Mutations must never execute a stale dynamically resolved component, even
# inside the short read-only freshness memo window.
http_component_wasm="$cache_dir/http-component-child.uce.wasm"
(
	exec 8>"$http_component_wasm.lock"
	flock 8
	: >"$component_lock_ready_file"
	sleep 2
) &
component_lock_pid=$!
deadline=$((SECONDS + 5))
while [[ ! -e "$component_lock_ready_file" && $SECONDS -lt $deadline ]]; do sleep 0.05; done
if [[ ! -e "$component_lock_ready_file" ]]; then
	echo "HTTP component mutation lock was not acquired before deadline" >&2
	exit 1
fi
sed -i 's/http-component-marker-b/http-component-marker-c/' "$source_dir/http-component-child.uce"
rm -f "$component_mutation_file"
component_post_status=$(curl -sS -o "$component_post_body" -D "$component_post_headers" -w '%{http_code}' -X POST -H "Host: $http_host" "http://127.0.0.1/$test_name/http-component-parent.uce")
if [[ "$component_post_status" != "503" ]]; then
	echo "stale HTTP component mutation did not fail closed: status=$component_post_status body=$(cat "$component_post_body")" >&2
	exit 1
fi
if ! grep -qi '^Retry-After: 1' "$component_post_headers"; then
	echo "stale HTTP component mutation omitted Retry-After" >&2
	exit 1
fi
if [[ -e "$component_mutation_file" ]]; then
	echo "stale HTTP component mutation executed application code" >&2
	exit 1
fi
wait "$component_lock_pid"
deadline=$((SECONDS + 15))
while [[ "$(http_component_marker)" != *"http-component-marker-c"* && $SECONDS -lt $deadline ]]; do sleep 0.2; done
if [[ "$(http_component_marker)" != *"http-component-marker-c"* ]]; then
	echo "HTTP component did not recover after the guarded mutation rebuild" >&2
	exit 1
fi

# A failed parent build must remember the exact dependency graph that failed.
# If a dependency is restored byte-for-byte to the last working source, the
# old successful metadata must not make that transient failure permanent.
printf '%s\n' \
	'#ifndef BEARER_DEPENDENCY_CACHE_CHILD' \
	'#define BEARER_DEPENDENCY_CACHE_CHILD' \
	'String dependency_cache_marker() { return(deliberate_dependency_compile_failure); }' \
	'#endif' >"$source_dir/child.uce"
if restored_failure=$(scripts/bearer-cli --get "/$test_name/parent.uce" __bearer_expected_compile_failure=1 2>&1); then
	echo "invalid dependency unexpectedly compiled: $restored_failure" >&2
	exit 1
fi
if [[ "$restored_failure" != *"deliberate_dependency_compile_failure"* ]]; then
	echo "invalid dependency did not report the expected compiler failure: $restored_failure" >&2
	exit 1
fi
printf '%s\n' \
	'#ifndef BEARER_DEPENDENCY_CACHE_CHILD' \
	'#define BEARER_DEPENDENCY_CACHE_CHILD' \
	'String dependency_cache_marker() { return("dependency-marker-a"); }' \
	'#endif' >"$source_dir/child.uce"
assert_marker parent dependency-marker-a

# The load-graph signature is content-addressed. A byte-identical dependency
# touch must invalidate stat caches without recompiling its parent artifact.
parent_wasm="$cache_dir/parent.uce.wasm"
parent_wasm_identity=$(stat -c '%y:%s' "$parent_wasm")
sleep 1.1
touch "$source_dir/child.uce"
assert_marker parent dependency-marker-a
if [[ "$(stat -c '%y:%s' "$parent_wasm")" != "$parent_wasm_identity" ]]; then
	echo "byte-identical dependency touch recompiled the parent artifact" >&2
	exit 1
fi

printf '%s\n' 'CLI(Request& context) { print("readable-source-marker"); }' >"$source_dir/unreadable.uce"
chmod 000 "$source_dir/unreadable.uce"
if unreadable_output=$(scripts/bearer-cli --get "/$test_name/unreadable.uce" __bearer_expected_source_read_failure=1 2>&1); then
	echo "unreadable source unexpectedly compiled: $unreadable_output" >&2
	exit 1
fi
if [[ "$unreadable_output" != *"source file is not readable"* ]]; then
	echo "unreadable source did not report its actual compile error: $unreadable_output" >&2
	exit 1
fi
if [[ -e "$cache_dir/unreadable.uce.wasm" ]]; then
	echo "unreadable source published a wasm artifact" >&2
	exit 1
fi
chmod 644 "$source_dir/unreadable.uce"
assert_marker unreadable readable-source-marker

sed -i 's/dependency-marker-a/dependency-marker-b/' "$source_dir/child.uce"
started_at=$(date +%s%N)
http_during_rebuild=$(http_marker)
elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
if [[ "$http_during_rebuild" != *"dependency-marker-a"* && "$http_during_rebuild" != *"dependency-marker-b"* ]]; then
	echo "HTTP rebuild request returned neither complete artifact: $http_during_rebuild" >&2
	exit 1
fi
if (( elapsed_ms >= 2000 )); then
	echo "HTTP request spent ${elapsed_ms}ms rebuilding a stale artifact" >&2
	exit 1
fi
deadline=$((SECONDS + 15))
while [[ "$http_during_rebuild" != *"dependency-marker-b"* && $SECONDS -lt $deadline ]]; do
	sleep 0.2
	http_during_rebuild=$(http_marker)
done
if [[ "$http_during_rebuild" != *"dependency-marker-b"* ]]; then
	echo "requested stale HTTP unit did not receive a demand-priority rebuild" >&2
	exit 1
fi
assert_marker parent dependency-marker-b

# A proactive rebuild owns these same per-unit locks. While it publishes fresh
# artifacts, requests must use the last complete artifacts instead of waiting
# across the transitive graph. Atomic publication keeps those artifacts safe.
child_wasm="$cache_dir/child.uce.wasm"
(
	exec 8>"$parent_wasm.lock"
	exec 9>"$child_wasm.lock"
	flock 8
	flock 9
	: >"$lock_ready_file"
	sleep 3
) &
rebuild_lock_pid=$!
deadline=$((SECONDS + 5))
while [[ ! -e "$lock_ready_file" && $SECONDS -lt $deadline ]]; do
	if ! kill -0 "$rebuild_lock_pid" 2>/dev/null; then
		echo "test rebuild lock process exited before acquiring locks" >&2
		exit 1
	fi
	sleep 0.05
done
if [[ ! -e "$lock_ready_file" ]]; then
	echo "test rebuild lock process did not acquire locks before deadline" >&2
	exit 1
fi
sed -i 's/dependency-marker-b/dependency-marker-d/' "$source_dir/child.uce"
started_at=$(date +%s%N)
http_during_lock=$(http_marker)
elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
if [[ "$http_during_lock" != *"dependency-marker-b"* ]]; then
	echo "HTTP request did not serve the last complete artifact during rebuild: $http_during_lock" >&2
	exit 1
fi
if (( elapsed_ms >= 2000 )); then
	echo "HTTP request waited ${elapsed_ms}ms for an active transitive rebuild" >&2
	exit 1
fi
rm -f "$mutation_file"
started_at=$(date +%s%N)
post_status=$(curl -sS -o "$post_body" -D "$post_headers" -w '%{http_code}' -X POST -H "Host: $http_host" "http://127.0.0.1/$test_name/parent.uce")
elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
if [[ "$post_status" != "503" ]]; then
	echo "stale POST executed an application artifact instead of returning 503: status=$post_status body=$(cat "$post_body")" >&2
	exit 1
fi
if ! grep -qi '^Retry-After: 1' "$post_headers"; then
	echo "stale POST did not return a Retry-After header" >&2
	exit 1
fi
if [[ -e "$mutation_file" ]]; then
	echo "stale POST executed the old mutation handler: $(cat "$mutation_file")" >&2
	exit 1
fi
if (( elapsed_ms >= 2000 )); then
	echo "stale POST waited ${elapsed_ms}ms instead of failing closed promptly" >&2
	exit 1
fi
started_at=$(date +%s%N)
assert_marker parent dependency-marker-d
elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
if (( elapsed_ms < 2000 )); then
	echo "CLI request returned before the locked rebuild published the current artifact (${elapsed_ms}ms)" >&2
	exit 1
fi
wait "$rebuild_lock_pid"

# CLI freshness applies to every nested component, not only the request entry
# unit. Hold the changed child's compile lock as a proactive rebuild would: the
# parent CLI request must wait and render the new child, never its stale wasm.
printf '%s\n' 'COMPONENT(Request& context) { print("nested-component-marker-a"); }' >"$source_dir/nested-child.uce"
printf '%s\n' \
	'CLI(Request& context) { DValue props; print(component("nested-child", props, context)); }' \
	'RENDER(Request& context) { DValue props; print(component("nested-child", props, context)); }' >"$source_dir/nested-parent.uce"
assert_marker nested-parent nested-component-marker-a
nested_child_wasm="$cache_dir/nested-child.uce.wasm"

# A proactive source rebuild must publish the native serialized module before
# any request worker has to load the changed unit.
nested_parent_cwasm="$cache_dir/nested-parent.uce.cwasm"
rm -f "$nested_parent_cwasm"
printf '%s\n' \
	'CLI(Request& context) { DValue props; print(component("nested-child", props, context)); /* proactive-serialization-v2 */ }' \
	'RENDER(Request& context) { DValue props; print(component("nested-child", props, context)); /* proactive-serialization-v2 */ }' >"$source_dir/nested-parent.uce"
curl -fsS -H "Host: $http_host" "http://127.0.0.1/$test_name/nested-parent.uce" >/dev/null
deadline=$((SECONDS + 10))
while [[ ! -s "$nested_parent_cwasm" && $SECONDS -lt $deadline ]]; do sleep 0.1; done
if [[ ! -s "$nested_parent_cwasm" ]]; then
	echo "proactive compiler did not serialize rebuilt module" >&2
	exit 1
fi

(
	exec 8>"$nested_child_wasm.lock"
	flock 8
	: >"$nested_lock_ready_file"
	sleep 3
) &
nested_lock_pid=$!
deadline=$((SECONDS + 5))
while [[ ! -e "$nested_lock_ready_file" && $SECONDS -lt $deadline ]]; do
	if ! kill -0 "$nested_lock_pid" 2>/dev/null; then
		echo "nested component lock exited before acquisition" >&2
		exit 1
	fi
	sleep 0.05
done
if [[ ! -e "$nested_lock_ready_file" ]]; then
	echo "nested component lock was not acquired before deadline" >&2
	exit 1
fi
sed -i 's/nested-component-marker-a/nested-component-marker-b/' "$source_dir/nested-child.uce"
started_at=$(date +%s%N)
nested_output=$(scripts/bearer-cli "/$test_name/nested-parent.uce")
elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
wait "$nested_lock_pid"
if [[ "$nested_output" != *"nested-component-marker-b"* ]]; then
	echo "CLI nested component returned stale wasm: $nested_output" >&2
	exit 1
fi
if (( elapsed_ms < 2000 )); then
	echo "CLI nested component did not wait for the active rebuild (${elapsed_ms}ms)" >&2
	exit 1
fi

# Warm every configured worker, then replace the artifact while retaining its
# whole-second mtime. The worker cache must notice the nanosecond/ctime change.
for _ in {1..16}; do assert_marker parent dependency-marker-d; done
sed 's/dependency-marker-d/dependency-marker-c/' "$source_dir/child.uce" >"$source_dir/alternate-child.uce"
sed 's/child.uce/alternate-child.uce/' "$source_dir/parent.uce" >"$source_dir/alternate.uce"
assert_marker alternate dependency-marker-c
alternate_wasm="$cache_dir/alternate.uce.wasm"
parent_mtime=$(stat -c %Y "$parent_wasm")
cp "$alternate_wasm" "$parent_wasm"
touch -d "@$parent_mtime" "$parent_wasm"
rm -f "$cache_dir/parent.uce.cwasm"
for _ in {1..16}; do assert_marker parent dependency-marker-c; done

race_unit="$source_dir/source-race.uce"
race_cpp=""
{
	printf '%s\n' 'String source_race_marker() { return("source-race-marker-a"); }'
	for i in $(seq 1 3500); do
		printf 'int source_race_padding_%s() { return(%s); }\n' "$i" "$i"
	done
	printf '%s\n' 'CLI(Request& context) { print(source_race_marker()); }'
} >"$race_unit"
rm -f "$cache_dir/source-race.uce.cpp" "$cache_dir/source-race.uce.wasm" "$cache_dir/source-race.uce.cwasm" "$cache_dir/source-race.uce.meta.txt"
(
	set +e
	scripts/bearer-cli "/$test_name/source-race.uce" >"$race_output" 2>"$race_error"
	echo "$?" >"$race_status"
) &
race_pid=$!
deadline=$((SECONDS + 10))
while [[ -z "$race_cpp" && $SECONDS -lt $deadline ]]; do
	race_cpp=$(find "$cache_dir" -maxdepth 1 -name 'source-race.uce.invocation-*.cpp' -print -quit 2>/dev/null || true)
	if ! kill -0 "$race_pid" 2>/dev/null; then
		echo "source-race compile finished before staging generated C++" >&2
		cat "$race_output" "$race_error" >&2 || true
		exit 1
	fi
	sleep 0.02
done
if [[ -z "$race_cpp" || ! -e "$race_cpp" ]]; then
	echo "source-race compile did not stage generated C++ before deadline" >&2
	exit 1
fi
if ! grep -q 'source-race-marker-a' "$race_cpp"; then
	echo "source-race generated C++ did not contain the initial marker" >&2
	exit 1
fi
if ! kill -0 "$race_pid" 2>/dev/null; then
	echo "source-race compile finished before source mutation hook" >&2
	cat "$race_output" "$race_error" >&2 || true
	exit 1
fi
sed -i 's/source-race-marker-a/source-race-marker-b/' "$race_unit"
wait "$race_pid"
if [[ "$(cat "$race_status" 2>/dev/null || echo 1)" != "0" ]]; then
	echo "source-race CLI request failed" >&2
	cat "$race_output" "$race_error" >&2 || true
	exit 1
fi
if [[ "$(cat "$race_output")" != *"source-race-marker-b"* ]]; then
	echo "source changed during compile but BEARER served stale wasm: $(cat "$race_output")" >&2
	exit 1
fi

worker_count=$(awk -F= '/^[[:space:]]*WORKER_COUNT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg 2>/dev/null || true)
worker_count="${worker_count:-4}"
worker_pids=""
for _ in {1..48}; do
	output=$(scripts/bearer-cli "/$test_name/parent.uce")
	worker_pids+="${output##*:}"$'\n'
done
unique_workers=$(printf '%s' "$worker_pids" | sed '/^$/d' | sort -u | wc -l)
if (( unique_workers > worker_count )); then
	echo "worker pool recycled during 48 requests: $unique_workers PIDs for $worker_count workers" >&2
	exit 1
fi

echo "dependency invalidation passed"
