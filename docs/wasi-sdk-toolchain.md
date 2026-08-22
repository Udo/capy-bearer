# WASI SDK for the Bearer Core

Bearer uses the WASI SDK only to build `bin/wasm/core.wasm`. Capy units compile in process through `capyc`. A deployed Bearer service does not need the WASI SDK.

## Build requirement

Set `WASI_SDK` to the pinned SDK directory before you run `scripts/build_core_wasm.sh` or `scripts/build_linux.sh`. The SDK must provide `clang++` and `wasm-ld`.

Use `scripts/install_wasi_sdk.sh --check-only` to check the configured SDK. Do not bundle the SDK in a Bearer package.
