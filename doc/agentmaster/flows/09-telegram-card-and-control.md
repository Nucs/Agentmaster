# 09 · Telegram: the card and driving a session (TGC)

> Conventions and fixtures: [README.md](README.md). Spec: MESSAGING.md §7.1–§7.3, §21.5–§21.6, §21.24,
> §21.26–§21.28, §21.33; DELIVERY.md §18; the command list in `Messaging.cpp` (`setMyCommands`). Needs **F-TG**.

**What shipped (0.8.0).** A status card that stays the topic's last message (state and since when, title,
folder · branch · model · effort · context, a queued prompt, the unsent draft at the PC, Claude Code's recap),
ringing when the session exits, errors or needs the user; its buttons on two pages; a tapped button stays busy
until its work is done. A message in a topic is the session's next prompt (whatever the runner's mode; a parked
tab is started for it), with its status as a reaction; `/steer` sends NOW, into a running turn too; the tab
menu's actions are `/commands`; Claude Code's own commands are queued like prompts.

## Lifecycle model

```text
 a message in the topic ─► ✍ queued (first, ahead of the runner's plan) ─► sent at the next ready moment
        (any runner mode, past Pause all, past the question guard; dormant tab started; settle 5 s) ─► 👨‍💻 taken
        └─► 👎 failed (+ the reason as a reply)
 /steer <words> ─► ⚡ sending ─► 👨‍💻 taken          (or ✍ waits first: dormant / ended / a menu / the box held)
 the card: posted ─► edited on a real change ─► deleted + re-sent as the last message once newer posts settle
           ─► re-sent WITH SOUND on exit / Error / Needs approval (one sound per needs-you)
```

---

### TGC-01 · The status card

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock · **Spec** MESSAGING.md §6.1, §21.5, §21.26, §21.27

**Promise.** Every topic ends with a card that says what the session is doing and since when, and rings when
the session needs the user.

**Phases**
1. **Content.** *Expect lines in order:* the state line (`🟡 Waiting for you since 08:21`, `📌` at its end when
   pinned); the title (a chip in the tab's color family, `📌 🟦 title` when pinned); the place line
   (`/source/proj · branch · opus-5-5 · max · 42K` — the folder cut to its last two parts without `…`, the model
   without `claude-`, no `ctx`); the next queued prompt when one waits; the unsent draft at the PC when there is
   one; Claude Code's recap (`away_summary`) only while it is newer than every message.
2. **Sticky.** *Do:* run a turn. *Expect:* after the turn's posts settle, the card is deleted and re-sent as the
   LAST message (no pin).
3. **Edits only on change.** *Expect:* no edit while nothing on the card changed (clock texts are `<tg-time>`;
   no per-minute re-edits). *Check:* [TG] edit count stays flat at rest.
4. **Rings.** *Do:* an ask (Needs approval) / an API error / `/exit` at the PC. *Expect:* the card re-sent at the
   bottom WITH sound, once per needs-you episode.

**Edge cases**
- **E1 A draft typed at the PC** → appears on the card; cleared when the box empties.
- **E2 A pinned session** → 📌 on both lines.
- **E3 A card the user deleted** → posted again within 60 s.

---

### TGC-02 · The card's buttons and pages

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock · **Spec** MESSAGING.md §21.24, §21.28

**Phases**
1. **Main page.** *Expect:* [⚙️ Session]; beside it [🔄 Restart Session] ONLY when the session is in Error or its
   claude exited, or [▶ Reactivate] for a parked tab.
2. **Session page.** *Do:* tap ⚙️ Session. *Expect:* the keyboard flips to: [⑂ Fork] [🆕 New Session]; [🔄 Restart
   Session] (+ [🔧 Resume Session] in Error); the runner (Pause / Resume), Queue, Pin / Unpin, Transcript,
   [🖥 Screen Text], [🖼 Screen Image]; [◀ Back]. Two buttons to a row.
3. **Busy until done.** *Do:* tap 🖼 Screen Image. *Expect:* the button shows Telegram's loading indicator until
   the photo arrives (the tap's answer held); a job over 9 s → the toast "⏳ Still working on it …". *Check:* [LOG]
   `[msg] tap <button>: answer queued N ms after the tap`.
4. **Back to Main** when the card starts to ring.

**Edge cases**
- **E1 Restart on a parked tab** → starts it in place (like Reactivate).
- **E2 A button of an earlier card / a previous run** → "no longer open" style answer; nothing acts.
- **E3 Interrupted-turn question** (NOT BUILT, INT-07) → Restart / Fork ask in the topic.

---

### TGC-03 · A message in a topic is the session's next prompt

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock · **Spec** MESSAGING.md §7.1 (D8, D30)

**Promise.** What the user types in a session's topic is queued as that session's next prompt and sent when
the session is ready — whatever its runner mode — with its status shown as a reaction.

**Phases**
1. **At rest.** *Do:* write `from the phone [[mock reply "ok"]]` in F-S's topic. *Expect:* ✍ on the message, then
   (after a 5 s settle) the prompt is typed into the tab; 👨‍💻 when its echo arrives; the reply mirrored; the
   message is NOT repeated as a `👤 You` line. *Check:* [ENG] the queue row has `remoteSource telegram:<id>`;
   [MOCK] the prompt text.
2. **Runner Paused + Pause all.** *Do:* set the runner Paused and the global pause on; write a message. *Expect:*
   it is still sent (the user's own message).
3. **Running.** *Do:* write while a turn runs (`hold g1`). *Expect:* ✍ until the turn ends; then sent (never a
   steer into the running turn).
4. **Parked tab.** *Do:* Deactivate the tab; write. *Expect:* the tab is started in place (no focus change); the
   prompt goes once it is up and settled.
5. **Queue order.** *Do:* the runner has two planned prompts; write two messages. *Expect:* the two messages go
   FIRST, in the order sent, then the planned ones.

**Edge cases**
- **E1 Edit the message while queued** → the queued text is replaced; after it was sent → "already sent — send
  the change as a new message".
- **E2 `/unqueue <n>`** → removes it; `/queue` lists the queue.
- **E3 A question card open** → the message is HELD and asked about (TGQ-04).
- **E4 A delivery that fails** → 👎 with a reply giving the reason; the runner is not paused for a remote prompt's
  lost send — verify.
- **E5 A Claude Code slash command** (`/model sonnet`) → queued like a prompt; no lost-send (DELIVERY.md §18): 👨‍💻
  on its echo; the runner stays on.
- **E6 `/cc /steer x`** → forced through to Claude Code as text (Claude Code has no /steer).
- **E7 A message in a Done session's topic** → queued; nothing is typed into the shell; `/reactivate` starts it.

---

### TGC-04 · `/steer` sends now — into a running turn too

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock · **Spec** MESSAGING.md §21.33

**Phases**
1. **Mid-turn.** *Do:* a long turn (`[[mock bash "echo 1"; delay 20s; reply "end"]]`); `/steer also print the time`.
   *Expect:* ⚡, then 👨‍💻 within seconds; the transcript has a `queued_command` attachment (absorbed mid-turn); no
   `👤 You` line repeats it.
2. **At rest.** *Expect:* a turn starts at once.
3. **Parked tab.** *Expect:* a reply that the tab's claude is not running; the tab starts; the words go FIRST once it
   is up (✍ meanwhile).
4. **A question card open / a menu on screen.** *Expect:* the words wait first (QueueMenu) and go after the answer.
5. **A draft at the PC.** *Expect:* the draft swap keeps it (back in the box after the steer).

**Edge cases**
- **E1 `/steer` with no words** → the usage line.
- **E2 Closed session** → "This session is closed. Resume it from the 🔎 Search topic." Nothing queued.
- **E3 Claude ended in the tab (Done)** → never typed (the shell would run it); waits first; `/reactivate`.
- **E4 The window declines later** (a menu opened in between) → "↩ Not sent now: <reason>. Your message waits first in
  the queue …" once.
- **E5 In the 🔎 Search topic** → refused as a session's command.
- **E6 From the backlog** (older than 10 min) → asked [Queue] / [Drop].

---

### TGC-05 · The tab menu as `/commands`

> **Since** 0.8.0 · **P1** · **Needs** F-TG, mock · **Spec** `Messaging.Inbound.cpp` `_runSessionCommand`; MESSAGING.md §7.3

**Phases** (each in F-S's topic; check the PC effect and the reply)
1. **Info:** `/status` (the card), `/queue`, `/sessionid`, `/path`, `/gitbranch`, `/draft`, `/summary`.
2. **Runner:** `/runner paused` → the PC's toggle Paused; `/runner auto`; `/sendnow` → the next queued prompt now;
   `/pauseall` / `/resumeall` (the global pause, every window).
3. **Lifecycle:** `/deactivate` (asks first while running) → parked; `/activate` → started in place;
   `/reactivate` → resumes a parked tab, or restarts a session in Error / an exited claude; `/restartsession`
   (asks while running); `/interrupt` (asks first) → one Esc → the interrupt hold (PQR-07).
4. **New tabs:** `/forksession [model]` → a fork beside the tab; `/newsession [model]` → a new session in this
   folder; the bot answers which session opened (or why nothing did).
5. **Marks:** `/pin` / `/unpin`, `/wake` (Idle → Waiting for you), `/moveidle`, `/markunread`, `/favorite` /
   `/unfavorite`, `/tags`, `/tag <name>`, `/untag <name>`, `/rename <title>` (TGT-07), `/verbose [on|off]`.
6. **Files:** `/transcript [minimal|compact|default|full|max]` → a `.md` document; `/download <path>` (TGF);
   `/takedraft` → a COPY of the PC's unsent draft queued (the box keeps it).

**Edge cases**
- **E1 Unknown command** → "Unknown command here: /x. /help lists them."
- **E2 `/help`** → the topic's list, its first line saying messages are queued and never typed into a running turn
  (only `/steer` does that).
- **E3 A session command in 🔎 Search** → refused with the Search list.
- **E4 `/newsession <model>` with shell metacharacters** → KNOWN DEFECT (TGC-K1).

---

### TGC-06 · The screen, as text or as a picture

> **Since** 0.8.0 · **P1** · **Needs** F-TG · **Spec** MESSAGING.md §21.24

**Phases**
1. `/screen` (or 🖥 Screen Text) → the terminal's visible text as monospace.
2. `/screenimage` (or 🖼) → a PNG of the styled screen (colors, box drawing joined, emoji) via `sendPhoto`;
   a document when the photo is refused.
3. **Fullscreen renderer** → the alt screen is drawn.

**Edge cases**
- **E1 A dormant tab** → says nothing is on screen / starts nothing (verify).
- **E2 Privacy:** the picture is NOT redacted (documented).

---

## Known defects (expected failures until fixed; re-check on 0.8.0)

### TGC-K1 · `/newsession` and `/forksession` take any text as the model

MESSAGING.md §21.21; memory `telegram-remote-launch-bugs` (session 41db7b17). *Do:* `/newsession K:/x;calc`.
*Correct:* refused as an invalid model id (the wizard's `ParseNewThreadModelId` rule). *Today:* the text reaches the
PowerShell line that starts claude, and the session's launch snapshot keeps it for every later start.

### TGC-K2 · Held messages of a new thread replay as topic commands

§21.21. *Do:* in a new thread, before picking its folder, send `/newsession K:\other`; then pick a folder. *Correct:*
the held command is not run as the new topic's command. *Today (0.8.0: verify):* it runs as the new topic's
`/newsession`, opening a second session.
