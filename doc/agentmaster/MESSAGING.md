# Agentmaster — messaging: the fleet in Telegram, Microsoft Teams, and any other messenger

> **Status: Telegram v1 IMPLEMENTED on 2026-10-16 (§21 says exactly what was built, where it differs from the
> design below, and what is still open; §21.6 = the topics per TAB, Claude Code's own recap, the card that
> rings — dev 0.0.1.16, not production; §21.11 = Claude's Markdown as Telegram RICH messages (tables render), a
> VERBOSE or conversation-only mode per topic with a default for all, the state glyph in every topic name, and an
> optional second SECRETARY bot that posts the user's turns as the user — dev 0.0.1.21, 2026-10-17; §21.12 = the
> "👤 You" line, the reply that quotes its prompt, the status on the last prompt — dev 0.0.1.22, and LIVE in PRODUCTION
> 0.6.8.21 since 2026-10-17 12:01 with 413 topics (§21.12.5); §22 = folder groups — the topics grouped by working
> folder, a discovery, DESIGN ONLY); designed 2026-10-15, revised twice on 2026-10-16.** One bot per install since 2026-10-17: the DEV
> install polls `@ClaudeSessionsDebugGateBot`, the RELEASE install `@ClaudeSessionsGateBot` (the dev bot from 2026-10-16
> 18:50 until then); each token in `K:\source\telegram\<bot>.env`, stored in Windows Credential Manager per profile by the
> cog — never in a profile file; the first bot, `@SidKatrineBot`, never got Threaded Mode on. Teams, webhooks and external connectors
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
| Buttons | `callback_data` 1–64 bytes; `style` danger (red) / success (green) / primary (blue) — no color of their own, the app picks the shades; copy-text buttons (1–256 characters) | Tokens (§7.5), copy actions (§7.3), the title chips (§21.15) |
| Reactions | `setMessageReaction`: "as non-premium users, bots can set up to one reaction per message"; allowed emoji include ✍ 👀 👨‍💻 👍 👎 | The status of a queued message (§7.1) |
| Text | 1–4,096 characters; HTML with `pre`/`code`, `blockquote expandable`, `tg-time` | §10.4 |
| Rich messages | 10.1+: `sendRichMessage` with tables, details, lists; 10.3: buttons INSIDE the text (`<tg-button>`, a look each); `editMessageText` turns a classic message into a rich one and back | Claude's Markdown (§21.11); the title chips (§21.15) |
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
  - **Re-checked 2026-10-17** (the user asked whether the bot can pin it by default, and check once on an
    existing chat that it is pinned), against the live reference and its changelog through Bot API 10.3 of
    2026-08-24. No method pins a topic: the topic methods are create / edit / close / reopen / delete /
    `unpinAllForumTopicMessages` and the General topic's. `ForumTopic` has no pinned field, no method reads a
    topic back, and pinning a topic sends the bot no service message. So the bot can neither pin Search nor
    learn whether it is pinned, and there is nothing to migrate.
  - **A pin the user sets is kept.** The bot never deletes or recreates the Search topic: `/recreate` and a
    `kTopicLayoutVersion` recreation queue only session topics (`orphanThreads`), and only a token for a
    DIFFERENT bot resets `searchThreadId` (that bot's chat is another chat). Production's Search topic is
    thread 22371 (made 2026-10-16 18:50, when @ClaudeSessionsGateBot was still the dev bot), dev's is 25027
    (made 2026-10-17 11:24 under @ClaudeSessionsDebugGateBot).
  - ⚠ **A Search topic the user deletes does not come back** (read from the code, not tried): nothing reads
    a refused send's "message thread not found" (`TgResult::ThreadNotFound` is read only by the rich-fallback
    rule and the two topic-delete paths), and `_ensureSearchTopic` makes the topic only while
    `searchThreadId` is 0. Its sends then keep failing. The way back today: stop the app, set
    `searchThreadId` to 0 in `<profile>\messaging\telegram\state.json`, start it.
  - **A link that opens Search: the right number exists, and only Telegram Desktop's code opens it** (asked
    2026-10-17, "preferably in the 'all' area"; read from the docs and the three clients' sources, nothing
    built, nothing tried on a device). ⚠ This bullet first said (commit 01ea104cf) that the bot's number for
    Search is not the user's. That is wrong for TOPICS. A private chat has two message-id spaces, one per
    account (`api/updates`), but a topic's canonical id is the USER-space id of its creation message, and the
    HTTP Bot API's `message_thread_id` already IS that id. Tested by mtcute (issue #153, layer 223, re-checked
    2026-10-08 on layer 229): `getForumTopicsByID` finds a bot-made topic only by its user-space id, and the
    Bot API's `createForumTopic` returns that id. So 22371 / 25027 are the numbers a link needs. Telegram's
    deep-link page documents topic links only for groups and channels. The clients' current sources:
    - **Telegram Desktop** (`dev`, 2026-10-15): `local_url_handlers.cpp` turns `https://t.me/<bot>?topic=<id>`
      (or `t.me/<bot>/<id>/<id>`) into `tg://resolve?domain=<bot>&topic=<id>`. `showPeerByLinkResolved` hands
      the topic to `showRepliesForMessage` for ANY peer, which opens it when `forumTopicFor(id)` is loaded
      (else, for a non-channel peer, it does nothing).
    - **Telegram Android** (13.0.2, 2026-10-13): `LaunchActivity` reads `topic` / `thread`, but for a user or
      bot it opens the chat (at `post`, if given) and ignores the topic. Its "All" view has two native jumps
      instead. Under the chat's LAST message, when that message belongs to a topic, it adds a "Continue last
      thread ›" button (`ChatMessageCell`: `isAllChats && isBotForum`, `addContinueThreadKeyboard`;
      `ChatActivity` → `topicsTabs.selectTopic`). While scrolling, a floating topic header shows, and tapping it
      selects that topic.
    - **Telegram iOS** (2026-10-07): `UrlHandling.swift` reads `topic` / `thread`, but for a non-channel peer
      the link ends as a plain message link into the chat.
    - A Search message in "All" never gets the tappable topic label of a group's "View as Messages": on
      Android that label needs `isForum`, which is false for a bot chat.
  - **What can be built from that (nothing built yet, nothing tried on a device):**
    - A **URL button** `https://t.me/<bot>?topic=<searchThreadId>` on a message in "All" (or anywhere). Telegram
      Desktop's code opens Search from it; Android and iOS open the bot chat only. BUILT since §21.16: the pinned
      directory's chips are such buttons, in the form `https://t.me/<bot>/<id>?topic=<id>`. ⚠ Read again for it:
      Android does not even open the chat — for the bot chat that is open already it only calls `setBotUser`.
    - A **`/search`** command, or a card button, that makes the bot post one line IN Search. It does three
      things. That line becomes the chat's last message, so Android's "All" view puts "Continue last thread ›"
      under it, one tap into Search. This lasts only while it stays the chat's newest message: the next post in
      any topic takes the button over, which on a busy chat is seconds. Telegram lists topics by their newest
      message (§21.8), so Search also moves
      to the top of the list, under the user's own pins. And tapping the line's notification, if it is not
      silent, should open Search on every client.
    - Both together: the post in Search carries the URL button, which covers Desktop and Android.
  - **How "All" takes a typed message:** both bots
    report `allows_users_to_create_topics: true` (getMe, 2026-10-17), so in the "All" view every message the
    user types becomes a NEW topic ("Type any message to create a new thread", `api/forum` → Bot forums). A
    `/search` typed there arrives in its own fresh topic, which the bot can delete as it answers. BotFather's
    "Disallow users to create new threads" makes "All" (the General topic) take messages directly, but it also
    stops the user from creating, editing and deleting topics (`BOT_FORUM_CREATE_FORBIDDEN`); whether it also
    stops pinning one is not documented.
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
  its tools (`search`, `grep`, `read`, `show` since 2026-10-17; the session database's tools and `read`
  when that is On) and system prompt — at
  **high** thinking (the harness's live thinking level, set in the cog and applied to the running
  conversation). Its own conversation (`harness\telegram-search.json`), so a search from the phone does not
  move the PC page's list (D24, decided). Parked after 10 idle minutes like the page's.
- **One conversation for the whole app, which the user can only clear** (the user's rules, D24 and D33,
  2026-10-16): like the page's `sessions-search`, `telegram-search` is ONE conversation per process, made
  on first use and kept for the process; a stop and a start of the bot reattach to it, and its record makes
  it carry on after an app restart. It is **not a session** (no transcript, `--no-session-persistence`): it
  is never a search result, and nothing wakes, resumes, forks or pins it — a session's `/commands` sent in
  🔎 Search are refused with that explanation. **Clearing it** is `/clear` or `/reset` (in this topic only:
  in a session's topic they stay Claude Code's own commands for that session) or the **⊘ Clear** button
  under every answer — the page's crossed circle.
- **A message in 🔎 Search is a request** ("the session where I fixed the tab drag crash"); follow-ups
  refine it, as on the page. The agent READS every session before it shows it, and can grep the
  conversations' full text (the user's choice of 2026-10-17; `SESSIONS.md` §9.12). Its answer arrives as one
  message (the final message: it makes a sound) in the user's **evidence-card** format ("C1", §21.10): the
  agent's one VERDICT sentence ("Two sessions are about releasing Agentmaster."), then one **card** per
  session — its number, a glyph for where it stands (📦 closed · 💤 idle tab · 💬 has a topic) and its title;
  a line with its folder, the day it was last active and how many prompts it had; and **one quote**, the
  evidence the agent saw when it read it (its `why`: "You asked: …", or Claude's words, or what the session
  shows):

  ```
  Two sessions are about releasing Agentmaster.

  1 📦 Release v0.7.9
  Agentmaster · 11 Oct · 14 prompts
  ┃ You asked: "cut v0.7.9 as a pre-release and publish the draft"
  ```

  Its buttons **pick a session** (D34): one number per card (at most 10; a longer list says how many more),
  then ⊘ Clear. A pick opens a second message, the session's **action menu**: the card in full — the whole
  folder and the branch, when it ran ("11 Oct 14:02 → 16:40") and its prompts, the same quote, where it
  stands now in words — and what to do:

| Button | Closed (on disk only) | In a tab but Idle | Listed (it has a topic) |
|---|---|---|---|
| [▶ Resume] | opened into a tab without taking focus (the window: D27), held in Waiting for you so its topic appears; the tab stays dormant until its first queued message starts it | woken (the tab's **Move to Waiting-for-you**); its topic appears | nothing to open: says so |
| [▶ Resume 📌] | the same, then pinned (never idle) | the same, pinned | pinned |
| [⑂ Fork] | a fork in a new tab (and so a new topic) — never pinned: **no fork-and-pin** (the user's rule) | the same | the same |
| [✕ Cancel] | the menu goes | the menu goes | the menu goes |

  The state is read again at the tap, so a session that changed since the answer does what it does now.
  A pick is stateless (it opens a fresh menu, also after a restart); the menu's actions are single-use and
  expire (M5).
- **Fleet commands in 🔎 Search:** `/idle` (the idle tabs, each with [Wake]), `/board` (counts per
  state), `/newsession <folder> [model] [-- first prompt]` (a new tab + topic), `/pauseall`,
  `/resumeall`, `/status`, `/clear`, `/help`.
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

> **Decided 2026-10-16 by the user: the whole session as messages** (the alternative below), packed so it fits the
> budget, the user's turns sent as the user where secretary mode works. Built: §21.9.

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
| **D24** (rest) | 🔎 Search's conversation is its own (`telegram-search`), ONE for the whole process like the page's `sessions-search`, and it can be cleared like the page's | "the telegram-search should be a single session shared by all windows that can be cleared like sessions-search can." |
| **D33** | The Search conversation is not a session: it is never woken, resumed or forked; it carries on after a restart by itself; the user's one action on it is to clear it | "You can't fork/resume/wake a telegram-search. it does persist throughout restarts by resuming the session but user can only really clear the session." |
| **D34** | A Search answer's buttons pick a session; a second menu then asks Resume or Fork; there is no fork-and-pin | "The buttons of a search result should be the session picking and then a second menu asks if to Resume, Fork (no fork and pin)" |
| **D35** | The Search answer is EVIDENCE CARDS (variant C1: a verdict sentence, then per session its title, a facts line and ONE quote of what the agent saw); the agent reads every candidate before it shows it and can grep the transcripts; on Telegram and on the Sessions page (§21.10, `SESSIONS.md` §9.12) | 2026-10-17: the choices "C · Evidence cards" ("This is the lead, produce 3 variants"), then "C1 · One quote per card"; "Read every candidate but also be able to full-text/grep the transcripts"; "Telegram and Sessions page" |

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
| **D25** | The idle grace before a topic is deleted | 10 minutes, with [📌 Pin] and [Wake] on the Idle card | 0 (at once); 1 hour |
| **D26** | Backfill of a new topic for a session with history | The whole transcript as a file + the last 10 turns as messages | Every turn as messages, paced — **the user's decision, 2026-10-16** (packed; §21.9) |
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
| The Search agent | `MessagingSearchBridge.{h,cpp}` (TerminalApp-only) | The process's ONE `SessionSearchAgent` named `telegram-search` (its own conversation, `<profile>\harness\telegram-search*.json`, made on first use and kept for the process like the page's `SharedSessionSearchAgent()`), high thinking by default; each start of the bot attaches a bridge to it behind the service's `SearchBridge` seam (§21.7). |
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
  it; the user pins it). Its answers carry one pick button per listed session and ⊘ Clear; a pick opens the
  session's action menu — [▶ Resume] [▶ Resume 📌] [⑂ Fork] [✕ Cancel], no fork-and-pin (§5.4, §21.7); a
  revived session is held in Waiting for you once it is live (its topic then appears), pinned when asked.
  `/newsession <folder> [model]` in Search opens a new session in a folder on this PC. The design's
  [→ Topic] and [Details] buttons are not built (a listed session's menu says it has its topic).
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
6. Search: a request in plain words → the agent's sentence + numbered rows, a number button per row and
   ⊘ Clear; a number opens the session's menu as a reply; [▶ Resume] on a closed session opens a background
   tab in the most recently used window, and the topic appears once the session is live (the checks of
   §21.7 cover the rest).
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

### 21.7 The Search topic's conversation and buttons (2026-10-16 night)

The user's directions, after a review that compared the Search topic with the Sessions page's AI search:
*"the telegram-search should be a single session shared by all windows that can be cleared like sessions-search
can"* · *"You can't fork/resume/wake a telegram-search. it does persist throughout restarts by resuming the
session but user can only really clear the session"* · *"The buttons of a search result should be the session
picking and then a second menu asks if to Resume, Fork (no fork and pin)"* (D24 rest, D33, D34 in §18).

- **One conversation for the process** (`MessagingSearchBridge.cpp`): the `telegram-search` `SessionSearchAgent`
  is a function-local static made on first use and leaked like the page's `SharedSessionSearchAgent()`. A
  bridge only attaches the service's sink (`AddListener`, a weak capture of the bridge's state, since
  `RemoveListener` does not wait for a call already running) and detaches it when destroyed. Before, each
  start of the bot built and shut down its own agent object (the record made it look like one conversation).
- **The cog's thinking level reaches it live**: `SearchBridge::SetThinkingLevel` → `SessionSearchAgent::
  SetThinkingLevel`, called by `Apply` on every Save and by each new bridge. Before, the level was read only
  when the bridge was made (the first search after a start of the bot), and parking kept that bridge, so a
  Save did nothing until messaging was switched off and on — against the cog tooltip's "the next Search process".
- **Clearing** (`ClassifySearchTopicMessage` → `MessagingService::_clearSearch`): `/clear` and `/reset` in the
  Search topic, and the **⊘ Clear** button under every answer (stateless `s:clear`). It makes the bridge when
  this start of the bot has none, so a record left by an earlier run is cleared too, then calls `Reset` (the
  context, the record and the view file go; a running request is interrupted) and posts one silent line.
  Backlog rule: a `/clear` older than 10 minutes is answered, not carried out. In a session's topic `/clear`
  and `/reset` stay Claude Code's own commands for that session.
  - **Before, the Search conversation could not be cleared from Telegram at all**: `/reset` is not one of
    `kAgentmasterCommands`, so `ParseRemoteCommand` read it as a Claude Code command and the topic answered
    "Claude Code commands go in a session's topic" — while `/help` advertised it. The unreachable branch behind
    it would also have frozen the bot: it replied while holding `_mtx`, which `_enqueue` takes again
    (`std::mutex` is not recursive). `_askSearch`'s "Search is not available" reply had the same self-deadlock
    (only in a build with no factory); `_ensureSearchBridge` now replies after releasing the lock.
- **Not a session** (`SearchTopicVerb::SessionCommand`): a session's command in the Search topic (`/wake`,
  `/pin`, `/forksession`, `/reactivate`, `/transcript`, `/cc`, …) is refused with "This conversation is not a
  session: it cannot be woken, resumed, forked or pinned — it carries on by itself after a restart, and /clear
  starts it over". The commands the topic runs are `kSearchTopicFleetCommands`.
- **Pick, then the menu** (`MessagingService::_postSearchResults`, `_showSearchPick`, `_onSearchPickAction`):
  an answer lists at most 10 rows of THIS request (an answer whose request showed nothing lists nothing; before,
  it re-posted the previous request's rows) with one number button each, five to a row, and "N more not
  listed" past ten. A number is the stateless callback `p:<n>:<session id>` (`SearchPickCallbackData`, ≤ 41
  bytes; `ParseSearchPickCallbackData` checks the prefix, a 1–2 digit number and an id of 8–40 hex digits and
  dashes). It opens the menu as a silent reply to the answer: the title, folder and last activity as the answer
  listed them (`_searchRowInfo`, the last 200 rows of this run; else the registry's record, else the short id),
  the line for where the session stands now (`ClassifySearchPickState`, `SearchPickStateLine`), and four
  single-use tokens of 2 hours (`pick-resume`, `pick-resume-pin`, `pick-fork`, `pick-cancel`; the old
  `resume` / `resume-pin` / `fork` tokens are gone, `wake` stays for `/idle`). An action re-reads the state:
  Resume on a closed session launches a background resume (`_requestLaunch`), on an idle tab wakes it, on a
  listed session says it is open (📌 pins it); Fork forks, never pinned; Cancel deletes the menu. The menu loses
  its buttons and keeps the choice or the outcome. The launch gets an empty title when nothing knew one, so it
  derives one from the folder, not the short id.
- **Not changed (open from the review):** the agent's system prompt still says "Sessions browser" and "on the
  screen", so the model writes "…are on the screen…" in Telegram; the Search topic does not prewarm the index
  before a request (the first live use on 2026-10-16 21:21 searched 3, then 13–17 sessions, while ~70 stale
  sidecar files were re-parsed); `RowsFrom` keeps its own title / folder / time rules; the bridge reads
  `SharedEngine().claudeExePath` on the agent's thread; the `[ai-search]` log lines do not name the agent.
- **Tests:** `tests_messaging_search.cpp` (`TestMessagingSearch`): the verbs (`/clear`, `/reset`, a bot suffix,
  `/clearance`, the fleet commands, every refused session verb, `/cc /clear`, Claude Code's commands), the pick
  data round trip and every malformed form, the keyboard layout, the pick states and their lines. The bridge's
  lifetime is app-only (the SDK) and checked by review. m5 on an isolated copy: 7147 checks, the 3 known
  TestTaskbarBadge failures only; `cl /Zs` with the app's recorded lines (warnings as errors) of
  `MessagingSearchBridge.cpp`, `Messaging.cpp`, `Messaging.Inbound.cpp`, `Engine.cpp`,
  `TerminalPage.AgentEngine.cpp` and `AgentManagerContent.Settings.cpp`, each plain unit with a negative control.
  NOT BUILT into the app.
- **Live checks owed:** two searches, then a dev restart, then a follow-up → it continues the conversation;
  `/clear` (and ⊘ Clear) → the cleared line, and the next request starts fresh (`[ai-search] reset` in
  hooks.log); `/wake` in Search → the refusal; tap a number → the menu replies to the answer, tap it again → a
  second menu; ▶ Resume on a closed session → the background tab and its topic; on an idle tab → woken; on a
  listed one → "open already"; ⑂ Fork → a new tab, not pinned; ✕ Cancel → the menu is gone; an old answer's
  number after a restart still opens a menu; a cog change of the Search thinking level → `[ai-search] thinking ->
  <level>` at once.

### 21.8 The topic list in MOST RECENT order, and the recreation (2026-10-16 night)

The user, after §21.6: *"What of the existing threads on telegram? don't we need to recreate them all?"* and *"can
you ensure that the threads are on the list are ordered by MOST RECENT like the sidemenu does, naturally and in the
recreation"*.

- **How Telegram orders topics.** A chat's topics are listed by their LAST message, newest on top (a topic the user
  pins stays above them). The Bot API can neither reorder topics nor date a message, so the order is made by WHEN
  the service posts into each topic.
- **Why the list was out of order.** The first topics (dev, 2026-10-16 18:50) were created in session-id order — the
  plan's map order — so the list started random, and only later activity moved single topics up.
- **One key.** `MostRecentSortKeyMs` (`SessionModels.h`) — the newer turn boundary (`turnStartUnixMs` /
  `turnEndUnixMs`), else the last activity (a new session's or fork's creation) — is the key of the side panel, the
  board and the tree (`MakeSortKey` now calls it) and of the topic list; ties go by the title, case-insensitive
  (`TopicRanksAbove`, upper-casing like the side panel's `CompareStringOrdinal`).
- **Creation order.** `PlanTopicSync` lists the creates in the REVERSE of the list's order (lowest-ranked first,
  `TabFact::mostRecentMs`); the service makes ~6 per tick in that order, so the topic that ranks highest is made —
  and its card posted — last, and lands on top. A single new tab is the most recent anyway: on top.
- **Posting order.** The tick works through the topics lowest-ranked first (`_orderKeys`, refreshed by every sync),
  so the posts one pass queues (replies, card re-posts) leave the highest-ranked topic on top.
- **The direct topic calls are paced.** `createForumTopic`, `deleteForumTopic` and `editForumTopic` (made on the tick
  thread) now take the outbox's per-chat slot (`_awaitCallSlot`, counted in the traffic line); they ran unpaced, and a
  recreation makes ~150 calls (§21.4's 429 cost 21 minutes).
- **The RECREATION.** `kTopicLayoutVersion` = 1 and `MessagingState::topicLayout` (persisted; absent = 0). At the
  first start of this build, the topics made under layout 0 are made again ONCE, after the start-up hold (every
  window has re-homed its tabs, so every tab that should have a topic is live): `_recreateTopics` moves every session
  topic's thread into `MessagingState::orphanThreads` (persisted — a stop midway finishes the deletes at the next
  start), clears the topic map, the mirrors, the cards and the queued work for the old topics, and posts a silent note
  in the 🔎 Search topic. The sync then deletes the orphans (paced, ~6 per tick) BEFORE it makes any topic, and builds
  the list from nothing, oldest first. The Search topic stays. What goes: each old topic's messages in Telegram (the
  conversations stay in their sessions); a new topic shows its session from now on (§21.6's seed: the end of the
  transcript, or a running turn's own prompt), with Claude Code's recap while it is fresh. Logs: `[msg] recreation
  (…)`, `[msg] recreation: the old topics are deleted …`, one `[msg] topic created … most recent HH:MM` per topic in
  creation order, `[msg] recreation done: N topic(s) …`; `[nav] remote telegram recreate-topics old=N`.
- **`/recreate`** (the 🔎 Search topic or any session's topic; it asks first, a single-use button) runs the same
  recreation later. The chat's "/" menu is now sent once per start, so a command a build adds reaches an existing chat.
- **What keeps the order "naturally", and where it cannot.** A session moves in MOST RECENT order only at a turn's
  start (the prompt) and its end (the reply), and those are exactly the posts that move its topic up. Two things move
  a topic that the side panel's MOST RECENT does not: (1) the assistant's remarks and work-log messages DURING a turn
  (§21.5 made remarks messages of their own, and every message is its topic's newest), so a running session's topic
  rises while it works and is back in line at the turn's end; (2) the card re-sent with sound for an exit, an Error or
  an approval (a notification needs a new message). Exact parity during a turn would mean posting nothing new
  mid-turn — remarks folded into one edited message per turn, or held until the turn ends — which is the user's
  choice, not made here.
- **Tests** (`TestMessaging`): `MostRecentSortKeyMs` (boundaries, the activity fallback); `TopicRanksAbove` (keys, the
  case-insensitive title tie, the upper-casing); the creates' order (`adecb` — ties included); `NeedsTopicRecreation`;
  the round trip of `topicLayout` + `orphanThreads` and a file without them; `/recreate` a Search fleet command, never a
  prompt. m5, isolated on HEAD 4edb9f377 + this change: 7163 checks, the 3 known TestTaskbarBadge failures only.
- **The live run** (dev 0.0.1.17, swapped 22:46:38, downtime 7.7 s): the start-up hold ended at 122 s with the window's
  56 Claude tabs re-homed; the recreation deleted the 48 old topics in 57 s (22:48:40 → 22:49:37), then made 48 new
  ones in 70 s, every `most recent` stamp in non-decreasing order (the oldest, 18 Sep 11:25, first; ba1ee743 — the
  session the user was testing, 22:03 — last, so on top); no 429, no exception. Four session titles hold a line
  break (their `topic created` log lines split there; what Telegram made of such a topic name is not checked). Owed:
  the user's look at the list on the phone.

### 21.9 Every topic carries its session's whole history (2026-10-16 night)

The user, after §21.8: *"In here I described how a thread is a reflection of the entire session. Recreating all threads
should produce bot messages as assistant message and user's turn messages as telegram user messages (via the secretary
enabled). When you recreated all threads you did not recreate with history which is the expected."* §6.7's D26 is decided:
the whole session as messages, not a file plus the last 10 turns.

- **What a topic replays.** The session's transcript from its first line up to where the live mirror starts (§21.6's
  seed: the end of the file, or a running turn's own prompt line), read in HISTORY mode: a fork's copied `forkedFrom`
  lines are included, so a fork's topic shows the conversation it was forked from (the live mirror still skips them). A
  fork with no message of its own yet replays its source's transcript. The range is persisted on the topic
  (`TopicRecord::backfillPath / backfillPos / backfillEnd / backfillDayKey`); a topic made by a Rekey (a /resume in the
  tab) replays the resumed conversation after its "a new conversation in this tab" line. Not replayed: subagent and
  teammate transcripts (sidechain lines), and the sessions a conversation continues (a /clear predecessor, a plan-restart
  parent), which are other sessions.
- **How a turn reads** (`RenderHistory`, `MessagingModels.h`): a day line when the day changes
  (`— Fri 16 Oct 2026 —`); the user's prompt; the assistant's narrative, PACKED into messages of at most 3,500
  characters — each remark (the "why the bash is invoked") followed by the tool calls it led to in one folded quote, a
  failed result labeled with its tool, and questions, plans, compactions, interrupts and API errors as blocks; and the
  turn's final reply as its own message. A message typed while the turn ran, a slash command and the user's ANSWER to a
  question are the user's posts between the assistant's: Claude Code keeps an answer on the question's tool-result line
  (`toolUseResult.answers`: the question → the chosen option's label or the user's own words), which the classifier
  turns into an `Answer` item — one answer reads as itself, several as `header: answer` lines; the bot's line in its
  place is `⌨ HH:MM · ↩ <answer>`. The LIVE mirror posts the answer the same way, under the question (before, both
  showed the question and dropped the answer). Measured on the dev fleet: 266 turns of 49 sessions come to 1,175
  messages (2.4 M characters); the live layout (one message per remark and per work-log segment) would have taken 6,581.
- **Checked on real sessions, offline.** A small driver (`histdump`, the session's scratchpad; never committed) runs
  `SplitHistoryWindow` + `PlanHistoryPosts` over a transcript and prints every post: ba1ee743 gives the 8 messages the
  live run sent; 9c3454bb 82, and 92 once its 10 answers are posts. It found a bug of the live mirror too: a link whose
  url holds parentheses (`https://en.wikipedia.org/wiki/Jordan_King_(basketball)`) lost its last `)` and left a stray
  one after the link — `TgMarkdownToHtml` now balances parentheses inside a url (CommonMark's rule), and a url with
  unbalanced ones is not a link. Still plain text in Telegram: Markdown TABLES (Telegram has no table), which Claude's
  replies often hold — a reply's table shows as its `|` rows.
- **The user's turns as the user.** With secretary mode working (the probe's `businessMode == 1` and a connection) a
  prompt, a mid-turn message and a slash command are sent AS THE USER (`_sendAsUser`, plain text); otherwise the bot posts
  `⌨ HH:MM · <first line>` (⚙ for a prompt the runner sent, when the queue still holds it), the whole prompt in a folded
  quote when it is long. A refused send as the user puts the bot's line in its place at once (a higher priority than the
  rest of the batch, so the order holds); three refusals in a row turn secretary mode off, as for a live prompt. The echo
  guard's entry for a history send lives 90 minutes instead of 2 (`kHistoryEchoMs`: a history message can wait long
  behind the live traffic and the flood budget), so an old prompt of the history that Telegram hands back is never
  queued again. ⚠ On this machine secretary mode is NOT
  connected (no `business_connection` update has ever arrived): BotFather's "Secretary Mode" switch is only the bot's
  side; the user must also add the bot under Telegram Settings → Business → Chatbots (Telegram Premium). Until then the
  user's turns are the bot's ⌨ lines.
- **The reader** (`SplitHistoryWindow`, `MessagingModels.h`; `ReadHistoryWindow`, `Messaging.cpp`): windows of 1 MB
  (growing to 64 MB when a window holds no complete line — a pasted image can make a line megabytes long; a longer line
  is skipped), cut at the start of the last turn begun inside, so every turn is rendered whole with its reply. A turn
  longer than a window is cut and goes on in the next (its reply is found where it ends); a turn that ends exactly at a
  window's edge with no turn after it in the window shows its reply as narrative.
- **Pacing and order.** History messages have their own, LOWEST send priority (`SendPriority::History`): the send loop
  takes one only when nothing else is queued, at most one per 2 s (`kBackfillGapMs`), and only while the last minute
  holds fewer than 18 calls of any kind (`kHistoryCallsPerMinute`, the CALL WINDOW `_recentCallsMs`, fed by the send loop
  and the tick's direct calls) — so ~18 history messages a minute when nothing else runs, fewer while the live
  traffic runs, which always goes first; after any 429 the replay pauses 5 minutes (`kHistoryFloodPauseMs`,
  `_noteFlood`, logged `[msg] history replay paused …`), live messages go on. Measured in the first live run (before
  the window): ~27 messages a minute for ~11 minutes earned a 429 (retry after 40 s) and then ran 5 more minutes without
  one — Telegram's budget for this chat drains near the documented group limit of 20 a minute, not at the 1 a second of a
  private chat. The pump (`_pumpBackfill`, every tick) keeps ONE batch (one window's messages) in the outbox at a time;
  each TURN is committed as soon as its last message is answered (`PlanHistoryPosts`: the window rendered turn by turn,
  the turn's last post carrying where the next turn starts), so a stop midway re-sends at most the turn that was going
  out — the first build committed only whole windows, and its 00:07 restart repeated part of one window in b480b2d7's
  topic. A batch is "stalled" only when all its messages left the outbox and none was answered for 30 minutes
  (`kBackfillStallMs`; a batch held by the budget is never stalled); it is then dropped and read again. The topics go
  most recent first (the highest-ranked in MOST RECENT order with history pending), so the sessions the user cares about
  now get their history first; the pump waits for the start-up hold (the order keys come from the re-homed sessions —
  the first build began an old session's replay at a restart) and, during a recreation, for the new list (at most 5
  minutes).
- **The card and the live mirror.** The card is posted first, above the history, with the line *"⏳ the session's history
  is being added below; the live conversation follows it"*; it does not chase each history message down (that would
  double the replay's calls) and is re-posted ONCE at the bottom, without the line, when the topic's history is in. For
  that, a batch's commit stamps the topic's last post (`lastPostedMs`) with the commit time: a post is stamped when it is
  QUEUED and a card when Telegram ANSWERS, so a history queued before its card went out (a new topic: both in one tick)
  read as older than the card, and the card stayed above the history (the first live run's first topic, 23:49). The
  live mirror of the topic waits meanwhile and then goes on from where the history ends, so whatever the session did in
  between follows in order.
- **The list's order after a replay** (`_resortTick`, `PlanTopicResort`). Telegram lists topics by their last message, so
  a replay that goes most recent first leaves the list in REPLAY order (the most recent at the bottom). When a
  recreation's replay is in, the list is RE-SORTED: the shortest set of card re-posts that puts it in MOST RECENT order
  (the top part of the desired order, re-posted lowest-ranked first, the top one last — after a full replay, all but
  one). The re-posts go out ONE per tick, and only while the call window has room for their two calls (the old card's
  delete, the new card's send): all at once, 48 re-posts are ~60 calls a minute, over the chat's flood budget
  (`_resortQueue`; the first build, 0.0.1.19 on dev, still marks them all at once). Afterwards, at most once a minute and only when everything is quiet (nothing queued, no replay, no session
  Running, nothing posted for 30 s), a list that drifted is fixed the same way when that takes at most 24 re-posts; a
  longer fix is logged once and left to `/recreate` (it happens when an old session is resumed and not used: its
  topic sits on top while its MOST RECENT key is old, and only re-posting every topic above its rank can move it down).
- **Layout 2.** `kTopicLayoutVersion` is 2, so the first start of this build makes the topics again once (§21.8's
  recreation), this time with their history; the notes in the 🔎 Search topic and `/recreate`'s question say so.
- **Logs:** `[msg] history replay of <sid8> started|goes on: bytes A to B of its transcript into thread N`,
  `[msg] history in for <sid8>: N message(s) from K KB of transcript, S s after the topic was made; …`,
  `[msg] history replay done: N message(s) into M topic(s); …`, `[msg] re-sort after the history replay: …` /
  `[msg] re-sort (the list drifted): …` / `[msg] re-sort skipped: …`, and on trouble `[msg] history of <sid8>: …`
  (a batch dropped as unanswered, refused messages, a transcript gone or shorter), `[msg] history replay paused 5 min:
  Telegram refused a <method> (429, …)`. The traffic line counts the replay's `sendMessage` calls with the rest.
- **Tests** (`TestMessaging`): the day key and day text; `RenderHistory` — the day lines, the user's posts with the
  bot's line in their place, the packed narrative (folded tool lines, a labeled failure), the reply as its own message,
  a turn cut by the reader's window and its rest, a mid-turn message, a slash command, a runner prompt (⚙), an
  interrupted turn, and 200 tool lines packed under the limit with none lost; `SplitHistoryWindow` — the end reached,
  the cut at the last turn's start, a turn longer than the window, a torn last line, a window with no complete line, a
  fork's copied prompt, each item's line offset; `PlanHistoryPosts` — each turn's last post commits where the next turn
  starts, the window's last turn its end, with the day; `PlanTopicResort` — in order, reversed, a low-ranked topic on
  top, a swap in the middle, a topic missing from the list; the record's round trip and `TopicBackfillPending`; layout
  2; the replay's lowest priority; the card's history line; the `Answer` item (one answer, several, a plain tool result
  stays one, a conversation message) and its post between the question and the reply; a url with balanced parentheses,
  and one with unbalanced. m5, isolated on HEAD 5f4c8f608 + the first build: 7214 checks; on 3bc71b412 + the flood
  budget and the per-turn commits: 7220; on c5f4d0c6c + the answers and the link fix: 7230 — the 3 known
  TestTaskbarBadge failures only, each time.
- **The live run** (dev, this machine, the owner's bot; 2026-10-16 23:44 → 2026-10-17 01:34):
  - **0.0.1.18** (3bc71b412, `Q:\agentmaster-layouts\Debug`, swapped 23:44:11, downtime 8.0 s): the start-up hold ended
    at 127 s; the layout-2 recreation deleted the 48 old topics in 60 s and made 48 new ones in 69 s (23:46:28 →
    23:48:37), in MOST RECENT order; the replay began at once with ba1ee743 (the session under QA: 8 messages), then
    9c3454bb (82 messages in 166 s — one per 2 s, as designed).
  - 23:49 — the first topic's card stayed ABOVE its history (the commit-time stamp, above). 23:59:54 — a **429**
    (retry after 40 s) after ~11 minutes at ~27 messages a minute: the flood budget (above).
  - **0.0.1.19** (`Debug-B`, swapped 00:07:45, downtime 6.2 s): the call window at 18 a minute; no 429 after it — the
    traffic lines read 180–185 calls per 10 minutes for the rest of the run. Its restart repeated part of one window in
    b480b2d7's topic and began an old session's replay before the start-up hold ended (both fixed in c5f4d0c6c, live
    from 0.0.1.20).
  - **0.0.1.20** (d9464b038 — c5f4d0c6c, a6cd24078 and 982615a6e with the other session's committed Search work —
    `Debug`, swapped 01:09:39, downtime 6.5 s, 20 s after a topic's history was in: the first messages of the next
    topic, 3d7a87b8, repeat once): it waited for the start-up hold (120 s), went on by rank from each topic's committed
    position, and from then on rendered the user's answers.
  - **01:28:33 — the replay was done:** all 48 topics, 1,600 history messages (the offline estimate was 1,175: the real
    renderer adds day lines and splits a narrative at its blocks), 1 h 40 min from the recreation. The re-sort released
    47 of 48 card re-posts one at a time, 01:28:33 → 01:33:49, no 429; afterwards the list (by the posts' own stamps)
    read in exactly the MOST RECENT order the recreation had logged — 0 of 1,128 topic pairs out of order, ba1ee743 on
    top (it had run a turn at 00:32), the oldest session (18 Sep) at the bottom. No HTML refusal, no exception.
  - **Open:** (1) the topics replayed before 0.0.1.20 do not show the user's answers to questions (a `/recreate` replays
    everything with them, ~1 h 40 min); (2) every restart re-edits ~2–3 times per card (125 edits in the 10 minutes after
    the 01:09 swap, 101 after the 23:44 one) — not the replay's, a cost to look into; (3) Markdown tables in replies stay
    `|` rows; (4) secretary mode is not connected, so the user's turns are the bot's ⌨ lines; (5) the user's look at the
    topics on the phone.

### 21.10 The Search answer as evidence cards, and an agent that reads before it shows (2026-10-17)

> Status: written 2026-10-17 (session cb14b056), NOT BUILT, no live run. The agent's half is `SESSIONS.md` §9.12.

**The user's choices** (after "Kindly work on the answering format better, show me 3 variations to choose from" and
"Does the search agent read the compact transcript of every session it matches against?"): the format **"C · Evidence
cards"**, variant **"C1 · One quote per card"**; the agent must **read every candidate and be able to grep the
transcripts**; both **Telegram and the Sessions page**. Before, the answer was one sentence ("…are on the screen…",
seen live) and a numbered line per session (title, state word, folder, "5m ago"), and the agent showed what a search
matched by title or path without reading it.

- **The answer** (`BuildSearchAnswerHtml`, MessagingModels.h, pure): the agent's verdict (escaped), then per listed
  session `<b>N</b> <glyph> <b>title</b>` (the short id when nothing knows a title; the title's first line, ≤ 100
  characters), a facts line `folder · day · N prompts` (the folder's last segment; `SearchDayText`: today / yesterday
  / "11 Oct" / "11 Oct 2025", local time, day first like `TgClockText`; parts that are unknown are left out), and the
  agent's `why` as one `<blockquote>` (left out when it gave none), then "(N more not listed …)". "Nothing found." when
  there is nothing at all. A long answer is split by `_sendText` (the buttons stay on the last part).
- **The glyphs** (`SearchPickStateGlyph`): 📦 closed, 💤 an idle tab, 💬 a topic of its own — the old row words
  ("closed", "idle tab", "has a topic") stay in the log line of a pick.
- **The pick menu** (`BuildSearchPickMenuHtml`): `<b>N · title</b>`, the whole folder and the branch, when it ran
  (`SearchSpanText`: "11 Oct 14:02 → 16:40", one date for one day, both across days, clock times today) and its
  prompts, the same quote, and `<i><glyph> <the state line></i>`; the buttons are unchanged (§21.7). The old closing
  question "Resume or fork it?" is gone: the buttons ask it. A pick of a session this run did not list (after a
  restart) shows what the registry knows (`branch`, `convCreatedUnixMs`, `convLastActivityUnixMs`), with no quote.
- **The row** (`SearchRow`, Messaging.h) gained `why`, `branch`, `createdMs`, `prompts`. The bridge fills them
  (`RowsFrom`): the why from the Shown event (`SessionSearchEvent::reasons`, index-aligned), the times and the count by
  the Sessions page's own rules (`SessionSearchLastActivityMs`, `SessionSearchCreatedMs`, `stats.userPrompts`); the
  title and the folder keep the bridge's rules on purpose (no "(no prompt yet)" placeholder, no project-folder
  fallback), because a pick's Resume launches with them.
- **The help text** of the topic says the agent reads the sessions it finds and quotes the evidence it saw.
- **Tests** (`TestMessagingSearch` §5): the day words (today, yesterday across a month and a year, this year, an
  older year, unknown), the menu spans, the whole answer of two cards (a quote, a session known only by its id, the
  count not listed), "Nothing found.", the escaping of a title and a quote, distinct glyphs, and both menus.
- **Not done:** no live run; the answer's look on a phone (the blockquotes' spacing) is unverified.

### 21.11 Rich messages, verbose or conversation-only topics, a state glyph in every topic name, a secretary bot (2026-10-17)

> Status: written 2026-10-17 (session 0b1f8635); LIVE on dev 0.0.1.21 (`Q:\agentmaster-layouts\Debug-B`, a purged full
> rebuild, 03:08), not in production. Checked live: the glyph renames and their notice deletes; the rich Markdown as
> Telegram parses it. Still open: §21.11.5.

**The user's four requests** (2026-10-17, after the §21.9 replay): "Can the threads name be edited to have an emoji
representing the status of the tab/thread similarily to how tabs present"; "When recreating the threads, prompts/messages
sent to claude by the user should be sent to the thread using secretary (bot acting on user's behalf posting in his
name), search online how it works"; "The threads should have global and personal verbose mode (telegram specific verbose
mode, not claude code's verbose) where: Verbose shows like now, the bash commands and more; Non-verbose: historically
shows only user's prompt and the assistant's turn's last message - so we get user->assitant->user->assistant messages.
User can recreate a specific topic as verbose or all, incl configuring the default for all (which is non-verbose by
default)"; "each message from claude code is most of the time markdown but we are plain text so markdown tables are not
showing beyond unformatted ascii".

#### 21.11.1 Claude's Markdown as RICH messages (tables, headings)

- **What Telegram offers** (Bot API 10.1, `sendRichMessage` + `InputRichMessage`): a message given as `markdown` (GitHub
  Flavored Markdown "where possible", with "arbitrary HTML" whose supported tags are parsed), `html` or `blocks` —
  headings, lists, task lists, TABLES (≤ 20 columns, inline formatting only in a cell), quotes, fenced code, `<details>`
  collapsible blocks (Markdown is parsed inside them), footnotes, math. Limits: 32,768 characters, 500 blocks.
  `editMessageText` takes `rich_message` too. The HTML parse mode of `sendMessage` (what §10.3's converter targets) has
  no table form at all — a table came out as its pipes.
- **Measured** (`richtest.py`, the bot's own chat, each message read back from the API's parsed `rich_message` and
  deleted): a `<`-tag Telegram does not know is DROPPED with its text ("List<int>" → "List"); `&lt;` restores it,
  `\<` does not; the other backslash escapes work (`\* \_ \| \$ \= \- \# \[ \] \` \~`); a single line break inside a
  paragraph is a space (GitHub's rule) and two trailing spaces keep it; `$5` stays text, `$HOME` becomes a cashtag;
  `<details><summary>…</summary>` around a fenced block is a collapsible block.
- **The preparation** (`TgPrepareRichMarkdown`, TelegramFormat.h, pure): every `<` outside code becomes `&lt;`; fenced
  blocks and inline code spans are copied exactly; a line another non-blank line follows gets two trailing spaces
  (Claude writes for a terminal that keeps its line breaks); a TABLE ROW (a line that starts with `|`) gets none; a line
  that starts with `<details>`, `<details open>` or `</details>` keeps its tags and a `<summary>…</summary>` right after
  the opening tag (`detail::MdKeepCollapsibleTags`) — the history's tool groups are such blocks.
- **The split** (`TgChunkMarkdown`): ≤ `kRichChunkMaxChars` (24,000 UTF-16 units — the room up to the 32,768 limit is
  for what the preparation adds) and ≤ 400 lines (the 500-block limit) per part, cut at a blank line outside code where
  one exists; a part that ends inside a fenced block closes it and the next part opens it again with the same line; a
  line longer than a part is cut hard, never between the two halves of a surrogate pair. A prepared part still over
  `kRichMessageMaxChars` (32,000: thousands of `<`) goes as HTML.
- **Our own lines** (`TgMdEscapeText`): a question, an option, an API error as Markdown that shows as typed — the
  formatting characters backslash-escaped, `<` → `&lt;`, `&` → `&amp;`, a number at a line's start kept a number
  (`1\.`), every line break kept.
- **The sender** (`MessagingService::_sendRich`): the parts as `sendRichMessage` items (silent but the last, `onDone` on
  the last, the reply reference on the first). A part Telegram refuses — any 400 that names no missing chat or thread,
  or a 404 (a Bot API without the method; then no rich message is tried again this run) — goes again as HTML **in its
  own place**: the caller's HTML for a message of one part, else that part through `TgMarkdownToHtml`. The place is kept
  by a new outbox order, `(priority, seq, subSeq)`: a message's parts share its seq, part j sits at `j × 4096`, its HTML
  fallback's chunks after it, and an HTML chunk's plain-text fallback (the old "can't parse entities" rescue, which went
  out at Final priority with a new seq — after later messages) now takes the slot right after the refused chunk too
  (`_htmlItems`, shared by `_sendText`). `_enqueue` keeps a seq an item brings.
- **What goes rich**: the final reply of a turn, the remarks (verbose), a plan ("📋 **Plan ready for approval** …" +
  the plan), an interrupt's text + "■ *interrupted*", and in the HISTORY replay the replies, the narrative and its tool
  groups. The ⌨ prompt lines, the questions, API errors, the work log (edited in place), the cards and every notice stay
  HTML (`sendMessage`). The traffic census counts `sendRichMessage` on its own.
- **The switch**: `AppSettings::messagingRichMessages` (default on; the cog's Messaging tab, "Send Claude's Markdown as
  rich messages (tables, headings)") — off = the HTML of §10.3, for a Telegram app too old to show rich messages.

#### 21.11.2 VERBOSE or the CONVERSATION ONLY — per topic, with a default for all

- **The modes**: verbose = the topic as before (§21.5/§21.9): the assistant's remarks, a work log of the tool calls,
  failed results, compactions. The conversation only = the user's posts (the prompt, a message typed while the turn ran,
  an answer, a slash command) and each turn's LAST message, plus the questions and plans that asked the user something,
  and an interrupt or an API error that ended a turn — user → Claude → user → Claude.
- **Where it is set**: `AppSettings::messagingVerbose` is the default for all topics (default OFF — the user's rule; the
  cog's "Topics show the work (verbose)"); `TopicRecord::verbose` overrides it per topic (-1 follows the default, 0, 1;
  persisted). `TopicIsVerbose` resolves it.
- **Commands**: `/verbose` in a session topic shows the topic's mode with buttons (Verbose · Conversation · Follow the
  default); `/verbose on|off|default` sets it. In the 🔎 Search topic `/verbose` (buttons, or `on|off`) sets the DEFAULT
  (a settings.json read-modify-write, live at once). A change applies to the messages from then on.
- **Recreating in a mode**: `/recreate` in a session topic offers "This topic · verbose / conversation" (the topic is
  deleted and made again at once with its session's whole history in that mode; `TopicRecord::verbose` keeps it; one per
  tick, `_recreateOneTopic` through `_recreateOne`; the outcome is the button's toast, the topic it sat in being gone)
  and "All topics · verbose / conversation"; `/recreate` in Search offers the latter two. Recreating all sets the chosen
  mode as the default first (the per-topic modes go with the old topics), then runs the §21.8 recreation.
- **The live mirror** (`_applyItems`): in the conversation-only mode a tool call drops the text gathered before it (a
  remark is not the turn's last message) instead of posting it, no work-log line is written, failed results and
  compactions are skipped, the text before a question is dropped. What is left when the turn ends is its final reply.
- **The history** (`RenderHistory(…, verbose)`, `PlanHistoryPosts(…, verbose)`): the same rule. Every bot post carries its
  Markdown (`HistoryPost::markdown`) beside the HTML: Claude's text as written, our lines escaped, a remark's tool calls as
  `<details><summary>🔧 Bash ×3, Edit</summary>` around a fenced block whose fence is longer than any backtick run in the
  lines. The day lines and the user's lines stay HTML only.

#### 21.11.3 The state glyph in every topic name

- **The glyph** (`StateGlyph`): the tab strip's status dot as an emoji — Running 🔵, Waiting for you 🟡, Needs approval
  🟠, Error 🔴, Done (claude exited) 🟢, Idle ⚪ (the tab's dot is gray; Telegram has no gray circle). The name is
  `TopicDisplayName(state, title)`: the glyph, a space, the title (≤ 120 units, "…", never cut inside a pair).
  `TopicRecord::title` keeps the BARE title (the sync compares it with the tab's), `nameState` the state the name shows
  (-1 = none yet), `nameSetMs` when it was set (both persisted). A topic made by an older build has no glyph and is
  renamed once.
- **When a name follows the state** (`PickStateRename`, pure, + `_stateNameTick`): at most ONE rename per tick, only for
  a state that has held `kNameStateSettleMs` (8 s — a short turn's Running ↔ Waiting flicker renames nothing), at least
  `kNameRenameGapMs` (30 s) after the topic's last rename, the states that need the user first (Needs approval, Error,
  Waiting for you, then the rest), and only while the last minute holds fewer than `kNameRenameCallsPerMinute` (12)
  calls and no 429 pause runs; never in the start-up hold or a recreation. A title change still renames through the
  sync's plan (its Rename / Create / Rekey ops carry the state), with the glyph.
- **The "topic renamed" notice**: Telegram posts a service message for every topic edit; the bot gets its own back
  through getUpdates (like the `forum_topic_created` of every topic it makes) and DELETES a `forum_topic_edited` one
  (Backfill priority; three refusals in a row stop the deletes). A rename therefore costs two calls.

#### 21.11.4 The SECRETARY: the user's turns as the user's own messages

- **How Telegram's Secretary Mode works** (core.telegram.org/bots/features, "Business Bots"; read 2026-10-17): the bot
  enables Secretary Mode in @BotFather; the user connects it to their account (Telegram → Settings → Chat Automation —
  formerly Business → Chatbots; since Bot API 10.0 no Premium is needed; ONE such bot per account) and chooses which
  chats it may access; the bot gets a `business_connection` update with the rights (`can_reply` covers "private chats
  with incoming messages in the last 24 hours") and sends on the user's behalf with `business_connection_id`. And, the
  part that decides the design, "Communication via Business Accounts": "If a bot is connected to a business account
  using Chat Access Mode, it can send messages to OTHER bots used by that business account. This requires Bot-to-Bot
  Communication Mode to be enabled for the bot that is sending the messages." A bot posting into its OWN chat with the
  user on the user's behalf — what §21.5's probe tries — is not documented.
- **So two paths**, one code path (`_sendAsUser` sends with the client of whichever bot holds the connection,
  `MessagingState::businessBotId`):
  1. **A second, SECRETARY bot** (new, optional): its token in Credential Manager
     (`Agentmaster:messaging:telegram-secretary:<profile leaf>`, the cog's "Secretary bot token (a second bot)"); the
     service makes a client and a poller of its own for it (`_secretaryPollLoop`: getMe once — its id, and
     `can_connect_to_business`, i.e. Secretary Mode on in @BotFather — then getUpdates with `allowed_updates` =
     `business_connection` ONLY, so it never receives the chats it may access; its own offset, `secretaryUpdateOffset`,
     reset when the bot changes; one poller per token machine-wide, like the topics bot's). A changed secretary token
     restarts the service at the next Apply. Its sends go through the same outbox (`OutItem::asSecretary`), in order with
     the bot's posts. The topics bot drops the copies it receives: they carry `sender_business_bot`, and the echo guard
     knows their text.
  2. **The topics bot itself** connected under Chat Automation: §21.5's path, unchanged — the probe finds out.
  A disconnect of the bot that does NOT hold the stored connection changes nothing (moving the connection between the
  two bots sends one a disconnect and the other a connect, in any order). The probe (one message as the user in the 🔎
  Search topic) runs after every connection; a refusal now explains the second-bot route.
- **The setup** (in the cog, under "SECRETARY (OPTIONAL)"): @BotFather → /newbot → paste its token; in @BotFather turn on
  its Secretary Mode and Bot-to-Bot Communication; Save; in Telegram → Settings → Chat Automation add the second bot, let
  it reply, and let it reach the chat with the topics bot. The cog's status line (`StatusText`) says where it stands:
  off / running and waiting to be connected / connected without the right to reply / checking / works (sent by @bot) /
  refused (with Telegram's reason) / a problem of the secretary bot (Secretary Mode off, token refused, polled
  elsewhere, the same token as the topics bot).
- **The history** already sends the user's turns as the user while secretary mode works (§21.9: `_pumpBackfill`'s
  `asUser`); a topic recreated after the secretary works carries them as the user's messages.

#### 21.11.5 Tests, the build, and what is open

- **Tests** (`TestMessaging`, the §21.11 blocks): the preparation (escapes, code spans and fences as written, hard breaks,
  none for a table row or before a blank line, the details tags kept and their summary escaped, a details tag inside a
  line escaped, CR dropped); the split (300 paragraphs, each part fits and starts at a paragraph, none lost; a 100-line
  code block closed and reopened with its language, no line lost; 300 emoji in one line never cut inside a pair); the
  escaping of our lines; the conversation-only history (user, mid-turn user, question, answer, reply — no narrative; an
  interrupted turn; a reply's table as written); the verbose history's details block (summary "Bash ×2, Edit", a 4-tick
  fence around a line holding three); the plan in both modes; the glyphs, the display name and its cut; the rename
  picker (urgency, the settle, the per-topic gap, an unknown clock, a session not live); a create carrying its state;
  the state file's new keys and an older file; both settings and their defaults. The isolated m5 run: 7,396 checks, the
  3 known TestTaskbarBadge failures only. `/Zs /W4` of Messaging.cpp + Messaging.Inbound.cpp: clean.
- **The build incident**: the first dev build of this work (0.0.1.21, incremental) crashed 2 s after the start-up hold
  ended — an AV in `std::_Container_base12::_Orphan_all_locked_v3` under `TerminalPage::_SidePanelSelectSession`'s
  assignment to `_sidePanelSelectedId` (`TerminalPage.AgentSidePanel.cpp:3101`), i.e. a member at another offset than
  the code expected. `AppSettings` (SessionModels.h) had gained two fields, and the incremental build left objects
  compiled against the old layout — exactly the build-install skill's §5.8 rule ("purge whenever SessionModels.h …
  changes"), which this build skipped. The purge (`Generated Files`, `obj\x64\Debug\TerminalApp`, `TerminalAppLib`,
  `agentmaster-cli`, `agentmaster-sessiondb`) and a full rebuild fixed it.
- **Checked live** (dev 0.0.1.21, 2026-10-17 03:08–03:20): the start-up hold ended at 03:10:16 and the first glyph
  rename went at 03:11:34 ("🔴 perf", "🟡 Agentmaster", …), then one about every 2 s until the call window filled; the
  traffic census of the first 10 minutes read `editForumTopic=42 deleteMessage=42` — Telegram DOES post a "topic
  renamed" notice in the bot's private chat, the bot gets each back through getUpdates, and every one was deleted (no
  refusal). The rich path, offline through the app's own code (`prepmd`, a probe that runs `TgChunkMarkdown` +
  `TgPrepareRichMarkdown`) and then sent to the 🔎 Search topic and deleted: a Claude-like reply came back as a heading, a
  paragraph whose line breaks and `List<int>` (in and outside code) are kept, a TABLE block (header row, a right-aligned
  column, `compile <TerminalApp>` and a bold cell intact), an ordered list, a `cpp` code block with `<typename T>`, a
  quote with a link whose url holds parentheses, and "$5 … x < y && y > z" as text; a verbose narrative's tool group came
  back as a `details` block titled "🔧 Bash ×2, Edit" holding the `text` code block (a three-backtick run and `"a<b>"`
  intact). The same 10 minutes held 123 card edits — the restart re-edit the §21.9 run already counted (125), not this
  work.
- **Open** (live): a reply's table on the phone and the desktop app (the API's parse is checked, not the clients'
  rendering; an app too old for rich messages is what the switch is for); a refused rich part falling back in its
  place; `/verbose` and the four recreate buttons; whether a rename moves a topic up the list for a moment (the notice is
  its last message until it is deleted); the secretary bot end to end — it needs the user's second bot and the Chat
  Automation step, nothing can be checked without them.

### 21.12 The user's line, the reply that quotes it, the status on the prompt; one bot per install (2026-10-17)

> Status: written 2026-10-17 (session 0b1f8635); LIVE on dev 0.0.1.22 (`Q:\agentmaster-layouts\Debug`, a purged full
> rebuild, 11:24), on the NEW dev bot. LIVE in production 0.6.8.21 since 12:01 (`Q:\agentmaster-layouts\Release`, a
> Release build of 46e3aad42, swapped by a one-shot task at the end of session 0b1f8635's turn, the bot switched on while
> production was stopped — §21.12.5).

**The user's requests** (2026-10-17 morning): "1. Lets introduce the changes to the user message: a. "👤 You · 14:02 · typed
at the PC" (or "📱 from the phone", "⚙ runner")\n<the prompt>" b. Make each reply a reply to its prompt. Send Claude's
final reply with reply_parameters pointing at that turn's prompt message. Telegram then shows a small quote of your prompt
at the top of the answer, so every pair is visibly you → Claude. This costs no extra calls; the prompt's message id only has
to be kept for the turn. c. A stronger, consistent marker. Replace the subtle ⌨ with a bold 👤 You header. d. A status on
the prompt itself. Edit the prompt line from "⏳ running"/"🔴 error" and others to "✅ answered" and when no longer the last
prompt user sent, remove the status on the prompt itself." and "2. We have a release version of telegram and a debug version
of telegram. The one used by debug should move to the release agentmaster … The new debug: <token>. Kindly do the move".

#### 21.12.1 The user's lines

- **One format** for every line the bot posts in the user's place, live and in the history (`BuildPromptLineHtml`,
  `BuildAnswerLineHtml`, `BuildCommandLineHtml`, MessagingModels.h, pure): a bold **👤 You** header, the clock (local), what
  it is — "typed at the PC", "📱 from the phone", "⚙ runner" (`PromptSource`), "mid-turn" for a message typed while the turn
  ran, "↩ answer", "command" — and, on the turn's prompt, its status; then the words on the next line (escaped; past 600
  characters or 8 lines an expandable quote; cut at 3,000 — an edit cannot be split). The old `⌨ HH:MM · <first line>` and
  `⚙` lines are gone.
- **Where the source comes from**: live, the session's queue (`QueuedPrompt::remoteSource` = a message of this topic, origin
  Autorun = the runner); a prompt sent from this topic is still not mirrored (the user's own message is there). In the
  history the same queue decides, so a phone prompt replays as "📱 from the phone".
- A send AS THE USER (the secretary, §21.11) stays the user's own bubble with no header.

#### 21.12.2 The reply quotes its prompt

- The turn's final message (the reply, a question, a plan, an interrupt, an API error — every `_postFinal`) is sent with
  `reply_parameters` = the turn's prompt message, `allow_sending_without_reply` true. **No extra call**: the id is the
  prompt send's own answer.
- **How the id reaches the reply** (`MessagingService::PromptRef`, an object shared behind a pointer — the tick works on a
  COPY of a session's mirror and writes it back, so a field a send's callback wrote meanwhile would be lost): the prompt
  line's `onDone` stores its message id; the reply's outbox item carries a pre-send hook (`OutItem::beforeSend`,
  `_applyReplyRef`) that reads it when the item GOES — the send loop makes one call at a time, in order, so the prompt has
  answered by then. A prompt sent from the phone: its id is the user's own message (from `remoteSource`), known at once. A
  prompt sent as the user by the secretary bot: its id in THIS bot's view comes with the echo the topics bot receives
  (`_notePromptEcho`, matched by the text's hash); until then, or when a send was refused, the reply goes without a quote.
- **The order fix it needed**: prompt lines went at the New priority and replies at Final, so a fast turn (prompt and reply
  read in one tick) posted the reply FIRST. Prompt lines now go at Final, the reply's priority, in queue order.
- **The history** marks each turn's prompt and reply (`HistoryPost::turnPrompt` / `turnReply`); `_pumpBackfill` keeps the
  turn's PromptRef while it queues the batch, so the replayed reply quotes the replayed prompt too (not a prompt replayed as
  the user — that id is in the user's view).

#### 21.12.3 The status on the prompt itself

- **Only the LAST prompt** carries a status (`TopicRecord::prompt*`, persisted, so a restart keeps updating it): ⏳ running,
  🟠 needs you, ✅ answered, 🔴 error, ⏹ ended, ■ interrupted (`PromptTurnStatusFor`: Running, Needs approval, Error, Done,
  and at rest answered — or interrupted when the user stopped the turn). When the next turn's prompt arrives, the previous
  prompt's status is removed (a message typed mid-turn is not a turn's prompt and changes nothing).
- **Our line** (typed at the PC, the runner's, a refused send as the user): the status is an EDIT of the line
  (`editMessageText`, the cards' priority, keyed per message so a newer status replaces one still queued). **The user's own
  message** (from the phone; sent as the user): a bot cannot edit the user's words, and Telegram allows a fixed reaction set
  (no ⏳ / ✅ / 🔴), so the status is a REACTION: 👨‍💻 running, 🤔 needs you, 👌 answered, 💔 error, 😴 ended, 🙈 interrupted;
  none once a newer prompt was sent. A phone prompt that FAILED keeps its 👎 (it never became a turn).
- A new state shows once it has held `kPromptStatusSettleMs` (3 s — a recon synth undone a pass later must not edit twice);
  an Error at once. Cost: two edits (or reactions) per turn — ✅ at the end, removal at the next prompt.
- **A fix it needed**: `_syncReactions` re-set 👨‍💻 on every Sent phone prompt of every queue after each restart (`_reacted` is
  per run) — a burst of calls that would also undo the statuses. A prompt picked up before the run saw it queued is now
  recorded, not reacted.

#### 21.12.4 One bot per install — the move

- **Before**: `@ClaudeSessionsGateBot` (8965034992) was polled by the DEV install; the RELEASE install had no bot.
- **The new dev bot**: `@ClaudeSessionsDebugGateBot` (8986177119, created by the owner; Threaded Mode ON — checked with a
  read-only getMe). Its token is in Credential Manager `Agentmaster:messaging:telegram:.agentmaster-dev` and in the hub file
  `K:\source\telegram\ClaudeSessionsDebugGateBot.env` (untracked, like the other bot's).
- **The move** (11:24): dev's GUI stopped (no session was running); dev's state file (the old bot's 48 topics, the Search
  topic 22371, the update offset) COPIED to the release profile (`~\.agentmaster\messaging\telegram\state.json`), so the
  release, once its bot runs, knows the old topics: it deletes them as "closed" after its start-up hold (they are dev's
  sessions) and keeps the Search topic; the old bot's token copied to `Agentmaster:messaging:telegram:.agentmaster`; the new
  token stored for dev; dev swapped to 0.0.1.22. Dev's log: `the bot changed (@ClaudeSessionsGateBot ->
  @ClaudeSessionsDebugGateBot): … reset`, `connected as @ClaudeSessionsDebugGateBot (topics enabled)`, a new Search topic
  (thread 25027). The hub map (`K:\source\telegram\CLAUDE.md`) and the old bot's file header say who polls which bot now.
- **⚠ Production could not run it as it was**: production is 0.6.8.20 (`Q:\agentmaster-layouts\Release-B`, swapped 06:22
  today), whose TerminalApp.dll was built on 2026-10-14 — before the Telegram feature existed (2026-10-16; a probe of the
  DLL finds none of its strings). The user's answer (2026-10-17): build and swap production now, the bot on (§21.12.5).

#### 21.12.5 Production gets the bot (2026-10-17 midday)

- **The build**: a purged Release build of HEAD 46e3aad42 (`SessionModels.h` and `SessionRegistry.*` changed since the last
  Release build, dd8e97e35) into `Q:\agentmaster-layouts\Release` while production kept running from `Release-B`: 692 s
  ("Build OK in 685.7s"), 3 compilers, lowest free commit 22.1 GB. Checked: TerminalApp.dll bakes 46e3aad42 and holds the
  messaging strings (production's has none), the manifest is the unstamped Agentmaster 0.0.1.0, 192 icons, nothing runs
  from the folder. Agentmaster.exe (2026-10-11) and Microsoft.Terminal.Control.dll (2026-10-06) kept their dates: none of
  their sources changed since (git log).
- **Why the bot is switched on by a script, at the one safe moment**: 0.6.8.20 saves `settings.json` from its own settings
  struct, which has no messaging fields, so any save it made (a cog Save, a sort toggle's read-modify-write) would have
  dropped "Run the Telegram bot" written before the swap. The swap runs `-NoLaunch`; while nothing of production runs, a
  helper writes `messagingEnabled: true` + `messagingTelegramOwnerId` (a JSON load + dump that gave back the identical bytes
  for the release file, plus a backup copy beside it); then the app starts and reads them at its engine init.
- **Who runs the swap**: session 0b1f8635 runs INSIDE production, and the swap ends every production claude. So a one-shot
  scheduled task (Interactive, the user's own token) runs it outside production's process tree: it deletes itself, gates
  the layout, waits for 0b1f8635's turn to end (15 s at rest), takes the build mutex, gates again, waits up to 20 min for
  the other Running sessions (then cuts their turns off — the user accepted "a running turn is cut off"; such a turn reads
  Error after the restart), swaps, writes the two settings, launches, and watches the new instance up to 60 min for the
  bot's `connected as` line. The new instance first asks "Reopen your 4 previous Agentmaster windows?"; the engine, and so
  the bot, starts after the user's answer.
- **What its first start does** (measured before the swap, confirmed by the user): a topic for every tab that is not Idle —
  411 of production's 666 Claude tabs (406 are saved "Waiting for you", which decays to Idle only once READ and past the
  timeout), against dev's 48 — and dev's 48 old topics in that chat deleted as "closed" after the start-up hold; then the
  history replay, conversation-only by default: ~2,000 typed turns in those sessions (the Sessions sidecars' `userPrompts`),
  ~4,000 silent messages at 18 calls a minute — about an hour for the topics and their cards and four for the history.
  Live turns post meanwhile, slower.
- **Rollback**: `tools\Swap-AgentmasterLayout.ps1 -LayoutDir 'Q:\agentmaster-layouts\Release-B' -PackageName Agentmaster`
  (0.6.8.20; its settings saves then drop the two messaging keys again, and its bot does not run).
- **The outcome — production 0.6.8.21 runs the bot since 12:01** (the task's log, then production's hooks.log after the
  recorded offset): the task waited 55 s for the session's turn to end (11:59:37 → 12:00:32), took the mutex, passed the
  gate again, found no session Running. Meanwhile the USER had quit production (12:00:15 a first "close all windows",
  cancelled; 12:00:34 `[nav] quit confirmed (closing all windows)`), so the swap found no GUI to stop: drain 1.3 s (0 hosts
  left), register 3.2 s at the first attempt, the settings written (`setmsg: written: messagingEnabled=true,
  messagingTelegramOwnerId=473303454, 111 settings in all (verified)`, backup `settings.json.pre-telegram-20261017-120100`),
  the launch at 12:01:00 — about 26 s without production, counted from the user's quit. The new instance: `[msg] messaging
  started (owner 473303454, idle grace 10 min, topics conversation only by default, rich messages)` 12:01:03.9,
  `[msg] connected as @ClaudeSessionsGateBot (topics enabled)` 12:01:04.8 (no Reopen dialog was seen by the watch), the four
  windows re-homed 69 + 543 + 7 + 48 Claude tabs and 4 shells, none skipped, by 12:04:41, `[msg] start-up hold ended 229 s
  after the start (every window restored)`; then dev's 48 old topics deleted as "closed" and **413 topics created** by
  ~12:25 (the estimate was 411), the history replay begun most recent first; no 429 at ~40–50 calls a minute
  (`[msg] traffic (10 min): createForumTopic=235 … sendMessage=249 … (496 calls)` at 12:21).

### 21.13 The title wears the tab's color, and a line tells a new title or color (2026-10-17)

> Status: written 2026-10-17 (session 13e76a66, commit 3a73980d9); LIVE on dev 0.0.1.23 since 13:16
> (`Q:\agentmaster-layouts\Debug-B`); not in production (0.6.8.21 is the build of 46e3aad42) — a production build and
> swap need the user's word.

**The user's request** (2026-10-17): "Can the title have a color background in all places we print title, that matches and
also detect change and change according / Also when title edited, can we send a message like whatsapp does for group chat
name changed (agentmaster on tab title change or color change should report to the bot and edit the title mentions,
efficiently)". Their answers: the topic name **"State, then color"** (`🟡 🟦 agent manager tab`, the card
`📌 🟦 agent manager tab`), with "Can we check if we can using markdown or html or whatever works"; the line **"Telegram's
own line"**, worded `"<from>" changed the title to "<to>"`.

#### 21.13.1 What Telegram can show (read and measured 2026-10-17)

- **The Bot API** (10.3, the reference read 2026-10-17): a topic's `icon_color` is one of six (0x6FB9F0, 0xFFD67E, 0xCB86DB,
  0x8EEE98, 0xFF93B2, 0xFB6F5F) and is set only by `createForumTopic`; `editForumTopic` changes the name and
  `icon_custom_emoji_id`, never the color; `getForumTopicIconStickers` lists 112 icons, none a color swatch; a custom emoji
  needs a Premium owner (or a Fragment username). A topic NAME is plain text.
- **A live test in the dev chat** (scratchpad `colortest.py`, its test topic — thread 26785 — left there for the user to
  see): (1) a square emoji — kept; (2) HTML `<span style="background-color">` — refused ("Tag span must have class
  tg-spoiler"); (3) HTML `<mark>` — refused; (4) a rich message's `==marked==` — kept, as a "marked" highlight the CLIENT
  paints in its own color; (5) a rich `<mark>` — the same; (6) a rich `<span style>` — the style dropped; (7) a custom-emoji
  entity — dropped (no Premium); (8) colored buttons (`InlineKeyboardButton.style`: danger red, success green, primary
  blue) — kept, but a button is no title.
- **So a colored SQUARE emoji** is the one marker that shows in a topic name and in every message, in the tab's color
  family, and that an edit can change. Telegram's own "topic renamed" notice is fixed text that names no old title and
  carries no color, so the line is ours, in the user's words.

#### 21.13.2 The square, and where it shows

- **`TgTitleColorSquare`** (TelegramFormat.h, pure; `TgParseRgbHex`, `TgTitleColorPrefix`): the tab color by HUE (HSL),
  not by distance — the palette is pastel (One Dark), and an RGB or Lab distance to the vivid emoji made its reds and
  corals brown: very dark ⬛, very light ⬜, unsaturated ⬛ / ⬜ by lightness, a dark warm hue 🟫, then red [330°, 12°) 🟥 ·
  orange [12°, 35°) 🟧 · yellow [35°, 70°) 🟨 · green [70°, 165°) 🟩 · blue [165°, 255°) 🟦 (cyan joins blue) · purple 🟪.
  The 14 automatic colors land on six squares: #E06C75 🟥 · #E5C07B 🟨 · #98C379 🟩 · #56B6C2 🟦 · #61AFEF 🟦 · #C678DD 🟪 ·
  #D19A66 🟧 · #BE5046 🟥 · #528BFF 🟦 · #7FD962 🟩 · #FF9E64 🟧 · #2BBAC5 🟦 · #B267E6 🟪 · #F78C6C 🟧. Two folders can
  share a square: it marks a color family, it is not unique. "Remove colors" (or no color) = no square anywhere.
- **The color** is resolved like the tab's: the sync fills `TabFact::colorHex` for every live session with
  `ResolveSessionColorHex(mode, session, LoadDirColorIndex())` — ONE dir-colors.json snapshot per pass — so the topics follow
  the same tab-color mode, git-worktree rule and inferred folder as the tabs, the board and the side panel.
- **Where it shows**: the topic NAME — `TopicDisplayName(state, square, title)`, the glyph, the square, the title (within
  Telegram's 128 characters); the CARD's title — `📌 🟦 <b>title</b>`, the square the NAME shows (`CardExtras::colorSquare` =
  `TopicRecord::nameColor`), so the card and the list always agree; the Search answer's cards (`<b>1</b> 📦 🟦 <b>title</b>`:
  "State, then color") and the pick menu (`<b>1 · 🟦 title</b>`) and its action line; every reply that names a session —
  `/rename`, `/summary`, `/status` (the card), the confirm of `/restartsession` / `/deactivate` / `/interrupt`, `/idle`,
  "Opened … — its topic is coming up" (`TitleMentionHtml`, `MessagingService::_titleSquare`). A session the registry does not
  know (an old one the Search found) wears its folder's color, like the Sessions page's chip of an on-disk session.
- **Not changed**: the topic's ICON color stays the creation-time pick of the six (`TgTopicIconColor`; Telegram cannot
  change it later). Messages already sent keep the square they were sent with: the name and the card are what is edited.

#### 21.13.3 Following a change

- **A new title**: the sync's Rename (`PlanTopicSync`, at once, as before), now with the square.
- **A new color** (`PickNameRename`, pure, replaces `PickStateRename` in `_stateNameTick`; the state-only rule stays the
  first part of it): a name whose square differs from the tab's is due once that color has held `kNameStateSettleMs`
  (8 s, `TabFact::colorSinceMs` — the service's own clock, a first sight in the run reads as settled), so an inferred folder
  that flips back and forth renames nothing. The order: a state that needs the user (as before), then a real color change
  of a tab, then the QUIET re-squares (a topic named before §21.13, or under another mode). Still at most one rename per
  tick, only while the last minute holds fewer than 12 calls and no 429 pause runs, never in the start-up hold or a
  recreation (§21.11.3). Each rename sets the whole name, so one call fixes the glyph and the square.
- **A rename for another reason** never carries a color that is still changing (`ChooseNameColor`: the tab's square once it
  has held, else the one the name shows, with its mode).
- **`TopicRecord::nameColor` + `nameColorMode`** (persisted: `nameColor`, `nameColorMode`; absent = "" and -1): the square the
  name shows and the tab-color mode it stands for. A square that changes under the SAME mode is a change of the tab's
  color; one recorded under another mode (or -1) is re-squared quietly. A name whose square already IS the tab's takes the
  current mode silently, without a call — once the mode and the color have held.
- **A mode switch** (the cog's Tab coloring): the sync logs `[msg] tab color mode changed (a -> b): the topic names take the
  new colors quietly`; for `kColorModeSettleMs` (60 s) after it a color change is the switch's doing (the Individual mode
  deals new colors moments after the switch), so no line tells it (`NoticeOldSquare`). The same 60 s hold covers a topic
  just made: a new tab's Individual color is dealt at its first paint, a home-folder launch finds its inferred folder later.
- **The first start of this build**: every existing topic has no square yet; the name tick re-squares them quietly, one at a
  time — three calls each (the edit, the delete of Telegram's notice, the card's edit), about four a minute while the chat
  is otherwise quiet. Production's ~413 topics take roughly 1 h 45 min, and only once the history replay is done (the
  replay keeps the minute near 18 calls; the name tick waits for fewer than 12). Nothing else changes for them meanwhile:
  the card shows the square the name shows, so no card is edited before its name.

#### 21.13.4 The line in the topic

- **`BuildTopicChangeNoticeHtml`** (pure), italic, in the user's words: `“🟦 old” changed the title to “🟩 new”` (a color that
  changed at the same time shows in the two squares) · `“🟦 tab” changed the color to 🟩` · `The title is now “🟦 new”` (no old
  title known). The titles are escaped and cut like the Search answer's (200 units).
- **Sent** by `_renameTopic` after a rename Telegram took: silent, at the New priority, and it does NOT move the sticky card
  (one call, not three: the next real post moves the card below it). Telegram's own `forum_topic_edited` notice is still
  deleted when it arrives (§21.11.3).
- **No line** for a state-only rename, a quiet re-square (a topic named before §21.13, a mode switch, "Remove colors", a topic
  younger than 60 s), a refused rename, or a topic whose history is still being replayed (it would land among the old turns;
  logged `[msg] topic <id> changed (title|color): no line while its history is being added`). A sent one logs
  `[msg] topic <id> told: its title|color changed`.
- **With other lines**: a `/rename` from the topic gets its reply (`Renamed to 🟦 new …`) and then the line; a topic carried
  over to the next conversation in its tab (a Rekey, §21.6) with a new title gets "— a new conversation in this tab —" and
  then the line.
- **Cost**: a title change = the rename (1) + Telegram's notice deleted (1) + the line (1) + the card's edit (1); a color
  change the same, paced by the name tick; a quiet re-square 3.

#### 21.13.5 Tests and what is open

- **Tests**: `TestMessaging` — the 14 automatic colors' squares, alpha, brown, greys, black, white, malformed colors, the
  parse, the prefix; the name with the square (a one-unit glyph too, the cut within 128, no square = the old name); the
  mention; the line (a new title, a new title and color, the color alone, nothing changed, a square that only appears or
  goes, escaping, no old title); `PickNameRename` (a quiet re-square, the color settle, a real change before quiet ones, a
  state and a color together, a needs-you state first, the gap, not live, a square already right); `ChooseNameColor`;
  `NoticeOldSquare` (the same settled mode, a mode switch moments ago, another mode, a topic named before the square, a
  young topic and an older one); the state file's new keys and an older file; the card with and without the square.
  `TestMessagingSearch` — the answer's card and the pick menu with a square. The isolated m5 run (HEAD + this work): 7,460
  checks, the 3 known TestTaskbarBadge failures only. Two earlier runs of the work had one more failure each, a different
  Scheduler integration check each time ("question cleared: the queued prompt fires", then "the held prompt self-releases
  once the status held for the full window") — timing checks on a machine busy with production's history replay; nothing
  here touches the scheduler. `/Zs /W4` of Messaging.cpp + Messaging.Inbound.cpp: clean.
- **The build**: Debug, HEAD 3a73980d9, into `Q:\agentmaster-layouts\Debug-B` while dev ran from `Debug` — run from a
  session INSIDE PRODUCTION, so no scheduled task was needed (the dev swap never touches production): `devbuild.ps1`
  (relbuild.ps1 with the Debug configuration and the dev identity; this session's scratchpad `swap\`), no purge trigger
  since the last Debug build (b669747c0 + the §21.12 work), yet a FULL rebuild of TerminalAppLib (pch.cpp first) — the
  11:40 Release build had purged the `Generated Files` folder the two configurations share — 537 s with 3 compilers, lowest
  free commit 26.4 GB. Checked: TerminalApp.dll bakes 3a73980d9, the unstamped AgentmasterDev 0.0.1.0, 192 icons, nothing
  ran from the folder.
- **The swap** (13:16:30, `Swap-AgentmasterLayout.ps1 -PackageName AgentmasterDev`, one open session, Idle): drain 2.8 s,
  register 3.1 s at the first attempt, **downtime 6.5 s**; `[msg] connected as @ClaudeSessionsDebugGateBot` 1 s after the
  start; the window resumed 56 tabs, none skipped; the start-up hold ended at 120 s.
- **Checked live** (dev hooks.log after the swap, the state file): the name tick re-squared all **48** existing topics
  QUIETLY, 13:19:23 → 13:29:01 (about five a minute, one or two seconds apart in bursts, then a wait for the call window):
  `🟡 🟥 Agentmaster`, `🟡 🟦 docs 2`, `🟡 🟦 OptunaSharp`, `🟡 🟨 Desktop`, `🟡 🟩 chinese models`, …; no change line was sent
  for any of them, as designed; no 429. The first 10-minute census: `deleteMessage=35 editForumTopic=34 editMessageText=131
  sendMessage=2 setMyCommands=1 (203 calls)` — every rename's "topic renamed" notice deleted, and the card edits are the
  restart's usual re-edit plus one per re-squared card. `messaging\telegram\state.json`: 48 topics, 48 with `nameColor`,
  48 with a known `nameColorMode`.
- **Open** (live, the user's): the squares on the phone and the desktop app; a tab's recolor (the line `… changed the color
  to 🟩` about 8 s later); a rename at the PC and with `/rename` in the topic (the line at once); a mode switch in the cog
  (quiet, `[msg] tab color mode changed`); the Search answers' squares.

### 21.14 A topic's name follows its session during the history replay; a long 429 holds one method, not the bot (2026-10-17)

> Status: written 2026-10-17 (session 0b1f8635, commit 4f3c643d9); LIVE on dev 0.0.1.24 since 17:24
> (`Q:\agentmaster-layouts\Debug`); not in production — 0.6.8.21 (the build of 46e3aad42) still has the rename gate that the
> replay starves and the 429 sleep that silences the bot.

**The user's request** (2026-10-17 ~17:00, after five hours of production's topics): "Can we rename topic when the topic has
a running session (blue circle) instead of the yellow one or as red when error".

#### 21.14.1 What was wrong (production 0.6.8.21, 12:01 → 17:03)

- **No rename could happen.** The glyph rule of §21.11 and its picker were right (Needs approval, Error, Waiting for you
  first; a state renames once it has held 8 s; a topic at most every 30 s). But `_stateNameTick` renamed only while the
  chat's call window held FEWER THAN 12 calls, and from 12:16 on the history replay of the 413 new topics held that window
  at its cap of 18 a minute (every `[msg] traffic (10 min)` line: 180–186 calls). Result: **one rename in five hours** (a
  title change, `topic renamed 0b1f8635 → “🟡 teams/telegram integrationforum”` at 13:38). A topic made 🟡 stayed 🟡 while its
  session ran (`e7fe1323 waiting -> running` 16:19:33, no rename) or failed.
- **A second fault, in the same log.** At 17:00:59 Telegram answered one editMessageText with 429 and `retry_after 6542 s`.
  The send loop slept a 429's whole retry_after before retrying the same item, so from then on NOTHING was posted —
  replies, prompt lines, cards, history — until about 18:50, while `_noteFlood`'s line said "live messages go on". The
  tick's topic calls had the same sleep, and createForumTopic's did not even end for Stop (a quit could hang that long).
  Which edits piled up is not in the log: the census counted every editMessageText as one number (36–57 per 10 minutes all
  afternoon). One fact found reading for it: the card shows the unsent draft (`✎ unsent at the PC: “…”`), so typing in a tab
  re-edits that topic's card every 20 s while the first line changes.

#### 21.14.2 A rename goes before the replay

- A due rename (PickNameRename, unchanged) is made when the window leaves room for its two calls (the edit, and the delete of
  Telegram's "topic renamed" notice) under the replay's budget: `NameRenameFits(calls, kHistoryCallsPerMinute)`, i.e.
  calls + 2 ≤ 18. With no room it stamps `_renameWaitingSinceMs`, and the send loop holds HISTORY items while the stamp is
  younger than `kNameRenameHoldMaxMs` (90 s; `HistoryYieldsToRename`): the window drains a call every few seconds, the next
  name tick (every 1.5 s) makes the rename, and the replay goes on. The live traffic still goes first (it never checks the
  window); the 90 s bound only matters when the live traffic alone keeps the window full.
- **A rename refused by the flood control stays due.** The record took the new state "either way", so a 429'd rename kept
  the old glyph until the session's next change; now it is made again after the pause. Any other refusal is still recorded
  (a name Telegram will not take must not be retried every tick).
- The m5 suite models two minutes of the replay at its cap: the due rename is made within seconds (the old rule found no
  moment in the whole run), and the replay goes on after it.

#### 21.14.3 A long 429 parks the refused method

- **`FloodParking`** (MessagingModels.h, pure): a 429 whose retry_after is above `kFloodParkAfterSec` (60 s) parks only the
  refused METHOD until its deadline. Its items stay queued in their places (an edit that a newer edit of the same message
  replaced meanwhile is dropped), and every other method goes on. A long 429 on ANOTHER method while one is parked means
  Telegram holds the whole chat: every method parks until the later deadline. A shorter wait is still slept through by the
  thread that met it (the outbox keeps its order). Log: `[msg] pacing: 429 on editMessageText (retry after 6542 s):
  editMessageText waits until 18:50; the other calls go on`, or `… a second method refused — every call waits until …`.
- **The tick's topic calls** (create, delete, rename) skip a parked method: `_awaitCallSlot` returns false and the work is
  left for a later tick. Their own 429s go through `_handleTickFlood`: a long one parks, a short one is waited out
  stop-aware. A closed session's topic delete refused this way keeps its record (it used to drop it and leave the topic in
  the chat).
- The secretary bot's sends park separately (`_secretaryFloodParking`: its flood control is that bot's own).
- **The hold covers the whole chat, measured after the commit** (§21.14.5): the topic renames production tried at 17:06 were
  refused with the SAME deadline as the 17:00:59 edit (18:50:02). So the first call on another method after a long 429
  gets its own 429 and every method parks: one more refused call than the old code made, and the same silence until the
  deadline. The parking keeps a hold that covers only one method from silencing the others; a hold on the whole chat cannot
  be worked around.
- **The census says what was edited:** `[msg] traffic (10 min): … editMessageText=57 … edits(card=40 prompt=12 other=5)`
  (`EditKindOfKey`: `card:` → card, `pl:` → prompt, `wl:` → worklog, else other).

#### 21.14.4 Checked

- **m5, isolated** (HEAD 9dbff4afc + these files only): 7,482 checks, only the 3 known `TestTaskbarBadge` failures. The new
  blocks: the budget rule (room for two calls under 18), the replay's hold and its 90 s bound, a two-minute model of the
  replay at its cap with a rename falling due (made within 10 s; the old rule found no moment; the replay goes on), and the
  flood parking (one method parked, the others free; a second method escalates to all until the latest deadline; an
  expired entry neither holds nor escalates; a retry_after under a second counts as one), plus the census kinds.
- `/Zs /W4` of `Messaging.cpp` + `Messaging.Inbound.cpp`: clean.
- **Dev 0.0.1.24** (17:24): an incremental Debug build into `Q:\agentmaster-layouts\Debug` (no purge trigger since dev's
  3a73980d9 build; 196 s, `-MaxCpu 4 -ClMpCount 6`); the layout bakes 4f3c643d9 and carries the new log strings; swapped
  from a shell inside production (0.0.1.23 @ `Debug-B` → 0.0.1.24, drain 2.3 s, register 2.7 s, **downtime 5.8 s**, one Idle
  session). The new instance: connected as `@ClaudeSessionsDebugGateBot` 1 s after the start, 57 tabs resumed, none skipped,
  the start-up hold ended at 121 s, no `[exc]`. A real state change came at once (`61e3e3ff idle -> error`, a cut-off turn)
  and renamed nothing — correctly: its topic was named 🔴 at 13:23 by the previous build. Dev runs no replay any more, so
  its renames went before this change too: the dev run proves the build, not the fix.
- **Not checked live yet:** a rename during a replay, and the parking of a long 429 — production is where both happen (its
  replay runs for hours; its 6542 s hold ends at 18:50:02).

#### 21.14.5 Production after 17:00 (read from its hooks.log at 17:33; production still runs the old code)

- **One deadline for every method.** The edit's 429 at 17:00:59 said `retry after 6542` = 18:50:01. The renames the old
  code tried (the window had drained while the send loop slept) were refused with the same deadline: five at
  17:06:00–17:06:07 (`topic rename failed for 0cf68609: Too Many Requests: retry after 6242`, then 54821a94, 13e76a66,
  7eb21003, 0b1f8635), 0cf68609 again at 17:10:59 (`retry after 5943`), 13e76a66 at 17:26:50 (`retry after 4992`). Each
  one ends at 18:50:02. Telegram is holding the chat, not only editMessageText. Sends were never tried (the old send loop
  sleeps through the hold), so they are not proven held, but nothing points the other way.
- **Records the old code wrote wrong.** It recorded a rename refused this way as made, so these topics' records may name a
  glyph their topics do not show. A record corrects itself at the session's next state change; the new code does not
  re-check a name it recorded.
- **The replay:** between 12:16 and 17:00, 289 of the 412 topics got their history in; 123 are left (232 MB of transcript).
- **The traffic before the hold** (the `[msg] traffic (10 min)` lines):

  | Time | Calls per 10 min | editMessageText | Notes |
  |---|---|---|---|
  | 12:01–12:21 | 404, 496 | 0–3 | the topic creation: 413 createForumTopic + 426 sendMessage (the cards) and 48 orphan deletes, ~45 calls a minute, no 429 |
  | 12:21–13:01 | 181–186 | 36–44 | the replay at its cap of 18 a minute |
  | 13:01–15:11 | 174–187 | 10–25 | |
  | 15:11–17:01 | 180–187 | 23–70 | the edits rose from about 15:31 on (47, 43, 42, 51, 48, 67, 69, 70, 57) |

- **Why Telegram answered with 1 h 49 min, not seconds: not known.** No short 429 came first. Two readings fit the log.
  Either the chat has a budget over a longer window than a minute (dev's replay of §21.9 ran 1 h 40 min at the same cap
  without a long hold; production's ran 4 h 45 min), or the edits that rose from about 15:31 on crossed a limit of their own.
  Either way, the 123 topics left (about two hours at the same pace) could earn another long hold after 18:50. A lower
  replay budget than 18 calls a minute would lower that risk; it is not changed.

### 21.15 Every title is a color chip — an in-text button in the tab's color family (2026-10-17)

> Status: written 2026-10-17 (session 13e76a66, commit 7017fd863); LIVE on dev 0.0.1.25 since 18:25
> (`Q:\agentmaster-layouts\Debug-B`); not in production (0.6.8.21 is the build of 46e3aad42) — a production build and
> swap need the user's word.

**The user's request** (2026-10-17, after looking at the color tests in thread 26785 of the dev chat): "Go for 10 with
copy_text button is great, even better if we can specify hex color code or equivalent of hex color code". Test 10 showed the
title as an in-text copy_text button of a rich message, in each of Telegram's button looks.

#### 21.15.1 What Telegram allows (read and measured 2026-10-17, the dev bot, thread 26785)

- **No color of its own, three looks.** A button's `style` is “danger” (red), “success” (green) or “primary” (blue); a rich
  message's button adds “link” (a borderless link, callback buttons only). The docs: "Apps may use theme-specific colors for
  the button background and text based on the style". So a hex color cannot be asked for, and the shade on the screen is the
  app's — the background and a matching text color, which is what a chip gives.
- **Exact colors would need custom emoji** (a set of 14 colored squares): "Custom emoji entities can only be used by bots that
  purchased additional usernames on Fragment or in the messages directly sent by the bot to private, group and supergroup
  chats if the owner of the bot has a Telegram Premium subscription." The dev bot's custom emoji was stripped in a classic
  message (test 7) and in a rich one (message 906).
- **What the API kept** (every message silent; only disabled and copy_text buttons, so no tap reaches the bot):

  | Message | Sent | Kept |
  |---|---|---|
  | 904 | the title as a `disabled` button in each look (plain, primary, success, danger) | all four, with their styles |
  | 905 | the same as `copy_text` buttons | all four, with their styles |
  | 906 | a custom emoji in a rich message | stripped to its plain emoji |
  | 907 | classic HTML (`\n` breaks, a blank line, a quote) sent as rich HTML | the breaks read as spaces, the two paragraphs ran into one |
  | 908 | the same with `<p>` and `<br>` | the line breaks and the paragraphs kept |
  | 909 | `editMessageText` of a classic message with `rich_message` | the message became rich, its keyboard kept |
  | 910 | a rich message edited back to classic text, then to rich again | both edits taken, the keyboard kept |

- **Clients:** Telegram Desktop shows buttons inside a rich message from 7.1 (2026-08-21; its changelog: "Add buttons and file
  blocks to rich messages"); the current release is 7.3.1. The user's Desktop is 7.0.5. Topic names are plain text: no
  button can be in one.

#### 21.15.2 The chip, and where it shows

- **The chip** (`TgTitleChipHtml`): `<tg-button type="copy_text" style="primary" text="docs 2">🟦 docs 2</tg-button>`. Its
  label is the §21.13 square and the title (one line, cut at 100 UTF-16 units with an ellipsis); a tap copies the title (up
  to 256 units, Telegram's CopyTextButton limit); both cuts never split a surrogate pair. A blank title has nothing to copy and
  stays the plain mention.
- **The look follows the square's family** (`TgTitleChipStyle`): 🟦 primary, 🟩 success, 🟥 danger. 🟧 🟨 🟪 🟫 ⬛ ⬜ get the
  app's plain button rather than a neighbor's color (an orange tab with a red chip would read as a red tab); the square in the
  label names the family either way. Of the 14 tab palette colors, 8 get a colored chip (4 blue, 2 green, 2 red).
- **Where:** every place §21.13 put a square — the card's title line (`📌 [🟦 docs 2]`), `/status`, a Search answer's rows,
  a pick menu's head and the action edits of that menu, `/rename`, `/summary`, the interrupt confirm, `/idle`, "Opened …", and
  the change line: `[🟦 old] changed the title to [🟩 new]`, `[🟦 tab] changed the color to [🟩 tab]` (in a rich line the
  quotes give way to the chips, and a new color shows as a chip of the title). The topic NAMES keep the square.
- **Only with rich messages:** the cog's rich switch (`AppSettings::messagingRichMessages`, on by default, the switch of
  §21.11) turns the chips on and off with Claude's rich Markdown. Off, or when the Bot API has no rich messages (a 404 latches
  for the run), every title is the §21.13 square and title, as before.

#### 21.15.3 One text, two renders

- **A title is a TOKEN until it is sent.** The builders (`BuildCardText`, `BuildSearchAnswerHtml`, `BuildSearchPickMenuHtml`,
  `BuildTopicChangeNoticeHtml`, and the six replies in Messaging.Inbound.cpp) write `TgTitleToken(form, square, title)`:
  private-use characters U+E000…U+E002 frame a form, the square and the escaped title.
- **The classic render** (`TgRenderTitleTokensClassic`) turns each token into the §21.13 text of its form — Plain
  `🟦 title`, Bold `🟦 <b>title</b>`, Quoted `“🟦 title”`, Square `🟦` — byte for byte what the builders printed before. So a
  card's hash, now taken over this render, is the hash its record already holds: the upgrade itself edits no card.
- **The rich render** (`TgRichHtmlFromClassic`): rich HTML reads a raw line break as a space (message 907), so the converter
  makes the structure explicit — a blank line ends a paragraph (`<p>`), a line break is `<br>`, a quote or a code block is a
  block of its own, every other tag (b, i, u, s, code, a, tg-spoiler — all in the rich tag set) and every entity stays. A chip
  is lifted out of the formatting around it (the tags closed before it and opened again after it; the empty pairs that leaves
  are dropped), so a button never sits inside bold or italic.
- **Sending** (`_sendText`): a text with tokens goes as ONE rich message (`_sendTitledRich`: `sendRichMessage`, the buttons, the
  reply reference and the caller's callback on it) while chips are wanted; Telegram's refusal (a 400 about the message, or a
  404) sends the classic render in its own place in the outbox, without a key (a key would erase a newer queued item with the
  same key). A text without tokens goes the HTML way exactly as before.
- **Editing** (`_editText`): the same for edits (`editMessageText` with `rich_message`, else `text`); a refused rich edit is made
  again with the classic render, except for "message is not modified" and "message to edit not found", which go to the caller.
  The edit reports the form the message shows (`onForm`, from Telegram's answer: `rich_message` or `text`).
- **A belt:** a token that reached the outbox unrendered (a send path that bypasses `_sendText` / `_editText`) is rendered
  classic there and logged once: `[msg] a title token reached the outbox unrendered (…)`.

#### 21.15.4 The cards that exist already

- **The card's form is recorded** (`TopicRecord::cardRich`, persisted as `cardRich`; absent in an older file = a classic card),
  from Telegram's own answer to the post or the edit.
- **A card whose text is unchanged but whose form is not the wanted one** — every card posted before the chips, or every card
  after the rich switch flipped — is re-edited only when `_takeCardFormatSlot` gives it the turn: one card per 10 s across all
  topics (`kCardFormatGapMs`), none for 5 minutes after a 429 or while a long one parks `editMessageText`, and only while the
  last minute's calls leave room for one more under 18 (the history replay's budget). 47 cards edited at once earned a 429
  with a 21-minute `retry_after` on 2026-10-16. Each such edit logs `[msg] <sid8> card re-edited with a title chip (its form
  only; N more card(s) to go)`.
- **A card whose text changes** (a state, the queue, a rename) is edited in the wanted form at once, and a re-posted card
  (the sticky card, a ping) is posted in it — the upgrade rides the ordinary traffic too.
- **A refused rich form is not retried** for the same text (`CardState::richRefusedHash`): a card that went as HTML waits for
  its next text change before the rich form is tried again.

#### 21.15.5 Tests, the build, what is open

- **Tests:** `TestMessaging` — the look of every square and the 8-of-14 palette count; the token forms and their classic
  renders (a framing character inside a title and a stray one dropped); the chip (the button of message 905, the plain look,
  the attribute escaping, one line, the label and copy cuts, a surrogate pair at the copy cut, a blank title); the converter
  (the card's shape, a chip in bold, both change lines, a quote and a blank line inside italic, code, a link with entities,
  empty input); the card's two renders; `cardRich`'s round trip and an older file. The §21.13 checks now compare the classic
  render, which proves it byte-identical to before. `TestMessagingSearch` — the rich answer and pick menu. The isolated m5
  run (HEAD + this work): 7,512 checks, only the 3 known TestTaskbarBadge failures. `/Zs /W4` of Messaging.cpp +
  Messaging.Inbound.cpp: clean.
- **The build**: Debug, HEAD 7017fd863, into `Q:\agentmaster-layouts\Debug-B` while dev ran from `Debug`, from a session
  inside production (`devbuild.ps1`, this session's scratchpad `swap\`); no purge trigger since dev's 4f3c643d9, so an
  incremental build: 447 s with 3 compilers, "Build OK", TerminalApp.dll bakes 7017fd863, 192 icons. The script's last check
  refused the layout because its manifest read `0.0.1.23` — the stamp of that slot's previous swap, which the incremental
  package step carried over — instead of the unstamped `0.0.1.0`. The manifest was identical to the running one apart from
  the version, the packaging files had not changed since 3a73980d9, and the swap stamps its own version, so the swap went on.
- **The swap** (18:25:24, `Swap-AgentmasterLayout.ps1 -PackageName AgentmasterDev`, no open session): 0.0.1.24 → 0.0.1.25,
  drain 1.1 s, register 18.9 s at the first attempt, **downtime 20.6 s**; `[msg] connected as @ClaudeSessionsDebugGateBot`
  1 s after the start.
- **Checked live** (dev hooks.log and `messaging\telegram\state.json` after the swap): two cards took the slow form upgrade
  (`898e19f8` at 18:26:04, "47 more card(s) to go"; `f48cd627` at 18:26:14, "38 more"); every other card became rich through
  the edits and re-posts a restart makes anyway. By 18:31 all **49** topics' cards were rich (`cardRich` true, a card id
  each). The first 10-minute census: `editMessageText=121 sendMessage=1 setMyCommands=1 (123 calls) edits(card=121)` — the
  dev restart before it, without chips (17:24), made `editMessageText=102 … edits(card=102)`. No rich message refused, no 429,
  no unrendered token, no swallowed exception.
- **Open** (the user's, live): the chips on the phone (the looks, a tap copying the title); Telegram Desktop needs 7.1 or
  later to show them; a title Telegram's entity detection might find something in (a URL, a /command) inside a button label —
  if Telegram refuses such a message, it goes as HTML with the square (logged).

### 21.16 The pinned directory — the dedicated topics and every tag on an open tab with its tabs, in one message of the main view (2026-10-17)

> Status: written 2026-10-17 (session 0b1f8635, commit 00b4bc3aa); LIVE on dev 0.0.1.26 since 20:40
> (`Q:\agentmaster-layouts\Debug`, the build of 19c6b7d1b); not in production (0.6.8.21 is the build of 46e3aad42) — a
> production build and swap need the user's word. The 0-tab rule (§21.16.1: a tag no open tab carries is not listed) was
> written the same evening, after the user saw the first directory; NOT BUILT yet.

**The user's request** (2026-10-17): "I want a pinned message per bot, can we pin a message with a nice list of dedicated
topics, out of which we right now have Search but will more in future. Also a pin all tags and the tabs in the tag. make sure
the message shows titles with coloring and when clicked jumps to the topic. This way we have builtin + tags of agentmaster
covered. Also a two rows of stats "5 running 15 waiting 300 idle" and second row you decide". Once the first directory was
up (it listed six of dev's seven tags as "no open tab"): "Tags with 0 tabs should not be shown".

#### 21.16.1 What it shows

One rich message (§21.11) in the chat's main view, "All" — the General topic of a bot forum, where a message with no
`message_thread_id` lands (`api/forum`: "topic-less messages … end up in the "General" topic"):

```
🔵 5 running · 🟡 15 waiting · ⚪ 300 idle
🟠 2 need approval · 🔴 1 error · 🟢 3 done · ⏳ 12 queued
🧭 Dedicated topics
[🔎 Search]
🏷️ Tags
🟨 release · 3 tabs
🔵 [🟦 Agentmaster]
🟡 [🟩 docs]
⚪ [🟦 an idle tab]          a chip that opens nothing: the tab has no topic
updated 5 minutes ago
```

(A tag that no open tab carries — `research` above, say, carried only by closed sessions — has no line at all.)

- **The stats come first**, because a pinned message's bar shows the start of its text: the counts read at the top of the chat.
  Row 1 is the user's own example. Row 2 (the user left it to me) holds the states row 1 leaves out — Needs approval, Error,
  Done (claude exited) —, the Pending prompts of those tabs (the Prompt Queue Runner's backlog), and `· ⏸ runner paused` while
  the Manager's global pause is on. Counted over the Claude sessions in a tab, the population of the 🔎 Search topic's
  `/board` (Codex tabs have no topic, D23). Every number is bold; "1 needs approval" / "2 need approval", "1 error" / "2 errors".
- **Dedicated topics:** 🔎 Search today, as a chip in the look of its icon color (blue: primary); a chip that opens nothing
  until the topic is made. The list is `_directoryTopics()`: a new dedicated topic adds one entry there (label, thread, icon
  color), and the directory lists it from the next check on.
- **Tags:** every tag of Agentmaster that at least one OPEN tab carries. The model joins the Tags panel's universe — the
  tags any session carries, an archived one's too, and the known-tag registry's 0-carrier ones (`CollectGlobalTags` over
  `LoadAllSessionTags` and `LoadKnownTags`, which also picks each tag's display casing) — with the open tabs (`JoinDirectoryTags`),
  and the render leaves out every tag with no open tab: one only closed sessions carry, or a 0-carrier tag the registry keeps
  for re-use (Rule #17) — the user's "Tags with 0 tabs should not be shown". Such a tag changes neither the message nor its
  hash, so adding, renaming, recoloring or deleting it never edits the directory; its first open tab makes it appear.
  `DirectoryRender::tagsWithoutTabs` counts the left-out tags for the log. The listed tags go in case-insensitive order of
  their names, each head in bold with its color square (`ResolveTagDisplayHex`: the picked color, else the palette's,
  §21.16.5) and its count of open tabs. Under it, the OPEN tabs that carry it (live in a tab, any agent) in case-insensitive
  order of their titles: the state glyph of the topic names (§21.11), then the tab's chip — a URL button to its topic, in
  the look of the tab color's square (§21.15: 🟦 primary, 🟩 success, 🟥 danger, else the plain button), the square and the
  title in its label. A tab with no topic (Idle and not pinned, a Codex tab) gets a DISABLED chip in the same look: the title
  shows, a tap does nothing. Orders that do not move while the fleet works, so the content changes only when a count, a
  state, a title or a listed tag does. When no tag is listed (no tag at all, or none on an open tab — one line for both, so
  a tag nobody sees cannot change it): "No open tab carries a tag. A tab's right-click menu ▸ Tags adds one."
- **The footer:** `updated <tg-time … format="r">19:04</tg-time>` — a date-time entity in the RELATIVE format, which every app
  renders and keeps current by itself ("5 minutes ago"); the clock time inside is shown by an app that does not know the
  entity. So the clock never costs an edit.
- **Limits:** a tag lists at most 20 tabs (`… and 4 more`); the rich render stays under 32,000 characters and the classic one
  under 3,800 + its tail, so it is ONE message; tags that do not fit are counted in one line (`… and 3 more tags (the message is
  full)`); at most 100 tag sections (a rich message takes 500 blocks).
- **The classic fallback** (rich messages off, or Telegram refused the rich one): the same text, each chip a link
  (`<a href="…">🟦 Agentmaster</a>`), a tab without a topic its bare label.

#### 21.16.2 Where it lives, and the pin

- **Posted once** in the main view (no thread), silent, at the Backfill priority, keyed `dir:post`; then **pinned**
  (`pinChatMessage`, silent, keyed `dir:pin`) — in a private chat every non-service message can be pinned, and a pin never
  notifies there. A refused pin is logged and tried again at the next start.
- **The pin's note** ("… pinned «🔵 5 running …»") comes back through `getUpdates` as the bot's own service message; when it
  names the directory it is deleted, so it never sits above every new message of the main view. A pin the user makes leaves no
  bot note.
- **Persisted** in `state.json`: `directoryMessageId`, `directoryHash` (what it shows), `directoryRich` (its form),
  `directoryPinned`. A state file of another bot resets them with the rest (the message is in the old bot's chat).
- **The first start of a build with it** (dev and production alike) posts and pins one directory; nothing else migrates.

#### 21.16.3 What a tap does (read from the clients' sources; nothing tried on a device)

- **The link:** `https://t.me/<bot>/<topic>?topic=<topic>` — the deep-link spec's topic form ("the topic ID is actually the ID
  of the service message that created the topic, so whenever the client resolves a message link that points to a
  messageActionTopicCreate service message, it should open the topic") plus the `topic` parameter Telegram Desktop's link code
  reads (§5.1). The topic id is the user-space id the Bot API hands out, so the number is right.
- **Telegram Desktop** opens the topic: `t.me/<bot>/<id>?topic=<id>` becomes `tg://resolve?domain=<bot>&post=<id>&topic=<id>`,
  and for a non-channel peer the `post` is ignored (`showPeerByLinkResolved` opens it at the unread message). ⚠ Buttons inside a
  rich message need Desktop 7.1 or later (§21.15.1); the user's Desktop is 7.0.5 (last started 2026-09-25).
- ⚠ **Telegram Android does NOTHING** for a link to the bot chat that is open already: `LaunchActivity`, for a bot whose chat
  is the current fragment (`isBot && sameDialogId`), only calls `setBotUser(…)` — the message id and the topic are dropped.
  **iOS** reads the link as a plain message link into the chat. So on the phone the directory shows the colors, the states and
  the counts, and its chips do not move the view.
- **What would make the phone jump (not built, the user's call):** a chip that is a callback instead of a link, whose tap makes
  the bot RE-POST that topic's sticky card. The card then is the chat's newest message, so Android's "All" view adds its own
  "Continue last thread ›" button under it, and the topic moves to the top of the topic list. It costs two calls per tap (the
  card's delete and its send), and a callback chip opens nothing on Desktop — one chip cannot be both.

#### 21.16.4 When it changes

- **A check every 15 s** (`kDirectoryCheckMs`) builds the model — the sync's facts, the session store's cached tag reads (one
  directory listing, one stat per small file), one lean registry copy for the queues — and renders it. An EDIT goes only when
  the content hash (`DirectoryRender::hash`, everything but the footer) or the form is not what the message shows, and at
  most once per **2 minutes** (`kDirectoryEditGapMs`): at most ~30 edits an hour on a busy fleet, ~0.5 call a minute of the
  chat's flood budget. A quiet fleet costs no call. The decision is the pure `DecideDirectoryAction`.
- **Not during** the start-up hold or a recreation (the facts are incomplete), before the session store's start-up warm-up is
  done (its first whole read opens every record file), before getMe gave the bot's username, for 5 minutes after a 429, or
  while a long 429 parks `editMessageText`, `sendRichMessage` or `sendMessage`.
- **A refusal** backs off 5 minutes (`kDirectoryRetryMs`); a rich form Telegram refused is not tried again for the same content.
  **The message gone** ("message to edit not found", `MESSAGE_ID_INVALID`, "message can't be edited" — the user deleted it) is
  forgotten, and a new one is posted and pinned **30 minutes** later (`kDirectoryRepostGapMs`), so a deletion is not answered at
  once.
- **One send at a time** (`_directoryInFlight`); a send with no answer for 15 minutes is forgotten (`kDirectoryStallMs`): every
  directory send carries a key, so the next replaces a queued twin, and a post that still lands after it is deleted — the chat
  never holds two directories.
- **Logs:** `[msg] directory: posting it in the chat's main view (N tag(s), M tab chip(s)[, K tag(s) on no open tab not
  listed][, H tag(s) left out: the message is full], rich)`, `[msg] directory posted (message …); pinning it`, `[msg]
  directory pinned (message …)`, the run's first `[msg] directory edited (message …, rich: N tag(s), M tab chip(s)[, …]);
  later edits are logged only when one fails` (the words are `DescribeDirectoryRender`'s, the one place on the PC where the
  content of a sent directory can be read back), and every refusal. The traffic census counts its edits as
  `edits(directory=N)` (`EditKindOfKey`, key `dir:edit`).

#### 21.16.5 One tag palette for the app and the bot (Rule #17)

- The tag palette (12 colors) and the name hash that picks a tag's entry moved from `AgentStatusColors.h` (WinRT, the UI only)
  into the engine: `SessionStore.h` `kTagPaletteRgb`, `TagPaletteIndexFor`, `TagPaletteHex`, and `ResolveTagDisplayHex` (the
  text twin of `ResolveTagDisplayColor`: a picked, well-formed color, else the palette's). `TagColorFor` / `TagPaletteColor`
  build their winrt Colors from it. The table and the hash are unchanged byte for byte, so every tag keeps its color, and the
  directory can never paint a tag differently from its ribbons.

#### 21.16.6 The code, the tests, what is open

- **The code:** `AgentMaster/MessagingDirectory.h` (pure, header-only: the model, `RenderDirectory`, `JoinDirectoryTags`,
  `CountDirectoryStats`, `DirectoryTopicUrl`, `DescribeDirectoryRender`, `DecideDirectoryAction`,
  `DirectoryEditFoundNoMessage`); Messaging.cpp
  (`_directoryTick` after the name tick, `_directoryTopics`, `_onDirectoryPosted`, `_editDirectory`, `_onDirectoryEdited`,
  `_pinDirectory`); Messaging.Inbound.cpp (the pin's note); MessagingModels.h (the four state fields, the census kind).
- **Tests:** `TestMessagingDirectory` (tests_messaging_directory.cpp) — the links; the counts' population; the join (case,
  repeats, a 0-carrier tag, the fold equal to `FoldTagName`); both renders (the user's row 1 at the very start, row 2's words
  and the pause, the Search chip and its disabled form, the tag and tab order, every chip look, a disabled chip, escaping, the
  footer, balanced elements, no title token, one classic message); the hash (the clock out, a count or a state in, the model's
  order out); the limits (20 tabs a tag, 60 big tags against both caps); the decision (post, in flight, back-off, gap, form,
  a refused rich form); the "gone" answers; the state fields' round trip; the census kind; the palette pinned against an
  independent FNV-1a. The isolated m5 run (HEAD + this work): 7,582 checks, only the 3 known TestTaskbarBadge failures.
  `cl /Zs /W4 /WX` of Messaging.cpp, Messaging.Inbound.cpp, SessionStore.cpp and the test: clean. `/Zs` of two app files that
  paint tags from the moved palette (TabHeaderControl.cpp, TerminalPage.AgentObserver.cpp) against the last Debug build's
  PCH: clean, and a negative control (`static_assert(kTagPaletteRgbCount == 13)` appended) failed at that line as it must.
- **The build** (20:27, after the session that was editing `SessionModels.h` in the same tree committed its work as
  19c6b7d1b — a build before that would have shipped its half-done code): Debug, HEAD 19c6b7d1b, into
  `Q:\agentmaster-layouts\Debug` while dev ran from `Debug-B`, from a session inside production (`devbuild.ps1`, this
  session's scratchpad `swap\`); PURGED (`SessionModels.h` changed since dev's 7017fd863), 580 s with 3 compilers, "Build OK",
  TerminalApp.dll bakes 19c6b7d1b, the manifest `AgentmasterDev 0.0.1.0`, 192 icons.
- **The swap** (20:37:51, `Swap-AgentmasterLayout.ps1 -PackageName AgentmasterDev`, one open dev session, Idle): 0.0.1.25 →
  0.0.1.26, drain 1.5 s, register 2.1 s at the first attempt, **downtime 4.5 s**; the window re-homed 57 tabs, none skipped.
- **Checked live** (dev hooks.log, `messaging\telegram\state.json`, `messaging.log`): 20:37:58 connected; 20:39:57 the start-up
  hold ended and, in the same tick, `[msg] directory: posting it in the chat's main view (7 tag(s), 1 tab chip(s), rich)` (dev
  has 7 tags; one open tab carries a tag); 20:40:14 `directory posted (message 916, rich)` — Telegram took a thread-less rich
  message with in-text URL buttons in the bot forum; 20:40:16 `directory pinned (message 916)`. state.json:
  `directoryMessageId` 916, `directoryRich` and `directoryPinned` true. The first census (20:47:58): `pinChatMessage=1
  sendRichMessage=1 deleteMessage=2` — the pin's note and the notice of the one topic rename (`editForumTopic=1`) — no 429,
  and no directory edit (nothing it shows changed). No `[tg]` failure in messaging.log after the swap, no swallowed exception.
- **The 0-tab rule** (the same evening, after the user saw the first directory: "Tags with 0 tabs should not be shown"):
  `RenderDirectory` lists only the tags an open tab carries. The join still holds every tag (it keeps the display casing
  `CollectGlobalTags` picks); the render counts the rest in `tagsWithoutTabs`, and its one empty-state line covers "no tag at
  all" and "no tag on an open tab" alike, so a tag nobody sees changes neither the message nor its hash. The log lines read
  a render through `DescribeDirectoryRender`, and the run's first edit now says what it lists. The first start of a build with
  it EDITS the directory (the hash moves: dev's six tags with no open tab go). Tests: the tag left out of both renders and
  counted; "only 0-tab tags" renders and hashes exactly like "no tag at all"; a 0-tab tag added, renamed or recolored keeps the
  hash, and its first open tab makes it appear; the tag order case-insensitive against an ordinal one ("Zulu" after
  "release"); the log's words. The isolated m5 run (HEAD 19c6b7d1b + this work): 7,652 checks, only the 3 known
  TestTaskbarBadge failures. `cl /Zs` with the last Debug build's flags of Messaging.cpp and Messaging.Inbound.cpp: clean,
  and a negative control (a `static_assert` on `DirectoryRender` appended to a copy of Messaging.cpp) failed as it must.
- **Open:** the look on the user's phone and Desktop (the chips' looks, the disabled chips, the pinned bar's first line, a
  tap); whether a pin in a bot forum's General shows in every topic's pinned bar or only in "All" (not documented); the first
  edit (it comes with the first change of a count, a state or a tag); a production build and swap need the user's word.

## 22. Folder groups — the topics grouped by working folder (2026-10-17; discovery, DESIGN ONLY, nothing built)

> The user's questions (2026-10-17, session 0b1f8635): "Lets discover how can we make forums/groups to: Can you make groups
> optional? meaning we retain the view and access of all topics and optionally can open the groups view? Can two groups
> have same topic? I am interested in having groups per (workdir ?? inferred working directory)."

### 22.1 What Telegram allows (the Bot API reference and changelog up to 10.3 of 2026-08-24, read 2026-10-17)

- **A bot cannot create** a group, a supergroup, a channel, a chat folder or a community, and it cannot turn Topics on in
  a group: no such method exists in the reference or in any changelog entry from 9.0 to 10.3. The USER creates each group
  in the app (up to 50 a day; at most 500 groups and channels joined, 1,000 with Premium).
- **In a forum supergroup where the bot is an admin** with `can_manage_topics`, it does what it does in the private chat
  today: `createForumTopic` / `editForumTopic` / `deleteForumTopic`, messages into a `message_thread_id`. With
  `can_change_info` it also sets the group's title (`setChatTitle`, 1–128 chars) and photo (`setChatPhoto`). As an admin it
  receives every message of the group.
- **Adding the bot takes one tap**: `t.me/<bot>?startgroup=<parameter>&admin=manage_topics+delete_messages+pin_messages+change_info`
  opens a picker of the user's groups and adds the bot as an admin with those rights; the group then gets
  `/start@<bot> <parameter>` (up to 64 base64url characters), so the app learns which folder the group is for. Telegram
  Desktop has a known bug where the parameter is not sent when `admin` is given (tdesktop #25924); Android sends it. The
  other route, a `KeyboardButtonRequestChat` button (`chat_is_forum`, `chat_is_created`, `bot_administrator_rights`),
  grants the rights the same way; whether a client's picker offers "create a new group" is not documented — to try.
- **A topic belongs to exactly one chat** (`message_thread_id` is per chat). There is no topic shared between chats and no
  link between two topics.
- **Communities** (the Telegram update of 2026-07-14; Bot API 10.2 / 10.3: the `Community` class, `community` on
  `ChatFullInfo`, the `community_chat_added` / `_removed` / `_joined` service messages — read-only for bots): "several
  supergroups, channels, and bots linked together". The user makes one from a chat's profile ("Add Group / Bot to a
  Community") and adds chats to it (bots can be members); the community "can appear as one expandable item in the chat
  list" or as separate chats (the blog post; guides call the switch "Show as One Chat"). No limit on its chats is
  published.
- **Managed bots** (9.6: a bot asks the user to create a bot it controls, `t.me/newbot/<manager>/<username>`): a bot per
  folder would give each folder a private chat with topics, but every such bot needs its own @BotFather topics switch and
  its own poller, and an account owns at most 20 bots (40 with Premium), two of them taken already. Not a fit for 23+
  folders.
- **Flood budgets are per chat**: ~20 messages a minute in one group and ~30 per second for the whole bot
  (limits.tginfo.me). A second chat does not slow the first.
- **Inside one chat** the only grouping aids are the topic NAME (128 chars), the topic ICON (a custom emoji from
  `getForumTopicIconStickers`, editable) and the icon COLOR (one of 6 values, set at creation only).

### 22.2 The three answers

- **Optional, with every topic kept: yes.** The private chat with the bot stays the all-topics view, unchanged, and each
  session ALSO gets a topic in its folder's group (a mirror). The groups view is then the user's choice in Telegram: open a
  group, or put the bot and the groups into one community and fold it into one chat-list item that opens into the
  all-topics chat and one group per folder.
- **Two groups with the same topic: as copies, yes; as one object, no.** Each chat gets its own topic for the session. A
  message typed in any copy is a prompt to the same session; statuses, the card and the history are kept per copy. A copy
  costs its own calls, but on its chat's own budget: the private chat does not get slower, and the groups replay their
  history side by side (23 groups × ~18 messages a minute ≈ 7 calls a second, under the bot's ~30). One copy in the
  private chat plus one in the folder group is the proposal; a session in two folder groups (launch and inferred) is
  possible, not proposed.
- **Which folder** — measured on the release profile on 2026-10-17 (`tgresearch\groupkeys.py`, `inferkeys.py` in session
  0b1f8635's scratchpad; the 413 tabs that hold a topic, of 668):

  | Key | Groups | Notes |
  |---|---|---|
  | Launch folder (`SessionInfo::workingDir`) | 25 (30 for all 668 tabs) | NumSharp 100 topics, Agentmaster 77, TraderLab-options 62, BetterClipboard 29, OptunaSharp 22, idleon 19; 3 groups with one topic |
  | Launch folder, worktrees folded into their repo (the tab colors' `SessionColorKeyDir` in this install's mode) | **23** (27) | 1 group with one topic |
  | Inferred folder (the app's `InferWorkingDirectory`, run offline: this install colors tabs per launch folder, so nothing is inferred) | 49 | 88 topics (21%) in another group than their launch folder — some right (a worktree's sessions to their repo, sisyphusresearch sessions that work in OptunaSharp, options sessions on `Q:\AlphaVantage`), 33 in `C:\Users\ELI\.claude` (sessions that mostly read skills and memory), a few in `C:\Windows` / `K:\source`; ~38% of the sessions change their inferred folder at least once over their path list |

  A topic cannot move between chats: a session whose key changes would be deleted, re-created and replayed in the other
  group. **Proposed key: the launch folder with worktrees folded into their repo**, fixed when the topic is made, with a
  `/group <folder>` command to move one by hand. The inferred folder only later, opt-in, and frozen once the session has
  settled.

### 22.3 The design (a proposal)

- **A setting**: Folder groups **Off** (today) / **Mirror** (the private chat keeps every topic, a copy goes to the folder's
  group) / **Move** (folder groups only; the private chat keeps the 🔎 Search topic and a folder index).
- **Setup, per folder** (the bot cannot create groups): a 📂 list in the private chat names the folders that hold topics,
  each with a link that carries a one-time key (`startgroup` + `admin`). The user creates a group, turns Topics on, taps
  the link and picks the group. The bot binds the group to the folder (`/start <key>`), renames it to the folder's name,
  sets the folder's tab color as its photo, and fills it: topics in MOST RECENT order, history, cards. Optionally the user
  makes an "Agentmaster" community of the bot and the groups. A folder that loses its last topic keeps its group (the app
  never leaves a group by itself).
- **Inbound**: route each update by chat id to its destination, then by thread to its session; the owner check as today.
- **Code**: the one chat is hard-wired today (`chat_id = _ownerId` at ~16 send sites, `MessagingState::topics` one record
  per session, one flood window). Needed: destinations (per-chat topic maps, pacing, cards, history pumps, re-sorts and
  glyph renames), the bind handshake, group title and photo, the setting and its persistence (keyed by bot id and chat id),
  tests. Not small.

### 22.4 Cheaper, inside the one chat (no groups)

- The folder's name in every topic name (`🟡 NumSharp · fix the GIL`) and a per-folder topic icon (a custom emoji,
  editable; the creation color one of the 6, the nearest to the folder's tab color): the topics of a folder look alike and
  the topic list's search finds them by name (to check in the apps). No setup; one rename per topic.

### 22.5 Open decisions (the user's)

- **G1** Mirror or Move. **G2** the key: launch folder with worktrees folded (proposed), launch folder exact, or inferred.
  **G3** which folders get a group: every folder with a topic, or only the groups the user makes. **G4** a group's content:
  like the private chat (verbose / conversation-only per topic), or conversation-only always. **G5** §22.4 first, or with
  the groups.

Sources: [the Bot API](https://core.telegram.org/bots/api), [its changelog](https://core.telegram.org/bots/api-changelog),
[deep links](https://core.telegram.org/api/links), [bot features](https://core.telegram.org/bots/features), [request
buttons](https://core.telegram.org/api/bots/buttons), [the Communities post](https://telegram.org/blog/communities-editor-invisible-messages),
[a Communities guide](https://blog.invitemember.com/telegram-communities/), [Telegram limits](https://limits.tginfo.me/en),
[tdesktop #25924](https://github.com/telegramdesktop/tdesktop/issues/25924).
