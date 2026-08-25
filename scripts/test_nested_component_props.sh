#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="nested-component-props-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r /etc/bearer/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
	if [[ -n "${configured_site_directory:-}" ]]; then
		site_directory="$configured_site_directory"
	fi
fi
source_dir="$site_directory/$test_name"
threshold_ms="${BEARER_NESTED_COMPONENT_PROPS_MAX_MS:-1500}"
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

cat >"$source_dir/parent.capy" <<'EOF'
function CLI(request : dval) {
    request.props.sentinel = "caller"
    var payload := "nested-props"
    var props := {items: []}
    for i := 0..300 { props.items = push(props.items, payload) }
    var output := component("outer", props)
    if string(request.props.sentinel) != "caller" { print("caller props not restored"); return }
    print(output)
}
EOF

cat >"$source_dir/outer.capy" <<'EOF'
function COMPONENT(request : dval) {
    if length(request.props.items) != 300 { print("outer props missing"); return }
    for i := 0..300 {
        var props := {index: "value"}
        component_render("leaf", props)
        if length(request.props.items) != 300 { print("outer props not restored"); return }
    }
    print("nested-component-props-ok")
}
EOF

cat >"$source_dir/leaf.capy" <<'EOF'
function COMPONENT(request : dval) {
    if string(request.props.index) == "" { print("leaf props missing") }
}
EOF

warm_output=$(scripts/bearer-cli "/$test_name/parent.capy")
if [[ "$warm_output" != "nested-component-props-ok" ]]; then
	echo "nested component props warmup failed: $warm_output" >&2
	exit 1
fi

start_ns=$(date +%s%N)
output=$(scripts/bearer-cli "/$test_name/parent.capy")
elapsed_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))
if [[ "$output" != "nested-component-props-ok" ]]; then
	echo "nested component props failed: $output" >&2
	exit 1
fi
if (( elapsed_ms > threshold_ms )); then
	echo "nested component props took ${elapsed_ms}ms (limit ${threshold_ms}ms)" >&2
	exit 1
fi

echo "nested component props passed in ${elapsed_ms}ms"
