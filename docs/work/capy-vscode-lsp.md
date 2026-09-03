# Capy editor support

## Objective and invariants

Move the existing VS Code extension into this repository and align it with the current Capy language.

Keep these invariants:

- Treat `docs/capy-language.md`, `src/capy/frontend.cpp`, and `src/capy/stdlib.capy` as authoritative.
- Generate grammar word lists from the authoritative files.
- Keep TextMate highlighting active when the language server runs.
- Start `capyc --lsp` through stdio without a bundler or TypeScript build.
- Preserve single-file and stdin check behavior.
- Report one diagnostic for each checked file.
- Do not modify `/root/projects/capy` or unrelated work in the target tree.
- Do not add hosted automation, package publishing, or package payload changes.

## Success criteria

- [x] `editors/vscode/` contains the extension without `node_modules`.
- [x] The grammar matches current comments, directives, declarations, types, handlers, operators, strings, and HTML literals.
- [x] `scripts/generate_capy_grammar.py --check` detects grammar drift.
- [x] The extension starts `capyc --lsp` with the specified path order and settings.
- [x] The extension README covers Visual Studio Code, Neovim, Helix, Zed, and Emacs eglot.
- [x] `capyc --check` accepts multiple files and recursive directories.
- [x] `AGENTS.md` and `CLAUDE.md` document check mode.
- [x] All seven acceptance gates pass on `capy-bearer-dev`.

## Current state

- Status: complete
- Source: `/root/mount_ssh/capy-bearer`
- Build and tests: `root@capy-bearer-dev:/Code/capy-bearer`
- Base commit: `daa4ee2`
- Existing unrelated modifications remain outside this task.

## Goal tree

- [x] G1: Move and correct the grammar. Verify the extension tests.
  - [x] G1.1: Copy the selected extension files without dependencies.
  - [x] G1.2: Add the grammar generator from authoritative language files.
  - [x] G1.3: Correct the grammar, editor rules, and grammar tests.
- [x] G2: Connect editor clients to the server. Verify the extension manifest and client tests.
  - [x] G2.1: Add the JavaScript VS Code language client and settings.
  - [x] G2.2: Use only standard semantic token types in the server legend.
  - [x] G2.3: Document setup for the supported editors.
- [x] G3: Extend batch checks. Verify files, directories, stdin, and aggregate failure status.
  - [x] G3.1: Expand `capyc --check` path handling.
  - [x] G3.2: Extend `scripts/test_capy_lsp.py`.
- [x] G4: Document the agent command. Verify both agent files contain concise current instructions.
  - [x] G4.1: Add `AGENTS.md` and `CLAUDE.md`.
- [x] G5: Run final acceptance and review. Verify all gates and inspect only task changes.
  - [x] G5.1: Run the compiler and artifact gates.
  - [x] G5.2: Run the native, phase-one, and LSP gates.
  - [x] G5.3: Run the extension and grammar generator gates.
  - [x] G5.4: Run an adversarial review and inspect the final status.

## Next

1. No implementation work remains.

## Decisions, assumptions, and risks

- Use `vscode-languageclient` as a runtime dependency.
- Keep dependency versions pinned in `package-lock.json`.
- Map every semantic token to a standard LSP token type.
- Bound directory recursion and sort paths for stable check output.
- The phase-one gate can require a Bearer rebuild after compiler source changes.
- Concurrent work exists in the repository. Only this task's paths can change.

## Evidence

- 2026-09-03: The target tree starts at commit `daa4ee2` with only the listed unrelated modifications.
- 2026-09-03: The source extension has obsolete comments, directives, declarations, types, strings, and HTML rules.
- 2026-09-03: The UdonScript extension confirms the small direct extension layout.
- 2026-09-03: The generated grammar check passes.
- 2026-09-03: The extension tests pass for the corrected grammar and server path resolution.
- 2026-09-03: The compiler build and LSP test pass with recursive and multi-path check coverage.
- 2026-09-03: The semantic token test proves that all legend entries use standard token types.
- 2026-09-03: Both agent instruction files document file, multi-path, directory, and stdin checks.
- 2026-09-03: `vscode-languageclient` reads `capy.trace.server` from the client ID. The client configuration test fixes that ID as `capy`.
- 2026-09-03: The final compiler build passed without warnings.
- 2026-09-03: The artifact gate reported `Capy Wasm and source-map golden artifacts passed`.
- 2026-09-03: The native gate reported `native Capy frontend, Wasm, compiler, CLI, and tracked fixture checks passed`.
- 2026-09-03: The phase-one gate reported `Capy phase 1 parser/direct-Wasm/CLI smoke passed` after a development binary rebuild.
- 2026-09-03: The LSP gate reported `Capy LSP and check-mode tests passed`.
- 2026-09-03: The extension gate passed all manifest, grammar, and server path tests.
- 2026-09-03: The final generated grammar check passed without output.
