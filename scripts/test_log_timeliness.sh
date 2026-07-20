#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="log-timeliness-test-$$"
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

cleanup() {
	rm -rf "$source_dir"
	if [[ -n "$cache_dir" ]]; then
		rm -rf "$cache_dir"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

printf '%s\n' 'CLI(Request& context) { deliberate_log_timeliness_compile_failure }' >"$source_dir/probe.uce"
rm -rf "$cache_dir"
started_at=$(date '+%Y-%m-%d %H:%M:%S')

set +e
scripts/bearer-cli --get "/$test_name/probe.uce" __bearer_expected_compile_failure=1 >/dev/null 2>&1
set -e

for _ in $(seq 1 20); do
	journal_output=$(journalctl -u bearer --since "$started_at" --no-pager)
	if [[ "$journal_output" == *"BEARER expected compile error"* && "$journal_output" == *"$test_name/probe.uce"* ]]; then
		echo "log timeliness passed"
		exit 0
	fi
	sleep 0.1
done

echo "compile diagnostic was not journaled promptly: $test_name/probe.uce" >&2
exit 1
