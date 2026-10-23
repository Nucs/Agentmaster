# 08 · Telegram: setup, topics and the mirror (TGT)

> Conventions and fixtures: [README.md](README.md). Spec: MESSAGING.md §4–§6, §21.4–§21.31 (as built),
> §21.21 (known bugs); release notes 0.8.0. Every flow needs **F-TG** — a fake Telegram Bot API the test
> drives as "the user's phone" (README §Infrastructure: NOT BUILT; it needs an E2E override of the API host,
> today hard-coded `api.telegram.org`).

**What shipped (0.8.0, opt-in).** A bot the user creates runs INSIDE Agentmaster. Every Claude tab that is
not Idle, in every window, is a topic of the bot's chat, named with the tab's color square and title, its
state shown as the topic's icon (⚡️ running · 💬 waiting · ❓ needs approval · ‼️ error · 🏁 done · ☕️ idle).
A topic carries the whole session: the history replayed first (paced), then the live mirror. A new tab or
fork gets its topic at once; a topic follows its tab through `/clear` and `/resume`; an idle session's topic
leaves after a grace (10 min) with one line in the main view; `/pin` keeps a session from going idle; the
topic list follows MOST RECENT; a pinned directory message sits in the main view; every app start posts a
start note in 🔎 Search; only the owner's messages are read.

## Lifecycle model

```text
 bot off ──cog: Run the Telegram bot + token + owner id──► connect (getMe, identity gate) ─► 🔎 Search topic ─► menu (setMyCommands)
                                                             │
 a tab's session leaves Idle ──► topic created ─► card ─► HISTORY REPLAY (oldest first, paced) ─► card re-posted ─► LIVE MIRROR
        │                                                                                            │
        │  /clear · /resume · /handover-here in the tab: the topic FOLLOWS the tab (re-keyed)          │
        │  rename / recolor: topic renamed + one change line                                          │
        │  Idle ──grace (10 min)──► topic DELETED + one line in the main view  (/pin: never idle)      │
        │  tab closed ──► topic deleted (quiet)                                                       │
        └─ app quits ── topics stay ── next start: the sync keeps those that still qualify ── start note in 🔎 Search
```

---

### TGT-01 · Set the bot up; only the owner is answered

> **Since** 0.8.0 · **P0** · **Needs** F-TG · **Spec** MESSAGING.md §10.1, §21.2, §21.12 (one bot per install)

**Phases**
1. **Configure.** *Do:* cog → **Messaging**: turn on **Run the Telegram bot**, paste the token, type the owner's
   numeric user id; Save. *Expect:* the token is stored in Windows Credential Manager (never in a file or a
   log); the status line reads Connected. *Check:* [OS] `settings.json` holds no token; the credential exists;
   [LOG] no token text in any log.
2. **First connect.** *Expect:* the bot creates the **🔎 Search** topic with its welcome text (ask in plain
   words · pick · Resume or Fork · fleet commands · "Pin this topic at the top of the list"), sets the chat's
   `/` menu (one `setMyCommands`), and posts the START NOTE in Search. *Check:* [TG] `createForumTopic` once,
   `setMyCommands` with the command list, `[msg] topic created: Search (thread N)`.
3. **A stranger writes.** *Do:* a message from another user id. *Expect:* dropped after reading only the
   sender; no reply, nothing queued.

**Edge cases**
- **E1 Threaded Mode off** in @BotFather → the cog's status says so; no topics are made.
- **E2 Token of ANOTHER bot** pasted later → the identity gate resets the chat state (topics, Search id,
  wizards) before the first `getUpdates`; the new bot's chat gets its own Search.
- **E3 Wrong token** → a clear error in the status line; nothing stored as working.
- **E4 Turn the bot off** → polling stops; topics stay in Telegram; the ask hook leaves the hooks settings
  (TGQ).
- **E5 Release and dev side by side** → each install polls its own bot (one bot per install).

---

### TGT-02 · A topic per working tab, named and iconed like the tab

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock · **Spec** MESSAGING.md §5.1, §21.13, §21.22

**Promise.** Every Claude tab that is not Idle has exactly one topic: its name is the tab's color square and
title, its icon the session's state; a new tab or fork is listed at once.

**Phases**
1. **Launch.** *Do:* F-S (first turn done → Waiting). *Expect:* a topic `🟦 <title>` (the square nearest the
   tab's color) with the 💬 icon. *Check:* [TG] `createForumTopic`; [LOG] `[msg] topic created …`.
2. **State icon follows.** *Do:* run `[[mock hold g1]]`. *Expect:* the icon becomes ⚡️ within the rename budget;
   release → 💬; an ask → ❓; an API error → ‼️; `/exit` → 🏁.
3. **A fork.** *Do:* Fork session. *Expect:* the fork's topic exists at once (before its first message).
4. **Not listed.** *Expect:* no topic for a shell tab, a Codex tab, an unprompted claude, a session in no tab,
   or an Idle session.

**Edge cases**
- **E1 Telegram refuses an icon** for a run → the state glyph falls back into the name.
- **E2 40 tabs at once** → all topics made without a 429 (paced ≤ ~1 call/s, the 60 s window held under 18).
- **E3 A tab whose title has a line break** → one line in the name.
- **E4 Topic order** → the list follows MOST RECENT (topics are created / posted lowest-ranked first; the quiet
  re-sort after the history replay).

---

### TGT-03 · A new topic replays the session's whole history

> **Since** 0.8.0 · **P1** · **Needs** F-TG, a session with 5+ turns · **Spec** MESSAGING.md §21.9, §21.11

**Promise.** A topic is a reflection of the entire session: its past turns are replayed oldest first, then
the live mirror continues, without flooding the chat.

**Phases**
1. **History.** *Do:* a session with 5 turns made BEFORE the bot was on; turn the bot on. *Expect:* per turn:
   a day line when the day changes, the user's prompt as `👤 You · <time> · typed at the PC` under the session's
   title, the final reply as its own message (rich Markdown: tables, headings, code render); in a VERBOSE
   topic also the remarks and folded tool lines; the card re-posted below the history at the end.
2. **Pacing.** *Expect:* history messages go only while the last 60 s hold < 18 calls; live posts of other
   sessions overtake history; no 429. *Check:* [LOG] the 10-minute `[msg] traffic` line.
3. **Forks.** *Expect:* a fork's topic replays the copied lines of its parent too.

**Edge cases**
- **E1 The user's answers** to questions → posted as user posts (`↩ answer`), notes included.
- **E2 A long reply** (> 3,500 chars) → split into parts in order.
- **E3 A rich message Telegram refuses** → that part goes as HTML in its place (order kept).
- **E4 A restart mid-replay** → resumes where it stopped (each turn commits when answered).

---

### TGT-04 · The live mirror of a turn

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock · **Spec** MESSAGING.md §6.4–§6.5, §21.12

**Promise.** Each turn appears as the user's line, then Claude's reply quoting it; only the final message
makes a sound; the user's line carries the turn's status.

**Phases**
1. **Typed at the PC.** *Do:* type `hello [[mock reply "**bold** and a table…"]]` in the tab. *Expect:* a silent
   `👤 You · 14:02 · typed at the PC` line (the session's title as its second row), then the reply as a rich
   message that REPLIES to that line, with sound. *Check:* [TG] `disable_notification` true on the user line,
   false on the final reply; `reply_parameters` pointing at the user line.
2. **Status on the prompt.** *Expect:* while the turn runs the user line shows ⏳; after the reply ✅; an error
   🔴; an Esc ■; an exit ⏹.
3. **Next turn.** *Expect:* the new prompt line replies to the previous turn's line; the previous line's status
   is removed.
4. **Runner and phone origins.** *Expect:* a queued prompt sent by the runner reads `⚙ runner`; a message sent
   from the topic itself is the user's own message (not repeated; its status is a REACTION 👨‍💻 🤔 👌 💔 😴 🙈).

**Edge cases**
- **E1 Conversation-only topic** (the default) → no remarks or tool lines; **verbose** (`/verbose` or the
  cog's "Topics show the work") → a remark before a tool call is its own message, the work log rolls under it.
- **E2 A turn whose reply is a question** → the question card (TGQ) instead of a plain reply.
- **E3 Mid-turn message typed at the PC** (absorbed) → `👤 You · mid-turn`.
- **E4 Hebrew / emoji** → intact; `<` in text escaped (never dropped).

---

### TGT-05 · An idle session's topic leaves; /pin keeps it

> **Since** 0.8.0 · **P0** · **Needs** F-TG, a short grace (cog: idle grace 1 min) · **Spec** MESSAGING.md §5.2–§5.3, §21.31

**Promise.** When a session becomes Idle its topic is deleted after the grace, and the main view says so in one
line; a pinned session never goes idle; a woken session gets a new topic with its history.

**Phases**
1. **Idle.** *Do:* move the session to Idle (board menu → Move to Idle/Done, or the Waiting decay). *Expect:*
   after the grace (`messagingIdleGraceMinutes`, default 10) the topic is DELETED; ≤ 60 s later ONE silent
   line in the main view: `💤 🟧 <title> is idle — its topic left the list. /idle in 🔎 Search wakes it.`
   (several sessions within 10 s → one line, ≤ 20 titles).
2. **Wake.** *Do:* in Search, `/idle` → Wake on it. *Expect:* the session is held in Waiting for you; a NEW
   topic with its history.
3. **Pin.** *Do:* in its topic `/pin` (or 📌 on the card). *Expect:* "📌 Pinned: this session never goes idle, so
   its topic stays."; the card shows 📌; Move to Idle lands in Waiting for you instead; the topic stays.
4. **Unpin.** *Do:* `/unpin`. *Expect:* the normal rule applies from now.

**Edge cases**
- **E1 Leaves Idle within the grace** → the topic stays.
- **E2 A closed tab** → its topic is deleted without a main-view line.
- **E3 App start** → every session reads not-live until its window re-homes: no topic is deleted before the
  windows finished restoring (the start-up hold, ≥ 120 s, ≤ 30 min).
- **E4 Reading on the phone** (any action in a topic) marks the session read at the PC (the flash ring and
  unread count clear) — viewing alone cannot be seen.

---

### TGT-06 · A topic follows its tab

> **Since** 0.8.0 · **P1** · **Needs** F-TG, mock · **Spec** MESSAGING.md §21.6 (Rekey)

**Phases**
1. **/clear in the tab.** *Expect:* the SAME topic continues with the new conversation (a marker), not a new
   topic.
2. **/resume <other id> in the tab** → the same topic follows the tab.
3. **/handover-here** → the successor in the same tab keeps the topic.
4. **A plain /handover** (a new tab) → a new topic for the successor.

---

### TGT-07 · Renames and colors

> **Since** 0.8.0 · **P1** · **Needs** F-TG · **Spec** MESSAGING.md §21.13, §21.29, §21.17 (thread rename)

**Phases**
1. **Rename at the PC.** *Do:* F2 → `new name`. *Expect:* the topic renamed; one silent line
   `“🟦 old” changed the title to “🟦 new”`; Telegram's own "topic renamed" notice deleted.
2. **Rename from the topic.** *Do:* `/rename newer`. *Expect:* the tab and topic renamed; the change line is the
   ONLY answer; renaming to the current title → "The title is already …".
3. **Rename the thread in Telegram.** *Expect:* the session (and tab) renamed to the thread's name (decorations
   stripped).
4. **Recolor at the PC.** *Do:* the tab's color → green. *Expect:* after ~8 s and within the rename budget: the
   name's square 🟩 and `… changed the color to 🟩`.

**Edge cases**
- **E1 During the history replay** → a change line is HELD and posted after the replay.
- **E2 Telegram answers TOPIC_NOT_MODIFIED** → counted as done (no retry loop).

---

### TGT-08 · The start note and messages sent while Agentmaster was closed

> **Since** 0.8.0 · **P1** · **Needs** F-TG, restart · **Spec** MESSAGING.md §4.3, §21.18

**Phases**
1. **Clean quit, start.** *Expect:* a start note in Search, with sound: the version (and "(was <old>)" after an
   update), "The last run closed at <time> · offline <span>".
2. **Crash, start.** *Do:* kill the GUI process; start. *Expect:* "⚠ The last run did not close normally … last
   seen running at <time> · offline about <span>".
3. **Offline message.** *Do:* while the app is closed, the user writes in a topic; start 11+ minutes later.
   *Expect:* not queued silently — "This message is <age> old — queue it for <title>?" [Queue] [Drop]; a read
   command (`/status`) is simply answered; an operate command is asked again.

**Edge cases**
- **E1 The Search topic deleted by the user** → made again at the next start (the start note's send reads
  "thread not found").
- **E2 A `/steer` from the backlog** → asked like a plain message.

---

### TGT-09 · Telegram's flood limits never stall the bot

> **Since** 0.8.0 · **P1** · **Needs** F-TG with 429 simulation · **Spec** MESSAGING.md §6.6, §21.14

**Phases**
1. **Burst.** 40 sessions active at once → no 429 (the pacing).
2. **A 429 with a long `retry_after`** (fake answers 600 s on `editMessageText`) → only that METHOD is parked;
   other sends go on; a second method refused → all parked; [LOG] `[msg] traffic … edits(card=… prompt=…)`.
3. **The hold ends** → the parked items go, in order.

---

### TGT-10 · The pinned directory in the main view

> **Since** 0.8.0 · **P1** · **Needs** F-TG, tags · **Spec** MESSAGING.md §21.16

**Promise.** One pinned message in the chat's main view lists the fleet's counts, the dedicated topics, every
tag carried by an open tab with those tabs, and the ten tabs used last; its links open topics, its dots bring a
topic to the top.

**Phases**
1. **Posted and pinned.** *Expect:* stats first (`🔵 5 running · 🟡 15 waiting · ⚪ 300 idle`, then
   approval / error / done / queued, `⏸ runner paused` while paused); 🔎 Search; each tag with its open tabs (a tag
   on no open tab is left out); `🕘 Recent activity` (the ten newest open, non-Done tabs, Idle included); a footer
   with a relative time. The pin's service notice is deleted.
2. **Updates.** *Expect:* edited only when its content changes, at most every 2 min.
3. **Links.** *Expect:* each title is a chip in its tab's color family; its URL `https://t.me/<bot>/<topic>?topic=<topic>`.
4. **Dots.** *Do:* tap a tab's state dot. *Expect:* its card re-posted silently, so its topic moves to the TOP
   of the list on every client; the tap stays busy until done.
5. **Deleted by the user** → posted again after 30 min.

---

### TGT-11 · /recreate rebuilds the topic list

> **Since** 0.8.0 · **P2** · **Needs** F-TG · **Spec** MESSAGING.md §21.8, §21.11

**Phases**
1. `/recreate` in Search → asks first (verbose or conversation only); confirmed → the session topics are made
   again in MOST RECENT order with their history; the Search topic is kept.

---

## Known defects (expected failures until fixed; re-check on 0.8.0)

### TGT-K1 · A session's topic the user deletes is never made again

MESSAGING.md §21.21. *Do:* delete a session's topic in Telegram; run a turn. *Correct:* the topic is made again
(topics = tabs). *Today:* every post fails; the card is re-posted every 60 s until the session idles or closes.

### TGT-K2 · A send that gets no answer is lost

§21.21. *Do:* the fake drops 3 sends (transport failure). *Correct:* retried. *Today:* a final reply or remark sent
without a callback is gone (only the card and the start note are retried).
