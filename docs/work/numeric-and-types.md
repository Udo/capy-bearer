# Numeric Grid and Type Removal

## Objective and Invariants
Add the full scalar numeric grid. Use `s64` for unconstrained integer literals and public counts. Remove tuple and markup types. Keep interpolation encoding behavior unchanged.

## Success Criteria
- [x] The numeric grid, s64 literals, and public s64 counts work.
- [x] Tuple and markup types are absent.
- [x] Documentation, manifest, generated content, and golden artifacts match the source.
- [x] All required acceptance checks pass.

## Current State
- Status: complete
- Source/runtime: `/root/mount_ssh/capy-bearer`, `root@10.4.2.122:/Code/capy-bearer`

## Goal Tree
- [x] G1: Add numeric types and set s64 defaults. Verify compiler and runtime tests.
- [x] G2: Widen public count and index values. Verify call sites and fixtures.
- [x] G3: Remove tuple support. Verify tuples fail and variadics work.
- [x] G4: Remove markup support. Verify literal and interpolation behavior.
- [x] G5: Generate documentation and artifacts. Verify the manifest.
- [x] G6: Run all acceptance checks and commit.

## Next
1. Commit the verified work.

## Decisions / Assumptions / Risks
- Match the existing s32 conversion policy for narrow integer types.
- Keep private `__bearer_*` operation identifiers unchanged.
- Do not change parser or context-scanner interpolation rules.

## Evidence
- 2026-08-25: Task read. Work started.
- 2026-08-26: Removed tuple parsing and lowering. Grouping and array variadic spread compile. Tuple comma syntax fails.
- 2026-08-26: Native checks pass after fixture ports. Phase 1 fails on nested DValue assignment with an s64 list index.
- 2026-08-26: DValue reads and nested writes now accept public s64 list indices. The compiler validates the range before it narrows to the private s32 ABI. Invalid reads return none. Invalid writes still trap. Native checks pass. The rebuilt service passes the focused DValue gate. Phase 1 now reaches an unrelated u64 constructor radix failure.
- 2026-08-26: Contextual f32 float literals now emit `f32.const` directly. The numeric grid covers the f32 fallback path without recursive `f32(0)`.
- 2026-08-26: Added safe path normalization, output flushing, KV splitting and joining, type-page routing, and host/lib badges.
- 2026-08-26: All acceptance checks and the final golden write/check pass.
