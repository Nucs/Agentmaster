# 11 · Telegram: 🔎 Search, new threads and the wizard (TGS)

> Conventions and fixtures: [README.md](README.md). Spec: MESSAGING.md §5.4, §21.7, §21.10, §21.17, §21.19–§21.20,
> §21.23, §21.25; SESSIONS.md §9 (the agent). Needs **F-TG**, the mock (the search agent's model requests are scripted
> like any other: the request text carries a `[[mock …]]` calling `mcp__sessions__search` / `mcp__display__show` /
> `mcp__tabs__*`), and **F-CORP** (a seeded corpus of sessions).

**What shipped (0.8.0).** 🔎 Search runs the Sessions page's AI search in a conversation of its own: an answer lists
the sessions with a quote of the evidence; a tap offers Resume, Resume 📌 or Fork into a tab on the PC. `/idle` lists
idle tabs with Wake buttons; `/search <words>` asks from any topic; `/search_tab` brings Search to the top. Opening a
NEW thread starts a new session through a wizard: folder (recent / most used, or a typed path; a missing folder can be
created), an optional model or preset, and the window (by default right after the newest tab working in that folder);
the thread's name becomes the session's title, and renaming the thread renames the session. An aborted creation
deletes the thread.

## Lifecycle model

```text
 🔎 Search:  request ─► the agent's plan line ─► tool calls ─► answer (≤ 10 evidence cards + the rest as lines, ⊘ Clear)
                                                                 └─ tap a session ─► [▶ Resume] [▶ Resume 📌] [⑂ Fork] [✕ Cancel]   (NOT BUILT: by state — Activate / Wake / Go to topic, TGS-05)
                                                                                       └─► a tab on the PC ─► its topic appears

 a NEW thread (user-made) ─► FOLDER ─(missing: CREATE?)─► MODEL (optional) ─► WINDOW ─► 🚀 launch ─► bind: the thread IS the session's topic
        │ held messages 👀 are handed to the session at the bind               │ refused ─► back to the step that can fix it
        └─ ✖ Cancel / 24 h untouched ─► thread DELETED (what it held copied to the main view)      └─ 3 min, no session ─► back to the wizard
```

---

### TGS-01 · Ask Search for a session and reopen it

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock, F-CORP · **Spec** MESSAGING.md §21.7, §21.10, §21.20

**Phases**
1. **Ask.** *Do:* in 🔎 Search write `the session where I fixed the tab drag crash` (plus the mock script that makes
   the agent call `search`, `read`, then `show` with ids and reasons). *Expect:* the agent's plan line ("I'll 1. … 2.
   …") while it works; then the answer: a descriptive reply and the likely next step, ≤ 10 evidence cards (each: the
   session's title chip, a quote of the evidence), the rest as one-line rows; ⊘ Clear.
2. **Pick.** *Do:* tap a CLOSED session's number (📦). *Expect:* the menu [▶ Resume] [▶ Resume 📌] [⑂ Fork] [✕ Cancel].
   (An open session's menu: TGS-05, NOT BUILT; 0.8.0 offers this same menu for every session.)
3. **Resume.** *Expect:* the session opens as a tab on the PC (background unless asked); it is held in Waiting for
   you; its topic appears; the bot ANSWERS which session opened (or why nothing did).
4. **Resume 📌** → the same, pinned (its topic never idles).
5. **Fork** → a fork opens (beside its source when the source is open); its own topic.

**Edge cases**
- **E1 Several windows open and the request names none** → the agent ASKS which window (the tabs tools refuse
  without a window).
- **E2 A closed (archived) session** → Resume brings it back; its queue is rehydrated.
- **E3 /clear (or ⊘ Clear)** → the Search conversation starts over; it otherwise carries on after a restart.
- **E4 The first Search request of a run** → waits for the index (≤ 90 s) and never answers from a partial index.
- **E5 A session command in Search** (`/steer`, `/runner`) → refused with Search's own command list.
- **E6 Interrupted-turn fork** (NOT BUILT, INT-07) → asks in Search.

---

### TGS-02 · Fleet commands in Search: /idle, /board, /search, /search_tab

> **Since** 0.8.0 · **P1** · **Needs** F-TG · **Spec** MESSAGING.md §21.19

**Phases**
1. **/idle** → the idle tabs with [Wake] buttons; Wake → the session held in Waiting for you; its topic appears with
   history.
2. **/board** → counts per state.
3. **/search <words>** in a session topic → answered IN Search (the request relayed); Search moves to the top of the
   thread list.
4. **/search_tab** (or typed `/search-tab`) from any topic → Search first in the list (a line posted in it).
5. **/pauseall /resumeall** → the global pause on every window.
6. **/newsession <folder> [model]** in Search → a new session in that folder.

**Edge cases**
- **E1 A new thread whose only message is `/search …`** → deleted once the command left it for Search.

---

### TGS-03 · A new thread starts a new session (the wizard)

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock, F-W2 · **Spec** MESSAGING.md §21.17.4, §21.25

**Promise.** A thread the user opens becomes a new session's topic after a short wizard; nothing is launched until
every step is answered; an aborted creation leaves no stray thread.

**Phases**
1. **Open a thread.** *Do:* in the chat's main view type `look at the parser` (Telegram makes a new thread).
   *Expect:* the message is HELD (👀) and one picker message: 🕘 Recent and 🏆 Most used folders (ten a page, pager
   row `[1 / 4] [Recent ▶]`), "Then a model (optional) and a window. ✖ Cancel deletes this thread."
2. **Folder.** *Do:* tap a recent folder. *Expect:* the MODEL step: "Default is the settings model, `opus`";
   [✅ Default] [✏ Specify…], then the launch models and presets (as the PC's Fork/Open New Session menus), the last
   specified model (↺) when missing from the menu.
3. **Model.** *Do:* ✏ Specify… → type `claude-sonnet-5-5[1m]`. *Expect:* accepted (letters, digits, `- _ . : / [ ]`,
   one word); it joins the PC's "Specify a model" droplist and pre-fill.
4. **Window.** *Expect (two windows):* per window its number, tab count, "used last", how many tabs work in the
   folder, its newest tab and ten newest titles; "⭐ Default: window N — next to “X”, the newest tab in <folder>".
   *Do:* ⭐ Default.
5. **Launch.** *Expect:* "🚀 Opening a new session in <folder> · <model> · window N, next to “X”…"; the picker deleted;
   the tab opens right after X in window N; when the session is live the thread is bound as its topic, renamed to
   the session's title; the held message `look at the parser` is handed over as the first prompt.
   *Check:* [LOG] `[msg] new thread N: picked folder …`, `opening session S in X model M in window W after A`;
   [ENG] the session's `createdUnixMs`; [MOCK] the first prompt with the chosen model.

**Edge cases**
- **E1 A typed folder that differs only in case** (`k:\SOURCE\agentmaster`) → resolved to the disk's spelling; no
  second folder.
- **E2 A typed folder that does not exist** → "📁 X does not exist yet: K:\a\b\X" with what Create makes ("2 new
  folders") → [📁 Create the folder] creates it; [◀ Back to the folders].
- **E3 An illegal path** (`< > : " | ? *`, a device name, `..`, a trailing dot or space, > 240 chars) → refused with the
  reason.
- **E4 A model id with a space, `;`, `$`, a backtick or a leading `-`** → refused.
- **E5 ✖ Cancel at any step** → the thread is DELETED; what it held is copied into the main view (a silent line with
  an expandable quote).
- **E6 Nothing chosen for 24 h** → "🗑 “X” was deleted — nothing was chosen in it for 24 hours."
- **E7 The window REFUSES the launch** (a preset that cannot launch) → back to the MODEL step with the reason; never
  retried without a tap.
- **E8 The chosen window closed meanwhile** → "That window is not open any more — pick another one".
- **E9 The session does not come up in 3 min** → back into the wizard, the picker re-posted.
- **E10 Restart during the wizard** → it comes back where it stood (`state.json`), its buttons still working.
- **E11 One window only** → the window step is skipped (after its folder tab).
- **E12 A button of an earlier step** → "That button belongs to an earlier step".
- **E13 Rename the thread during the wizard** → that name becomes the session's title.
- **E14 The user deletes the thread / the picker** → the wizard is dropped / a new picker posted.
- **E15 No preset at all** → the model step says where to add one (Settings ⚙ → Presets → TEMPLATES).

---

### TGS-04 · The secretary bot (optional)

> **Since** 0.8.0 · **P2** · **Needs** F-TG with a second bot, Telegram Business (Premium) · **Spec** MESSAGING.md §21.11

**Phases**
1. **Configure** a second bot token (Credential Manager), Secretary Mode + Bot-to-Bot Communication in @BotFather,
   connected under Telegram → Settings → Chat Automation.
2. **Typed at the PC** → the prompt appears as the USER's own message (live and in a history replay); without the
   connection, the main bot's `👤 You` line.
3. **No echo loop** → the secretary's posts are never read back as user prompts.

---

### TGS-05 · A pick's menu fits where the session stands

> **NOT BUILT** (0502c9380, after 0.8.0) · **P1 when built** · **Needs** F-TG, mock, F-S2 + a deactivated tab, an idle tab, an
> exited claude, a pinned session · **Spec** MESSAGING.md §21.38, §5.4

**Promise.** A session Search finds is offered what it needs from where it stands: a closed one is resumed, a deactivated
or exited one is ACTIVATED in its own tab, an idle one is woken, one with a topic is shown — never a "Resume" of a session
that already has a tab.

**Phases** (one sub-case per state; the Search request's script `show`s that session)
1. **Closed** (📦) → [▶ Resume · ▶ Resume 📌] / [⑂ Fork · ✕ Cancel]; Resume opens it in a background tab (TGS-01).
2. **Deactivated, idle, no topic** (🌙, ParkedTab: Deactivate it at the PC first) → the line says "deactivated (its Claude is
   not running), so it has no topic"; [▶ Activate · ▶ Activate 📌] / [⑂ Fork · ✕ Cancel]. *Do:* ▶ Activate. *Expect:* the tab's
   claude starts IN PLACE on the PC with no window or tab switch (the dot fills); the session held in Waiting for you; its
   topic appears within seconds; a message sent there goes in. *Check:* [LOG] `[nav] remote telegram search pick N <sid8>
   (deactivated tab)`, `… search activate <sid8> (…) woken`, `remote telegram reactivate <sid8> -> start`,
   `[nav] activate-dormant <sid8>`.
3. **Deactivated with a topic** (🌙, ParkedListed: pinned, or parked in Waiting / Needs approval / Error) → [▶ Activate · ▶ Activate
   📌] / [💬 Go to topic] / [⑂ Fork · ✕ Cancel].
4. **Claude exited** (⏹, ExitedTab: `/exit` in the tab) → the same menu as 3; ▶ Activate starts claude again in the same
   conversation through the window's restart (interrupted turns kept interrupted). *Check:* [LOG] `reactivate <sid8> ->
   restart (exited)`.
5. **Idle, claude running** (💤, IdleTab) → [⏰ Wake · ⏰ Wake 📌] / [⑂ Fork · ✕ Cancel]; Wake brings its topic back.
6. **Listed** (💬, a topic exists) → [💬 Go to topic · 📌 Pin] / [⑂ Fork · ✕ Cancel]; Go to topic puts that topic FIRST in the
   thread list (a re-post); 📌 Pin pins it.

**Edge cases**
- **E1 A pinned open session** → no 📌 twin in its menu.
- **E2 The state changed between the menu and the tap** (the deactivated tab entered at the PC, then ▶ Activate) → acts on the
  state AT THE TAP: "woken", no second start.
- **E3 An ADOPTED session** (a claude typed into the user's own shell) → never "deactivated" or "exited"; Search never starts or
  restarts it.
- **E4 A session closed since the menu** → Go to topic / Pin say so.
- **E5 The topic left since the menu** → Go to topic says so.
- **E6 Every menu** → at most two buttons a row, ⑂ Fork · ✕ Cancel last, "Resume" only for a closed session.
- **E7 A request in words ("activate it")** → the agent has no activate tool: `mcp__tabs__resume` says where the open tab is
  (§21.38 "Not done"); the test pins that until an activate tool exists.
