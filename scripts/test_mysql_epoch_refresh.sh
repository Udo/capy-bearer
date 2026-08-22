#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="mysql-epoch-refresh-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
settings_file="${BEARER_SETTINGS_FILE:-/etc/bearer/settings.cfg}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r "$settings_file" ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	[[ -n "$configured_site_directory" ]] && site_directory="$configured_site_directory"
fi
source_dir="$site_directory/$test_name"
bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
bin_directory="${bin_directory:-/tmp/bearer/work}"
ticks=$(awk -F= '/^[[:space:]]*WASM_EPOCH_DEADLINE_TICKS[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
period_ms=$(awk -F= '/^[[:space:]]*WASM_EPOCH_PERIOD_MS[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
segment_seconds=$(awk -v ticks="${ticks:-200}" -v period="${period_ms:-50}" 'BEGIN { printf "%.6f", ticks * period * 0.00055 }')
test_user="bearer_epoch_$$"
test_password=$(printf '%s' "$test_name-$(date +%s%N)" | sha256sum | cut -c1-32)
cache_dir=""
cleanup() { mariadb -e "DROP USER IF EXISTS '$test_user'@'127.0.0.1'" >/dev/null 2>&1 || true; rm -rf "$source_dir" "$cache_dir"; }
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"
mariadb -e "DROP USER IF EXISTS '$test_user'@'127.0.0.1'; CREATE USER '$test_user'@'127.0.0.1' IDENTIFIED BY '$test_password'"
cat >"$source_dir/test.capy" <<EOF
function mysql_epoch_burn(seconds : f64) {
    var deadline := time_precise() + seconds
    while time_precise() < deadline { }
}
function CLI(request : dval) {
    var db := mysql_connect("127.0.0.1", "$test_user", "$test_password", "")
    mysql_epoch_burn($segment_seconds)
    var first := mysql_query(db, "SELECT 1 AS value")
    mysql_epoch_burn($segment_seconds)
    var second := mysql_query(db, "SELECT 2 AS value")
    if mysql_connected(db) && mysql_error(db) == "" { print("mysql-epoch-refresh-ok") }
    else { print("mysql-query-failed:", mysql_error(db)) }
    mysql_disconnect(db)
}
EOF
output=$(scripts/bearer-cli "/$test_name/test.capy" 2>&1) || { echo "MySQL hostcall did not refresh the guest epoch deadline: $output" >&2; exit 1; }
[[ "$output" == "mysql-epoch-refresh-ok" ]] || { echo "MySQL epoch refresh failed: $output" >&2; exit 1; }
echo "MySQL epoch refresh passed with ${segment_seconds}s guest segments"
