# Capy LSP

## Objective and invariants

Add an LSP mode and a batch check mode to `capyc`.

Keep these invariants:

- Reuse the current parser and compiler for each complete buffer.
- Preserve compiler output and existing compiler behavior.
- Keep the LSP independent from the Bearer runtime and `src/lib` JSON code.
- Use stdio or one private Unix socket connection.
- Preserve correct codepoint, UTF-16, and UTF-32 position conversion.
- Report one compiler diagnostic because the compiler stops at the first error.
- Do not modify unrelated work already present in the tree.

## Success criteria

- [x] `capyc --lsp` implements the requested lifecycle, document, language, and semantic token methods.
- [x] `capyc --lsp --socket PATH` serves one connection through a mode `0600` Unix socket and removes the socket.
- [x] `capyc --check FILE` and `capyc --check -` produce the required exit status and diagnostic format.
- [x] `scripts/test_capy_lsp.py` tests the real binary, UTF-8 positions, markup contexts, socket mode, and check mode.
- [x] The compiler build passes without warnings.
- [x] The artifact golden test passes without a golden update from this work.
- [x] The native and phase-one tests pass.
- [x] The LSP test passes.
- [x] New LSP source and test code stays near the 1,800-line budget.

## Current state

- Status: complete
- Source: `/root/mount_ssh/capy-bearer`
- Runtime and tests: `root@capy-bearer-dev:/Code/capy-bearer`
- Existing unrelated changes: the initial `git status` listed changes outside this work, including an earlier golden update.

## Goal tree

- [x] G1: Define and connect the command modes. Verify with a clean compiler build.
  - [x] G1.1: Add the LSP source boundary and build integration. Verify that `capyc --lsp` starts.
  - [x] G1.2: Add `--check` for files and stdin. Verify good and bad inputs.
- [x] G2: Implement the LSP protocol and transport. Verify stdio and socket handshakes.
  - [x] G2.1: Add bounded JSON-RPC framing and a local JSON codec.
  - [x] G2.2: Add lifecycle, cancellation, configuration, and private socket cleanup.
  - [x] G2.3: Add full document synchronization, debounce, compilation, and diagnostics.
- [x] G3: Implement language features. Verify each result through protocol requests.
  - [x] G3.1: Add declaration extraction, symbols, hover, completion, signatures, and definitions.
  - [x] G3.2: Add UTF-16 and UTF-32 position conversion.
  - [x] G3.3: Add lexer-based semantic tokens and distinct markup contexts.
- [x] G4: Add acceptance coverage. Verify all required assertions against the real binary.
  - [x] G4.1: Add `scripts/test_capy_lsp.py`.
  - [x] G4.2: Fix only failures within this task scope.
- [x] G5: Run final acceptance. Verify all five gates and inspect the final diff.
  - [x] G5.1: Run the build and artifact golden gates.
  - [x] G5.2: Run the native, phase-one, and LSP gates.
  - [x] G5.3: Review the diff, line count, delegate cleanup, and remaining work.

## Next

1. No implementation work remains.

## Decisions, assumptions, and risks

- Use ABI version zero for diagnostics because check mode does not publish an artifact.
- Parse documents separately for language features and token data.
- Compile the same text for semantic diagnostics.
- Keep document analysis in memory. Do not add incremental state.
- The existing compiler cancellation check has a 4,096-byte granularity.
- A single-threaded event loop cannot receive cancellation during a synchronous compile. The implementation needs a worker or scheduled analysis.
- Existing unrelated changes can affect broad tests. The baseline build and artifact golden gate pass.

## Evidence

- 2026-09-03: The baseline compiler build passed on `capy-bearer-dev`.
- 2026-09-03: The baseline artifact golden gate reported `Capy Wasm and source-map golden artifacts passed`.
- 2026-09-03: The compiler build passed after the command dispatch and LSP source integration.
- 2026-09-03: Check mode returned zero for valid file input. It returned one with exact locations for invalid file and stdin input.
- 2026-09-03: The adversarial review found an unbounded workspace scan. The final code limits files, total bytes, per-file bytes, and scan time.
- 2026-09-03: The final compiler build passed without output.
- 2026-09-03: The final artifact gate reported `Capy Wasm and source-map golden artifacts passed`.
- 2026-09-03: The final native gate reported `native Capy frontend, Wasm, compiler, CLI, and tracked fixture checks passed`.
- 2026-09-03: The final phase-one gate reported `Capy phase 1 parser/direct-Wasm/CLI smoke passed`.
- 2026-09-03: The final LSP gate reported `Capy LSP and check-mode tests passed`.
- 2026-09-03: The new LSP source and test contain 1,424 lines. The size budget permits about 1,800 lines.
