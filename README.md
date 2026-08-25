# Capy on Bearer

Capy is the language for Bearer application units. A Capy unit uses the `.capy` suffix. Bearer compiles each unit to WebAssembly and runs it for a request.

Bearer uses native C++ for its build and runtime implementation. C++ is not a supported application-unit language.

## Start a page

Create `site/hello.capy`:

```capy
function RENDER(request : dval) {
    var name := string(request.query.name, "guest")
    request.out.headers["Content-Type"] = "text/plain; charset=utf-8"
    print("Hello, ", name, "!\n")
}
```

`RENDER` handles an HTTP request. It receives one copied `dval` request snapshot. Read request data from fields such as `request.query`, `request.form`, and `request.body`.

Use these handler forms for public entry points:

```capy
function RENDER(request : dval) {
    print("HTTP response\n")
}

function CLI(request : dval) {
    print("local command\n")
}

function WS(request : dval) {
    ws_send(string(request.body, ""), false)
}
```

`CLI` runs only through the local CLI socket. Do not expose that socket through a web server. `WS` receives WebSocket data in `request.body`. Read message metadata from `request.websocket`.

Every Bearer handler takes exactly one `request : dval` parameter and returns no application value. See [the Capy language specification](docs/capy-language.md) for all handler forms and language rules.

## Build

Build the WebAssembly core, then the native Bearer runtime:

```bash
bash scripts/build_core_wasm.sh
bash scripts/build_linux.sh
```

The build requires:

- `clang++`, build tools, and Linux development headers for `dl`, threads, sockets, and backtraces
- PCRE2 development headers and library
- OpenSSL development headers and library
- MySQL development tools, including `mysql_config`
- Wasmtime C API and C++ headers, plus `libwasmtime.so`, at `/opt/wasmtime` or `WASMTIME_HOME`
- WASI SDK tools at `/opt/wasi-sdk` or `WASI_SDK`, including `clang++` and `wasm-ld`

Bearer vendors SQLite and miniz. You do not need system SQLite or zlib packages for those libraries.

The native runtime binary is `bin/bearer_fastcgi.linux.bin`. The core build writes `bin/wasm/core.wasm`.

## Run and deploy

Bearer runs behind nginx or Apache. The web server serves static files and sends `.capy` requests to Bearer's FastCGI socket. It sends WebSocket upgrades to Bearer's HTTP and WebSocket listener.

Read [docs/setup.md](docs/setup.md) for installation, configuration, service management, nginx, Apache, and verification steps.

## Learn Capy

- [Capy language specification](docs/capy-language.md)
- [Capy guide](site/doc/source/guide/01-install-and-first-program.txt)
- [Generated Capy API signatures](site/doc/lib/capy_signatures.generated.h)
- [Runtime tests](site/tests/)

Run the documentation checks after changing Capy documentation:

```bash
python3 scripts/check_capy_doc_examples.py --self-test
python3 scripts/check_capy_doc_examples.py
scripts/test_capy_guide_examples.sh
```

The guide test needs a running local Bearer service. It compiles guide snippets and checks one rendered example from each guide page.
