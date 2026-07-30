# Capy embedded-stdlib typed-host boundary

**Baseline:** `616c1d4`
**Source changes:** `src/capy/{frontend.*,compiler.cpp,stdlib.capy,stdlib.embedded.h}`, `src/wasm/core.cpp`, `site/tests/`, and Capy test/golden scripts.

`src/capy/stdlib.capy` is the tracked source and `src/capy/stdlib.embedded.h` is its generated embedding. Public Capy APIs are ordinary, demand-selected stdlib declarations. The embedded source alone may declare private `__bearer_*` typed host bindings; user source cannot declare or call those private implementation names.

The compiler has no Bearer API-name lowering or host signature registry. It collects selected typed declarations and uses one generic ABI lowerer: scalar values pass directly; strings and copied DValues pass as pointer/length spans; string/DValue results use the existing two-pass sized-result transport. A failed sizing call releases owned managed inputs before trapping at the marked user callsite. Virtual stdlib frames remain suppressed.

`bearer_dv_apply_brrb` is now only copied-DValue mechanics. Its decode/stage/copy transport is shared by cohesive host capabilities. The former mixed core dispatcher is split into `bearer_text_parsing_brrb`, `bearer_route_path_brrb`, and `bearer_runtime_diagnostics_brrb`; each has function-local named C++ operation constants. Regex splitting uses the existing typed regex DValue result host instead of a text-family discriminant.

Focused live coverage invokes a stale SQLite sized-result host with a concatenated owned string and constructed DValue. It proves the trap is at the user callsite with no `capy://stdlib.capy` frame, and the next SQLite invocation returns `arc_live()==0`.

`unit_load(path)` returns opaque `module`; `module.call(name[, input])` is an ordinary overloaded stdlib method lowered through the same typed host path. The module value is an i32 capability only at the membrane and cannot enter user aggregates, closures, conversions, or conditions. Calls carry copied BRRB DValues through a request-local two-pass stage so target execution occurs only during sizing. The native worker validates the selected artifact's staged `.exports.txt` declaration as exactly `DValue*(DValue*)` before it resolves a table slot; this admits normalized Capy `EXPORTS`, legacy `EXPORT_name`, and legacy C++ `EXPORT`, but never arbitrary Wasm exports.

Regenerate the embedding with `capyc --embed-stdlib src/capy/stdlib.capy src/capy/stdlib.embedded.h`; regenerate `scripts/capy_artifact_golden.sha256` with `CAPYC=/tmp/capy-native-tests/capyc bash scripts/test_capy_artifact_golden.sh --write` after reviewing import/source-map deltas.
