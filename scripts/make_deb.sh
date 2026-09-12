#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEB_ASSET_DIR="$SCRIPT_DIR/deb"
PACKAGE_NAME="bearer"
REVISION="${BEARER_DEB_REVISION:-}"

usage() {
	cat <<'EOF'
Usage:
  scripts/make_deb.sh [VERSION]

When VERSION is omitted, scripts/make_deb.sh reads VERSION from version.txt.

Environment:
  BEARER_DEB_REVISION             Optional Debian package revision suffix
  BEARER_DEB_ARCH                 Override package architecture
  BEARER_DEB_BUNDLE_WASMTIME      Bundle /opt/wasmtime into the package (default: 1)
EOF
}

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Required command not found: $1" >&2
		exit 1
	fi
}

resolve_arch() {
	if [[ -n "${BEARER_DEB_ARCH:-}" ]]; then
		printf '%s\n' "$BEARER_DEB_ARCH"
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
	install -m 0644 "$REPO_ROOT/LICENSE" "$REPO_ROOT/README.md" "$destination/"
	mkdir -p "$destination/bin/wasm" "$destination/scripts/systemd"
	install -m 0755 "$REPO_ROOT/bin/bearer_fastcgi.linux.bin" "$destination/bin/"
	install -m 0755 "$REPO_ROOT/bin/capyc" "$destination/bin/"
	install -m 0755 "$REPO_ROOT/bin/wasm/core.wasm" "$destination/bin/wasm/"
	if [[ -d "$REPO_ROOT/bin/assets" ]]; then cp -a "$REPO_ROOT/bin/assets" "$destination/bin/"; fi
	install -m 0755 "$REPO_ROOT/scripts/bearer-cli" "$destination/scripts/"
	install -m 0755 "$REPO_ROOT/scripts/systemd/wait-ready.sh" "$destination/scripts/systemd/"
}

write_packaged_settings() {
	local output_file="$1"
	python3 - "$REPO_ROOT/etc/bearer/settings.cfg" "$output_file" <<'PY'
from pathlib import Path
import sys
src, dst = sys.argv[1:3]
s = Path(src).read_text()
replacements = {
    "SITE_DIRECTORY=site": "SITE_DIRECTORY=/var/www/html",
    "WASM_CORE_PATH=/Code/bearer.openfu.com/bearer/bin/wasm/core.wasm": "WASM_CORE_PATH=/usr/lib/bearer/bin/wasm/core.wasm",
    "HTTP_DOCUMENT_ROOT=": "HTTP_DOCUMENT_ROOT=/var/www/html",
    "page_runtime_error=site/errors/runtime-error.capy": "page_runtime_error=",
}
for old, new in replacements.items():
    if old in s:
        s = s.replace(old, new)
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

bundle_wasmtime() {
	local stage_dir="$1"
	local wasmtime_root="${WASMTIME_HOME:-/opt/wasmtime}"
	if [[ "${BEARER_DEB_BUNDLE_WASMTIME:-1}" != "1" ]]; then
		return
	fi
	if [[ ! -f "$wasmtime_root/include/wasmtime.hh" || ! -f "$wasmtime_root/lib/libwasmtime.so" ]]; then
		echo "BEARER_DEB_BUNDLE_WASMTIME=1 but WASMTIME_HOME does not point at a complete C API tree: $wasmtime_root" >&2
		exit 1
	fi
	local resolved base destination
	resolved="$(readlink -f "$wasmtime_root")"
	base="$(basename "$resolved")"
	destination="$stage_dir/opt/$base"
	mkdir -p "$destination/lib"
	cp -a "$resolved/lib"/libwasmtime.so* "$destination/lib/"
	for file in LICENSE README.md; do
		[[ ! -f "$resolved/$file" ]] || install -m 0644 "$resolved/$file" "$destination/$file"
	done
	ln -sfn "$base" "$stage_dir/opt/wasmtime"
}

write_md5sums() {
	local stage_dir="$1"
	(
		cd "$stage_dir"
		roots=(usr etc lib)
		[[ ! -d opt ]] || roots+=(opt)
		find "${roots[@]}" -type f -print0 | sort -z | xargs -0 --no-run-if-empty md5sum > DEBIAN/md5sums
	)
}

validate_package_payload() {
	local package="$1" listing expected
	listing=$(dpkg-deb --contents "$package")
	for expected in \
		/usr/lib/bearer/bin/bearer_fastcgi.linux.bin \
		/usr/lib/bearer/bin/capyc \
		/usr/lib/bearer/bin/wasm/core.wasm \
		/usr/lib/bearer/scripts/bearer-cli \
		/usr/lib/bearer/scripts/systemd/wait-ready.sh \
		/etc/bearer/settings.cfg \
		/lib/systemd/system/bearer.service
	do
		grep -Fq " .$expected" <<<"$listing" || { echo "The Debian package is missing $expected." >&2; exit 1; }
	done
	if grep -Eq ' \./?(var/www|srv/www)/' <<<"$listing"; then
		echo "The Debian runtime package contains an application web root." >&2
		exit 1
	fi
	if grep -Eq '/usr/lib/bearer/site(/|$)' <<<"$listing"; then
		echo "The Debian runtime package contains the bundled site." >&2
		exit 1
	fi
	if grep -Eq '/usr/lib/bearer/(etc|src|docs|codesearch|scripts/(test_|install_wasi_sdk\.sh|build_core_wasm\.sh)|opt/wasi-sdk|bin/([^/]*\.o|\.build|capyc-request-dval|tmp/))|/opt/wasmtime[^/]*/(include/|lib/[^ ]*\.a)' <<<"$listing"; then
		echo "The Debian package contains a development-only file." >&2
		exit 1
	fi
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
INSTALL_ROOT="$STAGE_DIR/usr/lib/bearer"
DIST_DIR="$REPO_ROOT/dist"
OUTPUT_DEB="$DIST_DIR/$PACKAGE_BASENAME.deb"

echo "Making package $PACKAGE_BASENAME"
echo "==================================="

bash "$REPO_ROOT/scripts/build_linux.sh" release

rm -rf -- "$STAGE_DIR"
mkdir -p "$DEBIAN_DIR" "$INSTALL_ROOT" "$STAGE_DIR/etc/bearer" "$STAGE_DIR/lib/systemd/system" "$DIST_DIR"

copy_payload "$INSTALL_ROOT"
bundle_wasmtime "$STAGE_DIR"

write_packaged_settings "$STAGE_DIR/etc/bearer/settings.cfg"
install -m 0644 "$DEB_ASSET_DIR/bearer.service" "$STAGE_DIR/lib/systemd/system/bearer.service"
install -m 0644 "$DEB_ASSET_DIR/bearer.socket" "$STAGE_DIR/lib/systemd/system/bearer.socket"
install -m 0644 "$DEB_ASSET_DIR/conffiles" "$DEBIAN_DIR/conffiles"
install -m 0755 "$DEB_ASSET_DIR/postinst" "$DEBIAN_DIR/postinst"
install -m 0755 "$DEB_ASSET_DIR/prerm" "$DEBIAN_DIR/prerm"
install -m 0755 "$DEB_ASSET_DIR/postrm" "$DEBIAN_DIR/postrm"

INSTALLED_SIZE="$(du -sk --apparent-size "$STAGE_DIR" | awk '{print $1}')"
write_control_file "$DEBIAN_DIR/control" "$PACKAGE_VERSION" "$ARCH" "$INSTALLED_SIZE"
write_md5sums "$STAGE_DIR"

dpkg-deb --root-owner-group --build "$STAGE_DIR" "$OUTPUT_DEB"
validate_package_payload "$OUTPUT_DEB"

dpkg-deb -I "$OUTPUT_DEB" | sed -n '1,80p'
echo
dpkg-deb -c "$OUTPUT_DEB" | sed -n '1,60p'
