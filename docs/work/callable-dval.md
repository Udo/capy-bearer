# Callable dynamic values

## Objective and invariants

Add named functions and closures as request-local `dval` entries. Keep this code near the dynamic-value codec and closure ownership paths.

- Public serialization never exposes a closure pointer, table slot, or type index.
- Callable entries do not outlive their Wasm workspace.
- Existing closure capture and ARC rules remain unchanged.
- Ordinary `dval` behavior and wire bytes remain unchanged when no callable is present.
- Use one callable value kind. Do not add path sidecars or a callback registry.

## Success criteria

- [x] A named function can be stored, read with an exact function type, and called.
- [x] A capturing closure can be stored, copied, replaced, and released without an ARC leak.
- [x] A wrong function signature traps with a useful error.
- [x] Public serializers replace callable values with `none` and preserve map keys and list indexes.
- [x] Callable values cannot cross request, unit, component, task, codec, or persistence boundaries.
- [x] Native, Wasm, language, documentation, and artifact-golden gates pass.

## Current state

- Status: complete
- Source/runtime: `/root/mount_ssh/capy-bearer`, `root@10.4.2.122:/Code/capy-bearer`

## Goal tree

- [x] G1: Add one local callable dynamic-value kind.
  - [x] G1.1: Preserve callable nodes in local dynamic-value operations.
  - [x] G1.2: Reuse existing closure ARC and indirect-call lowering.
  - [x] G1.3: Project callable nodes to `none` at public boundaries.
- [x] G2: Verify ownership and boundary safety.
  - [x] G2.1: Test named functions, captures, copies, mutation, and exact signatures.
  - [x] G2.2: Test serializers and cross-boundary behavior.
  - [x] G2.3: Run full gates and regenerate the artifact golden file last.

## Implementation notes

- The local BRRB codec uses `C` nodes. A node stores a guest closure pointer and its Wasm function-type index.
- The public BRRB codec converts `C` nodes to `none`. It keeps map keys and list positions.
- `bearer_dv_*` uses the local codec. Typed `dval` extraction checks the stored type index before it returns a closure.
- The compiler retains a closure when it creates a callable node. The dval release helper walks local callable nodes and releases each closure.
- `site/tests/capy-dval-callable.capy` covers named functions, closure literals, typed extraction, list access, and public JSON and BRRB projection.

## Decisions and risks

- Use a guest-local callable codec tag with a closure pointer and exact Wasm function-type index.
- Preserve map shape and list indexes. Wire projection converts callable values to `none`.
- A callable value is valid only in its current workspace.

## Test and documentation update

- `capy-dval-callable.capy` checks named and capturing callables in nested maps and lists.
- The fixture checks public BRRB, JSON, YAML, and XML projection to `none`. It checks retained map keys and list indexes.
- The fixture checks ARC baseline recovery after assignment, replacement, deletion, clearing, and DValue copy scope exit.
- `capy-dval-callable-signature-trap.capy` checks an exact function-signature mismatch at typed extraction.
- The phase-one runtime runner checks the fixture, the trap source location, and request recovery.
- The native runner validates both focused fixtures. It does not add an artifact-golden step.
- The language specification and current function, dynamic-value, and type source pages describe local callable DValues and public projection.
- The implementation must remain near the existing codec, ARC, and function conversion paths.
