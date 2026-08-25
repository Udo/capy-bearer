#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="mysql-persistent-pool-test-$$"
settings_file="${BEARER_SETTINGS_FILE:-/etc/bearer/settings.cfg}"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r "$settings_file" ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	[[ -n "$configured_site_directory" ]] && site_directory="$configured_site_directory"
fi
source_dir="$site_directory/$test_name"
bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
bin_directory="${bin_directory:-/tmp/bearer/work}"
http_host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"
test_user="bearer_pool_$$"
test_database="bearer_pool_$$"
test_database_other="bearer_pool_other_$$"
test_password=$(printf '%s' "$test_name-$(date +%s%N)" | sha256sum | cut -c1-32)
cache_dir=""
cleanup() {
	mariadb -e "DROP DATABASE IF EXISTS \`$test_database\`; DROP DATABASE IF EXISTS \`$test_database_other\`; DROP USER IF EXISTS '$test_user'@'127.0.0.1'" >/dev/null 2>&1 || true
	rm -rf "$source_dir" "$cache_dir"
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"
mariadb -e "DROP DATABASE IF EXISTS \`$test_database\`; DROP DATABASE IF EXISTS \`$test_database_other\`; DROP USER IF EXISTS '$test_user'@'127.0.0.1'; CREATE DATABASE \`$test_database\`; CREATE DATABASE \`$test_database_other\`; CREATE USER '$test_user'@'127.0.0.1' IDENTIFIED BY '$test_password'; GRANT ALL ON \`$test_database\`.* TO '$test_user'@'127.0.0.1'; GRANT ALL ON \`$test_database_other\`.* TO '$test_user'@'127.0.0.1'; CREATE TABLE \`$test_database\`.pool_identity (label VARCHAR(16) NOT NULL); INSERT INTO \`$test_database\`.pool_identity VALUES ('primary'); CREATE TABLE \`$test_database_other\`.pool_identity (label VARCHAR(16) NOT NULL); INSERT INTO \`$test_database_other\`.pool_identity VALUES ('other')"
cat >"$source_dir/test.capy" <<EOF
function RENDER(request : dval) {
    var db := mysql_connect("127.0.0.1", "$test_user", "$test_password", "$test_database")
    var other := mysql_connect("127.0.0.1", "$test_user", "$test_password", "$test_database_other")
    var unselected := mysql_connect("127.0.0.1", "$test_user", "$test_password", "")
    var primary := mysql_query(db, "SELECT DATABASE() AS db, label FROM pool_identity")
    var secondary := mysql_query(other, "SELECT DATABASE() AS db, label FROM pool_identity")
    var no_database := mysql_query(unselected, "SELECT DATABASE() AS db")
    var marker := mysql_query(db, "SELECT @bearer_pool_marker AS marker")
    var missing_temp := mysql_query(db, "SELECT id FROM bearer_persistent_temp LIMIT 1")
    var clean := bool(mysql_info(db, "connection")) && bool(mysql_info(other, "connection")) && bool(mysql_info(unselected, "connection")) && string(primary[0]["db"]) == "$test_database" && string(primary[0]["label"]) == "primary" && string(secondary[0]["db"]) == "$test_database_other" && string(secondary[0]["label"]) == "other" && string(no_database[0]["db"]) == "" && string(marker[0]["marker"]) == "" && string(mysql_info(db, "error")) != ""
    var dirty_marker := mysql_query(db, "SET @bearer_pool_marker='dirty'")
    var dirty_temp := mysql_query(db, "CREATE TEMPORARY TABLE bearer_persistent_temp (id INT PRIMARY KEY)")
    var dirty_db := mysql_query(db, "USE $test_database_other")
    var perf := runtime_perf()
    if clean { print(string(perf.worker_pid), "|clean") }
    else { print(string(perf.worker_pid), "|dirty") }
    mysql_disconnect(unselected)
    mysql_disconnect(other)
    mysql_disconnect(db)
}
EOF
cat >"$source_dir/failure.capy" <<EOF
function RENDER(request : dval) {
    var db := mysql_connect("127.0.0.1", "$test_user", "$test_password", "${test_database}_missing")
    if !bool(mysql_info(db, "connection")) && string(mysql_info(db, "error")) != "" { print("database-selection-failed") }
    else { print("database-selection-was-ignored") }
    mysql_disconnect(db)
}
EOF
failure=$(curl -fsS --max-time 10 -H "Host: $http_host" "http://127.0.0.1/$test_name/failure.capy")
[[ "$failure" == database-selection-failed ]] || { echo "Unknown initial database did not surface a connection failure: $failure" >&2; exit 1; }
reused=0
for _ in $(seq 1 160); do
	output=$(curl -fsS --max-time 10 -H "Host: $http_host" "http://127.0.0.1/$test_name/test.capy")
	[[ "$output" == *'|clean' ]] || { echo "Persistent MySQL reuse leaked cross-request state: $output" >&2; exit 1; }
	[[ "$output" != '|clean' ]] && reused=$((reused + 1))
done
[[ $reused -gt 0 ]] || { echo "Persistent MySQL pool did not report a worker PID" >&2; exit 1; }
echo "Persistent MySQL pool reset passed across $reused requests"
