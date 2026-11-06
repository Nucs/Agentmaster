# 06 · Interrupted work goes on (INT)

> Conventions and fixtures: [README.md](README.md). Spec: HOOKS.md *API-error turns*, *Interrupted turns*,
> *Cut-off turns*; PERSISTENCE.md §14; MESSAGING.md §21.37. Probe kits: `tools/interrupt-probe/`,
> `tools/e2e-spike/`.

**What shipped.** 0.8.0: a turn that died on an API error continues when its session is resumed (Restart,
Deactivate → wake, a window restore, Sessions "Resume here") — one rewind line appended after the old
claude exits; after a restart the restored tabs that were busy start by themselves (the restore wake).
**After 0.8.0 (NOT BUILT, commit 5bc91d3de):** Restart asks whether to continue a turn the USER
interrupted; a fork of a session in the middle of a turn asks "Fork interrupted / uninterrupted", and a
stop watch Esc's the fork's continued turn when asked to. **2026-11-06 (NOT BUILT):** a Restart of a
session in the middle of a turn the restarted claude would continue by itself asks "Restart interrupted /
uninterrupted" (the same stop watch), in the topic from Telegram too; the AI search's fork tool (the
Telegram Search topic's assistant) refuses such a fork until the user answered. A turn that died on an API error
is part of the same choice for Restart and Fork: "uninterrupted" takes the error back and the new Claude
continues by itself; "interrupted" takes it back too and stops the continuation with one Esc, so a regular
interrupt replaces the error (INT-09). The same day: every question reads "interrupted / uninterrupted" (the
first restart question was "Continue the turn / Keep it interrupted"), a fork of a turn the user interrupted asks
too (INT-10), and Agentmaster ships `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` as a Global env default (INT-11).

**Test environment rule.** These flows need `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` in the test machine's
Claude settings (Appendix A of E2E_TESTING.md) unless a flow says otherwise: without it Claude Code only
PREPARES a continuation and waits. A build after 2026-11-06 (NOT BUILT) seeds it into a fresh profile's Global
env by itself (INT-11), so the settings line is then needed only by a flow that deletes that default.

## Lifecycle model

```text
 a turn ends abnormally                         what the next start of that conversation does
 ─────────────────────────────────────────────  ──────────────────────────────────────────────────────────
 API error after output ("Connection lost")     Claude Code alone: sits at the error
                                                + rewind line (Agentmaster, after the old claude exits): continues
                                                → Restart / Fork ask interrupted / uninterrupted (NOT BUILT, INT-09)
 API error before any output                    Claude Code alone: continues (nothing to do)
 cut off by a shutdown / crash                  Claude Code alone: continues (interruptedByShutdown)
                                                → a Restart of it asks interrupted / uninterrupted (NOT BUILT)
 a Restart of a RUNNING turn                    its own shutdown cuts the turn; the new claude continues it
                                                → Restart asks interrupted / uninterrupted (NOT BUILT)
 the user pressed Esc during a reply            Claude Code alone: sits at the Esc   → Restart asks; a Fork asks (NOT BUILT, INT-10)
 the user pressed Esc during a tool call        Claude Code alone: sits at the Esc   → Restart asks; a Fork asks (NOT BUILT, INT-10)
 the user pressed Esc before any output         Claude Code alone: resends the prompt (Agentmaster cannot tell)
 a fork taken while the source runs             the fork continues the turn by itself → Fork asks (NOT BUILT)

 the line: {"type":"last-prompt","lastPrompt":…,"leafUuid":<user row below the failed reply>,"explicit":true,"sessionId":…}
           appended ONLY while no claude runs the conversation (a live claude re-stamps its own leaf at exit)
```

---

### INT-01 · Restart session continues a turn that died on an API error

> **Since** 0.8.0 · **P0** · **Needs** mock (`say` + `drop`) · **Spec** HOOKS.md *API-error turns*

**Promise.** After "Connection lost mid-response", **Restart session** brings back a claude that continues
the turn by itself — nothing is deleted from the transcript.

**Phases**
1. **The failure.** *Do:* F-S; `fix it [[mock bash "echo step1"; say "I will now"; drop]]`. *Expect:* Claude
   retries 3×, then the session reads Error with "API Error: Connection lost mid-response…". *Check:* [TR]
   a partial assistant row + a synthetic `isApiErrorMessage` / `server_error` row; [UI] crimson dot.
2. **Restart.** *Do:* tab menu → Restart session (since 2026-11-06, NOT BUILT: it first asks **Restart a session whose
   turn stopped on an API error?** — answer **Restart uninterrupted** for this flow; INT-09 covers both answers). *Expect:*
   the old claude closes; only then is the line
   appended (one line at the file's end); the new claude starts and sends a continuation. *Check:* [LOG]
   `[api-error-rewind] <sid8> restart: rewound past "API Error: Connection lost mid-response…" (server_error,
   …) — 1 half-response row(s) … to <target8> (tool result); … waited N ms for 1 claude process(es)`; [TR] the
   marker line (`explicit:true`, no `rewound`), then `Continue from where you left off.`; [MOCK] a `main`
   request whose newest user message is that meta text.
3. **The turn goes on.** *Expect:* Error → Waiting (the leaf moved) → Running (the continuation) → Waiting.
   Provide the continuation's answer: the mock's default reply (`OK`) or a `[[mock …]]` scenario keyed on the
   continuation (§4.3 `use @continue-ok`).

**Measured (2026-10-24, 0.8.0 + 2.1.291, probe q13; `e2e/suites/test_interrupted_work.py`):** a connection lost after
"I will now" is NOT retried (Claude Code keeps the partial text and writes "API Error: Connection lost mid-response. The
response above may be incomplete.") — error, the runner paused. Restart session logged `[api-error-rewind] <sid8>
restart: rewound past "API Error: Connection lost mid-response. …" (server_error, …)`, appended ONE `last-prompt` line
with `explicit: true` (the old claude's own exit re-stamp came before it), and the scanner released the error
(`[recon-error-release] … (rewound past error -> Waiting)`); the new claude sent "Continue from where you left off."
carrying the tool result, so the mock served the scripted turn's SECOND response again. The continuation needs no
dedicated scenario: the turn's own next response answers it — and the test holds it at a second gate (`hold g; hold g2`:
the first attempt is cut at `g`, then `g` is released) to see it running. A continuation that is answered at once
leaves no running state behind: the turn is over before a scan sees it (probe q13: error → waiting-for-you).

**Edge cases**
- **E1 Setting off** (cog → Sessions → "Continue a turn that ended in an API error when its session is
  resumed") → no line; the restarted claude sits at the error.
- **E2 Error before any output** (`error 500 api_error "x"` with retries exhausted) → `NOT rewound —
  NotApplicable`; Claude Code continues it alone.
- **E3 A refusal / safety block** (`error 400 invalid_request_error` shaped as a refusal) → never retried
  (`RefusalError`).
- **E4 An error that will repeat** (`prompt is too long`) → rewound, the resume fails again (as a manual
  retry would) — Error again, no loop.
- **E5 The same conversation running in another tab** (STA-08) → the append refuses (a live holder),
  logged; no line.
- **E6 The old claude does not exit within the wait** → the new claude still starts, no line, logged.
- **E7 A torn transcript tail** (no final newline) → `TornTail`, nothing appended.
- **E8 Idempotence** → a second Restart appends nothing more.
- **E9 Interrupted-turn question** (INT-05, NOT BUILT) → an API-error tail is not an Esc: no dialog.

---

### INT-02 · Deactivate, then wake, continues an API-errored turn

> **Since** 0.8.0 · **P1** · **Needs** mock · **Spec** HOOKS.md *API-error turns* (seams table)

**Phases**
1. **Failure** as INT-01 phase 1.
2. **Park.** *Do:* Deactivate ▸ Deactivate Tab. *Expect:* after the parked claude exits, the line is
   appended. *Check:* [LOG] `[api-error-rewind] <sid8> deactivate: …`.
3. **Wake.** *Do:* click the tab. *Expect:* the resumed claude continues the turn.

**Edge cases**
- **E1 Woken before the old claude exited** → the append refuses (a live claude); the turn sits at the
  error; a later Restart rewinds it.

---

### INT-03 · A restored resume continues an API-errored or cut-off turn

> **Since** 0.8.0 · **P1** · **Needs** mock, F-RS, Sessions page · **Spec** HOOKS.md *API-error turns*

**Phases**
1. **Window restore.** *Do:* failure as INT-01, quit, reopen, enter the tab. *Expect:* the line was appended
   synchronously before the connection was built (the record is Error); the resumed claude continues.
2. **Sessions → Resume here.** *Do:* close the tab instead (Close), Sessions page → the row → Resume here.
   *Expect:* the same continuation.

**Edge cases**
- **E1 A record not in Error and without `midTurnAtRestore`** → no rewind attempted.
- **E2 A fork of an errored session** → the source's tail is not rewound (restart the source instead).

---

### INT-04 · After a restart the busy tabs wake by themselves (the restore wake)

> **Since** 0.8.0 · **P0** · **Needs** mock, F-W2, F-RS · **Spec** PERSISTENCE.md §14

**Promise.** Every restored tab whose session was in the middle of something at the restart (Running, Needs
approval, Error) starts by itself — after every window's focused tab has launched, paced across windows —
while idle tabs stay dormant.

**Preconditions.** Window 1: A (Running, `hold g1`), B (Needs approval, `ask …`), C (Waiting), D (Idle,
focused). Window 2: E (Error, a final API error), F (Waiting, focused).

**Phases**
1. **Quit and reopen.** *Do:* quit; relaunch; Yes to reopen. *Expect:* every tab re-homed dormant; D and F
   (the focused ones) launch first.
2. **The barrier.** *Expect:* no busy tab starts before BOTH windows' focused tabs launched (their
   SessionStart arrived after the restore, or 15 s / 90 s caps). *Check:* [LOG] `[restore-wake]` barrier
   lines.
3. **The wake.** *Expect:* A, B and E start in place (no tab switch), one per ≥ 500 ms slot, at most 4 per
   10 s across both windows; C stays dormant. *Check:* [ENG] `started` flips; [LOG] one wake line each.
4. **What they do.** *Expect:* A (cut-off) continues its turn; E continues if the rewind applies (INT-03);
   B's conversation comes back up (what Claude Code does with the dead question is its own).

**Measured in one window (2026-10-24, probes q07 / q08; `test_session_state.py::
test_a_turn_cut_off_by_the_app_ending_reads_error_then_the_restore_wake_continues_it`):** t1 mid-turn, t2 focused; after
a kill or a quit the reopened window logged `[restore-wake] … restored; 1 busy tab(s) to wake once the tab every reopened
window opens to has launched: <t1>(Error)`, then — t2's claude up (`… its focused tab finished launching …`, 10.8 s; or
"its claude started but sent no hook in time", 16.4 s) — `[nav] activate-dormant <t1>` and `… done - woke 1 busy
tab(s)`; t1 continued its cut-off turn. With no busy tab the window leaves the barrier at once (`left the wake barrier -
no busy tab to wake here`). B (a question at the restart): the continued turn asks the question again (STA-07 E4).

**Edge cases**
- **E1 A pinned session** (Telegram `/pin`, Waiting by the pin hold) that was Running → woken.
- **E2 A session Running in ANOTHER tab's claude** (a duplicate) → not woken (no second writer).
- **E3 The user opens a busy tab first** → skipped (already started), no slot spent.
- **E4 "Activate All Tabs" during the wake** → both drips cooperate; nothing starts twice.
- **E5 A window with no busy tab** → still joins the barrier, then leaves.
- **E6 A focused tab that never starts** → the 90 s cap releases its window.
- **E7 "No" to the reopen prompt** → the one default window joins at the end of its own restore.
- **E8 A record that was Idle / Waiting / Done at the restart** → never woken, even if the scanner later
  reads Error.

---

### INT-05 · Restart asks whether to continue a turn the user interrupted

> **NOT BUILT** (5bc91d3de, after 0.8.0) · **P0 when built** · **Needs** mock (`hold`, tools), Esc · **Spec** HOOKS.md *Interrupted turns*

**Promise.** Restarting a session the user stopped with Esc asks whether to continue the turn; the default
keeps it stopped.

**Phases**
1. **Interrupt during a reply.** *Do:* `do x [[mock stream 5 50ms; say "a long answer …"; hold g1; reply "end"]]`,
   press Esc while text streams. *Expect:* `[Request interrupted by user]`; Waiting.
2. **Restart.** *Do:* tab menu → Restart session. *Expect:* a dialog **Restart a session you interrupted?** quoting
   the prompt that would be resent, buttons **Restart interrupted** (default, Enter) / **Restart uninterrupted**
   / **Cancel** (since 2026-11-06, NOT BUILT; before: **Continue the interrupted turn?** — **Continue the turn** /
   **Keep it interrupted**).
3. **Restart interrupted** (Enter). *Expect:* restart without a line; the new claude sits at the Esc; nothing sent.
4. **Repeat, Restart uninterrupted.** *Expect:* the plan is recomputed; the old claude exits; the line is
   appended; the new claude starts with `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` in its environment and
   resends the prompt (`interrupted_prompt`). *Check:* [TR] the line; [MOCK] the prompt arrives again.
5. **Cancel.** *Expect:* no restart at all.

**Edge cases**
- **E1 Esc during the FIRST tool call** → Restart uninterrupted reruns the whole turn (3 requests in the probe).
- **E2 Esc during the SECOND tool call** → `Continue from where you left off.` from the first call's result.
- **E3 Esc before any output** → no dialog (Claude Code resends the prompt by itself; known gap).
- **E4 Restart from the Manager for a session another window hosts** → the dialog appears in the
  Manager's window; the hosting window restarts with that answer and never asks again.
- **E5 The switch OFF in a settings file** (project `settings.local.json` sets it 0) → the dialog says so;
  Restart uninterrupted still takes the interrupt back; the user's next message continues the turn.
- **E6 WouldNotContinue targets** (a denied tool result, a slash command's meta text, a compaction
  summary) → no dialog.
- **E7 The same conversation in another tab** → no line (that tab's claude would re-stamp it).
- **E8 The dead pane's Enter and the `restartConnection` action** → the same dialog.

---

### INT-06 · Forking a session in the middle of a turn asks how

> **NOT BUILT** (5bc91d3de) · **P0 when built** · **Needs** mock (`hold`, `bash "sleep 40"`) · **Spec** HOOKS.md *Interrupted turns* (fork question, stop watch)

**Promise.** Forking a running session asks whether the fork should also run the turn; the default stops
the fork's copy of the turn by itself, without the user racing to press Esc.

**Phases**
1. **Source running.** *Do:* `long [[mock bash "echo 1"; hold g1; reply "done"]]` (Running).
2. **Fork.** *Do:* tab menu → Fork session. *Expect:* a dialog **Fork a session in the middle of a turn?** —
   **Fork interrupted** (default) / **Fork uninterrupted** / **Cancel**; the text names a tool call the
   fork would run again when there is one.
3. **Fork interrupted.** *Expect:* the fork's tab opens locked read-only (keyboard blocked); when its claude
   continues the turn (heartbeat `busy`) one Esc is written; if still running 1.5 s later, another (≤ 3).
   Then the lock is released. *Check:* [ENG] `stopForkedTurn` persisted; [MOCK] the fork's continuation
   request starts and is aborted; [TR] the fork's interrupt row.
4. **Fork uninterrupted** (repeat). *Expect:* the fork runs the turn alongside the source.

**Edge cases**
- **E1 Every fork entry point asks:** tab menu Fork (+ its model submenu), Ctrl+Shift+D, board / tree /
  side-panel card menus, Sessions row menu / detail pane / Resume-Fork dialog, the launch bar's and the "+"
  card's Fork.
- **E2 The AI search's fork tool** → no dialog: since 2026-11-06 (NOT BUILT) the tool REFUSES while the fork
  would continue the source's turn and the model must ask the user; its `turn` argument (`interrupted` /
  `uninterrupted`) carries the answer to the window (`interrupted` arms this same stop watch).
- **E3 A fork inside `sleep 40`** → the fork re-runs the call (Claude Code drops a call with no result); the
  Esc stops a long call within ~100 ms of `busy`.
- **E4 Nothing to stop** → idle with the box on screen for 6 s, or 45 s with no turn → the watch ends.
- **E5 A dormant fork** (background) → the watch waits (1 s timer) until it starts.
- **E6 Restart / restore / Deactivate of a "Fork interrupted" fork** → the watch re-arms while the next start
  would still continue a turn; a "Restart uninterrupted" answer (either restart question) clears the intent.
- **E7 A fork-carry draft** (DRF-07) → held during the watch, then filled.
- **E8 The tab torn out to another window** → the watch ends; the intent stays for the next start.
- **E9 The switch off where the fork runs** → no dialog (a fork only prepares the continuation).

---

### INT-07 · The same questions asked from Telegram

> **NOT BUILT** (MESSAGING.md §21.37) · **P1 when built** · **Needs** F-TG

**Phases**
1. `/restartsession` at rest on an interrupted session, or the card's 🔄 Restart → the topic asks ⏸ Restart
   interrupted / ▶ Restart uninterrupted / Cancel (tokens `restart-keep` / `restart-uninterrupt`; the words since
   2026-11-06, NOT BUILT — Continue the turn / Keep it interrupted before); never a dialog on the PC.
2. ▶ Reactivate, and the confirmed restart of a RUNNING turn → keep it interrupted (no question). Since
   2026-11-06 (NOT BUILT) the confirm of a RUNNING turn appears only with the switch off; with it on,
   `/restartsession` and the card's 🔄 Restart ask the third question instead (INT-08 phase 6).
3. `/forksession`, the card's ⑂ Fork, the Search topic's ⑂ → "Fork interrupted / uninterrupted" in the topic
   when the fork would continue a turn.
4. (2026-11-06, NOT BUILT) In the 🔎 Search topic, type "fork the <title> session" while that session runs.
   *Expect:* the assistant does not fork at once: it asks interrupted or uninterrupted (its `fork` tool refused
   with that question); answer "interrupted" → the fork opens and its stop watch stops its continued turn
   (`[fork-stop] <fork8> armed (fork)` … `stopped`). *Check:* [LOG] `[ai-search] tool fork <sid8>: the fork
   would continue a turn (running) - the user's answer is needed`, then the open with the answer.

---

### INT-08 · Restarting a session in the middle of a turn asks how

> **NOT BUILT** (2026-11-06) · **P0 when built** · **Needs** mock (`hold`, `bash "sleep 40"`), F-TG for phase 6 · **Spec** HOOKS.md *Interrupted turns* (the third question)

**Promise.** Restarting a session whose turn the restarted Claude would continue by itself asks whether that
turn may go on; the default stops it without the user racing to press Esc, and nothing is asked when no turn
would go on.

**Phases**
1. **Running.** *Do:* `long [[mock bash "echo 1"; hold g1; reply "done"]]` (Running).
2. **Restart.** *Do:* tab menu → Restart session. *Expect:* a dialog **Restart a session in the middle of a
   turn?** — **Restart interrupted** (default, Enter) / **Restart uninterrupted** / **Cancel**; the text names
   a tool call that would run again when there is one. *Check:* [LOG] `[nav] restart-question <sid8> (the turn
   runs…)`.
3. **Restart interrupted** (Enter). *Expect:* the tab restarts, locked read-only; when the new claude
   continues the turn (heartbeat `busy`) ONE Esc is written; the lock is released. *Check:* [ENG]
   `stopForkedTurn` true until the watch ends, then false; [LOG] `[fork-stop] <sid8> restart: the user chose
   "Restart interrupted"`, `… armed (restart interrupted)`, `… stopped`; [MOCK] the continuation request
   starts and is aborted; [TR] the interrupt row under the continuation.
4. **Restart uninterrupted** (repeat with a running turn). *Expect:* the restarted claude carries on with the
   turn; no lock, no Esc. *Check:* [LOG] `restart-answer <sid8> -> let-turn-continue`.
5. **Cancel.** *Expect:* no restart at all; the turn keeps running in the old claude.
6. **From Telegram.** *Do:* `/restartsession` in the running session's topic (or the card's 🔄 Restart).
   *Expect:* no dialog on the PC; the topic asks ⏸ Restart interrupted / ▶ Restart uninterrupted / Cancel;
   a tap restarts with that answer (phase 3 or 4's outcome). *Check:* [LOG] `remote telegram restart <sid8> ->
   asked: the turn would go on (running…)`, `/restartsession <sid8> -> restart interrupted`.

**Edge cases**
- **E1 A turn the USER interrupted** (Esc) → INT-05's question, never this one.
- **E2 A finished turn at rest** → no dialog (`[restart-question] <sid8> not asked: the turn is finished`).
- **E3 Needs approval / a call in flight / a turn a crash cut off, at rest** → the dialog ("stopped in the
  middle of a turn").
- **E4 The switch off** (a project `settings.local.json` sets it 0) → no dialog; from Telegram, a running turn
  gets the old "the turn is cut off" confirm.
- **E5 Restart from the Manager for a session another window hosts** → the dialog in the Manager's window; the
  hosting window restarts with the answer.
- **E6 A never-messaged fork** (no transcript of its own) → the restart re-forks it; the source's tail and
  state decide (`re-forks its source` in the log).
- **E7 A fork carrying a "Fork interrupted" intent** → Restart uninterrupted clears it; Restart interrupted
  keeps watching.
- **E8 The pane hosts another session by the answer** (an in-session `/resume`) → restarted without it.
- **E9 A Codex session, a parked tab** → no dialog.

---

### INT-09 · Restart and fork offer to take back an API error

> **NOT BUILT** (2026-11-06) · **P0 when built** · **Needs** mock (`say` + `drop`: a server error after output began), F-TG for phase 5 · **Spec** HOOKS.md *Interrupted turns* (situation B), *API-error turns*

**Promise.** A turn that died on an API error after output began ("API Error: Server error mid-response") can be continued
from a Restart or a Fork by answering "uninterrupted": the error is taken back and the new Claude carries on by itself. An
"interrupted" answer takes the error back too and stops the continuation with one Esc, so the new Claude waits with a regular
`[Request interrupted by user]` in place of the error (the user's rule of 2026-11-06: "interrupted keeps but replaces error
with regular interrupt").

**Phases**
1. **The error.** *Do:* `x [[mock think "…"; drop server_error]]` (thinking, then the stream cut). *Expect:* the tab shows
   `API Error: Server error mid-response…`; state Error. *Check:* [TR] the error row (`truncatedAfterOutput`).
2. **Restart.** *Do:* tab menu → Restart session. *Expect:* **Restart a session whose turn stopped on an API error?** quoting
   the error — **Restart interrupted** (default) / **Restart uninterrupted** / **Cancel**.
3. **Restart uninterrupted.** *Expect:* the old claude exits, ONE `last-prompt` line is appended (aimed at the tool result /
   prompt below the half-response), the new claude starts with `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` and continues the turn
   by itself (`Continue from where you left off.`). *Check:* [LOG] `[api-error-rewind] <sid8> restart: rewound past …`; [MOCK]
   the continuation request.
4. **Fork uninterrupted** (repeat phase 1, then tab menu → Fork session). *Expect:* **Fork a session whose turn stopped on an
   API error?**; "Fork uninterrupted" → the line lands in the SOURCE's transcript while the source's claude still runs, the
   fork opens and starts at once and carries on with the turn; the source still shows the error and waits. *Check:* [LOG]
   `[api-error-rewind] <src8> fork: rewound past … (claude pid N still runs the source …)`, `[nav] fork-start <fork8> started
   in place`; [MOCK] the fork's continuation request; the source's tab unchanged.
5. **From Telegram.** `/restartsession` and `/forksession` in that session's topic → the same questions with the error
   quoted; the fork's buttons are `fork-take-back-and-interrupt` / `fork-take-back-and-continue`. No dialog on the PC.
6. **Restart interrupted** (repeat phase 1, then Restart → **Restart interrupted**, Enter). *Expect:* the line is appended as
   in phase 3 and the new claude starts the continuation, locked read-only; ONE Esc stops it; the lock is released; the tab
   waits. *Check:* [LOG] `[api-error-rewind] <sid8> restart: rewound past …`, `[fork-stop] <sid8> restart: the user chose
   "Restart interrupted"`, `… armed (restart interrupted)`, `… Esc pressed (1/3)`, `… stopped`; [TR] the active branch ends in
   `[Request interrupted by user]`, the error on the dead branch; a second Restart then asks INT-05's question.
7. **Fork interrupted** (repeat phase 1, then Fork → **Fork interrupted**). *Expect:* phase 4's take-back and in-place start,
   plus the stop watch on the fork: ONE Esc stops its continuation; the fork waits with a regular interrupt. *Check:* [ENG]
   the fork's `stopForkedTurn`; [LOG] `fork-answer <src8> -> fork interrupted (take-back-and-interrupt)`, `[fork-stop] <fork8>
   armed (fork)` … `stopped`.

**Edge cases**
- **E1 Restart interrupted with a skipped take-back** (another tab's claude runs the conversation) → the error stays,
  nothing runs, the watch ends at its idle settle (`nothing to stop`).
- **E2 Fork interrupted with the switch off in a settings file** → the take-back happens, nothing starts to stop, the fork
  waits; the dialog said so (no Esc promised).
- **E3 `resumeApiErrorTurns` off** → no question; the restart keeps the error, as every restart does then.
- **E4 The switch off in a settings file** → asked; the text says the next message continues it; the line is still appended.
- **E5 A second fork after a take-back** → situation A (Claude Code continues by itself): the stop watch answers "interrupted".
- **E6 A dormant fork that starts after the source's claude exited** → it reads the source's re-stamp and waits at the error
  (the in-place start keeps that window ~1 s).
- **E7 A never-messaged fork restarted** → the re-fork takes its SOURCE's error back (`restart re-fork of <sid8>`).
- **E8 The AI search's fork** (the Telegram Search topic) → refused until `turn`; `uninterrupted` → the take-back,
  `interrupted` → the take-back and the stop watch.
- **E9 A refusal-class error** (a refusal, a DLP denial) or a target Claude Code would not continue (a meta row, a compaction
  summary) → no question.

---

### INT-10 · Forking a session the user interrupted asks how

> **NOT BUILT** (2026-11-06) · **P1 when built** · **Needs** mock (`hold`), Esc, F-TG for phase 5 · **Spec** HOOKS.md *Interrupted turns* (situation C)

**Promise.** Forking a session whose last turn the user stopped with Esc asks whether the fork carries on with that turn:
"uninterrupted" takes the interrupt back for the fork (the source stays interrupted), "interrupted" forks it as it is.

**Phases**
1. **Interrupt.** *Do:* `do x [[mock stream 5 50ms; say "a long answer …"; hold g1; reply "end"]]`, press Esc while text
   streams. *Expect:* `[Request interrupted by user]`; Waiting.
2. **Fork.** *Do:* tab menu → Fork session. *Expect:* a dialog **Fork a session you interrupted?** quoting the prompt that
   would be sent again — **Fork interrupted** (default, Enter) / **Fork uninterrupted** / **Cancel**. *Check:* [LOG] `[nav]
   fork-question <src8> (<origin>; the user's interrupt)`.
3. **Fork uninterrupted.** *Expect:* ONE line in the SOURCE's transcript (aimed at the prompt) while its claude still runs;
   the fork opens, starts at once with `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` and sends the prompt again; the source's tab
   still shows the interrupt and waits. *Check:* [LOG] `[interrupt-rewind] <src8> fork: took back … (claude pid N still runs
   the source …)`, `fork-answer <src8> -> fork uninterrupted (take-back-and-continue)`, `[nav] fork-start <fork8> started in
   place`; [MOCK] the fork's request.
4. **Fork interrupted** (repeat phases 1–2). *Expect:* a plain fork: no line, no lock; it waits at the interrupt like its
   source. *Check:* [LOG] `fork-answer <src8> -> fork interrupted (uninterrupted)`.
5. **From Telegram.** `/forksession` in the source's topic → the same question (⏸ Fork interrupted `fork-keep-interrupt` /
   ▶ Fork uninterrupted `fork-take-back-and-continue` / Cancel); the 🔎 Search topic's assistant refuses its fork until the
   user answered. No dialog on the PC.

**Edge cases**
- **E1 Esc during a tool call** → the take-back lands on the last finished tool result; the fork carries on from it.
- **E2 The switch off in a settings file** → asked; the text says the next message in the fork continues it; the line is
  still appended.
- **E3 A target Claude Code would not continue** (a denied tool result, a slash command's meta text, a compaction summary)
  → no question; a plain fork.
- **E4 A second fork after a take-back** → situation A (Claude Code sends the prompt again by itself): the stop watch answers
  "interrupted".
- **E5 Esc before any output** → no interrupt row: situation A (the prompt is resent by itself).

---

### INT-11 · A fresh profile ships the auto-continue switch

> **NOT BUILT** (2026-11-06) · **P1 when built** · **Needs** a fresh profile · **Spec** ENV_VARS.md §8 (v3), HOOKS.md *Interrupted turns*

**Promise.** Agentmaster sets `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` for its sessions by itself, so a cut-off turn goes on
after a restart without the user's own Claude Code settings; a user who removes it keeps it removed.

**Phases**
1. **Fresh profile.** *Do:* start Agentmaster on an empty profile, with no `CLAUDE_CODE_RESUME_INTERRUPTED_TURN` in any Claude
   Code settings file. *Expect:* ⚙ → Environment variables ▸ Global lists `CLAUDE_CODE_MAX_RETRIES=50000`,
   `CLAUDE_CODE_ADDITIONAL_DIRECTORIES_CLAUDE_MD=1` and `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1`. *Check:* [FILE] the profile's
   `settings.json` has `envDefaultsVersion` 3.
2. **An updated profile.** *Do:* a profile at `envDefaultsVersion` 2. *Expect:* the v3 line alone is added, on its own line.
3. **A cut-off turn goes on.** *Do:* `long [[mock bash "echo 1"; hold g1; reply "done"]]`, quit while it runs, reopen.
   *Expect:* the restore wake starts the tab and the turn continues by itself (INT-04). *Check:* [MOCK] the continuation.
4. **Removed stays removed.** *Do:* delete the line in the cog, Save, restart. *Expect:* it does not come back; a Restart of a
   turn that would go on no longer asks (nothing continues by itself), and an "uninterrupted" answer to INT-05 / INT-09 still
   sets the switch for that one start.

**Edge cases**
- **E1 A settings file sets it off** (`settings.local.json` → `0`) → the file wins; the questions say so.
- **E2 A user's own value** (`claude_code_resume_interrupted_turn=0` in the Global env before the seed) → kept, never
  duplicated.
- **E3 A folder's env sets it 0** → that folder's sessions do not continue by themselves.
