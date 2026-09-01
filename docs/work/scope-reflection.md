# Capy scope reflection

## Objective and invariants

Replace `type(value)` with `value::type`. Add static type and struct reflection without adding runtime type metadata to ordinary values.

Preserve these invariants:

- Reflection must not change struct object layouts.
- ABI changes must have explicit version increments.
- `value::items` must evaluate the receiver once.
- Reflection must preserve managed-value ownership.
- Every declared struct must carry a descriptor. Units that do not use `value::items` must not import the reflection host function.
- Imported type lookup must continue to work.

## Success criteria

- [x] `value::type` works for dependent generic parameters and results.
- [x] The compiler rejects `type(value)`.
- [x] `value::type_name` returns the concrete static type name as a string.
- [x] A struct value exposes its field count through `value::size` as `s64`.
- [x] A struct value exposes a `dval` map through `value::items`.
- [x] Each `items` entry contains `value` and `type_name` keys.
- [x] Indexed and fixed-key DValue access both work.
- [x] Reflection preserves ARC and evaluates the receiver once.
- [x] Documentation, generated files, manifests, and artifact hashes are current.
- [x] The complete acceptance suite passes.

## Current state

- Status: complete. Commit and push remain.
- Source and runtime: `/root/mount_ssh/capy-bearer` and `capy-bearer-dev:/Code/capy-bearer`
- Baseline: `daba223a9e73a4df5e3724b5721ed2c827eadc57`

## Goal tree

- [x] G1: Define the reflection contract and representation.
  - [x] G1.1: Trace scope lookup, struct layout, DValue conversion, and ARC.
  - [x] G1.2: Define an embedded descriptor for every declared struct.
  - [x] G1.3: Define one metadata-driven runtime path for field values.
- [x] G2: Implement the metadata and syntax.
  - [x] G2.1: Emit a reflection descriptor for every struct declaration.
  - [x] G2.2: Remove `type(value)` and keep dependent `value::type`.
  - [x] G2.3: Implement `value::type_name`, `value::size`, and `value::items` through the descriptors.
- [x] G3: Add focused tests.
  - [x] G3.1: Cover generic dependent types and rejected `type(value)`.
  - [x] G3.2: Cover field access, dynamic keys, nested values, ARC, and evaluate-once behavior.
  - [x] G3.3: Cover invalid receivers and unknown scope members.
- [x] G4: Update documentation and generated artifacts.
- [x] G5: Run acceptance and adversarial review.
- [ ] G6: Commit, push, and update project notes.

## Decisions, assumptions, and risks

- `value::type_name` reports the concrete static type after alias resolution.
- Every struct declaration must emit reflection metadata into the Wasm unit. Reflection must not depend on `#exports` or an external `.api` file.
- A custom section alone is insufficient because guest reflection code cannot read it. The unit needs descriptor data in linear memory.
- `value::size` and `value::items` apply only to structs.
- `value::items` has this shape: `{field_name: {value: field_value, type_name: "field_type"}}`.
- The DValue conversion must support every type that a struct can store. Nested structs and arrays need recursive conversion.
- Function fields can use the existing DValue callable encoding. Structs cannot store module handles.
- The compiler currently also accepts `import_alias::Type`. This work must not remove that imported type spelling unless the implementation proves it obsolete.
- Type aliases lose their source spelling during current type resolution. Reflection will report the resolved type unless preserving alias spelling has a clear local implementation.

## Evidence

- 2026-09-01: `ScopeLookup` parses any `expression::identifier`. The compiler currently gives it meaning only for dependent `::type` and imported types.
- 2026-09-01: DValue construction currently accepts scalars, strings, DValues, callables, map literals, and list literals. It does not directly accept struct or array expressions.
- 2026-09-01: The compiler keeps struct field metadata only while it compiles. `#exports` copies selected type declarations to external unit API metadata. The Wasm unit has no general struct reflection descriptor.
- 2026-09-01: An initial expression-specific lowering prototype was discarded. It did not meet the embedded-metadata requirement.

## Result

- The compiler emits recursive descriptors in linear memory for local and imported structs.
- Reflection lowering uses descriptor offsets relative to `__memory_base`.
- `value::items` calls `bearer_capy_reflect_struct_brrb`. It preserves callable and receiver ownership.
- Core ABI 32 and compiler unit ABI 34 identify the new host function and unit metadata.
- The compiler emits no reflection host import when source does not use `value::items`.

## Verification

- Native frontend, compiler, Wasm, CLI, and tracked fixture tests passed.
- The phase-one runtime suite passed.
- The reflection fixture passed in the live runtime. It covered fixed and indexed field access, nested structs, typed arrays, DValues, wide integers, callables, an empty struct, receiver evaluation, and ARC.
- Documentation generator and compile-timeout tests passed.
- Guide tests passed after the generated example included its type alias.
- Artifact hashes were regenerated after the ABI and embedded descriptors changed unit bytes. The non-write golden test passed.

## Concerns

- Imported struct descriptors have compile-time coverage. Imported struct values do not have a current cross-unit runtime path.
- The reflection host validates all addresses against the unit linear memory. A unit can still pass descriptors that point at other data in its own memory. This does not cross the unit memory boundary.
