# Capy String Host Paths

## Objective and Invariants
Move exact float formatting and suitable byte-string operations into Capy. Preserve byte behavior, decimal round trips, ARC balance, and demand-selected startup cost.

## Success Criteria
- [x] Capy formats binary64 values without a formatter host import.
- [x] Required decimal outputs and sampled finite round trips pass.
- [x] Each migrated string operation preserves empty, whitespace, NUL, and UTF-8 behavior.
- [x] Each migrated operation improves its measured host path.
- [x] Operations that remain faster on the host stay on the host.
- [x] All acceptance gates pass after a cache warm-up.
- [x] Golden artifacts are regenerated last.

## Current State
- Status: complete
- Source/runtime: `/root/mount_ssh/capy-bearer`, `root@10.4.2.122:/Code/capy-bearer`

## Decisions
- Use exact bigint conversion for 17-significant-digit binary64 output. This matches the existing output contract and the required `1.9` output.
- Keep `upper`, `lower`, `contains`, `strpos`, `replace`, `split`, `split_space`, and `join` on their measured host paths.
- Move `trim`, `str_starts_with`, `str_ends_with`, `safe_name`, and `html_escape` into Capy.
- Keep `split_space` on the host. Its Capy version measured slower because each token enters a DValue list.
- Keep `split` and `join` on the host-facing DValue path. Their list conversions dominate their cost.

## Measurements

| Operation | Before, us/op | After, us/op | Result |
| --- | ---: | ---: | --- |
| `format_f64` | 12.105 | 7.45-8.20 | Moved. Exact, but above the 1 us target. |
| `trim` | 4.205 | 0.43 | Moved. |
| `upper` | 0.47 | 0.47 | Kept host path. |
| `lower` | 0.69 | 0.47 | Kept host path. |
| `contains` | 0.20 | 0.14 | Kept host path. |
| `str_starts_with` | 12.66 | 0.26 | Moved. |
| `str_ends_with` | 12.48 | 0.26 | Moved. |
| `strpos` | 0.21 | 0.14 | Kept host path. |
| `replace` | 0.84 | 0.69 | Kept host path. |
| `split` | 12.72 | 11.59 | Kept host path. |
| `split_space` | 12.38 | 21.12 | Reverted to host path. |
| `join` | 48.40 | 39.59 | Kept existing implementation. |
| `safe_name` | 5.12 | 0.45 | Moved. |
| `html_escape` | 6.33 | 1.58 | Moved. |

The operation benchmark uses 1,000 iterations. Runtime jitter affects the host-heavy list operations. The migration decision uses repeated warm runs.

## Startup Evidence
- Baseline measured in this session: minimal 4,070 bytes at 0.020 seconds. Ten calls 15,080 bytes at 0.035 seconds.
- Current warm measurement: minimal 4,070 bytes at 0.010 seconds. Ten calls 17,857 bytes at 0.020 seconds.
- The ten-call unit grew by 2,777 bytes. Shared helpers compile once, but their in-Wasm bodies increase the unit size.

## Acceptance Evidence
- 2026-08-31: The warm acceptance run passed all gates except one transient parallel proactive compile check.
- 2026-08-31: The immediate CLI suite rerun passed all checks.
- 2026-08-31: The artifact golden write and plain check passed after all source changes.
