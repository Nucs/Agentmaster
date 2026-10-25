# Agentmaster build server

A service that builds, tests and e2e-runs Agentmaster on demand, in slots sized to what this PC (and later dock) has
free. The design, the owner's decisions and every measured number are in [`DESIGN.md`](DESIGN.md); this file says what
exists so far and how to use it. **Status (2026-10-25): P0.1 and P0.2 passed; Phase 1 runs on this PC: the site distro
holds the images `am-base` (Tiny11 Core + the guest agent's bootstrap) and `am-e2e` (the e2e tooling), and
`amb import` + `amb e2e` run a suite on a build in a fresh VM that leaves nothing behind whether the run passes, fails,
is cancelled or loses its VM (DESIGN.md §13 "Phase 1 as built", §16.10).**

**Phase 1, as built, differs from DESIGN.md §10 in one way:** there is no always-on host agent yet. The coordinator reads
Windows' numbers itself (it runs `amb-hostfacts.exe` through WSL's interop, which works from a daemon after the call that
started it ended: measured), and the CLI wakes the site by starting the coordinator (`coordinator-up.sh`). The host
agent's other duties (the bare mirror, copying artifacts to Windows, installs, a LAN port for Caddy) come with Phases 3
and 4. Slots run through the podman + dockur stand-in until fastrun's VM slots exist (Track A, DESIGN.md §13).

## Use it

```text
amb status                                   # the coordinator, Windows' numbers, homepc's free share, active requests
amb import <portable zip> [--commit SHA]     # a hand-built zip or a GitHub release's zip becomes a build id (b<date>-<n>)
amb builds                                   # the build ids
amb e2e <build id> [--wait] -- suites/test_smoke.py -v     # a suite in a fresh am-e2e VM; pytest arguments after --
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

## The images

```text
python buildserver/image/images.py base  [--name am-base] [--version core11]   # dockur's unattended install (13 min)
python buildserver/image/images.py smoke [--image am-base]                     # boot, `cmd /c ver`, shut down
python buildserver/image/images.py e2e   [--base am-base] [--name am-e2e]      # the e2e layer (3 min)
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
- ⚠ A boot passes the image's own dockur version (read from its `windows.base`): dockur reinstalls Windows over a slot
  whose `VERSION` differs (DESIGN.md §15).

## Build and test

```text
dotnet test buildserver/src/Buildserver.slnx            # build everything + 53 unit tests (output in Q:\tmp\buildserver-build)
python buildserver/tests/local_chain.py                 # the chain without a VM (Q:\tmp\buildserver-test, removed after)
```

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
| [`image/`](image) | the image kit: `images.py`, the OEM bootstrap (`oem/`), the layer steps (`steps/`) | `am-base` and `am-e2e` made with it |
| [`measure/`](measure/README.md) | the measurements and proofs behind DESIGN.md §16, with their raw results | P0.1 passed (§16.9) |
| [`src/Amb.Core`](src/Amb.Core) | the shared core: the allocation rule (§3), the request / build / slot model, the guest and API protocol, the atomic record store (§6) | 27 unit tests (`src/Amb.Core.Tests`) |
| [`src/Amb.Coordinator`](src/Amb.Coordinator) | `amb-coordinator`: the API, the scheduler (admission by priority × availability, wait and run timeouts, cancel), the guest broker, the jobs (import, smoke, install, layer, e2e), the slot manager and its drivers (podman + dockur; local, for tests) | runs in the site; 26 unit tests (`src/Amb.Coordinator.Tests`) |
| [`src/Amb.GuestAgent`](src/Amb.GuestAgent) | `amb-guest.exe` (Native AOT, 7 MB): the agent in every VM; hello, heartbeat with the guest's memory, one step at a time by long poll (run, fetch, expand, put, shutdown) | in every slot |
| [`src/Amb.HostFacts`](src/Amb.HostFacts) | `amb-hostfacts.exe` (Native AOT): Windows' available memory, free commit, Q: free space and a full-screen game, as one line of JSON; the coordinator runs it every 10 s through WSL's interop | in use |
| [`src/Amb.Cli`](src/Amb.Cli) | `amb.exe`: the command line above | in use |
| [`tests/local_chain.py`](tests/local_chain.py) | the whole chain on this PC without a VM: the coordinator with the local slot driver, the real guest agent, the real API | 15 checks pass |
