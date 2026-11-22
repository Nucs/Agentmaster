# Agentmaster — the fleet in a Teams channel, posted as the owner (no bot)

> **Status: DESIGN (2026-11-18), decided with the owner; implemented in the milestones of §13.** The research, the
> measurements and the bot shapes are in [`TEAMS.md`](TEAMS.md) (§15 measures this shape). This connector needs **no
> administrator, no app registration and no bot**: it works in a locked-down tenant (TEAMS.md §3) through two delegated
> tokens that a normal user already has.

## 0. Summary

- Agentmaster mirrors the fleet into ONE private channel that the owner picks in the cog. It writes AS THE OWNER through
  Microsoft Graph. It routes each Graph call to a token that can make it (§4).
- **Per session, one thread.** The thread's root post is the session's card: its subject is the state icon, the color
  square and the title; its body is the card and a line of the emoji actions that fit the state. The replies are the
  conversation: prompts, final replies, questions, pictures.
- **A legend post** explains every emoji action and carries the fleet board. The owner pins it once by hand (Graph
  cannot pin in a channel); Agentmaster edits it in place.
- **Buttons are emoji reactions** (TEAMS.md §15.4). Agentmaster reads the owner's reaction, acts, and removes it.
- **Alerts are @mentions of the owner** (TEAMS.md §15.1 #11): at the end of a turn, when Claude needs the owner, and on
  an error. Every other post is silent.
- **Inbound is polling** (the owner's choice): one small delta read per poll, every 5 s, 30 s or 90 s (§7).
- **A new post by the owner opens a new session**, its text the first prompt (§9). Posts of other tools in the same
  channel (a marker prefix) and Agentmaster's own posts never do.
- It runs beside the Telegram bot: each connector keeps its own state; either one or both can be on.

## 1. Decisions (the owner, 2026-11-18)

| # | Decision | Choice |
|---|---|---|
| D-C1 | The shape | Bot-less, in a channel, as the owner. **No administrator, ever.** |
| D-C2 | v1 scope | The mirror and the cards; emoji actions; prompts from Teams; pictures and extras (the screen, the whole conversation, `/status`, the history replay, the 🔎 Search thread). |
| D-C3 | The channel | A setting (team and channel), first the owner's existing private channel, which other tools may also use. |
| D-C4 | Which sessions | All Claude tabs (no folder filter). Every mirrored message becomes the company's data (retention, eDiscovery). |
| D-C5 | Identity | Two tokens, routed by capability: the Azure CLI's (post, edit) and an app the tenant already allows (read, react, post, upload). §4. |
| D-C6 | Inbound | Polling, with Microsoft's "poll once a day" rule knowingly not followed (TEAMS.md §2.11). No push, no public endpoint. |
| D-C7 | Poll cadence | 5 s while hot, 30 s normally, 90 s after an hour with no change. §7.3. |
| D-C8 | Alerts | An @mention of the owner: a turn ends; Claude needs the owner (a question, an approval, a menu: with high importance); an error. Not when an action that the owner started finishes. |
| D-C9 | A new post by the owner | Always a new session, its text the first prompt (the new-session steps ask what is missing). |
| D-C10 | Documentation | Generic: nothing specific to one company or tenant. |
| D-C11 | Verification | No compile on the work PC. The owner compiles and runs the tests at home. |
| D-C12 | Where the code lives | The main checkout, branch `feature/teams`. Before the first push, the Teams work is squashed onto a fresh branch, so no tenant-specific text stays in the history. |

## 2. What the owner sees

```
The private channel (posts layout; the chat layout works the same)
 ├─ 📌 Agentmaster · legend and fleet     ← pinned by the owner; Agentmaster edits it
 ├─ 🟡 🟦 drag crash fix                  ← a session's thread; the root is the card
 │     card: 🟡 Waiting for you since 14:09 · 🟦 drag crash fix
 │           /source/app · main · opus · max · 182K · Runner Auto · 0 queued
 │           🖼️ screen · 📊 status · ↩️ Enter · 📜 all · 🔀 fork · 🌙 park · 🔄 restart
 │     ├─ 👤 You · 14:02 · typed at the PC: fix the drag crash       👀 → 👨‍💻 → 👌
 │     ├─ 🤖 the final reply (formatting, tables, code) · @Owner      ← the turn's one sound
 │     ├─ ❓ Claude asks: … 1️⃣ … 2️⃣ …  · @Owner, IMPORTANT             ← answered by a reaction
 │     └─ 👤 the owner's reply → a queued prompt                      👀 → 🫡 → 👨‍💻 → 👌
 ├─ 🔎 Search                             ← replies are search requests
 └─ (the owner writes a new post)         → a new session; that post's thread is its thread
```

## 3. Feature map

✅ the same or better · 🔧 compensated (how) · ✖ given up.

| Telegram | In the channel |
|---|---|
| A topic per session; its name | ✅ A thread per session; the root's subject (edited with the body, always both: a body-only edit clears the subject, TEAMS.md §15.4) |
| The sticky card | ✅ The root post, edited in place; never re-posted |
| The mirror, formatting | ✅ Replies; HTML with tables and code blocks; up to 80 KB a post (split at 60 KB) |
| Status reactions | ✅ The same emoji |
| One sound per turn | ✅ An @mention of the owner (§6.4) |
| Topics in recent order | ✅ Teams orders threads by their latest activity |
| No lost message while closed | ✅ The delta read at start; a backlog reply is offered, not run (§7.4) |
| Card buttons, question cards, menus | 🔧 Emoji reactions and the legend (§8) |
| The new-thread wizard | 🔧 Numbered replies answered by reactions (§9) |
| The pinned directory | 🔧 The legend post, pinned by hand |
| Deleting idle topics | 🔧 Threads stay; the card says idle or closed, and the thread sinks |
| Deleting a message | 🔧 Edited down to "·" (a delete needs an administrator) |
| Instant answers | 🔧 Polling, 5 s while hot |
| You and Claude look different | 🔧 Every post is the owner's; Claude's start with 🤖 |
| Unread marks per topic | ✖ The owner's own posts are never unread |
| Voice notes | ✖ Channels have no voice messages |
| The "/" command menu, the typing indicator | ✖ Typed commands still work (§7.5) |

## 4. Identity and tokens

### 4.1 The two sources

- **The Azure CLI** (`TeamsAuth::AzureCliSource`): runs `az account get-access-token --resource
  https://graph.microsoft.com --output json` (with `--tenant <the app token's tid>` once that is known) in a hidden child
  process (`RunClaudeCapture`: a kill-on-close job), and keeps the token until 5 minutes before it expires (the earlier
  of `expires_on` and the token's `exp`). Its scopes are fixed by Microsoft for every tenant; the one that matters is
  `Group.ReadWrite.All`. The run is detached: no caller and no Stop ever waits for it; a failed run is not repeated for a
  minute. `az` is searched on the PATH only (never the current folder), then in the installer's folder. No `az`, or no
  sign-in: the status line says "the Azure CLI: run az login". A CLI signed in as another account than the app's is not
  used (Agentmaster never posts as someone else).
- **An app the tenant already allows** (`TeamsAuth::AppSource`): a public client id and a tenant in the settings (§10).
  The device-code flow (`POST /{tenant}/oauth2/v2.0/devicecode`, scope `https://graph.microsoft.com/.default
  offline_access`) shows a code and an address in the status line and in `hooks.log` (`[teams] sign in: open … and enter
  …`), and Agentmaster asks the token endpoint at the interval Microsoft names. A code is offered once per Apply (each
  start and each Save of the settings) while the app has no stored sign-in or Microsoft refused it; the cog's **Sign in**
  (M7) asks for one at any time. The refresh token goes to Windows Credential Manager (`Agentmaster:messaging:teams:<profile
  leaf>`) as UTF-8, over several credentials (`…#2`, `…#3`) when it is longer than the 2,560 bytes one credential holds;
  the first one's user name `teams:<parts>:<client id>` names the count and the app, so a changed client id asks for a new
  sign-in. Every new access token comes from it, and a rotated refresh token replaces it. A refused refresh (a 4xx:
  `invalid_grant`, a revoked consent, a password change) stops the refreshes until a new sign-in.
- Agentmaster never prints, logs or stores an access token, a refresh token or a device code; the log names a token's
  owner, its scopes and its expiry. A bearer token travels to `graph.microsoft.com` only (`GraphRequestPath` refuses any
  other host, a link from an answer included), and WinHTTP follows no redirect with it.

### 4.2 Capabilities (measured, TEAMS.md §15)

| Operation | Scopes that work | The Azure CLI | A typical allowed app |
|---|---|---|---|
| Post a root or a reply (also with `hostedContents`) | `ChannelMessage.Send`, `Group.ReadWrite.All` | ✅ | ✅ |
| Edit (PATCH) | `ChannelMessage.ReadWrite`, `Group.ReadWrite.All` | ✅ | ✖ |
| Read messages, replies, delta | `ChannelMessage.Read.All` (`Group.*` is refused for reads, although documented) | ✖ | ✅ |
| Set or unset a reaction | `ChannelMessage.Send` | ✖ | ✅ |
| Upload a file to OneDrive | `Files.ReadWrite`, `Files.ReadWrite.All` | ✖ | ✅ |
| Delete (`softDelete`) | `ChannelMessage.ReadWrite` only | ✖ | ✖ |

- **The router** (`PickTokenFor`, pure) reads each token's `scp` claim and picks, per operation, the first source whose
  scopes can make it, the app first for post, read, react and upload, the CLI first for edits. A 403 marks that
  source unusable for that operation for 30 minutes and the call is tried once with the next source. A 401 renews that
  source's token and sends the call once more. The log has one `[teams] routing: posts …, edits …, reads …, reactions
  …` line whenever the routing changes.
- **No source for an operation** degrades the feature, and the status line says which: no edit → a state change is a
  short reply in the thread instead of a card edit; no read → no inbound (a mirror only); no react → status words
  instead of reactions; no upload → pictures only inline, never files.
- **The owner** is the `oid` claim (and `name`) of the app's token: the @mention target, and the test "did the owner
  write this?" (§7.2).

## 5. The channel and its threads

- **State:** one file per channel, `<profile>\messaging\teams\state-<8 hex of the team and the channel>.json`
  (`ChannelStateFileLeaf`), so a channel switched away from and back to finds its threads again; written through
  `WriteStateDocument` (PERSISTENCE.md §17): the legend's and the Search thread's root ids; per session the root id, the
  card's last hash (subject + body), the transcript cursor, the last reply read, the watched messages (§7.2) and the
  alert bookkeeping; the journal of Agentmaster's own posts and reactions (ids, kept 14 days); the delta link. A file the
  start cannot use is kept aside before it is next written (PERSISTENCE.md §16), and its previous copy (`.prev`,
  refreshed at most once a minute from a file the run could read) is the state to run with.
- **A thread is created** when a session qualifies by Telegram's rule (`QualifiesForThread` = `QualifiesForTopic`: a
  Claude tab that is not Idle, pinned, or used within the idle grace — `messagingIdleGraceMinutes`, one setting for both
  connectors), and never deleted. The root is posted with the subject and the card (journaled: never read as the owner's
  new session); the history replay follows (§6.3), then the live mirror. A failed root post is tried again a minute later.
- **The card** (`CardFactsFromSession` → `BuildCardHtml`, `CardSubject`) is edited on a change of state, title, facts or
  legend line, at most once per 10 s per card (changes in between are merged), always subject and body together. A card
  edit answered 404 means the owner deleted the thread: its record goes, and the next pass makes a new thread with the
  whole history. With no source that can edit (§4.2), a changed state line is posted as a short reply instead.
- **An idle or closed session** keeps its thread. A session that leaves the live set gets its transcript finished (it
  rests: a held text is its final reply) and its card brought up to date ("· closed") once. A session that opens again
  continues in it.
- **The legend post** is created at the first start ("📌 Agentmaster · legend and fleet"), and its fleet board is
  edited at most once a minute when the fleet changes (the board without its clock word decides: an unchanged fleet is
  never edited). The board lists every live Claude tab, the most recent first, at most 50 (`kLegendFleetMax`; the rest
  are counted), each linked to its thread. A failed post is tried again a minute later; an edit answered 404 (the owner
  deleted the post) makes a new legend. The status line asks the owner to pin it until the owner reacts 👍 on it once
  (the only signal Agentmaster can see).
- **The 🔎 Search thread** is created at the first start; its replies are search requests (§9.3).

## 6. Outbound: the mirror

### 6.1 What is posted

- Read from the session's transcript with a per-session cursor (complete lines only, at most 1 MiB a pass, every 2 s),
  classified by Telegram's own `ClassifyTranscriptLine`; the decisions are `MirrorLines` (pure, tested).
- Posted at once, silently: a turn's prompt, a message typed while it ran, an answer, a slash command (Telegram's "👤 You"
  lines), an interrupt. At once with an alert: a question (its options numbered) or a plan (NeedsYou), an API error
  (Error).
- **A text is held** until what follows tells: a tool call, a question, an error or another text makes it a remark
  (posted only with `teamsVerbose`, in one narrative post with the tool lines and failed tools); the turn's end makes it
  the **final reply** ("🤖 Claude · 14:05" + the reply), posted with the turn's alert. The turn ends when the text's own
  stop reason ends it, when the next turn's prompt arrives, or when the session rests (not Running, not Needs approval,
  or closed) and every complete line was read.
- **The cursor is committed** only when the posts that cover it are answered, and never past a held text: a restart reads
  again what was not posted (at least once; a post whose answer was lost may show twice), and a held text is never lost.
  A post given up after its retries commits too (it would only fail again).
- Every post starts with Claude's 🤖 marker or the owner's 👤 line, so the channel tells them apart. Every post Agentmaster
  makes is journaled (§7.2).

### 6.2 Formatting

- Telegram's HTML builders are reused (`BuildPromptLineHtml` and the others), then `TeamsHtmlFromTelegramHtml` (pure)
  adapts them: `\n` → `<br>` outside `<pre>`; `<tg-spoiler>` → a plain `<span>`; `<blockquote expandable>` →
  `<blockquote>`; code blocks keep their language.
- A post longer than 60 KB is split at a block boundary (`SplitTeamsHtml`, pure).

### 6.3 The history replay

- When a thread is created for a session that already has a conversation, its history is posted oldest first,
  packed into posts of up to 60 KB, paced behind the live traffic (§6.5). Nothing in it rings.
- It reuses Telegram's pipeline: windows of 1 MiB of complete lines (growing ×4 to 64 MiB for one giant line; a longer line
  is skipped), `SplitHistoryWindow` (whole turns, a fork's copied lines included), `PlanHistoryPosts` (Telegram's history
  form: the conversation, plus the narrative with `teamsVerbose`), then `PackHistoryPosts` joins consecutive entries into
  one post with a blank line between them (Telegram's 4,096-character limit does not apply). One window is in flight per
  thread; each post commits the newest turn end it holds.
- The replay's last window stops before the **newest turn** (`CutNewestTurn`): the live mirror takes that turn, so a turn
  that is still running continues live, and one that ended within the last 10 minutes alerts like any other.

### 6.4 Alerts (D-C8)

- **A turn ends:** the turn's final reply @mentions the owner (`<at id="0">name</at>` and its `mentions` entry).
- **Claude needs the owner:** a question or a plan post @mentions the owner, with `importance: high`. A permission prompt
  is in no transcript: ENTERING Needs approval (a recent change, not a state restored at a start) posts "🟠 Claude needs
  your approval at the PC" with the same alert, unless a question or a plan alerted within the last 30 s.
- **An error:** the API error post @mentions the owner.
- Each of these is one alert: a turn that ends in a question has no final reply, so it alerts once; a turn that asks a
  question and then goes on alerts for the question and again for its final reply (both need the owner).
- **Only fresh items alert:** an item older than 10 minutes (`kAlertFreshMs`) — a backlog the mirror reads after a
  restart, the newest turn handed over by a history replay — is posted silently. A history replay never alerts. The alert
  rides the last part of a post that is split.

### 6.5 Pacing (`TeamsOutbox`)

- One queue of calls for the channel, in priority order: answers to the owner's actions, alerts, the live mirror, card
  edits, the history replay, the legend board.
- **Lanes:** every post into a session's thread (its root, its history, its mirror, its notices) is in that session's lane,
  and a lane's posts go out strictly in their order — a retry in back-off holds its lane — so a thread never shows a reply
  before its prompt whatever their priorities. Card edits and the legend have no lane (a newer edit supersedes an older
  one). A thread's post that no token can make waits in its lane (30 s, then again) instead of being dropped.
- At most one call every 1.2 s for the whole channel (Graph allows 1 per second per channel and per user, about 83 % of
  that sustained; reactions count too). A 429 holds the whole channel for its `Retry-After` (5 s without one, 10 minutes
  at most) and keeps the call's place; a 5xx or no answer retries 1 s, 2 s, 4 s … 60 s and gives up after 8 attempts
  (about 2 minutes, logged). A call no token can make is answered at once ("no token can edit"), and the feature
  degrades (§4.2).
- v1 keeps the queue in memory (a stop drops it, logged with the count); the durable journal of the Telegram outbox
  (MESSAGING_OUTBOX.md) is a later step.

## 7. Inbound: polling

### 7.1 The poll

- **At start:** the delta read is drained (all pages) to a delta link. Nothing in that first read runs an action; it
  only records what exists, and the backlog rule (§7.4) takes the owner's replies written while Agentmaster was closed.
- **Each poll:** one `GET` of the delta link. An unchanged channel answers in ~600 bytes. Each root in the answer is a
  thread that changed: a new or edited root, a new reply, or a reaction on any of its messages (measured, TEAMS.md §15.1
  #14b).
- A poll reads at most 20 pages; each page's roots are handled at once and the link is saved after every page, so a long
  first sync goes on at the next poll from where it stopped. A link that is no longer good (400, 404, 410) is dropped and
  the next poll syncs from the channel's start. A root older than the run's start is history at EVERY poll, never a
  request: the delta returns an old post again when a reply or a reaction is added to it, and that does not make it new;
  a re-sync's roots that this run handled already are known by their threads and steps. The poll waits for the tick's
  state load at a start; with no source that can read, it looks again every 30 s.

### 7.2 A changed thread

- **A session's thread:** (a) the root's reactions, compared with the last set read → card actions (§8); (b) `GET
  replies?$top=20` (newest first) → the owner's replies newer than the last one read → prompts and commands (§7.5); (c)
  a `GET` of each WATCHED reply in the thread (an open question, a confirm, a queued prompt) → its reactions.
- **The legend:** its reactions (➕ starts a new session).
- **The 🔎 Search thread:** the owner's new replies are search requests.
- **A post whose new session is under way** (its step waits, or this run started one for it): the step's reactions and
  the owner's replies under it (§9.1).
- **A new root:** written by the owner in this run, not Agentmaster's (the journal), not starting with a marker prefix of another
  tool (`teamsForeignMarkers`), not a system message → a new session (§9).
- **Anything else** (other tools' threads and replies) is ignored.
- **"The owner wrote it"** = `from.user.id` is the owner AND the id is not in Agentmaster's journal (both write as the
  owner). Agentmaster's own reactions are journaled too (when they are queued) and never read as actions.
- **The fingerprint** closes the gap before a post's id is journaled (the poll can read a reply before the send's answer
  comes back, and Claude's reply read as the owner's would come back as a prompt): every post is fingerprinted when it is
  queued (`PostFingerprint`: an FNV-1a hash of its plain text's ASCII letters and digits, lowercased — what Teams rewrites
  in a stored post, emoji, spacing and tags, does not count), and a reply or a root that matches one of the last 10
  minutes' posts of its thread is Agentmaster's (journaled on the spot).

### 7.3 Cadence (D-C7, `NextPollDelayMs`, pure)

| Condition | Next poll |
|---|---|
| The owner acted in Teams in the last 2 minutes, or a session waits for the owner (Waiting for you, Needs approval, an open question) | 5 s |
| Nothing changed for 60 minutes | 90 s |
| Otherwise | 30 s |

- A `GET` never follows another within 1.1 s (Graph allows 1 read per second per channel). A 429 waits its
  `Retry-After` and doubles the next delay once.

### 7.4 The backlog

- A reply the owner wrote while Agentmaster was closed (older than the run's start) is not run. Agentmaster answers it:
  "📥 written while Agentmaster was closed (2 h ago): “…” — react ✅ to send it now, ❌ to drop it" (`BacklogOfferHtml`),
  and watches that answer for 2 hours (a `Watched` entry of kind Backlog, persisted; the reactions are M5's).

### 7.5 What a reply in a session's thread does

- The changed thread's newest 20 replies are read (`replies?$top=20`), and the owner's new ones (`NewOwnerReplies`:
  the owner's, not journaled, plain messages, not deleted, newer than the last read) are handled oldest first.
- A plain text, or a Claude Code command that leaves a record (`/compact …`): a queued prompt, built field by field as
  Telegram's (next in line before the runner's own prompts; the Autorun origin; the question guard's pass; `remoteSource`
  `teams:<reply id>`; `RemotePromptWait` stamps a waiting one); a dormant tab is started. A bare `/add-dir` gets its usage
  line (its path input cannot be filled from here).
- **Its status** shows on the owner's reply as one emoji at a time (`PromptReactionFor`): 👀 queued (or sent and not
  echoed), 👨‍💻 its turn runs, 👌 the turn is over, ⚠️ it failed; the follow ends at 👌 or ⚠️, when the queue no longer
  holds the prompt, or after 6 hours. With no source that can react (§4.2) no status is shown.
- The mirror posts no "👤 You" line for a prompt the owner wrote in the thread (their own reply is there): the prompt's
  compare form (`NormalizePromptTextForCompare`) is matched once in its transcript echo.
- A command: Telegram's parser (`ParseRemoteCommand`) and its meanings. In the code now: `/steer` (MessagingSteer.h: sent
  NOW when the session can take it, else first in the queue, with Telegram's answers), `/enter` (MessagingEnter.h: ONE
  Enter, read back by the hosting window), `/interrupt` (one Esc, at most one per 3 s per session: two open Claude Code's
  Rewind menu), `/deactivate` (parks a tab at rest; during a turn it points to the card's 🌙 and its confirm), `/status`
  (📊's report), `/screen` (🖼️'s picture), `/allscreen [level] [diffs] [last]` (📜's pictures; Telegram's parser and
  its words, `ParseAllScreenArgs`; other words get the usage line, `AllScreenUsageHtml`), `/forksession [model]` (🔀's
  fork, with a per-launch model), `/newsession [model]` (a new session in this session's folder, Telegram's in a topic:
  no first prompt; its own thread comes up, `_newSessionTeams`), `/help`. A refused Claude Code
  command says why (`RefusedCommandHtml`); the rest come later and say so. Every window action's answer
  is the hosting window's own words; with no window hosting the tab, the owner is told so (`AnswerOnce`).
- While a question is open, a text reply is its "Other" answer (M5).

## 8. Emoji actions

- **The legend** is TEAMS.md §15.4's table: on a card 🖼️ 📊 🛑 ↩️ 📜 🔀 🌙 ☀️ 🔄; on a question 1️⃣–🔟, ✅, ❌; ✅ on a
  confirm; ❌ on a queued prompt; ➕ on the legend.
- **Matching:** emoji compared without U+FE0F (`NormalizeEmoji`); only an emoji of the owner, newer than the last read,
  on a message whose kind allows it (`ActionForReaction`, pure).
- **Acting** goes through the same `ServiceHooks` as Telegram: 🖼️ → `sessionAction "screen"`, drawn by `ScreenImage`,
  posted inline; 📊 → the `/status` report (`ClaudeStatus`); 🛑 → `"press-esc"`; ↩️ → `"press-enter"`; 📜 →
  `ConversationImage`, posted as pictures; 🔀 → a fork launch (asking interrupted / uninterrupted when the session needs
  it, by a numbered question); 🌙 → `deactivateSession`; ☀️ → `"start"`; 🔄 → `restartSession`.
- **Consume:** before it acts, Agentmaster removes the owner's emoji (the card stays free for the next press); the
  action's own answer (a reply in the thread) says what happened or why not. No 👌 is added to the card: it would stay
  there for good.
- **Read:** a changed thread's root reactions are compared with the last set read (`DiffReactions`); only what the owner
  ADDED since, minus Agentmaster's own (the journal) and the status emoji (`OwnerReactionsToAct`), acts. An emoji added
  while Agentmaster was closed is removed and not acted on (a note says to add it again). The watched replies of the
  thread (a confirm, a backlog offer, a queued prompt) are read one by one, and a watch acts once: its first meaningful
  emoji ends it.
- **In the code now:** 🖼️ posts the screen as a PICTURE (the window's styled `screen-snapshot`, drawn by
  `RenderScreenSnapshotPng` on a worker, posted inline as the post's own hosted content, `BuildImagePostJson`; a snapshot
  that does not parse falls back to its text, `ScreenTextPostHtml`), 📊 posts Telegram's status report (the heartbeat, the
  transcript's facts, the account, the plan usage: `BuildClaudeStatusReport`, one report at a time process-wide), 🛑 one
  Esc, ↩️ one Enter, 🌙 a park (a confirm during a turn), ☀️ starts a parked tab, 🔄 a restart after its confirm
  (KeepInterrupted: the PC asks nothing); 📜 draws the whole conversation as pictures (below); 🔀 forks the session
  (below). ✅ / ❌ on a backlog offer sends or drops it; ❌ on the owner's queued prompt takes it out of the
  queue while it waits (the watch ends when it is sent). A keycap on a new post's folder step picks that folder (§9.1);
  ➕ on the legend answers under it that a new post opens a session (§9.2).
- **📜 and `/allscreen`** (Telegram's §21.39, `_allScreenTeams`): the WHOLE conversation drawn from the TRANSCRIPT, never
  the terminal (a fullscreen Claude Code keeps no scrollback), so no window is asked and a parked tab or a closed session
  draws too; a fork with no turn of its own draws its source's. `RenderConversationImages` runs on a worker; each part is
  posted inline in the session's thread as soon as it is drawn (the lane keeps the order), captioned "📜 <title> ·
  conversation (full + diffs) · 2/4 · turns 12–31" (`AllScreenCaptionHtml`). The level: the one typed, else the cog's
  Messaging tab's (`messagingAllScreenLevel`, one setting for both connectors; Full unless set otherwise). Parts are cut
  at 8,000 px (`kAllScreenPartHeightPx`, Telegram's are 32,000 px documents): each must be a post of its own under 2.8 MB
  of PNG — an ESTIMATE, not measured in Teams (TEAMS.md §15.4, O8); a part past it is told in words. Ten parts at most; a longer
  conversation keeps its most recent rows, and a line says how much was left out and which typed level draws more
  (`AllScreenCutNoticeHtml`). One drawing at a time in this connector and one per session (a long transcript holds
  hundreds of MB while it is drawn); a Codex session is refused.
- **🔀 and `/forksession [model]`** (`_forkTeams`): a fork of the session in its folder. A Claude Code source is assessed
  first with the PC's and Telegram's one rule (`AssessForkContinuation`, HOOKS.md "Interrupted turns"; situation B while
  `resumeApiErrorTurns` is on, C always). When the fork would continue the source's turn by itself (A: the source runs, or
  stopped mid-turn), wait at its API error (B) or wait at the owner's interrupt (C), a question is posted in the thread
  (`ForkQuestionHtml`, Telegram's words): "1️⃣ Fork interrupted" and "2️⃣ Fork uninterrupted", each saying what it does in
  that situation; react within an hour, ❌ for no fork. It is watched as `MessageKind::ForkChoice` (persisted as "fork";
  the situation, the folder, the title and the model in its payload, `ForkQuestionPayload`), and an answer launches
  Telegram's choice (`ForkChoiceFor`: A Interrupted / Uninterrupted; B TakeBackAndInterrupt / TakeBackAndContinue; C
  Uninterrupted / TakeBackAndContinue). Nothing to ask, or a Codex source: the fork goes Uninterrupted at once. The
  launch (`_launchForkTeams`): "🔀 Forking it into a new tab…", then `ServiceHooks::launchAnswered` (Fork; the source's
  title, from which the window derives the fork's own). Its answer takes the new session's path (queued, settled on the
  tick, `LaunchAsk::askedIn`): opened, the thread names the fork, and the fork gets a thread of its own at the next pass
  even when Telegram's rule would not list a new idle tab (`_wantThread`); refused, the reason (`RemoteLaunchRefusalText`).
- **Confirm:** 🔄 always, and 🌙 during a turn: Agentmaster replies "Restart <title>? … React ✅ within 60 s"
  (`ConfirmPostHtml`), watches that reply for 60 s from its answer, and acts on the ✅; ❌ answers "Not done".
- **Questions** (`AskUserQuestion` through the ask registry, as for Telegram): one reply per question with its options
  numbered; a number picks (single choice: at once; several choices: the picks, then ✅); a text reply is "Other".
  `/add-dir` menus the same way. ⚠ **Not in the code yet:** the ask registry has ONE acceptor (INTERACTIVE_TOOLS.md §6),
  and the Telegram bot is it while it runs; answering from Teams needs the registry to take a second acceptor (or the two
  connectors to share one). Until then the question post shows its options and the owner answers at the PC or in
  Telegram.

## 9. New sessions and Search

### 9.1 From a new post (D-C9)

- **Which posts:** the owner's new post (§7.2's rule) written while Agentmaster runs. A post older than the run is
  history at every poll (§7.1): a post written while Agentmaster was closed opens nothing. A post is asked about once
  per run (`_wizardRoots`): every reply under it changes it again in the delta.
- **The folder step** (`_startWizard`; `WizardStepHtml`): a reply under the post, "🆕 New session for “<first line>”
  📁 Which folder? React with its number, or reply with a folder's full path:", then up to ten folders, 1️⃣–🔟. The list
  is Telegram's picker list, the recent half (`_wizardFolders`, as `MessagingService::_buildNewThreadListsNow`): where
  the registry's sessions worked, the most recent first, the machine's temp folders left out, a folder known to be gone
  left out (bounded checks: 250 ms each, 1.5 s in all; a UNC path, or a folder that did not answer, is listed). With no
  folder to list, only a typed path is asked for. The step is watched for an hour (`kWizardStepMs`, a `Watched` of kind
  Step whose payload keeps the words, the post's link and the list: `WizardPayload`, so a restart in between still knows
  them); the poll runs every 5 s for two minutes after the post.
- **The pick** (`_wizardStep`): the owner's keycap on the step (consumed: removed again) picks a listed folder; or the
  owner's reply that is one folder's full path (`Msg::NewThreadTypedFolder`) names one. Each reply is read once
  (`_wizardRepliesMs`): words that name no folder are not read again. The folder is checked again
  (`_pickWizardFolder`: a fresh probe, at most 2 s): a folder that is not there, or that did not answer, is said under the
  post, and the step stays for another pick. A deleted post, or a deleted step, ends the step.
- **The launch** (`_launchFromWizard`): the step's watch ends, "Opening a session in <folder>…" is posted, and a window
  is asked through `ServiceHooks::launchAnswered` (NewSession in the folder; the tab titled by the post's first line,
  `Msg::NewThreadNameForMessage`; the settings' model; the window the engine picks, the one activated last). The session
  id is minted by the service and marked as launching (`_launching`) BEFORE the ask: the tab can exist, live in the
  registry, before the answer reaches the tick, and the threads' step must not post a root of its own for it meanwhile.
- **The answer** (`RemoteLaunchReply`, exactly once, from any thread, maybe before the ask returned) is only queued under
  a leaf lock and wakes the tick (`_onLaunchAnswered`, as `MessagingService::_onLaunchAnswered`), so a window's thread
  never waits on the service's lock; a fork's answer (§8 🔀) takes the same path. The tick settles it first in its pass
  (`_settleLaunchAnswer`):
  - **Opened:** the post becomes the session's thread (`Thread::firstPrompt` keeps the words): the card replaces the
    post's words at the next pass (an edit, which only the Azure CLI's token can make; without it the post stays and the
    state is told as replies, §4.2) and shows them as "📝 first prompt: …" (cut at 4,000 characters,
    `kCardFirstPromptMaxChars`: every card edit carries them); no history is replayed (a new session has none); the
    replies the step read (the typed folder among them) are not prompts, those written after the pick are. The words are
    queued as the first prompt (`_queueTeamsPrompt` with no reply: no status emoji and no ❌ watch, which on the card
    would stay for good, §8); the dormant tab is started.
  - **Refused:** the reason (`RemoteLaunchRefusalText`) under the post; the post is done (the owner writes a new one).
  - An answer that reaches a later run (the settings changed meanwhile) binds nothing (`LaunchAsk::runMs`).
- **Not in the code yet:** the model step and the window step (Telegram's wizard, `MessagingNewThreadWizard.h`), extra
  folders (`/add-dir`, Telegram's ➕ message), an offer for a post written while Agentmaster was closed (like §7.4's).

### 9.2 From the legend

- ➕ on the legend (consumed) is answered under the legend: "🆕 To open a session, write a NEW post in this channel: its
  words are the session's first prompt." A session needs its first prompt, and a new post carries it (D-C9), so the
  designed root "🆕 New session" with the steps in its thread (the owner's first reply the first prompt) is not built.

### 9.3 Search

- **The thread:** a root of Agentmaster's, "🔎 Agentmaster · search your sessions" (`SearchRootHtml`), posted once when
  it is missing (`_searchRootStep`: with a search factory, a source that can post and the run's state loaded; a failed
  post is tried again a minute later); its id is the state's `searchId`.
- **The agent:** the app sets the factory (`TeamsService::SetSearchFactory`, from `TerminalPage.AgentEngine.cpp`, once
  per process): `MakeTeamsSearchBridge` (MessagingSearchBridge.cpp), the Sessions page's agent in a conversation of its
  OWN, "teams-search" (its record under `<profile>\harness\`, its debug log), never Telegram's: the agent hands every
  event to every listener, so a shared conversation would answer a Teams request in Telegram's Search topic too. Its
  thinking level is the cog's `messagingSearchThinking` (one setting for both connectors), applied live. The bridge is
  made at the first request (`_ensureSearch`) and dropped at the stop; the conversation goes on.
- **A reply** (`_searchStep`, `ClassifySearchReply`; each read once, from the run's start: a request written while
  Agentmaster was closed is not run): words → "🔎 Searching…" and a request to the agent (one at a time: "A search is
  running" while one runs); `/clear` or `/reset` → the conversation starts over; `/help` → the root's text; a number
  (1–999, `#12`, or a keycap typed) → that row of the last answer.
- **The answer** (`_onSearchEvent`, on the agent's thread): the model's sentence, then each session numbered — ▶ when a
  tab hosts it, its title, folder, age and prompts, the model's why under it; past 30, a count (`SearchAnswerHtml`). A
  failure says why. The rows are kept for the picks until the next answer (a restart forgets them: search again).
- **A pick** (`_searchPick`): an open session is named with a link to its thread (or gets one, `_wantThread`); a closed
  one is resumed into a tab (`RemoteLaunchRequest` Resume, `LaunchAsk::Kind::SearchPick`), and the Search thread hears
  how it went (`_settleSearchPick`: resumed, open already, a NEW session when its transcript is gone, or why not); the
  resumed session gets a thread of its own.
- Not in the code yet: picks by reaction (a number is typed), Telegram's per-pick menu (fork, new session here).

## 10. Settings and the cog

| Setting | Default | Meaning |
|---|---|---|
| `teamsEnabled` | false | The master switch. |
| `teamsTeamId`, `teamsChannelId` | "" | The channel, typed or pasted (a list of the owner's teams and channels is not built). |
| `teamsClientId`, `teamsTenantId` | "", "organizations" | The allowed app for the device-code sign-in. |
| `teamsUseAzureCli` | true | Use the Azure CLI's token for edits. |
| `teamsVerbose` | false | Post tool lines too. |
| `teamsForeignMarkers` | "🤖" | Prefixes of other tools' posts: such a new post never opens a session. Agentmaster's own posts are known by id. |

- The ids are trimmed (a pasted id often carries a space or a line break), and a channel id copied from a channel's link
  is decoded (`19%3a…%40thread.tacv2` → `19:…@thread.tacv2`, `detail::PercentDecodeAscii` in `TeamsConfigFrom`: the Graph
  paths encode it again). Another channel, app, tenant or Azure CLI switch restarts the service; verbose and the markers
  apply while it runs (`RestartNeeded`, pure).
- **The cog's Messaging tab, TEAMS CHANNEL (NO BOT)** (M7, `AgentManagerContent.Settings.cpp`), under the Telegram bot's
  sections: the setup steps (where the two ids are in a channel's link; `az login`; an allowed app's id), the switch, the
  team and channel ids, the app id and the tenant (empty = `organizations`, its placeholder), the Azure CLI switch,
  verbose, the markers, **Sign in** and **Sign out**, and the status line (`BuildStatusLine`, pure: "Teams: on as Owner ·
  12 threads · posts by the app, edits by the Azure CLI, reads by the app, reactions by the app", the degraded features
  named, the code to enter while a sign-in waits, and the newest problem). The fields are saved and applied by the cog's
  Save like the bot's (the settings sink calls `TeamsService::Apply`). The two buttons act on the RUNNING service at once
  (`RequestSignIn`, `SignOut`: Save first); after Sign in the status line is read again 2.5 s and 8 s later, when the code
  has come from the token endpoint. The code is also in `hooks.log` (`[teams] sign in: open … and enter …`), and each
  Save offers a new code while one is needed. No secret is put into a control.

## 11. Architecture

| File | What it holds |
|---|---|
| `AgentMaster/TeamsModels.h` | Pure, tested by `tests/tests_teams.cpp`: the legend and the actions, `NormalizeEmoji`, `ActionForReaction`, the card / legend / alert HTML, `TeamsHtmlFromTelegramHtml`, `SplitTeamsHtml`, `NextPollDelayMs`, `DiffReactions`, `PickTokenFor` and the scope table, the owner-post rule, the state's JSON. |
| `AgentMaster/GraphClient.{h,cpp}` | WinHTTP: a JSON request with a bearer token; timeouts; `Retry-After`; never throws. |
| `AgentMaster/TeamsAuth.{h,cpp}` | The two token sources, the device-code flow, Credential Manager, the JWT claims. |
| `AgentMaster/TeamsService.{h,cpp}` | The service: its threads, the state, the mirror, the poll, the actions; created by the engine with the same `ServiceHooks` as Telegram. |
| `Engine.{h,cpp}` | `Engine::teams`, created and applied beside `Engine::messaging`. |
| `SessionModels.h`, `Persistence.cpp` | The settings of §10. |
| `AgentManagerContent.Settings.cpp` | The cog's TEAMS section. |
| Build lists | `TerminalAppLib.vcxproj`, `tests/run-m5-tests.bat`, `tests/run-harness-live.bat` (the new `.cpp` files are link dependencies of `Engine.cpp`). |
| Logs | `hooks.log` lines `[teams] …`. |

- **Reused as they are:** `MessagingModels.h` (the line classifier, `TabFact` and `QualifiesForTopic`, the state icons
  and names, `ParseRemoteCommand`, `CardButtonFacts`, the history packing, `IsBacklogUpdate`), `AskQuestion.h`,
  `TuiMenus.h`, `MessagingSteer.h`, `MessagingSessionControl.h`, `MessagingEnter.h`, `MessagingFromQueue.h`,
  `MessagingNewThreadWizard.h`, `ScreenImage`, `ConversationImage`, `ClaudeStatus`.
- **Not changed:** the Telegram service. The Teams service has its own copy of the few steps that are private there
  (queueing a remote prompt), built from the same public pieces, so the working Telegram code is not touched while
  nothing can be compiled here (D-C11).

## 12. Threads and safety

- Three threads as in `MessagingService`: **poll** (the delta reads), **send** (the outbox), **tick** (the fleet
  facts, the mirror's transcript reads, the card and legend edits). Nothing runs on a UI thread.
- Every entry point is no-throw; every `catch` logs (Rule #18). `Stop` aborts the requests in flight.
- The state file is replaced only through `WriteStateDocument` (Rule #16).

## 13. Milestones

| # | Milestone | Verified by |
|---|---|---|
| M1 | `TeamsModels.h` and its tests | the m5 harness (at home) |
| M2 | `GraphClient`, `TeamsAuth`, the settings, the engine wiring, the service's start / stop / status, the legend post, the token router; their pure parts in `TeamsModels.h` §12–§16 with tests | a live start against the owner's channel |
| M3 | Threads, the card, the mirror, the alerts, the history replay; their pure parts in `TeamsModels.h` §17 with tests | the same |
| M4 | The poll, prompts and commands from Teams, the backlog; their pure parts in `TeamsModels.h` §18 with tests | the same |
| M5 | Emoji actions, confirms (questions: open, §8); their pure parts in `TeamsModels.h` §19 with tests | the same |
| M6 | New sessions, Search, the screen, the whole conversation, `/status` — built so far: the screen picture and `/status` (📊), with `TeamsModels.h` §20; a new session from a new post (§9.1: the folder step, the pick, the launch, the post as the card) and ➕'s answer (§9.2), with §21; 📜 and `/allscreen`, the whole conversation as pictures (§8), with §22; 🔀 and `/forksession`, a fork with the interrupted / uninterrupted question (§8), with §23; `/newsession` in a thread (§7.5); the 🔎 Search thread (§9.3, a conversation of its own), with §24 — M6 is complete | the same |
| M7 | The cog's TEAMS section — built: the Messaging tab's TEAMS CHANNEL (NO BOT) section (§10), with the channel id's decode in `TeamsConfigFrom` (§16, tested) | the owner's eyes |
| M8 | CLAUDE.md, SETTINGS.md, LOGGING.md entries (written after M5; each later milestone updates them) | review |

## 14. Risks

- **Polling** breaks Microsoft's polling rule; the owner accepted the risk (D-C6). The load is kept minimal (one small
  read per poll), and the app's token may be shared with other tools.
- **Edits depend on the Azure CLI's `Group.ReadWrite.All`**, which Microsoft accepts "for backward compatibility only".
  If that ends, §4.2's degradation applies (a state change becomes a reply).
- **Every post is the owner's**: other people with access to the channel's team see them as the owner's words (the
  channel should stay private, with the owner its only member).
- **Company data:** every mirrored message stays in the tenant under its retention, eDiscovery and DLP rules (D-C4).
