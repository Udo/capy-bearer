# Capy API surface removals

## Objective and invariants

Remove redundant or misleading Capy APIs without aliases or compatibility shims.

- Keep `safe_name`. Remove `ascii_safe_name` and its duplicate host operation.
- Replace every `component_exists` call with `length(component_resolve(name)) > 0`.
- Normalize memcache keys at the host boundary. Document key collisions.
- Make all digest functions return raw bytes. Use lowercase `hex()` for text.
- Keep the one-argument `shell_exec` behavior. Add a strict two-argument background overload.
- Remove the Capy `password_needs_rehash` surface. Preserve required host behavior.
- Regenerate the artifact golden file after all other changes and gates.

## Success criteria

- [x] Removed symbols fail compilation and have no pages, manifests, or call sites.
- [x] Replacement behavior has focused runtime tests.
- [x] All required acceptance gates return zero.
- [x] The artifact golden write and plain check are the final validation steps.

## Current state

- Status: complete
- Source/runtime: `/root/mount_ssh/capy-bearer`, `root@10.4.2.122:/Code/capy-bearer`

## Goal tree

- [x] G1: Implement runtime and standard-library changes.
- [x] G2: Port callers and add focused tests.
- [x] G3: Remove and update authored documentation.
- [x] G4: Regenerate generated documentation and manifests.
- [x] G5: Run acceptance gates and regenerate the golden file last.

## Decisions

- `memcache_command` needs protocol-aware key normalization. It must not replace whitespace in the complete command.
- Public BRRB and callable dynamic-value behavior from the preceding task must remain unchanged.
