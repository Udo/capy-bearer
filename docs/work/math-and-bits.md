# Math and Bit Functions

## Objective and Invariants
Add math and bit functions without new Capy operators. Use Wasm instructions where they exist. Use one typed host ABI call for libm transcendentals. Keep integer widths and signedness exact.

## Success Criteria
- [x] Float, predicate, bit, reinterpret, and clamp functions compile at every applicable type.
- [x] `bit_shr` selects arithmetic or logical shifting from the integer type.
- [x] Bit results use the declared width for `u8`, `u16`, `u32`, and `u64`.
- [x] `bits_of` and `f64_from_bits` round-trip normal values, zero, negative values, and NaN.
- [x] Documentation, manifest, and generated files match the source.
- [x] The compiler and core use one shared Wasm object layout definition.
- [x] Capy formatter wrappers replace the handwritten formatter bodies without a hot-path regression.
- [x] All required acceptance gates return zero.

## Current State
- Status: complete
- Source/runtime: `/root/mount_ssh/capy-bearer`, `root@10.4.2.122:/Code/capy-bearer`

## Goal Tree
- [x] G1: Map compiler and typed host ABI lowering. Verify each lowering path.
- [x] G2: Add float functions and libm host calls. Verify native and Capy tests.
- [x] G3: Add exact-width integer bit functions and reinterpret functions. Verify width gates.
- [x] G4: Add documentation, groups, guide section, signatures, and manifest. Verify documentation checks.
- [x] G5: Run acceptance gates and regenerate golden artifacts last.
- [x] G6: Define the shared Wasm object layout. Verify compiler and core assertions.
- [x] G7: Replace handwritten scalar formatter bodies with Capy formatter wrappers. Preserve output and hot-path cost.

## Next
1. Commit the verified work.

## Decisions / Assumptions / Risks
- Use functions only. Do not add infix syntax.
- Use arithmetic right shift for signed integers. Use logical right shift for unsigned integers.
- Normalize small integer inputs and results at each width boundary.
- Use a typed host ABI bridge backed by libm for transcendental functions.

## Evidence
- 2026-08-26: Task read. Delegated independent compiler, bit, and documentation mapping.
- 2026-08-26: Added initial typed libm host ABI and intrinsic lowering. Review found incomplete integer select, clamp, and rotate lowering. Do not generate artifacts until execution tests pass.
- 2026-08-28: Fixed integer select, clamp, rotations, count width handling, and integer absolute value. Added executable math and bit fixtures. Native and Phase 1 checks pass.
- 2026-08-28: Added shared Wasm object-layout constants to the compiler and core. Both sides compile-time check the handle field relationships.
- 2026-08-28: Rejected a `bits_of()` and `string(dval(value))` formatter. It changed infinity and NaN output and added DValue allocation and host imports.
- 2026-08-28: Replaced the three handwritten scalar formatter bodies with private Capy wrappers around the existing typed formatter ABI. The conversion path still makes one Wasm call followed by one formatter call. The comparison unit grew from 5,937 to 6,115 bytes. It did not add allocations or formatter calls.
- 2026-08-28: build_linux, service restart, native, Phase 1, reference, guide, documentation, compile-timeout, CLI, golden write, and golden check passed after the formatter migration.
