# 05 · Session state on every surface (STA)

> Conventions and fixtures: [README.md](README.md). Spec: HOOKS.md *State machine*, *Cut-off turns*,
> *Compaction*; OBSERVER.md §20; CLAUDE.md *Engine*, *The tab strip state dot*.

**What shipped.** 0.7.7: a retried API error recovers to Running; an over-1 MiB transcript line no longer
strands Running. 0.7.9: no Running for hours after a quiet turn end (the catch-up read), no
Running → Waiting → Running flicker (the turn-start race guard), a cut-off turn reads **Error**, a
compaction no longer drops Running to Idle, Ctx updates at a compaction's end. 0.8.0: one conversation in
two tabs no longer hops between them (one host tab), a killed fullscreen claude no longer breaks its tab.

## Lifecycle model

```text
                       ┌──── Esc (scanner) / Stop ────┐
 launch ─► Idle ─prompt─► Running ──Stop──► Waiting for you ──read + timeout──► Idle
             ▲              │  ▲                 │
             │      question│  │answered         └─ /exit, claude ended ─► Done
             │              ▼  │
             │         Needs approval
             │
             └─ Error ◄── API error (final) · cut-off turn at restore · a stream lost mid-response
                  └──► Running (next message, a continued turn, claude's busy heartbeat)
```

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
   claude's resume line).

**Edge cases**
- **E1 Waiting → Idle decay** only after the session was READ (visited) and the cog's Waiting-for-you
  timeout passed (**default 4320 min = 3 days** in code — CLAUDE.md's "60 min" is stale; slider 1 min–7 d,
  free text, "Never"). An unread or Mark-Unread card keeps Waiting.
- **E2 Fast turn under 2 s** (`[[mock fast; reply "x"]]`) → the Stop reads stale; the scanner settles it to
  Waiting ~2.5 s later; no stuck Running.
- **E3 Two quick prompts** (type-ahead during a running turn) → stays Running through the batch, one
  Waiting at the end.
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
- **E3 A permission prompt** (cog: skip permissions OFF, `[[mock bash "echo x"; reply "y"]]`) → Needs
  approval; approve → Running → Waiting.
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
  404 not_found_error) → Error, not Done; stop-on-error pauses the runner.
- **E5 A tool failure** (`tool_result.is_error`) → stays Running (not an API error).

---

### STA-05 · Long quiet work stays Running

> **Since** 0.7.x · **P1** · **Needs** mock (`delay`, `agent`) · **Spec** CLAUDE.md *Subagent / fork activity*

**Promise.** A turn that is busy but silent in the transcript (a long tool call, a subagent, a background
teammate) keeps reading Running; it settles to Waiting only when the work is really over.

**Phases**
1. **Long tool call.** `[[mock bash "sleep 40"; reply "done"]]` (Git Bash `sleep`) → Running for 40 s.
2. **Subagent.** `[[mock agent "[[mock delay 30s; reply \"sub\"]]"; reply "parent"]]` → Running while the
   subagent writes its side file; Waiting after the parent's reply.
3. **Background work outliving the turn** (`run_in_background` agent) → after the parent's end_turn the
   session re-lights Running while the background agent writes (`[recon-subagent]`), then Waiting ~15 s
   after it quiets.

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
   *Check:* [LOG] `[PreCompact]`, `[PostCompact]`; [TR] `compact_boundary` with `postTokens`.
2. **Auto-compaction during a turn.** *Do:* a turn whose `usage in=` crosses the auto-compact threshold,
   then continues. *Expect:* Running throughout (no Idle blip), the turn's accounting intact; the runner
   does not send during the window.

**Edge cases**
- **E1 A prompt typed during a manual compaction** → queued by claude, sent after it; Waiting ~0.6 s, then
  Running (measured; acceptable).
- **E2 A compaction that fails / is cancelled** → the window closes on claude's idle ≥ 5 s or the 15-min cap.
- **E3 The summary after a compaction** lists the earlier segment's prompts (15-transcripts TRN-06).

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
- **E3 Known gap:** the session keeps its last state (no Done) until restarted — expected to fail a "Done
  after kill" assertion.
