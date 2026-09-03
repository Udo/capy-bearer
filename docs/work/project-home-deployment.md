# Capy-Bearer project home deployment

## Objective and invariants

Create an Ubuntu 26.04 container on `k4`. Run nginx, MariaDB, Memcached, and Bearer in the container. Serve the repository site at `https://capy-bearer.openfu.com/`. Publish the project home in the README. Mirror the repository to `git.openfu.com/udo/capy-bearer`.

Preserve these invariants:

- Keep existing `k4` guests and public routes available.
- Use a new container ID and a dedicated address.
- Terminate TLS through the established `k4` frontend path.
- Keep the Bearer CLI socket private.
- Do not expose MariaDB or Memcached publicly.
- Build and run the same repository revision that the public forge contains.
- Do not add an external deployment or mirror schedule.

## Success criteria

- [x] The new container runs Ubuntu 26.04.
- [x] nginx, MariaDB, Memcached, Bearer, and the required socket units are active.
- [x] The project front page and documentation return HTTP 200 through the public hostname.
- [x] HTTP redirects to HTTPS.
- [x] The public TLS certificate matches `capy-bearer.openfu.com`.
- [x] MariaDB and Memcached listen only on loopback interfaces.
- [x] The README names the public project home.
- [x] `git.openfu.com/udo/capy-bearer` contains all branches and tags from the source repository.
- [x] The deployment survives a container restart.

## Current state

- Status: deployed
- Source: `/root/mount_ssh/capy-bearer`
- Proxmox host: `k4`
- Runtime: container 100 at `10.10.10.100`
- Public project home: `https://capy-bearer.openfu.com/`
- Private forge copy: `ssh://git@git.openfu.com:11622/udo/capy-bearer.git`

## Goal tree

- [x] G1: Map the established `k4` container, network, DNS, and TLS patterns.
  - [x] G1.1: Select an unused container ID and address.
  - [x] G1.2: Identify the public frontend and certificate workflow.
  - [x] G1.3: Identify the Ubuntu 26.04 template and storage.
- [x] G2: Create and secure the container.
  - [x] G2.1: Create Ubuntu 26.04 with bounded CPU, memory, disk, and swap.
  - [x] G2.2: Configure networking, SSH, time, locale, and package sources.
  - [x] G2.3: Install nginx, MariaDB, Memcached, and build dependencies.
- [x] G3: Deploy Capy-Bearer and its included site.
  - [x] G3.1: Clone the repository at the accepted revision.
  - [x] G3.2: Build and install Bearer.
  - [x] G3.3: Configure nginx and systemd for the repository site.
  - [x] G3.4: Verify restart behavior and service isolation.
- [x] G4: Publish the public route.
  - [x] G4.1: Verify DNS.
  - [x] G4.2: Configure the established frontend proxy.
  - [x] G4.3: Issue and verify TLS.
- [x] G5: Publish project locations.
  - [x] G5.1: Create the private forge repository and push all refs once.
  - [x] G5.2: Add the project home to the README.
- [x] G6: Run acceptance, update records, commit, and push.

## Next

No deployment action remains.

## Decisions, assumptions, and risks

- The requested mirror is a one-time mirror push. This task will not add scheduled synchronization.
- The public route and forge copy are approved external surfaces.
- Package installation will use Ubuntu release packages. The deployment will not install a package release newer than three days without review.

## Evidence

- 2026-09-01: `k4` is reachable and runs Proxmox VE 8.4.12.
- 2026-09-01: Container IDs 101 through 123 contain gaps. Container 114 is named `frontend`.
- 2026-09-03: Created unprivileged container 100 with Ubuntu 26.04, four CPUs, 4 GiB RAM, 2 GiB swap, and a 32 GiB root disk.
- 2026-09-03: Installed nginx, MariaDB, Memcached, the native build tools, WASI SDK 33.0, and Wasmtime 45.0.1.
- 2026-09-03: Built Bearer from the accepted Git revision. The service runs as the unprivileged `bearer` user. The source and public site trees are read-only to that user.
- 2026-09-03: MariaDB and Memcached listen on `127.0.0.1` only. The Bearer CLI uses a mode `0600` Unix socket.
- 2026-09-03: Container 114 proxies the public hostname to container 100. Let's Encrypt issued a certificate for the exact hostname.
- 2026-09-03: Public checks returned 200 for `/info/`, `/doc/`, a pretty documentation route, and a static asset. HTTP redirects to HTTPS.
- 2026-09-03: Created the private Gitea repository and pushed `main` and all source tags. No scheduled mirror was added.
- 2026-09-03: Restarted container 100. All five enabled service units became active, the private CLI health check passed, and the public routes still returned the expected responses.
- 2026-09-03: The focused Certbot renewal dry run succeeded. OpenSSL verified the public certificate chain and exact hostname.
