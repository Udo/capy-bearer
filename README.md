# Bearer

## Current State

This is in the early stages of development. Don't use this for anything important!

## Overview

Bearer is a PHP-inspired server-side runtime that lets you build web pages and handlers in C++ using a small `.uce` preprocessor plus a FastCGI application server.

- `.uce` pages compile to WebAssembly side modules on demand
- normal HTTP pages expose `RENDER(Request& context)`
- WebSocket pages can additionally expose `WS(Request& context)`
- local CLI/admin/test entrypoints can expose `CLI(Request& context)` and are invoked through the Unix CLI socket
- sub-rendering and components pass structured data through `context.props`
- nginx can forward normal `.uce` requests to the FastCGI socket, while WebSocket upgrade requests for `.uce` endpoints go to the built-in HTTP/WebSocket listener
- the example application tree lives under `site/`; deployments should publish app files to a normal web root such as `/var/www/html`
- you can include C++ code as much as you want, but only .uce files called via API functions and entry points will be pre-processed
- the preprocessor has two jobs:
       - allow for inline HTML within C++ and the use of templating tags inside of that HTML
       - convenience directive and macro parsing so BEARER files don't need a lot of boiler plate

The abolition of boilerplate was a major design factor, resulting in a page as small as this:

```uce
RENDER(Request& context)
{
       <>hello world</>
}
```

*The runtime is still experimental. This is not production-ready. Use at your own risk!*

Capy is the new direct-to-WebAssembly language under development for Bearer. Its expression-based declaration grammar, compile-time polymorphism, direct side-module ABI, and ARC direction are documented in [`docs/capy-language.md`](docs/capy-language.md). C++ `.uce` units remain supported alongside `.capy` units.

## Build

Build the runtime with:

```bash
bash scripts/build_linux.sh
```

The current build expects:

- `clang++`
- `mysql_config`
- PCRE2 development headers and library (`libpcre2-dev` on Debian / Ubuntu)
- standard Linux development headers for `dl`, `pthread`, sockets, and backtrace support
- Wasmtime C API / C++ headers, defaulting to `/opt/wasmtime` or `WASMTIME_HOME`
- WASI SDK tools, defaulting to `/opt/wasi-sdk` or `WASI_SDK`, for `scripts/build_core_wasm.sh` and unit compilation

SQLite and miniz are vendored under `src/3rdparty/`; no system SQLite or zlib package is required for those helpers.

The binary is written to:

```bash
bin/bearer_fastcgi.linux.bin
```

## Runtime Model

Bearer pages use explicit request handlers instead of implicit globals:

- `RENDER(Request& context)` for normal HTTP rendering
- `WS(Request& context)` for inbound WebSocket messages
- `CLI(Request& context)` for local command-line/admin/test invocations through `CLI_SOCKET_PATH`

Useful related runtime patterns:

- `unit_render(String file_name)` or `unit_render(String file_name, Request& context)` to invoke another page
- `context.cfg` for request-local structured configuration
- `context.props` for invocation-local structured input such as component props
- `context.connection` for broker-owned per-WebSocket-connection state shared across `WS(Request& context)` calls
- `context.params["BEARER_CLI"] == "1"` while handling a local CLI socket request
- `context.in` for the current request body, including the current WebSocket message payload inside `WS(Request& context)`
- `context.params["WS_..."]` for direct WebSocket message metadata on the request parameter map
- `context.params`, `context.get`, `context.post`, `context.cookies`, `context.session`, and `context.header` for request/response state
- `context.set_status(code[, reason])` to set the HTTP response status

Useful helpers for that data model include:

- `DValue::get_by_path("a/b/c")` for path-style config traversal without creating missing keys
- `DValue::has("key")` / `key("key")` for non-mutating child lookup, and `get_or_create("key")` when creation is intended
- `DValue::to_u64()`, `to_s64()`, `to_f64()`, `to_bool()`, and `to_stringmap()` for typed reads from structured values
- `json_encode(String)` for emitting JavaScript-safe string literals directly
- `ascii_safe_name(String)` for conservative ASCII identifier normalization
- `path_join(base, child)` for filesystem-style path assembly
- `sqlite_connect()`, `sqlite_query()`, and related helpers for embedded SQLite storage with named prepared parameters
- `zip_create()`, `zip_list()`, `zip_read()`, and `zip_extract()` for minimal ZIP archive workflows
- `gz_compress()` and `gz_uncompress()` for gzip-format byte strings
- `server_start_http()` / `server_stop()` for runtime-managed custom HTTP listeners backed by `SERVE_HTTP` handlers
- `map()`, `filter()`, `dv_filter()`, `dv_map()`, `dv_pick()`, and related helpers for route/menu/card data shaping near render code

Named component handlers are also supported:

```cpp
COMPONENT:BODY(Request& context)
{
	<>
		<p><?= context.props["body"].to_string() ?></p>
	</>
}
```

Those are intended for sub-rendering through helpers such as `component("components/card:BODY", props, context)` rather than direct page entry.

Additional lifecycle hooks are also available on ordinary `.uce` units:

- `INIT(Request& context)` runs once when a worker instantiates that unit's wasm module
- `ONCE(Request& context)` runs once per request before the first `RENDER()`, `CLI()`, or `COMPONENT...` entrypoint from that file

CLI units can be invoked locally with the convenience wrapper or directly over HTTP-over-Unix:

```bash
scripts/bearer-cli /tests/cli.uce action=echo message=hello
scripts/bearer-cli --json '{"action":"echo","message":"hello"}' /tests/cli.uce
curl --unix-socket /run/bearer/cli.sock http://localhost/tests/cli.uce
```

For structured CLI commands, prefer JSON POST bodies and read them with `cli_input(context)`.

## Template Output

BEARER treats template parsing as one shared code-vs-literal state machine.

- `<>` and `?>` both enter literal output mode
- `</>` and `<?` both return to code mode
- the delimiter pairs are interchangeable, so either style can be used consistently or mixed locally

Inside literal output, BEARER supports three inline forms:

- `<? ... ?>` to emit raw C++ statements
- `<?= expression ?>` to print HTML-escaped output
- `<?: expression ?>` to print unescaped output

Use `<?= ... ?>` by default for user-visible text. Use `<?: ... ?>` only for trusted markup or content that has already been escaped.

The parser treats C++ `//` and `/* ... */` comments as comments in both normal code and `<? ... ?>` islands, so quotes or delimiter markers inside comments do not confuse template parsing.

The preprocessing implementation is split between `src/lib/compiler.cpp` and `src/lib/compiler-parser.cpp`. `compiler.cpp` owns unit compilation and cache orchestration, while `compiler-parser.cpp` owns source rewriting and template parsing.

## Components

BEARER includes a native component layer built on top of ordinary `.uce` files:

- `component(name[, props[, context]])`
- `component_render(name[, props[, context]])`
- `component_exists(name)`
- `component_resolve(name)`

Component props are passed through `context.props`.

Component names resolve:

1. as the exact file name you supplied
2. as that same name plus `.uce`
3. as those same two forms under `components/`

When you want returned component markup inside a literal block, prefer:

```cpp
<>
	<div class="panel"><?: component("components/card", props, context) ?></div>
</>
```

because `<?= ... ?>` HTML-escapes the returned markup. For direct output from C++ code, use `component_render(...)`.

Components expose `COMPONENT(Request& context)` as their default entrypoint and may expose additional named handlers with `COMPONENT:NAME(Request& context)`.

The component helpers call only `COMPONENT...` handlers. A file meant purely for component use can define `COMPONENT()` without defining `RENDER()`, which keeps direct page entry and component entry cleanly separated. Inside a component file, `component(":NAME", props, context)` and `component_render(":NAME", props, context)` target another named component handler in that same file.

If the component file also defines `ONCE(Request& context)`, that hook runs once per request before the file's first component/render entrypoint. If it defines `INIT(Request& context)`, that hook runs once when the worker loads the unit.

## WebSockets

The runtime keeps the socket lifecycle in-process and exposes a low-boilerplate API to page code:

- `ws_message()`
- `ws_connection_id()`
- `ws_scope()`
- `ws_opcode()`
- `ws_is_binary()`
- `ws_connections([scope])`
- `ws_connection_count([scope])`
- `ws_send(message[, binary[, scope]])`
- `ws_send_to(connection_id, message[, binary])`
- `ws_close([connection_id])`

By default, the WebSocket scope is the current page file, so `ws_send()` queues a message for clients connected to that same `.uce` endpoint.

Each live WebSocket connection owns a broker-side `DValue` exposed to page code as `context.connection`. Mutations to that tree persist for the life of the socket and are visible on later `WS(Request& context)` calls for the same client.

The current inbound payload is available directly as `context.in`, and the runtime mirrors message metadata into `context.params` using keys such as `WS_CONNECTION_ID`, `WS_SCOPE`, `WS_CONNECTION_COUNT`, `WS_OPCODE`, `WS_MESSAGE_TYPE`, and `WS_DOCUMENT_URI`.

`ws_message()` may still be used when you want the payload through a helper API. Use `ws_opcode()` / `ws_is_binary()` to inspect the current inbound message type.

Set `binary = true` on `ws_send()` or `ws_send_to()` to queue a binary frame instead of a text frame.

The runtime accepts fragmented messages, validates reserved bits and UTF-8 for text payloads, and delivers both text and binary message frames into `WS(Request& context)`.

## Error Reporting

Unhandled exceptions and recovered fatal request signals return a `500 Internal Server Error` response with a plain-text trace instead of simply dropping the upstream connection and leaving nginx to show a generic `502`.

The demo page `site/test/error-reporting.uce` can be used to exercise:

- uncaught exception handling
- recovered `SIGABRT`
- recovered `SIGSEGV`

The current error page includes:

- request URI
- resolved script path
- generated C++ path when available
- high-level error summary
- source/generated excerpts and raw compiler output paths for template/component/unit failure modes
- signal number and name when applicable
- a native backtrace

Compile failures are also formatted with the source path, generated C++ path, compile-output artifact path, a nearby source/generated excerpt when a line can be identified, and the raw compiler output.

This recovery path currently covers normal request handling. It is not yet the universal recovery path for every runtime subsystem.

## Docs And Tests

The most current user-facing reference lives under `site/doc/`, and the demo pages live under `site/test/`. Developers coming from React, Next, or Remix should start with `site/doc/pages/coming_from_react.txt` / `/doc/index.uce?p=coming_from_react` for the concept map and starter-router notes.

Useful entry points:

- repo files:
  - `site/doc/index.uce`
  - `site/doc/singlepage.uce`
  - `site/test/index.uce`
- published URLs:
  - `/doc/index.uce`
  - `/doc/singlepage.uce`
  - `/test/index.uce`

Representative test pages:

- `site/test/components.uce`
- `site/test/websockets.ws.uce`
- `site/test/error-reporting.uce`
- `site/test/post-multipart.uce`
- `site/test/session.uce`

## Deploy Behind Nginx

The intended production shape is:

- nginx serves static files directly
- nginx forwards ordinary `.uce` page loads to the BEARER FastCGI Unix socket
- nginx proxies WebSocket upgrade requests for `.uce` endpoints to the runtime's built-in HTTP/WebSocket listener
- systemd keeps the runtime built, started, and restarted on failure

The repository ships the pieces used for this:

- `scripts/systemd/bearer.service`
- `scripts/systemd/manage-bearer-service.sh`
- `etc/bearer/settings.cfg`

### 1. Install build and runtime dependencies

On a Debian or Ubuntu host, start with the packages needed to build and run BEARER behind nginx:

```bash
apt update
apt install -y nginx clang mariadb-client libmariadb-dev libpcre2-dev build-essential curl rsync ca-certificates
```

The exact package names may vary by distro. The important requirements are:

- `nginx`
- `clang++`
- `mysql_config`
- PCRE2 development headers and library (`libpcre2-dev` on Debian / Ubuntu)
- normal Linux development headers for threads, sockets, `dl`, and backtrace support
- Wasmtime C API / C++ headers installed at `/opt/wasmtime` or configured with `WASMTIME_HOME`
- WASI SDK installed at `/opt/wasi-sdk` or configured with `WASI_SDK`

### 2. Put the repo on the server

This README assumes the repository lives at:

```bash
/opt/bearer
```

The examples below use that path for the runtime. Publish public application files under the normal web root, for example:

```bash
cd /opt/bearer
mkdir -p /var/www/html
rsync -a site/ /var/www/html/
```

If you deploy somewhere else, update the systemd unit's `WorkingDirectory`, build path, and `ExecStart` path before enabling the service.

### 3. Configure `/etc/bearer/settings.cfg`

The runtime reads its server settings from:

```bash
/etc/bearer/settings.cfg
```

The example contains the filesystem and FastCGI settings:

```ini
BIN_DIRECTORY=/var/cache/bearer/work
TMP_UPLOAD_PATH=/var/lib/bearer/uploads
SESSION_PATH=/var/lib/bearer/sessions
SESSION_COOKIE_SECURE=1

FCGI_SOCKET_PATH=/run/bearer/fastcgi.sock
FCGI_SOCKET_MODE=0666
FCGI_PORT=9993
CLI_SOCKET_PATH=/run/bearer/cli.sock
CLI_SOCKET_MODE=0600

PRECOMPILE_FILES_IN=
SITE_DIRECTORY=/var/www/html
PROACTIVE_COMPILE_CHECK_INTERVAL=60

WORKER_COUNT=4
MAX_MEMORY=16777216
SESSION_TIME=2592000
```

For nginx deployments, the most important setting is:

- `FCGI_SOCKET_PATH=/run/bearer/fastcgi.sock`

That is the Unix socket nginx should use for normal `.uce` requests. `CLI_SOCKET_PATH` is for local admin/test execution through `scripts/bearer-cli`; keep `CLI_SOCKET_MODE=0600` unless a trusted Unix group explicitly needs access.

`FCGI_PORT` is optional if nginx is talking to the Unix socket. Leave it set if you also want a TCP FastCGI listener, or remove it if you want the socket to be the only FastCGI entry point.

If you want WebSocket support through nginx, also make sure the built-in HTTP listener is available. The runtime currently defaults `HTTP_PORT` to `8080` even if it is not present in the config file, but it is clearer to set it explicitly:

```ini
HTTP_PORT=8080
```

Proactive compilation settings:

- `SITE_DIRECTORY=/var/www/html` tells the runtime which public web tree to scan on startup for `.uce` files when `PRECOMPILE_FILES_IN` is left empty.
- `PRECOMPILE_FILES_IN=` can override that startup scan root with a different absolute or runtime-relative directory.
- `PROACTIVE_COMPILE_CHECK_INTERVAL=60` controls how often the low-priority background compiler rechecks known `.uce` files for stale or missing wasm modules.

The runtime keeps a shared known-file registry under `BIN_DIRECTORY` and updates it as request handling discovers new `.uce` files, so proactive recompiles are not limited to the initial startup scan.

Recommended deployment notes:

- keep `HTTP_PORT` bound to localhost only at the firewall or by network policy; nginx should be the public entry point
- keep `BIN_DIRECTORY`, `TMP_UPLOAD_PATH`, and `SESSION_PATH` on writable local storage
- use `SESSION_COOKIE_SECURE=1` for HTTPS-only deployments; leave it `0` only for local/plain-HTTP development
- after editing `/etc/bearer/settings.cfg`, restart `bearer.service`

### 4. Install and enable the systemd service

As root, from the repository root:

```bash
scripts/systemd/manage-bearer-service.sh setup
```

That script:

- installs `scripts/systemd/bearer.service` as `/etc/systemd/system/bearer.service`, rewriting the repository-root path in the unit to the checkout you ran it from
- installs `etc/bearer/settings.cfg` to `/etc/bearer/settings.cfg` if it does not already exist, likewise rewriting checkout-root paths
- reloads systemd
- enables the service at boot
- starts the runtime immediately

Useful follow-up commands:

```bash
scripts/systemd/manage-bearer-service.sh status
scripts/systemd/manage-bearer-service.sh restart
scripts/systemd/manage-bearer-service.sh logs 200
```

The unit currently:

- uses systemd-managed runtime/state/cache roots under:
  - `/run/bearer`
  - `/var/lib/bearer`
  - `/var/cache/bearer`
- prepares:
  - `/var/cache/bearer/work`
  - `/var/lib/bearer/uploads`
  - `/var/lib/bearer/sessions`
- removes any stale `/run/bearer/fastcgi.sock`
- rebuilds the runtime before start
- runs the binary from the repo root so `COMPILER_SYS_PATH` resolves correctly

### Debian package build

To build a Debian package from the repository root:

```bash
bash scripts/make_deb.sh 0.1.2
```

That script:

- rebuilds the runtime first
- stages the current runtime tree under `/usr/lib/bearer`
- installs `/etc/bearer/settings.cfg` as a package conffile
- installs a packaged `bearer.service` under `/lib/systemd/system/`
- writes Debian maintainer scripts for systemd reload/enable handling
- follows a more PHP-like/FHS deployment shape with immutable runtime files under `/usr/lib`, config under `/etc`, cache/state under `/var`, and the FastCGI socket under `/run/bearer/`

### 5. Configure nginx for `.uce` and WebSocket upgrades

Any `.uce` unit can expose `WS(Request& context)`. WebSocket upgrade requests for `.uce` paths should be routed to the runtime's HTTP/WebSocket listener.

You need two transport paths for `.uce` endpoints:

- FastCGI for ordinary `.uce` page renders
- HTTP proxying only for WebSocket upgrade traffic on `.uce` endpoints

If you use WebSockets, add this `map` in the nginx `http` block:

```nginx
map $http_upgrade $connection_upgrade {
	default upgrade;
	''      close;
}
```

Then use a server block along these lines:

```nginx
server {
	listen 80;
	server_name example.com;
	root /var/www/html;

	index index.uce index.html;

	location / {
		try_files $uri $uri/ =404;
	}

	location ~ \.(?:uce|capy)$ {
		error_page 418 = @bearer_websocket;
		if ($http_upgrade = "websocket") {
			return 418;
		}

		include fastcgi_params;
		fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
		fastcgi_param DOCUMENT_ROOT $document_root;
		fastcgi_param SCRIPT_NAME $fastcgi_script_name;
		fastcgi_param DOCUMENT_URI $uri;
		fastcgi_param REQUEST_URI $request_uri;
		fastcgi_pass unix:/run/bearer/fastcgi.sock;
	}

	location @bearer_websocket {
		proxy_http_version 1.1;
		proxy_set_header Host $host;
		proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
		proxy_set_header X-Forwarded-Proto $scheme;
		proxy_set_header Upgrade $http_upgrade;
		proxy_set_header Connection $connection_upgrade;
		proxy_pass http://127.0.0.1:8080;
	}
}
```

Important details:

- `fastcgi_pass` should point at the same socket path as `FCGI_SOCKET_PATH`
- `proxy_pass` should point at the runtime's `HTTP_PORT`
- ordinary `GET /page.uce` page renders should stay on FastCGI
- only upgrade requests for `/page.uce` should go through the HTTP/WebSocket listener
- `SCRIPT_FILENAME` should resolve to the requested `.uce` file on disk
- `proxy_http_version 1.1` and the `Upgrade` / `Connection` headers are required for WebSockets
- socket-capable pages are ordinary `.uce` units; route client WebSocket upgrade requests to the HTTP/WebSocket listener

The `location /` block only serves files from `/var/www/html`. If your app uses a front-controller pattern such as routing everything through `/index.uce`, change that block accordingly.

### 6. Think about document root and private files

Point nginx at `/var/www/html`, not the runtime repository root. The repo still contains source, scripts, packaging files, and operational assets that are not meant to be public.

At minimum, explicitly block internal directories that should never be served directly. For example:

```nginx
location ~ ^/(src|scripts|etc|bin|work|dist|pkg)/ {
	return 404;
}
```

If nginx is rooted at `/var/www/html`, most of those paths will not be reachable anyway, which is the preferred setup.

### 7. Reload nginx and verify the deployment

After writing the nginx config:

```bash
nginx -t
systemctl reload nginx
```

Then verify:

```bash
systemctl status bearer.service
curl -i http://127.0.0.1/test/index.uce
curl -i http://127.0.0.1/doc/index.uce
```

If WebSockets are enabled, also verify a `.uce` endpoint that defines `WS(Request& context)` through nginx rather than talking to the runtime directly.

### 8. Troubleshooting

Common failure modes:

- `502 Bad Gateway`
  Usually means `bearer.service` is down, the Unix socket path does not match, or the request crashed before sending a valid response.
- WebSocket upgrade fails
  Check that nginx is routing WebSocket upgrade requests to `proxy_pass`, not `fastcgi_pass`, and that `HTTP_PORT` is reachable on localhost.
- Requests compile but immediately crash
  Check `journalctl -u bearer.service`. Generated units carry an ABI metadata sidecar and should be recompiled automatically after runtime ABI changes, but clearing stale artifacts under `BIN_DIRECTORY` is still a useful last-resort recovery step if the cache has been damaged manually.
- nginx serves raw source or internal files
  Tighten the server root and add explicit deny rules for non-public directories.

## Repo Helpers

- `./codesearch <pattern> [rg options...]`

This is a small repo-root wrapper around `rg` that searches from the project root and skips generated/build directories such as `.git/`, `bin/`, `dist/`, and `work/`.

## Reference Notes

For up-to-date usage, prefer:

- the live docs under `site/doc/`
- the declarations in `src/lib/compiler.h`, `src/lib/sys.h`, and `src/lib/functionlib.h`
- the example pages under `site/test/`

## AI Disclosure

This project is largely human-made, with all the typical idiosyncracies of my projects clearly visible. However, OpenAI Codex was used for code review, debugging, API integration work, and documentation. Claude Opus was used for UI design work, and I used VS Code's git commit message generator.
