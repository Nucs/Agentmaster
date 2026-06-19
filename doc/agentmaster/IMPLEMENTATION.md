# Agentmaster — Implementation Plan

**Agentmaster** is a fork of Windows Terminal (`microsoft/terminal`, MIT) that adds
first-class management of multiple **Claude Code** sessions across many working
directories — with full user interaction *and* full programmatic control.

- Upstream: forked at `v1.24.2372` (`main`), local work branch `agentmaster`.
- Build entry: **`OpenConsole.slnx`** (new XML solution format).
- License: MIT — keep the upstream copyright/`NOTICE`. Our additions are marked
  `Agentmaster` and grouped under `doc/agentmaster/` and `src/cascadia/TerminalApp/AgentMaster/`.

> **Status (current).** Milestones **M0–M10 are complete and shipped**, and the project has gone
> well beyond this original plan — the **Fleet Observer**, **Codex** as a managed agent, the
> **`agentmaster` CLI**, the **Sessions browser** + **Archive page**, the **per-tab summary panel**,
> and **per-install profiles** all shipped since. This file is the original implementation *plan*;
> the authoritative, continuously-updated status by area is [`../../CLAUDE.md`](../../CLAUDE.md). The
> milestone list below is extended past M8.

## Converged design

- **Design A — Native Graft.** The Claude-management "brain" lives *inside* the
  forked app in C++/WinRT. One process, native fidelity, deepest control.
- **Manager tab = C1 "Linked Lenses".** A pinned, leftmost, non-closable tab
  (tab 0, open by default) split into three synced regions:
  - **Triage Board** (top) — sessions as cards in state columns
    (Running · Waiting-for-you · Needs-approval · Error · Idle/Done), plus an *External* column.
  - **Explorer Tree** (bottom-left) — `M` working directories → their `N` sessions.
  - **Flight Plan** (bottom-right) — per-session prompt queue + Autopilot.
  - Selection is bidirectional **and tab-synced both ways** (card ⇄ tree node ⇄ terminal tab; pick a
    dir ⇒ filter the board); Activate/Rename reach a session in **any** window.
- **Flight Plan / Autopilot.** A per-session queue of prompts. On **turn-complete**
  the scheduler auto-sends the next prompt. See "Correctness rules".

## Integration points (grounded in the 1.24 codebase)

WT 1.24 uses a pluggable **pane-content** model. A `Tab` hosts panes; each pane's
content implements `IPaneContent`. Prior art: `ScratchpadContent`, `SettingsPaneContent`,
`SnippetsPaneContent`.

| Concern | Location |
| --- | --- |
| Content interface | `src/cascadia/TerminalApp/IPaneContent.idl` |
| Simplest example to mirror | `src/cascadia/TerminalApp/ScratchpadContent.{h,cpp}` |
| Content dispatch by type string | `TerminalPage::_MakePane` — `TerminalPage.cpp:4073` (add `"agentManager"`) |
| Create tab at a position | `TerminalPage::_CreateNewTabFromPane(pane, insertPosition)` — `TabManagement.cpp:241` |
| Track a special tab (pattern) | `_settingsTab = _CreateNewTabFromPane(...)` — `TerminalPage.cpp:4822` |
| Project that compiles content classes | `src/cascadia/TerminalApp/TerminalAppLib.vcxproj` |
| Per-session terminal I/O | `src/cascadia/TerminalConnection/ConptyConnection.{h,cpp,idl}` |

Notes:
- `ScratchpadContent` has **no `.idl`** — content classes are implementation-only
  (`winrt::implements<…, IPaneContent>` + `BasicPaneEvents`). `AgentManagerContent`
  follows suit: no projection/MIDL changes needed.
- Input injection uses `ITerminalConnection::WriteInput(text)` (UTF‑16). Mind the
  null-termination crash (upstream issue #17697): always send a null-terminated buffer.

## Data model

`src/cascadia/TerminalApp/AgentMaster/SessionModels.h` (plain C++ for now; the XAML
layer in M6 wraps these in observable view-models):
`SessionState`, `AutopilotMode`, `PromptGate`, `PromptStatus`, `QueuedPrompt`,
`ApprovalPolicy`, `AutopilotState`, `SessionInfo`. (`SessionInfo` later gained **`kind`** (Claude /
Codex) + **`codexSessionId`** for managed Codex; `TabKind` — incl. Codex — backs the M10 window
records.)

## Hooks bridge

State is authoritative via **Claude Code hooks** (now reconciled by a transcript-tail reader + the
Fleet Observer for dropped / out-of-order events), never screen-scraping. See
[`HOOKS.md`](./HOOKS.md). Each `claude.exe` is spawned with `CCMGR_SESSION_ID`; hooks are flattened
to a 9-field TAB-separated wire line (event · sessionId · cwd · isQuestion · permission · tool ·
tabToken · prompt · ts, **payload-first** session id) over a local named pipe; the registry maps
events to sessions and drives the Triage Board + Autopilot.

## Correctness rules (do not regress)

> These five are the originals; the full **current** set is **15 rules** in
> [`../../CLAUDE.md`](../../CLAUDE.md) (*Correctness rules*).

1. **"Waiting-for-you" is three states.** A session is *ready* for an auto-send when it is
   turn-complete (`Stop` → `WaitingForInput`) **or** sitting **Idle** with no turn in progress (a
   freshly launched / just-resumed plan must start, not wait for a `Stop` it will never emit).
   `Notification(permission)` → Approval Policy (NOT the queue). A `Stop` whose last message is a
   question → **Held** by the question-guard.
2. **Tree `Enter` = Activate** (jump to the session's live tab). It must **never** be
   forwarded to `ITerminalConnection::WriteInput` (that would submit a stray CR).
3. **Bind queue → sessionId**, never "the selected session" at send time.
4. **Idempotent sends.** Mark `Sent` atomically + persist; survive restart without replay.
5. **Backstops.** `stopOnError`, `maxAutoSends`, global pause/kill, `pauseOnHumanInput`.

## Milestones (mirror the task list)

- **M0** ✅ Fork, branch, toolchain, map integration points.
- **M1** ✅ This doc + scaffold `AgentManagerContent` & data model.
- **M2** ✅ Baseline build of vanilla WT (`OpenConsole.slnx`), confirm runnable.
- **M3** ✅ Register `AgentManagerContent` in `TerminalAppLib.vcxproj` + `_MakePane` dispatch.
- **M4** ✅ Auto-open Manager tab pinned leftmost, non-closable, on startup (+ M4.1 own identity, deployed).
- **M5** ✅ `SessionRegistry` + `claude.exe` spawn + hooks bridge (live state). Engine is plain
  C++ (`AgentMaster/`): `HookEvents.h` (state machine), `HookWire.h`, `SessionRegistry`,
  `HooksBridge` (local named-pipe server), `ClaudeSpawn` (id + forwarder + `--settings`
  hooks recipe). Wired into `TerminalPage` (`_InitAgentmasterEngine`, `_SpawnClaudeSession`)
  and the Manager UI (Launch button). 67/67 standalone checks pass incl. a live pipe
  round-trip (`AgentMaster/tests/`).
- **M6** ✅ C1 "Linked Lenses" UI (`AgentManagerContent`): Triage Board + Explorer Tree +
  Flight Plan, built imperatively, snapshot-driven from the registry (cross-thread refresh
  via DispatcherQueue), bidirectional selection + directory scope; Explorer `Enter`=Activate
  / `Del`=**Archive** (never injects — Rule #2); Flight Plan queue editing (add/reorder/delete/Send
  now) + per-session Autopilot mode selector. Shipped + live-verified.
- **M7** ✅ Autopilot scheduler (`AgentMaster/Scheduler`): a pure `DecideAdvance()`
  (every branch unit-tested) + a worker thread on the registry's advance seam. On a clean
  turn-complete it sends the next Pending prompt in Full mode, arms a one-click confirm in
  SemiAuto, Holds behind the question-guard (transient — auto-resumes), honors Manual gate,
  pause-on-human-input, maxAutoSends, stopOnError, and a global Pause-all backstop; sends
  are idempotent (atomic mark-Sent before inject). UI: per-session mode selector, confirm
  banner, Pause-Autopilot toggle. 79/79 checks pass; runtime check pending deploy.
- **M8** ✅ Persistence + plan templates + apply-to-many (`AgentMaster/Json.h`,
  `AgentMaster/Persistence`): a tiny dependency-free JSON value/parser/printer; sessions
  (queue + autopilot + metadata) and named plan templates (de)serialize to JSON under
  `%USERPROFILE%\.agentmaster\` (restore preserves `Sent` statuses — no replay). UI to save
  a session's queue as a template, apply a template to the selected session, or broadcast
  it to every session in a directory; sessions autosave on every registry change. 102/102
  checks pass (incl. JSON + round-trips + template apply). **Lifecycle = Open ⇄ Archived (no
  discard; transcripts are never deleted).** On startup `_RestoreClaudeSessions()` loads the
  persisted fleet into the registry as **Archived** and does **NOT** auto-launch it (Rule #6 — a
  background `claude --resume` before the control initializes crashes the app); the app opens to
  just the Manager tab. Restore is **on demand** (per-row or bulk, from the **Archive page**) and
  **transcript-gated**: `claude --resume <id>` only when Claude has a conversation for that id, else
  a fresh session. Closing a tab **archives** (keeps the record, `live=false`) — there is no
  kill/discard. Verified live (`[restore]`/`[resume]`/`[restore-fresh]`/`SessionStart` in
  `<profile>/hooks.log`).
- **M9** ✅ One process-wide **`SharedEngine`** (`AgentMaster/Engine.{h,cpp}`): the WindowEmperor's
  many windows share ONE registry / bridge / scheduler / observer over one pipe instead of
  per-window registries racing on `sessions.json`. Each `TerminalPage` is a *lens* over the one
  fleet; observers/handlers attach by token and detach on teardown (Rule #10); the fleet loads
  process-once under a load barrier (`Engine::restoreMutex`).
- **M10** ✅ Per-window **`WindowRecord`** (`windows/<id>.json`) — geometry + the Manager lens
  (selection / scopes / splitters / collapsed dirs) + an ordered list of tab **refs** (Option 1:
  references sessions by id; `sessions.json` stays the one source of truth; a tab ref carries a
  `TabKind`, incl. Codex). A debounced autosave captures it; reopen re-applies geometry + lens,
  **re-homes the whole workspace** (Claude *and* Codex sessions resumed in place, shell tabs
  replayed at their real cwd), and re-selects the focused tab by stable identity. The open-at-exit
  set reopens via an `open-windows.json` manifest. (This **superseded** the original M9–M14 ladder;
  see [`PERSISTENCE.md`](./PERSISTENCE.md).)

### Beyond the milestones — shipped since (authoritative status: [`../../CLAUDE.md`](../../CLAUDE.md))

- **Fleet Observer** (O1–O7, [`OBSERVER.md`](./OBSERVER.md)) — the out-of-band PULL floor beneath the
  hooks: detects, correlates, and enriches **every** Claude (and Codex) session by reading each
  process's PEB + transcript, keyed on `WT_SESSION`. Read-only; finds even a hand-typed `claude`
  that fires zero hooks.
- **Codex** (the OpenAI Codex CLI) — a first-class **managed** agent: observe + rollout-tail state +
  the full launch / restore / window-restore / adopt lifecycle (a two-id model — our durable handle
  + the rollout uuid). Driving its TUI (injector + Autopilot) is the one part still deferred.
- **`agentmaster` CLI** ([`CLI.md`](./CLI.md)) — read-only fleet introspection from any shell, app
  up or down (`show`/`list`/`sessions`/`tabs`/`windows`/`external`, `--self`, `--json`).
- **Sessions browser** ([`SESSIONS.md`](./SESSIONS.md)) — a full-window page over every on-disk
  session: two-phase (indexed + ripgrep) search with in-process scope attribution; Resume / Fork /
  Jump.
- **Archive page** — the full-window, grouped-by-window archive (sortable/searchable table + detail,
  bulk restore, reopen-whole-window) that replaced the in-content modal.
- **Per-tab overlay** ([`TAB_OVERLAY.md`](./TAB_OVERLAY.md)) — the link badge, the observe badge on
  every classified tab, and the pencil-toggled **summary panel**; plus the tab-strip status dot.
- **Per-install profiles + release/dev identity split** ([`PROFILES.md`](./PROFILES.md)) — first
  launch auto-selects the per-identity default; the two packages install side-by-side.
- **State-engine hardening** ([`STATE.md`](./STATE.md), [`HOOKS.md`](./HOOKS.md)) — the
  SessionScanner reconciles dropped / out-of-order hooks (interrupt, blocked-on-question,
  presence-idle, subagent activity, `/clear`·`/compact`·`/resume` divergence) on top of the ordered
  hook state machine.

## Build & run

```powershell
# from K:\source\Agentmaster — parallel build of just the app target (see CLAUDE.md "Building FAST")
pwsh -File .\tools\Build-Agentmaster.ps1            # first build (restores packages)
pwsh -File .\tools\Build-Agentmaster.ps1 -NoRestore # inner loop
```

Deploy the loose layout (`Add-AppxPackage -Register …\bin\x64\Debug\AppxManifest.xml`), which
registers the **dev** identity `AgentmasterDev` (alias `agentmasterdev`), then launch it. The full
build/deploy details — the build mutex, the inner-loop close→build→relaunch cycle, and the release
pipeline — live in [`../../CLAUDE.md`](../../CLAUDE.md) (*Building FAST* / *Deploy & run* /
*Releasing a public version*).

> First build is long (NuGet restore of Microsoft.UI.Xaml + cppwinrt); incremental rebuilds are
> quick. See `CLAUDE.md` for Defender exclusions, lib-only compile checks, and the build mutex.
