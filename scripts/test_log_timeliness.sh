#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="log-timeliness-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
settings_file="${BEARER_SETTINGS_FILE:-/etc/bearer/settings.cfg}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r "$settings_file" ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	[[ -n "$configured_site_directory" ]] && site_directory="$configured_site_directory"
fi
source_dir="$site_directory/$test_name"
bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
bin_directory="${bin_directory:-/tmp/bearer/work}"
cache_dir=""
cleanup() { rm -rf "$source_dir" "$cache_dir"; }
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"
printf '%s\n' 'function CLI(request : dval) { deliberate_log_timeliness_compile_failure }' >"$source_dir/probe.capy"
started_at=$(date -d '1 second ago' '+%Y-%m-%d %H:%M:%S')
set +e
scripts/bearer-cli --get "/$test_name/probe.capy" __bearer_expected_compile_failure=1 >/dev/null 2>&1
rc=$?
set -e
[[ $rc -ne 0 ]] || { echo "controlled Capy compile failure succeeded" >&2; exit 1; }
for _ in $(seq 1 20); do
	journal_output=$(journalctl -u bearer --since "$started_at" --no-pager)
	if grep -Fq 'BEARER expected compile error' <<<"$journal_output" && grep -Fq "$test_name/probe.capy" <<<"$journal_output"; then
		echo "log timeliness passed"
		exit 0
	fi
	sleep 0.1
done
echo "compile diagnostic was not journaled promptly: $test_name/probe.capy" >&2
exit 1
