# 06 · Interrupted work goes on (INT)

> Conventions and fixtures: [README.md](README.md). Spec: HOOKS.md *API-error turns*, *Interrupted turns*,
> *Cut-off turns*; PERSISTENCE.md §14; MESSAGING.md §21.37. Probe kits: `tools/interrupt-probe/`,
> `tools/e2e-spike/`.

**What shipped.** 0.8.0: a turn that died on an API error continues when its session is resumed (Restart,
Deactivate → wake, a window restore, Sessions "Resume here") — one rewind line appended after the old
claude exits; after a restart the restored tabs that were busy start by themselves (the restore wake).
**After 0.8.0 (NOT BUILT, commit 381c6c715):** Restart asks whether to continue a turn the USER
interrupted; a fork of a session in the middle of a turn asks "Fork interrupted / uninterrupted", and a
stop watch Esc's the fork's continued turn when asked to.

**Test environment rule.** These flows need `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` in the test machine's
Claude settings (Appendix A of E2E_TESTING.md) unless a flow says otherwise: without it Claude Code only
PREPARES a continuation and waits.

## Lifecycle model

```text
 a turn ends abnormally                         what the next start of that conversation does
 ─────────────────────────────────────────────  ──────────────────────────────────────────────────────────
 API error after output ("Connection lost")     Claude Code alone: sits at the error
                                                + rewind line (Agentmaster, after the old claude exits): continues
 API error before any output                    Claude Code alone: continues (nothing to do)
 cut off by a shutdown / crash                  Claude Code alone: continues (interruptedByShutdown)
 the user pressed Esc during a reply            Claude Code alone: sits at the Esc   → Restart asks (NOT BUILT)
 the user pressed Esc during a tool call        Claude Code alone: sits at the Esc   → Restart asks (NOT BUILT)
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
2. **Restart.** *Do:* tab menu → Restart session. *Expect:* the old claude closes; only then is the line
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

> **NOT BUILT** (381c6c715, after 0.8.0) · **P0 when built** · **Needs** mock (`hold`, tools), Esc · **Spec** HOOKS.md *Interrupted turns*

**Promise.** Restarting a session the user stopped with Esc asks whether to continue the turn; the default
keeps it stopped.

**Phases**
1. **Interrupt during a reply.** *Do:* `do x [[mock stream 5 50ms; say "a long answer …"; hold g1; reply "end"]]`,
   press Esc while text streams. *Expect:* `[Request interrupted by user]`; Waiting.
2. **Restart.** *Do:* tab menu → Restart session. *Expect:* a dialog **Continue the interrupted turn?** quoting
   the prompt that would be resent, buttons **Continue the turn** / **Keep it interrupted** (default, Enter)
   / **Cancel**.
3. **Keep** (Enter). *Expect:* restart without a line; the new claude sits at the Esc; nothing sent.
4. **Repeat, Continue the turn.** *Expect:* the plan is recomputed; the old claude exits; the line is
   appended; the new claude starts with `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` in its environment and
   resends the prompt (`interrupted_prompt`). *Check:* [TR] the line; [MOCK] the prompt arrives again.
5. **Cancel.** *Expect:* no restart at all.

**Edge cases**
- **E1 Esc during the FIRST tool call** → Continue reruns the whole turn (3 requests in the probe).
- **E2 Esc during the SECOND tool call** → `Continue from where you left off.` from the first call's result.
- **E3 Esc before any output** → no dialog (Claude Code resends the prompt by itself; known gap).
- **E4 Restart from the Manager for a session another window hosts** → the dialog appears in the
  Manager's window; the hosting window restarts with that answer and never asks again.
- **E5 The switch OFF in a settings file** (project `settings.local.json` sets it 0) → the dialog says so;
  Continue still takes the interrupt back; the user's next message continues the turn.
- **E6 WouldNotContinue targets** (a denied tool result, a slash command's meta text, a compaction
  summary) → no dialog.
- **E7 The same conversation in another tab** → no line (that tab's claude would re-stamp it).
- **E8 The dead pane's Enter and the `restartConnection` action** → the same dialog.

---

### INT-06 · Forking a session in the middle of a turn asks how

> **NOT BUILT** (381c6c715) · **P0 when built** · **Needs** mock (`hold`, `bash "sleep 40"`) · **Spec** HOOKS.md *Interrupted turns* (fork question, stop watch)

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
- **E2 The AI search's fork tool** → no dialog (uninterrupted).
- **E3 A fork inside `sleep 40`** → the fork re-runs the call (Claude Code drops a call with no result); the
  Esc stops a long call within ~100 ms of `busy`.
- **E4 Nothing to stop** → idle with the box on screen for 6 s, or 45 s with no turn → the watch ends.
- **E5 A dormant fork** (background) → the watch waits (1 s timer) until it starts.
- **E6 Restart / restore / Deactivate of a "Fork interrupted" fork** → the watch re-arms while the next start
  would still continue a turn; a "Continue the turn" restart answer clears the intent.
- **E7 A fork-carry draft** (DRF-07) → held during the watch, then filled.
- **E8 The tab torn out to another window** → the watch ends; the intent stays for the next start.
- **E9 The switch off where the fork runs** → no dialog (a fork only prepares the continuation).

---

### INT-07 · The same questions asked from Telegram

> **NOT BUILT** (MESSAGING.md §21.37) · **P1 when built** · **Needs** F-TG

**Phases**
1. `/restartsession` at rest on an interrupted session, or the card's 🔄 Restart → the topic asks Continue
   the turn / Keep it interrupted / Cancel (tokens `restart-uninterrupt` / `restart-keep`); never a dialog on
   the PC.
2. ▶ Reactivate, and the confirmed restart of a RUNNING turn → keep it interrupted (no question).
3. `/forksession`, the card's ⑂ Fork, the Search topic's ⑂ → "Fork interrupted / uninterrupted" in the topic
   when the fork would continue a turn.
