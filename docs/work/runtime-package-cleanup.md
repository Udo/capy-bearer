# Runtime package cleanup

## Objective and invariants

Make the Bearer package install only the runtime. Follow the useful parts of the PHP-FPM package model.

1. The package must not own an application webroot.
2. Bearer must not run as root by default.
3. The administrator must control process counts and memory limits.
4. Existing source deployments and package upgrades must retain their configuration.
5. Debian and RPM packages must describe the same runtime boundary.

## Success criteria

- [x] Debian and RPM payloads contain no files from `site/`.
- [x] The packaged service runs as an unprivileged account.
- [x] The unit has no fixed total-memory policy.
- [x] The setup guide distinguishes the request memory limit from a systemd service limit.
- [x] The setup guide explains the single-site trust boundary.
- [x] Package checks reject an application webroot in the payload.
- [x] Relevant builds and tests pass.

## Current state

- Status: complete
- Source: `/root/mount_ssh/capy-bearer`
- Runtime: no deployment is part of this change

## Goal tree

- [x] G1: Make the packages runtime-only.
  - [x] G1.1: Remove the site copy and obsolete package options.
  - [x] G1.2: Remove site claims and webroot checks from package metadata.
  - [x] G1.3: Reject accidental site and development payloads during package validation.
- [x] G2: Use an unprivileged default service identity.
  - [x] G2.1: Set the Debian and source service units to `www-data`.
  - [x] G2.2: Give the RPM a dedicated `bearer` account and unit files.
  - [x] G2.3: Migrate ownership of standard mutable state once during package installation.
- [x] G3: Put resource controls in the correct configuration layer.
  - [x] G3.1: Remove the fixed total service memory limit.
  - [x] G3.2: Document `WASM_MEMORY_LIMIT_BYTES`, worker settings, and systemd drop-ins.
- [x] G4: Document the site-root behavior.
  - [x] G4.1: Explain FastCGI routing through `SCRIPT_FILENAME`.
  - [x] G4.2: Explain the shared scan, WebSocket, task, and file-access boundary.
- [x] G5: Verify the complete change.

## Next

1. Commit and publish the reviewed change when approved.
2. Build target-distribution artifacts before any server installation.

## Decisions, assumptions, and risks

- Decision: Use `www-data` on the Debian-oriented units. Ubuntu does not provide a `www-run` account.
- Decision: Use a dedicated `bearer` account in the RPM because RPM web-server account names differ.
- Decision: Keep four request workers and two proactive compiler workers as configurable defaults.
- Decision: Use a systemd drop-in for the total service memory limit. Keep the per-request Wasm limit in Bearer configuration.
- Assumption: One Bearer instance can serve sites that intentionally share one filesystem and runtime trust boundary.
- Risk: Independent sites need separate instances because Bearer has one proactive scan root and one WebSocket document root.

## Evidence

- 2026-09-12: `scripts/make_deb.sh` and `scripts/make_rpm.sh` copy `site/` into the package webroot.
- 2026-09-12: Both service units omit `User` and `Group`, so systemd runs Bearer as root.
- 2026-09-12: `WORKER_COUNT` and `PROACTIVE_COMPILE_JOBS` already come from `/etc/bearer/settings.cfg`.
- 2026-09-12: Normal FastCGI requests execute nginx's `SCRIPT_FILENAME`. Other runtime features still use the global `SITE_DIRECTORY`.
- 2026-09-12: The Debian and RPM builders completed. Payload inspection found no application root, bundled site, source tree, test tree, or development documentation.
- 2026-09-12: The generated Debian package reports an apparent installed size of 44,077 KiB after removing headers and development files.
- 2026-09-12: An isolated runtime started as `www-data`. Its private CLI socket and state paths had `www-data:www-data` ownership.
- 2026-09-12: The package layout test, shell syntax checks, documentation example checks, and `git diff --check` passed.
- 2026-09-12: Adversarial review found an RPM account blocker and three documentation or migration warnings. The changes add RPM-specific units, account provisioning, exact-default migration, a slim payload, and complete process-count guidance.
- 2026-09-12: Final review rejected the world-writable RPM socket and full Wasmtime development tree. The RPM socket now uses `bearer:bearer` mode `0660`. Both packages contain only the Wasmtime shared library and license files.
- 2026-09-12: The extracted package binary started its help path with the packaged Wasmtime shared library.
