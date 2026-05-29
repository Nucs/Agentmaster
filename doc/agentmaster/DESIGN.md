# Agentmaster — Design

> The complete design for **Agentmaster** — a fork of Windows Terminal that turns it into a
> manager for many **Claude Code** sessions. This document is the "what & why + full
> architecture." Companions: [`IMPLEMENTATION.md`](./IMPLEMENTATION.md) (milestones & build)
> and [`HOOKS.md`](./HOOKS.md) (the Claude Code hooks contract).

---

## 1. Vision & goals

Run **N Claude Code sessions across M working directories** from one native Windows app, where you can:

- **Fully interact** with any session — it's a real `claude.exe` in a real terminal (colors, mouse, scrollback, the genuine Ink TUI), not a proxy or scrape.
- Give the app **100% programmatic control** — inject prompts, read output, observe state, and drive sessions on a schedule.
- **See and triage the whole fleet** — when N×M is large, surface what needs you and hide the calm.
- **Automate sequences** — queue a plan of prompts per session and let an **Autopilot** advance it as each turn completes.

The headline differentiator: **full human interaction and full machine control coexist on the same session**, because both write to the same ConPTY stdin.

### Success criteria
- Launch ≥10 sessions across several repos and never lose track of which need attention.
- Queue a 10-step plan and have it drive a session to completion unattended (with guardrails), while you can still type into it.
- Zero "silent wrong action" (e.g., autopilot answering a permission prompt with a planned prompt).

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
│        │ (SessionInfo × N)  │   │ Autopilot    │   │ (named-pipe listener)  │  │
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
- **Scheduler / Autopilot** — drives Flight Plan queues by injecting prompts on the right signal.

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
              lastActivity, queue: QueuedPrompt[], autopilot: AutopilotState }
SessionState  = Idle | Running | WaitingForInput | NeedsApproval | Error | Done
QueuedPrompt  { id, label, text, status(Pending|Sent|Held|Skipped|Failed),
                gate(OnTurnComplete|AfterDelay|Manual), guardPattern, dependsOn, attempts }
AutopilotState{ mode(Off|SemiAuto|Full), throttleMs, stopOnError, pauseOnHumanInput,
                maxAutoSends, approval: ApprovalPolicy }
ApprovalPolicy{ pauseForHuman, autoApproveTools[] }
```

- **M × N structure** is first-class: `workingDir` is the grouping axis; multiple sessions can share a dir.
- **Isolation:** new sessions can be created in **git worktrees** so parallel agents don't collide (own branch/dir).
- **Correlation:** each session's `id` is injected into its `claude.exe` as `CCMGR_SESSION_ID` so hook events map back (§8).

## 7. State machine (hook-driven)

```
            UserPromptSubmit / inject
   Idle ───────────────────────────────▶ Running
     ▲                                      │ Stop hook
     │ (next turn)                          ▼
     └──────────────────────────── WaitingForInput ──▶ (Autopilot.tryAdvance)
                                          │
   Notification(permission) ──▶ NeedsApproval ──▶ ApprovalPolicy
   turn ends in error ───────▶ Error      SessionEnd ──▶ Done
```

The crucial subtlety: **"the agent is waiting" is three different states**, and only the first should pull a planned prompt — see §10 and the correctness rules.

## 8. Hooks bridge

Authoritative state with no screen-scraping. Full contract in [`HOOKS.md`](./HOOKS.md); summary:

- On spawn, set `CCMGR_SESSION_ID=<guid>` on the child and write a hooks config whose commands post `{sessionId, cwd, event, …}` to a **local named pipe** (`\\.\pipe\agentmaster.<pid>`), read on a dedicated thread. (Fallback: per-session JSONL the app watches.)
- Event → effect: `SessionStart`→register/Idle; `UserPromptSubmit`→Running (also confirms our injected prompt landed → idempotency); `PreToolUse/PostToolUse`→activity; `Notification(permission)`→NeedsApproval + ApprovalPolicy; `Stop`→WaitingForInput + `Autopilot.tryAdvance`; `SubagentStop`→info; `SessionEnd`→Done.
- The hook payload carries a best-effort `lastMessageIsQuestion` flag that feeds the question-guard.

## 9. The Manager tab — C1 "Linked Lenses"

A pinned, leftmost, non-closable tab (tab 0, open by default), implemented as an `IPaneContent` (`AgentManagerContent`). Three regions over **one shared model**, with bidirectional selection sync.

### 9.1 Layout
```
MANAGER (tab 0 · pinned · non-closable)
┌─ TRIAGE BOARD ─────────────────────────────── [scope: K:/api] ─┐
│ Running      │ Waiting-for-you │ Needs-approval │ Error          │
│ api-test ⚙2/6│ api-fix ⚙4/10 ◄ │      —         │  ui-test        │
├──────────────────────── drag divider ──────────────────────────┤
│ EXPLORER TREE              │ FLIGHT PLAN — api-fix   Autopilot[▶] │
│ ▾ K:/api  3 ·1◐ ◄scoped    │ next: turn-complete · 3/10 sent      │
│   ● api-fix  waiting ◄hl   │ ✓ add unit tests                     │
│   ● api-test running ⚙     │ ⏳ add docs to API     [turn-done]    │
│   ○ api-docs idle          │ ⛔ commit  (held: agent asked a Q)    │
│ ▸ K:/ui   5                │ [+Add][Send now][Edit][↑][↓][Tmpl]   │
│ ▸ K:/docs 1                │— live peek —  [Focus][ESC][Kill]     │
└────────────────────────────┴───────────────────────────────────┘
```

### 9.2 Triage Board (top) — *triage*
- Columns are hook-driven states: **Running · Waiting-for-you · Needs-approval · Error** (+ Idle/Done filterable). Cards = sessions tagged with dir/branch/task/elapsed; they auto-move as hooks fire. Optional **swimlanes by working directory**.
- The **Waiting / Needs-approval** columns are the work queue — act only on what's blocked. Cards show an autopilot badge **⚙ sent/total**. Batch actions per column (approve all, broadcast a prompt to a column/lane).

### 9.3 Explorer Tree (bottom-left) — *structure*
- Roots = the M working directories (branch/worktree + roll-up like "3 · 1◐"); children = that dir's sessions with status badges (●running ◐waiting ○idle ✕error). Collapse to scale.
- **Keymap:** `↑↓` select (preview only); `→←` expand/collapse dir; **`Enter` = Activate** → jump to the session's live tab (NEVER inject — see invariants); `Space` = larger inline peek; `Del` = kill (confirm); type = filter.

### 9.4 Flight Plan (bottom-right) — *plan & automate*
- The selected session's **prompt queue** + **Autopilot** controls + a slim live peek + the action bar. Add/edit/reorder/delete prompts; per-item **gate** badge (turn-done / delay / manual) and **guard**; `Send now`, `Templates`, import/apply-to-many. Detailed mechanics in §10.

### 9.5 Selection sync (the "Linked Lenses" core)
One shared selection/`ICollectionView` over the registry:
- Select a **card** → tree expands/scrolls/highlights that session + its peek + loads its Flight Plan.
- Select a **directory** in the tree → the board filters to that dir's lanes.
- Everything stays consistent because there is one model, three views.

### 9.6 Other manager paradigms (deferred, optional toggle-views)
A2 Mission-Control Wall (live tiles + semantic zoom), A4 Command-Palette switcher, A5 Spatial graph — not built initially; C1 is the primary. They could become alternate views over the same registry later.

## 10. Flight Plan & Autopilot (the scheduler)

**Flight Plan** = a per-session ordered list of prompts. **Autopilot** advances it.

### The loop
```
Stop hook ─▶ WaitingForInput ─▶ tryAdvance(session)
tryAdvance(s):
  if s.autopilot.mode == Off: return
  if s.state != WaitingForInput: return
  if humanTypedWithin(s, ~1500ms): return                 # pauseOnHumanInput
  item = s.queue.firstPending(); if none: notifyPlanDone; return
  if !passesGuard(s, item): item=Held; flag card; return  # e.g. not-a-question
  if s.autopilot.mode == SemiAuto: askConfirm; return
  sleep(throttleMs)
  s.connection.WriteInput(item.text + "\r")               # inject + submit (terminated)
  item=Sent; item.sentAt=now; persist(s)
→ Running → (next) Stop → repeat until queue drains
```

### The one correctness call: "waiting" is three states
| Sub-state | Signal | Autopilot |
|---|---|---|
| Turn complete, ready for next msg | `Stop` | ✅ dequeue + send next |
| Needs tool approval ("y/n") | `Notification(permission)` | ❌ **ApprovalPolicy**, NOT the queue |
| Asked a clarifying question | `Stop` + last msg is a question | ⛔ **Held** by the question-guard |

### Policy & guards
- **Default mode:** `SemiAuto` for the first ~2 sends of a new plan (one-click confirm) → then `Full`. Earns trust before hands-off.
- **Question-guard ON** by default; per-item override `answers-a-question: ok`.
- **Approval policy** is separate: auto-approve an allowlist of safe tools, else pause for human.
- **Backstops:** `stopOnError`, `maxAutoSends`, throttle, global **Pause-all / Kill-all**, `pauseOnHumanInput`.
- **Idempotency:** mark `Sent` atomically + persist; `UserPromptSubmit` confirms landing; survive restart without replay.
- **Multiline:** a bare `\n` may submit early in the Ink input — use the paste/bracketed-paste path for multi-line bodies, then one submit.

### Plans across the fleet
- **Templates:** save a plan (e.g. *implement → test → fix → docs → commit → PR*) and apply to any session.
- **Apply-to-many:** queue one plan into every session in a directory (or a board column) at once.

## 11. Input & control model

- **Shared stdin:** user keystrokes and injected prompts both reach `claude.exe`; they interleave. `pauseOnHumanInput` suspends Autopilot while you type.
- **Gating:** read-only toggle blocks user input for moments of exclusive control.
- **Reading:** the serialized terminal buffer feeds previews and the optional output-parse; hooks feed state.
- **Activate vs send:** navigating the Manager never sends to an agent. Sending is explicit: Enter→tab→type, Flight Plan `Send now`, or queue+Autopilot.

## 12. Windows Terminal integration

- **Content class:** `AgentManagerContent : IPaneContent` (no `.idl`, like `ScratchpadContent`); compiled in `TerminalAppLib.vcxproj`.
- **Dispatch:** `TerminalPage::_MakePane` gains an `else if (paneType == L"agentManager")` branch.
- **Pinned tab:** `_OpenAgentManagerTab()` from `_OnFirstLayout` inserts at index 0; `CloseButtonVisibility = Never`; tracked in `_managerTab` (nulled on close, mirroring `_settingsTab`). Follow-ups: prevent splitting it; window-close semantics when only the Manager remains.
- **Sessions:** each is a `TermControl` over a `ClaudeConnection` (a `ConptyConnection`, possibly wrapped to add session id + hook wiring). Previews reuse the renderer/buffer serialize.
- **Identity:** ships as its own package **`Agentmaster`** (`Package-Dev.appxmanifest`), distinct from any Windows Terminal.

## 13. Persistence

- Per session: queue + autopilot state + metadata (title, dir, branch) saved as JSON under the app's state dir; restored on restart **without replaying** sent prompts.
- Optionally restore the **layout** (which sessions/dirs were open) like WT's session restore.
- Plan **templates** saved separately and reusable across sessions/machines.

## 14. Safety & security

- **No silent wrong actions:** approvals never consume planned prompts; question-guard catches clarifications.
- **Runaway protection:** `maxAutoSends`, `stopOnError`, global Kill-all, optional per-session token/turn budget.
- **Approvals:** default to pausing for human; only an explicit allowlist auto-approves.
- **Local-only IPC:** the hooks pipe is local; no network surface by default.
- **Don't fight the OS:** never `taskkill` deploy locks or touch the user's running terminal (see Gotchas in `CLAUDE.md`).

## 15. Correctness invariants (do not regress)

1. Auto-send fires only on **turn-complete** (`Stop`); approvals → ApprovalPolicy; question → Held.
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
- **Agent teams:** visualize orchestrator→subagent topology (ties to WT agent-teams; mind `--teammate-mode in-process`).
- **Alternate Manager views:** Mission-Control wall, command palette, spatial graph as toggles over the registry.
- **Remote/mobile monitoring:** expose the registry read-only over a local server (opt-in).
- **Branching plans:** conditional Flight Plan steps (on output match → jump/skip/stop).
- **Metrics:** per-session cost/tokens/throughput, an attention/cost dashboard.

## 18. Open decisions

- Window-close semantics when only the non-closable Manager tab remains (keep open as home vs allow close).
- Prevent splitting the Manager tab (mirror the `_settingsTab` split guard) — likely yes.
- Whether sessions are always git-worktree-isolated or optional per session.
- Where Autopilot confirmation UI lives (inline card vs toast).
- Package identity: keep publisher for signing simplicity vs. fully self-branded cert.

## 19. Milestones

Build sequence and current status live in [`IMPLEMENTATION.md`](./IMPLEMENTATION.md):
**M0–M4.1 done** (fork, scaffold, builds, content wired, pinned Manager tab, own identity, deployed) →
**M5** SessionRegistry + ConPTY `claude.exe` + hooks bridge → **M6** C1 UI → **M7** Autopilot → **M8** persistence/templates/apply-to-many.
