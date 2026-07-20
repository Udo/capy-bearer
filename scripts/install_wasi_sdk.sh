#!/usr/bin/env bash
set -euo pipefail

# Install the pinned WASI SDK used by BEARER's request-time wasm compiler.
# This is a runtime dependency: BEARER compiles .uce units on demand and during
# proactive startup scans, so every deployment host needs the same toolchain.

WASI_SDK_VERSION="${WASI_SDK_VERSION:-33.0}"
WASI_SDK_RELEASE_TAG="${WASI_SDK_RELEASE_TAG:-wasi-sdk-33}"
WASI_SDK_ARCHIVE="${WASI_SDK_ARCHIVE:-wasi-sdk-33.0-x86_64-linux.tar.gz}"
WASI_SDK_SHA256="${WASI_SDK_SHA256:-0ba8b5bfaeb2adf3f29bab5841d76cf5318ab8e1642ea195f88baba1abd47bce}"
WASI_SDK_URL="${WASI_SDK_URL:-https://github.com/WebAssembly/wasi-sdk/releases/download/${WASI_SDK_RELEASE_TAG}/${WASI_SDK_ARCHIVE}}"
INSTALL_BASE="${INSTALL_BASE:-/opt}"
INSTALL_DIR="${WASI_SDK_INSTALL_DIR:-${INSTALL_BASE}/wasi-sdk-${WASI_SDK_VERSION}-x86_64-linux}"
SYMLINK_PATH="${WASI_SDK_SYMLINK:-${INSTALL_BASE}/wasi-sdk}"
CACHE_DIR="${WASI_SDK_CACHE_DIR:-/tmp/bearer-deps}"

usage() {
	cat <<EOF
Usage: scripts/install_wasi_sdk.sh [--check-only]

Installs pinned WASI SDK ${WASI_SDK_VERSION} for BEARER request-time unit compilation.

Environment overrides:
  WASI_SDK_VERSION       ${WASI_SDK_VERSION}
  WASI_SDK_RELEASE_TAG   ${WASI_SDK_RELEASE_TAG}
  WASI_SDK_ARCHIVE       ${WASI_SDK_ARCHIVE}
  WASI_SDK_SHA256        ${WASI_SDK_SHA256}
  WASI_SDK_URL           ${WASI_SDK_URL}
  WASI_SDK_INSTALL_DIR   ${INSTALL_DIR}
  WASI_SDK_SYMLINK       ${SYMLINK_PATH}
  WASI_SDK_CACHE_DIR     ${CACHE_DIR}
EOF
}

check_only=0
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
	usage
	exit 0
elif [[ "${1:-}" == "--check-only" ]]; then
	check_only=1
elif [[ $# -gt 0 ]]; then
	usage >&2
	exit 2
fi

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Required command not found: $1" >&2
		exit 1
	fi
}

verify_tree() {
	local root="$1"
	for tool in clang++ wasm-ld llvm-objcopy llvm-nm llvm-dwarfdump; do
		if [[ ! -x "$root/bin/$tool" ]]; then
			echo "Missing WASI SDK tool: $root/bin/$tool" >&2
			return 1
		fi
	done
	if ! command -v curl >/dev/null 2>&1; then
		echo "Missing runtime dependency: curl (required by BEARER http_request/http_request_async)" >&2
		return 1
	fi
	"$root/bin/clang++" --version | head -n 1
	curl --version | head -n 1
}

if [[ $check_only -eq 1 ]]; then
	verify_tree "${WASI_SDK:-$SYMLINK_PATH}"
	exit 0
fi

require_command curl
require_command sha256sum
require_command tar
require_command mkdir
require_command ln

mkdir -p "$CACHE_DIR" "$INSTALL_BASE"
archive_path="$CACHE_DIR/$WASI_SDK_ARCHIVE"

if [[ ! -f "$archive_path" ]]; then
	echo "Downloading $WASI_SDK_URL"
	curl -fL --proto '=https' --tlsv1.2 -o "$archive_path.tmp" "$WASI_SDK_URL"
	mv "$archive_path.tmp" "$archive_path"
fi

printf '%s  %s\n' "$WASI_SDK_SHA256" "$archive_path" | sha256sum -c -

rm -rf -- "$INSTALL_DIR.tmp"
mkdir -p "$INSTALL_DIR.tmp"
tar -xf "$archive_path" -C "$INSTALL_DIR.tmp" --strip-components=1
verify_tree "$INSTALL_DIR.tmp"

rm -rf -- "$INSTALL_DIR"
mv "$INSTALL_DIR.tmp" "$INSTALL_DIR"
ln -sfn "$INSTALL_DIR" "$SYMLINK_PATH"

echo "Installed WASI SDK at $INSTALL_DIR"
echo "Updated symlink $SYMLINK_PATH -> $INSTALL_DIR"
verify_tree "$SYMLINK_PATH"
