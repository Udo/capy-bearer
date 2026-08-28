# Capy Scalar Formatting in Wasm

## Objective and Invariants
Move scalar string formatting into Wasm as Capy code. Do not restore handwritten Wasm formatters. Keep `retain`, `release`, and `clone` handwritten. Preserve scalar output and round trips.

## Success Criteria
- [ ] `string_from_bytes([u8])` and `bytes_of(string)` lower in-module without host imports.
- [ ] Signed and unsigned integer formatters use at most two allocations per format.
- [ ] The integer benchmark is below 0.5 microseconds per operation.
- [ ] Formatter loops do not leak ARC objects.
- [ ] All required scalar and byte conversion cases pass.
- [ ] The float formatter preserves shortest-round-trip output, or remains a host formatter with a documented reason.
- [ ] Documentation, generated artifacts, and all acceptance gates match the source.

## Current State
- Status: executing
- Source/runtime: `/root/mount_ssh/capy-bearer`, `root@10.4.2.122:/Code/capy-bearer`
- Baseline: `string(s64(i))` costs about 1.55 microseconds per operation.
- Budget: less than 0.5 microseconds per operation.

## Goal Tree
- [x] G1: Add byte and string conversion primitives. Verify no integer formatter host import.
- [x] G2: Add single-reservation integer formatters. Verify exact output and ARC balance.
- [x] G3: Keep the host float formatter. It preserves shortest-round-trip output.
- [x] G4: Measure the specified benchmark before and after.
- [x] G5: Add documentation and test coverage.
- [ ] G6: Run all gates and regenerate golden artifacts last.

## Decisions / Assumptions / Risks
- The staged formatter host ABI costs two boundary crossings and a copy.
- Reserve the byte array before adding digits. Do not concatenate strings per digit.
- Keep the float host formatter if Capy cannot preserve exact output.

## Evidence
- 2026-08-28: The baseline host `s64` formatter measured about 1.55 microseconds per operation. A naive Capy formatter with per-digit string operations measured 1.565 microseconds.
- 2026-08-28: The Capy integer formatter uses one reserved byte array and one result string. The warm benchmark results were 0.306, 0.320, and 0.317 microseconds per operation. The formatter loop preserved ARC count.
- 2026-08-28: Kept the typed host `f64` formatter. It preserves `0.1 + 0.2` as `0.30000000000000004`. A Capy shortest-round-trip formatter remains impractical without a Ryu-class implementation.

## Next
1. Inspect the compiler paths for native string and byte-array allocation.
