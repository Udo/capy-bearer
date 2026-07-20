# BEARER Runtime Setup

This guide describes how to run BEARER behind nginx or Apache. BEARER is a FastCGI application server for `.uce` units; the web server should serve static files directly and forward dynamic `.uce` requests to the BEARER runtime.

## Deployment shape

A typical deployment has four pieces:

1. A checked-out or packaged BEARER runtime tree.
2. `/etc/bearer/settings.cfg`, read by the BEARER runtime at startup.
3. `bearer.service`, a systemd service that builds/starts/restarts the runtime.
4. nginx or Apache as the public HTTP server.

Recommended filesystem layout for a source checkout (replace paths as needed):

```text
/path/to/bearer-root                  BEARER repository/runtime root (for example `/opt/bearer` or `/Code/bearer.openfu.com/bearer`)
/var/www/html/                     public web root served by nginx/Apache
/etc/bearer/settings.cfg              runtime configuration
/run/bearer/fastcgi.sock              FastCGI socket used by nginx/Apache
/run/bearer/cli.sock                  local CLI/admin/test socket
/var/cache/bearer/work                generated source, wasm modules, caches
/var/lib/bearer/uploads               multipart upload scratch space
/var/lib/bearer/sessions              session files
```

For packaged installs, the runtime may live under `/usr/lib/bearer` instead of your checkout root. Keep the public web root at `/var/www/html` or another normal web-root path, not under the runtime source tree.

## Build requirements

On Debian/Ubuntu-like systems, install the distro packages first:

```bash
apt update
apt install -y clang build-essential libpcre2-dev libssl-dev mariadb-client libmariadb-dev curl rsync ca-certificates
```

BEARER also requires two non-vendored dependencies. WASI SDK is load-bearing at runtime because BEARER compiles units on demand during requests and during proactive startup scans. The `curl` binary is also a pinned runtime package dependency: `http_request()` and `http_request_async()` execute it directly with an explicit argument vector for TLS-capable outbound HTTP.

- **Wasmtime C API / C++ headers** at `/opt/wasmtime` by default. `scripts/build_linux.sh` expects:
  - `/opt/wasmtime/include/wasmtime.hh`
  - `/opt/wasmtime/include/wasmtime/*.h`
  - `/opt/wasmtime/lib/libwasmtime.so`

- **Pinned WASI SDK** at `/opt/wasi-sdk` by default. `scripts/build_core_wasm.sh` and request-time `scripts/compile_wasm_unit` expect:
  - `/opt/wasi-sdk/bin/clang++`
  - `/opt/wasi-sdk/bin/wasm-ld`
  - `/opt/wasi-sdk/bin/llvm-objcopy`
  - `/opt/wasi-sdk/bin/llvm-nm`
  - `/opt/wasi-sdk/bin/llvm-dwarfdump`

You can use different install locations by setting environment variables before building and in the systemd service environment:

```bash
export WASMTIME_HOME=/path/to/wasmtime
export WASI_SDK=/path/to/wasi-sdk
```

Install one web server:

```bash
apt install -y nginx
# or
apt install -y apache2
```

Build BEARER from your repository root:

```bash
repo_root=/path/to/bearer-root
cd "$repo_root"
bash scripts/build_core_wasm.sh
bash scripts/build_linux.sh
```

Publish the starter site or your application files into the web root:

```bash
mkdir -p /var/www/html
rsync -a site/ /var/www/html/
```

The main binary is written to:

```text
bin/bearer_fastcgi.linux.bin
```

### Installing Wasmtime and WASI SDK

The BEARER build does not download these dependencies for you. Install Wasmtime through a compatible distro package or a pinned upstream C API archive. Install WASI SDK with BEARER's pinned installer, or unpack the same pinned archive under `/opt/wasi-sdk`.

Do not use `curl | sh` installers in production setup scripts. Download archives from the upstream release pages, verify checksums/signatures when available, and record the exact versions in your deployment notes. Avoid installing a release published in the last few days unless you have reviewed it separately.

The expected directories are:

```text
/opt/wasmtime/include/wasmtime.hh
/opt/wasmtime/include/wasmtime/store.h
/opt/wasmtime/lib/libwasmtime.so

/opt/wasi-sdk/bin/clang++
/opt/wasi-sdk/bin/wasm-ld
/opt/wasi-sdk/bin/llvm-objcopy
/opt/wasi-sdk/bin/llvm-nm
/opt/wasi-sdk/bin/llvm-dwarfdump
```

Install the WASI SDK:

```bash
repo_root=/path/to/bearer-root
cd "$repo_root"
scripts/install_wasi_sdk.sh
scripts/install_wasi_sdk.sh --check-only
```

The current pin is documented in `docs/wasi-sdk-toolchain.md`. The script verifies the archive SHA256 before installing and updates `/opt/wasi-sdk` to point at the pinned versioned directory.

For Wasmtime, use a compatible distro package or an upstream C API archive. Example flow using an archive you have already chosen and verified:

```bash
mkdir -p /opt /tmp/bearer-deps
cd /tmp/bearer-deps

# Download the Wasmtime C API archive for your architecture from the upstream
# release page, then verify its checksum before unpacking. The archive name
# normally contains "c-api".
sha256sum -c wasmtime-c-api.sha256
mkdir -p /opt/wasmtime
tar -xf wasmtime-*-c-api*.tar.* -C /opt/wasmtime --strip-components=1
```

After unpacking, verify the tools BEARER needs. Also record the exact Wasmtime and WASI SDK versions used. The native build embeds an rpath for `$WASMTIME_HOME/lib`, so the service environment should use the same `WASMTIME_HOME` value used during build.

```bash
test -f /opt/wasmtime/include/wasmtime.hh
test -f /opt/wasmtime/lib/libwasmtime.so
/opt/wasi-sdk/bin/clang++ --version
/opt/wasi-sdk/bin/wasm-ld --version
/opt/wasi-sdk/bin/llvm-objcopy --version
/opt/wasi-sdk/bin/llvm-nm --version
/opt/wasi-sdk/bin/llvm-dwarfdump --version
```

If your paths differ, export the variables for manual builds:

```bash
WASMTIME_HOME=/usr/local/wasmtime WASI_SDK=/usr/local/wasi-sdk bash scripts/build_core_wasm.sh
WASMTIME_HOME=/usr/local/wasmtime WASI_SDK=/usr/local/wasi-sdk bash scripts/build_linux.sh
```

For systemd, add an override:

```bash
systemctl edit bearer.service
```

```ini
[Service]
Environment=WASMTIME_HOME=/usr/local/wasmtime
Environment=WASI_SDK=/usr/local/wasi-sdk
```

Then reload and restart:

```bash
systemctl daemon-reload
systemctl restart bearer.service
```

## Runtime configuration

Create `/etc/bearer/settings.cfg` from `etc/bearer/settings.cfg` and adjust paths to your checkout root. Replace checkout-specific settings such as `WASM_CORE_PATH` with `<BEARER_REPO_ROOT>/bin/wasm/core.wasm` or leave it relative to `COMPILER_SYS_PATH` as appropriate.

Minimum useful settings:

```ini
BIN_DIRECTORY=/var/cache/bearer/work
TMP_UPLOAD_PATH=/var/lib/bearer/uploads
SESSION_PATH=/var/lib/bearer/sessions
SESSION_COOKIE_SECURE=1

FCGI_SOCKET_PATH=/run/bearer/fastcgi.sock
FCGI_SOCKET_MODE=0666
CLI_SOCKET_PATH=/run/bearer/cli.sock
CLI_SOCKET_MODE=0600

SITE_DIRECTORY=/var/www/html
HTTP_DOCUMENT_ROOT=/var/www/html
JIT_COMPILE_ON_REQUEST=1
SHOW_DYNAMIC_COMPILE_ERRORS=1
SERVE_LAST_KNOWN_GOOD=0
PROACTIVE_COMPILE_ENABLED=1
PROACTIVE_COMPILE_JOBS=2
PROACTIVE_COMPILE_CHECK_INTERVAL=60

WASM_COMPILE_SCRIPT=scripts/compile_wasm_unit
WASM_BACKEND_VERBOSE=0
WASM_CORE_PATH=<BEARER_REPO_ROOT>/bin/wasm/core.wasm
WASM_MEMORY_LIMIT_BYTES=536870912
WASM_EPOCH_DEADLINE_TICKS=200
WASM_EPOCH_PERIOD_MS=50
WASM_INVOCATION_TIMEOUT_MS=30000
MYSQL_PERSISTENT_POOL_SIZE=8

WORKER_COUNT=4
MAX_MEMORY=16777216
SESSION_TIME=2592000

HTTP_PORT=8080
WS_BROKER_OUTBOUND_TIMEOUT_SECONDS=30
```

Important settings:

- `FCGI_SOCKET_PATH` is the Unix socket used for normal `.uce` requests. Set it explicitly and keep this value and the web-server `fastcgi_pass` path identical. The reference config uses `/run/bearer/fastcgi.sock`; if you choose `/run/bearer.sock`, use it in both places.
- `CLI_SOCKET_PATH` is a local HTTP-over-Unix socket used by `scripts/bearer-cli` and test/admin units. Keep it private (`CLI_SOCKET_MODE=0600`) unless you intentionally delegate admin/test execution to a trusted Unix group (`0660`).
- `FCGI_SOCKET_MODE` and `CLI_SOCKET_MODE` are octal permission modes applied after socket bind. Prefer tightening `FCGI_SOCKET_MODE` to `0660` when nginx/Apache can share a trusted group with the BEARER worker.
- `SITE_DIRECTORY` is the public site tree to scan for `.uce` files. Use `/var/www/html` when the web root is outside the runtime tree; relative paths are resolved from the runtime working directory. Installed regression gate scripts derive their temporary test root from this setting unless `BEARER_TEST_SITE_DIRECTORY` is explicitly provided.
- `HTTP_DOCUMENT_ROOT` is the root used by the built-in HTTP/WebSocket listener when it resolves upgrade requests. Set it to the same web root as nginx/Apache.
- `BIN_DIRECTORY` stores runtime state plus ABI-scoped unit generations. Unit
  C++, wasm, serialized modules, source maps, and compile diagnostics live in
  `units-c<compiler ABI>-w<core ABI>` so an upgrade cannot mix generations.
- `TMP_UPLOAD_PATH` and `SESSION_PATH` must be writable by the runtime.
- `SESSION_COOKIE_SECURE=1` adds the `Secure` attribute to BEARER-managed session cookies and should be used for HTTPS-only deployments. Leave it `0` only for local/plain-HTTP development.
- `MYSQL_PERSISTENT_POOL_SIZE` caps credential-keyed connections retained by each Wasm worker. The default `8` is clamped to `64`; set it to `0` to restore request-lifetime connections. Cached sessions are reset before reuse.
- `HTTP_PORT` is the built-in HTTP/WebSocket listener used for WebSocket upgrade traffic and direct local probes. Bind/firewall it for local access only; nginx/Apache should be the public entry point.
- `WS_BROKER_OUTBOUND_TIMEOUT_SECONDS` controls how long a forwarded WS message can remain queued in the broker before being dropped (default `30`). Set to `0` to disable the timeout.
- `WASM_COMPILE_SCRIPT` must point to `scripts/compile_wasm_unit` unless you provide an equivalent compiler. Relative paths are resolved from the runtime root/`COMPILER_SYS_PATH`. That script calls `scripts/check_unit_wasm.py` after linking each unit and uses the pinned WASI SDK on every deployment host.
- `SHOW_DYNAMIC_COMPILE_ERRORS=1` makes a failed dynamic `component()`, `unit_render()`, or `unit_call()` show the bounded compiler diagnostic instead of only a generic missing-handler message. Set it to `0` on deployments where source paths and compiler output must not reach HTTP responses.
- `SERVE_LAST_KNOWN_GOOD=1` lets HTTP GET/HEAD/OPTIONS requests keep using a compatible complete unit artifact while the proactive compiler builds changed source. It defaults to `0`; CLI and mutation requests always use current code or fail closed. The option requires an enabled proactive compiler with a positive check interval. Failed background builds preserve the prior Wasm, source map, and serialized module until a successful atomic publication replaces them. Missing source or an incompatible compiler/core ABI is never served as last-known-good.
- `PROACTIVE_COMPILE_JOBS` selects 1–16 low-priority full-site scanner processes (default `2`). Each canonical unit path has one scanner owner. When last-known-good serving is enabled, the separate higher-priority demand compiler remains reserved for stale units requested over HTTP, so total background compile concurrency can reach this value plus one.
- `WASM_CORE_PATH` must point at the built `core.wasm` file.
- `WASM_EPOCH_DEADLINE_TICKS` and `WASM_EPOCH_PERIOD_MS` bound one
  uninterrupted guest CPU segment. `WASM_INVOCATION_TIMEOUT_MS` is the
  separate absolute wall-clock bound for app-owned unit loading,
  initialization, the selected handler, and all nested component/unit calls.
  The three values must be positive integers; the ticker period is capped at
  `1000` ms and the invocation timeout at `86400000` ms. Invalid values prevent
  the Wasm backend from starting. The invocation timeout defaults to `30000` and is
  enforced to the epoch ticker's period resolution. Blocking host helpers
  retain their own shorter limits and are capped to the remaining invocation
  budget where the underlying operation is cancellable. Cold entry compilation,
  explicit `unit_compile()`, dynamic component JIT, transitive `#load` work,
  compiler and registry lock waits, and the configured runtime-error page all
  consume the same request budget. A timed-out compiler process group is killed
  without replacing the prior artifact set or persisting a compile-failure
  backoff. Request compiles require a zero child exit status and complete staged
  output; generated C++, exports, source map, Wasm, metadata, cached-module
  invalidation, and diagnostics are published or rolled back as one guarded
  generation. Proactive and offline precompile work remain independent of
  request invocation deadlines. Serialized-module metadata scanning checks the
  absolute deadline between section headers and before and after every bounded
  4 KiB positional read. Cache-miss full-artifact reads use the same checks
  around 64 KiB positional chunks. A single in-progress regular-file syscall
  cannot be cooperatively interrupted; later reads and parsing cannot overrun the budget.
  Initial/final descriptor identity, unique selected metadata sections, and
  strict 64-bit LEB high-bit validation reject changed or ambiguous artifacts.

After editing settings, restart BEARER:

```bash
systemctl restart bearer.service
```

## systemd service

For source-checkout deployments, install the provided service helper:

```bash
repo_root=/path/to/bearer-root
cd "$repo_root"
scripts/systemd/manage-bearer-service.sh setup
```

That helper installs `scripts/systemd/bearer.service` from your checked-out tree, rewrites the checked-out repository path into the installed unit and first-time config, creates runtime directories, enables the service, and starts it. Use a custom unit only if you need a nonstandard runtime layout.

Useful commands:

```bash
scripts/systemd/manage-bearer-service.sh status
scripts/systemd/manage-bearer-service.sh restart
scripts/systemd/manage-bearer-service.sh logs 200
```

The server binary accepts only these process modes:

```bash
bin/bearer_fastcgi.linux.bin             # start the server
bin/bearer_fastcgi.linux.bin --precompile
bin/bearer_fastcgi.linux.bin --help
```

Help and invalid arguments exit before configuration is loaded or any listener
is opened. Invalid or extra arguments return status 2. This ordering prevents a
diagnostic invocation from replacing or removing a configured Unix socket.

Managed restart builds and precompiles the complete candidate ABI generation
before it switches the service. Precompile uses two low-priority processes by
default; set `PRECOMPILE_JOBS` in `/etc/bearer/settings.cfg` to tune the bounded
1–16 process count for the host. A failed worker, compile, serialization, or
result report aborts the switch and leaves the current service running. The
managed invocation is bounded to 900 seconds by default; set
`BEARER_PRECOMPILE_TIMEOUT` on `manage-bearer-service.sh restart` to select another
GNU `timeout` duration. Timeout exit 124 also aborts before the service switch.

A trusted direct `--precompile` process may set `BEARER_PRECOMPILE_FILES_IN` and
`BEARER_PRECOMPILE_BIN_DIRECTORY` to isolate that invocation's source scan and
artifact registry. Both overrides apply only to precompile mode; normal server
workers continue to use `/etc/bearer/settings.cfg`. The parallel precompile
regression uses private roots so a running proactive compiler cannot consume
or publish its controlled race fixtures.

Direct `--precompile` is a listener-free whole-generation gate, not a
single-unit readiness probe. It scans the configured source root, waits for
unit publication locks, and has no internal wall-clock timeout. Automation
must wrap it in a finite external timeout, as the managed restart does, before
starting separately deadline-bounded CLI or HTTP invocations.

Prefer the managed restart when the service runs as an unprivileged user: it
precompiles as that same user. Request-time publication still accepts readable
artifacts produced by a trusted administrator. When Linux protected-hardlink
policy rejects the normal rollback snapshot, BEARER copies the prior artifact
under the unit lock; generation markers are replaced atomically instead of
requiring write access to the existing inode. Existing readable lock files can
be locked without write access, including the shared-PCH lock.

Equivalent manual systemd service for a source checkout (`<BEARER_REPO>` = checkout root):

```ini
[Unit]
Description=BEARER FastCGI Runtime
After=network-online.target mariadb.service memcached.service
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=<BEARER_REPO>
RuntimeDirectory=bearer
StateDirectory=bearer
CacheDirectory=bearer
ExecStartPre=/usr/bin/mkdir -p /var/cache/bearer/work /var/lib/bearer/uploads /var/lib/bearer/sessions
ExecStartPre=/usr/bin/rm -f /run/bearer/fastcgi.sock
ExecStartPre=/usr/bin/bash <BEARER_REPO>/scripts/build_linux.sh
ExecStart=<BEARER_REPO>/bin/bearer_fastcgi.linux.bin
ExecStopPost=/usr/bin/rm -f /run/bearer/fastcgi.sock
Restart=always
RestartSec=2
TimeoutStopSec=15
KillMode=mixed
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Install it as `/etc/systemd/system/bearer.service` and run:

```bash
systemctl daemon-reload
systemctl enable --now bearer.service
```

### Debian package build

To build a Debian package from the repository root:

```bash
bash scripts/make_deb.sh 0.1.2
```

The Debian package creator bundles WASI SDK and Wasmtime by default when `/opt/wasi-sdk` and `/opt/wasmtime` are present. Verify the pinned SDK before building:

```bash
scripts/install_wasi_sdk.sh --check-only
bash scripts/make_deb.sh 0.1.2
```

This includes the resolved `/opt/wasi-sdk-...` tree, `/opt/wasi-sdk` symlink, resolved `/opt/wasmtime-...` tree, and `/opt/wasmtime` symlink in the package. It makes the package large, but keeps request-time unit compilation and runtime linking tied to the toolchain versions that passed the test suite. Set `BEARER_DEB_BUNDLE_WASI_SDK=0` or `BEARER_DEB_BUNDLE_WASMTIME=0` only if your deployment provides those exact dependencies separately.

### RPM package build

To build an RPM package from the repository root, install `rpmbuild` on the packaging host, verify the pinned SDK, then run:

```bash
scripts/install_wasi_sdk.sh --check-only
bash scripts/make_rpm.sh 0.1.2
```

The RPM creator mirrors the Debian package layout: runtime files under `/usr/lib/bearer`, public files under `/var/www/html`, config under `/etc/bearer/settings.cfg`, systemd unit under `/usr/lib/systemd/system/bearer.service`, and bundled `/opt/wasi-sdk` plus `/opt/wasmtime` trees by default. Set `BEARER_RPM_BUNDLE_WASI_SDK=0` or `BEARER_RPM_BUNDLE_WASMTIME=0` only if your deployment provides those exact dependencies separately.

## How request routing works

### Static files

The web server should serve ordinary static files directly from the public web root, for example `/var/www/html`.

Examples:

```text
/style.css
/images/logo.png
/examples/bearer-starter/js/site.js
```

These should not touch the BEARER runtime.

### Normal `.uce` page requests

For a request such as:

```text
GET /doc/index.uce?p=component
```

nginx/Apache forwards the request to `FCGI_SOCKET_PATH` as FastCGI. The web server must provide CGI/FastCGI variables including:

- `SCRIPT_FILENAME` — full filesystem path to the `.uce` file.
- `DOCUMENT_ROOT` — public web root, normally `/var/www/html` or whatever nginx/Apache uses as `root`/`DocumentRoot`.
- `SCRIPT_NAME` — URL path to the script, such as `/doc/index.uce`.
- `DOCUMENT_URI` — normalized URI path without query string.
- `REQUEST_URI` — original request URI including query string.
- standard request variables such as method, query string, content type, body length, cookies, and headers.

BEARER resolves the unit, compiles it to wasm if needed, creates a request workspace, and calls:

```cpp
RENDER(Request& context)
```

The unit writes output with template literals or `print()`. Response headers and status are set through `context.header` and `context.set_status()`.

### Component and sub-render calls

Inside a request, BEARER code can call other units:

```cpp
component("components/card", props, context);
unit_render("other-page.uce", context);
```

These calls stay inside the BEARER runtime. They are not new HTTP requests and do not go back through nginx or Apache.

### WebSocket pages

Any `.uce` unit can provide both an ordinary page render and WebSocket message handling:

```cpp
RENDER(Request& context) { ... }  // normal page load
WS(Request& context) { ... }      // later WebSocket messages
```

The nginx and Apache examples below split traffic by checking for a WebSocket upgrade request on `.uce` paths. A file such as `chat.uce` or `events.uce` can expose `WS(Request& context)`.

Routing split:

- Plain `GET /demo/chat.uce` should use FastCGI, just like any other page render.
- WebSocket upgrade requests for `/demo/chat.uce` should proxy to the BEARER built-in HTTP/WebSocket listener at `HTTP_PORT`.

The built-in listener owns the socket lifecycle. When a message arrives, the broker forwards a render-style invocation back to the worker pool so `WS(Request& context)` runs inside the same wasm runtime model as normal pages.

### CLI requests

`CLI(Request& context)` handlers are not public web endpoints. They are invoked over `CLI_SOCKET_PATH`:

```bash
scripts/bearer-cli /tests/cli.uce action=echo message=hello
curl --unix-socket /run/bearer/cli.sock http://localhost/tests/cli.uce
```

Use CLI units for local tests, admin commands, and maintenance tools. Do not expose the CLI socket through nginx or Apache.

### Custom runtime HTTP servers

BEARER code can start local custom HTTP listeners with `server_start_http()`. Those are runtime-managed listeners for app-specific local services. They are separate from the public nginx/Apache entry point and should be firewalled or bound locally unless you explicitly want them reachable.

## nginx configuration

### Required modules

A normal nginx build includes the needed FastCGI and proxy modules. Confirm nginx is installed and can load your config:

```bash
nginx -t
```

### WebSocket upgrade map

Put this in the nginx `http` block if using WebSockets:

```nginx
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}
```

### Server block

Example site config:

```nginx
server {
    listen 80;
    server_name example.com;

    root /var/www/html;
    index index.uce index.html;

    # Serve static files directly. Directory requests use index.uce when present.
    location / {
        try_files $uri $uri/ =404;
    }

    # BEARER page requests use FastCGI. If the client asks to upgrade a .uce
    # request to WebSocket, send that connection to the built-in listener.
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

    # Defense in depth if the root is changed later.
    location ~ ^/(src|scripts|etc|bin|work|dist|pkg|docs|changelog)/ {
        return 404;
    }
}
```

Notes:

- `fastcgi_pass` must match `FCGI_SOCKET_PATH`.
- `proxy_pass` must match `HTTP_PORT`.
- The example routes WebSocket upgrades for `.uce` paths to the HTTP/WebSocket listener.
- The built-in HTTP/WebSocket listener resolves scripts from `HTTP_DOCUMENT_ROOT`; do not depend on client-supplied or proxied `Script-Filename` headers for routing.
- Ordinary `.uce` page loads continue to use FastCGI.
- Keep `root` pointed at `/var/www/html`, not the runtime repository root.
- If your app uses a front controller, replace `location /` with a `try_files` rule that ends at `/index.uce`.

Front-controller variant:

```nginx
location / {
    try_files $uri $uri/ /index.uce?$query_string;
}
```

Reload nginx:

```bash
nginx -t
systemctl reload nginx
```

## Apache configuration

Apache can run BEARER through `mod_proxy_fcgi` for FastCGI and `mod_proxy_wstunnel` or `mod_proxy_http` for WebSocket upgrades.

### Enable modules

On Debian/Ubuntu:

```bash
a2enmod proxy proxy_fcgi proxy_http proxy_wstunnel rewrite headers setenvif
systemctl restart apache2
```

### VirtualHost example

```apache
<VirtualHost *:80>
    ServerName example.com
    DocumentRoot /var/www/html

    <Directory /var/www/html>
        Require all granted
        Options FollowSymLinks
        AllowOverride None
        DirectoryIndex index.uce index.html
    </Directory>

    # Do not expose repository internals if DocumentRoot changes later.
    <LocationMatch "^/(src|scripts|etc|bin|work|dist|pkg|docs|changelog)/">
        Require all denied
    </LocationMatch>

    RewriteEngine On

    # WebSocket upgrade traffic for any .uce unit goes to BEARER's built-in HTTP listener.
    RewriteCond %{HTTP:Upgrade} =websocket [NC]
    RewriteCond %{REQUEST_URI} \.uce(?:\?|$) [NC]
    RewriteRule ^/(.*)$ ws://127.0.0.1:8080/$1 [P,L]

    # Normal .uce page loads go to FastCGI.
    <FilesMatch "\.uce$">
        SetHandler "proxy:unix:/run/bearer/fastcgi.sock|fcgi://localhost/"
    </FilesMatch>

    # Optional: make the key CGI variables explicit for BEARER.
    ProxyFCGISetEnvIf "true" DOCUMENT_ROOT "/var/www/html"
    ProxyFCGISetEnvIf "true" DOCUMENT_URI "%{REQUEST_URI}"
</VirtualHost>
```

Apache notes:

- `SetHandler "proxy:unix:/run/bearer/fastcgi.sock|fcgi://localhost/"` must use the same socket path as `FCGI_SOCKET_PATH`.
- The WebSocket rewrite rule must run before the FastCGI handler.
- Plain `.uce` page loads should not be proxied as WebSockets unless the client sends `Upgrade: websocket`.
- Apache's FastCGI environment differs by version and module configuration. If BEARER cannot resolve a page, inspect the request environment and make sure `SCRIPT_FILENAME` points to the target file under the web root.

If your Apache version does not populate `SCRIPT_FILENAME` correctly through `SetHandler`, use `ProxyPassMatch` for `.uce` files instead:

```apache
ProxyPassMatch ^/(.*\.uce)$ unix:/run/bearer/fastcgi.sock|fcgi://localhost/var/www/html/$1
```

Use only one FastCGI mapping style at a time (`SetHandler` or `ProxyPassMatch`) to avoid duplicate routing.

## Permissions

The web server needs permission to connect to `/run/bearer/fastcgi.sock`. Common approaches:

- run BEARER and the web server under compatible groups;
- add the web server user (`www-data` on Debian/Ubuntu) to the socket's group;
- adjust the service or runtime socket mode if needed.

The runtime creates the FastCGI socket and CLI socket under `/run/bearer`. The CLI socket should remain local-only and should not be reachable from the public web server.

Writable paths for the runtime:

```text
/var/cache/bearer/work
/var/lib/bearer/uploads
/var/lib/bearer/sessions
/run/bearer
```

## Verification

Check service state:

```bash
systemctl status bearer.service
journalctl -u bearer.service -n 100 --no-pager
```

Check the FastCGI/web-server path:

```bash
curl -i http://127.0.0.1/doc/index.uce -H 'Host: example.com'
curl -i http://127.0.0.1/examples/bearer-starter/ -H 'Host: example.com'
```

Check the local CLI path:

```bash
repo_root=/path/to/bearer-root
cd "$repo_root"
scripts/bearer-cli /tests/cli.uce action=echo message=hello
scripts/run_cli_tests.sh
```

Check WebSocket routing with a WebSocket client against a `.uce` endpoint that defines `WS(Request& context)` through nginx/Apache, not directly against `HTTP_PORT`:

```bash
python3 - <<'PY'
import base64, os, socket
host = "example.com"
path = "/chat.uce"
key = base64.b64encode(os.urandom(16)).decode()
request = (
    f"GET {path} HTTP/1.1\r\n"
    f"Host: {host}\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n"
).encode()
sock = socket.create_connection(("127.0.0.1", 80), timeout=5)
sock.sendall(request)
print(sock.recv(4096).decode("latin1", "replace").split("\r\n", 1)[0])
sock.close()
PY
```

## Password hashing

Use the native password API for application credentials:

```cpp
String encoded = password_hash(password);
if(encoded == "")
	// fail the write; native hashing did not complete

bool valid = password_verify(candidate, encoded);
if(valid && password_needs_rehash(encoded))
	encoded = password_hash(candidate);
```

`password_hash()` returns a self-contained `$bearer$scrypt$...` encoding with a random 16-byte salt and the bounded scrypt parameters `N=65536`, `r=8`, `p=1`. `password_verify()` accepts only structurally valid encodings with bounded cost parameters and compares the derived key in constant time. `password_needs_rehash()` reports malformed, legacy, or non-current parameters so applications can upgrade a credential after a successful legacy verification. Treat an empty hash as an operational failure and never store it. Application-level password length policy, rate limiting, and legacy-format verification remain the application's responsibility.

## Operational footguns

- Keep the FastCGI socket path consistent: `FCGI_SOCKET_PATH` and the web-server `fastcgi_pass` must match exactly. The reference config uses `/run/bearer/fastcgi.sock`; if you choose `/run/bearer.sock`, use it in both places.
- Keep the public web root separate from the runtime source tree. Replace `/opt/bearer` with your actual checkout path in local examples (for example `/opt/bearer` or `/Code/bearer.openfu.com/bearer`), and keep public files under `/var/www/html`.
- Set `HTTP_DOCUMENT_ROOT` when the web root is outside the runtime working directory. The built-in HTTP/WebSocket listener resolves upgrade paths from this setting.
- Do not expose `CLI_SOCKET_PATH` or `HTTP_PORT` as public entry points. The public path should be nginx/Apache.
- Do not trust `Script-Filename` request headers from direct HTTP clients. The built-in HTTP listener resolves from `HTTP_DOCUMENT_ROOT` and rejects `..` path segments.
- WASI SDK is a deployment/runtime dependency, not just a developer build tool. BEARER compiles units to wasm on demand during requests and during proactive startup scans, so each host must use the pinned SDK version documented in `docs/wasi-sdk-toolchain.md`.
- After toolchain or compile-script fixes, clear stale failed artifacts under `BIN_DIRECTORY`; otherwise a later request may report an old compile failure.

## Troubleshooting

### 502 Bad Gateway

Check:

- `systemctl status bearer.service`
- `journalctl -u bearer.service -n 200 --no-pager`
- socket path in web server config equals `FCGI_SOCKET_PATH`
- web server user can connect to the Unix socket
- `SCRIPT_FILENAME` resolves to an existing `.uce` file

### Raw `.uce` source is downloaded or displayed

The `.uce` request did not match the FastCGI rule. Check location/order rules and confirm the public root is `/var/www/html` or your chosen web-root path.

### Static files 404

Confirm the web server `root`/`DocumentRoot` is `/var/www/html` or your chosen web-root path and that `location /` or Apache directory rules allow static file reads.

### WebSocket page renders but upgrade fails

Check:

- the client sends `Upgrade: websocket`
- `.uce` upgrade traffic reaches `HTTP_PORT`
- nginx/Apache preserves `Upgrade` and `Connection` headers
- `HTTP_DOCUMENT_ROOT` matches the web-server root; if it points at the runtime tree while files live in `/var/www/html`, the built-in listener will return `404 script not found`
- firewall/network policy allows localhost access to `HTTP_PORT`

### Page compiles fail

Check the compile artifact paths shown in the BEARER error response and service logs. Generated files and compile output live under `BIN_DIRECTORY`.

Common compile footguns:

- `WASM_COMPILE_SCRIPT` is unset or points at a removed script such as `scripts/compile`; set it to `scripts/compile_wasm_unit`.
- `scripts/check_unit_wasm.py` is missing or not executable; `scripts/compile_wasm_unit` calls it after linking each unit.
- `WASI_SDK` does not point at the pinned tree with `clang++`, `wasm-ld`, `llvm-objcopy`, `llvm-nm`, and `llvm-dwarfdump`; run `scripts/install_wasi_sdk.sh --check-only`.
- `WASMTIME_HOME` does not point at a tree with Wasmtime headers and `libwasmtime.so`.
- A previous failed compile left stale `.compile.txt`, `.wasm-check.txt`, or partial `.wasm` files under `BIN_DIRECTORY`.

Failed compile output is persisted under the unit's ABI-generation path in
`BIN_DIRECTORY` and may be reused until the source or compiler inputs change.
The managed `restart` command first runs the new binary's `--precompile` mode as
the configured service user while the old service remains live. It restarts
systemd only after every scanned unit compiles and serializes successfully.
First fix source/toolchain failures and retry; do not remove the prior generation,
which remains the rollback path. Avoid deleting the whole `BIN_DIRECTORY` unless
you intentionally want a full rebuild and have accepted losing rollback artifacts.

### CLI commands fail

Check:

- `CLI_SOCKET_PATH` in `/etc/bearer/settings.cfg`
- `/run/bearer/cli.sock` exists
- `scripts/bearer-cli --socket /run/bearer/cli.sock /ping` works
- the target unit defines `CLI(Request& context)`
