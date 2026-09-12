# Capy compiler performance

## Objective and invariants

Reduce cold Capy compilation and Wasmtime module preparation time for the 4,237-line Ludum CLI unit.

Preserve these invariants:

1. Preserve Capy behavior, diagnostics, source locations, ownership, and runtime results.
2. Preserve deterministic artifacts unless Udo accepts an optimization that changes the Wasm encoding.
3. Do not slow normal small units or generated runtime hot paths.
4. Keep compilation memory bounded for dense and sparse input.
5. Do not deploy compiler or runtime changes during this work.

## Success criteria

- [x] Measure Capy parsing, analysis, lowering, assembly, Wasmtime compilation, and serialization separately.
- [x] Find a measured reduction in cold preparation time and artifact size.
- [x] Define a regression gate for each candidate that adds code.
- [~] Pass native compiler, Wasm, artifact, source-map, fixture, and runtime checks.
- [x] Record rejected prototypes and their measurements.

## Current state

- Status: deployed. The expected separate approval checkpoint was missed. Four DValue fault-injection paths remain untested.
- Source: `/root/mount_ssh/capy-bearer`
- Benchmark source: `tmp/ludum-cli.capy`
- Source revision: `7e571ed`, including the DValue helper, ownership and MySQL contract repairs, and CLI worker retirement.
- Runtime deployment: CT100 received `7e571ed` through the nightly updater on 2026-09-05 at about 03:47 UTC.
- Saved prototype: `tmp/dval-allocation-helper.patch`

## Goal tree

- [x] G1: Establish a complete cost model.
  - [x] G1.1: Measure cold Wasmtime compile and serialization separately.
  - [x] G1.2: Measure native compiler phases and peak RSS.
  - [x] G1.3: Measure artifact sections and source-marker count.
- [x] G2: Measure generated Wasm reductions.
  - [x] G2.1: Quantify repeated instruction sequences and helper candidates.
  - [x] G2.2: Reject the low-value array-capacity helper.
  - [x] G2.3: Measure marker removal with a valid temporary artifact.
  - [x] G2.4: Prototype and measure a shared DValue allocation helper.
- [~] G3: Reduce native compiler work.
  - [x] G3.1: Retain the direct final-module emitter from revision `5f15696`.
  - [x] G3.2: Measure scope, overload, capability, and lowering costs.
  - [ ] G3.3: Prototype a direct append-only lowerer only if native compile latency becomes important.
- [x] G4: Improve development feedback.
  - [x] G4.1: Attribute C++ compiler build time by translation unit.
  - [x] G4.2: Use four fixture workers without reducing coverage.
- [~] G5: Retain only accepted changes.
  - [x] G5.1: Complete an adversarial review of the DValue helper.
  - [~] G5.2: Pass native and runtime acceptance. Allocation-failure injection remains untested.
  - [-] G5.3: The expected approval before committing changed Wasm and source-map bytes was not recorded. Publication and automatic deployment occurred. This is a missed checkpoint, not completed approval.

## Decision

The DValue helper is the only measured candidate worth retaining. It removes repeated generated code with no detectable runtime cost.

The helper was committed and deployed. The earlier instruction to wait for separate approval did not match the automatic deployment path. The original no-deployment invariant above was not preserved. Do not interpret this correction as retroactive approval.

Do not implement the array-capacity helper. It saves only about 4 KiB and adds a call to each typed-array growth check.

Do not implement marker sidecar metadata for performance alone. Marker removal saved input bytes but had little effect on Wasmtime compilation.

## Measurements

### Baseline cost model

The native compiler phase sample measured these costs:

| Phase | Time |
|---|---:|
| User parse | 38.2 ms |
| Standard library parse | 6.6 ms |
| Source validation | 10.8 ms |
| Standard library selection | 13.3 ms |
| Capability discovery | 35.6 ms |
| Function lowering | 97.0 ms |
| Wasm assembly | 7.1 ms |
| Marker index | 3.0 ms |
| Source-map formatting | 18.1 ms |
| Module total | 171.4 ms |

The 3,598,211-byte artifact has 330 functions. Its code section has 3,232,939 bytes. Its data section has 362,092 bytes.

Wasmtime module compilation dominates cold preparation. An initial three-run sample measured 25.33 seconds median CPU and 2.13 GiB median peak RSS. Host contention made wall time unstable.

A quieter interleaved baseline measured 17.45 seconds median CPU and 5.52 seconds median wall time. Median peak RSS was 1,977 MiB.

Serialization and deserialization are cheap after compilation. Baseline serialization took 30.9 ms. Deserialization took 14.7 ms in the interleaved run.

### Source markers

The artifact has 43,850 ten-byte source markers. They consume 438,500 code bytes, or 12.2% of the artifact.

A temporary stripping pass produced valid Wasm. It reduced the artifact to 3,159,709 bytes.

An interleaved three-pair comparison reduced Wasmtime compile CPU by 2.5%. Serialized output stayed at 25,178,200 bytes.

The earlier non-interleaved result showed a false 31% CPU reduction. Host load caused that result. Do not use it.

A structured marker sidecar would touch most lowerer composition paths. The 2.5% result does not justify that risk.

### Repeated generated code

`allocate_dval()` appears 10,854 times in the Ludum artifact. Its inline sequences consume 1,832,429 bytes.

The callable-retention loop appears 4,805 times and consumes 230,093 bytes. ARC retain and release operations already use shared helpers.

`array_ensure_capacity()` appears only 33 times. Its inline sequences consume about 5.3 KiB.

### DValue allocation helper

The prototype moves allocation, header setup, and ARC increment into one private Wasm function. Each caller keeps cleanup, its source marker, and its trap.

| Metric | Baseline | DValue helper | Change |
|---|---:|---:|---:|
| Wasm artifact | 3,598,211 B | 2,504,778 B | -30.4% |
| Source map | 823,284 B | 514,151 B | -37.5% |
| Wasmtime compile CPU | 17.45 s | 10.96 s | -37.2% |
| Wasmtime compile wall | 5.52 s | 3.35 s | -39.3% |
| Wasmtime peak RSS | 1,977 MiB | 1,324 MiB | -33.0% |
| Serialized module | 25,178,200 B | 17,053,104 B | -32.3% |
| Native compiler peak RSS | 33.89 MiB | 27.53 MiB | -18.8% |

A precise 16-run native comparison found no CPU improvement. Median native CPU changed by +0.7%, within noise. Median wall time changed by -3.2%.

A CT122 stress route made 100,000 DValue allocations per request. Across 240 measured requests, median runtime changed from 423.358 ms to 423.316 ms.

The measured runtime change was -0.01%. The additional Wasm call had no detectable cost in this test.

The helper preserved all 9,843 unique source coordinates. It removed duplicate rows at identical allocation locations.

Three repeated builds produced byte-identical Wasm and source maps when each build used the same output basename.

The full native suite passed with 483 fixtures. `wasm-validate` and the artifact golden test also passed.

The adversarial review found one dependency concern. The saved patch makes DValue allocator imports and the ARC global explicit.

### Native C++ build cost

`compiler.cpp` dominates a native rebuild. One release compile took 108.03 seconds under host contention.

A debug `compiler.cpp` compile took 22.73 seconds. The full debug source compile took 50.09 seconds.

The current build command recompiles all six translation units. An object cache could cut a compiler-only debug rebuild by about 54%.

Do not split `compiler.cpp` only to reduce build time. Cached objects offer the same feedback benefit with less architecture risk.

## Validation repairs found during the Ludum port

The Ludum validation exposed two runtime contract defects that are separate from the DValue helper prototype.

- The Wasm MySQL adapter now returns an empty array for empty row sets, statements without rows, SQL errors, and invalid handles. `mysql_info(handle, "error")` remains the error channel. Invalid handles now report a stable error and return false or zero for scalar status fields. The phase-1 fixture covers empty iteration, SQL errors, disconnects, stale handles, reconnects, and unavailable databases.
- The compiler now propagates ownership through the no-op `string(string)` constructor. It does not classify a local callable named `string` as that constructor. The regression fixture covers borrowed and owned values, nested identity calls, assignment, reassignment, returns, direct calls, a local callable, a host print call, literals, and final ARC balance.

The rebuilt release compiler and runtime passed the native suite and the phase-1 suite. The artifact golden test and reproducibility test also passed. The intentional compiler correction changed only the constructor fixture output among previously unchanged compiler fixtures. The MySQL fixture changed because its coverage increased. The golden file now includes the new string-identity fixture.

## Verification gaps

The DValue helper still needs direct fault injection for these paths:

1. A negative host-returned length.
2. A failed payload allocation.
3. A failed handle allocation after payload success.
4. A zero-length allocation with header and ARC inspection.

Each fault test must verify cleanup count, payload release, ARC count, and mapped source location.

## Next

1. Track the four live fault-injection gaps above. The deployment-automation task does not test them.
2. Use the explicit `DEPLOY_APPROVAL` checklist marker from `AGENTS.md` for future changes that require approval before deployment.
3. Keep this record's missed checkpoint visible. Do not claim that a completed commit implies separate deployment approval.

## 2026-09-04 isolated acceptance

The DValue allocation helper was applied after review. This changed Wasm and source-map bytes as expected. The dense-DValue native gate, artifact golden gate, and cross-directory reproducibility gate pass.

The current Ludum CLI source produces a 2,571,192-byte Wasm artifact and a 17,459,328-byte serialized module. Standalone Wasmtime preparation peaked at 1,296,348 KiB. The native compiler used 30,948 KiB and completed in 0.40 seconds.

A runtime lifecycle change now serializes Capy CLI execution. A worker retires after it serves a CLI artifact larger than 1 MiB. The worker closes its listeners and unrelated clients. It uses an output-only drain for the completed CLI response. The CLI client retries only the explicit `BEARER_CLI_BUSY` response while the prior worker releases the admission lock. It does not impose a second timeout that can expire before the configured worker drain period.

The lifecycle passed these checks on isolated CT105:

1. A 1,200,000-byte response arrived without truncation.
2. A slow reader received the complete response.
3. A peer disconnect did not prevent worker replacement.
4. Two concurrent large CLI calls completed without concurrent module residency.
5. A normal HTTP page returned status 200 during the test.
6. The complete Ludum suite passed 681 checks across 34 groups.
7. The Bearer cgroup recorded no OOM event. Memory returned below 700 MiB after worker replacement.
8. A CLI request waited behind the admission lock for 12 seconds. It completed after 13 seconds without a false failure.

The highest observed service memory peak was 1,993,117,696 bytes under the 3 GiB service limit. Those measurements came from the isolated test deployment.

## 2026-09-12 deployment record correction

This record previously ended with: "Production remains unchanged. Production deployment still requires explicit approval." That claim was wrong after 2026-09-05 at about 03:47 UTC.

The nightly CT100 updater deployed `7e571ed` to production. That revision includes the DValue allocation helper, ownership and MySQL contract fixes, and CLI worker retirement. Deployment occurred without the separate approval checkpoint that this record expected.

The author assumed a human separated commit publication from deployment. The nightly updater instead made `origin/main` eligible for deployment without that checkpoint. An ordinary unfinished checklist item did not stop it.

All four paths under Verification gaps are live and still untested by direct fault injection. The new updater approval marker provides a mechanical hold for future work. It does not establish retroactive approval or close these test gaps.
