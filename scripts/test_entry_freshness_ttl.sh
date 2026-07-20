#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="entry-freshness-ttl-test-$$"
site_directory="${UCE_TEST_SITE_DIRECTORY:-site}"
worker_count="${UCE_TEST_WORKER_COUNT:-4}"
bin_directory="${BIN_DIRECTORY:-/tmp/uce/work}"
http_host="${UCE_TEST_HTTP_HOST:-uce.openfu.com}"
if [[ -r /etc/uce/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	configured_worker_count=$(awk -F= '/^[[:space:]]*WORKER_COUNT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	configured_bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	[[ -n "${UCE_TEST_SITE_DIRECTORY:-}" || -z "$configured_site_directory" ]] || site_directory="$configured_site_directory"
	[[ -n "${UCE_TEST_WORKER_COUNT:-}" || -z "$configured_worker_count" ]] || worker_count="$configured_worker_count"
	[[ -n "${BIN_DIRECTORY:-}" || -z "$configured_bin_directory" ]] || bin_directory="$configured_bin_directory"
fi
source_dir="$site_directory/$test_name"
cache_dir=""
mutation_file="/tmp/$test_name-mutation"
post_body="/tmp/$test_name-post-body"
post_headers="/tmp/$test_name-post-headers"
generation_file=""

cleanup() {
	if [[ -n "$generation_file" ]]; then
		exec 7>"$generation_file.lock"
		flock 7
		printf 'test-cleanup:%s\n' "$(date +%s%N)" >"$generation_file"
		flock -u 7
		exec 7>&-
	fi
	rm -rf "$source_dir"
	[[ -z "$cache_dir" ]] || rm -rf "$cache_dir"
	rm -f "$mutation_file" "$post_body" "$post_headers"
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"
generation_file="$(scripts/unit_cache_directory "$bin_directory")/source-generation.txt"

printf '%s\n' 'String entry_freshness_marker() { return("marker-a"); }' >"$source_dir/child.uce"
printf '%s\n' \
	'#load "child.uce"' \
	"void entry_freshness_output(Request& context) { DValue perf = request_perf(); if(context.params[\"REQUEST_METHOD\"] == \"POST\") file_put_contents(\"$mutation_file\", \"executed\"); print(entry_freshness_marker(), \":\", perf[\"worker_pid\"].to_string(), \":\", perf[\"ready_freshness_cache_hit_count\"].to_string(), \":\", perf[\"ready_freshness_full_check_us\"].to_string()); }" \
	'RENDER(Request& context) { entry_freshness_output(context); }' \
	'CLI(Request& context) { entry_freshness_output(context); }' >"$source_dir/parent.uce"

http_probe() {
	curl -fsS --max-time 5 -H "Host: $http_host" "http://127.0.0.1/$test_name/parent.uce"
}

declare -A seen
collect_all_workers() {
	local require_hit="$1"
	local expected_marker="$2"
	local deadline=$((SECONDS + 35))
	seen=()
	while (( SECONDS < deadline && ${#seen[@]} < worker_count )); do
		local output marker pid hit full
		output=$(http_probe)
		IFS=: read -r marker pid hit full <<<"$output"
		pid=${pid%%.*}; hit=${hit%%.*}; full=${full%%.*}
		if [[ "$marker" != "$expected_marker" || -z "$pid" ]]; then
			echo "entry freshness probe returned unexpected output: $output" >&2
			exit 1
		fi
		if [[ "$require_hit" == "1" && ( "$hit" == "0" || "$full" != "0" ) ]]; then
			continue
		fi
		seen["$pid"]=1
	done
	if (( ${#seen[@]} != worker_count )); then
		echo "entry freshness probe reached ${#seen[@]}/$worker_count workers (hit=$require_hit)" >&2
		exit 1
	fi
}

collect_all_workers 0 marker-a
collect_all_workers 1 marker-a

parent_wasm="$cache_dir/parent.uce.wasm"
exec 8>"$parent_wasm.lock"
flock 8
sed -i 's/marker-a/marker-b/' "$source_dir/child.uce"
rm -f "$mutation_file"
post_status=$(curl -sS --max-time 5 -o "$post_body" -D "$post_headers" -w '%{http_code}' -X POST -H "Host: $http_host" "http://127.0.0.1/$test_name/parent.uce")
if [[ "$post_status" != "503" || -e "$mutation_file" ]] || ! grep -qi '^Retry-After: 1' "$post_headers"; then
	echo "stale mutation did not fail closed: status=$post_status body=$(cat "$post_body")" >&2
	exit 1
fi

sleep 10.2
expired_output=$(http_probe)
IFS=: read -r expired_marker expired_pid expired_hit expired_full <<<"$expired_output"
expired_hit=${expired_hit%%.*}; expired_full=${expired_full%%.*}
if [[ "$expired_marker" != "marker-a" || "$expired_hit" != "0" || "$expired_full" == "0" ]]; then
	echo "expired positive entry did not perform a full safe check: $expired_output" >&2
	exit 1
fi
flock -u 8
exec 8>&-

deadline=$((SECONDS + 25))
output=""
while (( SECONDS < deadline )); do
	output=$(http_probe)
	[[ "$output" == marker-b:* ]] && break
	sleep 0.2
done
if [[ "$output" != marker-b:* ]]; then
	echo "entry did not converge after dependency rebuild: $output" >&2
	exit 1
fi
collect_all_workers 1 marker-b

exec 9>"$generation_file.lock"
flock 9
printf 'test:%s\n' "$(date +%s%N)" >"$generation_file"
flock -u 9
exec 9>&-
generation_output=$(http_probe)
IFS=: read -r generation_marker generation_pid generation_hit generation_full <<<"$generation_output"
generation_hit=${generation_hit%%.*}; generation_full=${generation_full%%.*}
if [[ "$generation_marker" != "marker-b" || "$generation_hit" != "0" || "$generation_full" == "0" ]]; then
	echo "source generation change did not invalidate immediately: $generation_output" >&2
	exit 1
fi

collect_all_workers 1 marker-b
touch "$parent_wasm"
identity_output=$(http_probe)
IFS=: read -r identity_marker identity_pid identity_hit identity_full <<<"$identity_output"
identity_hit=${identity_hit%%.*}; identity_full=${identity_full%%.*}
if [[ "$identity_marker" != "marker-b" || "$identity_hit" != "0" || "$identity_full" == "0" ]]; then
	echo "wasm identity change did not invalidate immediately: $identity_output" >&2
	exit 1
fi

collect_all_workers 1 marker-b
touch "$cache_dir/parent.uce.meta.txt"
metadata_output=$(http_probe)
IFS=: read -r metadata_marker metadata_pid metadata_hit metadata_full <<<"$metadata_output"
metadata_hit=${metadata_hit%%.*}; metadata_full=${metadata_full%%.*}
if [[ "$metadata_marker" != "marker-b" || "$metadata_hit" != "0" || "$metadata_full" == "0" ]]; then
	echo "metadata identity change did not invalidate immediately: $metadata_output" >&2
	exit 1
fi

collect_all_workers 1 marker-b
exec 9>"$generation_file.lock"
flock 9
rm -f "$generation_file"
flock -u 9
missing_generation_output=$(http_probe)
IFS=: read -r missing_generation_marker missing_generation_pid missing_generation_hit missing_generation_full <<<"$missing_generation_output"
missing_generation_hit=${missing_generation_hit%%.*}; missing_generation_full=${missing_generation_full%%.*}
if [[ "$missing_generation_marker" != "marker-b" || "$missing_generation_hit" != "0" || "$missing_generation_full" == "0" ]]; then
	echo "missing generation token did not force a full check: $missing_generation_output" >&2
	exit 1
fi
flock 9
printf 'test:%s\n' "$(date +%s%N)" >"$generation_file"
flock -u 9
exec 9>&-

cli_output=$(timeout --signal=TERM --kill-after=3s 20s scripts/uce-cli "/$test_name/parent.uce")
IFS=: read -r cli_marker cli_pid cli_hit cli_full <<<"$cli_output"
cli_hit=${cli_hit%%.*}; cli_full=${cli_full%%.*}
if [[ "$cli_marker" != "marker-b" || "$cli_hit" != "0" || "$cli_full" == "0" ]]; then
	echo "CLI unexpectedly reused the HTTP freshness cache: $cli_output" >&2
	exit 1
fi

echo "entry freshness TTL passed"
