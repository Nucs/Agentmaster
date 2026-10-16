# Agentmaster — messaging: the fleet in Telegram, Microsoft Teams, and any other messenger

> **Status: Telegram v1 IMPLEMENTED on 2026-10-16 (§21 says exactly what was built, where it differs from the
> design below, and what is still open; §21.6 = the topics per TAB, Claude Code's own recap, the card that
> rings — dev 0.0.1.16, not production); designed 2026-10-15, revised twice on 2026-10-16.** The bot on this
> machine is the user's `@ClaudeSessionsGateBot` since 2026-10-16 18:50 (the token in
> `K:\source\telegram\ClaudeSessionsGateBot.env`, stored in Windows Credential Manager by the cog — never in a
> profile file; the first bot, `@SidKatrineBot`, never got Threaded Mode on). Teams, webhooks and external connectors
> are still design only. The user's ask: "We plan messaging apps abstract support for Telegram + Microsoft
> Teams. We will support anything."
> **The user's decisions so far** (quoted in §18): the bot runs inside the running Agentmaster process
> (D1); the Telegram topics are synced to the tabs of every window, idle tabs being the only ones not
> listed (D3); a pinned 🔎 Search topic hosts the Sessions page's search agent (Sonnet 5.5, high thinking)
> and revives old sessions (D24); a topic shows the whole session, every message type configurable and
> able to be disabled, default or verbose (D19); a message sent in a topic is ENQUEUED, never a steer
> (D8); Claude Code only for now (D23); a message is exactly a prompt, except Claude Code commands that
> leave no record in the session (D22); most of the tab's right-click menu runs as `/slash` commands,
> the non-destructive ones plus `/restartsession` (D22); every message in a topic is silent except the
> turn's final message (D5). A message gets into a session through the session link of
> [`MOD_LINK.md`](MOD_LINK.md) (designed the same day) where the session has it, else by the typed path
> (§2.4). Platform facts were checked on 2026-10-15/16 in each platform's own
> documentation (Telegram's and Claude Code's pages read in full; Microsoft's through searches of its
> pages) unless a line names a third-party source or says "from memory" (§20). Decisions: §18. What must
> be measured before any code: §19.

## 0. Summary

**What it is.** One Telegram bot, run by Agentmaster itself, turns the bot's private chat into a remote
Agentmaster: **one topic per tab** of every window (idle tabs excepted), each topic showing its whole
session live, and a pinned **🔎 Search** topic that finds any session ever run and brings it back. In a
session's topic you see what the session is doing, you queue prompts for it, you answer its questions,
and you run its tab-menu actions as `/slash` commands. Only a turn's final message makes a sound.

**The picture.**

```
Agentmaster.exe  (one process per profile; runs the bot — D1)
 ├─ engine: registry · scanner · scheduler · observer · unread census           (exists)
 ├─ FleetLink (new): topic sync · transcript follower · intents · authorization · redaction
 ├─ Messaging hub (new): connections · outbox + pacing · topic map · tokens · renderers
 ├─ the Search agent (exists: SessionSearchAgent on the agent harness; Sonnet 5.5, high thinking)
 ├─ connectors (new, worker threads): Telegram · Teams Lite · Teams Bot · Webhook
 │    └─ child processes it starts: external connectors (JSON-RPC over stdio) · cloudflared (Teams)
 └─ into each session: the session link (MOD_LINK.md: submit / command / abort, exact events),
      else the typed path (SubmitPrompt, the draft swap)              ⇄ claude.exe ×N on their tabs

Telegram, the bot's private chat (Threaded Mode):
 📌 🔎 Search                 ← the agent: find, wake, resume, fork; fleet commands
    Agentmaster/feature        ← tab in window 1, Running      (topic = the whole session)
    proxmox                    ← tab in window 2, Waiting for you
    NumSharp/perf              ← tab in window 1, Needs approval (a question as buttons)
    (idle tabs: no topic — Search wakes them)
```

**Four platform facts shape it (verified 2026-10-15/16):**

1. **Telegram needs no public address, and its private-chat topics were made for AI bots** ("Topics in
   Private Chats keep separate projects or support cases organized – improving on established web-based
   AI chat UIs", Bot Features). A bot reads its messages by long polling; since Bot API 9.3–9.5 its
   private chat holds topics and streamed drafts. Only one reader may poll a bot: Agentmaster needs a bot
   of its own (not the homelab's `@balash_fastrun_bot`), one per install.
2. **The whole chat shares about one message per second** (all topics together, "in a single chat"), so
   a live mirror of many busy sessions is paced: one work-log message per turn edited as the turn runs,
   one final message, a pinned card per topic (§6.6).
3. **Telegram keeps a bot's incoming messages 24 hours.** The bot lives inside Agentmaster, so it is
   offline while Agentmaster is closed; messages sent meanwhile are handled at the next start under a
   backlog rule (§4.3).
4. **Claude Code's own remote features are per session** — Remote Control (the Claude app, claude.ai
   logins only, never presets) and channels (a research preview whose custom channels ask for a
   confirmation at every launch). Agentmaster's bot is fleet-wide and built on Agentmaster's own states,
   queue and tabs (§2.2–§2.3). Teams has no two-way path without a public HTTPS endpoint (§3.2); it gets
   alerts first.

**What a topic can and cannot carry** (§5–§7, §10.4): everything that is conversation or control — the
card, every message type, queued prompts, Claude Code's recorded commands, the tab's actions, questions
as buttons, the lifecycle. Not the terminal's screen-only parts (a text snapshot stands in), not
keyboard-only actions, not a large busy fleet mirrored without pacing, and nothing while Agentmaster is
closed.

**Build order (§17):** P0 measurements → P1 topics synced to tabs + the whole session at the default
level + queued prompts → P2 questions as buttons, pin, the tab commands → P3 every message type
configurable, backfill, the activity draft → P4 the Search agent and revive → P5 Teams alerts + webhooks
→ P6 Teams bot → P7 external connectors.

## 1. Goals and non-goals

**Goals**

- **G1 — Every working tab in the pocket.** Each tab of every window whose session is not idle has a
  topic; the topic list IS the working fleet.
- **G2 — The whole session.** A topic shows the session as it happens — prompts, replies, tool calls,
  results, agents, questions, markers — each type on, off, default or verbose (§6.3).
- **G3 — Act like at the PC.** A message is a prompt (queued, never a steer); a `/command` is either
  Claude Code's or the tab menu's; a question is a set of buttons. Every action goes through the seams
  the app's own buttons use (the delivery gate, the draft swap, the verified press, the runner's holds).
- **G4 — Find and revive.** The 🔎 Search topic finds any Claude Code session and wakes, resumes or forks
  it back into a tab — and so into a topic.
- **G5 — Quiet unless done.** Everything is silent except a turn's final message.
- **G6 — Safe.** Only the owner can act; stale buttons, old backlog messages, secrets and hostile input
  are each handled by a rule (§9, §16). Managed sessions run with `--dangerously-skip-permissions`: a
  remote prompt is remote code execution on this PC.
- **G7 — Any messenger later.** The same model through a contract (§12); Teams first among the others.

**Non-goals (version 1)**

- **N1** Codex and any non-Claude agent (D23); sessions that are not in a tab.
- **N2** Sharing the fleet with other people (one owner per connection).
- **N3** Mirroring the terminal's pixels; `/screen` sends its text.
- **N4** Any always-on helper or cloud service: the bot runs in Agentmaster (D1).
- **N5** Voice and files from the phone into a session (later, §7.6).

## 2. What exists already

### 2.1 In Agentmaster — what this design reuses

| Need | Existing piece | Where |
|---|---|---|
| Which tabs exist, in which window, with which session | `_claudeTabs` per window, the registry's live sessions with their tab tokens (`LiveTabBindings`) | `TerminalPage.h`, `SessionRegistry.h` |
| Idle, and the decay to it | The state machine; `ShouldDecayWaitingToIdle` (an unread or Mark-Unread session never decays); `waitingForYouTimeoutMinutes` | `SessionScanner.h`, `SessionModels.h` |
| Wake an idle session | The tab menu's and board's **Move to Waiting-for-you** (and **Move to Idle/Done**) | `Tab.cpp` `_triageMoveMenuItem`, the board |
| Follow the conversation as it grows | The scanner's transcript tail (`ParseTranscriptDelta`); the turns renderer and its five levels (`TranscriptTurns`, the Copy ▸ Transcript levels) | `SessionScanner.{h,cpp}`, `TranscriptTurns.{h,cpp}` |
| Send a prompt safely | `SessionRegistry::SubmitPrompt` — the one send seam: delivery gate, draft swap, verified placement, Enter-retry, lost-send verdict, invisible-character sanitize | DELIVERY.md, PENDING_INPUT.md §9 |
| Queue and run | The Prompt Queue + `Scheduler::DecideAdvance` (dormant gate, settle hold, question guard with `kAnswersQuestionOk`, global pause, interrupt hold) | DELIVERY.md §14–§17 |
| Read the box or a menu | `ControlCore::ReadPendingInputDraft`, `IsMenuOptionCaret`, `pendingBoxState`, the remembered draft `pendingInput` | PENDING_INPUT.md |
| The tab's right-click actions | The tab menu (Rename, Change tab color, Copy ▸, Mark Unread, Move to Waiting-for-you / Idle, Favorite, Tags, Activate, Fork session ▸, New Session Here ▸, Restart session, Deactivate ▸, Close ▸, …) and the per-window sinks that reach the hosting window | `Tab.cpp`, `Engine.h` |
| Search every session | `SessionSearchAgent` on the agent harness (the Sessions page's AI search; thinking levels off → max) and, when On, the session database's tools | SESSIONS.md §9, SDK_HARNESS.md, SESSIONS_DB.md |
| Resume, fork, start | `_RestoreArchivedSession`, `_ResumeSessionFromDisk`, `_ForkSessionFromDisk`, `_ForkManagedSessionById`, the launch seam | `TerminalPage.AgentSessions.cpp` |
| Durable per-session flags | `SessionStore` (the `favorite`, `tags` keys) | `SessionStore.h` |
| Secrets, HTTPS, JSON | Credential Manager helpers (`LaunchPresetsOs.h`), WinHTTP (`Updater.h` `HttpsGet`, already in `TerminalApp.dll`), nlohmann-json | |
| Child processes that die with the app | The agent harness's kill-on-close job | `ClaudeSdkProcessWin32.cpp` |
| Logs and failures | `AppendStateLog`, `[nav]`, Rule #18 | CLAUDE.md |

### 2.2 In Claude Code: Remote Control (verified 2026-10-15)

`claude --remote-control` or `/remote-control` connects ONE local session to claude.ai/code and the
Claude app ("each interactive Claude Code process registers one remote session"); pushes for questions
and permission prompts; skipped while a `CLAUDE_CLIENT_PRESENCE_FILE` marker exists. Claude.ai plans
only, and only when `ANTHROPIC_BASE_URL` is `api.anthropic.com` — never for an Agentmaster preset session.
It has no fleet view, no Agentmaster states, no queue, no tabs. Agentmaster can keep the presence file
in step with the user at the PC (D17).

### 2.3 In Claude Code: channels (verified 2026-10-15)

An MCP server that claude spawns, pushing `notifications/claude/channel` events into ONE session ("events
queue into the session and are processed in order"), optionally relaying tool-permission prompts. Research
preview: custom channels need `--dangerously-load-development-channels`, which asks for a confirmation at
start — an unanswerable modal in a managed launch. Not used: the session link (§2.4) is the in-session
route this design uses instead (D16).

### 2.4 The session link (MOD_LINK.md, designed 2026-10-16 — nothing built)

A mod, `agentmaster-link`, loaded into every session Agentmaster launches, reporting the session's own
events to Agentmaster and acting through Claude Code's API ([`MOD_LINK.md`](MOD_LINK.md)). For this
design it is the better route into a session, wherever a session has it:

| This design needs | Through the link | Without it (typed path) |
|---|---|---|
| A queued prompt, never a steer (D8) | `submit` → `$.prompt.submit({text, asUser: true})`, which "starts a turn of its own after the running turn and its Stop hooks" — D8 by construction; an `ack` when the turn starts | Prompt Queue + `SubmitPrompt` (delivery gate, draft swap, Enter rescue) |
| Claude Code's slash commands | `command` → `$.command.run` | typed like a prompt |
| `/interrupt` | `abort` → `$.turn.abort` | one Esc through the hosting window |
| What it is doing, and the turn's end (the final message) | exact `turn.start` / `turn.complete {reason}` / background work / `session.compact` events | the scanner's states and the transcript tail |
| Questions and plans | `tool.call` on `AskUserQuestion` / `ExitPlanMode`, carrying the questions | the transcript's pending `tool_use` + the screen's menu |
| The settle before a queued send | 3 s (linked states do not flap) | 40 s |

The transcript stays the record either way (MOD_LINK §13), so a topic's mirror always follows the
transcript; the link changes how fast and how exactly the topic learns the state, and how a message gets in.
A prompt with an image keeps the typed path (a plugin's submit carries text only).

### 2.5 Prior art (seen 2026-10-15/16)

- Telegram's own pitch for private-chat topics and streamed replies to AI bots (Bot Features; the
  October 2025 announcement: "especially useful for AI chatbots").
- Agent frameworks that map one topic to one session (OpenClaw; Hermes Agent: "each topic becomes an
  isolated workspace with its own conversation history").
- OpenClaw's Teams connector issues about Teams streaming (a reason to edit, not stream, on Teams).
- matterbridge (unmaintained since v1.26.0, 2023-01-29: not a dependency); Apprise (alerts, not re-checked).

## 3. The platforms

### 3.1 Telegram (Bot API 10.3, 2026-08-24)

| Fact | Detail | Use here |
|---|---|---|
| Receiving | `getUpdates` long polling (`timeout`, `offset`, `allowed_updates`); not while a webhook is set | §10.2 |
| Retention | "Incoming updates are stored on the server until the bot receives them … not kept longer than 24 hours" | The backlog rule (§4.3) |
| One poller per bot | A second concurrent `getUpdates` gets `409 Conflict` (known behavior; not on the API page) | A bot per install, a per-token lock |
| Topics in a private chat | 9.3: `has_topics_enabled`, `message_thread_id` / `is_topic_message` for private chats; 9.4: `createForumTopic` in private chats, `allows_users_to_create_topics`; turned on in the BotFather Mini App → Threaded Mode (third-party docs) | The topics (§5) |
| How clients start topics | In Threaded Mode a non-reply message from the main view starts a new topic by itself, with an implicit name the bot should replace (`is_name_implicit`; Telegram-iOS #2236) | §5.1 |
| Topic verbs | Name 1–128; `icon_color` one of six, set ONLY at creation; `editForumTopic` (name, icon emoji); `deleteForumTopic` ("along with all its messages"); per-topic pins (`unpinAllForumTopicMessages`); no close in private chats (supergroups only) | §5.2, §10.4 |
| What a bot cannot see | No method lists topics; no update for a deleted topic | Agentmaster's topic map (§4.4) |
| Drafts | `sendMessageDraft` (private chats, `message_thread_id`), "an ephemeral 30-second preview", `can_stop` (10.3) | The activity draft (§6.2) |
| Chat actions | `sendChatAction` (typing…) in a private-chat topic; lasts "5 seconds or less" | §6.2 |
| Buttons | `callback_data` 1–64 bytes; `style` danger/success/primary; copy-text buttons (1–256 characters) | Tokens (§7.5), copy actions (§7.3) |
| Reactions | `setMessageReaction`: "as non-premium users, bots can set up to one reaction per message"; allowed emoji include ✍ 👀 👨‍💻 👍 👎 | The status of a queued message (§7.1) |
| Text | 1–4,096 characters; HTML with `pre`/`code`, `blockquote expandable`, `tg-time` | §10.4 |
| Rich messages | 10.1+: `sendRichMessage` with tables, details, lists | Plans and long replies, later |
| Checklists | `sendChecklist` is "on behalf of a connected business account" only | Claude's to-do list as a text checklist |
| Files | Upload 50 MB (photos 10 MB); download 20 MB | Transcripts, diffs, outputs as files |
| Deleting | Bots may delete their own and the user's messages in a private chat within 48 h | The PIN message (§9.3) |
| Limits (Bots FAQ) | "In a single chat, avoid sending more than one message per second" (bursts, then 429 with `retry_after`); 20 per minute in a group; ~30 per second overall | §6.6 |
| Privacy | Bot chats are cloud chats (Telegram's design) | §9.6 |

### 3.2 Microsoft Teams

| Fact | Detail |
|---|---|
| Bot SDKs | Bot Framework SDK out of support 2025-12-31, archived 2026-01-05; successors: Microsoft 365 Agents SDK, Teams SDK 2.1 (2026-09-28) — the REST protocol is enough here |
| How a bot receives | Teams → Azure Bot Service → `POST https://<public host>/api/messages`; "the endpoint must be publicly reachable" |
| Registration | Single-tenant (multi-tenant creation deprecated 2025-07-31); work or school tenants only; **Upload custom apps** needed; a bot must be part of a Teams app |
| Polling | Graph: "poll a resource … only once per day" |
| Messages to yourself | `48:notes` posts as you — no alert; "Teams as a log file" breaks the terms |
| One-way alerts | Workflows webhooks (Office 365 connectors retired 2026-05-18…22, MC1181996) |
| Limits | 7 / 1 s, 60 / 30 s, 1,800 / h per conversation; 50 RPS per app per tenant; ~100 KB per message; streaming 1:1 only, 2 minutes per stream |
| Threads | A 1:1 chat has none; a Team channel has a thread per post (the topic model's equivalent, §11) |

### 3.3 Other platforms: dial-out or push-in

| Platform | Inbound | Threads | Checked |
|---|---|---|---|
| Slack | dial-out (Socket Mode WebSocket, ack in 3 s, ≤ 10 sockets) | message threads | 2026-10-15 |
| Discord | dial-out (Gateway; Message Content intent) | threads, forum channels | 2026-10-15 |
| Matrix | dial-out (`/sync`) | threads | from memory |
| Signal | dial-out (signal-cli JSON-RPC, local) | none | from memory |
| WhatsApp Cloud API | push-in (webhooks) | none | from memory |
| Google Chat | dial-out possible (Pub/Sub pull) | threads | from memory |
| Email | dial-out (IMAP IDLE) | conversations | from memory |
| ntfy, Gotify, Pushover, incoming webhooks | none / subscribe | none | from memory |

## 4. Architecture: the bot runs inside Agentmaster

### 4.1 The parts

All in the running Agentmaster process — one set per process, shared by every window (Rule #10):

- **FleetLink** (engine, plain C++, one worker thread): keeps the topic set equal to the qualifying tabs
  (§5), follows each listed session's transcript and Agentmaster events into typed items (§6), executes
  inbound intents through the Fleet Control API (§8), redacts every text (§9.6) and authorizes every
  intent (§9.3). Started from `SharedEngine`, like `SessionDbRefresher`.
- **The messaging hub** (engine, one worker thread): the connections, the persistent outbox and its
  pacing (§6.6), the topic map, pairing, action tokens, the renderers.
- **The Search agent**: a `SessionSearchAgent` conversation on the agent harness (§5.4).
- **Connectors** (worker threads): Telegram (a long poll per connection), Teams Lite, Webhook, Teams Bot
  (a loopback listener). They move bytes and authenticate senders; the hub keeps every rule.
- **Child processes** it starts in a kill-on-close job: external connectors (§12.2) and `cloudflared`
  for the Teams bot (§11.4).
- **UI-hosted actions** (answer a menu, read a screen, open a tab, the tab's actions) go to the ONE
  window that hosts the session through per-window sinks (§8.2).

### 4.2 Why inside Agentmaster (D1) — and what replaces a process boundary

The user's decision (2026-10-16). It gains: no second program, no pipe protocol, nothing that can block a
hot swap, direct reach of the registry, scheduler and sinks. It costs, with the answers:

| Cost | Answer |
|---|---|
| Offline while Agentmaster is closed | Telegram keeps updates 24 h → the backlog rule at the next start (§4.3); a Teams message fails in Teams |
| No crash alert from outside | `app.restarted` at the next start, from a dirty-run marker (§4.3) |
| Internet input parsed in the process that types into every session | Hardening H1–H6 |

- **H1** Network work only on worker threads; every thread body and callback wrapped (Rule #18) — an
  exception escaping a `std::thread` ends the process.
- **H2** Bodies capped before parsing (Telegram ≤ 2 MB per `getUpdates` answer, Teams ≤ 256 KB per
  request, an external connector's line ≤ 1 MB); JSON nesting capped; strings capped before use.
- **H3** The sender first: an update from anyone but the paired owner is dropped after reading only its
  sender id.
- **H4** The Teams listener: loopback only, `POST` only, the JWT verified before the body is parsed.
- **H5** A circuit breaker: a connector that throws or keeps failing is stopped and reported.
- **H6** Plugins out of process: external connectors are child processes speaking validated JSON-RPC.

### 4.3 Lifetime

- **Start.** After the registry has loaded: read the connections and their secrets; start the
  connectors; Telegram `getMe` (Threaded Mode on?), the BACKLOG, then the long poll; then the **topic
  sync** (§5.2): topics of tabs that no longer qualify are deleted, missing ones created.
- **The backlog rule (D2).** An update older than 10 minutes when processed: read commands are answered;
  a message for a session is NOT queued silently — "This message is 3 h old — queue it for
  *Agentmaster/feature*?" [Queue] [Drop]; a button press is checked against the current state (§7.5);
  Operate commands never run — the bot asks again.
- **Clean exit.** Flush the outbox (at most 2 s); post "Agentmaster closed — the bot is offline until it
  starts again" in 🔎 Search (optional, off by default); stop the connectors and children; remove the
  dirty-run marker.
- **Crash, kill, hot swap.** The marker (`messaging\run.marker`, rewritten every 60 s with the time and
  the version) stays; at the next start 🔎 Search gets "the previous run ended unexpectedly at <time>",
  or "updated to <version>" when the version changed.
- **Sleep, network loss.** Backoff 1, 2, 4 … 60 s with jitter; poll at once on resume.
- **Two installs.** Release and dev each run their own bot (D13).

### 4.4 Files (all under the active profile, Rule #15)

| File | What |
|---|---|
| `messaging\connections.json` | The connections and their policy (level, message types, idle grace, backfill, routing, quiet hours); edited by the cog, read by FleetLink (authoritative) and the hub. |
| `messaging\<connectionId>\state.json` | Telegram's update offset; **the topic map** (session → `message_thread_id`, card message id, work-log message id, last mirrored transcript position); the 🔎 Search topic id. Written atomically, with the previous copy kept: the Bot API cannot list topics, so this is their only record. |
| `messaging\<connectionId>\outbox.jsonl` | Messages not yet delivered (compacted on start). |
| `messaging\tokens.json` | Live action tokens (§7.5). |
| `messaging\run.marker`, `messaging\present.marker` | The dirty-run marker (§4.3); "present at the PC" for Remote Control (D17). |
| `messaging\status.json`, `messaging\messaging.log` | Connection states every 30 s; the transport log (§14). |
| `harness\telegram-search.json` | The Search topic's agent conversation (its record and digest, SDK_HARNESS.md §6). |
| Windows Credential Manager | Every secret: `Agentmaster:messaging:<profile hash>:<connectionId>:<name>`. Per profile on purpose: two installs must never share a bot. |
| `SessionStore` keys (per session) | `pinned` (§5.3), `topicView` (the topic's message-type overrides, §6.3). |

### 4.5 Who does what

| Job | FleetLink | Hub | Connector |
|---|---|---|---|
| Which topics exist (§5) | ✓ decides | creates / renames / deletes | serializes |
| What appears in a topic (§6) | ✓ typed items, redacted | renders, paces | serializes |
| Notifications (§6.5) | marks the final message | sets `disable_notification` | |
| Sender authentication | | ✓ | reads the sender id first (H3) |
| Authorization, backlog rule | ✓ authoritative | first check | |
| Executing intents | ✓ Fleet Control API | | |

## 5. The chat: topics synced to the tabs

### 5.1 Which topics exist (D3, decided)

- **🔎 Search** — created by the bot at pairing, the first topic; the user pins it at the top of the list
  in the Telegram client (the Bot API has no call to pin a topic; P0-16 checks the clients allow it).
- **One topic per tab** — for every tab, in every Agentmaster window, that hosts a **Claude Code** session
  (managed, or a hand-typed claude Agentmaster adopted), **whose state is not Idle**. Running, Waiting for
  you, Needs approval, Error and Done are listed; **Idle is the only state that is not**. A **pinned**
  session is never Idle (§5.3), so it is always listed. Not listed at all: Codex tabs, shell tabs, an
  unprompted claude with no session id yet, sessions in no tab.
- The topic's name is the tab's title (Rule #11: one title); its color is the nearest of Telegram's six
  topic colors to the tab's color, fixed when the topic is created (Telegram cannot change it later).
- Topics are ordered by Telegram (most recent activity first); the tabs' order is not reflected.
- **A topic started in Telegram** — in Threaded Mode a message sent from the chat's main view starts a
  new topic by itself (§3.1). Such a topic asks for a folder and starts a new session there (D20); its
  first message becomes the session's first prompt; the topic is then bound to the new tab and renamed
  to its title. A new topic whose first message is a 🔎 Search command is answered in 🔎 Search and
  deleted.
- An optional routing filter per connection (all tabs by default; favorites, tags or folders) narrows
  the list.

### 5.2 A topic's life

| On the PC (tab, session) | In Telegram |
|---|---|
| The session first leaves Idle (its first prompt; a wake; a revive) | Its topic is created: the pinned card, then the backfill (§6.7), then the live mirror |
| It becomes **Idle** (the 60-minute decay of a read Waiting session; Move to Idle/Done; a restore that normalized it) | The card turns to "💤 Idle — leaves the list at 14:32" with [📌 Pin] and [Wake]; after the **idle grace** (D25, default 10 min) the topic is DELETED, with its messages (Telegram cannot hide or close a private-chat topic) |
| It leaves Idle within the grace | It stays; the card goes back to normal |
| Woken again later (Move to Waiting-for-you at the PC, or Wake / Revive from 🔎 Search) | A new topic, rebuilt by the backfill (§6.7) |
| Renamed (F2, the tab menu, the Explorer, or `/rename` in the topic) | `editForumTopic` renames the topic |
| Tab color changed | Nothing (a topic's color is set at creation only) |
| Moved to another window | Nothing |
| `/clear` (a new conversation in the same tab) | The same topic continues the tab: a marker "— /clear: a new conversation —" |
| Restarted / Deactivated / Activated | The same topic; a marker; a deactivated tab keeps its state, so it stays listed unless Idle |
| Forked, or a new session in the same folder | A new tab → a new topic when it leaves Idle (at once when started from Telegram) |
| Closed (archived) | The topic is deleted at once |
| Agentmaster closed and reopened | Topics stay in Telegram; at start the sync keeps the topics of the tabs that still qualify (restored Waiting / Needs approval / cut-off Error sessions) and deletes the rest |
| The user deletes a topic in Telegram | Nothing happens to the session; the next message for it fails, and the topic is created again (topics = tabs) |

**Reading on the phone counts.** Any action in a topic (a message, a button, a command) marks the session
read on the PC — its flash ring and unread count clear — so the 60-minute Waiting → Idle decay works for a
session handled only from the phone. (Merely viewing a topic cannot be seen: bots get no read receipts.)

### 5.3 Pin: never idle (a new engine feature)

- **What it is:** a durable per-session flag, `pinned` (a `SessionStore` key, like `favorite`). A pinned
  session never decays to Idle (`ShouldDecayWaitingToIdle` refuses), and wherever it would land in Idle (a
  restore's normalization, a resume, Move to Idle/Done) it is held in **Waiting for you** instead — the
  state the **Move to Waiting-for-you** button sets. So a pinned session's topic never leaves the list.
- **Where:** `/pin`, `/unpin` in its topic; [📌 Pin] on its card, on the Idle card and on 🔎 Search's
  results ("Revive 📌"); **Pin (never idle)** / **Unpin** in the tab's right-click menu and the board
  card's menu, beside Move to Waiting-for-you; a small 📌 beside the tab's status dot (D32).
- **Unpinning** lets the normal decay resume from that moment.

### 5.4 🔎 Search: find, wake, revive (D24)

- **The agent:** the Sessions page's search agent — `SessionSearchAgent` on the agent harness, Sonnet 5.5,
  its tools (`search`, `show`; the session database's tools when that is On) and system prompt — at
  **high** thinking (the harness's live thinking level). Its own conversation by default
  (`harness\telegram-search.json`), so a search from the phone does not move the PC page's list; sharing
  the page's one conversation is the alternative (D24). Parked after 10 idle minutes like the page's.
- **A message in 🔎 Search is a request** ("the session where I fixed the tab drag crash"); follow-ups
  refine it, as on the page. The agent's answer — one sentence and the sessions it shows — arrives as one
  message (the final message: it makes a sound), each session on a line with its state and its buttons:

| The session is | Buttons |
|---|---|
| Listed (it has a topic) | [→ Topic] (a link if private-chat topic links work, P0-16; else its name) |
| In a tab but Idle | [Wake] [Wake 📌] — the tab's **Move to Waiting-for-you** (+ pin); its topic appears |
| Closed (on disk only) | [Resume] [Resume 📌] [Fork] [Fork 📌] — opened into a tab without taking focus (the window: D27), held in Waiting for you so its topic appears; the tab stays dormant until its first queued message starts it |
| Any | [Details] — the summary card (folder, branch, dates, prompts) |

- **Fleet commands in 🔎 Search:** `/idle` (the idle tabs, each with [Wake]), `/board` (counts per
  state), `/newsession <folder> [model] [-- first prompt]` (a new tab + topic), `/pauseall`,
  `/resumeall`, `/help`.
- **App notes** land here: started, the previous run ended unexpectedly, updated, closed (§4.3).

## 6. Inside a session's topic: the whole session

### 6.1 The card

The topic's pinned message, edited in place:

```
📌 Agentmaster/feature
🔵 Running 3m · Bash: npm test (1m 12s)
K:\source\Agentmaster · agentmaster · Opus 5.5 · high · ctx 182K / 1M
Runner Paused · queue 2 — next: "also update the docs"
✎ unsent at the PC: "and then rebase onto…"
[Queue ▸] [Runner ▸] [Screen] [Transcript] [📌 Unpin] [More ▸]
```

**More ▸** opens the tab's actions as buttons (the same set as the `/commands` of §7.3).

### 6.2 What it is doing, or whether it is idle

- **The card's state line** — the state, since when, and while Running the current step ("Bash: npm
  test"), updated on every state change and at most every 30 s while Running (§6.6). A linked session's
  state comes from the link's exact events (`turn.start`, `turn.complete {reason}`, background work,
  compaction); an unlinked one's from the scanner, as today.
- **The activity draft** — while the session runs, a streamed draft at the bottom of the topic
  (`sendMessageDraft` with the topic's `message_thread_id`) shows the current step and the text being
  written, refreshed every 10–20 s (a draft lives 30 s); at the turn's end it is replaced by the final
  message. A draft is a preview, not a message: no notification, no history (P0-18). Plus Telegram's
  "typing…" (`sendChatAction`) while a tool runs.
- **Idle** — the Idle card of §5.2 during the grace, then no topic.
- **Dormant** (a restored or deactivated tab whose claude is not running) — "⏸ parked: a queued message
  starts it" on the card.

### 6.3 Every message type, configurable (D19)

Each type is **Off**, **Default** or **Verbose**, set per connection (with a preset) and overridable per
topic. The presets are the Copy ▸ Transcript levels, so a topic at *Default* reads like Copy ▸ Transcript
▸ Default.

| Group | Type (id) | Default | Verbose |
|---|---|---|---|
| Prompts | `prompt.pc` — typed at the PC | the text, marked ⌨ | + the time and attachments |
| | `prompt.queue` — sent by the runner | the text, marked ⚙ | + the queue item and attempt |
| | `prompt.topic` — sent from this topic | your own Telegram message, its status as a reaction (§7.1) | + a "sent at" line |
| | `prompt.midturn` — typed at the PC while it worked | the text, marked "(mid-turn)" | same |
| Replies | `reply.final` — the turn's last text | the full text (expandable past 3,500 characters; a file past 4 chunks) | same |
| | `reply.remark` — text between tool calls | one line in the work log | the full text |
| | `thinking` | Off | the thinking text, where the transcript holds it |
| Tools | `tool.bash` | the command line in the work log | + exit code, duration, the output (first 20 lines; the rest as a file) |
| | `tool.read` | the path | + the range |
| | `tool.edit` | the path and "+12 −3" | + the diff (a file when long) |
| | `tool.write` | the path and size | + the content (a file when long) |
| | `tool.search` (Grep, Glob) | the pattern | + the match count and first matches |
| | `tool.web` (WebFetch, WebSearch) | the URL or query | + a result summary |
| | `tool.mcp` | server · tool | + arguments and result |
| | `tool.todo` (TodoWrite) | the checklist (☐/☑), one message edited in place | same |
| | `tool.other` | the tool's name | + its input |
| | `tool.error` — a failed tool result | the error line | the full result |
| Agents | `agent` — a subagent / Task | its brief and its final report, one line each | + the subagent's turns as a file |
| | `teammate` — teammate messages | one line | the full text |
| | `background` — task notifications, background shells | one line | the full text |
| Interactive | `question` — `AskUserQuestion` | the question with buttons (§7.4) | + the options' descriptions |
| | `plan` — `ExitPlanMode` | the plan (expandable) with the approval buttons | same |
| | `permission` — a tool permission prompt | the tool and description with buttons | + the input preview |
| Markers | `command` — a slash command and its output | `/compact` + the output's first line | the full output |
| | `compaction` | "— compacted 412K → 38K —" | + the summary text |
| | `clear`, `rewind`, `restart`, `interrupt` | a one-line marker | same |
| | `api.error` | the error (status, message, a usage limit's reset) | + the request details |
| | `attachment` — images and files in a prompt | "🖼 image" | the image itself |
| | `hook` — hook output | Off | the hook and its output |
| Agentmaster | `state` — state changes | the card only | + a line per change |
| | `queue` — queued, sent, failed, reordered | failures only (+ the card) | every change |
| | `runner` — mode changes, backstop pauses | pauses only | every change |
| | `draft` — the unsent draft at the PC | the card only | + a line when it changes |
| | `tab` — renamed, tagged, favorited, pinned, restarted, deactivated | Off | a line each |
| | `delivery` — enter-retry, lost send, reclaim | lost sends only | every step |

**The presets** (D31: the connection's default preset is *Default*):

| Preset | What it turns on |
|---|---|
| Minimal | prompts, final replies, questions / plans / permissions, errors |
| Compact | + slash commands, markers, agents' briefs and reports, the to-do list |
| Default | + every tool call as a line in the work log, remarks, failed results |
| Full | + Bash output heads and durations, diffstats, agents' and background details |
| Max | everything Verbose, thinking and hooks included |

**Per topic:** `/show <type or group> off|default|verbose`, `/level <preset>`, `/show` alone prints the
topic's grid. Stored per session (`topicView`), so a revived session keeps its view.

### 6.4 How a turn is laid out

1. **The prompt** (when it did not come from this topic): ⌨ or ⚙ and the text — silent.
2. **The work log** — ONE message per turn, created at the first tool call or remark and edited in place
   as the turn runs (tool calls and remarks per the type modes, folded in an expandable quote past a few
   lines); it rolls over into a new message past ~3,500 characters — silent (edits never notify).
3. **Verbose items** — diffs, outputs, contents, thinking: separate messages, or files when long —
   silent.
4. **The final message** — the turn's last text (the reply); or what stopped the turn: the question, the
   plan, the permission prompt, the error, "claude exited", "■ interrupted" — **the only message sent
   with sound** (§6.5). It is posted when the session leaves Running for a state at rest: a linked
   session's `turn.complete` with its reason, an unlinked one's state edge after the toast's holds (an
   outlived turn's background work keeps it Running).

### 6.5 Notifications: silent except the final message (D5)

- Every message in a session's topic goes out with `disable_notification` — Telegram: "Users will receive
  a notification with no sound" — except **the turn's final message**, which uses the default: it alerts
  with sound unless the user muted the topic in Telegram.
- Edits (the card, the work log) never notify; drafts are previews (P0-18).
- 🔎 Search: the agent's answer is its final message (sound); app notes are silent except "ended
  unexpectedly".
- Optional, off by default to keep the rule as specified: "silent while I'm at the PC" (D6) — the final
  message goes silent too while the user is present at the PC (unlocked, input in the last 5 minutes),
  since the PC's own toast already rang.

### 6.6 Pacing: one chat, one budget

Telegram allows about one message per second **per chat**, and every topic of the private chat shares it;
edits are paced from the same budget. The outbox serves, in this order:

1. final messages, questions, plans, permission prompts, answers to the user's own commands;
2. new prompts and new work-log messages;
3. work-log edits (normally every 30 s; stretched up to 2 minutes under load);
4. card edits (on state changes; activity lines thinned first);
5. backfill (§6.7) and verbose files.

**Estimate** for 20 listed sessions, each ending a turn every 3 minutes, 8 running at a time: new messages
~0.33/s (prompt + work log + final per turn), work-log edits ~0.27/s, card edits ~0.22/s — ~0.8/s, close to
the budget. So the pacing is adaptive: under load the work log and the card are edited less often, while a
final message never waits behind them. P0-15 measures the real limit (edits may be lenient; drafts may not
count).

### 6.7 Backfill: when a topic is created for a session with history (D26)

A topic must show the whole session, but a revived 300-turn session cannot be replayed message by message
within the budget. Recommended:

1. the whole transcript at the topic's preset as one `.md` file (complete at once);
2. the last 10 turns as messages, oldest first, at the backfill's priority;
3. then the live mirror (live items for this session wait until 1–2 are out, so the order holds).

The alternative — every turn as messages, oldest first — keeps the topic's own history complete, at the
cost of minutes for a long session ("rebuilding 120 / 480" on the card meanwhile).

## 7. What you do in a topic

### 7.1 A message is a queued prompt (D8)

Anything you type in a session's topic is exactly a prompt — delivered as if typed into the box, recorded
in the session's queue with the origin `Remote`, sanitized like any paste (DELIVERY.md §15) — and it is
**enqueued, never a steer**: it never reaches a running turn.

| The session is | What happens to your message |
|---|---|
| At rest (Waiting for you, Error) | Queued and sent at once (or when the 40 s settle of a session that just came to rest ends) |
| Running | Queued; sent when the turn ends |
| Needs approval (a question, plan or permission prompt is open) | Queued; the bot offers once "Answer the question with this text?" [Answer] — otherwise it is sent after the question is answered |
| Dormant (restored, revived or deactivated: claude not running) | Queued; the tab is started in place without taking focus, and the message is sent once the session is started and settled |
| Done (claude exited) | Queued; [Restart and send] |
| A delivery is in flight | Queued |
| From the backlog (older than 10 minutes) | Asked first (§4.3) |

- **Its place in the queue (D30):** next, ahead of the runner's planned prompts, in the order you sent
  them; delivered at the next ready moment **whatever the runner's mode** (Paused included), not held by
  Pause all (it is your own message, like Send now), and past the question guard (`kAnswersQuestionOk`,
  as the overlay's mail button). The dormant gate, the delivery gate and the settle hold still apply.
- **How it gets in (§2.4):** a linked session receives it as the link's `submit` (`$.prompt.submit`),
  which by construction starts a turn of its own after the running one — never a steer — and never touches
  the input box (the user's draft at the PC is safe); the `ack` marks it sent. An unlinked session gets the
  typed path (`SubmitPrompt`, the draft swap). A message with a photo keeps the typed path.
- **Its status is a reaction on your message** (no extra message): ✍ queued → 👨‍💻 sent, the turn started
  (its echo arrived) → 👎 failed, with a reply giving the reason (lost send, refused, closed).
- **Editing** your message while it is still queued replaces the queued text (`edited_message`); once
  sent, the bot answers "already sent — send the change as a new message".
- Removing a queued message: `/unqueue <n>` (Telegram tells a bot nothing about a deleted message).

### 7.2 Claude Code's slash commands (D22)

A `/command` that is not one of Agentmaster's (§7.3) is Claude Code's, and it is a prompt like any other —
queued, then typed into the session — **except** the commands that leave no record in the session. The
user's rule: if, reopening the session, you would not see the command's messages (`/reload-plugins`,
`/reload-skills`, …), it is not sent from a topic. Also refused: the commands that would take the
conversation out of its tab or end it.

| Kind | Commands (from Claude Code's command reference, 2026-10-16) | From a topic |
|---|---|---|
| Prompts and recorded commands | skills and the user's own commands (`/commit`, `/handover`, …), MCP prompts, `/compact`, `/clear`, `/model <name>`, `/effort <level>`, `/fast on\|off`, `/plan <task>`, `/goal`, `/init`, `/review`, `/code-review`, `/security-review`, `/simplify`, `/subtask`, `/deep-research`, `/loop`, `/schedule`, `/add-dir`, `/output-style <style>`, `/advisor`, `/autocompact`, `/color` (Claude's prompt-bar color) | queued and sent |
| No record (TUI-only) | `/reload-plugins`, `/reload-skills`, `/btw`, `/config`, `/status`, `/help`, `/hooks`, `/permissions`, `/memory`, `/mcp` and `/model` without arguments, `/tasks`, `/workflows`, `/diff`, `/rewind`, `/release-notes`, `/skills`, `/theme`, `/tui`, `/keybindings`, `/scroll-speed`, `/terminal-setup`, `/voice`, `/export`, `/copy`, `/heapdump`, `/ide`, `/chrome`, `/login`, `/logout`, `/upgrade`, `/usage-credits`, `/privacy-settings`, `/bug`, `/feedback`, `/install-github-app`, `/install-slack-app`, `/mobile`, `/radio`, `/stickers`, `/passes`, `/powerup`, `/artifacts`, … | refused: "leaves no record in the session — run it at the PC" |
| Take the conversation out of its tab, move it, or end it | `/resume`, `/branch`, `/fork`, `/background`, `/teleport`, `/desktop`, `/stop`, `/exit` (`/quit`); `/cd` (moves the session's folder, which Agentmaster tracks for the transcript and the tab) | refused, with the Agentmaster equivalent where there is one (`/forksession`, `/newsession`) |

- **The table is provisional** — each row is confirmed by P0-19 (run every command in a scratch session
  and read the transcript; and count which commands already appear in the user's own transcripts). It
  ships as a versioned table, viewable and editable in the cog; a command it does not know (a new
  built-in, a plugin's) is sent, since skills, plugins and the user's commands expand into prompts.
- `/cc <command …>` sends a command to Claude Code even when its name is one of Agentmaster's (the refusal
  table still applies).
- A linked session runs the command through the link's `command` (`$.command.run`); an unlinked one gets it
  typed into its box. Either way it waits its turn in the queue like any prompt.

### 7.3 The tab's actions as `/commands` (D22)

The right-click menu of a tab, as commands in its topic — the non-destructive actions, plus
`/restartsession`. Names never collide with Claude Code's (its `/rename`, `/fork`, `/color`, `/stop`,
`/branch` exist; §7.2).

| Tab menu | Command | Notes |
|---|---|---|
| Rename tab (F2) | `/rename <title>` | renames the tab, the topic and the saved title; also passes `/rename <title>` to Claude Code so its own session name (prompt bar, `/resume` picker) matches (D28) |
| Change tab color | `/tabcolor <name \| #hex \| default>` | the topic's color stays (set at creation) |
| Copy ▸ Session Id / Path / Branch / Current Prompt / Launch CLI | `/sessionid`, `/path`, `/gitbranch`, `/draft`, `/launchcli` | a message with a **Copy** button (≤ 256 characters; a code block when longer) |
| Copy ▸ Summary | `/summary` | the session's summary box |
| Copy ▸ Transcript ▸ level, Show Transcript | `/transcript [minimal\|compact\|default\|full\|max]` | a `.md` file |
| Mark Unread | `/markunread` | |
| Move to Idle/Done | `/moveidle` | the topic leaves the list after the idle grace |
| *(new)* Pin / Unpin | `/pin`, `/unpin` | §5.3 |
| Favorite / Unfavorite | `/favorite`, `/unfavorite` | |
| Tags | `/tags`, `/tag <name>`, `/untag <name>` | |
| Activate Tab | `/activate` | starts a dormant tab's claude in place, without focus |
| Fork session ▸ (models, presets) | `/forksession [model \| @preset]` | a new tab + a new topic, held in Waiting for you |
| New Session Here ▸ (models, presets) | `/newsession [model \| @preset]` | the same folder; a new tab + topic |
| Restart session | `/restartsession` | a confirmation while Running (the turn is cut) |
| Deactivate ▸ Deactivate Tab | `/deactivate` | parks the tab (its claude ends, the conversation resumes on the next message); a confirmation while Running |
| Deactivate ▸ other scopes, Close ▸ (all), Move ▸, Split tab, Close pane, Export text, Find, Open In Explorer | — | destructive, other tabs, layout, or the PC's screen only |

**Not on the tab menu, but the session's own:** `/queue` (the queue, numbered), `/sendnow` (send the next
queued prompt now), `/unqueue <n>`, `/runner auto|paused`, `/takedraft` (queue the unsent draft at the
PC — the overlay's mail button), `/screen` (the visible terminal text, redacted), `/show`, `/level`
(§6.3), `/interrupt` (stops a running turn, with a confirmation — D29: the link's `abort` →
`$.turn.abort` in a linked session, one Esc through the hosting window otherwise), `/help`. Telegram's "/"
menu (`setMyCommands`, one list for the whole chat) lists these.

### 7.4 Questions, plans and permission prompts as buttons (D9)

- **What is open:** in a linked session, the link's `question` / `permission` events (from `tool.call` on
  `AskUserQuestion` / `ExitPlanMode`, carrying the questions; MOD_LINK §4.4); otherwise the transcript's
  pending `tool_use` (the questions with their options and `multiSelect`; the plan text) — a new bounded
  capture in the scanner, `pendingQuestion`. Plus, in both, the menu on screen, read by the hosting window
  (`ReadMenuSnapshot`; pure parser `MenuSnapshot.h` over captured screens), the only source for the plan's
  approval choices and a permission prompt.
- **Shown as:** the question with one button per option, **Other…** (a reply to it is the free text), and
  toggles + **Submit** for multi-select.
- **Answered by:** the hosting window, under the delivery gate (reserved tag `#menu-answer`): re-read the
  menu, check it is the same menu, send the measured key plan (P0-1), verify the menu is gone and the
  transcript's tool result carries the answer. Anything else: "not confirmed — look at the terminal".
- **Answered elsewhere** (the terminal, the Claude app): the message is edited to the answer given.

### 7.5 Action tokens

A button carries `a:<token>` (10 base-62 characters; `callback_data` holds 64 bytes). A token maps to
`{connection, session, intent, binds}`, where `binds` names exactly what it acts on (the question's
`tool_use` id, the queue item, the session's state stamp); a mismatch refuses with the current state.
Action tokens are single use and expire (2 h; reads 24 h).

### 7.6 Later: files, photos, voice

A photo or file in a topic → saved under the session's folder (`.agentmaster\inbox\`) and referenced in
the queued prompt as an `@` path; a voice note → a transcription connector.

## 8. The Fleet Control API (engine)

### 8.1 Calls

```
Topics:   QualifyingTabs() → [(session, window, title, color, state, pinned)]       the sync's input
Read:     Card(id) / Board() / Idle() / Tail(id, n, level) / Transcript(id, level) / Screen(id) / Summary(id)
Search:   (the Search agent's tools)
Prompts:  Enqueue(id, text, origin=Remote, sourceRef) / Unqueue(id, n) / SendNext(id) / TakeDraft(id)
Runner:   RunnerMode(id, mode) / PauseAll(on)
Menus:    Answer(id, menuStamp, choice) / Interrupt(id)
Tab:      Rename / TabColor / MarkUnread / MoveIdle / Wake / Pin / Favorite / Tags / Activate / Deactivate /
          RestartSession / ForkSession(model) / NewSession(model) / Revive(id, resume|fork, pin)
```

Every call returns `{ok, code, text}` (`queued`, `sent`, `refused:level`, `refused:no-record`,
`refused:stale`, `refused:backlog`, `refused:unknown-session`, `refused:no-tab`, `failed:…`) and logs one
`[nav] remote …` line.

### 8.2 Window-hosted actions

New per-window sinks of the existing shape (registered at engine init, detached in `~TerminalPage`,
Rule #10): **key plan** (menus, interrupt), **screen read**, **launch** (resume, fork, new — without
taking focus), **tab action** (rename, color, mark unread, triage moves, favorite, tags, activate,
deactivate, restart). A sink that finds no host answers `refused:no-tab`.

### 8.3 New engine features this needs

| Feature | Where |
|---|---|
| **Pin** (never idle): the `pinned` store key, the decay refusal, the hold in Waiting for you, the tab and board menu items, the 📌 glyph | `SessionStore`, `SessionScanner` (`ShouldDecayWaitingToIdle`), the registry's state seam, `Tab.cpp`, the board |
| **Remote queue items**: `PromptOrigin::Remote` (serialized `"Remote"`), next-in-line placement, delivered whatever the runner mode and Pause all, `kAnswersQuestionOk` | `SessionModels.h`, `Scheduler::DecideAdvance` |
| **The session link** (MOD_LINK.md) as the delivery, command, abort and state route of linked sessions (§2.4); messaging does not wait for it — P1 ships on the typed path and linked sessions switch over when the link exists | `LinkServer` (MOD_LINK §7) |
| **Open without focus**: resume, fork and new session into a chosen window as a dormant tab, then held in Waiting for you | `TerminalPage.AgentSessions.cpp` |
| **The transcript follower**: per listed session, new transcript lines → typed items (§6.3), built on `TranscriptTurns`' parsing, incremental | `AgentMaster/` |
| **Menus**: `pendingQuestion`, `ReadMenuSnapshot`, key plans | scanner, `ControlCore`, the hosting window |
| **The command table**: recorded / no record / leaves the tab (§7.2) | `AgentMaster/` |

### 8.4 Other consumers

The same API serves CLI.md's deferred P2 / P3 (`restore`, `archive`, `watch`, `enqueue`, `send-now`,
`set-autorunner`) and a future MCP control server; exposing it to them is a separate decision.

## 9. Security

### 9.1 Threat model

| Threat | Rule |
|---|---|
| A stranger messages the bot | Sender allowlist by user id; dropped after reading only the sender (H3); pairing answers "this bot is private" (rate-limited) |
| The owner's Telegram account is taken over | Permission levels, an optional drive PIN, confirmations on restart / deactivate / interrupt, every action audited |
| A stale button, an old backlog message | Tokens bound to what they act on (§7.5); the backlog rule (§4.3) |
| A leaked bot token | The thief can read new messages first (a 409 we did not cause raises an alert) and post as the bot — not drive the fleet; rotate in BotFather |
| A hostile POST (Teams) | H4 |
| A parser bug fed by internet input, inside Agentmaster | H1–H6 |
| Secrets in a mirrored session | The redactor, always on (§9.6) |

### 9.2 Pairing

The cog shows a one-time code (6 digits, 10 minutes) and `https://t.me/<bot>?start=<code>` (with a QR).
Opening it binds the owner to that `from.id` and the private chat, creates 🔎 Search, and runs the first
topic sync. Teams Bot: `pair <code>` in the 1:1 chat stores `aadObjectId`, tenant and conversation
reference.

### 9.3 Levels, the drive PIN, confirmations

| Level | Allows |
|---|---|
| **Read** | the topics' mirror, 🔎 Search's searching, `/screen`, `/transcript`, `/summary`, `/queue`, the copy commands |
| **Steer** | + messages (queued prompts), Claude Code's commands, answers, `/unqueue`, `/sendnow`, `/runner`, `/takedraft`, `/pin`, `/moveidle`, Wake, `/rename`, `/tabcolor`, `/markunread`, `/favorite`, tags, `/show`, `/level` |
| **Operate** | + `/restartsession`, `/deactivate`, `/activate`, `/forksession`, `/newsession`, Resume / Fork from 🔎 Search, a new topic → a new session, `/interrupt`, `/pauseall` |

- The default for a paired owner (D4): **Operate**, since the topics exist to manage the sessions;
  confirmations on `/restartsession`, `/deactivate` and `/interrupt` while a turn runs.
- **Drive PIN** (optional): Steer and Operate refused until `/unlock <PIN>` (30 minutes); salted hash in
  Credential Manager; 5 wrong tries lock for 15 minutes; the PIN message is deleted after reading.

### 9.4 Secrets

Bot tokens, webhook URLs, the Teams app secret, the tunnel token and the PIN hash: Windows Credential
Manager only — never in `connections.json`, settings, logs, the catalog, artifacts or commits (M4).

### 9.5 Audit

`[nav] remote <connection> <verb> <sid8> → <code>` in hooks.log for every action from a chat, the user id
hashed.

### 9.6 Privacy and redaction

- A topic carries the whole session (D19), so every prompt, reply and tool call of every listed session
  goes to Telegram's servers (cloud chats, not end-to-end). The cog says so plainly; the message-type grid
  (§6.3) is also the privacy control (tool outputs, file contents and thinking are Verbose-only by default).
- **Redaction, always on, in the engine, before any text reaches the hub:** `RedactSecrets`, the redactor
  SESSIONS_AGENT.md §8.4 specifies (GitHub, Anthropic, OpenAI, AWS, Slack, Google, Stripe and Azure key
  shapes, JWTs, PEM private keys, high-entropy values after `password` / `secret` / `token` / `api_key`)
  → `[redacted <kind>]`, plus the `telegram` bot-token shape. Files (diffs, outputs, transcripts) pass
  through it too.
- `protect_content` (no forwarding or saving) is a per-connection option; Telegram's auto-delete timer is
  the user's own.
- One-way providers (Teams Lite, webhooks) keep the Minimal / Standard / Full levels for their alerts.

## 10. The Telegram connector

### 10.1 Setup (about five minutes)

1. @BotFather → `/newbot` → copy the token (a bot per install; never the homelab's bot).
2. The BotFather **Mini App** → My bots → the bot → Bot Settings → Threads Settings → **Threaded Mode**
   on; "users may create topics" on when a new topic should start a session (D20).
3. Cog → MESSAGING → Add → Telegram → paste → **Check** (`getMe`: the name; `has_topics_enabled`).
4. **Pair** (link or QR) → 🔎 Search appears; pin it at the top in your Telegram client.
5. The first sync creates a topic for each qualifying tab (§6.7 paces the backfill, Waiting-for-you and
   Needs-approval sessions first).

### 10.2 Receiving

- `getUpdates(offset, timeout=50, allowed_updates=["message","edited_message","callback_query",
  "stopped_message_generation"])`; at start the backlog first. The offset is saved once FleetLink took or
  refused each update (at least once; intents carry the update id, so repeats are dropped).
- `message_thread_id` routes a message to its topic's session; a topic id the map does not know, with a
  `forum_topic_created`, starts the new-session flow (§5.1).
- Never a webhook for this provider; one poller per token (a machine-wide mutex named from the token's
  hash; a 409 stops the loop and alerts — M7).
- Callback queries are answered at once ("working…"); the message is edited when FleetLink returns.

### 10.3 Sending

HTML mode (`<`, `>`, `&` escaped); code and screens in `<pre>`; work logs and long replies in
`<blockquote expandable>`; 4,096 characters per message, split on paragraphs, never inside a `<pre>`;
past 4 chunks a file; buttons with `style` (`primary` for the safe action, `danger` for restart /
deactivate / interrupt); `<tg-time unix=… format="r">` for times that stay current; reactions for a
queued message's status; `disable_notification` on everything but a turn's final message.

### 10.4 What a topic cannot do

| Limit | What stands in |
|---|---|
| The terminal's screen-only parts: spinners, the TUI's own pickers and panels, the input box | `/screen` (a text snapshot); menus as buttons (§7.4); the unsent draft on the card |
| Keyboard-only actions: Shift+Tab (modes), double Esc (rewind), Ctrl+B | A few as measured key plans later; the rest at the PC |
| Claude Code commands that leave no record | Refused (§7.2), the user's rule |
| ~1 message per second for the whole chat | The work log + card + final message layout, adaptive pacing (§6.6) |
| A deleted topic loses its messages; private-chat topics cannot be hidden or closed | Idle topics are deleted after a grace; a revived session's topic is rebuilt (§6.7) |
| A topic's color is fixed at creation | Tab color changes do not follow |
| No way to list topics or to learn that one was deleted | Agentmaster's topic map, written atomically with a previous copy |
| Telegram checklists are business-only | The to-do list as a text checklist edited in place |
| Nothing while Agentmaster is closed (D1) | 24 h of backlog, handled at the next start |
| Merely reading a topic is invisible to a bot | Only actions mark a session read (§5.2) |

### 10.5 Failure modes

| Symptom | Handling |
|---|---|
| 401 (token revoked) | stop, alert in the cog and with a Windows toast |
| 403 (the user blocked the bot) | stop; status "blocked by the user" |
| 409 | §10.2 |
| 429 | wait `retry_after`; the pacing lowers its rate |
| "message thread not found" (a deleted topic) | the topic is created again |
| The topic map lost | the old topics are orphans the bot cannot find; new ones are created; the previous copy of `state.json` is tried first |

## 11. Microsoft Teams

- **Teams Lite (P5):** a Workflows webhook ("Post to a chat when a webhook request is received"; the URL
  is a credential) — one-way **alerts**: each listed session's final messages (§6.4), as Adaptive Cards
  with links only; no edits (the call returns no message reference — P0-6). Appears as the Workflows bot.
- **Teams Bot (P6):** needs a work tenant with **Upload custom apps**, a single-tenant Azure Bot (free
  tier), an app package the cog generates, and a public HTTPS endpoint — **Cloudflare Tunnel on the PC**,
  started by Agentmaster (`cloudflared tunnel run --token …`, loopback target; D12), or a dev tunnel or
  ngrok. Receiving: JWT checked against the Bot Framework OpenID metadata (issuer set for a single-tenant
  bot: P0-7); `message`, `invoke adaptiveCard/action` (Universal Actions). Sending: client-credentials
  token from the tenant, `POST {serviceUrl}/v3/conversations/{id}/activities`, edits with `PUT`.
- **The topic model on Teams:** a 1:1 chat has no threads; a Team channel does — one thread per listed
  tab (the card as the thread's first post, edited; the turns as replies). Limits: 7 / 1 s, 60 / 30 s
  per conversation; ~100 KB per message; streams die after 2 minutes (edit instead).
- Agentmaster closed = the bot unreachable: Teams shows the message as failed (no backlog).

## 12. Any other messenger

### 12.1 Webhook (built in, one-way)

`POST` to a URL with a body template (`{{title}}`, `{{state}}`, `{{text}}`, `{{ref}}`, …): alerts from
listed sessions' final messages. Presets: ntfy, Pushover, Gotify, Discord and Slack incoming webhooks,
Home Assistant, n8n.

### 12.2 The external connector contract

- A connector is a folder `<profile>\messaging\connectors\<id>\connector.json` (`command`, `args`,
  `settings`, `secrets`) and a process Agentmaster starts as a child (kill-on-close job), speaking
  JSON-RPC 2.0, one object per line, over stdio. The bot — rules, state, decisions — stays in Agentmaster.
- Agentmaster → connector: `initialize {protocol, connection, secrets}` → `{capabilities}` (threads?
  edits? buttons? drafts? silent sends? limits); `ensureThread {session, title, color}` → `{thread}`;
  `renameThread`, `deleteThread`; `send {thread, render {plain, markdown, html, card}, buttons, silent}` →
  `{ref}`; `edit`, `delete`, `react`, `draft`; `ack`; `shutdown`.
- Connector → Agentmaster: `inbound {id, user, chat, thread?, replyTo?, newThread?, text?, edited?,
  token?, stop?, sentAt}`, `status`, `log`.
- A platform with threads (Slack threads, Discord forum channels, Matrix threads) gets the topic model; one
  without gets alerts and reply chains.
- Samples (P7): Slack Socket Mode (Python), Discord gateway (Node), Matrix (Python).

### 12.3 Push-in platforms

WhatsApp, a Teams bot and other webhook-only platforms share Agentmaster's loopback listener and the
tunnel (one path per connection); the provider's signature check is part of its connector.

## 13. Settings and UI

- **Cog → a new MESSAGING tab:** a master switch (default off); connections (status dot, last delivery,
  **Test**, **Edit**, **Remove**); **Add** (Telegram / Teams Lite / Teams Bot / Webhook / External…).
- **Per Telegram connection:** pairing (code, link, QR); level and drive PIN; **the message-type grid**
  (a preset selector over the §6.3 table, Off / Default / Verbose per row); the idle grace (D25); the
  backfill depth (D26); the window revived sessions open in (D27); "silent while I'm at the PC" (D6);
  routing (all tabs / favorites / tags / folders); a new topic starts a session (D20); 🔎 Search's
  thinking level (default high) and conversation (own / shared with the Sessions page, D24); the slash
  command table (§7.2: view, edit, reset).
- **Tab and board menus:** **Pin (never idle)** / **Unpin** beside Move to Waiting-for-you; a 📌 beside
  the status dot of a pinned tab.
- **Manager toolbar:** a status pill (📱 + a dot per connection), its tooltip naming states and the
  number of listed topics.

## 14. Logs

- hooks.log: `[msg] topic created|renamed|idle|deleted <sid8> (<why>)`; `[msg] <sid8> queued from topic
  #<n> → sent|failed (<why>)`; `[msg] <sid8> refused /<command> (no record | leaves the tab)`;
  `[msg] backlog N updates (oldest <age>)`; `[msg] pacing <rate>/s (budget <b>)`; `[nav] remote
  <connection> <verb> <sid8> → <code>`.
- messaging.log (the connectors' transport, via `AppendStateLog`): `[tg]`, `[teams]`, `[hook]`,
  `[ext:<id>]` — HTTP status, latency, `retry_after`, poll gaps, restarts. Never a message body or a token.
- Rule #18 everywhere (H1).

## 15. Testing

- **m5 (pure):** the topic sync decider (tabs × states × pins → creates / renames / deletes, the idle
  grace); the pin rule (no decay, the hold in Waiting); the message-type renderer at every preset against
  the TranscriptTurns fixtures (goldens); the work-log builder and its roll-over; the final-message
  marker (exactly one sound per turn); the pacing scheduler (priorities, adaptive intervals); the backlog
  rule; the remote queue item in `DecideAdvance` (Paused, Pause all, question guard, dormant); the slash
  command router (Agentmaster / recorded / refused, `/cc`); the reaction state machine; tokens; the
  redactor; Telegram escaping and chunking; `MenuSnapshot`.
- **Connector integration:** a fake Telegram server on loopback (topics, drafts, reactions, edits, 429s,
  a backlog, deleted topics).
- **Abuse:** strangers, replayed updates, stale tokens, PIN guessing, huge and deeply nested JSON, a 409
  storm, a connector that throws on every update.
- **Live (by hand):** a test bot; the claude-wrap-ConPTY driver for the menu key plans and the command
  table (P0-1, P0-19); the cog on a disposable Windows desktop (claude-desktops).

## 16. Proposed invariants (to join the Correctness rules when built)

- **M1 — One send seam.** A message from a topic goes through the Prompt Queue and `SubmitPrompt`; a key
  press through the hosting window under the delivery gate; no connector touches a ConPTY or the registry.
- **M2 — Sender, not chat.** Every inbound message is checked by the platform's user id before anything
  else reads it.
- **M3 — FleetLink authorizes**, in its own read of `connections.json`.
- **M4 — Never send a secret.** Redaction in the engine before the hub; credentials in Credential Manager;
  no message body in a log.
- **M5 — Never act on a stale button.**
- **M6 — Never guess a session.** A message outside a session's topic names no session; only 🔎 Search
  and new topics take such messages.
- **M7 — One poller per token.**
- **M8 — Topics are the tabs.** A topic exists exactly for each Claude Code tab whose session is not Idle
  (pinned ones never are), within the idle grace; Agentmaster creates, renames and deletes topics to keep
  it so.
- **M9 — An old message never acts unasked** (the backlog rule).
- **M10 — The network never stalls the app** (H1).
- **M11 — A message is a queued prompt.** Nothing typed in a topic reaches a running turn.
- **M12 — One sound per turn.** Only a turn's final message is sent with sound; everything else is silent,
  an edit or a draft.
- **M13 — No hidden commands.** A Claude Code command that leaves no record in the session is never sent
  from a topic.

## 17. Phases

| Phase | Scope | Exit |
|---|---|---|
| **P0** | The measurements of §19 | Every P0 row answered here |
| **P1** — topics = tabs | The hub and the Telegram connector in the engine (H1–H5); pairing; 🔎 Search (fleet commands, app notes; no agent yet); the topic sync with the idle grace; the card; the turn layout at the Default preset (prompts, work log, final message with sound); messages as remote queue items with reaction status; the backlog rule; pacing; the MESSAGING tab | The topic list equals the non-idle Claude tabs of every window, live; a session's turns appear as they happen; a message from a topic is sent at the turn's end and shows 👨‍💻; only final messages make a sound |
| **P2** — act | Pin (engine + menus); the tab commands (§7.3); Claude Code's commands with the refusal table (P0-19); questions, plans and permission prompts as buttons (P0-1); `/interrupt` | Every §7.3 command works from a topic; a question answered from the phone, verified |
| **P3** — the whole session, configurable | The full §6.3 grid, presets, `/show` and `/level`; verbose files; the activity draft; backfill options | A topic at Max matches the transcript; at Minimal only prompts and final replies |
| **P4** — 🔎 Search | The Search agent (own conversation, high thinking); Wake, Resume, Fork (+📌) opening tabs without focus | An old session found from the phone and revived into a tab and a topic |
| **P5** — Teams Lite + Webhook | Alerts from final messages | Alerts in a Teams chat and in ntfy |
| **P6** — Teams Bot | Endpoint (cloudflared), JWT, cards, a thread per tab in a Team channel | Two-way in the user's tenant |
| **P7** — external connectors | The contract; three samples | A Slack and a Discord sample pass the suite |

## 18. Decisions

**Decided by the user** (2026-10-16):

| # | Decision | The user's words |
|---|---|---|
| **D1** | The bot runs inside the running Agentmaster process; external connectors and `cloudflared` are child processes it starts | "The agentmaster process running is supposed to be the program running the bot." |
| **D3** | Topics are synced to the tabs of all windows; idle tabs are the only ones not listed; 🔎 Search is the pinned top topic | "The topics are synced to the tabs in all our windows. Tabs idle are the only ones not listed." |
| **D5** | Every message in a topic is silent (`disable_notification`) except the turn's final message | "Only the final turn's message are sending the notification with default" |
| **D8** | A message sent in a topic is enqueued, never a steer | "Sending a message to the topic is enqueueing a message (not steer)." |
| **D19** | A topic shows the whole session; every message type is configurable and can be disabled, default or verbose | "every kind of message or type of message is configurable extensively and can be disabled (supporting default or verbose)" |
| **D21** | A closed tab's topic is deleted; an idle one leaves after the grace (follows from D3) | — |
| **D22** | A message is exactly a prompt, Claude Code's commands included, except those that leave no record in the session; the tab menu's non-destructive actions (plus `/restartsession`) are `/commands` | "any messages are exactly like sending a prompt (to an exclusion of some /slash commands like /reload …)"; "We are able to run most of functions … of a tab through a /slash command, non destructive ones (but incl /restartsession)" |
| **D23** | Claude Code only for now | "We only integrate with claude code for now." |
| **D24** (part) | 🔎 Search runs the Sessions page's search agent, Sonnet 5.5 at high thinking; it searches, wakes, resumes and forks; a pin keeps a session from going idle | "a sonnet 5.5 agent high thinking (same one from the sessions page search) …" |

**Open** (recommendation first):

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| **D2** | Messages sent while Agentmaster was closed (Telegram keeps them 24 h) | Handled at the next start; older than 10 min → asked before queueing; Operate never from the backlog | Drop them |
| **D4** | A paired owner's level | **Operate** with confirmations on restart / deactivate / interrupt while running; the drive PIN offered, off | Steer |
| **D6** | "Silent while I'm at the PC" (the final message too) | Offered, **off** (the rule as specified) | On by default |
| **D9** | Answering questions and plans from a topic | Yes, by verified key plans (§7.4) | Leave them to the terminal |
| **D10–D12** | Teams: route, tenant, endpoint | Teams Lite first; the tenant is the user's call; Cloudflare Tunnel on the PC for the bot | Bot first; dev tunnel / ngrok |
| **D13** | Bots per install | One for release, one for dev | One install only |
| **D15** | The external contract | JSON-RPC over stdio, a child process | In-process plugins |
| **D16** | The route into a session | **The session link** (MOD_LINK.md) wherever a session has it; the typed path otherwise; Claude Code channels not used | Channels, with the development flag |
| **D17** | The Remote Control presence file | Yes | No |
| **D18** | The UI name | Messaging | Remote; Phone |
| **D20** | A topic started in Telegram | Starts a new session (asks for the folder) | Ignored; `/newsession` in 🔎 Search only |
| **D24** (rest) | 🔎 Search's conversation | Its own (the PC page keeps its own list) | Shared with the Sessions page |
| **D25** | The idle grace before a topic is deleted | 10 minutes, with [📌 Pin] and [Wake] on the Idle card | 0 (at once); 1 hour |
| **D26** | Backfill of a new topic for a session with history | The whole transcript as a file + the last 10 turns as messages | Every turn as messages, paced |
| **D27** | Where a revived closed session opens | The most recently active window, without focus, dormant until its first message | A fixed window; the window it last lived in |
| **D28** | `/rename` also renames Claude Code's own session name | Yes | Agentmaster's title only |
| **D29** | `/interrupt` from a topic | Yes, Operate, with a confirmation | No |
| **D30** | A topic message's place in the queue | Next, ahead of the runner's planned prompts; sent whatever the runner mode; not held by Pause all; past the question guard | Last in the queue, under the runner's mode |
| **D31** | The default message-type preset | Default (like Copy ▸ Transcript ▸ Default) | Compact; Full |
| **D32** | The pin's mark on the PC | A 📌 beside the tab's status dot | None |

## 19. P0 — measure before code

| # | Question | How |
|---|---|---|
| P0-1 | The key plan of each menu: `AskUserQuestion` (single, multi, several questions, "Other"), plan approval, a permission prompt | claude-wrap-ConPTY driver |
| P0-2 | `SubmitPrompt` right after a turn ends, and into a dormant tab that was just started | the same driver |
| P0-3 | How the transcript records `AskUserQuestion` and its answer | the corpus |
| P0-4 | Per-topic pinned messages (the card) on Android, iOS, Desktop | a test bot |
| P0-6 | Teams: custom app upload, Workflows, phone alerts in the user's tenant | the user's Teams |
| P0-7 | Bot Connector JWT issuers for a single-tenant bot | Microsoft's page + one real request |
| P0-8 | A loopback listener without admin (HTTP Server API URL rules) | a probe |
| P0-9 | `CLAUDE_CLIENT_PRESENCE_FILE` at launch suppresses Remote Control pushes | one session |
| P0-11 | The clients in Threaded Mode: does a main-view message always start a topic (Telegram-iOS #2236)? Where do the bot's messages without a thread id appear? | a test bot on each client |
| P0-12 | Does `editForumTopic` (a rename) post a visible message, and does it notify? | a test bot |
| P0-13 | Can the user rename or delete a topic in a bot's chat, and what does the bot receive? | a test bot |
| P0-14 | 100+ topics: the list, search, unread counts, any limit | a test bot |
| P0-15 | The real pacing: messages and edits per second across topics before a 429; do drafts count? | a test bot |
| P0-16 | Can the user pin 🔎 Search at the top? Do links to a private-chat topic work? | each client |
| P0-17 | `setMessageReaction` on the user's messages in a private-chat topic | a test bot |
| P0-18 | Drafts in a topic: notifications, a 10–20 s refresh for an hour, the stop update | a test bot |
| P0-19 | Which Claude Code commands leave a record in the session (the §7.2 table) | every command in a scratch session + the user's own transcripts |
| P0-20 | `edited_message` for a message in a private-chat topic | a test bot |
| P0-21 | Topic creation and deletion: is the user notified, is a deletion instant on every client? | a test bot |

| P0-22 | Through the session link: can a mod answer an open `AskUserQuestion` / plan / permission dialog, or only observe it (`$.prompt.fill` answers `isFilled: false` under a dialog)? | MOD_LINK's probes (its Q4, Q6) |

(P0-5 and P0-10 of the earlier drafts are retired: drafts are P0-18; channels are not used, D16.)

## 20. Sources (read 2026-10-15/16)

- Telegram: [the Bot API](https://core.telegram.org/bots/api) (getting updates and the 24-hour retention, topics, drafts, reactions, copy buttons, checklists, chat actions), [the changelog](https://core.telegram.org/bots/api-changelog) (9.3 – 10.3), [the Bots FAQ](https://core.telegram.org/bots/faq), [Bot Features](https://core.telegram.org/bots/features), [the October 2025 announcement](https://telegram.org/blog/comments-in-video-chats-threads-for-bots), [Telegram-iOS #2236](https://github.com/TelegramMessenger/Telegram-iOS/issues/2236); Threaded Mode's location: [Hermes Agent's Telegram docs](https://hermes-agent.nousresearch.com/docs/user-guide/messaging/telegram), [its PR #116803](https://github.com/NousResearch/hermes-agent/pull/116803).
- Claude Code: [Commands](https://code.claude.com/docs/en/commands) (the command list of §7.2), [channels](https://code.claude.com/docs/en/channels), [the channels reference](https://code.claude.com/docs/en/channels-reference), [Remote Control](https://code.claude.com/docs/en/remote-control).
- Teams: [Bot Framework SDK status](https://learn.microsoft.com/en-us/azure/bot-service/bot-service-overview?view=azure-bot-service-4.0), [Teams SDK 2.1](https://microsoft.github.io/teams-sdk/blog/announcing-teams-sdk-dotnet-2-1/), [Azure Bot resource](https://learn.microsoft.com/en-us/azure/bot-service/abs-quickstart?view=azure-bot-service-4.0), [rate limits](https://learn.microsoft.com/en-us/microsoftteams/platform/bots/how-to/rate-limit), [message size](https://learn.microsoft.com/en-us/microsoftteams/platform/bots/how-to/format-your-bot-messages), [streaming](https://learn.microsoft.com/en-us/microsoftteams/platform/bots/streaming-ux), [prerequisites](https://learn.microsoft.com/en-us/microsoftteams/platform/toolkit/tools-prerequisites), [the Teams API overview](https://learn.microsoft.com/en-us/graph/api/resources/teams-api-overview?view=graph-rest-1.0), [send a chat message](https://learn.microsoft.com/en-us/graph/api/chat-post-messages?view=graph-rest-1.0), [the connectors' retirement](https://devblogs.microsoft.com/microsoft365dev/retirement-of-office-365-connectors-within-microsoft-teams/), [MC1181996](https://mc.merill.net/message/MC1181996).
- Others: [Slack Socket Mode](https://docs.slack.dev/apis/events-api/using-socket-mode/), [Discord gateway](https://docs.discord.com/developers/events/gateway), [matterbridge releases](https://github.com/42wim/matterbridge/releases).
- The homelab's Telegram bot: the `fastrun` skill (`~/.claude/skills/fastrun/SKILL.md`).

## 21. What was built (2026-10-16) — Telegram v1

The user's instruction: "Use my `K:\source\telegram\SidKatrineBot.env` to be used by our setup on this
machine. Your goal is to implement this end to end." Built in one pass, against the design above; this
section is the record of what the code does today, where it differs from §4–§10, and what is still open.

### 21.1 The code

| Piece | File | What |
|---|---|---|
| The pure text half | `AgentMaster/TelegramFormat.h` | HTML escaping (`TgEscapeHtml`), Claude's Markdown → Telegram HTML (`TgMarkdownToHtml`: fences with their language, code spans, `**bold**`, `~~strike~~`, links, headings as bold, `> quotes`, bullets; italic and tables deliberately not converted), splitting at Telegram's 4,096 characters with `<pre>` kept balanced (`TgChunkHtml`), the nearest of the six topic colors to a tab color (`TgTopicIconColor`), short spans. |
| The pure core | `AgentMaster/MessagingModels.h` | What a transcript line means for a topic (`ClassifyTranscriptLine` → `TopicItem`: prompt / mid-turn prompt / text with `turnEnds` / tool call with a one-line summary / failed result / question with its options / plan / compaction / interrupt / API error / command echo; noise, meta, sidechain and the resume placeholder skipped); `PlanTopicSync` (topics = tabs, M8: creates, deletes, renames, the Idle card and the grace); `ParseRemoteCommand` (Agentmaster's `/commands`, Claude Code's commands, the no-record and leaves-the-tab refusal tables, `/cc`); the state file (`MessagingState` ⇄ JSON); the backlog rule (`IsBacklogUpdate`, 10 min); the card (`BuildCardText`). |
| The HTTPS client | `AgentMaster/TelegramApi.{h,cpp}` | WinHTTP POSTs with a JSON body, the error body read (`retry_after`, `description`), `sendDocument` as multipart, an `Abort` another thread can call (the long poll returns at once). The token lives only in this object. |
| The service | `AgentMaster/Messaging.{h,cpp}` + `Messaging.Inbound.cpp` | `MessagingService`, one per process, created by `SharedEngine()` (`Engine.messaging`): the three threads (poll / send / tick), the token in Credential Manager, the one-poller-per-token machine mutex (M7), the state file (`<profile>\messaging\telegram\state.json` + `.prev`), the outbox with priorities and the ~1 s pacing, Threaded-Mode detection with a one-a-day setup notice, the Search topic, the topic sync, the transcript mirror (prompts, ONE work-log message per turn edited in place, the final message with sound), the cards, reactions, the inbound half (updates, `/commands`, buttons, tokens, the backlog question, remote prompts, revives). |
| The Search agent | `MessagingSearchBridge.{h,cpp}` (TerminalApp-only) | A `SessionSearchAgent` named `telegram-search` (its own conversation, `<profile>\harness\telegram-search*.json`), high thinking by default, behind the service's `SearchBridge` seam. |
| The window half | `TerminalPage.AgentMessaging.cpp` + two engine sinks (`Engine.h`: `RegisterWindowSessionActionHandler` / `RequestSessionActionInWindows`, `RegisterWindowLaunchHandler` / `RequestLaunchInWindow`, `NoteWindowActivated`) | `_RemoteSessionAction` (start a dormant tab, read the screen, mark unread, rename, repaint tags / favorite — only the hosting window acts) and `_RemoteLaunch` (resume / fork / new session as a BACKGROUND tab of the most recently activated window, D27). Restart and park reuse the existing cross-window seams. |
| The engine rules | `Scheduler.h` (`NextPromptIsRemote`, in `DecideAdvance` and `_wantsIdleStart`), `SessionModels.h` (`QueuedPrompt::remoteSource`, `SessionInfo::pinned`, `ApplyPinHold`), `SessionRegistry.cpp` (the pin hold in Upsert / Update / OnHookEvent), `SessionScanner.cpp` (the decay refuses a pinned session), `Persistence.cpp` (both keys) | D30: a remote prompt is sent whatever the runner's mode, past Pause all and the send budget, and past the question guard (it IS the answer); every other gate holds. §5.3: a pinned live session is held in Waiting for you wherever it would land in Idle. |
| The settings | `AppSettings::messagingEnabled` / `messagingTelegramOwnerId` / `messagingIdleGraceMinutes` / `messagingSearchThinking`; the cog's **Messaging** tab (`AgentManagerContent.Settings.cpp`: the switch, the token box, the owner id, Check bot, Forget token, the grace, the thinking level, a status line) | Save stores the token in Credential Manager FIRST (`MessagingService::StoreToken`, target `Agentmaster:messaging:telegram:<profile leaf>`), then the page's handler calls `messaging->Apply(settings)`. |
| Tests | `tests/tests_messaging.cpp` (`TestMessaging`) | The formatting, the classifier on synthetic lines, the sync decider with the grace, the command router, the state file, the card, the D30 bypass in `DecideAdvance`, the pin hold through the registry, the two persisted keys. |

Logs: `[msg] …` in hooks.log (started / stopped / connected / topic created / renamed / idle / deleted /
queued from topic / final message / revive done / state edges per session), `[nav] remote telegram …` for
every action from the chat (never the token, never a message body), `[tg] …` in `messaging.log` for the
transport's failures and slow calls.

### 21.2 Where it differs from the design

- **The setup is the cog, not a pairing link (§9.2).** The owner is the numeric Telegram user id typed
  into the cog (`@userinfobot` tells it), not a `/start <code>` pairing; H3 is the same (every other sender
  is dropped after reading only the sender). Levels and the drive PIN (§9.3) are not built: a paired owner
  can do everything the commands offer.
- **No backfill (§6.7).** A topic mirrors the session from the moment it is created; its first message says
  so and points at `/transcript` (the whole session as a `.md` file) and `/summary`.
- **One fixed mirror level, close to the Default preset (§6.3).** Prompts (⌨ typed at the PC, ⚙ sent by the
  runner; a prompt sent from the topic is the user's own message and is not mirrored), the work log with
  every tool call on one line (and a remark's first line, a failed result, a compaction), the final reply as
  Markdown; verbose items, files, thinking, hooks, the `/show` and `/level` grid are not built.
- **The activity draft (§6.2) is not built**; the card's state line (edited on every state change, at most
  every 20 s otherwise) and the work-log edits (every 15 s) show what the session does.
- **Questions and plans are shown, not answered (§7.4).** A question arrives with its options as a numbered
  list, a plan with its text, both as the turn's final message; answering is at the PC (the key plans of
  P0-1 are unmeasured). A permission prompt shows as the state edge's "Needs your approval at the PC".
- **`/interrupt` is one Esc through the registry's injector** (with a confirmation while running), not the
  link's abort (the session link is not built).
- **`/takedraft` queues a COPY** of the unsent draft (the box at the PC keeps it); the mail button's verified
  clear is a UI-thread action a later build routes through the hosting window.
- **The card's buttons** are five (Pause / Resume the runner, Queue, Pin / Unpin, Transcript, Screen); **More ▸**
  is the `/commands`.
- **The Search topic is created by the bot at its first connection with topics on** (the Bot API cannot pin
  it; the user pins it). Its results carry [Wake] [Wake 📌] for idle tabs and [Resume] [Resume 📌] [Fork] for
  closed sessions; a revived session is held in Waiting for you once it is live (its topic then appears),
  pinned when asked. `/newsession <folder> [model]` in Search opens a new session in a folder on this PC.
- **A topic the user starts in Telegram (D20) is not a new session**: the bot answers with the `/newsession`
  hint, and the topic can be deleted.
- **The pin has no PC-side menu item or tab glyph yet (D32)**; `/pin` and the buttons set it.
- **The run marker's crash note**: the tick loop rewrites `messaging\run.marker` every minute and `Stop`
  deletes it; the FIRST marker write of a run copies a marker still on disk to `run.marker.last`, which the
  Search topic's start note reads (and removes) — so a run that ended without a clean stop is named in the
  next start's note.
- **Teams, webhooks, external connectors (§11–§12): not built.**

### 21.3 Check after the build (live, with the owner's bot)

1. Cog → Messaging: token pasted, user id set, Check bot → `topics are ON`. ⚠ Threaded Mode lives ONLY in
   the BotFather **Mini App** (search botfather → tap Open → My bots → the bot → Bot Settings → **Threads
   Settings** → Threaded Mode); the classic `/mybots` chat menu does not show it. On 2026-10-16 the user
   flipped "Secretary Mode" and the group rights in the chat menu first — `getMe` then showed
   `can_connect_to_business: true` (the business-bot setting) while `has_topics_enabled` stayed `false`;
   that first bot (`@SidKatrineBot`) never got the mode on, and the owner made a NEW bot
   (`@ClaudeSessionsGateBot`, Threaded Mode on from the start — §21.4). Save (or replace the Credential
   Manager entry and restart: the token is read ONCE, at `_start`). hooks.log: `[msg] messaging started …`,
   `[msg] connected as @<bot> (topics enabled)`, `[msg] topic created: Search`.
2. In Telegram: the 🔎 Search topic with its welcome; one topic per non-idle Claude tab of every window
   (`[msg] topic created <sid8> …`), each with a pinned card; no topic for idle tabs, Codex tabs or shells.
3. Type a prompt in a topic: the ✍ reaction, the prompt queued (`[msg] <sid8> queued from topic`), delivered
   at the next ready moment even with the runner Paused, then 👨‍💻 when the turn starts; the turn's work log
   grows in place; the final reply arrives with sound; everything else silent.
4. `/queue`, `/sendnow`, `/runner paused`, `/pin` (the card's 📌; the session never goes Idle), `/transcript`
   (a `.md` file), `/summary`, `/screen`, `/rename x` (the tab, the topic and the saved title), `/status`.
5. A session that goes Idle: the Idle card with the countdown; after the grace the topic is deleted
   (`[msg] topic deleted <sid8> (idle)`); `/wake` from Search's `/idle` list brings a new topic.
6. Search: a request in plain words → the agent's sentence + rows with buttons; [Resume] opens a background
   tab in the most recently used window, and the topic appears once the session is live.
7. `/reload-plugins` is refused (no record); `/resume x` is refused (leaves the tab); `/cc /mycmd` passes.
8. A second Agentmaster instance (dev + release) with the SAME token: the second one reports the poller
   mutex and does not start (one bot per install).

### 21.4 The first live run (2026-10-16, dev) and what it changed

- **18:49:59** `[msg] messaging started (owner …, idle grace 10 min)` — the token of `@ClaudeSessionsGateBot`
  replaced the first bot's in Credential Manager and the dev GUI was restarted (the token is read once, at
  `_start`; `Apply` on a running service only re-reads the settings). **18:50:00** `connected as
  @ClaudeSessionsGateBot (topics enabled)`, `topic created: Search (thread 22371)` with the welcome, the start
  note and `setMyCommands` (22 commands in the chat scope, read back with `getMyCommands`).
- **18:50–18:52: 47 topics**, one per non-Idle Claude tab of the two dev windows — every one a restored,
  dormant tab whose persisted state was Waiting for you / Needs approval (D3 lists them: they wait for the
  user) — about 1.2 s apart (the outbox pacing: `createForumTopic`, the "Mirroring from here on" note, the
  card, `pinChatMessage` per topic), `cardMessageId` set on all 47 in `state.json`, and not one `[tg]` line in
  `messaging.log` (no 4xx, no 429). Items 3–7 of §21.3 (the phone's side) were not exercised in this run.
- **Fixed from that run** (the commit after `d1d23a5ca`, swapped into dev the same evening):
  - Every topic creation logged `[msg] dropped a message from a sender that is not the paired owner` (48
    lines): the bot's OWN `forum_topic_created` service message comes back through `getUpdates` with the bot
    as the sender. `_handleMessage` now returns silently for the bot's own id (`_botId`, from `getMe`) before
    the foreign-sender test; H3's order (the sender is read first, nothing else before it) is unchanged.
  - **The state file was keyed by nothing.** Replacing the token with ANOTHER bot's would have kept the old
    bot's update offset, Search topic and topic map. Update ids are per bot, so an offset above the new bot's
    ids confirms — drops — every one of its updates for good, and the thread ids name topics that exist only in
    the old bot's chat. `MessagingState::botId` (JSON `botId`; 0 in an older file = adopt the first bot seen,
    never a reset) is compared with `getMe`'s id at every start: a different bot resets the offset, the Search
    topic, the topic map, the per-session mirrors and the reactions (`[msg] the bot changed (@old -> @new): …`).
    The poll loop does not call `getUpdates` before that `getMe` has answered (the IDENTITY GATE on `_botId`,
    re-armed at every `_start`), so the reset always precedes the first poll. On this machine the switch from
    `@SidKatrineBot` happened with offset 0 and no topics, so nothing was lost — the trap was for the next time.
  - **A restart churned the chat** (seen at the swap that carried the two fixes above, dev 0.0.1.11, 19:04): the
    first tick after the start found every session `live=false` — the registry loads `sessions.json` as
    archived and each reopened window flips its own tabs live only in its first layout — so `PlanTopicSync`
    deleted all 47 topics as "closed" (19:04:28–29), and from 19:04:57 re-created 38 of them as the tabs came
    back (9 had meanwhile read Idle). The topics' history in the chat was gone with them. A first fix, a fixed
    120 s hold (dev 0.0.1.12, 19:10), kept the 47 at the start and still churned 16: that start's `[startup]`
    lines read `restore-sessions 72625ms` (the fleet load, while an m5 run competed for the CPU) and
    `restore-window-tabs 67765ms` (the re-home of 56 tabs, ~1.2 s each on the UI thread), so the last 16 tabs
    came live after the hold had ended — the restore of ONE window can outlast any fixed number.
    Now the hold follows the restore itself: `Engine::restoringWindowIds` holds every registered window until
    the end of its `_RestoreWindowTabs` (a `scope_exit`, early returns included; a window closed mid-restore
    leaves the set at `UnregisterLiveWindow`), the service reads the count through `ServiceHooks::windowsRestoring`,
    and a "closed" delete of a topic whose session has not been seen live in this run (`_seenLive`, filled from
    each tick's facts) is skipped while the start is younger than `kStartupCloseHoldMinMs` (120 s — the windows
    that have not registered yet: the Emperor's second window registers only once the first one's restore has
    released the shared UI thread) OR any window is still restoring, up to `kStartupCloseHoldCapMs` (30 min,
    so a restore that never ends cannot hold forever). A session seen live and then closed loses its topic at
    once; one still closed when the hold ends loses it then. Logged once per start: `[msg] start-up hold: N
    topic(s) kept until their sessions re-home (W window(s) still restoring; …)`, and when it ends `[msg]
    start-up hold ended S s after the start (every window restored | the cap ran out)`. Other delete reasons
    ("idle" past its own grace, "not a claude tab", "left its tab") are not held. The engine half is in the m5
    suite (`TestPersistence`: register → 1 restoring, done → 0, unregister clears); the service half is checked
    on every swap's log — no `topic deleted … (closed)` before the "hold ended" line. **Checked at the swap to
    dev 0.0.1.13 (19:28):** `start-up hold: 47 topic(s) kept … (1 window(s) still restoring …)` at 19:28:55,
    `start-up hold ended 121 s after the start (every window restored)` at 19:30:55.233 — 0.1 s after that
    window's `[rehome] … resumed=55` line (`restore-sessions 52 s` + `restore-window-tabs 69 s`) — and not one
    `topic deleted`.
  - **Telegram's 429 — and its real cause, the card.** At 19:24:34 an `editMessageText` answered `429` with
    `retry_after 1526` (`messaging.log`); the 19:28 run's first `sendMessage` got the remainder, `1265 s`
    (`[msg] pacing: 429 on sendMessage, waiting 1265 s` — the send loop waits it out, as Telegram requires: a
    call inside the window extends it), so nothing the bot sent landed before ~19:50. The topic churn above
    contributed (47 + 38 + 16 topics, four calls each), but the sustained load was the CARD: its state line
    read `Running 3m` and the idle line `leaves the list in 7m` — relative spans, re-rendered every tick and
    edited whenever the text changed, so every card changed every minute (every 20 s in its first minute),
    47 cards made ~1 edit/s for ten minutes, and the outbox paced edits at 300 ms (`kLightGapMs`) on top.
    Telegram's per-chat flood limit (~1 call/s, bursts tolerated, then a long penalty) counts edits. Fixed:
    the card shows CLOCK times (`since 19:04`, `leaves the list at 19:54` — `TgClockText`, local, with the
    date when not today; a card now changes only when a fact changes, plus once at midnight), every call
    keeps the ~1 s per-chat pace (`kLightGapMs` = `kSendGapMs`), and the work log's edit gap grows with the
    number of open turns (`_openTurns` × 2 s, 15 s at least), so N busy sessions never exceed ~0.5 edit/s
    between them. A traffic line every 10 minutes (`[msg] traffic (10 min): sendMessage=N editMessageText=M …
    429=K`) is where the chat's call rate can be read from now on. Expected steady state for a quiet fleet:
    a handful of calls per 10 minutes. **Checked at the swap to dev 0.0.1.14 (19:44):** the hold kept 47 and
    ended 121 s after the start (the window's restore took 98 s this time, so the 120 s minimum governed), no
    topic deleted; the first `sendMessage` still met the remainder of the 19:24 penalty (`waiting 346 s`), so
    the first clean traffic line is the one after ~19:50.
- **Open after the first live day:** (1) ~~a session at rest in `Error` is announced again at EVERY start~~ —
  fixed in §21.5 (`TopicRecord::errorAnnounced`). (2) The m5 scheduler timing checks ("the held prompt
  self-releases …", "woken tab: the queue resumes …") fail under CPU contention (a concurrent msbuild) and
  pass on a quiet machine — widen their 3 s wait or mark them load-sensitive. (3) Items 3–7 of §21.3 — the
  phone's side — are still unexercised.

### 21.5 The user's first look from the phone (2026-10-16 evening) and what it changed

The user's five points, verbatim in short: *"Secretary mode for the bot was enabled (allows sending messages
by user as if the user did)"* · *"The buttons and active status message are useful but should be sticky as a
last message … Add the recap if we have a fresh one in respect to its time of creation to other messages"* ·
*"A thread of every session should actually be the messages of user and assistant including the message of
why the bash is invoked"* · *"User should not care if a tab is activated or deactivated … 'Reactivate' …
If I send a prompt and session is deactivated then activate and then send the message"* · *"Prompts of user
should be also sent 'by' the user (via secretary mode)"*.

- **The STICKY CARD.** The card is the LAST message of its topic. `TopicRecord::lastPostedMs` is stamped by
  every post into the topic but the card's own (`_sendText`'s `movesCard`, at enqueue time) and by the user's
  own messages (`_handleTopicMessage`); `cardPostedMs` when the card was sent. Once newer posts have been
  quiet for `kCardStickyLagMs` (6 s — a reply and its remarks cost ONE re-post), `_refreshCard` deletes the
  old card and sends a fresh one at the bottom (`_postCard`: `deleteMessage` + `sendMessage`, both at the
  card's priority so the message that triggered them goes first; `cardMessageId` = -1 while in flight, 0 with
  `cardPostedMs` = now on a refusal, retried after `kCardRepostAfterFailureMs` = 60 s, never every tick). The
  card is NOT pinned any more (a pin per re-post would leave a "pinned a message" line each time; the old pins
  go when their cards are deleted). The topic's setup posts its note BEFORE the card. A stop mid-send used to
  leave -1 markers in the state file for good (the work log's too): `_start` clears them.
- **The RECAP in the card** (REPLACED in §21.6 by Claude Code's own recap: this block only repeated the reply
  right above the card). `TopicRecord::recapPrompt` / `recapReply` / `recapAtMs` (the transcript moment,
  shown as a clock time) / `recapSeenMs`: seeded once from the transcript's last 256 KB when the topic is
  created (`_seedRecap`: the last prompt, and the last final reply after it, else the last assistant text),
  then kept by the mirror (a prompt, a final). The card shows the block (`Recap · 19:42` / 👤 the prompt's
  first line / 🤖 the reply's head, 240 chars) only while `recapSeenMs > lastPostedMs` — "fresh with respect
  to the other messages": a topic that was just created mirrors from now on, so its first card says where the
  conversation stands; once the reply itself sits above the card, repeating it would be noise.
- **The thread is the conversation.** An assistant text that precedes a tool call ("why the bash is invoked")
  is posted as ITS OWN silent message (`_postRemark`), no longer a `›` line in the work log; the work log so
  far is closed with its final content, and the tools that follow start a new work-log message below the
  remark — so a turn reads remark → tools → remark → tools → reply. A create of the closed segment that was
  still in flight keeps its message (`Mirror::workLogGen`); the final reply still carries the sound; a
  question's leading remark is posted the same way. Slash commands keep their one `<code>` line.
- **Activated / deactivated is not the user's concern.** A message into a parked tab already started it
  (`_queueRemotePrompt` → the `start` action) and is delivered when it is ready; the card no longer says
  "parked" but `💤 not running — a message here or ▶ Reactivate resumes it`, and a session in Error reads
  `▶ Reactivate restarts it (the same conversation goes on)`. **▶ Reactivate** (the card's first button when
  the session is parked or in Error; `/reactivate`; `_reactivate`): a parked tab's claude is started in place
  (with `CLAUDE_CODE_RESUME_INTERRUPTED_TURN` a cut-off turn goes on by itself), a session in Error whose
  claude is up is restarted (the same conversation id, the same tab); a running one is left alone. A REMOTE
  prompt is now also delivered into a session in `Error` (`DecideAdvance`: the user's message from the phone
  IS the retry; the runner's own prompts still stop there). And an Error is announced ONCE per episode
  (`TopicRecord::errorAnnounced`, cleared when the session runs again): a session at rest in Error read Idle
  at every start until the scanner re-asserted Error, and the topic got the "Error" message each time.
- **Secretary mode — prompts as the user's own messages.** Telegram Business lets a bot connected under
  Settings → Business → Chatbots send ON BEHALF of the user (`sendMessage` with `business_connection_id`;
  the right is `BusinessBotRights.can_reply`). The connection arrives ONLY as a `business_connection` update,
  so the poll now asks for it (`allowed_updates`), `_handleBusinessConnection` keeps id / enabled / can_reply
  in the state file (H3: the connecting user must be the owner), and every (re)connection runs the
  **SECRETARY PROBE**: one send as the user into the 🔎 Search topic (`_sendAsUser`: `business_connection_id`,
  `chat_id` = the bot's own id — from the business account's side its chat with the bot is "the private chat
  with user <bot id>" — and the topic's `message_thread_id`). `businessMode` 1 = it works: a prompt typed at
  the PC is sent as the user's own bubble (the ⚙/⌨ line only on a refusal; three refusals in a row turn the
  mode off again); -1 = Telegram refused (`businessNote` holds why), prompts keep the ⌨ line, and the Search
  topic says so. ⚠ The docs scope business sends to "private chats that had incoming messages in the last 24
  hours" and call a business chat "independent from any potential bot chat" — the probe is how we learn
  whether the user's chat with the bot itself is allowed; the first connection the user made (while the bot did
  not listen for the update) is gone, so the entry must be toggled off and on once after this build.
- **Cost:** a re-post is two calls per quiet burst per topic; a remark is one message (a turn with ten tool
  calls and four remarks: four messages + up to five work-log messages + the reply + one re-post). The pace
  (one call per ~1 s, §21.4) holds; the `[msg] traffic (10 min)` line is where to watch it.
- **Tests:** the state round trip of the new fields, the card's recap shown / hidden and the two
  Reactivate lines, `TgStripTags`, `/reactivate` as an Agentmaster command (`TestMessaging`).
- **Live checks owed:** the sticky card after a reply; the recap on a fresh topic; a remark as a message; a
  prompt into a parked tab; ▶ Reactivate on a session in Error; the probe's verdict in the Search topic after
  the user re-toggles the Chatbots entry.

### 21.6 The user's manual QA on dev (2026-10-16 night) and what it changed

The user QA'd session `ba1ee743` on dev 0.0.1.15 and restated four points of §21.5 (the sticky card, a FRESH
recap, "the messages of user and assistant including the message of why the bash is invoked … unless I use a
/slash command", Reactivate, prompts sent "by" the user), and added one: *"A new session/tab with no new
messages should appear on telegram. If we show per tab, we will cleanly also support forked sessions."* What was
found and what changed (dev 0.0.1.16, swapped 21:57:54):

- **A new tab is listed at once, before its first message.** `QualifiesForTopic(f, now, grace)`: an Idle tab
  qualifies when it was used within the idle grace — `TabFact::lastActivityMs` is `SessionInfo::lastActivityUnixMs`,
  which the launch seam stamps with a NEW session's or a fork's creation instant (and every hook moves on), and
  every launched tab carries its `tabToken` from the launch (stamped eagerly), a background one too. Before, a
  new tab (Idle until its first prompt) got its topic only at that prompt — and the topic, mirroring from the
  transcript's END, lost the prompt itself (seen live). Window-restored tabs keep their old last activity, so an
  Idle one still gets no topic (no churn at a start). An Idle topic leaves one grace after the LATER of "first
  seen Idle" and the last activity (`IdleTopicLeavesAtMs` — the plan and the card's "leaves the list at" share
  it, so the clock only moves when the session is used).
- **Forks.** A fork is a new tab, so it gets its own topic at once. Its transcript appears with its first message
  and begins with the source's history copied line by line, each stamped `forkedFrom` (measured: 958 copied lines,
  then the fork's own) — `ClassifyTranscriptLine` now skips those lines, so the fork's topic shows only its own
  conversation. Its first card's recap is read from the source's transcript (`ResolveClaudeDisplayTranscript`).
- **A topic follows its TAB (Rekey).** `TopicRecord::tabToken`, kept current by every sync (a restart or a park
  gives a tab a new token). When a topic's session CLOSES and another live Claude session without a topic holds
  the same tab (a /clear, a /resume in the tab), or continues the closed one (`SessionInfo::continuedFromId` — a
  /handover-here successor, which takes the tab on a new connection), `PlanTopicSync` emits a `Rekey` instead of a
  Delete; `_rekeyTopic` moves the record to the new key (a fresh mirror, seeded from the new transcript; the card
  and the thread stay), posts one silent line `— a new conversation in this tab (<id8>) —`, renames the topic and
  refreshes the card (`[msg] topic carried over <old8> -> <new8> (the same tab …)`). While a topic's session still
  holds the tab, a second session in that tab gets no topic of its own. A topic with no recorded tab (written
  before this build) is never carried over.
- **Where a new topic's mirror starts** (`_seedFromTranscript`): no transcript yet (a new session, a fork) → its
  first line; at rest → the file's end (the topic shows the session from now on); mid-turn (Running /
  NeedsApproval) → the start of the turn's own prompt line within the last 256 KB, so the prompt that made the tab
  qualify is in the thread. The "Mirroring from here on" note is gone (the thread is the conversation). A transcript
  that cannot be read for a moment keeps its cursor (it was reset to 0, which would have replayed the whole
  conversation once the file was found again); an unresolved transcript is looked up every 2 s while the session
  is busy, every 20 s at rest.
- **The recap is Claude Code's own.** The §21.5 block (last prompt + last reply) only repeated the reply sitting
  right above the card. The card now shows Claude Code's idle recap — the `system/away_summary` line it writes a
  few minutes after a turn when the user is away (`TopicItemKind::Recap`, `NormalizeRecapText`) — and only while
  it is FRESH, newer than every conversation message (`RecapIsFresh`; `IsConversationItem` = a prompt, a reply,
  a tool call, a question, a plan, an API error, a command, an interrupt). `TopicRecord::recapText / recapAtMs /
  lastMessageAtMs`, folded by the one `FoldRecapFacts` for the seed (the transcript's tail when the topic is made)
  and the mirror (every batch). A 0.0.1.15 topic's `recapPrompt` / `recapReply` keys are no longer read.
- **Why the Bash runs.** `ToolCallSummary` puts a Bash call's `description` (Claude Code writes one per call)
  first and the command after it, cut to fit: `Bash: Run the unit tests ($ npm test -- --watch=false)`.
- **The thread is the conversation; a state rings the CARD.** The bot's own "⏹ claude exited", "⚠ Error …" and
  "🟠 Needs your approval at the PC" messages are gone: the card says the state, and for those three it is re-sent
  at the bottom WITH SOUND (a PING: `_refreshCard(…, ping)` → `_postCard(…, ping)`, kept until a post can carry it;
  `[msg] <id8> card re-posted with sound (<state>)`); what the assistant had said before an exit or an Error closes
  the turn silently. ONE sound per "needs you": a question or a plan posted with sound makes the NeedsApproval
  edge that follows silent (`Mirror::awaitingAnswer`), and a NeedsApproval ping makes the question posted after it
  silent (`approvalPinged`); an API error message rings unless the Error edge already rang (`errorAnnounced` both
  ways). An interrupt posts what was said + `■ interrupted`, silently, and nothing for a stray marker.
- **A card is never edited for nothing.** `TopicRecord::cardTextHash` (FNV-1a 32 of the card's text + buttons,
  persisted): a text the topic already shows is not sent again — the 20:30 start had re-edited every card (117
  edits in ten minutes). The card's "since" now reads persisted stamps (`CardStateSinceMs`: the turn's start while
  it runs, the newer turn boundary at rest, then the last activity — `stateChangedAtUnixMs` is stamped at every
  load, so every start changed every card). This build's first start edits each card once more (the hash is new
  and the text changed). A card the user deleted on the phone comes back (the edit's "message to edit not found"
  clears the record's card; the next refresh posts one).
- **Reactivate** also restarts a session whose claude EXITED (Done; the card reads `⏹ claude exited — ▶ Reactivate
  starts it again (the same conversation)`). The button answers with a toast — nothing is posted in the thread;
  `/reactivate` answers in the thread.
- **A message from the phone settles 5 s, not 40 s** (`kRemotePromptSettleMs`, applied inside
  `StateSettleRemainingMs`, so the overlay's "settling" cue follows). The STATE SETTLE hold protects the runner's
  PLANNED prompts from a momentary "ready"; for the user's own message it only made the phone wait — 40 s after a
  parked tab's claude started, or 40 s after being queued into a session that sat waiting anyway. The input-box
  gate (NoBox while claude draws its UI) and the ready-state gate still hold the rest.
- **Secretary echo guard.** A message that carries `sender_business_bot` or `business_connection_id` is dropped
  before anything reads it, and a text this service sent AS THE USER into a topic within the last two minutes is
  consumed once if it comes back as the owner's message (`_takeSecretaryEcho`; `[msg] secretary: our own send as
  the user came back as a message …`). Without it a prompt typed at the PC, mirrored as the user's own bubble,
  could return as a new prompt from the phone — sent to claude again, mirrored again: a loop. A bot change also
  resets the business connection (it is made to one bot).
- **Two races of the tick fixed.** The tick works on a COPY of a session's mirror and topic and writes them back:
  (1) a work-log create whose answer landed in between lost its message id — or, refused, left the record "in
  flight" (-1) for the rest of the turn, so the work log stopped; `_flushWorkLog` now marks the shared record -1
  when it sends, and the write-back keeps an answer that landed; (2) the card's transient state (`CardState`: the
  last edit, the last state, a wanted ping) moved out of the Mirror, whose write-back overwrote it (a card could be
  edited twice). The card post's answer is matched to its topic by THREAD, so a Rekey in flight never loses it.
- **The live start** (dev 0.0.1.16, 21:57:54, downtime 6.5 s): 48 topics held by the start-up hold, one window
  re-homed its 56 Claude tabs in 74 s, the hold ended at 124 s with every window restored, nothing was deleted.
- **Tests** (`TestMessaging`): a new tab qualifies / a restored one does not; `IdleTopicLeavesAtMs`; a new tab is
  created at once; the grace counted from the last use; Rekey by tab (case-insensitive) and by continuation; no
  heir in another tab; no Rekey without a recorded tab; no second topic in a held tab; the fork's copied lines
  skipped; the `away_summary` Recap item; the Bash description; `FoldRecapFacts` / `RecapIsFresh` /
  `IsConversationItem`; the state round trip of the new fields and a 0.0.1.15 file; the fresh / stale / empty recap
  block; the exited line; `CardStateSinceMs`; `CardTextHash`; `SessionIsReactivatable`; the 5 s remote settle
  (`DecideAdvance`) beside the runner's 40 s. m5: 7068 checks, the 3 known TestTaskbarBadge failures only.
- **Live checks owed (the user's side):** open a new tab and a fork → each gets its topic at once, the fork's
  showing only its own messages; a /clear in a tab → the same topic, one "a new conversation" line; a Bash call's
  line shows its reason; a fresh `Recap` block after a few idle minutes, gone at the next message; a session that
  needs approval / exits / errs → the card comes back at the bottom with sound, and no bot line; a message into a
  parked tab → delivered ~5 s after its claude is up; ▶ Reactivate → a toast; secretary mode → see §21.5 (the
  Chatbots entry must be toggled once after a build that listens; `[msg] secretary:` lines say what Telegram said).
