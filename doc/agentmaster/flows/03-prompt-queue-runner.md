# 03 · The Prompt Queue Runner (PQR)

> Conventions, fixtures (F-…), oracle tags ([UI] [ENG] [LOG] [TR] [MOCK] [OS] [TG]) and the mock script
> language: [README.md](README.md). Spec: DELIVERY.md §5–§18; CLAUDE.md *Prompt Queue Runner*;
> memory `prompt-queue-runner-study-2026-10-21`. The suite: `e2e/suites/test_prompt_queue_runner.py`; the
> runner across claude's own life cycle (exit, kill, Close, Resume here, adoption, `/clear`): [24-claude-lifecycle.md](24-claude-lifecycle.md)
> and `e2e/suites/test_claude_lifecycle.py`.

**What shipped.** 0.7.7: atomic, verified delivery (the delivery gate; fill → read back → submit), the
20 s send spacing, the interrupt hold, the question-guard releases, Ctrl+Shift+Up. 0.7.9: renamed from
"Tests Autorunner", **in every build**, two modes (Paused / Auto; SemiAuto removed), one global pause,
the 40 s state settle, the dormant-tab gate, "(Off globally)" on the badges. 0.8.0: a queued local
command (`/model x`, `/add-dir`) no longer fails the queue.

## Lifecycle model

```text
 a prompt                 Pending ──advance──► Sent ──echo (push UPS / pull transcript)──► Sent+echoed ──► history (SENT, "auto")
                            │  ▲                 │                                              (the turn runs)
          Delete / pop ◄────┘  └── rollback ◄────┤ dormant tab / gate declined / aborted swap
                                                 ├── no echo, at rest 15 s ──► Failed + runner Paused   ([lost-send])
                                                 └── Enter eaten ──► re-press ≤3 ──► give up ──► Failed + Paused
 typed in the terminal ─────────────────────────────────────────────────────────────────► history (SENT, "typed")

 a session's runner        Auto ◄──toggle──► Paused        Auto ──Esc──► "Paused → Auto on your next message" ──your message──► Auto
                            │                                  │
                            └── stopOnError / lost-send / give-up / merge verdict ──► Paused (sticky)

 the app                   Pause all (one process-wide state; every window, every badge) ◄──► Resume (re-asks every held queue)
```

**When an automatic send happens.** All of: the runner is Auto and not globally paused; the session is
open, its claude started (or adopted), and Idle or Waiting for you; no delivery holds the box; the box is
visible and holds no menu; the previous send was picked up; the next Pending prompt is not held by the
question guard; the status has held for the settle window (40 s; E2E lane: 2 s) counted from the later
of the last status change and the prompt's queue time; at least the send spacing (20 s; E2E lane: 1 s)
since the previous automatic send; fewer than `maxAutoSends` (100) automatic sends since the last re-arm.

## Measured (2026-10-24)

The suite (26 tests) and the probe run `pqprobe1` drove the runner against **Agentmaster 0.8.0** and **Claude Code
2.1.291**, with the mock, in a claude-fleet Windows 11 desktop of a loaded PC, at the PRODUCTION values (the 40 s settle,
the 20 s spacing: the E2E lane's short values are not built). What a test can rely on, and what it cannot:

- **A reason is written only when the runner is ASKED.** A change asks it only for a live, started (or adopted)
  session in Idle or Waiting for you whose mode is not Paused and that has a Pending prompt
  (`Scheduler::_wantsIdleStart`); a turn's end and a timed re-check (a settle window ending, a dormant tab) ask too.
  So while a session runs, needs approval, reads Error or Done, or its runner is Paused, NO `[advance-skip]` line is
  written: "not ready …" and "autorunner off" never appeared there (run `pqr1`: five tests that waited for them
  failed). The reasons seen: `queue settle`, `status settle`, `delivery in flight`, `awaiting injection pickup`,
  `global pause`, `question pending …`, `tab dormant …`, `input box not visible[ (a menu/dialog is open)]`,
  `autorunner off (interrupt hold …)` (the Esc's synthesized turn end asks), and `autorunner off` after a turn ends in
  a Paused session. A hold is judged by the mock's silence and no `[send]`, never by a reason that may not come.
- **The decision is the clock.** autorunner.log's `[send] <id> #<n>` is stamped when the runner decides; the mock saw
  the prompt 10–20 s later on that desktop (the paste, the read-back, the Enter, then Claude Code's UserPromptSubmit
  hook, which runs before the model request and took 10–22 s there — flows/05). Time a settle window from `[send]`,
  never from the mock's arrival.
- **The anchor is the LAST status change, and the end of a turn is usually not it.** After almost every turn the side
  panel showed waiting-for-you → running (~20 s later) → waiting-for-you: claude's busy heartbeat outlived the slow
  Stop hook (scanner.log `[recon-subagent] <id> (presence=busy, outlived turn-end, Waiting -> Running)`), and the blip
  moved the anchor. A queued prompt then went 40 s after the blip's end, a minute or more after its turn — the accepted
  status lag of flows/05 working as the safety margin the owner wants it to be. The tests read the anchor off the side
  panel's timeline (its last change before the `[send]`) and accept 37–60 s after it.
- **A released hold goes within one window, not at once.** When a hold ends (the question answered, Auto again, the
  global pause lifted, the user's own message after Esc, a dialog or menu closed), the next decision waits out the
  settle window of the change that release caused, if it caused one: the tests accept the send within ~45 s of the
  release.
- **`/exit` can leave the session ready.** Claude Code cancelled the SessionEnd hook at its 5 s budget ("SessionEnd
  hook … failed: Hook cancelled"), so the session kept reading waiting-for-you after `/exit` (flows/05 STA-K1). The
  runner held the queue only because no Claude input box was on the screen ("input box not visible"; after 60 s the
  `[send-verify] <sid8> queued prompts HELD: no parseable input box for >60s …` warning): the BOX gate, not the state,
  keeps a prompt out of the PowerShell host a claude leaves behind (LIF-02, LIF-03). When the hook does arrive, the
  session reads Done and the runner is not asked at all (run `pqr1`).
- **A wake settles; a restart does not.** A parked tab woken by Activate Tab got its prompt 40.7 s after the wake
  (`started` changed, the window restarted); a session whose ended claude was started again by Restart session got its
  prompt 8 s after the restart, a second before the new claude's SessionStart hook ran (PQR-03 E5 — filed as PQR-K6,
  ACCEPTED by the owner on 2026-10-26: "what we have now is okay").
- **`/clear` leaves the queue behind** with the old conversation (LIF-07 — filed as PQR-K7, a FEATURE by the owner's
  ruling of 2026-10-26: the queue does not follow the tab across `/clear`).
- **0.8.0: a reopened session runs on the cog's default mode.** A queue left Paused came back Auto after a quit and a
  reopen (PQR-15 phase 2) and after Close + Resume here (LIF-04), and went once the tab had started and settled. The
  owner's rule of 2026-10-26 is the opposite — "we need the mode per session persisted" — and the code now keeps it
  (PQR-K8, fixed in code, NOT BUILT).
- **`/model`'s picker reads MenuOpen:** `[advance-skip] <id> (input box not visible (a menu/dialog is open))` for as
  long as it is on the screen (55 s watched, nothing sent); Esc closed it and the prompt — queued 69 s earlier — was in
  Claude's box 3 s later and at the model 10.7 s later: a picker changes no status, so no new window starts (PQR-09
  phase 3; this settles the question noted there).

---

### PQR-01 · One queued prompt is delivered into an idle session

> **Since** 0.7.7 / 0.7.9 · **P0** · **Needs** mock · **Spec** DELIVERY.md §3, §11–§13, §16

**Promise.** A prompt added to the queue of an idle session in Auto is typed into Claude's box exactly
once, exactly as written, after the session has settled — and moves from UPCOMING to SENT.

**Preconditions.** F-S (`w1`), state Waiting for you, runner Auto (the cog default).

**Phases**
1. **Select the session.** *Do:* open the Agent Manager (Home), click the session's board card, open the
   bottom-right pane's **Prompt Queue** tab. *Expect:* the header names the session and its folder; the
   toggle reads `Prompt Queue Runner: Auto` (green ●); `SENT — 1 (1 typed)` (the fixture's first prompt)
   and `UPCOMING — 0`. *Check:* [UI] the texts; [ENG] queue = 1 Typed row.
2. **Queue.** *Do:* type `p1 [[mock reply "one"]]` into the compose box, click **Add to queue**
   (envelope). *Expect:* the box empties and keeps focus; `UPCOMING — 1` shows the row; the tab's badge
   shows `⏳1` and a third row `⏳ p1 [[mock reply "one"]]`; the badge's row 3 also shows the steel-blue
   `settling` cue. *Check:* [UI]; [ENG] the prompt Pending with `queuedAtUnixMs` set; [LOG]
   `[nav] queue <sid8> "p1 …"`.
3. **Settle.** *Do:* nothing. *Expect:* no delivery before the settle window ends; the cue disappears when
   it ends. *Check:* [LOG] autorunner.log `[advance-skip] <sid8> queue settle (…)` (change-deduped);
   [MOCK] no request carrying `p1` yet.
4. **Delivery.** *Expect:* the runner pastes the prompt, reads the box back, then presses Enter; the
   session goes Running (blue dot, board Running column), then Waiting for you. *Check:* [LOG]
   autorunner.log `[send] <sid8> … #1`, hooks.log `[delivered] <sid8> … verified=exact`, `[ups] … echo`;
   [MOCK] exactly one `main` request whose newest user message equals `p1 [[mock reply "one"]]`;
   [TR] exactly one user line with that text; [ENG] status Sent, `echoed` true.
5. **History.** *Expect:* the row moves to `SENT — 2 (1 typed)` with an `auto` pill; `UPCOMING — 0`; the
   badge's `⏳` count and row 3 disappear. *Check:* [UI].

**Edge cases**
- **E1 Invisible characters.** Queue `p1​ [[mock reply "x"]]` (a zero-width space). *Expect:* the paste
  is cleaned with Claude Code's own rule, the send is not cancelled, no lost-send follows. *Check:* [LOG]
  `[delivered] … sanitized=<a>-><b>`; [TR] the cleaned text; [ENG] Sent+echoed (the compare folds the
  character out).
- **E2 Hebrew + emoji.** Queue `שלום ⚠️ [[mock reply "ok"]]`. *Expect:* arrives intact, recorded once.
  *Check:* [MOCK] the exact text; [UI] one SENT row, no extra `typed` row. Measured (2026-10-26, run `pqr5`, 0.8.0):
  an emoji arrives intact (`test_an_emoji_prompt_arrives_intact_and_is_recorded_once`); a prompt with Hebrew is
  never delivered — **PQR-K9**.
- **E3 Multi-line.** Compose three lines (Enter adds a line in the box). *Expect:* one message with the
  newlines kept. *Check:* [TR] one user line holding `\n`s; [MOCK] one request.
- **E4 Long prompt.** 2,000 characters. *Expect:* one message (Claude may show it as a paste placeholder);
  the transcript holds the full text. *Check:* [TR] full text. Measured (2026-10-26, run `pqr6`, 0.8.0): on 40 lines
  it goes whole, verified as the collapsed render `[Pasted text #1 +N lines]`
  (`test_a_long_multi_line_prompt_goes_whole_as_one_message`); on ONE line it is never delivered — **PQR-K11**.
- **E5 Empty compose box.** *Do:* Add to queue with an empty box. *Expect:* nothing is queued.
- **E6 Queued while Running.** *Expect:* the send waits for the turn's end, then the settle.

---

### PQR-02 · Several queued prompts run one per turn, in order

> **Since** 0.7.7 · **P0** · **Needs** mock (`delay`) · **Spec** DELIVERY.md §9, §13, §16

**Promise.** A queue drains one prompt per completed turn, in queue order, never into a running turn,
never two at once.

**Preconditions.** F-S, Auto.

**Phases**
1. **Queue three.** *Do:* queue `s0 [[mock delay 3s; reply "d0"]]`, `s1 …`, `s2 …`. *Expect:*
   `UPCOMING — 3` in order; badge `⏳3`.
2. **Drain.** *Expect:* s0 is sent after the settle; while its turn runs (3 s) nothing else is sent; s1
   follows only after s0's turn ended AND the session re-settled AND the spacing passed; then s2.
   *Check:* [MOCK] for each pair, the next prompt's first request starts after the previous turn's final
   response ended (no overlap); [LOG] three `[send]` lines with gaps ≥ the spacing; [TR] user lines s0,
   s1, s2 in order, each once.
3. **Done.** *Expect:* `UPCOMING — 0`; badge clear; [LOG] `[plan-done] <sid8>`.

**Edge cases**
- **E1 Tool turns.** s0 = `[[mock bash "echo a"; write <w1>\a.txt "x"; reply "done"]]`. *Expect:* s1 waits
  for the whole turn (three model responses), not the first response.
- **E2 A subagent outlives the reply.** s0's reply ends while a background agent still writes. *Expect:*
  the session reads Running until the background work quiets; s1 waits (needs a mock `agent` script with
  a long `delay`).
- **E3 The send budget.** Set **maxAutoSends** to 2 (cog → Prompt Queue Runner) for new sessions, launch,
  queue three. *Expect:* two are sent, the third waits with `[advance-skip] … maxAutoSends reached`; a
  re-arm (toggle Paused → Auto) resets the budget and the third goes.
- **E4 Reordering mid-drain.** Move s2 above s1 while s0 runs. *Expect:* s2 goes before s1 (the send
  resolves the target by id, never a stale index).
- **E5 Deleting the next prompt mid-drain.** Delete s1 while s0 runs. *Expect:* s2 goes next; nothing
  references the deleted row.

---

### PQR-03 · The settle hold waits for a status that holds still

> **Since** 0.7.9 · **P1** · **Needs** mock, realtime lane for the 40 s value · **Spec** DELIVERY.md §16

**Promise.** An automatic send waits until the session has held a sendable status for the settle window,
counted from the later of its last status change and the prompt's queue time; every status change
restarts the window.

**Phases**
1. **Queue anchor.** *Do:* leave F-S Waiting for longer than the window, then queue p1. *Expect:* p1
   still waits a full window from its queue time. *Check:* [LOG] `queue settle (…)`; [MOCK] the send at
   ≥ window after the queue time.
2. **Status anchor restarts.** *Do:* queue p2 while the session is Waiting; before the window ends, type a
   quick prompt `[[mock reply "x"]]` in the terminal. *Expect:* the status change restarts the window:
   p2 goes ≥ one window after that turn ended (not after p2's queue time). *Check:* [LOG]
   `status settle (…)`; timing in [MOCK].
3. **The cue.** *Expect:* while a window runs, the badge's row 3 shows `settling` (steel blue, tooltip
   explains); it clears by itself when the window ends (a timer redraw — no other event needed).

**Edge cases**
- **E1 Global pause on.** *Expect:* no `settling` cue (nothing waits for a send).
- **E2 Send now.** *Expect:* bypasses the window.
- **E3 Telegram prompt** (F-TG). *Expect:* a remote prompt settles 5 s, not 40 s
  (`kRemotePromptSettleMs`) — see TG flows.
- **E4 Realtime lane.** *Expect:* exactly 40 s (± a scheduler tick) after the later anchor.
- **E5 Restart session after claude ended** (killed, or `/exit` whose SessionEnd hook was cancelled — the session
  still reads waiting-for-you). *Expect:* no new window: the prompt held while claude was gone goes as soon as the
  restarted claude's input box draws (measured 8 s after the restart). A restart changes none of the window's facts
  (the state stays waiting-for-you, `started` stays true), unlike a parked tab's wake, which changes `started` and
  waits the whole window. ACCEPTED (the owner, 2026-10-26: "what we have now is okay"; filed as PQR-K6).
  *Check:* [LOG] the `[send]` < 30 s after the restart. Test: `test_restart_session_sends_a_held_prompt_once_its_box_is_back`.

---

### PQR-04 · Paused and Auto, per session

> **Since** 0.7.9 · **P0** · **Needs** mock · **Spec** DELIVERY.md §17.4

**Promise.** Each session has its own mode. Paused never sends by itself; Auto sends when ready. The
pane toggle and the tab badge always agree.

**Phases**
1. **Default.** *Do:* launch a new session. *Expect:* its mode is the cog's **default mode for new
   sessions** (Auto out of the box). *Check:* [UI] pane toggle and badge; [ENG] `autorunner.mode Full`.
2. **Pause from the pane.** *Do:* click the toggle. *Expect:* `Prompt Queue Runner: Paused` (gray ○);
   the badge's mode button reads `Paused` at once. *Check:* [LOG] `[nav] autorunner <sid8> -> Off`.
3. **Queue while Paused.** *Do:* queue two prompts; wait several settle windows. *Expect:* nothing sent. *Check:*
   [MOCK] silence; [LOG] no `[send]`. (`[advance-skip] … autorunner off` is written only when a turn ends in the
   Paused session: a queue alone asks the runner nothing — measured 2026-10-24, "Measured" above.)
4. **Auto from the badge.** *Do:* hover the badge, click its mode button. *Expect:* both surfaces read
   Auto; the queue starts after the settle.

**Edge cases**
- **E1 Change the default.** Cog default = Paused → a new session starts Paused; existing sessions keep
  theirs.
- **E2 A stored SemiAuto** (a seeded `sessions.json` record or cog default `SemiAuto`). *Expect:* loads as
  Paused; queue and other fields kept.
- **E3 Re-arm clears latches.** Paused → Auto resets the send budget and releases a question-guard hold
  (PQR-08).
- **E4 Mode of a session hosted in another window** (F-W2, Manager GLOBAL scope). *Expect:* the toggle
  works from either window; the hosting window's badge follows.

---

### PQR-05 · One global pause for the whole app

> **Since** 0.7.9 · **P0** · **Needs** mock, F-W2 · **Spec** DELIVERY.md §17.3

**Promise.** The toolbar's **Pause Prompt Queue Runner** holds every automatic send in every window at
once, shows the same state everywhere, and on Resume the held queues continue by themselves.

**Phases**
1. **Arrange.** Two sessions (one per window), each Auto with two queued prompts behind a running turn
   (`hold g1`, `hold g2`).
2. **Pause.** *Do:* window A's toolbar **Pause Prompt Queue Runner**. *Expect:* the button reads
   **Resume Prompt Queue Runner** in BOTH windows; every badge reads `Auto (Off globally)` (the mode in
   its color, the suffix gray); no `settling` cue. *Check:* [LOG] autorunner.log `[pause-all] on (…)`.
3. **Turns end.** *Do:* release g1 and g2. *Expect:* both sessions Waiting; nothing sent past several
   settle windows. *Check:* [LOG] `[advance-skip] … global pause`; [MOCK] silence.
4. **Send now still works.** *Do:* Send now a composed prompt into session 1. *Expect:* delivered at once.
5. **Resume.** *Do:* window B's button. *Expect:* both windows show Pause again; both queues continue with
   NO other trigger. *Check:* [LOG] `[pause-all] off (re-requested an advance for 2 session(s) with
   queued work)`; [MOCK] the next prompts arrive.

**Edge cases**
- **E1 A window opened during the pause** shows Resume at once.
- **E2 Restart during the pause.** *Expect:* the app starts resumed (not persisted).
- **E3 Modes during the pause.** Toggle session 1 to Paused during the pause → the badge reads
  `Paused (Off globally)`; after Resume session 1 stays Paused, session 2 sends.
- **E4 An interrupt hold during the pause** → badge `Paused → Auto (Off globally)`.
- **E5 A question hold during the pause** → `❓ held …` stays visible (it still holds after Resume).
- **E6 A Telegram prompt** (F-TG) is sent despite the pause (the user's own message — D30).

---

### PQR-06 · Send now

> **Since** 0.7.7 · **P0** · **Needs** mock · **Spec** DELIVERY.md §6, §17.2

**Promise.** **Send now** types a prompt immediately, ahead of the queue, whatever the mode — after a
confirm — and never loses it when the tab cannot take it.

**Phases**
1. **Composed text.** *Do:* type `now [[mock reply "n"]]`, click **!**. *Expect:* a confirm **Send now?**
   quoting the prompt (≤ 200 characters, newlines as spaces), buttons Send / Cancel.
2. **Cancel.** *Expect:* nothing sent; the text stays in the box.
3. **Send.** *Do:* again, Send. *Expect:* typed at once (no settle, no spacing, Paused or not); the box
   empties; a SENT row; focus returns to the compose box. *Check:* [LOG] `[nav] send-now <sid8>`,
   `[delivered]`; [MOCK].
4. **Queued row.** *Do:* empty box, select an UPCOMING row, **!**. *Expect:* the confirm quotes that row;
   Send delivers it; the row moves to SENT.
5. **Nothing to send.** *Do:* empty box, no Pending rows, **!**. *Expect:* no dialog, nothing happens.

**Edge cases**
- **E1 Dormant tab** (F-S deactivated). *Expect:* nothing is typed; the prompt goes back to Pending;
  [LOG] `[send-verify] <sid8> prompt <id8> NOT sent: the tab is dormant …`; after the tab is entered it
  goes out (Auto) or waits (Paused).
- **E2 A delivery in flight.** Send now while an automatic send holds the box. *Expect:* declined at once
  (one rollback), sent when the gate closes; never two prompts merged.
- **E3 While the global pause is on** → works (PQR-05 phase 4).
- **E4 A running turn.** *Expect:* Claude queues it as type-ahead; it runs as the next turn.
- **E5 Known defect K1** (below): a selected history row is re-sent.

---

### PQR-07 · Esc parks the runner until your next message (the interrupt hold)

> **Since** 0.7.7 · **P0** · **Needs** mock (`hold`), keys into the terminal · **Spec** DELIVERY.md §14

**Promise.** Interrupting a turn is the user taking the wheel: no queued prompt goes into that session
until the user's own next message, which then resumes the previous mode.

**Preconditions.** F-S Auto; queue `p1 [[mock hold g1; reply "p1"]]`, `p2 [[mock reply "p2"]]`.

**Phases**
1. **Running with a queue.** *Expect:* p1 delivered, Running, p2 Pending.
2. **Interrupt.** *Do:* click into the terminal, press Esc. *Expect:* Claude writes
   `[Request interrupted by user]`; the runner parks: pane toggle `Prompt Queue Runner: Paused → Auto on
   your next message` (hollow circle in Auto's color), badge `Paused → Auto`, board ⚙ tooltip names the
   parked mode. *Check:* [LOG] `[interrupt-hold] <sid8> autorunner Full -> Off (…)`; [ENG] mode Off,
   `interruptHeldMode` Full.
3. **Held at rest.** *Do:* wait two settle windows. *Expect:* p2 not sent. *Check:* [LOG] `[advance-skip]
   … autorunner off (interrupt hold - resumes on your next message)`.
4. **Resume.** *Do:* type `go [[mock reply "ok"]]` + Enter in the terminal. *Expect:* the mode returns to
   Auto at that message; after that turn ends and settles, p2 is sent. *Check:* [LOG] `[interrupt-resume]
   <sid8> autorunner Off -> Full (your message arrived …)`; [MOCK] p2 after the typed turn.

**Edge cases**
- **E1 Already Paused.** Esc on a Paused session → no hold; the next message leaves it Paused.
- **E2 Second Esc** while held → nothing changes.
- **E3 The user picks a mode during the hold** (either toggle, either value) → the pending resume is
  cancelled ([LOG] `[nav] autorunner … (cancels the interrupt hold)`); the next message changes nothing.
- **E4 Send now while held** → its echo is the user's next message → resumes.
- **E5 Noise is not a message.** A teammate/agent delivery (a prompt starting `<teammate-message`) →
  no resume.
- **E6 Type-ahead before Esc** (accepted gap) → that prompt cannot resume the hold; the next real message
  or the toggle does.
- **E7 A backstop pause wins.** While held, the next turn ends in an API error → stop-on-error turns the
  hold into a sticky Paused; the following message does NOT resume Auto.
- **E8 An old marker.** A resumed session whose transcript ends on an interrupt older than 60 s → no hold.
- **E9 Restart while held** → the hold is part of the session's persisted mode (since 2026-10-26, PQR-K8's fix,
  NOT BUILT): the reopened session still reads `Paused → Auto on your next message`, nothing goes by itself, and the
  user's first message after the reopen resumes Auto. *Check:* [UI] the toggle after the reopen; [MOCK] silence past a
  window, then the queued prompt after the message's turn; [LOG] `[interrupt-resume]`. Test (`needs_fix`):
  `test_an_interrupt_hold_survives_a_restart`. (0.8.0: the hold was transient and the reopen re-stamped the cog's
  default, so the queued prompt went by itself.)

---

### PQR-08 · The question guard holds the queue after a question

> **Since** 0.7.7 (releases) · **P0** · **Needs** mock · **Spec** DELIVERY.md §8 (question-guard trap)

**Promise.** When Claude's final text ends with a question, the next queued prompt is NOT sent as a
blind answer; it waits for the user — and the user can always see why and release it.

**Phases**
1. **A question turn.** *Do:* queue `q [[mock reply "Shall I continue?"]]` then `next [[mock reply "n"]]`.
   *Expect:* after `q`'s turn, `next` stays Pending. *Check:* [LOG] `[advance-skip] … question`; [UI] the
   badge's row 3 prefix `❓ held — answer the question` (orange) with a tooltip naming the releases.
2. **Release by answering.** *Do:* type `yes [[mock reply "fine."]]` in the terminal. *Expect:* that turn
   ends without a question → `next` goes after the settle.
3. **Release by re-arm** (repeat 1, then Paused → Auto). *Expect:* `next` goes after the settle.
4. **Release by the mail button** (repeat 1, type a draft in Claude's box, click the badge's mail
   button). *Expect:* the moved draft is queued with the "answers a question" pass and is sent past the
   guard.

**Edge cases**
- **E1 Trailing whitespace** after `?` → still a question.
- **E2 `?` mid-text**, final sentence not a question → not held.
- **E3 Claude's habit "Shall I …?"** → held (by design; the 10-21 study notes the cost).
- **E4 A question in an earlier block, a statement at the end** → not held.
- **E5 After release, a NEW question turn** re-latches the guard.

---

### PQR-09 · States and screens that hold the queue

> **Since** 0.7.7–0.7.9 · **P1** · **Needs** mock (`ask`, `hold`), keys · **Spec** DELIVERY.md §3, §12, §17.2; CLAUDE.md *Prompt Queue Runner*

**Promise.** The runner never types into a session that is working, asking, broken, showing a dialog or
whose box it cannot see.

**Phases** (one sub-case each; F-S Auto with `next [[mock reply "n"]]` queued behind the trigger)
1. **Running** (`hold g1`) → held until the turn ends.
2. **Needs approval** (`ask "Pick?" A,B`) → NeedsApproval (orange-red), held; answer in the terminal
   (wait for the dialog to settle, Enter) → the turn ends → `next` goes.
3. **A menu on screen** (`/model` typed in the terminal opens its picker) → held; Esc closes it → goes.
   Measured (2026-10-24): the picker reads MenuOpen — `[advance-skip] … (input box not visible (a menu/dialog
   is open))` — like an AskUserQuestion menu (PENDING_INPUT.md, `IsMenuOptionCaret`); the prompt went ~11 s
   after Esc (no status change, so no new settle window).
4. **The box not visible** (Ctrl+G opens Claude's external editor in the terminal) → held; after 60 s
   [LOG] `[send-verify] … HELD` (warning, not a failure); closing the editor → goes.
5. **Error** (`error 400 invalid_request_error "x"`) → not ready (and see PQR-11).
6. **Done** (`/exit` typed) → not ready; the queue waits. Measured (2026-10-24), two outcomes: the SessionEnd
   hook arrived — Done, and nothing asks the runner (no line at all, run `pqr1`); or, on a loaded PC, it was
   cancelled — the session kept reading waiting-for-you and the box gate held the queue ("input box not
   visible": the tab shows a PowerShell prompt). LIF-02 accepts both and judges the hold by the mock's silence.

**Edge cases**
- **E1 A delivery in flight** (the draft swap holds the box) → `[advance-skip] … delivery in flight`,
  re-fired by the gate's close (no polling).
- **E2 A compaction** (`/compact`) between turns → no send during the compaction window.

---

### PQR-10 · Never into a dormant or closed tab

> **Since** 0.7.9 · **P0** · **Needs** mock, Deactivate, restart · **Spec** DELIVERY.md §17.2

**Promise.** A queued prompt is never typed into a tab whose claude is not running; it waits and goes out
by itself once the tab starts.

**Phases**
1. **Park.** *Do:* F-S Auto with `p [[mock reply "p"]]` queued behind `hold g1`; release g1 and
   immediately **Deactivate ▸ Deactivate Tab**. *Expect:* the tab is dormant; p stays Pending past many
   settle windows. *Check:* [LOG] `[advance-skip] <sid8> tab dormant (its claude is not started - …)`;
   [MOCK] silence; no `[lost-send]`.
2. **Enter the tab.** *Do:* click the tab. *Expect:* claude resumes; the session settles; p is sent.
3. **Closed session.** *Do:* queue q, close the tab (Close). *Expect:* `[advance-skip] … session closed
   (no open tab)`. *Do:* Sessions page → Resume here. *Expect:* the queue comes back with the session and
   q goes out after start + settle (Auto). Measured (2026-10-24, LIF-04): with the runner Paused at the
   Close nothing asks the runner to decide, so the `session closed` line does not appear (the test checks
   `[archive] <id>` and the card leaving the board). Resume here keeps the session's own mode (PQR-K8's fix,
   NOT BUILT; LIF-04 E1): a queue closed Paused stays Paused. (0.8.0 re-armed it on the cog's default, Auto, so a
   queue closed Paused went after the resume too.)

**Edge cases**
- **E1 Window-restored background tab** with a queue (F-RS) → same as phase 1–2.
- **E2 A hold scheduled before the park** (the timed re-check fires after it) → still not sent.
- **E3 Adopted (hand-typed) session** → exempt from the dormant gate (its claude runs in the user's
  shell) → sends normally.

---

### PQR-11 · Failures pause the runner instead of guessing

> **Since** 0.7.7 · **P0** · **Needs** mock (`error`), cog switches · **Spec** DELIVERY.md §12, CLAUDE.md *Enter-retry*, *lost-send*

**Promise.** When a turn fails or a send cannot be proven, the runner stops that session's queue and says
why — it never re-sends blindly and never sends into a broken session.

**Phases**
1. **Stop on error.** *Do:* queue `bad [[mock error 400 invalid_request_error "prompt is too long: 1 > 0"]]`
   and `after [[mock reply "a"]]`. *Expect:* the session reads Error (crimson dot, board Error column, the
   card shows the message); the runner goes Paused; `after` stays Pending. *Check:* [LOG]
   `[stop-on-error] <sid8>`; [TR] `isApiErrorMessage: true`.
2. **Recover.** *Do:* re-arm Auto; type `retry [[mock reply "ok"]]` in the terminal. *Expect:* the
   session leaves Error (Running → Waiting); then `after` goes after the settle.
3. **stopOnError OFF.** *Do:* cog → Prompt Queue Runner → stop on error off; new session; repeat 1.
   *Expect:* Error, but the mode stays Auto; `after` waits for the session to leave Error, then goes.

**Edge cases**
- **E1 A retried error** (`error 529 overloaded_error "x" times=2; reply "ok"`) → no Error, the queue goes on.
- **E2 A cut stream** (`say "half"; drop`) → Error "Connection lost mid-response" → paused (and see
  06-interrupted-work INT-01 for the rewind on Restart).
- **E3 Eaten Enter** → `[enter-retry] <sid8> press k/3` until the turn starts; after 3 presses with no
  start → `[enter-retry-giveup]`, the prompt `Failed`, the runner Paused. Never a press into a foreign
  draft or an open menu. ⚠ Hard to provoke on purpose: the dormant gate (PQR-10) and the settle hold keep
  a send away from a claude that is still starting, which is how the Enter used to be eaten. Provoke it
  with a test-only hold of the TUI's input (not built), or keep it to the m5 coverage of `DecideEnterRetry`
  and assert only "never a double message" here.
- **E4 Lost send** (a Sent prompt with no echo, at rest 15 s, injected) → `Failed` + Paused,
  `[lost-send]` in both logs; never resent automatically.
- **E5 Reclaim** (Sent but never injected, gate expired) → back to Pending (`[send-reclaim]`), mode
  untouched, retried.

---

### PQR-12 · Local commands in the queue

> **Since** 0.8.0 · **P1** · **Needs** mock · **Spec** DELIVERY.md §18

**Promise.** A queued Claude Code local command (one that writes no prompt line) counts as delivered
when its echo appears, and never fails the queue.

**Phases**
1. **/model.** *Do:* queue `/model sonnet` then `after [[mock reply "a"]]`. *Expect:* /model is
   delivered; no UserPromptSubmit fires; the transcript's command echo marks it echoed; `after` follows
   after the settle; the runner stays Auto. *Check:* [LOG] `[pull-echo] <sid8> prompt <id8> (the
   transcript's /model echo: the delivered command ran)`; no `[lost-send]`; [MOCK] `after`'s request
   names the sonnet model. Measured (2026-10-26, run `pqr5`, Claude Code 2.1.291): the first switch of a
   conversation opens "Switch model? … ❯ 1. Yes, switch to …" (it reads MenuOpen: the queue holds, no verdict for
   20 s; Enter confirms, then the echo comes); a second switch right after it applied without asking; `after`
   ran on `claude-sonnet-5-5`.
2. **/add-dir with its dialog.** *Do:* queue `/add-dir <a folder beside w1>` (an existing folder OUTSIDE the
   session's folder — measured: one inside it is added at once, no dialog, its echo marking the delivery).
   *Expect:* Claude opens "Add directory to workspace"; the box reads MenuOpen; no lost verdict while it is open (even
   past 15 s). *Do:* wait for the dialog to settle, press Enter. *Expect:* the echo arrives, the prompt is
   echoed, the queue continues. Measured (2026-10-26, 0.8.0): in two runs of three (`pqr6`, `pqr7`) the command
   was given up and marked Failed before its dialog showed — **PQR-K12**; in the third (`pqr8`) the dialog held
   the queue, as it should. Not automated while the defect is intermittent.

**Edge cases**
- **E1 Same command twice** → each echo consumed by its own delivery (oldest first).
- **E2 Case and spacing** (`/MODEL  sonnet`) → still matched.
- **E3 A dialog left open** at the PC for minutes → no verdict until it closes; judged then.

---

### PQR-13 · Editing the queue

> **Since** 0.7.7 · **P1** · **Needs** mock · **Spec** CLAUDE.md *C1 UI* (compose row)

**Phases**
1. **History recall.** *Do:* in the compose box press Up repeatedly, then Down. *Expect:* earlier prompts
   of this session cycle in; Down at the bottom restores the in-progress text. Measured (2026-10-26, run `pqr5`,
   0.8.0): the first Up recalls the newest sent prompt, but a second Up recalls it again — **PQR-K10**.
2. **Pop the last queued prompt.** *Do:* empty box, Ctrl+Shift+Up. *Expect:* the LAST Pending row leaves
   the queue and its text is in the box, caret at the end. *Check:* [LOG] `[nav] queue-pop <sid8> "…"`.
3. **Row menu.** *Do:* right-click an UPCOMING row. *Expect:* Copy / Move up / Move down / Delete; on a
   SENT row only Copy. Copy puts the prompt on the clipboard (a paste placeholder expanded to its text).
4. **Templates.** *Do:* paper icon → save the queue as `t1`; select another session → apply `t1`.
   *Expect:* the prompts are appended Pending there. Apply to dir → every session of that folder gets
   them.

**Edge cases**
- **E1 Ctrl+Shift+Up with text in the box** → extends the selection, pops nothing.
- **E2 Pop racing a send** → a prompt that just went Sent is not popped (history never pops).
- **E3 Up/Down with Shift or Ctrl** → caret/selection moves, no history.
- **E4 Known defects K2 and K4** below.
- **E5 `/clear` in the tab** → the tab's new conversation is a new session: an empty queue, the cog's default
  mode; the queued prompts stay with the old conversation (archived, its own mode kept) and are never sent into the
  new one. A FEATURE (the owner's ruling of 2026-10-26: the queue does not follow the tab across `/clear`; filed as
  PQR-K7). Test: `test_clear_leaves_the_tabs_queue_with_the_old_conversation` (also LIF-07).

---

### PQR-14 · Prompts typed by hand are recorded once

> **Since** 0.7.7–0.7.9 · **P1** · **Needs** mock · **Spec** DELIVERY.md §5.7, §10, §15

**Promise.** Every message a session gets appears once in its history: typed ones as `typed`, queued ones
as `auto`.

**Phases**
1. **Typed.** *Do:* type `hello [[mock reply "h"]]` + Enter in the terminal. *Expect:* one SENT row,
   `typed` pill. *Check:* [LOG] `[ups] … typed`.
2. **Queued, CR-composed.** *Do:* compose two lines in the box (the box stores `\r`), queue. *Expect:* one
   `auto` row; the echo (with `\n`) consumed, no extra `typed` row.
3. **Non-ASCII.** *Do:* type `שלום [[mock reply "ok"]]` in the terminal. *Expect:* one row, Hebrew intact.

**Edge cases**
- **E1 Empty duplicate hook** (the late empty UserPromptSubmit twin) → no row, state only.
- **E2 A teammate message** → not recorded as typed (noise filter).

---

### PQR-15 · The queue across a restart

> **Since** 0.7.9–0.8.0 · **P0** · **Needs** mock, F-RS · **Spec** PERSISTENCE.md §14; HOOKS.md *Cut-off turns*; memory `prompt-queue-runner-study-2026-10-21`

**Promise.** A queue survives the app's restart; nothing already sent is sent again; a turn the restart
cut off is never followed blindly.

**Phases**
1. **At rest.** *Do:* Paused session with 2 Pending + 3 Sent rows; quit; reopen (Yes to reopen).
   *Expect:* the same rows and statuses; the tab dormant until entered; nothing sent while dormant.
2. **Mode on reopen.** *Expect:* the restored session KEEPS its own mode (the owner's rule of 2026-10-26: "we
   need the mode per session persisted"; PQR-K8's fix, NOT BUILT): a session left Paused comes back Paused and
   sends nothing by itself; switched to Auto, its queue goes one per turn; the SENT row is never typed again.
   *Check:* [UI] the toggle after the reopen; [MOCK] silence past a window, then each prompt once. Test
   (`needs_fix`): `test_a_paused_queue_stays_paused_across_a_restart_and_nothing_sent_is_sent_again`; Close +
   Resume here: `test_claude_lifecycle.py::test_resume_here_keeps_the_sessions_paused_mode`. (0.8.0, measured
   2026-10-24: the restored launch re-stamped the cog's default, so a queue left Paused came back Auto and both
   prompts went once the tab had started and settled — PQR-K8.)
3. **Mid-turn quit.** *Do:* Auto session with `p1 [[mock hold g1; …]]` running and `p2` queued; quit.
   *Expect after reopen:* the session reads **Error** (a cut-off turn), the runner is Paused
   (stop-on-error); p2 not sent. If the session was the focused tab or woken by the restore wake, its
   claude continues the turn (`CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1`) → Running → Waiting; the runner
   stays Paused until re-armed.

**Edge cases**
- **E1 A crash** (kill the GUI process only) instead of a quit → same as phase 3.
- **E2 A prompt Sent but unechoed at the quit** → loads `echoed=true` (never re-sent, never a false
  pickup hold).
- **E3 A `/handover` briefing Pending from an earlier run** → sent once its session is open and settled.

---

### PQR-16 · Several sessions and windows

> **Since** 0.7.9 · **P1** · **Needs** mock, F-W2 · **Spec** CLAUDE.md *Agent Manager*, *Engine*

**Phases**
1. **Independent runners.** Two sessions with queues, one Auto, one Paused → only the Auto one drains.
2. **Cross-window queueing.** *Do:* from window A's Manager (scope GLOBAL), queue into a session hosted
   in window B. *Expect:* B delivers it (B's box, B's draft swap), A's pane shows the rows move.
3. **Jump to tab** (eye) for a session in window B → B comes to the front on that tab.

**Edge cases**
- **E1 Window B closed** while its session has a queue → the session is archived (Close semantics); the
  queue is kept for a later resume.

---

## Known defects (expected failures until fixed)

Each is written as a flow whose expected result is the CORRECT behavior; it fails today. Re-checked in
the code on 2026-10-22.

### PQR-K1 · Send now re-sends a selected history row

> `e2e/suites/test_prompt_queue_runner.py::test_known_defect_send_now_sends_a_selected_history_row_again`
> (`known_defect`)

`AgentManagerContent.AutoTesting.cpp:804` `_DoSendNow`: with an empty box it sends the SELECTED row
whatever its status. *Do:* select a `typed` row in SENT, click **!**, Send. *Correct:* nothing is sent (or
the button is disabled for a history row). *Today:* the row is typed again; as a `typed` origin it gets no
echo consume and no Enter-retry rescue, and a duplicate `typed` row is recorded.

### PQR-K2 · Move up / down swap by raw queue index

> `e2e/suites/test_prompt_queue_runner.py::test_known_defect_move_up_swaps_a_queued_row_with_a_history_row`
> (`known_defect`)

`AgentManagerContent.AutoTesting.cpp:1321` `_OnMovePrompt`: swaps with the neighbor in the raw queue,
history rows included. *Do:* a queue whose raw order is [Sent a, Pending b, Sent c, Pending d]; Move up on
d. *Correct:* d moves above b in UPCOMING. *Today:* d swaps with c (a history row) — UPCOMING looks
unchanged.

### PQR-K3 · The board's ⚙ sent/total counts history

> `e2e/suites/test_prompt_queue_runner.py::test_known_defect_the_boards_send_count_counts_typed_history`
> (`known_defect`)

*Do:* a session with typed history and no queue. *Correct:* no ⚙ count (or 0/0). *Today:* a blue
`⚙ N/N` appears.

### PQR-K4 · Templates copy history and reach archived sessions

*Do:* save a template from a session with history; apply to its folder with an archived session in it.
*Correct:* only Pending prompts are saved; only open sessions receive it. *Today:* history rows are saved
too, and the archived session receives the prompts.

### PQR-K5 · Records the registry mints get only the cog's mode

*Do:* a hand-typed claude in a shell tab (adopted) with the cog's maxAutoSends = 2. *Correct:* the adopted
session's runner takes the cog's maxAutoSends / stopOnError too. *Today:* it takes the struct defaults
(100 / on).

### PQR-K8 · A reopened or resumed session loses its own runner mode — **fixed in code 2026-10-26** (NOT BUILT)

> **P0** · Found by the e2e suite on 2026-10-24 against 0.8.0 (PQR-15 phase 2, LIF-04; then filed as "by design");
> the owner's rule of 2026-10-26: "no, we need the mode per session persisted" · tests (`needs_fix`):
> `test_prompt_queue_runner.py::test_a_paused_queue_stays_paused_across_a_restart_and_nothing_sent_is_sent_again`,
> `…::test_an_interrupt_hold_survives_a_restart`, `test_claude_lifecycle.py::test_resume_here_keeps_the_sessions_paused_mode`

**What it was.** `sessions.json` kept each session's mode (`autorunner.mode`), but `_LaunchClaudeSession`'s restored
branch re-stamped the cog's "default mode for new sessions" on every OPENED session too ("all new OR opened sessions
run on the default"): a window's reopen and the Sessions page's Resume here brought a session left Paused back Auto,
and its queue went by itself once the tab had started and settled. An interrupt hold (Esc's "Paused → Auto on your
next message") was transient and voided the same way. With the cog's default on Auto, a restart also undid every
automatic pause — stop-on-error, the Enter-retry give-up, the lost-send verdict; such a pause now survives a
restart too, until the user re-arms the session.

**The fix.** The restored branch keeps the record's mode (`TerminalPage.AgentSessions.cpp`); the interrupt hold is
persisted with it (`interruptHeldMode` + `interruptHeldUnixMs` beside `mode: Off`, written only while a hold is
active; an incoherent pair is dropped at load — `Persistence.cpp`, `TestPersistence`); an on-disk session the
registry never knew gets the cog's defaults when it is first resumed (`_ResumeSessionFromDisk`), as a new session.
NEW sessions — a launch, a fork, a `/clear` or `/handover` successor, an adopted claude — still start on the cog's
default (DELIVERY.md §14).

### PQR-K9 · A queued prompt with right-to-left text is never delivered

> **P0** · Found by the e2e suite on 2026-10-26 against 0.8.0 + Claude Code 2.1.291 (PQR-01 E2, run `pqr5`) ·
> `e2e/suites/test_prompt_queue_runner.py::test_known_defect_a_queued_prompt_with_hebrew_is_never_delivered`
> (`known_defect`; a Hebrew-first line and a Hebrew word inside a left-to-right line)

Claude Code draws a line that holds right-to-left text (Hebrew, Arabic) in VISUAL order: the characters stand
in the terminal's cells the way they read on screen, not in the order they were typed. The verified delivery
fills the box, reads it back from the terminal and compares — and the read-back of
`שלום ⚠️ [[mock reply "ok"]]` was `" ]]"mock reply "ok[[ ⚠️ םולש"` (hooks.log `[send-verify] <sid8> FOREIGN
read-back after the fill (box chars=28 head=…) - the merge this would have submitted is refused; undoing our
insertion`). The delivery undid its own fill, tried once more (`strike 1/2 - prompt rolls back to Pending`, then
a `[draft-swap]` that stashed the box), and gave up: `[send-verify] <sid8> prompt <id8> FAILED after 2
unverifiable deliveries (foreign content in the box …) - not resent`. The prompt reads Failed (✕), it never
reaches the model, and the pending-input monitor recorded the reversed read-back as the session's unsent draft
(`pendingInput` in sessions.json — what a reopen types back into the box, not measured here).

*Correct:* the read-back is compared in the box's visual order (the Unicode bidirectional algorithm applied to
the expected text: ICU's `ubidi`, which the engine links for ClaudeInvisibleChars), and a draft read from the box
is put back in logical order. A typed Hebrew prompt is unaffected: its UserPromptSubmit hook carries the logical
text (PQR-14 phase 3). The suite's own driver met the same trap: `Terminal.submit` waits for the box to read like
the typed text, so a right-to-left prompt is typed through `_submit_right_to_left` (the same characters, in any
order).

### PQR-K10 · Up in the compose box recalls only the newest prompt

> **P2** · Found by the e2e suite on 2026-10-26 against 0.8.0 (PQR-13 phase 1, run `pqr5`) ·
> `e2e/suites/test_prompt_queue_runner.py::test_known_defect_a_second_up_does_not_walk_back_to_older_prompts`
> (`known_defect`)

*Do:* a session with two sent prompts; in the compose box type a draft, then Up, Up. *Correct:* the first Up
recalls the newest prompt, the second the older one; Down walks back, and Down past the newest (or Esc) restores
the draft. *Today:* the second Up recalls the newest prompt again. The likely cause, read in the code:
`AgentManagerContent::_ApplyPromptHistoryText` writes the recalled text under the `_promptHistoryNavigating` latch
on the assumption that the TextBox raises TextChanged synchronously (its comment says so), but TextChanged is
asynchronous (CLAUDE.md, Gotchas *Popup placement*), so the handler runs after the latch is down, takes the recall
for the user's edit and resets the walk (`_ResetPromptHistory`), and the next Up starts over at the newest.
Shift+Up and Ctrl+Up are not affected (the text box's own) — `test_up_in_the_compose_box_recalls_the_newest_sent_prompt`.

### PQR-K11 · A long one-line prompt is never delivered

> **P1** · Found by the e2e suite on 2026-10-26 against 0.8.0 + Claude Code 2.1.291 (PQR-01 E4, run `pqr6`) ·
> `e2e/suites/test_prompt_queue_runner.py::test_known_defect_a_long_single_line_prompt_is_never_delivered`
> (`known_defect`)

Claude Code shows a long paste of ONE line collapsed as a bare `[Pasted text #1]` — without the `+M lines` part
that a multi-line paste's marker carries. The verified delivery's paste-marker parser (`PendingPaste.h`
`ParseMarkerAt`) accepts only `[Pasted text #N +M lines]` and `[...Truncated text #N +M lines...]`, so
`VerifyFillAgainstPrompt` finds no marker, the box does not read like the prompt, and the fill is judged foreign:
`[send-verify] <sid8> FOREIGN read-back after the fill (box chars=16 head="[Pasted text #1]")`, a second fill,
then `FAILED after 2 unverifiable deliveries` — a queued 2,000-character prompt on one line never reaches the
model. *Correct:* the bare marker is the collapsed render of a one-line paste (`DraftCarry.h` already parses it:
`M == 0 ? "[Pasted text #N]" : "[Pasted text #N +M lines]"`), so the fill verifies as collapsed and the prompt goes
once, whole. The same 2,000 characters on 40 lines go (PQR-01 E4).

### PQR-K12 · A queued `/add-dir` is given up before its dialog shows

> **P1** · Found by the e2e suite on 2026-10-26 against 0.8.0 + Claude Code 2.1.291 (PQR-12 phase 2) · INTERMITTENT:
> runs `pqr6` and `pqr7` gave the command up, run `pqr8` delivered it and its dialog held the queue as it should —
> so no test yet (a known-defect test needs a way in that is deterministic)

*Do:* queue `/add-dir <a folder beside the session's folder>` and a prompt after it. *Correct:* the command is
delivered once, Claude Code's "Add directory to workspace" dialog holds the queue until it is answered, the command's
echo counts the delivery (`[pull-echo] … /add-dir echo`), the runner stays on and the queue goes on. *Today:* the
delivery's CR leaves the command in Claude Code's box (`[pending] <sid8> draft … /add-dir C:\…` right after
`[delivered]`), and so does each of the Enter-retry watchdog's presses (`[enter-retry] <sid8> prompt <id8> verified
press (box holds the watched prompt)`, three of them); the watchdog gives up — `[enter-retry-giveup] <id> (turn never
started after 3 Enter retries; marked Failed, autorunner paused)` — and only then does the dialog show. Answering it runs
the command (its echo is in the transcript), but the prompt is already Failed and is never matched to the echo; the
runner stays Paused. Claude Code most likely spends those Enters on its path completion (not pinned). A folder INSIDE
the session's folder is added at once, no dialog, and its echo counts the delivery (run `pqr5`).

## Reclassified: accepted behavior (not defects)

Filed by the e2e suite on 2026-10-24, ruled by the owner on 2026-10-26. Each test now proves the behavior.

### PQR-K6 · Restart session starts no settle window — ACCEPTED

> The owner (2026-10-26): "what we have now is okay" · PQR-03 E5 ·
> `e2e/suites/test_prompt_queue_runner.py::test_restart_session_sends_a_held_prompt_once_its_box_is_back`

A prompt the runner held because the session's claude had ended — no Claude input box on the screen, the tab at its
PowerShell prompt — is typed into the claude that **Restart session** starts as soon as that claude's input box draws:
measured 8 s after the restart, a second before the new claude's SessionStart hook ran; it went through (the
Enter-retry's press 3 s later found the box already empty: only the UserPromptSubmit hook was slow). The window counts
from `SessionInfo::stateChangedAtUnixMs`, which moves when the state, `live` or `started` changes; around such a restart
none of them does (a claude that ended without its SessionEnd hook keeps the session waiting-for-you — STA-K1 — and
`_RestartManagedSession` swaps the connection in place without touching `started`), so the window, long over, is not
started again. A parked tab's WAKE does start one (`started` changes: the send 40.7 s after Activate Tab), and so does
a restart after a `/exit` whose SessionEnd hook arrived (Done → the restored state is a change).

### PQR-K7 · `/clear` leaves the tab's queue with the old conversation — a FEATURE

> The owner (2026-10-26): "no don't follow tab across /clear" · LIF-07 phase 2, PQR-13 E5 ·
> `e2e/suites/test_prompt_queue_runner.py::test_clear_leaves_the_tabs_queue_with_the_old_conversation`

`/clear` in a tab starts a new conversation there, and the tab, its card and its Telegram topic follow it (hooks.log
`[rehome] <old> -> <new> (same tab, new conversation id)`, `[nav] tab-swap <old8> -> <new8> …`, `[lineage] <new8>
continues <old8> (/clear …)`). The queue stays with the old conversation's record, which is archived with its own mode;
the tab's new conversation is a new session — an empty queue on the cog's default mode — and the old queue's prompts
are never sent into it (they would go into the old conversation if that one were resumed and its mode allowed it). The
Prompt Queue tab says "Select a session …" until the new card is picked.
