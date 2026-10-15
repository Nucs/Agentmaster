# Agentmaster — messaging: the fleet in Telegram, Microsoft Teams, and any other messenger

> **Status: DESIGN ONLY (2026-10-15). Nothing here is built.** The user's ask: "We plan messaging apps
> abstract support for Telegram + Microsoft Teams. We will support anything." Every platform fact was
> checked on 2026-10-15 in the platform's own documentation (Telegram's and Claude Code's pages read in
> full; Microsoft's through searches of its pages), unless the line names a third-party source or says
> "from memory" (sources in §20). Every name from Agentmaster's own code exists in the tree today. The
> decisions that are the user's: §18. What must be measured before any code: §19.

## 0. Summary

**What it is.** Agentmaster tells you, in a messenger you already carry, when a session needs you, and
lets you act from there: reply to a session, answer its question, approve its plan, queue its next
prompt, pause or resume its runner, stop its turn, resume a closed session, start a new one. **One bot
serves the whole fleet** — every session, every window, every folder — and every message is about ONE
session, in that session's own place in the chat.

**The picture.**

```
claude.exe ×N  ⇄ ConPTY ⇄  Agentmaster.exe (the GUI process, one per profile)
                            engine: registry · scanner · scheduler · observer · unread census
                            └─ FleetLink (new, §4–§8): fleet events out · intents in · redaction · authorization
                                    ║  named pipe, length-prefixed JSON (the session database's framing, §4.3)
                           agentmaster-messaging.exe (new, one per profile, §4)
                            ├─ outbox · conversation map · pairing · action tokens · renderers
                            └─ connectors: Telegram · Teams · Webhook · External (JSON-RPC over stdio, §12)
                                    ║  outbound HTTPS only (+ a tunnel for the platforms that only push, §11.4)
                           Telegram Bot API · Azure Bot Service (Teams) · Workflows (Teams) · Slack · Discord · …
```

**Three facts shape the design (verified 2026-10-15):**

1. **Telegram needs no public address.** A bot reads its messages by long polling (`getUpdates`).
   Since Bot API 9.3–9.5 (December 2025 – March 2026) a bot's private chat can hold **topics** — one per
   session — and **stream a draft** of a reply; Bot API 10.3 (2026-08-24) adds a **stop button** on that
   draft. Only one reader may poll a bot, so Agentmaster needs a bot of its own — not the homelab's
   `@balash_fastrun_bot`, and one each for the release and the dev install.
2. **Teams has no two-way path without a public HTTPS endpoint.** A Teams bot receives through Azure Bot
   Service, which POSTs every message to the bot's messaging endpoint. Microsoft's Teams API rules allow
   an app to poll a chat **once a day**; a message you post to yourself (Graph's `48:notes` self chat)
   never alerts your phone. One-way alerts need no endpoint: a **Workflows** webhook posts cards into a
   chat (the Office 365 connectors were retired 18–22 May 2026).
3. **Claude Code has its own remote features, and this plan is built around them, not over them.**
   *Remote Control* puts ONE session into the Claude app and pushes its questions and permission prompts
   — claude.ai logins only, so never for a preset session (OpenRouter, Kimi, …) or a Codex session.
   *Channels* (a research preview) are MCP servers that push messages into ONE session; a custom channel
   must start with a development flag that asks for a confirmation at every launch. Agentmaster's layer is
   fleet-wide, built on Agentmaster's own session states, queue and lifecycle; version 1 uses neither,
   and keeps the delivery into a session pluggable so channels can take it over later (§2.3).

**The abstraction (§5).** A **provider** (Telegram, Teams Lite, Teams Bot, Webhook, External) declares
what it can do (buttons, topics, edits, drafts, inputs inside a message, lengths, rates). A
**connection** is one configured account and chat, with an owner, a permission level, an event set, a
privacy level, an away rule and a routing filter. A **binding** ties a session to a place in that chat
(a topic, a thread, a reply chain, a card, or an explicit `#3f9a`). Events out and intents in are
provider-neutral; one pure renderer turns them into each provider's format, degrading gracefully
(no buttons → numbered choices; no topics → reply chains; no edits → a new short message). Anything that
can carry text can join: the built-in connectors are C++ inside the service, every other platform is a
process that speaks a small JSON-RPC contract over stdio (§12).

**The recommended build order (§17):** P0 measurements → P1 Telegram alerts → P2 Telegram two-way → P3
questions, plans and stop → P4 Teams alerts (Workflows) + generic webhooks → P5 Teams bot → P6 external
connectors → P7 live view, digests, the concierge.

## 1. Goals and non-goals

**Goals**

- **G1 — Know.** A session that needs you reaches your phone within seconds when you are away, and does
  not when you are at the PC looking at it. Agentmaster's states decide (hooks + scanner + observer), not a
  second copy of them.
- **G2 — Act.** From the chat: reply, queue, answer, approve, stop, pause, resume, open, start — every
  action through the same seams the app's own buttons use, with the same safety (the delivery gate, the
  draft swap, the verified press, the runner's holds).
- **G3 — Look.** The board, one session's card, its last turns, its screen, a search — on demand.
- **G4 — Any messenger.** Telegram and Teams first; Slack, Discord, Matrix, Signal, WhatsApp, email,
  ntfy and the rest through the same contract, built in or as a separate process.
- **G5 — Safe.** Only the owner can drive. A stranger, a group member, a stale button, a replayed message
  and a leaked secret are each handled by a rule (§9, §16). Managed sessions run with
  `--dangerously-skip-permissions` by default: a remote prompt is remote code execution on this PC, and
  the design treats it so.
- **G6 — One place per session.** In the chat, each session has its own thread (a Telegram topic, a
  Teams card or channel thread), named with the session's ONE title (Rule #11) and colored after its tab.

**Non-goals (version 1)**

- **N1** Sharing a fleet with other people (roles, a team bot). One owner per connection; a group forum
  works only as the owner's own room (§10.2).
- **N2** Mirroring the terminal live (a screen stream). `screen` returns the visible text on demand; the
  live view (P7) streams the conversation, not pixels.
- **N3** Replacing Remote Control. A user on a claude.ai plan can keep it for in-session work; §2.2 says
  what Agentmaster adds and the one setting it shares.
- **N4** A cloud service run by us. Everything runs on the user's PC; the only optional third party is
  the user's own tunnel or relay (§11.4).
- **N5** Voice, photos and files from the phone into a session (P7, §7.7).

## 2. What exists already

### 2.1 In Agentmaster — what this design reuses

| Need | Existing piece | Where |
|---|---|---|
| Notice a session left Running | Registry observers (`AddObserver`), the toast edge rules and holds (`ShouldHoldCompletionToast`, `DecideHeldToast`, `ShouldSuppressDuplicateToast`, `kNotifyExternalHoldCapMs` 30 s, `kNotifyDuplicateToastMs` 20 s) | `SessionRegistry.h`, `SessionScanner.h`; NOTIFICATIONS.md §3, §6 |
| Know you have not looked | The process-wide unread census (`PublishWindowUnreadSessions`, the flash-ring set ∪ Mark Unread) | `Engine.h`; NOTIFICATIONS.md §8 |
| Say what just finished | `LastDeliveredPrompt` + `PromptPreviewLine` (the toast's line 3); the turns renderer for the final reply | `SessionModels.h`, `TranscriptTurns.{h,cpp}` |
| Send a prompt safely | `SessionRegistry::SubmitPrompt` — the ONE send seam: delivery gate, draft swap, verified placement, Enter-retry, lost-send verdict, invisible-character sanitize | DELIVERY.md, PENDING_INPUT.md §9 |
| Queue and run | The Prompt Queue + `Scheduler::DecideAdvance` (dormant gate, settle hold, question guard, global pause, interrupt hold) | DELIVERY.md §14–§17 |
| Read the box or a menu | `ControlCore::ReadPendingInputDraft`, `IsMenuOptionCaret`, `pendingBoxState` (`MenuOpen`) | PENDING_INPUT.md §2, §12 |
| Act in the window that hosts a tab | Per-window sinks: activate, activate-all-dormant, restart, close, deactivate, command-action | `Engine.h` |
| Keep a secret | Windows Credential Manager (`ReadPresetProviderKey` / `WritePresetProviderKey` / `DeletePresetProviderKey`) | `LaunchPresetsOs.h` |
| Talk HTTPS | WinHTTP with bounded timeouts (`HttpsGet` — GET only today) | `Updater.h` |
| Parse JSON | nlohmann-json (vcpkg, `terminal` feature), `Json.h` | |
| Run a per-profile service | `agentmaster-sessiondb.exe`: a per-profile mutex, a named pipe with 4-byte LE length + UTF-8 JSON frames, copied into the profile and started from there (so it never pins the package during a hot swap), idle exit | SESSIONS_DB.md §4, §16; `SessionDbProtocol.h` |
| Read verbs | The CLI's `show` / `sessions` readers; the session database's `search` | CLI.md, SESSIONS_DB.md |
| A hosted model | The agent harness (`HarnessConversation`) | SDK_HARNESS.md |
| Routing keys | Bookmark tags, favorites, `EffectiveWorkingDir` | `SessionStore.h`, `Persistence.h` |
| Logs and failures | `AppendStateLog`, `[nav]`, Rule #18 (`LogSwallowedException`) | CLAUDE.md |

### 2.2 In Claude Code: Remote Control (verified 2026-10-15)

- **What it is:** `claude remote-control` (a server for many sessions), `claude --remote-control` or
  `/remote-control` connect a LOCAL session to claude.ai/code and the Claude iOS / Android app. Work runs
  on the PC; the phone is a window into it. `remoteControlAtStartup: true` in the user's settings connects
  every interactive session; "each interactive Claude Code process registers one remote session".
- **Pushes:** "Push when actions required" (permission prompts and questions) and "Push when Claude
  decides". Skipped while you type in or focus the connected terminal; `CLAUDE_CLIENT_PRESENCE_FILE`
  names a marker file — "notifications are skipped while the file exists".
- **Limits that matter here:** Pro / Max / Team / Enterprise, no API keys; unavailable when
  `ANTHROPIC_BASE_URL` points anywhere but `api.anthropic.com` (so never for an Agentmaster PRESET
  session, HARNESSES.md) and never for Codex; while connected, the session transcript is stored on
  Anthropic's servers.
- **What it does not do:** fleet triage (sessions are separate entries), Agentmaster's states (a lost
  send, a paused runner, a cut-off turn), the Prompt Queue and runner, Agentmaster's lifecycle (resume a
  closed session into a tab, start one in a folder), and any messenger but the Claude app.
- **What Agentmaster can share with it at almost no cost (D17):** own the presence file. Agentmaster's
  away detector (§6.3) knows when the user is at the PC; launching managed sessions with
  `CLAUDE_CLIENT_PRESENCE_FILE=<profile>\messaging\present.marker` lets Remote Control's own pushes follow
  the same rule.

### 2.3 In Claude Code: channels (verified 2026-10-15)

- **What a channel is:** an MCP server that claude spawns over stdio. It declares
  `capabilities.experimental['claude/channel']` and emits `notifications/claude/channel` with `content`
  and `meta`; the model sees `<channel source="…" …>content</channel>`. "Events queue into the session and
  are processed in order"; while claude is busy they are "delivered together on the next turn". There is
  no acknowledgement. A two-way channel exposes a reply tool; the terminal shows the tool call, the text
  goes to the other platform.
- **Permission relay:** a channel that also declares `claude/channel/permission` receives
  `notifications/claude/channel/permission_request` (`request_id` of five letters, `tool_name`,
  `description`, `input_preview`) and answers `notifications/claude/channel/permission`
  (`request_id`, `behavior: allow|deny`). Only tool approvals relay; trust and MCP-consent dialogs do not.
- **Research-preview limits:** `--channels` accepts only plugins on Anthropic's allowlist (or an
  organization's `allowedChannelPlugins`); a custom channel needs
  `--dangerously-load-development-channels server:<name>`, which runs only in an interactive session
  "where Claude Code can show the confirmation prompt", and is ignored under `-p` and the Agent SDK.
  claude.ai or Console authentication only.
- **Why version 1 does not deliver through channels:** (1) the development flag's confirmation would be
  an unanswerable startup modal in every managed launch (the Gotcha "An unanswerable startup modal is
  invisible-by-design"); (2) a channel event is not a user prompt — whether it fires `UserPromptSubmit`,
  how the transcript records it and how Agentmaster's state machine and queue see it are unmeasured;
  (3) preset sessions are excluded by the authentication rule.
- **Why the door stays open (D16):** a channel would deliver a remote message without touching the input
  box (no draft swap, no eaten Enter) and would relay permission prompts natively. FleetLink therefore
  keeps the delivery into a session behind one interface (`DeliveryRoute`: `Keystrokes` today, `Channel`
  later), decided per session.

### 2.4 Prior art (seen 2026-10-15)

- **One topic per agent session in a Telegram bot's private chat** is already the pattern of other agent
  frameworks (OpenClaw, Hermes Agent), whose documentation names the BotFather switch (§10.1).
- **OpenClaw's Teams connector** has issues that report Teams streaming failures (a final chunk rejected
  and the answer delivered twice; messages lost around tool calls) — one more reason this design updates
  Teams by editing a message instead of streaming (§11.7).
- **matterbridge** bridges ~20 platforms behind a REST API, but the original has had no release since
  v1.26.0 (2023-01-29) and its community fork has published none: not a dependency.
- **Apprise** (outbound notifications to many services) can be wrapped as an external connector for
  alert-only platforms (not re-checked today).

## 3. The platforms

### 3.1 Telegram (Bot API 10.3, 2026-08-24)

| Fact | Detail | Use here |
|---|---|---|
| Receiving | `getUpdates` long polling: `timeout` seconds, `offset` confirms everything below it, `limit` 1–100, `allowed_updates`; "will not work if an outgoing webhook is set up" | The service's poll loop (§10.3) |
| One poller per bot | A second concurrent `getUpdates` gets `409 Conflict: terminated by other getUpdates request` (known behavior; not on the API page) | One bot per install, a per-token lock (§10.3) |
| Topics in a private chat | 9.3 (2025-12-31): `has_topics_enabled` in `getMe`, `message_thread_id` for private chats; 9.4 (2026-02-09): `createForumTopic` in private chats, `allows_users_to_create_topics` | One topic per session (§10.2) |
| Turning topics on | BotFather **Mini App** → My bots → the bot → Bot Settings → Threads Settings → **Threaded Mode** (third-party docs; the classic `/mybots` menu does not have it); when off, `createForumTopic` fails "the chat is not a forum" | The setup check (§10.1) |
| Topic fields | Name 1–128 characters; `icon_color` one of six (0x6FB9F0, 0xFFD67E, 0xCB86DB, 0x8EEE98, 0xFF93B2, 0xFB6F5F); `editForumTopic` (name, icon emoji) and `deleteForumTopic` work in private chats; `closeForumTopic` is for supergroups | Topic name = the session title; color = the nearest of the six to the session's tab color |
| Drafts | `sendMessageDraft` (9.3; every bot since 9.5, 2026-03-01): private chats only, `draft_id`, text 0–4,096 ("empty = Thinking…"), "an ephemeral 30-second preview", finalize with `sendMessage`; 10.3: `can_stop`, `keep_on_stop`, the `stopped_message_generation` update | The live view (§6.7), stop (§7.4) |
| Rich messages | 10.1 (2026-06-11): `sendRichMessage` / `sendRichMessageDraft` with blocks (tables, details, lists, preformatted); 10.3: buttons inside rich messages, expandable quotations, document blocks | Plans and long replies, later |
| Buttons | `callback_data` 1–64 bytes; `style` `danger` / `success` / `primary` (9.4); copy-text buttons (7.11); disabled buttons (10.3) | Action tokens (§5.7) |
| Text | `sendMessage` 1–4,096 characters after entity parsing; HTML mode escapes only `<` `>` `&` (and `&quot;`); `pre` + `code class="language-…"`, `blockquote expandable`, `tg-time unix=… format=…` | The renderer (§10.4) |
| Files | Upload 50 MB (photos 10 MB); download 20 MB | Transcripts as `.md` documents |
| Pinning | In private chats "all non-service messages can be pinned" | The board message (§6.5) |
| Limits (Bots FAQ) | One message per second per chat (short bursts, then 429); 20 per minute in a group; ~30 per second overall; a 429 carries `retry_after` | The outbox's rate limiter |
| Privacy | Bot chats are cloud chats: Telegram's servers see the content (Telegram's design) | §9.6 |

### 3.2 Microsoft Teams

| Fact | Detail | Use here |
|---|---|---|
| Bot SDKs | The Bot Framework SDK went out of support 2025-12-31 and was archived 2026-01-05; the successors are the Microsoft 365 Agents SDK and the **Teams SDK** (2.1, 2026-09-28) | Agentmaster talks the REST protocol itself (§11.5–§11.6); no SDK needed |
| How a bot receives | Teams → Azure Bot Service → `POST https://<public host>/api/messages`, a JWT in `Authorization`; "the endpoint must be publicly reachable" | §11.4 |
| New bot registrations | Single-tenant or user-assigned managed identity since 2025-07-31; multi-tenant creation is deprecated | A single-tenant bot in the user's own tenant |
| Who can use a custom bot | Work or school tenants only; the user needs **Upload custom apps** (an admin policy); since 2024-01-31 a bot must be part of a Teams app | The tenant check (§19, D11) |
| Polling | "Your app can poll a resource to see whether it has changed only once per day"; real-time = change notifications (a webhook again) | No Graph-polling design |
| Messages to yourself | Graph `POST /chats/48:notes/messages` (undocumented id, delegated `ChatMessage.Send`) posts as YOU — no alert; personal Microsoft accounts not supported; "It is a violation of the terms of use to use Microsoft Teams as a log file. Only send messages that people will read." | Rejected; the volume rule applies to every route |
| One-way alerts | **Workflows** ("When a Teams webhook request is received" → post a card to a chat or channel); appears as the Workflows bot; Adaptive Cards; buttons only with workflow plumbing. Office 365 connectors retired 2026-05-18…22 (MC1181996) | Teams Lite (§11.2) |
| Rate limits per bot per conversation | 7 / 1 s, 8 / 2 s, 60 / 30 s, 1,800 / 3,600 s for sends; 50 requests per second per app per tenant | The outbox's limiter |
| Message size | ~100 KB counted in UTF-16, keep ≤ 80 KB; 413 `MessageSizeTooBig` | The renderer's budget |
| Streaming | 1:1 chats only; ≤ 1 request per second; each chunk repeats all previous text; a stream lives 2 minutes (then 403 `ContentStreamNotAllowed`); edits have no time limit | The live view edits a message instead (§11.7) |

### 3.3 Everything else: the transport rule

A platform where the program **dials out** (long poll, WebSocket, pull) works from a PC behind NAT with
nothing else. A platform that only **pushes in** (webhooks) needs a public endpoint (§11.4). Outbound-only
platforms are alert channels.

| Platform | Inbound | Outbound | Notes | Checked |
|---|---|---|---|---|
| Telegram | dial-out (`getUpdates`) | Bot API | topics, drafts, buttons | 2026-10-15 |
| Teams Bot | push-in (Bot Service) | Bot Connector REST | needs tenant permission + endpoint | 2026-10-15 |
| Teams Lite | none | Workflows webhook | alerts only | 2026-10-15 |
| Slack | dial-out (Socket Mode: `apps.connections.open` → WebSocket, ack in 3 s, ≤ 10 sockets) | Web API (`xoxb-`) | Socket Mode apps cannot be in the Marketplace | 2026-10-15 |
| Discord | dial-out (Gateway WebSocket) | REST | Message Content is a privileged intent (a toggle under 100 servers) | 2026-10-15 |
| Matrix | dial-out (`/sync` long poll) | client-server API | bridges reach WhatsApp / Signal / others | from memory |
| Signal | dial-out (signal-cli JSON-RPC daemon, local) | signal-cli | unofficial; a phone number or a linked device | from memory |
| WhatsApp (Cloud API) | push-in (webhooks) | Graph API | business account; template rules outside the 24 h window | from memory |
| Google Chat | dial-out possible (Cloud Pub/Sub pull) | Chat API | Workspace accounts | from memory |
| Email | dial-out (IMAP IDLE) | SMTP | universal; replies by mail | from memory |
| ntfy / Gotify | dial-out (subscribe stream) | HTTP POST | self-hostable; simple actions | from memory |
| Pushover, Discord / Slack incoming webhooks | none | HTTP POST | alerts only | from memory |

## 4. Architecture

### 4.1 The processes

- **Agentmaster.exe** — the engine, plus a new engine component, **FleetLink**: it watches the registry
  and the unread census, decides which fleet events to send and when (§6), executes inbound intents
  through the **Fleet Control API** (§8), redacts every text that leaves the process (§9.6) and
  authorizes every intent against the connection's level (§9.3). Plain C++ (no WinRT), like
  `SessionDbRefresher`; one worker thread; UI-hosted actions go through per-window sinks.
- **agentmaster-messaging.exe** — one per profile. It owns everything that touches a messaging platform:
  the connectors, the network, the outbox, rate limits and retries, the conversation map (session ↔
  topic / thread / message ids), pairing, sender authentication, action tokens and rendering. It never
  touches a session: it only sends events out and hands intents to FleetLink.
- **External connectors** — optional child processes of the service, one per configured external
  connection, speaking JSON-RPC over stdio (§12.2).
- **A tunnel** — optional, only for a push-in platform (the Teams bot): `cloudflared` (or the user's own),
  run by the service or by the user, forwarding to the service's loopback listener (§11.4).

### 4.2 Why a separate service

- **Internet input is parsed outside the process that can type into shells.** The GUI holds a stdin
  injector for every session, and the sessions run with skip-permissions. A parser bug fed by a stranger's
  message (a Telegram bot is public by username) or a hostile POST (a Teams endpoint is public by design)
  should cost a restart of a small process, not the GUI with 600 tabs. The service can later run with
  less privilege (a restricted token / AppContainer with network access and the one pipe) without
  touching the GUI.
- **It outlives the GUI** (D2): when Agentmaster exits or crashes, the service can still say so ("⚠
  Agentmaster stopped at 03:12 without saying goodbye"), answer "offline" to an incoming message, and —
  at the Operate level — start Agentmaster again (§4.4).
- **It never pins the package.** Like the session database's service, it runs from a copy in the
  profile, so a hot swap (`Swap-AgentmasterLayout.ps1`) is not blocked by it (`0x80073D02`).
- **The platform code stays out of TerminalApp.dll** (WinHTTP sessions, a listener, JWT checks), which
  keeps the GUI's link and its startup unchanged.

### 4.3 The pipe

- `\\.\pipe\agentmaster-messaging.<profile hash>`, a DACL for the current user only (the sessiondb pipe's
  rules); frames are the session database's: a 4-byte little-endian length, then UTF-8 JSON
  (`SessionDbProtocol.h` `EncodeFrame` / `DecodeFrameLength`); 1 MiB per frame.
- **FleetLink → service:** `hello` (protocol version, app identity and version, profile, pid, the
  launch command that starts this install), `sessions` (upserts and removes of the session HEADLINES the
  service needs: id, short ref, title, state, effective folder, tab color, tags, unread — never
  conversation text), `event` (§5.3), `retract` (an event stopped being true: edit or clear its message),
  `result` (the outcome of an intent), `stream` (live view text, P7), `bye` (a clean exit, with the
  reason: exit / update / restart).
- **Service → FleetLink:** `ready` (connections and their states), `intent` (§5.5), `status` (a
  connection's state changed: connected, degraded, auth failed, rate limited), `ack` (an event was
  delivered: platform message refs, for later edits).
- **At least once, never twice:** every event and intent has an id; both sides keep the ids of the last
  hour and drop repeats.

### 4.4 Lifetime

- **Start.** At engine init, when messaging is on and a connection exists, FleetLink copies the service
  from the package into `<profile>\messaging\bin` if the copy differs (the sessiondb rule), starts it
  (or finds it running by its mutex), and connects. `hello` carries both versions; a service older than
  the GUI is asked to exit and is replaced.
- **GUI exit.** A clean exit sends `bye(exit)`; a swap or an update `bye(update)`. No `bye` and a broken
  pipe = a crash: the service waits 90 s for a new GUI (the measured hot swaps were down 2–40 s), then
  sends "Agentmaster stopped unexpectedly" once; a new GUI within the wait turns that into nothing (or,
  for an update, "Agentmaster updated to &lt;version&gt;").
- **While the GUI is gone** (D2): the service keeps polling. An incoming message gets "Agentmaster is not
  running" with a **Start** button (Operate level only), which runs the install's own alias by name
  (`agentmaster.exe` / `agentmasterdev.exe`, the `_AgentmasterReopenTarget` rule); the workspace restores
  as from the Start menu. The service exits after 24 h without a GUI, or when the user turns messaging off.
- **Sleep and network loss.** Long polls time out; reconnect with backoff (1, 2, 4 … 60 s, jitter); on
  resume from sleep, poll at once. Events queued meanwhile are revalidated before sending (§6.2).

### 4.5 Files (all under the active profile, Rule #15)

| File | What |
|---|---|
| `messaging\connections.json` | The connections: provider, name, non-secret settings, owner identity, level, events, privacy, away rule, routing, quiet hours. Edited by the cog; read by FleetLink (authorization) and the service. |
| `messaging\bin\` | The service's copy (and `cloudflared.exe` if the user points the setting at one). |
| `messaging\<connectionId>\state.json` | Provider state: Telegram's update offset, the topic map (session → `message_thread_id`), the board message id; Teams' conversation reference. |
| `messaging\<connectionId>\outbox.jsonl` | Events not yet delivered (compacted on start). |
| `messaging\tokens.json` | Live action tokens (id → intent, expiry, single-use flag), at most a few hundred, expired ones dropped on load. |
| `messaging\status.json` | Written every 30 s by the service: per connection state, last delivery, last error; read by the cog's status line and a future `agentmaster msg status`. |
| `messaging\messaging.log` | The service's log (§14). |
| Windows Credential Manager | Every secret: `Agentmaster:messaging:<profile hash>:<connectionId>:<name>` (bot token, webhook URL, app secret, tunnel token, PIN hash salt). Per profile ON PURPOSE, unlike the preset keys: two installs must never share one bot. |

### 4.6 Which side does what

| Job | FleetLink (engine) | Service |
|---|---|---|
| Decide that something is worth telling | ✓ (§6) | |
| Redact, trim to the privacy level | ✓ | trims per connection |
| Render to a platform | | ✓ |
| Rate limits, retries, outbox | | ✓ |
| Authenticate the sender (platform identity → owner) | | ✓ |
| Authorize the intent (level, PIN window, confirmation) | ✓ (authoritative) | first check |
| Resolve a session reference | ✓ (authoritative) | from bindings and headlines |
| Execute | ✓ through the Fleet Control API | |
| Report the outcome | builds the text | sends / edits |

## 5. The abstraction

### 5.1 Providers, connections, bindings

- **Provider** — a kind of platform with one implementation: `telegram`, `teams-lite`, `teams-bot`,
  `webhook`, `external:<id>`. Declares its capabilities (§5.2) and its settings fields (the PRESETS idiom:
  a field table drives the cog's form).
- **Connection** — one configured use of a provider: credentials (in Credential Manager), the target chat,
  the owner identity (set by pairing), and the policy: level, events, privacy, away rule, routing filter,
  quiet hours. Several connections run at once (e.g. sessions under `K:\work\` → Teams, everything else →
  Telegram).
- **Binding** — where a session lives inside a connection, chosen from the provider's capabilities in this
  order: **topic / thread** (a Telegram topic, a Teams channel thread) → **card** (a message that carries
  the session id and its own reply input: Teams) → **reply chain** (a reply to a message about the session)
  → **explicit reference** (`#3f9a` in the text). Bindings persist in the connection's `state.json`.

### 5.2 Capabilities

```
struct ProviderCaps {
  inbound: none | dialOut | pushIn          // §3.3
  maxTextUnits; textUnit: utf16 | bytes | chars
  formats: { plain, html?, markdown?, adaptiveCard? }
  buttons: { max, perRow, payloadBytes, styles? }      // 0 = none
  inputsInMessage: bool                     // a text box inside a card (Teams)
  threads: topic | thread | reply | none
  edit, delete, pin: bool
  stream: draft | editThrottled | none      // Telegram drafts / Teams edits
  files: { maxUploadBytes }
  silentSend: bool                          // Telegram disable_notification
  rate: { perChatPerSecond, perChatPerMinute, globalPerSecond }
}
```

### 5.3 The outbound event (FleetLink → service)

```json
{ "type": "event", "id": "ev-3f9a1c2e-0042", "kind": "session.needs-you", "severity": "attention",
  "at": 1760536800123, "session": "3f9a1c2e-6c1d-4f7e-9a51-0b6f2a7d9c11",
  "headline": "Has completed after 12m and is waiting for you",
  "blocks": [
    { "t": "prompt", "text": "rebase onto agentmaster and fix the three conflicts…" },
    { "t": "reply",  "text": "Rebased; the three conflicts are fixed; the m5 suite passes." },
    { "t": "facts",  "items": [["queue", "2 waiting · runner Paused"], ["branch", "agentmaster"]] } ],
  "actions": [
    { "id": "reply",      "label": "Reply",            "intent": { "kind": "reply" },     "input": true },
    { "id": "send-next",  "label": "Send queued #1",   "intent": { "kind": "queue.sendNext", "item": "q-77" }, "style": "primary" },
    { "id": "runner-on",  "label": "Runner: Auto",     "intent": { "kind": "runner.mode", "mode": "Full" } },
    { "id": "show",       "label": "Show",             "intent": { "kind": "show" } } ],
  "replaceKey": "needs:3f9a1c2e", "validWhile": { "state": "WaitingForInput", "stateChangedAt": 1760536799000 },
  "privacy": "standard" }
```

- **Text is already redacted and trimmed** to the HIGHEST privacy level among the connections that will
  receive the event; the service trims further per connection.
- **`replaceKey`** groups the messages that describe one fact, so a newer one edits the older (Telegram
  `editMessageText`, Teams `updateActivity`) instead of piling up, and a `retract` finds it.
- **`validWhile`** is what the event assumed; FleetLink sends a `retract` when it stops holding (§6.6).

### 5.4 Rendering

One pure function per provider family, fed the event, the provider's capabilities and the connection's
privacy level, returns a render plan (text parts with formats, button rows, inputs, attachments), which the
connector serializes. Degradation is in the renderer, never in the connectors:

| Missing capability | Fallback |
|---|---|
| buttons | numbered choices in the text ("reply 1, 2 or 3") |
| topics / threads | reply chains; then `#ref` prefixes on every message |
| inputs in a message | "reply to this message" |
| edits | a new short message ("✓ answered at 12:03") |
| HTML / markdown | plain text, code as indented lines |
| short limits | the headline only + `show #ref` hint |

### 5.5 The inbound intent (service → FleetLink)

```json
{ "type": "intent", "id": "in-tg-884213377", "connection": "c-telegram-1",
  "principal": { "provider": "telegram", "userId": "123456789", "name": "Eli" },
  "kind": "reply", "session": "3f9a1c2e-6c1d-4f7e-9a51-0b6f2a7d9c11", "via": "topic",
  "text": "also update the docs", "token": null, "receivedAt": 1760536812000 }
```

- `kind` is one of the verbs of §7 (`reply`, `queue.add`, `queue.sendNext`, `answer`, `stop`,
  `runner.mode`, `pause.all`, `show`, `tail`, `screen`, `board`, `search`, `open`, `new`, `fork`,
  `restart`, `deactivate`, `close`, `away`, `mute`, …).
- `session` is resolved by the service from the binding (`via`: `topic`, `card`, `reply`, `ref`, `pick`);
  FleetLink re-resolves and refuses an id it does not know.
- `token` is set when the intent came from a button (§5.7).

### 5.6 Session references

- **In a binding** — the topic, the card, the replied-to message — the session is known; nothing to type.
- **In text** — `#3f9a`: the shortest prefix of the session id, at least 4 hex characters, unique among
  the sessions the bot has mentioned in that connection (lengthened on a collision); a list the bot just
  sent numbers its rows (`/s 3`).
- **Nothing names a session** — the bot answers with a picker (the sessions that need you first, then the
  most recent), and holds the text under a token until a pick; it never guesses (M6).

### 5.7 Action tokens

- Telegram's `callback_data` holds 64 bytes, so a button carries only `a:<token>` (a random 10-character
  base-62 id); Teams cards carry the same token in `Action.Execute` data.
- A token maps to `{connection, session, intent, binds}` where `binds` names exactly what the button
  answers: the question's `tool_use` id, the queue item id, the session's `stateChangedAt`. At press time
  FleetLink checks `binds` against now; a mismatch refuses with the current state ("already answered in
  the terminal: *Option 2*").
- Tokens for answers, sends and every Operate verb are **single use**; read buttons (`show`, `tail`) are
  reusable. Every token expires (24 h for reads, 2 h for actions, or earlier when its `binds` change).

## 6. Outbound: fleet events

### 6.1 The catalog

| Kind | When | Default (D5) | Severity | Silent? |
|---|---|---|---|---|
| `session.needs-you` | Running → Waiting / Needs approval / Idle, after the toast holds (§6.2) | Waiting ✓, Needs approval ✓, Idle ✗ | attention | no |
| `session.question` | An unanswered `AskUserQuestion` (its options as buttons, §7.3) | ✓ | urgent | no |
| `session.plan` | A plan waits for approval (`ExitPlanMode`) | ✓ | urgent | no |
| `session.permission` | A tool permission prompt (sessions without skip-permissions) | ✓ | urgent | no |
| `session.error` | Error: an API failure (with its HTTP status — a usage limit names its reset), a cut-off turn | ✓ | urgent | no |
| `session.done` | claude exited (Done) | ✗ | info | yes |
| `runner.paused` | A backstop paused a session's runner: stop-on-error, Enter-retry give-up, lost send, merge verdict | ✓ | attention | no |
| `runner.plan-done` | A session's queue ran empty after sending the last prompt | ✓ | info | yes |
| `app.stopped` / `app.updated` | §4.4 (from the service) | ✓ | attention | no |
| `fleet.digest` | Every N minutes while away: counts and the sessions that need you | ✗ | info | yes |

### 6.2 The tracker

- **Process-wide, exactly once.** The toast tracker is per window (`_EvaluateAgentNotification`, keyed to
  the window that hosts the tab). FleetLink keeps its OWN edge map over the whole fleet, fed by a registry
  observer that only queues session ids (the notifier must never wait on it); the worker reads the
  current records with the lean reads (`HeadlinesOf`, `SnapshotLiveForCards`).
- **Same edges, same holds.** A Running → Idle/Waiting edge is held while the session's external-work
  signal is live, dropped if it re-lights Running, sent with the CURRENT state when the signal clears or
  the 30 s cap passes (`DecideHeldToast`); one message per session per 20 s at most
  (`ShouldSuppressDuplicateToast`). The signal's two inputs (the heartbeat copy, side-file freshness) are
  engine reads, so the engine can evaluate them without a window.
- **Unread or away decides "needs you".** A needs-you event goes out when the census marks the session
  unread (the flash-ring rule: not the focused tab of the active window) OR the user is away (§6.3) — a
  session that finishes in the focused tab of a locked PC is never marked unread, and must still reach the
  phone. It is dropped when the session is read before it is sent.
- **Revalidate before telling.** An event leaves FleetLink only if its `validWhile` still holds, and the
  outbox drops anything older than 30 minutes unless it is still true (M8).

### 6.3 Away detection

- **Present** = the workstation is unlocked AND there was keyboard or mouse input in the last N minutes
  (`GetLastInputInfo`; default 5). Lock and unlock come from `WTSRegisterSessionNotification` on the
  Emperor's message window. **Away** otherwise, or after `/away` from the phone (until `/back` or the next
  local input).
- **Per connection (D6):** *when away* (default) / *always* / *never* (pull only). **Escalation:** a
  needs-you or question that stays unread for 10 minutes is sent even while present (the user is at the PC
  but not at Agentmaster).
- **Shared with Remote Control (D17):** FleetLink keeps `<profile>\messaging\present.marker` in step with
  "present", for sessions launched with `CLAUDE_CLIENT_PRESENCE_FILE` pointing at it.
- **Quiet hours** per connection: events are held and sent as one digest when the hours end; `urgent`
  events can be allowed through (a setting).

### 6.4 Coalescing and rates

- Several sessions finishing within 5 s become one message ("3 sessions need you" + one button each) in
  the general chat, plus the usual message in each session's topic, silent.
- The outbox's limiter follows each provider's documented rates (Telegram 1/s per chat, 30/s overall;
  Teams 7/1 s, 60/30 s per conversation) and obeys `retry_after` / `Retry-After`.
- Telegram `disable_notification` for `info` events; everything else alerts.

### 6.5 The board message

One pinned message in the general chat (Telegram) or one card (Teams), **edited in place** at most every
15 s when something changes:

```
Fleet · 21 open sessions                        updated 14:02
🔵 Running 4   🟡 Waiting 3   🟠 Needs you 1   🔴 Error 0   ⚪ Idle 13
Needs you:  #3f9a Agentmaster/feature · question    #b210 proxmox · waiting 12m
[ #3f9a ] [ #b210 ] [ Pause all ] [ Sessions ]
```

### 6.6 A message follows its fact

- The session is read on the PC → the message is edited: "✓ seen on the PC 14:05" and its buttons go.
- The question is answered (here, in the terminal or in the Claude app) → "✓ answered: *Option 2*".
- The session is Running again → "↻ running since 14:06".
- The session is closed → its topic gets "closed — `open #3f9a` to resume"; the topic stays (history).

### 6.7 The live view (P7)

`follow #3f9a` streams the current turn into the session's topic: the assistant's text as it is written
(the scanner already reads each new transcript line) and one line per tool call ("● Bash: npm test"),
throttled to one update per second. Telegram: `sendMessageDraft` with `can_stop` (refreshed before its
30 s preview ends), then `sendMessage` at the turn's end. Teams: edits of one message (a Teams stream
dies after 2 minutes; a turn often runs longer). The stop button maps to §7.4.

## 7. Inbound: what you can do from the chat

### 7.1 Commands

| Command (Telegram `/…`; Teams words) | Level | Does | Through |
|---|---|---|---|
| `board`, `status` | Read | the board (§6.5) | registry headlines |
| `sessions [words]` | Read | live sessions, numbered, with buttons | registry |
| `show #ref` | Read | the session card: state and since, folder, branch, model · effort, runner and queue, last prompt, last reply | registry + TranscriptTurns |
| `tail #ref [n]` | Read | the last n turns at the Compact level, redacted | `RenderTranscriptTurnsFile` |
| `screen #ref` | Read | the visible terminal text (≤ 60 lines), redacted | the hosting window's buffer read |
| `transcript #ref [level]` | Read | the conversation as a `.md` file | TranscriptTurns |
| `search words` | Read | sessions matching, with buttons | the session database (On), else the Sessions page's fast phase |
| *(text in a session's place)* | Steer | reply (§7.2) | `SubmitPrompt` / the queue |
| `queue #ref text` | Steer | add to the session's queue | the Prompt Queue |
| `next #ref` | Steer | Send now the first queued prompt | the Manager's Send-now path |
| `auto #ref`, `pause #ref` | Steer | the session's runner mode | `AutorunnerMode` |
| `pauseall`, `resumeall` | Steer | the global pause | `Scheduler::SetGlobalPause` |
| *(buttons)* | Steer | answer a question / plan / permission (§7.3) | the hosting window, verified |
| `stop #ref` | Operate | interrupt the turn (§7.4) | the hosting window |
| `open #ref` | Operate | resume a closed session into a tab, or start a dormant one | restore + activate sinks |
| `new <folder> [model] [-- first prompt]` | Operate | a new session | the launch seam |
| `fork #ref` | Operate | fork it | `_ForkManagedSessionById` |
| `restart #ref`, `deactivate #ref`, `close #ref` | Operate | as in the tab menu (with a confirmation) | the existing sinks |
| `away`, `back`, `mute #ref [time]`, `quiet [time]` | Notify | notification control | FleetLink |
| `help` | Notify | this table, filtered by the connection's level | |

### 7.2 Replying to a session

A pure decider (`DecideRemoteReply`) maps the session's state to the outcome; FleetLink executes it and
answers with what happened.

| The session is | Outcome |
|---|---|
| Waiting or Idle, started, live | **Send now** through `SubmitPrompt` (the draft swap keeps an unsent local draft safe); a direct send like the Manager's Send now, so it skips the runner's 40 s settle; "✓ sent · running" once its echo arrives |
| Running | **Queued as next** (shown with a **Steer now** button); it is sent when the turn ends even if the runner is Paused (a one-shot remote item, D8), after the same holds as an automatic send (the 40 s settle included). "Steer now" types it into claude's own queue now, absorbed at its next tool boundary (P0 measures that `SubmitPrompt` handles a running box) |
| Needs approval (a question or a plan is open) | Buttons: **Use as the answer** (the "Other" text) / **Queue for after** |
| Error | Send now (a retry is the user's intent) |
| Dormant (restored, never focused; or deactivated) | **Activate and send**: the tab starts in place without taking focus, the prompt waits as a one-shot item until the session is started and settled |
| Closed (archived) | **Resume and send** (Operate level): restored into the window that last hosted it, else the most recent window |
| Done (claude exited, tab open) | **Restart and send** (Operate level) |
| A delivery is in flight (gate held) | Queued as next |

- **Recorded like any prompt.** A remote prompt joins the session's queue history with a new origin,
  `PromptOrigin::Remote` (serialized `"Remote"`), and the connection id; the Prompt Queue's SENT list tags
  it **remote**; its echo is consumed like an Autorun send's.
- **Sanitized like any paste.** It goes through `BuildPromptFill` (DELIVERY.md §15), so a phone's
  invisible characters (an RLM in Hebrew text) cannot cancel the Enter.

### 7.3 Questions, plans and permission prompts

- **What is open.** Two sources, combined: the transcript's pending `tool_use` (for `AskUserQuestion`:
  each question's text, header, `multiSelect`, options with label and description; for `ExitPlanMode`: the
  plan text) — a new bounded capture in the scanner's parse, stored transient on the session
  (`pendingQuestion`, cleared by the matching tool result) — and the **menu on screen**, read by the
  hosting window (a new `ReadMenuSnapshot`, the `ReadPendingInputDraft` idiom: the question line and the
  `❯ N. label` rows; pure parser `MenuSnapshot.h`, tested on captured screens). The screen is the only
  source for menus the transcript never describes (the plan's approval choices, a permission prompt).
- **How it is shown.** Telegram: the question, one button per option (`N. label`), **Other…** (a forced
  reply for free text), and for `multiSelect` toggle buttons (✓) plus **Submit**. Teams: an Adaptive Card
  with `Input.ChoiceSet` (single or multi) and `Input.Text` for "Other".
- **How it is answered.** The hosting window, holding the delivery gate under a reserved tag
  (`#menu-answer`), re-reads the menu, checks it is the SAME menu (same question line, same option labels),
  sends the key plan for that menu shape, and verifies: the menu is gone AND the transcript's tool result
  for that `tool_use` id carries the chosen label (or, for a plan or a permission, the session left
  Needs approval). Anything else reports "not confirmed — look at the terminal" and leaves the gate.
- **The key plans are measured, not guessed** (§19, P0): which key selects an option, whether a digit
  selects or only moves, how multi-select toggles and submits, how several questions advance, how "Other"
  takes text, and how the plan and permission menus number their choices on the Claude Code version in use.
- **Already answered elsewhere** (the terminal, the Claude app): the token's `binds` no longer match → the
  message is edited to the answer that was given.

### 7.4 Stop

- `stop #ref`, the stop button of a live draft (`stopped_message_generation`), or a card's Stop sends ONE
  `Esc` through the hosting window, only while the session is Running and no menu is open (an `Esc` on an
  idle empty box twice opens the rewind menu; PENDING_INPUT.md §9). Operate level and a confirmation,
  unless the connection allows a one-tap stop.
- The interrupt hold then parks the session's runner until your next message (DELIVERY.md §14); the
  answer says so: "■ stopped · runner Paused until your next message".

### 7.5 Lifecycle verbs

`open`, `new`, `fork`, `restart`, `deactivate`, `close` reach the window-hosted seams through per-window
sinks (§8.2). Each answers with the session's new state and its topic. `new` with a first prompt passes it
as the launch command line's positional prompt when it fits (the /handover tier-1 path: nothing typed into
the TUI), else queues it.

### 7.6 Read verbs

`show`, `tail`, `screen`, `transcript` and `search` read through FleetLink, apply the connection's privacy
level and the redactor, and answer in the session's topic (or in the general chat for `board`, `sessions`,
`search`).

### 7.7 Later: files, photos, voice

A photo or file sent in a session's topic → saved under the session's folder
(`.agentmaster\inbox\<date>-<name>`) and referenced in the prompt as an `@` path. A voice note → a
transcription connector. Not in version 1.

## 8. The Fleet Control API (engine)

### 8.1 What FleetLink calls

```
Headlines(filter) / Card(id) / Board()                      read, lean registry copies
Tail(id, n, level) / Transcript(id, level) / Screen(id)      read
Search(words)                                                session database or the fast phase
Reply(id, text, mode, origin)                                §7.2 — SubmitPrompt or the queue
QueueAdd(id, text) / QueueSendNext(id, itemId) / QueueSkip(id, itemId)
RunnerMode(id, mode) / PauseAll(on)
Answer(id, menuStamp, choice)                                §7.3
Stop(id)                                                     §7.4
Open(id) / New(folder, model, firstPrompt) / Fork(id) / Restart(id) / Deactivate(id) / Close(id)
```

Every call returns `{ok, code, text}`, where `code` is one of a fixed set (`sent`, `queued`, `refused:level`,
`refused:dormant`, `refused:stale`, `refused:unknown-session`, `failed:…`), and logs one `[nav] remote …`
line (§14).

### 8.2 Window-hosted actions

The engine already fans actions out to the ONE window that hosts a tab (activate, restart, close,
deactivate, command-action). This design adds three sinks of the same shape (registered at engine init,
token-detached in `~TerminalPage`, Rule #10): **key plan** (answer a menu, stop), **screen read**, and
**launch** (open / new / fork, with "do not take focus"). A sink that finds no host answers `refused:no-tab`.

### 8.3 Other consumers

The same API is what CLI.md's deferred P2 / P3 (`restore`, `archive`, `watch`, `enqueue`, `send-now`,
`set-autorunner`) and a future MCP control server need. Building it once for messaging gives them a seam;
exposing it to them is a separate decision.

## 9. Security

### 9.1 Threat model

| Threat | Rule |
|---|---|
| A stranger messages the bot (Telegram bots are found by username) | Sender allowlist by platform user id; silent drop; pairing answers strangers only with "this bot is private" (rate-limited) |
| A member of a group the bot is in | Gate on the sender (`from.id`), never the chat (the channels reference makes the same rule) |
| The owner's messenger account is taken over | Permission levels; an optional drive PIN for Steer and Operate (§9.3); Operate off by default; every remote action audited |
| A stale or replayed button | Single-use tokens bound to the exact question / item / state (§5.7) |
| A leaked bot token | The thief can read new messages first (and so break ours, 409) and post as the bot — not drive the fleet; rotate in BotFather; the service raises an alert on a 409 it did not cause |
| A hostile POST to a public endpoint (Teams) | JWT validation before parsing the body; size caps; loopback-only listener behind the tunnel |
| Secrets in session output reaching a chat | The redactor (§9.6), always on |
| A prompt-injection in a chat message | Remote text is the owner's own prompt; it is never interpreted by FleetLink beyond the command grammar |

### 9.2 Identity and pairing

- **Telegram:** the cog shows a one-time code (6 digits, valid 10 minutes) and the deep link
  `https://t.me/<bot>?start=<code>` (with a QR code). Opening it sends `/start <code>`; the service binds
  the connection's owner to that `from.id` and the private chat id, and both sides confirm. Re-pairing
  replaces the owner.
- **Teams Bot:** after the app is installed, the owner sends `pair <code>` in the 1:1 chat; the service
  stores the user's `aadObjectId`, the tenant id and the conversation reference (`serviceUrl`,
  `conversation.id`) for proactive messages. Messages from any other user or tenant are dropped.
- **Teams Lite, Webhook:** one-way; no identity.

### 9.3 Permission levels and the drive PIN

| Level | Allows |
|---|---|
| **Notify** | events out; `away`, `back`, `mute`, `quiet`, `help` |
| **Read** | + `board`, `sessions`, `show`, `tail`, `screen`, `transcript`, `search` |
| **Steer** | + replies, queue, Send now, runner modes, pause all, answers to questions / plans / permissions |
| **Operate** | + stop, open, new, fork, restart, deactivate, close, Start Agentmaster |

- The default for a paired owner is a decision (D4); the cog sets it per connection.
- **Drive PIN (optional):** with it on, Steer and Operate intents are refused until `/unlock <PIN>`
  (valid 30 minutes); the PIN is stored salted and hashed in Credential Manager; 5 wrong tries lock the
  connection for 15 minutes and send an alert; the PIN message is deleted from the chat after it is read.
- **Confirmations:** every Operate verb answers with a confirm button first (single-use token, 2 minutes).

### 9.4 Secrets

Bot tokens, webhook URLs (a Workflows URL is itself a credential), the Teams app secret, the tunnel
token and the PIN hash live in Windows Credential Manager only (§4.5); never in `connections.json`,
`settings.json`, logs, the catalog, artifacts or commit messages (M4). The service reads them at start;
an external connector receives only the names its manifest declares, at `initialize` (§12.2).

### 9.5 Audit

Every executed or refused remote intent logs `[nav] remote <connection> <verb> <sid8> → <code>` in
hooks.log, with the platform user's id hashed. `grep '\[nav\] remote' hooks.log` lists every action taken
from a phone.

### 9.6 Privacy levels and redaction

| Level | Leaves the PC |
|---|---|
| **Minimal** | title, state, folder name (leaf), timing; no conversation text |
| **Standard** | + the prompt preview (120 characters, the toast's line 3) + the final reply's first 600 characters + question / plan text |
| **Full** | + whole replies, `tail`, `screen`, `transcript` on demand |

- **Redaction, always on, in the engine** (before the text reaches the service): the ONE redactor
  SESSIONS_AGENT.md §8.4 specifies for the AI search's results, `RedactSecrets` (pure, header-only,
  unit-tested; whichever feature is built first builds it): GitHub, Anthropic, OpenAI, AWS, Slack, Google,
  Stripe and Azure key shapes, JWTs, PEM private-key blocks, and a long high-entropy value after
  `password` / `secret` / `token` / `api_key` → `[redacted <kind>]`. A reply that echoes a `.env` file
  must not reach Telegram. The `telegram` and `teams` kinds join the list (a bot token is
  `<digits>:<35 characters>`).
- **What the platforms keep:** Telegram stores bot chats on its servers; Teams stores them in the tenant.
  The cog says so next to the privacy level. Telegram's per-chat auto-delete timer is the user's own
  setting; `protect_content` (no forwarding or saving) is a per-connection option.
- **Microsoft's volume rule** ("only send messages that people will read") is met by the away rule and the
  event defaults; the digest replaces bursts.

### 9.7 Organizations

A work tenant may forbid custom apps, or forbid sending code context to a personal device. The Teams Bot
needs the tenant's policy (D11); Teams Lite needs only Workflows. Telegram for work sessions is the user's
own policy call; routing filters let work folders go only to Teams.

## 10. The Telegram connector

### 10.1 Setup (about five minutes)

1. In Telegram, @BotFather → `/newbot` → a name and a username ending in `bot` → copy the token. One bot
   per install (release and dev each), never the homelab's bot.
2. In the BotFather **Mini App**: My bots → the bot → Bot Settings → Threads Settings → **Threaded Mode**
   on (topics in the private chat), and "users may create topics" off.
3. Cog → MESSAGING → Add → Telegram → paste the token → **Check** (`getMe`: the bot's name, and
   `has_topics_enabled`; when false, the form says how to turn it on and the connection falls back to reply
   chains).
4. **Pair**: open the link or scan the QR → Start. The chat shows "Paired with Agentmaster (release) on
   ELI-PC"; the cog shows the owner.
5. Pick the level, the events, the privacy level and the away rule; **Test** sends a sample event.

`setMyCommands` registers the command list (§7.1, filtered by the level).

### 10.2 The chat's shape

- **Private chat with topics** (D3, recommended): **General** holds the board, digests, app events and
  commands; each session that has something to say gets a topic on first need, named with its title
  (Rule #11; renamed with `editForumTopic` on a rename), colored with the nearest of the six topic colors
  to its tab color. Anything typed in a session's topic goes to that session. A fork gets its own topic.
- **Without Threaded Mode:** one chat; every session message starts `#3f9a · <title>`; a reply to it
  (swipe-reply) targets that session.
- **A forum supergroup** (alternative): the same topic model in a group the bot administers
  (`can_manage_topics`) — for an owner who wants a separate room; other members are ignored.

What a session's topic looks like (a sketch):

```
Agentmaster (bot) · topic "Agentmaster/feature" (blue)
  🟡 Waiting for you · 12 minutes ago                              ← tg-time, stays current
  ❯ rebase onto agentmaster and fix the three conflicts…
  ● Rebased; the three conflicts are fixed; the m5 suite passes.
  [ Send queued #1 ]  [ Runner: Auto ]  [ Show ]
you: also update CLAUDE.md
  ✓ sent · running                                                 ← edited to "✓ done · 3m" at the turn's end
  🟠 Question: Which approach for the strip fix?
  [ 1. Keep the caret design ]  [ 2. Native MUX drag ]  [ Other… ]
```

### 10.3 Receiving

- One thread per connection: `getUpdates(offset, timeout=50, allowed_updates=["message",
  "callback_query","stopped_message_generation"])`; the offset is persisted only after FleetLink accepted
  or refused each update (at least once; intents carry the update id, so a repeat is dropped).
- **Never a webhook** for this provider (it would disable `getUpdates`); `deleteWebhook` on setup if one
  is set (with the user's OK: someone else may own it).
- **One poller per token:** a machine-wide mutex named from the token's hash; a 409 from Telegram stops
  the loop and raises "another program is reading this bot" (M7).
- Callback queries are answered at once (`answerCallbackQuery` "working…"), the message is edited with the
  outcome when FleetLink returns.

### 10.4 Sending

- HTML parse mode; text escaped (`<`, `>`, `&`); code and screens in `<pre>`; replies longer than the
  limit become an expandable quotation + the rest as a `.md` document.
- **Length:** 4,096 characters after entity parsing; chunks split on paragraph, then line boundaries,
  never inside a `<pre>`; more than 3 chunks → a document instead.
- **Buttons:** inline keyboards, `style` `primary` for the safe default, `danger` for Stop / Close.
- **Times:** `<tg-time unix=… format="r">` shows "12 minutes ago" that stays right as time passes.
- **Silent:** `disable_notification` for `info` severity.

### 10.5 Failure modes

| Symptom | Handling |
|---|---|
| 401 (token revoked) | stop the connection, alert in the cog and with a Windows toast |
| 403 (the user blocked the bot) | stop sending; status "blocked by the user" |
| 409 | §10.3 |
| 429 | wait `retry_after`, then resume in order |
| A topic was deleted by the user | recreate it on the next event; the old binding is dropped |
| Network down | backoff; the outbox keeps events, revalidated before sending |

## 11. The Teams connector

### 11.1 Two tiers

| | **Teams Lite** | **Teams Bot** |
|---|---|---|
| Direction | one-way (alerts) | two-way |
| Needs | Workflows in the tenant | a work tenant with **Upload custom apps**, an Azure Bot (single-tenant, free tier), a public HTTPS endpoint |
| Looks like | cards from "Workflows" in a chat | cards from "Agentmaster" in a 1:1 chat |
| Buttons | links only | everything in §7 |
| Phase | P4 | P5 |

### 11.2 Teams Lite (Workflows)

- **Setup:** in Teams, Workflows → the template "Post to a chat when a webhook request is received" (the
  trigger "When a Teams webhook request is received", then a card posted to a chat) → pick the chat → copy
  the URL → cog → Add → Teams Lite → paste (stored in Credential Manager: the URL is a credential).
- **Send:** `POST <url>` with `{"type":"message","attachments":[{"contentType":
  "application/vnd.microsoft.card.adaptive","content":<card>}]}`; one card per event. The call returns no
  reference to the posted message, so the sender cannot edit it (to confirm in P0-6) and `replaceKey`
  degrades to "a new short message".
- **Cannot:** receive anything, set the sender's name, edit or delete. A card's buttons can only open
  links.
- **P0:** whether the chosen chat alerts the phone, the trigger's current authentication options, and the
  card schema version the Workflows card action accepts.

### 11.3 Teams Bot — what the user creates

1. An **Azure Bot** resource, type single-tenant (free F0 tier; the Teams channel is free; an Azure
   account asks for a card) — or a bot registered in the Teams Developer Portal. Its app id, tenant id
   and a client secret go into the cog (the secret into Credential Manager).
2. The **messaging endpoint** = the public URL of §11.4 + `/teams/<connectionId>/api/messages`.
3. **The Teams app:** the cog writes `<profile>\messaging\teams-app-<connectionId>.zip` (manifest with the
   bot id, personal scope, the command list, two icons) → Teams → Apps → Manage your apps → Upload a
   custom app.
4. **Pair** (§9.2).

### 11.4 The public endpoint (D12)

| Option | How | For | Against |
|---|---|---|---|
| **Cloudflare Tunnel on the PC** (recommended) | `cloudflared tunnel run --token <T>` as a child of the service (token in Credential Manager); a hostname on the user's own domain → `http://127.0.0.1:<port>` | free; a stable name; nothing listens beyond loopback; no relay to run | a Cloudflare account and a domain on it |
| Microsoft dev tunnel | `devtunnel host` with a persistent tunnel, anonymous access | Microsoft's own, used in Teams docs | meant for development; account login; usage limits |
| ngrok | a static free domain | quick | account; free-tier limits |
| A relay | a small public service receives, validates and holds the posts; the PC dials out to collect them | works while the PC sleeps (queues; answers "offline"); one URL for every push-in platform | another component to build, host and secure; the homelab has no public ingress today ("an internet caller needs a tunnel in front (not built)") |

The listener: loopback only; `POST` only; 256 KB body cap; JWT checked before the body is parsed; one path
per connection. Implementation choice (P0): the Windows HTTP Server API (kernel-side request parsing, if a
loopback URL can be registered without admin) or a minimal bounded parser on a loopback socket.

### 11.5 Receiving (the Bot Connector protocol)

- Validate every request's JWT: the signing keys from the Bot Framework OpenID metadata, the issuer, the
  audience (= the bot's app id), the expiry (5-minute skew), and the `serviceUrl` claim against the
  activity's `serviceUrl` (RS256 checked with BCrypt). The exact issuer set for a single-tenant bot is a
  P0 check against Microsoft's current "authenticate requests from the Bot Connector service" page.
- `message` → command or reply (a 1:1 chat has no threads: replies come from the cards' inputs, §11.6, or a
  picker); `invoke` `adaptiveCard/action` (Universal Actions, `Action.Execute`) → the token's intent,
  answered within the invoke's time with a refreshed card; `installationUpdate` / `conversationUpdate` →
  capture or drop the conversation reference.

### 11.6 Sending

- A token from `https://login.microsoftonline.com/<tenantId>/oauth2/v2.0/token` (client credentials,
  scope `https://api.botframework.com/.default`), cached until its expiry.
- `POST {serviceUrl}/v3/conversations/{conversationId}/activities` with an Adaptive Card; the reply id is
  kept for `PUT …/activities/{id}` (edits).
- **Cards:** a header (title + state chip in the tab color), facts, the text blocks, an `Input.Text`
  ("Reply to this session") with a **Send** `Action.Execute`, and the action buttons — so every card is its
  own binding (§5.1).
- **Size:** keep a card's JSON under 40 KB of UTF-8 (≈ 80 KB in UTF-16); longer text → a "Show more"
  button that sends the rest.

### 11.7 Limits and the live view

7 sends per second per conversation, 60 per 30 s, 1,800 per hour; 50 requests per second per app per
tenant; 100 KB per message; streaming 1:1 only, 1 request per second, 2 minutes per stream. The live
view therefore edits one message every 3 s instead of streaming.

### 11.8 Failure modes

| Symptom | Handling |
|---|---|
| 401 / 403 from the connector | refresh the token once, then stop and alert |
| 413 `MessageSizeTooBig` | not retryable: re-render at the next lower privacy level, then headline only |
| 429 | wait `Retry-After` |
| The tunnel is down | inbound fails in Teams ("the bot is not responding"); outbound still works; status degraded |
| The app was removed from Teams | the conversation reference dies (403); status "uninstalled" |

## 12. Any other messenger

### 12.1 The Webhook connector (built in, one-way)

`POST` (or `PUT`) to a URL with headers and a body template (`{{title}}`, `{{state}}`, `{{headline}}`,
`{{text}}`, `{{ref}}`, `{{severity}}`, JSON-escaped). Presets for ntfy, Pushover, Gotify, Discord and Slack
incoming webhooks, Home Assistant and n8n. The URL and any header marked secret go to Credential Manager.

### 12.2 The external connector contract

- **A connector is a folder** `<profile>\messaging\connectors\<id>\connector.json`:

```json
{ "id": "slack", "name": "Slack (Socket Mode)", "protocol": 1,
  "command": "python", "args": ["-I", "am_slack.py"],
  "settings": [ { "name": "channel", "label": "Channel id", "type": "text" } ],
  "secrets":  [ { "name": "appToken", "label": "App-level token (xapp-…)" },
                { "name": "botToken", "label": "Bot token (xoxb-…)" } ] }
```

  The cog renders the form from `settings` and `secrets`; secrets go to Credential Manager.
- **The process** speaks JSON-RPC 2.0, one JSON object per line, on stdin / stdout (stderr = its log).
  The service starts it, restarts it with backoff when it exits, and stops it with `shutdown`.
- **Service → connector:** `initialize {protocol, connection {id, name, settings}, secrets {…}}` →
  `{capabilities}` (§5.2); `send {eventId, binding, render {plain, markdown, html, card}, buttons, silent}`
  → `{ref, binding}`; `edit {ref, render}`; `delete {ref}`; `ensureThread {session, title, color}` →
  `{binding}`; `stream {key, text, final}`; `ack {inboundId, outcome {ok, text}}`; `shutdown`.
- **Connector → service (notifications):** `inbound {id, user {id, name}, chat {id, kind}, binding?,
  replyTo?, text?, token?, stop?}`, `status {state, detail}`, `log {level, text}`.
- **The service keeps every rule** — authentication (the owner's platform id is stored by the service at
  pairing, which works through `inbound` like any other provider), authorization, tokens, rate limits,
  redaction — so a connector is only a transport. A connector that crashes three times in ten minutes is
  stopped and reported.
- **Samples** (P6, in `tools/messaging-connectors/`): Slack Socket Mode (Python), Discord gateway (Node),
  Matrix (Python), each under 300 lines.

### 12.3 Push-in platforms

WhatsApp, a Teams bot, LINE and other webhook-only platforms share the service's loopback listener and the
tunnel of §11.4 (one path per connection); a connector for such a platform declares `inbound: pushIn` and
receives its requests from the service as `inbound` notifications after the provider's signature check
(the check is part of the connector).

## 13. Settings and UI

- **Cog → a new MESSAGING tab:** a master switch (default off); the connections list (provider, name,
  owner, a status dot, last delivery, **Test**, **Edit**, **Remove**); **Add** (Telegram / Teams Lite /
  Teams Bot / Webhook / External…).
- **Per connection:** the provider form (§10.1, §11.2, §11.3, §12); pairing (code, link, QR); level and
  drive PIN; events (the §6.1 table as checkboxes); privacy level; away rule (when away / always / never;
  idle minutes; escalate after minutes); quiet hours; routing (all sessions / favorites / tags / folders);
  topic colors on/off.
- **Global:** away detection (idle minutes, lock); the Remote Control presence file (D17).
- **The Manager toolbar:** a status pill (📱 + a dot per connection: green, amber, red), its tooltip naming
  each connection's state and last delivery; a click opens the tab.
- **Tab menu:** Messaging ▸ Mute this session (1 h, today, until unmuted) / Follow live (P7) / Send its
  card now.
- **Per-tab badge:** a small 📱 while a remote follow is on (P7).

## 14. Logs

- **hooks.log:** `[msg] <conn> event <kind> <sid8> → sent|held(<why>)|dropped(<why>)`;
  `[msg] <conn> intent <kind> <sid8> → <code>`; `[nav] remote <conn> <verb> <sid8> → <code>` (§9.5);
  `[msg] service up|down|restarted (<why>)`.
- **messaging.log** (the service): `[tg]`, `[teams]`, `[hook]`, `[ext:<id>]` with HTTP status, latency,
  `retry_after`, poll gaps, connector restarts. Never a message body or a token: ids, sizes and hashes only.
- **Rule #18** applies to the service as to the app: every `catch (...)` reports through the service's own
  `LogSwallowedException`, with the throw-site stack.

## 15. Testing

- **m5 (pure):** the event decider (edges, holds, dedupe, unread, away, escalation, quiet hours, routing);
  `DecideRemoteReply`; the command grammar and session references; levels, PIN windows and confirmations;
  the token table (bind checks, single use, expiry); the redactor (a pattern table + false-positive
  fixtures); Telegram escaping and chunking (UTF-16 counting, `<pre>` safety); the Adaptive Card builder
  (goldens); the JWT check (RS256 with a test key); `MenuSnapshot` over captured screens.
- **Service integration:** a fake Telegram server on loopback (the connector's base URL is a setting —
  Telegram's self-hosted Bot API server uses the same knob); a fake Bot Connector with test-signed JWTs; a
  webhook echo; a fake external connector.
- **Abuse:** a stranger, a group member, a replayed update id, an expired / reused / mismatched token, PIN
  guessing, a 64 KB message, a malformed JSON update, a 409 storm.
- **Live (by hand):** a test bot and chat (`AgentmasterTestBot`), the claude-wrap-ConPTY driver for the
  menu key plans, a Teams developer tenant, the cog on a disposable Windows desktop (claude-desktops).

## 16. Proposed invariants (to join the Correctness rules when built)

- **M1 — One send seam.** A remote reply goes through `SubmitPrompt` or the Prompt Queue; a remote key
  press goes through the hosting window under the delivery gate. The service never writes to a ConPTY
  (extends Rules #3, #4, #13).
- **M2 — Sender, not chat.** Every inbound message is checked against the connection's owner by the
  platform's USER id (Telegram `from.id`; Teams `aadObjectId` + tenant id); anything else is dropped.
- **M3 — The engine authorizes.** FleetLink checks every intent against the connection's level in its own
  read of `connections.json`; the service's claim is not trusted for it.
- **M4 — Never send a secret.** Redaction runs in the engine before any text leaves the GUI process;
  credentials live in Credential Manager only; message bodies never reach a log.
- **M5 — Never act on a stale button.** A token names exactly what it answers; a mismatch refuses and shows
  the current state.
- **M6 — Never guess a session.** A message that names no session gets a picker.
- **M7 — One poller per token.** A machine-wide lock per bot token; a 409 stops polling and alerts.
- **M8 — Revalidate before telling; follow up after.** An event is sent only if still true; when it stops
  being true after it was sent, its message is updated.

## 17. Phases

| Phase | Scope | Exit |
|---|---|---|
| **P0** | The measurements of §19; no product code | Every P0 row answered in this document |
| **P1** — Telegram alerts | The service skeleton + pipe; FleetLink's tracker (needs-you, question, error, runner paused, app stopped); away detection; redactor + privacy levels; pairing; the MESSAGING tab (one Telegram connection); logs; m5 | A needs-you edge reaches the phone within 5 s while the PC is locked; nothing while present (until escalation); a stranger is dropped; no secret leaves (redactor tests) |
| **P2** — Telegram two-way | Read verbs; replies (§7.2) with `PromptOrigin::Remote`; queue and runner verbs; topics; tokens and confirmations; the board; message follow-ups (§6.6) | From the phone: replies into sessions in four different states, each with the right outcome; every action in `[nav] remote` |
| **P3** — menus and stop | `pendingQuestion` capture; `ReadMenuSnapshot`; verified key plans; stop | 20 of 20 answers verified across the measured menu shapes in the ConPTY harness; stale tokens refused |
| **P4** — Teams Lite + Webhook | The Workflows connector; the Webhook connector with presets | Alerts in a Teams chat and in ntfy |
| **P5** — Teams Bot | App package generator; the loopback listener + tunnel; JWT; Bot Connector client; cards with inputs; pairing; read and steer verbs | Two-way from Teams in the user's tenant (or a developer tenant) |
| **P6** — external connectors | The contract (§12.2); the three samples; the connector manager | A Slack and a Discord sample pass the integration suite |
| **P7** — live and more | The live view; digests; the concierge (`ask …` answered by a harness conversation with the session database's tools); Start Agentmaster from the phone; files and photos | |

## 18. Decisions for the user

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| **D1** | Where the platform code runs | **A per-profile service** (`agentmaster-messaging.exe`, §4.2) | Inside the GUI process |
| **D2** | The service after Agentmaster exits | **Keep running up to 24 h**: report the exit, answer "offline", offer Start (Operate level) | Exit with the GUI |
| **D3** | Telegram's chat shape | **A private chat with one topic per session** (reply chains when Threaded Mode is off) | One chat with reply chains only; a forum supergroup |
| **D4** | A paired owner's default level | **Steer** (Operate off; the drive PIN offered, off) | Read; Operate |
| **D5** | Default events | **Waiting, Needs approval, questions, plans, permissions, errors, runner paused, app stopped** | Also Idle and Done; also digests |
| **D6** | When to send | **When away** (locked, or idle ≥ 5 min), **escalate** an unread needs-you after 10 min even when present | Always; only when away |
| **D7** | Default privacy | **Standard** (prompt preview + a 600-character reply excerpt), redaction always on | Minimal; Full |
| **D8** | A reply to a Running session | **Queued as next, sent when the turn ends even under Paused**, with a **Steer now** button | Always steer now; queue under the runner's own mode |
| **D9** | Questions and plans from the chat | **Yes, by verified key plans** (§7.3) | Leave them to the terminal or Remote Control |
| **D10** | The Teams route | **Teams Lite first** (P4), the bot when the tenant allows custom apps (P5) | The bot first; no Teams |
| **D11** | Which Microsoft tenant | The user's call: the work tenant (its policy, its data rules) or a personal developer tenant | — |
| **D12** | The public endpoint for push-in platforms | **Cloudflare Tunnel on the PC**, run by the service | A dev tunnel; ngrok; a relay service |
| **D13** | Bots per install | **One bot for release, another for dev** (one poller per token) | Messaging in one install only |
| **D14** | Built-in connectors' language | **C++** in the service (WinHTTP, nlohmann-json, BCrypt), the house style | A .NET service using the Teams SDK |
| **D15** | The external contract | **JSON-RPC over stdio, one object per line** | Loopback HTTP |
| **D16** | Claude Code channels | **Not in version 1**; delivery stays pluggable (§2.3) | Deliver through channels now, with the development flag |
| **D17** | The Remote Control presence file | **Yes**: Agentmaster keeps it in step with "present" for managed launches | Leave Remote Control alone |
| **D18** | The name in the UI | **Messaging** | Remote; Phone |

## 19. P0 — measure before code

| # | Question | How |
|---|---|---|
| P0-1 | The key plan of each menu: `AskUserQuestion` single-select, multi-select, several questions, "Other" with text; the plan approval menu; a permission prompt — on the Claude Code version in use | claude-wrap-ConPTY driver: provoke each, capture the screen, try keys, read the transcript's tool result |
| P0-2 | Does `SubmitPrompt` (the draft swap) place a prompt into a RUNNING session's box, and how is it recorded (the `queued_command` attachment)? | the same driver, on a turn that runs a long tool |
| P0-3 | How the transcript records `AskUserQuestion` input and answers (for `pendingQuestion`) | the corpus: `tool_use` / `tool_result` pairs of the last 30 days |
| P0-4 | Telegram topics in a private chat: rename noise (service messages?), `pinChatMessage` in a topic, icon colors on Android / iOS / Desktop, the limit on the number of topics | a test bot |
| P0-5 | Telegram: drafts in a topic (`message_thread_id`) and the stop update end to end | a test bot |
| P0-6 | Teams: is **Upload custom apps** allowed in the user's tenant? Is Workflows available? Does a Workflows post to a chat alert the phone? | the user's Teams; a developer tenant if the work tenant says no |
| P0-7 | The Bot Connector's JWT issuers and endorsements for a single-tenant bot, today | Microsoft's page + one real request through the tunnel |
| P0-8 | A loopback listener without admin: the HTTP Server API's URL registration rules | a probe exe |
| P0-9 | The presence file: does `CLAUDE_CLIENT_PRESENCE_FILE` set at launch (spawn env) suppress Remote Control pushes as documented? | one session with Remote Control |
| P0-10 | Channels later: does a channel event fire `UserPromptSubmit`, and how is it written to the transcript? | a fakechat session |

## 20. Sources (read 2026-10-15)

- Telegram Bot API: [the API reference](https://core.telegram.org/bots/api), [the changelog](https://core.telegram.org/bots/api-changelog) (9.3 – 10.3), [the Bots FAQ](https://core.telegram.org/bots/faq) (limits); Threaded Mode's location: [Hermes Agent's Telegram docs](https://hermes-agent.nousresearch.com/docs/user-guide/messaging/telegram) and [its fix PR #116803](https://github.com/NousResearch/hermes-agent/pull/116803).
- Claude Code: [channels](https://code.claude.com/docs/en/channels), [the channels reference](https://code.claude.com/docs/en/channels-reference), [Remote Control](https://code.claude.com/docs/en/remote-control).
- Teams bots: [what is the Bot Framework SDK](https://learn.microsoft.com/en-us/azure/bot-service/bot-service-overview?view=azure-bot-service-4.0) (end of support), [the Teams SDK 2.1 announcement](https://microsoft.github.io/teams-sdk/blog/announcing-teams-sdk-dotnet-2-1/), [choosing an SDK](https://microsoft.github.io/teams-sdk/teams/choosing-an-sdk/), [create an Azure Bot resource](https://learn.microsoft.com/en-us/azure/bot-service/abs-quickstart?view=azure-bot-service-4.0) (single-tenant), [rate limits](https://learn.microsoft.com/en-us/microsoftteams/platform/bots/how-to/rate-limit), [message formatting and size](https://learn.microsoft.com/en-us/microsoftteams/platform/bots/how-to/format-your-bot-messages), [streaming](https://learn.microsoft.com/en-us/microsoftteams/platform/bots/streaming-ux), [the two-minute stream limit PR](https://github.com/microsoft/teams-sdk/pull/2989), [developer prerequisites](https://learn.microsoft.com/en-us/microsoftteams/platform/toolkit/tools-prerequisites).
- Microsoft Graph and Teams: [the Teams API overview](https://learn.microsoft.com/en-us/graph/api/resources/teams-api-overview?view=graph-rest-1.0) (polling once a day), [send a message in a chat](https://learn.microsoft.com/en-us/graph/api/chat-post-messages?view=graph-rest-1.0) (the "log file" rule), [list messages in a chat](https://learn.microsoft.com/en-us/graph/api/chat-list-messages?view=graph-rest-1.0), [change notifications for messages](https://learn.microsoft.com/en-us/graph/teams-changenotifications-chatmessage), the `48:notes` self chat ([a community article](https://dev.to/thecodewrapper/how-to-send-microsoft-teams-self-message-using-graph-sdk-1ojm)).
- Workflows and the connectors' retirement: [Microsoft 365 Developer Blog](https://devblogs.microsoft.com/microsoft365dev/retirement-of-office-365-connectors-within-microsoft-teams/), [MC1181996](https://mc.merill.net/message/MC1181996), [Office 365 IT Pros, 2026-09-18](https://office365itpros.com/2026/09/18/teams-workflows-updates/).
- Others: [Slack Socket Mode](https://docs.slack.dev/apis/events-api/using-socket-mode/), [Discord gateway](https://docs.discord.com/developers/events/gateway), [matterbridge releases](https://github.com/42wim/matterbridge/releases) and [its community fork](https://github.com/matterbridge-org/matterbridge).
- The homelab's own Telegram bot and its "no public address" note: the `fastrun` skill (`~/.claude/skills/fastrun/SKILL.md`).
