# Capy language specification

This document specifies the Capy source language. The compiler and runtime must follow these rules. Bearer API behavior belongs in the online API reference. WebAssembly object layouts, caches, imports, and artifact publication belong in implementation documents.

## 1. Source files and tokens

A Capy source file uses the `.capy` suffix. Source text is case-sensitive.

Spaces, tabs, and newlines are whitespace. A newline does not end an expression. Grammar delimiters end expressions. A semicolon separates adjacent expressions when the grammar would otherwise be ambiguous. Commas separate call arguments, array items, tuple items, map entries, parameters, and names in `#exports`.

A line comment starts with `//` and ends at the next newline.

An identifier starts with a letter or underscore. Later characters can also be decimal digits. Names that start with `__bearer_` are reserved for the standard-library boundary. Public standard-library names are also reserved.

String literals use double quotes. The lexer accepts the documented byte escapes. Markup uses `<>...</>`. An escaped markup field uses `<?= expression ?>`. A raw markup field uses `<?: expression ?>` and requires `markup`.

Integer literals have type `s32` when no expected integer type is present. A stated local type, parameter, function result, assignment target, typed array, struct field, or other binary operand can supply an expected integer type. An integer constructor also selects a wide type. For example, `var count : s64 = 8` and `var count := s64(8)` both produce `s64`. Numeric suffixes are not part of Capy. A decimal point or exponent selects `f64`. The compiler rejects literals outside the selected type range.

An unconstrained literal prefers an exact `s32` overload. If no `s32` overload matches, one unambiguous integer parameter type can supply the context. Equal `s64` and `u64` candidates are ambiguous. Capy does not use a result type to select an overload.

`none` is a reserved literal. It has type `dval`.

The directive `#exports` declares exported unit metadata. The directive `#import` imports exported type metadata into a compile-time namespace. The directives `#compile`, `#callsite`, and `emit` are reserved. They are not part of executable Capy.

## 2. Program structure

A source file contains top-level declarations. Executable expressions are valid only in function bodies. The top-level forms are:

```text
function declaration
host function declaration
trace host function declaration
struct declaration
type alias
#exports directive
#import directive
```

Application source cannot declare host functions. Host declarations are valid only in the embedded standard library.

A block starts with `{` and ends with `}`. Blocks contain expressions. `type`, `#exports`, and `#import` are top-level only. Capy has no `const` declaration.

## 3. Type expressions

Capy has these built-in types:

- `bool`, `s32`, `s64`, `u64`, and `f64` are scalar types.
- `string` and `markup` are managed byte values.
- `dval` is a managed mutable dynamic value.
- `module` is an opaque Bearer value.
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

An alias to an array, tuple, function type, `module`, or `void` has no constructor call. `any` and dependent `value::type` expressions require a generic function context and cannot be closed top-level aliases.

An alias cannot conflict with a built-in type, struct, function, handler, public standard-library name, or another alias.

## 4. Declarations, scope, and assignment

Use `var name := value` or `name := value` to declare an inferred local. Use `var name : Type = value` for a stated type. Use `name = value` to replace an existing local.

A declaration introduces its name in the current lexical scope. A second declaration of that name in the same scope is an error. A nested scope can shadow an outer name. Parameters are immutable bindings. Thus, a function cannot assign a new value to a parameter name. Content mutation through an aggregate parameter is valid and affects the caller. Captures keep scalar copies or shared managed values. A closure cannot assign an outer local or a capture binding.

A declaration and an assignment evaluate the right side once. Each expression yields the stored value. For a managed declaration, the local owns the stored reference and the declaration result is a borrow.

A declaration in an `if` or `while` condition occurs in the surrounding function block. The conditional right side of `&&` or `||` has a nested scope and does not export declarations.

## 5. Expressions and precedence

From highest precedence to lowest, Capy parses:

1. calls, indexing, member access, postfix `?`, and `::` lookup;
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

Arithmetic and comparisons require equal operand types. Boolean operators require `bool`. A condition for `if` or `while` must have type `bool`. Integer division by zero traps. Typed-array bounds checks and failed checked conversions trap at their source expression.

Postfix `?` requires `dval` and returns `bool`. It binds after a member or index read. It returns `false` only for `none`.

```capy
profile.nickname?       // false when nickname is missing
profile.tags[3]?        // false when item 3 is absent
none?                   // false
```

The range `start..end` is a half-open `s32` range. It contains values from `start` up to but not including `end`.

## 6. Blocks and conditionals

A block evaluates its expressions in order. An empty block has type `void`. A block produces a value only when its final reachable item is `-> expression`. Earlier produced values are discarded.

The block owns its locals. Before normal block exit, the compiler preserves the yielded managed result. It then releases the locals in reverse declaration order. It releases each managed value exactly once on normal exit, `return`, `break`, and `continue`. A runtime trap discards the complete request workspace.

An `if` condition evaluates once. An `if` without `else` has type `void`. Its selected branch result is discarded.

An `if` with `else` produces the selected branch yield. All branches that can fall through must yield the same canonical type. A branch that always returns does not constrain the other branch type. If neither branch can fall through, the conditional does not produce a reachable value.

```capy
var label := if ready {
    -> "ready"
} else {
    -> "waiting"
}
```

Only the selected branch runs. Managed branch results transfer one owned reference across branch and block cleanup.

## 7. Loops and function exit

`while condition { ... }` repeats while its Boolean condition is true. A `for` loop has these forms:

```capy
for value := iterable { ... }
for value, index := array { ... }
for value, key := dynamic_value { ... }
```

The optional metadata follows the value. An array index has type `s32`. A DValue key has type `string`. DValue lists use decimal index strings as keys. A range permits only the one-value form. Structs are not iterable. The old `=` and `in` loop forms are invalid.

Array and DValue loops use a live view. Each iteration checks the current count again. Growth can add later iterations, including growth from the loop body. Shrinkage cannot cause an invalid item access. An array loop retains its current managed item before the body runs. A DValue loop copies its current item before the body runs.

Loops have type `void`. `break` leaves the nearest loop. `continue` starts its next iteration. Neither accepts a value.

`return expression` exits the function with a value. Bare `return` is valid only for a `void` result. `-> expression` yields the current block value and must be its final reachable item. A non-void function must yield its declared result from the outer body or return explicitly on all paths.

## 8. Functions, overloads, and constructors

A function has a name, optional parameters, an optional result type, and a body:

```capy
function add(left : s32, right : s32) s32 {
    -> left + right
}
```

A missing result type means `void`. A trailing parameter can have a literal default of its exact annotated type. Defaults are valid only on ordinary named, non-generic, non-variadic functions. A final `...values : Type` parameter is variadic and receives `[Type]`. It must be last. Prefix `...` spreads an array into a variadic tail. A tuple or struct can spread into statically known fixed parameters.

A direct named call can omit only trailing default parameters. The call evaluates supplied arguments once from left to right. It then evaluates omitted defaults in parameter order. A function value has its full parameter arity. Defaults do not shorten its function type. Hosts, handlers, lambdas, and function types cannot declare defaults.

```capy
function label(text : string, suffix : string = "!") string { -> text + suffix }
label("Capy") // "Capy!"
```

Overload identity is the function name plus canonical parameter types and the variadic contract. Defaults and return types do not distinguish overloads. Transparent aliases do not distinguish overloads.

The compiler ranks matching calls in this order:

1. exact concrete parameters;
2. an `any` specialization without construction;
3. fixed parameters with declared construction;
4. variadic parameters.

The compiler uses defaults only after it matches the supplied parameters. Fewer constructor calls win within a coercive rank. A longer fixed prefix wins within a variadic rank. Equal best candidates are ambiguous. The compiler does not backtrack after a selected generic specialization fails.

A parameter `name : as Type` accepts either `Type` or one inserted `Type(argument)` call. It never inserts a conversion chain. For example, `print(profile.nickname)` inserts `string(profile.nickname)`. Do not write a redundant `string(...)` call for an `as string` parameter.

A type name is a constructor overload set. Built-in constructors convert scalar types and format scalars as strings. DValue scalar constructors have one signature each:

```capy
string(value : as string, opt : dval = {}) string
bool(value : dval, opt : dval = {}) bool
s32(value : as string, opt : dval = {}) s32
s64(value : as string, opt : dval = {}) s64
u64(value : as string, opt : dval = {}) u64
f64(value : as string, opt : dval = {}) f64
```

`opt.fallback` replaces the type default. The integer constructors also accept a numeric whole `opt.base` from 2 to 36. The default base is 10. Empty text, an absent `dval`, and text that does not parse use the fallback. Use the `?` suffix before conversion when code must distinguish an absent value from an empty value.

`as string` preserves scalar source text. `s64` and `u64` preserve exact in-range wide integer values. `u64(dval(true))` uses the fallback because `true` converts to text and does not parse as a number. `bool` keeps a `dval` parameter because strings cannot be conditions. A constructor does not change a DValue read into a strict read.

A struct receives a generated constructor in field order. User functions can add constructor overloads. They cannot duplicate a built-in conversion or generated field constructor.

Constructor defaults and omitted call arguments do not change the Wasm call ABI. Copied DValue `none` uses Wasm core ABI 26. Future ABI changes must use a new ABI version.

`any` is compile-time polymorphism. It has no runtime representation. In a generic function, `parameter::type` names the concrete static type bound to that `any` parameter.

## 9. Collections and aggregates

An array `[T]` contains one static item type. An array is a mutable reference value inside one workspace. Assignment, parameter passing, returns, and captures share its identity. Content mutation through one alias is visible through all aliases. Growth preserves the array identity. Array items can be narrow scalars, wide scalars, or managed values. Indexes start at zero.

A comma expression creates an immutable tuple. `()` is the empty tuple. `(value)` is grouping. Tuples have fixed arity and static field types.

A struct is nominal and lists fields in declaration order:

```capy
struct Sample {
    count : s32
    total : u64
    ratio : f64
    name : string
}
```

Tuples, structs, and closure environments support `s32`, `s64`, `u64`, `f64`, and managed fields. A struct instance is a mutable reference value inside one workspace. Assignment, parameter passing, returns, and captures share its identity. Struct fields are assignable. Their source semantics do not expose byte offsets or padding.

A map literal creates a DValue map. Use `{}` for an empty DValue map. The old `{:}` spelling is a parse error. The `dval({...})` spelling has the same result.

`dval(...)` accepts `string`, `s32`, `s64`, `u64`, `f64`, `bool`, an existing DValue, or a list. It preserves in-range `s64` and `u64` values exactly. A list passed to `dval(...)` creates a dynamic DValue list. A bare list remains a typed mutable array.

`none` is a DValue value. It differs from an empty string, `false`, an empty map, and an empty DValue list. A copied `none` remains `none` in BRRB. JSON and YAML encode it as `null`. JSON `null`, YAML `null`, and YAML `~` decode as `none`. XML encodes it as empty text. XML decode cannot recover the distinction.

DValue member and index reads are safe. A missing key, unusable scalar intermediate, negative list index, and out-of-range list index return `none`. Reads do not mutate the receiver. For an identifier key, `value.name` is equivalent to `value["name"]` when `value` has static type `dval`. Use bracket indexing for dynamic, numeric, or non-identifier keys. A member followed by `()` remains a receiver-first method call.

`dval_has(value, key)` tests map-key existence. It returns `true` when the child is `none`. `dval_require(value, key)` and `dval_require(value, index)` are strict reads. They trap for a missing child or unusable access.

A DValue is a mutable reference value inside one workspace. Assignment, parameter passing, returns, and captures share its identity. Nested assignment mutates the selected path and returns the same root identity. Its root must be a named local, parameter, or captured `dval` binding. Temporary, call, member, array-item, and loop-item roots are invalid. The compiler evaluates selectors once from left to right. It then evaluates the right side once.

Missing, `none`, and scalar intermediate values become maps. Existing maps remain maps. Existing lists accept only in-range, nonnegative numeric selectors. A missing intermediate before a numeric selector becomes a map with its decimal key. Negative and out-of-range existing-list writes trap. Capy does not create sparse lists.

```capy
var profile := {}
profile.contact.email = "ada@example.test"
print(profile.contact.email) // ada@example.test
```

`dval_set`, `dval_assign`, `dval_push`, `dval_pop`, `dval_remove`, `dval_clear`, `dval_get_or_create`, `dval_set_array`, `dval_set_bool`, `dval_set_type`, and `dval_put` mutate their target. Each function returns that same target identity. Existing code can still assign the returned value back to its target binding.

DValue member and index reads return copied child values. Pure transformations return new values where their API specifies a copy. DValue loop items are also copies. Diagnostics identify the nested assignment expression.

Selected standard-library value parameters use `as dval`. They can insert one DValue construction from `string`, `s32`, `s64`, `u64`, `f64`, or `bool`. Parameters that require a specific map, list, or opaque DValue shape remain exact. Typed arrays remain strict. They are distinct from DValue lists.

## 10. Strings, markup, and ownership

Strings preserve bytes. String indexing is not a source-language operation. Standard-library functions provide byte lengths, search, slicing, replacement, and case conversion.

A markup literal starts with `<>` and ends with `</>`. It creates a `markup` value. It does not write to the response by itself. `RENDER` and `COMPONENT` do not give bare markup expressions special output behavior. Use `print(<><p>Ready</p></>)` to output markup from a handler or component. Return `markup` from helper functions when the caller should choose where to print it.

```capy
function badge(label : string) markup {
    -> <><strong><?= label ?></strong></>
}

function RENDER(request : dval) {
    print(badge("Ready"))
}
```

`<?= value ?>` selects an encoding from its markup context. HTML text and quoted attributes use HTML escaping. A string in a `script` element becomes one self-contained JavaScript literal. A string in a `style` element becomes one self-contained CSS literal. Integer and Boolean values remain scalar values in those elements. `f64` interpolation is not supported in `script` or `style` elements.

Script and style interpolation must start at a value boundary. Static source after the interpolation can apply an operator or a CSS unit. Interpolation cannot occur inside a quoted string, template literal, or comment. An attribute value must be quoted. Tag names and attribute names cannot contain interpolation. URL, event-handler, and inline-style attributes still use HTML attribute escaping. Validate their application-specific value before interpolation.

A value with static type `markup` remains trusted when `<?= value ?>` inserts it into HTML text. In attributes, scripts, and styles, the compiler escapes it as a string for that context. Raw `<?: value ?>` interpolation also requires `markup` and works only in HTML text. `trusted_markup(string)` is the explicit trust boundary.

Capy uses request-local automatic reference counting for strings, markup, arrays, tuples, structs, closures, and DValues. Managed parameters are borrowed. Managed results are owned. Assignment retains the replacement before it releases the old value. Captures retain managed values. Strong ARC cycles remain until request teardown. Capy has no weak-reference source type.

Arrays, DValues, and struct instances share identity only inside one workspace. A Bearer, module, component, task, custom export or C++ call, codec, request, or serialization boundary copies BRRB. Identity never crosses these boundaries.

A DValue can hold a named function or closure in its current workspace. A typed read requires the exact function signature. A signature mismatch traps. Public BRRB, JSON, YAML, and XML replace callable entries with `none`. Map keys and list indexes remain present.

## 11. Function values and closures

A function type has a complete static parameter and result signature. An overloaded name requires a declared function type that selects one concrete overload. A generic declaration becomes a function value only after a concrete signature selects it.

An anonymous function uses `function(parameters) Result { ... }`. It can read surrounding values. Scalar captures copy. Managed captures retain. Wide scalar captures are valid. A closure cannot capture `module` and cannot mutate an outer binding.

A callable struct field takes priority over receiver-first method lookup. Otherwise, `receiver.method(arguments)` resolves as an ordinary function call with `receiver` as the first argument.

A closure stored in a DValue stays valid only in the current workspace. DValue assignment, replacement, deletion, clearing, and scope exit release its closure reference. DValue copies retain the same local callable until all copies release it.

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

A handler cannot be overloaded, generic, variadic, coercive, or value-returning. Every Bearer handler requires exactly one `request : dval` parameter. This includes named handlers and custom HTTP handlers.

`INIT` runs after Bearer materializes the unit in a request workspace and before its first dispatch. Bearer creates a new workspace for each request, so `INIT` can run again for the next request. It does not provide persistent worker-local state. `ONCE` runs before the first entry or component call for one resolved unit in one request. It runs at most once for that unit in that request.

`#exports name` exposes local unit metadata. If `name` is a callable `(dval) dval` function, Bearer also exposes it to dynamic module calls. If `name` is a function and a type, Capy exports both metadata entries.

`module.call(name)` supplies an empty DValue. `module.call(name, input)` copies its input and result through the Bearer membrane. A member call on a module is dynamic sugar for the same operation. For example, `service.echo(input)` is equivalent to `service.call("echo", input)`.

`#import "path" as alias` is compile-time only. It imports exported Capy type metadata from another unit. Imported types are available in type positions as `alias.TypeName`. Static imports do not load a runtime module and do not make `unit_load()` static.

Capy units do not expose language object layouts. DValues cross custom export boundaries as copied BRRB values. Module values are request-local verified capabilities. Components, tasks, jobs, requests, files, databases, codecs, serialization, and network operations follow their online API pages.

## 13. Errors, traps, and source locations

Syntax, type, overload, alias-cycle, scope, and unsupported-operation failures are compile errors. Checked conversion, allocation, typed-array bounds, strict DValue access, invalid DValue list writes, and explicit `trap` failures are runtime traps.

The compiler emits source locations for function entries and trapping expressions. Standard-library implementation frames do not replace the user call location. Compilation and artifact generation are deterministic for the same source, compiler identity, ABI, and options.

## 14. Unsupported and deferred features

Capy does not provide source includes, macros, exceptions, runtime reflection, inheritance, implicit mutable captures, weak references, enums, tagged unions, resource types, or compile-time metaprogram execution.

Enums and tagged unions are planned after this consistency pass. `language-ideas.capy` is design material. It is not normative or guaranteed to compile.
