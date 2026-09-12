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
- [x] T4: Block explicit approval holds and log the author release.
- [x] A1: Install and run the updater on CT100. Verify service and public health.
- [x] A2: Verify backup preservation and five-point retention.

## Current state

- Status: complete
- Source: `/root/mount_ssh/capy-bearer`, also `/Code/capy-bearer` on `capy-bearer-dev`.
- Runtime: CT100 on `k4`, as defined in `project-home-deployment.md`.
- T1 source revision: `21fb299`.
- Maintained assets: `deploy/capy-bearer-update`, its `.service`, `.timer`, and `.default` files.
- Installed updater and accepted live source: `639d472`.
- The final evidence commit changes only this record. It does not change the installed updater.

## Goal tree

- [x] G1: Publish the accepted source commits.
- [x] G2: Preserve the running deployment assets before editing them.
- [x] G3: Measure the staging gates and add passing gates.
- [x] G4: Define and test the approval hold, then correct the performance record.
- [x] G5: Install, exercise, and verify the updated automation.

## Next

No implementation step remains. Use the approval marker for future changes that need a deployment hold. The DValue fault-injection gaps remain outside this task.

## Decisions, assumptions, and risks

The user explicitly approved updates to the existing CT100 timer operational surface. The timer remains unchanged. No new forge workflow, webhook, schedule, or service is added.

Deployment assets belong in the existing `deploy/` directory. The first asset commit preserves the live bytes, including historical comments. Later commits will show each change.

The nightly updater makes a push to `origin/main` eligible for deployment. A documentation-only human checkpoint cannot stop that path.

`AGENTS.md` defines the exact `DEPLOY_APPROVAL` checklist marker. The installed updater scans tracked `docs/work/*.md` at the fetched commit before building. Unchecked markers fail closed. Checked markers log an explicit release with file and line. Records without markers and ordinary tasks pass.

The same commit is used for the scan, staging checkout, and live fast-forward. This prevents a second fetch from substituting a newer unapproved candidate.

For a read-only approval check, run:

```bash
/usr/local/sbin/capy-bearer-update --check-approval /opt/capy-bearer origin/main
```

The command prints the same approval results as the updater. Normal updater runs also append these results to the configured log.

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

The updater does not replace its own installed assets. Install later reviewed updater changes with these commands.

## Evidence

- 2026-09-12: All eight required gates passed at `21fb299` on the dev host. Durations were 61 s for the compiler build, 121 s for core/native builds, 8 s for goldens, 125 s for native tests, 1 s for reproducibility, 19 s for phase 1, 1 s for LSP, and less than 1 s for grammar validation. Full output is in `/root/tmp/capy-deploy-t1-gates.log` on that host.
- 2026-09-12: A plain push advanced `origin/main` from `7e571ed` to `21fb299`. No existing commit changed.
- 2026-09-12: SHA-256 values match between all four captured assets and CT100. `bash -n` passed for the updater. Three existing backup points remain on CT100.
- 2026-09-12: Golden and cross-directory reproducibility gates passed in `/opt/capy-bearer-staging` on CT100 at `7e571ed`. No golden rewrite flag was used. The compiler hash matches the accepted compiler. The updater lock excluded concurrent deployment during these checks.
- 2026-09-12: Approval tests passed on the dev host. A temporary committed hold returned 1 and named its file and line. A committed checked release returned 0 and logged the release. Ordinary tasks passed. Uncommitted edits did not release a committed hold. A missing revision failed closed. No fixture marker remains in the repository.
- 2026-09-12: Five CT100 scaling trials passed at the service's nice and I/O priorities. Depth ratios were 1.010, 1.022, 1.002, 1.016, and 0.995. Marker ratios were 2.005, 2.008, 2.010, 1.787, and 2.007. Large depth medians ranged from 15.608 to 15.823 ms. Large marker medians ranged from 64.019 to 113.818 ms. Keep the default 1000 ms absolute limit and 2 s subprocess timeout. No override is necessary.
- 2026-09-12: The focused adversarial review found no blocker. It verified approval scan ordering, file and line logs, Git error handling, and immutable candidate selection. It did not inspect concurrent packaging work.
- 2026-09-12: CT100 passed the approval fixture suite. An unchecked committed marker blocked its revision. A checked committed marker released it. The test printed both revision:file:line results. No fixture marker remains. All four installed assets match revision `639d472`. Unit validation passed, and the existing timer remains enabled and active.
- 2026-09-12: The updater deployed `639d472` in 124 seconds. It passed all six compiler/runtime gates and returned service result `success` with exit status 0. Its deployment-run scaling ratios were 0.990 and 2.005, with large medians 15.621 ms and 64.071 ms. Bearer is active. The public `/info/` request returned HTTP 200.
- 2026-09-12: A second updater invocation logged `already at 639d4720d3bf52bad3bb6bafc9d6df51360b8996, nothing to do` and returned success.
- 2026-09-12: All 12 files in the three original rollback points retained their hashes. The new deployment added a fourth complete point at `/var/backups/capy-bearer/20260912T135543`, containing the prior `7e571ed` artifacts. The exact installed retention command reduced seven scratch points to the newest five without altering their contents. No real rollback point was deleted.
