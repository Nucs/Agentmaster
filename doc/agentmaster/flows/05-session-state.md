# 05 · Session state on every surface (STA)

> Conventions and fixtures: [README.md](README.md). Spec: HOOKS.md *State machine*, *Cut-off turns*,
> *Compaction*; OBSERVER.md §20; CLAUDE.md *Engine*, *The tab strip state dot*. The mock's script for Claude Code's
> events: [`../E2E_TESTING.md`](../E2E_TESTING.md) §4.3 (`stop refusal`, `next`). The suite:
> `e2e/suites/test_session_state.py` (README.md "The suite that runs these flows").

**What shipped.** 0.7.7: a retried API error recovers to Running; an over-1 MiB transcript line no longer
strands Running. 0.7.9: no Running for hours after a quiet turn end (the catch-up read), no
Running → Waiting → Running flicker (the turn-start race guard), a cut-off turn reads **Error**, a
compaction no longer drops Running to Idle, Ctx updates at a compaction's end. 0.8.0: one conversation in
two tabs no longer hops between them (one host tab), a killed fullscreen claude no longer breaks its tab.

## Lifecycle model

```text
                       ┌──── Esc (scanner) / Stop ────┐
 launch ─► Idle ─prompt─► Running ──Stop──► Waiting for you ──read + timeout──► Idle
             ▲              │  ▲                 │   ▲
             │      question│  │answered         │   └─ a turn nobody typed: a task's notification,
             │   permission │  │                 │      a Stop hook's block (STA-13) ─► Running
             │              ▼  │                 └─ /exit, claude ended ─► Done (only its SessionEnd hook: STA-K1)
             │         Needs approval
             │
             │   Running stays Running through Claude Code's own recoveries (STA-14): a max_tokens
             │   continuation, a refusal's one retry, a compaction of a prompt too long, retried 429/529/401
             │
             └─ Error ◄── API error (final) · a refusal refused again · cut-off turn at restore · a stream
                  │        lost mid-response
                  └──► Running (next message, a continued turn, claude's busy heartbeat)
```

## Claude Code's events, measured (2026-10-23)

The flows were checked against what Claude Code really does: every transcript on the owner's PC was surveyed
(read-only, counts only), the installed profiles' scanner.logs were tallied, and each event was replayed with the mock
against **Claude Code 2.1.291** and **Agentmaster 0.8.0** in a claude-fleet Windows 11 desktop (probe runs stprobe1–3;
`e2e/suites/test_session_state.py` keeps the cases that matter). On that desktop the PC was busy (77% CPU), and every
PowerShell hook took 10–22 s: Claude Code cancelled many at their 10 s timeout (`hook_cancelled`), some still arrived
late, and the scanner's belts carried the state — STA-16 in action, all the time.

**What this PC's Claude Code wrote** (7,044 transcripts: 4,627 conversations, 2,417 subagent side files):

| Event in the transcript | Count | Flow |
|---|---|---|
| Turns ended (`system/turn_duration`) — under 5 s / under 1 min / under 10 min / longer | 14,035 — 134 / 2,368 / 7,338 / 4,195 | STA-01 |
| Esc: `[Request interrupted by user]` / `[Request interrupted by user for tool use]` | 680 / 298 | STA-12 |
| A message typed during a turn, queued (`queue-operation enqueue`) / absorbed into the running turn (`remove absorbed_mid_turn`) / all queued ones popped back (`popAll`) | 12,053 / 2,092 / 192 | STA-01 E3 |
| Queued messages delivered INTO a running turn (`queued_command` attachments): task notifications / the user's own prompts (a task's notification at rest is a turn of its own, a `<task-notification>` user line) | 3,314 / 236 | STA-13, STA-01 E3 |
| Background work started: Bash / Agent `run_in_background` | 4,235 / 374 | STA-05, STA-13 |
| Compactions (`compact_boundary`): auto / manual · microcompactions | 1,647 / 271 · 126 | STA-06 |
| API errors written as messages (`isApiErrorMessage`) by kind: invalid_request (refusals, "Prompt is too long" …) / rate_limit / unknown / server_error / billing / authentication / model_not_found | 876: 560 / 171 / 71 / 26 / 15 / 12 / 11 | STA-04, STA-14 |
| Retries written as `system/api_error` (with `retryAttempt` / `maxRetries`) | 124 | STA-04 |
| Refusals (`stop_reason refusal`) / the retry note (`system/informational`) / final (`model_refusal_no_fallback`) / a retry on another model (`model_refusal_fallback`) | 205 / 347 / 87 / 3 | STA-14 |
| `max_tokens` / `model_context_window_exceeded` | 3 / 1 | STA-14 |
| Hooks cancelled at their timeout (`hook_cancelled`): UserPromptSubmit / Stop / SessionStart / PostToolUse | 73 / 12 / 8 / 8 | STA-16 |
| Scheduled task runs (`scheduled_task_fire`) · teammate messages (`<teammate-message`) | 365 · 13,760 | STA-13 E3, E4 |
| Claude Code's resume placeholder `No response requested.` | 203 | STA-07 E6 |
| Local commands (`system/local_command`) | 1,089 | STA-15 |
| Lines written in each permission mode: bypassPermissions / auto / plan / default | 127,400 / 189 / 78 / 4 | STA-03 |

**What put sessions in Error on this PC** (`[recon-error]` in the release and dev profiles' scanner.log, 231 lines):
"Prompt is too long" (compaction could not make it fit) **91** · a refusal ("… safeguards flagged this message /
session") **73** · a usage or session limit ("You've hit your session limit", "out of usage credits", "Usage credits are
required") **28** · a broken stream ("Connection lost / closed mid-response", "Server error mid-response", "Stream idle
timeout") **19** · a 500 **7** · not logged in **5** · rate limited **4** · a model not found **3** · autocompact
thrashing **1**. The release's scanner reconciled, over its log: recon-stop 3,800 · recon-subagent 2,081 · recon-run
1,421 · recon-block-presence 1,121 · recon-resume 579 · recon-stop-hold 503 · recon-resume-presence 419 · decay-waiting
407 · recon-stop-idle 381 · recon-error 174 · recon-block 66 · recon-cutoff 27 · recon-error-release 3.

**Each event, replayed** (mock script → what Claude Code 2.1.291 did → what the side panel showed on 0.8.0):

| Event (mock) | Claude Code 2.1.291 | Agentmaster 0.8.0 | Flow |
|---|---|---|---|
| A turn (`say …; hold g; reply …`) | UserPromptSubmit … Stop | running → waiting-for-you, no blip | STA-01, STA-02 |
| A turn under 2 s (`fast`) | a Stop the engine reads as too fast | waiting-for-you after the scanner's settle | STA-01 E2 |
| Type-ahead during a tool | enqueue → absorbed mid-turn → a `queued_command` attachment; ONE turn, answered by the newest message | running throughout → waiting-for-you | STA-01 E3 |
| AskUserQuestion | heartbeat `waiting` ("input needed") + a Notification | needs-approval in ~1–5 s; Enter → waiting-for-you | STA-03 |
| A Write with skip-permissions off | heartbeat `waiting` ("permission prompt") + a Notification (an `echo` runs without asking) | needs-approval; Enter → waiting-for-you | STA-03 E3 |
| `error 400` (not retried) | an API-error message | error, the card's "⚠ 400: …" line; the runner paused (stop-on-error) | STA-04 |
| `error 404 not_found_error` | "There's an issue with the selected model …" | error (not done) | STA-04 E4 |
| `error 529 … times=2` | retried silently | running → waiting-for-you, never error | STA-04 E1 |
| `error 401` / `error 429` / `error 500` | retried up to 15 times with backoff, minutes ("Retrying in Ns · attempt n/15"); a prompt typed meanwhile is queued; `CLAUDE_CODE_MAX_RETRIES=1` changed nothing | running the whole time; Esc → waiting-for-you | STA-04 E6 |
| `error 529` on every attempt | three attempts, then "API Error: Repeated 529 Overloaded errors …" | error, stop-on-error | STA-04 E6 |
| `say …; drop` | the partial text kept, then "API Error: Connection lost mid-response …" | error | STA-04 E2 |
| `say …; hold g` + the gate failed (an error event mid-stream) | the partial text kept, then "API Error: Server error mid-response …" — no retry | error | STA-04 E2, STA-14 E5 |
| `stop max_tokens` / `stop model_context_window_exceeded` | continues by itself in the same turn (a meta user line "Output token limit hit. Resume directly …") | running → waiting-for-you | STA-14 |
| `stop refusal; next; reply …` | the refusal line, `informational` "… safeguards stopped the response above · continuing once with that noted", a meta nudge, the retry | running → waiting-for-you, never error | STA-14 |
| `stop refusal; next; …; stop refusal` | a retry on a fallback model (`model_refusal_fallback`, "Opus 4.8"), refused → `model_refusal_no_fallback` + an API error (stop_reason `refusal`) | error + stop-on-error | STA-14 |
| `error 400 "prompt is too long …" times=1` | PreCompact → a compaction agent (SubagentStop) → SessionStart(compact) → PostCompact → the turn sent again | running throughout → waiting-for-you | STA-14, STA-06 E4 |
| `error 400 "prompt is too long …"` on every attempt | the same compaction, the 400 again, then "Prompt is too long" (status 400) | running → error, stop-on-error | STA-14 E2 |
| `usage in=99000` with a 100K window | the same hooks mid-turn; the scripted steps are gone after it (the mock answers its default) | running throughout | STA-06 |
| `/compact` | an empty UserPromptSubmit, PreCompact, SubagentStop, SessionStart, PostCompact (manual) | running; at PostCompact waiting-for-you, then **running again for ~30 s** (STA-K3) | STA-06 |
| Esc mid-stream / during a tool | "[Request interrupted by user]" / "… for tool use]"; no Stop hook | waiting-for-you ([recon-stop] interrupted), the interrupt hold | STA-12 |
| Esc before any output | the prompt back in the box; nothing written | running ~30 s more (STA-K2), then waiting-for-you | STA-12 E1 |
| A background Bash | the turn ends; its `<task-notification>` is a turn of its own (a UserPromptSubmit) | waiting-for-you → running → waiting-for-you; the notification not recorded as typed | STA-13 |
| Any Agent call | ALWAYS asynchronous ("Async agent launched successfully", even with `run_in_background: false`): the turn ends at once, a notification turn follows | waiting-for-you while it works; running again once its work outlived the turn by 20 s | STA-05 |
| A Stop hook that blocks once | `hook_blocking_error` + a "Stop hook feedback" user line; the turn continues | **waiting-for-you during the continuation**, running ~20 s later (STA-K4) | STA-13 |
| `/exit` | SessionEnd (5 s budget) | done — or, when the hook was cancelled on the busy desktop, waiting-for-you for good (STA-K1) | STA-01 |
| claude.exe killed right after a turn / mid-turn | nothing | **running** 20 s later and for good / running for good (STA-K1) | STA-11 |
| `disableAllHooks` | no hook at all | running ([recon-run]) → waiting-for-you ([recon-stop]) | STA-16 |
| `/model sonnet` | a "Switch model?" dialog; nothing written | no change | STA-15 |

Surfaces that must always agree for a session (each test checks all it can see): the tab's status dot
(Running blue · Waiting goldenrod · Needs approval orange-red · Error crimson · Done green · Idle gray),
the Triage Board column, the side-panel card's dot, the per-tab badge, the Explorer Tree row, the
engine record ([ENG] census `state`), and Telegram's topic icon (F-TG).

---

### STA-01 · A turn's states, everywhere at once

> **Since** 0.7.x (whole line) · **P0** · **Needs** mock (`hold`) · **Spec** HOOKS.md *State machine*

**Promise.** A session's state follows its turn on every surface within a scan tick, with no blips.

**Phases**
1. **Idle after launch.** *Do:* launch F-S without a first prompt. *Expect:* gray dot, Idle column (or the
   observer's `○ claude · unlinked` badge until the first prompt gives it a transcript id — §11d).
2. **Running.** *Do:* type `go [[mock hold g1; reply "done"]]` + Enter. *Expect:* blue dot, Running column,
   badge Running; the side-panel card moves under its sort; never a Waiting in between. *Check:* [LOG]
   `[UserPromptSubmit]`; [ENG] census `running`.
3. **Waiting for you.** *Do:* release g1. *Expect:* goldenrod dot, Waiting column. *Check:* [LOG] `[Stop]`.
4. **Unread.** *Do:* have another tab selected while it finishes. *Expect:* the red flash ring blinks on the
   dot; the taskbar count +1 (17-display DSP-01); visiting the tab clears both.
5. **Done.** *Do:* type `/exit`. *Expect:* green dot, Done column; the tab stays (a shell prompt with
   claude's resume line). Done comes ONLY from claude's SessionEnd hook, whose budget is 5 s: on a busy PC the hook
   was cancelled and the session stayed Waiting for you (STA-K1).

**Edge cases**
- **E1 Waiting → Idle decay** only after the session was READ (visited) and the cog's Waiting-for-you
  timeout passed (**default 4320 min = 3 days** in code — CLAUDE.md's "60 min" is stale; slider 1 min–7 d,
  free text, "Never"). An unread or Mark-Unread card keeps Waiting.
- **E2 Fast turn under 2 s** (`[[mock fast; reply "x"]]`) → the Stop reads stale; the scanner settles it to
  Waiting ~2.5 s later; no stuck Running.
- **E3 Two quick prompts** (type-ahead during a running turn) → stays Running through the batch, one
  Waiting at the end. Measured (2.1.291): a prompt typed while a tool runs is queued and ABSORBED into the running turn
  (`queue-operation enqueue` → `remove` `absorbed_mid_turn` → a `queued_command` attachment), so it is one turn, which
  the newest message answers; both prompts fire a UserPromptSubmit and are recorded as typed.
- **E4 The empty duplicate UserPromptSubmit** → state only, no extra row, no false Waiting.

---

### STA-02 · No flicker when a turn starts

> **Since** 0.7.9 · **P0** · **Needs** mock, a frame-accurate state sampler · **Spec** HOOKS.md *State machine* (turn-start race)

**Promise.** Starting a turn never shows Running → Waiting → Running (it did on ~29% of prompts).

**Phases**
1. **Sample.** *Do:* sample the census every 50 ms; send 20 prompts (`[[mock delay 1s; reply "x"]]`) one per
   settled Waiting. *Expect:* each turn reads Running continuously from its prompt to its end. *Check:*
   [ENG] no Waiting sample inside a turn; [LOG] scanner.log may show `[recon-stop-hold]` (the prevented
   flicker), never a `[recon-stop]` inside a turn.

**Edge cases**
- **E1 A prompt Claude never records** (Esc during the hook phase) → the hold expires after 30 s and the
  state settles; scanner.log says it "never reached the transcript".

---

### STA-03 · Questions and permissions read Needs approval

> **Since** 0.7.x · **P0** · **Needs** mock (`ask`), skip-permissions off for E3 · **Spec** CLAUDE.md *blocked-on-user — THREE belts*

**Promise.** A session waiting for the user's answer reads Needs approval at once and leaves it the moment
the answer is given.

**Phases**
1. **Ask.** *Do:* `[[mock ask "Pick a side?" Left,Right; reply "ok"]]`. *Expect:* orange-red dot, Needs
   approval column within ~2 s (claude's `waiting` heartbeat), even if no hook fires.
2. **Answer.** *Do:* wait for the dialog to settle, Enter. *Expect:* Running (briefly), then Waiting; no
   unread flash for a sub-3 s Running blip after an answer.

**Edge cases**
- **E1 A slow answer write** (the dialog's tool_use line written late) → still Needs approval (the presence
  belt).
- **E2 Answered at the PC while a Telegram question card is open** (F-TG) → the card closes as answered by
  the PC (10-telegram-questions).
- **E3 A permission prompt** (cog: skip permissions OFF, `[[mock write <a file in the folder> "x"; reply "y"]]`) →
  Needs approval (claude's heartbeat `waiting` "permission prompt", `[recon-block-presence]`, and a Notification);
  approve → Waiting, the file written. ⚠ Measured on 2.1.291: an `echo` runs WITHOUT asking in the default mode
  (`⏸ manual mode on`), so it cannot trigger the prompt.
- **E4 A resumed session whose transcript ends on an unanswered question** (known gap, memory
  `stale-askuserquestion-needsapproval`) → reads Needs approval forever while claude's screen says the
  question was declined — expected to fail until fixed.

---

### STA-04 · API errors read Error and recover

> **Since** 0.7.7 / 0.7.9 · **P0** · **Needs** mock (`error`, `drop`) · **Spec** CLAUDE.md *API-error turn → Error*

**Promise.** A turn that dies on an API error shows **Error** with Claude's message on the card, and leaves
Error as soon as work resumes.

**Phases**
1. **Final error.** *Do:* `[[mock error 500 api_error "boom"]]` (Claude retries, then gives up — use a
   non-retryable `error 400 invalid_request_error "x"` for speed). *Expect:* crimson dot, Error column, the
   card's `✕` line shows the message (and status). *Check:* [LOG] scanner.log `[recon-error]`; [TR]
   `isApiErrorMessage: true`.
2. **Retry.** *Do:* type `again [[mock reply "ok"]]`. *Expect:* Running at the prompt (not after), then
   Waiting. *Check:* [LOG] `[recon-run] … (user retried after an API error — Error -> Running)` when the
   pull path catches it.

**Edge cases**
- **E1 A retried error that succeeds** (`error 529 … times=2; reply "x"`) → never Error.
- **E2 A cut stream** (`say "half"; drop`) → Error "Connection lost mid-response" (and INT-01).
- **E3 The rewind of a double-Esc** to before the error → released to Waiting
  (`[recon-error-release]`).
- **E4 Error + SessionEnd** (a model that 404s: `--model` a bogus id via "Specify a model…", mock answers
  404 not_found_error) → Error, not Done; stop-on-error pauses the runner. Measured with the mock's 404 (no bogus
  model needed): Claude Code writes "There's an issue with the selected model (…). It may not exist or you may not
  have access to it. Run /model to pick a different model." after one retry; Error, `[stop-on-error]`.
- **E5 A tool failure** (`tool_result.is_error`) → stays Running (not an API error).
- **E6 Retries that run out** — Claude Code retries 429, 5xx and 401 by itself (the TUI says "Retrying in Ns ·
  attempt n/15"), for minutes, and the session reads Running the whole time (correct: the turn is alive); a prompt
  typed meanwhile is queued by claude ("Press up to edit queued messages"); Esc ends the turn (Waiting for you). A
  529 is given up after three attempts ("Repeated 529 Overloaded errors"): Error with its message. When any class
  runs out, the final error reads Error with its message (STA-14 E5 has each class, as measured).
- **E7 What put real sessions in Error** (this PC, "Claude Code's events, measured"): a prompt too long that compaction
  could not fix (STA-14 E2), refusals (STA-14 phase 3), usage / session limits (a 429 whose response headers say the
  limit — the mock sends no headers yet), broken streams (E2; a server error mid-stream: STA-14 E5), 500s, not logged
  in. Each must show its own message on the card.

---

### STA-05 · Long quiet work stays Running

> **Since** 0.7.x · **P1** · **Needs** mock (`delay`, `agent`) · **Spec** CLAUDE.md *Subagent / fork activity*

**Promise.** A turn that is busy but silent in the transcript (a long tool call, a subagent, a background
teammate) keeps reading Running; it settles to Waiting only when the work is really over.

**Phases**
1. **Long tool call.** `[[mock bash "sleep 40"; reply "done"]]` (Git Bash `sleep`) → Running for 40 s.
2. **Subagent.** `[[mock agent "[[mock delay 30s; reply \"sub\"]]"; reply "parent"]]` → Running while the
   subagent writes its side file; Waiting after the parent's reply. ⚠ **Not reachable on 2.1.291 as the suite runs
   it:** every Agent call runs ASYNCHRONOUSLY — its tool result is "Async agent launched successfully" at once (even
   with `run_in_background: false`), the parent's turn ends, and the agent's completion arrives as a notification turn
   (STA-13). The `agent` shorthand therefore tests phase 3.
3. **Background work outliving the turn** (`run_in_background` agent) → after the parent's end_turn the
   session re-lights Running while the background agent writes (`[recon-subagent]`), then Waiting ~15 s
   after it quiets. Measured (an agent of four 8 s Bash calls): waiting-for-you at the parent's end, **running 22 s
   later** (`[recon-subagent] … (presence=busy, outlived turn-end, Waiting -> Running)` — the 20 s grace), then its
   notification turn, then waiting-for-you. An agent that writes nothing for its first 20 s (one long model call) reads
   waiting-for-you while it works.

**Edge cases**
- **E1 claude's own heartbeat `busy` / `shell`** → holds Running; a quiet transcript is not a turn end.
- **E2 The flash ring** does not fire for the Running ↔ Waiting re-light caused by background work… only on a
  real needs-you edge.

---

### STA-06 · Compaction keeps a running session Running

> **Since** 0.7.9 · **P1** · **Needs** mock (classifies the compaction request), `usage` · **Spec** HOOKS.md *Compaction*

**Phases**
1. **Manual /compact.** *Do:* at Waiting, type `/compact`. *Expect:* Running during the compaction (blue),
   then Waiting; the card's Ctx changes to the post-compaction size at the end (not at the next turn).
   *Check:* [LOG] `[PreCompact]`, `[PostCompact]`; [TR] `compact_boundary` with `postTokens`. Measured (2.1.291):
   an EMPTY UserPromptSubmit starts it (`[ups] … chars=0 -> EMPTY`, then `[recon-run]`), then PreCompact, a
   SubagentStop (the compaction runs as an agent), SessionStart(compact) and PostCompact(manual) → Waiting — and 2 ms
   later the scanner puts it back to **Running** for ~30 s (STA-K3).
2. **Auto-compaction during a turn.** *Do:* a turn whose `usage in=` crosses the auto-compact threshold,
   then continues. *Expect:* Running throughout (no Idle blip), the turn's accounting intact; the runner
   does not send during the window. Measured (2.1.291 runs Opus 5.5 with a window the mock's 190K usage does not
   fill, so the test sets `CLAUDE_CODE_AUTO_COMPACT_WINDOW=100000` and `usage in=99000`): after the tool call Claude
   Code compacts (PreCompact → SubagentStop → SessionStart → PostCompact `auto`, a `compact_boundary` with trigger
   `auto`) and goes on; running throughout → waiting-for-you. The compaction drops the scripted prompt, so the rest of
   the turn gets the mock's default answer.

**Edge cases**
- **E1 A prompt typed during a manual compaction** → queued by claude, sent after it; Waiting ~0.6 s, then
  Running (measured; acceptable).
- **E2 A compaction that fails / is cancelled** → the window closes on claude's idle ≥ 5 s or the 15-min cap.
- **E3 The summary after a compaction** lists the earlier segment's prompts (15-transcripts TRN-06).
- **E4 A prompt too long** (a 400 "prompt is too long") → Claude Code compacts by itself and sends the turn again:
  Running throughout (STA-14 phase 4); when compaction cannot make it fit, Error (STA-14 E2).

---

### STA-07 · A turn cut off by a crash reads Error

> **Since** 0.7.9 · **P0** · **Needs** mock (`hold`), restart / kill · **Spec** HOOKS.md *Cut-off turns*

**Promise.** A turn that never finished and that no process can still be running reads **Error**
("The last turn did not finish: the session was shut down while the turn was running. Send a message to
continue it.") — never a ready Idle the runner would send into.

**Phases**
1. **Mid-turn.** *Do:* `[[mock bash "echo a"; hold g1; reply "end"]]` (a tool result, then a held response).
2. **Kill.** *Do:* kill the GUI process (only `Agentmaster.exe`; the hosts drain and end the claudes).
3. **Reopen.** *Do:* relaunch, Yes to reopen. *Expect:* the dormant tab reads Error with the cut-off
   message; the runner of that session goes Paused (stop-on-error). *Check:* [LOG] scanner.log
   `[recon-cutoff]`; [ENG] `midTurnAtRestore` was set.
4. **Continue.** *Do:* enter the tab (claude resumes; with `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` it continues
   by itself) — or type `go on [[mock reply "ok"]]`. *Expect:* Running, then Waiting.

**Edge cases**
- **E1 Graceful quit mid-turn** → the same (the turn was cut off by the close).
- **E2 A bare prompt with no reply** at the quit → Error only through the restore-time hint (an Esc before
  any reply leaves the same tail and is NOT an error otherwise).
- **E3 A fork that never ran its own turn** → never cut off.
- **E4 A question pending at the quit** → Error, not a stale Needs approval.
- **E5 Deactivate or Restart mid-turn** → Error after the park/restart.
- **E6 Claude's placeholder `No response requested.`** written over the cut-off at resume → does not make it
  a finished turn.

---

### STA-08 · One conversation in two tabs has one host

> **Since** 0.8.0 · **P0** · **Needs** mock, two tabs, `/resume` · **Spec** OBSERVER.md §20; UI_THREAD.md §12

**Promise.** When the same conversation runs in two tabs, the session stays bound to one host tab; the
other tab says so; nothing hops, nothing leaks.

**Phases**
1. **Duplicate.** *Do:* F-S in tab A; open a shell tab B, run `claude --resume <A's session id>` (the shim
   adds our hooks). *Expect:* tab A keeps the session (title, color, badge, queue); tab B's badge reads
   `○ claude · same conversation as another tab · unlinked`.
2. **Activity in both.** *Do:* send a prompt in B, then in A. *Expect:* the registry record stays bound to
   A (census: one tab with the session id); state follows A's claude only. *Check:* [LOG]
   `[duplicate-host] dropped …` once per pair; no repeated `[rehome]` / `[liveness] dead -> archived`
   lines; the board rebuild count stays low.
3. **Close A.** *Expect:* the session can move to B (its claude then hosts it) — verify the takeover is
   single and stable.

**Edge cases**
- **E1 A third tab running it** → still one host.
- **E2 A Restart copy** (tab menu Restart in A while B runs it) → no hop loop.
- **E3 Leak check** (`tools/leakhunt` or a handle count before/after 10 minutes of duplicates) → flat.

---

### STA-09 · Large transcripts never strand Running

> **Since** 0.7.7 / 0.7.9 · **P1** · **Needs** a seeded > 4 MB transcript, mock · **Spec** CLAUDE.md Gotcha *A windowed tail reader…*

**Phases**
1. **Seed.** Resume a session whose transcript is 4 MB (F-CORP) and whose tail is an old `tool_use`.
2. **Turn.** `[[mock reply "x"]]`. *Expect:* Running → Waiting within seconds; never Running for
   minutes.
3. **One huge line.** A turn that writes a > 1 MiB tool result line (`write` a 2 MB file and `read` it back) →
   the state still settles.

---

### STA-10 · `/clear` keeps the tab managed

> **Since** 0.7.9 · **P1** · **Needs** mock · **Spec** CLAUDE.md *Compaction, /clear and resume*

**Phases**
1. **Clear.** *Do:* F-S, type `/clear`, then `new [[mock reply "n"]]`. *Expect:* the tab follows the new
   conversation (new session id in the census; the title kept or re-derived per Rule #11); the record is
   NOT `external`; Deactivate and Restart work on it.
2. **Summary link.** *Expect:* the new session's summary lists the cleared conversation above a separator
   (15-transcripts TRN-06).

---

### STA-11 · A killed claude leaves a clean tab

> **Since** 0.8.0 · **P1** · **Needs** the fullscreen lane, process kill · **Spec** CLAUDE.md Gotcha *A killed agent leaves its fullscreen terminal modes on*

**Promise.** When a session's claude dies abruptly, its tab drops to a clean PowerShell prompt with one
note — no escape-sequence garbage — and Restart session brings the conversation back.

**Phases**
1. **Kill.** *Do:* F-S in the fullscreen lane; `Stop-Process` its `claude.exe`. *Expect:* the tab shows
   `[Agentmaster] The agent exited with code 1. …` (naming Restart session) above a clean `PS <cwd>>`.
2. **Mouse and focus.** *Do:* move the mouse over the tab, switch away and back. *Expect:* no `[555;…`,
   `[I`, `[O` text appears; Ctrl+C and arrows behave at the prompt.
3. **Restart.** *Do:* tab menu → Restart session. *Expect:* the conversation resumes.

**Edge cases**
- **E1 `/exit`** → claude's own resume line, no note.
- **E2 Classic renderer** → the same, no garbage.
- **E3 Known gap — worse than filed** (STA-K1): never Done. Killed long after its turn, the session keeps Waiting for
  you; killed mid-turn, it never leaves Running; killed within a survey (~2 s) after its turn, while the observer last
  read claude's heartbeat as `busy`, it reads **Running** ~20 s later and keeps it.

---

### STA-12 · Esc ends a turn into Waiting for you

> **Since** ≤ 0.7.7 (the interrupt marker), 0.7.9 (the interrupt hold) · **P0** · **Needs** mock (`hold`) · **Spec**
> CLAUDE.md *Engine* (missed-Stop reconciliation (a) interrupt), DELIVERY.md §14

**Promise.** Esc stops the turn, and the session reads Waiting for you within a scan or two although Claude Code fires
no Stop hook for it; the Prompt Queue Runner holds its next prompt until the user's next message.

**Phases**
1. **Mid-stream.** *Do:* `talk [[mock say "thinking out loud"; hold g; reply "never"]]`, Esc once the text shows.
   *Expect:* running → waiting-for-you. *Check:* [TR] `[Request interrupted by user]`; [LOG] scanner.log
   `[recon-stop] <id> q=0 interrupted`, hooks.log `[interrupt-hold] <sid8> autorunner Full -> Off`; [MOCK] the
   request's client gone.
2. **During a tool.** *Do:* `[[mock bash "sleep 60"; reply "slept"]]`, Esc while it runs. *Expect:* waiting-for-you.
   *Check:* [TR] `[Request interrupted by user for tool use]`.
3. **The next message.** *Do:* type a prompt. *Expect:* the runner's held mode comes back (`[interrupt-resume]`, the
   PQR flows).

**Edge cases**
- **E1 Esc before any output** (the request still unanswered) → Claude Code cancels it, puts the prompt back in the
  box and writes NOTHING (no marker, no Stop): the session reads running ~30 s more (STA-K2), then waiting-for-you
  (`[recon-stop-idle] … presence=idle`), the box's draft showing as dots.
- **E2 A double Esc** → the rewind menu; no state change (an error's rewind: STA-04 E3).
- **E3 Esc on a question or a permission prompt** → the dialog closes, the turn ends; waiting-for-you.

---

### STA-13 · Turns nobody typed

> **Since** ≤ 0.7.7 · **P1** · **Needs** mock (`next`), a user Stop hook (phase 2) · **Spec** CLAUDE.md *Subagent /
> fork activity*, *Engine* (the noise filter)

**Promise.** A turn Claude Code starts by itself — a finished background task's notification, a Stop hook that sends
it back to work, a message absorbed mid-turn — reads Running while it runs and ends Waiting for you, and is never
recorded as something the user typed.

**Phases**
1. **A background task's notification.** *Do:* `[[mock tool Bash {"command": "sleep 20", "run_in_background": true,
   "description": "bg"}; reply "started"]]`. *Expect:* running → waiting-for-you (the turn ended) → ~20 s later
   running (the notification's turn) → waiting-for-you. *Check:* [TR] `queue-operation enqueue` / `dequeue`, a user
   line `<task-notification> …`; [LOG] its UserPromptSubmit `[ups] <sid8> chars=N … -> noise (not recorded)`.
2. **A Stop hook that blocks.** *Do:* a user Stop hook (Claude Code's settings) that answers
   `{"decision": "block", "reason": "keep going"}` once; `[[mock say "first stop"; next; hold g; reply "continued"]]`,
   the continuation held 20 s. *Expect:* running throughout — the turn goes on. **Today:** waiting-for-you during the
   continuation (our own Stop hook already said "turn complete"), running again only ~20 s after the transcript's last
   write (`[recon-subagent] … outlived turn-end`), then waiting-for-you (STA-K4). *Check:* [TR] `hook_blocking_error`,
   the user line "Stop hook feedback:\nkeep going", then the continuation.
3. **A message absorbed mid-turn** — STA-01 E3.

**Edge cases**
- **E1 A background agent** (every Agent call on 2.1.291) → the same notification turn; while it works, STA-05 phase 3.
- **E2 Two notifications at once** → queued: one turn each.
- **E3 A scheduled task** (`system/scheduled_task_fire`, 365 on this PC) → a turn nobody typed; not mocked yet.
- **E4 A teammate's message** (`<teammate-message`, 13,760 lines on this PC) → each wakes the lead with a
  UserPromptSubmit the noise filter keeps out of the typed record; not mocked yet.

---

### STA-14 · Claude Code's own recoveries keep the turn alive

> **Since** 0.7.7 (retries), 0.7.9 (compaction) · **P0** · **Needs** mock (`stop`, `next`, `error … times=N`) · **Spec**
> CLAUDE.md *API-error turn → Error*, HOOKS.md *Compaction*

**Promise.** When Claude Code recovers by itself — continuing after max_tokens, retrying a refused answer once,
compacting a prompt that grew too long, retrying a failed request — the session reads Running throughout and ends
Waiting for you; it reads Error only when Claude Code gives up, with Claude Code's own message.

**Phases**
1. **max_tokens.** *Do:* `[[mock say "partial"; stop max_tokens; next; hold g; reply "the rest"]]`, the continuation
   held 10 s. *Expect:* running the whole time, then waiting-for-you. *Check:* [TR] the assistant line with
   `stop_reason: max_tokens`, the meta user line "Output token limit hit. Resume directly …", the continuation.
2. **A refusal, answered on the retry.** *Do:* `[[mock say "I cannot help with that."; stop refusal; next; reply
   "answered after the retry"]]`. *Expect:* running → waiting-for-you, never error. *Check:* [TR] the refusal line
   (stop_reason `refusal`, the mock's `stop_details`), `system/informational` "Opus 5.5's safeguards stopped the
   response above · continuing once with that noted", a meta nudge ("Your response above was stopped by a safety
   classifier …"), the answer.
3. **A refusal, refused again.** *Do:* `[[mock say "I cannot help with that."; stop refusal; next; say "Still no.";
   stop refusal]]`. *Expect:* error; the card's line "API Error: … safeguards flagged this message …"; the runner
   paused (stop-on-error). *Check:* [TR] `model_refusal_fallback` (Claude Code retried on a fallback model, "Opus
   4.8"), `model_refusal_no_fallback`, an API-error line with stop_reason `refusal`; [MOCK] three requests.
4. **A prompt too long.** *Do:* `[[mock error 400 invalid_request_error "prompt is too long: 250001 tokens > 200000
   maximum" times=1; reply "after the compaction"]]`. *Expect:* running throughout → waiting-for-you, never error, no
   waiting blip during the compaction. *Check:* [TR] `compact_boundary`; [MOCK] main (400) → compact → main;
   [LOG] PreCompact … PostCompact.

**Edge cases**
- **E1 `model_context_window_exceeded`** → handled like max_tokens (the same continuation line).
- **E2 A prompt too long that compaction cannot fix** (the most common Error on this PC: 91 of 231) → Claude Code
  compacts (PreCompact … PostCompact), sends again, gets the 400 again and writes "Prompt is too long" (an API error,
  status 400): running through the compaction, then error, stop-on-error.
- **E3 Retries** → STA-04 E1 (succeeds: never error), E6 (runs out).
- **E4 `CLAUDE_CODE_DISABLE_REFUSAL_RETRY`** → no retry: the API error at once (not measured).
- **E5 How each error class retries** (measured): a 529 three times, then "API Error: Repeated 529 Overloaded errors.
  The API is at capacity …" (error after ~5 s); a 500, a 429 and a 401 up to 15 times with backoff ("Retrying in Ns ·
  attempt n/15") — minutes of running; `CLAUDE_CODE_MAX_RETRIES=1` changed none of them (it governs other callers);
  a 400 / 404 never. An error event in the middle of a started stream (a 529 or a 500 after some text) is not
  retried: the partial text is kept and "API Error: Server error mid-response. The response above may be incomplete."
  ends the turn — error.

---

### STA-15 · Claude Code's local commands change no state

> **Since** ≤ 0.7.7 · **P2** · **Needs** — · **Spec** DELIVERY.md §18

**Promise.** A command Claude Code handles by itself (a dialog, /status, /model) changes no session state; the ones
that do have flows of their own (/compact STA-06, /clear STA-10, /exit STA-01).

**Phases**
1. **/model.** *Do:* `/model sonnet`. *Expect:* Claude Code's "Switch model? … ❯ 1. Yes, switch to Sonnet 5.5 /
   2. No, go back" dialog; Esc; waiting-for-you throughout, nothing in the transcript.

**Edge cases**
- **E1 /status, /cost, /help** → a local-command echo (`system/local_command`, 1,089 on this PC); no state change.
- **E2 /add-dir from Telegram** → its menu card (10-telegram-questions TGQ).

---

### STA-16 · The state holds when hooks never arrive

> **Since** ≤ 0.7.7 · **P1** · **Needs** Claude Code settings (`disableAllHooks`) · **Spec** CLAUDE.md *The Fleet
> Observer*, HOOKS.md *Hook invocation*

**Promise.** The state never depends on a hook alone: when Claude Code's hooks never reach Agentmaster (hooks
disabled, a hook cancelled at its timeout on a busy PC, a hand-typed claude), the scanner and claude's own heartbeat
carry it, a few seconds later.

**Phases**
1. **Hooks disabled.** *Do:* Claude Code's settings `{"disableAllHooks": true}`, launch, a held turn. *Expect:*
   running within ~3 s (`[recon-run] … prompt hook missed/folded`), running while held, waiting-for-you after it
   (`[recon-stop]`). *Check:* [LOG] no `[SessionStart] <id>` line at all.

**Edge cases**
- **E1 A hook cancelled at its timeout** (`hook_cancelled`: 101 in this PC's transcripts; on the busy test desktop
  every hook took 10–22 s) → the state follows through the scanner; a late hook is weighed by the stale-Stop rules.
- **E2 SessionEnd cancelled** (its 5 s budget) → no Done (STA-K1).

---

## Known defects (expected failures until fixed)

### STA-K1 · A claude that ends without its SessionEnd never reads Done — killed, it can read Running for good
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (probes stprobe1 p23, stprobe2 q16) ·
> `e2e/suites/test_session_state.py::test_known_defect_a_killed_claude_keeps_reading_running` (`known_defect`) ·
> evidence: the probes' hooks.log / scanner.log lines below

**What it is.** Done comes only from claude's SessionEnd hook.
- **Killed right after a turn.** After `Stop-Process` of the session's claude.exe the tab shows the console guard's
  "[Agentmaster] The agent exited with code 15. To start it again, right-click this tab and choose Restart session."
  over a clean prompt, but the session reads **running** 20 s later and keeps it: scanner.log `[recon-subagent] <id>
  (presence=busy, outlived turn-end, Waiting -> Running)`, though the observer's census already counted no claude
  (`[observer] census claudes=0`).
- **Killed mid-turn.** It never leaves running.
- **/exit on a busy PC.** The SessionEnd hook missed its 5 s budget ("SessionEnd hook … failed: Hook cancelled") and
  the session stayed waiting-for-you, the tab at a shell prompt (stprobe1 p23; the same /exit read done in stprobe2).

**The cause.** `SessionInfo::presenceStatus` comes from the S-lane's survey through `ObserveClaude` and is never
cleared when the process goes away (nothing observes a dead claude). The scanner's "work outlived the turn" rule
(`SessionScanner.cpp`: `externalOutlivesTurn = … || (presenceWorking && parentQuietMs > kScanExternalWorkGraceMs)`)
trusts the stale `busy` 20 s after the transcript's last write, promotes Waiting → Running, and the same expression
then holds every recon-stop. (`turnInFlight` for the command watch already checks `ProcessAlive(s.pid)` first.)

**Expected.** Within a scan or two of the claude's exit: not running — Done (the conversation ended), or Error ("the
agent exited") when a turn was in flight — and the presence fact cleared. A fix could clear `presenceStatus` /
`presenceWaitingFor` when the observer no longer finds the session's pid (or gate `presenceWorking` on
`ProcessAlive(s.pid)`), and derive the end from a managed claude's exit (the observer logs `[activity] wt=… claude ->
pwsh`).

### STA-K2 · Esc before any output reads Running for ~30 s
> **P2** · Found by the e2e suite on 2026-10-23 against 0.8.0 (probe stprobe2 q14) ·
> `e2e/suites/test_session_state.py::test_known_defect_esc_before_any_output_reads_running_for_half_a_minute`
> (`known_defect`)

**What it is.** Esc while the model has sent nothing yet: Claude Code cancels the request, puts the prompt back in the
input box and writes nothing to the transcript. The session keeps reading **running** until the presence-idle release
(`[recon-stop-idle] … q=0 presence=idle`), then waiting-for-you: 28 s measured.

**The cause.** The tail is a bare prompt with no reply, and a Running session with a cleared tail is released only
after 30 s of quiet (`kScanPresenceIdleRunningQuiescenceMs`, the guard against an API-retry pause read as a turn end).

**Expected.** Waiting for you within a few seconds: claude's own heartbeat reads `idle` at once, and the prompt is
back in the box (the pending-input monitor reads it as a draft) — two signals no API-retry pause gives.

### STA-K3 · A manual /compact reads Running ~30 s after it ended
> **P2** · Found by the e2e suite on 2026-10-23 against 0.8.0 (probes stprobe2 q09, stprobe3 r01) ·
> `e2e/suites/test_session_state.py::test_known_defect_compact_reads_running_half_a_minute_after_it_ended`
> (`known_defect`)

**What it is.** `/compact` reads running while it compacts (correct), and its PostCompact(manual) sets waiting-for-you
— then **2 ms later** the scanner sets running again (`[recon-subagent] <id> (presence=busy, Waiting -> Running)`),
and the session stays running until the presence-idle release 30 s later (`[recon-stop-idle] … presence=idle`):
waiting-for-you ~30 s after the compaction ended (r01: PostCompact at 21:24:56.98, waiting again at 21:25:27.33).

**The cause.** When the PostCompact hook lands, claude's heartbeat still reads `busy` (it finishes the compaction's
hooks first), and the scanner's busy-heartbeat promotion fires on a Waiting session whose tail (the
`<local-command-stdout>Compacted …` line) is not terminal; the release then needs the 30 s quiet of STA-K2.

**Expected.** Waiting for you at the PostCompact (the compaction window's close is a turn end for the heartbeat
promotion too: it should need a busy EDGE after the window closed, like the presence-resume belt's latch).

### STA-K4 · A Stop hook that blocks leaves the session Waiting for you while Claude Code continues
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (probes stprobe1 p20, stprobe2 q08, stprobe3 r02) ·
> `e2e/suites/test_session_state.py::test_known_defect_a_blocking_stop_hook_reads_waiting_while_claude_continues`
> (`known_defect`) · a Stop hook blocked 164 times in this PC's transcripts

**What it is.** A user's Stop hook answers `{"decision": "block", "reason": "…"}`, so Claude Code does not stop: it
writes `hook_blocking_error` and the user line "Stop hook feedback:\n<reason>" and goes on with the SAME turn. But
Agentmaster's own Stop hook ran beside it and already reported the turn complete: the session reads
**waiting-for-you** while Claude Code works on the continuation. It reads running again only when the busy heartbeat
has outlived the transcript's last write by 20 s (`[recon-subagent] … (presence=busy, outlived turn-end, Waiting ->
Running)`), and waiting-for-you after the continuation's own Stop. Measured (r02, the continuation held 20 s):
waiting-for-you from the first Stop for ~20 s, then running, then waiting-for-you. The runner's 40 s state-settle
hold keeps it from sending into that window (DELIVERY.md §16), and the "has completed" toast is held while the
heartbeat says busy — but the board, the tab dot, the side panel and Telegram's topic all say the turn ended.

**The cause.** Our Stop forwarder cannot know that another Stop hook blocks (the payload's `stop_hook_active` is
false on the first stop), and the "Stop hook feedback" user line that follows is not read as the turn going on.

**Expected.** Running throughout: the scanner can read the `hook_blocking_error` + "Stop hook feedback" lines written
right after a Stop as the turn continuing (a Running promotion, like a prompt's), or the busy heartbeat right after
the Stop as such.
