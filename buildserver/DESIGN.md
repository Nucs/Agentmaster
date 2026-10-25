# Agentmaster build server — design and plan (fourth revision)

> **Status: DESIGN ONLY. Nothing here is built.** The measurements of 2026-10-25 ran in throwaway test machines and
> folders, which were removed afterwards (§16.8); their scripts are in [`measure/`](measure/README.md).
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
> **The owner's answers to the third design's decisions, 2026-10-25:** "E1. be fluid allowing to allocate slot based
> on availability x priority where the proxmox machine is higher priority. E2. dedicated folder E4. design the image to
> best fit for a slot based on tiny11 E5. measure varieties / update the design/plan and measure ahead all numbers and
> make decisions accordingly". E3 (the homelab changes for dock) was not answered and stays open (§14).
>
> **This revision replaces the third (8a840e9c5)**, which replaced the second (317c4d6eb) and the first (58a31fd7c); all
> three are in git history. Every number was measured on 2026-10-24 or 2026-10-25; §16 has the tables, §17 how.

## 0. In one page

**What it is.** A service that does what it is told, on demand:
- **build** a commit as Debug or Release (dev or release identity) and get a **build id** with its artifacts;
- **test** a build (the engine harness and the other test kinds);
- **e2e** a build's artifacts with the e2e suites;
- later, **install** a build on this PC's dev or production instance, and **publish** a Release build as a GitHub
  release. A build is never a publish.

Every request says the least and the most it needs. The service starts a slot where the request fits best and sizes
it to what is free between the two; otherwise the request waits, up to its timeout. There are no approvals.

**How it runs.**
- **Two sites, both fastrun:** dock (the k3s cluster's fastrun, priority 2) and homepc (this PC, priority 1). A request
  goes to the site with the highest *priority × availability* (§3, E1).
- **homepc's site is a WSL distro of its own, `buildserver`, whose disk is a file in the dedicated folder
  `Q:\buildserver`** (§5.2, E2). fastrund runs inside it, and so do the VMs. Docker Desktop plays no part: the site
  works while Docker Desktop is off.
- **Windows work runs in VMs from three images, each made for its slot kind, all from Tiny11** (§4, E4): `am-e2e`
  and `am-build` from Tiny11 Core, `am-install` from regular Tiny11.
- **A slot's memory follows its need:** a VM boots at its ceiling and its balloon keeps it near what the guest uses.
  Measured: 6 GB taken back in under 30 s and returned to Windows at once; 5 GB given back in 18 s.
- **Lazy:** nothing runs while nothing is asked. The distro starts in under a second; a VM from its image is ready
  in 30–44 s; a build VM stays up 10 minutes after its last request ("the ready machine").

**What the measurements decided (E5):**
- **A VM's disk does ~2,000 random I/Os per second, whatever the attachment** (nested virtualization: KVM inside
  WSL 2 inside Hyper-V, no APICv), **and compiling costs ~1.55× the host's time** at the same parallelism (the engine
  harness: 107.5 s in the VM, 69.2–69.9 s on the host). Builds stay in VMs, as the owner chose (W4): the factor is the
  price of a clean, isolated build machine; P0.4 measures a full build.
- **A VM holds all of its RAM** (the guest zeroes its free memory within 1–2 minutes of boot) until the balloon takes
  it back, so every VM slot has a balloon and the allocator counts balloon targets, not ceilings.
- **homepc's real limits are Windows' free memory and WSL's 31.2 GB VM, not commit.** During the tests Windows had
  2.5–8.9 GB available while commit had 33–44 GB free, and the WSL VM was full with other sessions' VMs in it.
- **No data through Docker binds or pipes:** a bind of a Windows folder is 13–43× slower than the distro's own disk, a
  guest never sees a file the host writes there, and a 12 GB image piped through `docker | wsl.exe` moved at 25 MB/s.
  Disks live in the distro; images are made where they are used; the guest agent talks HTTP.
- **dock had 6.0–7.1 GB available all week at 2–3 % CPU:** once E3's changes exist it takes e2e slots first; builds
  never fit it.
- **Tiny11 Core carries both jobs:** Agentmaster's portable zip runs on it, and VS 2022 Build Tools installs (exit 0,
  28 minutes, 18.5 GB). ⚠ Its servicing worker then ran for over 30 minutes and slowed the compiles by up to 47 %, so
  an image is sealed only with servicing idle.
- **Disk stays the PC's tightest resource:** the images and the e2e slots fit in `Q:\buildserver`; a build VM (the
  18.5 GB build layer plus 40–60 GB of its own) does not, until space is freed (W3).

## 1. The owner's answers, and what each changes

| # | The question | The owner's answer | In this design |
|---|---|---|---|
| D1 | the record store | a file store on disk, small JSONs, read and written atomically; the whole infra lazy | §6: JSON files on the site's own disk (the distro's ext4 in `Q:\buildserver`), temp file + fsync + rename |
| D2 | Tiny11 Core 25H2 as the base, build tools in a layer | yes | §4: Core is the base of `am-e2e` and `am-build`; the build tools are the build image's own |
| D3 | the VM manager | fastrun on both sites | fastrun runs the VMs as slots (§5, §9); claude-desktops stays separate |
| D4 | the dock side | no VM 100; Tiny11 when needed | Tiny11 slots on dock's fastrun once E3 is done |
| D5 | memory settings | "Why WSL?" | WSL is the PC's only way to run Linux: the site is a distro of its own (§5.2), and WSL's VM is a measured limit (§7) |
| D6 | disk | check | measured (§8, §16) |
| D7 | approvals | none; allocation by estimate; pending with a timeout; build → build id → test / e2e | §2, §3 |
| D8 | a Telegram bot | don't touch | none |
| D9 | Docker Desktop on demand | defer | no longer needed: the site does not use Docker Desktop (§5.2) |
| D10 | the host native lane | defer | still deferred; W4 (§14) asks whether builds belong there |
| D11 | the GitHub lane | defer | deferred, with `publish` |
| D12 | the stack | .NET 10; Python for setup and the e2e engine | unchanged (§10) |
| E1 | the allocation | fluid, availability × priority, dock first | §3 |
| E2 | where VM disks live | a dedicated folder | `Q:\buildserver` holds the site's distro disk and the artifact store (§5.2, §8) |
| E3 | the homelab changes for dock's Windows slots | not answered | open (§14) |
| E4 | the images | each designed for its slot, from Tiny11 | §4 |
| E5 | reserves and budgets | measure varieties | §16; the reserves of §3.2 come from it |

## 2. Operations

The service is instructed through one API (HTTP + JSON). The CLI `amb` and scripts use it. Every request returns its
id at once; `--wait` streams its events until it ends.

| Operation | Takes | Gives | Runs in |
|---|---|---|---|
| `build` | a commit (uncommitted work only as an explicit snapshot commit), `Debug` or `Release`, identity `dev` or `release`, purge `auto` / `force` / `none`, optional sizes | a **build id** and its artifacts: the package layout, the portable zip, the build log and binlog, a manifest (commit, configuration, identity, versions, each artifact's SHA-256) | a build VM (§5.1) |
| `test` | a build id and a kind: `m5`, `syntax` (the `/Zs` sweep), `installer`, more later | a result with its logs | the build VM that holds the build's tree |
| `e2e` | a build id and the suite's arguments | JUnit XML, the run's evidence, logs | fresh `am-e2e` VMs; the install lane in `am-install` |
| `install` (later) | a build id and an instance | the swap's record | the host agent, through `Swap-AgentmasterLayout.ps1` |
| `publish` (deferred, D11) | a Release build id and a version | a GitHub release | — |

- **A build id** names an immutable result, `b<yyyymmdd>-<n>`. A request for a build that exists (the same commit,
  configuration, identity and purge) returns its id, or joins it while it runs.
- **Pending and timeouts:** a request waits for a slot up to its wait timeout (build 60 min, test 30 min, e2e
  120 min), then ends `no-capacity`, naming what was short. A running request has a run timeout (build 3 h, test 1 h,
  e2e 2 h).
- **Cancel** ends a request in any state and releases its slot.
- **Who may instruct it:** anyone with a token. Claude sessions keep CLAUDE.md's rule: they ask the user before they
  ask the service for a build or an install.

## 3. Allocation: fluid, by availability × priority (E1)

### 3.1 What a request asks for

A request names a **minimum** and a **preferred** amount of each resource: guest memory, vCPUs, disk growth; plus its
run timeout and locks (a build VM's data disk, an install's instance). The first values per kind:

| Kind | Memory min / preferred | vCPUs min / preferred | Disk | Why these |
|---|---|---|---|---|
| e2e VM (`am-e2e`) | 3 / 8 GB | 2 / 6 | an overlay, 1–3 GB, deleted | Tiny11 Core uses 1.4–1.9 GB at rest; Agentmaster with a tab adds ~150 MB; the e2e runner's default is 8 GB / 6 CPUs |
| install-lane VM (`am-install`) | 3 / 6 GB | 2 / 4 | an overlay, deleted | the same guest plus Defender (~180 MB, measured by claude-desktops) |
| build VM, Debug (`am-build`) | 8 / 16 GB | 4 / 8 | a kept data disk, ~35 GB the first time | native: a Debug build at 3 compilers took 8.7 GB of commit; the TerminalApp PCH is 1.8–2.3 GB per compiler |
| build VM, Release | 8 / 16 GB | 4 / 8 | ~40 GB the first time | native: 7.8 GB of commit at 3 compilers |
| `m5` / `syntax` test | runs in the build VM it tests | | | |
| Linux job | 0.25 / 1 GB | 1 / 2 | — | |

After ten runs of a kind, its values come from them: minimum = the median of the guest's committed memory at rest +
1 GB; preferred = the median peak + 1 GB. A request may name its own.

### 3.2 What a site has free

Each value is *free minus a reserve*; the reserves were set from §16's measurements.

**homepc** (the host agent reports Windows; fastrund reports the distro):
- **memory** = min(Windows available − 6 GB, the WSL VM's MemAvailable − 2 GB, Windows' free commit − 16 GB)
  + what our own running VMs can give back now (each one's balloon target minus its minimum, when its guest is not
  using it).
  - The 6 GB keeps the owner's own work fast: Windows had 2.5 GB available while three VMs ran during the tests.
  - The WSL VM (31.2 GB) also holds Docker Desktop and everything other sessions run there: 13.5 GB of other
    sessions' VMs at one point of the tests.
  - Commit is the owner's "utilize committed memory": it had 33–44 GB free during the tests, so it binds last. A
    slot marked *tolerant* (an idle build VM kept warm, a VM ballooned down to its minimum) may be admitted on commit
    alone, beyond Windows' available memory; its cold pages then live in the page file. A running build or e2e is
    never tolerant.
- **vCPUs** = 24 − the running slots' vCPUs (24 of the 32 threads; a VM computes at 70–97 % of the host, §16.5).
- **disk** = Q: free − 15 GB (other sessions' builds and e2e runs write 4–20 GB at a time on Q:).
- **holds:** a full-screen game holds new slots (`QUNS_RUNNING_D3D_FULL_SCREEN`).

**dock** (fastrund's capacity report, §9 F7):
- **memory** = MemAvailable − 2 GB (6.0–7.1 GB available all week, median 6.3 GB);
- **vCPUs** = 12 of 16 − the running slots' vCPUs (2–3 % busy all week);
- **disk** = the node's free space − 5 GB;
- Windows slots only after E3.

### 3.3 The choice

For each resource *r* of a request at a site with *A_r* free:

```text
fit_r        = 0                        if A_r < min_r
             = min(1, A_r / preferred_r) otherwise
availability = the smallest fit_r
score        = priority(site) × availability        dock 2.0, homepc 1.0 (settings)
```

- The request goes to the highest score; a tie goes to the higher priority. When every score is 0 it waits, and is
  scored again whenever a slot ends, a balloon shrinks, Windows frees memory, and every 15 s.
- **The slot gets** memory = min(preferred, *A_mem*) and vCPUs = min(preferred, *A_cpu*): at least the minimum, at
  most the preferred.
- **What the product means:** dock wins while it can give at least half of what homepc can give in full
  (0.5 × 2 = 1 × 1). So e2e VMs go to dock first, and builds, whose minimum is above dock's free memory, always land
  on homepc.

### 3.4 While a VM runs: the balloon

- Every VM boots with RAM_SIZE = its preferred memory and a virtio balloon (`-device virtio-balloon-pci`, dockur's
  `ARGUMENTS`; dockur's own balloon controller stays off).
- Every 10 s the site sets the balloon's target to the guest's committed memory plus a headroom, between the slot's
  minimum and its current grant. The headroom covers what the guest can add before the next step: 1 GB for an e2e
  VM, 3 GB for a build VM (one more compiler loading the TerminalApp PCH adds 1.8–2.3 GB); a guest that outgrows its
  target meanwhile pages to its own page file until the balloon gives memory back. The grant grows toward the
  preferred size when the site has room, and shrinks toward the minimum under pressure.
- **Measured on Tiny11 Core:** 8 → 3 GB in ~45 s and 8 → 2 GB in under 30 s; Windows' available memory rose by
  nearly as much at once (3.27 → 8.90 GB; vmmemWSL's working set −4.95 GB, and its private bytes fell from 31.19 to
  23.48 GB in the minutes after). Back up 3 → 8 GB in 18 s. `free-page-reporting` does nothing for a Windows guest.
- Who sets it: fastrund, through the VM's QEMU monitor (§9 F8).

### 3.5 Order, pressure

- **Order:** first come, first served; a smaller request may pass a larger one that waits, until that one has waited
  10 minutes.
- **Pressure while slots run** (the host agent watches Windows): first shrink balloons toward their minimums, oldest
  idle VM first; then hold new slots; then shut down an idle build VM; only at the floor (Windows available under
  2 GB twice in a row) stop the newest slot.

### 3.6 Worked example, with numbers measured during the tests on 2026-10-25

homepc, after our test VM's balloon was set to 4 GB: Windows available 8.6 GB, the WSL VM's MemAvailable 11.1 GB,
free commit 32.8 GB; that VM's minimum taken as 3 GB → memory = min(2.6, 9.1, 16.8) + 1 = **3.6 GB**. dock (the
week's median): 6.3 − 2 = **4.3 GB**. vCPUs had room on both.

| Request | homepc | dock (after E3) | Goes to |
|---|---|---|---|
| e2e VM (3 / 8 GB) | fit 0.45 → score 0.45 | fit 0.54 → score 1.08 | dock, at 4.3 GB |
| e2e VM, before E3 | 0.45 | — | homepc, at 3.6 GB |
| Debug build (8 / 16 GB) | 3.6 < min → 0 | 4.3 < min → 0 | waits; at its 60-minute timeout `no-capacity: memory` |

## 4. Images: one per slot kind, all from Tiny11 (E4)

### 4.1 The two Tiny11 builds

- **Tiny11 Core** (dockur `VERSION=core11`, a 3.0 GB download): removes the bundled apps, OneDrive, WinRE and most of
  the component store, and disables Windows Update and Defender. It cannot be serviced: no update, feature or
  language can be added. dockur's ISO is the November 2025 build (Windows 11 Pro 26200.7171).
- **Tiny11** (dockur `VERSION=tiny11`, 5.3 GB): removes the bundled apps, Edge and OneDrive; keeps Update, Defender,
  WinRE and the Store, and stays serviceable.
- **Measured on Tiny11 Core (§16.2):** installed in 12½ minutes (16½ with the download); 9.2 GB on disk; at rest
  131 processes, 93 services, 1.4–1.9 GB of memory used; ready 30–44 s after the VM starts. Present: MSI and AppX
  installs, BITS, Task Scheduler, winget, WebView2 122. Absent or off: Windows Update, Defender, the VC++ runtime,
  Edge. **Agentmaster v0.8.0's portable zip runs on it** (window, side panel and a PowerShell tab drawn; 103 MB).
  **VS 2022 Build Tools installs on it** (C++ and UWP with their recommended components, both SDKs, vcpkg, the .NET
  SDK): exit 0, no reboot, 28 minutes with its 4.8 GB download, 18.5 GB. ⚠ Afterwards TiWorker (Windows' servicing
  worker) kept one vCPU busy for over 30 minutes, until the TrustedInstaller service was stopped.
- **A fresher base is possible:** Tiny11 Builder's own engine (ntdevlabs/tiny11builder) builds a Tiny11 ISO from
  Microsoft's current Windows 11 ISO, with services disabled offline. claude-desktops was building exactly that on
  2026-10-25 (`images/windows/build_tiny11.py`, its uncommitted work): the regular flavor with Update, Defender and
  telemetry disabled offline. The build server's image script can use the same engine (pinned by commit) when
  dockur's Core ISO is too old; it is not needed to start.

### 4.2 The images

| Image | Base | What it adds | Slot | Size (measured / expected) |
|---|---|---|---|---|
| `am-e2e` | a layer over the Tiny11 Core base (which holds the guest agent) | Python 3.13 with the e2e runner's venv and its wheel cache, PowerShell 7, PortableGit (what `e2e/guest/provision.ps1` installs after every reset today); crash dumps on (WER LocalDumps); the trims of §4.4 | e2e VMs | the base 9.2 GB (measured) + ~2 GB of layer |
| `am-build` | a layer over the same base | VS 2022 Build Tools (C++, UWP, the 22621 and 26100 SDKs, vcpkg, the .NET SDK), Git, .NET 10; the trims of §4.4 | build VMs | the base + 18.5 GB of layer (measured with the workloads' recommended components; the build's own subset is smaller) |
| `am-install` | Tiny11 (serviceable) | the guest agent and nothing else: Defender, SmartScreen and Windows Update stay as a user's Windows has them | the install lane | not measured yet (P0.3); ~13 GB expected |

- **Why three and not one:** a test VM with a compiler, the Windows SDK or a VC++ runtime installed can pass a test
  that a user's PC fails (a portable zip missing a runtime DLL), so e2e VMs never boot the build image; and the
  install lane tests what Windows does around an installer (SmartScreen's "Run anyway", the mark of the web, UAC,
  Apps & Features), which Tiny11 Core has switched off.
- **Not in any image:** `claude.exe` (the Standard lane copies it per run from the store's cache, as today), Git's
  credentials, any token.
- **e2e on Tiny11 Core is not e2e on Windows 11:** a suite moves to `am-e2e` only after P0.5 shows the same results
  there and on a full Windows 11; a weekly run of every suite stays on `am-install` (the closest to a user's Windows).

### 4.3 How an image is made

- **In place, on the site that uses it**, never copied through a pipe (a 12 GB copy through `docker … | wsl.exe …`
  ran at 25 MB/s). The image script (`buildserver/image/`, Python, D12) runs dockur's unattended install in the
  site's distro with an OEM folder: `install.bat` → `setup.ps1` installs what the image adds and registers the guest
  agent; the VM then shuts down.
- **The result is a read-only folder:** `data.qcow2`, the UEFI files (`windows.rom`, `windows.vars`), dockur's state
  files and the empty marker `windows.boot`. ⚠ Without `windows.boot` (and without the install ISO) dockur takes the
  folder for an unfinished install, deletes its disks and installs again (measured; `cleanupStorage` in dockur's
  `install.sh`).
- **Layers (D2):** Tiny11 Core is installed once as the base (with the guest agent); `am-e2e` and `am-build` are each a
  qcow2 layer over it (`qemu-img create -b base/data.qcow2`), made by booting the base through the new layer and
  installing what that image adds. Both images share the base's 9.2 GB.
- **A slot boots a copy-on-write overlay over its image:** `qemu-img create -b <image>/data.qcow2` takes under a
  second; the slot gets its own `windows.vars`. An e2e overlay is deleted with its slot; a build VM keeps its overlay
  and its data disk until its image changes.
- **dock makes its own copies** with the same script (12½ minutes for the base, measured on homepc), rather than
  receiving 10+ GB over the LAN.

### 4.4 Trims (for `am-e2e` and `am-build`)

Tiny11 Core still runs services a test VM does not need. To disable in the image (P0.3 measures the saving): Windows
Search (`WSearch` and the SearchHost UI), SysMain, DiagTrack, the print spooler, Delivery Optimization (`DoSvc`),
geolocation (`lfsvc`), the connected-devices platform (`CDPSvc`, `CDPUserSvc`), Remote Desktop (`TermService`; dockur's
console is VNC), Windows' widgets. **Kept:** text input (`TextInputManagementService`, the keyboard), push notifications
(`WpnService`, `WpnUserService`: Agentmaster's toasts), themes, UI Automation, the balloon service (`BalloonService`).
Also in the image: the time zone set to UTC (dockur's guest came up in Pacific time on a Jerusalem clock), a fixed
page file, and power plan High performance.

## 5. Sites and slots

### 5.1 Slot kinds

| Kind | Boots | Disks | Life |
|---|---|---|---|
| `win-build` | `am-build` | the image (read-only) + its own overlay and raw data disk, both kept | started by a request; stays up 10 min after its last step; then Windows shuts down (8–14 s) and the run ends |
| `win-e2e` | `am-e2e` (or `am-install` for the install lane) | the image (read-only) + a throwaway overlay | one run; deleted in every outcome; an install-lane "reset" is a fresh slot |
| `linux` | a container | none, or a volume | the coordinator; small jobs |

- **A VM slot is a run of one fastrun release, `am-win` (`dockurr/windows:6.05`)**, with `/dev/kvm`, dockur's
  user-mode network (no `NET_ADMIN`, no `/dev/net/tun`), its disks and exchange folder as volumes (§9 F2, F3), its size
  in `RAM_SIZE` / `CPU_CORES`, and its balloon (§3.4).
- **A build VM's data disk is a sparse raw file** (D: in the guest) holding one worktree per configuration, the NuGet
  and vcpkg caches and the guest's page file. With `discard=unmap`, what the guest deletes goes back to the distro
  (measured: a 64 GB data disk held 92 MB after its test files were deleted).

### 5.2 homepc: a WSL distro of its own (E2)

**Measured options for where homepc's VM disks live (§16.6):**

| Option | Disk speed (fio, O_DIRECT: random 4K write / read IOPS, sequential write / read) | Needs | Verdict |
|---|---|---|---|
| **A. A distro of its own, its disk `Q:\buildserver\wsl\ext4.vhdx`; fastrund and the VMs run inside it** | 64,092 / 87,624 IOPS; 4.75 / 7.20 GB/s | WSL (present); fastrund for it (§9 F9) | **chosen** |
| B. fastrund in Docker Desktop, its data on that distro's disk through Docker Desktop's WSL integration | 52,439 / 99,667 IOPS; 1.55 / 4.32 GB/s (measured through Ubuntu, the integrated distro) | Docker Desktop running, integration turned on for the distro | works; ties the site to Docker Desktop, which does not start with Windows |
| C. A Docker volume (inside `docker_data.vhdx` on C:) | 46,705 / 71,695 IOPS; 2.09 / 4.28 GB/s | — | no: not the dedicated folder; C: holds the page file; that VHDX is not sparse and never shrinks |
| D. A bind of a Windows folder (`Q:\…` or `C:\…`) | 2,898–4,858 / 4,336–5,126 IOPS; 150–176 MB/s | — | no: 13–43× slower than A, and a guest never sees a file the host writes there |
| E. Docker Desktop's whole disk moved to Q: | — | 158 GB | no: Q: had 31–70 GB free during the tests |

- **The distro:** Alpine 3.22 from its minirootfs, imported with `wsl --import buildserver Q:\buildserver\wsl <tar>`.
  Measured with the test distro: a cold start 0.52–0.79 s, a call into the running distro 0.33 s; `podman` and `crun`
  installed in 7 s; the dockur image (578 MB) pulled in 12 s; a VM booted from an overlay ready in 44 s.
- **Its disk becomes sparse only at a WSL restart.** WSL 2.3.24 offers `wsl --manage <distro> --set-sparse true`, but
  refuses it while WSL's VM runs ("the vhdx is currently in use"), even for a distro just imported and never started,
  and `fsutil sparse setflag` fails the same way (measured). WSL keeps every distro's disk attached while its VM runs,
  and Docker Desktop keeps it running. So the switch, or `.wslconfig`'s `[experimental] sparseVhd=true` for disks
  created afterwards, waits for one WSL restart, W1's. Until then the distro reuses the space it frees but does not
  give it back to Q:.
- **Networking:** every WSL distro shares one network namespace. Each slot gets its own (the proofs used rootful
  podman's `slirp4netns`; rootful podman has no `pasta`), so the site adds no bridge and no firewall rule to the
  namespace Docker Desktop also uses.
- **cgroups:** WSL 2.3.24 mounts every controller on cgroup v1; its cgroup v2 hierarchy has none. fastrund then runs
  with slot cgroups off ("spawner: no cgroup v2 here; slots stay in fastrund's cgroup", `Spawner.cs`): no per-run
  limits and no freeze until W1. VM slots stay bounded by QEMU's `-m` and `-smp` and by the balloon.
- **Who wakes it:** the host agent (§10), a logon task that is always up, starts the distro and fastrund when a
  request arrives. After 10 minutes with no slot and no request it stops fastrund, and WSL then stops the distro by
  itself (a distro with no process left is shut down).
- **Its ports:** fastrund's API (2200; gRPC 2201, raw lane 2202: fastrun's free range 2200-2499) and activators listen
  in the distro. **Windows reaches them at the distro's own address**, which changes at every start of WSL's VM, so the
  host agent asks the distro for it (`ip -4 addr show eth0`) and asks again after a refused connection. WSL's localhost
  forwarding is no way in: it passes only IPv4 listeners, and fastrund listens on the IPv6 wildcard (`:::2200`;
  measured, §16.9). The LAN (Caddy, for `https://build.local`) reaches the site through the host agent, which forwards
  one port opened to Caddy (`192.168.50.5`) only.
- **Its registry:** fastrund pulls runtime images over plain HTTP through Traefik (`FASTRUN_REGISTRY_ENDPOINTS=
  docker-registry.elibelash.com=http://docker-registry.local`), the way production pulls in-cluster: the static
  fastrund image cannot complete a TLS handshake with `https://docker-registry.elibelash.com` from here (measured,
  §16.9), while curl in the same distro verifies that certificate.
- **The coordinator lives here** (an app on this node, as in the third revision): the source (K:) and the artifact
  store (Q:) are on this PC, so nothing can be built while it is off anyway.

### 5.3 dock (k3s)

- **The cluster's fastrun 1.2.1, priority 2.** It runs Linux slots today.
- **Windows slots there need E3's four changes:** `/dev/kvm` in CT 113/114 (one `lxc.mount.entry` each and a restart
  of that node); fastrund's pod memory limit (1 GiB for everything it starts) raised to a VM's size; ~20 GB of disk
  for `am-e2e` (the base and its layer, ~11 GB) and its overlays (the nodes have 2.3–12 GB free; the thin pool is at
  71.45 %, its alert at 80 %); and RAM: 6.0–7.1 GB available all week, so one e2e VM at 3–4 GB.
- **dock never takes a build:** a build's minimum is above its free memory.

### 5.4 Everything else in the WSL VM

claude-desktops' Windows desktops and other sessions' dockur VMs run in Docker Desktop's part of the same WSL VM
(8.2 + 5.3 GB of them during the tests). The allocator sees them only as less free memory (the WSL VM's MemAvailable
and Windows' available memory) and never starts or stops them.

## 6. Records: small JSON files, written atomically (D1)

- **Where:** `/srv/buildserver/records` on the distro's ext4 (in `Q:\buildserver\wsl\ext4.vhdx`), mounted into the
  coordinator's slot (§9 F2). fastrund's own records (§9 F1) live on the same disk.
- **What:** `requests/<id>.json` (kind, inputs, sizes, state, times, slot, a result summary; at most 64 KB),
  `requests/<id>.events.jsonl` (one line per event, appended and flushed per line), `builds/<build id>.json` (the
  manifest), `slots/<slot id>.json`, `vms/<name>.json` (each kept build VM: disks, image, last use),
  `estimates.json`, `config.json` (reserves, priorities, timeouts). Logs, binlogs and evidence go to the store, never
  into JSON.
- **The write:** serialize → write `<name>.json.tmp-<pid>-<n>` in the same folder → fsync the file → rename it over
  `<name>.json` → fsync the folder. A reader reads whole files and never a `.tmp-` file, so it sees the old version or
  the new one. Start-up deletes `.tmp-` files a crash left.
- **One writer:** the coordinator runs one instance (`--max-instances 1`), with a lock per record; each record carries
  a `version`, and a write replaces only the version it read (compare-and-swap).
- **Lazy:** the coordinator keeps nothing the files don't hold; it can stop between any two writes.

## 7. Memory: what a slot really costs

**The chain, measured on 2026-10-25:**

```text
Windows       63.7 GB of RAM; 2.5–8.9 GB available during the tests; commit 133–147 GB used of 177–190 GB
 └ vmmemWSL     the WSL 2 VM: working set 18.1–25.5 GB, private bytes 23.5–31.2 GB; it follows what Linux uses
    └ Linux      31.2 GB + 8 GB of swap (WSL's defaults), ONE VM for every distro: docker-desktop (with other
                 sessions' VMs), Ubuntu, and the site's distro
       └ fastrund slot
          └ QEMU   resident = RAM_SIZE within 1–2 min of boot (the guest zeroes its free memory), until the balloon
                   takes some back; + ~0.4 GB of QEMU
             └ Windows guest  Tiny11 Core at rest: 1.4–1.9 GB used; its own page file takes MSVC's peaks
```

- **A VM costs its whole RAM_SIZE unless ballooned:** 4.08 GB resident for a 4 GB guest, 8.07–8.46 GB for 8 GB.
  Hence §3.4.
- **Memory the balloon takes reaches Windows at once:** −6 GB of balloon gave Windows +5.6 GB of available memory
  within 30 s; vmmemWSL's private bytes (its commit) fell with it.
- **The page cache:** Linux keeps file cache until it needs the memory, and WSL counts it as used. QEMU opens its disks
  with `cache=none`, so VM I/O adds none; big copies do (the 12 GB image copy pushed the WSL VM to its cap). The site
  drops clean cache after large transfers.
- **Not measured: KSM** (W2). It is one switch for the whole WSL VM (off today), and another session was measuring
  memory in the same VM. Windows guests keep their free memory zeroed, so KSM may merge much of it.
- **Raising WSL's 31.2 GB** (`.wslconfig` `memory=`) needs a restart of the WSL VM; it is not needed while the
  balloons keep VMs near their use.

## 8. Disk: the dedicated folder (E2)

### 8.1 Layout

```text
Q:\buildserver\
  wsl\ext4.vhdx            the site's distro disk (sparse from W1's restart on); inside it, /srv/buildserver:
                             images/<name>/     each image: data.qcow2 + windows.* + windows.boot (read-only)
                             slots/<id>/        an e2e slot's overlay (deleted with the slot)
                             vms/<name>/        a kept build VM: overlay + raw data disk
                             exchange/<slot>/   the guest's share: source bundles in, artifacts out
                             records/           §6
                             fastrun/           fastrund's store, scratch and data folders
  store\                   artifacts as plain files (layouts, zips, logs, binlogs); 40 GB, oldest out first
  git\agentmaster.git      the bare mirror the host agent keeps (the repository's packed history is 198 MB)
```

### 8.2 Budget

| Item | Size | Measured / expected |
|---|---|---|
| the Tiny11 Core base, shared by `am-e2e` and `am-build` | 9.2 GB | measured |
| the `am-e2e` layer | ~2 GB | expected (Python, PowerShell 7, PortableGit, the wheel cache) |
| the `am-build` layer | 18.5 GB | measured: Build Tools with the workloads' recommended components (C: +14.6 GB, D: +3.9 GB) |
| `am-install` | ~13 GB | expected (Tiny11's ISO is 5.3 GB) |
| a build VM: overlay + data disk (two worktrees, the caches, the guest's page file) | ~40–60 GB | from the native trees: Debug ~25 GB, Release ~30 GB the first time |
| e2e overlays | 1–3 GB each | deleted with the slot |
| the store | 40 GB | a budget, oldest out first |

- **Today it does not all fit:** Q: had 31–70 GB free during the tests (another session's Tiny11 build was writing
  ~40 GB there too), with 15 GB kept for other work. The e2e images, overlays and the store fit; a build VM does not
  (W3).
- **Space goes back in two steps:** what a guest deletes is unmapped in its disk file (`discard=unmap`; measured: a
  64 GB data disk held 92 MB after its test files were deleted), and the distro's ext4 (mounted with `discard`) frees
  it. Q: gets it back only once the VHDX is sparse (§5.2); before that, the distro keeps it for its next files.

## 9. What fastrun must gain (the owner's project)

Read in `K:\source\fastrun`:
- an app takes no limits (`AppReleases.cs`); a run outlives its caller, but a call sets no per-run limits and
  `Runs.Stop` ends every run of a release on a node (`proto/fastrun/v1/fastrun.proto`);
- a slot gets no devices, and a writable root gets eleven capabilities (`Runtime.cs`);
- with no ServiceAccount, releases, apps and schedules answer `UNAVAILABLE` (`Kube.cs`);
- with no cgroup v2 controllers, slots run in fastrund's own cgroup (`Spawner.cs`, `SetUpCgroups`).

| # | Feature | Why | Done when |
|---|---|---|---|
| F1 | a file record store (`FASTRUN_RECORDS_DIR`) used when no ServiceAccount is mounted: the same calls as `Kube.cs`, each record a JSON file written as in §6, watches in-process | releases, apps and schedules on homepc (D1) | fastrun's conformance test passes on homepc, and a `kill -9` mid-write leaves every record readable |
| F2 | volumes that outlive runs, and mounts of node paths on an allow-list | records, VM disks, images, exchange folders | a volume outlives runs, revisions and fastrund restarts |
| F3 | VM slots: devices from an allow-list (`/dev/kvm`), root and a writable root, more capabilities from an allow-list if dockur needs them, and a network namespace of the slot's own with a user-mode uplink (slirp4netns or pasta) | Tiny11 VMs; each VM's passt binds its forwarded ports (3389) inside its slot, never in the namespace every WSL distro shares | two dockur VMs boot side by side in slots with user-mode networking |
| F4 | per-run limits that may lower the release's: memory, CPU, processes | a slot sized per request | a run with a lower limit is held to it (needs cgroup v2 on homepc: W1) |
| F5 | one run by its id: start returns the id; a watch streams its state; a stop of that run alone (SIGTERM, a grace period, then kill) | VM runs of hours; several VMs of one release; clean Windows shutdowns | the coordinator restarts mid-build, finds its run by id, and stops it alone |
| F6 (later) | freeze and thaw a run | pause under pressure | needs cgroup v2 on homepc |
| F7 | capacity in the node status: memory total and available, CPUs, the running slots' grants | admission (§3.2) | — |
| F8 | a VM slot's memory control: set and read its balloon through QEMU's monitor socket in the slot | fluid slots (§3.4) | a slot's balloon follows a target the node sets |
| F9 | a node in a WSL distro: fastrund without Kubernetes (F1) on WSL's kernel, its ports reachable from Windows | homepc (§5.2) | fastrund 1.2.x runs ad-hoc and release runs in the `buildserver` distro |

On dock, also fastrund's pod memory limit and the LXC changes of §5.3 (E3).

## 10. The agents, the CLI and the e2e engine

- **The guest agent** (`amb-guest`, .NET 10, in every image) **talks HTTP**: it dials the coordinator through the
  VM's user-mode network and long-polls for its steps. ⚠ Never poll a share for instructions: the guest's SMB client
  serves a folder listing from its lease cache, and dockur's Samba is never told of a file written behind its back
  (measured: a host-written job file was never listed). The share stays for bulk files only (source bundles in,
  artifacts out), opened by exact name. The agent's steps: fetch the source bundle into the data disk's clone; build
  with that commit's `tools\Build-Agentmaster.ps1`; run a test or the e2e engine's guest half; write artifacts; stream
  logs; report the guest's memory every 10 s (the balloon's input, §3.4).
- **The host agent** (`amb-host`, .NET 10, a logon task outside every Agentmaster instance): the front door
  (`localhost:2107`, and a LAN port for Caddy only); starts the distro and fastrund on demand; keeps the bare mirror;
  copies artifacts from the exchange folders into `Q:\buildserver\store` and checks their SHA-256; reports Windows'
  available memory, free commit, C: and Q: free space and a full-screen game every 5 s; later runs installs.
- **The CLI `amb`** (.NET 10): `amb build release --at <commit>`, `amb test <build id> m5`,
  `amb e2e <build id> -- -m "not slow"`, `amb status`, `amb logs -f <id>`, `amb cancel <id>`,
  `amb artifacts <build id>`; `--wait-max 270` exits 97 while the request still runs; `--json` everywhere.
- **The e2e engine stays Python** (D12): its guest half runs unchanged in an e2e slot; its host half gets the build
  server as a second transport beside claude-fleet (a slot instead of a desktop, a fresh slot instead of a reset).

## 11. Source in, artifacts out

- **Builds come from commits, never from the live tree.** The host agent keeps the bare mirror
  (`Q:\buildserver\git\agentmaster.git`), fetched from `K:\source\Agentmaster` by path. Uncommitted work builds only as
  a snapshot commit under `refs/buildserver/snapshots/<id>`; no branch or HEAD moves.
- **Into a VM:** a git bundle of what its clone lacks, written into its exchange folder.
- **Out of a VM:** the guest writes the layout, zip and logs to its exchange share; the host agent moves them into the
  store and records their SHA-256 in the build's manifest.
- **The build steps come from the commit; the install steps from the host agent's versioned copy**
  (`buildserver/steps/`).
- **Purge decisions:** the build-install skill's triggers (`SessionModels.h`, `SessionRegistry.*`, any `.idl`, widened
  for a TerminalControl `.idl`), new data members in a TerminalApp header, or `--purge force`; one purge and retry
  after a link error on a symbol the source defines.

## 12. Life cycle and stability

| Step | Measured |
|---|---|
| start the distro (cold / running) | 0.52–0.79 s / 0.33 s |
| make a slot (overlay + a sparse data disk) | under 1 s |
| VM start → guest agent ready | 30–44 s (75 s once, while Windows had 2.9 GB available); a changed VM configuration costs two extra reboots the first time (99 s) |
| inside that: QEMU + UEFI, then from Windows' kernel: desktop, agent started, share answering | ~7 s; then 12.6–22.7 s, ~22 s, 24–32 s |
| graceful stop (ACPI shutdown) | 7.6–9.9 s idle; 13.6 s with Agentmaster open |
| image install (Tiny11 Core, ISO cached / with download) | 12½ / 16½ min |
| VS Build Tools install in the image | 27.9 min, its 4.8 GB download included (exit 0, no reboot); then ⚠ 30+ min of servicing (TiWorker) unless stopped |

- **A request's states:** `pending → starting → running → collecting → succeeded | failed | cancelled | no-capacity |
  lost`, each change an event line.
- **Heartbeats:** the guest agent every 10 s; a slot silent for 3 minutes is stopped and its request marked `lost`; a
  build is retried once.
- **Restarts:** the coordinator reads its records and fastrund's runs (F5) and carries on.
- **The homelab is not needed** for homepc work once the dockur image is in the distro.
- **Coexistence:** hand builds of the stock tree keep working; the allocator sees their memory as load it does not own.

## 13. Plan

Each phase needs the owner's go to build and deploy, and ends with checks a run proves. **The order** (proposed
2026-10-25 and started the same day at the owner's "kindly begin implementing"): the proofs that could still change
the design come first; the e2e lane comes before builds, because e2e fits this PC's memory and disk today and builds do
not; and the Agentmaster side does not wait for fastrun's features. **The starting point** is what agents did by hand
until now (`Q:\agentmaster-e2e`): private builds from a source cache checked against a commit, under the build lock and
in a Job object (`sessionstart-build\build_controller.py`), packaged and checked with their PDBs, and e2e runs on
claude-fleet desktops whose creation alone took 549 s (`vm-setup-validation-20261024`).

**Phase 0, the proofs that can change the design:**

| # | Proof | State |
|---|---|---|
| P0.1 | the `buildserver` distro on Q: (sparse after W1's restart); fastrund 1.2.x in it: ad-hoc runs, its ports from Windows, its idle cost | **passed 2026-10-25** (§16.9): the distro made by `site/sitectl.py create` in 11.5 s; production's fastrund image in it answers `/healthz` from Windows 0.36 s after `podman run`, runs ad-hoc scripts (the first 2.6 s after its start, then ~15 ms from Windows), 30 MB resident; found: Windows reaches it only at the distro's address, its HTTPS registry pulls fail (plain HTTP through Traefik works), slots run without cgroups |
| P0.5 | e2e on Tiny11 Core: a few suites on Core and on a full Windows 11 desktop, side by side (decides whether `am-e2e` is Core or regular Tiny11) | Agentmaster's portable zip runs on Tiny11 Core |
| P0.4 | a full build in a VM: Debug, cold and incremental, against native (W4: over 2× native → the host lane) | the engine harness: 1.55× the host (107.5 s against 69.2–69.9 s); a full build needs ~55 GB of Q: while it runs (W3) |

**Then two tracks in parallel:**
- **Track A, fastrun** (its repository, in a worktree of its own; tested on this PC's node and fastrun-next, never on
  dock's production until Phase 5): F9 + F1 → F2 + F3 → F5 → F8 → F7. F4 and F6 wait for W1's cgroup v2.
- **Track B, this repository:** the phases below. Until Track A's VM slots exist, Track B starts VMs through a
  stand-in (rootful podman + dockur, what the proofs used) behind ONE slot interface; the fastrun driver replaces it in
  Phase 2, so the finished system runs every slot on fastrun (D3).

| Phase | Delivers | Done when |
|---|---|---|
| **P1, e2e on demand on this PC** | the image script, the Tiny11 Core base with the guest agent, the `am-e2e` layer; the guest agent (HTTP), the host agent (front door, wakes the distro, the store), a minimal coordinator (records, one site, §3.2's reserves; a VM boots at its grant), the CLI, the e2e engine's second transport; `amb import <zip>`: a hand-built zip or a GitHub release as a build id, so e2e works before builds exist | `amb e2e <build id>` runs a suite in a fresh VM and leaves nothing behind, in every outcome |
| **P2, fluid memory, on fastrun** | balloons, pressure handling (§3.5), pending with timeouts, several VMs at once; KSM measured (W2); the slot driver switched to fastrun; the coordinator a fastrun app | two e2e VMs side by side while Windows keeps its 6 GB; a request under pressure waits and starts by itself, or ends `no-capacity` |
| **P3, builds** (after P0.4, W3, W1) | the `am-build` layer sealed with servicing idle; build VMs kept 10 minutes; the mirror and bundles; artifacts into the store; `amb build`, `amb test m5\|syntax` | a Debug build of HEAD gets a build id and its artifacts; two identical requests → one build; nothing written to K:; `amb e2e <that id>` closes the loop |
| **P4, the install lane, then `install`** | `am-install` from regular Tiny11; the install suites and a weekly full run on it; `install` of a build id through the swap script | a dev install of a build id; an injected failure after the swap's stop rolls back |
| **P5, dock** (E3) | dock's e2e slots | an e2e run lands on dock by its score while homepc builds |
| deferred | `publish` (D11), freeze (F6), the host lane unless P0.4 calls for it (D10) | — |

## 14. Decisions

**Made by the owner and the measurements:** E1 → §3; E2 → §5.2, §8; E4 → §4; E5 → §3.2 and §16.

**Open:**

| # | Decision | Recommendation |
|---|---|---|
| E3 | the homelab changes for dock's Windows slots (§5.3) | after P3; with its priority, dock then takes e2e first |
| W1 | one restart of WSL's VM, which ends every container and other sessions' desktops: it brings cgroup v2 (for per-run limits and freeze on homepc: WSL 2.3.24 → 3.0.3, WSL dropped cgroup v1 in 2.5.1, together with a Docker Desktop that supports it; 4.35.1 here, from October 2024) and makes the site's disk sparse (§5.2) | not needed to start (VMs are bounded by QEMU and the balloon; the distro reuses its own free space). When wanted: at a quiet time, after reading both release notes |
| W2 | KSM in the WSL VM | measure it when no other session measures memory there; then decide |
| W3 | disk for builds: the `am-build` layer (18.5 GB) and a build VM (~40–60 GB) on Q:, which had 31–70 GB free during the tests, with 15 GB kept for other work | the third revision's reclaim list (the stock build trees in `Q:\tmp`, session leftovers on Q:); until then build requests wait or end `no-capacity: disk` |
| W4 | builds in VMs or on the host (D10) | builds stay in VMs, as the owner chose: ~1.55× on compile work is the price of a clean, isolated build machine (a process starts in 23–31 ms there, in 109 ms on the host under Defender). The host lane (D10, deferred) is the fallback if P0.4's full build takes more than 2× the native time |

## 15. Risks

| Risk | How it is contained |
|---|---|
| builds are slow in a VM: the guest's disk does ~2,000 random I/Os per second | measured 1.55× on compile work at equal parallelism; a build VM gets more vCPUs than the host's hand builds use (8 against 3–6 compilers); P0.4 measures a full build; above 2× builds take the host lane (D10) |
| Tiny11 Core lacks something a build or the app needs, and cannot be serviced | measured: the app runs, and Build Tools installs (exit 0); ⚠ its servicing worker then ran for 30+ minutes, so the build image is sealed only with servicing idle (TrustedInstaller stopped, checked at the next boot) |
| a test passes on Tiny11 Core where a user's Windows 11 fails, or the reverse | P0.5's side-by-side run; the install lane and a weekly run on `am-install`; no build tools in e2e images |
| dockur's Core ISO is old (November 2025) and gets no updates | VMs are LAN-only, take no inbound connection, live for one request or a few minutes after it, and run only this repository's code; a fresh base from Tiny11 Builder's engine when needed (§4.1) |
| the WSL VM fills (other sessions' VMs, page cache) | the allocator reads its free memory; balloons shrink first; the site drops clean cache after big copies |
| dockur wipes an image folder without `windows.boot` | the image script writes the marker and checks it before every boot; images are read-only to slots |
| the fastrun features take longer than planned | the proofs ran dockur under podman; the coordinator's design does not depend on how a slot starts |
| dock's thin pool fills | dock takes e2e slots only, after E3, with ~20 GB counted against the 80 % alert |

## 16. Measurements

### 16.1 This PC (2026-10-25)

- Windows 11 Pro 26200, i9-13900K (32 threads), 63.7 GB of RAM. During the tests: 2.5–8.9 GB available; commit
  133.3–146.7 GB used of a 177.1–190.1 GB limit (the page file on C: grows and shrinks); C: 70.9–84.3 GB free; Q:
  70.4 → 27 GB free (the test distro's disk grew to 30.8 GB; another session's Tiny11 build wrote ~13 GB); K: 10.3 GB.
- WSL 2.3.24.0, kernel 5.15.153.1-2, no `.wslconfig`: one VM of 31.2 GB + 8 GB of swap for `docker-desktop`, `Ubuntu`
  and the test distro; cgroups hybrid (all controllers on v1); `/dev/kvm` in every distro; KSM present and off;
  `kvm_intel`: `nested=Y`, `enlightened_vmcs=Y`, `enable_apicv=N`.
- Docker Desktop 4.35.1 (engine 27.3.1). Other sessions ran two dockur VMs of 8.2 and 5.3 GB during the tests.
- vmmemWSL: working set 25.46 GB and private bytes 31.19 GB at the fullest; 18.06 / 23.48 GB after our VM gave 6 GB back.

### 16.2 Tiny11 Core 25H2 in dockur 6.05 (QEMU 11.1.0), 4 vCPUs

| What | Measured |
|---|---|
| ISO download (3.3 GB, archive.org) / check | 3 min 59 s / 9 s |
| install, container start → guest agent ready | 16 min 34 s (12 min 26 s without the download); Windows' own record of that first boot: 203.6 s |
| system disk (qcow2) after install / after four boots and an app | 9.2 GB / 12.1 GB |
| Windows / WinSxS / page file | 7.79 GB / 0.39 GB / 1.4 GB (with 4 GB of RAM) |
| at rest (4 GB guest, 389 s up) | 2,380 MB available, 1,427 MB committed, 131 processes, 93 services |
| running and trimmable | WSearch, SysMain, DiagTrack, Spooler, DoSvc, lfsvc, CDPSvc, TermService, SearchHost, StartMenuExperienceHost, msedgewebview2 |
| present | msiserver, AppXSvc, BITS, Task Scheduler, winget, WebView2 122.0.2365.106, BalloonService, .NET Framework 4.8.1 (release 533509), PowerShell 5.1 |
| absent / off | Windows Update (disabled; UsoSvc gone), Defender (disabled), the VC++ runtime, Edge, hibernation; optional features cannot be listed |
| Agentmaster v0.8.0 portable | unzip 10.4 s; runs and draws (window, side panel, PowerShell tab, badge); 103 MB working set, 34 MB private; the guest's available memory −148 MB |
| VS 2022 Build Tools (the VCTools and UniversalBuildTools workloads with recommended components, SDKs 22621 + 26100, vcpkg, the .NET SDK) | exit 0, no reboot; bootstrapper 15.2 s, install 1,675.6 s (the 4.8 GB download at ~16 MB/s included); C: +14.59 GB (Program Files (x86) 15.6 GB, Windows Kits 3.35 GB of it; .NET SDK 9.0.319 1.01 GB; MSI package cache 0.93 GB; Windows Installer cache 0.67 GB), D: +3.93 GB (`D:\BuildTools`); MSVC 14.44.35207 (cl 19.44.35229), MSBuild; TiWorker then used 1,791 CPU-seconds until TrustedInstaller was stopped |

### 16.3 Boot, stop, slot

| What | Measured |
|---|---|
| VM start → agent ready, steady state | 30–44 s; 75 s once (boot main path 48.9 s by Windows' own record, while Windows had 2.9 GB available) |
| phases from Windows' kernel (the Docker-volume VM, fourth boot) | desktop 12.6 s; the agent's first report at 24 s |
| from an overlay in the test distro | 44 s (from the kernel: agent started 22.1 s, desktop 22.7 s, share answering 32.3 s) |
| graceful stop | 7.6 s, 9.9 s idle; 13.6 s with Agentmaster open |
| overlay + 64 GB sparse data disk | under 1 s |
| copy of the 12.1 GB image, `docker … tar | wsl.exe … tar` | 480.6 s (25 MB/s) |

### 16.4 Memory and the balloon

| What | Measured |
|---|---|
| QEMU resident, 4 GB guest / 8 GB guest | 4.08 GB / 8.07–8.46 GB, within 1–2 min of boot |
| balloon 8 → 3 GB / 8 → 2 GB | ~45 s / under 30 s; resident followed (3.07 GB) |
| balloon 3 → 8 GB | 17.9 s; resident back to 8.07 GB at once |
| what Windows got back from −6 GB | available 3.27 → 8.90 GB; vmmemWSL working set 25.46 → 20.51 GB |
| `free-page-reporting=on` | no effect for a Windows guest |

### 16.5 CPU, processes, files and compiling (guest: 4 vCPUs; host: 4 threads of 32)

| Test | Host | Guest |
|---|---|---|
| managed SHA-256, 1 thread | 1,318 MB/s | 1,200–1,279 MB/s (91–97 %) |
| managed SHA-256, 4 threads | 5,271 MB/s | 3,714–4,927 MB/s (70–93 %) |
| `cmd.exe /c exit`, per process | 109 ms (Defender on the host) | 23–31 ms (no Defender) |
| 5,000 files of 4 KB: create / read / delete per second | 2,012 / 17,033 / 10,174 (Q:) | 1,143–5,560 / 33,802–42,547 / 7,211–11,310 |
| the engine harness: 63 C++20 files (62 objects + the link) at /MP4 | 69.9 s, 69.2 s (MSVC 19.44.35228) | 107.5 s with servicing stopped (1.55×; CPU 79 %, peak commit 3.9 GB); 115.6–158.2 s while TiWorker ran (CPU 96 %) |

### 16.6 Disk

**Host level** (fio in an Alpine container, 2 GB file, 15 s per test, O_DIRECT unless noted):

| Where | random 4K write / read, QD32 | sequential 1M write / read, QD4 |
|---|---|---|
| a Docker volume (C:) | 46,705 / 71,695 IOPS | 2.09 / 4.28 GB/s |
| the test distro's disk (Q:) | 64,092 / 87,624 IOPS | 4.75 / 7.20 GB/s |
| an integrated distro's disk bound into a Docker Desktop container (Ubuntu, C:) | 52,439 / 99,667 IOPS (256 MB file, 10 s) | 1.55 / 4.32 GB/s |
| a bind of `Q:\…` | 4,858 / 5,126 IOPS | 0.16 / 0.17 GB/s |
| a bind of `C:\…` | 2,898 / 4,336 IOPS | 0.15 / 0.17 GB/s |
| the host's own Q: (DiskSpd, OS cache off) | 75,724 / 135,619 IOPS | 975 / 6,072 MiB/s (one thread) |

**In the guest** (DiskSpd, OS cache off, a 1 GB file written first so no read hits a hole):

| Attachment | random 4K overwrite QD32 / QD1 | random 4K read QD32 / 4×8 | sequential fill |
|---|---|---|---|
| virtio-scsi, raw, cache=none, aio=native (dockur's defaults) | 2,249 / 1,877 IOPS | 1,537 / 1,658 IOPS | 329 MB/s |
| virtio-scsi, qcow2 overlay, the same options | 2,035 / 1,700 IOPS | 2,024 IOPS | 353 MB/s |
| virtio-scsi, raw, cache=writeback, aio=threads | 3,324 / 2,925 IOPS | 3,667 / 2,902 IOPS | 177 MB/s |
| virtio-scsi with 4 queues, raw | 1,712 / 1,154 IOPS | 1,425 / 1,095 IOPS | 264 MB/s |
| virtio-blk with 4 queues, raw | 2,107 / 1,763 IOPS | 1,937 / 1,873 IOPS | 222 MB/s |
| the Docker-volume VM (qcow2 on C:), DiskSpd's own 2 GB file, writes into new space | 1,122–1,814 IOPS | (14,109–16,415: mostly holes) | 493 MiB/s at QD4 |

Every attachment lands at ~1,100–3,700 IOPS, at any queue depth: each I/O pays its way through nested virtualization
(`enable_apicv=N`), about 0.3–0.6 ms. Sequential work is fine; random work is the cost.

### 16.7 dock (a week of Proxmox RRD, and SSH on 2026-10-24)

32.8 GB of RAM, 26.6 GB used (median; 25.7–26.8) → 6.0–7.1 GB available; CPU 1.9–2.7 % of 16 threads; load
0.30–0.76; swap 0.01–1.07 GB of 8 GB; iowait 0.16 %. 7-Zip: 55,384 MIPS on its 16 threads, as much as this PC's 32
threads under their usual load (55,913).

### 16.8 What the tests made, and removed afterwards

- Made and removed: the Docker container and volume `bs-core11-install` and the fio volume `bsbench`; the test
  distros `bs-measure` and `bs-sparse` (unregistered, their disks deleted); the folders `Q:\buildserver` (the image
  copy, the VM disks, the tools, the results) and `C:\buildserver-bench`; a test folder in the Ubuntu distro. Q: was
  back to 70.9 GB free afterwards.
- Kept: the scripts in [`measure/`](measure/README.md) and the results in
  [`measure/results-2026-10-25/`](measure/results-2026-10-25) (JSON, and a screenshot of Agentmaster on Tiny11 Core).
- Not changed: either Agentmaster instance, fastrund, the homelab, any WSL or Docker Desktop setting; KSM stayed off.
- A graceful stop of the build VM took 15.4 s (after the Build Tools install).

### 16.9 P0.1: fastrund in the site distro (2026-10-25)

The real site distro, made by [`site/sitectl.py`](site/sitectl.py) `create`, and production's fastrund image in it,
by [`measure/p01_fastrund.py`](measure/p01_fastrund.py) (result: `measure/results-2026-10-25/p01-fastrund.json`).

| What | Measured |
|---|---|
| `sitectl.py create`: Alpine 3.22.7's minirootfs fetched and checked / imported / packages + configuration | 1.1 s / 0.8 s / 9.7 s; the disk 0.25 GB; podman 5.6.2 (cgroups v1, cgroupfs, overlay) |
| fastrund 1.2.1 (`fastrund:20261012-2`), privileged, host network, read-only root, releases off | API listening 15.7–50 ms after its start; the spawner "ready: cgroups off" ("no cgroup v2 here; slots stay in fastrund's cgroup") |
| `/healthz` from Windows at the distro's address, after `podman run` | 0.36 s |
| the same through Windows' localhost | refused: fastrund listens on `:::2200` (the IPv6 wildcard) and WSL 2.3.24's localhost forwarding passes only IPv4 listeners (a Python server on `0.0.0.0` was forwarded: 200 in 97 ms) |
| runtime images pulled from `https://docker-registry.elibelash.com` | refused at once: "The SSL connection could not be established", with or without `SSL_CERT_FILE`, while curl in the distro verifies the certificate (Let's Encrypt, TLS 1.3) |
| the same over plain HTTP through Traefik (`http://docker-registry.local`) | sh and bash ready 1.0–1.2 s after the start, .NET 3.7 s |
| the first ad-hoc script / the next nine (from Windows, NAT) | 2.6 s after the start / 3.5–26.7 ms, median 15 ms; every exit 0; the slot's cgroup is fastrund's container (`/libpod_parent/libpod-…`) |
| fastrund idle, the 30 s right after the runs / resident memory | 14.7 ms of CPU per second (production's nodes: ~0.6 ms, its rule 36; not measured settled) / 30.3 MB |
| `podman stop` (SIGTERM; fastrund ends its slots) | 0.22 s |
| a process started by a `wsl.exe -e sh -c '… &'` call | ends with that call; a podman container (conmon) keeps running |

## 17. Evidence

- **The scripts:** [`measure/README.md`](measure/README.md), one per measurement.
- **dockur:** its `/run/install.sh` (`hasBootMarker`, `cleanupStorage`), `/run/utils.sh` (`hasDisk`),
  `environment.md` and `ballooning.md` (6.05); QEMU's command line read from the running container.
- **Tiny11:** dockur's README (`core11` 3.0 GB, `tiny11` 5.3 GB); tiny11builder's `docs/usage.md`; claude-desktops'
  `images/windows/build_tiny11.py` (read 2026-10-25, uncommitted work of another session).
- **WSL:** `wsl --version`, `wsl --help` (`--manage --set-sparse`, `--move`); the release notes on GitHub: 2.5.1
  "Remove cgroupv1 support", 2.6.2 a `wsl.conf` opt-in to cgroup v1 mounts, 3.0.3 the current release (2026-10-09).
- **fastrun:** `Spawner.cs` (`SetUpCgroups`), `Kube.cs`, `Settings.cs`, `AppReleases.cs`, `Runtime.cs`,
  `proto/fastrun/v1/fastrun.proto`, `deploy/k8s/fastrun.yaml`.
- **The third revision's evidence** (2026-10-24) still holds for kubectl, Docker Desktop's settings, dock over SSH and
  k3s; see 8a840e9c5.
