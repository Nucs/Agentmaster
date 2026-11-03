# Claude Code ⇄ Telegram — Integration Specification

> **What this is.** A complete, host-neutral specification of a Telegram bot that mirrors and drives the Claude Code
> sessions running on a PC: one forum topic per working session, the session's whole conversation in it, prompts and
> commands sent from the phone, Claude Code's own questions and dialogs answered with buttons, voice notes read as
> prompts, a search topic that finds and revives any session ever run, and new sessions started from a new thread.
>
> **Where it comes from.** It is distilled from a working implementation (its design log, its code and its commit
> history), and every platform fact was checked against Telegram Bot API **10.3** (2026-08-24) and Claude Code
> **2.1.291 – 2.1.310** unless a line says otherwise. Where a behavior exists because of a MEASURED platform limit, the
> measurement is given: those limits are the expensive part of this design.
>
> **Version:** 1.0 · 2026-11-01.

## 0. How to read this document

- **MUST / MUST NOT / SHOULD / MAY** are normative. Everything else describes or explains.
- **The bridge** is the software this document specifies: the Telegram bot and everything it runs.
- **The host** is whatever runs the Claude Code processes on the PC: a terminal application with windows and tabs, a
  PTY multiplexer, a daemon. §5.2 lists exactly what the bridge needs from it. The reference implementation embeds the
  bridge in a tabbed terminal application on Windows (ConPTY, WinHTTP, Credential Manager, Direct2D); platform-specific
  details are marked as such.
- `{app}` is a placeholder for the product's name in user-visible strings; `<profile>` is the bridge's data folder (one
  per installation); `<pid>` the host process id.
- Constants appear with their reference values; Appendix A collects them.
- Every user-visible string quoted here is the reference wording. An implementation MAY reword, but the structure (what a
  message says, in which order, what makes a sound) is normative.

### Contents

1. Overview · 2. Terms · 3. Telegram facts · 4. Claude Code facts · 5. Architecture · 6. Setup, identity, settings ·
7. The chat's layout · 8. Topics are sessions · 9. The mirror · 10. The sticky card · 11. Inbound: messages, commands,
buttons · 12. Claude Code's dialogs from Telegram · 13. Session control · 14. The Search topic · 15. A new thread starts a
session · 16. The pinned directory · 17. Pictures · 18. `/status` · 19. Voice notes · 20. File links and `/download` ·
21. The outbox: pacing and flood control · 22. Persistence · 23. Lifetime: start, quit, crash · 24. Security and privacy ·
25. Logging · 26. Testing · 27. Known gaps and open items · Appendices A–F.

---

## 1. Overview

### 1.1 What the user gets

The bot's private chat runs in Telegram's **Threaded Mode** (forum topics in a private chat). Its layout:

```
"All" (the main view)
   📌 DIRECTORY — state counts · 🔎 Search · every tag with its tabs · the ten newest tabs
   💤 "🟧 project-x is idle — its topic left the list. /idle in 🔎 Search wakes it."
🔎 Search                 ← the search agent: find, resume, activate, wake, fork any session
⚡ 🟦 drag crash fix       ← a working session: the icon is its STATE, the square its color, then its title
💬 🟩 docs
❓ 🟪 hotfix review        ← needs approval: a question card waits here
(a thread the user opens)  ← a wizard: folder → model → window → a new session is born in it
```

Inside a session's topic:

```
👤 You · 14:02 · 📱 from the phone · ✅ answered        (the user's own message or the bot's line for it)
🟦 drag crash fix                                        (the title row: a link back to this topic)
fix the drag crash

Claude's remark before a tool call ("Let me look at the drag handler first.")
[🔧 Read ×2, Edit] (verbose topics only: the tool calls, folded)
Claude's final reply — rich Markdown (tables render), quoting the prompt it answers   🔔 (the only sound of the turn)

┌ the STICKY CARD (always the topic's last message) ─────────────────┐
│ 🟡 Waiting for you since 14:05                                     │
│ 📌 [🟦 drag crash fix]                                             │
│ /source/project · main · opus-5-5 · max · 182K                     │
│ ✎ unsent at the PC: "and then rebase onto…"                        │
│ [screenshot of the terminal]   🖥 the screen at 14:05:12           │
│ [⚙️ Session]                                                        │
└────────────────────────────────────────────────────────────────────┘
```

- **A message in a topic is the session's next prompt**: queued, delivered at the next ready moment, never typed into a
  running turn (`/steer <text>` is the one explicit exception).
- **`/commands`** run the host's actions for that session (rename, pin, restart, fork, transcript, screen…, and `/enter`, which
  submits the prompt already waiting in the box at the PC); Claude Code's own recorded commands (`/compact`, `/model x`, skills)
  are queued like prompts; commands that leave no record are refused.
- **Buttons** answer Claude Code's `AskUserQuestion` dialogs (options, own words, notes, several questions as tabs) and its
  `/add-dir` dialog; the card's buttons restart, reactivate, interrupt (Esc), park, fork, and draw the screen.
- **Voice notes** are transcribed with the session's own vocabulary and read as the prompt they say (in the Search topic: with
  the fleet's names, as the search they say).
- **Only a turn's final message makes a sound.** Everything else is silent, an edit, or deleted after use.

### 1.2 The picture

```
PC
 ├─ Claude Code × N        interactive TUI in PTYs, hosted by the host
 │    ├─ hooks ────────────► the host's hook pipe ──► session registry ──► state machine
 │    ├─ transcript JSONL ─► transcript tail: the mirror's source, the state reconcilers
 │    ├─ presence heartbeat ► state belts, version, pid
 │    └─ PermissionRequest hook (AskUserQuestion) ──► the ask pipe ──► the question card
 ├─ host                    windows · tabs · prompt queue + runner · ONE send seam into the PTY · screen reads
 └─ bridge (one per installation; inside the host process or beside it, §5.2)
      ├─ poll thread    getUpdates long poll → sender check → router
      ├─ send thread    the OUTBOX: priorities, ~1 call / 1.05 s, flood parking, tap answers
      ├─ tick thread    every 1.5 s or on a registry change: topic sync, transcript mirror, cards, names,
      │                 directory, history replay, wizards, question / menu cards, steers, voice lanes
      ├─ workers        pictures · /status probes · folder probes · window census · voice
      └─ optional       Search agent (an LLM with session-search tools) · file-link HTTP server · voice engine
```

### 1.3 Invariants

These hold everywhere; every later section is consistent with them.

| # | Invariant |
|---|---|
| **I1** | **Owner only, sender first.** Every update is checked by the sender's numeric user id before anything else of it is read; any other sender is dropped (a button tap is answered "This bot is private."). |
| **I2** | **Topics are sessions.** A topic exists exactly for each qualifying session (§8.1), within its idle grace; the bridge creates, renames, re-keys and deletes topics to keep it so. |
| **I3** | **A message is a queued prompt.** Nothing typed in a topic reaches a running turn, except the explicit `/steer`. |
| **I4** | **One sound per turn**, and one per "needs you" moment. Everything else is sent with `disable_notification`, or is an edit. |
| **I5** | **Never act on a stale button.** Actions are single-use, expiring tokens, or stateless verbs that re-read the current state at the tap. |
| **I6** | **Never guess a session.** A message outside a session's topic names no session; only the Search topic and new threads take such messages. |
| **I7** | **One poller per bot token**, machine-wide. |
| **I8** | **An old message never acts unasked** (the backlog rule, §11.2.6). |
| **I9** | **The network never stalls the host.** All network work runs on the bridge's threads; every wait is bounded; every exception is contained and logged. |
| **I10** | **Never type into what you have not just seen.** Keys go into a session only through the host's one send seam or into a dialog the host has just re-read on the thread that owns the terminal; never into a menu, a held input box, a parked tab, or the shell a dead claude left behind. |
| **I11** | **No hidden commands.** A Claude Code command that leaves no record in the session is never sent from Telegram. |
| **I12** | **One chat, one budget.** Every call to the chat shares one flood budget; nothing re-edits periodically (clock times, never relative spans). |
| **I13** | **Nothing is lost across restarts.** The topic map and everything that names a Telegram message is persisted atomically; a restart MUST NOT churn topics. |
| **I14** | **The phone answers as the PC does.** Answers to Claude Code's dialogs are composed exactly as the TUI composes them, byte for byte. |

### 1.4 Non-goals

- Agents other than Claude Code (their sessions get no topic), and Claude Code processes the host does not host
  (observe-only at most).
- Sharing the fleet with other people: one owner per bot.
- A continuous pixel mirror: pictures are drawn on request and at card updates only.
- Working while the bridge is down: Telegram keeps updates 24 h, and the backlog rule handles them at the next start.
- Approving plans (`ExitPlanMode`) and tool-permission prompts from Telegram: they are shown (the card rings), answered at
  the PC.
- Files and photos sent from the phone into a session (answered "not taken yet").

---

## 2. Terms

| Term | Meaning |
|---|---|
| **Session** | One Claude Code conversation, identified by its session id (a GUID). The host knows it as a record with a state, a title, a folder, a queue. |
| **Conversation id** | The id Claude Code writes the transcript under. `/clear` and `/resume` switch a live claude to another id; `/compact` keeps it. |
| **Transcript** | Claude Code's JSONL file of the conversation: `<claude config>/projects/<folder key>/<session id>.jsonl`. |
| **Heartbeat** | Claude Code's presence file `<claude config>/sessions/<pid>.json`: version, session id, status (busy / idle / waiting / shell). Trusted only when it names the conversation. |
| **Host** | The program that runs claude processes in PTYs and owns their windows and tabs (§5.2). |
| **Tab** | One host slot that runs (or can run) one claude in a PTY. Has a title, a color, tags, a queue, a runner mode, a tab token (the PTY connection's id). |
| **Parked / dormant tab** | A tab whose claude is not running (restored and never entered, or deactivated). Its conversation resumes when it is started. |
| **Adopted session** | A claude the user started by hand in a shell the host owns. The host can type into it but cannot restart or park it (its PTY is the user's shell). |
| **Window** | A group of tabs. Windows are numbered in creation order; the user MAY name one. |
| **Live** | The session is open in a tab (as opposed to closed/archived, which is on disk only). |
| **States** | Running · Waiting for you · Needs approval · Error · Done (claude exited) · Idle (§4.4). |
| **Turn** | One prompt and everything until Claude Code comes to rest. |
| **Prompt queue / runner** | Per-session ordered prompts; the runner (Auto or Paused) sends the next one when the session is ready (§11.2). |
| **Topic / thread** | A forum topic of the bot's private chat; `message_thread_id` is its id. |
| **Main view ("All")** | The chat's General area; a message with no thread id lands there. |
| **Card** | The topic's sticky status message (§10). |
| **Question card / menu card** | The managed message that answers an `AskUserQuestion` dialog (§12.1) / a `/add-dir` dialog (§12.2). |
| **Directory** | The one pinned message of the main view (§16). |
| **Search topic** | The dedicated topic that hosts the search agent (§14). |
| **Tap** | A press of an inline button: a `callback_query` the bridge must answer. |
| **Rich message** | Bot API 10.1+ `sendRichMessage` (GitHub Markdown with tables, in-text buttons, media blocks). **Classic** = `sendMessage` with HTML. |
| **Title token** | An internal marker that frames a title until the send layer renders it as a chip, a link or plain text (§9.8). |

---

## 3. Telegram facts the design depends on

Each row is a fact of the platform and what the design does about it. "Measured" means observed with a real bot.

### 3.1 The chat and its topics

| Fact | Consequence |
|---|---|
| Threaded Mode is switched on ONLY in the **BotFather Mini App** (search BotFather → Open → My bots → the bot → Bot Settings → Threads Settings → Threaded Mode). The classic `/mybots` chat menu does not show it; "Secretary Mode" and group rights are other switches (`getMe` then shows `can_connect_to_business: true` while `has_topics_enabled` stays false). | The setup text names the Mini App path. While `getMe.has_topics_enabled` is false the bridge re-asks `getMe` every **60 s**, its status line says where the switch is, and the owner gets ONE setup notice a day. |
| `getMe` reports `has_topics_enabled` and `allows_users_to_create_topics`. With users allowed to create topics, every message typed in the main view **opens a new topic** with that message ("Type any message to create a new thread"). | A thread the user opens starts a new session (§15). Commands typed in the main view arrive in their own new thread too (§15.5). |
| A new topic named after its first message carries `is_name_implicit: true` in its `forum_topic_created`. | An implicit name waits for the first message; an explicit name becomes the new session's title. |
| `createForumTopic`: name 1–128 characters; `icon_color` one of **six** (0x6FB9F0, 0xFFD67E, 0xCB86DB, 0x8EEE98, 0xFF93B2, 0xFB6F5F), set ONLY at creation; `icon_custom_emoji_id` optional. `editForumTopic` changes the name and the custom-emoji icon at any time, never the color. `deleteForumTopic` deletes the topic **with all its messages**. A private chat's topic cannot be closed or hidden. | A topic's color is the nearest of the six to the tab's color, fixed at creation. The STATE is shown as the editable icon (§8.5). An idle session's topic is DELETED (there is no other way to take it off the list). |
| A topic with no icon of its own is drawn by the apps as a bubble in its creation color, with a character of the NAME inside. Android, iOS and Web draw the **first grapheme** (Android: `ForumUtilities.createTopicDrawable`). Telegram Desktop draws the **first letter or digit**, skipping emoji (`ExtractNonEmojiLetter`), so a name `🔎 Search` is an "S" bubble there. | A name that starts with a state glyph showed the state twice on the phone; hence the state icon, and the name starts with the color square. A glyph in a name never reaches Desktop's icon. |
| `getForumTopicIconStickers` lists **112** custom emoji, the ONLY ones a bot may set as a topic icon (the same list for every bot). Telegram's words: "Telegram Premium users can pass any custom emoji to icon_emoji_id, while other users can only use the custom emojis contained in the inputStickerSetEmojiDefaultTopicIcons emoji pack". **None is a colored circle**, and **every one is an ANIMATED sticker** (TGS). Measured 2026-10-18 and 2026-11-02. | The six state icons (§8.5) and the Search topic's 🔎 come from that list; the owner picked them from a sheet of all 112. A still icon is impossible for a bot. The apps decide the motion: the phone's Settings › Power Saving stops animated emoji. Telegram Desktop's topic list ignores its Animated Emoji switch: it pauses a topic icon only while a menu or layer is open or the window is inactive. |
| No method lists a chat's topics or reads one back; no update tells a bot a topic was deleted; deleting a message sends no update either. | The bridge's **topic map** (session → thread, card, cursors) is its only record and is persisted atomically with a backup (§22). A deleted message is detected only when a call on it fails ("message to edit not found", "message to react not found", `MESSAGE_ID_INVALID`). |
| The bot's own service messages come back through `getUpdates` with the BOT as the sender: `forum_topic_created`, `forum_topic_edited` (every rename / icon change), the "pinned a message" note. | Own-id messages are skipped silently before the foreign-sender test. A `forum_topic_edited` and a pin note the bot caused are **deleted** (they would sit in the topic / above the main view). A rename therefore costs **two calls**. |
| `editForumTopic` that changes nothing answers `400 TOPIC_NOT_MODIFIED`. | Counted as a rename that took (its change line and card follow). |
| The topic list is ordered by each topic's **last message**, newest on top (the user's own pins stay above). There is no call to reorder or pin a topic, and no read-back of a pin. | Order is made by WHEN the bridge posts (§8.9). The user pins Search by hand; the bridge never deletes or recreates the Search topic, so the pin survives. |
| A topic's `message_thread_id` IS the user-space id of its creation message (`createForumTopic` returns it). | `https://t.me/<bot>/<thread>?topic=<thread>` names the topic correctly. |
| Opening a topic from a link: **Telegram Desktop** opens it when its topic list is loaded (in-text buttons need Desktop ≥ 7.1); **iOS** should (read from source); **Android never does** — every link form lands in "All" at the topic's creation message (`LaunchActivity` opens forums only for groups). Android switches a bot chat's topic only through its own controls, including a "Continue last thread ›" button under the chat's LAST message when that message is in a topic. | Titles are links (Desktop / iPhone), and every place that lists sessions also offers a **front button** that re-posts the topic's card so the topic becomes FIRST in the list on every app (§16.4). |
| A bot learns nothing about reading, viewing, presence or typing: no `Update` field and no method carries it (TDLib drops read receipts for bots). The only "user is here" signal is an action in a topic. | Any action in a topic marks the session read on the PC. Live per-topic activity cannot target "the topic the user looks at" (§F.1). |

### 3.2 Messages and formatting

| Fact | Consequence |
|---|---|
| Classic text: 1–4,096 characters; HTML parse mode with `b i u s code pre a blockquote` (`expandable`), `tg-spoiler`, `tg-time`; **no table form**. A message Telegram cannot parse is refused ("can't parse entities"). | Long text is split at paragraphs, never inside `<pre>`; a refused HTML chunk is re-sent as plain text **in its own place** (§21.3). |
| `<tg-time unix="…" format="r|t|dt">text</tg-time>`: the relative format (`r`) is kept current by the apps themselves; the text inside is what an app that does not know the entity shows; an entity over NO text shows nothing. | Every time the bridge prints is a `tg-time` with the PC's clock text inside. Relative "updated 5 minutes ago" footers cost no edit. |
| **Rich messages** (Bot API 10.1+): `sendRichMessage` with `InputRichMessage` as `markdown` (GitHub-flavored), `html` or `blocks`: headings, lists, task lists, **tables** (≤ 20 columns), quotes, fenced code, `<details><summary>` collapsible blocks (Markdown parsed inside), footnotes, math. Limits **32,768 characters, 500 blocks**. `editMessageText` takes `rich_message` and turns a classic message rich and back, keyboard kept. | Claude's replies go rich (§9.7). The card and the directory are rich. |
| Measured on rich Markdown: an unknown `<tag>` is DROPPED with its text (`List<int>` → `List`); `&lt;` restores it, `\<` does not; the other backslash escapes work (`\* \_ \| \$ \= \- \# \[ \] \` \~`); a single line break inside a paragraph is a space (two trailing spaces keep it); `$HOME` becomes a cashtag, `$5` stays text. Rich HTML also reads a raw line break as a space; `<p>` and `<br>` keep the structure. | The rich preparation rules of §9.7. |
| Rich **in-text buttons** (10.3): `<tg-button type="copy_text|url|callback_data|disabled" style="primary|success|danger|link" …>label</tg-button>`. | Titles render as colored chips (§9.8); the directory's dots are callback buttons (§16). |
| Rich **media** (10.2): `<figure><img src="tg://photo?id=X"/><figcaption>…</figcaption></figure>` with the file uploaded as `attach://X` in the same multipart call, or reused by `file_id`; an edit can replace the photo by upload or by `file_id`; the same edit twice answers "message is not modified". | The card carries the terminal's screenshot with no extra call (§10.6). |
| Button `style` is `primary` (blue), `success` (green), `danger` (red) — "apps may use theme-specific colors": **no hex color** can be asked for. Custom emoji in a message need a Premium owner (or a Fragment username): stripped otherwise. A topic NAME is plain text. | Colors are shown as colored **square emoji** (🟥🟧🟨🟩🟦🟪🟫⬛⬜) plus the nearest button style (§8.6). |
| `callback_data` is 1–64 bytes; a `copy_text` button copies up to 256 characters. | Every callback grammar of Appendix B fits 64 bytes; paths and model ids are never put in callback data (indexes into stored lists instead). |
| A tapped button shows a progress indicator until the bot calls `answerCallbackQuery`. Telegram's deadline is undocumented; measured ~15 s (later answers fail `QUERY_ID_INVALID`), some frameworks say 10 s. The toast text is ≤ 200 characters. | A tap is answered when its WORK is done, at most **9 s** after it arrived (§11.5.2). Toasts are cut at 190 units, never inside a surrogate pair. |
| `ForceReply` makes the phone open its keyboard as a reply to a given message. | The question card's "write my own answer" / "add a note" (§12.1). |
| Reactions: a non-Premium bot may set **one** reaction per message, from a fixed emoji set (✍ 👀 👨‍💻 👍 👎 👌 🤔 💔 😴 🙈 ⚡ 🫡 … are in it; ⏳ ✅ 🔴 are not). | A user's own message shows its status as a reaction (§11.2.4); the bot's own lines show it as text (edits). |
| A bot may delete its own and the user's messages in a private chat (within 48 h). | Commands that are moves, not content (`/search-tab`), are deleted where typed (§14.6). |
| `setMyCommands` takes names of `a–z 0–9 _` only and **refuses the whole list** otherwise. A client makes a command LINK of `/search-tab` only up to the hyphen. | The "/" menu lists `search_tab`; printed help puts `/search-tab` only inside `<code>` (never a link). |
| Files: upload ≤ 50 MB; a photo ≤ 10 MB with width + height ≤ 10,000 px and a side ratio ≤ 20 (shrunk to 2,560 px on its long side); `getFile` downloads ≤ 20 MB; "send by URL" only for PDF / ZIP ≤ 20 MB. | Tall conversation pictures go as DOCUMENTS (§17.3); a refused photo is re-sent as a document; voice notes over 20 MB are refused (§19). |

### 3.3 Receiving

| Fact | Consequence |
|---|---|
| `getUpdates` long polling (`offset`, `timeout`, `limit`, `allowed_updates`); updates are kept **24 h** at most; no polling while a webhook is set. | Poll with `timeout` **40 s**, `limit` 100, `allowed_updates = [message, edited_message, callback_query, business_connection]`; a receive timeout of 60 s around it. Never a webhook. |
| A second concurrent `getUpdates` on a token gets **409 Conflict**. | One poller per token: a machine-wide mutex named from the token's hash; a 409 stops the poll loop with a status line ("another poller reads this bot … One bot per install."). |
| Update ids are per bot. An offset above a new bot's ids confirms (drops) every update of it. | The state file is keyed by the bot's id (§6.3). |
| `edited_message` arrives for an edit of the user's message. | An edit of a still-queued message replaces the queued text (§11.2.5). |

### 3.4 Flood control (measured)

| Fact | Consequence |
|---|---|
| Documented: "avoid more than one message per second in a single chat", "20 per minute in a group", ~30 per second per bot. Every topic of the private chat is the SAME chat, and **edits count**. | One outbox paces every call to the chat at **1.05 s** (§21). |
| Measured: ~27 messages a minute for ~11 minutes earned a 429 (retry after 40 s); **18 calls a minute** held for 1 h 40 min without one. The private chat's budget drains near the GROUP rate, not 1/s. | Background traffic (history replay, re-sorts, renames, format upgrades) only while the last minute holds fewer than **18 calls** (§21.4). |
| Measured: ~1 card edit per second for ten minutes (47 cards whose "3m" became "4m") earned `retry_after` **1,526 s**; later a hold of **6,542 s** (1 h 49 min) — and a hold on one method answered every other method of the chat with the same deadline. | Cards show clock times, never spans (§10.1). A 429 over 60 s PARKS the refused method, a second method parks everything (§21.5). |
| A call made inside a 429 window extends it. | A 429 is always honored before the same call is made again. |

### 3.5 Business ("secretary") mode

| Fact | Consequence |
|---|---|
| A bot with Secretary Mode (BotFather) can be connected by the user under Telegram Settings → Chat Automation (formerly Business → Chatbots; no Premium since Bot API 10.0; ONE such bot per account); the connection, its id and rights arrive **only** as a `business_connection` update; sends carry `business_connection_id`; `can_reply` covers "private chats with incoming messages in the last 24 hours". | The bridge listens for `business_connection`, keeps the id / enabled / can_reply, and PROBES whether it can post as the user (§9.5). |
| Telegram documents a business bot sending to the user's chats with OTHER bots (with Bot-to-Bot Communication Mode on). A bot posting into its OWN chat on the user's behalf is undocumented. | An optional second **secretary bot** holds the connection and posts the user's turns into the topics bot's chat (§9.5). |
| A message a business bot sends arrives at the topics bot carrying `sender_business_bot` / `business_connection_id`. | Dropped before anything reads it, plus an echo guard (§9.5). |

---

## 4. Claude Code facts the design depends on

### 4.1 Launching and identity

- A new session: `claude --session-id <new GUID>` (the bridge or host mints the id BEFORE the launch, so a launch from the phone
  can be bound to its thread). Resume: `claude --resume <id>`, valid only when the transcript exists ("No conversation found"
  otherwise). Fork: `claude --resume <source> --fork-session --session-id <new>`.
- **A fork writes no transcript until its first turn**, and its transcript then BEGINS with the source's lines copied, each
  stamped `forkedFrom`. Its first `SessionStart` hook arrives under the SOURCE id.
- `/clear` and `/resume` switch a live claude to another conversation id; `/compact` (manual or auto) runs in place.
- A conversation that ended in an interrupt or mid-turn can be continued at the next start by appending one `last-prompt`
  line and setting `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1`; an Esc before any output writes no row (the prompt simply
  returns to the box).
- The fullscreen renderer (`CLAUDE_CODE_NO_FLICKER=1`, `/tui fullscreen`) draws the whole UI in the terminal's
  **alternate screen buffer**, which keeps no scrollback: the terminal holds one screen, never the conversation.

### 4.2 Hooks

- Hooks are given with `--settings <file>`; for claude ≥ **2.1.139** they run in exec form (a program with an `args`
  array, no shell). Events used by the host: `SessionStart` (source: startup / resume / clear / compact), `UserPromptSubmit`,
  `Stop`, `Notification` (permission prompts), `SessionEnd` (reason), `PreCompact`, `PostCompact`, and — for this
  integration — **`PermissionRequest` on `AskUserQuestion`** (§12.1).
- ⚠ A `PreToolUse` / `PermissionRequest` entry that cannot be loaded makes Claude Code skip the permissions declared in the
  same settings file: the generated settings MUST stay exactly valid.
- **Local commands** (`/add-dir`, `/model <id>`) are run by Claude Code itself: no `UserPromptSubmit`, no prompt line. Their
  transcript ECHO (`<command-name>` + `<command-args>`, then `<local-command-stdout>`) is the only evidence they ran.

### 4.3 The transcript lines the bridge reads

| Line | What the bridge makes of it |
|---|---|
| `user` with text content (not meta, not a tool result) | the turn's **prompt** |
| `attachment` `queued_command` (`commandMode: "prompt"`, `origin.kind: "human"` or absent) | a prompt typed **while the turn ran** (absorbed mid-turn). Other origins (`peer`, `task-notification`) are machine traffic. |
| `assistant` content blocks — one LINE per block, sharing `message.id`, with `stop_reason` | text → a remark (when a tool call follows) or the turn's **final reply** (when the stop reason ends the turn); `tool_use` → a tool call (`AskUserQuestion` → a question; `ExitPlanMode` → a plan) |
| `user` tool result (`is_error`, and `toolUseResult`) | a failed result; for `AskUserQuestion`: `toolUseResult.answers` (question → label or own words), `annotations[q].{preview,notes}`, `questions`, `afkTimeoutMs` (Claude Code's idle timeout resolved it), `response` (free text instead of answers); for edits: `structuredPatch` (the hunks the TUI draws) |
| `system` `compact_boundary` (`postTokens`) | a compaction marker |
| `system` `away_summary` | Claude Code's **recap**, written a few minutes after a turn when the user is away |
| a `[Request interrupted by user…]` marker | the turn was interrupted |
| an `assistant` line with `isApiErrorMessage` | the turn died of an API error (state Error) |
| `<command-name>/x</command-name><command-args>…</command-args>` echo, `<local-command-stdout>`, `<local-command-caveat>` | a slash command (and a local command's result) |
| `custom-title` (a `/rename`), `ai-title` | the session's name in Claude Code's own `/status` |
| `attachment` `model`, `deferred_tools_delta`, `instructions`, `environment`, `permission-mode` lines, `hook_permission_decision` | facts for `/status` (§18); the hook's decision proves a hook answered |
| lines stamped `forkedFrom` | a fork's copy of its source's history (skipped by the live mirror, kept by the history replay) |
| `isSidechain` lines | subagents / teammates (not mirrored) |
| `isMeta` lines, bookkeeping | nothing |

### 4.4 The state model the bridge consumes

The host derives one state per session from three sources: hooks (push), the transcript tail (pull) and the heartbeat. The
bridge never derives state from the screen. It relies on:

| State | Meaning | Typical entries |
|---|---|---|
| **Running** | a turn is in flight | `UserPromptSubmit`; a heartbeat `busy`; background work after the turn (a subagent, a shell) keeps it |
| **Waiting for you** | the turn ended; the user is wanted | `Stop`; a terminal transcript tail quiet ≥ 2 s; held here instead of Idle for a pinned session |
| **Needs approval** | a dialog is open (a question, a permission prompt, a plan) | `Notification(permission)` (~6 s after the dialog opens), a blocked tail, a heartbeat `waiting` |
| **Error** | an API error ended the turn, or a turn was cut off and no process can be running it | the transcript's `isApiErrorMessage`; a mid-turn restore |
| **Done** | claude exited in its tab (the tab may hold the shell that hosted it) | `SessionEnd` |
| **Idle** | at rest and of no interest | Waiting for you decays to Idle after a timeout (default **60 min**) ONCE READ (an unread session never decays); a user's close; a restore's normalization |

### 4.5 Interactive dialogs

- `AskUserQuestion` (1–4 questions, 2–4 options each, `multiSelect`, an option MAY carry a `preview` — a Markdown mockup;
  previews only on single-select questions). Its PC answer and the hook's answer formats: §12.1.
- `/add-dir <path>` opens "Add directory to workspace" for a readable-but-new directory; answered by a digit (no Enter);
  Esc = No (§12.2).
- A permission prompt and plan approval: shown at the PC; the state reads Needs approval.

### 4.6 Text into the TUI

- `\r` submits; a lone `\n` is a literal line break in the box.
- Claude Code (2.1.277+) **cancels an Enter that finds an "invisible" character** (zero-width, bidi controls, tags, variation
  selectors, C0/C1, soft hyphen…) and leaves the cleaned text in the box. Every text the host types MUST be sanitized with
  Claude Code's own rule; every prompt comparison MUST go through one normalization (NFC + the same rule).
- A text submitted while a turn runs is held and taken in by the running turn at its next step (a `queued_command`
  attachment), or starts the next turn if the running one ends first. `/steer` relies on exactly this (§11.4).
- Two quick Escs on an empty box open Claude Code's Rewind menu.

### 4.7 Headless commands

- `claude auth status --text`: the account rows of `/status`, ~0.5 s, starts no session.
- `claude -p /usage --output-format json` (with no tools, no MCP, no settings): the plan's usage limits — a **local command**
  on ≥ **2.1.310** (`num_turns` 0, `total_cost_usd` 0, `local_command: "usage"`), ~6 s. An older claude may send "/usage"
  to the model (a paid call).

---

## 5. Architecture

### 5.1 Components

| Component | Thread(s) | Job |
|---|---|---|
| **Poll loop** | poll thread | `getUpdates` long poll → the sender check (I1) → the router (§11.1). Never calls `getUpdates` before this run's `getMe` answered (the identity gate, §6.3). |
| **Outbox** | send thread | Every outbound call except a few tick-thread topic calls: priorities, order, keys, pacing, flood parking, tap answers (§21). |
| **Tick** | tick thread, every **1.5 s** or when the registry notifies | The topic sync (§8), the transcript mirror (§9), the cards (§10), the name / icon tick (§8.5), the directory (§16), the history replay (§9.10), the wizards (§15), the question and menu cards (§12), steer watches (§11.4), voice lanes (§19), the state save (§22). |
| **Workers** | detached threads, each bounded | Screen pictures, card pictures, conversation pictures, `/status` probes, folder probes, the window census, the voice thread. Each holds the service weakly and contains every exception. |
| **Ask bridge** | listener + one worker per waiting hook | The duplex pipe `\\.\pipe\{app}.<pid>.ask` that the `AskUserQuestion` hook helper connects to (§12.1). |
| **Search agent** (optional) | its own process / thread | An LLM agent with session-search tools (§14). |
| **File-link server** (optional) | listener + connection threads | Serves linked files over HTTP (§20). |
| **Voice engine** (optional) | the voice thread + a worker process | Transcribes voice notes (§19). |

### 5.2 The host contract

The bridge needs these capabilities. An implementation that embeds the bridge in the host calls them directly; one that runs
beside the host needs an RPC for each. "Window-hosted" means the call MUST run on the thread that owns that tab's terminal,
in the one window that hosts the tab, and answer with a word.

**Reads**

| Need | Shape |
|---|---|
| Live sessions | For each live session: id, title, tab token, hosting window, agent kind (Claude / other), state and its stamps (turn start / turn end, last activity, a creation stamp `createdUnixMs` set once at launch), `pinned`, `started` (its claude runs in the tab), `external` (adopted), working folder (and an inferred one), tab color (hex), tags, git branch, model (current and launch), effort, context tokens, runner mode, the queue (each prompt: id, text, status Pending / Sent / Failed / Typed, origin, `remoteSource`, `remoteQueuedBusyUnixMs`, echoed / injected stamps), the unsent draft at the PC, the error message, `continuedFromId` (a `/clear` or `/handover` predecessor), `forkParentId`. A lean form without queues for hot paths. |
| Every session ever held | For the folder lists (§15.2): each record's launch folder, inferred folder and newest activity, archived ones included. |
| Change notifications | A callback on every registry change (the tick wakes on it). |
| Windows | The census: windows in creation order, each with its number, its user-given name, its tabs (session ids, titles) and which was activated last. Bounded (the reference waits ≤ 4 s for every window's UI thread, on a worker). |
| Start-up facts | How many windows are still restoring their tabs; which sessions a window HOLDS (saved tabs its restore could not make). |
| The claude executable | Its full path (never a PATH shim) and file version. |
| The launch menu | The configured models and presets (as the PC's "new session / fork" menus list them), the last model typed into a "specify a model" prompt. |
| Colors and tags | Each tag's display color; each folder's color; the tab-color mode (per folder / per session / by inferred folder / none). |
| Transcript paths | A session id → its transcript (including a never-messaged fork → its source's transcript, for display). |
| Version and run facts | A version word ("1.2.3.4", "dev 0.0.1.29", "portable 0.7.9.0", "" for a local build) and the "last window closed" moment. |

**Writes (registry)**

Set / clear `pinned`; wake (Idle → Waiting for you); move to Idle; mark unread; rename; favorite; tags; runner mode;
the global pause; insert a prompt at an index (with `remoteSource`, the question-guard pass, `remoteQueuedBusyUnixMs`);
remove a queued prompt; mark Sent; **submit a prompt through the ONE send seam**.

**The one send seam** (`SubmitPrompt`) MUST provide: a per-session delivery gate (one owner of the input box at a time, with
an expiry — 75 s reference); a draft swap (an unsent draft in the box is stashed, the prompt typed and submitted, the draft
restored, each step verified by reading the box); sanitized, verified placement (§4.6); an Enter rescue (a lone Enter
re-pressed up to 3 times when the turn did not start, never into a menu, a foreign draft or a parked tab); a lost-send
verdict (a Sent prompt with no echo after 15 s at rest is Failed and the runner paused, never resent); never typing into a
parked tab.

**Window-hosted session actions** (each answers a word; an empty or unknown word reads as a failure, never a success)

| Action | Does | Answers |
|---|---|---|
| `start` | starts a parked tab's claude in place, without focus | — (the card's next change shows it) |
| `restart` | restarts the tab's claude in the same conversation; carries the interrupted-turn choice (§12.3) | — |
| `deactivate` | parks the tab (claude stops gracefully; the tab keeps place, title, color, tags, queue, runner mode); arg `cut-off-turn` allows cutting a running turn | `parked` · `already-parked` · `adopted` · `turn-running` · `busy` · `closed` · `failed` |
| `press-esc` | ONE Esc into the session's own injector, only when no delivery or draft swap holds the box | `pressed` · `not-running` · `not-claude` · `busy` · `failed` |
| `screen-text` | the visible terminal text | the text, or "" |
| `screen-snapshot` | the visible screen as styled cells (§17.1) | the snapshot, or "" (dormant tab, window not up) |
| `menu-screen` | the terminal's last 40 rows | the rows |
| `menu-answer` | re-reads the screen and types one digit ONLY when the expected dialog is still open, shows the expected path and nothing holds the box | `pressed` · `gone` · `other` · `busy` · `failed` |
| `rename`, `mark-unread`, `repaint` | title / unread / tag & favorite repaint | — |

A window that does not host the tab answers nothing; when no window answers at all, the bridge MUST say so (§13.6).

**Launch** (answered exactly once)

`RequestLaunch(request, reply)`: kinds NewSession (folder, chosen session id, optional model or preset, optional window,
optional anchor session to open right after), Resume (session id; background tab), Fork (session id, model, fork-turn
choice). The host tries the most recently activated window first, then the others (or ONLY the named window), opens a
background tab, and answers once (Appendix E): `Opened` / `AlreadyOpen` (successes) or `WindowClosed` / `ClaudeMissing` /
`PresetRefused` / `NoFolder` / `Failed`, with the session id the tab hosts, `fresh` (a NEW conversation in place of the one
asked: no transcript to continue), the PC's own words for a refusal, and the window. A launch the PHONE asked for MUST NOT
open a dialog on the PC: the refusal goes back to the phone. When the last copy of the reply dies unanswered (a closing
window dropped the work), it answers `WindowClosed`.

### 5.3 Threads, locks and containment

- One service lock guards the state; **leaf locks** (never held while taking another) guard: tap answers, launch answers,
  the voice lanes, the link-bot username, the save. The save lock is always taken before the service lock.
- A reply MUST NOT be sent while holding the service lock if sending takes it again (a non-recursive mutex deadlocks).
- The tick works on COPIES of a topic record and its mirror and writes back only the fields the mirror owns, compared
  first (§22.2); anything a send callback writes meanwhile lives in shared objects (a prompt reference, the card state) so
  the write-back cannot lose it.
- Every thread body and callback is wrapped: an exception escaping a thread ends the process. Bodies are capped before
  parsing (a `getUpdates` answer ≤ 2 MB, JSON nesting bounded, strings capped).
- Calls that could block on the network or a slow disk (folder probes on network shares, census, drawing, child processes)
  run on their own threads with a bounded wait (§15.3).

---

## 6. Setup, identity, settings

### 6.1 Setting up a bot (about five minutes)

1. @BotFather → `/newbot` → copy the token. **One bot per installation** (a second installation, e.g. a development build,
   gets its own bot).
2. The BotFather **Mini App** → My bots → the bot → Bot Settings → Threads Settings → **Threaded Mode** on; "users may
   create topics" on (a new thread then starts a new session, §15).
3. In the bridge's settings: paste the token, enter the owner's **numeric Telegram user id**, **Check bot** (`getMe`: the
   username; whether topics are on), Save.
4. The first connect creates the 🔎 Search topic with its intro, sets the "/" menu, posts the start note, and — after the
   start-up hold — creates one topic per qualifying session, then pins the directory in the main view.

The owner check is the numeric id, not a pairing code: every other sender is dropped after reading only the sender (I1).

### 6.2 Secrets

- The bot token, the secretary bot's token, the voice endpoint's API key and the file-link secret live in the OS credential
  store (Windows Credential Manager in the reference), one entry per installation — e.g. `{app}:messaging:telegram:<install>`,
  `{app}:messaging:telegram-secretary:<install>`, `{app}:messaging:telegram-filelinks:<install>`, `{app}:voice:api-key:<install>`.
  Per installation on purpose: two installations MUST never share a bot.
- Never in a settings file, a state file, a log line, an error text or a commit. The settings UI saves the token to the store
  FIRST, then applies the settings.
- The token is read once when the bot starts; a changed token restarts the bridge.

### 6.3 The identity gate

- `state.json` records the bot's id (`botId`, from `getMe`). At every start, `getMe` is compared with it; a DIFFERENT bot
  resets everything that is per bot: the update offset, the Search topic, the topic map, the mirrors and reactions, the
  business connection, the directory message, the new-thread wizards (logged `the bot changed (@old -> @new): … reset`).
  An older file without the key (`botId` 0) adopts the first bot seen, never resets.
- The poll loop MUST NOT call `getUpdates` before this run's `getMe` answered: the reset always precedes the first poll (an
  old offset would confirm, i.e. drop, the new bot's updates for good).
- The bot's own id from `getMe` is also how its own service messages are recognized (§3.1).

### 6.4 Settings

Every default reproduces the behavior of a settings file that lacks the field.

| Setting | Default | Meaning |
|---|---|---|
| `messagingEnabled` | off | Run the bot. Also wires the question hook into the claudes that START afterwards (§12.1). |
| `messagingTelegramOwnerId` | — | The owner's numeric user id. |
| `messagingIdleGraceMinutes` | 10 (0–1440) | How long an Idle session keeps its topic (0 = deleted at once). |
| `messagingSearchThinking` | high | The Search agent's thinking level (low · medium · high · xhigh · max). |
| `messagingRichMessages` | on | Claude's Markdown as rich messages, title chips, the card's picture. Off = classic HTML everywhere (for apps too old for rich messages). |
| `messagingVerbose` | off | The default topic mode: verbose (the work too) or the conversation only (§9.1). |
| `messagingCardScreenshots` | on | The card carries the terminal's screenshot (needs rich messages). |
| `messagingAllScreenLevel` | full | The default level of 📜 All Screen (minimal · compact · default · full · max; an unknown word reads full). |
| `messagingFileLinksMode` | off | `off` · `lan` · `hosted` (§20). |
| `messagingFileLinkStation` / `Port` / `LanBase` / `HostedBase` / `Exclude` | per install | §20.4. |
| `messagingVoiceEnabled` | on | Read voice notes (§19). |
| `voiceEngine` | worker | `worker` · `openai` · `off`. |
| `voiceWorkerCommand` | "" | The voice worker's command line. |
| `voiceEndpoint` / `voiceModel` | "" | An OpenAI-compatible transcription endpoint and model. |
| `voiceLanguage` | en | "" = detect. |
| `voiceUseSessionContext` | on | The session's folder and CLAUDE.md join the vocabulary (a Search note's: the fleet's folders and tab titles, §19.3). |
| `voiceDictionary` | "" | The owner's own spellings (§19.4). |
| `voiceWorkerIdleMinutes` | 10 | The worker ends after this long without a note. |
| `voiceEchoTranscript` | on | Post the transcript under the note. |
| `voiceCorrectionModel` | claude-sonnet-5-5 | The correction's model; `off` / "" = none. |
| Secretary bot token | — | Optional second bot (§9.5). |

`Apply` re-reads the settings live (the idle grace, the thinking level — applied to the running Search conversation —, the
verbose default, the rich switch, the All Screen level, file links, voice). The settings UI shows a **status line**: off ·
connecting · connected as @bot (topics enabled / topics OFF: where the switch is) · token refused · polled elsewhere (409) ·
the secretary's state (§9.5).

---

## 7. The chat's layout

| Place | What lands there |
|---|---|
| **Main view ("All")** | The pinned directory (§16); the idle notices (§8.8); the line a cancelled new thread leaves (§15.5); the answers to `/start`, `/help`, `/status` typed there. Any other text typed there opens a new thread by Telegram's own rule (§15); where it does not (users may not create topics), the answer is "This is {app}'s bot. Each working session has its own topic; the 🔎 Search topic finds old ones (/search &lt;words&gt; asks it from anywhere, /search_tab puts it first in the list); a new thread starts a new session (you pick its folder). Send /help for the commands." |
| **🔎 Search** | Created by the bot at its first connect with topics on, with an intro; the user pins it at the top. Its icon is Telegram's own 🔎 topic icon (custom emoji id `5309965701241379366`) and its name the bare `Search`: the magnifier is not repeated beside its icon. It keeps the blue creation color, which shows only while it has no icon. While Telegram refuses that icon (in one run), it is `🔎 Search` without one. A Search topic made before the icon got it is given it once, by one rename. The record names the thread that has the icon, so a Search topic made again gets it too. The start note of every launch (§23.2), the recreation and replay notes, every search request and answer (§14). |
| **A session's topic** | The session's conversation (§9), its sticky card (§10), its question and menu cards (§12), the answers to its commands. |
| **A thread the user opened** | Its new-session wizard until the session is bound to it (§15), then it IS that session's topic. |
| **A recreation's old topic** | Told it is about to be deleted ("This topic is from before the recreation and is deleted in a moment …"); nothing else runs there. |

---

## 8. Topics are sessions

### 8.1 Which sessions have a topic

A session **qualifies** when all hold:

- It is **live** in a tab of any window, and the agent is **Claude Code** (other agents' tabs, shell tabs and sessions in no
  tab get no topic). A hand-started claude that has no conversation id yet (never prompted) has none either.
- Its state is not **Idle** — Running, Waiting for you, Needs approval, Error and Done all qualify — **or** it is Idle and
  was used within the idle grace: the grace is counted from the LATER of "the topic first saw it Idle" and the session's
  last activity. The launch stamps a new session's and a fork's last activity with its creation instant, so **a new tab or
  a fork gets its topic at once**, before its first message. A window-restored Idle tab keeps its old activity and gets no
  topic (no churn at a start).
- A **pinned** session is never Idle (§8.12), so it always qualifies.
- A session a window **holds** (its saved tab could not be restored this start: claude not found, a preset that cannot
  start, a pane failure) keeps its topic for as long as the window holds it.

The topic's name is the session's title (one title everywhere: the tab, the registry, the topic); its creation color the
nearest of Telegram's six to the tab's color; its icon the state (§8.5).

### 8.2 The sync

Each tick builds the facts of every live session (one registry snapshot, one color-map snapshot) and plans, purely
(`PlanTopicSync`):

| Op | When |
|---|---|
| **Create** | a qualifying session has no topic. Creates go in the REVERSE of the list's order (lowest-ranked first, §8.9). |
| **Rename** | the title (as typed) differs from the topic's recorded title. |
| **IdleCard** | an Idle session within its grace: the card gets "💤 leaves the list at HH:MM (/pin keeps it, /wake brings it back)". |
| **Delete** | reason `closed` (the session is no longer live), `idle` (its grace ran out), `not a claude tab`, `left its tab`. |
| **Rekey** | §8.4. |

At most **6** topic operations per tick (`createForumTopic` / `deleteForumTopic` / `editForumTopic` are made on the tick thread
but take the outbox's per-chat slot, §21.2, and skip a parked method). A delete refused by flood control keeps its record
(retried later); a topic's delete also drops every outbox item still queued for its thread.

### 8.3 A topic's life

| On the PC | In Telegram |
|---|---|
| A new tab, a fork, a revive, a wake, a session leaving Idle | The topic is created: its card first, then its history replay (§9.10), then the live mirror. |
| The session becomes Idle (the Waiting-for-you timeout once read; Move to Idle; a restore's normalization) | The card turns to the Idle line with its clock; after the grace the topic is **deleted with its messages**, and the main view gets the idle notice (§8.8). |
| It leaves Idle within the grace | It stays; the card goes back to normal. |
| Renamed (anywhere: the tab, `/rename`, the thread's own name) | The topic is renamed, then its change line (§8.7). |
| Its tab's color changes | The topic's NAME square follows (the creation color cannot). |
| Moved to another window | Nothing. |
| `/clear` or `/resume` in the tab | The topic follows the tab (§8.4). |
| Restarted / deactivated / activated | The same topic; the card shows it; a deactivated tab keeps its state, so it stays listed unless Idle. |
| Forked, or a new session in the folder | A new tab → a new topic (at once). |
| Closed by the user | The topic is deleted at once, as `closed` — never through the Idle card or the idle notice. A user's close of a session that was Waiting for you lands it in Idle in the same registry update, so a later resume does not bring the topic back before it is used. |
| The host quits and starts again | Topics stay in Telegram; the start-up hold (§8.11) keeps them while the windows restore their tabs; then the sync keeps the qualifying ones and deletes the rest. |

### 8.4 A topic follows its tab (Rekey)

The topic belongs to the TAB as much as to the session. Each sync keeps the record's tab token current (a restart or a park
gives the tab a new one). When a topic's session CLOSES and

- another live Claude session **without** a topic holds the same tab (tokens compared case-insensitively) — a `/clear`, a
  `/resume` in that tab — **or**
- another live session **continues** the closed one (`continuedFromId`: a hand-over successor that took the tab on a new
  connection),

the plan emits **Rekey** instead of Delete: the record moves to the new session with a fresh mirror (seeded from the new
transcript, its history replayed after the marker), the thread and the card stay, one silent line marks the boundary —
`— a new conversation in this tab (<id8>) —` — the topic is renamed when the title differs (with its change line), the card
is refreshed, and the old conversation's prompt status is removed. While a topic's session still holds the tab, a second
session in that tab gets no topic of its own. A topic with no recorded tab is never carried over.

### 8.5 The name and the icon

- **Name** = the color square, a space, the title: `🟦 drag crash fix`. Without a color (the host's "no colors" mode)
  the title alone. The title is printed on ONE line (§9.8), cut at 120 units with "…" (never inside a surrogate pair), the
  whole name within Telegram's 128.
- **Icon** = the state, as `icon_custom_emoji_id`:

  | State | Icon | Custom emoji id |
  |---|---|---|
  | Running | ✍️ | 5238156910363950406 |
  | Waiting for you | 🔥 | 5312241539987020022 |
  | Needs approval | ❓ | 5377316857231450742 |
  | Error | ‼️ | 5377498341074542641 |
  | Done (claude exited) | 🏁 | 5408906741125490282 |
  | Idle | ☕️ | 5350392020785437399 |

  - None of them is a mark of the prompt line's status (✅ answered, ⏹ ended, §9.4), so Done never reads as "answered". None is
    the Search topic's 🔎.
  - The first table had Running ⚡️ `5312016608254762256` and Waiting for you 💬 `5417915203100613993`.
- **Which icon a topic shows** is recorded with its form (`stateIconId`: the id the last create or rename set; none in the
  glyph form).
  - A topic whose icon is not its state's current one is re-iconed QUIETLY. It is the same one rename, at the rank of a quiet
    re-square, once its state has held.
  - So a changed table reaches the topics whose state does not change, and costs a call only for the states whose icon
    changed.
  - A record saved before the key existed reads as the first table's id for its state.
  - The key is volatile across versions: an older version re-icons without it.
- **Fallback**: when Telegram refuses an icon (a 400 whose description names the icon, the emoji or the document), the
  icons go off for the rest of the run: every name carries a state GLYPH first — 🔵 Running · 🟡 Waiting · 🟠 Needs
  approval · 🔴 Error · 🟢 Done · ⚪ Idle — (`🟡 🟦 title`), a state icon a topic still shows is removed
  (`icon_custom_emoji_id` ""), and a create is made again in the glyph form. Each start tries icons again (one refused call
  per run at most). The record keeps which form a topic shows (`stateIcon`), and a topic in the other form is re-formed
  quietly, once its state has held.
- **When the name follows the state** (the NAME TICK, every tick): at most **one** rename per tick; only a state that has
  held **8 s** (a short turn's Running ↔ Waiting flicker renames nothing); at least **30 s** after the topic's last rename;
  urgency order — Needs approval, Error, Waiting for you, then the rest; then real color changes; then quiet re-squares,
  re-forms and re-icons; only when the call window leaves room for its **two** calls (the edit and the delete of Telegram's notice:
  calls + 2 ≤ 18 in the last minute); never during the start-up hold or a recreation. While a due rename waits for room,
  the history replay yields to it for up to **90 s**. A rename refused by flood control, or one that got no answer, stays
  due; any other refusal is recorded (a name Telegram will not take must not be retried every tick).
- A title change renames through the sync at once (the name tick is for state and color), with the current icon.

### 8.6 Color

- **The square** is chosen by HUE (HSL), not by RGB distance (a pastel palette's reds and corals came out brown by
  distance): very dark ⬛, very light ⬜, unsaturated ⬛ / ⬜ by lightness, a dark warm hue 🟫, then red [330°, 12°) 🟥 ·
  orange [12°, 35°) 🟧 · yellow [35°, 70°) 🟨 · green [70°, 165°) 🟩 · blue [165°, 255°) 🟦 (cyan joins blue) · purple 🟪.
  Two folders can share a square: it marks a color FAMILY.
- **The chip's button style** follows the square: 🟦 `primary`, 🟩 `success`, 🟥 `danger`; 🟧 🟨 🟪 🟫 ⬛ ⬜ the plain button
  (an orange tab with a red chip would read as a red tab). The square stays in the label, so the family is always named.
- **The creation color** (`icon_color`) is the nearest of the six to the tab's color; it never changes.
- **Following a change**: the color is resolved exactly as the host paints the tab (its color mode, a git worktree keyed to
  its main repository, an inferred folder). A changed color is applied once it has held **8 s** (an inferred folder that
  flips renames nothing). A switch of the host's color MODE re-squares every topic QUIETLY, and for **60 s** after it (and for
  a topic younger than 60 s: a new tab's color is dealt at its first paint) no change line tells a color change. The first
  name of a session bound to a user's thread takes its color fresh from the record (a sync snapshot taken before the tab's
  first paint holds a preview color).

### 8.7 The change line

After a rename Telegram took (or answered `TOPIC_NOT_MODIFIED`), the topic gets ONE silent italic line, in the user's words:

- `“🟦 old” changed the title to “🟩 new”` (a color that changed at the same time shows in the two squares),
- `“🟦 tab” changed the color to 🟩`,
- `The title is now “🟦 new”` (no old title known).

In a rich line the quotes give way to title chips (`[🟦 old] changed the title to [🟩 new]`). Titles are printed on one line
and cut at 200; the line tells a title change only when the two PRINTED titles differ. No line for a state-only rename, a
quiet re-square or re-form, a refused rename. The line does NOT move the sticky card (one call; the next real post moves
it). Telegram's own `forum_topic_edited` notice is deleted when it arrives.

While the topic's **history replay** runs, the line is HELD on the record (persisted): its starting title and square are
kept, later renames move only its end, and once nothing of the replay is left ONE line is posted from the first title to the
last (renames that undo each other post nothing; a quiet re-square during the hold is not told as a recolor; a real recolor
held first is still told).

`/rename <title>` posts no reply of its own: the change line is its answer. A `/rename` to the title the session has already
(as typed, or as printed on one line) changes nothing and is the one case with a reply: "The title is already 🟧 x."

### 8.8 Idle: the card, the delete, the notice

- An Idle session within its grace shows on its card: `💤 leaves the list at 14:32 (/pin keeps it, /wake brings it back)`.
  The clock moves only when the session is used (the plan and the card share one computation).
- Past the grace the topic is deleted (reason `idle`). Each topic deleted as idle (and only when THIS call erased the
  record) joins a pending list with the title and square its name showed.
- **The idle notice**: right after the sync, once no topic has left for **10 s** or the oldest has waited **60 s**, ONE
  silent line goes to the MAIN VIEW: one session — `💤 🟧 Small telegram fixes is idle — its topic left the list. /idle in
  🔎 Search wakes it.`; several — `💤 3 sessions are idle — their topics left the list. /idle in 🔎 Search wakes them.` and
  one title per line in leaving order, at most **20**, then `… and N more`. Titles are chips with no link (their topics are
  gone); an untitled session shows its short id. A burst after a start (every grace that ran out while the host was down)
  is one line, not one per session. In memory only: a quit within the wait loses that one line.
- A closed session's topic posts no such line.

### 8.9 Order: MOST RECENT

- Telegram lists topics by their last message. The bridge makes the list follow ONE key, shared with the host's "most
  recent" sort: the newer of the session's last turn start and last turn end, else its last activity (a new session's
  creation); ties by title, case-insensitively (upper-casing).
- **Creation**: the creates of one plan go lowest-ranked first, so the highest-ranked is made — and its card posted — last,
  and lands on top.
- **Posting**: the tick works through the topics lowest-ranked first, so one pass's posts leave the highest-ranked on top.
- **What is not exact**: a session moves in the key only at a turn's start and end — exactly the posts that move its topic
  up — but mid-turn remarks, work-log messages and the card's rings are new messages too, so a running topic rises while it
  works and falls back in line at its turn's end.
- **The re-sort**: after a recreation's replay (which leaves the list in replay order), the shortest set of card re-posts
  that restores MOST RECENT order (the top part of the desired order, re-posted lowest-ranked first, the top one last) is
  released ONE per tick, only while the call window has room for its two calls (delete + send). Afterwards, at most once a
  minute and only when everything is quiet (nothing queued, no replay, no session Running, nothing posted for 30 s), a list
  that drifted is fixed the same way when that takes at most **24** re-posts; a longer fix is logged once and left to
  `/recreate`.
- A topic put first by a **front button** (§16.4) is left OUT of the re-sort while its key is unchanged (else the re-sort
  would undo the move a minute later); its next turn ends the hold.

### 8.10 Recreation

- The layout of the session topics has a version (`topicLayout`). When the bridge's layout version is newer than the
  state's, the session topics are made again ONCE, after the start-up hold: every session topic's thread moves to a
  PERSISTED orphan list, the topic map / mirrors / cards / queued work for them are cleared, the state is saved at once, a
  silent note goes to Search; the sync deletes the orphans (paced, ~6 per tick; a refusal is logged and dropped) BEFORE it
  makes any topic, then builds the list from nothing in MOST RECENT order, each topic with its whole history again. The
  Search topic stays. A stop midway finishes the deletes at the next start.
- `/recreate` runs the same later. It always asks first (single-use buttons, 1 h): in the Search topic "♻ Recreate ·
  verbose" / "♻ Recreate · conversation" / Cancel (the choice becomes the default mode for all topics); in a session's topic
  also "This topic · verbose / conversation" (that topic is deleted and made again at once with its whole history in that
  mode; one per tick; the outcome is the button's toast — the topic it sat in is gone).

### 8.11 The start-up hold and the window hold

- The registry loads every session closed and each window flips its own tabs live as it restores them, one by one, possibly
  for minutes (measured: 56 tabs in 69 s; 667 tabs over 229 s). A naive first sync would delete every topic as "closed" and
  re-create them a minute later, their history gone.
- Hence: a `closed` delete of a topic whose session has not been seen live in this run is skipped while the start is younger
  than **120 s** OR any window is still restoring, up to a cap of **30 min**. A session seen live and then closed loses its
  topic at once; one still closed when the hold ends loses it then. Other reasons are not held. Logged once per start
  (`start-up hold: N topic(s) kept until their sessions re-home (W window(s) still restoring)`) and at its end
  (`start-up hold ended S s after the start (every window restored | the cap ran out)`).
- **The window hold**: a session whose saved tab a window could NOT restore (claude not found, a preset key missing, a pane
  failure) reads closed but is HELD by that window; its topic is kept with no time limit, until the tab exists again or the
  window is closed. (Without it, one start without claude.exe deleted every topic and its whole history.)
- Nothing that needs complete facts runs during the hold: the name tick, the directory, the history replay's start order,
  wizard timeouts.

### 8.12 Pin: never idle

- A durable per-session flag. A pinned session never decays to Idle and, wherever a change would land it in Idle (a restore's
  normalization, a resume, Move to Idle), it is held in **Waiting for you** instead. So a pinned session's topic never leaves.
  (A session that ends — Done — is not held.)
- Set by `/pin` / `/unpin` in its topic and the card's Session page; shown as 📌 at the end of the card's state line.
  Unpinning lets the normal decay resume from that moment. A fork is never pinned by the bridge.

### 8.13 What the user does to topics in Telegram

- **Renames a session's topic** → the session is renamed: the name without its decoration (an optional state glyph and an
  optional color square are stripped) goes through the host's rename like `/rename`; the sync then renames the topic back to
  its decorated form, with its change line. A name equal to the session's one-line title is ignored (saving the bridge's own
  name back must not flatten a two-line title).
- **Deletes a session's topic** → see §27 (the reference does not re-create it).
- **Deletes the Search topic** → the start note's own send sees "message thread not found", forgets the topic, and the next
  tick creates a new one (its intro first), where the note then goes.
- **Changes a topic's icon** → replaced at the session's next state change.

---

## 9. The mirror: the session's conversation in its topic

### 9.1 Two modes, per topic

| Mode | Shows |
|---|---|
| **Conversation only** (the default) | The user's posts — each prompt, a message typed while the turn ran, an answer to a question, a slash command — and each turn's LAST message, plus the questions and plans that asked the user something, and an interrupt or an API error that ended a turn. User → Claude → user → Claude. |
| **Verbose** | Also each **remark** (Claude's text before a tool call, "why the bash runs") as a message of its own, a **work log** of the tool calls, failed results, compactions. |

- A default for all topics (`messagingVerbose`, off) and a per-topic override (follow the default · verbose · conversation),
  persisted.
- `/verbose` in a session's topic shows the topic's mode with buttons — 🔍 Verbose · 💬 Conversation · ↺ Follow the default
  (verbose|conversation); `/verbose on|off|default` sets it. In the Search topic `/verbose on|off` (or its buttons) sets the
  DEFAULT for all. A change applies to the messages from then on; `/recreate` replays a topic in a mode (§8.10).
- Live, in the conversation-only mode: a tool call DROPS the text gathered before it (a remark is not the turn's last
  message), no work-log line is written, failed results and compactions are skipped, the text before a question is dropped;
  what is left when the turn ends is its final reply.

### 9.2 What a transcript line becomes

The classifier (`ClassifyTranscriptLine`, pure) turns each line into at most one item: **Prompt** (with `midTurn` for a
`queued_command`), **Text** (a remark, or the final reply when `turnEnds`), **ToolCall** (name + a one-line summary),
**ToolResult** (its error flag and first line only), **Question** (an `AskUserQuestion` call), **Plan** (an `ExitPlanMode`
call), **Compaction**, **Interrupt**, **ApiError**, **Command** (a slash command's echo), **Recap** (an `away_summary`),
**Answer** (an `AskUserQuestion` result carrying `answers`). It skips noise, meta lines, sidechain (subagent) lines, the
resume placeholder and — in the LIVE mirror — a fork's `forkedFrom` copies.

A tool call's summary (≤ 120 characters, "Name: summary"):

| Tool | Summary |
|---|---|
| Bash | its `description` FIRST, then the command: `Bash: Run the unit tests ($ npm test -- --watch=false)` (the command is the part that is cut); without a description, the command |
| Read · Edit · Write · MultiEdit · NotebookEdit | the path's last three segments, `…/src/server/handlers.cpp` |
| Grep | the pattern, ` in ` and the path's last two segments |
| Glob · WebFetch · WebSearch · Skill | the pattern · the URL · the query · the skill |
| Task / Agent | the description (else the prompt's first line), `(subagent_type)` after the name |
| TodoWrite | the counts (done of total) |
| anything else | the name |

An **Answer** reads as itself for one question, as `header: answer` lines for several; a question's NOTE follows on its own
line as `📝 <note>`; `(notes only)` reads as `📝 <note>`; Claude Code's `response` (free text instead of answers) reads as
`💬 <text>`; an idle-timeout result is marked as such.

### 9.3 Where a new topic's mirror starts

- No transcript yet (a new session, a fork before its first turn) → its first line.
- At rest → the end of the file's last COMPLETE line (the session from now on; everything before is the history replay).
- Mid-turn (Running, Needs approval) → the start of the running turn's own prompt line within the last 256 KB, so the prompt
  that made the topic appear is in it (a turn longer than that starts at the end).
- A transcript that cannot be read for a moment keeps its cursor (resetting to 0 would replay the whole conversation when it
  comes back). An unresolved transcript path is looked up every **2 s** while the session is busy, every **20 s** at rest.
- The live mirror reads at most 4 MB of one transcript per tick, and stats the file first: it is opened only when it holds
  bytes past the cursor.

### 9.4 How a live turn is laid out

1. **The prompt** — when it did not come from this topic: the user's line (§9.5), or, with the secretary working, the user's
   own words sent as the user (§9.6). Sent at the reply's priority (Final) so a fast turn never posts its reply first, and as
   a REPLY to the previous turn's prompt (the chain, §9.5).
2. **Remarks** (verbose) — each text that precedes a tool call is its own silent message (rich). The work log so far is
   closed; the tools after the remark start a NEW work-log message below it, so a turn reads remark → tools → remark → tools
   → reply.
3. **The work log** (verbose) — one message per segment, created at the first tool call, edited in place: one line per tool
   call (and per failed result, labeled with its tool, and per compaction), at most every **15 s** — the gap grows with the
   number of turns open across the fleet (open turns × 2 s, 15 s at least), so N busy sessions never exceed ~0.5 edit/s
   together —, rolling over into a new message past **3,200** characters.
4. **The final message** — the turn's last text (rich Markdown), or what stopped it: the question (when no question card
   shows it, §12.1), the plan (`📋 **Plan ready for approval**` + the plan), what was said + `■ *interrupted*`, the API error.
   It is the ONLY message of the turn sent WITH sound, and it replies to the turn's prompt message (`reply_parameters`,
   `allow_sending_without_reply`), so every pair reads you → Claude. No extra call: the prompt's message id comes from its
   own send (resolved at send time by a pre-send hook) or from the user's own message.
5. **A message typed at the PC while the turn ran** — a user line marked `mid-turn` (it is not the turn's prompt; it carries
   no status).
6. **A slash command** — a user line `command` over `<code>/x args</code>`.
7. **An answer to a question** — a user line `↩ answer` (unless a question card already shows that answer, §12.1).
8. **A prompt sent from THIS topic** is not mirrored (the user's own message is there) — unless it WAITED in the queue
   (§11.2.7).

A turn that ends in an exit or an Error closes silently: the CARD rings (§10.3), the thread is the conversation.

### 9.5 The user's lines

```
<b>👤 You</b> · 14:02 · typed at the PC · ⏳ running
[🟦 drag crash fix]                         ← the title row: this topic's title as a link / chip
fix the drag crash
```

- **Header**: `👤 You`, the clock, the source — `typed at the PC` · `📱 from the phone` · `⚙ runner` (a prompt the runner
  sent from its own queue) · `📱 from the phone · 📤 sent from the queue` (§11.2.7) · `mid-turn` · `↩ answer` · `command` —
  and, on the turn's prompt only, its status. The source comes from the session's queue (a prompt whose `remoteSource` names a
  message of this topic is the phone's; a runner prompt is the runner's), live and in the history alike.
- **The title row** (second line): the session's title (the short id when blank; one line; 120 units) as a title token that
  links to this topic. In the main view, where every topic's messages run together, it says whose line this is. A line with
  a row goes rich (classic fallback). A send AS THE USER carries the words only: no header, no row.
- **The words**: escaped; an expandable quote past **600** characters or **8** lines; cut at **3,000** (an edit cannot be
  split).
- **The status on the last prompt only**:

  | State | On the bot's line (an edit) | On the user's own message (a reaction) |
  |---|---|---|
  | Running | ⏳ running | 👨‍💻 |
  | Needs approval | 🟠 needs you | 🤔 |
  | at rest, answered | ✅ answered | 👌 |
  | Error | 🔴 error | 💔 |
  | Done | ⏹ ended | 😴 |
  | interrupted by the user | ■ interrupted | 🙈 |

  A new status shows once it has held **3 s** (an Error at once); when the next turn's prompt arrives the previous prompt's
  status is removed (a mid-turn message changes nothing). The prompt's record (message id, whether it is the bot's line,
  time, source, text, status) is persisted so a restart keeps updating it. A phone prompt that FAILED keeps its 👎. Cost: two
  edits or reactions per turn.
- **The chain**: a new turn's prompt line replies to the PREVIOUS turn's prompt (the live reference, else the record's id,
  else — once the topic's history replay is done — the replay's last line), so a tap on the quote jumps turn to turn. A
  user's own message cannot be made a reply; it is quoted when it is the previous turn's prompt.

### 9.6 Secretary mode: the user's turns as the user's own messages

- **Two paths, one code path** (a send "as the user" goes with the client of whichever bot holds the business connection):
  1. **The topics bot itself**, connected by the user under Chat Automation (whether Telegram lets a bot post into its OWN
     chat on the user's behalf is undocumented: the probe finds out).
  2. **A second, secretary bot** (optional): its own token and client and poll loop — `getMe` once (its id, and
     `can_connect_to_business` = Secretary Mode on), then `getUpdates` with `allowed_updates = [business_connection]` ONLY
     (so it never receives the chats it may access), its own persisted offset (reset when that bot changes), one poller per
     token. Setup: @BotFather `/newbot`, Secretary Mode and Bot-to-Bot Communication on; Telegram → Settings → Chat Automation
     → add it, allow it to reply, let it reach the chat with the topics bot. A changed secretary token restarts the bridge.
- **The connection** arrives only as `business_connection` (the connecting user MUST be the owner): its id, `is_enabled`,
  `rights.can_reply`, and which bot holds it. A disconnect of the bot that does NOT hold the stored connection changes
  nothing (moving between the two bots sends a disconnect and a connect in any order).
- **The probe**: after every (re)connection, ONE send as the user into the Search topic (`business_connection_id`; `chat_id` =
  the topics bot's id — from the business account's side its chat with that bot; the topic's thread id). Verdict: works /
  refused (Telegram's reason kept; the refusal text explains the second-bot route) / not tried.
- **When it works**: a prompt typed at the PC, a mid-turn message, a slash command and an answer are sent as the user — the
  live mirror and the history replay alike. A refused send puts the bot's line in its place at once (at a higher priority, so
  the order holds); three refusals in a row turn the mode off.
- **The echo guard**: a message carrying `sender_business_bot` or `business_connection_id` is dropped before anything reads
  it; a text sent as the user into a topic within the last **2 min** (**90 min** for a history send, which can wait long
  behind the live traffic) is consumed ONCE if it comes back as the owner's message. Without it, a prompt typed at the PC and
  mirrored as the user's bubble would return as a new prompt from the phone: a loop.
- **The secretary's own flood control** is separate (its parking is its own, §21.5).

### 9.7 Notifications

- Sent WITH sound: a turn's final message; the card re-posted for an exit, an Error or a Needs approval (§10.3); a question
  card (unless that Needs approval ring already rang); a menu card (§12.2); the Search topic's answer; the start note of an
  app launch (§23.2). Everything else carries `disable_notification`, and edits never notify.
- **One sound per "needs you"**: a question or plan posted with sound silences the Needs-approval card ring that follows; a
  Needs-approval ring silences the question posted after it; an API error message rings unless the Error ring already did;
  an Error is announced ONCE per episode (a flag cleared when the session runs again — else a session at rest in Error rang at
  every start).

### 9.8 Rendering

**Rich (the default).** The final reply, the remarks, a plan, an interrupt's text, and in the history replay the replies, the
narrative and its tool groups go as `sendRichMessage` Markdown. The preparation (pure):

- every `<` outside code becomes `&lt;` (an unknown tag would be dropped with its text); fenced blocks and inline code are
  copied exactly;
- a line that another non-blank line follows gets two trailing spaces (Claude writes for a terminal that keeps line
  breaks), except a TABLE ROW (a line starting with `|`);
- a line that starts with `<details>`, `<details open>` or `</details>`, and a `<summary>…</summary>` right after the opening
  tag, keep their tags (the history's tool groups use them);
- the split: parts of at most **24,000** UTF-16 units and **400** lines (under the 32,768 / 500-block limits, with room for
  what the preparation adds), cut at a blank line outside code; a part that ends inside a fence closes it and the next part
  reopens it with the same opening line; never between the halves of a surrogate pair; a prepared part still over **32,000**
  goes as HTML;
- the bridge's OWN lines inside Markdown (a question, an option, an API error) are escaped to show as typed (formatting
  characters backslash-escaped, `<` → `&lt;`, `&` → `&amp;`, a number at a line's start kept a number `1\.`, every line break
  kept).

A part Telegram refuses (a 400 that names no missing chat or thread; or a 404 = a Bot API without the method, after which no
rich message is tried again in the run) is sent again as HTML **in its own place** in the outbox (§21.3).

**Classic HTML** (the prompt lines without a title row, questions shown as text, API errors, the work log, notices, and
everything when the rich switch is off): Markdown → Telegram HTML — fences with their language as `<pre><code>`, code spans,
`**bold**`, `~~strike~~`, links (only `http(s)://` and `tg://`; a URL's parentheses balanced by CommonMark's rule, an
unbalanced one is no link), headings as bold, `> quotes`, bullets; italics and tables deliberately not converted. Split at
4,096 with `<pre>` kept balanced; a chunk Telegram cannot parse is re-sent as plain text in its own place.

**Titles.** A title is a TOKEN until it is sent: private-use characters (U+E000 … U+E003) frame a form (Plain · Bold · Quoted ·
Square-only), the color square, the escaped title and an optional thread id. The send layer renders it:

| Render | A title whose topic exists | Without a topic |
|---|---|---|
| Rich | a URL chip `<tg-button type="url" style="<the square's style>" url="https://t.me/<bot>/<thread>?topic=<thread>">🟦 title</tg-button>` | a `copy_text` chip (a tap copies the title, ≤ 256) |
| Classic | `🟦 <a href="https://t.me/<bot>/<thread>?topic=<thread>">title</a>` (bold / quoted per form) | `🟦 title`, `🟦 <b>title</b>`, `“🟦 title”` |

- A chip's label is one line cut at 100 units; a chip is lifted out of bold / italic around it (a button never sits inside
  formatting). The bot's username comes from this run's `getMe`; before it answers, titles go without links.
- **Hashes ignore the link and the chip**: every content hash (the card's, the directory's) is taken over the classic
  render WITHOUT the username, so turning links or chips on re-edits nothing in a burst; the FORM is tracked separately
  (§10.5).
- **One line, wherever a title is printed**: every line break — LF, CR LF, and the lone CR some rename boxes write — becomes
  ONE space; spaces and tabs around a break and runs of breaks merge into it; a break at an end leaves nothing; a one-line
  title comes back byte for byte. Never print only a title's first line (a two-line rename would read as no change). The
  host and the registry keep the title as typed; the sync compares titles as typed.

**Times.** Every time is a `tg-time` with the PC's local clock text inside: `HH:MM` today, the date first on another day
("11 Oct 14:02", "11 Oct 2025" for another year), day words in search cards (today / yesterday / 11 Oct).

### 9.9 Forks, `/clear`, `/resume`

- A fork is a new tab → a new topic at once, showing only its own conversation live (the copied `forkedFrom` lines skipped);
  its history replay shows the conversation it was forked from; a never-messaged fork's first card reads its recap from the
  source's transcript.
- `/clear` and `/resume` in a tab → Rekey (§8.4).

### 9.10 The history replay

A new topic carries its session's WHOLE history, then the live mirror.

- **What**: the transcript from its first line up to where the live mirror starts (§9.3), read in history mode — a fork's
  copied lines INCLUDED (a fork's topic shows what it was forked from; a never-messaged fork replays its source). The range is
  persisted on the topic (path, position, end, current day). Not replayed: subagent and teammate transcripts, and the
  sessions a conversation continues (a `/clear` predecessor, a plan-restart parent), which are other sessions.
- **How a turn reads**: a day line when the local day changes (`— Fri 16 Oct 2026 —`); the user's prompt (as the user through
  the secretary when it works, else the bot's user line with its source — a phone prompt replays as `📱 from the phone`);
  the assistant's narrative PACKED into messages of at most **3,500** characters (verbose: each remark followed by the tool
  calls it led to in ONE `<details><summary>🔧 Bash ×3, Edit</summary>` block around a fence longer than any backtick run in
  it; a failed result labeled with its tool; questions, plans, compactions, interrupts, API errors as blocks); a message typed
  mid-turn, a slash command and an answer as the user's posts between; the turn's final reply as its own message. The
  conversation-only mode keeps only the user's posts, the replies, the questions and plans, and an ending interrupt or error.
  Measured: 266 turns of 49 sessions → 1,175 packed messages, against 6,581 in the live layout.
- **The chain**: each replayed prompt line replies to the previous replayed one; the chain's last link is persisted, so an
  interrupted replay goes on with its chain and the first live turn quotes it; a replayed reply quotes its replayed prompt.
- **The reader**: windows of **1 MB** (growing ×4 up to **64 MB** while a window holds no complete line — a pasted image can
  make one line megabytes long; a longer line is skipped), each cut at the start of the LAST turn begun inside it so every
  turn renders whole with its reply. Each TURN is committed (the persisted position moves) as soon as its last message is
  answered, so a stop re-sends at most the turn that was going out.
- **Pacing**: the LOWEST priority; at most one message per **2 s**; only while the last minute holds fewer than **18** calls of
  any kind; paused **5 min** after any 429; it yields to a due rename for up to 90 s (§8.5); ONE batch (a window's messages) in
  the outbox at a time; a batch is "stalled" only when all its messages have left the outbox and none was answered for
  **30 min** — then it is dropped and read again.
- **Order**: the highest-ranked topic in MOST RECENT order with history pending goes first; the replay waits for the start-up
  hold (the order keys come from the re-homed sessions) and for a recreation's new list (≤ 5 min).
- **The card meanwhile**: posted FIRST, above the history, with `⏳ the session's history is being added below; the live
  conversation follows it`; not chased down after every history message (that would double the calls); re-posted ONCE at the
  bottom, without the line, when the history is in (a batch's commit stamps the topic's last-post clock with the commit
  time, so the card knows it is above newer posts).
- **The live mirror** of that topic waits, then continues from exactly where the history ends.
- A topic created by a Rekey replays the resumed conversation after its "a new conversation in this tab" line.
- Measured: 48 topics, 1,600 history messages in 1 h 40 min, no 429; 413 topics ≈ 4,000 messages in ~4 h.

### 9.11 The recap

Claude Code's `away_summary` (written a few minutes after a turn when the user is away) is shown in the CARD only while it is
FRESH — newer than every conversation message of the topic (a prompt, a reply, a tool call, a question, a plan, an API error,
a command, an interrupt, an answer). One fold computes it for the seed (the transcript's last 256 KB when the topic is made)
and for the mirror (every batch). Shown as `📝 Recap · HH:MM` and the text in italics (≤ 1,200 characters).

---

## 10. The sticky card

### 10.1 What it says

The card is ONE rich message per topic, always the topic's last message, edited in place. Its lines, in order:

```
🟡 Waiting for you since 08:21 📌                         ① the state, since when, 📌 when pinned
📌 [🟧 messaging bot answer questions]                    ② the title: a chip that links to this topic
🔄 Restart Session restarts it (the same conversation goes on)   ③ the state's own action (Error / parked / exited)
💤 leaves the list at 14:32 (/pin keeps it, /wake brings it back) ④ an Idle session within its grace
⏳ the session's history is being added below; the live conversation follows it   ⑤ while its replay runs
/source/project-a · main · opus-5-5 · max · 907K          ⑥ folder · branch · model · effort · context
Runner Paused · queue 2 — next: “also update the docs”     ⑦ only when not the default (Auto, nothing queued, no global pause)
✎ unsent at the PC: “TODO get the jsonl of with examples.” ⑧ the draft in the session's input box at the PC
⚠ <the error message>                                      ⑨ in Error
📝 Recap · 08:24  (italic text)                            ⑩ Claude Code's recap while fresh
[screenshot]  🖥 the screen at 08:24:51                     ⑪ the terminal's screen (§17.2)
```

- ① `🔵 Running` · `🟡 Waiting for you` · `🟠 Needs approval` · `🔴 Error` · `✅ Done` · `💤 Idle`, then ` since HH:MM` from
  PERSISTED stamps (the turn's start while it runs, the newer turn boundary at rest, then the last activity — never a stamp
  that every load re-writes), then ` 📌` for a pinned session.
- ③ Error is read FIRST: `🔄 Restart Session restarts it (the same conversation goes on)`, or for a parked tab in Error
  (a crash cut its turn off) `🔄 Restart Session starts it in its tab (a cut-off turn goes on by itself)`; a parked tab
  `💤 not running — a message here or ▶ Reactivate resumes it`; an exited claude `⏹ claude exited — 🔄 Restart Session
  starts it again (the same conversation)`.
- ⑥ the folder's last two segments with a leading `/` and no "…" (a folder of at most two segments whole, drive first:
  `K:/source`), the git branch, the model without a leading `claude-` (`opus-5-5`, `[1m]` kept; a preset id or an alias
  unchanged), the effort, the context size in thousands (`907K`, no label).
- ⑦ `Runner Auto|Paused[ (Off globally)] · queue N[ — next: “<first line, 60>”]`.
- **Clock times, never spans.** A card whose text said "Running 3m" changed every minute; 47 such cards earned a 21-minute
  429. A card changes only when a fact changes (plus once at midnight, when a clock gains its date).
- **Never edited for nothing**: a 32-bit FNV-1a hash of the card's classic text and its buttons is persisted; an unchanged
  hash sends nothing (also across a restart). The line ORDER is part of the hash: a layout change edits every card once at
  the first refresh after an update, so layout changes belong together in as few releases as possible.
- A changed card is edited at most every **20 s** while the state is unchanged; a state change edits at the next pass.

### 10.2 Sticky at the bottom

- Every post into the topic other than the card's own stamps the topic's last-post clock (at enqueue time), and so do the
  user's own messages; the card's post stamps its own clock when Telegram answers.
- Once newer posts have been quiet for **6 s** (a reply and its remarks cost ONE re-post), the old card is deleted and a
  fresh one is sent at the bottom (`deleteMessage` + `sendMessage` / `sendRichMessage`, at the card's priority, so the message
  that triggered them goes first). The card is marked in flight meanwhile; a refused post is retried after **60 s**, never
  every tick.
- The card is NOT pinned (a pin per re-post would leave a "pinned a message" line each time).
- A card the user deleted comes back: an edit answering "message to edit not found" clears the record's card, and the next
  refresh posts one.

### 10.3 The card rings

For an exit (Done), an Error and a Needs approval the card is re-sent at the bottom **with sound** (one ring per episode,
kept until a post can carry it). There is no separate bot message for these states: the card says the state. A card that
starts to ring goes back to its Main page (§10.4), where that state's action is.

### 10.4 Its buttons: pages

Two buttons a row at most (a phone shows every label whole).

| Page | Rows |
|---|---|
| **Main** (every card's default) | `[⚙️ Session]`, plus `[🔄 Restart Session]` for a session in Error or one whose claude exited, or `[▶ Reactivate]` for a parked tab; alone otherwise (running, waiting, idle, needs approval, closed). The cog never moves under the thumb. |
| **Session** | `[⑂ Fork] [🆕 New Session]` · `[🔄 Restart Session]` (with `[🔧 Resume Session]` beside it in Error) · `[⏸ Pause runner]` or `[▶ Runner auto]` `[📋 Queue]` · `[📌 Pin]` or `[Unpin]` `[📄 Transcript]` · `[🖥 Screen Text] [🖼 Screen Image]` · `[📜 All Screen ▸]` · `[⎋ Esc] [🌙 Deactivate]` (while the session's claude runs: Esc only for a Claude session whose claude reads keys — not an exited one; Deactivate only for a started tab the host launched — not an adopted shell; no row for a parked tab or a closed session) · `[◀ Back]` |
| **All Screen** (four variants, §17.3) | `[⬜ File Diffs · the level's default]` / `[☑️ File Diffs · at every level]` · `[⬜ Last Turn · the whole conversation]` / `[☑️ Last Turn · the newest turn only]` · one row each: `[📜 Minimal · prompts & replies]` `[📜 Compact · + call reasons & answers]` `[📜 Default · + every tool call]` `[⭐ Full · + commands & output]` `[📜 Max · + written files]` (⭐ marks the default level) · `[◀ Back]` (to the Session page) |

- Every mark is an EMOJI (⬜ U+2B1C, ☑️ U+2611 U+FE0F, ⭐ U+2B50, ⚙️ U+2699 U+FE0F): a text glyph next to emoji looks out of place
  and some apps draw U+2611 / U+2699 as text without the variation selector.
- The data is `c:<verb>:<session id>` (Appendix B), no token: a verb acts on the session's state at the tap.
- The page lives in memory per card: ⚙️ Session and ◀ Back flip the keyboard of the message they sit on, at the answers'
  priority (Final) so the flip is instant behind a busy outbox; the next edit and re-post keep the page; a copy of the card
  (`/status`, `/card`) changes only that message's keyboard (`editMessageReplyMarkup`, keyed per message); after a restart
  every card shows Main.

What each button does: §13. The All Screen picker: §17.3.

### 10.5 Form upgrades

The card's FORM — rich or classic, its title linked or not — is recorded from Telegram's own answer (`rich_message` or `text`
in the result). A card whose text is unchanged but whose form is not the wanted one (every card posted before chips or links
existed, or after the rich switch flipped) is re-edited only through a slow slot: one card per **10 s** across all topics,
none for 5 min after a 429 or while `editMessageText` is parked, only while the last minute's calls leave room under 18. A
card whose text changes is edited in the wanted form at once (the upgrade rides the ordinary traffic too). A rich form
Telegram refused is not tried again for the same text.

### 10.6 The screenshot

The card carries the terminal's screen as its last block: §17.2.

---

## 11. Inbound: messages, commands, buttons

### 11.1 The router

For every update, in this order:

1. **The sender** (I1). `from.id` is the owner → go on. The bot's OWN id → its service messages: a `forum_topic_edited` it
   caused and its "pinned a message" note for the directory are deleted (three refusals in a row stop those deletes); nothing
   else happens. Anyone else → dropped after reading only the sender (logged without the body; a stranger's tap answered
   "This bot is private.").
2. **Business traffic**: a message carrying `sender_business_bot` or `business_connection_id` → dropped; the secretary echo
   guard (§9.6) consumes a returning text it sent as the user.
3. **`/search-tab` and `/search <words>`**, in ANY thread, before a topic's own handling (which would note the command as the
   topic's newest post and re-post its card): §14.6. An EDIT that turns a message into either does nothing.
4. **A voice note** → its route (§19.2).
5. **Topic service messages from the owner**: `forum_topic_created` in a thread no session owns → a new thread (§15);
   `forum_topic_edited` → a rename of the session (§8.13) or of the coming session (a wizard's thread).
6. **By thread**:
   - the Search topic → §14;
   - a session's topic → §11.2–§11.4 (a thread that has a topic AND a bind still handing over → into the bind, §15.4);
   - thread 0 (the main view) → `/start`, `/help`, `/status`, else the intro line (§7);
   - a recreation's old topic → told it is about to be deleted;
   - any other thread → held for a new session (§15).
7. `callback_query` → §11.5. `business_connection` → §9.6. `edited_message` → §11.2.5.

**Any action in a topic marks the session read** on the PC (its unread ring and count clear), so the Waiting → Idle decay
works for a session handled only from the phone. Merely viewing a topic cannot be seen (§3.1).

### 11.2 A message is a queued prompt

#### 11.2.1 The funnel

Every message that becomes a prompt — a typed message, a backlog [Queue], a message a new thread or a question card held, a
voice note's transcript, `/takedraft`, a forced `/cc` command — goes through ONE function, which:

- refuses a closed session: "This session is closed. Resume it from the 🔎 Search topic.";
- inserts the prompt **next in the queue**: after the earlier remote (phone) prompts, AHEAD of the runner's own planned
  prompts, in the order the user sent them;
- marks it remote (`remoteSource = "telegram:<message id>"`), with the question guard's pass (it IS the user's answer);
- decides, INSIDE the same registry update, whether it WAITS (§11.2.7) and stamps it;
- records the reaction (✍) BEFORE the prompt reaches the queue (a prompt the reaction sync meets already taken, with no
  record, would never get its 👨‍💻);
- starts a parked tab in place (`start`) so the queue can deliver;
- starts the `/add-dir` watch for a `/add-dir <path>` (§12.2).

#### 11.2.2 Delivery

A remote prompt is delivered at the next ready moment **whatever the runner's mode** (Paused included), past the global
pause and the automatic-send budget, and past the question guard. Every safety gate still holds: the tab is open and its
claude started; the session is READY (Waiting for you, Idle with no turn in flight — and, for a remote prompt only, **Error**:
the user's message from the phone IS the retry; the runner's own prompts still stop there); the delivery gate is free; the
input box reads as a plain box (no menu, no dialog); one prompt per turn (the pickup guard); and a SETTLE of **5 s** of steady
ready state since the later of the last state change and the queueing (the runner's own prompts settle 40 s: the settle
protects planned prompts from a momentary "ready"; for the user's own message it only made the phone wait).

| The session is | What happens to the message |
|---|---|
| At rest (Waiting for you, Idle, Error) | queued and sent ~5 s later |
| Running | queued; sent when the turn ends (it WAITED: §11.2.7) |
| Needs approval | queued; sent after the dialog is answered. With a question card open, a plain message is ASKED about instead (§12.1.3). |
| Parked (restored or deactivated) | queued; the tab is started in place without focus; sent once the session is up and settled |
| Done (claude exited) | queued; held — the card offers 🔄 Restart Session / `/reactivate` |
| A delivery in flight | queued |
| From the backlog | asked first (§11.2.6) |

#### 11.2.3 How it gets in

Through the host's ONE send seam (§5.2): typed into the input box with the draft swap (the user's unsent draft at the PC is
kept), sanitized (§4.6), verified, Enter-rescued; marked Sent atomically and persisted BEFORE the injection (a restart never
replays a Sent prompt); a failed injection rolls back to Pending; a Sent prompt with injection evidence and no echo after 15 s
at rest is judged lost (Failed, the runner paused, never resent). A LOCAL command's transcript echo (`/add-dir`, `/model x`)
counts as its delivery echo (§12.2.6).

#### 11.2.4 Its status on the user's message

A reaction, no extra message: **✍** queued → **👨‍💻** picked up (its echo arrived) → **👎** failed, with a reply giving the
reason (lost send, refused, closed). From the turn's start the prompt-status reactions of §9.5 take over (👨‍💻 🤔 👌 💔 😴 🙈).
A message held for something (a new thread, a question card) shows **👀**. A message that waited and went from the queue shows
**🫡** (§11.2.7). After a restart, a prompt that was picked up before this run saw it queued is recorded, not reacted again
(a burst of reactions at every start would also undo the statuses).

#### 11.2.5 Editing and removing

- An edit of a message still QUEUED replaces the queued text (`edited_message`). Once sent: "already sent — send the change as
  a new message". An edit of a HELD message (a new thread's, a question card's) replaces what is held.
- Telegram tells a bot nothing about a deleted message: `/unqueue <n>` (or the ✖ n buttons of `/queue`) removes a queued one.

#### 11.2.6 The backlog rule

Telegram keeps updates for 24 h, so a message written while the bridge was down arrives at the next start. An update older
than **10 min** when it is processed:

- a message for a session is NOT queued silently: a reply to it asks "This message is 3h old (sent while {app} was closed).
  Queue it now?" `[Queue] [Drop]` (single-use tokens, 2 h);
- a read command is answered; an operating command is not run (the bot asks again); a `/clear` in Search is answered, not
  carried out; a `/search` is answered, never run unasked; a `/steer` is asked about like a message;
- a button press is checked against the current state (I5);
- a message HELD by a new thread and a voice note (judged by when it ARRIVED, not when its transcript is ready) are never
  backlog.

#### 11.2.7 A message that waited: "📤 sent from the queue"

- **When it waits**: while the session's turn is in flight (Running, Needs approval), while a delivery is in flight (a prompt
  Sent and not yet echoed), or behind an earlier remote message that waits. The queue entry is stamped with the moment
  (`remoteQueuedBusyUnixMs`, persisted); the scheduler never reads the stamp — the message goes exactly when it would have.
- **When the queue sends it**, the transcript's prompt is matched to the stamped, Sent queue entry, and the bot posts the
  user's line WHERE THE TURN STARTS: `👤 You · 14:03 · 📱 from the phone · 📤 sent from the queue · ⏳ running`, the title row,
  the words — the turn's prompt from then on (its status edits, the reply's quote, the chain). The user's own message gets
  **🫡** (a fallback sets it 2 min after the send if the line never came). The tick's session snapshot is older than the
  transcript read, so a match that still reads Pending is checked once more against the registry (mark-Sent precedes the
  injection): the same words typed at the PC while the message waits are not mistaken for it.
- Taken in MID-TURN by a running turn → the line reads `· mid-turn`, no status.
- A message delivered at a ready moment keeps the plain form (the user's message is the prompt, its status a reaction):
  nothing was posted between it and its turn. Error and Done hold a message but no turn runs there, so it is not stamped;
  `/steer` is not stamped; a history replay shows it as `📱 from the phone` in transcript order.
- In every mode, the secretary included: the line is the BOT's (its header is what says it went from the queue).

### 11.3 Commands

#### 11.3.1 Reading a command

`/word[@botname] args`: the word is read up to whitespace, case-insensitively, a `@botname` suffix dropped; the arguments are
the rest of the first line plus any further lines, trimmed. A lone `/` is text. **`/cc /x …`** forces `/x …` through to Claude
Code even when `x` is one of the bridge's names (the refusal tables still apply); `/cc words` is plain text.

| Kind | Rule | From a topic |
|---|---|---|
| The bridge's own (§11.3.2) | in the bridge's list | run |
| Claude Code's, recorded | anything else (skills, the user's commands, MCP prompts, `/compact`, `/clear`, `/model <id>`, `/effort`, `/fast`, `/plan`, `/goal`, `/init`, `/review`, `/code-review`, `/security-review`, `/simplify`, `/loop`, `/schedule`, `/add-dir <path>`, `/output-style`, `/autocompact`, `/color`, unknown ones) | queued like a prompt and typed into the box |
| Claude Code's, **no record** | `reload-plugins reload-skills btw config status help hooks permissions memory mcp tasks workflows diff rewind release-notes skills theme tui keybindings scroll-speed terminal-setup voice export copy heapdump ide chrome login logout upgrade usage-credits privacy-settings bug feedback install-github-app install-slack-app mobile radio stickers passes powerup artifacts doctor cost context vim statusline agents plugin plugins remote-control resume-list`, and `/model` with no argument (it opens a picker) | refused: "leaves no record in the session — run it at the PC" (I11: reopening the session would show nothing of it) |
| Claude Code's, **leaves the tab** | `resume branch fork background teleport desktop stop exit quit cd` | refused, naming the bridge's equivalent (`/forksession`, `/newsession`) |

The tables are versioned data: a command they do not know (a new built-in, a plugin's) is SENT, since skills and plugins
expand into prompts. They must be re-checked when Claude Code changes its command set.

#### 11.3.2 A session's topic

| Command | Does |
|---|---|
| `/help` · `/start` | the topic's help (Appendix D.1) |
| `/status` | the card copy, then Claude Code's status report (§18); `/card` the copy alone |
| `/queue` | the Pending prompts, numbered (📱 marks the phone's), with `[✖ n]` buttons and `[▶ Send next now]` |
| `/sendnow` | sends the NEXT QUEUED prompt now (the PC's Send now) |
| `/steer <text>` | sends NEW words now, into a running turn too (§11.4) |
| `/enter` | presses ONE Enter at the PC: the prompt already waiting in the input box is submitted — only a prompt is pressed on, never a menu, a dialog or an empty box (§13.8); words after it get the usage line |
| `/unqueue <n>` | removes queued prompt n |
| `/runner auto\|paused` | the session's runner mode |
| `/pin` · `/unpin` | never idle (§8.12) |
| `/wake` · `/moveidle` · `/markunread` | Idle → Waiting for you · → Idle · unread at the PC |
| `/rename <title>` | the tab, the topic and the saved title, through the hosting window; the change line is its answer (§8.7); a bare `/rename` gets the usage line. (Claude Code's own session name, its `custom-title`, is not changed.) |
| `/favorite` · `/unfavorite` · `/tags` · `/tag x` · `/untag x` | the host's favorite and tags |
| `/sessionid` · `/path` · `/gitbranch` · `/draft` | the value as a message with a Copy button (≤ 256; a code block when longer) — `/draft` is the unsent draft at the PC |
| `/summary` | the session's summary |
| `/transcript [minimal\|compact\|default\|full\|max]` | the whole conversation as a `.md` document at that level (§17.3's levels) |
| `/screen` · `/screenimage` | the visible terminal text · the screen as a picture (§17.1) |
| `/allscreen [level] [diffs] [last]` | the whole conversation as tall pictures (§17.3) |
| `/download <path>` | a file's download link and 📤 Send as file (§20) |
| `/add-dir <path>` | Claude Code's, queued and WATCHED: its dialog becomes a menu card (§12.2); a bare `/add-dir` is never sent (usage line) |
| `/activate` · `/reactivate` | start a parked tab · start or restart as needed (§13.1) |
| `/restartsession` | §13.2 (asks while Running) |
| `/interrupt` | one Esc, asked first (§13.3) |
| `/deactivate` | parks the tab (§13.4; asks while a turn is in flight) |
| `/forksession [model\|@preset]` · `/newsession [model\|@preset]` | a fork / a new session in this folder, in a new tab and topic (§13.5) |
| `/takedraft` | queues a COPY of the unsent draft at the PC (the box keeps it) |
| `/verbose [on\|off\|default]` | the topic's mode (§9.1) |
| `/recreate` | asks: this topic or all, verbose or conversation (§8.10) |
| `/search <words>` · `/search_tab` (`/search-tab`) | §14.6 |
| `/idle` · `/board` · `/pauseall` · `/resumeall` | the fleet commands (they work in any topic) |
| `/cc /x …` | force `/x` through to Claude Code |

The Search topic's commands: §14.5. The main view takes `/start`, `/help` and `/status`.

#### 11.3.3 The "/" menu

Set once per start (one `setMyCommands` call, chat scope — so a command a release adds reaches an existing chat):
`queue`, `sendnow`, `steer`, `enter`, `runner`, `pin`, `unpin`, `wake`, `transcript`, `summary`, `screen`, `screenimage`, `allscreen`,
`download`, `status`, `search`, `search_tab`, `rename`, `restartsession`, `deactivate`, `activate`, `reactivate`,
`forksession`, `newsession`, `interrupt`, `idle`, `board`, `recreate`, `pauseall`, `resumeall`, `help` — each with a one-line
description (Appendix D.3). `/clear` is deliberately absent (in a session's topic it is Claude Code's own).

### 11.4 `/steer`: words sent now

- `/steer <message>` types the words into the session's box AT ONCE and submits them, through the one send seam (the delivery
  gate, the draft swap, the verified placement, the Enter rescue) — the PC's Send now with a NEW prompt. In a running turn
  this is a steer by Claude Code's own handling (§4.6): the running turn takes the words in at its next step, or a turn that
  ends first starts the next one with them. At rest a turn starts.
- **When the words are NOT sent at once** (the facts read in this order):

  | The session | `/steer` does |
  |---|---|
  | no words | the usage line |
  | closed | "This session is closed. Resume it from the 🔎 Search topic." — nothing queued |
  | its claude not started (parked) | the tab is started; the words wait FIRST in the queue and go once it is up |
  | claude ended in the tab (Done) | NEVER typed (the tab may hold the shell that hosted claude, where typed words would RUN as a command); the words wait first; `/reactivate` starts it |
  | the last box read is a MENU (a question, a permission prompt, a picker) | never typed (Enter would pick an option); the words wait first and go once it closes |
  | anything else (Running, Waiting, Idle, Error, a stale Needs approval whose box reads plain) | sent now |

  "First" = before every waiting prompt, the topic's and the runner's alike; sent at the next ready moment whatever the
  runner's mode.
- **Its status**: reactions on the `/steer` message — **⚡** while the words are being sent, **👨‍💻** once claude took them,
  **✍** while they wait. No text reply when sent.
- **A declined send** comes back as the first waiting prompt and is told ONCE: at once when the delivery gate is held
  ("↩ Not sent now: another delivery is typing into the session's input box. Your message waits first in the queue and goes
  out when the session is ready."), or by the tick when the hosting window declines later (a menu opened in between, no box,
  the tab turned out dormant, a draft it could not move aside), with the reason read from what the decline left. Each sent
  steer is watched until claude took it, it came back, it failed or **10 min** passed.
- The backlog rule applies (a `/steer` written while the bridge was down is asked about). A `/steer` held by a new thread is
  never backlog. The Search topic refuses it as a session's command; `/cc /steer` forces the word through (Claude Code has no
  `/steer`).
- Known effect: a message submitted mid-turn can make the turn's real `Stop` read Running ~2.5 s longer.

### 11.5 Buttons

#### 11.5.1 Two kinds of button data

- **Stateless verbs** that act on the CURRENT state at the tap (they work again and after a restart): the card's
  `c:<verb>:<sid>`, the Search answer's `p:<n>:<sid>` and `s:clear`, the directory's `d:front:<thread>`, the new-thread
  wizard's `nt:…` (indexes into what the wizard showed), the question card's `q:<key>:…` and the menu card's `tm:<key>:…`
  (random per-card keys), `/download`'s `dl:<offer>`. Grammar: Appendix B.
- **Single-use tokens** `t:<id>` for confirmations and one-off actions: in memory, each bound to a kind, a session and a
  payload, expiring — **1 h** for confirmations (restart / park / interrupt now, recreate, the interrupted-turn and fork-turn
  questions), **2 h** for menus and lists (a pick's actions, `/queue`'s ✖ n, the backlog's Queue / Drop, `/idle`'s Wake,
  `/verbose`). Consumed at the first tap; an expired or unknown token answers "That button expired.".

#### 11.5.2 A tap stays busy until its work is done

Telegram shows a tapped button's progress indicator until `answerCallbackQuery`. The bridge HOLDS the answer while the tap's
work runs:

- The handler runs inside a tap scope; every outbox item queued in it carries a hold on the tap until Telegram has answered
  that call; a newer item that replaces a queued one by its key takes over its holds; a fallback queued from an item's
  callback (a refused rich message re-sent as HTML, a refused photo as a document) holds the tap too.
- Work that finishes elsewhere carries the tap along: a hosting window's reply (screen text, screen picture), a drawing
  worker, a launch's "Opened …" line or refusal, the wizard's census, and "the card's NEXT change" for actions whose only
  completion signal is the card (restart, reactivate, resume, a confirmed park or interrupt, a directory dot's re-post, a page
  flip that met a card post in flight). That card change then goes at once, past the edit gap, at the answers' priority.
- When the tap's LAST hold goes, its answer is queued ahead of every other item of its priority. At the limit — **9 s** after
  the tap (Telegram's deadline is ~10–15 s) — it is answered anyway, with the handler's toast, or "⏳ Still working on it — the
  result shows up here when it is done." while work goes on; the work itself is never cut short.
- A toast set from a hosting window's reply is that reply's word (Esc, Deactivate): the handler then MUST NOT also answer with
  an empty toast (the window's reply would race it and be wiped).
- Logged: `tap <label>: answer queued N ms after the tap (its work is in | the hold's limit; its work goes on)`.

| Button | Its answer goes when |
|---|---|
| ⚙️ Session, ◀ Back | the card's new keyboard is in |
| 🖥 Screen Text · 🖼 Screen Image | the text · the picture (or the line saying why there is none) is in the topic |
| ⑂ Fork · 🆕 New Session | "Opened …" (or the refusal) is in the thread |
| 🔄 Restart · ▶ Reactivate · 🔧 Resume | the card shows the restarted / started session |
| Runner · Pin · Queue · Send next now · Transcript | its reply is in (Pin: and the card's edit; Transcript: the file) |
| Restart now · Park now · Interrupt now | the reply and the card's change |
| Search ⊘ Clear · a pick number | the "cleared" line · the pick's menu |
| A pick's ▶ Resume, ⑂ Fork | the menu's edit, then "Opened …" (or the refusal) |
| A directory dot | its card's re-post (answered at once while the topic's history replays) |
| A wizard button | the picker's edit; the model: the window list; a window: the launch's answer |
| 📜 a level | the last picture (the card flips back to Main at once, which takes the spinner away with the button) |
| Recreate this topic | at once (the topic is deleted a moment later) |

### 11.6 Files, photos, stickers

Not taken: "Files and photos are not taken yet (a later build). Text and voice notes only for now." (a reply to the message).
Voice notes and round video notes are read (§19).

---

## 12. Claude Code's dialogs from Telegram

### 12.1 `AskUserQuestion`: the question card

#### 12.1.1 The way in: a PermissionRequest hook

Claude Code runs a `PermissionRequest` hook BESIDE the open dialog (in the main session); the dialog stays fully usable; the
FIRST answer wins (PC or hook). The hook's `allow` with `updatedInput` (the input plus `answers` and `annotations`) is accepted
for `AskUserQuestion`, and the model then reads exactly what a PC answer gives it. Measured on 2.1.291 in seven scenarios.

- **Wiring**: while the bot is on, the generated hooks settings file (the shared one, and every per-preset one) carries one
  more entry: `"PermissionRequest": [{"matcher": "AskUserQuestion", "hooks": [{"type": "command", "command": <helper host>,
  "args": [...], "timeout": 86400}]}]` — exec form, so only for claude ≥ 2.1.139 (an older claude gets no question hook and the
  show-only behavior). The file is rewritten by every writer (the bridge's start, every spawn, a settings save), so turning the
  bot on reaches the claudes that START afterwards. Without the bot, the file is exactly the file without the entry.
- **The helper** (the reference: a Windows PowerShell 5.1 script in the profile, exec form; a native helper would hold a few
  MB instead of ~93 MB per open question):
  1. reads the payload from the raw stdin stream as UTF-8;
  2. leaves AT ONCE with no output when the payload carries `agent_id` (a subagent's or teammate's dialog WAITS for its hooks
     before it shows at the PC), when the tool is not `AskUserQuestion`, or when the bridge's pipe cannot be found (an env var
     the host sets, else a discovery file in the profile);
  3. sends ONE request line `{"v":1,"hookPid":…,"claudePid":0,"payload":<the payload as claude wrote it>}` over
     `\\.\pipe\{app}.<pid>.ask` FIRST (0.41–0.47 s after its start), then looks its parent claude up and waits for ONE reply
     line (an async read polled every 2 s);
  4. a decision → printed as UTF-8 bytes; `{}`, a broken pipe, its parent's exit or 24 h → prints nothing; every path exits 0
     (fail open: the PC keeps the dialog).
- **The pipe and the registry**: duplex; a listener and one worker per waiting helper; the request line bounded (1 MiB,
  10 s). The ACCEPTOR is the bridge while its bot runs with topics on AND the payload's session is a live Claude session;
  anything else hears `{}` at once. Entry states: Open → Answered → Delivered / Released, or Gone (the helper left). A worker
  waits for its reply, the helper's departure (a one-byte probe read) or the bridge's stop (`{}`).
- ⚠ **Claude Code does NOT end the hook when the PC answers first** (measured: alive 3 s later). The bridge RELEASES it (`{}`)
  as soon as it reads the PC's result, when the session is no longer live, and for every open one at its stop.
- The payload has NO `tool_use_id`: the card pairs with the transcript's `tool_use` by content (the request's question texts =
  the mirror's question item), and a hook's decision is recorded in the transcript as a `hook_permission_decision` attachment.

#### 12.1.2 The card

ONE managed message per dialog in the session's topic, edited in place as the user goes: posted at the answers' priority as a
reply to the turn's prompt, WITH sound unless the Needs-approval card ring already rang. A session without a topic is waited
for 30 s, then released. The card's buttons carry a random 10-character key (`q:<key>:<verb>[:<arg>]`); a button of an earlier
run, or of a card forgotten 30 min after its outcome, answers "This question is no longer open here." and loses its keyboard.
An edit renders the card AGAIN right before its call (one edit per card in the outbox), so it always shows the latest state
however many taps came while it waited.

```
❓ Claude is asking · 4 questions

▶ 1. NDArray<T> — not answered yet
   2. NDMatrix — not answered yet
   3. NDListArray — not answered yet
   4. s_, flatiter — not answered yet

Question 1 of 4
With one indexer per class, what should NDArray<T> keep?

1. One typed indexer (Recommended) — Keep only `NDArray<T> this[params object[] key]`. …
2. Inherit NDArray's — NDArray<T> declares no indexer. …
3. Keep the scalar indexer — Keep `T this[params long[]]` …

Tap an option — or ✍️ write your own.
An answer moves on to the next question; 📨 Submit shows them all before they are sent.
[ ▶ NDArray<T> ] [ NDMatrix ] [ NDListArray ]
[ s_, flatiter ] [ 📨 Submit ]
[ 1 · One typed indexer (Recommended) ]
[ 2 · Inherit NDArray's ]
[ 3 · Keep the scalar indexer ]
[ ✍️ Write my own answer ]
[ 💬 Chat about this ] [ ✖️ Cancel ]
```

- **The options** with their descriptions (the PC's preview layout hides them); for a PREVIEW question, each option's preview
  as a monospace block while they fit (3,800 characters; else a line points at 🖼). Two option buttons a row when every label
  is short.
- **Several questions are TABS**, as at the PC: a tab row (each question's header cut to 16 characters, else `Q<n>`; ▶ on the
  shown one, ✓ on an answered one; then `📨 Submit`; three to a row).
- **Buttons by question kind**: single-select — the options, `[✍️ Write my own answer]` (the PC's "Type something."); a
  preview question — the options, `[📝 Add a note]` (the PC's Notes field; then `[✏️ Change the note] [✖ Remove the note]`,
  `[📨 Send the note alone]`), `[🖼 Previews as a picture]`; multi-select — ☐ / ☑ toggles, `[✍️ Add my own]`, `[✅ Done ▶]`;
  always `[💬 Chat about this] [✖️ Cancel]`.
- **Long content is shortened, never dropped**, in steps: the previews to the picture, then the list of the other questions,
  then the descriptions (400 / 250 / 160 / 100 / 60), then the question (400 / 250 / 160) — saying "(Shortened to fit one
  message — the PC shows it in full.)"; only a question no step fits falls back to its labels. Checked against 1,065 real
  calls (20,200 card states): every card within 4,096, tags balanced, every button ≤ 64 bytes.

#### 12.1.3 The flow

- A tap on an option answers a ONE-question card at once, with the note if one is set (the PC's Enter). On a card of several
  questions it records the pick and moves to the next question WITHOUT an answer (wrapping), and once every question has one,
  to the **review**. A multi-select question toggles, and `✅ Done ▶` moves on.
- **The review** (the 📨 Submit tab): "Review your answers", "You have not answered all questions" while one is open, every
  question with "→ answer" or "→ not answered" and its note, "Ready to submit your answers?" — `[📨 Submit answers (a of n)]`
  then `[💬 Chat about this] [✖️ Cancel]`. Unanswered questions may be submitted.
- **The user's own words**: ✍️ / 📝 post a ForceReply prompt ("✍️ Your answer to: …", placeholder "Your answer"), so the phone
  opens its keyboard as a reply; the reply — or the next message in the topic, or any reply to the card itself — is TAKEN
  (👌 on it) and the prompt deleted (a `[✖ Cancel writing]` drops it). An own answer of a one-question card sends at once; a
  note is stored. A message typed on the review goes to the first question without an answer.
- **A plain message while the card is open** is not queued silently: it is HELD and asked about, as a reply to it — "❓ Claude's
  question is still open: … What is this message?" — `[✍️ My answer]` (or `[📝 My note]` for a preview question) ·
  `[💬 A reply]` (to Claude, instead of answering) · `[📨 The next prompt]`. An edit of the held message changes what it
  holds; a `/command` stays a command.
- **✖️ Cancel** asks first — "Cancel the question? Claude stops this turn and waits for you — as Esc does at the PC" `[✖️ Yes,
  cancel] [◀ No, go back]` — then sends Claude Code's own STOP rejection with `interrupt` (the transcript then holds exactly
  what an Esc leaves, the turn ends, and an Auto runner is held as after an Esc).
- **💬 Chat about this** sends Claude Code's own clarify rejection with each question's answer so far and its note; the turn
  goes on and Claude asks what to clarify.
- **💬 A reply** sends Claude Code's `response` field (free text instead of answers): the model reads "The user responded:
  <text>" and goes on (the answers made so far are not sent with it).
- **🖼 Previews as a picture**: the current question's previews drawn into ONE picture (§17.4), sent as a photo; the button stays
  busy until it is in.

#### 12.1.4 Composing the answer exactly as the PC does

| Choice | `updatedInput` |
|---|---|
| a pick | `answers[question] = label` |
| multi-select picks | the labels in TICK order (the order the user ticked, not the options' order), then the user's own text last, joined with `", "`; a label or text that holds `", "` or a `"` is JSON-quoted (Claude Code ≥ 2.1.259's rule) — e.g. `"Olives, Cheese, "extra basil, please""` |
| own words (a plain question) | `answers[question] = the text` |
| a note without a pick | `answers[question] = "(notes only)"` |
| a preview question | `annotations[question] = {preview: the picked option's preview when it is shown in full — sanitized as Claude Code sanitizes it (invisible format characters → U+FFFD except the kept ones, VS15/VS16 dropped, TABs to the next multiple of 8), with or without a note; notes: the trimmed note when not empty}`; the entry left out when it has neither; `annotations = {}` when no question has one |
| bounds | each answer ≤ 8,192 characters, all ≤ 32,768 (Claude Code's own remote-card bounds) |

The decision lines (ASCII JSON, one line):

```
allow:  {"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow","updatedInput":{…the input…,"answers":{…},"annotations":{…}}}}}
reply:  … "updatedInput":{…,"response":"<text>"} …
chat:   {"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"deny","message":"<the clarify rejection>"}}}
cancel: {"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"deny","message":"<the STOP rejection>","interrupt":true}}}
```

Claude Code's texts, verbatim (they are what the model reads; Appendix D.4 has them whole): the rejection prefix "The user
doesn't want to proceed with this tool use. The tool use was rejected (eg. if it was a file edit, the new_string was NOT
written to the file). To tell you how to proceed, the user said:" + the clarify block; the STOP rejection "… STOP what you are
doing and wait for the user to tell you how to proceed." A `deny` message becomes the tool result verbatim (`is_error`).
Proven live: a pick with a note, a note alone, a release, Chat about this, three mixed tabs, Cancel, a reply; 937 real
answers recomposed byte for byte.

#### 12.1.5 Outcomes

The card's text is replaced and its buttons removed:

| Outcome | Card |
|---|---|
| sending | ⏳ Sending your answer to Claude… |
| confirmed (the transcript's tool result within 60 s) | ✅ Answered from Telegram · 14:02 — the questions and the answers, notes included (else "⚠ Claude Code did not confirm this answer") |
| the PC answered first | ✅ Answered at the PC · 14:03 — the PC's answer, notes included; the hook is released |
| Chat about this | 💬 You asked Claude to clarify |
| Cancel | ✖️ Cancelled from Telegram |
| a reply | 💬 Replied from Telegram — instead of answering (the reply) |
| Claude Code's idle timeout (`afkTimeoutMs`) | ⏱ No answer in time — Claude Code went on by itself after 1 min (what was selected before it) |
| closed / ended / topic gone | ⏹ No longer open here — it was closed at the PC / its session ended / its topic is gone |
| a decision the registry refused (the PC was first by a hair) | said so; the watch then names the PC's answer |

#### 12.1.6 The card's watch and the mirror

- Each tick reads the request's transcript (its first read looks back 512 KB) for the dialog's `tool_use` (the newest question
  item with the card's signature) and then its result — which confirms the phone's answer or reports the PC's.
- A topic made again (a recreation) gets the open card posted anew; a topic gone releases after 30 s.
- The mirror around it: the transcript's question item posts NOTHING while a card shows the dialog; it waits **8 s** for a
  hook, else it is posted show-only (the question and its options as text) with the reason and "🔄 Restart Session gives it the
  question hook" (a claude started before the bot was on, or older than 2.1.139). The answer item posts no "↩ answer" line
  under a card that shows it. Every answer line carries its notes (S0).
- Not covered: Claude Code's EXTENDED questions (`kind` text / number, `title`, `followUp`: a host launch option of SDK and
  remote hosts, unused by CLI sessions); a subagent's question (answered at the PC).

### 12.2 `/add-dir`: the menu card

#### 12.2.1 What Claude Code does (2.1.291, measured)

`/add-dir <path>` is a LOCAL command: no hook, no prompt line. A path to a directory the session cannot read yet opens, in
place of the input box:

```
Add directory to workspace
Q:\work\other                                  (the RESOLVED absolute path; ~ and relative paths against the session's folder)
Claude Code will be able to read files in this directory and make edits when auto-accept edits is on.
❯ 1. Yes, for this session
  2. Yes, and remember this directory          (saved to <cwd>\.claude\settings.local.json, permissions.additionalDirectories)
  3. No
Enter to confirm · Esc to cancel
```

A digit picks at once (no Enter); Esc is No. NOTHING reaches the transcript while it is open; once answered, three lines land
together (a `<local-command-caveat>` meta line, the echo, the result) — the echo's timestamp is the ANSWER's. A path already
readable, missing, not a directory, a network share or unreadable opens no dialog: the echo and its message land at once. A
bare `/add-dir` opens a path INPUT.

#### 12.2.2 The watch

- Started in the prompt funnel (and for `/steer /add-dir`) BEFORE the prompt reaches the queue, reading the transcript from
  where it stands, so a dialog-free result written milliseconds after delivery is never missed.
- Once delivered (Sent and injected), the hosting window reads the terminal's last **40** rows (`menu-screen`): from 1 s after
  the delivery, at most every 2 s, at most **15** times.
- The screen parser finds the BOTTOM-MOST title with its three options in order and their exact labels (a label a narrow
  terminal cut with "…" counts), the path between the title and the dim note (a wrapped path joined), and at most 4 non-blank
  rows under the last option (a frame left in scrollback is followed by more; the bare command's path input has no options).
  For a FULL path the dialog must show it (case, slashes and whitespace ignored); a relative path is resolved by Claude Code, so
  the dialog is taken as the command's.

#### 12.2.3 The card

ONE menu card, posted WITH sound as a reply to the user's message (a dialog that answers itself in a minute is a "needs you"):

```
📁 Claude Code asks · Add directory to workspace
Q:\work\other
Claude Code will be able to read files in this directory and make edits when auto-accept edits is on.

⏱ Picks Yes, for this session by itself in 1 minute — or tap your answer.
[ ⏱ 1 · Yes, for this session ]
[ 2 · Yes, and remember this directory ]
[ 3 · No ]
```

The countdown is a `tg-time` the apps keep current. Data `tm:<key>:<digit>`.

#### 12.2.4 Answering

- A tap turns the card to "⏳ Answering · <option>" and asks the hosting window to press that digit (`menu-answer`); the tap
  stays busy until the window answered. **Nobody answers within 60 s** of the dialog being SEEN → the default, option 1, is
  pressed the same way (it is the dialog's own focus and the one choice that changes nothing on disk).
- The window presses ONLY into the very dialog the card shows: it reads the screen AGAIN at the press, on the terminal's own
  thread, and types the digit only when the dialog is still open, shows the card's path, and no delivery or draft swap holds the
  box — else `gone` (answered or closed at the PC first, or the tab dormant), `other` (a dialog for another path), `busy`,
  `failed`. A digit typed anywhere else would land in the input box as text.
- Repeats: a `busy` press is asked again every 2 s for as long as a delivery may hold the box (75 s); a failed press repeats
  (3 attempts); so does a confirmed press with no result within 10 s (the dialog read again first); then the card closes with
  "answer it at the PC".

#### 12.2.5 Outcomes

The card's buttons go; it reads the outcome — "✅ Added for this session", "✅ Added and remembered", "⚠️ Added for this session,
not remembered" (the settings file refused the save), "✖️ Not added" — WHO answered ("answered from Telegram", "⏱ picked by
itself after 60 s", "answered at the PC": the card credits its own press only when the window confirmed it AND the outcome is
that option's), the time and Claude Code's own message. "⏹ No longer open here — <why>" when the session closed, its claude
ended or was parked, the dialog went away with no result, or the box stayed busy. A command with no dialog (or whose result
came first) gets its result as a reply to the user's message: "ℹ️ Already in the workspace · /add-dir" over Claude Code's
message. A terminal that shows a menu the parser cannot read after 15 reads gets one note: "⚠️ /add-dir opened a dialog this
{app} cannot read — answer it at the PC". Only a `/add-dir` SENT FROM A TOPIC is answered by the clock; one typed at the PC is
answered there.

#### 12.2.6 The delivery fixes every local command needs

- **The echo is a local command's delivery evidence**: a delivered `/add-dir` or `/model x` has no hook and no prompt line, so
  without this the lost-send verdict failed it with a 👎 15 s later and paused the runner. The transcript's command echo is
  consumed as the prompt's pull echo: a Sent, unechoed, stamped prompt of the same command word (any case) and the same
  arguments (whitespace runs folded), its delivery not newer than the echo by more than **2 s**, the oldest such first.
- **No lost-send verdict while a menu is open**: the dialog holds the command's whole record back, so it reads unechoed for as
  long as it is up; the verdict waits until the menu closes.

### 12.3 The interrupted-turn questions

A restart or a fork asked from the phone MUST NOT pop a dialog on the PC; the bot asks in the topic and passes the answer on.

- **Restart** (`/restartsession` at rest, the card's 🔄): when the session's last turn was interrupted by the user in a way a
  rewind would let Claude Code continue, the bot posts "⏸ You interrupted <title> …" with `[▶ Continue the turn]` (one
  `last-prompt` line appended after the old claude exited + `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1`) · `[⏸ Keep it interrupted]`
  · `[Cancel]`; the toast says the question is below. A restart of a RUNNING turn (confirmed) and a Reactivate of an errored or
  exited session pass "keep interrupted".
- **Fork** (`/forksession`, the card's ⑂, a Search pick's ⑂): when a fork would continue the source's turn (the source Running
  or Needs approval, or its tail one a resume continues), "⑂ <title> is in the middle of a turn …" (naming a tool call the fork
  would run again) with `[⏸ Fork interrupted]` (the fork's tab is locked and ONE Esc is pressed the moment it runs) ·
  `[▶ Fork uninterrupted]` · `[Cancel]`; the tokens carry the request's folder, title and model; a tap sends the request again
  with the answer. Otherwise it forks at once.

### 12.4 Plans and permission prompts

Not answered from Telegram. A plan is posted as the turn's final message ("📋 **Plan ready for approval**" + the plan, rich);
a permission prompt shows as Needs approval: the card rings (§10.3). (A hook can approve or reject `ExitPlanMode` too —
measured — but plan approval is outside this design.)

---

## 13. Session control from Telegram

### 13.1 ▶ Reactivate · 🔧 Resume Session · `/reactivate` · `/activate`

- A **parked** tab (its claude never started, or deactivated) → its claude is started in place (the host's Activate, no focus
  change); with the interrupted-turn variable a cut-off turn goes on by itself.
- A session in **Error** or **Done** whose tab exists → restarted (the same conversation id, the same tab, keep interrupted).
- A running session → left alone.
- The button answers with a toast (nothing is posted: the thread is the conversation); `/reactivate` answers in the thread.
  `/activate` only starts a parked tab.

### 13.2 🔄 Restart Session · `/restartsession`

- A closed session → a toast pointing at the 🔎 Search topic.
- **Running** → `/restartsession`'s question goes into the thread — "Restart **<title>** while a turn is running? The turn is
  cut off." `[Restart now] [Cancel]` (a token, 1 h) — and the toast points at it (a cut-off turn reads Error when it resumes).
- A **parked** tab → started in place, as ▶ Reactivate (a host's restart of a never-started connection must be refused, not
  crash; so a restart there does what the user means).
- At rest → the interrupted-turn question when it applies (§12.3), else restarted at once: "🔄 Restarting it — the same
  conversation resumes in its tab."

### 13.3 ⎋ Esc · `/interrupt`

- **⎋ Esc** (the card's Session page only — never on Main, where a tap meant for ⚙️ Session could stop the turn): ONE Esc key
  at once, no question. It does what the key does at the PC: a running turn stops (`[Request interrupted by user]`) and the
  session waits; a permission or question dialog is turned down; an open menu closes; at rest with an empty box nothing
  happens. An Auto runner is held as after a PC Esc.
- **Judged again by the hosting window at the write** (`press-esc`): refused while a delivery or a draft swap holds the box —
  an Esc between a delivery's keystrokes or right after its Enter puts the prompt back into the box, and the Enter rescue
  would then SEND THE VERY PROMPT THE USER STOPPED. Through the session's own injector, never a split's active pane.
- **One Esc per session per 3 s**: two quick Escs on an empty box open Claude Code's Rewind menu, and a phone's double tap is
  exactly that. The stamp goes in BEFORE the request leaves and is taken back when nothing was written (a refused Esc never
  blocks its retry).
- Toasts (the window's answer): Running "⎋ Esc pressed — the turn stops and the session waits for you. A message here goes on
  from where it stopped."; Needs approval "⎋ Esc pressed — as at the PC, the permission or question it waited on is turned
  down, and the turn stops."; at rest "⎋ Esc pressed. No turn was running, so it only closes a menu or dialog, if the screen
  shows one."; refusals name their reason (parked or exited: nothing reads the key; another agent; a held box — "tap ⎋ Esc
  again in a few seconds"; the 3 s gap; no injector). An Esc is a key, not a verified action: the card's next state shows the
  outcome (the button stays busy until it does, ≤ 9 s).
- **`/interrupt`** asks first — "Interrupt **<title>** while a turn is running? The turn is cut off." (or "… while it waits for
  your answer at the PC? …" for a permission or question wait) `[Interrupt now] [Cancel]` — then takes the same path; its line in
  the thread is the window's answer. With nothing in flight: "Nothing is running." Another agent's topic refuses it at once.

### 13.4 🌙 Deactivate · `/deactivate`

- Parks the tab: claude stops gracefully; the tab keeps its place, title, color, tags, queue and runner mode; a message in its
  topic or ▶ Reactivate resumes it. Only for a started tab the host launched (never an adopted shell tab: its PTY is the
  user's shell).
- **At rest** it parks at once. **While a turn is in flight** (Running or Needs approval) it parks NOTHING: the question goes
  into the thread — "Park <title> while a turn is running? The turn is cut off." `[Park now] [Cancel]` — and the toast points at
  it. Only the argument `cut-off-turn`, sent after "Park now", allows the cut-off; the window answers `turn-running` for a turn
  that started between the tap and the park, and the bot asks instead.
- Toast "🌙 Deactivated — its claude is stopped and the tab keeps its place. A message here or ▶ Reactivate resumes it."; the
  button stays busy until the card shows the parked tab. A refused park is never reported as "Parked".

### 13.5 ⑂ Fork · 🆕 New Session · `/forksession` · `/newsession`

- A fork of the session, or a new session in its folder, in a new BACKGROUND tab and so a new topic — never pinned. The card's
  buttons take no model; the typed commands take `[model | @preset]`.
- The thread gets the commands' own lines: "Forking into a tab…" (or "Opening a new session…"), then the answered outcome
  (§13.6). The fork-turn question of §12.3 comes first when it applies.

### 13.6 Every launch is answered

Every launch the bridge asks for — a new thread's session (§15), `/forksession`, `/newsession`, the Search topic's
`/newsession <folder>`, ▶ Resume, ⑂ Fork — gets a token and its pending record (a "revive" or a "bind") BEFORE the request
goes out (the answer may come back from another thread at once). The answer (Appendix E) is applied by the next tick:

- **Opened** → "Opened <title> — its topic is coming up." · **AlreadyOpen** → "<title> is open already, with its own topic." (or
  "… — its topic is coming up.") · **fresh** → "Opened <title> as a NEW session in the folder (there was no transcript to
  continue) — its topic is coming up."
- The revive settles on EXACTLY the session the window opened (never a guess: a fork's or a `/newsession`'s source is live and
  would match). The opened session is held in Waiting for you so its topic appears (pinned only where asked — never for a fork).
- **A refusal** is posted after the request's own "Forking into a tab…" line (so the two never swap): "⚠ The fork of “X” did
  not open. <the reason>" — and NO dialog shows on the PC (the host records the refusal instead of showing it while a phone's
  launch is in flight).
- **3 minutes** without an answer → never silent: "The PC did not answer within 3 minutes: the fork of “X” may still open — its
  topic appears if it does." / "The fork of “X” opened on the PC, but its session did not come up within 3 minutes."
- A request that names a window goes to that window ONLY (a window that closed meanwhile takes nothing, and the wizard asks
  again); an anchor session opens the new tab right after it.

### 13.7 When no window answers

Every window-hosted action whose reply is never called (no window hosts the tab: closing, frozen) MUST be reported — "No window
answered for its tab (it is closing, or the PC is busy) — …" — not left as a button that just stops spinning. An answer word a
window does not know (an older host) reads as a failure.

### 13.8 ⏎ `/enter`: the prompt in the box, submitted

- **What it is for:** a prompt that sits UNSENT in the session's input box at the PC — typed there and left, typed in by
  `/handover-standby` for review, carried into a fork's box, typed back after a restart — is submitted from the phone, as if the
  user pressed Enter at the PC. Claude Code does what its Enter does: at rest a turn starts; in a running turn the prompt is held
  and taken in at the turn's next step (§4.6).
- **Only a prompt is pressed on.** The hosting window reads the box at the press (`press-enter`; the delivery's tri-state read,
  1,000 rows) and writes ONE `\r` through the session's own injector only when the box holds a prompt:

  | The box reads | `/enter` does |
  |---|---|
  | a prompt | presses, then reads the box back (below) |
  | empty | nothing — "there is no prompt to submit" (a dim suggestion is not a prompt) |
  | a menu or a dialog | nothing — Enter would PICK the option it highlights (named in the line); plans and permission prompts are not answered from Telegram (§12.4) |
  | no box (a dialog over it, claude still drawing) | nothing |
  | held by a delivery, a swap, a mail move or a fill | nothing — "send /enter again in a few seconds" |

  A parked tab, an exited claude (its tab may hold a shell) and a closed session read the key nowhere; a Codex session takes no
  keys; an adopted claude does.
- **One owner of the box.** The window takes the delivery gate (`#telegram-enter`) before it writes, and keeps it until the
  read-back ends: no runner send, Send now, `/steer`, swap or fill types into the box meanwhile (and ⎋ Esc / 🌙 Deactivate answer
  busy for that moment, ~0.2 s when the prompt goes, 4 s at most).
- **The read-back**, every 100 ms for up to 4 s: the box holding no prompt for TWO reads in a row (empty, a dialog, no box — one
  read can be a repaint frame) is **submitted**; a prompt still there at the end — the same one, or a changed one (an entry picked
  from a list Claude Code shows, a new line after a trailing `\`, the clean-out of invisible characters that cancels a submit) — is
  **kept**; an unreadable screen ends the watch as **pressed, unverified**.
- **The answer** is a silent reply to the `/enter` message: "⏎ Submitted: «first line»" ("… into the running turn: «…» — Claude
  Code takes it in at its next step, or starts the next turn with it." while a turn ran), "⏎ Enter pressed, but the box still
  holds «…» — Claude Code did not submit it. … send /enter again to submit it as it is now.", or the refusal's reason. When no
  window answers: "⏎ No window answered for its tab … — the Enter may not have been pressed." (§13.7).
- **Never later:** an `/enter` older than the backlog age (§11.2.6) is answered "nothing was pressed: an Enter belongs to the
  screen of its moment" (a new thread's held message is no backlog). `/enter <words>` presses nothing: the usage line points at
  `/steer`. The Search topic refuses it; `/cc /enter` forces the word through (Claude Code has no `/enter`).
- A host without the window round trip has no fallback press (a blind Enter is what the command must never write).

---

## 14. The 🔎 Search topic

### 14.1 What it does

A message in the Search topic is a REQUEST in plain words ("the session where I fixed the tab drag crash"), typed or said in a
voice note (§19: transcribed with the fleet's names as its vocabulary, its transcript echoed under it is the request); follow-ups
refine it. A search agent — an LLM with session-search tools (the reference: Claude Sonnet 5.5 through a headless Claude Code process
with in-process MCP tools, no user settings, no hooks, no session persistence) — reads the sessions it finds BEFORE it shows
them, and answers with evidence. The topic is the phone's way to find any Claude Code session ever run on the PC and bring
it back into a tab, and so into a topic.

### 14.2 One conversation, never a session

- ONE conversation for the whole process (separate from any search UI on the PC), made on first use and kept for the process;
  a stop and start of the bot reattach to it; its record makes it carry on after a restart.
- It is NOT a session: no transcript, never a search result, and nothing wakes, resumes, forks or pins it. A session's command
  sent in Search is refused: "This conversation is not a session: it cannot be woken, resumed, forked or pinned — it carries on
  by itself after a restart, and /clear starts it over". Claude Code's commands are refused ("Claude Code commands go in a
  session's topic").
- **Clearing is the user's one action on it**: `/clear` or `/reset` in this topic (read from the raw first word, so `/cc /clear`
  is not a clear), or the **⊘ Clear** button under every answer (stateless `s:clear`): the context, the record and its view go,
  a running request is interrupted, one silent line confirms. A clear older than the backlog limit is answered, not carried out.
- Its process is parked after idle minutes and continues from its record. Its thinking level is a setting applied live to the
  running conversation (low · medium · high · xhigh · max; a level "off" is not offered — it fails every request on Claude Code
  2.1.310, so a stored "off" runs as low).

### 14.3 The agent's tools (the contract the answer format relies on)

| Tool | Does |
|---|---|
| `search` | finds sessions by title, folder, content (every session by default; a pasted session id matches it and its forks); rows carry their status and age by the most recent key |
| `grep` | full-text search of every transcript (one ripgrep over the projects folder, narrowed by each alternative's required literals) |
| `read` | a session's conversation at a transcript level |
| `show` | the ONLY way to put sessions into the answer, each with a `why` (the evidence it saw) |
| tab tools | `overview` (every window's tabs as one page grouped by folder), `status` (what up to 32 sessions are doing, and why their next queued prompt waits), `list`, `resume`, `fork`, `deactivate` |

Guards in code, not in the prompt: with more than one window and no window named, `resume` and `fork` refuse and describe the
windows (from a phone the agent must ask); a window may be named by number, by the user's name for it, or by id (a number in
range first, then the name, then an id; a name two windows share is refused with the list); at most 5 opens and 1 deactivate
per request; `deactivate` refuses an adopted tab, and a Running or Needs-approval one unless told to cut the turn off. A
phone's resume / fork opens a background tab.

The index: ONE session index per process, built for every session at start and never freed; a tool call waits up to 90 s for
a complete, fresh index and never searches a partial one ("searches must never miss").

### 14.4 The answer

"Searching…", then ONE message — the Search topic's final message, WITH sound:

```
I'll 1. search for release sessions 2. read the candidates 3. show the two that match.      ← the agent's plan, above

Two sessions are about releasing the app.                                                  ← its verdict sentence

1 📦 [🟦 Release v0.7.9]
project-a · 11 Oct · 14 prompts
┃ You asked: "cut v0.7.9 as a pre-release and publish the draft"                           ← ONE quote: its why

2 💬 [🟩 Release notes]
…
(94 more below, one line each.)
[1] [2] [3] [4] [5]
[6] [7] [8] [9] [10]
[⊘ Clear]
```

- At most **10** evidence cards: `<b>N</b> <glyph> <title chip>` (the title on one line, ≤ 100), a facts line `folder leaf · day ·
  N prompts` (day words: today, yesterday, "11 Oct", "11 Oct 2025"; unknown parts left out), and the `why` as one blockquote.
  "Nothing found." when there is nothing. A long answer is split, the buttons on the last part.
- **Glyphs** (where the session stands NOW): 📦 closed · 💤 an idle tab · 💬 has its own topic · 🌙 a deactivated tab · ⏹ its
  claude exited. Each card's title is a link to its topic when it has one.
- **The rest**, one line each, as silent messages packed into ≤ **3,500** characters: number, glyph, color square, title (80),
  folder leaf, day — plain HTML, no chips, no buttons (a message of a hundred chips would be refused). The user names a number
  in a reply ("resume 37"), which the conversation knows.
- One **pick** button per card (five to a row), then ⊘ Clear.

### 14.5 A pick and its menu

- A pick is stateless: `p:<n>:<session id>` (≤ 41 bytes; parsed strictly: the prefix, a 1–2 digit number, an id of 8–40 hex
  digits and dashes). It posts the session's ACTION MENU as a silent reply to the answer: `<b>N · title</b>`, the whole folder and
  the branch, when it ran ("11 Oct 14:02 → 16:40"; clock times today), its prompts, the same quote, and in italics where it
  stands now. A pick of a session this run did not list (after a restart) shows what the host's registry knows, without a quote.
  A pick may be tapped again (a new menu).
- **The menu fits where the session stands** (re-classified at the pick, and again at the tap):

  | Where it stands | Glyph | The menu's line | Buttons |
  |---|---|---|---|
  | Closed (on disk only) | 📦 | Closed: Resume opens it in a background tab, and its topic appears. | `[▶ Resume]` · `[⑂ Fork] [✕ Cancel]` |
  | Deactivated tab, no topic (never started / parked, Idle, not pinned) | 🌙 | Open in a tab, but deactivated (its Claude is not running), so it has no topic: Activate starts it there, and its topic appears. | `[▶ Activate]` · `[⑂ Fork] [✕ Cancel]` |
  | Deactivated tab with its topic | 🌙 | Open in a tab with its own topic, but deactivated (its Claude is not running): Activate starts it there. | `[▶ Activate] [💬 Go to topic]` · `[⑂ Fork] [✕ Cancel]` |
  | Its claude exited (Done) | ⏹ | Open in a tab with its own topic, but its Claude has exited: Activate starts it again there, in the same conversation. | `[▶ Activate] [💬 Go to topic]` · `[⑂ Fork] [✕ Cancel]` |
  | Idle tab (claude runs), no topic | 💤 | Open in a tab, idle: Wake brings its topic back. | `[⏰ Wake]` · `[⑂ Fork] [✕ Cancel]` |
  | Listed (claude runs, it has its topic) | 💬 | Open, with its own topic: Go to topic puts that topic first in the thread list. | `[💬 Go to topic]` · `[⑂ Fork] [✕ Cancel]` |

  The parked test comes FIRST (a restore keeps Waiting for you or Needs approval, a cut-off turn reads Error, and each still
  needs its claude started). An adopted session is never parked or exited (its tab is the user's own shell, which Search must
  not start or restart). No menu button pins: a session is pinned in its own topic; ▶ Resume is never offered for a session
  that has a tab.
- **The actions** (single-use tokens, 2 h), on the state at the TAP: ▶ Resume / ▶ Activate / ⏰ Wake all make the session usable
  from here — closed → the background resume (§13.6); parked or exited → start it in place / restart it (judged by the state
  BEFORE the wake), then an Idle or Done session is held in Waiting for you; idle → Waiting for you (its topic comes back);
  listed → "it is open already". 💬 Go to topic → the front move (§16.4) — "its topic is gone" when it left since. ⑂ Fork → a
  fork in a new tab (never pinned; the fork-turn question first when it applies). ✕ Cancel → the menu is deleted. The menu loses
  its buttons and keeps the choice or the outcome.

### 14.6 Commands of the Search topic, and reaching it from anywhere

- **Its own commands**: `/start` · `/help` (Appendix D.2) · `/status` (the bot's line — "**{app}** — <the status line, e.g. Connected as @bot (topics
  enabled)>" and "N topic(s)" — then Claude Code's account and plan usage, §18) · `/idle` (the idle tabs, each with `[Wake n]` `[Wake 📌 n]`: Wake puts the
  session back to Waiting for you, its topic comes back) · `/board` (counts per state) · `/newsession <folder> [model]` (a new
  session in that folder: a new tab and topic; the folder checked with a bounded probe) · `/pauseall` · `/resumeall` (the
  global pause of every runner) · `/clear` · `/reset` · `/recreate` · `/verbose on|off` (the default for all topics) ·
  `/search_tab` · `/search <words>` (here simply a request of the words).
- **`/search-tab`** (also `/search_tab` and a bare `/search`), typed in ANY thread: the command message is DELETED where it was
  typed (a move, not content — left in a session's topic it would be that topic's newest message and its card would re-post
  below it, putting THAT topic back on top), and one silent line goes to Search: "🔎 Search — first in the thread list now
  (/search_tab). Ask for a session in plain words here, or with /search <words> from any topic; /help lists the rest." Search
  is then the chat's newest message: FIRST in the thread list on every app (Android's "All" also shows "Continue last thread ›").
- **`/search <words>`** in any other thread: the command is deleted where typed; the words are posted IN Search as the user's
  line — `👤 You · 02:41 · 🔎 search, from a topic` (or `from the main view`), the topic it came from as a title that links back,
  the words (the header writes "search", never "/search": a command in a bot's message is a tappable link) — then asked of the
  agent there: "Searching…" and the answer with sound.
- Not done (answered where typed, nothing deleted): with no Search topic yet; for a `/search` older than the backlog limit.
  A new thread whose ONLY message was such a command is deleted once the command left it.
- Notes that land in Search: the start note (§23.2), the recreation and replay notes ("✅ History added to N topic(s) (M
  message(s)) …"), the secretary probe's verdict. A voice note there is a request like a typed one (§19.2).

---

## 15. A new thread starts a session

### 15.1 What starts it

- The owner opens a thread in the bot's chat (`forum_topic_created` from the owner in a thread no session owns). A name the
  user GAVE (`is_name_implicit` false) becomes the session's title and the picker goes up at once; a name Telegram made up waits
  for the first message (one post, not a post and an edit).
- Any message in a thread no session owns (one the user opened, one made long ago, one whose session closed while the thread
  stayed) is HELD (👀; at most 20 held) and the thread gets a picker if it has none.
- The wizard is ONE message, edited in place step by step; the session starts when every step is answered.

### 15.2 The steps

| Step | Shows | Takes |
|---|---|---|
| **Folder** | "📂 New session — pick the folder it works in", what happens to the held messages ("Your message waits here and becomes the session's first prompt"), how the title is chosen ("Its title: **name** — rename this thread to change it", or the folder's name), "Or send a folder's full path", "Then a model (optional) and a window. ✖ Cancel deletes this thread."; two lists, each paged ten at a time — 🕘 **Recent** · 1–10 of 34 (`1 K:\source\project · 3m ago`: the folder whose newest session was active last first) and 🏆 **Most used** · 1–10 of 34 (`1 K:\source\project · 412 sessions`) | a folder button (`🕘 1 · project`, two to a row; the parent's name too when two shown folders share a name), a list's PAGER row, ✖ Cancel, or a TYPED path |
| **Create folder** (a typed folder that does not exist) | "📁 X does not exist yet: `K:\a\b\X`" — what exists, what a Create makes ("2 new folders") | `[📁 Create the folder]` · `[◀ Back to the folders]` · `[✖ Cancel]` |
| **Model** (optional) | the chosen folder; "Default is the settings model, `opus`" (or Claude Code's own default); how to type an id | `[✅ Default] [✏ Specify…]` first (the PC menus' order), then the model last typed in a "specify a model" prompt (↺) when the menu lacks it, the host's models (🧠) and presets (🔌), `[◀ Back] [✖ Cancel]`; or `/model <id>` typed. With no preset at all the step says where the host adds one. |
| **Model id** (after ✏ Specify…) | "Send the model id as your next message — like `claude-opus-5-5[1m]`" | one word typed; `[✅ Use Default]` · `[◀ Back to the list]` · `[✖ Cancel]` |
| **Window** (only with several windows) | per window: its number (and its name), its tab count, "used last", how many tabs work in the folder, its newest tab ("newest: “beta” · opened 2h ago"), its ten newest titles (40 characters each, one line; shrunk 10 → 6 → 3 → none until twenty windows fit one message); "⭐ Default: window 1 — next to “beta”, the newest tab in proj" ("Research (window 1)" for a named window) | `[⭐ Default — window N]`, one `[🪟 N]` per window (four to a row; `[🪟 N · Name]` two to a row while any is named), `[🔄 Refresh]`, `[◀ Back]`, `[✖ Cancel]`; or the number typed (2, #2, w2, window 2), or the window's name |

Then the launch line in the thread — "🚀 Opening a new session in **proj** · Opus · window 2, next to “beta”…" (quoting the
user's message when the last choice was typed), posted BEFORE the launch so a refusal can never land above it — the picker is
deleted, and the bind (§15.4) makes the thread the session's topic.

- **The lists** are the folders of every session the host holds, archived ones too (a session counts for its launch folder
  and, when it differs, its inferred one); the machine's temp roots left out; every listed folder probed (a deleted folder is
  not offered); at most 100 a list. The PAGER row: the page label `2 / 4` always shown (it keeps a lone arrow on its side —
  Telegram stretches a lone button over the row — and says how far the list goes), `◀ Recent` on its left from the second page,
  `Recent ▶` on its right until the last: `[1 / 4] [Recent ▶]` · `[◀ Recent] [2 / 4] [Recent ▶]` · `[◀ Recent] [4 / 4]`; a list
  of one page has no pager; a tap on the label answers a toast ("Recent: page 2 of 4 (folders 11–20 of 34)").
- **A typed choice re-posts the picker BELOW the user's message** (the user looks at the bottom); a button edits it in place. A
  typed folder path is a folder choice at EVERY step (the user changed their mind); `/model x` at the model step, one word at the
  model-id step, a window number or name at the window step; anything else is the user's message, held for the session.
- A button of an EARLIER step (tapped before the edit reached the phone) answers "That button belongs to an earlier step" and
  changes nothing; buttons carry indexes into the choices AS THE STEP SHOWED THEM (kept in the record), never a path or a model
  id (`nt:` grammar, Appendix B).
- A notice (a folder that is gone, a refused creation, a launch that did not go through) heads the step until the user's next
  choice clears it.

### 15.3 The folder, the model, the window: rules

- **A typed folder** (one line, absolute; quotes and `/` allowed) is resolved by walking it down: each part looked up with the
  OS's own lookup (on Windows `FindFirstFileExW`, which ignores case and reports the disk's spelling); a part that misses LISTS
  its parent and searches case-insensitively (a folder marked case-sensitive answers only its exact spelling; there the exact
  spelling wins over a case variant). So `k:\SOURCE\project-a` IS `K:\source\Project-A` and the session opens in the disk's
  spelling. Every part is checked BEFORE any lookup (`< > : " | ? *`, control characters, device names — `< > "` are wildcards
  to the lookup); `..` is refused; a missing drive or share is said.
- **A Create** makes the first missing part and everything below it; each name is checked for what Windows cannot make as
  written (a trailing dot or space); the whole path at most 240 characters (the tools a session runs are not all long-path
  aware). A list folder that is gone is never offered for creation (the lists are read again).
- **Bounded waits** (a slow folder MUST NOT freeze the bot): every folder check that runs on the poll thread goes through a
  prober — each probe on a thread of its own, the caller waiting only its own bound (a pick: a FRESH probe, **2 s**; the
  lists: **250 ms** a folder, **1.5 s** for all, answers up to a minute old accepted; the walk 2 s; a creation 5 s); one probe
  per path at a time (a second ask joins it); at most 16 in flight (past it an ask answers at once, unprobed); a late answer
  is kept for the next ask. A folder that did not answer is listed unprobed, and its pick says "The folder did not answer within
  2 s (an offline network share?) — try again once it is reachable: <folder>".
- **A typed model id** is checked before it can reach a launch: letters, digits and `- _ . : / [ ]`, one word, a letter or digit
  first, at most 120 characters (`claude-opus-5-5[1m]`, `sonnet[1m]`, `anthropic/claude-sonnet-5.5`, `qwen3:32b` pass; a space, a
  quote, `;`, `$`, a backtick, a leading `-` do not — the id goes into the command line that starts claude). A listed model typed
  by its id is that choice; `@id` names a listed preset. A typed id joins the host's "specify a model" list. Skip = Default (no
  `--model`).
- **The window default**: the window holding the NEWEST tab that works in the chosen folder, over every window — a tab counts by
  its launch folder OR its inferred one; newness by the session's creation stamp, then its transcript's age, then the MOST RECENT
  key, then the strip position — and the new tab opens RIGHT AFTER that tab. With no such tab: the window used last, else the
  first. One window: the step is skipped. None answered: skipped (the host chooses). The launch names its window: a window that
  closed since the census takes nothing, and the wizard asks again (the tab never lands in a window the user did not pick).
  After a launch that did not go through, the window step SHOWS its list even with one window (a refusal is never retried
  without a tap).
- **A typed window name** picks its window: the whole message, trimmed, any case; a NUMBER is read first (a window named "2"
  never hides window 2); a name two windows share picks neither ("More than one window has that name — send the window’s number
  instead").

### 15.4 The bind: the thread becomes the session's topic

- Before the launch, a session id is MINTED and recorded with the thread, the title and the held messages (the launch is
  asynchronous); the request carries that id (the host launches a fresh claude under it with `--session-id`).
- On **Opened**, the first sync that sees the session live (BEFORE the topic sync's plan, so the plan makes no topic of its own)
  makes the user's thread its topic: a record seeded like a created topic (a fresh session mirrored from its first line), the
  thread renamed like every topic (icon, square — its first color resolved fresh —, title), the card posted, the session held in
  Waiting for you (so its topic outlasts the idle grace), the background tab started.
- **The held messages** are handed over in order, as if sent in the topic (held: never asked about as backlog however long the
  pick took; the first is the session's first prompt; a `/rename` among them renames). Each hand-over starts with one direct
  reaction call that takes the 👀 off — its "message to react not found" answer is the only sign the user DELETED the message
  meanwhile, and such a message is left out; any other answer hands it over (a lost prompt costs more than a stray one).
- **No gap**: the bind stays in place until its record is in AND every held message is handed over (the record in and the held
  messages out in ONE lock scope; the bind leaves only in a scope that finds it empty). A message the user sends meanwhile joins
  the bind and reaches the session AFTER the held ones, in the order written; an edit of a held message replaces it.
- **Refused** (§13.6) → not an abort: the thread goes back into its wizard at the step that can fix it (a refused preset → the
  model step, its choices read again; a failed tab or a closed window → the window step; the model step when the window step had
  been skipped), with its held messages and "⚠ The new session did not start. <the reason>".
- **No session within 3 min** → back into the wizard the same way, the picker posted again below: "The new session did not
  start: the PC did not answer within 3 minutes (its window may be busy)." or "The tab opened on the PC, but the new session did
  not come up within 3 minutes."; the held messages are kept for the next pick.
- A thread renamed during the wizard renames the COMING session (the folder step shows the new title); renamed while the launch
  runs → the session is renamed at the bind.

### 15.5 The lifecycle

| Event | What happens |
|---|---|
| ✖ Cancel at any step | the thread is DELETED (its queued items dropped first); what it held is copied into the main view in one silent line with an expandable quote — the messages go with the thread. A refused delete deletes the picker instead. |
| Nothing chosen for a day (every message or tap counts) | the same: "🗑 “X” was deleted — nothing was chosen in it for 24 hours." |
| A new thread whose only message is `/search` / `/search_tab` | deleted once the command left it for the Search topic |
| The launch refused / no window took it / no session in 3 min | back into the wizard (§15.4) |
| The bot or the host restarts | every wizard and launch comes back from `state.json` (written with the topics in one atomic save): a wizard where it stood, its buttons still working; a launch waits for its session (its clock restarts at the restore, and nothing times out while the windows still restore); a window step whose census was lost asks again once the start-up hold is over |
| A different bot | the wizards and launches are cleared with the topics |
| The user deletes the thread | Telegram says nothing; the next edit or post answers "thread not found" and the wizard is dropped |
| The user deletes the picker | the next edit answers "message not found": a new picker is posted |
| A voice note in the thread | held UNTRANSCRIBED (the session's folder and CLAUDE.md are its vocabulary, and the session does not exist yet); transcribed at the hand-over with the new session and queued as its first prompt (§19.2) |

---

## 16. The pinned directory

### 16.1 What it shows

One rich message in the main view, pinned:

```
🔵 5 running · 🟡 15 waiting · ⚪ 300 idle                                ← row 1 (a pinned bar shows the start of the text)
🟠 2 need approval · 🔴 1 error · 🟢 3 done · ⏳ 12 queued · ⏸ runner paused  ← row 2
🧭 Dedicated topics
[🔎 Search]
🏷️ Tags
Tap a title to open its thread (Desktop, iPhone). Tap its dot to put the thread first in the list (any app).
🟨 release · 3 tabs
[🔵] [🟦 project-a]          ← the dot: a callback button (§16.4); the title: a URL chip to its topic
[🟡] [🟩 docs]
⚪ [🟦 an idle tab]          ← no topic: a plain dot and a disabled chip
🕘 Recent activity
[⚪] [🟦 notes]
[🟡] [🟩 docs]
[🔵] [🟦 project-a]
updated 5 minutes ago        ← a relative tg-time: the apps keep it current; it never costs an edit
```

- **The stats** count the live Claude sessions in a tab (the population of `/board`): row 1 Running · Waiting for you · Idle;
  row 2 Needs approval · Error · Done (claude exited), the Pending prompts of those tabs, and `⏸ runner paused` while the global
  pause is on. Bold numbers; "1 needs approval" / "2 need approval", "1 error" / "2 errors".
- **Dedicated topics**: 🔎 Search as a chip in its icon color's look (blue: primary), disabled until the topic exists. A new
  dedicated topic adds one entry.
- **Tags**: every tag AT LEAST ONE OPEN TAB carries, in case-insensitive name order, each head bold with its color square (the
  host's tag color, one palette shared with the PC) and its count of open tabs; under it those tabs in case-insensitive title
  order, at most 20 (`… and 4 more`). A tag no open tab carries is not listed at all and does not change the message's hash
  (adding, renaming, recoloring or deleting it never edits the directory). With none: "No open tab carries a tag."
- **Each tab row**: the state glyph of the topic names (🔵 🟡 🟠 🔴 🟢 ⚪) — in the rich render a CALLBACK button when the tab has a
  topic — then the title chip: a URL button to its topic in the look of its color square, or a DISABLED chip for a tab without a
  topic (Idle past its grace, another agent's tab).
- **The legend** ("Tap a title … Tap its dot …") goes under the first heading whose rows have dot buttons, once.
- **🕘 Recent activity**: the ten newest open Claude tabs, in the topic list's own MOST RECENT order (ties by title, then
  session id) — every state but **Done** (a tab whose claude ended; nothing happens in it until restarted). Built BEFORE the tag
  sections so a full message leaves out tags before rows. Empty: "No tab to list: none is open, or every open one has ended."
  Not the most-ACTIVE key (every hook): it would reorder running tabs on every check and edit the directory for nothing; MOST
  RECENT moves only at turn boundaries, when the stats change anyway.
- **Limits**: the rich render under 32,000 characters (with a 400-character reserve for its tail), the classic one under
  3,800 + its tail, so it is always ONE message; tags that do not fit counted in one line (`… and 3 more tags (the message is
  full)`); at most 100 tag sections (500 blocks).
- **The classic fallback** (rich off, or refused): the same text, each chip a link, plain dots (classic HTML has no in-text
  callback button), a tab without a topic its bare label.

### 16.2 Where it lives

- Posted ONCE in the main view (no thread), silent, at low priority, then PINNED (`pinChatMessage`, silent — a pin never
  notifies in a private chat). The pin's service note comes back as the bot's own message and is deleted when it names the
  directory (it would sit above every new message of the main view). A refused pin is tried again at the next start.
- Persisted: its message id, its content hash, its form (rich / classic), whether it is pinned. Per bot.

### 16.3 When it changes

- A check every **15 s** builds the model (the sync's facts, cached tag reads, one lean registry copy for the queues) and
  renders it. An EDIT goes only when the content hash (everything but the footer) or the form differs from what the message
  shows, and at most every **2 min** (≤ ~30 edits an hour on a busy fleet; a quiet fleet costs nothing).
- Not during the start-up hold or a recreation, before the host's tag store is warm, before `getMe` gave the username, for
  5 min after a 429, or while a long 429 parks `editMessageText` / `sendRichMessage` / `sendMessage`.
- A refusal backs off 5 min; a rich form refused is not tried again for the same content. **The message gone** ("message to edit
  not found", `MESSAGE_ID_INVALID`, "message can't be edited" — the user deleted it) → forgotten, and a new one is posted and
  pinned **30 min** later (a deletion is not answered at once).
- One send at a time; a send with no answer for 15 min is forgotten; every directory send carries a key (the next replaces a
  queued twin), and a post that still lands after it is DELETED — the chat never holds two directories.

### 16.4 The front move (the dot)

No link opens a bot-chat topic on Android, so the phone's way in is to make the topic the chat's NEWEST message:

- A dot's data is `d:front:<thread>` (stateless: it works after a restart and survives a rekey). A tap re-posts that topic's
  sticky card SILENTLY (delete + send, two calls). Telegram orders a bot chat's topics by their last message, so the topic is
  FIRST in the thread list on every app (after the user's own pins); Android's "All" view also puts "Continue last thread ›" under
  the card. On Android that is one more tap (the first thread tab, or that button).
- Toast: "↑ “project-a” goes first in the thread list" — "… once its history is in" while the topic's replay runs (the card waits
  for it) — or, for a topic that is gone: "That thread is gone: its tab closed or went idle. The list follows within two
  minutes." A second tap on the same dot within **10 s** answers the same toast and queues nothing.
- The topic is held out of the re-sort while its MOST RECENT key is unchanged (§8.9); a re-sort already under way re-posts it
  LAST.
- The same move serves the Search menu's 💬 Go to topic (§14.5).

---

## 17. Pictures

### 17.1 🖼 Screen Image (`/screenimage`)

1. **The read** (window-hosted `screen-snapshot`, on the terminal's own thread, under its read lock): the ACTIVE buffer's live
   viewport — main or alternate (Claude Code's fullscreen UI lives in the alternate one), never a view the user scrolled back
   to. Per cell: the glyph, its width (a wide glyph covers two columns), its colors as the renderer resolves them (the color
   table, bright-for-intense, faint, reverse video, invisible text, a reversed screen), bold by the renderer's rule, italic,
   underline, strikethrough; plus the screen's default colors and the terminal's font family. A parked tab, a window that is
   not up or an uninitialized terminal answers "" → "The screen could not be read (the tab is dormant, or in another window
   that is not up)."
2. **The format** (text, pure, bounded): a header `SCRN1 <cols> <rows> <fg6> <bg6> <len>:<font>`, a style table of
   `<fg6><bg6><flags2>` entries, then one line per row of `<style>,<width>,<len>:<text>` cells, trailing blank cells left out.
   Bounds: 1,000 × 500 cells, 64 code units a cell, 8 M characters. The reader REFUSES anything malformed rather than guess.
3. **The drawing** (a worker thread, never a UI thread; the reference: Direct2D on a WIC bitmap, DirectWrite, PNG by WIC):
   whole-pixel cells — the cell width is a 2,400 px budget over the columns, clamped to 8–16 px; the font sized so its advance
   is exactly one cell; the cell height its line height; 16 px of padding in the screen's background. Each row paints its
   backgrounds as merged runs, then every glyph at its cell (grayscale antialiasing, color emoji), box-drawing and block
   characters (U+2500–U+259F) STRETCHED to fill the cell so frames join, then underline and strikethrough. The font: the
   snapshot's family when installed, else Cascadia Mono, Cascadia Code, Consolas, Lucida Console, Courier New. Measured: 200 × 60
   → 2,232 × 1,352 px, ~0.5 MB, 88–137 ms.
4. **The send**: `sendPhoto` (multipart, `image/png`), silent, at the answers' priority, captioned `🖼 <title> · <clock> ·
   <cols>×<rows>`, the file `screen-<title>.png`. A 400 refusal of the PHOTO (its limits: 10 MB, width + height ≤ 10,000, a side
   ratio ≤ 20 — a very wide terminal can pass them) sends the same bytes again as a document; a transport failure or a 429 stays
   the send loop's (a picture is never sent twice).
- **Bounds**: at most 2 drawings at once ("ask again in a moment"); a second request for one session within **3 s** asks nothing
  new (a double tap must not draw and send two photos); a session with no tab: "This session is not open in a tab, so it has no
  screen to show." `/screen` is the visible TEXT of the same screen.

### 17.2 The card's screenshot

The rich card's LAST block is the terminal's screen: `<figure><img src="tg://photo?id=screen"/><figcaption>🖥 the screen at
19:50:12</figcaption></figure>` (seconds: a running card takes a new picture every ~20 s).

- **A card UPDATE** (its classic TEXT changed: a state, the since-clock, a running turn's context size, the runner, the queue,
  the draft, the recap): the hosting window reads the screen (§17.1's snapshot, ~0.5 ms); a screen that CHANGED since the card's
  last picture (by its hash) is drawn on the ONE drawing worker (a queue; a job a newer update overtook is not drawn) and
  uploaded IN THE SAME CALL as the card's edit or post (a multipart `editMessageText` / `sendRichMessage` with `attach://screen`):
  the picture costs no call of its own, and the number of card edits is what it was. An unchanged screen reuses the photo
  Telegram holds by its `file_id` (no upload), captioned with the moment it was read again.
- **Not an update** (a page flip, a form upgrade): the card keeps its picture with no read and no drawing — a page flip stays
  instant (the picture's text hash is the classic text WITHOUT the buttons).
- **No screen** (a parked tab, a window that is not up): the card keeps its last picture, captioned with ITS moment; a card that
  never had one goes as text. Every POST of the card (first, re-post, ring, re-sort) reads the screen; the card is marked in
  flight at once so no second post starts while the screen is read and drawn.
- **Order**: each picture-carrying post or edit stamps a generation; an EDIT that a newer post or edit of the same card overtook is
  dropped when its picture settles (finishing last, it would put older text back). The card's send runs EXACTLY ONCE whatever
  settles the picture (the window's answer, the worker, a guard for a read no window answered, a failure), and a card change is
  never lost to its picture.
- **A card never waits more than 4 s for its picture**: a FROZEN window would otherwise stop its cards exactly when the phone
  is needed most; past the bound the card goes with the picture it shows, and a late picture is dropped.
- **Refusals**: a post or edit WITH its picture that Telegram refuses is sent once more WITHOUT it in the next slot of the same
  outbox sequence (no key — a fresh slot could land after a newer edit), and only that retry's refusal falls back to classic.
  Three refusals in a row stop the pictures for the run.
- **Kept across a restart**: the photo's `file_id`, its moment and its screen's hash (progress), so a tab restored parked keeps
  its picture.
- **The picture follows the status: LOOKS** (SHOULD; the newest refinement of the reference). One read at the moment of a change
  can miss — the window answers after the 4 s bound (a busy UI thread), the read comes before Claude Code has drawn the new state
  (a turn ends, and its idle prompt is drawn, only once its Stop hooks have exited, while the bridge learns of the Stop at once),
  or a page flip overtakes the read — and a card at rest is never edited again (its text names clock times), so it would keep the
  old picture for good. So every picture step records how it settled (fresh · no screen · kept · stale) and the card LOOKS at its
  screen again when one is owed:
  - a **settle look** 3 s after every post or edit that shows a new state AT REST (anything but Running; a change into Running
    cancels it — a running card reads its screen at every edit anyway);
  - a **retry** 5 s after a step that got no screen in time, then 10, 20, 40, 60 s (each look waits up to 30 s for the window; no
    text waits for it), giving up after 8 looks in a row until the card's next status update;
  - a **page flip's look** 3 s after a flip that overtook a read, or that carries an OLDER picture than the one handed before.

  A look runs only while the card shows its current text and buttons, while no post or edit of the card waits for its picture,
  and one at a time. It compares the screen with the picture the card's newest post or edit CARRIED (maybe still in the outbox):
  the same → nothing is sent; changed → drawn (or the held photo reused) and the card edited with its same text and buttons and
  the new picture, at the cards' priority, without moving the 20 s edit gap or the hashes. A post or edit decided while a look
  runs wins (the look is dropped). Every picture step's generation number is unique across the run (a rebound or recreated
  topic's card must never match an old step by chance). Cost: one more screen read per change into a state at rest, and one more
  edit with its upload only where the screen changed in those 3 s or a read came late.
- The switch: `messagingCardScreenshots` (on). A classic card has no picture. Cost: upload bandwidth, not calls — ~0.1–0.6 MB
  per new picture, roughly 100–250 MB an hour with several sessions running.

### 17.3 📜 All Screen: the whole conversation as tall pictures

- **From the transcript, not the terminal**: the fullscreen renderer keeps no scrollback (§4.1), and the classic renderer's is
  capped, emptied by `/compact` and repainted on resize. The transcript holds every turn — also for a parked tab — so no window
  is asked.
- **Pipeline** (a worker thread): render the conversation turn by turn at a TRANSCRIPT LEVEL, following the fork / plan-restart
  chain (the whole conversation, as a "copy transcript" shows it) → parse its Markdown → lay it out on a terminal grid the way
  Claude Code draws a conversation → draw tall sheets → send.
- **The levels** (the same as `/transcript`):

  | Level | Draws |
  |---|---|
  | Minimal | your messages and each turn's final reply |
  | Compact | + every tool call as one line with the reason Claude gave for it, compactions, your answers, the slash commands you ran, each agent's brief and report |
  | Default | + every tool call on one line, and Claude's remarks between calls |
  | **Full** (the default, a setting) | + each Bash call's exact command, the first line each call returned, how long each took |
  | Max | + every written file's content and a stats line per turn |

- **The look**: 100 columns of 12 px cells, Cascadia Mono on a Campbell-like palette, 1,232 px wide. A turn opens with a rule
  `── Turn 3 · Oct 21 20:51 → last reply 23:13 ───…` (its number bold in Claude's orange); a fork chain's sessions with a heavier
  orange rule `━━ Session 2 of 3 · a3a3a3a3 · forked from session 1 … ━━━`. The user's message is Claude Code's prompt: `❯` and
  the text on a gray band across the row (a teammate's with a cyan `❯` under its `✉ name` line; "typed while it worked" dim
  italic). Claude's work: `●` before each remark (white) and each tool call (green ok, red `✗` failed, gray no result — all gray
  at Compact, whose lines state no outcome); the tool name bold, inline code lavender; `⎿` before what a call returned (Full) or
  an answer, dimmed; a compaction an orange `✻`. The reply after a white `●`, its Markdown kept (bold, italic, lists, headings
  bold, links underlined, code blocks on a darker band). A subagent: an orange `●`, its name bold, its work indented two
  columns. Words wrap between words; wide glyphs take two cells; combining marks ride on their cell.
- **☑️ File Diffs** (a box, off by default): every file change (Edit, MultiEdit, Write, NotebookEdit) drawn as Claude Code draws
  one, under its call, at ANY level — from Claude Code's own hunks (`toolUseResult.structuredPatch`, with line numbers), else a
  created file's content as added lines from 1, else the call's own old / new strings (no numbers); none for a refused call or
  one with no result. A gutter with each line's number (the new file's for added and context lines, the old file's for removed
  ones), an added line on a green band (rgb 34, 92, 43), a removed one on a red band (rgb 122, 41, 54) to the sheet's edge, `⋮`
  between hunks, a quoted git diff's header dim. Minimal (which hides calls) lists the file-changing calls with their diffs
  only; at Max a Write's diff replaces its content block.
- **☑️ Last Turn** (a box, off by default): only the conversation's NEWEST turn (running now, or the one that ended last),
  numbered as in the whole conversation ("Turn 31"), with its own "before this turn" note and its agents; the chain block and
  session headings go.
- **Parts**: at most **32,000 px** tall each (~1,330 rows; a decoded part stays ~158 MB), a part starting at a turn in the last 40%
  of its room, else at a block in its last 15%, else where the room ends; at most **10** parts (~13,300 rows). A longer
  conversation keeps its MOST RECENT rows: the first picture's top row says how many earlier rows, through which turn, are left
  out, and the thread gets a line naming a shorter level ("A shorter level draws more of it: Default or Compact in 📜 All Screen ▸
  (or /allscreen compact)"; "Without File Diffs the pictures are shorter too."). Drawn through 2,048 px strips, each part PNG-
  encoded strip by strip (memory independent of the height); a run of printable ASCII in one color is one text call.
- **The send**: each part as a DOCUMENT (a photo is shrunk to 2,560 px and refused past 10,000 px of width + height), silent, at
  the answers' priority, queued as soon as drawn (part 1 travels while part 2 is drawn). Caption `📜 <title> · conversation
  (<level>[ + diffs]) [· 2/4] · turns 12–31` (Last Turn: `📜 <title> · last turn (full) · turn 31`); file
  `conversation-<title>-<level>[-diffs][-2of4].png` (`last-turn-…`).
- **The picker**: the card's All Screen pages (§10.4) — a box's tap flips to the page with THAT box flipped and the other kept
  (each tap names the page it shows, so a double tap never flips twice); a level's tap runs `/allscreen <level> [diffs]
  [last]` and the card goes back to Main at once (the pictures, or the line saying why there are none, follow). Typed:
  `/allscreen [minimal|compact|default|full|max] [diffs] [last]` (any order, any case, `diff` too; two levels or an unknown word
  → the usage line naming the default level); a bare `/allscreen` draws the default level.
- **Bounds**: one conversation drawn at a time, process-wide ("Another conversation is being drawn right now; ask again in a
  moment."); a second request for the same session ("This conversation's pictures are being drawn already; they come in a
  moment."); another agent's session refused ("All Screen draws Claude Code conversations; this session is not one."), and one
  with no transcript; a never-messaged fork draws its source's transcript.
- **Measured** (optimized build): 95 MB transcript, 11 turns, Default → 6,945 rows, 6 parts, 33.5 MB, 5.2 s; at Full → 17,295
  rows, the cap (10 parts, turns 1–3 left out), 57 MB, 12.4 s; Last Turn of an 18 MB session → 588 rows, 1 part, 0.9 s. Peak
  commit ~315 MB for the largest transcript (mostly its parse).

### 17.4 The previews' picture

For a question card's 🖼 (§12.1.3): each option's preview framed and stacked under `n · label`, as RAW text (box-drawing
mockups need it), TABs expanded, wrapped at 160 columns; a withheld preview shows Claude Code's own placeholder "(preview
cannot be shown in full — compare the option labels and descriptions instead)", an option without one "No preview available".
Built as a screen snapshot and drawn by §17.1's renderer; `sendPhoto`, silent, captioned `Previews · <header>`.

---

## 18. `/status`: what Claude Code's own /status shows

### 18.1 Out of band

Opening `/status` in the session's terminal would show the dialog on the PC, write two lines into the transcript, need an empty
input box (a draft stashed) and a running claude, and switch tabs by blind keys. So NOTHING is typed: the bridge reads the same
facts out of band.

| Row | Source |
|---|---|
| Version | the heartbeat's `version` while claude runs (only when the row names this conversation); else the transcript's newest response, marked "(its last run)" |
| Session name | the transcript's newest `custom-title`, else its newest `ai-title` (the dialog's own rule); no row when neither |
| Session ID · cwd | the registry (the conversation id; the live cwd, else the working folder) |
| Model | the transcript's `model` attachment — Claude Code's own name and id ("Opus 5.5 (1M context) (`claude-opus-5-5[1m]`)") — when the newest response asked for that id; else the id the newest response asked for (a `/model` switch shows there first); else what the host observed |
| Effort · Permission mode | the newest response's `effort`, the newest `permission-mode` line; else what the host observed |
| Context | the host's context size, else the newest response's input + cache read + cache write + output |
| MCP servers | the `deferred_tools_delta` attachments folded as Claude Code folds them (added names in, removed out, the fold reset at each compact boundary); a server = a tool's `mcp__<server>__` prefix; pending, needs-auth and failed from the newest delta carrying each list, a failure with Claude Code's own reason |
| Memory | the `instructions` attachments (a full list, then updates): each file's path and kind (User, Project, AutoMem, …) |
| Additional directories | the newest `environment` attachment's `additionalWorkingDirectories` |
| Claude | the heartbeat's word (busy · idle · waiting (what for) · shell), the pid and how long it runs; else "not running — the tab is parked; a message here or /activate starts it", "exited — /reactivate starts it again" |
| Account | `claude auth status --text`: Login method, Organization, Email, Anthropic base URL — Claude Code's own words |
| Plan usage | `claude -p /usage`: the limits and the "what's contributing" section as Claude Code prints them |
| This conversation | the transcript's responses, each counted ONCE by its message id (one line per content block repeats the usage): input, output, cache read, cache write tokens; the number of responses and compactions (a synthetic API-error line is no response) |

Not shown (nothing out of band records them): session kind, peer address, cloud sessions, setting sources, managed settings,
organization policy, the auto-mode server; the Config tab; the Usage tab's process cost and durations; the Stats tab. A
byte-level filter parses only the transcript lines that can carry a fact (a 10.8 MB transcript folds in ~0.6 s).

### 18.2 The headless runs

- The child is the claude every session runs, by FULL path (never a PATH shim), hidden, created suspended into a kill-on-close
  job (a timeout or the host's end ends it and everything it started), with only its three standard handles inherited, stdin
  NUL (an open, silent stdin costs a headless claude 3 s), its working folder the profile.
- Its environment is the host's minus every variable of a parent Claude Code session or terminal tab, plus a marker variable
  (`{APP}_HARNESS=claude-status`, so the host's own observers skip it), `DISABLE_AUTOUPDATER=1` (a child must never update the
  user's claude) and `CLAUDE_CODE_DISABLE_AUTO_MEMORY=1` (without it a headless run leaves an empty memory folder behind).
- `/usage` runs with `--no-session-persistence --strict-mcp-config --tools "" --setting-sources "" --settings
  {"verbose":false} --output-format json` (no transcript, no MCP, no hooks, no plugins; one JSON object — a verbose array's last
  result is read too). `auth status` keeps the default setting sources (a base URL the user's settings set shows as the dialog
  shows it).
- **Model safety**: `/usage` is asked only of a claude whose file version is ≥ **2.1.310**; an answer that came from a MODEL turn
  anyway (no `local_command: "usage"`, or turns taken) LATCHES that executable (its path, size and write time) for the run.
- One run at a time per command (a second caller waits for it); a good answer is reused (the account 10 min, the usage 60 s —
  "(read 40s ago)"); a failed one never; time limits 20 s / 60 s.

### 18.3 The report

- In a session's topic `/status` sends the card copy, then — from a worker, as a reply to the command — `📊 Claude Code status ·
  <title>` with its sections Status · Account · Plan usage · This conversation and a footer naming the sources. A limit line
  ("<title>: N% used · resets …") gets a ten-cell bar ▰▱ (rounded half up, at least one cell above 0 %); the "what's
  contributing" section goes into an expandable quote (cut at 2,400 characters). A message holds at most 3,800 characters; a
  longer report is split between whole sections. MCP names stop at 24 and memory files at 8 ("+N more"). A failed read says why;
  an error Claude Code printed is quoted.
- In the Search topic and the main view, `/status` sends the bot's line, then `📊 Claude Code · account and plan usage` (they
  are the account's, not a session's). Another agent's topic gets one line. At most two reports at once.
- Not redacted (§24.3): the report carries the account's e-mail address and the PC's paths.

---

## 19. Voice notes

### 19.1 What happens

1. A voice note (an audio file, a round video note) in a session's topic — or in the Search topic — gets **👀** (heard).
2. The voice thread downloads it (`getFile` — paced by the chat's slot — and the file download, ≤ **20 MB**), transcribes it with
   the SESSION'S VOCABULARY (§19.3; a Search note's is the FLEET's), runs the dictionary pass (§19.4) and the correction
   (§19.5), and deletes the file.
3. The transcript is posted silently under the note: `🎤 <i>Check why the ConPTY host drops …</i>` (a setting); a correction that
   ran and failed adds `⚠️ not corrected: <why>` and the engine's words go on.
4. The note is then read EXACTLY like a typed message of that topic: a queued prompt whose reactions land on the NOTE, or the
   answer an open question card takes, or the backlog question for a note sent while the bridge was down — judged by when the
   note ARRIVED, so a slow transcription never makes a live note backlog. In the Search topic it is a search typed there
   (§14: a request, `/clear`, a fleet command; "Searching…", then the answer), with the backlog rule judged at arrival too. A
   caption typed under the recording follows the transcript.

### 19.2 Routing and order

- A note goes where a typed message of its thread goes: a session's topic → transcribed; the Search topic → transcribed with
  no session (the fleet's vocabulary, §19.3) and read as a search typed there; a thread whose bind is handing over → held in
  the bind; a thread no session owns (writing in "All" opens one) → HELD by the new-thread wizard, untranscribed (its folder
  picker goes up with it: "Your message waits here and becomes the session's first prompt"), and transcribed at the hand-over
  WITH the new session (its folder and CLAUDE.md are the vocabulary), then queued as its first prompt, read as a held message
  (no backlog question); the main view, a recreated topic's old thread → answered, never queued (each answer says where a note
  IS read: the main view's names the session topics, the Search topic and a new thread).
- Answered at ARRIVAL, never after a wait (a held note too): voice notes off, no transcriber, the bot stopping, no file, over
  20 MB. A failed transcription: "🎤 I could not transcribe this voice note: <why>".
- **Order**: a message of the same thread that arrives while a note is being transcribed WAITS behind it in that thread's lane
  (an edit replaces the held message) and is handed on in order by the tick. One note at a time across every topic (one
  engine).
- **Stop** kills the engine first (a note in flight fails fast), then drops what the lanes still hold (logged; the owner sends
  them again).

### 19.3 The session vocabulary

| Source | Weight | Respell everyday words into it? |
|---|---|---|
| A built-in developer glossary (~170 terms, in PRIORITY order — AI and speech, audio and media, hardware and OS, data formats, languages, tools, databases, generic words — so every use that truncates cuts its END first) | 1 | yes (curated) |
| The session's folder name | 3 | yes ("tele bridge" → TeleBridge) |
| The hard names of the folder's CLAUDE.md (the nearest one, up to three parents), at most 60, most frequent first | 2 | no |
| The session title's hard words | 2 | no |
| The owner's dictionary (`voiceDictionary`) | 3 | yes, plus its explicit triggers |

- **A hard term** is one a speech model is likely to get wrong: it holds a digit or `+` / `#`, or one of its case-split parts is
  not everyday English (`ConPTY` → Con + **PTY**). **Everyday English** is asked of the OS's own spell checker (en-US): an
  ALL-CAPITAL word of 2+ letters only by its lowercase spelling ("ON", "AND" are words; "PTY", "DB" are not); any other word by
  its lowercase OR title-case spelling (proper nouns are capitalized there); no dictionary → every word reads as known (nothing
  is respelled).
- **Mining CLAUDE.md** keeps a hard, distinctive word seen at least twice, and only SPEAKABLE names: no code-only spellings (an
  underscore, `::`, a file name, a cmdlet, an error code), no shouted English, no two-letter acronym seen fewer than 8 times, no
  compound of everyday words, no `lowerCamelCase` identifier, nothing of three or more word parts, no two-character token that is
  not an acronym (identifiers had crowded the speech prompt and pushed out the words a note did hold).
- **Four uses**: (1) the speech model's prompt — `A developer dictates a prompt to Claude Code. Glossary: <terms>.`, highest
  weight LAST, cut from the start at ~700 characters (Whisper keeps the last ~224 tokens); (2) a keyword-spotting engine's
  hotwords — hard terms only, scores 2.0 / 2.5 / 3.0 by weight, at most 96 (more made it worse, and beam search with hotwords
  invented words); (3) the dictionary pass on every engine's text; (4) the correction's glossary — at most 160 terms, highest
  weight first.
- **The owner's dictionary**: one entry per line, `Word` (boost it, respell its generated forms) or `spoken form, other form =>
  Word` (an explicit trigger); `#` starts a comment.
- **A note in the Search topic** goes to no session, so it has no folder, title or CLAUDE.md of its own: the names a spoken search
  says are the FLEET's. Its vocabulary is the glossary, the name of every folder a session worked in (live and closed sessions,
  the launch and the inferred folder, each once by the folder key, the most recently active first) by the folder rule above
  (weight 3, respellable; a lowercase or everyday name adds nothing), the open tabs' titles (the most recent first, each once)
  by the title rule (weight 2), and the owner's dictionary. At most **40** folder names join and **40** titles are read — the
  prompt, the hotwords and the glossary are shared with the built-in glossary, and a request names a project or two. The
  setting that keeps a session's folder out keeps the fleet's names out too.

### 19.4 The dictionary pass

Windows of 1–4 words, longest first, left to right; each window SQUASHED (lowercase, letters and digits, `+` / `#` kept) and
looked up among the curated triggers, the canonical spellings and the GENERATED spoken forms of every distinctive term (its
case-split parts "con pty", its acronyms spelled out "con p t y", a long name's two-part splits "tele bridge"). A single
everyday word is never rewritten; several everyday words respell only into a CURATED term (the folder name, the glossary, the
owner's dictionary — never a mined identifier); a curated trigger is a decision someone made and passes ("clawed" → Claude,
"jason l" → JSONL). Punctuation around a window is kept; a canonical window is left alone (the pass is idempotent — except
beside a mined term that holds a curated one: with a title's `ConPTY-fix`, "con pty fix" becomes "ConPTY fix", and only a SECOND
pass would make it "ConPTY-fix"; the bridge runs the pass once per hearing).

### 19.5 The correction (fusion)

- ONE hidden `claude -p` per note, given the glossary and every engine's HEARING of the note, returns the same words with what
  was misheard fixed. The user message: `Glossary: <terms>` then `Transcripts of the same recording by different speech engines:
  A: … B: …` (each after the dictionary pass), or `Transcript:\n<text>` for one hearing (an empty or identical alternative is
  dropped).
- **The system prompt's rules**: keep the speaker's words, their order and their meaning — never add, answer, summarize or
  rephrase; fix only recognition errors (a word that SOUNDS like a glossary term becomes that term with the glossary's spelling; a
  word that makes no sense in a developer's prompt becomes the similar-sounding word that does — accents turn "thirty" into
  "dirty"; identifiers and acronyms get their usual casing; a stray filler the grammar rejects goes; numbers stay as written;
  punctuation is fixed); several transcripts are independent hearings — keep what they agree on, and for each disputed word
  choose the reading that fits the sentence, a developer's prompt and the glossary; nothing to fix → the transcript unchanged; the
  corrected message only. Its examples name no term of the evaluation sentences.
- **The call**: `claude -p --model <model> --no-session-persistence --output-format json --strict-mcp-config --mcp-config
  {"mcpServers":{}} --system-prompt-file <file> --tools "" --permission-mode dontAsk --setting-sources "" --settings
  {"verbose":false,"env":{"CLAUDE_CODE_MAX_RETRIES":"2","CLAUDE_CODE_RETRY_WATCHDOG":"0","CLAUDE_CODE_EFFORT_LEVEL":"low"}}
  <user message>`, run like §18.2's children (hidden, kill-on-close, NUL stdin, scrubbed environment, the marker variable,
  working folder `<profile>\voice`): no tools, no MCP, no settings, no CLAUDE.md, nothing of the session's repository.
- **The guard**: the answer replaces the text only when it is not blank and its length is within `0.6 × before − 10` … `1.5 ×
  before + 20` characters (a model that ANSWERED the prompt instead of correcting it is caught by its length).
- **Failure keeps the note**: no claude, a start failure, 60 s without an answer, an error result or a refused answer → the
  dictionary pass's text, and the echo says why. The thread's next message waits behind it.
- **Measured** (the owner's three recordings, 71 words, 14 terms): Whisper large-v3-turbo + the glossary 5.6 % WER; + the
  dictionary pass, every term spelled right; + the correction on the transcript alone 4.2–5.6 %; **+ a second engine's hearing
  (Parakeet on the CPU) → 0.0 % WER, 3 runs of 3**. Sonnet held 0.0 %; Haiku moved between 2.8 and 4.2 %. The correction adds
  3–11 s per note.
- **Privacy**: the TEXT and the glossary reach the model provider through the PC's own Claude Code login — the same text then
  becomes the session's prompt, which goes there anyway; the audio never does. `voiceCorrectionModel = off` stops it.

### 19.6 Engines

- **A voice worker** (the local engine): any program that speaks JSON lines on stdin / stdout — `hello` (load the engine; say
  whether it takes a `prompt` and `hotwords`), `transcribe {path, language, prompt, hotwords}` → `{ok, text, audio_ms, decode_ms,
  engine, alternatives?}`, `quit`. stdout carries only protocol lines (anything else is logged and skipped); every request gets
  exactly one answer with its id; a failure is `{"ok":false,"error":…}`, never an exit. `alternatives` (optional) carries the
  worker's OTHER engines' hearings for the fusion. The bridge sends both `prompt` and `hotwords`; the vocabulary, the dictionary
  pass and the correction stay in the bridge, so every engine gets the same corrections.
- One worker per process: started hidden by the first note (suspended → a kill-on-close job → resumed), only its three pipes
  inherited, its stderr into the voice log, `PYTHONUNBUFFERED=1`, `PYTHONIOENCODING=utf-8` and the marker variable
  (`{APP}_HARNESS=voice-worker`). A transcription's deadline is 60 s + 4 s per second of audio (≤ 15 min); a missed deadline ENDS
  the worker (the next note starts a fresh one); it ends after `voiceWorkerIdleMinutes`; a settings save that changes the engine
  kills it without waiting.
- **An endpoint** (`voiceEngine = openai`): one multipart POST per note to an OpenAI-compatible `/v1/audio/transcriptions` with
  `model`, `language`, `prompt` (the glossary line), `response_format=json`, `temperature=0`, the note as `voice.ogg`; its key in the
  credential store. ⚠ A cloud endpoint receives the owner's voice: never the default.
- The reference's best local command: Whisper large-v3-turbo on the GPU with `--fuse` Parakeet TDT 0.6B v2 (beam + hotwords) on
  the CPU. A bundled native worker (Parakeet through sherpa-onnx's C API on the CPU, an Ogg/Opus decoder, the model downloaded
  on first use and checked by SHA-256) is designed, not written.
- ⚠ Windows: the spell checker's COM object MUST be released before the voice thread's `CoUninitialize` (released at thread exit
  it calls into an unloaded server).

---

## 20. File links and `/download`

### 20.1 What the user gets (off by default)

- Every `[name](file:///K:/…)` link in CLAUDE'S OWN TEXT (its replies, remarks and plans, live and in a history replay) to a file
  that exists, is no folder, looks like no secret and lies in no excluded folder becomes a download link:
  `<base>/station-<station>/<session key>/<encoded path>`. Telegram shows an ordinary link; a tap downloads the file (served as
  an attachment). Every other link, a link inside code, and a file that fails a check stay as Claude wrote them. The user's own
  prompts are never touched (the mirror matches them against the queue).
- A link lives **30 days** after Claude last mentioned the file (a new mention renews it), at most **4,000** files a session.

### 20.2 The link

- `<encoded path>`: the file's own path, the drive as its first segment, each segment percent-encoded: `K:\source\x\report.md`
  → `/K/source/x/report.md` (the name LAST: an in-app browser names the download after the URL's last segment).
- **The session key**: SHA-256 whose eight initial hash words are replaced by the big-endian words of a 32-byte SECRET, over
  `lower(station) + ":" + lower(session id)`, as 64 lowercase hex characters. The secret comes from the system RNG the first time
  file links run and lives only in the credential store (never a file or a log). The construction is public (the code may be);
  the secret is what makes it a key — with known initial words anyone who saw a session id could compute it. Deleting the secret,
  or a new station name, voids every link.
- A path never appears in a URL without its session's key, and a key opens only the paths LINKED in that session (the "linked
  set", persisted: `filelinks.json` = `{"v":1,"sessions":{"<id>":[["<path>",<ms>],…]}}`, written at most every 10 s when it
  changed, at stop and at the host's last window; pruned hourly; a damaged file starts the set empty).

### 20.3 The server

- One HTTP listener per installation while the bot runs and file links are on (e.g. port 32594 for one installation, 32595 for a
  second): binds `0.0.0.0:<port>` exclusively; answers loopback and private (RFC 1918) peers only; at most 24 connections (503
  beyond); a 15 s read and a 60 s write timeout; `GET` and `HEAD` only (405); `/healthz`; ONE URL shape (anything else 404).
- **The authorizer**: the station is this installation's; the key is one of its sessions' (the key → session map is rebuilt from
  the linked set at every start, so links survive restarts); the path is in that session's linked set and not expired; the name
  and folder rules still pass (a folder excluded later counts).
- Then the server opens the file sharing every mode, refuses a handle that is not a disk file (a device), re-checks the FINAL
  path (a symlink into a secret or an excluded folder, or a path Windows cannot name) and streams it: `Content-Disposition:
  attachment` (an ASCII name and the exact UTF-8 one, RFC 8187), `Range` (206 / 416), `Cache-Control: no-store`, `nosniff`,
  `noindex`, `no-referrer`. One log line per request (status, method, station, key prefix, name, session, peer, the forwarded
  client address).
- **Never linked**: a name or folder that looks like a secret (`.env*`, keys and certificates, tokens, credentials files, `.ssh`,
  `.aws`, `.kube`, the user's own secrets folders, Claude Code's `.claude.json`, …), an excluded folder, a UNC share or a relative
  path, a climb, an escaped separator, a stream name, a reserved device name (`COM1`, `nul.txt` — a read would open the device),
  a segment that ends in a dot or a space (Win32 strips it: `key.pem.` would open `key.pem` past the secret check).

### 20.4 Two modes

- **lan**: the base is one of the PC's private addresses, `http://<address>:<port>/` (a dropdown lists them, physical adapters
  first, and the PC's name; a picked address that is gone shows as stale). Works on the home Wi-Fi and through a VPN into the LAN
  (Telegram's in-app browser and Android's DownloadManager accept `http://`). One inbound firewall rule for the port from the LAN.
- **hosted**: the base is `https://<relay host>`, a public name behind an EDGE that lets through ONLY `GET|HEAD
  /station-<a listed station>/<64 lowercase hex>/<path>` with no query string (checkable without regular expressions with
  `starts_with`, `len`, `substring` and `remove_bytes` on a free CDN plan) and refuses every other URL of the name; then a tunnel
  to a small relay that checks the same shape again (by a regular expression) and maps station → address ("the station is
  offline" 503 when it is down); then the station's server. A well-formed but WRONG key passes the edge (it cannot know the
  sessions) and gets 404 at the station.
- ⚠ Open: Android's DownloadManager uses the platform TLS, which may not pass an edge that demands a post-quantum key exchange;
  the fallbacks are Telegram's "open in an external browser" list for the domain, or no such guard for this path.
- Settings: the mode, the station (an installation's default), the port (0 = the default), the LAN base, the hosted base (only
  `http(s)://` and URL-safe characters), the excluded folders (one per line); the settings UI previews a link's shape, says what
  is not valid and shows the server's status.

### 20.5 `/download <path>`

In a session's topic (absolute, a `file:///` URL, or relative to the session's folder — claude's own process folder, else the
launch folder): answers the file's name and size, a ⬇ Download link, the path, and a `[📤 Send as file]` button (`dl:<offer>`):
the file is read again on a worker (still there, still allowed, ≤ 50 MB) and uploaded as a document from the PC; the tap stays
busy until it is in. A bare `/download`, a folder, a secret, an excluded or a missing file is answered with the reason; the
Search topic says it works in a session's topic. Telegram's send-by-URL is not used (PDF / ZIP ≤ 20 MB only, and an edge rule
would refuse Telegram's servers). An upload's send timeout grows with its size (30 s, or the body at 256 KB/s: ~205 s for
50 MB).

---

## 21. The outbox: pacing and flood control

### 21.1 Priorities

| Priority | Carries |
|---|---|
| **Final** (0) | final messages, questions, plans, the prompt lines (so a fast turn never posts its reply first), answers to the user's commands, a tapped card's flip, the answers to taps (ahead of every other Final item) |
| **New** (1) | new prompts and work-log messages, topic creation's posts, change lines, idle notices |
| **WorkLogEdit** (2) | work-log edits |
| **CardEdit** (3) | card edits and re-posts, prompt-status edits |
| **Backfill** (4) | the directory, deletes of service notes, files, reactions, the "/" menu |
| **History** (5) | the history replay: taken only when nothing else is queued |

### 21.2 Order and keys

- Items are served by `(priority, seq, subSeq)`: a message's parts share its `seq`; part `j` sits at `j × 4096`; its HTML fallback's
  chunks follow it; a refused chunk's plain-text fallback takes the slot right after it — a fallback lands IN ITS PLACE, never
  after later messages.
- An item may carry a KEY; a newer item with the same key REPLACES a queued one (and takes over its tap holds): card edits per
  card, prompt statuses per message, reactions per message (`react:<message id>`), the directory (`dir:post`, `dir:pin`,
  `dir:edit`), a copy's keyboard (`kb:<message id>`), a question card (`qcard:<key>`), a wizard's picker. A fallback is sent
  WITHOUT a key (a key would erase a newer queued item).
- A pre-send hook lets an item read facts that only exist when it GOES (a reply's prompt message id; a question card's latest
  render).
- A topic's delete drops every item still queued for its thread; a Rekey drops the old conversation's queued history.
- A token that reached the outbox unrendered (a send path that bypassed the renderer) is rendered classic there and logged once.

### 21.3 Pacing

- ONE call at a time, at least **1.05 s** apart, for every call into the chat — messages, edits, deletes, reactions, topic
  calls (the tick thread's direct calls take the same slot, and the slot answers "no" while stopping: a call started then would
  hold the stop for its whole timeout). Calls that put nothing into the chat (the "/" menu) MAY go unpaced.
- **Nothing periodic edits.** Clock times, not spans; hashes, not "maybe changed"; the work log's gap grows with the number of
  open turns.

### 21.4 The call window

The stamps of every call into the chat in the last **60 s**. Background traffic goes only while it holds room under **18** (the
measured sustained budget): the history replay (fewer than 18), renames (room for their two calls), re-sorts (room for two),
card form upgrades (room for one), the directory. Live traffic never checks it: it always goes first.

### 21.5 429s

- A `retry_after` of at most **60 s** is slept through by the thread that met it (stop-aware), then the same call is made once
  more — the outbox keeps its order.
- A longer one **parks only the refused METHOD** until its deadline: its items stay queued in their places (an edit that a newer
  edit of the same message replaced meanwhile is dropped; no callback runs until the item really goes), and every other method
  goes on. A long 429 on a SECOND method while one is parked means Telegram holds the whole chat: every method parks until the
  later deadline. (Measured: one hold covered every method of the chat with one deadline — the parking keeps a hold that covers
  only one method from silencing the others; a hold on the chat cannot be worked around.)
- After any 429: the history replay pauses **5 min**, the directory backs off 5 min, card form upgrades stop 5 min; a rename
  refused by the flood control stays due; a closed topic's delete refused that way keeps its record.
- The secretary bot's flood control is its own: its sends park separately.
- Logged: `pacing: 429 on editMessageText (retry after 6542 s): editMessageText waits until 18:50; the other calls go on` /
  `… a second method refused — every call waits until …`.

### 21.6 Other failures

- "can't parse entities" → the plain-text fallback in place. A rich refusal → classic in place (§9.8). "message is not modified"
  → done. "message to edit not found" → the message is forgotten (a card or the directory is posted again). "message thread not
  found" → the topic is gone (the Search topic is re-created; a session topic: §27). `TOPIC_NOT_MODIFIED` → done.
- 401 (the token revoked) → the poll loop stops; the status line says so.
- A call with NO answer at all (DNS, TLS, a timeout) goes once to the item's callback: the card and the start note retry, a
  rename stays due; other messages are not retried (§27).
- 409 on `getUpdates` → §3.3.

### 21.7 The traffic census

Every 10 minutes one log line: calls per method, 429s, edits by kind (`card`, `prompt`, `worklog`, `directory`, `other`), and the
card pictures (`drawn`, `kept`, `none`, `reused`, `skipped`, KB drawn). It is where the chat's real call rate is read. Expected:
a quiet fleet makes a handful of calls per 10 minutes; a replay at its cap ~180 per 10 minutes; a mass creation of 413 topics ran
at ~45 calls a minute without a 429.

---

## 22. Persistence

### 22.1 Files

| File | What |
|---|---|
| `<profile>\messaging\telegram\state.json` (+ `.prev`) | The update offset, the bot's identity, the Search topic, the topic map, the directory message, the business connection, the recreation's orphans, the new-thread wizards and launches (Appendix C). The Bot API cannot list topics: this is their ONLY record. |
| `<profile>\messaging\telegram\filelinks.json` | The linked set (§20.2). |
| `<profile>\messaging\telegram\voice\<message id>.oga` | A voice note while it is transcribed; deleted right after. |
| `<profile>\voice\` | The correction's system-prompt file and working folder. |
| `<profile>\messaging\run.marker` · `run.marker.last` · `run.closed` | §23.3. |
| The credential store | §6.2. |
| The host's session store | per session: `pinned`; per queued prompt: `remoteSource`, `remoteQueuedBusyUnixMs` (omitted at 0); per session: `createdUnixMs`. |

### 22.2 Saving

- **Two kinds of change**: STRUCTURAL — anything the next start cannot learn again (a topic made, re-keyed or deleted; a card's
  message id; the update offset; a bind; a wizard; a held change line) — is saved within **2 s**; PROGRESS — the mirror's
  cursors, recap facts and prompt-line fields, a work log's new id, a topic's last-post clock, a card's hash, its picture — within
  **10 s**. A structural save takes pending progress with it. ⚠ A new persisted field MUST decide its kind: progress only when
  losing its last 10 s is harmless; anything that would orphan a message or a topic is structural.
- A change is marked only when it HAPPENED: the tick writes back the fields the mirror owns (cursor and path, the work-log
  message, the recap text and time, the last message time, the error flag, the last prompt's fields) each compared first
  (an unconditional write-back once rewrote a 465 KB file every 3 s: 279 KB/s, ~24 GB a day). Fields the tick never writes back:
  the thread, the card's id, its clocks and hash, the title, the tab, the name's color.
- Bytes equal to the file's are never written. A save is atomic (a temp file, flushed, a write-through rename). `.prev` is
  refreshed at the first save of a start and then at most once a minute, never from a file this run could not read. One writer
  at a time (the save lock is taken before the service lock).
- **A normal quit loses nothing**: the host's last window closing forces a save (each lock waited for at most 250 ms; a give-up is
  logged). The trade-off of the 10 s gap: after a CRASH up to 10 s of progress is lost — those seconds' mirrored lines are posted a
  second time, a card may be edited once more.

### 22.3 Loading

- A tolerant reader (missing members = defaults; an older file reads cleanly; a newer member never retypes an old one).
- A LOCKED file is retried, never read as "no topics" (which would orphan every topic, and the first save would write the empty
  map over the file). An unreadable, torn, or not-a-state file falls back to `.prev`, and the file is marked
  keep-before-replace: the next save refuses while the file cannot be read, and copies an unparseable one aside (`state.json.
  could-not-read-<stamp>`) before replacing it. No size cap on the reader.
- At start, "in flight" markers (`-1` message ids) left by a stop mid-send are cleared.

### 22.4 In memory only

Lost at a restart, each by design: action tokens (an old button answers "That button expired."), the card pages (every card shows
Main), tap holds, launch answers and revives (an answer after its revive gave up is only logged), steer and `/add-dir` watches,
the idle notices still pending, the last 200 Search rows of this run (a pick still works through the registry), question cards
(their requests end with their hooks; an old card's buttons answer "This question is no longer open here.").

---

## 23. Lifetime: start, quit, crash

### 23.1 Start

The credentials → `getMe` (the identity gate, §6.3; topics on? if not, `getMe` again every 60 s, a status line, one setup notice
a day) → the state → the Search topic (created when missing: it takes the call slot, respects a parked method and waits 60 s
after a refusal) → the "/" menu → the start note → the start-up hold → the topic sync → the directory. The first marker write of a
run copies a marker still on disk to `run.marker.last`.

### 23.2 The start note

Every launch posts ONE note in the Search topic, one fact per line:

```
🟢 {app} started · dev 0.0.1.29 (was dev 0.0.1.28)
⚠ The last run did not close normally (a crash, a forced stop or a Windows shutdown): last seen running at 00:06 · offline about 1m.
48 topics kept from the last run.
```

| The start | Heading | Sound |
|---|---|---|
| the bot's first start within 5 min of the process's start (an app launch) | 🟢 **{app} started** | yes — the point is to notice restarts |
| the bot's first start later (messaging switched on in a running app) | 🟢 **The Telegram bot started** + "{app} has been running since …" | no |
| a later start in the same process (a settings change restarted the bot) | 🔄 **The Telegram bot restarted** + "{app} kept running; a Settings change restarted the bot." | no |

- After a clean quit the second line reads "The last run closed at 23:58 · offline 9m." A different version in the previous run's
  marker adds "(was <old version>)" — the one sign that a restart was an UPDATE, since a hot swap stops the app like a crash. A
  note sent 2 min or more after the bot started adds "⏳ Sent late: the bot started at 08:01, 14m ago, and could not post until now
  (no connection to Telegram, or its topics were off)". Every time is a `tg-time` WITH its clock text inside (an entity over no
  text shows nothing); the text is built at each attempt with that attempt's clock.
- Delivery: a passing failure (no answer, a 5xx, a 429 that outlasted the one wait) is sent again after 30 s, the wait doubling up
  to 10 min, 12 sends in all; "message thread not found" (the user deleted the Search topic) forgets the topic — the next tick
  makes a new one, with its intro, and the note goes there; any other refusal is logged and dropped. Logged when gathered, queued
  and posted.

### 23.3 Quit or crash

- The run marker `messaging\run.marker` = `"<unix ms> <version word>"` is rewritten every **60 s** (an older reader takes its first
  number).
- When the host's LAST window closes (the app is quitting), the bridge writes `messaging\run.closed` = `"<unix ms>"`, holds marker
  writes for **30 s** (a marker written between the stamp and the process's end would make a quit read as a crash), and forces a
  state save (§22.2). Past the 30 s the app evidently goes on (the last tab torn out into a new window), the next marker outdates
  the stamp, and a later crash reads as a crash.
- The previous run closed cleanly only when its stamp is at or after its last marker write. A crash, a forced stop, a Windows
  shutdown (`WM_ENDSESSION` ends the process without closing windows) and a power loss leave no stamp. Both files are read AND
  removed when the note's facts are gathered.
- ⚠ An app that ends with `TerminateProcess` never runs the bridge's `Stop`: everything a clean exit must do happens at the last
  window's close.

### 23.4 Stop

Voice first (the engine killed, its lanes dropped), then the question hooks released (`{}` to every open one), the threads
joined (an abort makes the long poll return at once), the file-link server stopped. Two installations run two bridges with two
bots, apart.

---

## 24. Security and privacy

### 24.1 Threats and rules

| Threat | Rule |
|---|---|
| A stranger messages the bot or taps a button | dropped after reading only the sender id (I1); a tap answered "This bot is private." |
| A stale button, an old message | single-use expiring tokens or state-reading verbs (I5); the backlog rule (I8) |
| Replayed updates | the offset is saved once each update was taken or refused; actions carry ids, so repeats are dropped |
| A leaked token | the thief can read new messages first and post as the bot (a 409 the bridge did not cause stops its loop and says so); rotate the token in BotFather |
| A parser bug fed by internet input, inside the process that types into every session | network work on worker threads; bodies capped before parsing (`getUpdates` ≤ 2 MB, JSON nesting bounded, strings capped); every thread body and callback contained; callback data parsed strictly (fixed grammars, digit counts, hex ids) |
| A typed model id or path reaching a command line | checked before any launch (§15.3); paths never in callback data |
| Secrets in mirrored text | ⚠ not redacted in the reference (§27) |

### 24.2 What reaches Telegram

- A topic carries the whole session: every prompt and reply of every listed session goes to Telegram's servers (cloud chats, not
  end-to-end). The verbose mode adds tool calls; pictures carry the terminal's screen; `/status` the account's e-mail; file links
  serve files. The settings UI SHOULD say so plainly.
- A **redactor** SHOULD run in the bridge before any text reaches the outbox (GitHub, Anthropic, OpenAI, AWS, Slack, Google,
  Stripe and Azure key shapes, JWTs, PEM private keys, a Telegram bot token's shape, high-entropy values after `password` /
  `secret` / `token` / `api_key` → `[redacted <kind>]`), and over a screen snapshot's rows BEFORE drawing (a picture cannot be
  redacted afterwards). Not built in the reference.
- Host sessions often run with `--dangerously-skip-permissions`: **a remote prompt is remote code execution on the PC**. The owner
  check is the only gate in the reference (permission levels and a drive PIN are designed, not built: §F.3).

### 24.3 Logs

Never a message body, a prompt's words, an answer, a note's text or a token: logs carry ids, lengths, counts, outcomes, and
the user id hashed where a user is named.

---

## 25. Logging

| Log | Lines (examples) |
|---|---|
| the host's main log | `[msg] messaging started (owner …, idle grace 10 min, topics conversation only by default, rich messages)` · `[msg] connected as @bot (topics enabled)` · `[msg] topic created <sid8> ⚡️ “🟦 title” (thread N, most recent HH:MM)` · `[msg] topic renamed <sid8> → 💬 “🟦 title”` · `[msg] topic deleted <sid8> (closed\|idle)` · `[msg] topic carried over <old8> -> <new8> (the same tab …)` · `[msg] <sid8> queued from topic (message N, M chars; waits: …)` · `[msg] <sid8> card re-posted with sound (<state>)` · `[msg] start-up hold: …` / `… ended …` · `[msg] history replay of <sid8> started\|goes on: bytes A to B …` · `[msg] history in for <sid8>: N message(s) …` · `[msg] re-sort …` · `[msg] directory posted\|pinned\|edited …` · `[msg] idle notice in the main view: N session(s) went idle (<ids>)` · `[msg] start note: …` / `… queued …` / `… posted (message N)` · `[msg] traffic (10 min): …` · `[msg] tap <label>: answer queued N ms after the tap (…)` · `[msg] screen picture of <sid8>: <w>x<h> px, <n> KB, <font>, drawn in <t> ms` · `[msg] conversation pictures of <sid8>: <level>[+diffs][+last], N of N part(s) sent, …` · `[msg] status report of <sid8>: …` · `[msg] <sid8> voice note queued for transcription …` / `… transcribed (…)` · `[msg] new thread N: …` · `[msg] launch token=<n>: the window opened\|had\|refused …` · `[ask] <sid8> question open (request N) …` / `… answered from Telegram (…)` / `… answered at the PC …` · `[menu] <sid8> /add-dir dialog …` · `[claude-status] account\|usage: …` · `[file-link] <status> <method> <station> <key8> "<name>" …` |
| the navigation trail | `[nav] remote telegram <verb> <sid8> -> <outcome>` for every action from the chat (a begin and an end for anything that launches or tears down a process) |
| the transport log | `[tg] …` — HTTP status, latency, `retry_after`, poll gaps, slow calls; never a body or a token |
| the voice log | the worker's stderr, start / stop / idle end, each transcription's timings, other hearings, respellings and the correction's outcome |
| exceptions | every caught exception with its throw site (nothing is swallowed silently) |

---

## 26. Testing

- **Pure decision functions with unit tests** carry nearly every rule of this document: the line classifier, the topic sync plan
  (tabs × states × pins × holds → creates / renames / rekeys / deletes, the grace), MOST RECENT ranking and the re-sort plan, the
  name / icon picker, the color square and chip style, the change line and its hold, the card text and buttons per state, every
  callback grammar (round trips, the 64-byte bound, every malformed form), the command router and its tables, the backlog rule,
  the queue placement and the "waited" decision, the steer decision, the history renderer and window splitter, the rich
  preparation and chunking, the title tokens and both renders, the directory render / hash / decision, the wizard's reducers and
  folder resolution (over an injected file system), the question card's model / steps / composer (against Claude Code's own
  answers as goldens), the `/add-dir` screen parser (against captured screens), the `/status` readers and layout, the vocabulary,
  dictionary pass and correction prompt (against a reference implementation's goldens), the snapshot format, the picture layout
  and part planner (decoded back), the file-link rules and server (on loopback), the state file's round trip with older files.
- **Live probes of Claude Code** (a real TUI on a PTY in a throwaway folder; re-run after every Claude Code upgrade that touches
  hooks, `AskUserQuestion`, `/add-dir`, `/status` or `/usage`): the PermissionRequest scenarios (PC answers, hook answers, notes,
  tabs, Cancel, reply), the `/add-dir` dialog and its echoes, the headless `/usage`. The parsers key on texts the probes record.
- **Corpus checks**: every real `AskUserQuestion` call of the machine replayed through the card (every tab, every state, every
  outcome within the limits; real answers recomposed byte for byte).
- **A fake Bot API** (planned): topics, rich messages, reactions, edits, 429s, a backlog, deleted topics, `getFile`; strangers,
  replayed updates, stale tokens, huge and deeply nested JSON, a 409 storm.
- **The checks after a build**, by hand on the phone: each feature's list (the phone's rendering cannot be unit-tested).

---

## 27. Known gaps and open items

| # | Gap |
|---|---|
| G1 | A session's topic the user DELETES is never re-created: the record stays, every post into the thread fails, the card is posted again every 60 s, until the session goes idle past its grace or closes. |
| G2 | A send that gets NO answer (DNS, TLS, a timeout) is lost for messages without a retrying caller (a final reply, a remark, a prompt line). |
| G3 | 403 (the user blocked the bot) is not handled: each send fails on its own; the status line does not say so. |
| G4 | The topic's `/newsession [model]` / `/forksession [model]` and Search's `/newsession <folder> [model]` take ANY text as the model (the wizard checks its typed ids; these do not), and a `/newsession <folder>` typed into a new thread BEFORE its folder pick replays as the new topic's own command at the bind. |
| G5 | The window-hosted `start` (a Reactivate, a queued message that starts a parked tab) reports no refusal. |
| G6 | No redaction (§24.2) — text, pictures and `/status` go out as they are. |
| G7 | Plans and permission prompts are not answerable from Telegram. |
| G8 | ⎋ Esc confirms the key was WRITTEN, not that the turn stopped (the card's next state shows it). |
| G9 | A tab closed while its dying claude is re-adopted by the host's observer can come back live and get a new topic (a host-side bind race). |
| G10 | A never-prompted session restores under a NEW id, so a launch restored from the state file can time out and return its thread to the wizard. |
| G11 | The wizard's window step counts other agents' tabs like any tab. |
| G12 | Every restart re-edits ~2–3 times per card (the restore's own state churn); a restart of a large fleet costs ~100–130 card edits in 10 minutes. |
| G13 | Markdown tables in a turn draw as pipe rows in All Screen pictures (rich messages render them). |
| G14 | A message the user deleted "for me" only stays with the bot and is still delivered when held. |
| G15 | The Search agent has no in-place "activate" tool; a request in words reaches its `resume`, which only says where the open tab is. |
| G16 | On Android a title link lands in "All": the dot / front move is the way in (§16.4). |
| G17 | File links (hosted mode): whether Android's DownloadManager passes a post-quantum-only edge is untested. |
| G18 | A `/status` for a session started from another claude executable reports the bridge's claude's account and usage. |
| G19 | The idle notice still pending at a quit (≤ 60 s) is lost. |
| G20 | The question helper costs ~93 MB per open question (a PowerShell host); a native helper would hold a few MB. |

---

## Appendix A — Constants

| Name | Value | Where |
|---|---|---|
| Tick | 1.5 s (or on a registry change) | §5.1 |
| `getUpdates` timeout / receive timeout / limit | 40 s / 60 s / 100 | §3.3 |
| `getMe` retry while topics are off | 60 s | §3.1 |
| Outbox gap per call | 1.05 s | §21.3 |
| Call window / background budget | 60 s / 18 calls | §21.4 |
| Short vs parked 429 | ≤ 60 s slept, > 60 s parked | §21.5 |
| Pauses after a 429 | history 5 min, directory 5 min, card upgrades 5 min | §21.5 |
| Topic operations per tick | 6 | §8.2 |
| Idle grace | 10 min (0–1440) | §8.1 |
| Waiting → Idle decay (host) | 60 min, once read | §4.4 |
| Start-up hold | ≥ 120 s, while any window restores, ≤ 30 min | §8.11 |
| Name tick: settle / gap / cost | 8 s / 30 s / 2 calls | §8.5 |
| Rename hold of the replay | ≤ 90 s | §8.5 |
| Color-mode quiet window | 60 s | §8.6 |
| Idle notice settle / max wait / titles | 10 s / 60 s / 20 | §8.8 |
| Re-sort cadence / quiet / max | 60 s / 30 s / 24 | §8.9 |
| Card edit gap / sticky lag / failed re-post | 20 s / 6 s / 60 s | §10 |
| Card form upgrade | one card per 10 s | §10.5 |
| Card picture wait / refusals to stop | 4 s / 3 | §17.2 |
| Card picture looks: settle / retries / look wait / max looks | 3 s / 5, 10, 20, 40, 60 s / 30 s / 8 | §17.2 |
| Work-log edit gap / rollover | ≥ 15 s (2 s × open turns) / 3,200 chars | §9.4 |
| Seed window | 256 KB | §9.3 |
| Transcript resolve retry | 2 s busy / 20 s at rest | §9.3 |
| Max read per tick | 4 MB | §9.3 |
| History pack / window / max window | 3,500 chars / 1 MB / 64 MB | §9.10 |
| History gap / stall | 2 s / 30 min | §9.10 |
| Recreation list wait | 5 min | §9.10 |
| Prompt status settle | 3 s (Error at once) | §9.5 |
| User line expand / cut | 600 chars or 8 lines / 3,000 | §9.5 |
| Rich part / lines / HTML switch | 24,000 units / 400 / > 32,000 | §9.8 |
| Remote prompt settle (runner's) | 5 s (40 s) | §11.2.2 |
| Backlog age | 10 min | §11.2.6 |
| "Sent from the queue" 🫡 fallback | 2 min | §11.2.7 |
| Steer watch | 10 min | §11.4 |
| Tap answer limit / toast cut | 9 s / 190 units | §11.5.2 |
| Token TTLs | 1 h confirmations, 2 h menus | §11.5.1 |
| Secretary echo window | 2 min (history 90 min) | §9.6 |
| Question hook timeout / helper poll | 86,400 s / 2 s | §12.1.1 |
| Ask request bound | 1 MiB, 10 s | §12.1.1 |
| Question card budget / confirm wait / forget | 3,800 chars / 60 s / 30 min | §12.1 |
| Mirror's wait for a hook | 8 s | §12.1.6 |
| Tab label / tabs a row | 16 chars / 3 | §12.1.2 |
| Answer bounds | 8,192 / 32,768 | §12.1.4 |
| `/add-dir` reads / gap / rows / auto-answer | 15 / 2 s / 40 / 60 s | §12.2 |
| Command echo skew | 2 s | §12.2.6 |
| Launch timeout | 3 min | §13.6 |
| Esc gap | 3 s | §13.3 |
| `/enter` read-back: poll / limit / gone reads / box rows | 100 ms / 4 s / 2 in a row / 1,000 | §13.8 |
| Search cards / rest pack | 10 / 3,500 chars | §14.4 |
| Search action TTL | 2 h | §14.5 |
| Front double-tap window | 10 s | §16.4 |
| Wizard idle abort | 24 h | §15.5 |
| Folder probes: pick / each / all / in flight | 2 s / 250 ms / 1.5 s / 16 | §15.3 |
| Folder walk / create | 2 s / 5 s (4 at once) | §15.3 |
| Typed model id | ≤ 120 chars | §15.3 |
| Created path | ≤ 240 chars | §15.3 |
| Folder lists | 10 a page, ≤ 100 | §15.2 |
| Directory check / edit gap / re-post / stall / tabs a tag / recent | 15 s / 2 min / 30 min / 15 min / 20 / 10 | §16 |
| Screen picture: budget / cell / padding / drawings / tap gap | 2,400 px / 8–16 px / 16 px / 2 / 3 s | §17.1 |
| Snapshot bounds | 1,000 × 500 cells, 64 units a cell, 8 M chars | §17.1 |
| All Screen: columns / cell / part height / parts / strip | 100 / 12 px / 32,000 px / 10 / 2,048 px | §17.3 |
| `/status` caches / timeouts / reports | 10 min account, 60 s usage / 20 s, 60 s / 2 | §18.2 |
| Voice: download cap / worker deadline / idle / correction | 20 MB / 60 s + 4 s per audio second (≤ 15 min) / 10 min / 60 s | §19 |
| File links: life / per session / connections / timeouts / save | 30 days / 4,000 / 24 / 15 s read, 60 s write / 10 s | §20 |
| State save: structural / progress / `.prev` / quit lock wait | 2 s / 10 s / 60 s / 250 ms | §22.2 |
| Run marker / closing hold | 60 s / 30 s | §23.3 |
| Start note: late / retries | 2 min / 30 s doubling to 10 min, 12 sends | §23.2 |

## Appendix B — Callback data

All ≤ 64 bytes; every parser refuses anything that does not match exactly.

| Data | Meaning |
|---|---|
| `c:<verb>:<session id>` | the card. Verbs: `panel` (⚙️ Session), `main` (◀ Back), `runner`, `queue`, `pin` (toggles), `transcript`, `screen`, `screenimg`, `sendnow`, `reactivate`, `resume`, `fork`, `newsession`, `restart`, `esc`, `deactivate`, `allscreen` (the picker), `allscreen+d` · `allscreen+l` · `allscreen+dl` (the picker with its boxes), `allscreen-<level>[+d\|+l\|+dl]` (draw). Longest 59 bytes. Older verbs stay understood until a card's next edit. |
| `p:<n>:<session id>` | a Search answer's pick (`n` 1–2 digits; the id 8–40 hex digits and dashes) |
| `s:clear` | Search's ⊘ Clear |
| `d:front:<thread>` | a directory dot (1–18 digits) |
| `nt:<kind>[:<value>]` | the new-thread wizard: `r:<i>` / `m:<i>` a folder of the Recent / Most-used list, `rp:<page>` / `mp:<page>` a page, `rl:<page>` / `ml:<page>` a page label, `x` cancel, `cf` create the folder, `bf` back to the folders, `md` the default model, `mo:<i>` a listed model, `ms` specify, `bm` back to the models, `w:<i>` a window, `wd` the default window, `wr` refresh (values: indexes into what the step showed; ≤ 9 digits) |
| `q:<card key>:<verb>[:<arg>]` | a question card (a random 10-character key). Verbs: `o:<i>` option, `w` write my own, `n` note, `nr` remove the note, `ns` send the note alone, `d` done (multi-select), `t:<i>` a tab, `s` submit, `c` chat about this, `i` the previews' picture, `k` cancel writing, `z` cancel · `zy` yes · `zn` no, `u:<msg>` use a held message as the answer / note, `r:<msg>` a held message as a reply, `h:<msg>` a held message as the next prompt, `p` / `x` previous / next (older keyboards) |
| `tm:<card key>:<digit>` | a menu card's option (`/add-dir`) |
| `dl:<offer>` | `/download`'s 📤 Send as file |
| `t:<id>` | a single-use token (base-36 id, ≤ 20 bytes): kinds `confirm-<command>`, `cancel`, `backlog-queue`, `backlog-drop`, `unqueue`, `wake`, `wake-pin`, `recreate-one`, `recreate-all`, `verbose-topic`, `verbose-default`, `restart-uninterrupt`, `restart-keep`, `fork-interrupted`, `fork-uninterrupted`, `pick-resume`, `pick-activate`, `pick-wake`, `pick-topic`, `pick-fork`, `pick-cancel` |

## Appendix C — `state.json`

```json
{
  "version": 1,
  "updateOffset": 123456789, "searchThreadId": 22371, "setupNoticeMs": 0,
  "botId": 1000000001, "botUsername": "MyBot",
  "businessConnectionId": "", "businessEnabled": false, "businessCanReply": false,
  "businessMode": 0, "businessNote": "", "businessBotId": 0, "secretaryBotId": 0, "secretaryUpdateOffset": 0,
  "topicLayout": 2,
  "directoryMessageId": 916, "directoryHash": 1661888261, "directoryRich": true, "directoryPinned": true,
  "orphanThreads": [],
  "topics": [
    {
      "sessionId": "…", "threadId": 26785, "cardMessageId": 4410, "workLogMessageId": 0,
      "title": "drag crash fix", "idleSinceMs": 0, "cursor": 1048576, "transcriptPath": "…\\<id>.jsonl",
      "createdMs": 0, "lastPostedMs": 0, "cardPostedMs": 0,
      "recapText": "", "recapAtMs": 0, "lastMessageAtMs": 0,
      "cardTextHash": 0, "cardRich": true, "cardLinked": true,
      "cardPictureFileId": "…", "cardPictureAtMs": 0, "cardPictureShotHash": 0,
      "historyPromptMessageId": 0,
      "tabToken": "{…}", "errorAnnounced": false,
      "backfillPath": "", "backfillPos": 0, "backfillEnd": 0, "backfillDayKey": 0,
      "verbose": -1, "nameState": 1, "nameSetMs": 0, "nameColor": "🟦", "nameColorMode": 0, "stateIcon": true,
      "promptMessageId": 0, "promptBotLine": true, "promptTsMs": 0, "promptSource": 0, "promptText": "",
      "promptStatus": 0, "promptInterrupted": false,
      "changeLineHeld": true, "changeLineFromTitle": "…", "changeLineFromSquare": "🟦"
    }
  ],
  "newThreads": { "…": "the wizards and their launches (opaque to the state reader)" }
}
```

- The picture fields are omitted until a card showed one; `historyPromptMessageId` until the replay sent a line; the prompt fields
  until the mirror saw a prompt; the change-line fields while none is held; `newThreads` while there are none.
- `verbose`: −1 follows the default, 0, 1. `promptSource`: 0 typed at the PC, 1 phone, 2 runner, 3 phone-from-the-queue (a larger
  unknown number reads as the newest known). `nameState`: −1 = none yet. `nameColorMode`: −1 = the name predates the square.
- Per bot: a different `botId` resets everything but the file's own version.

## Appendix D — Texts

### D.1 A session's topic: `/help`

> **In a session's topic**
> Any text is queued as the session's next prompt (never typed into a running turn — /steer &lt;text&gt; does that: it sends the
> words NOW, like Send now at the PC). Claude Code commands (/compact, /model x, your skills) are queued the same way; commands
> that leave no record are refused.
> /add-dir &lt;path&gt; adds a folder to the workspace: Claude Code's question comes here as buttons, and "Yes, for this session" is
> picked by itself after 60 s.
> /download &lt;path&gt; — a file's download link (absolute, or relative to the session's folder), with 📤 Send as file
> /enter — press Enter at the PC: the prompt waiting in the session's input box is submitted (nothing is pressed into a menu, a
> dialog or an empty box)
> /queue · /sendnow · /steer &lt;text&gt; · /unqueue n
> /runner auto|paused
> /pin · /unpin — never idle
> /wake · /moveidle · /markunread
> /rename &lt;title&gt;
> /favorite · /tags · /tag x · /untag x
> /sessionid · /path · /gitbranch · /draft
> /summary · /transcript [level] · /screen · /screenimage
> /status — the card, then what Claude Code's /status shows: version, model, MCP servers, memory, account, plan usage
> /allscreen [level] [diffs] [last] — the whole conversation as tall pictures, drawn like Claude Code draws it (bare: the default
> level, full unless set; diffs: every file change as a diff; last: the last turn only)
> /activate · /restartsession · /deactivate · /interrupt
> On ⚙️ Session's page the card has ⎋ Esc (one Esc at once, as at the PC) and 🌙 Deactivate (claude stops, the tab keeps its place)
> /forksession [model] · /newsession [model]
> /takedraft — queue the unsent draft at the PC
> /cc /x — force /x through to Claude Code
> /verbose on|off|default — this topic shows the work too, or the conversation only
> /recreate — make this topic (or all) again with its history, verbose or not
> /search_tab — jump to the 🔎 Search topic: it goes first in the thread list (typed, `/search-tab` works too)
> /search &lt;words&gt; — ask the 🔎 Search agent from here: the request and its answer go to the Search topic

### D.2 The Search topic: its intro and `/help`

> **🔎 Search** — ask for a session in plain words ("the session where I fixed the tab drag crash") and the search agent finds it.
> Tap a result's number to pick it: Resume a closed one into a tab, Activate a deactivated tab where it is, Wake an idle one, or
> Fork it.
> This is one conversation for the whole app: it carries on after a restart, and /clear (or ⊘ Clear under an answer) starts it
> over.
> Fleet commands: /idle (idle tabs), /board (counts), /newsession &lt;folder&gt;, /pauseall, /resumeall, /status, /help.
> Pin this topic at the top of the list (Telegram has no way for a bot to do it).

`/help` adds: the agent reads the sessions it finds and answers with one numbered card each, quoting the evidence it saw; a pick's
menu fits where the session stands; `/newsession <folder> [model]` (or open a new thread and pick its folder); `/status` — the
bot, then Claude Code's account and plan usage; `/clear`; `/verbose on|off` — the default for all topics; `/recreate`;
`/search_tab`; `/search <words>`.

### D.3 The "/" menu's descriptions

`queue` the queued prompts (session topic) · `sendnow` send the next queued prompt now · `steer` send a message NOW, into a running
turn too: /steer <text> · `enter` press Enter at the PC: submit the prompt waiting in the input box · `runner` runner auto|paused · `pin` never idle: keep this topic · `unpin` let the session idle again ·
`wake` Idle -> Waiting for you · `transcript` the session as a .md file [minimal|compact|default|full|max] · `summary` the session's
summary · `screen` the terminal's visible text · `screenimage` the terminal's screen as a picture · `allscreen` the whole
conversation as tall pictures [minimal|compact|default|full|max] [diffs] [last] (bare: the default level) · `download` a file's
download link: /download <path> · `status` Claude Code's status and plan usage (after the session's card / the bot's line) ·
`search` ask the 🔎 Search agent from any topic: /search <words> · `search_tab` jump to the 🔎 Search topic (from any topic) ·
`rename` rename the tab and the topic · `restartsession` restart the session (asks while running) · `deactivate` park the tab (asks
while running) · `activate` start a parked tab's claude · `reactivate` resume a parked tab or a cut-off / failed session ·
`forksession` fork into a new tab · `newsession` a new session in this folder (or <folder> in Search) · `interrupt` stop the
running turn (asks first) · `idle` Search: the idle tabs · `board` Search: counts per state · `recreate` Search: make the topics
again in MOST RECENT order · `pauseall` pause every runner · `resumeall` resume every runner · `help` what you can do here.

### D.4 Claude Code's own texts the bridge sends (verbatim)

- The rejection prefix: "The user doesn't want to proceed with this tool use. The tool use was rejected (eg. if it was a file
  edit, the new_string was NOT written to the file). To tell you how to proceed, the user said:\n"
- Chat about this (after the prefix): "The user wants to clarify these questions.\n    This means they may have additional
  information, context or questions for you.\n    Take their response into account and then reformulate the questions if
  appropriate.\n    Start by asking them what they would like to clarify.\n\n    Questions asked:\n" then per question `- "<question>"`,
  `  Answer: <answer>` (or `(No answer provided)`), and for a preview question with a note `  User notes: <note>`.
- The STOP rejection (Cancel, with `interrupt: true`): "The user doesn't want to proceed with this tool use. The tool use was
  rejected (eg. if it was a file edit, the new_string was NOT written to the file). STOP what you are doing and wait for the user to
  tell you how to proceed."
- What the model then reads, for reference: `Your questions have been answered: "Q"="A" …. You can now continue with these answers
  in mind.` (labels only); `The user answered: "Q"="A" selected preview:\n<preview> notes: <note>. Read the answers carefully — they
  may request clarification, changes, or that you not proceed — and follow what they actually say.` (any note);
  `"Q"=(no option selected) notes: …` for a note alone; `The user responded: <text>` for a reply.

## Appendix E — Launch outcomes

| Result | When | The phone is told |
|---|---|---|
| `Opened` | a new background tab hosts the session | "Opened <title> — its topic is coming up." (`fresh`: "Opened <title> as a NEW session in the folder (there was no transcript to continue) — its topic is coming up.") |
| `AlreadyOpen` | a resume of a session that was open already | "<title> is open already, with its own topic." |
| `WindowClosed` | the window closed, or dropped the request unrun, before it acted | "The window that took the request closed before it opened the tab." |
| `ClaudeMissing` | no native Claude Code executable | "Claude Code (the native claude.exe) was not found on the PC. Install it there, or set its path in the settings." |
| `PresetRefused` | the launch's preset cannot start | the preset's own words + "Nothing was started: a preset session never falls back to your Claude login." |
| `NoFolder` | a resume or fork whose folder is not known | "The session's working folder is not known." |
| `Failed` | the tab could not be made, an error | the PC's words ("The PC could not create the terminal tab."), or "The PC could not open it; its log says why." |

The subject in a refusal names what was asked: "the resume of “T”", "the fork of “T”", "the new session in F" (the title on one
line).

## Appendix F — Designed, not implemented

- **F.1 Live activity in a topic.** A draft (`sendMessageDraft`) is a typing action with its OWN rate limit (20 calls per 5 s and 40
  per 30 s per peer — the typing actions'), silent by nature, removed by the next real message, ephemeral (30 s). Each Running
  topic could carry a live draft of its current step ("⚙ Bash: npm test · 1m 12s", "💭 thinking…"), refreshed every ~20–25 s, the
  topic of the user's last action every ~2 s; the final message removes it. A line ABOVE the card could only be an edited message:
  one topic at a time.
- **F.2 Folder groups.** A bot cannot create groups; the user can, and a bot admin with `can_manage_topics` runs topics in a forum
  supergroup as in the private chat. Proposal: Mirror (every topic stays in the private chat, a copy goes to its folder's group) or
  Move; the key = the launch folder with git worktrees folded into their repository, fixed at the topic's creation; one-tap setup
  through `t.me/<bot>?startgroup=<key>&admin=manage_topics+delete_messages+pin_messages+change_info`; each group's flood budget is
  its own. Cheaper inside one chat: the folder's name in every topic name and a per-folder icon.
- **F.3 Permission levels and a drive PIN.** Read (the mirror, searches, reads) · Steer (+ prompts, commands, answers, queue and
  runner, pin, rename, tags) · Operate (+ restart, deactivate, activate, fork, new session, revive, interrupt, pause all); a PIN
  (`/unlock <PIN>`, 30 min; a salted hash in the credential store; 5 wrong tries lock 15 min; the PIN message deleted after
  reading).
- **F.4 Redaction** (§24.2).
- **F.5 Other messengers** through a connector contract (a child process speaking JSON-RPC over stdio: `initialize`, `ensureThread`,
  `renameThread`, `deleteThread`, `send`, `edit`, `delete`, `react`, `draft`, `ack`; inbound events), one-way alert webhooks.
- **F.6 Plan approval** through the same PermissionRequest hook (measured to approve and reject `ExitPlanMode`); outside this
  design's scope.
- **F.7 Voice**: a bundled native worker (§19.6); spoken commands ("slash compact" → `/compact`); other languages. (Voice in the
  Search topic is in: §19.2.)
