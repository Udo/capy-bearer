#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PACKAGE_NAME="uce"
RELEASE="${UCE_RPM_RELEASE:-1}"
WEBROOT="${UCE_RPM_WEBROOT:-/var/www/html}"

usage() {
	cat <<'EOF'
Usage:
  scripts/make_rpm.sh [VERSION]

When VERSION is omitted, scripts/make_rpm.sh reads VERSION, MAJOR, and RELEASE from version.txt.

Environment:
  UCE_RPM_RELEASE             Override RPM release suffix (default: RELEASE from version.txt)
  UCE_RPM_ARCH                Override RPM architecture
  UCE_RPM_WEBROOT             Public web root staged into the package (default: /var/www/html)
  UCE_RPM_BUNDLE_WASI_SDK     Bundle pinned /opt/wasi-sdk into the package (default: 1)
  UCE_RPM_BUNDLE_WASMTIME     Bundle /opt/wasmtime into the package (default: 1)
EOF
}

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Required command not found: $1" >&2
		exit 1
	fi
}

validate_version() {
	local version="$1"
	if [[ ! "$version" =~ ^[0-9][A-Za-z0-9._+~]*$ ]]; then
		echo "Invalid RPM version string: $version" >&2
		exit 1
	fi
}

resolve_arch() {
	if [[ -n "${UCE_RPM_ARCH:-}" ]]; then
		printf '%s\n' "$UCE_RPM_ARCH"
		return
	fi
	uname -m
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

bundle_wasi_sdk() {
	local stage_dir="$1"
	local wasi_root="${WASI_SDK:-/opt/wasi-sdk}"
	if [[ "${UCE_RPM_BUNDLE_WASI_SDK:-1}" != "1" ]]; then
		return
	fi
	if [[ ! -x "$wasi_root/bin/clang++" || ! -x "$wasi_root/bin/wasm-ld" || ! -x "$wasi_root/bin/llvm-nm" ]]; then
		echo "UCE_RPM_BUNDLE_WASI_SDK=1 but WASI_SDK does not point at a complete SDK: $wasi_root" >&2
		exit 1
	fi
	local resolved base
	resolved="$(readlink -f "$wasi_root")"
	base="$(basename "$resolved")"
	mkdir -p "$stage_dir/opt"
	cp -a "$resolved" "$stage_dir/opt/$base"
	ln -sfn "$base" "$stage_dir/opt/wasi-sdk"
}

bundle_wasmtime() {
	local stage_dir="$1"
	local wasmtime_root="${WASMTIME_HOME:-/opt/wasmtime}"
	if [[ "${UCE_RPM_BUNDLE_WASMTIME:-1}" != "1" ]]; then
		return
	fi
	if [[ ! -f "$wasmtime_root/include/wasmtime.hh" || ! -f "$wasmtime_root/lib/libwasmtime.so" ]]; then
		echo "UCE_RPM_BUNDLE_WASMTIME=1 but WASMTIME_HOME does not point at a complete C API tree: $wasmtime_root" >&2
		exit 1
	fi
	local resolved base
	resolved="$(readlink -f "$wasmtime_root")"
	base="$(basename "$resolved")"
	mkdir -p "$stage_dir/opt"
	cp -a "$resolved" "$stage_dir/opt/$base"
	ln -sfn "$base" "$stage_dir/opt/wasmtime"
}

if [[ $# -gt 1 ]]; then
	usage >&2
	exit 1
fi

FULL_VERSION=""
if [[ $# -eq 1 ]]; then
	FULL_VERSION="$1"
	RPM_VERSION="${FULL_VERSION%%-*}"
	RPM_RELEASE_PART="${FULL_VERSION#*-}"
	if [[ "$RPM_RELEASE_PART" == "$FULL_VERSION" ]]; then
		RPM_RELEASE_PART="$RELEASE"
	fi
else
	if [[ ! -r "$REPO_ROOT/version.txt" ]]; then
		echo "Missing version.txt and no VERSION argument supplied" >&2
		exit 1
	fi
	# shellcheck disable=SC1091
	. "$REPO_ROOT/version.txt"
	FULL_VERSION="${VERSION:-}"
	RPM_VERSION="${MAJOR:-${FULL_VERSION%%-*}}"
	RPM_RELEASE_PART="${RELEASE:-${FULL_VERSION#*-}}"
fi

if [[ -z "$FULL_VERSION" || -z "$RPM_VERSION" || -z "$RPM_RELEASE_PART" ]]; then
	echo "Could not derive RPM version from VERSION/MAJOR/RELEASE" >&2
	exit 1
fi
RPM_RELEASE_PART="${RPM_RELEASE_PART//-/_}"
VERSION="$RPM_VERSION"
RELEASE="${UCE_RPM_RELEASE:-$RPM_RELEASE_PART}"
validate_version "$VERSION"

require_command bash
require_command rpmbuild
require_command find
require_command cp
require_command tar
require_command sed
require_command python3
require_command readlink

ARCH="$(resolve_arch)"
PACKAGE_VERSION="$FULL_VERSION"
BUILD_ROOT="$REPO_ROOT/pkg/rpm-build"
STAGE_DIR="$BUILD_ROOT/stage"
RPMBUILD_DIR="$BUILD_ROOT/rpmbuild"
INSTALL_ROOT="$STAGE_DIR/usr/lib/uce"
SPEC_FILE="$RPMBUILD_DIR/SPECS/$PACKAGE_NAME.spec"
DIST_DIR="$REPO_ROOT/dist"

bash "$REPO_ROOT/scripts/build_linux.sh"

rm -rf -- "$BUILD_ROOT"
mkdir -p "$INSTALL_ROOT" "$STAGE_DIR/etc/uce" "$STAGE_DIR/usr/lib/systemd/system" "$STAGE_DIR/var/cache/uce" "$STAGE_DIR/var/lib/uce" "$RPMBUILD_DIR"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} "$DIST_DIR"

copy_payload "$INSTALL_ROOT" "$WEBROOT" "$STAGE_DIR"
bundle_wasi_sdk "$STAGE_DIR"
bundle_wasmtime "$STAGE_DIR"
write_packaged_settings "$STAGE_DIR/etc/uce/settings.cfg" "$WEBROOT"
install -m 0644 "$REPO_ROOT/scripts/deb/uce.service" "$STAGE_DIR/usr/lib/systemd/system/uce.service"
install -m 0644 "$REPO_ROOT/scripts/deb/uce.socket" "$STAGE_DIR/usr/lib/systemd/system/uce.socket"

(
	cd "$STAGE_DIR"
	tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -czf "$RPMBUILD_DIR/SOURCES/$PACKAGE_NAME-$VERSION.tar.gz" .
)

cat > "$SPEC_FILE" <<EOF
%global __os_install_post %{nil}

Name: $PACKAGE_NAME
Version: $VERSION
Release: $RELEASE%{?dist}
Summary: UCE FastCGI runtime and live-compiling C++ web environment
License: GPL-3.0-or-later
URL: https://example.com/uce
BuildArch: $ARCH
Requires: bash
Requires: python3
Requires: curl
Requires: systemd
Requires: pcre2
Requires: zlib
Requires: openssl-libs
Requires: libstdc++
Requires: mariadb-connector-c

%description
UCE is an experimental C/C++ web runtime with FastCGI request handling,
on-demand wasm unit compilation, and a packaged site/doc/test tree.

%prep
rm -rf %{_builddir}/$PACKAGE_NAME-$VERSION
mkdir -p %{_builddir}/$PACKAGE_NAME-$VERSION
cd %{_builddir}/$PACKAGE_NAME-$VERSION
tar -xzf %{_sourcedir}/$PACKAGE_NAME-$VERSION.tar.gz

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a %{_builddir}/$PACKAGE_NAME-$VERSION/. %{buildroot}/

%post
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable uce.socket uce.service >/dev/null 2>&1 || true
    systemctl start uce.socket >/dev/null 2>&1 || true
    systemctl restart uce.service >/dev/null 2>&1 || true
fi

%preun
if [ "\$1" = "0" ] && command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now uce.service uce.socket >/dev/null 2>&1 || true
fi

%postun
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

%files
%license /usr/lib/uce/LICENSE
%doc /usr/lib/uce/README.md
%config(noreplace) /etc/uce/settings.cfg
/usr/lib/uce
/usr/lib/systemd/system/uce.service
/usr/lib/systemd/system/uce.socket
$WEBROOT
%dir /var/cache/uce
%dir /var/lib/uce
/opt/*

%changelog
* Mon Jun 15 2026 UCE Packager <root@localhost> - $PACKAGE_VERSION
- Package UCE runtime with pinned deployment toolchains.
EOF

rpmbuild --define "_topdir $RPMBUILD_DIR" -bb "$SPEC_FILE"
find "$RPMBUILD_DIR/RPMS" -type f -name '*.rpm' -print -exec cp -a {} "$DIST_DIR/" \;
find "$DIST_DIR" -maxdepth 1 -type f -name "${PACKAGE_NAME}-${VERSION}-${RELEASE}*.rpm" -print
