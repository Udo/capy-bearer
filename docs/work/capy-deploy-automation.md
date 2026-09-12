# Capy deployment automation

## Objective and invariants

Version the existing CT100 updater. Add compiler regression gates and an explicit deployment approval hold.

- Preserve the existing staging, service health, backup, and retention workflow.
- Change deployment plumbing and documentation only.
- Do not touch concurrent Debian packaging work. Stage only named task paths.
- Do not add automation to CT119 or run DValue fault-injection tests.
- Keep the scaling ratio limit at 4.0.

## Success criteria

- [x] T1: Pass the eight host gates and push the three existing commits without rewriting them.
- [x] T2: Capture the updater, units, and defaults verbatim in `deploy/`.
- [x] T3: Prove golden, reproducibility, and scaling gates on CT100 before adding them.
- [ ] T4: Block explicit approval holds and log the author release.
- [ ] A1: Install and run the updater on CT100. Verify service and public health.
- [ ] A2: Verify backup preservation and five-point retention.

## Current state

- Status: executing
- Source: `/root/mount_ssh/capy-bearer`, also `/Code/capy-bearer` on `capy-bearer-dev`.
- Runtime: CT100 on `k4`, as defined in `project-home-deployment.md`.
- T1 source revision: `21fb299`.
- Live assets are now copied to `deploy/capy-bearer-update`, its `.service`, `.timer`, and `.default` files.

## Goal tree

- [x] G1: Publish the accepted source commits.
- [x] G2: Preserve the running deployment assets before editing them.
- [x] G3: Measure the staging gates and add passing gates.
- [~] G4: Define and test the approval hold, then correct the performance record.
- [ ] G5: Install, exercise, and verify the updated automation.

## Next

1. Add and test an explicit approval marker before the updater builds or changes the live service.

## Decisions, assumptions, and risks

The user explicitly approved updates to the existing CT100 timer operational surface. The timer remains unchanged. No new forge workflow, webhook, schedule, or service is added.

Deployment assets belong in the existing `deploy/` directory. The first asset commit preserves the live bytes, including historical comments. Later commits will show each change.

The nightly updater makes a push to `origin/main` eligible for deployment. A documentation-only human checkpoint cannot stop that path.

## Installation

Run these commands on CT100 from the reviewed repository revision. Do not run them on CT119.

```bash
install -m 0755 deploy/capy-bearer-update /usr/local/sbin/capy-bearer-update
install -m 0644 deploy/capy-bearer-update.service /etc/systemd/system/capy-bearer-update.service
install -m 0644 deploy/capy-bearer-update.timer /etc/systemd/system/capy-bearer-update.timer
install -m 0644 deploy/capy-bearer-update.default /etc/default/capy-bearer-update
systemctl daemon-reload
systemctl start capy-bearer-update.service
```

The service has a one-hour timeout. Its existing timer stays enabled. Inspect the updater log and public health after the command ends.

## Evidence

- 2026-09-12: All eight required gates passed at `21fb299` on the dev host. Durations were 61 s for the compiler build, 121 s for core/native builds, 8 s for goldens, 125 s for native tests, 1 s for reproducibility, 19 s for phase 1, 1 s for LSP, and less than 1 s for grammar validation. Full output is in `/root/tmp/capy-deploy-t1-gates.log` on that host.
- 2026-09-12: A plain push advanced `origin/main` from `7e571ed` to `21fb299`. No existing commit changed.
- 2026-09-12: SHA-256 values match between all four captured assets and CT100. `bash -n` passed for the updater. Three existing backup points remain on CT100.
- 2026-09-12: Golden and cross-directory reproducibility gates passed in `/opt/capy-bearer-staging` on CT100 at `7e571ed`. No golden rewrite flag was used. The compiler hash matches the accepted compiler. The updater lock excluded concurrent deployment during these checks.
- 2026-09-12: Five CT100 scaling trials passed at the service's nice and I/O priorities. Depth ratios were 1.010, 1.022, 1.002, 1.016, and 0.995. Marker ratios were 2.005, 2.008, 2.010, 1.787, and 2.007. Large depth medians ranged from 15.608 to 15.823 ms. Large marker medians ranged from 64.019 to 113.818 ms. Keep the default 1000 ms absolute limit and 2 s subprocess timeout. No override is necessary.
