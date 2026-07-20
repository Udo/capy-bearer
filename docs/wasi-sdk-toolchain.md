# WASI SDK Toolchain Pin

BEARER treats WASI SDK as a deployment/runtime dependency, not just a developer build tool.

The runtime compiles `.uce` units to wasm on demand during requests and during the proactive compiler scan. That means every deployment host must have the same compiler/linker toolchain available, and the generated `.wasm`/`.cwasm` artifacts are tied to that toolchain version and BEARER unit ABI version.

## Current pin

- Upstream: <https://github.com/WebAssembly/wasi-sdk>
- Release tag: `wasi-sdk-33`
- Version: `33.0`
- Linux x86_64 archive: `wasi-sdk-33.0-x86_64-linux.tar.gz`
- URL: `https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-x86_64-linux.tar.gz`
- SHA256: `0ba8b5bfaeb2adf3f29bab5841d76cf5318ab8e1642ea195f88baba1abd47bce`
- Expected install symlink: `/opt/wasi-sdk`
- Expected resolved path: `/opt/wasi-sdk-33.0-x86_64-linux`

Install or verify with:

```bash
scripts/install_wasi_sdk.sh
scripts/install_wasi_sdk.sh --check-only
```

## Required tools

BEARER expects these executables on each deployment host:

```text
/opt/wasi-sdk/bin/clang++
/opt/wasi-sdk/bin/wasm-ld
/opt/wasi-sdk/bin/llvm-objcopy
/opt/wasi-sdk/bin/llvm-nm
/opt/wasi-sdk/bin/llvm-dwarfdump
```

`llvm-nm` is used by `scripts/check_unit_wasm.py`, which is called by `scripts/compile_wasm_unit` after linking each unit.
`llvm-dwarfdump` is used at unit compile time to extract the compact, out-of-band
source map before debug sections are stripped from the runtime artifact. The
map is consulted only on a wasm trap.

The native compiler passes the ABI-generation root to
`scripts/compile_wasm_unit`. Unless `BEARER_WASM_PCH_DIR` is explicitly set, the
unit PCH lives beneath that writable generation rather than a
process-user-dependent `/tmp` path. Managed precompile therefore behaves the
same inside and outside the service unit.

## Upgrade policy

Treat WASI SDK upgrades like runtime dependency upgrades:

1. Update `scripts/install_wasi_sdk.sh` version, URL, and SHA256.
2. Record the new release and checksum here.
3. Rebuild `bin/wasm/core.wasm` with `scripts/build_core_wasm.sh`.
4. Rebuild the native runtime with `scripts/build_linux.sh`.
5. Bump the compiler or core ABI constant in `src/wasm/abi.h` when required.
   The managed restart precompiles the resulting isolated unit generation before
   switching workers; do not overwrite or delete the previous generation.
6. Run the full CLI suite including wasm kill tests:

```bash
scripts/run_cli_tests.sh --include-wasm-kill
```

## Known footgun

WASI SDK 33's `llvm-nm` was observed to crash on a degenerate but valid unit module with no exported handlers. `scripts/check_unit_wasm.py` treats that specific validator-tool crash as a skipped allocator-definition scan while still rejecting forbidden allocator exports and other ABI violations. This is one reason the toolchain is pinned instead of relying on whatever `/opt/wasi-sdk` happens to contain.
