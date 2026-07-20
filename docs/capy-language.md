# Capy language and compiler

Capy is a statically compiled language that emits Bearer-compatible WebAssembly side modules directly. It does not transpile through C or C++. Capy and C++ units communicate only through Bearer’s unit/component/hostcall membrane.

## Declaration grammar

Capy declarations are expression-based. A function declaration consists of:

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

Function overload identity is the function name plus normalized parameter type patterns. Return types never distinguish overloads and expected result types never select an overload.

## Compile-time polymorphism

`any` is specified as a compile-time type placeholder, not an erased runtime value. Phase 3 will bind each `any` parameter to its concrete static argument type, type-check the specialization, and cache it by function plus concrete parameter types.

```capy
function identity(x : any) x::type {
    return x
}
```

`x::type` is a compile-time dependent type expression. A specialization fails at its callsite if its body requires an operator or conversion unavailable for the bound type. There is no runtime `any` tag or dispatch.

The planned phase-3 overload ranking is deterministic:

1. concrete exact parameter matches;
2. `any` specializations without conversion;
3. candidates using declared `as` conversions, ranked by total conversion cost;
4. equal best candidates are ambiguous.

## Tuples and returns

Comma expressions form tuples. Parentheses group expressions, and therefore also delimit tuple type expressions where needed.

```capy
return 10, 20
return (10, 20)
var pair := (10, 20)
```

`()` is the empty tuple/unit value and `(x)` is grouping. Calls distinguish two arguments from one tuple argument: `f(a, b)` versus `f((a, b))`.

## Bearer unit ABI

A Capy source uses `.capy`; C++/template units retain `.uce`. Both compile into the same request-local Bearer workspace and export the same handler names:

```text
CLI       -> __bearer_cli
RENDER    -> __bearer_render
WS        -> __bearer_websocket
ONCE      -> __bearer_once
INIT      -> __bearer_init
```

The first direct-Wasm backend emits:

- PIC `dylink.0` memory metadata;
- imports of `env.memory`, `env.__memory_base`, and the stable `env.bearer_print_bytes` byte-span output function;
- `bearer.abi` and `bearer.module` custom sections;
- a matching `BEARER_SOURCE_MAP_V1` sidecar;
- no WASI imports; dynamic values use Bearer’s workspace allocator.

Compiler generation c17 uses core ABI w10. Artifact staging, freshness metadata, native serialization, bounded diagnostics, and last-known-good policy remain owned by Bearer’s existing compiler coordinator. Frontend, typed lowering, and CLI code are separate files, and all participate in artifact freshness signatures.

## Automatic reference counting

Dynamic Capy values use non-atomic, workspace-local automatic reference counting. Bearer workspace destruction is the final reclamation boundary, including after traps. Managed strings, arrays, and nominal structs are implemented; managed tuples, closures, and weak handles will reuse the same header and generated drop-glue contract.

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
- strings, arrays, managed structs/tuples, and closure environments provide type-specific drop glue;
- static string literals are immutable immortal objects;
- function table slots belong to the workspace; closures retain only their environments;
- traps skip cleanup safely because the complete workspace is discarded;
- ARC does not collect strong cycles. `weak<T>` will support deliberate back-references; remaining strong cycles persist only until workspace teardown.

Current managed-value lowering imports Bearer’s workspace allocator/free functions and emits private retain, release, clone, and type-directed drop helpers into each Capy module. Allocation failure traps before any header or payload write. Literal strings are aligned immortal objects. Managed parameters are borrowed and cannot be rebound; managed results are owned; assignments retain-before-release; owned argument temporaries are released after calls; and every supported normal lexical/early-return edge emits cleanup. Arrays use `[T]` in type slots, bounds-check indexing, support array iteration, and currently accept scalar or string elements. Nominal structs use declaration-order constructors and checked member names. String-array and struct drop glue recursively releases managed fields before freeing the aggregate. `arc_live()` is a temporary conformance counter; trapping requests intentionally skip releases and prove that the next workspace starts clean.

Capy values never expose their object layout to C++. Dynamic cross-language values use owned/copied DValue/BRRB adapters at the Bearer membrane.

## Deferred features

`#compile`, `#callsite`, and `emit` are reserved but deferred beyond phase 3. The parser emits a targeted diagnostic and never executes them. Their eventual implementation requires staged compilation, a compile-time workspace, ordering/hygiene rules, dependency tracking, bounded execution, and source provenance.

## Current implementation boundary

The current implementation includes the lexer, expression parser, bounded diagnostics, direct Wasm encoding, `.capy` artifact integration, and real Bearer CLI/HTTP execution. Scalar control flow plus ARC strings, arrays, and nominal structs execute as native Wasm. Parameter `any` now monomorphizes lazily by concrete argument types, caches specializations, validates operators after substitution, supports `x::type` results, prefers exact overloads, and rejects equally ranked generic matches. Parenthesized comma expressions lower as managed heterogeneous tuples; parenthesized function results carry multiple values through static checked indexing and recursive ARC drop glue. Explicit `as` conversions are implemented for `s32`/`bool`; conversion-ranked overload candidates and string formatting remain open. A uniquely overloaded function name can be stored in an inferred local and invoked through a module-private Wasm table with its fixed signature; closure captures and function types in public annotations remain open. Capy exports `COMPONENT` alongside the other Bearer handlers. `unit_render(string)` and `component_render(string)` enter other units through core Bearer dispatch; tested Capy→C++ render and C++→Capy component calls do not share language object layouts. `dval(string)` creates an owned ARC object containing copied BRRB2 bytes, `dval_string` decodes a copied scalar, and `unit_call(target, function, dval)` crosses the existing Bearer unit membrane with a staged copied result so the target executes once. Copy/reassignment and managed temporary cleanup are covered. Rich DValue construction/indexing and Capy custom DValue exports remain open, as do reflection and weak references. The authoritative remaining work is tracked in `/root/docs/work/capy-compiler.md`.
