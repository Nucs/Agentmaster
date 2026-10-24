# Agentmaster build server — design and plan

> **Status: DESIGN ONLY (2026-10-24). Nothing here is built.** The owner's request, the same day:
> "Building and installing and using VM to run e2e is very complicated, and very very resource heavy. We need a
> centralized server under buildserver/ that will use fastrun (K:/source/fastrun) to host two build servers machines
> (one here, another the machine K:/source/proxmox and the kubernetes if k8s has access to a lot of storage). The
> buildserver will centralize and lifecycle manage the build, allocating resource using fastrun and building and/or
> running tests and/or releasing and/or end-to-end tests. This machine has RAM and storage issues and so allocating per
> resources is essential. I want you to plan and design something stable and friendly to run on fastrun in a serverless
> manner (on demand) to serve the needs of agentmaster development."
>
> Every number below was measured on 2026-10-24 unless another date is given (§13 lists how).

## 0. In one page

- **What it is.** One queue and one resource scheduler for every Agentmaster build, check, test, e2e run, deploy
  (swap) and release. Small Windows agents do the work. A stateless service on fastrun coordinates it.
- **The coordinator runs on fastrun, and only on demand.** It has three parts:
  - **`buildserver`**, an app release: an HTTP API and a web page at `https://build.local`. It starts on a
    connection and stops when idle.
  - **`buildserver-tick`**, a one-minute schedule. It wakes the app only when a timer the app set is due.
  - **Its records** live in the cluster's idle PostgreSQL. Nothing of the coordinator runs while nothing happens.
- **The work runs on Windows, never on fastrun.** Agentmaster builds need MSVC, the WinRT/XAML tooling and MSIX
  packaging. fastrun's nodes are Linux LXCs, its runs end at 300 s, and a run gets at most 512 MiB. The workers are:
  - **an agent on this PC**, started by a logon scheduled task (the `llm-bridge` pattern);
  - **a build VM on dock**, started through the Proxmox API when a job needs it, once dock has the memory (§5);
  - **optionally, GitHub's free Windows runners**, for commits the owner has pushed (§4.2).
- **Allocation is the core.**
  - Every job declares what it needs: commit memory, physical memory, CPU, disk and exclusive locks.
  - The scheduler admits a job only where it fits right now.
  - The agent runs it inside a Windows Job Object that enforces that budget: a commit cap, a low priority class, a
    CPU rate and kill-on-close.
  - A guard ends only that job when the machine runs short.
  - A full-screen game holds new work.
- **Builds come from commits, never from the live working tree.**
  - The agent keeps a private mirror and one worktree per configuration on Q:.
  - No build waits for another session's edits, two requests for the same build share one run, and K: is never
    written.
  - Uncommitted work builds only as an explicit snapshot commit, which the binaries name.
- **Friendly and recorded.**
  - One command line (`amb`) serves sessions and the owner; the page and Telegram add approvals and results.
  - Every outcome lands in one place. The two production swaps that failed this week (10-21: 12 min down; 10-24:
    refused) were found only by reading session transcripts.
- **What the homelab can give today:**
  - **dock** has 6.2 GiB of RAM available and an oversubscribed thin pool, so a Windows build VM needs a RAM
    upgrade (the board takes 2 × 32 GB) or the loan of VM 100.
  - **k3s** has no large storage of its own, so it holds the coordinator and its database, not caches or artifacts.
  - **Artifacts** live on this PC's Q:, where the instances that consume them run.

## 1. Why: what goes wrong today

| Problem | Evidence |
|---|---|
| The machinery is hand-run scripts in temp folders | `stagebuild.ps1`, `prodswapR.ps1`, `devbuildswap.ps1`, `stagegate.ps1` live in session scratchpads under `%TEMP%` and are copied from session to session (memory `swap-scripts-scheduled-task`). A temp clean-up loses them. |
| Builds starve the PC | 2026-10-23 → 24: four attempts of one purged Debug build were stopped by the memory guard (free commit 0.1–7 GB beside two Windows test desktops, ~32 GB of WSL and ~58 claude processes); the sixth took 1,239 s at one compiler. 2026-10-11: two compilers took free commit from 5.6 to 1.1 GB in 27 s. |
| e2e is the heaviest thing on the PC | A test desktop takes 8 GB and 6 CPUs (`e2e_run.py`'s defaults) inside Docker Desktop's WSL VM, which may grow to 31 GiB. A full suite runs ~37 min plus provisioning. An install-lane reset takes 35–70 s on a quiet PC and 7–10 min beside three other desktops. Someone must remember to delete the desktop. |
| Builds use the live working tree | A build waits up to 10 min for other sessions' uncommitted edits, or builds over them (`-AllowDirtyBuildFiles`). The stock tree's `Generated Files` is shared by Debug and Release, so they never build at once. |
| No sharing, no queue | One mutex (`am-lock.sh`) serializes everything. Two sessions that want the same build build it twice, one after the other. |
| Purge decisions are manual | Missed purges crashed dev more than once: the 2026-10-17 AV in `_SidePanelSelectSession`, and earlier a `/RTC1` failure and a heap corruption from stale generated glue (the `build-install` skill §5.8). The rule lives in a skill and in scripts. |
| Outcomes are not recorded | The 10-21 production swap died after its stop (a WMI "Call cancelled"), and production stayed down 12 min. The 10-24 one was refused (`0x80073CFB`, production runs the v0.8.0 MSIX). Both were unrecorded until commit a05c0c2ff. |
| Disk | K: has 11.8 GB free. Q: has 112 GB free, and the stock build trees occupy another 52 GB there (21 + 22 + 8.7). The e2e golden disk is 14 GB on Q:, and the desktops' overlays grow Docker's image on C: (54.8 GB free). |

## 2. Requirements

| # | Requirement | Source |
|---|---|---|
| R1 | One central service, in `buildserver/`, hosted on fastrun, running only on demand | the request |
| R2 | Two build machines, this PC and the Proxmox host; k3s for what it can carry | the request |
| R3 | Life cycle of builds, tests, releases and e2e runs | the request |
| R4 | Per-resource allocation that protects this PC's RAM and disk | the request |
| R5 | Stable: survives restarts, crashes, a homelab outage and a power cut; never leaves an instance down | the request; the 10-21 outage |
| R6 | Friendly for Claude sessions and for the owner (phone included) | the request |
| R7 | Keeps the repository's rules: the user's permission before a build, deploy or install; every build writes to Q:; never stop `OpenConsole.exe`; the swap and purge rules; secrets never printed | CLAUDE.md *Development Rules*, *Deploy & run*, *Gotchas* |

## 3. The machines (measured 2026-10-24)

| Machine | What it has | Free now | Fit for |
|---|---|---|---|
| **homepc** (this PC, 192.168.50.17) | i9-13900K, 32 threads, 64 GiB; page file to 174 GB of commit; RTX 4060 Ti; Windows 11 Pro N; VS 2022 | 12.5 GB physical, 55.3 GB commit at 18:00; Q: 112 GB, K: 11.8 GB, C: 54.8 GB. Biggest holders: 24 claude 14.9 GB, `vmmemWSL` 12.4 GB, 106 node 6.2 GB, a game 5.7 GB | **Every kind of job.** The only machine that builds today, the only one with Docker for the test desktops, and the one that runs both instances. |
| **dock** (Proxmox VE 8.2.7, 192.168.50.202) | Ryzen 9 6900HX, 8 cores / 16 threads; 31.3 GiB (2 × 16 GB DDR5; the board takes up to 64 GB); NVMe thin pool 794 GiB; USB hard drive 10.9 TB (NTFS "Seedbox", the owner's data) | **6.2 GiB available** (VM 100 runs with 16 GiB). Thin pool 71.4 % used, 237 GB available, oversubscribed (1.96 TiB of thin volumes on 10-11). CPU idle (load 0.24) | A Windows build VM **only after** more RAM, or with VM 100's 16 GiB (§5). The USB hard drive is too slow for build trees and is not ours. |
| **VM 100 `win11-1`** on dock | Windows 11, 8 vCPU, 16 GiB, 96 GB disk; GPU passthrough; the TraderLab worker | in use as "a dedicated compute machine" (the owner, 2026-10-02) | A build worker only by the owner's choice, sharing with TraderLab (§5). |
| **VM 101 `win11-2`** | template: Windows 11 Pro N 23H2, 8 GiB, 6 cores, 96 GB, guest agent + virtio | stopped | The base to clone a build VM from. |
| **k3s** (CTs 112/113/114 on dock) | 8 + 10 + 10 GiB, 6 + 16 + 16 cores; root filesystems 7.8 / 24 / 40 GB; default storage `local-path` | nodes at 14–17 % memory, ~0 % CPU | **The coordinator only**: fastrun apps and schedules, plus the database. No Windows. No large disk. |
| **PostgreSQL** `default/postgresql-0` | Bitnami 17.0, ClusterIP 10.43.41.245:5432, 8 GiB `local-path` on k3s-node-1; limits 192 MiB / 0.15 CPU | no tables, nothing uses it (homelab §5.8) | The coordinator's records: jobs, events, approvals, small logs. Not bulk logs. |
| **fastrun** 1.2.1 | apps on 2101–2199 (2101–2106 taken: hello, yoinks, dlss, cc-relay, llm-chat, family-tv); releases ≤ 300 s, ≤ 512 MiB; one 1 GiB memory limit for the whole fastrund pod; `notBefore` at most 10 s ahead | 9 releases | Hosting the coordinator. A run cannot hold a build. |
| **QNAP NAS** | TS-431K, RAID 5, ~14 TB (93 % used by the PBS datastore when it last held it) | RAID offline 2026-10-07 → 17; backplane fault suspected | **Nothing.** Too fragile to depend on. |
| **GitHub Actions** | public repository: free `windows-2022` runners (4 vCPU, 16 GB) with VS 2022 | CI builds x64 Release in 17–26 min; `release.yml` builds x64 + ARM64 in 26–30 min | Clean Release builds of **pushed** commits, release pipelines. Not e2e (no nested virtualization, and UI tests on hosted runners are unproven). |

**What that means:**
- **Only Windows machines can build:** this PC, a VM on dock, or GitHub's runners.
- **"k8s with a lot of storage" does not exist today.** Its only large volume is an SMB mount of the NAS. So the
  cluster holds the coordinator and its database, and artifacts stay on the workers.
- **The PC remains the main worker.** The design must make it safe to load, not just add machines.

## 4. Architecture

```text
 who asks                       fastrun on k3s (Linux)                          Windows workers
┌────────────────┐  build.local  ┌──────────────────────────────┐  HTTP + token  ┌───────────────────────────────────┐
│ amb (CLI)      │──────────────▶│ app `buildserver` :2107       │──────────────▶│ homepc agent :5291 (logon task)    │
│ web page       │  Caddy CT 104 │  API + page, stateless,      │◀──────────────│  mirror + worktrees on Q:          │
│ Telegram       │               │  starts on a connection,     │  heartbeats,  │  each job in a Job Object          │
└────────────────┘               │  stops when idle             │  logs, results│  Docker test desktops (e2e)        │
                                 │        │          ▲          │               │  GitHub lane via the user's gh     │
                                 │        ▼          │ wake      │               └───────────────────────────────────┘
                                 │  PostgreSQL       │ when due  │  PVE API      ┌───────────────────────────────────┐
                                 │  (records)        │           │──────────────▶│ dock VM `win-build` (§5, later)    │
                                 │ schedule `buildserver-tick`   │  power on/off │  on when a job needs it, off idle  │
                                 │  every minute: the alarm      │               └───────────────────────────────────┘
                                 │  clock + Telegram approvals   │
                                 └──────────────────────────────┘
```

### 4.1 The coordinator on fastrun

- **App `buildserver`.** An image from `buildserver/control/` with a Dockerfile, built on this PC by `fastrun deploy`
  (Docker Desktop must run for that) and run by digest:
  - `--port 2107` (re-check that it is free at deploy time), `--health /healthz`;
  - `--nodes k3s-node-2,k3s-node-1` (the workers have the disk), `--idle 60`, `--max-instances 1` per node;
  - `--secret buildserver`, `--label app=buildserver,lifecycle=on-demand`.
- **.NET 10, Native AOT.** An ASP.NET Core minimal API with Npgsql and source-generated JSON. That is the homelab's
  usual stack (fastrun, llm-bridge, the catalog). The expected footprint is ~30–50 MB per running instance, inside
  fastrund's shared 1 GiB pod.
- **Warmth.** `--warm none` at first. `--warm frozen` keeps a booted instance only if the measured footprint is small
  enough for the page to deserve it.
- **Stateless.** Every request reads and writes PostgreSQL. No timer lives in the process, because a stopped or frozen
  instance runs none. Two instances (one per node) are safe: every claim is a row lock (`SELECT … FOR UPDATE SKIP
  LOCKED`).
- **The database.** A new database `buildserver` with its own role in the idle `default/postgresql`, reached through
  its ClusterIP from the node's network (to verify in P1; a NodePort is the fallback). The role's password is a key
  of the Secret.
  - Bulk logs stay on the workers. The database keeps each job's last 64 KB plus compressed full logs up to a cap
    (§4.6), because the pod's limit is 192 MiB of RAM and 0.15 CPU.
- **The alarm clock.** fastrun has no general timer (`notBefore` reaches 10 s ahead), and an idle app must stay
  stopped. The coordinator therefore owns one ConfigMap, `buildserver-clock`, with two keys:
  - `wakeAt`: the earliest moment something needs it, such as a lease expiry, a job waiting for memory or a VM
    idle-stop;
  - `pollTelegram`: whether an approval is waiting.
- **The tick, `buildserver-tick`.** A Python zip app on the schedule `* * * * *`, like `fastrun-panel`, home
  k3s-node-1. Each run:
  1. reads the clock with a ServiceAccount allowed only `get` and `patch` on that one ConfigMap (fastrun's
     "State in a ConfigMap" pattern). The app mounts the same ServiceAccount's token to write `wakeAt`;
  2. POSTs `/internal/tick` to the app when `wakeAt` has passed;
  3. while `pollTelegram` is true, long-polls the build bot for up to 55 s and hands each button press to the app.
  - **Cost.** An idle minute costs one run of a few tens of ms and one API read. The app never wakes, and no bot
    polls.
- **The hourly audit.** A second schedule, `buildserver-audit`, wakes the app once an hour whatever the clock says. It
  catches a clock that was never set (a bug, or a crash between a write and the clock update).
- **A name.** `https://build.local`: a Caddy block on CT 104 (`reverse_proxy 192.168.50.152:2107 192.168.50.151:2107`,
  `lb_policy first`, `keepalive 5s`), a Technitium A record to 192.168.50.5, and the name as a SAN of `local-tls`.
  This is the `llm-chat` recipe. `build.sysd.cc` behind the sysd gate can come later, for the phone outside home.
- **Releases and the test copy.** `buildserver-next` runs on :2182 and node-2 only, as fastrun's rule "try things under
  a name of your own" asks. It is removed after each test.

### 4.2 The workers

One program, **`amb.exe`** (.NET 10, `buildserver/agent/`), is both the agent (`amb agent`) and the command line.

| Worker | How it runs | Capabilities |
|---|---|---|
| **homepc** | Scheduled task `AmBuildAgent` at logon, interactive, limited rights, restarted on failure. The task's action is `amb.exe` itself, a WinExe with no window, so stopping the task stops the agent. That is the `llm-bridge` pattern. **It sits outside every Agentmaster process tree, so no swap can end it.** It listens on 192.168.50.17:5291 (re-check that it is free; llm-bridge holds 5281, dlss 5271) with a bearer token. A firewall rule admits TCP 5291 only from 192.168.50.151 and .152 (the fastrun workers) and loopback, for `amb.exe` only. Its state and secrets live in `Q:\services\buildserver\` (ACL: the user and SYSTEM); its data in `Q:\buildserver\` | `windows`, `vs2022`, `docker` (the e2e desktops), `instances` (it alone may stage and swap dev and production), `github` (the user's `gh` login; never copied into the cluster — the precedent of `opensource-monitors`) |
| **dock `win-build`** (§5) | A VM cloned from template 101, with VS 2022 Build Tools, the SDKs, Git, PowerShell 7 and the agent as a service. Off unless a job needs it: the coordinator starts it with a PVE token limited to that VM; it stops itself after 15 min with no job (a policy) | `windows`, `vs2022` |
| **GitHub lane** (optional, D6) | Not a machine. A job of the homepc agent dispatches a workflow (`workflow_dispatch` on `ci.yml` or a new `buildserver.yml`) for a commit that is already on GitHub, watches it through the Actions API and downloads its artifacts into the store. It costs the PC a few MB and no CPU | `windows-cloud` |

### 4.3 Where the source comes from

- **A private mirror.** `Q:\buildserver\git\agentmaster.git` is a bare mirror (198 MB of history today). The agent
  fetches it from `K:\source\Agentmaster` over the local path, a no-network fetch of milliseconds.
- **Worktrees.** One per configuration: `Q:\buildserver\trees\debug`, `…\release`, and `…\release-clean` for
  release pre-validation. Each holds a 95 MB checkout, its own `Generated Files` (1.1 GB), `obj` and `bin`. A
  `packages\` junction points to one shared NuGet folder (894 MB).
  - Debug and Release have separate generated glue, so they **may build at the same time** when memory allows. The
    stock tree forbids that.
- **Every job names a full commit hash.** The command line resolves the caller's ref (default `HEAD`) and pushes it to
  the mirror as `refs/buildserver/requests/<id>` (a local push), then submits. The build bakes that commit
  (`AgentmasterGenerateBuildInfo` runs `git rev-parse --short=9 HEAD` in the worktree).
- **Uncommitted work** (only on the user's word, CLAUDE.md's rule):
  - `amb build --snapshot` writes the working tree into a commit object with a private index (`GIT_INDEX_FILE`,
    `add -A`, `write-tree`, `commit-tree -p HEAD`). It records it as `refs/buildserver/snapshots/<id>` and moves no
    branch and no HEAD.
  - The binaries then name a commit that holds exactly the code they were built from. Today's
    `-AllowDirtyBuildFiles` only records hashes.
- **Other workers** get the commits as a `git bundle` from the homepc agent: only the objects since that worker's last
  build, authenticated by the agent token. No git server is needed; Gitea (CT 140) stays off.
- **Build steps come from the commit; deploy steps from the agent.**
  - `tools\Build-Agentmaster.ps1` runs from the worktree being built, because it is part of that commit's build.
  - The stage gate, the swap and the post-swap checks run from the agent's own versioned copy (`buildserver/steps/`),
    so a rollback to an old build still uses today's fixed swap.
- **K: is never written by a build**, and nothing builds in the stock tree. Its junctioned build folders
  (`Q:\tmp\agentmaster-*`, 52 GB) keep serving hand builds until the owner retires them (D7).

### 4.4 Jobs

| Kind | What it does | Runs on | Locks | Default approval (§4.8) |
|---|---|---|---|---|
| `compile-check` | `cl /Zs` of the changed translation units with the worktree's PCH (the memory note `zs-syntax-check-no-build`) | homepc, dock | tree (shared) | auto |
| `lib-check` | Builds `TerminalAppLib.vcxproj` (and the other `*Lib`s on request) in the worktree | homepc, dock | tree | auto |
| `m5` | The engine harness (`run-m5-tests.bat`) at the commit, in a scratch profile (never the live one: memory `m5-harness-inherits-live-profile`) | homepc, dock | — | auto |
| `build` | A full package build: `Debug` or `Release`, dev or release identity, purge `auto`, `force` or `none`, into the store | homepc, dock, GitHub (Release) | tree | ask |
| `zip` | A portable zip from a built layout (`New-UnpackagedTerminalDistribution.ps1`, its `Layout` parameter set), for e2e and test installs | where the layout is | — | auto |
| `stage` | A build into the slot the instance does **not** run from (`Q:\agentmaster-layouts\…`), checked and pinned (today's `stagebuild.ps1` + gate) | homepc (or built elsewhere and copied, with the six pinned files' hashes) | tree, the instance's slot pair, `build-launch` | ask |
| `swap` | Today's swap, with the three fixes (§4.7) | homepc | the instance, `build-launch` | ask, every time |
| `e2e` | A suite against a build: zip → desktop → provision → run → collect → delete (§6) | homepc | a desktop slot | ask |
| `install-lane` | Install scenarios, each on a freshly reset desktop | homepc | a desktop slot | ask |
| `ci` | The GitHub lane: a clean Release build of a pushed commit | GitHub | — | auto |
| `release` | The release pipeline with its gates (§7) | homepc + GitHub | `release` | ask, every gate |

- **Chains.** `deploy dev` is `stage` → approval → `swap`; `test <build>` is `zip` → `e2e`.
- **Deduplication.** A request whose kind, commit, configuration and options equal a queued or running job's joins it.
  An identical finished job's artifacts are reused while they exist.

### 4.5 Resource allocation (the core)

**What a worker reports.** The agent samples these when asked and every 30 s while a job runs:
- free commit (`Win32_OperatingSystem.FreeVirtualMemory`) and available physical memory;
- CPU load, and free space on Q: and C:;
- the full-screen state (`SHQueryUserNotificationState`: `QUNS_RUNNING_D3D_FULL_SCREEN`, `QUNS_BUSY`,
  `QUNS_PRESENTATION_MODE`) and the user's idle time (`GetLastInputInfo`);
- Docker's state, and the memory and CPUs of the desktops it hosts.

**What a job asks for.** A profile per kind, configuration and compiler count. The first values come from this
week's measurements; the median peak of the same kind's last 10 runs then replaces each one.

| Job | Commit | Physical | CPU | Disk on the worker | Measured from |
|---|---|---|---|---|---|
| `compile-check` | 3 GB per compiler | 2 GB per compiler | 1 per compiler | — | the Debug PCH is 1.8 GB (10-24) |
| `m5` | 4 GB | 3 GB | 1 | 1 GB | `/MP1`: 4 min, lowest free 1.5–2 GB (10-18) |
| `build` Debug, n compilers | 3 + 3n GB | 2 + 2n GB | n + 1 | tree ~25 GB, layout 0.7 GB | 3 compilers: 22.9 → 14.2 GB free (10-21) |
| `build` Release, n compilers | 4 + 3.5n GB | 3 + 2.5n GB | n + 1 | tree ~30 GB, layout 1.1 GB (966 MB PDBs) | 3 compilers: 34.1 → 26.3 GB (10-24); 2 compilers: 5.6 → 1.1 GB in 27 s (10-11) |
| `e2e` / `install-lane` | desktop RAM + 1 GB | the same | desktop CPUs | the desktop's overlay on C: (a few GB) | 8 GB / 6 CPUs default; Docker may hold 31 GiB |

**The admission rule** (per worker, at each wake). A job starts when all of these hold:

```text
free_commit       − reserve_commit ≥ job.commit      (homepc reserve: 12 GB, the owner's knob)
available_physical − reserve_phys  ≥ job.physical   (homepc reserve: 6 GB)
cores_free                         ≥ job.cpu        (after the policy's CPU cap)
disk_free(Q: and C:) − reserve     ≥ job.disk        (evicting the store's least recently used items first, §4.6)
its locks are free, its approval is given, and no hold applies (full-screen app, quiet hours, a manual pause)
```

- The compiler count follows the room. With room for three compilers, a build gets three; with less, fewer. One
  compiler is the floor.
- The 10-24 build that took 1,239 s at one compiler would have waited instead of being stopped four times. When it
  started, it would have run at the count the room allows.

**The enforcement**, on the agent, at the job's start:
- Every process of the job runs in **one Job Object** with these limits:
  - `JOB_OBJECT_LIMIT_JOB_MEMORY` at the admitted commit plus 25 %. An underestimate fails the build with an
    out-of-memory error in that build; it never takes the PC's commit;
  - `JOB_OBJECT_LIMIT_PRIORITY_CLASS` below normal (idle for checks);
  - a CPU rate cap (the policy: 75 % of the PC's cores, for example);
  - `KILL_ON_JOB_CLOSE`.
  - MSBuild runs without node reuse, so no worker node survives the build. The job's environment sets
    `MSBUILDDISABLENODEREUSE=1`, which works with every commit's `Build-Agentmaster.ps1`, an old one included.
    Leftover nodes holding `obj` files caused the transient `C1083`/`D8040` failures of the build-install skill's
    §5.7.
- **The guard.** Free commit is read every 2 s. Twice under 2 GB, and the job's tree ends: only this job's, never
  another session's (the lesson of 2026-10-11). Under the reserve, no new job starts. A job ended by the guard
  returns to the queue with its next peak raised.
- **A full-screen game** (a D3D full-screen app, as above):
  - new jobs wait;
  - running builds drop to the idle priority class and keep their memory;
  - a desktop that is not yet in use is not created.
  - The owner can also pause the worker by hand from the page, Telegram or `amb pause`.
- **Disk.**
  - The store has a budget (40 GB on homepc, D7) with least-recently-used eviction of anything not pinned. Pinned:
    staged layouts, the current rollback targets and release candidates.
  - A job that needs room evicts first, or waits.
  - C: is watched for the Docker overlays.
- **dock.** Before it starts the VM, the coordinator reads `MemAvailable` through the PVE API
  (`/nodes/dock/status`). The VM starts only when it fits with 2 GiB to spare.

**Fairness.** Two priorities: `interactive` (a session waits for it) and `background`. First in, first out within
each. Deduplication (§4.4) first. A swap waits for the requesting session's turn to end (below).

### 4.6 Artifacts and records

- **PostgreSQL** holds:
  - workers, jobs, steps, events (every state change: an audit trail), approvals, the policy and the artifact index;
  - resource samples, one per job per 30 s;
  - each job's last 64 KB of log, and its full log compressed up to 20 MB, kept 30 days.
  - It also holds a **measured-runs** row for every swap: versions, drain, register, downtime, hosts, sessions cut
    off, outcome. The table CLAUDE.md keeps by hand can be generated from it.
- **Bulk artifacts on homepc** (`Q:\buildserver\store\`):
  - layouts (Release 1.1 GB, Debug 0.7 GB), portable zips, e2e results, full logs.
  - Each is keyed by kind, commit, configuration, identity and options, with its SHA-256, size, pin and expiry.
- **A worker other than homepc** sends what outlives the job to homepc's store before it may stop: layouts, zips,
  failure evidence. A layout without its PDBs is ~130 MB; with them, 1.1 GB.
- **GitHub** keeps what it keeps today: release assets and Actions artifacts.

### 4.7 Life cycle, failure handling, stability

- **States.**

  ```text
  queued → waiting-approval → waiting-resources → assigned → preparing → running → collecting
                                                           ↘ succeeded | failed | cancelled | lost
  ```

  Every move is an event row. A submission carries an idempotency key, so a resent request never runs twice.
- **Leases.** An assigned job holds a lease, renewed by the agent's heartbeat every 30 s. Missed for 90 s, the job is
  `lost`, the coordinator's clock having woken it.
  - The agent fences itself: if it cannot renew for longer than the lease, it ends the job's tree, except during a
    swap's protected section (below).
- **Steps that change an instance run detached.**
  - The swap step runs in its own process, outside the job's kill-on-close Job Object, writing a transcript and a
    state file.
  - A crash or restart of the agent never ends a swap halfway. The agent re-attaches to it from the state file.
- **The swap, made safe.** The swap step runs `Swap-AgentmasterLayout.ps1` with three fixes, which the two failures of
  this week motivate. On 2026-10-24 another session was already making them in `tools\` (with an optional
  `-ReplaceInstalledPackage` and `-RollbackBundle` for D9), uncommitted while this was written. The step takes that
  version once it is in; whatever is still missing is phase 2's:
  1. **Step 1 refuses an installed package.** If `(Get-AppxPackage <name>).IsDevelopmentMode` is false, the step stops
     before anything else happens. That is the 10-24 case: Windows refuses any loose layout over it with
     `0x80073CFB`.
  2. **Any failure after the stop relaunches the still-registered version**, not only a failed register: a
     `try`/`finally` around steps 3–5. That is the 10-21 case: 12 min down.
  3. **The drain's process census survives a transient CIM failure.** It retries the census instead of throwing.
  - **Before the stop**, it waits for the requesting session's turn to end, as `prodswapR.ps1` does, and lists the
    turns it would cut off. Cutting them off follows the owner's standing choice.
  - **After the start**, it watches hooks.log from the recorded offset: the `[rehome]` lines, `[ui-stall] watchdog
    armed`, `[msg] start-up hold ended`, trouble lines, a dead pid or a WER event.
  - **If the outcome is bad**, it relaunches or rolls back as the policy says, and notifies at once.
- **The agent is safe without the coordinator.** Every guard is local. At start-up it ends job trees left by a
  crash and cleans its temp folders.
  - **Offline mode.** When `build.local` does not answer (the homelab was down from 2026-10-04 17:45 to 10-05 13:31),
    `amb --local` talks to the agent directly. The agent keeps a small local queue, takes the `build-launch` mutex
    for anything that touches an instance or a slot, and uploads the records when the coordinator returns.
  - **Building on the PC never depends on the homelab.**
- **The coordinator is safe without the agents.** It is stateless; a restart loses nothing. A deploy rolls the
  fastrun revision; `fastrun rollback buildserver` undoes it in ~4 ms.
- **Coexistence with hand builds.**
  - Hand builds of the stock tree still work. The governor sees their memory and holds its jobs.
  - Staging into a slot and swapping take the same `build-launch` mutex as the hand flows.
- **Purge decisions are the coordinator's.** For each worktree it keeps the last built commit and the last purge.
  - **Purge triggers:** the skill's rule (`SessionModels.h`, `SessionRegistry.*`, any `.idl`, widened to the control
    and the settings editor for a TerminalControl `.idl`), plus new data members in a TerminalApp header (a diff
    check). A purge also follows `--force-purge`, and an incremental link error on a symbol the source defines (a
    purge and one retry).
  - **Interrupted builds:** a build that stopped after its purge resumes without purging again (`-PurgedAt`).
  - **Version upgrades:** an agent and a coordinator of different protocol versions refuse each other with a clear
    message. The agent updates itself from the store when no job runs (phase 4).

### 4.8 Approvals and permissions

The repository's rule is "Do not build or deploy or install without the user's permission". The coordinator enforces
it with a policy the owner sets once, instead of a question in every conversation:

| Grant | Meaning | Proposed default |
|---|---|---|
| `auto` | runs without asking | `compile-check`, `lib-check`, `m5`, `zip`, `ci` |
| `once-per-session` | the first request of a session asks; later ones of the same kind run | none at first |
| `ask` | every request asks | `build`, `stage`, `e2e`, `install-lane` |
| `ask-every-step` | every gate asks, and the answer is valid for that job only | `swap` (dev and production), `release` |

- **Where approvals happen:**
  - a Telegram message with buttons (the owner's id only), from a bot of its own (D3);
  - the page, with the owner's token;
  - `amb approve <job>` from the owner's shell.
- **What a request names:** the job, the commit with its subject, the requesting session and title, what it will
  stop or cut off, and its resource estimate.
- **Expiry:** an approval left unanswered for 24 h expires, and the job is cancelled.

### 4.9 Interfaces

- **`amb`, the command line.** Plain output for people, `--json` for programs.
  - It waits without polling: it streams the job's events, and `--wait-max 270` exits **97** when still running,
    like `e2e_run.py`. Exit codes: 0 success, 1 failed, 97 still running, 98 the infrastructure failed.

  ```text
  amb build debug                      # HEAD of the current repository, into the store
  amb build release --at 526a55657 --wait
  amb build debug --snapshot           # the working tree as a snapshot commit (the user's explicit choice)
  amb check                            # compile-check of what changed since the last build
  amb test m5
  amb deploy dev                       # stage → approval → swap; waits for this session's turn to end first
  amb e2e --build <job> -- -m p0       # pytest arguments after --
  amb status | amb jobs | amb logs <job> -f | amb cancel <job> | amb pause homepc | amb resume homepc
  amb --local build debug              # straight to the agent, when build.local does not answer
  ```

- **The page at `https://build.local`:**
  - the queue;
  - each worker with live bars (free commit, physical memory, CPU, disk, desktops) and the full-screen hold;
  - a job's page with its steps, live log, resource curve and artifacts;
  - approvals, the policy, and the measured-runs history.
- **Telegram:** results (succeeded, failed with the first error line, ready to swap) and approvals.
  - A bot of its own, because a bot's updates go to one poller, and `@balash_fastrun_bot` belongs to
    `fastrun-panel`.
  - The fastrun panel can show the `buildserver` release like any other.
- **For Claude sessions:**
  - a `buildserver` skill, which replaces the scratchpad scripts in the `build-install` skill;
  - `--json` everywhere;
  - waits that fit `run_in_background` and the prompt cache's 270 s.
- **The API:**
  - REST under `/api/v1` with an OpenAPI document;
  - server-sent events for logs and job events;
  - one contract for the CLI, the page, the agents and the tick (§8).

### 4.10 Security

- **Network.** LAN only. fastrun's ports are LAN-only; the agents' ports are firewalled to the fastrun workers and
  loopback; the page is behind Caddy's LAN certificate.
- **Tokens.** One per worker agent, one for the owner, one per caller kind (sessions, the tick). Each lives in the
  k3s Secret `fastrun/buildserver` (labelled for release mounting, its `resourceVersion` in the spec) and in
  `Q:\services\buildserver\secrets\` with an ACL, written through stdin and never printed.
- **The PVE token.** A PVE user `buildserver@pve` whose role holds `VM.PowerMgmt` and `VM.Audit` on the build VM alone.
- **GitHub stays on the PC.** The `gh` login runs on the homepc agent. The coordinator never pushes on its own: the
  GitHub lane builds only commits the owner has pushed (the repository is public).
- **Approvals** come only from the owner's Telegram id or the owner's token.

## 5. The dock worker

**What a build VM needs:**
- 12–16 GiB of RAM: Release builds at two or three compilers, plus Windows.
- 8 vCPU.
- ~150 GB of disk: Windows and VS Build Tools (~60 GB), two warm worktrees (~55 GB), a small store and temp space.

**What dock has today:**
- **RAM:** 6.2 GiB available. VM 100 holds 16 GiB, and the k3s nodes, PBS and the small containers hold the rest.
- **Disk:** a thin pool at 71 % used and oversubscribed. 150 GB more would cross its 80 % alarm (`dock-pool-watch`).

**The owner's options (D5):**

| Option | What it takes | Gives | Against |
|---|---|---|---|
| **A. More RAM, and a drive** (recommended) | 2 × 32 GB DDR5 SO-DIMM (the board's maximum is 64 GB, read with `dmidecode`). A drive for the VM's data if the box has room for a second one (to check), otherwise reclaim the thin pool's old snapshots first | a real second build machine, on demand, with 16 GiB for builds and room left over | a purchase; heat: dock stopped at "CPU Over Temperature" after the 10-04 power cut, so sustained builds need its temperature watched and a CPU cap |
| **B. Lend VM 100** | the agent beside TraderLab's on `win11-1`, one heavy job at a time between the two; a data disk | 8 vCPU / 16 GiB today, no purchase | competes with TraderLab, which the owner gave priority; the disk problem remains; its clock runs 4.6 % slow (homelab §4), which build timestamps may notice |
| **C. No dock worker** | the GitHub lane (D6) as the second machine | off-site clean Release builds at no cost to the PC | pushed commits only; ~25 min cold; no warm trees |

**The on-demand life cycle** (A or B):
1. A job for dock is admitted, and dock's `MemAvailable` fits the VM.
2. The coordinator starts the VM through the PVE API; it boots in ~1–2 min (to measure).
3. The agent service starts and calls in.
4. It fetches the commit's bundle from homepc and builds in its warm worktree, which keeps its disk between boots.
5. It sends the artifacts to homepc's store.
6. After 15 min with no job, it shuts the VM down through the guest agent.

The VM stays off the fleet's backups (`backup=0` on its data disk), as PBS's own interim datastore does.

## 6. e2e

- **Now: on homepc, managed.**
  - The `e2e` and `install-lane` jobs wrap `e2e/host/e2e_run.py` and `install_lane.py`, unchanged. The suite's
    design stays its own (`doc/agentmaster/E2E_TESTING.md`).
  - The job owns the desktop's life:
    1. a desktop slot (8 GB, 6 CPUs) is admitted against the PC's room (§4.5); one slot by default (D8);
    2. Docker Desktop is started if needed (it does not start at sign-in);
    3. the desktop is created or reset, the build's zip is staged, the suite is provisioned and run;
    4. the results go to the store (they go to `Q:\agentmaster-e2e\runs\` today);
    5. the desktop is **deleted in every outcome**, success, failure and cancel included.
  - A **warm pool** (D8) can keep one provisioned desktop for up to 20 min while more e2e jobs are queued.
  - **Stopping Docker Desktop** when no e2e job remains is a policy. Reclaiming WSL's memory needs a `.wslconfig`,
    and there is none today; both are D8.
- **The plans of `E2E_RELIABILITY.md` / `E2E_CRASH_GUARD.md`** (PLAN ONLY) run inside the job when they are built.
  The build server adds no second guard.
- **Later: on dock.** With option A of §5, the install lane's "fresh desktop per scenario" maps onto Proxmox
  snapshots: a golden e2e VM, `qm rollback` in seconds, instead of a 35-s to 10-min reset on the PC. That takes the
  heaviest load off the PC (phase 6).

## 7. Releases

`amb release 0.8.1 [--prerelease | --nightly]` runs the `release-version` skill's pipeline with gates:

1. **The version is the owner's.** The first approval names it.
2. **Pre-validation is a clean Release build of the exact commit.** Either `ci.yml`'s run of that commit on GitHub
   (when pushed: a clean build at no cost to the PC), or a `release-clean` build on a worker.
3. **The tag**, pushed by the homepc agent with the user's `gh` login, starts `release.yml`. The job watches it and
   checks the six assets: the bundle, the certificate, two portable zips with public PDBs, two symbols zips.
4. **The notes.** The job assembles the commit list since the right baseline (the `release-notes` skill's rules). The
   prose stays a session's or the owner's.
5. **Publishing is the owner's.** The second approval runs `gh release edit --draft=false`.
6. **Production's update** follows production's install kind (D9): the in-app updater for the MSIX, a `swap` for a
   loose layout.

## 8. Data model and API (sketch)

- **Tables:**
  - `workers` (id, kind, capabilities, policy, last report, state);
  - `jobs` (id, kind, commit, snapshot, config, options, priority, requester, dedupe key, state, worker, lease, timings);
  - `steps`, and `events` (job, at, from → to, detail);
  - `approvals` (job, gate, asked, answered by, answer, expiry);
  - `artifacts` (key, kind, commit, worker, path, size, SHA-256, pinned, expiry);
  - `samples` (job, at, commit, physical, cpu);
  - `logs` (job, last 64 KB, compressed body);
  - `trees` (worker, config, last built commit, last purge commit);
  - `swap_runs` (the measured-runs record);
  - `policy` (grants, reserves, holds, budgets).
- **API v1:**
  - jobs: `POST /jobs`, `GET /jobs?state=`, `GET /jobs/{id}`, `GET /jobs/{id}/events` (SSE),
    `GET /jobs/{id}/log` (SSE or range), `POST /jobs/{id}:cancel`;
  - approvals: `POST /approvals/{id}:answer`;
  - workers: `GET /workers`, `POST /workers/{id}:pause|resume`;
  - artifacts: `GET /artifacts?commit=`;
  - policy: `GET|PUT /policy`.
  - **Agent-facing:** `POST /workers/{id}/report`, `POST /jobs/{id}/heartbeat`, `POST /jobs/{id}/log`,
    `POST /jobs/{id}/complete`.
  - **Coordinator → agent:** `POST /v1/jobs/{id}/start`, `DELETE /v1/jobs/{id}`, `GET /v1/capacity`,
    `GET /v1/bundles/{from}..{to}` (homepc), `GET /v1/artifacts/{key}`.
  - **Internal:** `POST /internal/tick` (the tick's token only).

## 9. What goes where in `buildserver/`

| Folder | What |
|---|---|
| `DESIGN.md` | this file |
| `control/` | the coordinator: .NET 10 minimal API, the page (static files), the scheduler, the Dockerfile |
| `agent/` | `amb.exe`: the agent and the command line; Job Objects, the governor, the guard, the git mirror and worktrees, the step runner |
| `shared/` | the API contract (one source for the coordinator, the agent and the CLI), protocol version |
| `steps/` | the PowerShell steps that are deploy tooling, versioned here instead of in session scratchpads: stage, gate, swap (with the three fixes), post-swap watch, zip, desktop life cycle |
| `tick/` | `buildserver-tick` and `buildserver-audit`: Python zip apps (the panel's pattern) |
| `deploy/` | `deploy.ps1` (`-Stage Test`, `Production`, `RemoveTest`: the `llm-chat` pattern), the k8s objects (the ServiceAccount, Role and ConfigMap of the clock, the database's role), `make-secrets.py` (values on stdin, never printed), the Caddy block and DNS record as notes for the homelab repository, `install-agent.ps1` (the task, the firewall rule, the folders, the ACLs) |
| `tests/` | the scheduler's admission rules, the purge rules and the state machine as unit tests; a conformance run against `buildserver-next` |

## 10. Plan

Each phase ends with checks that a run proves. Each needs the owner's go to build and deploy (R7).

| Phase | Delivers | Acceptance (all measured, none assumed) |
|---|---|---|
| **P0 — decisions** | the owner's answers to §11 | — |
| **P1 — coordinator + PC builds** (largest) | `buildserver` on fastrun with its database, tick and audit; the homepc agent with Job Objects, the governor, the guard, the mirror and worktrees; jobs `compile-check`, `lib-check`, `m5`, `build`, `zip`; `amb`; a read-only page | (1) A Debug build of HEAD finishes with free commit never under the reserve, its Job Object peak recorded. (2) Two sessions that ask for the same build get one job. (3) A build asked while memory is short waits and starts by itself when memory returns, with no caller polling. (4) With `build.local` down, `amb --local build debug` works. (5) Ending the agent mid-build leaves no `MSBuild`, `cl` or `link` process. (6) A write audit of the job shows nothing written to K:. (7) An idle hour costs 60 tick runs and one app start (the hourly audit). |
| **P2 — stage, swap, approvals** | `stage`, `swap` (with the three fixes), `deploy dev`/`deploy prod`, the post-swap watch, swap records; Telegram and the page for approvals | (1) A dev deploy from one command and one approval. (2) A swap against an installed package refuses before any stop. (3) A failure forced between the stop and the register relaunches the old version. (4) An agent restart during a swap neither ends the swap nor loses its record. (5) The measured-runs rows of P2's swaps generate. |
| **P3 — e2e on demand** | `e2e`, `install-lane`; the desktop slot, Docker on demand, teardown in every outcome | (1) A full suite from `amb e2e --build <job>`. (2) No desktop left after a success, a failure and a cancel. (3) A second e2e job waits for the slot. (4) A full-screen game holds a queued e2e job. |
| **P4 — GitHub lane + releases** | `ci`, `release` with its gates; the agent's self-update | (1) A pushed commit's clean build through `ci`. (2) A prerelease cut end to end with two approvals. |
| **P5 — dock worker** (after D5) | `win-build` from template 101, the PVE token, on-demand power, git bundles, artifact transfer, temperature watch | (1) A Release build on dock while the PC's commit use stays flat. (2) The VM off within 15 min of its last job. (3) A start refused while dock lacks memory, then made when it has it. |
| **P6 — e2e on dock** (optional, after D5 A) | a golden e2e VM with snapshot resets | an install-lane scenario reset in seconds |

## 11. Decisions for the owner

| # | Decision | Recommendation |
|---|---|---|
| D1 | The coordinator's records: the idle `default/postgresql` (a new database and role), or a database of its own | the idle one; it has held nothing for two years |
| D2 | Port and name | activator 2107 (re-checked at deploy), `https://build.local`; `build.sysd.cc` later |
| D3 | Telegram: a new bot for build approvals and results (`@BotFather`), or the page only | a new bot; approvals from the phone are the friendly path |
| D4 | The standing grants (§4.8) | the proposed defaults: checks and tests `auto`; builds, stages and e2e `ask`; swaps and releases `ask-every-step` |
| D5 | The dock worker: A (64 GB of RAM, a drive), B (lend VM 100) or C (none; the GitHub lane) | A for the long run; C until then |
| D6 | The GitHub lane for pushed commits | yes; the server never pushes on its own |
| D7 | Disk on Q:: the store's budget; when to retire the stock build trees `Q:\tmp\agentmaster-*` (52 GB) | 40 GB for the store; retire the stock trees once hand builds stop |
| D8 | e2e on the PC: desktops at once, their size, a warm pool, Docker Desktop on demand, a `.wslconfig` (memory cap, `autoMemoryReclaim`) | one desktop at a time, 8 GB / 6 CPUs, warm pool 20 min, Docker started on demand and stopped after 30 min idle; the `.wslconfig` is a separate change to try first by hand |
| D9 | Production's install kind: stay on the v0.8.0 MSIX (updates by release), or return to loose layouts (needs `Remove-AppxPackage` first, then swaps work again) | the owner's; until it is decided, `swap prod` refuses with the reason |
| D10 | The three swap-script fixes (§4.7), wherever they are not in `tools\` yet by P2 | yes |
| D11 | The stack: .NET 10 for the coordinator, the agent and the CLI; Python for the tick | yes |
| D12 | The holds: the full-screen detection, quiet hours, the reserves (12 GB commit, 6 GB physical) | full-screen hold on; no quiet hours; those reserves |

## 12. Risks and open questions

| Risk or question | Mitigation or how to settle it |
|---|---|
| fastrund's pod has one 1 GiB memory limit for everything | measure the app's footprint in P1; `--warm none`; one instance per node |
| PostgreSQL's 192 MiB / 0.15 CPU limits | records only; bulk logs on the workers; raise its limits if needed (a homelab change) |
| k3s-node-1, which holds the database, goes away | offline mode on the PC; the coordinator answers 503, and the agents keep their guards |
| The PC's Wi-Fi link for artifact transfers from dock (0.1–1.1 GB) | send layouts without PDBs unless asked; compress |
| Purge heuristics miss a layout change | the automatic purge-and-retry on a link error; `--force-purge`; the post-swap watch catches a crash at start |
| dock's heat under sustained builds | a CPU cap on the VM; read the temperature before a start (the homelab's sensors); stop at a limit |
| An agent and a hand build compete for memory | the governor counts all of the PC's memory, whoever holds it |
| The VM's boot time and Windows activation (template 101) | measure in P5; the owner's licence |
| UI tests on GitHub's hosted runners | not planned; would need a proof first |

## 13. Evidence: what was measured for this design (2026-10-24)

- **dock**, read-only over SSH:
  - `free -m`: 31,326 MiB total, 6,175 available;
  - `lscpu`, `qm list`, `pct list`;
  - `pvesm status`: local-lvm 71.44 %, 237 GB available;
  - `lsblk`: `sda` is a USB "One Touch Hub", NTFS "Seedbox";
  - `dmidecode -t memory`: 2 × 16 GB DDR5, 64 GB maximum.
- **k3s**, read-only:
  - `kubectl top nodes`: 14–17 % memory;
  - the PostgreSQL StatefulSet, Service, PVC and limits;
  - `fastrun releases`: 9 releases, activators 2101–2106 taken.
- **homepc:**
  - `Win32_OperatingSystem`: free physical and commit;
  - processes by private bytes;
  - no `.wslconfig`;
  - `du` of the layouts (Release 1.1 GB with 966 MB of PDBs, Debug 693 MB) and of the build trees (21 + 22 + 8.7 GB);
  - `git count-objects` (198 MB packed), the checkout (95 MB), `packages\` (894 MB), `Generated Files` (1.1 GB).
- **GitHub:** `gh run list` (CI 17–26 min, releases 26–30 min); the repository is public.
- **The homelab's records:** `K:\source\proxmox\CLAUDE.md` §1 (hosts), §3 (storage), §4 (VMs), §5 (containers),
  §6.3–6.4 (k3s, PostgreSQL, fastrun).
- **fastrun's limits and patterns:** the `fastrun` skill and its `references/kinds.md` (apps, schedules, limits),
  `api.md` (`notBefore` ≤ 10 s, scopes), and the `llm-bridge` worker pattern.
- **The swap failures** (CLAUDE.md, commit a05c0c2ff):
  - 10-24: `prodswapR.log` of session baa1b06d, and the AppXDeploymentServer events (ActivityId
    b0c268b3-57c2-001b-8a36-c5b2c257dd01);
  - 10-21: `prodswapR.log` of session 14fdd99b, and production's hooks.log after offset 587309909.
