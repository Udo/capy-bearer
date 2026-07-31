# Capy language and compiler

Capy is a statically compiled language that emits Bearer-compatible WebAssembly side modules directly. It does not transpile through C or C++. Capy and C++ units communicate only through Bearer’s unit/component/hostcall membrane.

> Engine note: Bearer native/Wasm carries the bounded UCE-compatible `crypto_operation` CBOR/COSE ES256 primitive, but it has no Capy compiler/stdlib binding. It is intentionally not a Capy capability yet; the parity manifest remains unchanged until that separate surface exists.

## Declaration grammar

Capy declarations are expression-based. Spaces, tabs, and newlines are ordinary whitespace; grammar and delimiters end expressions. Semicolons remain optional explicit separators when adjacent expressions would otherwise be ambiguous. A function declaration consists of:

```text
function name [parameter expression] [return type expression] code block expression
```

The expressions are optional and the opening `{` of the code block terminates the header. Parentheses are ordinary grouping/tuple syntax; they are not special arrow-function punctuation.

```capy
function hello_world {
    print("hello")
}

function add(x : s32, y : s32) s32 {
    return x + y
}

function pair() (s32, s32) {
    return 10, 20
}
```

A parenthesized header expression occupies one declaration slot, so `function pair() (s32, s32) {}` is deterministically an empty parameter expression followed by a tuple return-type expression, not a call expression.

Function overload identity is the function name plus normalized parameter type patterns. Return types never distinguish overloads and expected result types never select an overload. A parameter written `name : as Type` opts into one call-site `Type(value)` constructor for each nonmatching argument. The function body sees `name` as `Type`. A final `...name : Type` parameter gathers trailing arguments into `[Type]`.

## Compile-time constants

A top-level `const name : s32 = literal` substitutes its typed `s32` literal at every use. It emits no Wasm function, global, data, or import. Constants cannot duplicate or conflict with a top-level function/struct name; ordinary local bindings still shadow them. This intentionally small feature currently accepts only `s32` literals, not constexpr expressions or non-scalar constants.

```capy
const retries : s32 = 3
function CLI { print(retries) }
```

## Locals and operators

`var name := value` is the ordinary inferred local declaration; `var name : T = value` is its typed form. Both are value-producing expressions, as are `name := value` (inferred declaration) and `name = value` (assignment). A declaration evaluates its initializer once, installs the local, and yields that local's type/value; redeclaration in that scope is an error. Managed declaration results are borrowed aliases of the local, so only the local owns and releases the initializer. Declarations occur in the current lexical scope: a declaration in an `if` condition remains available after that `if`, consistent with ordinary expression lowering. `&&` and `||` remain the exception: their conditional right side is a nested lexical scope, so declarations there do not escape it.

`&&` and `||` require `bool` operands and short-circuit their right side. Unary `!` requires `bool`; unary `-` accepts `s32`, `s64`, and `f64`. Unsuffixed integers remain `s32` and are limited to `-2147483648` through `2147483647`; append `s64` for signed 64-bit literals or `u64` for unsigned literals through `18446744073709551615u64`. Decimal-point and exponent literals are `f64`. Arithmetic and comparisons require equal types; `s64` division/remainder/order are signed, `u64` operations are unsigned, and `f64` follows Wasm IEEE-754 with no remainder operator.

Type names are constructor overload sets. Built-in scalar constructors convert among `s32`, `s64`, `u64`, `f64`, and `bool`, and built-in `string(value)` constructors format those values. `string(bool)` produces `true` or `false`, including when `print` constructs a Boolean argument. User functions can add concrete or compile-time `any` constructor overloads. Integer narrowing wraps; negative `s32` to `u64` sign-extends modulo 2^64; `u64` to `f64` can round above 2^53; and out-of-range or non-finite `f64` to integer construction traps at the constructor call. The former `value as Type` operator is rejected. `time()` returns `u64` and `time_precise()` returns `f64` through Bearer's existing clock policy. Wide scalars work in locals, parameters, results, direct calls, function values, and arrays. Tuple, struct, and captured-closure layouts still reject wide scalar fields.

## Compile-time polymorphism

`any` is a compile-time type placeholder, not an erased runtime value. The compiler binds each `any` parameter to its concrete static argument type, type-checks the specialization, and caches it by function plus concrete parameter types.

```capy
function identity(x : any) x::type {
    return x
}
```

`x::type` is a compile-time dependent type expression. A specialization fails at its callsite if its body requires an operator or conversion unavailable for the bound type. There is no runtime `any` tag or dispatch.

Overload ranking is deterministic:

1. concrete exact parameter matches;
2. `any` specializations without construction;
3. fixed-arity candidates using declared `as` construction, ranked by the number of constructed arguments;
4. variadic candidates, ranked by fixed prefix length and constructed argument count;
5. equal best candidates are ambiguous.

A selected `any` constructor is concretized for the source type. Unsupported operations in its body fail at the call site. Resolution does not backtrack after a selected specialization fails.

## Tuples and returns

Comma expressions form tuples. Parentheses group expressions, and therefore also delimit tuple type expressions where needed.

```capy
return 10, 20
return (10, 20)
var pair := (10, 20)
```

`()` is the empty tuple/unit value and `(x)` is grouping. Calls distinguish two arguments from one tuple argument: `f(a, b)` versus `f((a, b))`.

## Strings

Strings support byte-preserving concatenation with `+`, byte equality with `==`/`!=`, `length(value)` for strings, markup, and arrays, and C++-compatible `substr(string, start)` / `substr(string, start, length)` with `s32` and `s64` bounds. Negative substring starts count from the end; a negative length excludes bytes from the end. `strpos(value, needle[, offset])` returns an `s64` byte offset or `-1` and accepts negative offsets; `find(value, needle)` returns a byte offset or `-1`; `contains(value, needle)` returns whether that offset exists and treats an empty needle as present; `replace(value, from, to)` replaces all non-overlapping matches; `lower(value)` and `upper(value)` use Bearer's established byte-oriented case conversion. String-producing operations return ARC-managed strings. `split(value, delimiter)` returns an owned copied DValue list and preserves empty fields; an empty delimiter returns the original value as one item. `join(list[, delimiter])` accepts a DValue list containing only strings, defaults to a newline delimiter, and returns an owned string. Bearer normalizes a DValue map with exactly contiguous canonical numeric keys into list shape, so such a value is accepted as the equivalent list. This list membrane avoids exposing C++ `StringList` layouts. `first(strings...)` uses Capy's deterministic left-to-right argument evaluation (native C++ call-argument order is intentionally not reproduced) and returns an owned copy of the original first value whose byte-oriented Bearer `trim` result is non-empty; it preserves the selected value's surrounding whitespace and returns an owned empty string when none qualifies.

## Markup values

JSX/UCE fragment delimiters form a value-producing markup expression:

```capy
var title := clone("<Capy & Bearer>")
var page := <><h1><?= title ?></h1></>
print(page)
```

`<?= expression ?>` HTML-escapes strings using the same five replacements as UCE (`&`, `<`, `>`, `"`, and `'`). `s32`, `s64`, `u64`, `f64`, and `bool` interpolate as locale-independent text. A nested `markup` value composes without double escaping. `<?: expression ?>` is the explicit raw-composition form and requires `markup`; `trusted_markup(string)` is the deliberately named unsafe conversion for externally established trusted HTML.

Markup expressions evaluate every field exactly once. Static markup is emitted as raw immortal bytes. Dynamic markup evaluates fields into locals, computes the exact escaped byte length, performs one workspace allocation, writes one ARC-managed value, and can be printed through one `bearer_print_bytes` call. Managed field temporaries are released after copying, and ordinary ARC return/assignment/cleanup rules apply to the resulting `markup` value.

The `<>...</>` boundary is both JSX fragment syntax and the existing UCE markup boundary. It keeps markup starts unambiguous with ordinary `<` comparisons and permits nested fragment delimiters. In literal markup, `\<>` and `\</>` emit the delimiter text without opening or closing a fragment; this is useful in scripts and documentation.

## Request and response context

`request_context()` returns an ARC-managed copied `dval` snapshot of the ambient current request. A handler that declares its reserved opaque request handle can instead call `request_context(request)`; this routes access through that handle without exposing the C++ `Request` layout. The zero-argument form exists for concise handlers and both forms copy the same request-local state. It contains `params`, `get`, `post`, `cookies`, `session`, `call`, `cfg`, `props`, `connection`, `input`, `session_id`, `session_name`, and `current_unit`. Server configuration is deliberately not copied into the snapshot; capabilities that need configuration receive narrow typed adapters rather than the complete operational settings map. The snapshot is a read-only copy; indexing and iteration follow ordinary strict `dval` rules. Its `cfg` member is the app-owned `Request::cfg` value and is distinct from Bearer's operational `ServerState::config`, which is not exposed.

Common scalar reads should use `request_param(key)`, `request_get(key)`, `request_post(key)`, `request_cookie(key)`, `request_session(key)`, and `request_body()`. They copy only the requested bytes and return an owned string (empty when a map key is absent), avoiding full BRRB snapshot encoding. Use `request_context` when structured maps or props are actually needed.

`response_status(code)` updates the current response status. `response_header(name, value)` sets a validated header and removes CR/LF from its value; an invalid status or header name traps at the callsite. `response_cookie(name, value)` emits a safe HttpOnly/Lax cookie. `redirect(url, status)` sets a validated 3xx status and Location header. These operations mutate only the current request workspace.

`session_start(name)`, `session_set(key, value)`, `session_remove(key)`, and `session_destroy(name)` use Bearer's existing session storage and cookie policy. `csrf_token(session, key)`, `csrf_valid(submitted, session, key)`, and `csrf_rotate(session, key)` use the same session-backed CSRF implementation as `.uce`. The current bindings require explicit arguments rather than C++ default arguments.

```capy
function RENDER {
    response_status(201)
    response_header("Content-Type", "text/plain; charset=utf-8")
    var request := request_context()
    print(dval_string(request["get"]["name"]))
}
```

## Handler lifecycle

`CLI`, `RENDER`, `COMPONENT` (including named handlers), `WS`, `SERVE_HTTP`, and `TASK` execute through the same Bearer selection and request-workspace path as `.uce`. A task handler declares `function TASK(request : request)` or `function TASK:NAME(request : request)` and reads the caller's sole copied DValue from `request_context(request)["props"]`. Dedicated task workers always create a fresh request and do not inherit caller request, session, connection, workspace, or resource state. Runtime acceptance covers direct CLI/HTTP, nested components, RFC 6455 text/binary dispatch, cross-language default/named tasks, and a C++-started HTTP service targeting a Capy `SERVE_HTTP` handler. `ONCE` and `INIT` exports exist, but their ordering/dedup and automatic INIT contract remain unresolved rather than receiving Capy-only behavior.

## WebSockets

A `WS` handler can inspect `ws_message()`, `ws_connection_id()`, `ws_scope()`, `ws_opcode()`, and `ws_is_binary()`. It can enqueue `ws_send(message, binary)`, `ws_send_to(connection, message, binary)`, and `ws_close(connection)` commands through Bearer's existing broker and request-isolated dispatch list. Send/close calls return `bool`.

## Bearer unit ABI

A Capy source uses `.capy`; C++/template units retain `.uce`. Both compile into the same request-local Bearer workspace and export the same handler names:

```text
CLI       -> __bearer_cli
RENDER    -> __bearer_render
WS        -> __bearer_websocket
ONCE      -> __bearer_once
INIT      -> __bearer_init
TASK      -> __bearer_task
TASK:NAME -> __bearer_task_NAME
```

The first direct-Wasm backend emits:

- PIC `dylink.0` memory metadata;
- imports of `env.memory`, `env.__memory_base`, and the stable `env.bearer_print_bytes` byte-span output function;
- `bearer.abi` and `bearer.module` custom sections;
- a matching `BEARER_SOURCE_MAP_V1` sidecar;
- no WASI imports; dynamic values use Bearer’s workspace allocator.

Compiler generation c31 uses core ABI w24. Artifact staging, freshness metadata, native serialization, bounded diagnostics, and last-known-good policy remain owned by Bearer’s existing compiler coordinator. Frontend, typed lowering, and CLI code are separate files, and all participate in artifact freshness signatures.

## Automatic reference counting

Dynamic Capy values use non-atomic, workspace-local automatic reference counting. Bearer workspace destruction is the final reclamation boundary, including after traps. Managed strings, vector arrays, and nominal structs are implemented; managed tuples, closures, and weak handles reuse the same header and generated drop-glue contract. Arrays store a length and capacity, grow geometrically, and use copy-on-write before mutation. Typed empty literals, indexed assignment, `push`, `pop`, `insert`, `remove`, `clear`, `reserve`, `resize`, and `capacity` preserve logical value copies. Array and call spreads use prefix `...`.

Managed object header, 16 bytes and 8-byte aligned:

```text
u32 strong_count
u32 weak_count
u32 type_descriptor
u32 size_and_flags
```

Type-specific payload follows the header. A string begins with `u32 length` at byte 16 and UTF-8 bytes at byte 20; that payload prefix is not part of the common 16-byte header. `dylink.0` declares an 8-byte module-memory alignment so static immortal objects retain this alignment after Bearer relocates the side module.

Rules:

- newly allocated objects own one strong reference and one implicit weak control reference;
- managed parameters are borrowed;
- managed returns transfer one owned reference;
- storing, copying, or capturing a borrow retains it;
- assignment retains the replacement before releasing the old value;
- generated cleanup blocks release owned locals on return, break, continue, and ordinary scope exit;
- strings, vector arrays, managed structs/tuples, and closure environments provide type-specific drop glue;
- static string literals are immutable immortal objects;
- function table slots belong to the workspace; closures retain only their environments;
- traps skip cleanup safely because the complete workspace is discarded;
- ARC does not collect strong cycles. `weak<T>` will support deliberate back-references; remaining strong cycles persist only until workspace teardown.

Current managed-value lowering imports Bearer’s workspace allocator/free functions and emits private retain, release, clone, and type-directed drop helpers into each Capy module. Allocation failure traps before any header or payload write. Literal strings are aligned immortal objects. Managed parameters are borrowed and cannot be rebound; managed results are owned; assignments retain-before-release; owned argument temporaries are released after calls; and every supported normal lexical/early-return edge emits cleanup. Arrays use `[T]` in type slots, bounds-check indexing, support array iteration, and currently accept scalar or string elements. Nominal structs use declaration-order constructors and checked member names. String-array and struct drop glue recursively releases managed fields before freeing the aggregate. `arc_live()` is a temporary conformance counter; trapping requests intentionally skip releases and prove that the next workspace starts clean.

Capy values never expose their object layout to C++. Dynamic cross-language values use owned/copied DValue/BRRB adapters at the Bearer membrane.

## Unit and component composition

`component_exists(target)` checks Bearer resolution and `component_resolve(target)` returns the resolved source path. `unit_render(target)` renders another unit into the current output. `component_render(target)` uses the current component props; `component_render(target, props)` accepts an explicit copied `dval` map, preserving Bearer's nested props restoration and sibling isolation. `component_capture(target)` and `component_capture(target, props)` execute the component once into an owned string without leaking its bytes into the outer output. `unit_call(target, function, input)` performs a structured custom call and returns an owned `dval`. `unit_info(path)` returns copied runtime metadata, `units_list()` returns a copied dval list of known unit paths, and `unit_compile(path)` requests bounded compilation through Bearer's existing coordinator. `unit_info()` and `unit_compile()` also accept no path for the current unit. These calls cross Bearer adapters rather than sharing language object layouts.

## Codecs

`json_encode(dval)` and `json_decode(string)` cross copied BRRB and return an owned string or dval. `base64_encode`, `base64_decode`, `uri_encode`, `uri_decode`, and `html_escape` accept a string and return an owned string using Bearer's established codecs. Malformed base64 returns an empty string. The shared codec adapter stages each result once per expression and clears it on request reset.

## Regular expressions

`regex_match(pattern, subject[, flags])`, `regex_search(pattern, subject[, flags])`, `regex_search_all(pattern, subject[, flags])`, `regex_replace(pattern, replacement, subject[, flags])`, and `regex_split(pattern, subject[, flags])` use Bearer's existing host-side PCRE2 implementation. Search and split return copied owned DValues; replace returns an owned string. Supported flags and match-tree shapes are identical to the documented `.uce` APIs. Invalid patterns, flags, and substitutions trap at the Capy call site, and staged results are cleared on request reset.

## Standard-library boundary

Bearer loads public APIs from the embedded `capy://stdlib.capy` source on demand, including request/response, WebSocket, component/unit, regex/codec, file/time, session/CSRF/redirect, string, and DValue convenience APIs. Public standard-library function names are reserved: user declarations using one fail clearly. The embedded source alone declares private typed `__bearer_*` host bindings; user source may neither declare nor call those implementation names. Selection follows lexical local shadowing, selects typed function-value overloads, and closes library dependencies to a fixed point; only the selected combined declarations are validated. Virtual stdlib frames are suppressed from runtime trap output so the user call site remains primary.

The compiler lowers embedded typed host declarations through one generic ABI path: scalars pass directly, strings/DValues are spans, and string/DValue results use the shared two-pass sized-result convention. The exact retained language primitives are scalar/type construction, `dval` construction, `dval_has`, `dval_string`, `dval_s32`, `dval_f64`, `dval_bool`, DValue indexing, vector-array operations, `length`, `trusted_markup`, `clone`, `trap`, and `arc_live`. Public `print(...values : as string)` is an ordinary selected stdlib declaration over the private typed byte-output host primitive. All other Bearer-facing public convenience names are ordinary stdlib declarations. Their implementations call private typed host declarations; private names are not language APIs.

## Parsed-source cache

`ParsedSourceCache` is an explicit process-local object supplied through `CompileOptions`; there is no compiler-global cache. A caller enables user-source reuse only by supplying a stable canonical identity. Cache acquisition is compiler-internal, so callers cannot obtain a shared AST with mutable `Expr*` nodes. Entries use structured raw-byte digest, canonical identity, diagnostic identity, parser/compiler identity, and ABI fields; exact source bytes are compared after a digest match. Diagnostic identities are deliberately part of the key, so location-bearing ASTs for different displayed paths never alias. The runtime coordinator owns one lazily created post-fork cache per `ServerState`; `capyc` owns one for its process; tests may create bounded instances directly.

The implicit library is acquired through that same implementation path as pinned `capy://stdlib.capy`, rather than through a static parsed AST. User entries default to 128 entries, an 8 MiB conservative source-byte admission/charge budget, and a 1 MiB maximum source; the charge adds source bytes, key metadata, and parsed-node count, not exact AST heap accounting. The entry and source limits bound cached input/count. Pinned stdlib is excluded from user bounds and reported separately by cache statistics. LRU eviction releases metadata under the cache mutex and destroys displaced AST ownership after unlock. PID mismatch after fork replaces inherited state before taking its mutex. Parsing/lowering never runs under that mutex; duplicate concurrent misses are valid. Cancellation is polled while hashing and immediately before every successful cache return, parsing, and lowering. Only complete successful parses publish; parse errors and cancellations never enter the cache, while later type/lowering errors may reuse their successful parse. Recompilation still always validates/lowers/publishes artifacts; this cache skips parsing only.

## Databases

`sqlite_connect(path)` returns an exact workspace-local `u64` capability handle. `sqlite_query(handle, sql[, params])` returns copied row DValues and accepts an optional string-valued DValue parameter map; `sqlite_error`, `sqlite_insert_id`, `sqlite_affected_rows`, and `sqlite_disconnect` preserve Bearer's SQLite policy. Handles cannot cross workspaces, and stale or explicitly closed handles trap at the Capy call site. Query results and errors are staged once; SQLite connections remain host-owned and are reclaimed at workspace teardown.

## Files and resource handles

File descriptors remain exact `u64` Wasm values and never pass through floating-point DValues. `file_open(path, mode)`, `file_read(handle, length)`, `file_write(handle, data)`, `file_seek(handle, offset, whence)`, `file_tell(handle)`, `file_fsync(handle)`, and `file_close(handle)` reuse Bearer's existing workspace handle table and path policy. `file_temp(prefix)` creates a policy-approved temporary path and `file_unlink(path)` removes it. Reads and temporary-path creation use execute-once staged copying, so sizing neither advances the file twice nor creates an unreturned temp file; staged bytes are cleared on request reset.

## Structured DValues

`dval(...)` accepts strings, `s32`, `u64`, `f64`, `bool`, nested map literals, list literals, and existing DValues:

```capy
var profile := dval({
    "name": "Ada",
    "age": 42,
    "active": true,
    "tags": ["math", "logic"]
})
```

String and integer indexing return copied `dval` children. Strict indexing traps on a missing key, invalid index, malformed value, or scalar container; `dval_has(value, key)` is the explicit non-trapping absence check. `dval_string`, `dval_s32`, and `dval_bool` require the matching BRRB scalar type. `dval_f64` accepts a BRRB float or a complete locale-independent finite numeric string because Bearer's JSON decoder deliberately preserves JSON numbers as strings; malformed and non-finite strings trap at the extraction call.

```capy
if dval_has(profile, "name") {
    print(dval_string(profile["name"]))
}
for key, value = profile["tags"] {
    print(key, "=", dval_string(value))
}
```

Maps iterate in lexical key order and lists in numeric order. Every key/value crossing into Capy is copied into an ARC-managed object; no borrowed C++ tree pointer is exposed. `StringMap` values cross as ordinary copied string-key/string-value DValues, including `parse_uri(uri)`, which returns `{parts: {...}, query: {...}}` with the documented native URI parsing semantics. `array_merge(left, right)` applies Bearer's copied DValue merge policy: string keys from the right overwrite, list-shaped numeric keys append and reindex, a non-map left yields the right value, and a non-map right leaves a map left unchanged. Reference nodes cross as copied dereferenced values; native alias/reference identity is not preserved.

DValue value APIs use explicit value semantics: `dval_set`/`dval_assign`, `dval_push`, `dval_remove`, `dval_clear`, `dval_set_array`, and `dval_set_bool` return the replacement copied `dval`, so callers reassign (`value = dval_push(value, child)`). This intentionally differs from C++ in-place mutation because Capy cannot expose a host-tree alias. `dval_pop` likewise returns the post-pop value; the removed-child identity result and `is_reference`, `reference_target`, `deref`, and `set_reference` are unsupported by design.

`dval_key`, `dval_keys`, and `dval_values` return copied DValues (a missing `dval_key` is an empty DValue). `dval_map(value, mapper)` and `dval_filter(value, predicate)` invoke ordinary Capy function values on copied children: maps retain keys, lists are reindexed, and scalar input is processed once into a list. The former StringList operations (`map`, `filter`, `unique`, `sort`, `some`, `every`, `string_list_find`, `keys`, and `each`) operate on that same list-shaped `dval`; they introduce no StringList layout or host import. `dval_to_s64` and `dval_to_u64` preserve native Wasm 64-bit results and fallbacks across the BRRB boundary; their conversion, fallback, and clamping behavior is Bearer's `DValue` behavior.

`EXPORTS` declares ordinary local `(dval) dval` functions as module-callable exports:

```capy
EXPORTS echo
function echo(input : dval) dval { dval({"echo": input}) }

var module := unit_load("child.capy")
var result := module.call("echo", dval({"message": "hello"}))
```

`unit_load()` returns an opaque request-local capability pinned to the selected workspace artifact. `module.call(name)` supplies an empty DValue and `module.call(name, input)` crosses copied BRRB values. Only names declared by `EXPORTS`, legacy `EXPORT_name`, or C++ `EXPORT DValue*(DValue*)` metadata are callable; module values cannot be stored, captured, converted, or forged. A module is first-class only during one request execution: it may occupy a local, be assigned, and pass through typed parameters, returns, and generic identity functions. It cannot serialize, convert, participate in arithmetic or conditions, enter an array/tuple/struct layout, be captured, or cross a workspace/request boundary. A capability is minted only after Bearer verifies the published metadata's Wasm and export-list SHA-256 hashes and the selected export's exact Wasm `(i32) -> i32` ABI. The sized result transport invokes the target once and clears the capability/staged result state when the request resets; nested resolution and relative file paths execute in the pinned target's source context.

`EXPORTS` is reserved as a top-level directive; using it in a block is rejected explicitly. A declaration named `EXPORT_name` with the same signature remains compatibility syntax. The generated wrapper converts the opaque core DValue pointer to copied BRRB2, invokes the Capy function once, converts the result back into a core-owned DValue, and releases its Capy temporaries. Existing `unit_call` handler aliases and C++ export semantics remain unchanged. Structured Capy→C++, C++→Capy, and Capy→Capy calls all use the same copied membrane and execute-once staging.

## Function types and closures

Fixed-signature function types use the same expression-slot declaration model:

```capy
var callback : function(value : s32) s32 = function(value : s32) s32 {
    return value + 1
}
```

When a function type is the return slot immediately before a declaration body, group it to keep the two braces unambiguous:

```capy
function make(base : s32) (function(value : s32) s32) {
    return function(value : s32) s32 { return base + value }
}
```

A function value is an ARC-compatible closure pointer. Its payload stores a private Wasm table slot followed by captured fields. Scalars copy into the environment; managed captures retain one reference and generated type-directed drop glue releases them. Parameters and captures are borrowed inside the closure body, while managed results remain owned. Returning a closure therefore safely extends captured parameter/local lifetimes, and closure reassignment uses the normal retain-before-release rule. Assignment targets are not implicit captures: a lambda may read captured outer values, but assigning an outer local from inside the lambda is currently rejected as an unknown local rather than silently mutating a copied capture.

Ordinary statically named calls remain direct Wasm calls. A private thunk is generated only when a named function is converted to a function value. Noncapturing lambdas use immortal closure records; capturing lambdas allocate one workspace-local environment. A table and element section are emitted only when the source actually forms a function value.

## Expression-level source maps

Capy's `BEARER_SOURCE_MAP_V1` sidecar records sorted absolute byte offsets in the final Wasm artifact. Function-entry markers provide a fallback, while array bounds, strict DValue access/extraction, explicit traps, and allocation checks carry their originating expression locations. Wasmtime trap offsets therefore resolve to the relevant Capy line and column rather than only the enclosing function declaration. C++ units continue using their independent DWARF-derived map path.

## Deferred features

`#compile`, `#callsite`, and `emit` are reserved but deferred beyond phase 3. The parser emits a targeted diagnostic and never executes them. Their eventual implementation requires staged compilation, a compile-time workspace, ordering/hygiene rules, dependency tracking, bounded execution, and source provenance.

## Current implementation boundary

The current implementation includes the lexer, expression parser, bounded diagnostics, direct Wasm encoding, `.capy` artifact integration, and real Bearer CLI/HTTP execution. Scalar control flow—including ARC-safe `break` and `continue` through nested while/range/array loops—plus ARC strings, vector arrays, and nominal structs execute as native Wasm. Units emit only the Bearer host imports, ARC helpers/global, and function table they use. Parameter `any` monomorphizes lazily by concrete argument types, caches specializations, validates operators after substitution, supports fixed constructor results and `x::type` results, prefers exact overloads, and rejects equally ranked generic matches. Parenthesized comma expressions lower as managed heterogeneous tuples; parenthesized function results carry multiple values through static checked indexing and recursive ARC drop glue. Type-constructor calls and declaration-site `name : as Type` construction support numeric and Boolean scalar conversion, scalar-to-string formatting, and user-defined overloads. Variadic declarations, array and tuple splats, coercive variadic packs, and variadic function values and lambdas are implemented. Public function types, noncapturing lambdas, and ARC-managed capturing closures are implemented. Named calls remain direct; converting a name to a value generates a private closure thunk, managed captures are retained and dropped with the environment, and modules without function values omit their table. Capy exports `COMPONENT` alongside the other Bearer handlers. `unit_render(string)` and `component_render(string)` enter other units through core Bearer dispatch; tested Capy→C++ render and C++→Capy component calls do not share language object layouts. `dval(string)` creates an owned ARC object containing copied BRRB2 bytes, `dval_string` decodes a copied scalar, and `unit_call(target, function, dval)` crosses the existing Bearer unit membrane with a staged copied result so the target executes once. Copy/reassignment and managed temporary cleanup are covered. Nested DValue maps/lists, strict string/integer indexing, explicit missing checks, strict scalar extraction, ordered map/list iteration, Capy custom exports, and structured round trips in both language directions are implemented. Reflection and weak references remain open. The authoritative remaining work is tracked in `/root/docs/work/capy-compiler.md`.
