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

Full design: [`doc/agentmaster/DESIGN.md`](doc/agentmaster/DESIGN.md).
Milestones & build: [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md).
Hooks bridge: [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md).

## Status

- **M0–M4.1 ✅** — fork mapped; scaffold in; baseline + incremental builds green;
  `AgentManagerContent` wired into `_MakePane`; the **pinned, non-closable "Agent Manager"
  tab opens at index 0 on startup**; the app ships under its **own package identity**
  (`Agentmaster`, not `WindowsTerminalDev`) and is **deployed & verified running**.
- **M5 ✅** — the native session engine (`src/cascadia/TerminalApp/AgentMaster/`): a
  thread-safe **`SessionRegistry`** (single source of truth), a **`HooksBridge`** local
  named-pipe server, and a **`ClaudeSpawn`** recipe that launches `claude.exe` on a ConPTY
  with `--settings` hooks + `CCMGR_SESSION_ID`/`CCMGR_HOOK_PIPE`, plus a PowerShell
  forwarder. Hooks → wire line → registry → hook-driven `SessionState` (Correctness Rule
  #1). Wired into `TerminalPage` (`_InitAgentmasterEngine`, `_SpawnClaudeSession`) and a
  Launch button in the Manager tab. State transitions log to
  `%USERPROFILE%\.agentmaster\hooks.log` (the M6 Triage Board will render the registry).
  **67/67** standalone checks pass incl. a live pipe round-trip (`AgentMaster/tests/`).
- **M6 ✅** — the C1 "Linked Lenses" UI (`AgentManagerContent`): a Triage Board (state
  columns), an Explorer Tree (M dirs → N sessions), and a Flight Plan (per-session prompt
  queue + Autopilot mode), built imperatively and snapshot-driven from the registry
  (cross-thread refresh via `DispatcherQueue`). Bidirectional selection + directory scope;
  Explorer `Enter`=Activate / `Del`=kill (never injects — Rule #2); Flight Plan
  add/reorder/delete/Send-now. Compiles clean (lib); runtime check pending deploy.
- **M7 ✅** — the Autopilot scheduler (`AgentMaster/Scheduler`): a pure, fully unit-tested
  `DecideAdvance()` + a worker thread on the registry's advance seam. Turn-complete →
  auto-send next Pending (Full) / one-click confirm (SemiAuto) / Held by the question-guard
  (transient) / skipped for Manual gate; backstops: pause-on-human-input, maxAutoSends,
  stopOnError, global Pause-all; idempotent sends (atomic mark-Sent before inject). UI adds
  the per-session mode selector, a confirm banner, and a Pause-Autopilot toggle.
- **M8 ✅** — persistence + plan templates + apply-to-many (`AgentMaster/Json.h`,
  `AgentMaster/Persistence`): a dependency-free JSON value/parser/printer; sessions and
  named plan templates (de)serialize to JSON under `%USERPROFILE%\.agentmaster\` (restore
  preserves `Sent` — no replay). UI to save a session's queue as a template, apply it, or
  broadcast it to a whole directory; sessions autosave on every change.
- **Session restore ✅ (live):** on startup `TerminalPage::_RestoreClaudeSessions()`
  re-launches every persisted session with **`claude --resume <id>`** in its working dir —
  resuming the real conversation — and reloads its Flight Plan + autopilot. So closing and
  reopening returns to the same state. **`Kill`** is the explicit discard (removes it from
  the registry + `sessions.json`); closing the app or a tab without Kill keeps it for next
  launch. Verified live.
- **All milestones M0–M8 are complete, built, deployed, and verified running.** Engine
  passes **103/103** standalone checks (`AgentMaster/tests/`).
- Follow-ups (not blocking): feed `pauseOnHumanInput` from a TermControl input tap;
  bracketed-paste for true multi-line prompt bodies; a live buffer "peek" in the Flight
  Plan; remove a session from persistence when its tab is closed via the X (today only
  `Kill` discards); prevent splitting the Manager tab.
- Milestones are tracked in `doc/agentmaster/IMPLEMENTATION.md`.

## Repo facts

- Forked from `microsoft/terminal` @ `v1.24.2372`; work branch **`agentmaster`**.
- Build entry: **`OpenConsole.slnx`** (slnx format). No git submodules in 1.24
  (`doc/building.md` is stale on that point).
- Toolchain: VS 2022 + C++/UWP workloads + Windows SDK 10.0.22621/26100.
- **Package identity = `Agentmaster`** (PFN `Agentmaster_8wekyb3d8bbwe`), set in
  `src/cascadia/CascadiaPackage/Package-Dev.appxmanifest` (the Debug branding). It is
  deliberately **distinct from `WindowsTerminalDev`** so it coexists with real Windows
  Terminal. ⚠️ There is a **separate `K:\source\windowsterminal` checkout on this machine
  that owns the `WindowsTerminalDev` identity** — never reuse that identity here (see Gotchas).
- Our additions: `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}`,
  `src/cascadia/TerminalApp/AgentMaster/`, `Package-Dev.appxmanifest` (identity),
  `doc/agentmaster/`, `tools/Build-Agentmaster.ps1`.

## Integration points (1.24 pluggable pane-content model)

- New tab content implements **`IPaneContent`** (like `ScratchpadContent` — no `.idl`).
  Ours: `AgentManagerContent`, compiled via `src/cascadia/TerminalApp/TerminalAppLib.vcxproj`.
- Content dispatch by type string in `TerminalPage::_MakePane` (`TerminalPage.cpp`): we add
  an `else if (paneType == L"agentManager")` branch → `make_self<AgentManagerContent>()`.
- The Manager tab is opened by **`TerminalPage::_OpenAgentManagerTab()`**, called from
  `_OnFirstLayout` *before* startup terminal tabs, so it lands at index 0 (leftmost). It
  sets the tab's `CloseButtonVisibility = Never` (non-closable) and is tracked in the
  `_managerTab` member (nulled on close in `TabManagement.cpp`, mirroring `_settingsTab`).
- Tab placement primitive: `_CreateNewTabFromPane(pane, insertPosition)` (`TabManagement.cpp`).

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
   are quick thanks to MSBuild's up-to-date check. Reference incremental times on this box:
   first ~236s, code-change rebuilds ~165–290s.

5. **Optional — MSBuildCache** for clean-rebuild / branch-switch cache hits: add
   `-p:MsBuildCacheEnabled=true` (uses file copies, not hardlinks).

## Deploy & run

A packaged app can't be launched by running `WindowsTerminal.exe` directly (WT #926/#4043);
it must be deployed. Deploy the **loose layout** (what VS F5 does) — no signing/cert/admin:

```powershell
# one-time per machine (or after the manifest changes): register the loose layout
Add-AppxPackage -Register "K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion
```

Launch any of these ways:
- execution alias: **`agentmaster`**
- Start menu: **“Agentmaster”**
- `Start-Process "shell:appsFolder\Agentmaster_8wekyb3d8bbwe!App"`

**Fast inner loop** (the loose layout is live, so binaries update in place):
```powershell
pwsh -File .\tools\Build-Agentmaster.ps1 -NoRestore   # rebuild
agentmaster                                            # relaunch — NO re-register needed
```
Re-register **only** when `Package-Dev.appxmanifest` changes. (VS F5 on `CascadiaPackage`
also works and handles deploy.)

## Gotchas (learned the hard way)

- **`nuget.exe` can't parse `.slnx`.** The bundled `dep\nuget\nuget.exe` errors with
  "file type was not recognized" on `OpenConsole.slnx`, and a bare `packages.config`
  restore needs `-PackagesDirectory`. The wrapper restores `dep\nuget\packages.config`
  into `packages\` and is non-fatal; use `-NoRestore` once packages exist. (Stock
  `Invoke-OpenConsoleBuild` has the same latent bug — it only "works" because VS already
  restored.)
- **`Grid`/`Panel` has no `Focus(FocusState)`** in this XAML projection — only
  `Control`-derived types do. `IPaneContent::Focus` must focus a `Control` child
  (`ScratchpadContent` focuses its `TextBox`); the Manager's `Focus()` is a no-op until
  M6 gives it a real focusable control. (This caused error C2039.)
- **Never reuse the `WindowsTerminalDev` package identity.** It belongs to the separate
  `K:\source\windowsterminal` checkout; registering the same identity tries to *replace*
  it and fails with a file-in-use lock (`0x80073CF6 / 0x80070020`) when its
  `OpenConsoleProxy.dll` is loaded. Our distinct `Agentmaster` identity sidesteps this.
- **Don't `taskkill`** to clear deploy locks, and never touch the running **Store**
  Windows Terminal (that's the live session). Pause and ask instead. (When closing *our*
  dev instance to relink, filter by `ExecutablePath -like 'K:\source\Agentmaster\*'` so the
  Store WT is spared.)
- **MSIX virtualizes a packaged app's `%LOCALAPPDATA%`** to the package LocalCache, but the
  spawned **`claude.exe` is external** and resolves paths against the real filesystem. So
  the hooks files + `--settings` path **must** live somewhere un-virtualized that both
  agree on — Agentmaster uses **`%USERPROFILE%\.agentmaster`** (`AgentmasterStateDir()`).
  Using `%LOCALAPPDATA%` here silently breaks hooks for spawned sessions (the app writes to
  LocalCache; Claude reads the empty real path). Verified live: with the fix, a spawned
  session's `SessionStart`/`UserPromptSubmit` reach the registry (`~/.agentmaster/hooks.log`).

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
