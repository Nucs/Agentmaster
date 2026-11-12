# Agentmaster build server

A service that builds, tests and e2e-runs Agentmaster on demand, in slots sized to what this PC (and later dock) has
free. The design, the owner's decisions and every measured number are in [`DESIGN.md`](DESIGN.md); this file says what
exists so far and how to use it. **Status (2026-10-26): P0.1, P0.2, P0.4 and P0.5 passed. Phase 1 runs on this PC: the
site distro holds the images `am-base` (Tiny11 Core + the guest agent's bootstrap) and `am-e2e` (the e2e tooling), and
`amb import` + `amb e2e` run a suite on a build in a fresh VM that leaves nothing behind whether the run passes, fails,
is cancelled or loses its VM (DESIGN.md §13 "Phase 1 as built", §16.10). Phase 3's MVP runs too: `amb build` builds a
commit in the kept build VM (image `am-build`) into a build id whose portable zip `amb e2e` runs and `amb fetch` copies
to Q:; a cold Release build takes 38 min of build, an incremental one 6–8 min, 1.17× and 1.31× the native time, so
builds stay in VMs (DESIGN.md §13 "Phase 3's MVP as built", §16.12). Since 2026-10-29: `amb install` installs a build
on dev or production through the swap script (a failure after the stop brings the old version back: tested), `amb build
--snapshot` builds uncommitted work, and every build keeps msbuild's binary log (DESIGN.md §13 "Phase 4's install as
built", §16.13). Since 2026-11-12 there is a second site, on the worker machine in Stuttgart (CT 803, see "The worker
site" below): builds and e2e runs go there by default (build ids `w…`), and `amb install` still runs through this PC's
site, which pulls a worker build first.**

**As built, it differs from DESIGN.md §10 in one way:** there is no always-on host agent yet. The coordinator reads
Windows' numbers itself (it runs `amb-hostfacts.exe` through WSL's interop, which works from a daemon after the call that
started it ended: measured), and the CLI wakes the site by starting the coordinator (`coordinator-up.sh`). Of the host
agent's other duties, the coordinator keeps the bare mirror itself (in the distro), `amb fetch` copies artifacts to
Windows through the API, and `amb install` runs its Windows side through the same interop (a process started that way
descends from wslhost.exe, never from an Agentmaster instance); a LAN port for Caddy is not built. Slots run through the
podman + dockur stand-in until fastrun's VM slots exist (Track A, DESIGN.md §13).

## Use it

```text
amb build [<rev>] [--config Release|Debug] [--identity dev|release] [--purge none|force] [--wait]
                                             # build a commit (default HEAD of K:\source\Agentmaster) in the build VM
amb build --snapshot [--config ...] [-- <paths>]    # build uncommitted work: a snapshot commit of HEAD + the work
amb artifacts <build id>                     # a build's files: portable zip, layout zip, symbols zip, logs, binlog, build.json
amb fetch <build id> [--kind KIND|all] [--to DIR]   # copy them to Q:\buildserver\out\<build id> (never K:), SHA-checked
amb install <build id> --instance dev|production [--dry-run] [--reinstall] [--wait-others MIN] [--wait]
                                             # install a build on this PC's dev or production instance (alias: deploy)
amb status                                   # the coordinator, Windows' numbers, homepc's free share, active requests
amb import <portable zip> [--commit SHA]     # a hand-built zip or a GitHub release's zip becomes a build id (b<date>-<n>)
amb builds                                   # the build ids
amb e2e <build id> [--wait] -- suites/test_smoke.py -v     # a suite in a fresh am-e2e VM; pytest arguments after --
amb e2e <build id> --run-timeout 480 -- -v                 # the whole suite (hours): a run timeout of its own
amb e2e|smoke ... [--ram GB] [--guest-ram GB] [--vcpus N]  # the VM's size: a cap on a larger guest RAM, its vCPUs
amb logs <request id> [-f]                   # its events: placement, slot, steps, each step's output, progress
amb list | show <id> | cancel <id>
amb ... --site home|worker                   # any command: the site (default AMB_SITE, else worker)
amb e2e <build id> --suite-rev REV           # the suite is REV's committed e2e/ (the worker's only way; default HEAD there)
```

- `amb` is `src/Amb.Cli` (built into `Q:\tmp\buildserver-build\Amb.Cli\bin\…\amb.exe`). It talks to the worker site at
  `http://192.168.50.21:2207` (`AMB_WORKER_URL` overrides) or this PC's at `http://127.0.0.1:2207` (`AMB_URL`), with
  the admin token it reads from each site once (the worker's through `ssh worker`) and keeps in
  `%LOCALAPPDATA%\amb\token-worker` / `token`; nothing prints them.
- **Which site** (`src/Amb.Cli/Sites.cs`): `--site`, else `AMB_SITE`, else the worker. A build id goes to the site its
  letter names (`w…` the worker, `b…` this PC), so `amb fetch`, `amb artifacts` and `amb e2e` of a build need no
  `--site`; a request id names no site, so `amb logs` / `show` / `cancel` ask the chosen site, then the other one.
  `amb install` always runs at this PC's site (it reaches the instances) and `amb import` too (it reads a file here).
  When the worker (or the link home) is down, `--site home` (or `AMB_SITE=home`) uses this PC's site as before.
- Exit codes: 0 succeeded (or accepted, without `--wait`); 1 ended otherwise; 2 usage; 97 still running at `--wait-max`;
  98 the coordinator could not be reached or woken.
- `amb --help` (or `-h`) prints every command and option. An option amb does not know - a typo such as `--dryrun`, or
  a value option without its value - is refused with exit 2 before anything is sent (until 2026-10-29 it was ignored,
  and `amb build --help` submitted a build).
- A request waits for a slot while homepc has too little free (`waiting: memory`, …), up to its kind's wait timeout,
  then ends `NoCapacity`. Windows keeps 6 GB available and 16 GB of free commit (DESIGN.md §3.2).
- Once a request has waited 10 minutes, a younger one that needs a slot waits behind it (`waiting: behind <id>, which
  has waited longer for capacity`) up to its own wait timeout; an import, an install or a build whose build exists
  needs no slot and starts anyway (DESIGN.md §3.5; until 2026-10-29 it waited too, without ever ending). The older one
  holds nothing back when it would not fit even with every slot of the site stopped (the owner's programs hold what it
  lacks).
- **An e2e VM is a capped slot by default** (DESIGN.md §16.15): a 2 GB cap on a 3 GB guest with 4 vCPUs; the guest's
  RAM above the cap lives in the site's swap file on Q:, so the PC is charged 2 GB, and the run takes ~1.15× the time of
  the uncapped 8 GB VM of before (`--ram 8` asks for that one). `--ram GB` sets the VM's memory (a cap when
  `--guest-ram GB` is larger), `--vcpus N` its vCPUs; a capped VM is admitted with 6 GB of free commit kept instead of
  16. Every e2e and smoke run ends with a `slot memory:` event (the peak in memory, the peak with swap, the cap hits),
  the figures to tune by. ⚠ A cap under ~2 GB breaks the suite's own waits (1.5 GB: Claude Code's input box did not
  show within 60 s).
- A released version runs e2e without a build: `amb import <its portable zip> --commit <the tag's commit>`, then `amb
  e2e` on the build id it prints; `--suite <folder>` pins the suite (e.g. `git archive <commit> e2e` extracted on Q:),
  since a request that waits for memory stages the suite only when it starts.
- A run's results stay in the site's store: `/srv/buildserver/store/requests/<id>/` (`<id>.status.json`,
  `<id>.progress.jsonl`, `<id>.zip` with pytest.log, junit.xml, the mock's requests and each failed test's evidence).

## Builds

`amb build` builds a **commit**, never the working tree: it resolves `<rev>` (default HEAD) in `K:\source\Agentmaster`
(`AMB_REPO` overrides) and sends the full SHA. A build id names immutable bytes, so commit first, or snapshot.

- **Uncommitted work** (`amb build --snapshot [-- <paths>]`, `src/Amb.Cli/WorkingTreeSnapshot.cs`): the CLI makes a
  commit of HEAD plus the work - the whole working tree (other sessions' uncommitted edits included), or only `<paths>`
  over HEAD - in a PRIVATE index (`GIT_INDEX_FILE`), so HEAD, the branches, the repository's index and the files stay as
  they are, and keeps it under `refs/buildserver/snapshots/<commit>`, which the site's mirror fetches. The commit is
  reproducible (a fixed author, HEAD's date, a message of HEAD and the tree): the same work twice is the same build id.
  Refused: a file over 100 MB or more than 300 MB in all (the objects go to K:). A clean tree builds HEAD. The build's
  record names the commit it was made on (`snapshotOf`; `amb builds` shows it); a built snapshot's ref in the repository
  goes at the next snapshot (the mirror keeps its own).

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
  as release.yml makes it; e2e-ready), the layout as a zip (what `amb install` installs), the symbols zip,
  `build.log` / `build.log.err`, `build.binlog` (msbuild's binary log, kind `build-binlog`: open it in the MSBuild
  Structured Log Viewer) and `build.json`, each with its SHA-256 in the build's manifest. The binary log comes from a
  `Directory.Build.rsp` the build step puts beside the solution for the build's duration (msbuild reads it by itself), so
  a commit of any age gets one; a failed build keeps it in `store/requests/<id>/`.
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
- **Installing a build on this PC:** `amb install` (next section). By hand, the same steps were first done on
  2026-10-26: `b20261026-5` (a Debug dev build of 756861eb1) became dev 0.0.1.36: the fetch took 0.8 s, the extraction
  1.6 s, its 316 files were exactly those of the local msbuild layout it replaced, and the swap was down 6.9 s. Its PDBs
  stay in the build's symbols zip.
- **Memory:** a build needs 8 GB beyond Windows' 6 GB reserve. While the PC has less, the request waits; the site's
  page cache counts as used memory for Windows, so after big copies it can help to drop it
  (`python buildserver/site/sitectl.py run -- sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'`).

## Installs

`amb install <build id> --instance dev|production` (alias `deploy`; `src/Amb.Coordinator/Jobs/DeployJob.cs`,
`steps/install/install-build.ps1`) moves one of this PC's Agentmaster instances onto a build, the way the hand installs of
2026-10-26/28 did, as one request. It is a deploy: ask the owner first, and update CLAUDE.md's *Builds* table after.

- **Where it runs:** the coordinator stages, in `Q:\buildserver\install\<request id>`, the build's layout zip (hashed on
  the way), its own `install-build.ps1`, and the COMMITTED `tools\Swap-AgentmasterLayout.ps1` and `tools\am-lock.sh` of
  `refs/heads/agentmaster` (`AMB_INSTALL_SCRIPTS_REF`: never a working tree's half-done edit, never an old build's copy),
  then runs the script with Windows' PowerShell 7 through WSL's interop. A process started that way descends from
  `wslhost.exe` ← `wslservice.exe`, in the user's session (measured), never from an Agentmaster instance: the swap's
  self-hosting guard passes and the stop cannot kill it, even when the session that asked lives in the instance being
  swapped. The stage goes when the request ends; `install.log` and `install.json` stay in `store/requests/<id>/`.
- **What it does:** (1) the instance must be a loose registration running from one folder of its pair (`AMB_DEV_SLOTS`,
  `AMB_PRODUCTION_SLOTS`: `Debug` / `Debug-B` and `Release` / `Release-B` under `Q:\agentmaster-layouts`); an installed
  MSIX is refused (its swap needs `-ReplaceInstalledPackage` and a rollback bundle: by hand). (1b) An instance that
  already runs this build (its running `TerminalApp.dll` is the zip's, byte for byte) is left alone: nothing is staged
  or stopped, and the request succeeds with `… already runs <build id> …` (`--reinstall` installs it anyway). (2) The
  other folder must run nothing; an older build there moves aside (`<folder>.amb-prev-<time>`) and the zip is
  extracted. (3) The checks: the manifest names the instance's package at 0.0.1.0, the key binaries exist,
  `TerminalApp.dll` bakes the build's commit, and no DLL or EXE of the running layout is missing. (4) A session of the
  instance that asked must not be Running for 15 s, up to 45 min. The CLI names it: its nearest `claude.exe` ancestor,
  else the `claude.exe` its `CLAUDE_PID` names (Claude Code sets it in every tool call; `bash script.sh` breaks the
  process chain, `ProcessAncestry.cs`), and prints which. The other sessions' running turns are cut off (the owner's
  choice of 2026-10-20) and named, unless `--wait-others MIN`. (5) The build-launch mutex, and the
  registration checked again under it. (6) The swap: stamp, stop the GUI only, drain, register, launch; a failure after
  the stop relaunches the old version. (7) A 60 s watch: the new GUI alive, a "Reopen your N windows?" dialog (it holds
  the restore until answered), the instance's new trouble lines in `hooks.log`. Any failure before the swap's success
  puts the folder back as it was; a folder the instance runs from is never removed.
- **Refused before anything is staged:** a build without a layout zip (an import), a build of the other identity
  (production takes `amb build --identity release` builds), an instance other than dev / production (`release` means
  production).
- **Outcomes:** `Succeeded: installed …`; `Succeeded: … already runs <build id> …` (nothing was stopped);
  `Failed: ROLLED BACK: …` (the swap failed after the stop, and the old version runs again); `Failed: LOOK AT THE
  INSTANCE NOW: …` (the rollback did not finish, or the new instance died during the watch); any other `Failed` stopped
  nothing.
- **Why the same build is a no-op:** a swap onto the build that already runs only restarts the instance and cuts off
  every running turn. On 2026-11-04 a session inside production ran its install from a script, so the CLI found no
  requester and the swap cut the session's own turn off; Claude Code continued that turn after the restart
  (`CLAUDE_CODE_RESUME_INTERRUPTED_TURN`), the turn ran the script again, and production got build `b20261104-1` seven
  times in 9 hours, six of them repeats that only restarted it. Either fix alone breaks that loop.
- **`--dry-run`:** stage, check and the swap's own `-WhatIf`, then the folder is put back. **`--inject-failure
  after-stop`** (a test): the swap fails after the stop and the drain (`Swap-AgentmasterLayout.ps1
  -InjectFailureAfterStop`), so the real rollback runs.
- **From a session inside the instance:** the install waits for that session's turn to end, and `--wait` returns as soon
  as it starts that wait (else the turn would hold itself open). The swap then ends that session's claude like every
  other one of the instance; it resumes when its tab is entered.
- **Cancel** (`amb cancel <id>`) stops it at its next safe point before the swap and puts the folder back; once the swap
  started it runs to its end (killing it there could leave the instance stopped and unregistered).
- **Measured 2026-10-29** (DESIGN.md §16.13), dev with build `b20261028-3`: the dry run 28 s from request to end; the
  install 2.5 min (dev 0.0.1.39 @ Debug-B → 0.0.1.40 @ Debug, 55.2 s down: its drain took 38 s while the PC had under
  5 GB of free commit); the injected failure: stopped, drained 4.4 s, the old version relaunched within ~5 s, the folder
  put back, `ROLLED BACK`.

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
dotnet test buildserver/src/Buildserver.slnx            # build everything + 108 unit tests (output in Q:\tmp\buildserver-build)
python buildserver/tests/local_chain.py                 # the chain without a VM (Q:\tmp\buildserver-test, removed after)
python buildserver/tests/site_outcomes.py               # Phase 1's done-when on the real site: e2e passed, failed, cancelled, lost VM
python buildserver/tests/site_build.py                  # the build MVP's done-when on the real site (it builds: ask first)
```

- **The SDK:** this PC's installed .NET SDK fails the restore since the runtime advisory fixed in 10.0.14 (NU1903, an
  error under the warnings-as-errors posture). Use the unpacked SDK: `DOTNET_ROOT=Q:\tools\dotnet-10.0.404
  DOTNET_MULTILEVEL_LOOKUP=0 Q:\tools\dotnet-10.0.404\dotnet.exe test …` (sitectl.py and worker.py do that by
  themselves). `-p:AmbBuildRoot=Q:\tmp\buildserver-build-wk\` builds apart from the live `amb.exe`.
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
  Stop your own `amb … --wait` and `amb logs -f` first: an `amb` call that finds the coordinator down starts it again,
  from the files the deploy then puts aside. `deploy` names such a coordinator (`still_running_old_binary`), and `up`
  fails on it ("running <pid> from an old binary"): `down`, then `up` (2026-10-29: `up` said "running 583" for one,
  and the fix being deployed seemed not to work).
- **Never `wsl --shutdown`** for the site: it ends every Docker container and other sessions' desktops. The script only
  ever terminates its own distro (`wsl --terminate buildserver`).
- **Windows reaches the coordinator at 127.0.0.1:2207** (it listens on IPv4); fastrund, which listens on the IPv6
  wildcard, only at the distro's address (`ip -4 addr show eth0` inside; it changes with every start of WSL's VM).
- **Something that must keep running is started with `setsid`, or in a podman container:** a background job of a
  `wsl.exe -e` call ends with that call.
- **The disk only grows** (`Q:\buildserver\wsl\ext4.vhdx` is not sparse until W1's WSL restart): ext4 reuses freed
  blocks, so it settles at the high-water mark of what the site held at once.
- **The slots' swap file:** `/srv/buildserver/swap/slots.swap`, 16 GB on the site's disk (fallocate: the vhdx on Q:
  grows only by what is swapped out), turned on at priority 10 (ahead of WSL's own swap on C:) by coordinator-up.sh at
  every start, since WSL forgets a swapon when its VM restarts. Capped slots swap their guests' RAM there; without it
  they use WSL's own swap.
- **The registry credentials** fastrund pulls with live in `/srv/buildserver/fastrun/regcreds` (root only), copied from
  Docker Desktop's credential store by `measure/p01_fastrund.py`; nothing prints them.

## The worker site

Since 2026-11-12 (the owner: "We have new worker machine, i want buildserver running there"; DESIGN.md §5.5). The worker
machine is our Proxmox node in Stuttgart (`K:\source\proxmox\workermachine`), on the home LAN through the link home.

```text
python buildserver/site/worker.py provision   # packages, podman's configuration, the coordinator's OpenRC service (idempotent)
python buildserver/site/worker.py deploy      # publish the coordinator + guest agent, swap them in, restart (refused while requests run)
python buildserver/site/worker.py restart     # after a settings change (worker/files/amb-coordinator.env, then provision)
python buildserver/site/worker.py status      # JSON: the container, the service, the coordinator, drive 2, the images
python buildserver/site/worker.py logs [-n N] | run -- podman ps
python buildserver/image/images.py --site worker base | e2e | build-image | check | list
```

- **Where:** CT 803 `buildserver`, a privileged Alpine container at `192.168.50.21` (`buildserver.local`) on the host's
  LAN bridge, made by `site/worker/create-ct803.sh` (its header says why each choice). Its root file system is on drive
  1 and backed up; everything the site holds (images, the build VM's disks, slots, the store, the git mirrors, the
  records, podman's storage) is on drive 2 (`/mnt/drive2/data/buildserver` = the container's `/srv/buildserver`), the
  owner's drive for "building and holding artifacts": data that may be lost (every build can be made again).
- **Why it is faster:** its VMs are plain KVM guests of the host (no nested virtualization, no WSL VM in between), on
  drive 2's SSD, with 128 GB of RAM and nobody at its screen. A build VM gets 16-32 GB and 8-16 vCPUs (4 projects × 3
  compilers at 32 GB), an e2e VM 6 GB uncapped with 4-6 vCPUs (`worker/files/amb-coordinator.env`).
- **Its capacity** (`SiteCapacity.LinuxFree`): the smaller of the HOST's available memory (its own `/proc/meminfo`,
  bind-mounted read-only into the container) minus 8 GB for the host's other guests, and the container's own (lxcfs:
  its 100 GB ceiling) minus 2 GB; 20 vCPUs; drive 2's free space minus 50 GB. No swap: a capped slot (`--guest-ram`
  above `--ram`) is refused when asked.
- **Its source:** it cannot read K:, so its mirror takes commits by push (`src/Amb.Cli/SourcePush.cs`): before a
  build or an e2e run, `amb` asks the site which commits it holds and sends a git bundle of only what it lacks (the
  whole history once, 174 MB in 50 s on 2026-11-12; then kilobytes), the submodule's commit likewise. The bundle is
  made in a scratch repository on Q: that borrows the repository's objects (git's alternates): nothing in K: changes.
  The site takes only `refs/push/<commit>` refs, verified before they are fetched. An e2e run's suite is a commit's
  `e2e/` folder written out of the mirror (`--suite-rev`, default HEAD), never a working tree.
- **Installs:** the site has no Windows beside it, so a deploy there fails at once. `amb install w…` runs at this PC's
  site: it fetches the build's layout zip and build.json from the worker into `Q:\buildserver\out\<id>` (SHA-256
  checked), has this PC's site import that folder under the SAME id (every file checked against the worker's record;
  `Imported`, its origin "pulled from worker"), then installs as before.
- **The service:** OpenRC's `amb-coordinator` under supervise-daemon (`worker/files/amb-coordinator.initd`): it starts at
  boot and again 5 s after any exit; a stop gives it 5 minutes to stop its slots. Its settings are
  `/etc/amb-coordinator.env`; it never idles out. `amb` wakes it through `ssh worker pct exec 803 --
  /srv/buildserver/bin/coordinator-up.sh` only when it does not answer.
- **Images are made there** (the owner's choice): `images.py --site worker` uploads the kit a request stages
  (`image/oem`, `image/steps`, VSSetup, `e2e/`) into `/srv/buildserver/kit/` first, SHA-256 checked; the ISO and the
  pinned downloads come from the worker's own internet connection.
- **cgroup v2 in a container** (`worker/files/amb-cgroups.start`, at every boot): the namespace root may hold no process
  when it delegates controllers, so its processes move to `/init` first; without it podman fails with "controller pids
  is not available". Alpine's LXC configuration drops `mknod`, which crun needs for a slot's `/dev/kvm`: CT 803 keeps it
  (`create-ct803.sh`).
- **Drive 2's controller cache** was found off on 2026-11-12 (the Smart Array's SSD Smart Path on drive 2's array
  bypasses the flash-backed write cache): podman's first writes ran at ~3 MB/s. With Smart Path off and the controller's
  cache on, drive 2 writes 222 MB/s and reads 395 MB/s (the drives' own write caches stay off, for power safety).

## What exists

| Path | What it is | State |
|---|---|---|
| [`site/sitectl.py`](site/sitectl.py) | makes, deploys into and inspects this PC's site: the WSL distro `buildserver`, its disk in `Q:\buildserver\wsl` (E2); publishes with the unpacked .NET SDK 10.0.404 in `Q:\tools` when present (`AMB_DOTNET` names another) | in use |
| [`site/files/`](site/files) | the configuration it installs (`/etc/wsl.conf`, podman's `containers.conf`), each file documented | in use |
| [`site/worker.py`](site/worker.py) | provisions, deploys into and inspects the worker machine's site (CT 803) over `ssh worker` | in use (2026-11-12) |
| [`site/worker/`](site/worker) | `create-ct803.sh` (makes CT 803 on the worker host) and `files/` (podman's configuration, the cgroup delegation, the OpenRC service, its settings and start script), each file documented | in use (2026-11-12) |
| [`image/`](image) | the image kit: `images.py`, the OEM bootstrap (`oem/`), the layer steps (`steps/`) | `am-base`, `am-e2e` and `am-build` made with it |
| [`steps/build/`](steps/build) | the build VM's steps, run with the image's PowerShell 7: `prepare-disk.ps1` (D:), `checkout.ps1` (the worktree from git bundles), `build.ps1` (the commit's own `tools\Build-Agentmaster.ps1`, with msbuild's binary log), `package.ps1` (the portable zip, the layout zip, the symbols, the logs, `build.json`); `sitectl.py deploy` copies them into the site, and each build stages them into its share | in use |
| [`steps/install/`](steps/install) | `install-build.ps1`: the Windows side of `amb install` (stage into the free slot, check, wait for the requesting session, the mutex, the swap, the watch), run by the coordinator through WSL's interop | in use (2026-10-29) |
| [`measure/`](measure/README.md) | the measurements and proofs behind DESIGN.md §16, with their raw results | P0.1 (§16.9), P0.5's comparison (§16.11) and P0.4's native half (§16.12) |
| [`src/Amb.Core`](src/Amb.Core) | the shared core: the allocation rule (§3, homepc's and a Linux site's), the request / build / slot / VM model (build ids by site letter), the guest and API protocol, the atomic record store (§6) | 31 unit tests (`src/Amb.Core.Tests`) |
| [`src/Amb.Coordinator`](src/Amb.Coordinator) | `amb-coordinator`: the API (artifacts streamed by name; the source mirrors' tips and pushed bundles), the scheduler (admission by priority × availability, wait and run timeouts, cancel, one build at a time, existing builds answered without a slot), the guest broker, the jobs (import, a pulled build too; smoke, install, layer, e2e, a suite out of the mirror too; build, deploy), the source mirror (fetching K:, or taking pushes; snapshots too) and its bundles, the slot manager and its drivers (podman + dockur, kept VMs with a data disk, capped slots with the site's swap file; cgroup v1 and v2 memory figures; local, for tests); its settings say which site it serves (homepc's or a Linux one) | runs in both sites; 62 unit tests (`src/Amb.Coordinator.Tests`) |
| [`src/Amb.GuestAgent`](src/Amb.GuestAgent) | `amb-guest.exe` (Native AOT, 7 MB): the agent in every VM; hello, heartbeat with the guest's memory, one step at a time by long poll (run, fetch, expand, put, shutdown) | in every slot |
| [`src/Amb.HostFacts`](src/Amb.HostFacts) | `amb-hostfacts.exe` (Native AOT): Windows' available memory, free commit, Q: free space and a full-screen game, as one line of JSON; the coordinator runs it every 10 s through WSL's interop | in use |
| [`src/Amb.Cli`](src/Amb.Cli) | `amb.exe`: the command line above; `Sites.cs` (the two sites, a build id's site by its letter), `SourcePush.cs` (a commit pushed to the worker as a bundle of what it lacks), `WorkingTreeSnapshot.cs` (`--snapshot`), `ProcessAncestry.cs` (the requesting claude.exe of `amb install`: the process chain, else `CLAUDE_PID`) | in use; 15 unit tests (`src/Amb.Cli.Tests`) |
| [`tests/local_chain.py`](tests/local_chain.py) | the whole chain on this PC without a VM: the coordinator with the local slot driver, the real guest agent, the real API | 15 checks pass |
| [`tests/site_outcomes.py`](tests/site_outcomes.py) | Phase 1's done-when on the real site: e2e runs that pass, fail, are cancelled and lose their VM, each leaving nothing behind | 14 checks passed (2026-10-25) |
| [`tests/site_build.py`](tests/site_build.py) | the build MVP's done-when on the real site: a build of HEAD, its manifest, an identical request, e2e on the build, fetch, no slot left, K: only read | 7 checks passed (2026-10-26, `b20261026-3`) |
