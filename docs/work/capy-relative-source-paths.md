# Capy relative source paths

## Objective and invariants
Make Capy artifacts independent of the absolute checkout path. Keep diagnostic paths, LSP URIs, runtime cache invalidation, and the unit ABI shape unchanged.

## Success criteria
- [x] The compiler embeds a normalized relative source path when the source is under a configured source root.
- [x] The compiler never embeds an absolute fallback path when the source is outside the root or no root exists.
- [x] Virtual source paths pass through unchanged.
- [x] `capyc` accepts `--source-root DIR` and defaults the root to the current directory.
- [x] The Bearer runtime uses its configured site directory as the source root.
- [x] Diagnostics keep their current source path.
- [x] Backtraces contain a relative path that a developer can resolve from the site root.
- [x] A regression gate compares artifacts compiled from two absolute directories.
- [x] All acceptance gates pass.
- [x] The golden update changes exactly 282 hash entries and does not change the file structure.

## Current state
- Status: complete
- Source/runtime: `/root/mount_ssh/capy-bearer`, `root@capy-bearer-dev:/Code/capy-bearer`
- Initial working tree: clean

## Goal tree
- [x] G1: Separate diagnostic and embedded source identities. Done/verify: focused compiler tests pass.
  - [x] G1.1: Add source-root normalization to `CompileOptions`. Done/verify: root, fallback, separator, and virtual-path cases pass.
  - [x] G1.2: Use the embedded identity in the ABI, source map, and backtrace records. Done/verify: no absolute source identity remains in artifacts.
  - [x] G1.3: Keep parser locations and diagnostics unchanged. Done/verify: an absolute invalid source reports its absolute path.
- [x] G2: Configure compiler entry points. Done/verify: CLI and runtime builds pass.
  - [x] G2.1: Add `capyc --source-root DIR` with a current-directory default. Done/verify: CLI artifacts contain repository-relative paths.
  - [x] G2.2: Set the runtime source root to the configured site directory. Done/verify: phase 1 runtime compilation passes.
- [x] G3: Add reproducibility coverage. Done/verify: the two-directory test passes through the native gate.
  - [x] G3.1: Add and wire a two-directory artifact comparison script. Done/verify: native test gate runs it.
  - [x] G3.2: Prove two separately built checkouts produce equal fixture hashes. Done/verify: Wasm and source-map hashes match.
- [x] G4: Refresh expected artifacts. Done/verify: exactly 282 existing hashes change with no structural diff.
- [x] G5: Run all acceptance gates. Done/verify: all nine requested gates pass on the host.

## Next
1. Review the uncommitted diff.

## Decisions, assumptions, and risks
- Use the basename when no root exists or the source lies outside the root. This fallback cannot expose a machine directory.
- Preserve strings that contain `://` as virtual source paths.
- Backtraces will print relative paths. The runtime has no source-root field in its source-map renderer. Relative output also removes the existing checkout-dependent assertion.
- Use the configured runtime site directory as the source root. Runtime unit resolution already uses this directory.
- Do not change the runtime `source_path=` cache metadata. It tracks the real source file for staleness checks.
- Do not bump the unit ABI version. Only a metadata value changes.

## Evidence
- 2026-09-03: `git status --short` produced no output before edits.
- 2026-09-03: Existing code uses the diagnostic source path for the ABI field, source-map file record, and in-Wasm backtrace records.
- 2026-09-03: `scripts/build_capy.sh` passed after the compiler and CLI changes.
- 2026-09-03: A manual CLI check embedded `site/tests/capy-arc.capy`. An invalid `/tmp` source still reported its absolute path.
- 2026-09-03: `scripts/test_capy_native.sh` passed. It includes the new two-directory regression gate.
- 2026-09-03: `scripts/build_core_wasm.sh` and `scripts/build_linux.sh` passed.
- 2026-09-03: The phase 1 gate passed after the development service restarted with the new binary.
- 2026-09-03: Runtime backtraces now use paths such as `tests/capy-backtrace.capy`, relative to the configured site directory.
- 2026-09-03: The golden preview had 282 entries and 282 changed hashes. Names and order matched the previous file.
- 2026-09-03: The golden write changed 282 existing lines and preserved all 282 entry names. The plain golden gate passed.
- 2026-09-03: The reference, LSP, grammar, and VS Code gates passed. `npm install` reported no vulnerabilities.
- 2026-09-03: Two fresh compiler builds produced Wasm hash `4eb8e9b05a74d23f4b0f9253c2dd8ff1f6d0f4c51ccce45046014b741cfff4db` in both directories.
- 2026-09-03: The same proof produced source-map hash `98b92d3a6adbc348681666439975f7d9dc8bddc1e71edbad006c382b84660df7` in both directories.
- 2026-09-03: An adversarial review found no functional issue. The strongest remaining assumption is lexical source-root containment, which is intentional.
