# UCE WASM Runtime Architecture

Status: current as of the W7e native-pipeline removal (June 2026). This document
describes the **runtime architecture as built** — the process topology, the
wasm membrane, the unified request dispatch, and the central WebSocket broker.
Native `.so` unit execution/dlopen fallback has been removed; the parser and
preprocessor remain only as the front-end that emits C++ for wasm side-module
compilation.

The guiding principle: **request code never shares an address space or an
allocator with the runtime.** Every unit runs as a WebAssembly module inside a
per-request workspace behind a narrow host membrane. Long-lived connection
state (WebSocket connections, listening HTTP sockets) lives in dedicated native
broker processes that own no unit code — they hold the connection and forward
the actual unit invocation to a clean-engine worker, exactly like a normal
page render.

---

## 1. Process topology

A single native binary (`uce_fastcgi`) forks into a small set of long-lived
roles. None of them is special-cased per request mode; the differences are
purely *which socket a process listens on* and *which function inside a unit
gets invoked*.

```
                          ┌────────────────────────────┐
        nginx  ──FastCGI──►  worker pool (N processes)  │  $FCGI_SOCKET_PATH (example `/run/uce/fastcgi.sock`)
   (port 80 etc.)          │  uniform unit renderers    │  (FastCGI + CLI)
                          └─────────────▲──────────────┘
                                        │ forward render (FastCGI, FCGI_SOCKET_PATH)
                                        │
   browser ──raw HTTP / WS──►  ┌────────┴─────────┐
        (HTTP_PORT 8080)       │   WS broker      │  owns HTTP_PORT + every
                               │  (1 process)     │  WS connection; renders
                               └────────▲─────────┘  nothing itself
                                        │ ws_* command flush
                                        │ (FastCGI, ws-broker.sock)
   on-demand serve_http   ──►  ┌────────┴─────────┐
        (per bind addr)        │ custom-server    │  owns one serve_http
                               │ dispatcher(s)    │  bind addr; forwards to pool
                               └──────────────────┘

   parent process: spawns/respawns all of the above + the proactive compiler.
```

| Process | Owns | Renders units? | Source |
|---|---|---|---|
| **Parent** | nothing; supervises children | no | `main()`, `init_base_process()` |
| **Worker** (×`WORKER_COUNT`) | `FCGI_SOCKET_PATH` (configured socket path; example `/run/uce/fastcgi.sock`) + `CLI_SOCKET_PATH` | **yes** — the only processes that run wasm | `listen_for_connections()` |
| **WS broker** (×1) | `HTTP_PORT` + every live WS connection + `WS_BROKER_SOCKET_PATH` | no — forwards to the pool | `run_ws_broker()` |
| **serve_http dispatcher** (×bind) | one custom-server bind address | no — forwards to the pool | `custom_server_http_dispatcher_loop()` |
| **Proactive compiler** | nothing; pre-compiles units | no | `run_proactive_compiler()` |

**only workers instantiate Wasmtime and run unit code.**
Every connection-owning process (broker, serve_http dispatcher) forwards the
request invocation back to a worker via `FCGI_SOCKET_PATH` using the minimal
FastCGI client in [`src/lib/fcgi_forward.h`](../src/lib/fcgi_forward.h). This is
forced by Wasmtime: an `Engine`/`Store` cannot be safely re-created across
`fork()`, and the brokers fork from the parent that already touched the
runtime. So the brokers hold the long-lived connection and units respond to
events the same way they respond to a page request — through a clean worker.

The broker-to-worker hop explicitly sets `GATEWAY_INTERFACE=CGI/1.1`. After
the Wasm core decodes request parameters it derives the guest's default status
syntax from that marker (`Status:` for FastCGI, `HTTP/1.1` for direct HTTP).
The FastCGI transport clears completed stdout/stderr as soon as their records
are queued and emits no stream records after `FCGI_END_REQUEST`. Together these
boundaries prevent a forwarded dynamic response from being parsed as body and
wrapped in a second HTTP response. The TCP and custom-server tests reject an
inner status line and require the exact dynamic body.

---

## 2. The membrane and the DValue ABI

Units are compiled to WebAssembly and linked against a host import surface (the
"membrane"). Host and guest exchange structured values as **UCEB** (a compact
binary `DValue` encoding — `ucb_encode`/`ucb_decode`); strings cross as
`std::string` on the native side throughout (no raw `char*` ownership across
the boundary).

- `DValue` is the universal value type: scalars plus an ordered child map
  (`_map`, a `std::map<String,DValue>`). `operator[]` is non-const (creates);
  `.key(k)` is the const probe returning `const DValue*`; `.each(fn)` iterates
  children yielding `const DValue&`.
- The request context (`params`/`get`/`post`/`cookies`/`session`, the raw body
  `in`, and—for WS—the connection context) is marshalled into a single `ctx`
  DValue and UCEB-encoded. Immutable server configuration is UCEB-encoded once
  per worker as a flat string map; both byte ranges are written into one guest
  buffer. The guest decodes the flat map directly into its fresh `Server` and
  the dynamic tree into its fresh `Request`. The response (body, headers,
  status, and any `meta` such as `ws_commands`) comes back as UCEB.

See [`docs/wasm-phase1-dvalue-abi.md`](wasm-phase1-dvalue-abi.md) for the wire
format details.

Decoder robustness note: UCEB input is untrusted across the wasm membrane. The
UCEB decoder must reject malformed magic/version/varint/length/trailing-data
inputs and excessive nesting with explicit errors, not native or wasm traps. Its
current hard nesting cap is intentionally low (64 levels) to stay well below the
guest stack limit. JSON decoding follows the same rule for malformed strings and
unicode escapes: validate bounds before every read and return an empty/partial
`DValue` rather than reading beyond the input.

---

## 3. Units, handlers, and export naming

A `.uce` unit compiles to a wasm module that exports one symbol per **handler**.
There is no per-mode machinery — a "CLI unit", "WebSocket unit", and "page" are
the same compiled artifact invoked at different exports. The dispatcher passes a
**handler string**; the host maps it to an export symbol:

```
__uce_<base>[_<sanitize(suffix)>]
```

`handler_export_symbol()` (`src/wasm/worker.cpp`) splits on the first `:`:

| Handler string | Export symbol |
|---|---|
| `render` | `__uce_render` |
| `cli` | `__uce_cli` |
| `websocket` | `__uce_websocket` |
| `once` | `__uce_once` (optional; absence is not an error) |
| `serve_http` | `__uce_serve_http` |
| `serve_http:named` | `__uce_serve_http_named` |
| `component:CARD` | `__uce_component_CARD` |
| `exists` | probe only — resolves the unit, loads nothing |

`sanitize_symbol_suffix()` keeps `[A-Za-z0-9_]` (mirrors `ascii_safe_name`).
Unit code is hidden by default so the linker can discard loaded helpers that the
unit does not use. Handler macros and the explicit `EXPORT` directive are the
only application symbols promoted to default visibility; generated request
binding and constructor exports remain part of the runtime ABI. Debug sections
are removed after linking while the wasm name section is retained for runtime
traces. This keeps each side module's code and public ABI local to its actual
handler responsibility instead of duplicating every helper from a loaded app
library.

`wasm_resolve_target(unit, handler)` (`src/wasm/core.cpp`) resolves the source
path and looks up the export's funcref slot; `exists` lets callers probe a unit
without instantiating it.

The request entry unit is already loaded by the host, so its selected handler
and optional `ONCE` export are placed directly into the workspace table and
passed to `uce_wasm_invoke_loaded_entry()`. Dynamic component and unit calls
continue through `wasm_resolve_target()`, where relative-path resolution is
required. Handler cache keys include the calling unit so identically named
relative targets in different directories cannot alias. Once a unit has passed
freshness checks and loaded into a request workspace, later handler lookups
reuse that immutable request-local instance; `component_loaded_reuse_count`
reports those lookups. Opt-in verbose response headers split entry invocation
into load, presence lookup, table linking, and core dispatch time.

`wasm_backend_should_handle(request, entry_unit)` checks whether the wasm
backend is initialized and the requested artifact/handler is currently
available. If an artifact is cold or stale, dispatch compiles it on demand via
`get_shared_unit()` and rechecks. There is no native unit-execution fallback.
Workers identify cached unit artifacts by nanosecond mtime, ctime, and size.
Whole-second mtime alone is insufficient because a dependency-triggered rebuild
can replace a wasm artifact within the same second as its prior build.
The worker also classifies each compiled module's immutable import descriptors
once when that artifact enters the module cache. Each request still creates its
own import vector, memory/table-base globals, GOT globals and function-table
slots, then runs relocations, constructors, and request-pointer binding. This
removes repeated Wasmtime import-type traversal without sharing request state or
changing core-first, unit-load-order symbol resolution.
Each FastCGI child initializes its process-local Wasmtime engine after fork and
before entering the accept loop. It also creates the linker, resolves the core's
host imports into a store-independent Wasmtime `InstancePre`, and births then
drops one empty workspace. This preserves Wasmtime's fork boundary while
preventing the first request assigned to each worker from paying engine, linker,
or pre-instantiation startup.
Server configuration is immutable by that point. The worker therefore retains
one native `DValue` view and one UCEB encoding of it. Each request transfers
those cached bytes and the guest decodes the flat scalar map directly into its
fresh `Server`, avoiding both repeated native encoding and an intermediate guest
`DValue` tree. Request parameters, body, cookies, session, call data, and
response state are never retained this way.
Startup duration or failure is written to the service log. The serialized core
module lives in the configured writable cache root rather than beside the
possibly root-owned deployed `core.wasm`; freshness still uses the deployed
artifact's metadata.

Packaged deployments use systemd socket activation for FastCGI. The socket unit
owns `/run/uce/fastcgi.sock`; UCE validates and adopts the single named listener
after exec. The listener therefore remains connectable and queues requests while
the service and its post-fork workers restart. Direct launches without systemd
activation retain the existing configured Unix/TCP listener behavior.
The service preserves its runtime directory across service restarts because the
socket unit, not the service unit, owns a listener path inside that directory.
On termination the parent asks render workers to close their listeners, finish
accepted connections within the bounded worker drain interval, and only then
exits. This prevents an accepted FastCGI request from being reset at handoff;
the socket unit queues later connections for the replacement workers.
The graceful signal handler belongs to the parent and render workers. Generic
`task()` children restore default termination signals after fork so
`task_kill()` and `server_stop()` retain their immediate stop contract.

Epoch interruption measures uninterrupted guest CPU segments. A separate
absolute workspace invocation deadline starts before app-owned entry-unit
readiness and cold compilation, loading, and initialization. It remains
unchanged through the selected handler, ONCE, every nested component/unit call,
and configured runtime-error rendering. The common hostcall membrane checks
that deadline before and after every native call and re-arms the store with the
smaller of the remaining absolute budget and the CPU-segment budget. A cheap
hostcall loop therefore cannot renew an invocation indefinitely. Blocking host
helpers retain operation-specific limits and cap them to the remaining
invocation budget where the underlying operation is cancellable. Forked task
callbacks receive a fresh invocation deadline capped by the task lifetime.
Synchronous compiler locks, transitive `#load` compilation, and compiler child
processes consume that same deadline. Compiler children run in a dedicated
process group; timeout kills the group, retains the previous generation, and
never records a caller-budget timeout as a persisted source failure. Bounded
compiles require a valid zero child exit, cap captured output, and stage
generated C++, exports, source maps, Wasm, and metadata. Publication removes
the prior serialized module and diagnostics inside the same rollback-protected
generation while repeatedly checking the live request deadline.

`request_perf()` reports worker module-cache hits and misses and divides a miss
into artifact lookup, wasm read, custom-section parse, serialized-module
deserialization or wasm compilation, and immutable import classification. This
includes a bounded per-request unit trace with site-relative unit names, cache
source, bytes, and phase durations so a cold outlier identifies its exact unit
and whether it used worker memory, serialized code, compilation, or failed.
Absolute source paths and source contents are not exposed. This keeps cold-worker
module latency distinguishable without exposing source paths.
The same snapshot divides pre-dispatch WASM readiness into entry normalization,
mutation freshness, artifact stat, source-generation lookup, dependency
freshness, and worker availability. `ready_freshness_full_check_us` and
`ready_freshness_cache_hit_count` distinguish full graph validation from a
positive worker-local hit; `ready_freshness_us` remains inclusive.
`ready_check_count` distinguishes the warm one-check path from an on-demand
compile and recheck; repeated snapshot reads retain the initial values.
Read-only HTTP entry checks may reuse a positive result for at most ten seconds
when the source generation and exact Wasm, metadata, and setup-template
identities are unchanged. CLI and mutation requests always validate the complete
graph. Misses, expiry, missing tokens, and changed identities also run the full
check. Exact repeated load paths are then deduplicated before canonicalization,
while distinct aliases are resolved independently so symlink retargets remain
visible by the next hard validation.
When a current serialized module exists, the worker scans wasm section headers
through a bounded 4 KiB positional buffer and retains only `dylink.0`, `uce.abi`,
and the tiny `uce.module` identity. It skips code and data bodies rather than
issuing byte-at-a-time reads or faulting those bodies into every new worker.
The request profile reports the physical read-ahead bytes and positional read
count. The scanner checks the same non-renewable invocation deadline before
each section and before and after every buffer refill; expiry is returned as the
canonical `UCE_INVOCATION_TIMEOUT` error. One in-progress regular-file
`pread()` remains the irreducible synchronous boundary. Initial descriptor
validation matches device, inode, mode, nanosecond timestamps, and size from
the preceding lookup; final validation rejects in-place truncation or mutation.
Selected metadata sections must be unique and individually remain at most 1 MiB,
which also bounds aggregate retained metadata. Both streamed and full-artifact
LEB readers reject nonzero unused bits in the tenth `u64` byte. A
missing/stale/invalid serialized module
still reads the complete wasm artifact in deadline-checked 64 KiB positional
chunks, validates it, compiles it, and republishes the serialization. The full
read uses the same initial/final descriptor identity guard.
The proactive compiler also creates that serialization immediately after source
compilation, keeping first-worker native compilation off the request path.
Cold module compilation and deserialization are host work, so `load_unit()`
refreshes the epoch deadline before its first guest call. Otherwise a component
whose compilation outlasted the guest CPU budget would immediately trap in the
following allocator/relocation call even though no guest loop consumed it.

Unit artifacts live beneath an ABI-generation directory such as
`BIN_DIRECTORY/units-c13-w7`: `c13` is the compiler/unit-metadata ABI and `w7`
is the runtime core ABI. Old and new service binaries therefore never publish
or read the same unit path during an ABI transition. The managed restart builds
and serializes the complete next generation before stopping the old service;
failure aborts the switch and leaves the running generation intact. Old
generation directories are retained as an explicit rollback path. Two
low-priority precompile processes are used by default so independent units do
not serialize deployment time while live workers retain scheduler priority.
`PRECOMPILE_JOBS` or `UCE_PRECOMPILE_JOBS` may select 1–16 processes. Unit
publication and the shared generation PCH use separate advisory locks, and any
worker/reporting failure rejects the candidate generation.

The proactive compiler and request workers coordinate through per-unit file
locks and a lock-protected demand-priority queue under `BIN_DIRECTORY`. The
full-site scan uses two low-priority processes by default, bounded from 1–16 by
`PROACTIVE_COMPILE_JOBS`; a stable canonical-path partition gives every unit one
scanner owner and keeps retry/backoff state local. A separate higher-priority
compiler process exclusively drains the demand queue. This matters when the
scanners are already inside long transitive C++ compiles: a requested stale
component can rebuild immediately instead of waiting for an unrelated compile.
All compiler processes use the same per-unit lock, so concurrent demand and scan
discovery cannot publish duplicate artifacts. The priority worker is idle when
there is no demand and never scans the site on its own. Unit
compilation writes and validates a process-unique temporary wasm file, then
publishes it with an atomic rename. By default, requests do not execute stale
code: a missing or changed unit is rebuilt synchronously and a failed dynamic
build surfaces its bounded compiler diagnostic. `SERVE_LAST_KNOWN_GOOD=1`
opts ordinary read-only HTTP requests into using the last complete,
ABI-compatible artifact while requesting that stale unit at the head of the
compiler queue. The proactive compiler must be enabled for this mode. Failed
background builds preserve that artifact and its source map and serialized
module until a later successful atomic publication replaces them. Missing
source, incompatible ABI generations, CLI, WebSocket dispatch, and mutation
requests never use last-known-good code. A stale mutation returns `503 Service
Unavailable` with `Retry-After: 1`; CLI and explicit compile paths remain
synchronous. If a proactive compiler already owns a stale child's lock, a CLI
request joins that compile and waits for the current artifact.

Before preprocessing, the compiler verifies that the worker can actually read
the source. An unreadable path is reported as a source-read failure with a
persisted diagnostic; it never becomes an apparently valid empty side module or
a generic compiler error. Source signatures mark unreadable inputs, so correcting
access invalidates that failure and permits a normal retry without changing
signatures for ordinary readable files. The private CLI harness may label its
intentional unreadable-source regression request as an expected source-read
failure, keeping production log scans focused on real runtime failures while
preserving the failing compile result and diagnostic artifacts.

Unit linking retains DWARF only long enough for
`scripts/build_unit_source_map.py` to extract a compact address/file/line table.
The published `.wasm` is debug-stripped and the table is stored beside it as
`.wasm.source-map`, keyed to the exact temporary module identity recorded in
the wasm's `uce.module` custom section. Normal module loading never reads this
sidecar. On a Wasmtime trap, the worker uses structured frame module offsets to
load only the matching map and appends source locations to the error. A missing,
stale, or malformed map is deliberately non-fatal: the ordinary named wasm
backtrace remains available. Generated C++ uses a `#line` directive naming the
original `.uce` file, so application frames resolve to application source rather
than the generated cache file. Artifact invalidation removes the wasm, serialized
module, and source map together.

---

## 4. The workspace runtime

Each request gets a fresh **workspace** — a per-request wasm instance tree with
the membrane wired in. `wasm_worker_serve(worker, ctx, entry_unit, handler)`
(`src/wasm/worker.cpp`) is the single entry point for *every* mode:

1. Birth a request-scoped workspace with fresh per-request state. Current
   production workers create a fresh Store and instantiate the store-independent
   `InstancePre`; the resulting core instance owns a fresh exported, growable
   function table, memory, globals, constructors, and request tree. The worker
   retains only Wasmtime's compiled module, linker, pre-instantiation plan, and
   store-independent host-function definitions. Every callback resolves the one
   active request workspace at invocation. Legacy cores that import their table
   remain supported through the prior per-request import path. Any future
   snapshot/CoW optimization must preserve this request-isolation contract.
2. Resolve `entry_unit` + `handler` to an export; components referenced at
   runtime are resolved on demand via the `uce_host_component_resolve` hostcall
   (`component_resolve()` → `__uce_<...>` slot), loading dependency modules
   lazily and recording resolve counts/timings.
3. Invoke the export. Unit code calls host functions (filesystem, sqlite, regex,
   markdown, time, tasks, `ws_*`, …) across the membrane.
4. Collect the response (`WasmResponse`: body, headers, status, `meta`).

Because the workspace owns no process-lifetime state, a unit crash or trap is
contained: it fails the one request and the worker stays healthy. Workers
suspend the native SIGSEGV/SIGILL recovery handler around the wasm call so that
Wasmtime's own trap signals are not escalated into a native fatal signal (see
`serve_via_wasm` in `handle_complete`).

Render workers are long-lived. The generic FastCGI transport retains a legacy
eight-connection recycle default, but `listen_for_connections()` disables it
for the UCE pool: recycling a healthy worker discards its Wasmtime engine and
module cache, adding a visible cold-start request. Request state remains bounded
by the fresh workspace and the normal per-request database/resource cleanup;
faulted workers still exit and are replaced by the parent.

A warm entry artifact is checked for compiler/source freshness once before
dispatch. A missing or stale artifact is compiled (or demand-prioritized) and
then checked again before execution. The second check belongs only to that
state-changing branch; repeating it immediately after a successful warm check
adds no freshness guarantee.

The internal request envelope carries application `context.call` as UCEB2,
each request `StringMap` as a flat UCEB2 map, scalar session/input metadata as
bounded byte segments, and optional WebSocket state as UCEB2. The guest
validates the complete envelope before moving those values into a fresh
`Request`; transport-only params, cookies, session data, and entry metadata no
longer become duplicate `context.call` children. The historical by-value
`DValue::operator=(DValue)` symbol remains exported so warmed side-module
artifacts stay ABI-compatible. `request_perf()` subdivides birth into policy,
import materialization, core instantiation, export/table lookup, and initialization,
and context transfer into bytes, host encode, guest allocation/write,
guest decode/application, and free. The byte profile separately reports the
worker-cached server-configuration portion.

### Task callbacks and workspace lifetime

`task()` and `task_repeat()` are fork-backed. The `uce_host_task_spawn` hostcall
captures the current `WasmWorkspace*`, but `src/lib/sys.cpp::task()` invokes the
captured callback only in the forked child, before the hostcall stack unwinds in
that child. The parent request may return and destroy its workspace; the child
is forked from the parent and keeps a private request-context copy of the per-request workspace. This means a delayed task callback can run after the spawning request
returns without dereferencing the parent's destroyed workspace. It is still a
callback into the inherited child workspace, not a fresh normal request
workspace; avoid adding host resources to `WasmWorkspace` that are invalid across
`fork()` unless task callback handling is changed to birth a fresh workspace.

---

## 5. Request dispatch (`handle_complete`)

`handle_complete()` (`src/linux_fastcgi.cpp`) is the worker's single dispatch
point. It selects a handler string from request params and calls the same
`serve_via_wasm(entry_unit, handler)` lambda for all of them:

```
UCE_WS == "1"            → serve_via_wasm(entry_unit, "websocket")
request.resources.is_cli → serve_via_wasm(entry_unit, "cli")
UCE_SERVE_HTTP == "1"    → serve_via_wasm(entry_unit, "serve_http"[:fn])
otherwise (page)         → serve_via_wasm(entry_unit, "render")
```

The `UCE_*` params are set by whichever broker forwarded the request:

- **Page render**: FastCGI nginx → `FCGI_SOCKET_PATH` directly; no `UCE_*` flags → `render`.
- **CLI**: the CLI socket sets `is_cli`.
- **serve_http**: the custom-server dispatcher sets `UCE_SERVE_HTTP=1` plus
  `UCE_SERVE_HTTP_FUNCTION` and rewrites `SCRIPT_FILENAME` to the configured
  unit (`custom_server_http_complete`).
- **WebSocket**: the WS broker sets `UCE_WS=1` and carries the connection
  identity as `UCE_WS_*` params (see §6). `handle_complete` rebuilds
  `request.resources.websocket_*` and `request.connection` from them before
  invoking `__uce_websocket`.

If a unit still cannot be served after the on-demand wasm compile, the worker
returns a clean 500 with the wasm/compile error; it does not execute native unit
code.

---

## 6. The central WebSocket broker

**One process owns the HTTP port
and every WebSocket connection**, so any unit's `ws_*` call can reach any or all
connections, and a unit-code crash (which happens in a worker) never drops live
connections.

### 6.1 Inbound: a WS frame → a worker render (non-blocking)

`ws_broker_ws_message(request, message, opcode)` fires when a complete
(reassembled) WS message arrives on a connection. It does **not** block the
broker loop:

1. Build FastCGI params: `SCRIPT_FILENAME`, `REQUEST_METHOD=GET`, a
   `REQUEST_URI` (required — `handle_request()` rejects requests without one
   *before* `on_complete` runs), `UCE_WS=1`, and the connection context as
   `UCE_WS_CONNECTION_ID / SCOPE / OPCODE / BINARY / CONNECTIONS / STATE`.
2. The message rides as `UCE_WS_MESSAGE` (base64) with an **empty STDIN body** —
   a non-empty STDIN makes the FastCGI transport flush a premature response
   before `on_complete` ever runs.
3. Connect to `FCGI_SOCKET_PATH` (non-blocking) and queue the encoded request in
   `ws_broker_outbound[fd]` with an enqueue timestamp.

`ws_broker_drain_outbound()` runs after every `process(50)` tick: it finishes
writing each queued request, then drains and discards the reply (the unit's
output comes back via the command socket, not this reply), closing the fd when
the worker closes its end. If a forward remains pending beyond
`WS_BROKER_OUTBOUND_TIMEOUT_SECONDS` (default `30`), the broker drops and
closes it so a wedged worker cannot pin broker fds/memory indefinitely.

### 6.2 Outbound: `ws_*` commands flushed back to the broker

Any unit code — not just WebSocket handlers — may call `ws_send` / `ws_send_to`
/ `ws_close`. In the workspace these **record dispatch commands** rather than
touching a socket (the workspace owns no connections); `wasm-core`'s `ws_*`
(`src/lib/sys.cpp`) append to `websocket_dispatch_commands`, and
`finish_response_meta` (`src/wasm/core.cpp`) emits them as `ws_commands`.
If the handler changed per-connection state, the core also emits
`ws_connection_state` even when no commands were emitted, and the native
backend flushes this state-only batch to the broker.

`wasm_backend_serve` (`src/wasm/backend.cpp`) flushes that batch at workspace
teardown — in **any** scenario, not just WS handlers — to the broker's command
socket (`WS_BROKER_SOCKET_PATH`, `/run/uce/ws-broker.sock`) via
`fcgi_forward_request` with `UCE_WS_DISPATCH=1`. This is the only path WS data
takes out of a workspace.

`ws_broker_apply_commands()` decodes the batch and applies each command against
the full registry it owns: `broadcast` (by scope), `send_to` (by connection id),
`close`. If the batch carries `connection_state`, it persists that onto the
matching live connection's `websocket_state`.

### 6.3 Un-upgraded HTTP on the WS port

The WS port can also receive ordinary (non-Upgrade) HTTP requests.
`ws_broker_complete()` routes by param: `UCE_WS_DISPATCH=1` → apply commands;
otherwise → `forward_request_to_worker()` — the *same* shared facility the
serve_http dispatcher uses, so there is no duplicated request-forwarding code.

### 6.4 The broker loop

`run_ws_broker()` drops the worker listeners it inherited
(`close_inherited_server_sockets`), installs permissive `on_request`/`on_data`
handlers (it renders nothing, so it accepts every request straight through to
`on_complete`), wires `on_complete=ws_broker_complete` and
`on_websocket_message=ws_broker_ws_message`, listens on `HTTP_PORT` and the
command socket, and loops `process(50)` + `drain_outbound(timeout)`. The
`timeout` comes from `WS_BROKER_OUTBOUND_TIMEOUT_SECONDS` (default `30`). The
design is **non-blocking outbound dispatch + async command-socket flush, all in
the broker's single epoll loop** — the broker never blocks on a worker.

The parent respawns the broker if it dies (`ws_broker_alive` / `ensure_ws_broker`
in `main()`). 

---

## 7. The serve_http facility

`serve_http` units are reachable on their own bind address via a custom-server
dispatcher (`custom_server_http_dispatcher_loop`). The dispatcher owns the bind
socket and, on each request, sets `UCE_SERVE_HTTP=1` + the configured unit/
function and calls `forward_request_to_worker()`. The worker then runs
`serve_via_wasm(entry_unit, "serve_http"[:fn])`. This is the same
hold-connection-forward-render model as the WS broker, sharing
`forward_request_to_worker` and `fcgi_forward.h`.

---

## 8. Build / object layout

The native side is split into separately-compiled objects so editing the wasm
runtime does not recompile the whole TU (`scripts/build_linux.sh`):

| Object | Contents |
|---|---|
| `bin/sqlite3.o` | sqlite amalgamation (cached) |
| `bin/wasm.o` | `src/wasm/wasm_module.cpp` (backend.cpp + worker.cpp + wasmtime) |
| `bin/main.o` | `src/linux_fastcgi.cpp` (includes `lib/uce_lib.cpp`) |

Linked into one `-rdynamic` binary. ODR hazards across the objects are handled
deliberately: `context` and `my_pid`/`parent_pid` are `extern` (guarded for the
wasm core vs. unit builds), `operator new`/`delete` live in `types.cpp`, and
header free-functions are `inline`. The wasm backend exposes only declarations
(`src/wasm/backend.h`) to `main.o`.

---

## 9. Configuration keys

| Key | Default | Meaning |
|---|---|---|
| `WASM_BACKEND_VERBOSE` | `0` | Emit `X-UCE-Wasm-*` workspace timing headers (benchmark only). |
| `WASM_EPOCH_DEADLINE_TICKS` | `200` | Maximum uninterrupted guest CPU segment in epoch ticks; must be positive. |
| `WASM_EPOCH_PERIOD_MS` | `50` | Worker epoch-ticker period and timeout resolution; range `1`–`1000` ms. |
| `WASM_INVOCATION_TIMEOUT_MS` | `30000` | Absolute app-owned unit load/init/handler/nested-call deadline; range `1`–`86400000` ms and nested calls cannot renew it. |
| `FCGI_SOCKET_PATH` | runtime-configured (`/run/uce/fastcgi.sock` in this doc) | Worker pool FastCGI socket (brokers forward here). |
| `CLI_SOCKET_PATH` | `/run/uce/cli.sock` | Worker CLI/admin socket. Keep private; reference `CLI_SOCKET_MODE` is `0600`. |
| `FCGI_SOCKET_MODE` | `0666` | Permission mode applied to `FCGI_SOCKET_PATH` after bind; set tighter if nginx/Apache can use a trusted group. |
| `CLI_SOCKET_MODE` | `0600` | Permission mode applied to `CLI_SOCKET_PATH`; set `0660` only for a trusted admin group. |
| `HTTP_PORT` | `8080` | Raw HTTP + WebSocket port — owned by the WS broker. |
| `WS_BROKER_SOCKET_PATH` | `/run/uce/ws-broker.sock` | Broker command socket for `ws_*` flushes. |
| `WS_BROKER_OUTBOUND_TIMEOUT_SECONDS` | `30` | Max lifetime in seconds for queued WS broker forwards before drop. |
| `WORKER_COUNT` | `4` | Number of uniform worker processes. |

---

## 10. Testing

- **Regression gate**: `scripts/run_cli_tests.sh --include-wasm-kill` runs the
  in-runtime CLI suite (`site/tests/cli_runner.uce`) plus the site test pages and
  `scripts/test_dependency_invalidation.sh`. The shell helper runs the CLI suite
  as named groups (`demo`, `http`, `site`, `doc-gate`, `security`,
  `task-lifetime`, `starter`, `tcp`, and optional `wasm-kill`) so a slow or
  failing group is visible before the rest of the gate runs. `doc-gate` can take
  several minutes on a cold runtime because it compiles generated documentation
  examples; `UCE_CLI_TEST_TIMEOUT` controls the per-group curl timeout and
  defaults to 900 seconds. Individual doc-page checks retry transient empty
  frontend reads, but still fail persistent status, marker, compile, runtime, or
  placeholder-example errors. The dependency-invalidation gate changes a transitive
  `#load`, then replaces a warmed worker artifact while preserving its
  whole-second mtime to prove both compiler and worker caches invalidate it. It
  also breaks a loaded dependency, verifies the parent compile fails, restores
  the dependency byte-for-byte, and requires the parent to recover. Failed
  builds persist the input signature that actually failed, so metadata from an
  older successful artifact cannot indefinitely defer that rebuild.
  The gate also rejects an unreadable unit without publishing a wasm artifact, restores
  its permissions and proves the next CLI request compiles it, then sends 48
  requests and asserts the observed worker PID set does not exceed
  `WORKER_COUNT`, guarding against accidental reintroduction of request-count
  recycling. It also races a source edit against an active unit compile after
  the generated C++ snapshot has been written. The compiler must either retry
  and serve the post-edit source or fail closed; it must never stamp current
  source metadata onto wasm produced from an older snapshot.
- **Metadata scanner deadline**: an isolated one-worker gate uses a targeted
  positional-read interposer to prove canonical expiry after one delayed 4 KiB
  metadata read and after one delayed cache-miss 64 KiB full-artifact read,
  concurrent truncation and same-size mutation rejection, malformed tenth-byte
  LEB rejection, duplicate selected-section rejection, and same-worker recovery.
- **Core compatibility**: the production core owns and exports its growable
  function table, enabling `InstancePre`. A separately built legacy
  `--import-table` core must still pass the demo and 64-request pool-isolation
  groups before the compatibility fallback changes.
  `scripts/test_wasm_compile_timeout.sh` runs an isolated one-worker runtime
  with a two-second request budget. It covers cold entry JIT, guest
  `unit_compile()`, dynamic component and transitive `#load` compilation,
  held unit and registry locks, silent nonzero and missing-output compiler
  results, compiler descendants, staged-output timeout, prior-generation
  hashes, same-worker recovery, configured-error-page boundedness, residue,
  foreign-owned offline-precompile artifacts, and deadline-independent offline
  precompile. Normal rollback snapshots are hard links. If Linux ownership or
  link policy rejects that fast path, the compiler copies the prior artifact
  under the same unit lock and keeps the same all-or-nothing publication.
  Source-generation markers likewise publish through a same-directory rename,
  so a runtime user can replace a readable marker created by an administrator.
  Existing foreign-owned unit, registry, generation, and PCH lock files are
  opened read-only for `flock`; new locks are still created read-write.
  `scripts/test_cold_component_deadline.sh` separately compiles a deliberately
  cold component that exceeds the development epoch window and proves the
  parent request still renders it. The focused shell gates create temporary
  `.uce` units under `UCE_TEST_SITE_DIRECTORY`, or under the installed
  `SITE_DIRECTORY` from `/etc/uce/settings.cfg` when no override is supplied.
  The dependency-invalidation gate also holds parent and child compile locks
  across a transitive source edit, proves a warmed HTTP request returns the
  last atomically published result without waiting, proves a POST returns a
  prompt retryable 503 without executing its old mutation handler, and verifies
  demand-priority convergence. A CLI request still waits and returns only the
  current dependency result after rebuild completion.
  The component page also calls a parent once without its optional relative
  child and again with that child activated. This guards the component-slot
  cache invariant that a cache hit must restore both the function-table slot
  and the resolved unit path; otherwise nested relative targets resolve from
  the caller after a repeated parent invocation. Resolved component paths are
  canonical absolute paths: equivalent spellings containing `.` or `..` must
  share one source, compile, artifact, and module-cache identity. The compiler
  independently enforces the same canonical identity. Within one resolution,
  identical entry/site bases and identical raw/`.uce` candidate spellings are
  probed only once while first-match order is preserved. Persisted failures are
  reused only when their recorded source path matches the current request.
  `scripts/test_nested_component_props.sh` passes a large prop tree through an
  outer component, invokes 300 nested components, and verifies both inner and
  caller props are restored. Its warm-request ceiling guards the request-scope
  invariant that entering a nested component swaps the active prop trees in
  constant time instead of deep-copying the outer tree for every child.
  `scripts/test_mysql_epoch_refresh.sh` spends more than half of the configured
  guest epoch on each side of a real MySQL query. It proves the blocking
  database hostcall refreshes the guest deadline so cumulative request work is
  allowed while a CPU loop without intervening host work still traps.

- **Log timeliness**: the base process line-buffers stdout before forking
  workers, the proactive compiler, and the WebSocket broker. This keeps each
  newline-terminated diagnostic attached to its actual journal time instead of
  releasing an old block-buffered failure during unrelated later traffic. The
  private CLI timing gate labels its deliberate invalid-source request as
  `UCE expected compile error`; public and unmarked CLI compilation failures
  retain the ordinary `UCE compile error` operator signal.

- **WebSocket end-to-end**: a headless client performs a raw WS handshake to
  `:HTTP_PORT` with path `/site/tests/websockets.ws.uce` (self-resolving
  `SCRIPT_FILENAME`) and asserts the `hello-ack` frame — exercising the full
  broker → worker → broker → client chain across process boundaries.
