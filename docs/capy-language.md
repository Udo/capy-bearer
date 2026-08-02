# Capy language specification

This document specifies the Capy source language. The compiler and runtime must follow these rules. Bearer API behavior belongs in the online API reference. WebAssembly object layouts, caches, imports, and artifact publication belong in implementation documents.

## 1. Source files and tokens

A Capy source file uses the `.capy` suffix. Source text is case-sensitive.

Spaces, tabs, and newlines are whitespace. A newline does not end an expression. Grammar delimiters end expressions. A semicolon separates adjacent expressions when the grammar would otherwise be ambiguous. Commas separate call arguments, array items, tuple items, map entries, parameters, and names in `EXPORTS`.

A line comment starts with `//` and ends at the next newline.

An identifier starts with a letter or underscore. Later characters can also be decimal digits. Names that start with `__bearer_` are reserved for the standard-library boundary. Public standard-library names are also reserved.

String literals use double quotes. The lexer accepts the documented byte escapes. Markup uses `<>...</>`. An escaped markup field uses `<?= expression ?>`. A raw markup field uses `<?: expression ?>` and requires `markup`.

Integer literals have type `s32` by default. The suffixes `s64` and `u64` select the wide integer types. A decimal point or exponent selects `f64`. The compiler rejects literals outside the selected type range.

The directives `#compile`, `#callsite`, and `emit` are reserved. They are not part of executable Capy.

## 2. Program structure

A source file contains top-level declarations. Executable expressions are valid only in function bodies. The top-level forms are:

```text
function declaration
host function declaration
trace host function declaration
struct declaration
type alias
EXPORTS declaration
```

Application source cannot declare host functions. Host declarations are valid only in the embedded standard library.

A block starts with `{` and ends with `}`. Blocks contain expressions. `type` and `EXPORTS` are top-level only. Capy has no `const` declaration.

## 3. Type expressions

Capy has these built-in types:

- `bool`, `s32`, `s64`, `u64`, and `f64` are scalar types.
- `string` and `markup` are managed byte values.
- `dval` is a managed copied dynamic value.
- `request` and `module` are opaque Bearer values.
- `void` means that an expression produces no value.

The following forms compose types:

```text
[T]                              array
(T1, T2, ...)                    tuple
StructName                       nominal struct
function(parameters) Result      function value
```

`void` is valid as a function result. It is not valid as a local, parameter, array item, tuple field, struct field, or captured value.

`module` can occupy a local, parameter, result, or generic identity value during one request. It cannot enter an array, tuple, struct, closure, DValue, condition, arithmetic operation, or serialized boundary.

### 3.1 Transparent aliases

A top-level alias has this form:

```capy
type Count = s64
type Counts = [Count]
type Pair = (Count, string)
type Callback = function(value : Count) string
```

An alias is a compile-time spelling for its expanded type. It creates no runtime value, type tag, storage, reflection entry, or nominal identity. Alias expansion occurs before overload identity, construction, aggregate layout, and code generation. Forward alias references are valid. Cycles are errors.

An alias works in each type position. An alias to a constructible scalar, struct, or `dval` can also name that constructor:

```capy
type Count = s64
var count := Count(4)
```

An alias to an array, tuple, function type, `request`, `module`, or `void` has no constructor call. `any` and dependent `value::type` expressions require a generic function context and cannot be closed top-level aliases.

An alias cannot conflict with a built-in type, struct, function, handler, public standard-library name, or another alias.

## 4. Declarations, scope, and assignment

Use `var name := value` or `name := value` to declare an inferred local. Use `var name : Type = value` for a stated type. Use `name = value` to replace an existing local.

A declaration introduces its name in the current lexical scope. A second declaration of that name in the same scope is an error. A nested scope can shadow an outer name. Parameters are immutable bindings. Captures are immutable copies or retained values. A closure cannot assign an outer local.

A declaration and an assignment evaluate the right side once. Each expression yields the stored value. For a managed declaration, the local owns the stored reference and the declaration result is a borrow.

A declaration in an `if` or `while` condition occurs in the surrounding function block. The conditional right side of `&&` or `||` has a nested scope and does not export declarations.

## 5. Expressions and precedence

From highest precedence to lowest, Capy parses:

1. calls, indexing, member access, and `::` lookup;
2. prefix `!`, prefix `-`, and spread `...`;
3. `*`, `/`, `%`;
4. `+`, `-`;
5. `<`, `<=`, `>`, `>=`;
6. `==`, `!=`;
7. `&&`;
8. `||`;
9. `..`;
10. type annotation `:`;
11. assignment `=` and declaration `:=`.

Assignment and declaration associate right-to-left. Other binary operators associate left-to-right. The old `value as Type` conversion expression is invalid. `as` is valid only in a coercive parameter annotation.

Capy evaluates call arguments, spread sources, collection items, constructor arguments, and markup fields once from left to right. `&&` and `||` evaluate the right side only when needed.

Arithmetic and comparisons require equal operand types. Boolean operators require `bool`. A condition for `if` or `while` must have type `bool`. Integer division by zero traps. Checked indexing and failed checked conversions trap at their source expression.

The range `start..end` is a half-open `s32` range. It contains values from `start` up to but not including `end`.

## 6. Blocks and conditionals

A block evaluates its expressions in order. An empty block has type `void`. A non-empty block produces the value of its final reachable expression. Earlier produced values are discarded.

The block owns its locals. Before normal block exit, the compiler preserves the final managed result and then releases the locals in reverse declaration order. It releases each managed value exactly once on normal exit, `return`, `break`, and `continue`. A runtime trap discards the complete request workspace.

An `if` condition evaluates once. An `if` without `else` has type `void`. Its selected branch result is discarded.

An `if` with `else` produces the selected branch value. All branches that can fall through must produce the same canonical type. A branch that always returns does not constrain the other branch type. If neither branch can fall through, the conditional does not produce a reachable value.

```capy
var label := if ready {
    "ready"
} else {
    "waiting"
}
```

Only the selected branch runs. Managed branch results transfer one owned reference across branch and block cleanup.

## 7. Loops and function exit

`while condition { ... }` repeats while its Boolean condition is true. `for value = range_or_array { ... }` iterates a range or array. A DValue loop can bind a value or a key and value.

Loops have type `void`. `break` leaves the nearest loop. `continue` starts its next iteration. Neither accepts a value.

`return expression` exits the function with a value. Bare `return` is valid only for a `void` result. A non-void function can omit explicit `return` when its final reachable expression has the declared result type. A function is also complete when all paths return explicitly.

## 8. Functions, overloads, and constructors

A function has a name, optional parameters, an optional result type, and a body:

```capy
function add(left : s32, right : s32) s32 {
    left + right
}
```

A missing result type means `void`. A final `...values : Type` parameter is variadic and receives `[Type]`. It must be last. Prefix `...` spreads an array into a variadic tail. A tuple or struct can spread into statically known fixed parameters.

Overload identity is the function name plus canonical parameter types and the variadic contract. Return types do not distinguish overloads. Transparent aliases do not distinguish overloads.

Resolution order is:

1. exact concrete parameters;
2. an `any` specialization without construction;
3. fixed parameters with declared construction;
4. variadic parameters.

Within a coercive rank, fewer constructor calls win. Within a variadic rank, a longer fixed prefix wins. Equal best candidates are ambiguous. The compiler does not backtrack after a selected generic specialization fails.

A parameter `name : as Type` accepts either `Type` or one inserted `Type(argument)` call. It never inserts a conversion chain.

A type name is a constructor overload set. Built-in constructors convert scalar types and format scalars as strings. A struct receives a generated constructor in field order. User functions can add constructor overloads, but cannot duplicate a built-in conversion or generated field constructor.

`any` is compile-time polymorphism. It has no runtime representation. In a generic function, `parameter::type` names the concrete static type bound to that `any` parameter.

## 9. Collections and aggregates

An array `[T]` contains one static item type. Arrays have copy-on-write value semantics. Assignment and parameter passing preserve logical independence. Array items can be narrow scalars, wide scalars, or managed values. Indexes start at zero.

A comma expression creates a tuple. `()` is the empty tuple. `(value)` is grouping. Tuples have fixed arity and static field types.

A struct is nominal and lists fields in declaration order:

```capy
struct Sample {
    count : s32
    total : u64
    ratio : f64
    name : string
}
```

Tuples, structs, and closure environments support `s32`, `s64`, `u64`, `f64`, and managed fields. Their source semantics do not expose byte offsets or padding.

A map literal creates a copied `dval`. Use `{:}` for an empty DValue map. `{}` remains an empty block. The `dval({...})` compatibility spelling has the same result.

A list passed to `dval(...)` creates a dynamic DValue list. A bare list remains a typed copy-on-write array.

DValue indexing is strict and traps on an invalid key, index, or container. For an identifier key, `value.name` is equivalent to `value["name"]` when `value` has static type `dval`. Use bracket indexing for dynamic, numeric, or non-identifier keys. A member followed by `()` remains a receiver-first method call.

Use `dval_has` for a non-trapping key check. Selected standard-library value parameters use `as dval`. They can insert one DValue construction from `string`, `s32`, `u64`, `f64`, or `bool`. Parameters that require a specific map, list, or opaque DValue shape remain exact.

## 10. Strings, markup, and ownership

Strings preserve bytes. String indexing is not a source-language operation. Standard-library functions provide byte lengths, search, slicing, replacement, and case conversion.

Markup escapes ordinary interpolated strings. Raw interpolation requires `markup`. `trusted_markup(string)` is the explicit trust boundary.

Capy uses request-local automatic reference counting for strings, markup, arrays, tuples, structs, closures, and DValues. Managed parameters are borrowed. Managed results are owned. Assignment retains the replacement before it releases the old value. Captures retain managed values. ARC does not collect strong cycles before request teardown. Capy has no weak-reference source type.

## 11. Function values and closures

A function type has a complete static parameter and result signature. An overloaded name requires a declared function type that selects one concrete overload. A generic declaration becomes a function value only after a concrete signature selects it.

An anonymous function uses `function(parameters) Result { ... }`. It can read surrounding values. Scalar captures copy. Managed captures retain. Wide scalar captures are valid. A closure cannot capture `module` and cannot mutate an outer binding.

A callable struct field takes priority over receiver-first method lookup. Otherwise, `receiver.method(arguments)` resolves as an ordinary function call with `receiver` as the first argument.

## 12. Handlers and unit composition

Bearer recognizes these handlers:

```text
CLI
RENDER and RENDER:NAME
COMPONENT and COMPONENT:NAME
WS
ONCE
INIT
SERVE_HTTP and SERVE_HTTP:NAME
TASK and TASK:NAME
```

A handler cannot be overloaded, generic, variadic, coercive, or value-returning. `TASK` and `TASK:NAME` require one `request` parameter. Other request handlers accept no parameter or one `request` parameter, as defined by their API pages.

`INIT` runs after Bearer materializes the unit in a request workspace and before its first dispatch. Bearer creates a new workspace for each request, so `INIT` can run again for the next request. It does not provide persistent worker-local state. `ONCE` runs before the first entry or component call for one resolved unit in one request. It runs at most once for that unit in that request.

`EXPORTS name` exposes an ordinary local function with the exact signature `(dval) dval`. It does not execute code. `module.call(name)` supplies an empty DValue. `module.call(name, input)` copies its input and result through the Bearer membrane.

Capy and C++ units share no language object layout. DValues cross as copied BRRB values. Module values are request-local verified capabilities. Components, tasks, jobs, requests, files, databases, and network operations follow their online API pages.

## 13. Errors, traps, and source locations

Syntax, type, overload, alias-cycle, scope, and unsupported-operation failures are compile errors. Checked conversion, allocation, bounds, DValue, and explicit `trap` failures are runtime traps.

The compiler emits source locations for function entries and trapping expressions. Standard-library implementation frames do not replace the user call location. Compilation and artifact generation are deterministic for the same source, compiler identity, ABI, and options.

## 14. Unsupported and deferred features

Capy does not provide source includes, macros, exceptions, runtime reflection, inheritance, implicit mutable captures, weak references, enums, tagged unions, resource types, or compile-time metaprogram execution.

Enums and tagged unions are planned after this consistency pass. `language-ideas.capy` is design material. It is not normative or guaranteed to compile.
