# Agentmaster

> A fork of **Windows Terminal** (`microsoft/terminal`, MIT) that turns it into a manager
> for multiple **Claude Code** sessions.

## What we're doing & our aim

**Aim:** one Windows Terminal–based app that manages **N Claude Code sessions across M
working directories** — letting you **fully interact** with each session *and* giving the
app **100% programmatic control** (inject prompts, read output, drive state).

Each session is a real `claude.exe` on a **ConPTY** connection: a full-fidelity terminal
with a shared stdin (you and the orchestrator coexist), output tapped for the UI, and
semantic state taken from **Claude Code hooks** — never screen-scraping.

**Converged design:**
- **Design A — Native Graft:** the management "brain" lives *inside* the fork (C++/WinRT).
- **Manager tab (C1 "Linked Lenses"):** a pinned, leftmost, non-closable tab (tab 0, open
  by default), split into three selection-synced regions:
  - **Triage Board** (top) — sessions as cards in state columns (Running · Waiting-for-you
    · Needs-approval · Error).
  - **Explorer Tree** (bottom-left) — the M working directories → their N sessions.
  - **Flight Plan** (bottom-right) — a per-session prompt queue + **Autopilot**.
- **Flight Plan / Autopilot:** queue prompts; on **turn-complete** (`Stop` hook) the next
  prompt is auto-sent. Approvals and clarifying-questions are handled separately.

Deep plan: [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md).
Hooks bridge: [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md).

## Repo facts

- Forked from `microsoft/terminal` @ `v1.24.2372`; work branch **`agentmaster`**.
- Build entry: **`OpenConsole.slnx`** (slnx format). No git submodules in 1.24
  (`doc/building.md` is stale on that point).
- Toolchain: VS 2022 + C++/UWP workloads + Windows SDK 10.0.22621/26100.
- Our additions: `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}`,
  `src/cascadia/TerminalApp/AgentMaster/`, `doc/agentmaster/`, `tools/Build-Agentmaster.ps1`.

## Integration points (1.24 pluggable pane-content model)

- New tab content implements **`IPaneContent`** (like `ScratchpadContent` — no `.idl`).
- Dispatch by type string in `TerminalPage::_MakePane` (`TerminalPage.cpp:3815`).
- Tab placement: `_CreateNewTabFromPane(pane, insertPosition)` (`TabManagement.cpp:215`);
  track our tab like `_settingsTab`.
- Compiled in `src/cascadia/TerminalApp/TerminalAppLib.vcxproj`.

## Building FAST

This machine: **i9-13900K — 32 threads (8 P + 16 E cores), 64 GB RAM.** The stock
`Invoke-OpenConsoleBuild` is slow because it runs msbuild with **no `/m`** (projects build
serially → 31 threads idle), builds the **whole** solution (tests/tools/samples), and
re-runs `nuget restore` every call. Per-file `/MP` is already enabled
(`src/common.build.pre.props:145`).

1. **Build on NVMe, not the A400.** `K:` is a DRAM-less SATA SSD; a WT build is tens of
   thousands of tiny files and 32 threads thrash it. Prefer **`Q:` (Kingston Fury
   Renegade, Gen4 NVMe + DRAM, ~546 GB free)** or `C:` (Corsair MP600 PRO).

2. **Use the wrapper** — parallel `/m` + per-file `/MP`, scoped to just the app target
   (`Terminal\CascadiaPackage`), skips the redundant double restore on rebuilds:
   ```powershell
   pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1            # first build
   pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore # inner loop
   ```
   Raw equivalent:
   `msbuild OpenConsole.slnx /m /p:Configuration=Debug /p:Platform=x64 /t:Terminal\CascadiaPackage /v:m`
   64 GB handles unbounded `/m`; if it ever pages, add `-ClMpCount 6`.

3. **Windows Defender exclusions** (Admin, once — 20–40% on cold builds):
   ```powershell
   Add-MpPreference -ExclusionPath (Resolve-Path .)
   'MSBuild.exe','cl.exe','link.exe','cppwinrt.exe','midl.exe','mc.exe','VBCSCompiler.exe','nuget.exe','tracker.exe' |
     ForEach-Object { Add-MpPreference -ExclusionProcess $_ }
   ```

4. **Iterate incrementally.** The cold build (restore + cppwinrt projection) is the
   expensive one; afterwards `-NoRestore` rebuilds (our edits touch only `TerminalApp`)
   are quick thanks to MSBuild's up-to-date check.

5. **Optional — MSBuildCache** for clean-rebuild / branch-switch cache hits: add
   `-p:MsBuildCacheEnabled=true` (uses file copies, not hardlinks).

To run/debug after building: F5 on `CascadiaPackage` in VS, or deploy the built appx
layout (see `doc/building.md`).

## Correctness rules (do not regress)

1. **"Waiting-for-you" is three states.** Auto-send fires only on **turn-complete**
   (`Stop` hook). `Notification(permission)` → Approval Policy (NOT the prompt queue). A
   `Stop` whose last message is a question → **Held** by the question-guard.
2. **Tree `Enter` = Activate** (jump to the session's live tab); never forward it to
   `ITerminalConnection::WriteInput` (that would submit a stray carriage return).
3. **Bind queue → sessionId**, never "the selected session" at send time.
4. **Idempotent sends:** mark `Sent` atomically + persist; survive restart without replay.
5. **Backstops:** stop-on-error, maxAutoSends, global pause/kill, pause-on-human-input.

## Conventions

- Mark our additions with `Agentmaster`. Keep the upstream MIT `LICENSE`/`NOTICE`.
- Keep the diff against upstream minimal where practical (additive files, small touches at
  integration points) so rebasing onto `microsoft/terminal` stays cheap.
- Build artifacts (`bin/`, `packages/`, `Generated Files/`) are gitignored — never commit them.
