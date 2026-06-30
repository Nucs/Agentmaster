# Agentmaster — Design

> The complete design for **Agentmaster** — a fork of Windows Terminal that turns it into a
> manager for many **Claude Code** (and now **Codex**) sessions. This document is the "what & why +
> full architecture." Companions: [`IMPLEMENTATION.md`](./IMPLEMENTATION.md), [`HOOKS.md`](./HOOKS.md),
> [`OBSERVER.md`](./OBSERVER.md), [`STATE.md`](./STATE.md), [`TAB_OVERLAY.md`](./TAB_OVERLAY.md),
> [`SESSIONS.md`](./SESSIONS.md), [`PERSISTENCE.md`](./PERSISTENCE.md), [`PROFILES.md`](./PROFILES.md),
> [`CLI.md`](./CLI.md).
>
> **Status:** M0–M8 are shipped + live-verified, as are the Fleet Observer, the per-window workspace
> layer, the Sessions browser, the Archive page, per-install profiles, the read-only `agentmaster`
> CLI, and a first-class **managed Codex** lifecycle (driving its TUI is deferred). Where this doc
> still phrases a now-shipped feature in future terms, a **Shipped:** note reconciles it; the
> authoritative current feature set is the repo-root `CLAUDE.md`.

---

## 1. Vision & goals

Run **N Claude Code sessions across M working directories** from one native Windows app, where you can:

- **Fully interact** with any session — it's a real `claude.exe` in a real terminal (colors, mouse, scrollback, the genuine Ink TUI), not a proxy or scrape.
- Give the app **100% programmatic control** — inject prompts, read output, observe state, and drive sessions on a schedule.
- **See and triage the whole fleet** — when N×M is large, surface what needs you and hide the calm.
- **Automate sequences** — queue a plan of prompts per session and let a **Tests Autorunner** advance it as each turn completes. *(Dev-only — see §9.4.)*

The headline differentiator: **full human interaction and full machine control coexist on the same session**, because both write to the same ConPTY stdin.

**Shipped:** the substrate is agent-agnostic, and **Codex (the OpenAI Codex CLI) is now a
first-class managed agent** alongside Claude — observe + state + the full launch/restore/window-
restore/adopt lifecycle (driving its TUI is deferred; §17). "Claude session" below is the primary
case; a managed Codex rides the same path with the divergences noted inline.

### Success criteria
- Launch ≥10 sessions across several repos and never lose track of which need attention.
- Queue a 10-step plan and have it drive a session to completion unattended (with guardrails), while you can still type into it.
- Zero "silent wrong action" (e.g., autorunner answering a permission prompt with a planned prompt).

## 2. Non-goals

- Not a Claude Code replacement or a reimplementation of its agent loop — we host the real CLI.
- Not a general remote-access tool (M-series may add remote viewing, but local-first).
- Not trying to upstream into `microsoft/terminal`; we keep a clean fork (minimal diff for rebasing, but it's ours).
- Not a cloud service — all orchestration is local; the only IPC is a local named pipe for hooks.

## 3. Why fork Windows Terminal ("Design A — Native Graft")

We evaluated forking the whole app (A), a thin fork + external brain (B), and harvesting just the engine (C); A won for native fidelity and deepest control. Rationale:

- WT is **MIT** — fork/modify/ship freely (keep the notice).
- Every WT pane already runs a child process over **ConPTY** via `ITerminalConnection`. That is *exactly* the substrate we need: a real terminal we can also write to and read from programmatically.
- WT 1.24's **pluggable pane-content model** (`IPaneContent`) gives a clean extension seam — we add UI as content classes without touching the renderer.
- We inherit tabs, panes, settings, theming, the renderer (AtlasEngine), and the connection layer for free.

The "brain" (orchestration, scheduling, state) lives **in-process in C++/WinRT** inside the fork.

## 4. Architecture overview

```
┌── Agentmaster.exe  (forked WindowsTerminal.exe, C++/WinRT) ───────────────────┐
│                                                                                │
│  Tab 0: MANAGER (pinned, leftmost, non-closable)      Tab 1..n: live sessions  │
│  ┌─────────────────────────────────────────┐         ┌──────────────────────┐ │
│  │ C1 "Linked Lenses" UI                    │         │ TermControl          │ │
│  │  Triage Board / Explorer Tree / Flight   │ binds   │  └ ClaudeConnection ──┼─┼─► claude.exe (cwd=K:/api)
│  │  Plan                                    │◄────────┤ TermControl          │ │
│  └───────────────┬─────────────────────────┘         │  └ ClaudeConnection ──┼─┼─► claude.exe (cwd=K:/ui)
│                  │ observes / commands                └──────────────────────┘ │
│        ┌─────────▼──────────┐   ┌──────────────┐   ┌────────────────────────┐  │
│        │ SessionRegistry    │──▶│ Scheduler /  │   │ Hooks bridge           │  │
│        │ (SessionInfo × N)  │   │ Tests Autorunner    │   │ (named-pipe listener)  │  │
│        └─────────▲──────────┘   └──────┬───────┘   └───────────▲────────────┘  │
│                  │ state updates       │ WriteInput            │ hook events    │
└──────────────────┼─────────────────────┼───────────────────────┼───────────────┘
                   └───────────── ConPTY stdin/stdout ────────────┘
                                                          claude.exe hooks ──┘
```

Layers:
- **UI** — the Manager tab (C1) + the normal session tabs. Pure XAML/WinRT, binds to the registry.
- **SessionRegistry** — the single source of truth: one `SessionInfo` per session, across M dirs.
- **ClaudeConnection** — a session's `ITerminalConnection` (ConPTY + `claude.exe`); the read/write channel.
- **Hooks bridge** — receives authoritative state events from Claude Code hooks over a local pipe.
- **Scheduler / Tests Autorunner** — drives Auto Testing queues by injecting prompts on the right signal.

## 5. The session substrate (ConPTY)

A **session** = one `claude.exe` launched in a working directory on a **pseudo-console**:

- **Input (you + us):** the `TermControl` forwards your keystrokes to the pty stdin; the Scheduler injects prompts via `ITerminalConnection::WriteInput(text)`. Same stream → interleaved, coexisting control. (Mind the null-termination crash, upstream #17697 — always send a terminated buffer.)
- **Output:** the pty stdout drives the renderer; we also keep a readable mirror (serialized buffer) for previews and light parsing.
- **Exclusive control when needed:** flip the pane **read-only** to block user input while we drive, then re-enable.
- **State:** never inferred from the redraw stream (Ink repaints constantly) — it comes from **Claude Code hooks** (§7–8).

## 6. Session model & registry

`SessionRegistry` owns all sessions and is what the UI binds to. Core types (see `src/cascadia/TerminalApp/AgentMaster/SessionModels.h`):

```
SessionInfo { id, title, workingDir (the M axis), branch, state,
              lastActivity, queue: QueuedPrompt[], autorunner: AutorunnerState }
SessionState  = Idle | Running | WaitingForInput | NeedsApproval | Error | Done
QueuedPrompt  { id, label, text, status(Pending|Sent|Held|Skipped|Failed),
                gate(OnTurnComplete|AfterDelay|Manual), guardPattern, dependsOn, attempts }
AutorunnerState{ mode(Off|SemiAuto|Full), throttleMs, stopOnError, pauseOnHumanInput,
                maxAutoSends, approval: ApprovalPolicy }
ApprovalPolicy{ pauseForHuman, autoApproveTools[] }
```

- **M × N structure** is first-class: `workingDir` is the grouping axis; multiple sessions can share a dir.
- **Isolation (design intent, not yet built — §18):** new sessions could be placed in **git worktrees** so parallel agents don't collide (own branch/dir).
- **Correlation:** each session's `id` is injected into its `claude.exe` as `CCMGR_SESSION_ID` so hook events map back (§8).

**Shipped (schema/correlation deltas):** `SessionInfo` gained an `AgentKind kind` (default `Claude`)
and, for Codex, a `codexSessionId` (Codex can't pin a session id at launch, so our `id` is a minted
durable handle and the rollout uuid is carried here — the "two-id model"; §17), plus many transient
runtime facts (`live`, `external`, `tabToken`, the Fleet Observer's process enrichment, presence —
none persisted). **Correlation is no longer hook-only:** the **Fleet Observer** (§8a) binds every
session by **`WT_SESSION`** out-of-band, so a hand-typed `claude` that fires zero hooks is still
detected and bound; `CCMGR_SESSION_ID` / hooks remain the low-latency push.

## 7. State machine (hook-driven)

```
            UserPromptSubmit / inject
   Idle ───────────────────────────────▶ Running
     ▲                                      │ Stop hook
     │ (next turn)                          ▼
     └──────────────────────────── WaitingForInput ──▶ (Tests Autorunner.tryAdvance)
                                          │
   Notification(permission) ──▶ NeedsApproval ──▶ ApprovalPolicy
   turn ends in error ───────▶ Error      SessionEnd ──▶ Done
```

The crucial subtlety: **"the agent is waiting" is three different states**, and only the first should pull a planned prompt — see §10 and the correctness rules.

## 8. Hooks bridge

Authoritative state with no screen-scraping. Full contract in [`HOOKS.md`](./HOOKS.md); summary:

- On spawn, set `CCMGR_SESSION_ID=<guid>` on the child and write a hooks config whose commands post `{sessionId, cwd, event, …}` to a **local named pipe** (`\\.\pipe\agentmaster.<pid>`), read on a dedicated thread. (Fallback: the forwarder discovers the pipe via a `bridge.json` file when it didn't inherit the env; the always-on floor beneath the lossy push is the Fleet Observer — §8a.)
- Event → effect: `SessionStart`→register/Idle; `UserPromptSubmit`→Running (also confirms our injected prompt landed → idempotency); `PreToolUse/PostToolUse`→activity; `Notification(permission)`→NeedsApproval + ApprovalPolicy; `Stop`→WaitingForInput + `Tests Autorunner.tryAdvance`; `SubagentStop`→info; `SessionEnd`→Done.
- The hook payload carries a best-effort `lastMessageIsQuestion` flag that feeds the question-guard.

### 8a. Fleet Observer — the PULL correlation/state floor (shipped)

**Shipped:** hooks are a *lossy push* — a `claude` you type yourself (a shell function/alias shadows
the PATH shim; WT regenerates a `+`-tab's env, dropping our runtime vars) fires **zero** hooks.
Beneath the push lives the **Fleet Observer** ([`OBSERVER.md`](./OBSERVER.md)) — a process- +
transcript-driven survey that detects, correlates, and enriches **every** Claude (and Codex) process
**out-of-band** (reads each `claude.exe`'s PEB + transcript, correlating by **`WT_SESSION`**) with
**no hooks, no shim, no settings, and zero writes to any shell**, guaranteeing a session is **seen
and bound** regardless. Push hooks + the transcript tail still own **state** (the observer only
enriches facts, never sets `SessionState`; the Observer-owned PULL state engine is in
[`STATE.md`](./STATE.md)). Codex, which has no hooks at all, runs entirely on this PULL model (a
3-state Running/Waiting/Idle floor — no approval/error event exists in a rollout).

## 9. The Manager tab — C1 "Linked Lenses"

A pinned, leftmost, non-closable tab (tab 0, open by default), implemented as an `IPaneContent` (`AgentManagerContent`). Three regions over **one shared model**, with bidirectional selection sync.

### 9.1 Layout
```
MANAGER (tab 0 · pinned · non-closable)
┌─ TRIAGE BOARD ─────────────────────────────── [scope: K:/api] ─┐
│ Running      │ Waiting-for-you │ Needs-approval │ Error          │
│ api-test ⚙2/6│ api-fix ⚙4/10 ◄ │      —         │  ui-test        │
├──────────────────────── drag divider ──────────────────────────┤
│ EXPLORER TREE              │ AUTO TESTING — api-fix  Tests Autorunner[▶] │
│ ▾ K:/api  3 ·1◐ ◄scoped    │ next: turn-complete · 3/10 sent      │
│   ● api-fix  waiting ◄hl   │ ✓ add unit tests                     │
│   ● api-test running ⚙     │ ⏳ add docs to API     [turn-done]    │
│   ○ api-docs idle          │ ⛔ commit  (held: agent asked a Q)    │
│ ▸ K:/ui   5                │ [+Add][Send now][Edit][↑][↓][Tmpl]   │
│ ▸ K:/docs 1                │— live peek —  [Focus][ESC][Kill]     │
└────────────────────────────┴───────────────────────────────────┘
```

### 9.2 Triage Board (top) — *triage*
- Columns are hook-driven states: **Running · Waiting-for-you · Needs-approval · Error** (+ an **Idle/Done** column and the Observer's **External** census). Cards = sessions tagged with dir/branch/task/elapsed; they auto-move as hooks fire.
- The **Waiting / Needs-approval** columns are the work queue — act only on what's blocked. Cards show an autorunner badge **⚙ sent/total**.

### 9.3 Explorer Tree (bottom-left) — *structure*
- Roots = the M working directories (branch/worktree + roll-up like "3 · 1◐"); children = that dir's sessions with status badges (●running ◐waiting ○idle ✕error). Collapse to scale.
- **Keymap:** `↑↓` select (preview only); `→←` expand/collapse dir; **`Enter` = Activate** → jump to the session's live tab (NEVER inject — see invariants); `Del` = archive (confirm); type = filter.
- **Shipped — scope + sort toggles.** A **3-way scope toggle (LOCAL · GLOBAL · EXTERNAL)** — this
  window's sessions · all windows' · the Observer's observe-only externals (real WindowsTerminal +
  cmd/console claudes, *and* Codex), persisted per window in the lens (`ManagerState.treeScope`); the
  Triage Board header has a matching 2-way LOCAL/GLOBAL twin. After it, a **sort toggle — NEWEST ·
  OLDEST · MOST ACTIVE · A–Z · BY PID** (`ExplorerSort`) orders the dir groups and rows within, a
  GLOBAL setting (`AppSettings.treeSort`); a **↻ refresh** re-pulls + `Wake()`s the observer. Activate
  is **cross-window** (fans out through the engine's activate sinks to the hosting window).

### 9.4 Auto Testing (bottom-right) — *plan & automate*
- **⚠ DEV-ONLY.** Auto Testing / Tests Autorunner ships only in the **AgentmasterDev** package
  (`Profiles::IsDevPackage()`). In a **release** install this whole pane is the read-only **Summary**
  view only — no `[Summary | Auto Testing]` toggle, no Tests Autorunner / queue / compose — and the
  autorunner scheduler is **never started** (`Engine.cpp`), so nothing auto-sends. Everything below
  describes the dev experience.
- The selected session's **prompt queue** + **Tests Autorunner** + the compose row. Add/edit/reorder/delete prompts; per-item **gate** badge (turn-done / delay / manual) and **guard**; `Send now`, `Test Templates`, import/apply-to-many. Detailed mechanics in §10.
- **Shipped — the as-built UI.** **Tests Autorunner is a colored toggle in the Auto-Testing header**
  (gray ○ Off / amber ◐ Semi / green ● Full, cycling on click), not a combo. The **compose row** is
  three icon buttons (**eye** = Focus/jump to the tab · **!** = Send now, confirms first · **envelope**
  = Add to queue) beside a growing multiline textarea; **Templates** collapse behind a paper icon.
  Per-message actions (Move up/down/Delete; Archive session) live on a **right-click menu over the
  messages**. The plan reflects **every** message a session received — a chronological **SENT**
  summary (rows tagged **flight** = we injected vs **typed** = the human typed it) over the
  **UPCOMING** queue. Selecting an **external** shows its conversation **read-only** (no
  queue/Tests Autorunner — we host no ConPTY for it).

### 9.5 Selection sync (the "Linked Lenses" core)
One shared selection over the registry:
- Select a **card** → tree expands/scrolls/highlights that session + loads its Auto Testing.
- Select a **directory** in the tree → the board filters to that dir's lanes.
- Everything stays consistent because there is one model, three views.

**Shipped — selection sync is BOTH ways (tab ⇄ Manager).** A board card mirrors the tree row
(single-click selects, **double-click Activates** the live tab cross-window, right-click = same menu)
— the board/tree → tab half. **The reverse holds too:** switching to a session's tab selects it in
the Manager (`_OnTabSelectionChanged` → `_SyncManagerSelectionToTab` → `SelectSession`), lighting up
the card + tree row + Auto Testing. The selection is part of the per-window lens, so it persists across
restart via the `WindowRecord` autosave.

### 9.6 Other manager paradigms (deferred, optional toggle-views)
A2 Mission-Control Wall (live tiles + semantic zoom), A4 Command-Palette switcher, A5 Spatial graph — not built initially; C1 is the primary. They could become alternate views over the same registry later.

### 9.7 Per-tab link badge (the overlay) — *the here-and-now lens*
The Manager tab is the *fleet* view; each Claude session tab also carries a small **link badge**
pinned to the **top-right of its terminal**, so the tab ⇄ Agentmaster relationship is legible
while you work *inside* a session: hook-driven status, whether/how Tests Autorunner is driving it
(`Manual` / `Semi` / `Full`), link state (surfaced only when *not* linked), queued count, and — on
hover/click — controls (Tests Autorunner cycle, Send-now, queue peek, Jump-to-Manager) + a contextual
SemiAuto confirm. Dim until hover; off-switchable. It only *reflects* registry/scheduler state
and *requests* the same actions the Auto Testing does — never a second source of truth. Full
spec: [`TAB_OVERLAY.md`](./TAB_OVERLAY.md).

**Shipped — the as-built badge (richer than the sketch).** Built as `AgentTabOverlay`. Beyond the
linked badge it shows on **every** classified tab as a registry-less **observe badge**
`○ <kind> · unlinked` (kind = `pwsh` / `cmd` / unprompted `claude` / `codex`) that **flips in place**
as activity changes — a `pwsh` tab → `claude` the moment you run it → the full linked badge on its
first prompt. The linked badge's **row 1** reads `status · actions · autorunner · queue` (**link state
shows only when *not* linked**) over a dim **second row** `<workdir folder>/<live branch>`; its
**always-shown row-1 action cluster** is a folder button (Open Path), a **copy
menu** (Session Id · dir · branch · the **REAL** Claude/Codex launch CLI · Summary · Transcript), and
a **pencil** toggling a **SUMMARY PANEL** (a second overlay ≤20% pane width rendering the
`session-end.js` box from the transcript; GLOBAL `AppSettings.showSummaryPanel`, with a wrap-line
toggle `AppSettings.summaryPanelWrapNewlines`). The **tab strip** itself also carries a state-colored
dot (`[icon] ● <title>`).

## 10. Auto Testing & Tests Autorunner (the scheduler)

**Auto Testing** = a per-session ordered list of prompts. **Tests Autorunner** advances it.

### The loop
```
Stop hook ─▶ WaitingForInput ─▶ tryAdvance(session)
tryAdvance(s):
  if s.autorunner.mode == Off: return
  if s.state != WaitingForInput && s.state != Idle: return  # Idle: a launched/resumed plan must START
  if humanTypedWithin(s, ~1500ms): return                 # pauseOnHumanInput
  item = s.queue.firstPending(); if none: notifyPlanDone; return
  if !passesGuard(s, item): item=Held; flag card; return  # e.g. not-a-question
  if s.autorunner.mode == SemiAuto: askConfirm; return
  sleep(throttleMs)
  s.connection.WriteInput(item.text + "\r")               # inject + submit (terminated)
  item=Sent; item.sentAt=now; persist(s)
→ Running → (next) Stop → repeat until queue drains
```

### The one correctness call: "waiting" is three states
| Sub-state | Signal | Tests Autorunner |
|---|---|---|
| Turn complete, ready for next msg | `Stop` | ✅ dequeue + send next |
| Needs tool approval ("y/n") | `Notification(permission)` | ❌ **ApprovalPolicy**, NOT the queue |
| Asked a clarifying question | `Stop` + last msg is a question | ⛔ **Held** by the question-guard |

### Policy & guards
- **Default mode:** `Off` (the cog stamps a configurable default onto new sessions). `SemiAuto`
  gives a one-click confirm per send; `Full` is hands-off.
- **Question-guard ON** by default; per-item override `answers-a-question:ok` (set as the prompt's `guardPattern`).
- **Approval policy** is separate: auto-approve an allowlist of safe tools, else pause for human.
- **Backstops:** `stopOnError`, `maxAutoSends`, throttle, global **Pause-all**, `pauseOnHumanInput`.
- **Idempotency:** mark `Sent` atomically + persist; `UserPromptSubmit` confirms landing; survive restart without replay.
- **Multiline:** a bare `\n` may submit early in the Ink input — use the paste/bracketed-paste path for multi-line bodies, then one submit.

**Shipped — refinements since this sketch.** (1) **"Ready" includes Idle:** a freshly launched /
just-`--resume`d plan emits no `Stop`, so `DecideAdvance` treats **Idle** as ready and advances fire
on **two** triggers — the `Stop` seam *and* observed changes (`OnObserved`) — held to **one prompt
per turn** by a pickup guard (the `echoed` flag) so a change-driven advance can't drain the queue
(invariant #1). (2) **Enter-retry backstop** (`DecideEnterRetry`): the ConPTY can deliver `prompt +
CR` faster than Claude's Ink TUI inits its input box, so the submit Enter is absorbed as a newline
and the turn never starts; the scheduler re-presses a **lone Enter** (never the text) up to 3 times,
regardless of mode. (3) **Failed-inject rollback:** every inject path rolls a failed send back to
`Pending` rather than stranding a phantom `Sent` (invariant #4).

### Plans across the fleet
- **Templates:** save a plan (e.g. *implement → test → fix → docs → commit → PR*) and apply to any session.
- **Apply-to-many:** queue one plan into every session in a directory (or a board column) at once.

## 11. Input & control model

- **Shared stdin:** user keystrokes and injected prompts both reach `claude.exe`; they interleave. `pauseOnHumanInput` suspends Tests Autorunner while you type.
- **Gating:** read-only toggle blocks user input for moments of exclusive control.
- **Reading:** the serialized terminal buffer feeds previews and the optional output-parse; hooks feed state.
- **Activate vs send:** navigating the Manager never sends to an agent. Sending is explicit: Enter→tab→type, Auto Testing `Send now`, or queue+Tests Autorunner.

## 12. Windows Terminal integration

- **Content class:** `AgentManagerContent : IPaneContent` (no `.idl`, like `ScratchpadContent`); compiled in `TerminalAppLib.vcxproj`.
- **Dispatch:** `TerminalPage::_MakePane` gains an `else if (paneType == L"agentManager")` branch.
- **Pinned tab:** `_OpenAgentManagerTab()` from `_OnFirstLayout` inserts at index 0; `CloseButtonVisibility = Never`; tracked in `_managerTab` (nulled on close, mirroring `_settingsTab`). Follow-ups: prevent splitting it; window-close semantics when only the Manager remains.
- **Sessions:** each is a `TermControl` over a `ClaudeConnection` (a `ConptyConnection`, possibly wrapped to add session id + hook wiring). Previews reuse the renderer/buffer serialize.
- **Identity:** ships as its own package, distinct from any Windows Terminal. **Shipped:** **two
  identities, one branding** ([`PROFILES.md`](./PROFILES.md)) — release `Agentmaster` /
  `agentmaster.exe` (`Package-Rel.appxmanifest`) and dev `AgentmasterDev` / `agentmasterdev.exe`
  (`Package-Dev.appxmanifest`, the loose-layout default) — installing **side by side**, each with its
  own per-install **state profile** (default `~/.agentmaster` release / `~/.agentmaster-dev` dev) that
  holds all persisted state, engine files AND Terminal's own settings.
- **Code layout (shipped):** the engine is plain C++ under `AgentMaster/`; the TerminalPage-side
  implementation is six same-class TUs
  (`TerminalPage.Agent{Engine,Sessions,Observer,WindowRecord,ArchivePage,SessionsPage}.cpp`).

## 13. Persistence & the Open ⇄ Archived lifecycle

- Per session: queue + autorunner state + metadata (title, dir, branch) saved as JSON under the app's state dir; reloaded **without replaying** sent prompts.
- **Lifecycle = Open ⇄ Archived** (transient `SessionInfo::live`, never persisted). *Open* = a live tab/`claude.exe` this run (shown on the Board/Tree); *Archived* = shut down but kept restorable (listed behind the Manager's **Archived** button).
- **Closing a session's tab archives it** (the X, tree `Del`, Manager Archive, Auto-Testing Archive — one seam, one consequence confirm): the record is **kept** (`live=false`), so it survives + lists under Archived. There is **no discard** — archive is terminal, and the Claude transcript on disk is never deleted.
- **Startup ARCHIVES, never auto-launches** (reversal of the earlier "close == reopen"): persisted sessions load as Archived; the app opens to just the Manager tab. The user re-opens what they want via **Restore** → `claude --resume <id>` (transcript-gated; a missing transcript ⇒ a fresh id, and the stale archived record is dropped). Restore is **agent-aware** — a Codex record re-launches via `codex resume <uuid>`, transcript-gated on its rollout. Quitting therefore archives the open fleet for next launch.
- Plan **templates** saved separately and reusable across sessions/machines.

**Shipped — the per-window workspace layer + the full-window Archive page**
([`PERSISTENCE.md`](./PERSISTENCE.md)). Persistence is now **two layers**: `sessions.json` (the
session truth) and a per-window **`WindowRecord`** (`windows/<id>.json`) holding geometry, the Manager
**lens**, and an **ordered list of tab refs** (a Claude/Codex tab = its `sessionId` + a `TabKind`; a
shell tab = an opaque WT `actionsJson`) — "Option 1": a thin layer **over** the archive model that
references sessions, never copies them. A relaunched window reopens at its geometry and **re-homes its
whole workspace** (resumes its Claude/Codex sessions, replays shell tabs in order, seeds the lens,
re-selects the focused-at-close tab); on startup the `WindowEmperor` reopens the **open-at-exit** set
(an `open-windows.json` manifest), gated by a decide-prompt when >1. The old "Restore all" modal is
replaced by a **full-window Archive page** (the **Archived (N)** button): a sortable + searchable
table, a detail pane, multi-select **bulk Restore**, and per-window **Reopen window** — grouped by
window. A separate **Sessions** browser ([`SESSIONS.md`](./SESSIONS.md)) lists **every** on-disk
Claude session (not just managed ones) with two-phase search and Jump / Resume / Fork actions.

## 14. Safety & security

- **No silent wrong actions:** approvals never consume planned prompts; question-guard catches clarifications.
- **Runaway protection:** `maxAutoSends`, `stopOnError`, global **Pause-all**, optional per-session token/turn budget.
- **Approvals:** default to pausing for human; only an explicit allowlist auto-approves.
- **Local-only IPC:** the hooks pipe is local; no network surface by default.
- **Don't fight the OS:** never `taskkill` deploy locks or touch the user's running terminal (see Gotchas in `CLAUDE.md`).

## 15. Correctness invariants (do not regress)

1. A session is *ready* for an auto-send when it is **turn-complete** (`Stop` → `WaitingForInput`)
   **or** sitting **Idle** with no turn in progress (a freshly launched / just-resumed plan must
   START, not wait for a `Stop` it will never emit); a change-driven advance is held to **one prompt
   per turn** by the pickup guard. Approvals → ApprovalPolicy; question → Held.
2. Tree **`Enter` = Activate**, never `WriteInput` (no stray carriage return into an agent).
3. Bind queue → **sessionId**, never "the selected session" at send time.
4. **Idempotent** sends; persist; no replay on restart.
5. Backstops always available: stop-on-error, maxAutoSends, global pause/kill, pause-on-human-input.
6. State is **hook-derived**, not screen-scraped.

## 16. Edge cases & risks

- **Approval ≠ ready** / **clarifying question** — handled by §10 (guards + policy).
- **Inject-too-early race:** `Stop` fires before the TUI repaints the prompt — add a small quiescence delay / detect prompt-ready before writing.
- **Double-send / restart replay:** atomic `Sent` + persist; treat `UserPromptSubmit` as confirmation.
- **Wrong-session injection:** hard-bind queue→connection by id.
- **Context exhaustion / compaction** on long auto-runs: surface tokens/cost; cap.
- **Rate limits** across many concurrent plans: a global throttle in the Scheduler.
- **Multiline submission** semantics in the Ink input (bracketed paste).
- **Preview cost:** many live mini-renders (if A2 wall is added) — virtualize/snapshot.

## 17. Extensibility / future

- **Multi-agent:** the same model hosts Codex/Gemini/etc. (anything that runs in a PTY with hooks/state signals).
  **Codex is already realized** — observe (C1) + state (C2, a rollout-tail PULL deriver) + the full
  launch / restore / window-restore / adopt lifecycle (*lifecycle + state only*; driving the Codex TUI is a
  later phase, C4) via a first-class `AgentKind` on `SessionInfo` (a two-id model — our durable handle +
  the rollout uuid). See OBSERVER.md §11f.
- **Agent teams:** visualize orchestrator→subagent topology (ties to WT agent-teams; mind `--teammate-mode in-process`).
- **Alternate Manager views:** Mission-Control wall, command palette, spatial graph as toggles over the registry.
- **Remote/mobile monitoring:** expose the registry read-only over a local server (opt-in).
  **Shipped (a local-shell analog):** the read-only **`agentmaster <verb>` CLI**
  ([`CLI.md`](./CLI.md)) makes the fleet queryable from a shell — `show <ref>`, `list` / `sessions` /
  `tabs` / `windows` / `external`, `--self`, `--json` — reading only persisted + OS-observable state
  (the Observer's PULL model run one-shot from a separate process), so it **works app-up or
  app-down**. Control verbs (`restore`/`archive`, `watch`, `enqueue`/`send-now`) are designed +
  deferred (CLI P2/P3).
- **Branching plans:** conditional Auto Testing steps (on output match → jump/skip/stop).
- **Metrics:** per-session cost/tokens/throughput, an attention/cost dashboard.

## 18. Open decisions

- Window-close semantics when only the non-closable Manager tab remains (keep open as home vs allow close).
- Prevent splitting the Manager tab (mirror the `_settingsTab` split guard) — likely yes.
- Whether sessions are always git-worktree-isolated or optional per session.
- Where Tests Autorunner confirmation UI lives (inline card vs toast). **Resolved:** a contextual confirm
  on the per-tab overlay + the SemiAuto one-click confirm in the Auto Testing (§9.4/§9.7).
- Package identity: keep publisher for signing simplicity vs. fully self-branded cert. **Resolved:**
  a **fully self-branded `CN=Agentmaster`** self-signed identity (the Publisher derives the shared
  PFN hash), with two side-by-side identities (release/dev) — see [`PROFILES.md`](./PROFILES.md).

## 19. Milestones

Build sequence and current status live in [`IMPLEMENTATION.md`](./IMPLEMENTATION.md):
**M0–M4.1 done** (fork, scaffold, builds, content wired, pinned Manager tab, own identity, deployed) →
**M5** SessionRegistry + ConPTY `claude.exe` + hooks bridge → **M6** C1 UI → **M7** Tests Autorunner → **M8** persistence/templates/apply-to-many.

**Status: M0–M8 are all shipped + live-verified.** The original M9–M14 persistence ladder is
**superseded** by `PERSISTENCE.md`'s Increments 1–4 — the per-window workspace layer (singleton
engine, `WindowRecord` capture/restore, multi-window reopen, window-grouped restore) is shipped.
Built on top since: the **Fleet Observer** (O1–O7), per-install **profiles**, the full-window
**Archive page**, the **Sessions** browser, the read-only **`agentmaster` CLI**, and a first-class
**managed Codex** lifecycle (C1 observe + C2 rollout-tail state + launch/restore/window-restore/adopt
— driving its TUI is the lone deferred piece). See the repo-root `CLAUDE.md` for the authoritative
current state.
