# Arty release-bundle pilot

This pilot stages one complete Linux release set. It contains the Bearer runtime, Capy compiler, WebAssembly core, and CLI launcher.

The pilot does not publish, promote, install a package, restart a service, or change a live release. An operator must approve those actions separately.

Use the editor's project-local `.npmrc`. It routes the pinned Visual Studio Code extension dependencies through the Arty npm remote. `npm ci` keeps the lockfile unchanged. `npm audit` remains enabled. Arty does not support npm audit POST routes. An audit failure does not mean a clean audit result.

Use a clean source revision. Build and run the required Capy checks before staging. Then create a stage directory outside the repository:

```bash
python3 scripts/arty_release_pilot.py stage --stage /var/tmp/capy-arty-stage
python3 /root/scripts/arty/release.py check deploy/arty.json
python3 scripts/arty_release_pilot.py manifest --stage /var/tmp/capy-arty-stage --output /var/tmp/capy-arty-stage/manifest.json
```

The stage receipt records the exact source revision, the `src/wasm/abi.h` digest, and payload digests. The manifest command rejects a changed source revision. The shared helper adds the pinned npm lockfile digest to the manifest. It also fetches `acceptance.json` with the release payload.

After an attended fetch, use the project installer with the real service account. It checks that this account can read each file and execute the runtime, compiler, and launcher before it changes `current`:

```bash
python3 scripts/arty_release_pilot.py install \
  --bundle /var/lib/bearer/fetched-release \
  --releases /var/lib/bearer/releases \
  --current /var/lib/bearer/current \
  --user bearer
```

The installer retains prior directories. It has no Arty client or network path. Roll back with a retained revision during a registry outage:

```bash
python3 scripts/arty_release_pilot.py rollback \
  --releases /var/lib/bearer/releases \
  --current /var/lib/bearer/current \
  --revision <source-revision>
```

Do not point a systemd unit at `current` until a separate deployment acceptance approves it.
