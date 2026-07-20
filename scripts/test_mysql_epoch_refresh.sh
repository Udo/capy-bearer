#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="mysql-epoch-refresh-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
settings_file="${BEARER_SETTINGS_FILE:-/etc/bearer/settings.cfg}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r "$settings_file" ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	if [[ -n "${configured_site_directory:-}" ]]; then
		site_directory="$configured_site_directory"
	fi
fi
source_dir="$site_directory/$test_name"
bin_directory="${BIN_DIRECTORY:-}"
if [[ -z "$bin_directory" && -r "$settings_file" ]]; then
	bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
fi
bin_directory="${bin_directory:-/tmp/bearer/work}"
ticks=$(awk -F= '/^[[:space:]]*WASM_EPOCH_DEADLINE_TICKS[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
period_ms=$(awk -F= '/^[[:space:]]*WASM_EPOCH_PERIOD_MS[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
ticks="${ticks:-200}"
period_ms="${period_ms:-50}"
segment_seconds=$(awk -v ticks="$ticks" -v period="$period_ms" 'BEGIN { printf "%.6f", ticks * period * 0.00055 }')
cache_dir=""
test_user="bearer_epoch_$$"
test_password=$(printf '%s' "$test_name-$(date +%s%N)" | sha256sum | cut -c1-32)
test_user_created=false

cleanup() {
	if [[ "$test_user_created" == true ]]; then
		mariadb -e "DROP USER IF EXISTS '$test_user'@'127.0.0.1'" >/dev/null 2>&1 || true
	fi
	rm -rf "$source_dir"
	if [[ -n "$cache_dir" ]]; then
		rm -rf "$cache_dir"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"
mariadb -e "DROP USER IF EXISTS '$test_user'@'127.0.0.1'; CREATE USER '$test_user'@'127.0.0.1' IDENTIFIED BY '$test_password'"
test_user_created=true

printf '%s\n' \
	'void mysql_epoch_burn(f64 seconds) {' \
	'  f64 deadline = time_precise() + seconds;' \
	'  u64 spins = 0; while(time_precise() < deadline) spins++;' \
	'}' \
	'CLI(Request& context) {' \
	"  MySQL* db = mysql_connect(\"127.0.0.1\", \"$test_user\", \"$test_password\");" \
	'  if(db == 0 || mysql_error(db) != "") { print("mysql-connect-failed:", db == 0 ? "null" : mysql_error(db)); return; }' \
	"  mysql_epoch_burn($segment_seconds);" \
	'  mysql_query(db, "SELECT 1 AS value");' \
	"  mysql_epoch_burn($segment_seconds);" \
	'  mysql_query(db, "SELECT 2 AS value");' \
	'  if(mysql_error(db) == "") print("mysql-epoch-refresh-ok"); else print("mysql-query-failed:", mysql_error(db));' \
	'  mysql_disconnect(db);' \
	'}' >"$source_dir/test.uce"

output=$(scripts/bearer-cli "/$test_name/test.uce" 2>&1) || {
	echo "MySQL hostcall did not refresh the guest epoch deadline: $output" >&2
	exit 1
}
if [[ "$output" != "mysql-epoch-refresh-ok" ]]; then
	echo "MySQL epoch refresh failed: $output" >&2
	exit 1
fi

echo "MySQL epoch refresh passed with ${segment_seconds}s guest segments"
