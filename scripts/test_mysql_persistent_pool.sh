#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="mysql-persistent-pool-test-$$"
settings_file="${BEARER_SETTINGS_FILE:-/etc/bearer/settings.cfg}"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r "$settings_file" ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	if [[ -n "${configured_site_directory:-}" ]]; then
		site_directory="$configured_site_directory"
	fi
fi
source_dir="$site_directory/$test_name"
bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file" 2>/dev/null || true)
bin_directory="${bin_directory:-/tmp/bearer/work}"
cache_dir=""
http_host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"
test_user="bearer_pool_$$"
test_database="bearer_pool_$$"
test_database_other="bearer_pool_other_$$"
test_password=$(printf '%s' "$test_name-$(date +%s%N)" | sha256sum | cut -c1-32)

cleanup() {
	mariadb -e "DROP DATABASE IF EXISTS \`$test_database\`; DROP DATABASE IF EXISTS \`$test_database_other\`; DROP USER IF EXISTS '$test_user'@'127.0.0.1'" >/dev/null 2>&1 || true
	rm -rf "$source_dir"
	if [[ -n "$cache_dir" ]]; then
		rm -rf "$cache_dir"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"
mariadb -e "DROP DATABASE IF EXISTS \`$test_database\`; DROP DATABASE IF EXISTS \`$test_database_other\`; DROP USER IF EXISTS '$test_user'@'127.0.0.1'; CREATE DATABASE \`$test_database\`; CREATE DATABASE \`$test_database_other\`; CREATE USER '$test_user'@'127.0.0.1' IDENTIFIED BY '$test_password'; GRANT ALL ON \`$test_database\`.* TO '$test_user'@'127.0.0.1'; GRANT ALL ON \`$test_database_other\`.* TO '$test_user'@'127.0.0.1'; CREATE TABLE \`$test_database\`.pool_identity (label VARCHAR(16) NOT NULL); INSERT INTO \`$test_database\`.pool_identity VALUES ('primary'); CREATE TABLE \`$test_database_other\`.pool_identity (label VARCHAR(16) NOT NULL); INSERT INTO \`$test_database_other\`.pool_identity VALUES ('other')"

printf '%s\n' \
	'RENDER(Request& context)' \
	'{' \
	"  MySQL* db = mysql_connect(\"127.0.0.1\", \"$test_user\", \"$test_password\", \"$test_database\");" \
	"  MySQL* other = mysql_connect(\"127.0.0.1\", \"$test_user\", \"$test_password\", \"$test_database_other\");" \
	"  MySQL* unselected = mysql_connect(\"127.0.0.1\", \"$test_user\", \"$test_password\");" \
	'  if(db == 0 || other == 0 || unselected == 0 || !mysql_connected(db) || !mysql_connected(other) || !mysql_connected(unselected)) { context.set_status(500, "MySQL connect failed"); print("connect-failed"); return; }' \
	'  String primary_database, primary_label, other_database, other_label;' \
	'  mysql_query(db, "SELECT DATABASE() AS db, label FROM pool_identity").each([&](DValue row, String key) { primary_database = row["db"].to_string(); primary_label = row["label"].to_string(); });' \
	'  mysql_query(other, "SELECT DATABASE() AS db, label FROM pool_identity").each([&](DValue row, String key) { other_database = row["db"].to_string(); other_label = row["label"].to_string(); });' \
	'  String unselected_database;' \
	'  mysql_query(unselected, "SELECT DATABASE() AS db").each([&](DValue row, String key) { unselected_database = row["db"].to_string(); });' \
	"  bool database_clean = primary_database == \"$test_database\" && primary_label == \"primary\" && other_database == \"$test_database_other\" && other_label == \"other\" && unselected_database == \"\";" \
	'  DValue marker_rows = mysql_query(db, "SELECT @bearer_pool_marker AS marker");' \
	'  String marker; marker_rows.each([&](DValue row, String key) { marker = row["marker"].to_string(); });' \
	'  bool marker_clean = marker == "";' \
	'  mysql_query(db, "SELECT id FROM bearer_persistent_temp LIMIT 1");' \
	'  bool temp_clean = mysql_error(db) != "";' \
	"  mysql_query(db, \"SET @bearer_pool_marker='dirty'\");" \
	'  mysql_query(db, "CREATE TEMPORARY TABLE bearer_persistent_temp (id INT PRIMARY KEY)");' \
	"  mysql_query(db, \"USE \\\`$test_database_other\\\`\");" \
	"  mysql_query(other, \"USE \\\`$test_database\\\`\");" \
	"  mysql_query(unselected, \"USE \\\`$test_database\\\`\");" \
	'  DValue perf = request_perf();' \
	'  String source;' \
	'  perf["mysql_operations"].each([&](DValue operation, String key) { if(operation["op"].to_string() == "connect" && operation["source"].to_string() == "worker") source = "worker"; });' \
	'  print(perf["worker_pid"].to_string(), "|", source, "|", marker_clean ? "clean" : "dirty", "|", temp_clean ? "clean" : "dirty", "|", database_clean ? "clean" : "dirty");' \
	'  mysql_disconnect(unselected);' \
	'  mysql_disconnect(other);' \
	'  mysql_disconnect(db);' \
	'}' >"$source_dir/test.uce"

printf '%s\n' \
	'RENDER(Request& context)' \
	'{' \
	"  MySQL* db = mysql_connect(\"127.0.0.1\", \"$test_user\", \"$test_password\", \"${test_database}_missing\");" \
	'  String error = mysql_error(db);' \
	'  print(!mysql_connected(db) && error != "" ? "database-selection-failed" : "database-selection-was-ignored");' \
	'  mysql_disconnect(db);' \
	'}' >"$source_dir/failure.uce"

failure=$(curl -fsS --max-time 10 -H "Host: $http_host" "http://127.0.0.1/$test_name/failure.uce")
if [[ "$failure" != "database-selection-failed" ]]; then
	echo "Unknown initial database did not surface a connection failure: $failure" >&2
	exit 1
fi

reused=0
for _ in $(seq 1 160); do
	output=$(curl -fsS --max-time 10 -H "Host: $http_host" "http://127.0.0.1/$test_name/test.uce")
	if [[ "$output" == *"|worker|"* ]]; then
		reused=$((reused + 1))
		if [[ "$output" != *"|worker|clean|clean|clean" ]]; then
			echo "Persistent MySQL reuse leaked cross-request state: $output" >&2
			exit 1
		fi
	fi
done
if [[ "$reused" -eq 0 ]]; then
	echo "Persistent MySQL pool did not produce a worker reuse in 160 requests" >&2
	exit 1
fi

echo "Persistent MySQL pool reset passed across $reused reused requests"
