# 15 · /handover, /handover-here, /handover-standby (HND)

> Conventions and fixtures: [README.md](README.md). Spec: COMMANDS.md §3 (the await), §3a (restart resilience), §5
> (/handover), §5a (-here), §5b (-standby), §6a–§6c (names, successor shaping, the write location), §7 (safeguards);
> CLAUDE.md *Slash-command bindings + /handover*. Needs the mock's `write <path> "<text>"` shorthand: the turn after
> the command writes the briefing the way Claude would, so the test controls the file's name, place and size.

**What shipped.** The family predates 0.7.7; the last releases changed it. 0.7.7: a two-file `/handover` no longer
loses its second successor when the seal fires mid-generation, and turning delete-after off disarms an already armed
deletion. 0.7.9: the successor's model from a per-command combo or a `[model]` hint in the message, a `[title]` hint,
presets by a bracketed prefix (PRE-05), the origin recorded on the successor (`continuedFromId`), so its summary shows
the origin's conversation (TRN-03). 0.8.0: the briefing budget accounts for the console guard (10,000 escaped
characters).

## Lifecycle model

```text
 "/handover <ctx>" typed ─► echo in the transcript ─► [cmd] sighted, await armed (≤ 15 min; 2 turn ends without a match = expired)
     ─► Write HANDOVER-<topic>.md (one or several, collected in write order) ─► the turn ends ─► SEALED ─► every file exists?
     ─► [cmd-fire] ─► the hosting window: one successor PER file
            /handover          a new tab right of the origin  ─► the briefing = its FIRST message (commandline ≤ budget, else the paste pump)
            /handover-here     the FIRST successor REPLACES the origin tab in place (origin archived, resumable); the rest open beside it
            /handover-standby  new tabs; the briefing TYPED into the box, never sent (verified by reading the box back)
     successor: titled "<origin> (handover)" (or the hint / rewrite), continuedFromId = origin, model = hint > combo > origin's preset > Default
     delete-after (opt-in, never for the scratchpad): the briefing file deleted once the successor STARTED (≤ the deadline)
```

---

### HND-01 · /handover opens a successor with the briefing as its first message

> **Since** ≤ 0.7.7 (fixes 0.7.7, origin link 0.7.9) · **P0** · **Needs** mock, F-S · **Spec** COMMANDS.md §5

**Promise.** One typed `/handover` produces one successor tab next to the origin, in the same working folder, whose
first user message is the briefing file's content, verbatim and whole.

**Preconditions.** F-S (`w1`), renamed `parser` (tab rename); the command definitions materialized
(`<CLAUDE_CONFIG_DIR>\commands\handover.md` exists — Agentmaster writes it at engine start).

**Phases**
1. **Type the command.** *Do:* in the tab: `/handover finish the lexer [[mock write C:\e2e\w\t1\HANDOVER-lexer.md "Continue the
   lexer: tokens A, B."; reply "written"]]`. *Expect:* the turn runs; Claude's Write lands. *Check:* [LOG] `[cmd] /handover
   sighted` (once), then `[cmd-fire] … md=C:\e2e\w\t1\HANDOVER-lexer.md` after the turn's end; [TR] the echo line carries
   `<command-name>/handover</command-name>`.
2. **The successor opens.** *Expect:* a new tab immediately right of the origin, titled `parser (handover)`, in the
   origin's effective working folder; the origin stays as it was. *Check:* [LOG] `[nav] handover-begin <sid8> md=…` ↔
   `[nav] handover-done new=<sid8'> from=<sid8> inject=content`; [ENG] the successor's `continuedFromId` = origin,
   `continuedVia` = `handover`.
3. **The first message.** *Expect:* the successor's first turn is the briefing. *Check:* [MOCK] its first `main` request's
   last user message equals the file's content exactly (BOM stripped, CRLF → LF, outer whitespace trimmed); [TR] the
   successor's first user line is that text; [ENG] one Typed row (not two).
4. **The summary chain.** *Do:* open the successor's summary panel. *Expect:* the origin's messages first, a separator
   naming the origin's id, then the successor's own (TRN-03).

**Edge cases**
- **E1 Run it again from the origin** → a second successor `parser (handover 2)`; from the successor → `parser (handover
  2)` too (chained, never `(handover) (handover)`).
- **E2 The turn writes no `HANDOVER-*` file** → nothing opens; after two turn ends the await expires ([LOG] `[cmd-expire]`).
- **E3 The file deleted before the turn ends** → the fire waits for the file; after the deadline nothing opens; never a
  successor pointed at a missing file ([LOG] `[handover] … md vanished`).
- **E4 The file written outside the session's folder** (the scratchpad, the default location — HND-06) → collected all
  the same (the match is on the NAME).
- **E5 A briefing in Hebrew / with emoji / with `$`, backticks and quotes** → arrives byte-equal ([MOCK]).
- **E6 The origin in a background window** → the successor opens in the ORIGIN's window.
- **E7 A Codex tab** → `/handover` is a Claude command: nothing happens in a Codex tab.

---

### HND-02 · Several briefings, several successors

> **Since** ≤ 0.7.7 (the seal fix 0.7.7) · **P1** · **Needs** mock, F-S · **Spec** COMMANDS.md §3, §5 (FAN-OUT)

**Phases**
1. **Write three.** *Do:* `/handover split it [[mock write C:\e2e\w\t1\HANDOVER-a.md "A"; delay 3s; write C:\e2e\w\t1\HANDOVER-b.md "B";
   write C:\e2e\w\t1\HANDOVER-c.md "C"; reply "three"]]`. *Expect:* ONE fire carrying all three paths, in write order.
   *Check:* [LOG] `[cmd-fire] … md=…a.md|…b.md|…c.md`.
2. **Three successors.** *Expect:* three tabs right of the origin, left to right a, b, c; titles `(handover)`, `(handover
   2)`, `(handover 3)`; each with its own file as its first message ([MOCK]); `handover-done new=<a>,<b>,<c>
   inject=content,content,content`.

**Edge cases**
- **E1 A pause of 20 s+ between writes while the turn is still running** (the mock's `delay 25s` between two writes) → still
  ONE fire with both (the settle is held while the turn is in flight — the 0.7.7 fix).
- **E2 The same file written twice** → one successor (deduped).
- **E3 A `notes.md` written in the same turn, after the first briefing** → not collected (the file-match rule, HND-06).
  Written BEFORE any briefing, in a Write batch of its own → the first-markdown fallback (on while the file-match
  pattern is the default) collects it too, and it gets a successor of its own: today's rule (COMMANDS.md §3: "while the
  pending has collected NOTHING, a batch's first markdown is the fallback pick"); the test pins it and the owner
  decides whether it should stay.
- **E4 One of three files deleted before the fire** → the two survivors open; the log names the dropped one.

---

### HND-03 · A long briefing goes through the paste pump

> **Since** 0.8.0 (the 10,000 budget) · **P1** · **Needs** mock (a generated 30,000-character briefing) · **Spec** COMMANDS.md §5 step 4–5

**Promise.** A briefing too long for the command line is never cut: it is queued at the front of the successor's queue and
pasted once the successor's claude has started.

**Phases**
1. **Hand over a long briefing.** *Expect:* the successor opens; [LOG] `inject=paste`; its Prompt Queue shows the briefing as
   the first UPCOMING row (Pending).
2. **The successor starts.** *Do:* select the successor tab (a background tab starts on its first visit). *Expect:* after
   ~1.5 s the briefing is pasted and submitted as ONE message; the row moves to SENT. *Check:* [MOCK] the first user message
   equals all 30,000 characters; [ENG] the row Sent + echoed.
3. **Boundary.** *Do:* repeat with briefings of 9,990 and 10,010 escaped characters (count `` ` ``, `"`, `$` twice).
   *Expect:* `inject=content` and `inject=paste` respectively; both arrive whole.

**Edge cases**
- **E1 The successor never visited for 10 min** → the pump gives up; the row stays Pending (visible, Send now-able, kept
  across a restart).
- **E2 A preset successor** (the key carrier adds to the command line) → a budget-sized briefing still launches (never a
  failed CreateProcess); [OS] the command line ≤ 32,767.
- **E3 The runner Paused on the successor** → the pump still delivers the briefing (it is not an automatic queue send).

---

### HND-04 · /handover-here replaces the origin tab in place

> **Since** ≤ 0.7.7 · **P1** · **Needs** mock, F-S · **Spec** COMMANDS.md §5a

**Phases**
1. **Hand over here.** *Do:* `/handover-here go on [[mock write C:\e2e\w\t1\HANDOVER-here.md "Go on with X."; reply "ok"]]`.
   *Expect:* the SAME tab (same strip position) now runs a fresh conversation titled `<origin> (handover)`; the origin's
   scrollback stays readable above; the briefing is the new conversation's first message. *Check:* [LOG]
   `[handover-here] <new> replaced <old> in place`, `[rehome]`, `tab-swap`, `[nav] handover-here-done new=… from=…
   inject=content`; [ENG] the origin `live=false` (resumable), the successor bound to the tab, started.
2. **The origin is kept.** *Do:* Sessions page. *Expect:* the origin listed (closed), Resume here works.
3. **Two files.** *Expect:* the first file's successor takes the tab; the second opens as a new tab beside it.

**Edge cases**
- **E1 The origin tab dormant** (deactivated between the command and the fire) → falls back to a new tab ([LOG]
  `(fallback=new-tab)`); the briefing still delivered.
- **E2 A split pane with a shell focused** → the swap targets the session's own pane, never the shell.
- **E3 Quit right after the swap** → the reopened window may show the archived origin's ref; the successor is resumable
  from Sessions (known characteristic, COMMANDS.md §5a).

---

### HND-05 · /handover-standby types the briefing and never sends it

> **Since** ≤ 0.7.7 · **P1** · **Needs** mock, F-S · **Spec** COMMANDS.md §5b

**Phases**
1. **Hand over in standby.** *Do:* `/handover-standby later [[mock write C:\e2e\w\t1\HANDOVER-sb.md "Review the PR."; reply
   "ok"]]`. *Expect:* a successor tab beside the origin; NO turn runs in it. *Check:* [LOG] `[nav] handover-standby-done …
   inject=standby`; [MOCK] no `main` request from the successor.
2. **Visit it.** *Do:* select the successor. *Expect:* after ~1.5 s the briefing appears in the input box, not submitted; the
   tab's unsent-draft dots appear. *Check:* [LOG] `briefing FILLED into the input box (chars=N, attempt 1/2) - NOT
   submitted`, then `draft VERIFIED … one Enter away`; [UI] the box text.
3. **Press Enter.** *Expect:* the briefing goes as the first message ([MOCK]).

**Edge cases**
- **E1 The user typed into the successor's box before the fill** → no fill at all (never appended to a human draft).
- **E2 The user sent a prompt of their own first** → hands off; the file kept ([LOG] took-over line).
- **E3 The paste eaten (the TUI not ready)** → re-filled once after 12 s; then given up with the file kept.
- **E4 A long (30,000-character) briefing** → filled whole (no command-line tier for standby).
- **E5 The runner in Auto** → the briefing is NOT a queue row, so the runner never submits it.

---

### HND-06 · Shaping the successor: model, title, which files, where, delete-after

> **Since** 0.7.9 · **P1** · **Needs** mock, F-PRE (for the preset hint), the cog's Commands tab · **Spec** COMMANDS.md §6b, §6c

**Phases**
1. **Model hint.** *Do:* `/handover [sonnet] x [[mock write …HANDOVER-m.md "m"; reply "ok"]]`. *Expect:* the successor runs on
   sonnet ([MOCK] its `model`); [LOG] `[handover] <sid8> successor model from the message hint: sonnet`.
2. **Title hint.** *Do:* `/handover [sonnet] [Lexer v2] x …` and `/handover [Lexer v2] x …` (no model matches "Lexer v2").
   *Expect:* both successors titled `Lexer v2`; the first on sonnet, the second on the combo's model.
3. **Per-command model combo.** *Do:* Commands tab → /handover's Successor model = Opus; Save; hand over without a hint.
   *Expect:* the successor on opus; a hint still wins.
4. **Title rewrite.** *Do:* set Find `^(.*) \(feature\)$`, Replace `$1 (next)`; hand over from a tab titled `x (feature)`.
   *Expect:* the successor titled `x (next)`; an origin the pattern does not match → the classic `(handover)` name; an
   invalid pattern → the status line says so and the classic name is used.
5. **Which files.** *Expect (default `HANDOVER\-`, matched case-insensitively on the file NAME):* `HANDOVER-x.md`
   collected; `handover.md` and `old-handover.md` written after it not; a turn that writes ONLY `notes.md` → collected by
   the first-markdown fallback (on while the pattern is the default or empty). *Do:* set the pattern to `^BRIEF-`.
   *Expect:* (after a restart: the bindings register at start) `BRIEF-x.md` collected, and a turn writing only `notes.md`
   opens nothing (a valid custom pattern turns the fallback off); an invalid pattern → the shipped hint ("handover" in
   the name) with a log line.
6. **Where.** *Do:* the location Presets ▾ → `./`; Save. *Expect:* the three live definition files re-rendered at once, no
   restart ([OS] the `WRITE IT IN: ` line of `<config>\commands\handover.md` now names the working directory, the
   phrase `CommandWritePathPhrase("./")` makes); Scratchpad again → `WRITE IT IN: your session scratchpad directory (…)`;
   `D:\briefings` → that folder. A value holding a backtick, a quote, `|` or a newline is cleaned on Save.
7. **Delete-after.** *Do:* location `./`, Delete-after ON, deadline 5 min; hand over. *Expect:* the briefing file deleted
   once the successor STARTED (visit it); with the location Scratchpad the toggle is disabled and un-checked and nothing
   is deleted.

**Edge cases**
- **E1 Delete-after with a successor never visited** → the file kept after the deadline.
- **E2 Delete-after turned OFF after the arm** → the armed deletion is dropped (the 0.7.7 fix).
- **E3 The pointer tier** (an unreadable briefing) → never deleted (its first message names the file).
- **E4 A blank deadline box** → 1440 minutes; `0` → delete only if already started.

---

### HND-07 · Commands renamed, disabled, edited by hand

> **Since** ≤ 0.7.7 · **P2** · **Needs** two app starts · **Spec** COMMANDS.md §6, §6a

**Phases**
1. **Rename.** *Do:* Commands tab → /handover's name `pass`; Save. *Expect:* the status line `/handover → /pass after
   restart`; nothing changes until a restart; after it `<config>\commands\pass.md` exists, `handover.md` is gone (it was
   pristine), and `/pass x [[mock write …HANDOVER-p.md "p"; reply "ok"]]` opens a successor.
2. **Disable.** *Do:* disable /handover-standby; restart. *Expect:* its file removed (pristine), the command unknown to
   Claude Code; typing it opens nothing.
3. **Edited by hand.** *Do:* change one word in `handover.md`; open the Commands tab. *Expect:* COMMAND DEFINITION FILES
   reads `EDITED BY YOU — left alone`; a restart does not touch it; **Reinstall definition files…** (confirmed) writes
   the shipped text back.
4. **Collision.** *Do:* name /handover-here `handover`. *Expect:* healed (the two names never collide).

---

### HND-08 · A handover in flight survives a restart, and never fires twice

> **Since** ≤ 0.7.7 · **P1** · **Needs** mock (`hold`), two app starts · **Spec** COMMANDS.md §3a

**Phases**
1. **Arm, then quit.** *Do:* `/handover x [[mock write …HANDOVER-r.md "r"; hold g1; reply "ok"]]`; while the turn holds, quit
   Agentmaster (the turn is cut off). *Expect:* [OS] the session store's `cmdProgress` holds an armed marker.
2. **Restart.** *Do:* start Agentmaster; release `g1` when the session resumes (or let the restore wake continue the turn).
   *Expect:* the turn ends; ONE successor opens. *Check:* [LOG] the marker revived, one `[cmd-fire]`.
3. **Resume the origin later.** *Do:* close and resume the origin from Sessions. *Expect:* no second successor (the fired
   watermark).

**Edge cases**
- **E1 Restart after the 15-minute deadline** → the marker pruned; nothing opens.
- **E2 /handover, then /handover-here before the file is written** → the newest command's path fires (one family: the
  first await is superseded); never both.
