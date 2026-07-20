#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != "--inside" ]]; then
	exec timeout --signal=TERM --kill-after=5s 120s unshare --mount --fork --kill-child=TERM "$0" --inside
fi

root="/tmp/bearer-metadata-deadline-$$"
site="$root/site"
work="$root/work"
settings="$root/settings.cfg"
socket="$root/run/cli.sock"
log="$root/service.log"
control="$root/pread.control"
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
sed -E '/^[[:space:]]*(BIN_DIRECTORY|PRECOMPILE_FILES_IN|SITE_DIRECTORY|FCGI_SOCKET_PATH|FCGI_PORT|CLI_SOCKET_PATH|WS_BROKER_SOCKET_PATH|HTTP_PORT|HTTP_DOCUMENT_ROOT|SESSION_PATH|TMP_UPLOAD_PATH|WASM_CORE_PATH|WASM_INVOCATION_TIMEOUT_MS|WASM_EPOCH_PERIOD_MS|PROACTIVE_COMPILE_ENABLED|SERVE_LAST_KNOWN_GOOD|SHOW_DYNAMIC_COMPILE_ERRORS|WORKER_COUNT)[[:space:]]*=/d' \
	/etc/bearer/settings.cfg >"$settings"
cat >>"$settings" <<CFG
BIN_DIRECTORY=$work
PRECOMPILE_FILES_IN=$site
SITE_DIRECTORY=$site
FCGI_SOCKET_PATH=$root/run/fastcgi.sock
FCGI_PORT=
CLI_SOCKET_PATH=$socket
WS_BROKER_SOCKET_PATH=$root/run/ws.sock
HTTP_PORT=
HTTP_DOCUMENT_ROOT=$site
SESSION_PATH=$root/session
TMP_UPLOAD_PATH=$root/upload
WASM_CORE_PATH=$(pwd)/bin/wasm/core.wasm
WASM_INVOCATION_TIMEOUT_MS=1000
WASM_EPOCH_PERIOD_MS=10
PROACTIVE_COMPILE_ENABLED=0
SERVE_LAST_KNOWN_GOOD=0
SHOW_DYNAMIC_COMPILE_ERRORS=1
WORKER_COUNT=1
CFG
mount --bind "$settings" /etc/bearer/settings.cfg

cat >"$site/driver.uce" <<'BEARER'
CLI(Request& context) {
  if(context.get["health"] == "1") { print(request_perf()["worker_pid"].to_u64(), "|health"); return; }
  print(component(context.get["target"], context));
}
BEARER
printf '%s\n' 'COMPONENT(Request& context) { print("deadline-component"); }' >"$site/components/deadline.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("truncate-component"); }' >"$site/components/truncate.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("mutation-component"); }' >"$site/components/mutation.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("uleb-component"); }' >"$site/components/uleb.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("duplicate-component"); }' >"$site/components/duplicate.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("full-component"); }' >"$site/components/full.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("full-uleb-component"); }' >"$site/components/full-uleb.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("full-duplicate-component"); }' >"$site/components/full-duplicate.uce"

timeout --signal=TERM --kill-after=5s 60s env BEARER_PRECOMPILE_FILES_IN="$site" BEARER_PRECOMPILE_BIN_DIRECTORY="$work" \
	BEARER_PRECOMPILE_JOBS=1 bin/bearer_fastcgi.linux.bin --precompile >"$root/precompile.log" 2>&1
cache="$(scripts/unit_cache_directory "$work")$(realpath "$site")"
for unit in deadline truncate mutation uleb duplicate full full-uleb full-duplicate; do
	[[ -s "$cache/components/$unit.uce.wasm" && -s "$cache/components/$unit.uce.cwasm" ]]
	cp "$cache/components/$unit.uce.wasm" "$root/$unit.original.wasm"
done

cat >"$root/interpose.c" <<'C'
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct control {
	char target[PATH_MAX];
	char mode[16];
	int delay_ms;
	char ready[PATH_MAX];
	char release[PATH_MAX];
	char counter[PATH_MAX];
};

static int read_control(int fd, struct control* control)
{
	const char* path = getenv("BEARER_TEST_PREAD_CONTROL");
	if(!path || !*path) return 0;
	int input = open(path, O_RDONLY | O_CLOEXEC);
	if(input < 0) return 0;
	char text[PATH_MAX * 4];
	ssize_t count = read(input, text, sizeof(text) - 1);
	close(input);
	if(count <= 0) return 0;
	text[count] = 0;
	if(sscanf(text, "%4095s %15s %d %4095s %4095s %4095s", control->target, control->mode,
		&control->delay_ms, control->ready, control->release, control->counter) != 6) return 0;
	char link[64], resolved[PATH_MAX];
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	ssize_t length = readlink(link, resolved, sizeof(resolved) - 1);
	if(length <= 0) return 0;
	resolved[length] = 0;
	return strcmp(resolved, control->target) == 0;
}

static void mark(const char* path)
{
	if(strcmp(path, "-") == 0) return;
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if(fd >= 0) { ssize_t ignored = write(fd, "1\n", 2); (void)ignored; close(fd); }
}

static void wait_release(const struct control* control)
{
	if(strcmp(control->release, "-") == 0) return;
	struct timespec started, now, pause = { 0, 10000000 };
	clock_gettime(CLOCK_MONOTONIC, &started);
	while(access(control->release, F_OK) != 0)
	{
		nanosleep(&pause, 0);
		clock_gettime(CLOCK_MONOTONIC, &now);
		if(now.tv_sec - started.tv_sec >= 2) break;
	}
}

static void delay_ms(int milliseconds)
{
	if(milliseconds <= 0) return;
	struct timespec pause = { milliseconds / 1000, (milliseconds % 1000) * 1000000L };
	while(nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
}

static void before_read(const struct control* control)
{
	mark(control->counter);
	if(strcmp(control->mode, "before") == 0 && access(control->ready, F_OK) != 0)
	{
		mark(control->ready);
		wait_release(control);
	}
	if(strcmp(control->mode, "delay") == 0) delay_ms(control->delay_ms);
}

static void after_read(const struct control* control)
{
	if(strcmp(control->mode, "after") == 0 && access(control->ready, F_OK) != 0)
	{
		mark(control->ready);
		wait_release(control);
	}
}

ssize_t pread(int fd, void* buffer, size_t size, off_t offset)
{
	static ssize_t (*real_pread)(int, void*, size_t, off_t);
	if(!real_pread) real_pread = dlsym(RTLD_NEXT, "pread");
	struct control control;
	int targeted = read_control(fd, &control);
	if(targeted) before_read(&control);
	ssize_t result = real_pread(fd, buffer, size, offset);
	if(targeted) after_read(&control);
	return result;
}

ssize_t pread64(int fd, void* buffer, size_t size, off64_t offset)
{
	static ssize_t (*real_pread64)(int, void*, size_t, off64_t);
	if(!real_pread64) real_pread64 = dlsym(RTLD_NEXT, "pread64");
	struct control control;
	int targeted = read_control(fd, &control);
	if(targeted) before_read(&control);
	ssize_t result = real_pread64(fd, buffer, size, offset);
	if(targeted) after_read(&control);
	return result;
}
C
timeout 15s cc -shared -fPIC -O2 -Wall -Wextra -Werror -o "$root/interpose.so" "$root/interpose.c" -ldl

timeout --signal=TERM --kill-after=5s 90s env LD_PRELOAD="$root/interpose.so" BEARER_TEST_PREAD_CONTROL="$control" \
	bin/bearer_fastcgi.linux.bin >"$log" 2>&1 &
server_pid=$!
for _ in $(seq 1 400); do [[ -S "$socket" ]] && break; sleep 0.02; done
[[ -S "$socket" ]] || { cat "$log" >&2; exit 1; }

request() {
	timeout --signal=TERM --kill-after=1s 5s scripts/bearer-cli --socket "$socket" --get /driver.uce "$@"
}

health=$(request health=1)
worker_pid=${health%%|*}
[[ "$health" == "$worker_pid|health" ]]

restore() {
	local unit="$1" wasm="$cache/components/$1.uce.wasm" cwasm="$cache/components/$1.uce.cwasm"
	cp "$root/$unit.original.wasm" "$wasm"
	python3 - "$wasm" "$cwasm" <<'PY'
import os, pathlib, sys
wasm, cwasm = map(pathlib.Path, sys.argv[1:])
stamp = cwasm.stat().st_mtime_ns - 1_000_000
os.utime(wasm, ns=(stamp, stamp))
PY
}

prepare_deadline() {
	local wasm="$cache/components/deadline.uce.wasm" cwasm="$cache/components/deadline.uce.cwasm"
	python3 - "$wasm" "$cwasm" <<'PY'
import os, pathlib, sys
wasm, cwasm = map(pathlib.Path, sys.argv[1:])
with wasm.open("ab") as output:
    output.write(b"\0\1\0" * 7000)
stamp = cwasm.stat().st_mtime_ns - 1_000_000
os.utime(wasm, ns=(stamp, stamp))
PY
}

prepare_mutation() {
	local wasm="$cache/components/mutation.uce.wasm" cwasm="$cache/components/mutation.uce.cwasm"
	python3 - "$wasm" "$cwasm" "$root/mutation-offset" <<'PY'
import os, pathlib, sys
wasm, cwasm, offset_file = map(pathlib.Path, sys.argv[1:])
payload = bytes([7]) + b"padding" + bytes(8192)
value = len(payload)
encoded = bytearray()
while True:
    byte = value & 0x7f
    value >>= 7
    encoded.append(byte | (0x80 if value else 0))
    if not value:
        break
with wasm.open("ab") as output:
    output.write(bytes([0]) + encoded + payload)
offset_file.write_text(str(wasm.stat().st_size - 4096))
stamp = cwasm.stat().st_mtime_ns - 1_000_000
os.utime(wasm, ns=(stamp, stamp))
PY
}

wait_ready() {
	local ready="$1" request_pid="$2" deadline=$((SECONDS + 3))
	while [[ ! -e "$ready" ]] && kill -0 "$request_pid" 2>/dev/null && (( SECONDS < deadline )); do sleep 0.01; done
	[[ -e "$ready" ]] || { echo "metadata read gate did not activate" >&2; return 1; }
}

prepare_deadline
deadline_wasm="$cache/components/deadline.uce.wasm"
deadline_counter="$root/deadline.counter"
printf '%s delay 1100 - - %s\n' "$deadline_wasm" "$deadline_counter" >"$control"
started=$(date +%s%N)
set +e
deadline_output=$(request target=components/deadline.uce 2>&1)
set -e
elapsed_ms=$(( ($(date +%s%N) - started) / 1000000 ))
[[ "$deadline_output" == *BEARER_INVOCATION_TIMEOUT:* ]] || { echo "metadata scan lacked canonical timeout: $deadline_output" >&2; exit 1; }
(( elapsed_ms >= 1000 && elapsed_ms < 3000 )) || { echo "metadata timeout took ${elapsed_ms}ms" >&2; exit 1; }
[[ $(wc -l <"$deadline_counter") -eq 1 ]] || { echo "metadata scanner read past the first delayed refill" >&2; exit 1; }
rm -f "$control"
[[ "$(request health=1)" == "$worker_pid|health" ]]
restore deadline
[[ "$(request target=components/deadline.uce)" == deadline-component ]]

rm -f "$cache/components/deadline.uce.cwasm"
touch "$deadline_wasm"
printf '%s delay 1100 - - %s\n' "$deadline_wasm" "$root/full-deadline.counter" >"$control"
started=$(date +%s%N)
set +e
full_deadline_output=$(request target=components/deadline.uce 2>&1)
set -e
elapsed_ms=$(( ($(date +%s%N) - started) / 1000000 ))
[[ "$full_deadline_output" == *BEARER_INVOCATION_TIMEOUT:* ]] || { echo "full artifact read lacked canonical timeout: $full_deadline_output" >&2; exit 1; }
(( elapsed_ms >= 1000 && elapsed_ms < 3000 )) || { echo "full artifact timeout took ${elapsed_ms}ms" >&2; exit 1; }
[[ $(wc -l <"$root/full-deadline.counter") -eq 1 ]] || { echo "full artifact reader continued after the first delayed chunk" >&2; exit 1; }
rm -f "$control"
[[ "$(request health=1)" == "$worker_pid|health" ]]
[[ "$(request target=components/deadline.uce)" == deadline-component ]]
[[ -s "$cache/components/deadline.uce.cwasm" ]]

truncate_wasm="$cache/components/truncate.uce.wasm"
truncate_ready="$root/truncate.ready"
truncate_release="$root/truncate.release"
printf '%s before 0 %s %s %s\n' "$truncate_wasm" "$truncate_ready" "$truncate_release" "$root/truncate.counter" >"$control"
( set +e; request target=components/truncate.uce >"$root/truncate.output" 2>&1; printf '%s\n' "$?" >"$root/truncate.status" ) &
request_pid=$!
wait_ready "$truncate_ready" "$request_pid"
truncate -s 4 "$truncate_wasm"
touch "$truncate_release"
wait "$request_pid"
[[ $(<"$root/truncate.status") -ne 0 ]]
grep -q '\[wasm\] component load failed: not a supported wasm module' "$log"
! grep -q 'truncate-component' "$root/truncate.output"
rm -f "$control"
[[ "$(request health=1)" == "$worker_pid|health" ]]
restore truncate
[[ "$(request target=components/truncate.uce)" == truncate-component ]]

prepare_mutation
mutation_wasm="$cache/components/mutation.uce.wasm"
mutation_ready="$root/mutation.ready"
mutation_release="$root/mutation.release"
printf '%s after 0 %s %s %s\n' "$mutation_wasm" "$mutation_ready" "$mutation_release" "$root/mutation.counter" >"$control"
( set +e; request target=components/mutation.uce >"$root/mutation.output" 2>&1; printf '%s\n' "$?" >"$root/mutation.status" ) &
request_pid=$!
wait_ready "$mutation_ready" "$request_pid"
python3 - "$mutation_wasm" "$(<"$root/mutation-offset")" <<'PY'
import os, pathlib, sys
path = pathlib.Path(sys.argv[1])
offset = int(sys.argv[2])
with path.open("r+b", buffering=0) as artifact:
    artifact.seek(offset)
    byte = artifact.read(1)
    artifact.seek(offset)
    artifact.write(bytes([byte[0] ^ 1]))
    os.fsync(artifact.fileno())
PY
touch "$mutation_release"
wait "$request_pid"
[[ $(<"$root/mutation.status") -ne 0 ]]
grep -q '\[wasm\] component load failed: wasm artifact changed while loading metadata' "$log"
! grep -q 'mutation-component' "$root/mutation.output"
rm -f "$control"
[[ "$(request health=1)" == "$worker_pid|health" ]]
restore mutation
[[ "$(request target=components/mutation.uce)" == mutation-component ]]

uleb_wasm="$cache/components/uleb.uce.wasm"
uleb_cwasm="$cache/components/uleb.uce.cwasm"
python3 - "$uleb_wasm" "$uleb_cwasm" <<'PY'
import os, pathlib, sys
wasm, cwasm = map(pathlib.Path, sys.argv[1:])
with wasm.open("r+b", buffering=0) as artifact:
    artifact.seek(9)
    artifact.write(bytes([0x80]) * 9 + bytes([0x02]))
    os.fsync(artifact.fileno())
stamp = cwasm.stat().st_mtime_ns - 1_000_000
os.utime(wasm, ns=(stamp, stamp))
PY
set +e
uleb_output=$(request target=components/uleb.uce 2>&1)
set -e
grep -q '\[wasm\] component load failed: malformed wasm section header' "$log"
[[ "$uleb_output" != *uleb-component* ]]
[[ "$(request health=1)" == "$worker_pid|health" ]]
restore uleb
[[ "$(request target=components/uleb.uce)" == uleb-component ]]

duplicate_wasm="$cache/components/duplicate.uce.wasm"
duplicate_cwasm="$cache/components/duplicate.uce.cwasm"
python3 - "$duplicate_wasm" "$duplicate_cwasm" <<'PY'
import os, pathlib, sys
wasm, cwasm = map(pathlib.Path, sys.argv[1:])
payload = bytes([13]) + b"bearer.module" + b"duplicate"
value = len(payload)
encoded = bytearray()
while True:
    byte = value & 0x7f
    value >>= 7
    encoded.append(byte | (0x80 if value else 0))
    if not value:
        break
with wasm.open("ab") as artifact:
    artifact.write(bytes([0]) + encoded + payload)
stamp = cwasm.stat().st_mtime_ns - 1_000_000
os.utime(wasm, ns=(stamp, stamp))
PY
set +e
duplicate_output=$(request target=components/duplicate.uce 2>&1)
set -e
grep -q '\[wasm\] component load failed: duplicate bearer.module metadata section' "$log"
[[ "$duplicate_output" != *duplicate-component* ]]
[[ "$(request health=1)" == "$worker_pid|health" ]]
restore duplicate
[[ "$(request target=components/duplicate.uce)" == duplicate-component ]]

full_wasm="$cache/components/full.uce.wasm"
full_cwasm="$cache/components/full.uce.cwasm"
python3 - "$full_wasm" <<'PY'
import pathlib, sys
wasm = pathlib.Path(sys.argv[1])
payload = bytes([10]) + b"bearer.abi" + bytes(1024 * 1024)
value = len(payload)
encoded = bytearray()
while True:
    byte = value & 0x7f
    value >>= 7
    encoded.append(byte | (0x80 if value else 0))
    if not value:
        break
with wasm.open("ab") as artifact:
    artifact.write(bytes([0]) + encoded + payload)
PY
rm -f "$full_cwasm"
set +e
full_output=$(request target=components/full.uce 2>&1)
set -e
grep -Eq '\[wasm\] component load failed: .*oversized wasm metadata section' "$log"
[[ "$full_output" != *full-component* ]]
[[ "$(request health=1)" == "$worker_pid|health" ]]
cp "$root/full.original.wasm" "$full_wasm"
[[ "$(request target=components/full.uce)" == full-component ]]
[[ -s "$full_cwasm" ]]

full_uleb_wasm="$cache/components/full-uleb.uce.wasm"
full_uleb_cwasm="$cache/components/full-uleb.uce.cwasm"
python3 - "$full_uleb_wasm" <<'PY'
import os, pathlib, sys
wasm = pathlib.Path(sys.argv[1])
with wasm.open("r+b", buffering=0) as artifact:
    artifact.seek(9)
    artifact.write(bytes([0x80]) * 9 + bytes([0x02]))
    os.fsync(artifact.fileno())
PY
rm -f "$full_uleb_cwasm"
malformed_before=$(grep -c 'malformed wasm section header' "$log" || true)
set +e
full_uleb_output=$(request target=components/full-uleb.uce 2>&1)
set -e
malformed_after=$(grep -c 'malformed wasm section header' "$log" || true)
(( malformed_after == malformed_before + 1 ))
[[ "$full_uleb_output" != *full-uleb-component* ]]
[[ "$(request health=1)" == "$worker_pid|health" ]]
cp "$root/full-uleb.original.wasm" "$full_uleb_wasm"
[[ "$(request target=components/full-uleb.uce)" == full-uleb-component ]]
[[ -s "$full_uleb_cwasm" ]]

full_duplicate_wasm="$cache/components/full-duplicate.uce.wasm"
full_duplicate_cwasm="$cache/components/full-duplicate.uce.cwasm"
python3 - "$full_duplicate_wasm" <<'PY'
import pathlib, sys
wasm = pathlib.Path(sys.argv[1])
payload = bytes([13]) + b"bearer.module" + b"duplicate"
value = len(payload)
encoded = bytearray()
while True:
    byte = value & 0x7f
    value >>= 7
    encoded.append(byte | (0x80 if value else 0))
    if not value:
        break
with wasm.open("ab") as artifact:
    artifact.write(bytes([0]) + encoded + payload)
PY
rm -f "$full_duplicate_cwasm"
duplicate_before=$(grep -c 'duplicate bearer.module metadata section' "$log" || true)
set +e
full_duplicate_output=$(request target=components/full-duplicate.uce 2>&1)
set -e
duplicate_after=$(grep -c 'duplicate bearer.module metadata section' "$log" || true)
(( duplicate_after == duplicate_before + 1 ))
[[ "$full_duplicate_output" != *full-duplicate-component* ]]
[[ "$(request health=1)" == "$worker_pid|health" ]]
cp "$root/full-duplicate.original.wasm" "$full_duplicate_wasm"
[[ "$(request target=components/full-duplicate.uce)" == full-duplicate-component ]]
[[ -s "$full_duplicate_cwasm" ]]

! grep -Eiq 'panic|segfault|permission denied' "$log"
echo "wasm metadata deadline passed: streamed and full-read timeouts, mutation guards, and parser bounds passed; same worker recovered"
