# Capy runtime package publication

## Objective and invariants

Commit and push the completed work. Publish the Debian and RPM runtime packages to Arty with exact source and test evidence.

- Preserve the separate loose-runtime pilot contract.
- Use the shared Arty release helper and a temporary namespace-scoped publish token.
- Do not install packages or promote an accepted reference as part of publication.

## Current state

- Status: complete
- Package source: `9abcefc44ceb9ad2cc8f20f6d4756fcae8fb1530`
- Contract: `deploy/arty-packages.json`
- Manifest: `sha256:82546ef54e8e8cb4e1319476d76454c47ee989ebb1b6abcaecb01f33ca4d3f94`
- Release name: `capy-bearer/packages-linux-amd64`
- Evidence: `/root/capy-package-releases/9abcefc` on `capy-bearer-dev`

## Acceptance

- [x] All completed source work is committed and pushed to both existing remotes.
- [x] Both runtime packages build from the clean source revision.
- [x] Compiler, runtime, artifact, editor, and package-layout gates pass.
- [x] The immutable manifest contains both packages, the test receipt, and build output.
- [x] A public-endpoint fetch verifies the manifest revision and exact artifact set.
- [x] Downloaded package bytes match the built packages.
- [x] The temporary publish token is revoked and its local copies are removed.

## Package identities

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `bearer.deb` | 11302820 | `a2bd93015b44ae84a71c3d288e8853e30e986aa2546235c4c7cafbf0e4d597da` |
| `bearer.rpm` | 11304118 | `5fa9ff8c0aef4f857adbda4b549d1863abfd801575805af7050e810d463187eb` |

The original filenames are `bearer_1-2026-06-001_amd64.deb` and `bearer-1-2026_06_001.x86_64.rpm`. Arty stores stable artifact filenames. Package metadata retains the version and architecture.

## Evidence

- 2026-09-12: The packaging agent had already committed and pushed its work. The source tree was clean before publication changes.
- 2026-09-12: The compiler build passed in 75.108 s. Core/native builds passed in 146.978 s. Goldens passed in 5.739 s. Native tests passed in 147.934 s. Reproducibility passed in 0.165 s. Phase 1 passed in 19.673 s. LSP passed in 1.066 s. Grammar validation passed in 0.115 s.
- 2026-09-12: The package-layout check passed in 0.064 s. Debian packaging passed in 209.135 s. RPM packaging passed in 30.464 s. The shared contract check passed in 0.164 s. The receipt records all commands and durations.
- 2026-09-12: Arty had about 254 GiB free before publication. The new namespace claims four objects totaling 22,620,637 bytes and one manifest. This was an attended publication, not a new schedule.
- 2026-09-12: Two cross-site HTTP uploads timed out with nginx status 408 before the package was received. The verified stage was transferred over SSH. The installed Arty CLI then published it through the reference service's loopback endpoint. No service configuration changed.
- 2026-09-12: The shared helper fetched the published digest through the public HTTPS endpoint on the dev host. It verified the source revision, all four artifact digests, and the receipt digest. Both package files also passed byte comparisons against the build outputs.
- 2026-09-12: The temporary one-hour publish credential was restricted to the package namespace. It was revoked after verification. No accepted or deployed reference was changed.

## Limits

No disposable target-distribution installation test ran during this publication. The release evidence states this limit. Publication does not establish package installation acceptance or change a running server.

The final documentation commit records publication. It does not change the published package source revision.
