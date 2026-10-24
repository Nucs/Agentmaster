# Agentmaster build server — design and plan (third revision)

> **Status: DESIGN ONLY. Nothing here is built.**
>
> **The request, 2026-10-24:** "Building and installing and using VM to run e2e is very complicated, and very very
> resource heavy. We need a centralized server under buildserver/ that will use fastrun (K:/source/fastrun) to host two
> build servers machines (one here, another the machine K:/source/proxmox and the kubernetes if k8s has access to a
> lot of storage) … allocating resource using fastrun … building and/or running tests and/or releasing and/or
> end-to-end tests. This machine has RAM and storage issues and so allocating per resources is essential … stable and
> friendly to run on fastrun in a serverless manner (on demand)."
>
> **The owner's first correction, the same day:** "fastrun can run locally on this machine with no k8s/k3s … I intend
> you to use VM100 or Tiny11 Core 25H2 docker container which will also power e2e on this machine or k3s. Remember we
> can also utilize commited memory."
>
> **The owner's answers to the second design's decisions, the same evening:** "We already have caddy, fastrun and
> fastrund on this network … D1. filestore has to be on disk but overall the entire infra is lazy, always 'offline' as
> fastrun allows and when needed, quickly spins up or just accesses the ready machine. As we are talking about small
> jsons probably then use the docker's permanent disk storage. make sure read-write atomically to the file. D2. Yes.
> D3. separate, we use fastrun for this machine slots and proxmox+k3s has fastrun slots. D4. We won't work with VM100,
> we will spin up tiny11 when needed. D5. Why WSL? we don't use WSL anywhere in the plan. D6. check, u have kubectl to
> both local and k3s. D7. no grants, fully managed allocation of slot per required resources est, awaiting for pending
> (with timeout). This system gets instructed and does so we can ask for build which produces an artifact and a buildid
> and then ask to run e2e on the buildid's artifacts or run tests on the build. A build != publish so we have debug or
> release. D8. don't touch bot. D9. Defer D10. Defer D11. Defer D12. .NET 10 except for the e2e engine, python for
> comfort like in cicd or scripts for setup and such."
>
> **This revision replaces the second (317c4d6eb) and the first (58a31fd7c).** Both are in git history. §1 maps each
> answer to this design.
>
> Every number was measured on 2026-10-24 unless another date is given; §16 says how.

## 0. In one page

**What it is.** A service that does what it is told, on demand:
- **build** a commit as Debug or Release (dev or release identity) and get a **build id** with its artifacts;
- **test** a build (the engine harness and the other test kinds);
- **e2e** a build's artifacts with the e2e suites;
- later, **install** a build on this PC's dev or production instance, and **publish** a Release build as a GitHub
  release. A build is never a publish.

Every request carries a resource estimate. The service starts a slot where the estimate fits; otherwise the request
waits, up to its timeout. There are no approvals: what the service is told, it does.

**How it runs.** Everything is lazy: nothing runs while nothing is asked; a request either starts what it needs in
seconds or reuses a machine that is still up.
- **fastrun is the slot manager on both sites:**
  - **homepc:** a new fastrund node in Docker Desktop on this PC, an installation of its own. With no Kubernetes
    here, its records are small JSON files on a Docker volume, each written atomically.
  - **dock:** the cluster's fastrun (k3s), the second site. It runs Linux work today; Windows slots there need the
    four homelab changes of §5.3.
- **Windows work runs in Tiny11 Core 25H2 VMs** (dockur `core11`) that fastrun starts as slots and ends when done. One
  image: e2e VMs boot it fresh; a build VM boots it plus a build layer and keeps a data disk with the warm trees, and
  stays up 10 minutes for the next request ("the ready machine").
- **The coordinator** is a fastrun app on the homepc node; idle, it runs no process. Its state is small JSON files on
  the node's volume.
- **A small Windows host agent** does what only Windows can: the git source on K:, the artifact store on Q:, the
  installs, and Windows' own memory readings.
- **Reused as they are:** Caddy (CT 104) for a LAN name, the registry, fastrun's CLI and API, the e2e engine
  (Python), `tools\Build-Agentmaster.ps1`, `tools\Swap-AgentmasterLayout.ps1`.

**What fastrun must gain** (§9, the owner's project): a file record store, persistent volumes, `/dev/kvm` for a VM
slot, per-run limits, control of one run by its id with a graceful stop, and a capacity report.

**What this evening's checks changed:**
- **There is no local Kubernetes.** kubectl has one context, k3s (`https://192.168.50.150:6443`). Docker Desktop's
  Kubernetes is off (`KubernetesEnabled = False`), and neither WSL distro runs k3s, kind or a fastrund. This PC was
  checked with docker, PowerShell and wsl.
- **Docker Desktop runs on WSL 2** (`WslEngineEnabled = True`, distro `docker-desktop`, kernel
  `5.15.153.1-microsoft-standard-WSL2`). Every container on this PC, the claude-desktops Windows desktops included,
  runs in that one Linux VM: 31.2 GB of RAM and 8 GB of swap (WSL's defaults), shared with the `Ubuntu` distro. The
  plan uses nothing of WSL, but that VM is the homepc site's memory ceiling, and committed memory reaches the slots
  through it (§7).
- **The k3s side cannot run a Windows VM today** (§5.3): no `/dev/kvm` in the k3s LXCs, fastrund's pod is capped at
  1 GiB for everything it starts, the nodes have 2.3–12 GB of free disk, and dock has 6.15 GB of RAM available.
- **Disk is the binding constraint** (§8): Docker's data disk is 158.4 GB on C:, and 137.4 GB of it are two volumes
  of another project, unused for ~7 weeks. C: has 53.5 GB free and also holds the 113 GB page file. Q: has 94.7 GB
  free.

## 1. The owner's answers, and what each changes

| # | The question (second design) | The owner's answer | In this design |
|---|---|---|---|
| D1 | the record store of a fastrund with no Kubernetes | a file store on disk: small JSONs on Docker's persistent storage, read and written atomically; the whole infra lazy | fastrund's records (§9 F1) and the coordinator's (§6) are JSON files on Docker volumes, written by temp file + flush + rename. Nothing runs while idle |
| D2 | Tiny11 Core 25H2 as the base, the build tools only in a layer | yes | §4 |
| D3 | the VM manager | separate: fastrun manages the slots, on this machine and on proxmox + k3s | fastrun runs the VMs as slots (§5, §9). claude-desktops' Windows host stays as it is, for Claude's own desktops, and is not used |
| D4 | the dock side | no VM 100; Tiny11 when needed | VM 100 is out. Tiny11 slots on the k3s fastrun once §5.3's changes are made |
| D5 | the memory settings | "Why WSL? we don't use WSL anywhere in the plan" | §7: Docker Desktop is WSL 2 here, so its VM is the site's ceiling. The plan changes no WSL setting; raising that VM's size is a later option, after measuring |
| D6 | disk | "check" | measured on both sides (§8). Reclaim candidates are listed; nothing is deleted |
| D7 | approvals | no grants; managed allocation by resource estimate; pending with a timeout; build → build id → tests or e2e on it; a build is not a publish | §2 (the operations), §3 (the allocation). No approval anywhere |
| D8 | a Telegram bot | don't touch the bot | no bot, no Telegram |
| D9 | Docker Desktop on demand | defer | deferred: the homepc site exists while Docker Desktop runs (it does not start at sign-in, `AutoStart = False`) |
| D10 | the host native lane | defer | deferred |
| D11 | the GitHub lane | defer | deferred, and `publish` with it |
| D12 | the stack | .NET 10, except the e2e engine; Python for CI/CD-like and setup scripts | the coordinator, the agents and the CLI in .NET 10; the e2e engine stays Python (§10); the image and setup scripts in Python |

## 2. Operations

The service is instructed through one API (HTTP + JSON). The CLI `amb` and scripts use it. Every request returns its
id at once; `--wait` streams its events until it ends.

| Operation | Takes | Gives | Runs in |
|---|---|---|---|
| `build` | a commit (uncommitted work only as an explicit snapshot commit), `Debug` or `Release`, identity `dev` or `release`, purge `auto` / `force` / `none`, an optional resource estimate | a **build id** and its artifacts: the package layout, the portable zip, the build log and binlog, a manifest (commit, configuration, identity, versions, the SHA-256 of each artifact) | the build VM (§5.1) |
| `test` | a build id and a test kind: `m5` (the engine harness), `syntax` (the `/Zs` sweep), `installer` (`tools\tests\Test-AgentmasterInstaller.ps1`), more later | a result with its logs | the build VM, which holds the build's tree |
| `e2e` | a build id and the suite's arguments (`-m …`, `-k …`, the lane) | the results: JUnit XML, the run's evidence, logs | fresh e2e VMs (§5.1); the e2e engine unchanged inside them (§10) |
| `install` (later) | a build id and an instance (dev or production) | the swap's record | the host agent, through `Swap-AgentmasterLayout.ps1` |
| `publish` (deferred with D11) | a Release build id and a version | a GitHub release | — |

- **A build id** names an immutable result, `b<yyyymmdd>-<n>` (e.g. `b20261024-7`); its record holds the commit,
  configuration, identity and purge. Asking for a build that already exists (the same four) returns its id, or joins
  it while it runs.
- **Pending and timeouts:** a request waits until a slot fits it (§3), up to its wait timeout (defaults: build
  60 min, test 30 min, e2e 120 min). Then it ends as `no-capacity`, naming what was short. A running request has a run
  timeout too (build 3 h, test 1 h, e2e 2 h).
- **Cancel** ends a request in any state and releases its slot.
- **Who may instruct it:** anyone with a token. Claude sessions keep CLAUDE.md's rule: they ask the user before they
  ask the service for a build or an install.

## 3. Allocation: a slot per estimate

- **Every request has an estimate:** guest memory, vCPUs, disk growth, its run timeout, and the locks it needs (a build
  VM's data disk; an install's instance).
  - The first estimates are §3.1's. After ten runs of a kind, the median peak of its last ten runs plus a margin
    replaces them. A request may name its own.
- **homepc fits a request when all of these hold:**
  - Windows: free commit − reserve ≥ the slot's memory (the reserve: 16 GB, a setting; §7);
  - Docker's VM: its memory − what runs in it (the slots and everything else, such as the claude-desktops VMs) ≥ the
    slot's memory;
  - CPU: the running slots' vCPUs + this one ≤ the site's budget (24 of the 32 threads, a setting);
  - disk: the volume's free space − reserve ≥ the estimated growth, and C: keeps its 20 GB reserve (§7);
  - its locks are free;
  - no hold: a full-screen game (`SHQueryUserNotificationState` = `QUNS_RUNNING_D3D_FULL_SCREEN`) holds new slots.
- **dock fits a request** when fastrund's capacity report (§9 F7) leaves room for the slot plus 2 GB.
- **The order:** first come, first served. A request smaller than the free room may pass a larger one that waits,
  until that one has waited 10 minutes.
- **Enforcement:** fastrun holds the slot to its limits (cgroup memory, CPU, processes, timeout), and the VM's own size
  (`RAM_SIZE`, `CPU_CORES`) is the estimate. No slot gets more than it was given.
- **Pressure while slots run** (the host agent watches Windows, the coordinator decides): hold new slots, then shut
  down an idle build VM, and only at the floor (free commit under 2 GB twice) stop the newest slot. Pausing a slot
  instead comes with fastrun's freeze (§9 F6).

### 3.1 First estimates

| Request | Guest memory | vCPUs | Disk growth | Run timeout |
|---|---|---|---|---|
| Debug build, 3 compilers | 12 GB, plus a 32 GB page file in the guest | 8 | ~25 GB the first time, little after | 3 h |
| Release build, 3 compilers | 14 GB, plus the same page file | 8 | ~30 GB the first time | 3 h |
| `m5` test | 4 GB | 2 | — | 1 h |
| e2e VM | 8 GB (the e2e runner's default) | 6 | a few GB, deleted after | 2 h |

Native measurements behind them: a Debug build at 3 compilers took free commit from 22.9 to 14.2 GB (2026-10-21); a
Release build from 34.1 to 26.3 GB (2026-10-24); the TerminalApp PCH is 1.8–2.3 GB per compiler. P0.4 measures them
again inside a VM.

## 4. The image: Tiny11 Core 25H2 in two layers

- **What Tiny11 Core is.** dockur lists `VERSION=core11`, "Tiny11 Core", a 3.0 GB download; its change #1513 moved
  Tiny11 to 25H2. In tiny11builder's own words (`docs/usage.md`), Tiny11 removes the bundled apps, "Edge/WebView2 and
  OneDrive binaries"; Core also deletes most of WinSxS, "removes WinRE and disables Windows Update and Defender. These
  changes cannot be reversed. Use only for test VMs." No language, feature or update can be added afterwards.
- **Built once** by the build server's own image script (`buildserver/image/`, Python, D12), on dockur's unattended
  install, the way claude-desktops builds its golden image (claude-desktops itself is not used, D3). Two qcow2 disks
  on Q:, the second over the first:
  1. **`am-core11`, the base, which every VM boots:**
     - the guest agent `amb-guest` (§10) as a service;
     - Python 3.13 and the e2e runner's test tooling: PowerShell 7 and the runner's venv in `C:\e2e`, and crash dumps
       on (WER LocalDumps), as `e2e/guest/provision.ps1` installs them after every reset today;
     - **not in it:** `claude.exe` and Git. The Standard lane installs them per run from its cache. The install lane
       runs without them today (no install scenario starts a session), and a `%USERPROFILE%\.local\bin\claude.exe`
       would change what the app finds at its first start.
  2. **`am-core11-build`, the build layer, which only build VMs boot:** VS 2022 Build Tools with the build subset of
     the repository's `.vsconfig` (44 components with IDE parts; a build needs MSBuild, `VC.Tools.x86.x64`,
     `Windows11SDK.22621`, `MSIX.Packaging`, `UWP.Support`, `NetFX.Native`, `Vcpkg` and the .NET SDK), Git and
     .NET 10.
     - **Why a layer:** a test machine with a compiler, the Windows SDK or a VC++ runtime installed can pass a test
       that a user's PC fails, for example a portable zip that is missing a runtime DLL.
- **Settings for both:** auto-logon, UAC off, updates off, no inbound firewall rule (the agent dials out).
- **e2e on Tiny11 Core is not e2e on Windows 11.** A suite moves to `am-core11` only after P0.5 shows the same results
  on both images. The install lane, which tests what Windows does around an installer, and a weekly run of every suite
  stay on a full Windows 11 25H2 image (E4).
- **It is proven before anything depends on it (P0.3, P0.5).** If Tiny11 Core cannot carry VS Build Tools, the build
  or the app, the same two layers go on a full Windows 11 25H2 image; only the size and the idle memory change.

## 5. Sites and slots

### 5.1 Slot kinds

| Kind | Boots | Disks | Life |
|---|---|---|---|
| `win-build` | the build layer | the image (read-only) + an OS overlay and a data disk, both kept per build VM | started by a request; stays up 10 min after its last step for the next one; then Windows shuts down and the run ends; its overlay is renewed when a layer changes |
| `win-e2e` | the base (or the full image) | the image (read-only) + a throwaway overlay | one run, deleted in every outcome; an install-lane "reset" is a fresh slot |
| `linux` | a container | none, or a volume | the coordinator; small jobs |

- **A VM slot is a run of one fastrun release, `am-win`** (the image `dockurr/windows:6.05`), started with:
  - `/dev/kvm`, root and a writable root inside the slot (§9 F3);
  - dockur's user-mode networking, `NETWORK=passt`: no `NET_ADMIN` and no `/dev/net/tun` (read in dockur's base
    `qemus/qemu`, `src/network.sh`). The guest reaches the coordinator through it; nothing reaches the guest;
  - volumes: the image folder (read-only), the slot's disks, and its exchange folder, which the guest sees as a share
    (§9 F2);
  - its limits = the estimate (§9 F4); env `RAM_SIZE`, `CPU_CORES` and the disks' names.
- **The build VM's tree stays warm** on its data disk: one worktree per configuration, the NuGet and vcpkg caches, and
  the guest's page file. Builds in it run one at a time. A second build VM, with a data disk of its own, starts only
  when the budget has room for it.

### 5.2 homepc (this PC)

- **The node:** fastrund in a Docker Desktop container, set up like the k3s pod (privileged, a read-only root, its
  store, scratch, data and run folders on volumes, its own tokens file), with `--restart unless-stopped`. The lab of
  2026-10-05 ran fastrund 0.3.0 this way on this PC (node `lab`).
- **Its ports** are published to this PC only: the API `2100` and the activators `2101`–`2199`. The coordinator's
  port is also open to Caddy (`192.168.50.5`) by one firewall rule, the catalog's pattern, for `https://build.local`.
- **Capacity, measured:** 32 CPUs; Docker's VM 31.2 GB of RAM + 8 GB of swap, shared with the `Ubuntu` distro and the
  claude-desktops desktops; `/dev/kvm` in the VM; Windows: 63.7 GB of RAM (18.7 GB free), a commit limit of 174.2 GB
  (42.7 GB free), the page file 113 GB (17 GB used) on C:.

### 5.3 dock (k3s)

- **The cluster's fastrun 1.2.1** (three nodes) is the second site. It runs Linux slots today.
- **A Windows slot there needs four changes, each measured missing today:**
  1. `/dev/kvm` inside CT 113 / 114: absent, and their configs have no kvm line. `cgroup2.devices.allow=a` already
     allows the device, so each needs one `lxc.mount.entry` and a restart of that node;
  2. room in fastrund's pod: its memory limit is 1 GiB for everything it starts (requests 10m CPU, 32 MiB). VM slots
     need it raised to their sizes;
  3. disk: CT 113 has 11 GB free, CT 114 12 GB, CT 112 2.3 GB. A Tiny11 base (~10–15 GB) and an overlay need a new
     mount point of ~40 GB from the thin pool, which is at 71.45 %; 40 GB takes it to ~76 %, and its alert starts at
     80 %;
  4. RAM: dock has 6.15 GB available (VM 100 holds 16 GiB; the LXCs and VM 105 most of the rest), 8 GB of swap with
     1 GB used, KSM on (138 MiB merged now). One 4–6 GB e2e VM fits, with nothing to spare.
- **So dock takes e2e slots only, one at a time,** after those changes (E3, the owner's go). Builds stay on homepc.

### 5.4 claude-desktops

Separate (D3). Its Windows desktops share Docker's VM with the homepc site, so the allocator counts their memory as
load it does not own, and never starts or stops them.

## 6. Records: small JSON files, written atomically

- **Where:** the Docker volume `buildserver-state` on the homepc node, mounted into the coordinator's slot (§9 F2).
  fastrund's own records (§9 F1) live on the same disk, in its store volume.
- **What:**
  - `requests/<id>.json`: one per request (kind, inputs, estimate, state, times, slot, the result's summary), at most
    64 KB;
  - `builds/<build id>.json`: the manifest (commit, configuration, identity, the artifacts' paths and SHA-256);
  - `slots/<slot id>.json`: what runs where, with its limits;
  - `vms/<name>.json`: each kept build VM (its disks, the layer it was made from, its last use);
  - `estimates.json`: the rolling peaks per kind; `config.json`: the reserves, budgets and timeouts;
  - `requests/<id>.events.jsonl`: the request's events, one line each, appended (`O_APPEND`) and flushed per line.
  - Build logs, binlogs and test evidence go to the artifact store on Q:, never into JSON.
- **The write:** serialize → write `<name>.json.tmp-<pid>-<n>` in the same folder → `fsync` the file → `rename` it
  over `<name>.json` (an atomic replace on Linux) → `fsync` the folder. A reader reads the whole file and never a
  `.tmp-` file, so it sees the old version or the new one, never a mix. Start-up deletes `.tmp-` files a crash left.
- **One writer:** the coordinator runs one instance (`--max-instances 1`) with one lock per record. Each record carries
  a `version`; a write replaces only the version it read (compare-and-swap; a mismatch reads again and retries).
- **Lazy:** the coordinator keeps nothing that the files don't hold, so it can stop between any two writes and start
  again from them.

## 7. Memory: commit, physical, and why WSL appears

**The chain on this PC (measured 2026-10-24 evening):**

```text
Windows    63.7 GB RAM (18.7 GB free); commit limit 174.2 GB (42.7 GB free) = RAM + page file 113 GB on C: (17 GB used)
 └ vmmemWSL  Docker Desktop's WSL 2 VM: commit 5.1 GB, working set 1.8 GB (Windows pages what Linux leaves cold)
    └ Linux    31.2 GB RAM + 8 GB swap (WSL's defaults; one VM for docker-desktop and Ubuntu); KSM present, off
       └ fastrund slots   cgroup limits: memory, CPU, processes
          └ QEMU             the guest's RAM_SIZE
             └ Windows guest  its own page file on its disk: MSVC's commit peaks land here
```

- **Why WSL appears.** Docker Desktop's engine runs in WSL 2 here, so every container on this PC lives in that VM. The
  plan uses no WSL feature and changes no WSL setting. But the VM's size (WSL's `memory=`, half of RAM by default) is
  the homepc site's ceiling, and it is the path by which committed memory reaches the slots: the VM's memory is
  charged to Windows as commit and paged by Windows (5.1 GB committed, 1.8 GB resident when measured).
- **Using committed memory.** A slot's memory may exceed the free physical memory when free commit covers it (§3's
  first test). Pages a guest never touches cost nothing; pages it touched and left cold go to the page file. Inside a
  build VM, the guest's own page file takes MSVC's peaks.
- **C: is the commit's disk.** The page file (113 GB, system-managed) lives on C:, which has 53.5 GB free, and Docker's
  data disk is on C: too. When C: fills, the page file cannot grow, and the commit limit stops growing with it. So the
  build server's disks grow on C: only within a budget (§8), and the coordinator keeps 20 GB free on C:, like the
  commit reserve.
- **Later, measured options** (not in the first phases): raise the VM's size if slots ever need more than ~25 GB at
  once; turn KSM on in Docker's VM so e2e VMs booted from one image share pages. P0.6 measures both.

## 8. Disk (D6: checked)

### 8.1 Measured, 2026-10-24 evening

| Where | Size / free | What holds it |
|---|---|---|
| C: | 1,862 GB, **53.5 GB free** | the page file (113 GB); Docker's data disk `docker_data.vhdx` 158.4 GB; Ubuntu's `ext4.vhdx` 37.4 GB |
| inside Docker's data disk | images 25.9 GB (19.4 reclaimable), containers 4 GB, volumes 145.5 GB, build cache 0.2 GB | volumes: `rdpdoorway-win11-target` 68.7 GB and `rdpdoorway-win11-test` 68.7 GB (another project; their containers exited ~7 weeks ago), `claude-desktop-win-1-storage` 4.9 GB, BuildKit 2.3 GB |
| Q: | 931.5 GB, **94.7 GB free** (112 GB in the afternoon) | the owner's data (AlphaVantage 206 GB, nautilus 98 GB, "Datacenter from C drive" 75 GB, ComfyUI 47 GB, ThetaData 47 GB, llm 44 GB, …) and Agentmaster's: `Q:\tmp` 78.3 GB (the build trees, 52 GB, and many `agentmaster-e2e-*` session folders), `Q:\claude-desktops` 64 GB, `Q:\agentmaster-e2e` 37.7 GB, `Q:\amwt-v067` 21.4 GB (a worktree from July), `Q:\am-build` 17.3 GB (July), `Q:\agentmaster-layouts` 4.4 GB |
| K: | 447 GB, **11.6 GB free** | the source tree; nothing of the build server goes here |
| k3s nodes | CT 112 7.8 GB (2.3 free), CT 113 24 GB (11 free), CT 114 40 GB (12 free) | fastrun stores 0.5 / 2.8 / 3.1 GB; local-path volumes (the registry 10 GiB, teleport 12 GiB, …) |
| dock's thin pool | 794 GiB, 71.45 % used, oversubscribed | — |

### 8.2 The build server's budget

| Item | Size | Where | Why there |
|---|---|---|---|
| the image: base + build layer | ~40 GB (to measure) | Q: (`Q:\buildserver\images`), mounted read-only | only read; the fleet's golden runs this way at ~125 MB/s |
| a full 25H2 image (install lane, weekly run) | 14 GB | the fleet's file on Q:, read-only (E4) | one copy |
| one build VM: OS overlay + data disk (two worktrees ~55 GB, caches ~5 GB, the guest's 32 GB page file) | ~100 GB | a volume inside Docker's data disk | a build's many small writes need ext4 on the virtual disk, not a Windows folder (P0.4 measures both) |
| e2e overlays | a few GB each, deleted | Docker's data disk | — |
| exchange folders (source bundles in, artifacts out) | ~5 GB | Q: (`Q:\buildserver\exchange`), mounted | the host agent reads them as files |
| the artifact store | 40 GB, oldest out first | `Q:\buildserver\store` | installs and e2e zips are taken from it |
| records | < 100 MB | a Docker volume | D1 |

- **It does not fit today without reclaiming space** (E1): the build VM needs ~100 GB in Docker's data disk, which
  is on C: (53.5 GB free, 20 GB of it kept for the page file), and Q: has 94.7 GB free.
- **Reclaim candidates, each the owner's call; this design deletes nothing:**
  1. the two `rdpdoorway-win11-*` volumes (137.4 GB in Docker's data disk), then a compaction of `docker_data.vhdx`,
     which gives C: the space back;
  2. 19.4 GB of unused Docker images;
  3. the stock build trees in `Q:\tmp` (52 GB), once the build server has replaced hand builds;
  4. session leftovers on Q: (`Q:\agentmaster-e2e` 37.7 GB, `Q:\tmp\agentmaster-e2e-*`, `Q:\amwt-v067` 21.4 GB,
     `Q:\am-build` 17.3 GB).
- **From then on the build server owns its space:** everything it makes lives under `Q:\buildserver` and its Docker
  volumes, each item with a budget and oldest out first. Sessions stop leaving build outputs elsewhere.

## 9. What fastrun must gain (the owner's project)

Read in `K:\source\fastrun`:
- an app takes no limits ("an app's instances share the pod's memory and process limits", `AppReleases.cs`);
- a run outlives its caller ("fastrun never cancels a run because its caller went away", `docs/progress.md`), but a
  call sets only `args`, `env`, `stdin`, a shorter `timeout`, `idempotency_key`, `node` and `not_before`, and
  `Runs.Stop` ends every run of a release on a node at once (`proto/fastrun/v1/fastrun.proto`);
- a slot gets no devices, and a writable root gets eleven capabilities (`Runtime.cs`);
- with no ServiceAccount, releases, apps and schedules answer `UNAVAILABLE` (`Kube.cs`).

| # | Feature | Why | Done when |
|---|---|---|---|
| F1 | a file record store (`FASTRUN_RECORDS_DIR`), used when no ServiceAccount is mounted: the same calls as `Kube.cs` (Secrets: list by label selector, watch with bookmarks, get, create, replace by `resourceVersion`, delete; the Lease: get, create, replace), each record a JSON file written by §6's write, the counters in an atomically written file, watches in-process | releases, apps and schedules on the homepc node (D1) | fastrun's conformance test passes on a homepc node, and a `kill -9` in the middle of a write leaves every record readable |
| F2 | volumes: named folders that outlive runs (`--volume name:/path[:ro]`), and mounts of node paths on an allow-list (`FASTRUN_BIND_ALLOW`) | the coordinator's records, a build VM's disks, the image folder, the exchange folders | a volume outlives runs, revisions and fastrund restarts |
| F3 | VM slots: devices from a node allow-list (`FASTRUN_DEVICES_ALLOW=/dev/kvm`), root and a writable root, and capabilities from an allow-list if dockur needs more than the eleven | Tiny11 VMs | a dockur VM boots in a slot with `NETWORK=passt` |
| F4 | per-run limits that may lower the release's: memory, CPU, processes | a slot sized to each request | a run with a lower limit is held to it |
| F5 | one run by its id: a start returns the run's id at once; a watch streams that run's state; a stop of that run alone sends SIGTERM, waits a grace period, then kills | VM runs of hours; several VMs of one release at a time; Windows shuts down cleanly; the coordinator can idle out while a build runs | the coordinator restarts mid-build, finds its run by id, and stops it without touching the others |
| F6 (later) | freeze and thaw a run (the cgroup freezer) | pause under pressure, resume at once | — |
| F7 | capacity in the node status: memory total and available, CPUs, the running slots' limits | admission | — |

On dock, also fastrund's pod memory limit and the LXC changes of §5.3.

## 10. The agents, the CLI and the e2e engine

- **The guest agent** (`amb-guest`, .NET 10, a service in the base image) dials out to the coordinator through the
  slot's user-mode network; nothing listens in the guest (P0.2 checks how the guest reaches the coordinator through
  passt). It runs a request's steps:
  - fetch the source bundle from the exchange share into the data disk's clone;
  - build with `tools\Build-Agentmaster.ps1` of that commit;
  - run a test, or the e2e engine's guest half;
  - write artifacts to the exchange share, stream logs, and report the guest's memory and CPU.
- **The host agent** (`amb-host`, .NET 10, a logon scheduled task outside every Agentmaster instance, like
  `llm-bridge`):
  - keeps a bare mirror of `K:\source\Agentmaster` on Q: and writes source bundles into exchange folders;
  - moves artifacts from exchange folders into the store and checks their SHA-256;
  - for `install` (later): copies a layout into a Q: slot and runs `Swap-AgentmasterLayout.ps1`;
  - reports Windows' free commit, free physical memory, C:'s free space and a full-screen game.
- **The CLI `amb`** (.NET 10): `amb build release --at <commit>`, `amb test <build id> m5`,
  `amb e2e <build id> -- -m "not slow"`, `amb status`, `amb logs -f <id>`, `amb cancel <id>`,
  `amb artifacts <build id>`. `--wait-max 270` exits 97 while the request still runs (the e2e runner's convention);
  `--json` everywhere.
- **The e2e engine stays Python** (D12):
  - its guest half (`e2e/guest/provision.ps1`, `run_suite.py`) runs unchanged in an e2e slot;
  - its host half gets the build server as a second transport beside claude-fleet: a slot instead of a desktop, a
    fresh slot instead of a reset, the exchange folder instead of the persist folder.

## 11. Source in, artifacts out

- **Builds come from commits, never from the live tree.** The host agent keeps a bare mirror
  (`Q:\buildserver\git\agentmaster.git`; the repository's packed history is 198 MB) and fetches from
  `K:\source\Agentmaster` by path. Uncommitted work builds only when asked, as a snapshot commit under
  `refs/buildserver/snapshots/<id>`; no branch or HEAD moves.
- **Into a VM:** a git bundle of what the VM's clone lacks, written into its exchange folder.
- **Out of a VM:** the guest writes the layout, the zip and the logs to its exchange share; the host agent moves them
  into the store and records their SHA-256 in the build's manifest.
- **The build steps come from the commit** (its own `Build-Agentmaster.ps1`). The install steps come from the host
  agent's versioned copy (`buildserver/steps/`), so installing an old build still uses today's swap script.
- **Purge decisions:** the build-install skill's trigger files (`SessionModels.h`, `SessionRegistry.*`, any `.idl`,
  widened for a TerminalControl `.idl`), new data members in a TerminalApp header, or `--purge force`; and one purge and
  retry after a link error on a symbol the source defines.

## 12. Life cycle and stability

- **A request's states:** `pending → starting → running → collecting → succeeded | failed | cancelled | no-capacity |
  lost`, each change an event line.
- **Heartbeats:** the guest agent every 30 s. A slot silent for 3 minutes is stopped and its request marked `lost`;
  a build is retried once.
- **Restarts:** the coordinator reads its records and fastrund's runs (§9 F5) and carries on.
- **The homelab is not needed** for homepc work once the dockur image is in the node's store.
- **Docker Desktop not running:** the homepc site is down, and requests wait until their timeout. Who starts Docker
  Desktop is D9, deferred.
- **Coexistence:** hand builds of the stock tree keep working; the allocator sees their memory as load it does not own.

## 13. Plan

Each phase needs the owner's go to build and deploy, and ends with checks that a run proves.

**P0, proofs (no instance touched; each with a fallback):**

| # | Proof | Pass | Fallback |
|---|---|---|---|
| P0.1 | fastrund 1.2.1 in a Docker Desktop container: ad-hoc runs, its ports from Windows, its idle cost | runs answer; idle CPU and memory recorded | — |
| P0.2 | a dockur VM as a fastrun slot, on a test build of fastrund with F2 + F3 prototyped: `NETWORK=passt`, `/dev/kvm`, the image mounted read-only; how the guest reaches the coordinator | Windows boots and the guest reaches a port of the node | run the VMs as plain Docker containers until F3 is done |
| P0.3 | the two layers: Tiny11 Core 25H2 + the e2e tooling + the guest agent, then VS Build Tools over it | both install; sizes and idle memory recorded | the same layers on a full 25H2 image |
| P0.4 | a build in the VM: Debug and Release, cold and incremental, 2–3 compilers, the guest page file; the data disk in a Docker volume and on a Q: mount | times within ~1.5× of native (to be agreed); no `C3859`; peaks recorded | — |
| P0.5 | every e2e suite on the base and on the full 25H2 image, side by side | the same results, each difference explained | the full image for each suite that differs |
| P0.6 | memory: vmmemWSL's commit and working set with slots running; a slot larger than free physical memory; KSM in Docker's VM with two e2e VMs | numbers recorded; no failure | lower budgets |

**The phases after P0:**

| Phase | Delivers | Done when |
|---|---|---|
| **P1** (fastrun repo) | F1–F5, F7 | each feature's "done when" of §9 |
| **P2** | the coordinator, its records, the host agent, the guest agent; `build` and `test`; the CLI | a Debug build of HEAD gets a build id and its artifacts while free commit stays above the reserve; two identical requests → one build; a request asked under pressure waits and starts by itself, or ends `no-capacity` at its timeout; nothing is written to K: |
| **P3** | `e2e` on the image P0.5 chose per suite; the install lane on the full image; the weekly full run | a full suite from `amb e2e <build id>`; no slot left after a success, a failure or a cancel |
| **P4** | `install` | a dev install of a build id; an injected failure after the swap's stop rolls back |
| **P5** | dock e2e slots (§5.3's changes) | an e2e run on dock while homepc builds |
| deferred | `publish` and the GitHub lane (D11), Docker Desktop on demand (D9), the host native lane (D10), freeze (F6) | — |

## 14. Decisions still open

| # | Decision | Recommendation |
|---|---|---|
| E1 | reclaim space: the two `rdpdoorway-win11-*` volumes and the unused images in Docker's data disk; later the stock build trees and the Q: leftovers | the volumes and images first (157 GB, nothing of Agentmaster's), then the rest after P2 |
| E2 | where the build VM's disks live: Docker's data disk on C: (after E1), or that disk moved to Q: (Docker Desktop's disk image location) | C:, after E1, with the 20 GB reserve for the page file |
| E3 | the homelab changes for dock e2e slots (§5.3) | after P3 |
| E4 | the install lane's full 25H2 image: read the fleet's golden file, or build one of our own | read the fleet's file (one copy on Q:) |
| E5 | the reserves and budgets: 16 GB of commit, 20 GB on C:, 24 vCPUs | set after P0.6 |

## 15. Risks

| Risk | How it is contained |
|---|---|
| Tiny11 Core lacks something VS Build Tools, the build or the app needs, and cannot be serviced to add it | P0.3 / P0.5; the full 25H2 image is the same plan with a bigger disk |
| a test passes on Tiny11 Core where a user's Windows 11 fails, or the reverse | P0.5's side-by-side run; the install lane and a weekly full run on the full image; no build tools in the image e2e VMs boot |
| builds under nested virtualization (KVM inside WSL 2) are too slow | P0.4; hand builds of the stock tree remain until then |
| the fastrun features take longer than planned | P0.2's fallback runs the VMs as plain Docker containers; the coordinator's design does not change |
| Docker's data disk fills C: and starves the page file | the 20 GB C: reserve in admission; E1 and E2 |
| Tiny11 Core gets no security updates | the VMs are LAN-only, take no inbound connection, live for one request or a few minutes after it, and run only this repository's code; the image is rebuilt when dockur ships a newer Tiny11 Core |
| dock's thin pool fills | dock takes e2e slots only, after E3, with ~40 GB measured against the 80 % alert |

## 16. Evidence (2026-10-24)

- **This PC:**
  - `kubectl config get-contexts`: one context, server `https://192.168.50.150:6443` (k3s);
  - `%APPDATA%\Docker\settings-store.json`: `WslEngineEnabled = True`, `KubernetesEnabled = False`,
    `AutoStart = False`, `Cpus = 32`; `wsl -l -v`: `Ubuntu` and `docker-desktop`, both running, version 2; inside:
    kernel `5.15.153.1-microsoft-standard-WSL2`, 31,952 MB of RAM and 8,192 MB of swap, `/dev/kvm` present,
    `/sys/kernel/mm/ksm/run` = 0; no k3s, kind, minikube or fastrund in either distro;
  - `Win32_OperatingSystem` / `Win32_PageFileUsage`: 63.7 GB of RAM, 18.7 GB free; commit limit 174.2 GB, 42.7 GB
    free; page file 113,140 MB allocated, 17,055 MB used; `vmmemWSL`: private bytes 5.1 GB, working set 1.8 GB;
  - drives: C: 53.5 GB free of 1,862 GB, Q: 94.7 GB of 931.5 GB, K: 11.6 GB of 447 GB; `docker_data.vhdx` 158.4 GB
    and Ubuntu's `ext4.vhdx` 37.4 GB, both on C:; `docker system df -v` for the volumes; Q:'s top-level folders summed
    file by file;
  - `docker ps -a`: no fastrund container (only `fastrun-aot` and the BuildKit builder); a claude-desktops dockur
    desktop running.
- **dock**, read-only over SSH: `free -m` (6,152 MB available; swap 8,191 MB, 1,018 used), KSM `run=1`,
  `pages_sharing=35370`; `lvs pve/data` 71.45 %; `pct config` of CTs 112–114 (8 / 10 / 10 GiB, swap 0, rootfs 8 / 24
  / 40 GB) and `df` inside them; `/dev/kvm` on dock itself.
- **k3s**, kubectl: fastrund's DaemonSet requests 10m CPU and 32 MiB, limits 1 GiB; node allocatable memory 8 / 10 /
  10 GiB; `local-path` the only storage class; the PVCs.
- **fastrun** (`K:\source\fastrun`): `Kube.cs` (Secrets and one Lease; "a local run outside k3s: then releases are
  off"; `FASTRUN_KUBE_URL`), `Settings.cs` (limits clamped at 86,400 s, 64 GiB and 65,536 processes; the store,
  scratch and data folders), `AppReleases.cs` (apps refuse limits), `Runtime.cs` (no devices; a writable root's
  eleven capabilities), `proto/fastrun/v1/fastrun.proto` (`InvokeRequest` without per-run limits; `Runs.Stop`),
  `deploy/k8s/fastrun.yaml` (the pod: privileged, host network, read-only root, its folders), `docs/progress.md`
  ("fastrun never cancels a run because its caller went away"), and
  `apps/yoinks/results/2026-10-05/lab-nodes.txt` ("fastrund 0.3.0 … starting on lab (127.0.0.1)").
- **dockur:** `qemus/qemu` `src/network.sh` (master, read 2026-10-24): `NETWORK` = `passt` / `slirp` / `user`
  selects user-mode networking; the bridge and macvtap modes need `NET_ADMIN` and `/dev/net/tun`.
- **Tiny11:** dockur/windows's README (`core11`, 3.0 GB) and its change #1513; tiny11builder's README and
  `docs/usage.md` (branch `winui-preview`).
- **The homelab doc** (`K:\source\proxmox\CLAUDE.md`): dock (30 GiB, 8 GiB swap, KSM via ksmtuned), the LXCs'
  configuration, the thin pool and `dock-pool-watch`'s 80 % alert, Caddy's workstation-service pattern (catalog,
  docfx), the fastrun DaemonSet and its 1 GiB pod.
- **e2e:** `e2e/host/e2e_run.py` (the claude-fleet transport, 8 GB / 6 CPUs by default), `e2e/guest/provision.ps1`
  (what each lane installs), `e2e/README.md`.
