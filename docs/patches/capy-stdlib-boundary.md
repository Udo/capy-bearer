# Capy standard-library boundary

`src/capy/stdlib.capy` is the tracked source patch for public Bearer wrappers; `src/capy/stdlib.embedded.h` is generated and checked by `capyc --embed-stdlib`.

The public-lowering migration moved request/response, WebSocket, component/unit, regex/codec, file/time, session/CSRF/redirect, string, and DValue convenience names into ordinary stdlib declarations. The compiler retains only private `__bearer_*` ABI lowerings plus these language primitives: `dval` construction, `dval_has`, scalar extraction (`dval_string`, `dval_s32`, `dval_f64`, `dval_bool`), DValue indexing, `length`, `trusted_markup`, `clone`, `print`, `trap`, and `arc_live`. Native regression checks reject `bearer_string_list`, `__legacy_`, and public names in direct lowering branches; they also prove stdlib source maps and omission of unrelated imports.

The compiler selects reachable declarations with lexical-local awareness, validates the selected combined program, and reserves all stdlib public names plus `__bearer_*` for compiler-owned intrinsics. Runtime source-map reporting suppresses virtual `capy://stdlib.capy` frames in favor of the marked user call site.

DValue map/filter callbacks run in ordinary Capy over copied values; exact-width s64/u64 conversion crosses one typed BRRB adapter and never transits an f64 result. Mutators return the replacement DValue under the documented value-semantics decision.

`backtrace_get_frames` is also an ordinary stdlib declaration, but its Capy contract deliberately differs from the native `void*` formatter: it returns an innermost-first source-mapped guest stack with bounded max/skip overloads. Demand selection emits the private guest shadow stack and `bearer_capy_backtrace` bridge only for callers of this declaration. The stack contains compiler-owned function/source/line/column text, suppresses virtual stdlib frames, and exposes no guest or host addresses. Frame metadata is length-delimited binary. The formatter accepts at most 64 KiB per record; malformed or larger records render one `<invalid frame metadata>` line and do not suppress later frames. It escapes every non-printable byte as `\\xHH` and truncates an oversized escaped function/path field, so one frame always remains one terminal-safe line.

Verified on CT122 with `scripts/test_capy_native.sh`, `scripts/test_capy_phase1.sh`, and HTTP success/trap requests for `capy-dval-api*.capy`. MySQL credentialed runtime coverage was not performed.
