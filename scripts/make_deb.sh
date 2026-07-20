#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEB_ASSET_DIR="$SCRIPT_DIR/deb"
PACKAGE_NAME="uce"
REVISION="${UCE_DEB_REVISION:-}"

usage() {
	cat <<'EOF'
Usage:
  scripts/make_deb.sh [VERSION]

When VERSION is omitted, scripts/make_deb.sh reads VERSION from version.txt.

Environment:
  UCE_DEB_REVISION             Optional Debian package revision suffix
  UCE_DEB_ARCH                 Override package architecture
  UCE_DEB_WEBROOT              Public web root staged into the package (default: /var/www/html)
  UCE_DEB_BUNDLE_WASI_SDK      Bundle pinned /opt/wasi-sdk into the package (default: 1)
  UCE_DEB_BUNDLE_WASMTIME      Bundle /opt/wasmtime into the package (default: 1)
EOF
}

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Required command not found: $1" >&2
		exit 1
	fi
}

resolve_arch() {
	if [[ -n "${UCE_DEB_ARCH:-}" ]]; then
		printf '%s\n' "$UCE_DEB_ARCH"
		return
	fi
	if command -v dpkg-architecture >/dev/null 2>&1; then
		dpkg-architecture -qDEB_HOST_ARCH
		return
	fi
	dpkg --print-architecture
}

validate_version() {
	local version="$1"
	if [[ ! "$version" =~ ^[0-9][A-Za-z0-9.+:~-]*$ ]]; then
		echo "Invalid Debian version string: $version" >&2
		exit 1
	fi
}

copy_payload() {
	local destination="$1"
	local webroot="$2"
	local stage_dir="$3"
	local path
	for path in LICENSE README.md codesearch bin scripts src docs; do
		cp -a "$REPO_ROOT/$path" "$destination/"
	done
	mkdir -p "$destination/etc" "$stage_dir$webroot"
	cp -a "$REPO_ROOT/etc/uce" "$destination/etc/"
	cp -a "$REPO_ROOT/site/." "$stage_dir$webroot/"
}

write_packaged_settings() {
	local output_file="$1"
	local webroot="$2"
	python3 - "$REPO_ROOT/etc/uce/settings.cfg" "$output_file" "$webroot" <<'PY'
from pathlib import Path
import sys
src, dst, webroot = sys.argv[1:4]
s = Path(src).read_text()
replacements = {
    "SITE_DIRECTORY=site": f"SITE_DIRECTORY={webroot}",
    "WASM_CORE_PATH=/Code/uce.openfu.com/uce/bin/wasm/core.wasm": "WASM_CORE_PATH=/usr/lib/uce/bin/wasm/core.wasm",
    "HTTP_DOCUMENT_ROOT=": f"HTTP_DOCUMENT_ROOT={webroot}",
}
for old, new in replacements.items():
    if old in s:
        s = s.replace(old, new)
if "WASM_COMPILE_SCRIPT=" not in s:
    s += "\nWASM_COMPILE_SCRIPT=scripts/compile_wasm_unit\n"
Path(dst).write_text(s)
PY
}

write_control_file() {
	local output_file="$1"
	local package_version="$2"
	local arch="$3"
	local installed_size="$4"

	sed \
		-e "s/@PACKAGE_NAME@/$PACKAGE_NAME/g" \
		-e "s/@VERSION@/$package_version/g" \
		-e "s/@ARCH@/$arch/g" \
		-e "s/@INSTALLED_SIZE@/$installed_size/g" \
		"$DEB_ASSET_DIR/control.in" > "$output_file"
}

bundle_wasi_sdk() {
	local stage_dir="$1"
	local wasi_root="${WASI_SDK:-/opt/wasi-sdk}"
	if [[ "${UCE_DEB_BUNDLE_WASI_SDK:-1}" != "1" ]]; then
		return
	fi
	if [[ ! -x "$wasi_root/bin/clang++" || ! -x "$wasi_root/bin/wasm-ld" || ! -x "$wasi_root/bin/llvm-nm" ]]; then
		echo "UCE_DEB_BUNDLE_WASI_SDK=1 but WASI_SDK does not point at a complete SDK: $wasi_root" >&2
		exit 1
	fi
	local resolved
	resolved="$(readlink -f "$wasi_root")"
	local base
	base="$(basename "$resolved")"
	mkdir -p "$stage_dir/opt"
	cp -a "$resolved" "$stage_dir/opt/$base"
	ln -sfn "$base" "$stage_dir/opt/wasi-sdk"
}

bundle_wasmtime() {
	local stage_dir="$1"
	local wasmtime_root="${WASMTIME_HOME:-/opt/wasmtime}"
	if [[ "${UCE_DEB_BUNDLE_WASMTIME:-1}" != "1" ]]; then
		return
	fi
	if [[ ! -f "$wasmtime_root/include/wasmtime.hh" || ! -f "$wasmtime_root/lib/libwasmtime.so" ]]; then
		echo "UCE_DEB_BUNDLE_WASMTIME=1 but WASMTIME_HOME does not point at a complete C API tree: $wasmtime_root" >&2
		exit 1
	fi
	local resolved
	resolved="$(readlink -f "$wasmtime_root")"
	local base
	base="$(basename "$resolved")"
	mkdir -p "$stage_dir/opt"
	cp -a "$resolved" "$stage_dir/opt/$base"
	ln -sfn "$base" "$stage_dir/opt/wasmtime"
}

write_md5sums() {
	local stage_dir="$1"
	(
		cd "$stage_dir"
		find usr etc lib opt -type f -print0 2>/dev/null | sort -z | xargs -0 --no-run-if-empty md5sum > DEBIAN/md5sums
	)
}

if [[ $# -gt 1 ]]; then
	usage >&2
	exit 1
fi

if [[ $# -eq 1 ]]; then
	VERSION="$1"
else
	if [[ ! -r "$REPO_ROOT/version.txt" ]]; then
		echo "Missing version.txt and no VERSION argument supplied" >&2
		exit 1
	fi
	# shellcheck disable=SC1091
	. "$REPO_ROOT/version.txt"
	VERSION="${VERSION:-}"
fi
validate_version "$VERSION"

require_command bash
require_command clang++
require_command dpkg-deb
require_command dpkg
require_command install
require_command find
require_command xargs
require_command md5sum
require_command mysql_config
require_command du
require_command sed
require_command awk
require_command cp
require_command sort

ARCH="$(resolve_arch)"
PACKAGE_VERSION="$VERSION"
if [[ -n "$REVISION" ]]; then
	PACKAGE_VERSION="${PACKAGE_VERSION}-${REVISION}"
fi
PACKAGE_BASENAME="${PACKAGE_NAME}_${PACKAGE_VERSION}_${ARCH}"
STAGE_DIR="$REPO_ROOT/pkg/$PACKAGE_BASENAME"
DEBIAN_DIR="$STAGE_DIR/DEBIAN"
INSTALL_ROOT="$STAGE_DIR/usr/lib/uce"
WEBROOT="${UCE_DEB_WEBROOT:-/var/www/html}"
DIST_DIR="$REPO_ROOT/dist"
OUTPUT_DEB="$DIST_DIR/$PACKAGE_BASENAME.deb"

echo "Making package $PACKAGE_BASENAME"
echo "==================================="

bash "$REPO_ROOT/scripts/build_linux.sh"

rm -rf -- "$STAGE_DIR"
mkdir -p "$DEBIAN_DIR" "$INSTALL_ROOT" "$STAGE_DIR/etc/uce" "$STAGE_DIR/lib/systemd/system" "$DIST_DIR"

copy_payload "$INSTALL_ROOT" "$WEBROOT" "$STAGE_DIR"
bundle_wasi_sdk "$STAGE_DIR"
bundle_wasmtime "$STAGE_DIR"

write_packaged_settings "$STAGE_DIR/etc/uce/settings.cfg" "$WEBROOT"
install -m 0644 "$DEB_ASSET_DIR/uce.service" "$STAGE_DIR/lib/systemd/system/uce.service"
install -m 0644 "$DEB_ASSET_DIR/uce.socket" "$STAGE_DIR/lib/systemd/system/uce.socket"
install -m 0644 "$DEB_ASSET_DIR/conffiles" "$DEBIAN_DIR/conffiles"
install -m 0755 "$DEB_ASSET_DIR/postinst" "$DEBIAN_DIR/postinst"
install -m 0755 "$DEB_ASSET_DIR/prerm" "$DEBIAN_DIR/prerm"
install -m 0755 "$DEB_ASSET_DIR/postrm" "$DEBIAN_DIR/postrm"

INSTALLED_SIZE="$(du -sk "$STAGE_DIR" | awk '{print $1}')"
write_control_file "$DEBIAN_DIR/control" "$PACKAGE_VERSION" "$ARCH" "$INSTALLED_SIZE"
write_md5sums "$STAGE_DIR"

dpkg-deb --root-owner-group --build "$STAGE_DIR" "$OUTPUT_DEB"

dpkg-deb -I "$OUTPUT_DEB" | sed -n '1,80p'
echo
dpkg-deb -c "$OUTPUT_DEB" | sed -n '1,60p'
