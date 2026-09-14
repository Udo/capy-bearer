#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PACKAGE_NAME="bearer"
RELEASE="${BEARER_RPM_RELEASE:-1}"
usage() {
	cat <<'EOF'
Usage:
  scripts/make_rpm.sh [VERSION]

When VERSION is omitted, scripts/make_rpm.sh reads VERSION, MAJOR, and RELEASE from version.txt.

Environment:
  BEARER_RPM_RELEASE             Override RPM release suffix (default: RELEASE from version.txt)
  BEARER_RPM_ARCH                Override RPM architecture
  BEARER_RPM_BUNDLE_WASMTIME     Bundle /opt/wasmtime into the package (default: 1)
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
	if [[ -n "${BEARER_RPM_ARCH:-}" ]]; then
		printf '%s\n' "$BEARER_RPM_ARCH"
		return
	fi
	uname -m
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

validate_package_payload() {
	local package="$1" listing expected
	listing=$(rpm -qlp "$package")
	for expected in \
		/usr/lib/bearer/bin/bearer_fastcgi.linux.bin \
		/usr/lib/bearer/bin/capyc \
		/usr/lib/bearer/bin/wasm/core.wasm \
		/usr/lib/bearer/scripts/bearer-cli \
		/usr/lib/bearer/scripts/systemd/wait-ready.sh \
		/etc/bearer/settings.cfg \
		/usr/lib/systemd/system/bearer.service
	do
		grep -Fxq "$expected" <<<"$listing" || { echo "The RPM package is missing $expected." >&2; exit 1; }
	done
	if grep -Eq '^/(var/www|srv/www)(/|$)' <<<"$listing"; then
		echo "The RPM runtime package contains an application web root." >&2
		exit 1
	fi
	if grep -Eq '/usr/lib/bearer/site(/|$)' <<<"$listing"; then
		echo "The RPM runtime package contains the bundled site." >&2
		exit 1
	fi
	if grep -Eq '/usr/lib/bearer/(etc|src|docs|codesearch|scripts/(test_|install_wasi_sdk\.sh|build_core_wasm\.sh)|opt/wasi-sdk|bin/([^/]*\.o|\.build|capyc-request-dval|tmp/))|/opt/wasmtime[^/]*/(include/|lib/.*\.a$)' <<<"$listing"; then
		echo "The RPM package contains a development-only file." >&2
		exit 1
	fi
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

bundle_wasmtime() {
	local stage_dir="$1"
	local wasmtime_root="${WASMTIME_HOME:-/opt/wasmtime}"
	if [[ "${BEARER_RPM_BUNDLE_WASMTIME:-1}" != "1" ]]; then
		return
	fi
	if [[ ! -f "$wasmtime_root/include/wasmtime.hh" || ! -f "$wasmtime_root/lib/libwasmtime.so" ]]; then
		echo "BEARER_RPM_BUNDLE_WASMTIME=1 but WASMTIME_HOME does not point at a complete C API tree: $wasmtime_root" >&2
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
RELEASE="${BEARER_RPM_RELEASE:-$RPM_RELEASE_PART}"
validate_version "$VERSION"

require_command bash
require_command rpmbuild
require_command rpm
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
INSTALL_ROOT="$STAGE_DIR/usr/lib/bearer"
SPEC_FILE="$RPMBUILD_DIR/SPECS/$PACKAGE_NAME.spec"
DIST_DIR="$REPO_ROOT/dist"

bash "$REPO_ROOT/scripts/build_linux.sh" release

rm -rf -- "$BUILD_ROOT"
mkdir -p "$INSTALL_ROOT" "$STAGE_DIR/etc/bearer" "$STAGE_DIR/usr/lib/systemd/system" "$RPMBUILD_DIR"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} "$DIST_DIR"

copy_payload "$INSTALL_ROOT"
bundle_wasmtime "$STAGE_DIR"
write_packaged_settings "$STAGE_DIR/etc/bearer/settings.cfg"
install -m 0644 "$REPO_ROOT/scripts/rpm/bearer.service" "$STAGE_DIR/usr/lib/systemd/system/bearer.service"
install -m 0644 "$REPO_ROOT/scripts/rpm/bearer.socket" "$STAGE_DIR/usr/lib/systemd/system/bearer.socket"

OPT_FILES=""
[[ ! -d "$STAGE_DIR/opt" ]] || OPT_FILES="/opt/*"

(
	cd "$STAGE_DIR"
	tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -czf "$RPMBUILD_DIR/SOURCES/$PACKAGE_NAME-$VERSION.tar.gz" .
)

cat > "$SPEC_FILE" <<EOF
%global __os_install_post %{nil}

Name: $PACKAGE_NAME
Version: $VERSION
Release: $RELEASE%{?dist}
Summary: Bearer FastCGI runtime for Capy web units
License: GPL-3.0-or-later
URL: https://github.com/Udo/capy-bearer
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
Requires(pre): shadow-utils

%description
Bearer handles FastCGI requests and compiles Capy units to WebAssembly on demand.
Applications and public web roots are packaged separately.

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

%pre
getent group bearer >/dev/null 2>&1 || groupadd --system bearer
getent passwd bearer >/dev/null 2>&1 || useradd --system --gid bearer --home-dir /var/lib/bearer --shell /sbin/nologin --comment "Bearer FastCGI runtime" bearer

%post
settings=/etc/bearer/settings.cfg
if [ -f "\$settings" ] && grep -qxF 'page_runtime_error=site/errors/runtime-error.capy' "\$settings"; then
    sed -i 's|^page_runtime_error=site/errors/runtime-error\\.capy\$|page_runtime_error=|' "\$settings"
fi
owner_marker=/var/lib/bearer/.bearer-owner
if [ ! -e "\$owner_marker" ]; then
    mkdir -p /var/cache/bearer /var/lib/bearer
    chown -R bearer:bearer /var/cache/bearer /var/lib/bearer
    touch "\$owner_marker"
    chown bearer:bearer "\$owner_marker"
fi
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable bearer.socket bearer.service >/dev/null 2>&1 || true
    systemctl start bearer.socket >/dev/null 2>&1 || true
    systemctl restart bearer.service >/dev/null 2>&1 || true
fi

%preun
if [ "\$1" = "0" ] && command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now bearer.service bearer.socket >/dev/null 2>&1 || true
fi

%postun
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

%files
/usr/lib/bearer
%exclude /usr/lib/bearer/LICENSE
%exclude /usr/lib/bearer/README.md
%license /usr/lib/bearer/LICENSE
%doc /usr/lib/bearer/README.md
%config(noreplace) /etc/bearer/settings.cfg
/usr/lib/systemd/system/bearer.service
/usr/lib/systemd/system/bearer.socket
$OPT_FILES

%changelog
* Mon Jun 15 2026 BEARER Packager <root@localhost> - $PACKAGE_VERSION
- Package BEARER runtime with pinned deployment toolchains.
EOF

rpmbuild --define "_topdir $RPMBUILD_DIR" -bb "$SPEC_FILE"
while IFS= read -r package; do
	validate_package_payload "$package"
	cp -a "$package" "$DIST_DIR/"
done < <(find "$RPMBUILD_DIR/RPMS" -type f -name '*.rpm' -print)
find "$DIST_DIR" -maxdepth 1 -type f -name "${PACKAGE_NAME}-${VERSION}-${RELEASE}*.rpm" -print
