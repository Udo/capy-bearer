# Final API and documentation review

## Objective and invariants

Review the public Capy API and its documentation after the API reduction work. Keep the public surface small. Keep documented behavior consistent with the compiler and runtime. Preserve existing behavior unless a defect has one clear correction.

The source of truth is `/Code/capy-bearer` on `capy-bearer-dev`. This review covers 232 API pages, the type pages, 13 guides, the language specification, and the related runtime paths.

## Current state

- Status: complete
- Baseline commit: `be28ae3`
- Runtime: CT 122
- Review areas: values and types, strings, request and response, components and units, storage, filesystem, process and concurrency, crypto, time, miscellaneous APIs, and guides

## Fixed

### Redundant survivors

No public survivor remains for `component_exists`, `float_val`, `sha256_hex`, `hmac_sha256_hex`, `ascii_safe_name`, `memcache_escape_key`, `shell_spawn`, `int_val`, or the deleted database accessors.

The standard library, parity manifest, tests, and documentation expose `json_consume_space`. The review corrected the API coverage manifest, which alone classified it as internal.

### Unconsolidated families

No incorrect field-accessor family remained in the reviewed API. `mysql_info`, `sqlite_info`, and `posix_info` own their respective field access.

The review corrected the `mysql_info` examples. They now use supported named parameters instead of rejected positional placeholders. The SQLite page now states that each result keeps its natural DValue type.

### Signature drift

The review corrected stale `s32` index and literal text in these files:

- `docs/capy-language.md`
- `site/doc/source/type/array.txt`
- `site/doc/source/guide/04-expressions-and-control-flow.txt`
- `site/doc/source/guide/07-collections-and-records.txt`
- `site/doc/source/guide/12-errors-testing-and-style.txt`

The language specification now lists the complete numeric grid. It also removes the deleted tuple and markup types from type, constructor, aggregate, and ownership sections.

Generated signature checks found no `:sig` mismatch.

### Stale prose

The review corrected these errors:

- `site/doc/source/type/request.txt` now names `request.out.headers` and `request.out.cookies`.
- `site/doc/source/api/component_render.txt` no longer claims that a missing component writes a banner.
- `site/doc/source/api/file_stat.txt` lists `mtime`, `ctime`, and `mode`.
- `site/doc/source/api/file_read.txt` and `file_pread.txt` state the 16 MiB limit.
- `site/doc/source/api/is_array.txt` describes both map and list values.
- `etc/bearer/settings.cfg` uses the current documentation route.
- `site/doc/source/guide/11-tasks-and-jobs.txt` uses the tracked task example and current command terminology.
- `site/doc/source/guide/01-install-and-first-program.txt` describes HTML literals as strings.

### Examples and call defaults

The two-argument `unit_call(file_name, function_name)` form now sends `{}`. The two-argument `call(handle, name)` form already sent `{}`.

Before the correction, the callee observed `""` through `unit_call` and `{}` through `call`. The module fixture now encodes both values as JSON and expects `{}` for each path.

The review also replaced obsolete explicit zero-handle comparisons in the MySQL and memcache examples. It corrected three MySQL examples that used unsupported positional placeholders.

### Noise API consolidation

The review replaced `gen_float`, `gen_int`, `gen_noise32`, and `gen_noise01` with one generic function:

```capy
function gen_noise(min : any, max : min::type, index : u64, seed : u64) min::type
```

The compiler uses `parameter::type` for a dependent result and for a later parameter. The referenced parameter must be an earlier `any` parameter. The compiler no longer accepts `type(parameter)`.

`gen_noise` supports `u64` and `f64` bounds. Integer bounds are inclusive. Float generation uses the prior fixed precision. `gen_noise64` remains available for raw 64-bit deterministic noise.

This consolidation changes deterministic output for old `gen_noise32` and `gen_noise01` calls. It also removes the optional float precision argument and the default seed. Udo approved this breaking consolidation. Callers must port these choices explicitly.

### Build correctness

`scripts/build_capy.sh` previously regenerated `stdlib.embedded.h` after it built `capyc`. One build could therefore produce a compiler with the prior standard library. The script now updates the embedding before the final compiler build. A clean bootstrap still uses a temporary compiler to generate the header.

## Recommend

### Align render result types in a separate ABI task

`component_render` returns `bool`. `unit_render` returns no value. Keep `component_render` unchanged because callers use its resolution result.

Change `unit_render` only in a separate compatibility task. A result change modifies the Wasm host import type. The task must rebuild cached units and update native fallback behavior, tests, signatures, documentation, and artifact hashes.

### Decide whether `find` should remain

`find(value, needle)` and `strpos(value, needle, offset = 0)` overlap. `strpos` is the larger operation. Confirm source compatibility requirements before one function is removed.

### Confirm the string-find width boundary

Public `find` returns `s64`, but `__bearer_string_find` returns `s32`. Prove that configured string limits make the narrow boundary unreachable, or change the host boundary to `s64`.

### Keep legacy host operations only for a stated compatibility need

The public Capy API no longer has `password_needs_rehash`. Its legacy host operation remains in native and Wasm runtime code. Keep it only if old compiled units require that ABI.

## Verified clean

- Generated signatures match current public declarations.
- Numeric constructors cover all signed, unsigned, and floating-point types.
- Bit functions preserve their declared integer widths.
- DValue, array, collection, codec, request, response, session, CSRF, routing, output, filesystem, storage, process, task, crypto, and time signatures match the reviewed source.
- The measured string host paths remain consistent with `docs/work/string-hostpath.md`.
- No new standard-library import was added by the empty-input correction.
- The current corrections add no hosted automation or external operational dependency.

## Verification

- [x] `git diff --check`
- [x] `generate_capy_doc_signatures.py --check`
- [x] `check_capy_doc_examples.py`
- [x] `test_capy_doc_generators.py`
- [x] Generated the embedded standard library and documentation after the noise API change.
- [x] Built and restarted Bearer after the noise API change.
- [x] Ran native, phase-one, reference, guide, documentation, timeout, and CLI gates.
- [x] Regenerated artifact hashes last and verified them without write mode.
- [x] Measured the minimal and ten-call startup fixtures.

The first full CLI run reached `capy-unit-admin.capy` before a cold-path invocation timeout. A direct warm run passed. The immediate complete rerun then passed. No source change was necessary.

The final startup benchmark used 10 runs. The minimal fixture is 4,070 bytes with a 0.010 second median. The ten-call fixture is 17,857 bytes with a 0.020 second median. Unit sizes match the latest recorded baseline.

The final adversarial review identified a failed-build consistency risk. The build script now restores the prior embedded standard library if the final compiler build fails. A forced-failure test verified the rollback. Managed-string specialization and unsupported `gen_noise` types now have focused tests.
