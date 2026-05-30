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

**All milestones M0–M8 + session restore are complete, built, deployed under the
`Agentmaster` identity, and verified running.** The engine passes **211/211** standalone
checks (`AgentMaster/tests/`), and the full pipeline has been exercised end-to-end in the
deployed package: Launch → real `claude.exe` on a ConPTY → `--settings` hooks → PowerShell
forwarder → named pipe → registry → state machine → UI, plus `claude --resume` restore on
reopen (traces in `~/.agentmaster/hooks.log`).

What works, by area:
- **Engine (M5, `AgentMaster/`).** Thread-safe `SessionRegistry` (single source of truth;
  multiple observers), `HooksBridge` (local named-pipe server `\\.\pipe\agentmaster.<pid>`),
  `ClaudeSpawn` (spawn/`--resume` recipe + the shared hooks config + PowerShell forwarder).
  Hooks → wire line → registry → hook-driven `SessionState` (Correctness Rule #1). The wire
  line carries an 8th, **escaped `prompt`** field on `UserPromptSubmit` (`WireEscape`/
  `WireUnescape`: `\ \t \r \n`), so the registry records **every** message a session got — a
  prompt typed straight into the ConPTY becomes a `Sent`/`Typed` Flight-Plan entry, while the
  `UserPromptSubmit` echo of a prompt WE injected is recognized (text + a recency window + the
  transient `QueuedPrompt::echoed` flag) and NOT double-recorded.
- **C1 UI (M6, `AgentManagerContent`).** Triage Board + Explorer Tree + Flight Plan,
  imperative and snapshot-driven from the registry (cross-thread refresh via
  `DispatcherQueue`), bidirectional selection + directory scope. The Board/Tree show only
  **OPEN** (`live`) sessions; closed ones are **ARCHIVED** (shut down, restorable) and listed
  behind an **Archived (N)** toolbar button (by the cog) — a modal list with per-row
  **Restore** + **Restore all** (resume via `claude --resume`). Explorer `Enter`=Activate /
  `Del`=archive (never injects — Rule #2). Flight Plan: compose box, Add / Send now / ↑↓ /
  Delete / Focus / Archive — and it reflects **all** messages a session got, not just queued
  ones: a chronological **SENT** summary (each row tagged **flight** = we queued+injected it
  vs **typed** = you typed it into the terminal) over the **UPCOMING** queue (Pending/Held).
  `Focus()` focuses the cwd `TextBox`. The Launch cwd box has a
  focus-triggered **path-picker drop-down** (`Primitives::Popup`): up to 5 recent dirs (the
  current one excluded) over the subfolders of the current path + a `..` up-nav; click a row
  to navigate, and it re-lists. Working dirs are grouped/scoped with a filesystem-aware
  `PathEq`, so case-variant spellings collapse to one Explorer Tree root (Rule #8).
- **Autopilot (M7, `Scheduler`).** Pure `DecideAdvance()` + a worker thread on the registry
  advance seam. Turn-complete → auto-send next Pending (Full) / one-click confirm (SemiAuto)
  / Held by the question-guard (transient) / skip Manual gate. Backstops: pause-on-human-
  input, maxAutoSends, stopOnError, global Pause-all. Idempotent (atomic mark-Sent before
  inject). Advances fire on **two** triggers: the `Stop` seam (turn-complete) AND observed
  changes (`OnObserved`), so a session sitting **Idle** (freshly launched / just `--resume`d —
  it never emits a `Stop`) with a queued plan + autopilot **starts** consuming instead of
  waiting forever. `DecideAdvance` treats `Idle` as *ready* alongside `WaitingForInput`; a
  time-bounded **pickup guard** (the `echoed` flag + `kPickupGuardMs`) holds the next send until
  the just-injected prompt is picked up, so a change-driven advance never drains the queue (one
  prompt per turn). `RequestAdvance` dedups; a failed inject (no injector bound yet, e.g.
  mid-restore) rolls the prompt back to `Pending` rather than stranding a phantom `Sent`.
- **Persistence + archive/restore (M8, `Json.h`/`Persistence`).** Sessions + named plan
  templates + the path-picker's recent-dirs MRU (de)serialize to JSON under
  `%USERPROFILE%\.agentmaster\`; sessions autosave on change. **Lifecycle = Open ⇄ Archived**
  (the transient `SessionInfo::live` flag, never persisted): Open == has a live tab/claude this
  run (on the Board); Archived == shut down but kept restorable (behind the Archived button).
  On startup `_RestoreClaudeSessions()` loads each saved session into the registry as
  **Archived** and does **NOT** auto-launch it (Rule #6) — the app opens to just the Manager
  tab; the prior fleet comes back via **Restore** / **Restore all**. Closing a session's tab
  (the X, the tree `Del`, the Manager's Delete/Archive, or the Flight-Plan **Archive** button)
  all route through the ONE archive seam (`_HandleCloseTabRequested`→`_ArchiveAndCloseClaudeTab`):
  a single consequence confirm (gated by `confirmBeforeKill`), then `live=false` + clear injector
  + persist + close the tab — the record is **kept**, so it lists under Archived. **There is no
  discard**: archive is terminal, and the Claude transcript on disk is never touched. Restore
  re-launches in the working dir + reloads the Flight Plan + autopilot; resume is
  **transcript-gated**: `claude --resume <id>` only when Claude actually has a conversation for
  that id, otherwise a **fresh** session (new id, same dir + queue) — and the stale archived
  record is dropped. A never-prompted session has no transcript and a blind `--resume` would die
  with "No conversation found" (Rule #6). `Sent` prompts are never replayed. Templates: save a
  session's queue, apply it, or broadcast to a whole directory.
- **Settings cog (`AppSettings`, `settings.json`).** A `⚙` after "Pause Autopilot" opens a
  global-settings surface — an **in-content modal overlay** (a dimmed `Grid` over `_root`),
  NOT a `ContentDialog` (a text box inside one gets no keypresses in XAML Islands — see
  Gotchas). Exposes **Claude-session** config — `skipPermissions` (the spawn's
  `--dangerously-skip-permissions`), `model` (== `/model <v>`), `includeCoAuthoredBy`, and a
  global **`env`** (a `;`-delimited `NAME=VALUE` list applied to every session via
  `ParseEnvAssignments`→`spec.env`, `CCMGR_*` filtered) — plus **Autopilot defaults** stamped
  onto NEW sessions (mode / maxAutoSends / stopOnError / pauseOnHumanInput) and **behavior**
  (`confirmBeforeKill` — relabeled "Confirm before archiving" — routes the archive action
  (tab X / Manager Archive / tree `Del`) through the confirm dialog;
  `defaultLaunchDir` seeds the cwd box). Loaded at engine init, seeded via `SetSettings`,
  persisted + re-materialized on Save via `SetSettingsHandler`. Every default reproduces prior
  behavior, so a missing `settings.json` (or any unset field) is a no-op.

Follow-ups (not blocking): feed `pauseOnHumanInput` from a TermControl input tap;
bracketed-paste for true multi-line prompt bodies; a live buffer "peek" in the Flight Plan;
**Restore all** re-opens tabs lazily (a non-foreground restored tab starts its `claude` only
when first focused — WT's lazy-background-tab behavior; restore one at a time to force start);
prevent splitting the Manager tab. Milestones tracked in `doc/agentmaster/IMPLEMENTATION.md`.

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
- Our additions (all marked `Agentmaster`):
  - `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}` — the Manager tab content (C1 UI).
  - `src/cascadia/TerminalApp/AgentMaster/` — the engine (plain C++, no WinRT; the `.cpp`
    are `<PrecompiledHeader>NotUsing`): `SessionModels.h`, `HookEvents.h`, `HookWire.h`,
    `SessionRegistry.{h,cpp}`, `HooksBridge.{h,cpp}`, `ClaudeSpawn.{h,cpp}`,
    `Scheduler.{h,cpp}`, `Json.h`, `Persistence.{h,cpp}`, and `tests/` (standalone harness,
    not in the msbuild — run `tests/run-m5-tests.bat`).
  - small touches in `TerminalPage.{h,cpp}` (engine wiring, spawn/restore) and
    `TabManagement.cpp`; registrations in `TerminalAppLib.vcxproj`.
  - `Package-Dev.appxmanifest` (identity), `doc/agentmaster/`, `tools/Build-Agentmaster.ps1`.
- **Runtime state dir: `%USERPROFILE%\.agentmaster\`** — `hooks-settings.json` +
  `agentmaster-hook.ps1` (the shared hooks config Claude is pointed at via `--settings`),
  `hooks.log` + `autopilot.log` (engine traces), `sessions.json` (persisted fleet),
  `templates.json` (saved plans), `recent-dirs.json` (path-picker MRU), `settings.json`
  (the Settings cog's `AppSettings`). Deliberately NOT under `%LOCALAPPDATA%` — see Gotchas
  (MSIX).

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
- **Engine wiring (`TerminalPage`):** `_InitAgentmasterEngine()` (from `_OnFirstLayout`,
  before the Manager tab) creates the `SessionRegistry` + `HooksBridge` + `Scheduler` and
  wires the registry's observer/advance seams. `_WireAgentManagerContent()` hands the
  content the registry + spawn/activate/kill/pause/confirm callbacks + the cog's
  settings seed/persist (`SetSettings`/`SetSettingsHandler`).
  `_LaunchClaudeSession(dir, title, restored)` builds a claude `ConptyConnection`
  (cmdline/cwd/env ours) and opens it as a normal terminal tab via `_MakePane(args, …,
  existingConnection)`; `_SpawnClaudeSession` = fresh, `_RestoreClaudeSessions()` = resume
  the persisted set. `sessionId → Tab` lives in `_claudeTabs` for Activate/Kill.
- **Shared stdin:** the registry holds a per-session injector bound to that session's
  `ConptyConnection::WriteInput`, so the user's keystrokes and the scheduler's prompts both
  reach the same `claude.exe` stdin (Correctness Rule #3 binds the injector to the id).

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

6. **Compile-check without relinking the exe.** `TerminalAppLib` is a **static lib**, so you
   can validate code changes (and catch all our compile errors) while the app is still
   running — build just the lib (after `vcvars64.bat`):
   ```
   msbuild src\cascadia\TerminalApp\TerminalAppLib.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=K:\source\Agentmaster\
   ```
   `/p:SolutionDir=` (trailing `\`) is **required** when building a `.vcxproj` directly —
   otherwise `$(SolutionDir)build\rules\*.targets` imports fail (MSB4019). The full exe link
   is `msbuild OpenConsole.slnx /m /p:Configuration=Debug /p:Platform=x64 /t:Terminal\CascadiaPackage`
   (~3–3.5 min on this box; SolutionDir is implicit for the `.slnx`).

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

**Inner loop.** The loose layout is live (binaries update in place), but you **cannot
relink `WindowsTerminal.exe` while the app is running** — it locks the exe. So: close *our*
dev instance (spare the Store WT), rebuild, relaunch. The user has **standing-authorized
this close→build→relaunch cycle** ("always auto deploy") — run it without prompting; just
never touch the Store WT (it's not under our path — see Gotchas).
```powershell
# 1. close ONLY our dev instance (path filter spares the Store WT — see Gotchas)
Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" |
  ? { $_.ExecutablePath -like 'K:\source\Agentmaster\*' } | % { Stop-Process -Id $_.ProcessId -Force }
# 2. build (full exe link)
pwsh -File .\tools\Build-Agentmaster.ps1 -NoRestore      # or: msbuild OpenConsole.slnx /t:Terminal\CascadiaPackage /m /p:Configuration=Debug /p:Platform=x64
# 3. relaunch
Start-Process "shell:appsFolder\Agentmaster_8wekyb3d8bbwe!App"   # or: agentmaster
```
Re-register **only** when `Package-Dev.appxmanifest` changes. (VS F5 on `CascadiaPackage`
also builds + deploys.) Runtime/session state lives in `%USERPROFILE%\.agentmaster\`; tail
`hooks.log` to confirm the engine is live (`[engine] bridge listening …`) and that spawned
sessions' hooks arrive (`[SessionStart]`, `[Stop]`, …).

## Gotchas (learned the hard way)

- **`nuget.exe` can't parse `.slnx`.** The bundled `dep\nuget\nuget.exe` errors with
  "file type was not recognized" on `OpenConsole.slnx`, and a bare `packages.config`
  restore needs `-PackagesDirectory`. The wrapper restores `dep\nuget\packages.config`
  into `packages\` and is non-fatal; use `-NoRestore` once packages exist. (Stock
  `Invoke-OpenConsoleBuild` has the same latent bug — it only "works" because VS already
  restored.)
- **`Grid`/`Panel` has no `Focus(FocusState)`** in this XAML projection — only
  `Control`-derived types do (this caused error C2039). `IPaneContent::Focus` must focus a
  `Control` child; `AgentManagerContent::Focus` focuses its cwd `TextBox`.
- **Building a `.vcxproj` directly needs `/p:SolutionDir=K:\source\Agentmaster\`** (trailing
  `\`), else `$(SolutionDir)build\rules\*.targets` imports fail with MSB4019. The `.slnx`
  build sets it implicitly. (See Building FAST #6.)
- **Imperative XAML name clashes:** a `using namespace winrt::Windows::UI;` pulls the nested
  `Text` namespace into scope and collides with a `Text(...)` helper (C2872/C2882) — prefer
  narrow `using`-declarations (`Color`/`ColorHelper`/`Colors`). The `.cpp` can't run-time
  test here, so the compiler is the safety net; build the lib (#6) after UI edits.
- **A text box inside a `ContentDialog` gets no keypresses in XAML Islands.** The dialog's
  PopupRoot sits outside our island's input path, so a hosted `TextBox`/`NumberBox` takes
  focus but receives no typing (first hit with the tree-rename box). So: use a **buttons-only**
  ContentDialog for confirms (`_OnDeleteSession`), and build anything that needs typing **into
  the main visual tree** instead — the in-place rename editor, and the Settings cog's
  **in-content modal overlay** (a dimmed `Grid` over `_root`, `RowSpan`-all; the card swallows
  taps via a handled `Tapped`, a backdrop tap = cancel). Don't reach for a ContentDialog when a
  field needs keyboard input.
- **Never reuse the `WindowsTerminalDev` package identity.** It belongs to the separate
  `K:\source\windowsterminal` checkout; registering the same identity tries to *replace*
  it and fails with a file-in-use lock (`0x80073CF6 / 0x80070020`) when its
  `OpenConsoleProxy.dll` is loaded. Our distinct `Agentmaster` identity sidesteps this.
- **Closing instances to relink.** Auto-closing **our** dev instance for the deploy inner
  loop is standing-authorized ("always auto deploy"): filter by
  `ExecutablePath -like 'K:\source\Agentmaster\*'` (matches our `WindowsTerminal.exe` *and*
  its `OpenConsole.exe` ConPTY hosts), `Stop-Process` them, build, relaunch — no prompt.
  But **never** touch the running **Store** Windows Terminal — it's the user's live session
  (under `Program Files\WindowsApps\…`, not our path). Never blanket-`taskkill` by image
  name; always path-filter so the Store WT is spared.
- **MSIX virtualizes a packaged app's `%LOCALAPPDATA%`** to the package LocalCache, but the
  spawned **`claude.exe` is external** and resolves paths against the real filesystem. So
  the hooks files + `--settings` path **must** live somewhere un-virtualized that both
  agree on — Agentmaster uses **`%USERPROFILE%\.agentmaster`** (`AgentmasterStateDir()`).
  Using `%LOCALAPPDATA%` here silently breaks hooks for spawned sessions (the app writes to
  LocalCache; Claude reads the empty real path). Verified live: with the fix, a spawned
  session's `SessionStart`/`UserPromptSubmit` reach the registry (`~/.agentmaster/hooks.log`).
- **`--dangerously-skip-permissions` also skips the startup "trust this folder" dialog.**
  Spawned sessions run with the flag by default (`AppSettings.skipPermissions`): besides
  auto-accepting tool prompts, permission mode `bypassPermissions` makes claude skip the
  per-folder trust dialog at startup (claude's block is gated on `mode !== "bypassPermissions"`),
  which would otherwise wedge an unattended ConPTY session waiting on a keypress. It does NOT
  suppress the one-time **global** "Bypass Permissions mode" acceptance (`~/.claude.json`
  `bypassPermissionsModeAccepted` — shown once, ever, until accepted). Toggling skipPermissions
  OFF drops the flag and instead pins `permissions.defaultMode:"default"` in the hooks-settings
  file (normal prompts + trust apply). The trust decision itself keys on the **git toplevel**
  of the cwd (forward-slash) under `~/.claude.json` `projects.<dir>.hasTrustDialogAccepted` — a
  trusted ancestor counts; accepting at your **home dir never persists** (so it re-prompts).
- **`claude --resume <id>` dies if there's no conversation.** A session that was opened but
  never prompted has no saved transcript; resuming it exits code 1 ("No conversation found")
  and the tab is dead. **Don't gate on the persisted `SessionState`** — it is overwritten
  with the *live post-restore* state (a just-resumed session reads `Idle` until its first
  new turn), so it's useless as a "was-it-used" signal. Gate on the transcript on disk:
  `ClaudeConversationExists(id)` globs `<CLAUDE_CONFIG_DIR | ~/.claude>/projects/*/<id>.jsonl`
  (ids are unique UUIDs, so no need to reproduce Claude's cwd→dir encoding). No transcript ⇒
  launch fresh (`[restore-fresh]` in `hooks.log`) instead of `--resume` (`[resume]`).
- **Working-dir comparison must be filesystem-aware** (else the Explorer Tree forks one dir
  into multiple roots). Windows is case-INsensitive (`C:\…\Desktop` == `…\desktop`) and
  treats `/`≡`\`; POSIX is case-SENSITIVE with `\` a literal char. Route every dir
  grouping/scope/match through `PathEq`/`NormPath` (`#ifdef _WIN32` → `CompareStringOrdinal`
  ignoreCase; `#else` → exact) — never raw `==`/`find`.
- **The hook wire's `prompt` field must be escaped — and the PowerShell escape must mirror
  `WireEscape` byte-for-byte.** The forwarder appends the `UserPromptSubmit` prompt as the
  trailing TAB-separated wire field; a real prompt has embedded TAB/newline, which would break
  the single-line, TAB-split, newline-framed record. Both the PowerShell forwarder and
  `BuildWireLine` escape it identically — `\ → \\` FIRST, then tab/CR/LF → `\t \r \n` — and the
  bridge `WireUnescape`s it. The PowerShell side is the one seam the C++ tests can't cover, so
  verify it with a parity check (`C:\src` → `C:\\src`, `https://` survives, no raw newline/tab
  remains). PowerShell gotcha: `-replace '\\','\\'` (pattern is regex = one backslash;
  replacement is literal = two) does the backslash-doubling, and it MUST run before the
  tab/CR/LF replacements or it would double the backslashes they introduce.
- **Path-picker Popup placement.** Parent the `Primitives::Popup` into the content root
  (top/left aligned) so its `Horizontal/VerticalOffset` is root-relative; the popup's own
  layout anchor already carries the root's offset within the XAML island, so the two compose
  to the box's on-screen position. Open it only on `FocusState::Pointer`/`Keyboard` (not
  `Programmatic`) so it doesn't pop on tab activation, and keep it open across row clicks via
  a **deferred** `LostFocus` check (re-focus the box on pick; bail if it regained focus).

## Correctness rules (do not regress)

1. **"Waiting-for-you" is three states.** A session is *ready* for an auto-send when it is
   **turn-complete** (`Stop` → `WaitingForInput`) **or** sitting **Idle** with no turn in
   progress (a freshly launched / just-resumed plan must START, not wait for a `Stop` it will
   never emit); `Running` / `NeedsApproval` / `Error` / `Done` are never ready. A change-driven
   advance is held to **one prompt per turn** by the pickup guard (don't regress that — it
   prevents a queue-drain). `Notification(permission)` → Approval Policy (NOT the prompt queue).
   A `Stop` whose last message is a question → **Held** by the question-guard.
2. **Tree `Enter` = Activate** (jump to the session's live tab); never forward it to
   `ITerminalConnection::WriteInput` (that would submit a stray carriage return).
3. **Bind queue → sessionId**, never "the selected session" at send time.
4. **Idempotent sends:** mark `Sent` atomically + persist; survive restart without replay.
5. **Backstops:** stop-on-error, maxAutoSends, global pause/kill, pause-on-human-input.
6. **Startup ARCHIVES, never auto-launches; restore = resume, not replay (transcript-gated).**
   On startup, persisted sessions load into the registry as **Archived** (`live=false`) and are
   **NOT** re-launched — the app opens to just the Manager tab, and the prior fleet is restorable
   as a whole from the **Archived** button (a deliberate reversal of the old auto-reopen). A
   user-initiated **Restore** re-launches one: `claude --resume <id>` (same id ⇒ hooks still
   correlate) **only when Claude has a transcript for that id**, else a fresh session (new id,
   same dir + queue, stale archived record dropped). Queues reload with statuses intact. Closing
   a tab **archives** (keeps the record, `live=false`); there is **no discard** — archive is
   terminal, and the Claude transcript on disk is never deleted. Never decide resume from the
   persisted `SessionState` (it's the live post-restore state) — see Gotchas.
7. **State is hook-derived,** never screen-scraped (the Ink TUI repaints constantly).
8. **Same directory = same path, filesystem-aware.** Group/scope/match sessions by working
   dir through `PathEq` (case-insensitive on Windows, case-sensitive on POSIX), so
   case-variant spellings of one directory never fork the Explorer Tree into two roots.

## Conventions

- Mark our additions with `Agentmaster`. Keep the upstream MIT `LICENSE`/`NOTICE`.
- Keep the diff against upstream minimal where practical (additive files, small touches at
  integration points) so rebasing onto `microsoft/terminal` stays cheap.
- Build artifacts (`bin/`, `packages/`, `Generated Files/`) are gitignored — never commit them.
