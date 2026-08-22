#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="relative-component-cache-test-$$"
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
mkdir -p "$source_dir/a" "$source_dir/b"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

cat >"$source_dir/entry.capy" <<'EOF'
function CLI(request : dval) {
    print(component("a/parent"), "/", component("b/parent"))
}
EOF
printf '%s\n' 'function COMPONENT(request : dval) { print(component("child")) }' >"$source_dir/a/parent.capy"
printf '%s\n' 'function COMPONENT(request : dval) { print("relative-a") }' >"$source_dir/a/child.capy"
printf '%s\n' 'function COMPONENT(request : dval) { print(component("child")) }' >"$source_dir/b/parent.capy"
printf '%s\n' 'function COMPONENT(request : dval) { print("relative-b") }' >"$source_dir/b/child.capy"

output=$(scripts/bearer-cli "/$test_name/entry.capy")
if [[ "$output" != "relative-a/relative-b" ]]; then
	echo "relative component cache crossed caller boundaries: $output" >&2
	exit 1
fi

echo "relative component cache passed"
