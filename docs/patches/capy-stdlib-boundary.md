# Capy embedded-stdlib typed-host boundary

**Baseline:** `616c1d4`
**Source changes:** `src/capy/{frontend.*,compiler.cpp,stdlib.capy,stdlib.embedded.h}`, `src/wasm/core.cpp`, `site/tests/`, and Capy test/golden scripts.

`src/capy/stdlib.capy` is the tracked source and `src/capy/stdlib.embedded.h` is its generated embedding. Public Capy APIs are ordinary, demand-selected stdlib declarations. The embedded source alone may declare private `__bearer_*` typed host bindings; user source cannot declare or call those private implementation names.

The compiler has no Bearer API-name lowering or host signature registry. It collects selected typed declarations and uses one generic ABI lowerer: scalar values pass directly; strings and copied DValues pass as pointer/length spans; string/DValue results use the existing two-pass sized-result transport. A failed sizing call releases owned managed inputs before trapping at the marked user callsite. Virtual stdlib frames remain suppressed.

`bearer_dv_apply_brrb` is now only copied-DValue mechanics. Its decode/stage/copy transport is shared by cohesive host capabilities. The former mixed core dispatcher is split into `bearer_text_parsing_brrb`, `bearer_route_path_brrb`, and `bearer_runtime_diagnostics_brrb`; each has function-local named C++ operation constants. Regex splitting uses the existing typed regex DValue result host instead of a text-family discriminant.

Focused live coverage invokes a stale SQLite sized-result host with a concatenated owned string and constructed DValue. It proves the trap is at the user callsite with no `capy://stdlib.capy` frame, and the next SQLite invocation returns `arc_live()==0`.

Regenerate the embedding with `capyc --embed-stdlib src/capy/stdlib.capy src/capy/stdlib.embedded.h`; regenerate `scripts/capy_artifact_golden.sha256` with `CAPYC=/tmp/capy-native-tests/capyc bash scripts/test_capy_artifact_golden.sh --write` after reviewing import/source-map deltas.
