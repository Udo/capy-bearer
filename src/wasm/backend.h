#pragma once

// Public surface of the wasm backend object (src/wasm/backend.cpp → wasm.o).
//
// The native main object (linux_fastcgi.cpp) includes this header — not
// backend.cpp — so it does not have to compile worker.cpp + wasmtime.hh on
// every build. Include only after bearer_lib.h (needs Request / String).

// True if this request can be served by the wasm backend for the named unit
// artifact; handler-agnostic, the caller names the handler.
bool wasm_backend_should_handle(Request& request, const String& entry_unit);
u64 wasm_backend_invocation_timeout_ms(Request& request);

// Initialize the process-local engine after fork and before the worker accepts
// requests. Returns "" on success or the retained backend initialization error.
String wasm_backend_start(Request& request);

// Proactive compiler hooks for keeping Wasmtime's native serialization newer
// than a compiled unit artifact before any request worker loads it.
bool wasm_serialized_module_needs_refresh(const String& wasm_path);
String wasm_serialize_module_artifact(const String& wasm_path);

// Serve a request through a wasm workspace by invoking a named unit handler —
// "render", "cli", "websocket", "serve_http", "serve_http:named" — and populate
// the native Request. Returns "" on success or a collapsed error. The handler is
// just an export name; there is no per-mode machinery.
String wasm_backend_serve(Request& request, const String& entry_unit, const String& handler = "render", u64 timeout_cap_ms = UINT64_MAX);

// Join the per-process epoch ticker before the worker process exits.
void wasm_backend_shutdown();
