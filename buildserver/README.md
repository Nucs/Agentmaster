# Agentmaster build server

A service that builds, tests and e2e-runs Agentmaster on demand, in slots sized to what this PC (and later dock) has
free. The design, the owner's decisions and every measured number are in [`DESIGN.md`](DESIGN.md); this file says what
exists so far and how to use it. **Status (2026-10-26): P0.1, P0.2, P0.4 and P0.5 passed. Phase 1 runs on this PC: the
site distro holds the images `am-base` (Tiny11 Core + the guest agent's bootstrap) and `am-e2e` (the e2e tooling), and
`amb import` + `amb e2e` run a suite on a build in a fresh VM that leaves nothing behind whether the run passes, fails,
is cancelled or loses its VM (DESIGN.md §13 "Phase 1 as built", §16.10). Phase 3's MVP runs too: `amb build` builds a
commit in the kept build VM (image `am-build`) into a build id whose portable zip `amb e2e` runs and `amb fetch` copies
to Q:; a cold Release build takes 38 min of build, an incremental one 6–8 min, 1.17× and 1.31× the native time, so
builds stay in VMs (DESIGN.md §13 "Phase 3's MVP as built", §16.12).**

**As built, it differs from DESIGN.md §10 in one way:** there is no always-on host agent yet. The coordinator reads
Windows' numbers itself (it runs `amb-hostfacts.exe` through WSL's interop, which works from a daemon after the call that
started it ended: measured), and the CLI wakes the site by starting the coordinator (`coordinator-up.sh`). Of the host
agent's other duties, the coordinator keeps the bare mirror itself (in the distro) and `amb fetch` copies artifacts to
Windows through the API; installs and a LAN port for Caddy come with Phase 4. Slots run through the podman + dockur
stand-in until fastrun's VM slots exist (Track A, DESIGN.md §13).

## Use it

```text
amb build [<rev>] [--config Release|Debug] [--identity dev|release] [--purge none|force] [--wait]
                                             # build a commit (default HEAD of K:\source\Agentmaster) in the build VM
amb artifacts <build id>                     # a build's files: portable zip, layout zip, symbols zip, build log, build.json
amb fetch <build id> [--kind KIND|all] [--to DIR]   # copy them to Q:\buildserver\out\<build id> (never K:), SHA-checked
amb status                                   # the coordinator, Windows' numbers, homepc's free share, active requests
amb import <portable zip> [--commit SHA]     # a hand-built zip or a GitHub release's zip becomes a build id (b<date>-<n>)
amb builds                                   # the build ids
amb e2e <build id> [--wait] -- suites/test_smoke.py -v     # a suite in a fresh am-e2e VM; pytest arguments after --
amb e2e <build id> --run-timeout 480 -- -v                 # the whole suite (hours): a run timeout of its own
amb logs <request id> [-f]                   # its events: placement, slot, steps, each step's output, progress
amb list | show <id> | cancel <id>
```

- `amb` is `src/Amb.Cli` (built into `Q:\tmp\buildserver-build\Amb.Cli\bin\…\amb.exe`). It talks to
  `http://127.0.0.1:2207` (`AMB_URL` overrides) with the admin token it reads from the site once and keeps in
  `%LOCALAPPDATA%\amb\token`; nothing prints it.
- Exit codes: 0 succeeded (or accepted, without `--wait`); 1 ended otherwise; 2 usage; 97 still running at `--wait-max`;
  98 the coordinator could not be reached or woken.
- A request waits for a slot while homepc has too little free (`waiting: memory`, …), up to its kind's wait timeout,
  then ends `NoCapacity`. Windows keeps 6 GB available and 16 GB of free commit (DESIGN.md §3.2).
- A run's results stay in the site's store: `/srv/buildserver/store/requests/<id>/` (`<id>.status.json`,
  `<id>.progress.jsonl`, `<id>.zip` with pytest.log, junit.xml, the mock's requests and each failed test's evidence).

## Builds

`amb build` builds a **commit**, never the working tree: it resolves `<rev>` (default HEAD) in `K:\source\Agentmaster`
(`AMB_REPO` overrides) and sends the full SHA. A build id names immutable bytes, so commit first.

- **Where it runs:** the kept build VM `build-1` (image `am-build`: Tiny11 Core + VS 2022 Build Tools 17.14, MSVC 14.44,
  SDKs 22621 and 26100, vcpkg, PowerShell 7, Git). Its overlay and its data disk D: (a sparse 128 GiB raw file) live in
  `/srv/buildserver/vms/build-1` and outlive each build, so the next build of a worktree is incremental. One build at a
  time: the next request waits (`waiting: the build VM is busy with …`).
- **The source:** the site's bare mirror (`/srv/buildserver/git/agentmaster.git`) fetches the repository's branches and
  tags from K: by path (it only reads K:); the VM gets a git bundle of what its worktree lacks (the whole history the
  first time, ~140 MB; then only the new commits) plus the `refs/claude-agent-sdk-cpp` submodule's.
- **The build:** in `D:\src\<config>-<identity>`, the commit's own `tools\Build-Agentmaster.ps1` (restore, DuckDB,
  vcpkg's binary cache on D:), `/m` and `CL_MPCount` picked from the VM's memory (2 × 2 below 10 GB). `--purge force`
  deletes the worktree's build outputs first (a clean build); `none` builds incrementally.
- **The artifacts** (`amb artifacts <id>`): the portable zip (`New-UnpackagedTerminalDistribution.ps1 -PortableMode`,
  as release.yml makes it; e2e-ready), the layout as a zip (for an install through the swap script), the symbols zip,
  `build.log` / `build.log.err` and `build.json`, each with its SHA-256 in the build's manifest.
- **The same request twice** (commit, configuration, identity, purge) returns the first build's id at once, without a
  slot; one that comes while the first still builds waits for the VM, then answers with the first's id.
- **A failed build** ends `Failed` with the end of its log; the whole log is kept in `store/requests/<id>/build.log`.
- **How long** (DESIGN.md §16.12): the first build of a worktree is cold: Release 38 min of build (NuGet, DuckDB,
  vcpkg from source, every project) plus ~5 min of boot, checkout and packaging; Debug 20.6 min of build, 23.9 min in
  all. A later build of that worktree builds only what changed: 6–8 min of build, 10–12 min in all (the VM's boot
  27–38 s, the checkout seconds, the packaging ~2.7 min). The VM shuts down after each build; its disks stay.
- **What each build keeps:** ~0.6 GB in the store for a Release build (the portable zip and the layout zip, ~200 MB
  each, the symbols zip ~175 MB, the log), ~0.3 GB for a Debug one; nothing deletes builds yet, so the site's disk
  grows with every build id.
- **Debug builds** (`--config Debug`) build and package the same way, in their own worktree (`D:\src\debug-dev`;
  `b20261026-4`: a 124 MB portable zip). ⚠ Their binaries need `ucrtbased.dll`, the debug UCRT that only the Windows
  SDK installs, so a Debug portable zip does not run on a PC without the SDK, `am-e2e` included: `amb e2e` takes
  Release builds.
- **Installing a build on this PC** (no `amb install` yet): `amb fetch <id> --kind layout-zip`, extract the zip into
  the EMPTY layout folder of the instance's pair that it does not run from (`Q:\agentmaster-layouts\Debug` / `Debug-B`
  for dev), check that `AppxManifest.xml` names the package at 0.0.1.0 and that `TerminalApp.dll` bakes the build's
  commit, then `tools\Swap-AgentmasterLayout.ps1 -LayoutDir <folder> [-PackageName AgentmasterDev]` after a
  `-WhatIf`. First done 2026-10-26: `b20261026-5` (a Debug dev build of a8d8b97a2, 6 min 48 s from request to build
  id, 3.1 min of it the build) became dev 0.0.1.36: the fetch took 0.8 s, the extraction 1.6 s, its 316 files were
  exactly those of the local msbuild layout it replaced, and the swap was down 6.9 s. Its PDBs stay in the build's
  symbols zip.
- **Memory:** a build needs 8 GB beyond Windows' 6 GB reserve. While the PC has less, the request waits; the site's
  page cache counts as used memory for Windows, so after big copies it can help to drop it
  (`python buildserver/site/sitectl.py run -- sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'`).

## The images

```text
python buildserver/image/images.py base  [--name am-base] [--version core11]   # dockur's unattended install (13 min)
python buildserver/image/images.py smoke [--image am-base]                     # boot, `cmd /c ver`, shut down
python buildserver/image/images.py e2e   [--base am-base] [--name am-e2e]      # the e2e layer (3 min)
python buildserver/image/images.py build-image [--base am-base] [--name am-build]   # the build layer (25 min)
python buildserver/image/images.py check [--image am-e2e]                      # boot it and check what it holds
python buildserver/image/images.py list
```

- **`am-base`:** Tiny11 Core 25H2 installed by dockur with `image/oem/` (install.bat → setup.ps1, which registers the
  `AmbBootstrap` logon task → bootstrap.ps1 at every logon: Z: = the slot's share, amb-guest copied from it and run).
  A new guest agent therefore needs no new image: each slot gets the coordinator's copy.
- **`am-e2e`:** a qcow2 layer over `am-base`: CPython 3.13.9, then `e2e/guest/provision.ps1 -Pwsh -Cache C:\e2e\cache`
  (Claude Code, PortableGit, PowerShell 7, the venv, the hosts guard, the mock's environment, crash dumps), then
  `image/steps/trims.ps1`. An e2e run calls provision.ps1 again with the same cache, so only the build under test is
  extracted.
- **`am-build`:** a qcow2 layer over `am-base` (22.3 GB of files, 24.6 min to make): VS 2022 Build Tools in
  `C:\BuildTools` (`image/steps/install-buildtools.ps1`, the measured arguments), PowerShell 7.6.8 and PortableGit
  2.56.0.3 (pinned, SHA-256-checked, staged from `Q:\buildserver\cache\dl`), the VSSetup module, long paths, the trims,
  and a seal that waits for Windows' servicing to be idle. `images.py check --image am-build` checks each tool.
- ⚠ A boot passes the image's own dockur version (read from its `windows.base`): dockur reinstalls Windows over a slot
  whose `VERSION` differs (DESIGN.md §15).
- Every slot's screen is 1600×900, the claude-fleet desktops' size the e2e suite was written against (dockur's own
  default came up at ~1024×768); `images.py check` checks it.

## Build and test

```text
dotnet test buildserver/src/Buildserver.slnx            # build everything + 64 unit tests (output in Q:\tmp\buildserver-build)
python buildserver/tests/local_chain.py                 # the chain without a VM (Q:\tmp\buildserver-test, removed after)
python buildserver/tests/site_outcomes.py               # Phase 1's done-when on the real site: e2e passed, failed, cancelled, lost VM
python buildserver/tests/site_build.py                  # the build MVP's done-when on the real site (it builds: ask first)
```

- `site_build.py` builds `--rev` (default HEAD) with `amb build --wait`, then checks the manifest, an identical
  request (same id, no second build), `amb e2e` on the build, `amb fetch`, that no slot is left and that K: was only
  read; 7 checks, exit 0 when all pass. `--config`, `--identity` and `--purge` pass through.

## The site

```text
python buildserver/site/sitectl.py create       # once: Alpine 3.22 minirootfs (checked by SHA-256) → wsl --import → packages
python buildserver/site/sitectl.py provision    # re-apply packages, folders and configuration (idempotent)
python buildserver/site/sitectl.py deploy       # publish the coordinator (linux-musl-x64), amb-guest and amb-hostfacts (AOT) into it
python buildserver/site/sitectl.py up | down    # start the coordinator (a setsid daemon) / stop it (its jobs are cancelled)
python buildserver/site/sitectl.py status       # JSON: state, disk size; free space and memory when it is running
python buildserver/site/sitectl.py run -- podman ps
```

- **What it holds:** podman (rootful, cgroupfs, overlay), crun, slirp4netns, qemu-img, python3; the folders of
  DESIGN.md §8.1 under `/srv/buildserver` (images, slots, exchange, records, store, bin, fastrun, …).
- **A redeploy:** `down` (check `amb status` first: it cancels running requests and stops their slots), `deploy`, `up`.
- **Never `wsl --shutdown`** for the site: it ends every Docker container and other sessions' desktops. The script only
  ever terminates its own distro (`wsl --terminate buildserver`).
- **Windows reaches the coordinator at 127.0.0.1:2207** (it listens on IPv4); fastrund, which listens on the IPv6
  wildcard, only at the distro's address (`ip -4 addr show eth0` inside; it changes with every start of WSL's VM).
- **Something that must keep running is started with `setsid`, or in a podman container:** a background job of a
  `wsl.exe -e` call ends with that call.
- **The disk only grows** (`Q:\buildserver\wsl\ext4.vhdx` is not sparse until W1's WSL restart): ext4 reuses freed
  blocks, so it settles at the high-water mark of what the site held at once.
- **The registry credentials** fastrund pulls with live in `/srv/buildserver/fastrun/regcreds` (root only), copied from
  Docker Desktop's credential store by `measure/p01_fastrund.py`; nothing prints them.

## What exists

| Path | What it is | State |
|---|---|---|
| [`site/sitectl.py`](site/sitectl.py) | makes, deploys into and inspects this PC's site: the WSL distro `buildserver`, its disk in `Q:\buildserver\wsl` (E2) | in use |
| [`site/files/`](site/files) | the configuration it installs (`/etc/wsl.conf`, podman's `containers.conf`), each file documented | in use |
| [`image/`](image) | the image kit: `images.py`, the OEM bootstrap (`oem/`), the layer steps (`steps/`) | `am-base`, `am-e2e` and `am-build` made with it |
| [`steps/build/`](steps/build) | the build VM's steps, run with the image's PowerShell 7: `prepare-disk.ps1` (D:), `checkout.ps1` (the worktree from git bundles), `build.ps1` (the commit's own `tools\Build-Agentmaster.ps1`), `package.ps1` (the portable zip, the layout zip, the symbols, `build.json`); `sitectl.py deploy` copies them into the site, and each build stages them into its share | in use |
| [`measure/`](measure/README.md) | the measurements and proofs behind DESIGN.md §16, with their raw results | P0.1 (§16.9), P0.5's comparison (§16.11) and P0.4's native half (§16.12) |
| [`src/Amb.Core`](src/Amb.Core) | the shared core: the allocation rule (§3), the request / build / slot / VM model, the guest and API protocol, the atomic record store (§6) | 27 unit tests (`src/Amb.Core.Tests`) |
| [`src/Amb.Coordinator`](src/Amb.Coordinator) | `amb-coordinator`: the API (artifacts streamed by name), the scheduler (admission by priority × availability, wait and run timeouts, cancel, one build at a time, existing builds answered without a slot), the guest broker, the jobs (import, smoke, install, layer, e2e, build), the source mirror and its bundles, the slot manager and its drivers (podman + dockur, kept VMs with a data disk; local, for tests) | runs in the site; 37 unit tests (`src/Amb.Coordinator.Tests`) |
| [`src/Amb.GuestAgent`](src/Amb.GuestAgent) | `amb-guest.exe` (Native AOT, 7 MB): the agent in every VM; hello, heartbeat with the guest's memory, one step at a time by long poll (run, fetch, expand, put, shutdown) | in every slot |
| [`src/Amb.HostFacts`](src/Amb.HostFacts) | `amb-hostfacts.exe` (Native AOT): Windows' available memory, free commit, Q: free space and a full-screen game, as one line of JSON; the coordinator runs it every 10 s through WSL's interop | in use |
| [`src/Amb.Cli`](src/Amb.Cli) | `amb.exe`: the command line above | in use |
| [`tests/local_chain.py`](tests/local_chain.py) | the whole chain on this PC without a VM: the coordinator with the local slot driver, the real guest agent, the real API | 15 checks pass |
| [`tests/site_outcomes.py`](tests/site_outcomes.py) | Phase 1's done-when on the real site: e2e runs that pass, fail, are cancelled and lose their VM, each leaving nothing behind | 14 checks passed (2026-10-25) |
| [`tests/site_build.py`](tests/site_build.py) | the build MVP's done-when on the real site: a build of HEAD, its manifest, an identical request, e2e on the build, fetch, no slot left, K: only read | 7 checks passed (2026-10-26, `b20261026-3`) |
