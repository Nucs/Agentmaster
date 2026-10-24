# Agentmaster build server — design and plan (revised)

> **Status: DESIGN ONLY. Nothing here is built.**
>
> **The request, 2026-10-24:** "Building and installing and using VM to run e2e is very complicated, and very very
> resource heavy. We need a centralized server under buildserver/ that will use fastrun (K:/source/fastrun) to host two
> build servers machines (one here, another the machine K:/source/proxmox and the kubernetes if k8s has access to a
> lot of storage) … allocating resource using fastrun … building and/or running tests and/or releasing and/or
> end-to-end tests. This machine has RAM and storage issues and so allocating per resources is essential … stable and
> friendly to run on fastrun in a serverless manner (on demand)."
>
> **The owner's corrections to the first design (58a31fd7c), the same day:** "fastrun can run locally on this machine
> with no k8s/k3s … I intend you to use VM100 or Tiny11 Core 25H2 docker container which will also power e2e on this
> machine or k3s. Remember we can also utilize commited memory."
>
> **This revision replaces the first design.** That one is in git history. §0 lists what changed.
>
> Every number below was measured on 2026-10-24 unless another date is given; §13 says how.

## 0. In one page

**What it is.** One queue and one resource allocator for every Agentmaster build, check, test, e2e run, deploy (swap)
and release. It replaces hand-run scripts copied between session scratchpads, builds in the live working tree, and e2e
desktops someone must remember to delete.

**What changed from the first design:**

1. **fastrun runs on this PC.**
   - fastrund 1.2.1 runs as a Docker container in Docker Desktop (WSL2), with no k3s. A lab of 2026-10-05 already ran
     fastrund (0.3.0, as node `lab`) and Caddy in Docker on this PC.
   - On a local node, its run limits can be raised to **24 h and 64 GiB per run** (the clamps in fastrun's
     `Settings.cs`).
   - The coordinator and every job's Linux-side steps run there, scaling to zero.
   - k3s becomes optional.
2. **The Windows capacity is one image, `am-core11`.** It is **Tiny11 Core 25H2** in a dockur container (dockur's
   `VERSION=core11`) with the e2e tooling baked in. The same image powers builds and e2e, as two roles:
   - a **build VM**: the image, plus a **build layer** (VS Build Tools and the agent) and a persistent data disk that
     keeps the warm build trees. Only build VMs get the layer, so no test machine holds a compiler, an SDK or a VC++
     runtime that could hide one the app fails to ship;
   - **e2e VMs** made fresh for each run, as today.
   - They run on this PC now, and on k3s once its nodes see `/dev/kvm`.
   - **VM 100** on dock is the second build machine: it has 16 vCPU and 16 GiB already reserved, and ~11 GB of them
     idle in the guest.
3. **Memory is budgeted in two currencies:**
   - **commit** (capacity): the PC has 174 GB of commit on 64 GB of RAM; its 113 GB page file is mostly untouched;
   - **physical** (speed).
   - Guest page files let a build VM run larger than the free RAM, paging instead of failing; so does WSL's VM if it
     is backed by Windows virtual memory, as P0.6 checks.
   - Pausing a VM (Docker's freezer) replaces killing it.

**The shape:**
- a fastrun node on this PC hosts the coordinator app `buildserver` and its jobs;
- **claude-desktops' Windows host**, extended, starts, stops, pauses and sizes the dockur VMs;
- a small agent in each Windows VM does the build;
- a host agent on Windows does what only the host can: the instances' swaps, the Q: slots, the git source, `gh`;
- VM 100 runs the same agent natively.

**What the code says must still be true or built** (the honest limits):
1. **Releases need a record store.** A local fastrund runs releases, apps and schedules only with a Secrets-and-Lease
   store. Without one it answers `UNAVAILABLE` for them and runs ad-hoc code only (`Kube.cs`). Either fix is small,
   and D1 picks:
   - a ~300-line stand-in for those few API calls, or
   - a file-backed record store in fastrund.
2. **fastrun cannot run a dockur VM yet.** Its slots have no capabilities and no devices (no `/dev/kvm`). So Docker
   enforces the VMs' budgets, and fastrun and the coordinator allocate them. Running the VMs inside fastrun is
   phase P6, a fastrun feature.
3. **Four things must be measured before anything else (P0):**
   - VS Build Tools and the Agentmaster build on Tiny11 Core;
   - build speed under nested virtualization (KVM inside WSL2);
   - the data disk's I/O;
   - whether the e2e suites behave on Tiny11 Core as on full Windows 11. Core has no Edge, no WebView2, no WinRE, and
     Windows Update and Defender are off, so a suite moves to it only after a side-by-side run, and the install lane
     stays on the full 25H2 image.
4. **Disk is the scarcest resource.** Docker Desktop's data disk is a 159 GB VHDX on C:, which has ~54 GB free.

## 1. Why: what goes wrong today

| Problem | Evidence |
|---|---|
| The machinery is hand-run scripts in temp folders | `stagebuild.ps1`, `prodswapR.ps1`, `devbuildswap.ps1`, `stagegate.ps1` live in session scratchpads under `%TEMP%`, copied from session to session (memory `swap-scripts-scheduled-task`). |
| Builds starve the PC | 2026-10-23 → 24: four attempts of one purged Debug build were stopped by the memory guard (free commit 0.1–7 GB beside two Windows test desktops, ~32 GB of WSL and ~58 claude processes); the sixth took 1,239 s at one compiler. |
| e2e is the heaviest thing on the PC | A test desktop takes 8 GB and 6 CPUs (`e2e_run.py`'s defaults); a full suite runs ~37 min plus provisioning; an install-lane reset takes 35–70 s on a quiet PC and 7–10 min beside three other desktops; someone must delete the desktop. |
| Builds use the live working tree | A build waits up to 10 min for other sessions' edits, or builds over them (`-AllowDirtyBuildFiles`); the stock tree's `Generated Files` keeps Debug and Release from building at once. |
| No sharing, no queue | One mutex (`am-lock.sh`) serializes everything; two sessions that want the same build build it twice. |
| Outcomes are not recorded | The production swaps of 10-21 (12 min down) and 10-24 (refused, `0x80073CFB`) were found only in transcripts (commit a05c0c2ff). The swap script's fixes are in 278e644f7. |
| Disk | C: 54.8 GB free (Docker's VHDX 159 GB there), K: 11.8 GB, Q: 112 GB (the stock build trees take another 52 GB there; the desktops' folders 51 GB; the e2e golden 14 GB). |

## 2. Requirements

| # | Requirement | Source |
|---|---|---|
| R1 | One central service in `buildserver/`, on fastrun, running only on demand | the request |
| R2 | Two build machines: this PC and the Proxmox side; k3s for what it can carry | the request |
| R3 | Life cycle of builds, tests, releases and e2e runs | the request |
| R4 | Per-resource allocation that protects this PC's RAM and disk | the request |
| R5 | Stable: survives restarts, crashes, a homelab outage and a power cut; never leaves an instance down | the request; the 10-21 outage |
| R6 | Friendly for Claude sessions and the owner (phone included) | the request |
| R7 | The repository's rules: permission before a build, deploy or install; every build writes to Q:; never stop `OpenConsole.exe`; the swap and purge rules; secrets never printed | CLAUDE.md |
| R8 | fastrun runs on this PC, with no k3s needed | the correction |
| R9 | One Windows image, Tiny11 Core 25H2 in Docker, powers builds and e2e, here or on k3s; or VM 100 | the correction |
| R10 | Committed memory counts as a resource, not only free RAM | the correction |

## 3. What the machines can give (measured 2026-10-24)

| Machine | What it has | Free now | Role in this design |
|---|---|---|---|
| **homepc** (this PC, 192.168.50.17) | i9-13900K, 32 threads; 64 GB RAM; **commit limit 174.2 GB** (`C:\pagefile.sys`, system-managed, 113 GB allocated, 17 GB used, 30.6 GB at peak); RTX 4060 Ti; VS 2022 | RAM 12.5–14.8 GB free; commit 40–55 GB free (~134 GB committed while ~49 GB of RAM and 17 GB of page file are in use: most commit is never touched); C: 54.8 GB, Q: 112 GB, K: 11.8 GB free | The main site: the local fastrun node, the Windows VMs, the host agent, the instances. |
| **Docker Desktop on homepc** | WSL 2.3.24 (kernel 5.15.153.1); 32 CPUs, 31.2 GiB (WSL's default of half the RAM; no `.wslconfig`); Docker 27.3.1; KVM inside WSL2; data disk `docker_data.vhdx` **159 GB on C:** | runs the fleet's Windows host (`claude-desktops-winhost`, :7790), the desktops, fastrun's AOT builder and BuildKit | Hosts the local fastrund and the dockur VMs. |
| **The fleet's Windows VMs** | dockur `dockurr/windows:6.05`, Windows 11 Pro 25H2 golden (14 GB on Q:), a qcow2 overlay per desktop; create 46–59 s, reset 33–70 s (2026-10-02) | one desktop running (another session's), one stopped | The model for `am-core11`'s two roles. |
| **dock** (Proxmox 8.2.7) | Ryzen 9 6900HX, 8 cores / 16 threads; 31.3 GiB (2 × 16 GB DDR5, board maximum 64 GB); KVM nesting on; NVMe thin pool 794 GiB | 6.2 GiB RAM available; thin pool 71.4 % used, 237 GB available, oversubscribed | Hosts VM 100; k3s VMs need RAM. |
| **VM 100 `win11-1`** on dock | Windows 11, **16 vCPU** (`qm config`; the homelab doc's table says 8c), 16 GiB pinned by its GPU passthrough (seven `hostpci` devices), one 96 GB disk; TraderLab's first compute target since 2026-10-02 ("VM 100 has higher priority", the owner); TraderLab's agent dials out and is installed through `qm guest exec` | the guest uses 5.8 GB, **11.35 GB free inside** (the balloon driver's figures; already reserved on dock) | The dock-side build worker: the agent runs natively. Containers inside it (nested virtualization under its `-hypervisor,kvm=off` CPU flags) are untested and not needed. |
| **k3s** (CTs 112–114 on dock) | privileged LXCs with every device allowed; root filesystems 7.8 / 24 / 40 GB; storage `local-path` | nodes at 14–17 % memory; **no `/dev/kvm` inside CT 113 / 114** (a bind-mount line is missing) | Optional: dockur VMs as pods once `/dev/kvm` is mounted and dock has RAM. |
| **fastrun 1.2.1** (cluster) | 9 releases; activators 2101–2106 taken | — | Unchanged. The build server needs nothing from it. |
| **GitHub Actions** | public repository: free `windows-2022` runners | CI x64 Release 17–26 min; releases 26–30 min | Optional lane for pushed commits. |
| **QNAP NAS** | RAID 5, ~14 TB | offline 2026-10-07 → 17 | Nothing. |

**What fastrun is, locally** (read in `K:\source\fastrun`):
- **It runs anywhere Docker runs a privileged container.** fastrund needs only Linux namespaces, cgroups, crun and its
  store folders.
- **Kubernetes is used for two things only:** release records (Secrets in one namespace) and the scheduler's Lease
  (`Kube.cs`). With no ServiceAccount mounted, "releases are off and their methods answer UNAVAILABLE". Ad-hoc code
  still runs.
- **`FASTRUN_KUBE_URL`** may point at any address, plain HTTP included.
- **Its limits are settings,** clamped in `Settings.cs`:
  - releases: `FASTRUN_RELEASE_TIMEOUT_S` ≤ 86,400 s and `FASTRUN_RELEASE_MEMORY_MIB` ≤ 64 GiB;
  - ad-hoc runs: the same (`FASTRUN_ADHOC_TIMEOUT_S`, `FASTRUN_ADHOC_MEMORY_MIB`);
  - both kinds' process limits ≤ 65,536 (`FASTRUN_RELEASE_PIDS`, `FASTRUN_ADHOC_PIDS`).
  - The cluster's 300 s / 512 MiB (ad-hoc 30 s / 256 MiB) are its own choices.
- **A slot runs as uid 65532** with no capabilities, `no_new_privs`, a seccomp filter and no device access. A dockur
  VM (`/dev/kvm`, root, networking) cannot run inside one today.

**What Tiny11 Core 25H2 is:**
- dockur lists `VERSION=core11`, "Tiny11 Core", a 3.0 GB download. Its change #1513 moved Tiny11 to 25H2.
- In tiny11builder's own words (its `docs/usage.md`): Tiny11 removes the bundled apps, "Edge/WebView2 and OneDrive
  binaries"; Core also deletes most of WinSxS, "removes WinRE and disables Windows Update and Defender. These changes
  cannot be reversed. Use only for test VMs." No language, feature or update can be added afterwards.
- For a build VM, the disabled Defender is a gain: nothing scans the compiler's output. For e2e it is a difference
  from a user's PC, and so are the missing Edge and WebView2 (§6).

## 4. Architecture

```text
 who asks                         homepc (this PC)                                              dock (Proxmox)
┌──────────────┐   ┌────────────────────────────── Docker Desktop (WSL2, KVM) ─────────────┐   ┌─────────────────────┐
│ amb (CLI)    │──▶│ fastrund "homepc" (local node, privileged)  + record store (D1)        │   │ VM 100 win11-1      │
│ web page     │   │   app `buildserver` :21xx  (coordinator: API, page, allocator)          │◀──│  amb agent (native, │
│ Telegram     │   │   schedule `buildserver-tick`; Linux-side job steps                     │   │  dials out), D: data│
└──────────────┘   │ claude-desktops-winhost :7790 (VM manager, extended)                    │   └─────────────────────┘
                   │   am-core11 (Tiny11 Core 25H2 + e2e tooling) + build layer (VS), Q:     │   ┌─────────────────────┐
                   │   ├ build VM: build layer + OS overlay + data disk (warm trees,         │   │ k3s (optional, D4): │
                   │   │           caches, guest page file); paused / stopped when idle      │   │  am-core11 pods once│
                   │   └ e2e VMs:  base + throwaway overlay, one per run                     │   │  /dev/kvm + RAM     │
                   └─────────────────────────────────────────────────────────────────────────┘   └─────────────────────┘
                     host agent (Windows, logon task, outside every instance): git source, Q: store and
                     slots, stage/swap (Swap-AgentmasterLayout.ps1), e2e_run.py, gh, the native fallback lane
```

### 4.1 fastrun on this PC

- **The node.**
  - A container `fastrund` in Docker Desktop: the production image (`fastrund:20261012-2`), privileged, with
    `--restart unless-stopped`. Its store and data folders sit in Docker volumes.
  - A **local registry** container sits beside it, so the node needs nothing from the homelab after its first pull.
    The 10-05 lab ran fastrund and Caddy this way (`apps/yoinks/lab/`, `results/2026-10-05/lab-*`); the registry
    beside them is new.
  - Local limits:
    - releases: 24 h, 4 GiB. Linux-side steps are light, because the build itself runs in a Windows VM;
    - ad-hoc runs: 1 h, 1 GiB.
  - Its own tokens file (`FASTRUN_TOKENS_FILE`). The `fastrun` CLI reaches it with `FASTRUN_URL`.
- **The record store (D1).** fastrund needs one to run releases, apps and schedules:
  - **(a) A file-backed record store in fastrund** (`FASTRUN_RECORDS_DIR`: records and the single node's Lease in its
    store folder). A fastrun change, tested on `fastrun-next` first per fastrun's rules. After it, fastrun runs
    anywhere Docker runs, for every user. **Recommended.**
  - **(b) A stand-in API** in `buildserver/fastrun-local/`: ~300 lines serving exactly what `Kube.cs` and
    `Releases.cs` call:
    - Secrets in one namespace: list with label selectors, watch, get, create, update, delete, with
      `resourceVersion`;
    - one Lease.
    - It is file-backed. fastrund gets a ServiceAccount folder (token, CA, namespace) and
      `FASTRUN_KUBE_URL=http://127.0.0.1:<port>`. No fastrund change.
  - **(c) Ad-hoc only.** No records: the coordinator runs as a plain container and every job step is an ad-hoc run. It
    is the fallback while (a) or (b) is not ready.
- **Ports.** The API (2100) and the activators (2101–2199) are published to the PC's address. Docker Desktop's port
  publishing or its host networking; P0 settles which. The page gets a Caddy name like `catalog.local`:
  `https://build.local` → 192.168.50.17:21xx.
- **The coordinator `buildserver`** is an app release on this node, from the first design:
  - .NET 10 Native AOT, stateless, scales to zero (`--idle 60`).
  - **Its records:** a PostgreSQL container beside fastrund (a Docker volume on the same disk plan, §4.9), so the PC
    needs nothing from the cluster. The cluster's idle PostgreSQL is the alternative (D1b).
  - **The tick:** an alarm clock — a record `wakeAt` read by a one-minute schedule `buildserver-tick` that wakes the
    app only when due — and an hourly audit.
- **The cluster's fastrun is not needed.** The same release could be deployed there unchanged if the owner ever wants
  the coordinator off the PC.

### 4.2 One Windows image for builds and e2e: `am-core11`

- **Two layers, both qcow2 disks on Q:, each built once** by an extended `images/windows/build_golden.py`
  (claude-desktops), on dockur `VERSION=core11` (Tiny11 Core 25H2):
  1. **`am-core11`, the base, which e2e VMs boot.**
     - What the fleet's golden carries today (its `oem/provision.ps1`: Python 3.13, Node 22, Playwright MCP, ffmpeg,
       the fleet agent's logon task, the guest firewall rule, updates and nags off). The fleet's browser tools drive
       Edge, which Core lacks; the e2e suites do not use them.
     - The test tooling that `e2e/guest/provision.ps1` installs after every reset today: PowerShell 7 and the
       runner's venv in `C:\e2e`, and its crash-dump settings (WER LocalDumps).
     - **Not in it:** `claude.exe` and Git. The Standard lane installs them per run, from its cache on `Z:`. The
       install lane runs without them today (no install scenario starts a session), and a
       `%USERPROFILE%\.local\bin\claude.exe` would change what the app finds at its first start.
     - With this, the Standard lane's provisioning shrinks to claude, Git and the build under test. The install
       lane's took 78 s after a reset (since that lane's fixes of 2026-10-24; ~6.4 min before them) and shrinks to its
       kit and test accounts.
  2. **`am-core11-build`, the build layer, which only build VMs boot:** an overlay over the base (a two-level qcow2
     chain; P0.2 checks that dockur boots it) with
     - **VS 2022 Build Tools** with the build subset of the repository's `.vsconfig`. That file lists 44 components
       with IDE parts; a build needs MSBuild, `VC.Tools.x86.x64`, `Windows11SDK.22621`, `MSIX.Packaging`,
       `UWP.Support`, `NetFX.Native`, `Vcpkg`, and the .NET SDK. ARM64 toolsets only if ARM64 builds move off GitHub;
     - Git, .NET 10, and the in-guest agent (`amb agent`, a Windows service).
     - **Why a layer:** a test machine with a compiler, the Windows SDK or a VC++ runtime installed can pass a test
       that a user's PC fails, for example a portable zip that is missing a runtime DLL.
- **Settings for both:** auto-logon, UAC off, a guest firewall rule for the gateway subnet only, updates off (the
  fleet's settings today).
- **It is proven before anything depends on it (P0).** If Tiny11 Core cannot carry VS Build Tools, the build or the
  app (it cannot be serviced: a missing component stays missing), the same two layers sit on the fleet's full
  Windows 11 25H2 golden. Only the size and the idle RAM change.
- **e2e on Tiny11 Core is not e2e on Windows 11** (§3: no Edge, no WebView2, no WinRE; Windows Update and Defender
  off).
  - The UI suites move to `am-core11` once P0.5 shows the same results on both images.
  - The install lane tests what Windows does around an installer, so it stays on the full 25H2 golden.
  - A weekly run of every suite on the full golden catches what Core hides.
- **Two roles:**

| Role | Disks | Size per start (the allocator's choice) | Life |
|---|---|---|---|
| **build** | the build layer (read-only) + an OS overlay + a **persistent data disk `D:`** (dockur's second disk): the git mirror's clone, one worktree per configuration, NuGet and vcpkg caches, and a **guest page file** | 8–24 vCPU, 8–24 GB RAM | started for a job, **paused** between jobs (Docker's freezer: no CPU; its memory stays allocated, idle), **stopped** after N idle minutes (the data disk stays); its OS overlay is renewed when a layer changes |
| **e2e** | the base (read-only) + a throwaway overlay (today's model) | 4–6 vCPU, 6–8 GB | made for one run and **deleted in every outcome**; an optional warm pool of one for up to 20 min while e2e jobs are queued |

### 4.3 Where the Windows capacity runs

| Place | What runs there | What it needs |
|---|---|---|
| **homepc** (now) | the build VM and e2e VMs | Docker Desktop running (it does not start at sign-in today); the disk plan (§4.9) |
| **VM 100** (P4) | `amb agent` natively in its Windows: builds only, no e2e (it is the owner's machine, with TraderLab and a GPU; e2e needs clean machines) | a data disk for ONE Release worktree and its caches, at most ~40 GB of real data (dock's thin pool is at 71 % and oversubscribed, and its alert starts at 80 %; 40 GB takes it to ~76 %); a CPU cap, a Job Object CPU rate inside the guest (no change on dock), so dock's other guests keep their share and its heat stays down (dock runs warm: after the 10-04 power cut its BIOS stopped the boot with "CPU Over Temperature"); TraderLab first (D4) |
| **k3s** (optional, D4) | `am-core11` VMs as privileged pods, the fleet's way | a `/dev/kvm` bind mount in CT 113 / 114 (a homelab change; it restarts that node) and RAM on dock: today 6.2 GiB, enough only for one small e2e VM; more if VM 100 is stopped or dock gets 2 × 32 GB |

### 4.4 The VM manager: claude-desktops' Windows host, extended

- **Today** it creates, resets, deletes and lists dockur VMs on this PC with a chosen RAM and CPU count
  (`RAM_SIZE`, `CPU_CORES`), each a qcow2 overlay over the golden on Q:, with a persist folder that the guest sees as
  `Z:`.
- **It gains:**
  1. an image chosen per request (`win11-25h2`, `am-core11`, `am-core11-build`), the last a two-level qcow2 chain;
  2. a persistent second disk for the build role (a named volume, dockur's `DISK2_SIZE`);
  3. start, stop, pause and resume;
  4. the numbers the allocator needs: the WSL VM's memory, each VM's resident memory and CPU;
  5. roles as labels.
- The buildserver calls it directly on this PC (`:7790`, its own token). The hub in k3s stays for Claude sessions'
  desktops and is not on the build path.
- **One VM manager for the PC means one budget.** e2e desktops, build VMs and the agents' own manual-QA desktops are
  counted together.

### 4.5 The agents

- **In-guest agent** (`amb agent`, .NET 10, a service baked into the build layer `am-core11-build`):
  - runs a job's steps in a Job Object;
  - reports to the coordinator through the VM's gateway (the fleet's Caddy gateway already holds a bearer token per
    desktop);
  - keeps the guest's resource readings.
- **Host agent** (`amb agent --host`, Windows, a logon scheduled task like `llm-bridge`), outside every Agentmaster
  process tree, so no swap can end it. It does the host-only work:
  - fetching `K:\source\Agentmaster` into a mirror on Q:;
  - snapshots of uncommitted work, on the user's word;
  - moving and verifying artifacts into Q:'s store and slots;
  - stage and swap with `tools\Swap-AgentmasterLayout.ps1` (since 278e644f7: it refuses an installed package unless
    `-ReplaceInstalledPackage -RollbackBundle`, and rolls back on any failure after its stop);
  - `e2e/host/e2e_run.py` and `install_lane.py`;
  - `gh`, with the user's login, never copied anywhere.
  - It also runs the **native fallback lane**: a build on the host in a Job Object, the first design's mechanism, for
    when Docker Desktop is off or the image is not ready.
- **VM 100's agent:** the same program as a service in VM 100. It **dials out** to the coordinator, as TraderLab's
  agent does, so VM 100 gets no new listener. It is installed and updated the way TraderLab's agent was, through the
  PVE guest agent (`qm guest exec`). On 2026-10-24 that guest agent did not answer a `get-fsinfo` probe, so P4 checks
  it first.

### 4.6 Source in, artifacts out

- **Builds come from commits, never the live tree.**
  - The host agent keeps a bare mirror (`Q:\buildserver\git\agentmaster.git`, 198 MB today). It fetches from
    `K:\source\Agentmaster` by path.
  - Every job names a full commit. Uncommitted work builds only on the user's word, as a snapshot commit under
    `refs/buildserver/snapshots/<id>`; no branch or HEAD moves, and the binaries bake that commit.
- **Into a VM.** The host agent writes a git bundle (the objects since that VM's last build) into the VM's persist
  folder. The guest sees it as `Z:`. The in-guest agent fetches from it into the data disk's clone. VM 100 gets the
  same bundle over the coordinator.
- **Out of a VM.** The guest writes the layout, the zip and the logs to `Z:\out\<job>`. The host agent moves them into
  `Q:\buildserver\store\`, checks their SHA-256, and copies a staged layout into the instance's free slot
  (`Q:\agentmaster-layouts\…`). Building straight into the slot is a host-native-lane feature.
- **Build steps come from the commit** (`tools\Build-Agentmaster.ps1` in the worktree). **Deploy steps come from the
  host agent's own versioned copy** (`buildserver/steps/`), so a rollback to an old build still swaps with today's
  fixed script.

### 4.7 Jobs and where they run

| Kind | What it does | Runs on (first choice → fallback) | Default approval (§4.11) |
|---|---|---|---|
| `compile-check`, `lib-check` | `cl /Zs` of the changed units; a `*Lib.vcxproj` build | build VM (warm PCH) → host native | auto |
| `m5` | the engine harness at the commit, in a scratch profile | build VM → host native | auto |
| `build` | a full package build, Debug or Release, dev or release identity, purge `auto` / `force` / `none` | build VM → VM 100 → host native → GitHub (Release, pushed commits) | ask |
| `zip` | a portable zip from a layout (`New-UnpackagedTerminalDistribution.ps1`, its `Layout` set) | where the layout is | auto |
| `stage` | a build delivered into the instance's free slot, checked and pinned | build VM + host agent | ask |
| `swap` | `Swap-AgentmasterLayout.ps1` with its checks; waits for the requesting session's turn first | host agent | ask, every time |
| `e2e`, `install-lane` | a suite against a build's zip, in fresh e2e VMs | host agent + e2e VMs | ask |
| `release` | the release pipeline with its gates (§7) | host agent + GitHub | ask, every gate |

- **Chains:** `deploy dev` = `stage` → approval → `swap`; `test <build>` = `zip` → `e2e`.
- **Deduplication:** an identical request joins the queued or running job, and an identical finished job's artifacts
  are reused.
- **Purge decisions** are the coordinator's, per worktree:
  - the skill's trigger files (`SessionModels.h`, `SessionRegistry.*`, any `.idl`, widened for a TerminalControl
    `.idl`);
  - new data members in a TerminalApp header;
  - `--force-purge`;
  - one automatic purge and retry after a link error on a symbol the source defines.

### 4.8 Memory: commit and physical (the allocator)

**Why two currencies:**
- MSVC fails on **commit**, not on RAM. `C3859` "Failed to create virtual memory for PCH" and `C1076` are commit
  failures; the TerminalApp PCH is 1.8–2.3 GB per compiler.
- A machine pages when **physical** memory runs out, and work slows; nothing fails.
- On this PC, commit is plentiful and physical is scarce: 174 GB versus 64 GB, with ~134 GB committed but ~49 GB of RAM
  and 17 GB of page file in use.
- So a job is charged in commit (a hard budget) and in physical (a soft one that decides speed and order).

**The layers, from the outside in:**

| Layer | What it holds | Its cap | How it pages |
|---|---|---|---|
| Windows (homepc) | everything | the commit limit (174 GB; the page file is system-managed, on C:) | Windows pages untouched memory, a WSL VM's included (to confirm in P0: WSL's VM is backed by Windows virtual memory) |
| WSL's VM | fastrund, the containers, every dockur VM | `.wslconfig` `memory=` (31.2 GiB today, the default; D5 raises it) | WSL's own swap (`swap=`, `swapFile=` on Q:); `autoMemoryReclaim` returns freed memory to Windows |
| Each Windows VM | the guest | its `RAM_SIZE` (hard); Docker's `--memory` on its container (QEMU's overhead included) | the **guest page file on its data disk** (e.g. 32 GB): a build VM with 12 GB of RAM and 32 GB of page file runs three compilers by paging instead of failing |
| Each job inside | MSBuild, `cl`, `link` | a Job Object commit cap; the compiler count from the VM's room | — |
| VM 100 | its guest | 16 GiB pinned on dock (passthrough) | its own page file; its 11 GB of idle RAM is capacity dock already reserved |

**The admission rule** for a new VM or a job in a running one:

```text
host free commit − reserve_commit        ≥ VM RAM growth + guest overhead     (homepc reserve: 16 GB, the owner's knob)
WSL headroom (memory= − in use)          ≥ VM RAM growth
physical: interactive jobs need available − reserve_phys ≥ their working set; background jobs may start paged
disk, locks, approval, and no hold (a full-screen game, quiet hours, a manual pause)
```

**The responses to pressure, gentlest first** (the host agent watches; the coordinator decides):

1. Hold new jobs.
2. **Pause** the lowest-priority build VM: `docker pause`, Docker's cgroup freezer, the same idea as fastrun's frozen
   apps. It uses no CPU and keeps its memory allocated but idle, which WSL's swap and Windows can page out (P0.6
   measures how much they give back). It resumes at once, and no work is lost.
3. Lower a running VM's CPU (`docker update --cpus`).
4. **Stop** an idle build VM. Its data disk keeps the warm trees.
5. Only at the floor (free commit under 2 GB twice) end one job.
   - It is the job's tree inside the VM, never another session's process.
   - The first design killed builds at this point; pausing comes first now.

**Other behavior:**
- **Games.** A full-screen D3D app (`SHQueryUserNotificationState` = `QUNS_RUNNING_D3D_FULL_SCREEN`) holds new work
  and pauses build VMs. e2e VMs are never paused mid-test, since their timing matters.
- **dock.** VM 100's jobs are admitted against its guest's free memory (11.35 GB today). k3s VMs are admitted against
  dock's `MemAvailable` (PVE API) with 2 GiB to spare.
- **Profiles.** A job's request starts from measured values, then becomes the median peak of the same kind's last ten
  runs:
  - Debug, 3 compilers: ~9 GB of commit;
  - Release, 3 compilers: 8–14 GB;
  - `m5`: ~4 GB;
  - an e2e VM: its RAM plus 1 GB.

### 4.9 Disk

| Item | Size | Where (D6) |
|---|---|---|
| `am-core11` base | ~10–15 GB qcow2, sparse (to measure in P0.2; the fleet's full 25H2 golden is 14 GB, and Tiny11 Core is smaller) | Q: (`Q:\claude-desktops\windows\golden\…`, read-only, like today's) |
| `am-core11-build` layer | ~20–30 GB qcow2 over the base: VS Build Tools and the SDKs (to measure in P0.2) | Q:, beside the base |
| build VM data disk | ~100 GB, sparse: two worktrees ~55 GB (Debug ~25, Release ~30), caches ~5 GB, page file 32 GB | a fast disk. QEMU ran the fleet's golden from a bind-mounted Windows folder at ~125 MB/s (the fleet's note): fine for a disk that is only read, likely too slow for a build's many small writes (P0.3 measures it). Options: a Docker volume inside the VHDX (on C:, which is short), the VHDX moved to Q:, or an ext4 VHDX on Q: attached with `wsl --mount --vhd` (administrator; a logon task) |
| e2e overlays | a few GB each, deleted | Docker's VHDX |
| store (layouts, zips, results, logs) | budget 40 GB, LRU | `Q:\buildserver\store` |
| reclaimable | the stock build trees `Q:\tmp\agentmaster-*` (52 GB) once builds move into VMs; Docker's 159 GB VHDX compacted (`docker system prune`, then `Optimize-VHD`) | — |

- **No build writes K:.** No build writes C: except through Docker's VHDX, which §4.9's choice moves or bounds.

### 4.10 Life cycle and stability

- **Jobs.** From the first design:
  - a state machine (`queued → waiting-approval → waiting-resources → assigned → preparing → running → collecting →
    succeeded | failed | cancelled | lost`), with every move an event row;
  - idempotent submission keys;
  - leases renewed by heartbeats every 30 s, with fencing: an agent that cannot renew ends its job.
- **The build VM's states:**

  ```text
  absent → starting → ready → busy → ready(paused) → stopped → (starting)
  ```

  - Creating it the first time takes ~1 min plus the warm-up build.
  - A start from stopped is a boot (~1 min, to measure); a resume from paused is instant.
  - The VM is never deleted except to renew its OS overlay. The data disk is its value.
- **Swaps** run on the host agent, detached from its own process, so an agent restart never ends one halfway. Since
  278e644f7 the script itself refuses an installed package without its rollback bundle and rolls back after any
  failure past its stop. The agent watches the new instance (hooks.log milestones from the recorded offset) and
  records the run.
- **Without the homelab, nothing changes:** the local fastrun node, its registry, PostgreSQL, the VM manager and the
  agents are all on this PC.
- **Without Docker Desktop:** the coordinator is down with it. `amb --local` talks to the host agent, which runs the
  native lane under the `build-launch` mutex, and uploads the records later.
- **Coexistence.** Hand builds of the stock tree keep working. The allocator sees their memory. Stage and swap take
  `build-launch` like the hand flows.

### 4.11 Approvals, interfaces, security (from the first design, unchanged)

- **Approvals.** A policy the owner sets once:
  - `auto`: compile checks, `m5`, zips, CI;
  - `ask`: builds, stages, e2e;
  - `ask-every-step`: swaps and releases.
  - Approvals come from Telegram (a bot of its own), the page with the owner's token, or `amb approve`.
- **The command line, `amb`:** `amb build debug [--at <commit>] [--snapshot] [--wait]`, `amb check`, `amb test m5`,
  `amb deploy dev`, `amb e2e --build <job> -- <pytest args>`, `amb status | jobs | logs -f | cancel | pause | resume`,
  `amb --local …`.
  - It streams events (no polling).
  - `--wait-max 270` exits 97 while running, like `e2e_run.py`. Exit codes: 0, 1, 97, 98. `--json` everywhere.
- **The page** at `https://build.local`:
  - the queue;
  - each machine with live bars: commit, physical, WSL memory, each VM, disk;
  - each job's steps, log and resource curve;
  - approvals, the policy, the swap history.
- **Security:**
  - LAN only, with a token per agent, the owner and the tick;
  - secrets in `Q:\services\buildserver\secrets\` (ACL) and the local node's Secrets, written on stdin, never
    printed;
  - `gh` only on the host agent;
  - the PVE token (VM 100's guest exec and power only) only if P4 uses it.

## 5. The dock side

**VM 100 (recommended for P4)** is the cheapest second machine:
- **Already reserved:** its 16 GiB are pinned on dock today, and 11.35 GB of them are free in the guest. A Release
  build at two or three compilers (8–14 GB of commit, §4.8) fits there, paging into the guest's page file at its
  peak, without taking a byte more from dock.
- **Shared with TraderLab.** Since 2026-10-02 VM 100 is TraderLab's first compute target (the owner: "VM 100 has
  higher priority"). The build server is its second user:
  - the agent takes a job only while TraderLab's agent is idle, and builds at below-normal priority;
  - when a TraderLab job starts, the agent drops the build's Job Object CPU rate to its minimum until that job ends.
- **What it needs:**
  1. the agent as a service;
  2. VS Build Tools with the same component subset as `am-core11-build`;
  3. a data disk `D:` for one Release worktree and its caches, at most ~40 GB of real data. dock's thin pool is at
     71 % and oversubscribed (1.96 TiB of thin volumes on 794 GiB, LVM's warning of 2026-10-11), and
     `dock-pool-watch` alerts from 80 %; 40 GB takes it to ~76 %. Debug builds stay on homepc;
  4. a CPU cap while building (it has all 16 of dock's threads): a Job Object CPU rate inside the guest, so dock's
     configuration does not change;
  5. the guest agent answering again (§4.5).
- **Its known quirk:** the guest clock runs ~4.6 % slow (homelab §4, open since 2026-10-01). Build timestamps can
  notice it, and the agent checks the time before a build.

**k3s (optional):** `am-core11` VMs as privileged pods with `/dev/kvm`, through the fleet's hub, which "accepts
several peers". It needs a bind mount of `/dev/kvm` into CT 113 / 114 (their config already allows every device) and
RAM: one 6 GB e2e VM fits in today's 6.2 GiB only with nothing to spare. So this is for e2e after a RAM upgrade or
while VM 100 is stopped.

## 6. e2e on the same image

- **The job** wraps `e2e/host/e2e_run.py` and `install_lane.py` unchanged; their design stays theirs (`E2E_TESTING.md`).
  It does:
  1. take an e2e slot from the allocator;
  2. start Docker Desktop if needed;
  3. create an e2e VM and hand it the build's zip. The VM boots the `am-core11` base, or the full 25H2 golden for
     the install lane, the weekly full run and any suite P0.5 keeps there (§4.2);
  4. run the suite and collect the results into the store;
  5. **delete the VM in every outcome.**
- **The gain from one image:**
  - the Standard lane's provisioning only copies `claude.exe`, Git and the build under test (the venv and
    PowerShell 7 come with the image);
  - the install lane's per-scenario resets stay overlay resets;
  - the build VM and the e2e VMs share one base image, one VM manager and one budget.
- **Not a user's PC.** Tiny11 Core differs from Windows 11 (§3), so a result on `am-core11` replaces a full-golden
  run only for the suites P0.5 compared.
- **What `E2E_RELIABILITY.md` / `E2E_CRASH_GUARD.md` plan** (PLAN ONLY) runs inside the job when they are built.

## 7. Releases

Unchanged from the first design: `amb release <version>` runs the `release-version` skill's pipeline with gates:
1. the version (approval);
2. a clean Release build of the exact commit: GitHub's CI run, or a clean build VM worktree;
3. the tag by the host agent's `gh` → `release.yml` → the six assets checked;
4. the notes' commit list;
5. publishing (approval);
6. production's update, by its install kind.

## 8. Data model and API (sketch)

- **Tables:** `machines` (homepc, the build VM, VM 100, k3s), `vms` (role, size, state, disks), `jobs`, `steps`,
  `events`, `approvals`, `artifacts`, `samples` (commit, physical, WSL, per VM), `logs`, `trees`, `swap_runs`,
  `policy`.
- **API v1:**
  - `POST /jobs`, `GET /jobs/{id}/events` (SSE), `GET /jobs/{id}/log`, `POST /jobs/{id}:cancel`;
  - `POST /approvals/{id}:answer`;
  - `GET /machines`, `POST /machines/{id}:pause|resume`;
  - `GET /artifacts`, `GET|PUT /policy`;
  - agent-facing: report, heartbeat, log, complete;
  - coordinator → VM manager: create, start, stop, pause, resume, delete, stats;
  - `POST /internal/tick`.

## 9. What goes where in `buildserver/`

| Folder | What |
|---|---|
| `DESIGN.md` | this file |
| `control/` | the coordinator (.NET 10 minimal API, page, allocator), its Dockerfile |
| `agent/` | `amb.exe`: the CLI, the host agent, the in-guest agent, VM 100's agent (one program, three modes) |
| `shared/` | the API contract and protocol version |
| `steps/` | the deploy steps, versioned here instead of in scratchpads: stage, gate, swap, post-swap watch, zip, e2e wrappers |
| `fastrun-local/` | running fastrund on this PC: the container's settings, the registry, PostgreSQL, and (if D1 = b) the record-store stand-in |
| `image/` | the two layers' provisioning, handed to claude-desktops' `build_golden.py`: the base (the e2e tooling) and the build layer (the build subset of `.vsconfig`, the agent service) |
| `deploy/` | install scripts (the host agent's task and firewall rule, the local node, the Caddy name as a note for the homelab repository), `make-secrets.py` |
| `tests/` | the allocator's admission rules, the purge rules, the state machines; a conformance run |

## 10. Plan

Each phase needs the owner's go to build and deploy (R7), and ends with checks a run proves.

**P0 — proofs (no instance touched; each is a go/no-go with a fallback):**

| # | Proof | Pass | Fallback |
|---|---|---|---|
| P0.1 | fastrund 1.2.1 as a container on this PC: ad-hoc runs, then releases, an app and a schedule through (a) or (b), ports reachable from Windows and the LAN, its idle footprint | everything answers; the coordinator's skeleton app scales to zero and back | (c) ad-hoc only |
| P0.2 | the two layers: the `am-core11` base (Tiny11 Core 25H2 + the e2e tooling) and the build layer over it (the VS build subset); dockur boots the two-level qcow2 chain | both install; sizes and idle RAM recorded | the same layers on the full Windows 11 25H2 golden |
| P0.3 | a build in the build VM: cold and incremental, Debug and Release, 2–3 compilers, the guest page file; the data disk on each candidate storage | times within ~1.5× of native (to be agreed); no `C3859`; peaks recorded | the host native lane for the inner loop |
| P0.4 | the VM's layout copied into a test slot on the host and registered (dev identity) | the app starts from it | build on the host |
| P0.5 | every e2e suite on the `am-core11` base and on the full 25H2 golden, side by side | the same results on both, each difference explained | the full golden for each suite that differs |
| P0.6 | memory behavior: a paused build VM under host pressure; WSL's memory and swap settings | commit and working sets measured; no failure | a lower WSL cap, fewer VMs |

**The phases after P0:**

| Phase | Delivers | Acceptance |
|---|---|---|
| **P1** | the local node + record store; the coordinator; the VM manager's extensions; the build VM's life cycle; the host and in-guest agents; jobs `compile-check`, `lib-check`, `m5`, `build`, `zip`; `amb`; a read-only page | a Debug build of HEAD in the build VM while free commit stays above the reserve; two identical requests → one job; a build asked under pressure waits and starts by itself; a paused build VM resumes and finishes; nothing written to K:; an idle hour costs the tick only |
| **P2** | `stage`, `swap`, `deploy dev/prod`, the post-swap watch, swap records; approvals (Telegram, page) | a dev deploy from one command and one approval; an injected failure after the stop rolls back |
| **P3** | `e2e` on the image P0.5 chose for each suite, `install-lane` on the full golden, the weekly full run; the e2e slot and teardown in every outcome | a full suite from `amb e2e`; no VM left after a success, a failure or a cancel |
| **P4** | VM 100 as the second build machine (D4) | a Release build in VM 100 while homepc's commit stays flat; TraderLab never delayed |
| **P5** | the GitHub lane and releases | a prerelease cut with two approvals |
| **P6** (optional) | **fastrun runs the VMs itself**: a fastrund feature giving slots `/dev/kvm`, a capability set and persistent mounts. A dockur VM then becomes a release (or an app whose activator boots it); fastrun's cgroups are the allocation, and `--warm frozen` keeps a booted Windows VM stopped, thawing in milliseconds | a build VM started, frozen and thawed by fastrun, its memory capped by fastrun's cgroup |

## 11. Decisions for the owner

| # | Decision | Recommendation |
|---|---|---|
| D1 | The local node's record store: (a) a file store in fastrund, (b) a stand-in API in `buildserver/`, (c) ad-hoc only. D1b: the records in a PostgreSQL container on this PC, or the cluster's idle one | (a), with (b) if fastrund should stay unchanged; PostgreSQL on this PC |
| D2 | The image: Tiny11 Core 25H2 (`core11`) as the base of builds and e2e, the build tools only in a layer that build VMs boot; which e2e suites move to it (P0.5) | yes; the install lane and a weekly full run stay on the full 25H2 golden, which is also the fallback for everything |
| D3 | The VM manager: extend claude-desktops' Windows host, or a separate one | extend it: one budget for every VM on the PC |
| D4 | The dock side: VM 100 as the second build machine (Release builds; TraderLab first; ≤ ~40 GB on dock's thin pool), and/or `am-core11` pods on k3s (`/dev/kvm` + RAM) | VM 100 in P4; k3s pods after a RAM upgrade |
| D5 | Memory: `.wslconfig` (`memory=`, e.g. 44 GB; `swap=` with `swapFile=` on Q:; `autoMemoryReclaim=gradual`); the guest page file size (32 GB); the commit reserve (16 GB); pausing as the first response | as written; measured in P0.6 before it is set |
| D6 | Disk: where the build VM's data disk lives (a VHDX on Q: attached with `wsl --mount --vhd`, recommended; or Docker's VHDX moved to Q:); compacting Docker's 159 GB VHDX; retiring the stock build trees (52 GB) | measure the VHDX's real use first; the `wsl --mount` disk on Q:; retire the stock trees after P1 |
| D7 | The standing grants | auto for checks and tests; ask for builds, stages and e2e; ask at every step for swaps and releases |
| D8 | Telegram: a new bot for approvals and results | yes |
| D9 | Docker Desktop: start at sign-in, or on demand by the host agent | on demand, stopped after 30 min with no VM |
| D10 | The host native lane: keep it as the fallback | yes |
| D11 | The GitHub lane for pushed commits | yes |
| D12 | The stack: .NET 10 (coordinator, agents, CLI), Python for the tick | yes |

## 12. Risks and open questions

| Risk or question | How it is settled or contained |
|---|---|
| Tiny11 Core lacks something VS Build Tools, the build or the app needs, and cannot be serviced to add it | P0.2 / P0.5; the full 25H2 golden is the same plan with a bigger image |
| A test on Tiny11 Core passes where a user's Windows 11 fails, or the reverse (no Edge, WebView2 or WinRE; Windows Update and Defender off) | P0.5's side-by-side run; the install lane and a weekly full run stay on the full 25H2 golden; no build tools in the image that e2e VMs boot (§4.2) |
| Tiny11 Core gets no security updates | the VMs are LAN-only, behind the fleet's gateway; an e2e VM lives for one run; a build VM runs only this repository's code; the image is rebuilt when dockur ships a newer Tiny11 Core |
| VM 100's data disk fills dock's oversubscribed thin pool | one Release worktree, ≤ ~40 GB of real data (§5); `dock-pool-watch` alerts from 80 % |
| Builds under nested virtualization (KVM inside WSL2) are too slow | P0.3; the host native lane remains, with the first design's Job Object budgets |
| The data disk's I/O over Docker Desktop's file sharing | never put it on a bind mount; a `wsl --mount` ext4 disk or a volume (P0.3 measures both) |
| A paused VM's guest clock after a long pause | the agent resyncs the guest's time on resume (P0.6 checks) |
| fastrund's record store (a) is a fastrun change | tested on `fastrun-next` first; (b) needs none |
| WSL's VM gives memory back slowly | `autoMemoryReclaim`; stopping idle VMs; P0.6 measures |
| Disk on C: and Q: | §4.9; nothing grows without a budget |
| VM 100 is the owner's machine | builds only, below normal priority, TraderLab first, a CPU cap; the owner can switch it off in the policy |
| dock's heat | the CPU cap on VM 100; no k3s VMs before a RAM upgrade |

## 13. Evidence (2026-10-24)

- **fastrun, local** (`K:\source\fastrun`):
  - `src/Fastrun.Daemon/Kube.cs`: Secrets and one Lease only (Secrets: list by label selector, watch with
    bookmarks and a relist after 410, get, create, replace with `resourceVersion`, delete; the Lease: get, create,
    replace); with no ServiceAccount "releases are off and their methods answer UNAVAILABLE"; `FASTRUN_KUBE_URL`;
  - `src/Fastrun.Daemon/Settings.cs` (lines 575–588): release and ad-hoc limits ≤ 86,400 s and ≤ 64 GiB, process
    limits ≤ 65,536; `FASTRUN_TOKENS_FILE`;
  - `apps/yoinks/README.md` and `results/2026-10-05/lab-nodes.txt`: the 10-05 lab, "fastrund 0.3.0 … starting on lab
    (127.0.0.1)" in Docker on this PC, with Caddy (`apps/yoinks/lab/Caddyfile`);
  - `docs/progress.md`: deployed releases survive an API outage, schedules need the Lease;
  - the `fastrun` skill's `kinds.md`: slots run as uid 65532 with no capabilities and no devices.
- **homepc:**
  - `Win32_PageFileUsage`: `C:\pagefile.sys` 113,143 MB allocated, 17,189 MB used, 30,629 MB at peak, system-managed;
  - `Win32_OperatingSystem`: commit limit 174.2 GB, 40.4 GB free; RAM 63.7 GB, 14.8 GB free;
  - `docker info`: 32 CPUs, 33,503,916,032 bytes, Docker 27.3.1, kernel 5.15.153.1;
  - `wsl --version`: 2.3.24.0; no `.wslconfig`;
  - `docker_data.vhdx` 159 GB on C:.
- **The fleet's Windows host** (`K:\source\claude-desktops`): `docs/windows.md` (dockur 6.05, the golden on Q:,
  overlays, timings, "not on the cluster: no `/dev/kvm`"); `fleet/windows_host.py` (`RAM_SIZE`, `CPU_CORES`, the
  overlay, the persist share).
- **Tiny11:** dockur/windows's README lists `core11` (Tiny11 Core, 3.0 GB); its change #1513 moved Tiny11 to 25H2.
  tiny11builder's README and `docs/usage.md` (its `winui-preview` branch, read 2026-10-24): Tiny11 removes
  "Edge/WebView2 and OneDrive binaries"; Core "removes WinRE and disables Windows Update and Defender. These changes
  cannot be reversed. Use only for test VMs"; "Core cannot be updated or serviced".
- **dock**, read-only over SSH:
  - `qm config 100`: `cores: 16`, `memory: 16384`, `args: -cpu 'host,-hypervisor,kvm=off,+invtsc'`, seven `hostpci`
    entries, one 96 GB disk (`scsi0`);
  - `qm status 100 --verbose`: the guest uses 5,782,581,248 bytes and has 11,354,873,856 free; its guest agent did
    not answer `get-fsinfo`;
  - `/sys/module/kvm_amd/parameters/nested` = 1;
  - `ls /dev/kvm` in CT 113 / 114: absent, and their configs have no `kvm` entry;
  - earlier the same day: 6.2 GiB available, the thin pool at 71.44 %.
- **The homelab doc** (`K:\source\proxmox\CLAUDE.md`): VM 100 is TraderLab's dedicated compute machine since
  2026-10-02, its table says 8c, its clock runs ~4.6 % slow (found 2026-10-01, open); the thin pool is oversubscribed
  and `dock-pool-watch` alerts from 80 %; CTs 112–114 have `cgroup2.devices.allow=a`; dock's 20-hour outage of
  2026-10-04 → 05 ended at a BIOS stop on "CPU Over Temperature".
- **e2e:** `e2e/README.md` and `E2E_TESTING.md` §8.5: ~37 min for the 49 tests of 2026-10-23, plus provisioning; an
  install-lane reset 35–70 s on a quiet PC, 435–570 s beside three other desktops; that lane's provisioning 78 s after
  its fixes of 2026-10-24, ~6.4 min before; `e2e/guest/provision.ps1`: what each lane installs (`claude.exe` and Git
  only in the Standard lane).
- **The repository's `.vsconfig`:** 44 components.
- **The swap fixes:** commit 278e644f7 (another session, the same evening).
