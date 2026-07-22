// BEARER runtime amalgamation include.
//
// The worker and generated units include this file to build the runtime in a
// single translation unit. Do not compile the listed .cpp files separately
// unless the build/compiler model is deliberately changed.

#include "types.cpp"
#include "dvalue.cpp"
#include "functionlib.cpp"
#include "hash.cpp"
#include "sys.cpp"
#include "uri.cpp"
#include "cli.cpp"

#ifdef __BEARER_WASM_CORE__
// markdown is pure compute (no PCRE/syscalls/regex) — it belongs in the wasm
// core so markdown_to_html/markdown_to_ast render in-workspace. compiler.cpp
// (which declares component() ahead of markdown in the native build) is carved
// out here, and the wasm core defines component() later in src/wasm/core.cpp,
// so markdown just needs the forward declaration.
String component(String name, DValue props, Request& context);
#include "markdown.cpp"
#endif

#ifndef __BEARER_WASM_CORE__
#include "../capy/frontend.cpp"
#include "../capy/wasm.cpp"
#include "../capy/compiler.cpp"
#include "compiler-parser.cpp"
#include "compiler.cpp"
#include "markdown.cpp"
#include "zip.cpp"
#include "mysql-connector.cpp"
#include "sqlite-connector.cpp"
#endif
