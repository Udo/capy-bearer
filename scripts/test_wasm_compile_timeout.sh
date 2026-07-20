#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

error_page_path=$(sed -n '/^bool render_wasm_error_page(/,/^}/p' src/linux_fastcgi.cpp)
[[ "$error_page_path" == *'get_shared_unit_bounded('* && "$error_page_path" == *'wasm_invocation_remaining_ms(invocation_deadline)'* ]]
[[ "$error_page_path" != *'get_shared_unit(&request'* ]]

if [[ "${1:-}" != --inside ]]; then
	exec timeout --signal=TERM --kill-after=5s 120s unshare --mount --fork --kill-child=TERM "$0" --inside
fi

root="/tmp/bearer-compile-timeout-$$"
site="$root/site"
work="$root/work"
settings="$root/settings.cfg"
log="$root/service.log"
server_pid=""

cleanup() {
	status=$?
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		for _ in $(seq 1 100); do kill -0 "$server_pid" 2>/dev/null || break; sleep 0.02; done
		kill -KILL "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	if (( status != 0 )) && [[ -r "$log" ]]; then cat "$log" >&2; fi
	rm -rf "$root"
	return "$status"
}
trap cleanup EXIT
mkdir -p "$site/components" "$work" "$root/run" "$root/session" "$root/upload"
sed -E '/^[[:space:]]*(BIN_DIRECTORY|PRECOMPILE_FILES_IN|SITE_DIRECTORY|FCGI_SOCKET_PATH|FCGI_PORT|CLI_SOCKET_PATH|WS_BROKER_SOCKET_PATH|HTTP_PORT|HTTP_DOCUMENT_ROOT|SESSION_PATH|TMP_UPLOAD_PATH|WASM_CORE_PATH|WASM_COMPILE_SCRIPT|WASM_INVOCATION_TIMEOUT_MS|WASM_EPOCH_PERIOD_MS|PROACTIVE_COMPILE_ENABLED|WORKER_COUNT|COMPILE_FAILURE_RETRY_SECONDS)[[:space:]]*=/d' \
	/etc/bearer/settings.cfg >"$settings"
cat >>"$settings" <<CFG
BIN_DIRECTORY=$work
PRECOMPILE_FILES_IN=$site
SITE_DIRECTORY=$site
FCGI_SOCKET_PATH=$root/run/fastcgi.sock
FCGI_PORT=
CLI_SOCKET_PATH=$root/run/cli.sock
WS_BROKER_SOCKET_PATH=$root/run/ws.sock
HTTP_PORT=
HTTP_DOCUMENT_ROOT=$site
SESSION_PATH=$root/session
TMP_UPLOAD_PATH=$root/upload
WASM_CORE_PATH=$(pwd)/bin/wasm/core.wasm
WASM_COMPILE_SCRIPT=$root/compile
WASM_INVOCATION_TIMEOUT_MS=2000
WASM_EPOCH_PERIOD_MS=20
PROACTIVE_COMPILE_ENABLED=0
WORKER_COUNT=1
COMPILE_FAILURE_RETRY_SECONDS=60
CFG
mount --bind "$settings" /etc/bearer/settings.cfg

mkdir -p "$root/compiler/src/wasm" "$root/compiler/scripts"
cp -a src/lib "$root/compiler/src/lib"
cp -a src/wasm/abi.h "$root/compiler/src/wasm/abi.h"
cp -a scripts/compile_wasm_unit scripts/build_unit_source_map.py scripts/check_unit_wasm.py "$root/compiler/scripts/"
chmod -R a+rX "$root/compiler"

cat >"$root/compile" <<SHIM
#!/usr/bin/env bash
set -euo pipefail
source_file="\$3"
if [[ -r '$root/stage-timeout' ]] && grep -Fxq "\$source_file" '$root/stage-timeout'; then
	printf '%s\n' "\$\$" >'$root/compiler-pid'
	(sleep 3; touch '$root/late-descendant') &
	cp "\$2/\$(basename "\$source_file").wasm" "\$2/\$5"
	cp "\$2/\$(basename "\$source_file").wasm.source-map" "\$2/\$5.source-map"
	sleep 60
fi
if [[ -r '$root/copy-delay' ]] && grep -Fxq "\$source_file" '$root/copy-delay'; then
	cp "\$2/\$(basename "\$source_file").wasm" "\$2/\$5"
	cp "\$2/\$(basename "\$source_file").wasm.source-map" "\$2/\$5.source-map"
	sleep 1.7
	touch '$root/copy-delay-complete'
	exit 0
fi
if [[ -r '$root/hold' ]] && grep -Fxq "\$source_file" '$root/hold'; then
	printf '%s\n' "\$\$" >'$root/compiler-pid'
	(sleep 3; touch '$root/late-descendant') &
	sleep 60
fi
if [[ -r '$root/silent-fail' ]] && grep -Fxq "\$source_file" '$root/silent-fail'; then exit 23; fi
if [[ -r '$root/empty-success' ]] && grep -Fxq "\$source_file" '$root/empty-success'; then exit 0; fi
if [[ -r '$root/delay' ]] && grep -Fxq "\$source_file" '$root/delay'; then sleep 1; fi
exec '$root/compiler/scripts/compile_wasm_unit' "\$@"
SHIM
chmod +x "$root/compile"

cat >"$site/driver.uce" <<'BEARER'
CLI(Request& context) {
  if(context.get["health"] == "1") { print(request_perf()["worker_pid"].to_u64(), "|health"); return; }
  if(context.get["dynamic"] == "1") { print(component("slow-component.uce")); return; }
  String target = first(context.get["target"], "slow.uce");
  print(unit_compile(target) ? "compiled" : "failed");
}
BEARER
printf '%s\n' 'CLI(Request& context) { print("entry"); }' >"$site/cold-entry.uce"
printf '%s\n' 'CLI(Request& context) { print("slow"); }' >"$site/slow.uce"
printf '%s\n' 'CLI(Request& context) { print("silent"); }' >"$site/silent.uce"
printf '%s\n' 'CLI(Request& context) { print("empty"); }' >"$site/empty.uce"
printf '%s\n' '#load "transitive-child.uce"' 'CLI(Request& context) { print("parent"); }' >"$site/transitive.uce"
printf '%s\n' 'String transitive_value() { return("a"); }' >"$site/transitive-child.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("component"); }' >"$site/components/slow-component.uce"

timeout 40s env BEARER_PRECOMPILE_FILES_IN="$site" BEARER_PRECOMPILE_BIN_DIRECTORY="$work" BEARER_PRECOMPILE_JOBS=1 bin/bearer_fastcgi.linux.bin --precompile >"$root/precompile.log" 2>&1 || { cat "$root/precompile.log" >&2; exit 1; }
cache="$(scripts/unit_cache_directory "$work")$(realpath "$site")"
generation=$(scripts/unit_cache_directory "$work")
service_user=${BEARER_TEST_SERVICE_USER:-www-data}
service_uid=$(id -u "$service_user")
service_gid=$(id -g "$service_user")
generation_before=$(<"$generation/source-generation.txt")
[[ $(sysctl -n fs.protected_hardlinks) == 1 ]]
[[ $(stat -c %u "$cache/slow.uce.wasm") == 0 && $(stat -c %u "$generation/source-generation.txt") == 0 ]]
[[ $(stat -c %u "$cache/slow.uce.wasm.lock") == 0 && $(stat -c %u "$generation/source-generation.txt.lock") == 0 ]]
find "$work" -type d -exec chown "$service_uid:$service_gid" {} +
chown -R "$service_uid:$service_gid" "$root/run" "$root/session" "$root/upload"
chown "$service_uid:$service_gid" "$root"
rm -f "$cache/cold-entry.uce."* "$cache/components/slow-component.uce."*

timeout --signal=TERM --kill-after=5s 90s setpriv --reuid="$service_uid" --regid="$service_gid" --clear-groups bin/bearer_fastcgi.linux.bin >"$log" 2>&1 &
server_pid=$!
for _ in $(seq 1 400); do [[ -S "$root/run/cli.sock" ]] && break; sleep 0.02; done
[[ -S "$root/run/cli.sock" ]] || { cat "$log" >&2; exit 1; }

request() {
	local spec="$1" path="${1%%\?*}"
	if [[ "$spec" == *\?* ]]; then
		local query="${spec#*\?}"
		IFS='&' read -r -a params <<<"$query"
		timeout 5s scripts/bearer-cli --get "$path" "${params[@]}"
	else
		timeout 5s scripts/bearer-cli "$path"
	fi
}
driver=""
for _ in $(seq 1 100); do
	set +e
	driver=$(request '/driver.uce?health=1' 2>/dev/null)
	rc=$?
	set -e
	(( rc == 0 )) && [[ "$driver" == *'|health' ]] && break
	sleep 0.05
done
[[ "$driver" == *'|health' ]] || { echo "driver did not become ready: $driver" >&2; exit 1; }
worker_pid=${driver%%|*}
[[ "$driver" == "$worker_pid|health" ]]
[[ "$(request /driver.uce)" == compiled ]]
[[ $(stat -c %u "$cache/slow.uce.wasm") == "$service_uid" ]]
[[ $(stat -c %u "$generation/source-generation.txt") == "$service_uid" ]]
[[ $(<"$generation/source-generation.txt") != "$generation_before" ]]
! grep -q 'Could not write.*source-generation.txt' "$log"
before=$(sha256sum "$cache/slow.uce.wasm" "$cache/slow.uce.wasm.source-map" "$cache/slow.uce.cpp" "$cache/slow.uce.exports.txt" "$cache/slow.uce.meta.txt")

truncate -s 68719476736 "$cache/slow.uce.cwasm"
printf '%s\n' "$site/slow.uce" >"$root/copy-delay"
rm -f "$root/copy-delay-complete"
started=$(date +%s%N)
set +e
copy_timeout=$(request /driver.uce 2>&1)
set -e
elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
rm -f "$root/copy-delay"
[[ "$copy_timeout" == *BEARER_INVOCATION_TIMEOUT:* ]] || { echo "foreign rollback copy lacked timeout: $copy_timeout" >&2; exit 1; }
(( elapsed >= 1700 && elapsed <= 3500 )) || { echo "foreign rollback copy took ${elapsed}ms" >&2; exit 1; }
[[ -e "$root/copy-delay-complete" ]] || { echo "foreign rollback compiler did not finish before timeout" >&2; exit 1; }
[[ $(stat -c '%u:%s' "$cache/slow.uce.cwasm") == '0:68719476736' ]]
[[ "$before" == "$(sha256sum "$cache/slow.uce.wasm" "$cache/slow.uce.wasm.source-map" "$cache/slow.uce.cpp" "$cache/slow.uce.exports.txt" "$cache/slow.uce.meta.txt")" ]]
! find "$cache" -name '*.invocation-*' -print -quit | grep -q .
rm -f "$cache/slow.uce.cwasm"

assert_timeout() {
	local url="$1" source="$2" started elapsed output
	printf '%s\n' "$source" >"$root/hold"
	rm -f "$root/compiler-pid" "$root/late-descendant"
	started=$(date +%s%N)
	set +e
	output=$(request "$url" 2>&1)
	set -e
	elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
	[[ "$output" == *BEARER_INVOCATION_TIMEOUT:* ]] || { echo "$url lacked timeout: $output" >&2; exit 1; }
	(( elapsed >= 1700 && elapsed <= 3500 )) || { echo "$url took ${elapsed}ms" >&2; exit 1; }
	[[ "$(request '/driver.uce?health=1')" == "$worker_pid|health" ]]
	[[ -s "$root/compiler-pid" ]]
	compiler_pid=$(<"$root/compiler-pid")
	! kill -0 "$compiler_pid" 2>/dev/null || { echo "compiler $compiler_pid survived" >&2; exit 1; }
	sleep 0.1
	[[ ! -e "$root/late-descendant" ]]
	rm -f "$root/hold"
}

assert_timeout /driver.uce "$site/slow.uce"
[[ "$before" == "$(sha256sum "$cache/slow.uce.wasm" "$cache/slow.uce.wasm.source-map" "$cache/slow.uce.cpp" "$cache/slow.uce.exports.txt" "$cache/slow.uce.meta.txt")" ]]
[[ ! -e "$cache/slow.uce.compile.txt" && ! -e "$cache/slow.uce.wasm-check.txt" ]]
! find "$cache" -name '*.invocation-*' -print -quit | grep -q .
[[ "$(request /driver.uce)" == compiled ]]
before=$(sha256sum "$cache/slow.uce.wasm" "$cache/slow.uce.wasm.source-map" "$cache/slow.uce.cpp" "$cache/slow.uce.exports.txt" "$cache/slow.uce.meta.txt")

printf '%s\n' "$site/silent.uce" >"$root/silent-fail"
[[ "$(request '/driver.uce?target=silent.uce')" == failed ]]
grep -q 'status 23' "$cache/silent.uce.compile.txt"
rm -f "$root/silent-fail"
printf '%s\n' "$site/empty.uce" >"$root/empty-success"
[[ "$(request '/driver.uce?target=empty.uce')" == failed ]]
grep -q 'without creating' "$cache/empty.uce.compile.txt"
rm -f "$root/empty-success"
! find "$cache" -name '*.invocation-*' -print -quit | grep -q .

printf '%s\n' "$site/slow.uce" >"$root/stage-timeout"
assert_timeout /driver.uce "$site/slow.uce"
rm -f "$root/stage-timeout"
[[ "$before" == "$(sha256sum "$cache/slow.uce.wasm" "$cache/slow.uce.wasm.source-map" "$cache/slow.uce.cpp" "$cache/slow.uce.exports.txt" "$cache/slow.uce.meta.txt")" ]]

transitive_before=$(sha256sum "$cache/transitive.uce.wasm" "$cache/transitive.uce.wasm.source-map" "$cache/transitive.uce.cpp" "$cache/transitive.uce.exports.txt" "$cache/transitive.uce.meta.txt")
printf '%s\n' 'String transitive_value() { return("b"); }' >"$site/transitive-child.uce"
assert_timeout '/driver.uce?target=transitive.uce' "$site/transitive-child.uce"
[[ "$transitive_before" == "$(sha256sum "$cache/transitive.uce.wasm" "$cache/transitive.uce.wasm.source-map" "$cache/transitive.uce.cpp" "$cache/transitive.uce.exports.txt" "$cache/transitive.uce.meta.txt")" ]]

assert_timeout '/driver.uce?dynamic=1' "$site/components/slow-component.uce"
assert_timeout /cold-entry.uce "$site/cold-entry.uce"

printf '%s\n' "$site/slow.uce" >"$root/delay"
timeout 25s env BEARER_PRECOMPILE_FILES_IN="$site" BEARER_PRECOMPILE_BIN_DIRECTORY="$work" BEARER_PRECOMPILE_JOBS=1 bin/bearer_fastcgi.linux.bin --precompile >/dev/null
rm -f "$root/delay"

exec 8>"$generation/known-bearer-files.txt.lock"
flock 8
started=$(date +%s%N)
set +e
registry_locked=$(request /driver.uce 2>&1)
set -e
elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
flock -u 8
[[ "$registry_locked" == *BEARER_INVOCATION_TIMEOUT:* && "$elapsed" -le 3500 ]]

exec 9>"$cache/slow.uce.wasm.lock"
flock 9
started=$(date +%s%N)
set +e
locked=$(request /driver.uce 2>&1)
set -e
elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
flock -u 9
[[ "$locked" == *BEARER_INVOCATION_TIMEOUT:* && "$elapsed" -le 3500 ]]

echo "wasm compile timeout passed (entry, explicit, dynamic, unit/registry locks, error page, process group, staging, native precompile, foreign-owned artifacts)"
