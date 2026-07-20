#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="component-resolution-ttl-test-$$"
site_directory="${UCE_TEST_SITE_DIRECTORY:-site}"
worker_count="${UCE_TEST_WORKER_COUNT:-4}"
if [[ -r /etc/uce/settings.cfg ]]; then
	if [[ -z "${UCE_TEST_SITE_DIRECTORY:-}" ]]; then
		configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
		site_directory="${configured_site_directory:-$site_directory}"
	fi
	if [[ -z "${UCE_TEST_WORKER_COUNT:-}" ]]; then
		configured_worker_count=$(awk -F= '/^[[:space:]]*WORKER_COUNT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
		worker_count="${configured_worker_count:-$worker_count}"
	fi
fi
http_host="${UCE_TEST_HTTP_HOST:-uce.openfu.com}"
source_dir="$site_directory/$test_name"
bin_directory="${BIN_DIRECTORY:-}"
if [[ -z "$bin_directory" && -r /etc/uce/settings.cfg ]]; then
	bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
fi
bin_directory="${bin_directory:-/tmp/uce/work}"
cache_dir=""

cleanup() {
	rm -rf "$source_dir"
	if [[ -n "$cache_dir" ]]; then
		rm -rf "$cache_dir"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

printf '%s\n' 'RENDER(Request& context) { print("parent:", request_perf()["worker_pid"].to_string(), ":", component("chosen", context)); }' >"$source_dir/parent.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("a"); }' >"$source_dir/a.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("b"); }' >"$source_dir/b.uce"
ln -s a.uce "$source_dir/chosen.uce"

declare -A seen
collect_workers() {
	local method="$1"
	local marker="$2"
	local path="${3:-parent.uce}"
	local deadline=$((SECONDS + 30))
	seen=()
	while (( SECONDS < deadline && ${#seen[@]} < worker_count )); do
		local response status body pid
		response=$(curl -sS --max-time 10 -X "$method" -H "Host: $http_host" -w $'\n%{http_code}' "http://127.0.0.1/$test_name/$path")
		status=${response##*$'\n'}
		body=${response%$'\n'*}
		if [[ "$status" == "503" ]]; then
			sleep 0.1
			continue
		fi
		if [[ "$status" != "200" || "$body" != parent:*":$marker" ]]; then
			echo "component resolution TTL probe failed: method=$method status=$status expected=$marker body=$body" >&2
			exit 1
		fi
		pid=${body#parent:}
		pid=${pid%%:*}
		seen["$pid"]=1
	done
	if (( ${#seen[@]} != worker_count )); then
		echo "component resolution TTL probe reached ${#seen[@]}/$worker_count workers" >&2
		exit 1
	fi
}

collect_missing_workers() {
	local deadline=$((SECONDS + 30))
	seen=()
	while (( SECONDS < deadline && ${#seen[@]} < worker_count )); do
		local response status body pid
		response=$(curl -sS --max-time 10 -H "Host: $http_host" -w $'\n%{http_code}' "http://127.0.0.1/$test_name/missing.uce")
		status=${response##*$'\n'}
		body=${response%$'\n'*}
		if [[ "$status" == "503" ]]; then
			sleep 0.1
			continue
		fi
		if [[ "$status" != "500" || "$body" != parent:* || "$body" != *"component not found: later"* ]]; then
			echo "missing component TTL probe failed: status=$status body=$body" >&2
			exit 1
		fi
		pid=${body#parent:}
		pid=${pid%%:*}
		seen["$pid"]=1
	done
	if (( ${#seen[@]} != worker_count )); then
		echo "missing component TTL probe reached ${#seen[@]}/$worker_count workers" >&2
		exit 1
	fi
}

collect_workers GET a
ln -sfn b.uce "$source_dir/chosen.uce"
collect_workers GET a
collect_workers POST b
collect_workers GET a
sleep 10.2
collect_workers GET b

printf '%s\n' 'RENDER(Request& context) { print("parent:", request_perf()["worker_pid"].to_string(), ":", component("later", context)); }' >"$source_dir/missing.uce"
collect_missing_workers
printf '%s\n' 'COMPONENT(Request& context) { print("created"); }' >"$source_dir/later.uce"
collect_workers GET created missing.uce

echo "component resolution TTL passed"
