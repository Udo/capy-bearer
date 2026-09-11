# Arty release pilot

This pilot stages an accepted Linux runtime set for the shared Arty helper. It contains the Bearer runtime, Capy compiler, WebAssembly core, CLI launcher, an external artifact identity, and an external test receipt.

The pilot does not build, test, publish, promote, fetch, install, restart, or roll back a release. A clean Git revision does not prove that ignored compiled files came from that revision. The pilot requires two files that an attended build and test process created. The pilot only validates and copies them.

The artifact identity records the exact source revision and artifact bytes.

```json
{
  "schema_version": 1,
  "source_revision": "<40-character Git commit>",
  "artifacts": [
    {"path": "bin/bearer_fastcgi.linux.bin", "sha256": "<64-character SHA-256>"},
    {"path": "bin/capyc", "sha256": "<64-character SHA-256>"},
    {"path": "bin/wasm/core.wasm", "sha256": "<64-character SHA-256>"},
    {"path": "scripts/bearer-cli", "sha256": "<64-character SHA-256>"}
  ]
}
```

The test receipt has this shape. Each command must name a completed required test. The receipt does not prove a test result by itself. An operator must keep the referenced test output with the receipt.

```json
{
  "schema_version": 1,
  "source_revision": "<40-character Git commit>",
  "tests": [
    {"command": "<required test command>", "result": "passed"}
  ]
}
```

The shared helper is maintained on the aiworkers. The development host does not install it by default. Copy `/root/scripts/arty/release.py` from an aiworker to `tmp/arty-shared-release.py` in this checkout. Verify that both copies have the same SHA-256.

Run the fixture tests on the development host. These tests use synthetic payloads, not an accepted production build.

```bash
export ARTY_RELEASE_HELPER="$PWD/tmp/arty-shared-release.py"
timeout 45 python3 scripts/test_arty_release_pilot.py
```

After the attended checks create both external receipts, stage and create the manifest outside the repository:

```bash
timeout 60 python3 scripts/arty_release_pilot.py stage \
  --stage /var/tmp/capy-arty-stage \
  --acceptance-receipt /var/tmp/capy-acceptance.json \
  --test-receipt /var/tmp/capy-test-receipt.json
timeout 45 python3 "$ARTY_RELEASE_HELPER" check deploy/arty.json
timeout 90 python3 scripts/arty_release_pilot.py manifest \
  --stage /var/tmp/capy-arty-stage \
  --release-helper "$ARTY_RELEASE_HELPER" \
  --output /var/tmp/capy-arty-stage/manifest.json
```

The manifest includes `acceptance.json` and `test-receipt.json` as artifacts. It also records the test receipt digest and the pinned npm lockfile digest in provenance. The shared helper verifies the exact fetched artifact set before it writes a fetched release directory.

Use the existing Debian package boundary for deployment. `scripts/make_deb.sh` builds a Debian package. The host package tools install, remove, and restore that package under the existing Debian and systemd rules. This pilot does not claim that its staged files are deployable. It has no installer or rollback command.

Use the editor's project-local `.npmrc`. It routes pinned Visual Studio Code extension dependencies through the Arty npm remote. `npm ci` keeps the lockfile unchanged. `npm audit` remains enabled. Arty does not support npm audit POST routes. An audit failure does not mean a clean audit result.
