# /doc Documentation Overhaul

## Objective

Implement the DOCS-TODO.md overhaul for the UCE `/doc` system on the sshfs source-of-truth tree, verifying builds/tests only on `root@10.4.2.110:/Code/uce.openfu.com/uce`.

## Success Criteria

- [x] Phase 1 infrastructure complete: title bug, live examples, CSS, format spec page.
- [x] Example mechanism verified end-to-end on host before broad content authoring.
- [x] Phase 2 example coverage sweep: all doc pages now have `:example`; filler/code-fence cleanup applied, resource-bound fences only.
- [x] Phase 3 doc rendering gate added.
- [x] Current host gate passes with 0 failed.

## Current State

- Status: verifying; Phase 1 + gate complete, StringList conversion complete, Phase 2 example/filler sweep complete.
- Last updated: 2026-06-16
- Source of truth: `/root/mount_ssh/uce-dev-root-htdocs-uce`
- Runtime/live target: `root@10.4.2.110:/Code/uce.openfu.com/uce`

## Goal Tree

Legend: `[ ]` not started, `[~]` in progress, `[x]` done, `[!]` blocked, `[-]` superseded

- [x] G1: Phase 1 infrastructure
  - Done when: live examples render source/output on the host and title/sidebar/spec fixes are in place.
  - Verify: host build/restart/curl spot check.
  - [x] G1.1: Fix title override behavior and redundant title blocks.
  - [x] G1.2: Add `:example` parser, materializer, executor, renderer.
  - [x] G1.3: Add sidebar/example CSS.
  - [x] G1.4: Add documentation format page.
  - [x] G1.5: End-to-end host verification with one page example.
- [x] G2: Phase 2 content sweep by area
  - Done when: all 257 pages match canonical template and examples render.
  - Verify: area batch curls and host gate stays green.
- [x] G3: Phase 3 CLI doc gate
  - Done when: CLI runner checks every doc page and examples.
  - Verify: full host gate passes.

## Execution Queue

1. G2 area batching/delegation now that the mechanism is proven.
2. Keep the host suite green after each area batch.

## Decisions

- 2026-06-16: Follow DOCS-TODO.md exactly; no git commit/push/tag.

## Evidence and Verification Log

- 2026-06-16: Read `DOCS-TODO.md`, `site/doc/lib/doc_page.h`, `site/doc/index.uce`, `site/tests/cli_runner.uce`.
- 2026-06-16: Host setup/build/restart/curl for `2_DValue_filter` showed source and captured output `Ada`.
- 2026-06-16: Full host gate passed: `Summary: 349 passed, 0 failed, 0 skipped`.
- 2026-06-16: Converted retiring `list_unique/list_sort/list_some/list_every/list_find` free functions to `StringList` methods, updated callers and method docs; full host gate passed: `Summary: 353 passed, 0 failed, 0 skipped`.
- 2026-06-16: Added live examples across types, string, regex, time, markup, noise, uri, and session area pages; full host gate passed: `Summary: 353 passed, 0 failed, 0 skipped`.
- 2026-06-16: Added `:example` blocks to all remaining doc pages, removed full PHP/JS Related Concepts filler, removed duplicate content code fences except resource-bound pages; full host gate passed: `Summary: 353 passed, 0 failed, 0 skipped`.

## Change Log

- 2026-06-16: Created initial goal tree.
