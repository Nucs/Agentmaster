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
- **Per-tab link badge (overlay) — built ([`TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md)):**
  **every terminal tab the Fleet Observer classifies** carries a small **top-right terminal HUD**
  that makes the tab ⇄ Agentmaster link legible *while you work inside the session*. A **linked**
  Claude session shows the full badge — status (color-matched to the Triage Board) + the Fleet
  Observer's `model · effort · kind`, Autopilot mode (**Manual/Semi/Full**), queued count, and link
  state **⛓ linked**. Any **other** tab shows a dim **observe badge** `○ <kind> · unlinked` (kind =
  `pwsh` / `cmd` / `claude` *started-but-not-yet-prompted* (§11d) / `codex`) that **flips in place**
  as the tab's activity changes — a `pwsh` tab → `claude` the moment you run it → the full linked
  badge on its first prompt; cleared when claude exits or the tab closes — so a started-but-unprompted
  claude (no transcript id yet) is never invisible. Dim until hover; hover/click **expands** controls
  (Autopilot cycle · Send-now · queue peek · Jump-to-Manager) + a contextual SemiAuto confirm.
  Off-switchable (`AppSettings.showTabOverlay`).
  The per-tab *here-and-now* lens, complementing the Manager's *fleet* view.

Full design: [`doc/agentmaster/DESIGN.md`](doc/agentmaster/DESIGN.md).
Milestones & build: [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md).
Hooks bridge: [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md).
Workspace persistence (window layer): [`doc/agentmaster/PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md).
Per-tab link badge (overlay): [`doc/agentmaster/TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md).
Fleet Observer (pull correlation + activity): [`doc/agentmaster/OBSERVER.md`](doc/agentmaster/OBSERVER.md).

## Status

**All milestones M0–M8 + session restore are complete, built, deployed under the
`Agentmaster` identity, and verified running.** The engine passes **376/376** standalone
checks (`AgentMaster/tests/`), and the full pipeline has been exercised end-to-end in the
deployed package: Launch → real `claude.exe` on a ConPTY → `--settings` hooks → PowerShell
forwarder → named pipe → registry → state machine → UI, plus `claude --resume` restore on
reopen (traces in `~/.agentmaster/hooks.log`).

**Workspace persistence ([`PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md)) — the window layer is
COMPLETE + live-verified.** (The original **M9–M14** milestone ladder is **superseded** by
PERSISTENCE.md §13's Increments 1–4: M9 + M10 shipped, **M11 obviated** — the lens rides the
`WindowRecord` autosave, not a persist-hook — M12/M13 delivered AS the Increments, and **M14
migration is moot** (no legacy format). The lone remaining item is one **cosmetic** refinement —
exact left-to-right tab interleave on reopen, noted at the end of this block.) **M9 (singleton
engine) is complete — compiles + unit-tested:** the
registry/bridge/scheduler are now ONE process-wide `SharedEngine` (`AgentMaster/Engine.{h,cpp}`),
so the WindowEmperor's many windows share one fleet over one pipe instead of two registries
racing on `\\.\pipe\agentmaster.<pid>` and clobbering `sessions.json`. **M10's data layer is
done — unit-tested:** the per-window `WindowRecord` schema + `windows/<id>.json` (de)serialize,
shaped as **Option 1** — per-window *UI state only* (geometry + Manager lens + ordered tab
*refs*); `sessions.json` + `SessionInfo.live` stay the single session source of truth, never
duplicated. **M10 capture + per-window lens restore is now done — built + live-verified**
(PERSISTENCE.md §13.5): each window **claims** its `windows/<id>.json` at engine init
(`Engine::ClaimWindowRecord`, or mints a fresh GUID), `_CaptureWindowRecord` reads live
geometry (the `PersistState` recipe) + ordered tab refs + the Manager lens, a debounced
autosave (`_saveWindowRecordThrottled`, fed by tab/resize/recolor/lens-push triggers + a
one-shot save at end of startup) persists it, and a claimed record **seeds the lens**
(`AgentManagerContent::Get/SetManagerState` + a lens-changed push). Verified in the deployed
package: the record carries real geometry + the real splitter layout, and a relaunch **reuses
the same windowId** (claim → restore round-trip). **Increment 2 (geometry re-apply) is also
done + live-verified:** a relaunched window reopens at its saved position/size/launch-mode via
the `TerminalWindow` startup seam (`GetInitialPosition`/`GetLaunchDimensions`/`GetLaunchMode` —
WT's own persisted layout is OFF in our DefaultProfile mode; note WT only calls
`TerminalPage::PersistState()` when `firstWindowPreference != DefaultProfile`, which our mode is
*not*, so geometry capture rides the autosave, not a close-flush). **So single-window workspace
restore — geometry + lens — fully works.** **Multi-window reopen (Increment 3) is also done +
live-verified — including both refinements.** On startup the `WindowEmperor` reopens the
**open-at-exit** set: it scans `windows/*.json` (sorted by filename, matching `LoadWindowRecords` so
`-s <idx>` is canonical), reads the **`open-windows.json` manifest** (a byte-scan for braced `{guid}`
tokens — it links `TerminalApp.dll`, not the `TerminalAppLib` static lib, so it can't call the JSON
helpers), intersects, and dispatches `wt -w new -s <idx>` only for the records that were open at last
exit; each window resolves `records[idx]` for geometry (TerminalWindow) and claims it by id
(TerminalPage), so geometry + lens agree. A **decide-prompt** (Yes/No "Reopen your N previous windows?")
gates the reopen when >1; a lone record / first run is silent. The **manifest** is owned by the
process-wide `SharedEngine` (NOT the Emperor enumerating the projected surface — the engine already has
every window's id): each `TerminalPage` `RegisterLiveWindow`s its `_windowId` at engine init and
`UnregisterLiveWindow`s in `~TerminalPage`, and every change rewrites `open-windows.json` = the live id
set **except** a change that empties it (skip-empty preserves the final snapshot; a hard shutdown that
kills the threads before they unregister leaves the full set). So a window closed mid-session is
**pruned** from the manifest (won't be re-offered) while its record stays on disk. The Manager's
**"Reopen Windows (N)"** recover button (next to Archived, shown when N>0 == records-minus-live,
`Engine::RecoverableWindows`) is the "if I answered No" path: it reopens each not-currently-open record
via `agentmaster.exe -w -1 -s <idx>` (`TerminalPage::_ReopenSavedWindows` ShellExecutes our execution
alias **by name** — not the upstream `wt.exe`, which doesn't exist for our package — and the
single-instance handoff routes it back to the Emperor) — the runtime analog of the Emperor loop. A
window claimed-then-closed THIS session is **re-claimed** (its real id + lens) from a second
**reclaimable pool** (`Engine::reclaimableWindowRecords`, fed by `UnregisterLiveWindow`, drawn by id
ONLY), not minted as a lens-less duplicate; the no-arg front-pop claim never draws from it, so a plain
"+ new window" never adopts a closed window's layout. Verified end-to-end: 2 records + a manifest
naming only one → exactly that one reopens (silent) at its geometry; a fake id is dropped + the second
never added (register rewrote to the live set); the manifest survives app-close (skip-empty); a
gracefully closed window is pruned; and the recover path reopens a not-open record at its saved geometry.
**Window-grouped restore is now shipped + live-verified** — a reopened window re-homes its **whole
workspace**, not just geometry + lens. `TerminalPage::_RestoreWindowTabs` (run from `_OnFirstLayout`
after `_RestoreClaudeSessions`, gated on a *claimed* record) walks the record's ordered tab refs and
rebuilds them in place: each **Claude** ref resumes its session INTO this window (`_LaunchClaudeSession`,
lazy-start safe — no eager `connection.Start()`); each **Other** ref replays its captured WT startup
actions (`WindowLayout::FromJson` → one `ProcessStartupActions`) to recreate the shell tab with its
title + color + cwd. Capture fills the Other ref's `actionsJson` from `BuildStartupActions(Persist)` →
`WindowLayout::ToJson` in `_CaptureWindowRecord` (so `windows/<id>.json` now carries the pwsh/cmd tabs,
not just Claude refs). The Manager's **Archived** UI (now the full-window Archive page, C1 UI) is **grouped by window** (`_GatherArchiveRows`
over `Engine::RecoverableWindows`): each not-currently-open record is a "Saved window" card with a
per-window **Reopen window** (`_ReopenSavedWindow(idx)` → `agentmaster -w -1 -s <idx>`) over its session
rows (**Restore here** = cherry-pick one into the current window); sessions in no record fall under
"Other archived sessions". A clobber guard keeps a not-yet-laid-out window (no tabs AND no geometry, or
pre-Initialized) from overwriting a good record on disk (`_FlushWindowRecord`), and the close-flush
captures the final state before the gap-#1 teardown clears `_claudeTabs` (`CloseWindow`). A reopened
(claimed-record) window also **suppresses the default startup tab** — `_OnFirstLayout` gates
`ProcessStartupActions` on `!_windowRecordClaimed`, because in DefaultProfile mode a reopen's `wt -w new
-s <idx>` (where `ShouldUsePersistedLayout` is off) falls back to a default `newTab` that would otherwise
append a spurious pwsh tab to every reopened window and **accumulate one shell per reopen cycle** in the
record; a reopened window's content comes from the re-home, not the default action. Live-verified:
a window with 1 Claude + 3 pwsh tabs closed → reopened at its geometry (1466×780 @ 14,173) with the
Claude session resumed + all three pwsh tabs (title/cwd) replayed; the record round-tripped intact.
The reopened window also **re-selects the tab that was focused at close** — persisted by STABLE
IDENTITY (`WindowRecord::selectedSessionId` = the Claude conversation id; a shell tab, which has no
cross-restart id, falls back to `selectedTabIndex`; Manager / none ⇒ both unset). `_RestoreWindowTabs`
resolves the focused tab's live position and applies it deterministically — a trailing `SwitchToTab`
appended to the shell `ProcessStartupActions` batch (so it runs AFTER the async shells exist), or a
synchronous select when there are no shells. **Still deferred:** exact left-to-right interleave of
Claude vs Other tabs on reopen (Claude tabs land first, then shells — see PERSISTENCE.md §13.5).

**The Fleet Observer (O1–O7, [`OBSERVER.md`](doc/agentmaster/OBSERVER.md)) is complete — built,
deployed, and live-verified.** It is the **PULL** half of the state engine: a process- +
transcript-driven survey that detects + correlates + enriches **every** Claude session and tab
activity **out-of-band** (reading each `claude.exe`'s PEB — cwd / cmdline / env — and its
transcript), so a hand-typed `claude` that fires **zero hooks** (a shell `claude` function/alias
shadows the PATH shim) is still detected, bound, and driven — with **no hooks, no shim, no
settings, and nothing the user can feel** (all reads are out-of-band; it **never** writes to a
shell). It is the always-correct floor beneath the lossy hook **push** and subsumes the old per-tab
Toolhelp walk + the global `projects/` discovery scan. Live-verified end-to-end in the deployed
package: a no-hook `claude.exe` typed after a `cd` lands a Triage-Board card + per-tab overlay
within ~3 s, classified `Agentmaster`; two claudes in one cwd bind to their **own** conversations;
real-WindowsTerminal claudes are classified external + never bound; steady-state cost is µs.

**The Archive UI is now a full-window page + a round-2 audit fixed 13 issues — both built + deployed.**
The **Archived** button opens a **full-window Archive page** (dense sortable + searchable table left;
detail — metadata + read-only Flight Plan + **Restore here** / **Reopen its window** — right; multi-select
**bulk Restore**) replacing the old in-content modal, mounted over `TerminalPage`'s Root content rows with
**every pointer handler deferring** its tree mutation (a synchronous mid-click tree change AVs the
XAML-Islands hit-test — pinned from two crash dumps, fixed + live-exercised crash-free). A read-only sweep
of that page, the resume/restore path, and the `WindowRecord` layer then **fixed 13 correctness issues**
(commit `b5768081e`): quit-all now flushes the window record, a fleet-load **barrier**
(`Engine::restoreMutex`) stops a reopened window racing its tab re-home against a half-loaded registry, the
`live=true` revive is gated on a changed pid, `SessionRegistry::Remove` notifies observers, inject-rollback
covers every send path, and `ProcessAlive` uses a wait-based liveness test. Detail: *C1 UI* + the round-2
audit bullet under *Persistence*.

What works, by area:
- **Engine (M5, `AgentMaster/`; M9 process singleton).** Thread-safe `SessionRegistry` (single
  source of truth; **token-based** observers — `AddObserver`→token + `RemoveObserver` — and
  multiple `AddAdoptionHandler`s, so a closing window detaches its lens observer + adoption
  handler cleanly instead of dangling on the shared registry), `HooksBridge` (local named-pipe
  server `\\.\pipe\agentmaster.<pid>`), `ClaudeSpawn` (spawn/`--resume` recipe + the shared hooks
  config + PowerShell forwarder). **M9:** one process-wide **`SharedEngine()`** (`Engine.{h,cpp}`)
  owns the registry/bridge/scheduler for ALL windows (the WindowEmperor is one process, N windows
  on N threads); each `TerminalPage` copies the shared `shared_ptr`s and its Manager tab is a
  per-window *lens* over the one fleet. The `<pid>` pipe is unambiguous *because* there is exactly
  one bridge; one writer for `sessions.json`; restore loads process-once
  under a load **barrier** (`Engine::restoreMutex` — a 2nd window blocks until the fleet is fully loaded, then
  skips, so it can't double-load NOR race its tab re-home against a half-loaded registry).
  Hooks → wire line → registry → hook-driven `SessionState` (Correctness Rule #1). The wire
  line carries a 7th **`tabToken`** field (the hosting `WT_SESSION`, for adopting a hand-typed
  `claude` — see *Adopt any `claude`* below) and an 8th, **escaped `prompt`** field on
  `UserPromptSubmit` (`WireEscape`/
  `WireUnescape`: `\ \t \r \n`), so the registry records **every** message a session got — a
  prompt typed straight into the ConPTY becomes a `Sent`/`Typed` Flight-Plan entry, while the
  `UserPromptSubmit` echo of a prompt WE injected is recognized (text + a recency window + the
  transient `QueuedPrompt::echoed` flag) and NOT double-recorded.
- **Adopt any `claude` — observe + control of sessions we did NOT Launch.** A `claude` you
  type yourself into any tab (the WT `+` button → `cd` → `claude`) is managed too, not just
  Manager-Launched ones. At engine init we export `CCMGR_HOOK_PIPE` into the app's process env
  and prepend a transparent **`claude` PATH shim** (`~/.agentmaster/shim/` — `claude.cmd` for
  cmd/PowerShell + a POSIX `claude`) that injects `--settings <ours>` then execs the real claude
  (`ResolveRealClaude`, resolved BEFORE the PATH prepend so it never finds the shim); every new
  tab is *meant* to inherit this env so a bare `claude` self-wires for hooks (but WT's env
  regeneration breaks that for `+` tabs — see the caveat below). (Launch's direct
  `CreateProcessW("claude …")` resolves `claude.exe` with no PATHEXT, bypassing the `.cmd` shim
  — no double-wiring.) The forwarder takes the session id from the hook **payload** and emits
  the hosting **`WT_SESSION`** as the `tabToken` wire field, falling back to a **`bridge.json`**
  discovery file when it didn't inherit the pipe env. The registry **adopts** an unknown session
  on `SessionStart` (flagged `SessionInfo::external`) and fires every window's adoption handler (`AddAdoptionHandler`, fanned out — whichever window hosts the `+` tab binds it);
  `TerminalPage::_AdoptExternalSession` matches the `tabToken` to a live ConPTY
  (`ITerminalConnection::SessionId`) and **binds a stdin injector** — promoting it to full
  observe+control (Autopilot can drive it). A claude hosted outside this app (no matching
  connection) stays observe-only (Rule #9). **Caveat — this hook fast-path is *degraded* for
  hand-typed `+`-tab claudes:** WT regenerates a `+`-tab's child env from the registry, dropping
  these runtime-only vars (`AM_SESSION` / `CCMGR_HOOK_PIPE` / the PATH shim — see Gotchas), so a bare
  `claude` there fires **zero** hooks (a Manager-Launched session is unaffected — it sets env
  explicitly via `spec.env`). The **Fleet Observer** (below) is the reliable detection + bind path
  that needs none of this env — it keys on the **roster correlation**, not the shim/pipe.
- **Fleet Observer — PULL correlation + activity (O1–O7, `OBSERVER.md`; `AgentMaster/ProcessInspect.{h,cpp}`,
  `ProcessObserver.{h,cpp}`, `Activity.h`).** The always-correct floor BENEATH the lossy hook push — the
  one path no shell function / alias / PATH quirk can shadow. Empirically grounded (we measured it live):
  **`WT_SESSION` is the correlation key** (== `ITerminalConnection::SessionId()`, stable across a claude
  close→`cd`→reopen), enumeration (~10 ms Toolhelp) is the only cost while PEB reads are µs, and Claude
  closes its `.jsonl` after each write so correlation is by **cwd → newest transcript**, not an open handle.
  - **Primitives (`ProcessInspect`).** ONE `SnapshotProcesses()` per survey (reused for census + every tab
    tree) + µs PEB reads via the x64 `RTL_USER_PROCESS_PARAMETERS` offsets WT itself reads (centralized +
    commented, WOW64-guarded): `ReadProcessCwd` (the claude PROCESS cwd — tracks `cd` across a relaunch;
    PowerShell never syncs ITS process cwd, but spawns claude with the right one), `ReadProcessCommandLine`
    (exposes `--resume`/`--session-id`/`--model`/`--effort`/`--permission-mode`), `ReadProcessEnv`
    (`WT_SESSION` + `AM_SESSION` + `CLAUDE_*`); `ProcessStartUnixMs`/`ProcessAlive`; pure tree helpers
    (`FindDescendantByImage`/`ChildrenOf`, replacing the old `FindClaudeDescendantPid`); `ReadClaudeFacts`
    (cmdline+env parse); `ClassifyRunningApp`; transcript resolution (`EncodeCwdToProjectDir` — every
    non-`[A-Za-z0-9]` → `-`; `ResolveSessionId` — see Rule #14); and **transcript content** —
    `TranscriptTimes` (cheap ctime/mtime stat = conversation age + last activity) + `ReadTranscriptInfo`
    (head or full read → the first-prompt **title**, `gitBranch`, and the human prompts, parsed like
    `ParseTranscriptDelta`; recent transcripts carry NO `summary` — verified 0/1842 over 90 days — so the
    first prompt is the title). A denied/elevated/WOW64 PEB read ⇒ observe-only, never mis-bound.
    (Promoted out of `ClaudeSpawn`'s anon namespace; `ClaudeCwdForShell` now delegates here.)
  - **`AM_SESSION` ownership stamp (O2, `Engine`).** A per-process GUID exported into the app env at
    engine init (alongside `CCMGR_HOOK_PIPE`); a Manager-Launched claude additionally carries
    `AM_SESSION=<guid>:<windowId>` (via `spec.env`, the launching window appended). `ClassifyRunningApp`
    matches on the GUID **prefix**: ours → `Agentmaster`, a bare `WT_SESSION` (no `AM_SESSION`) → the real
    `WindowsTerminal`, neither → `Other`. **This is the *census* signal, REFINED by the roster:** a
    hand-typed `+`-tab claude does NOT reliably inherit `AM_SESSION` (WT regenerates a tab's env — see
    Gotchas), so a claude correlated to one of OUR roster tabs is `Agentmaster` *regardless* (Rule #13);
    `AM_SESSION` only classifies the NON-rostered remainder (a background daemon we spawned, telling our
    instance from another's, or a real `WindowsTerminal` → external, observe-only). Per-process GUID ⇒ two
    Agentmaster instances stay cleanly separable.
  - **Registry feed (O3, `SessionRegistry::ObserveClaude`).** A provenance-aware upsert: first sight of a
    claude we did NOT Launch creates an `external` record + fires adoption; thereafter it idempotently
    ENRICHES the transient `SessionInfo` facts (pid / liveCwd / model / effort / permissionMode /
    background / runningApp / amSession / ownerWindowId, plus the transcript timing
    `convCreated/convLastActivityUnixMs` that drives the per-session timing adornment — all runtime-only,
    NOT persisted) but **NEVER** sets `SessionState` (push hooks + the transcript tail own state, Rule
    #1/#7). The timing is refreshed silently (like `lastObservedUnixMs`) — mtime ticks constantly, so it
    must not drive the change cascade; the UI recomputes the "ago" live on any rebuild. A steady-state re-observe
    with unchanged facts is a no-op (no observer / persist / UI churn). `hookWired`/`lastHookUnixMs` are set
    in `OnHookEvent` for provenance.
  - **S-lane (`ProcessObserver`).** A process-wide worker (next to the scanner, thread/condvar shape
    mirrored) on a ~2 s heartbeat + on-roster-change `Wake()`: ONE snapshot → `ReadClaudeFacts` + classify
    EVERY claude (the census, logged `[observer] census claudes=N ours=A wt=W other=O rostered=R`) → merge
    every window's published tab roster → per roster tab, `FindDescendantByImage` the shell's claude,
    resolve its id, and feed `ObserveClaude`. **A claude correlated to a tab in OUR roster is OURS
    regardless of `AM_SESSION`** (Rule #13). Publishes two copy-under-lock tables (Correlation + Activity).
  - **UI lane (`TerminalPage::_ObserverProbe`, replaces `_DiscoverClaudeTabsByCwd`).** The one WinRT
    thread: each scanner tick it builds THIS window's roster `{WT_SESSION = SessionId(), shell PID =
    GetProcessId(ConptyConnection::RootProcessHandle()), bound}`, `PublishRoster`s it (both reads are µs),
    then — after a short settle so the publish-triggered survey lands — reads the Correlation table and
    binds each of our unbound, id-resolved tabs via the **unchanged** `_BindClaudeSessionToTab` (injector +
    overlay + title + per-dir color). Detached via `_observer->UnpublishWindow(_windowId)` in
    `~TerminalPage` (Rule #10).
  - **Activity + adornments (O6).** Full `TabActivity` taxonomy (Powershell / Cmd / ClaudeCode / Codex
    (bare acknowledge, image+pid only) / Other), enriched `[activity]` events, `model · effort · kind`
    adornments on the Manager cards + per-tab overlay, and an **External (N)** group. The external
    census now includes **cmd-/console-hosted** claudes too (the `Other` bucket — previously counted but
    hidden), not only real-WindowsTerminal, and each `ExternalClaudeRow` is **enriched from its
    transcript** (worker-thread title cache so the head-read happens once): resolved `sessionId`, a
    **title** (first prompt), `gitBranch`, host kind (`wt` / `cmd` / shell leaf), and `created/lastActivity`
    timing. Observe-only — surfaced on the board AND the Explorer Tree's **EXTERNAL** scope, where a row's
    **Open New Session Here** / **Adopt** lives on the right-click menu and a **left-click → a read-only Flight Plan**
    of the conversation.
  - **Hardening (O7).** Steady-state is µs: the survey skips the Toolhelp snapshot when the roster is
    byte-identical to last tick AND every correlated `(pid, start-time)` pair is still alive (the start time
    is paired — per the rule that PIDs reuse — so a recycled PID can't masquerade as alive), except on the
    slow heartbeat. Retires the now-dead `SessionScanner` transcript-DISCOVERY sweep (`ArmDiscovery` /
    `RecentTranscripts`, which the observer subsumes; the scanner keeps its state-reconcile tail). A
    denied / elevated / WOW64 PEB read is guarded → observe-only, never a misread. 24-h soak: no leak / wedge.
- **C1 UI (M6, `AgentManagerContent`).** Triage Board + Explorer Tree + Flight Plan,
  imperative and snapshot-driven from the registry (cross-thread refresh via
  `DispatcherQueue`), bidirectional selection + directory scope. Each **Triage-Board column**
  is a fixed-width box at full board height with a **pinned header over a vertically-scrolling
  card list** (`_MakeBoardColumn`), so a tall column (e.g. a large **External** census) scrolls
  within the board instead of clipping past the bottom edge (the board's own ScrollViewer scrolls
  only horizontally). The Board header's **"Show all"** (clears the directory scope) is shown only
  when a directory IS scoped — it auto-hides (`_showAllBtn`, kept in sync by `_RebuildBoard`) while
  already showing all directories. The Board/Tree show only
  **OPEN** (`live`) sessions; closed ones are **ARCHIVED** (shut down, restorable) and opened from the
  **Archived (N)** toolbar button (the toolbar's rightmost, after the cog) — a **full-window Archive page**
  (`_BuildArchivePageShell`/`_ShowArchivePage`, mounted over `TerminalPage`'s Root content rows, ← Back to
  dismiss; it REPLACES the old in-content modal). LEFT = a dense, **sortable + searchable** table of archived
  sessions (Title · Directory · Branch · Created · Active · a saved-**window** chip), each row a checkbox for
  **multi-select bulk Restore**; RIGHT = the selected row's **detail** — metadata + a read-only Flight Plan +
  **Restore here** / **Reopen its window** (resume via `claude --resume`, transcript-gated). XAML-Islands hard
  rule: every pointer handler **defers** its visual-tree mutation to the dispatcher (a synchronous tree change
  mid-click AVs the hit-test), so row-select is highlight-only and open/sort/restore/back post to a clean tick. Explorer `Enter`=Activate /
  `Del`=archive (never injects — Rule #2). The tree's **scope toggle is 3-way — LOCAL · GLOBAL ·
  EXTERNAL** (this window's sessions · all windows · the Fleet Observer's observe-only externals),
  and after it a **sort toggle — NEWEST · OLDEST · MOST ACTIVE · A–Z · BY PID** (`_CycleTreeSort` /
  `_UpdateTreeSortButton`) that orders **both the directory groups and the rows within each, in every
  scope** (a dir's rank is an aggregate over its sessions: NEWEST/OLDEST by conversation ctime, MOST
  ACTIVE by recency with a currently-**Running** session pinned to the top, A–Z by name, **BY PID** by
  the **host window/shell pid** — externals: `ExternalClaudeRow::hostPid`, the same key as the
  color-coded pid underline, so same-window rows group together; managed: the claude pid — **then by
  most active** within each pid group; ctime/mtime via the same transcript timing as the adornment,
  `SortKey`/`MakeSortKey`/`SortKeyLess`). Unlike the
  per-window in-memory scope, the sort is a **GLOBAL, persisted** setting (`AppSettings::treeSort` →
  `settings.json`, via the same settings sink the cog uses) — it survives restart and seeds every
  window (the changing window re-sorts live; others adopt it on next launch). After the sort comes a
  **↻ refresh button** (`_treeRefreshBtn`) that **reloads the data for the current scope**: it redraws
  immediately (re-pulls the registry snapshot + recomputes the live "ago" timing) and fires
  `_refreshHandler` → the page's `_RefreshObserverData` — which `Wake()`s the Fleet Observer for an
  immediate full survey (re-enrich the registry + recompute the External/Correlation tables, bypassing
  the O7 debounce), re-runs `_ObserverProbe`, then forces one `RefreshNow()` redraw once it lands — so
  LOCAL/GLOBAL re-pull and the EXTERNAL census both refresh on demand instead of waiting for the next
  tick. EXTERNAL/GLOBAL keep their local-first grouping, with the sort applied **within** each group:
  **EXTERNAL** (`_RebuildExternalTree`) lists every claude we do NOT manage — both real-WindowsTerminal
  **and cmd-/console-hosted** (the `Other` census bucket, previously hidden) — grouped by cwd. Each row is
  **enriched out-of-band from the transcript**: a real **title** (the conversation's first prompt — recent
  transcripts carry no `summary`), a **host** tag (`wt` / `cmd` / the shell leaf), `gitBranch`,
  `model · effort · pid`, and the timing adornment. The **pid number carries a color-coded underline**
  keyed by its **host window/shell** (`ExternalClaudeRow::hostPid` = the claude's parent shell pid,
  filled by the observer census; `WindowKeyColor` maps it through a stable palette) — claudes running in
  the **same terminal window/tab share a host shell**, so they get the **same underline color** and are
  easy to identify at a glance, even across cwd groups (note: real WT runs single-process, so this
  groups by host **tab/shell**, the finest reliable unit — it never falsely merges distinct windows).
  **Left-click selects** an external — from the
  Explorer-Tree EXTERNAL row **OR a Triage-Board External card** (the whole card is the click target;
  there is no inline observe pill / Adopt button) — → the Flight Plan shows its conversation
  **read-only** (`_RebuildExternalPlan` — the human prompts, read from the transcript on a
  **background thread** and cached; observe-only — we host no ConPTY, so it is never drivable, no
  queue/Autopilot). Selecting an external is **Linked-Lenses-synced** (`_SelectExternal`): it switches
  the tree to **EXTERNAL** with the row highlighted, highlights the board card, and renders the
  read-only plan — so a board click behaves exactly like a tree click. **Right-click** (on either the
  tree row or the board card — both use `_MakeExternalTreeMenu`) offers **Adopt** (resume its
  conversation into a managed, controllable tab — `_AdoptExternalClaude` resolves the id via
  `ResolveSessionId`, then `claude --resume`s it, leaving the original external running — Rule #13)
  and, **as the last item, Open New Session Here** (spawn a managed session in that cwd, a new
  independent conversation). **Open New Session Here is offered in EVERY scope** — it is also the
  **last item** on the LOCAL/GLOBAL session-row menu (`_MakeSessionMenu`, after Rename / Archive),
  spawning in that session's working dir. With no external selected the
  Flight Plan reads **nothing-selected**. Every card/row (board, tree LOCAL/GLOBAL/EXTERNAL) carries a dim
  **timing adornment** `-createdAgo/activeFor/-lastActivityAgo` (e.g. `-2m7d/12h/-2h30m` — created ago /
  active span / last-activity ago; `m`=month or minute by position, tooltip-explained;
  `FormatSessionTiming`/`FormatSpan`) from the transcript's ctime/mtime (managed:
  `SessionInfo::convCreated/convLastActivityUnixMs`; external: `ExternalClaudeRow` times, falling back to
  the process start). **A session's title is one value** — the
  Explorer-tree row, the WT **tab** title, and the persisted `SessionInfo.title` are the same
  thing: it's **pinned** onto the tab at launch/restore (`Tab::SetTabText`, so it stops floating
  with claude's OSC title) and renaming from **either** side syncs the other + persists (Explorer
  right-click **Rename…** → `_RenameClaudeSession`; a WT tab rename → `_SyncClaudeTitleFromTab`;
  Rule #11). A launched session's default name is **smart-derived from its cwd**
  (`DeriveSessionTitle`: walk up past generic `bin/obj/Debug/...` segments to the first meaningful
  folder, then **≤16 chars** as-is / **>16 mixed-case** → its capitals only / **>16 all-lower** →
  as-is truncated past 30 with `...`), and each tab is **colored per working directory** (a stable
  auto palette color or the dir's persisted one; recoloring one tab recolors every tab in that dir
  and persists — Rule #12). Flight Plan: a **compose row** — three top-left icon buttons
  (**eye** = Focus / jump to the live tab · **!** = Send now, *which now confirms first* ·
  **envelope** = Add to the queue) beside a **multiline textarea** that grows as you type;
  per-message actions moved off a button strip onto a **right-click menu over the messages**
  (`_MakePromptMenu`: **Move up / Move down / Delete** on upcoming rows, **Archive session** on
  any). **Autopilot** is now a **toggle in the FLIGHT PLAN header** (mirrors the Explorer Tree
  LOCAL/GLOBAL/EXTERNAL toggle) — a colored state dot, gray ○ Off / amber ◐ Semi / green ● Full, that
  **cycles** Off → Semi-auto → Full on click (`_CycleAutopilot` / `_UpdateAutopilotButton`,
  replacing the old combo); the **Templates** row (save / apply / apply-to-dir) is collapsed
  behind a **paper icon** at the textarea's top-right (kept inline — NOT a Flyout — so its name
  box still takes keypresses; the XAML-Islands text-input trap). It still reflects **all**
  messages a session got, not just queued ones: a chronological **SENT** summary (each row
  tagged **flight** = we queued+injected it vs **typed** = you typed it into the terminal) over
  the **UPCOMING** queue (Pending/Held). `Focus()` focuses the cwd `TextBox`. The Launch cwd box has a
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
  mid-restore) rolls the prompt back to `Pending` rather than stranding a phantom `Sent` — on **every** inject
  path: auto-send, the SemiAuto `Confirm`, and the Manager's Send-now (Rule #4).
- **Persistence + archive/restore (M8, `Json.h`/`Persistence`).** Sessions + named plan
  templates + the path-picker's recent-dirs MRU (de)serialize to JSON under
  `%USERPROFILE%\.agentmaster\`; sessions autosave on change. **Lifecycle = Open ⇄ Archived**
  (the transient `SessionInfo::live` flag, never persisted): Open == has a live tab/claude this
  run (on the Board); Archived == shut down but kept restorable (behind the Archived button).
  On startup `_RestoreClaudeSessions()` loads each saved session into the registry as
  **Archived** and does **NOT** auto-launch it (Rule #6) — the app opens to just the Manager
  tab; the prior fleet comes back from the **Archive page** (per-row **Restore here** or **multi-select bulk Restore**). Closing a session's tab
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
  - **Lifecycle coverage — audit of the add-tab / launch / close / archive / resume paths
    end-to-end. #1 & #2 are ✅ FIXED since the audit; #3 (by design), #4 (mitigated), #5 (moot),
    #6 (cosmetic) remain — all low-severity.** The tab-X archive seam above is sound; the *non-tab-X*
    exits were the risk. **(1, HIGH — ✅ FIXED) closing a _window_ now archives its sessions.**
    `_ArchiveWindowSessionsOnTeardown` (`TerminalPage.cpp:1825`, commit `4fa9fb7bc`) mirrors
    `_ArchiveAndCloseClaudeTab`'s bookkeeping for every hosted session — minus the dialog + `tab.Close()`
    (the tabs go with the window): flip `live=false`, clear the injector (releases the ConptyConnection →
    claude.exe exits), drop the per-window maps, persist once. It runs from the deterministic close seam
    (`CloseWindow:4605`, after the confirm) for immediate phantom-clearing AND idempotently from
    `~TerminalPage:254` as the catch-all for quit / any other teardown. So a window-chrome ✕ (still the
    _primary_ window exit — the non-closable Manager tab means `_RemoveTab`'s `size()==0` path can't fire)
    no longer leaves `live=true` **phantom cards** or **leaked injectors/connections** on the other
    windows' Triage Boards. **(2, MED — ✅ FIXED) no-tab archive no longer fake-archives a still-running
    session.** `_ArchiveClaudeSession`'s no-tab branch (`:1730`, commit `0336fa420`) now guards on
    `ProcessAlive(info->pid)`: it refuses to flip `live=false` on a claude that is still alive but not
    hosted in THIS window (an *ours* claude the S-lane carded a tick before its bind completed, or one
    hosted by another window) — which `ObserveClaude` would otherwise bounce back to `live=true` the next
    survey (`SessionRegistry.cpp:339`, Rule #7). Only a claude that has actually EXITED archives there.
    **(3, MED — open, by design) adopt-external = two writers on one transcript** — `_AdoptExternalClaude`
    (`:1877`) `--resume`s the id into a managed tab while the original external keeps running, both
    appending the same `.jsonl` (only a code comment — "the user closes it" — mitigates). **(4, LOW —
    mitigated) `SessionEnd`/`Done` doesn't eagerly archive** — only the liveness sweep
    (`_SweepClaudeLiveness:2334`) does, when the hosting connection reaches Closed; the window-close case
    it once missed (→ gap 1) is now covered by the teardown archive, leaving only ~one slow heartbeat
    (~2 s) of a finished claude lingering as a live card. **(5, LOW — effectively moot) Launch with a null
    tab** — `Upsert(live=true)`+`SetInjector` still run unconditionally after `_CreateNewTabFromPane`
    (`:1213`), but the `pane==null` guard (`:1183`) makes a null `tab` unreachable
    (`_CreateNewTabFromPane` returns null only for a null pane), so there is no phantom in practice; the
    unconditional upsert is a latent defensive nit. **(6, LOW — open) persisted `state` is dead weight** —
    `ToJson(SessionInfo)` writes it but `_RestoreClaudeSessions` forces `Idle` on load (`:1389`); written,
    never read (the resume-gating gotcha already says don't trust it). **Verified clean (not gaps):**
    external WindowsTerminal/Other claudes never enter the registry or `sessions.json` (`ObserveClaude` is
    gated on rostered + resolved id, `ProcessObserver.cpp:427`) — no foreign-claude leak into the Archived
    list; cross-window double-bind is guarded (`HasInjector`; one ConPTY lives in one window); and
    resume-fresh drops the stale archived record (`:1218`).
  - **Archive-page + resume/restore + persistence audit (round 2) — 13 findings, all ✅ FIXED + deployed**
    (commit `b5768081e`; a read-only sweep of the Archive page, resume/restore, and the `WindowRecord`
    layer). *Archive page:* the header count tracks the active filter ("K of N"); **Reopen its window**
    re-resolves its target from the record's stable `windowId` at click time (a gather-time index goes stale
    if the record set shifts while the page is open); `_GatherArchiveRows` indexes archived sessions by id
    ONCE (was O(tabs·sessions)) + dedupes a session referenced by two records to a single row; the
    Restore/Reopen/bulk handlers **defer** their tree mutation (the page's pointer-handler crash class);
    bulk-restore + its "(N)" count act on **checked ∩ visible** only (a check hidden by a later search is
    never silently restored) and stale checks are pruned each gather. *Persistence:* **quit-all now flushes**
    each window's record (`RequestQuit` + the `~TerminalPage` catch-all, latched so the catch-all can't
    clobber a good close-seam flush after `_claudeTabs` is cleared) — previously only the 750 ms debounce
    saved it, losing trailing geometry/tab-order/lens on app quit; the process-once fleet load is now a true
    **barrier** (`Engine::restoreMutex`) so a 2nd reopened window BLOCKS until the registry is populated
    before re-homing, instead of skipping its not-yet-loaded sessions and flushing an EMPTY record over its
    workspace; `_CaptureWindowRecord` stops persisting a **dead** per-tab Claude color (the dir-color system
    owns it, Rule #12) and only records a focused shell-tab target that can actually be recreated. *Registry
    / autopilot:* `SessionRegistry::Remove` now `_notify`s (a resume-fresh drop refreshes every window's
    Archive list — no ghost row); `ObserveClaude`'s `live=true` revive is gated on a **different pid** so a
    just-archived session whose claude is briefly still alive isn't bounced back (Rule #7); `Scheduler::Confirm`
    (SemiAuto) + the Manager's Send-now now **roll a failed inject back to `Pending`** like the auto-send path
    (Rule #4). *Primitive:* `ProcessAlive` prefers the unambiguous `WaitForSingleObject` liveness test (avoids
    the `GetExitCodeProcess`==`STILL_ACTIVE`/259 false-alive), falling back to the query path when SYNCHRONIZE
    is denied.
- **Per-window records (M10 data layer, `Persistence`/`SessionModels`).** A `WindowRecord`
  (one file per window: `windows/<windowId>.json`) holds per-window **UI state** — geometry
  (position/size/launch-mode), the Manager **lens** (selection / dir scope / selected prompt /
  collapsed dirs / splitter fractions), and an **ordered list of tab refs** (a Claude tab = just
  its `sessionId`; a non-Claude tab = an opaque WT `actionsJson`). This is **Option 1** — a thin
  layer OVER the archive model: it records tab order + window↔session affinity + geometry/lens
  WITHOUT duplicating session data (`sessions.json` stays the session truth, so there is one copy
  of every session). Schema + (de)serialize + `Save/Load/Delete/LoadWindowRecord` are done and
  unit-tested; the live **capture** (debounced autosave) and **restore** (re-apply geometry/lens,
  claim/re-claim a record by id) are **shipped + live-verified** (see Status + `PERSISTENCE.md` §13.5).
  **Session re-home + Other-tab recreation are now shipped too** (`_RestoreWindowTabs`): a reopened
  window resumes its Claude sessions and replays its shell tabs (title/color/cwd) from the record's tab
  refs, in order — so closing and reopening a window brings the whole workspace back, not just
  geometry + lens. The Manager's full-window **Archive page** (C1 UI) groups closed sessions **by window** with a per-window
  "Reopen window". (Tab `actionsJson` capture, once deferred, is now live in `_CaptureWindowRecord`.)
- **Settings cog (`AppSettings`, `settings.json`).** A `⚙` (toolbar order: Launch · Reopen · `⚙` ·
  Pause Autopilot · Archived — the cog sits *before* Pause Autopilot / Archived) opens a
  global-settings surface — an **in-content modal overlay** (a dimmed `Grid` over `_root`),
  NOT a `ContentDialog` (a text box inside one gets no keypresses in XAML Islands — see
  Gotchas). Exposes **Claude-session** config — `skipPermissions` (the spawn's
  `--dangerously-skip-permissions`), `model` (== `/model <v>`), `includeCoAuthoredBy`, and a
  global **`env`** (a `;`-delimited `NAME=VALUE` list applied to every session via
  `ParseEnvAssignments`→`spec.env`, `CCMGR_*` filtered) — plus **Autopilot defaults** stamped
  onto NEW sessions (mode / maxAutoSends / stopOnError / pauseOnHumanInput) and **behavior**
  (`confirmBeforeKill` — relabeled "Confirm before archiving" — routes the archive action
  (tab X / Manager Archive / tree `Del`) through the confirm dialog;
  `defaultLaunchDir` seeds the cwd box). It also carries non-cog global state set elsewhere in the
  UI but persisted through the same file: `showTabOverlay` and **`treeSort`** (the Explorer Tree's
  NEWEST/OLDEST/MOST ACTIVE/A–Z sort — written by the tree's sort toggle via the settings sink, NOT
  the cog). Loaded at engine init, seeded via `SetSettings`,
  persisted + re-materialized on Save via `SetSettingsHandler`. Every default reproduces prior
  behavior, so a missing `settings.json` (or any unset field) is a no-op.

Follow-ups (not blocking): feed `pauseOnHumanInput` from a TermControl input tap;
bracketed-paste for true multi-line prompt bodies; a live buffer "peek" in the Flight Plan;
**bulk Restore** (the Archive page's "Restore selected") re-opens tabs lazily (a non-foreground restored tab starts its `claude` only
when first focused — WT's lazy-background-tab behavior; restore one at a time to force start);
a one-time **"Restore your previous layout?"** launch prompt (offers **all archived sessions**;
decided + **deferred** — it ships *after* the per-window `WindowRecord` capture is wired so it
restores true per-window layouts, not a flat global list — see `PERSISTENCE.md` §6/§6a);
prevent splitting the Manager tab. The **per-tab link badge / overlay**
([`TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md) / DESIGN §9.7) is **built** — `AgentTabOverlay`
wrapped in the terminal's slot `Grid` (`TerminalPaneContent::SetAgentOverlay`), driven by
`SessionRegistry::HasInjector` + `AppSettings.showTabOverlay`, attached on bind
(`_AttachClaudeOverlay` / `_claudeOverlays`) and **enriched by the Fleet Observer** with
`model · effort · kind` (O6). It now also shows on **every non-bound tab** as a registry-LESS
**observe badge** (`○ <kind> · unlinked`): the `_ObserverProbe` UI lane reads the observer's
`Correlation()` + `Activity()` tables and `_SetTabActivityBadge`s each tab's kind via
`AgentTabOverlay::ShowActivity` (registry-less — there is no session to observe; keyed by `WT_SESSION`
in `_pendingOverlays`, idempotent on an unchanged kind) — a `pwsh` / `cmd` / `codex` tab, or a
never-prompted `claude` (correlated but no transcript id yet, §11d). The badge flips kind in place as
activity changes and is **promoted to the bound `_AttachClaudeOverlay`** the instant a claude resolves
an id (its first prompt); `_DropPendingOverlay` collapses + releases it when the tab binds, the claude
exits, or the tab leaves the window's roster. Milestones tracked in `doc/agentmaster/IMPLEMENTATION.md`.

## Repo facts

- Forked from `microsoft/terminal` @ `v1.24.2372`; work branch **`agentmaster`**.
- Build entry: **`OpenConsole.slnx`** (slnx format). No git submodules in 1.24
  (`doc/building.md` is stale on that point).
- Toolchain: VS 2022 + C++/UWP workloads + Windows SDK 10.0.22621/26100.
- **Package identity = `Agentmaster`** (PFN `Agentmaster_56k4f06dsfp9r`), set in
  `src/cascadia/CascadiaPackage/Package-Dev.appxmanifest` (the Debug branding). It is
  deliberately **distinct from `WindowsTerminalDev`** so it coexists with real Windows
  Terminal. ⚠️ There is a **separate `K:\source\windowsterminal` checkout on this machine
  that owns the `WindowsTerminalDev` identity** — never reuse that identity here (see Gotchas).
- Our additions (all marked `Agentmaster`):
  - `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}` — the Manager tab content (C1 UI).
  - `src/cascadia/TerminalApp/AgentMaster/` — the engine (plain C++, no WinRT; the `.cpp`
    are `<PrecompiledHeader>NotUsing`): `SessionModels.h`, `HookEvents.h`, `HookWire.h`,
    `SessionRegistry.{h,cpp}`, `HooksBridge.{h,cpp}`, `ClaudeSpawn.{h,cpp}`,
    `Scheduler.{h,cpp}`, `Engine.{h,cpp}` (the M9 process-wide `SharedEngine`),
    `SessionScanner.{h,cpp}` (the interval reconciler / PULL transcript tail), the **Fleet
    Observer** — `Activity.h` (data models), `ProcessInspect.{h,cpp}` (PEB / Toolhelp / transcript
    primitives — id resolution + content: title / prompts / ctime·mtime timing),
    `ProcessObserver.{h,cpp}` (the S-lane) — `Json.h`, `Persistence.{h,cpp}`, and
    `tests/` (standalone harness, not in the msbuild — run `tests/run-m5-tests.bat`).
  - `src/cascadia/TerminalApp/AgentTabOverlay.{h,cpp}` — the per-tab link badge (TAB_OVERLAY.md),
    enriched by the observer with `model · effort · kind`; also the registry-less `ShowActivity`
    **observe badge** (`○ <kind> · unlinked`: pwsh / cmd / unprompted-claude / codex) for every non-bound tab.
  - small touches in `TerminalPage.{h,cpp}` (engine wiring, spawn/restore, tab-title sync, smart
    naming + per-dir tab color, the observer UI lane `_ObserverProbe`, external-claude adopt
    `_AdoptExternalClaude`), `Tab.{h,cpp}` (a
    `TabColorChanged` event + `GetRuntimeTabColor`), and `TabManagement.cpp`; registrations in
    `TerminalAppLib.vcxproj`.
  - `Package-Dev.appxmanifest` (identity), `doc/agentmaster/`, `tools/Build-Agentmaster.ps1`,
    `tools/am-lock.sh` (the global build/launch mutex — see Deploy & run → *Concurrency lock*).
- **Runtime state dir: `%USERPROFILE%\.agentmaster\`** — `hooks-settings.json` +
  `agentmaster-hook.ps1` (the shared hooks config Claude is pointed at via `--settings`),
  `hooks.log` + `autopilot.log` (engine traces), `sessions.json` (persisted fleet),
  `templates.json` (saved plans), `recent-dirs.json` (path-picker MRU), `dir-colors.json`
  (per-working-directory tab colors), `settings.json`
  (the Settings cog's `AppSettings`), `windows/<id>.json` (M10 per-window UI-state records —
  one file per window; captured + autosaved + restored), `open-windows.json` (the M10 Increment-3
  open-at-exit manifest — the live window-id set the next launch reopens), `bridge.json`
  (live-bridge discovery for the shim), `shim/` (the transparent `claude` PATH shim —
  `claude.cmd` + a POSIX `claude` — that auto-wires hand-typed sessions; see *Adopt any
  `claude`*), and `locks/` (the `build-launch` mutex — `tools/am-lock.sh`; see Deploy & run →
  *Concurrency lock*). Deliberately NOT under `%LOCALAPPDATA%` — see Gotchas (MSIX).

## Integration points (1.24 pluggable pane-content model)

- New tab content implements **`IPaneContent`** (like `ScratchpadContent` — no `.idl`).
  Ours: `AgentManagerContent`, compiled via `src/cascadia/TerminalApp/TerminalAppLib.vcxproj`.
- Content dispatch by type string in `TerminalPage::_MakePane` (`TerminalPage.cpp`): we add
  an `else if (paneType == L"agentManager")` branch → `make_self<AgentManagerContent>()`.
- The Manager tab is opened by **`TerminalPage::_OpenAgentManagerTab()`**, called from
  `_OnFirstLayout` *before* startup terminal tabs, so it lands at index 0 (leftmost), and is
  tracked in the `_managerTab` member (nulled on close in `TabManagement.cpp`, mirroring
  `_settingsTab`). It is **non-closable AND non-movable**:
  - *Non-closable* — `CloseButtonVisibility = Never` (→ `IsClosable(false)`) hides the X and
    `DisableCloseAndMoveMenuItems()` greys the context-menu Close/Move. **Critically,**
    `_updateAllTabCloseButtons()` (which re-applies the theme's global close-button setting to
    *every* tab on each theme/settings pass) **skips `_managerTab`** — otherwise it re-enabled
    the X right after we set it (see Gotchas). The only way to "kill" is closing the **window**.
  - *Non-movable* — its `TabViewItem` is `CanDrag(false)`/`AllowDrop(false)`; `_TryMoveTab`
    refuses to move it and reserves index 0 for it; and `_PinManagerTabFirst()` (called from
    `_TabDragCompleted`) snaps it back to 0 if another tab is dropped ahead of it.
- Tab placement primitive: `_CreateNewTabFromPane(pane, insertPosition)` (`TabManagement.cpp`).
- **Engine wiring (`TerminalPage`):** `_InitAgentmasterEngine()` (from `_OnFirstLayout`,
  before the Manager tab) **consumes the process-wide `::Agentmaster::SharedEngine()`** (M9) —
  copies the shared `SessionRegistry` + `HooksBridge` + `Scheduler` + `SessionScanner` +
  `ProcessObserver` `shared_ptr`s and registers THIS window's adoption handler + liveness probe
  (tokens, detached in `~TerminalPage`). The once-per-process wiring (the logging / scheduler /
  persistence observers, the pipe, bridge discovery + hook files + the PATH shim, the
  **`AM_SESSION`** mint + export, and the `ProcessObserver` start) lives in `Engine.cpp` and runs
  on first access. The Fleet Observer's UI lane is `_ObserverProbe()` (ticked by the scanner's
  liveness probe alongside `_ReconcileClaudeTabs` + `_SweepClaudeLiveness`; it replaced
  `_DiscoverClaudeTabsByCwd`): publish this window's tab roster, then bind via the observer's
  Correlation table. `~TerminalPage` calls `_observer->UnpublishWindow(_windowId)` (Rule #10).
  `_WireAgentManagerContent()` hands the content the shared registry + spawn / activate / archive
  / restore / rename / adopt-external / pause / confirm callbacks + the cog's settings seed/persist
  (`SetSettings`/`SetSettingsHandler`). `_LaunchClaudeSession(dir, title, restored)` builds a
  claude `ConptyConnection` (cmdline/cwd/env ours) and opens it as a normal terminal tab via
  `_MakePane(args, …, existingConnection)`; `_SpawnClaudeSession` = fresh,
  `_RestoreClaudeSessions()` = load the persisted fleet **as Archived** (process-once via
  the `Engine::restoreMutex` load barrier — a 2nd window blocks until it's loaded, then skips),
  `_RestoreArchivedSession()` = the on-demand resume.
  `sessionId → Tab` lives in `_claudeTabs` (per window) for Activate / Archive / retitle. A
  session's **title is one value** (Explorer name == tab title == persisted `SessionInfo.title`):
  `_LaunchClaudeSession` **pins** it onto the tab (`Tab::SetTabText`); an Explorer rename routes
  through the `rename` callback → `_RenameClaudeSession` (registry + tab in lockstep); a WT tab
  rename (double-click / right-click **Rename Tab** / `renameTab` action, all via `Tab::SetTabText`)
  flows back through `_UpdateTitle` → `_SyncClaudeTitleFromTab`, which writes the registry (Rule #11).
  `_LaunchClaudeSession`/`_AdoptExternalSession` also **smart-name** an untitled session
  (`DeriveSessionTitle`) and **color the tab per working dir** (`_ApplyDirColorToTab` — the dir's
  persisted color or a stable auto one); a user color change flows `Tab::SetRuntimeTabColor` → the
  new `Tab::TabColorChanged` event → `_OnClaudeTabColorChanged`, which persists it to
  `dir-colors.json` and recolors every live tab in that dir (Rule #12).
- **Shared stdin:** the registry holds a per-session injector bound to that session's
  `ConptyConnection::WriteInput`, so the user's keystrokes and the scheduler's prompts both
  reach the same `claude.exe` stdin (Correctness Rule #3 binds the injector to the id).

## Building FAST

This machine: **i9-13900K — 32 threads (8 P + 16 E cores), 64 GB RAM.** The stock
`Invoke-OpenConsoleBuild` is slow because it runs msbuild with **no `/m`** (projects build
serially → 31 threads idle), builds the **whole** solution (tests/tools/samples), and
re-runs `nuget restore` every call. Per-file `/MP` is already enabled
(`src/common.build.pre.props:145`).

> ⚠️ **Hold the `build-launch` mutex before any full exe build, launch, or deploy** — see
> Deploy & run → *Concurrency lock*. (A lib-only compile-check, #6, doesn't relink the running
> exe and so needs no lock.)

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
- `Start-Process "shell:appsFolder\Agentmaster_56k4f06dsfp9r!App"`

### Concurrency lock (multi-agent) — REQUIRED before any build, launch, or deploy

There is exactly ONE dev instance and ONE build output tree (`…\CascadiaPackage\bin\x64\Debug`),
so the close→build→relaunch cycle is **process-global and destructive**. Two actors running it at
once — two AI agents, or an agent + a human — collide: one closes the instance the other just
launched, two msbuilds race on the same outputs, and the **exe link fails because a running
`WindowsTerminal.exe` locks the very `WindowsTerminal.exe` being relinked**. **REQUIREMENT: hold
the global `build-launch` mutex for the WHOLE cycle before you build (full exe), launch, deploy,
OR close our instance.** (A lib-only compile-check — Building FAST #6 — doesn't relink the running
exe, so it needs no lock.) The mutex is a filesystem lock (`tools/am-lock.sh`, built on `mkdir(2)`
atomicity) under `%USERPROFILE%\.agentmaster\locks\`, so it **persists across separate Bash calls**
(each Bash tool call is a fresh shell) and is shared by every agent.

```bash
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
# ... entire close → build → relaunch cycle while holding $TOKEN ...
bash tools/am-lock.sh release --token "$TOKEN"
```
`acquire` prints the token to **stdout** (capture it) and logs to stderr; `--wait 600` **queues**
behind another agent's build (picking up within ~1s of its release) instead of failing. Without
`--wait`, a contended `acquire` exits **3 (`BUSY`)** with the holder printed — back off, don't
spin. A crashed holder can't wedge it: a lock past its TTL (default 30 min) is auto-broken on the
next `acquire`. Token-checked `release` means one agent can't drop another's lock. Helpers:
`status` (who holds it + age), `refresh --token T` (extend a long hold), `acquire --force` (steal
now), `with --wait 600 -- <cmd>` (acquire → run one command → release). `bash tools/am-lock.sh --help`.

**Inner loop.** The loose layout is live (binaries update in place), but you **cannot
relink `WindowsTerminal.exe` while the app is running** — it locks the exe. So: close *our*
dev instance (spare the Store WT), rebuild, relaunch. The user has **standing-authorized
this close→build→relaunch cycle** ("always auto deploy") — run it without prompting; just
never touch the Store WT (it's not under our path — see Gotchas). **Hold the `build-launch`
mutex around the whole cycle** (acquire at step 0, release at step 4 — see *Concurrency lock*).
```bash
# 0. acquire the global mutex — REQUIRED (queues behind another agent's build)
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
```
```powershell
# 1. close ONLY our dev instance (path filter spares the Store WT — see Gotchas)
Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" |
  ? { $_.ExecutablePath -like 'K:\source\Agentmaster\*' } | % { Stop-Process -Id $_.ProcessId -Force }
# 2. build (full exe link)
pwsh -File .\tools\Build-Agentmaster.ps1 -NoRestore      # or: msbuild OpenConsole.slnx /t:Terminal\CascadiaPackage /m /p:Configuration=Debug /p:Platform=x64
# 3. relaunch
Start-Process "shell:appsFolder\Agentmaster_56k4f06dsfp9r!App"   # or: agentmaster
```
```bash
# 4. release the mutex (always — even if a step above failed)
bash tools/am-lock.sh release --token "$TOKEN"
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
- **`TextBox` has no `VerticalScrollBarVisibility`** in this projection (WPF puts it on the
  TextBox; UWP doesn't) — set the **attached** `ScrollViewer.VerticalScrollBarVisibility`
  instead (`ScrollViewer::SetVerticalScrollBarVisibility(box, …)`; also a C2039, hit on the
  multiline Flight-Plan compose box). Related glyph-alignment quirk: a bare symbol rides
  differently per element — a **`TextBlock`** reserves descent space in its line box (so a
  centered `"!"` sits high), while a **`FontIcon`** centers the glyph's ink. Render compose-bar
  symbols as `FontIcon` (even a text-font one, e.g. `Segoe UI` glyph `"!"`) so an icon row lines
  up; mixing `TextBlock` + `FontIcon` siblings misaligns them.
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
- **A pinned tab's `CloseButtonVisibility::Never` is silently undone by
  `_updateAllTabCloseButtons()`.** Setting `Never` on a tab once (as `_OpenAgentManagerTab` does)
  is NOT enough: that method loops EVERY tab and re-applies the theme's *global* close-button
  setting (`tab.CloseButtonVisibility(theme…)` → `IsClosable`), and it runs on every theme/
  settings apply — which happens *after* the Manager tab is created — so the X reappeared. Fix:
  **skip `_managerTab` in that loop.** Note hiding the X (`IsClosable`) is independent of
  drag-reorder: a non-closable tab can still be dragged/torn out, so non-movable needs its own
  guard (`TabViewItem.CanDrag(false)` + `_PinManagerTabFirst()` snap-back + a `_TryMoveTab`
  refusal). Context-menu Close/Move is a third axis (`DisableCloseAndMoveMenuItems()`).
- **Never reuse the `WindowsTerminalDev` package identity.** It belongs to the separate
  `K:\source\windowsterminal` checkout; registering the same identity tries to *replace*
  it and fails with a file-in-use lock (`0x80073CF6 / 0x80070020`) when its
  `OpenConsoleProxy.dll` is loaded. Our distinct `Agentmaster` identity sidesteps this.
- **Upstream "wt identity" assumptions are package-BLIND and break under our rename** (commit
  `404c04f72`). Several WT helpers hardcode the `wt.exe`/`WindowsTerminal` identity and silently
  misbehave for the `Agentmaster` package: (1) **`GetWtExePath()`** (`WtExeUtils.h`) resolves
  `<PFN>\wt.exe`/`wtd.exe`, which **doesn't exist** (our manifest registers the **`agentmaster.exe`**
  execution alias) — so every launcher built on it (`_OpenNewWindow`, the Reopen-Windows button, Jump
  List shortcuts) silently no-ops; (2) **`windowClassName`** (`WindowEmperor.cpp` — it seeds BOTH the
  single-instance **mutex** and the **window class**) is built from `WT_BRANDING` only, so our Debug
  "Windows Terminal Dev" branding **shared one single-instance identity with the real
  `WindowsTerminalDev`** (a launch could hand its commandline to the *other* window — defeating the
  whole point of the rename). Fix (both, package-aware): `GetWtExePath` picks `agentmaster.exe` when the
  package family starts with `Agentmaster`; `windowClassName` appends `GetCurrentPackageFamilyName()`
  for packaged builds. So coexistence with `WindowsTerminalDev` needs a distinct package identity (above)
  AND package-distinct **runtime** identifiers. Launch the alias by **name** (`agentmaster.exe`) — it
  resolves on PATH + follows the APPEXECLINK reparse and hands off; a full reparse-path
  `CreateProcess`/`Start-Process` bypasses the alias and cascades a fresh window instead.
- **Closing instances to relink.** Auto-closing **our** dev instance for the deploy inner
  loop is standing-authorized ("always auto deploy"): filter by
  `ExecutablePath -like 'K:\source\Agentmaster\*'` (matches our `WindowsTerminal.exe` *and*
  its `OpenConsole.exe` ConPTY hosts), `Stop-Process` them, build, relaunch — no prompt.
  But **never** touch the running **Store** Windows Terminal — it's the user's live session
  (under `Program Files\WindowsApps\…`, not our path). Never blanket-`taskkill` by image
  name; always path-filter so the Store WT is spared.
- **Packaging (`PRI210 / 0x800704c8`) can fail to overwrite `resources.pri`.** The registered
  loose-layout package keeps `src\cascadia\CascadiaPackage\bin\x64\Debug\resources.pri`
  memory-mapped, so MakePri's final overwrite-move dies with `0x800704c8` (ERROR_USER_MAPPED_FILE)
  — and `handle64` shows *no* owning user process (it's a registered-package/kernel mapping). The
  exe/dll already linked by then (your code is in the fresh `TerminalApp.dll`, copied into the
  layout *before* this step), so it's purely the PRI. Fix: **delete that `resources.pri` and
  rebuild** — MakePri then *creates* it fresh instead of overwriting a mapped target. (The Defender
  exclusion in Building FAST #3 also reduces the transient-lock variant.)
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
- **A ConPTY connection's process spawns on the control's first non-zero layout — never
  eager-`Start()` it before the control initializes.** `TermControl::_InitializeTerminal`
  (gated on `SwapChainPanel().LayoutUpdated`) is what calls `_core.Connection().Start()`, so a
  background/unfocused tab's `claude.exe` doesn't launch until the tab is first shown (hence
  "Restore all" re-opens lazily). Trying to force a background session to run by calling
  `connection.Start()` early from the app layer **crashes the app**: the connection's output
  then reaches `ControlCore`'s output handler before `_core.Initialize()` (gated on that same
  layout) has run → **AV `0xc0000005` in `Microsoft.Terminal.Control.dll`**, on every startup
  restore. The safe way to start a background session is to let its control initialize first
  (select the tab) — which is *why* startup ARCHIVES instead of auto-launching (Rule #6): no
  startup tabs ⇒ nothing to lazily-not-start, and a user Restore opens one focused tab that
  initializes normally.
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
- **Erasing a map node invalidates references to its key — copy the key first.** `_BindClaudeSession-
  ToTab`'s re-home block did `for (const auto& [oldId, w] : _claudeTabs) { … _claudeTabs.erase(oldId);
  _claudeOverlays.erase(oldId); }` — `oldId` is a reference INTO the `_claudeTabs` node, so the first
  erase freed it and the second `erase(oldId)` hashed **freed memory** → AV `0xc0000005` in
  `std::_Fnv1a_append_bytes` (`std::hash<wstring>`). Latent for ages (re-home was rare); the Fleet
  Observer's far more frequent correlation/re-home **exposed it**. Fix: `const std::wstring superseded
  = oldId;` BEFORE any erase. (General rule: never use a `[key, val]` structured-binding ref after
  erasing that element.)
- **A `+`-tab claude does NOT inherit our runtime env** (`AM_SESSION` / `CCMGR_HOOK_PIPE` / the PATH
  shim are all absent), so a hand-typed `claude` there fires **zero hooks** and is classified `wt`
  by `AM_SESSION` alone. WT regenerates a tab's child env from the **registry** (`til::env::regenerate`)
  unless `reloadEnvironmentVariables` is genuinely OFF, which drops these runtime-only vars — so the
  "Adopt any `claude`" hook fast-path is **degraded for `+` tabs** (a Manager-Launched session is fine:
  it sets env explicitly via `spec.env`). This is **why the observer keys binding on the roster
  correlation, not `AM_SESSION`** (Rule #13): a claude under one of our tabs' shells is ours
  regardless. The observer + the transcript-tail reconciler give it full detection + state without
  hooks; restoring the hook fast-path for `+` tabs (fixing the env regeneration) is a separate,
  non-blocking follow-up.
- **Symbolize a crash without cdb/WinDbg via the in-box DIA SDK + DbgHelp.** When the app AVs, the
  WER **Application Error** event already gives `Faulting module` + `Exception code` + `Fault offset`
  (the RVA), and a full dump lands in `%LOCALAPPDATA%\CrashDumps\`. No debugger is installed, but
  `msdia140.dll` + the **DIA SDK** ship with VS 2022 (`…\DIA SDK\include\dia2.h`, `…\lib\amd64\
  diaguids.lib`). A ~60-line C++ tool can `NoRegCoCreate(msdia140.dll)` → `loadDataFromPdb(TerminalApp.pdb)`
  → `findSymbolByRVA(rva, SymTagFunction)` + `findLinesByRVA` to map the fault offset to **function +
  source line** (link `diaguids.lib ole32 oleaut32 advapi32`). For the full call chain, `DbgHelp`'s
  `MiniDumpReadDumpStream` (ExceptionStream → faulting thread id; ThreadListStream → its stack memory;
  ModuleListStream → `TerminalApp.dll` base/size) + scanning the stack for in-module return addresses,
  symbolized through the same DIA session, reconstructs the stack top-down. This is how the re-home
  use-after-free above was pinned exactly.

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
9. **Adopt only on `SessionStart`; bind by `tabToken`, never guess.** A hook for a session we
   didn't Launch creates an `external` record ONLY on `SessionStart` (any other event for an
   unknown id is ignored — there's no connection to bind). Promotion to control correlates the
   `WT_SESSION` `tabToken` to a live ConPTY and binds the injector to THAT id (Rule #3); if no
   connection matches (a claude hosted outside this app), it stays observe-only — never bind to
   "the active tab".
10. **One engine per process (M9); a closing window must detach; records reference, never copy.**
    Exactly ONE `SessionRegistry` / `HooksBridge` / `Scheduler` for the whole process
    (`SharedEngine()`), shared by every window — never re-create them per `TerminalPage`, and
    never key the bridge on anything that collides across windows (the `<pid>` pipe is fine
    *because* there is one bridge). Each window registers its lens observer + adoption handler by
    **token** and detaches them on teardown (`RemoveObserver` in `~AgentManagerContent`,
    `RemoveAdoptionHandler` in `~TerminalPage`); the fleet loads **process-once**
    under the `Engine::restoreMutex` barrier (a second window blocks until it's loaded, then skips) so it can't
    double-insert NOR race its tab re-home against a half-loaded registry. Per-window persisted UI
    state (geometry + lens + ordered tab refs) is the `WindowRecord` (`windows/<id>.json`) and it
    must **reference** sessions by id, never copy them — `sessions.json` + `SessionInfo.live`
    remain the one session source of truth (Option 1).
11. **A session's title is ONE value — Explorer name == tab title == persisted `SessionInfo.title`.**
    `s.title` is the single source of truth; it is **pinned** onto the WT tab at launch / restore /
    adopt (`Tab::SetTabText`, so the tab stops floating with claude's volatile OSC title). Renaming
    from **either** side writes that one value and persists (the autosave-on-change observer): the
    Explorer-tree **Rename…** routes through the `rename` callback → `_RenameClaudeSession` (registry
    + tab); a WT tab rename (double-click / right-click **Rename Tab** / `renameTab` action — all
    funnel through `Tab::SetTabText` → `PropertyChanged("Title")` → `_UpdateTitle`) flows back via
    `_SyncClaudeTitleFromTab`. Equality guards make an already-in-step sync a no-op (no loops); an
    emptied override (`ResetTabText`) re-pins. On **adopt**, a name the user already gave the `+` tab
    wins (mirrored into the registry); else the tab is pinned to the managed name. Don't reintroduce
    a separate tab title or scrape claude's OSC title for the name.
12. **A tab's color is ONE value per working directory.** Every Claude tab in a dir shares one
    color, persisted to `dir-colors.json` (keyed by `NormDirKey` — slash/case/trailing-normalized).
    On launch/restore/adopt `_ApplyDirColorToTab` paints the tab from the dir's persisted color, or
    a stable auto palette color (hash of the dir; persisted so it survives). A user color change
    (`Tab::SetRuntimeTabColor`/`Reset` → `TabColorChanged` → `_OnClaudeTabColorChanged`) **persists
    it for the dir AND recolors every live tab in that dir** (filesystem-aware, Rule #8); a reset
    drops the entry. A de-dupe vs the persisted color makes our own launch/propagation writes
    no-ops — don't regress that, it is what keeps propagation from looping. (The default *name*,
    `DeriveSessionTitle`, only seeds an *untitled* session — a real/renamed title wins, Rule #11.)
13. **Fleet Observer: rostered == ours; correlate by `WT_SESSION`, never guess; provenance: push
    wins for state.** A claude correlated to a tab in OUR published roster (its shell == the ConPTY
    root of one of our tabs) is running in one of our windows, so it is **OURS** (`RunningApp::
    Agentmaster`) and bindable **regardless of `AM_SESSION`** — a hand-typed `+`-tab claude does NOT
    reliably inherit `AM_SESSION` (WT regenerates a tab's env; see Gotchas), so the **roster
    correlation is the authoritative bind signal**, and `AM_SESSION` is only the secondary signal
    for the census of NON-rostered claudes (a background daemon we spawned, or telling our instance
    from another's / a real WindowsTerminal). Correlation keys on the **exact `WT_SESSION`** (==
    `ITerminalConnection::SessionId()`) — never PPID ancestry, never the stale tab cwd, never "the
    active tab". `ObserveClaude` **enriches facts but NEVER sets `SessionState`** — push hooks + the
    transcript tail own state (Rule #1/#7); a hooked and an un-hooked claude converge on the same
    record. The observer **only reads** (PEB / Toolhelp / filesystem) — it must NEVER write to a
    shell's stdin (the invisibility invariant). **Adopting** an external (the Explorer-Tree
    EXTERNAL scope's *Adopt*) upholds this — it never injects into / kills the foreign process; it
    resumes the external's *conversation* into a NEW managed tab (`claude --resume <id>`, id from
    `ResolveSessionId`, transcript-gated → fresh if none) and leaves the original running. The
    transient enrichment fields are **never persisted** (`Persistence.cpp`); the observer
    re-derives them each run.
14. **Conversation identity is by transcript CREATION time ≈ process start, not newest-mtime.** A
    claude's OWN transcript is created when it first writes — at/after its process start.
    `ResolveSessionId` (for a bare claude with no explicit `--session-id`/`--resume <guid>`) picks
    the cwd's transcript whose **creation time is closest to (and not significantly before) the
    claude's start**, and REJECTS transcripts created before it started. This is what keeps **two
    claudes sharing one cwd** bound to their OWN conversations (newest-mtime collapses them onto
    whichever is momentarily most active), and resolves a **never-prompted** claude to `""` (no card
    until its first prompt — §11d) rather than collapsing it onto a stale leftover transcript. An
    explicit `--session-id` / `--resume <guid>` is authoritative and wins (collision-free, known
    before the transcript exists).

## Conventions

- Mark our additions with `Agentmaster`. Keep the upstream MIT `LICENSE`/`NOTICE`.
- Keep the diff against upstream minimal where practical (additive files, small touches at
  integration points) so rebasing onto `microsoft/terminal` stays cheap.
- Build artifacts (`bin/`, `packages/`, `Generated Files/`) are gitignored — never commit them.
