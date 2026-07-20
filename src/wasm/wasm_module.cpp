// Translation unit for the wasm backend object (wasm.o).
//
// Pulls in the runtime *declarations* (bearer_lib.h) and then the backend
// definitions, so the heavy wasmtime.hh + worker.cpp only recompile when the
// wasm sources change — not on every native build. The symbols it references
// (String/DValue/config/connectors/unit_*/socket_*) are defined in the core
// (main) object and resolved at link.
#include "../lib/bearer_lib.h"
#include "backend.cpp"
