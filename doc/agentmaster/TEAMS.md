# Agentmaster — the fleet in Microsoft Teams

> **Status: DESIGN ONLY (2026-11-18). Nothing is built.** This document translates every feature of the Telegram bot
> ([`SPEC.md`](../claude-code-telegram/SPEC.md), [`MESSAGING.md`](MESSAGING.md) §21) to Microsoft Teams. The platform facts
> come from Microsoft's documentation as read on 2026-11-18 (§14 lists the pages). The tenant facts were read on
> 2026-11-18 in a locked-down company tenant through Microsoft Graph as a normal user, read-only (§3). No bot could be
> registered for a live test: users of such a tenant cannot register applications (§3). The bot's live measurements wait
> for a test bot (§12).
>
> One shape needs no bot, and it was measured live on 2026-11-18: Agentmaster writes as the owner, through Microsoft
> Graph, into the owner's private channel (T4, §4.4). §15 has the results.
>
> It replaces [`MESSAGING.md`](MESSAGING.md) §3.2 and §11 (written 2026-10-16). Four platform changes since then decide
> the design (§1).

## 0. Summary

- **The shape.** One Teams bot per install. Its 1:1 chat with the owner holds the whole fleet, as the Telegram bot's
  private chat does:
  - Each working session is one **thread**: a top-level session card that the bot posts, with the session's
    conversation as replies under it.
  - A **🔎 Search** thread hosts the search agent.
  - A **fleet board** card replaces the pinned directory.
  - A top-level message from the owner starts a **new session**.
  - Threads in 1:1 agent chats are a Teams preview feature (§2.3).
- **No public endpoint.** **Socket mode** (preview since 2026-11-13) lets Agentmaster open outgoing WebSockets to
  Microsoft and receive every activity on them, like Telegram's long poll. Agentmaster sends through the same REST API
  as any bot.
- **What Teams does better than Telegram:**
  - Messages up to ~80 KB in **extended Markdown** (tables, code, task lists).
  - **Cards that refresh themselves** when the owner looks at them, so the session card needs no timed edits.
  - One form for each Claude Code question, with no tap-by-tap steps.
  - A native **step list with a Stop button** for a turn started from the phone ("plans").
  - **Slash commands** in the "/" menu.
  - **Deep links** that open a thread on every client.
  - About **1,800 calls per hour** per conversation, against about 1,080 per hour (18 per minute) in Telegram.
- **What Teams cannot do:**
  - It keeps no queue while Agentmaster is closed: a message sent then never reaches the bot.
  - A bot cannot send a silent message. Section 6 explains how one sound per turn is kept.
  - Threads do not move up the list on new replies.
  - The bot cannot read its own 1:1 chat history without a tenant-wide permission.
  - Every message stays in the company tenant, under its retention and eDiscovery rules.
- **What blocks it in a locked-down tenant** (measured, §3): users cannot register applications, and only one security
  group may create teams. An administrator must register the bot and allow the app (§9). The preview features also need the
  owner's Teams client in public preview, an administrator policy.
- **Decision (the owner, 2026-11-18): build the bot-less shape first (T4), never with an administrator.** Its design is
  [`TEAMS_CHANNEL.md`](TEAMS_CHANNEL.md). The bot shapes (T1, T2) below stay as the later option for a tenant whose
  administrator registers a bot.
- **Without a bot (measured, §15):** Agentmaster writes the whole mirror into the owner's private channel as the
  owner, through Graph: a thread per session, the card edited in place, replies, formatting, reactions, pictures,
  files, cards, 80 KB messages. A post that @mentions the owner rings, so one sound per turn works; every other post
  is silent. Emoji reactions are the buttons, with a pinned legend (§15.4). Replies and reactions come back by
  polling the channel's delta (one small read per poll). Not possible: unread markers, a delete (it needs an
  administrator), and the bot-only features.

## 1. What changed since MESSAGING.md §3.2 and §11 (2026-10-16)

| MESSAGING.md said | Now (2026-11-18) |
|---|---|
| "Teams has no two-way path without a public HTTPS endpoint"; the plan was Cloudflare Tunnel on the PC (D12) | **Socket mode** (public preview, 2026-11-13): the agent opens outgoing WebSockets to the Agent Communication Service (ACS) and receives activities on them. No inbound port, no tunnel, no public URL. New bots only; "for development and testing". |
| "A 1:1 chat has no threads" | **Threads in 1:1 agent chats** (public preview): top-level (L1) messages and in-thread (L2) replies, in channels and in one-on-one agent chats. A "sessions" preview (2026-09-24) ended on 2026-10-22 and will not ship; threads replace it. |
| "Streams die after 2 minutes; streaming 1:1 only" | Streams close 10 minutes after the first update; streaming works in every conversation type (generally available 2026-10-28). |
| (not covered) | **Agent reactions** (GA 2026-07-10), **extended Markdown** (GA 2026-10-08), **slash commands** and targeted messages (GA 2026-07-30), **plans and steps** (preview), **HTML widgets** (preview, 2026-11-16), **apps in private channels** (GA 2026-07-07). |
| "Polling: once per day" | Confirmed: polling a Teams resource more than once a day breaks the Microsoft APIs Terms of Use. |

## 2. Teams platform facts the design uses (as of 2026-11-18)

### 2.1 Receiving activities

| Fact | Consequence |
|---|---|
| Two delivery methods: an HTTPS endpoint (GA) or **socket mode** (preview). Both deliver the same activity JSON. Sending is the same REST API for both. | Socket mode for the first version; an HTTPS endpoint behind the existing Cloudflare route (MESSAGING.md §21.32.4) is the production option (D-T6). |
| Socket mode: `POST https://{geo}.botapi.skype.com/v3/websockets/connect` with the bot's own token answers a SignalR URL and a one-socket access token (60 min). The agent opens the socket (Azure SignalR Service, JSON hub protocol, **client results**), waits for `SocketReady`, answers each `Activity` call with a reply frame, and replaces the socket about 60 s before its token expires (header `x-ms-socket-replaces-connection-id`). | A C++ client: WinHTTP WebSocket + the SignalR JSON hub protocol (handshake, invocation with completion, ping, close). Microsoft ships SignalR clients for JavaScript and .NET only; "you can't use a plain WebSocket library unless you implement the SignalR hub protocol yourself" (§8.3). |
| Regions `amer`, `emea`, `apac`: ACS delivers a conversation's activities only to sockets of that conversation's region. At most 4 sockets per agent per region. | One socket per region. A tenant's bot traffic goes to one region (for example `https://smba.trafficmanager.net/emea/`), but all three are connected, as Microsoft requires. |
| Reply within **25 s** (`deadlineMs`); invoke = the invoke response; other activities = an acknowledgment. Reply ≤ 1 MB. | Card taps are answered with the new card inside the deadline (§5, §11). |
| "No retry on the socket"; duplicates possible (deduplicate by activity `id`); no ordering guarantee. | The router deduplicates and orders by timestamp where order matters. |
| **Offline:** "ACS doesn't deliver activities in a region unless your agent has an active WebSocket connection in that region." No fallback to an HTTPS endpoint. | No backlog as Telegram's 24-hour queue. A message sent while Agentmaster is closed never reaches the bot (§5, D-T7). |
| ACS spreads one agent's activities over all its sockets in a region. | Two Agentmaster instances with the same bot would split the activities: one bot per install, and a machine-wide mutex on the app id (the Telegram rule M7). |
| HTTPS endpoint: a bearer JWT on each request; issuer `https://api.botframework.com`, keys from `https://login.botframework.com/v1/.well-known/openidconfiguration`, audience = the app id, the `serviceUrl` claim equal to the activity's. | The HTTPS option validates RS256 with BCrypt before it parses the body (the rule H4). |

### 2.2 Sending

| Fact | Consequence |
|---|---|
| Token: client credentials of the bot's Entra app, `POST https://login.microsoftonline.com/<tenant>/oauth2/v2.0/token`, scope `https://api.botframework.com/.default` (the tenant id for a single-tenant app). | Cached until shortly before expiry. A single-tenant bot needs the tenant id here, not `botframework.com`. |
| `POST {serviceUrl}/v3/conversations/{id}/activities` sends; `PUT …/activities/{activityId}` updates; `DELETE …/activities/{activityId}` deletes. | Edits and deletes of the bot's own messages. No documented age limit (Telegram: a bot deletes only within 48 h) — P0-T11. |
| Size: **100 KB** per message, approximately (text, links, mentions, reactions, UTF-16); keep within 80 KB; base64 images do not count. `413 MessageSizeTooBig` beyond. | Split at 80 KB, not at 4,096 characters. |
| Retry `429` (with `Retry-After`), and also `412`, `502`, `504`. | The durable outbox's answer table gains a Teams column (§8.2). |

### 2.3 Threads (preview)

| Fact | Consequence |
|---|---|
| "Available in channels and one-on-one agent chats … Group and one-on-one user chats will be available in preview soon." | The topology T1 (§4). |
| L1 = a top-level message, which can root a thread; L2 = a reply in the thread; an L2 cannot root a nested thread. | Session card = L1; conversation = L2. |
| IDs: the containing chat `channelData.chat.id` (or `channelData.channel.id`), the root `channelData.replyChain.id` (present on both levels when threads are supported). Treat `conversation.id` as opaque. | The thread map keys a session to (chat id, root id). |
| L1: `POST {serviceUrl}/v3/conversations/{chatOrChannelId}/activities`. L2: `POST {serviceUrl}/v3/conversations/{chatOrChannelId}/replyChains/{rootMessageId}` (the root may be the bot's own L1). | Proactive replies into any session's thread. |
| `400 BadArgument "Reply chains is not allowed in this thread type."` when the chat has no threads. | A runtime test at start: fall back to the flat chat (§4, T1′). |
| Preview features need the user's Teams client in **public preview** (administrator policy) and sometimes the developer-preview manifest, which installs only by direct upload. | An administrator step (§9). |

### 2.4 Formatting and media

| Fact | Consequence |
|---|---|
| **Extended Markdown** (`textFormat: "extendedmarkdown"`, GA 2026-10-08): GFM tables with alignment, task lists (read-only), fenced code with a language, LaTeX math, images, @mentions, citations; renders progressively while streaming. | Claude's replies go as they are, tables included. No HTML conversion, no 4,096-character chunks. |
| Default Markdown: bold, italic, links, and a few HTML tags. `xml` and `plain` are legacy. | Only extended Markdown is used. |
| Inline images: a public HTTPS URL or a base64 data URI; **≤ 1024 × 1024 px, ≤ 1 MB**, PNG / JPEG / GIF; out-of-range images can be accepted and then not shown. | The terminal screenshot (about 2,232 × 1,352 px) must be scaled or split (P0-T8). |
| Files to the user in the 1:1 chat: the file consent card (the user accepts; the bot uploads to the user's OneDrive). | 📤 Send as file and 📜 All Screen's tall pictures. |

### 2.5 Cards and actions

| Fact | Consequence |
|---|---|
| Adaptive Cards 1.5 with **Universal Actions**: `Action.Execute` sends an `adaptiveCard/action` invoke; the response can be a new card (`application/vnd.microsoft.card.adaptive`), which replaces the shown one, or a message. | Buttons with no 64-byte callback limit; a tap answers with the updated card. |
| **Refresh on view:** a card with `refresh` (an `Action.Execute` and `userIds`, ≤ 60) asks the bot for a fresh card when a listed user views it; "the refresh triggers only if the card from the last invoke is older than a minute", and opening the chat refreshes it too. The refreshed card goes only to the client that asked. | The session card and the fleet board stay current with no timed edits (Telegram: "clock times, never spans", §21.4). |
| Inputs: `Input.ChoiceSet` (radio, checkboxes, `style: "filtered"` typeahead; dynamic search by `Data.Query` → an `application/search` invoke), `Input.Text`, `Action.ShowCard`, `Action.ToggleVisibility`. | One form for a question card, the new-session form, the folder picker. |
| Suggested actions: at most three, 1:1 only. Prompt starters (GA 2026-04-27): suggested prompts in the manifest. | Optional shortcuts. |
| HTML widgets (preview, 2026-11-16): interactive HTML in a message; the documented prerequisite is a public HTTPS endpoint. | Not in version 1. |

### 2.6 Reactions, plans, streaming

| Fact | Consequence |
|---|---|
| **Reactions** (GA 2026-07-10): an agent adds and removes reactions on any message of its conversations; IDs such as `1f44b_wavinghand` from the Teams reactions reference; **2 per second across all conversations**; failed on a deleted message (do not retry). The agent also receives `messageReaction` events. | The status of the owner's own message (✍ → 👨‍💻 → 👌 …), as in Telegram (SPEC §11.2.4). The REST route is not documented on the page read (the SDK calls `api.conversations.reactions.add`) — P0-T13. |
| **Plans and steps** (preview): bound to the user message that a plan answers (`replyToId`). `typing` activities with a `planInfo` entity (`start`, `continue`, `stop`, `error`); the final `message` with `planType: "complete"`. Teams shows a thinking indicator under the prompt, the steps (proposed / active / completed), and a **Stop** button; completed steps fold under the final answer. First update within 5 min of the user's message; timeout after 24 h without updates; after the user's Stop, updates answer `409 PlanConflict`. Not on targeted activities, edits, or new conversations. | The work log and ⎋ Esc of a turn started from the phone, natively, with no extra messages (§5, §9 of SPEC). Whether a plan can bind to the bot's own message (a turn started at the PC) is unknown — P0-T5. |
| **Streaming** (GA everywhere, 2026-10-28): one open stream per conversation; informative updates ≤ 1,000 characters; a stream closes 10 min after the first update; not for proactive messages; the REST call needs `replyToId` outside 1:1. | Not used for the mirror (turns run longer than 10 min and most are proactive). |

### 2.7 Notifications

| Fact | Consequence |
|---|---|
| A bot message in a chat notifies the user according to the user's chat notification settings. The documentation names no per-message "silent" switch (Telegram: `disable_notification`). | §6. Measured on a test bot: P0-T2. |
| The user can mute a chat. | §6, option A. |
| **Activity feed notifications** (Graph): an OS notification with sound on desktop and mobile, a deep link, `chainId` to update an earlier notification; the app must be installed for the user; RSC `TeamsActivity.Send.User` (personal scope, app-only) is consented at installation; at most **20 per minute per user**; Graph limit 5 requests per second per app per tenant. | The "one sound per turn" signal when the bot chat is muted (§6). |

### 2.8 Limits

| Limit | Value |
|---|---|
| Per agent per conversation ("a conversation is 1:1 between agent and user, a group chat, or a channel in a team") | send: **7 / 1 s, 8 / 2 s, 60 / 30 s, 1,800 / 3,600 s**; create conversation: the same |
| Per conversation for all agents | send 14 / 1 s, 16 / 2 s |
| Per app per tenant | 50 requests per second |
| Reactions | 2 per second across all conversations |
| Message | 100 KB (keep within 80 KB) |
| Inline image | 1024 × 1024 px, 1 MB |
| Card refresh `userIds` | 60 |
| Invoke answer | 25 s (socket `deadlineMs`) |
| Activity feed | 20 per minute per user |

### 2.9 Commands

| Fact | Consequence |
|---|---|
| **Slash commands** (GA 2026-07-30): `bots[].commandLists[]` with `triggers: ["slash"]` in the manifest, with `supportsTargetedMessages: true`; Teams shows them when the user types `/` in an empty compose box, on every client including mobile; the command arrives as message text. | The "/" menu of SPEC §11.3.3, declared in the manifest instead of `setMyCommands`. The same router and refusal tables. |

### 2.10 Registration and preview

| Fact | Consequence |
|---|---|
| Teams CLI ≥ 3.1.0: `teams login` checks the account's permission to upload custom apps ("Sideloading: enabled"); `teams app create --name … --messaging-mode socket` creates an **Entra app registration with a client secret**, a **Teams-managed ACS registration** (no Azure subscription), a starter manifest in the Developer Portal and a local configuration file. | The setup of §9. It needs the right to register applications. |
| A Teams app reaches a user by direct upload (sideloading, if the app setup policy allows it) or from the organization's catalog (an administrator uploads or approves it). | §9. |
| Users need public preview for preview features; the developer-preview manifest installs by direct upload only. | §9. |
| The Bot Framework SDK is out of support since 2025-12-31; Teams SDK 2.2 (TypeScript, Python, .NET) and the Microsoft 365 Agents SDK replace it; the REST protocol is enough for C++. | No SDK dependency (§8). |

### 2.11 Microsoft Graph rules that matter

| Rule | Consequence |
|---|---|
| "If your app polls to see whether a resource has changed, you can only do that once per day … Apps that don't follow these polling requirements will be considered in violation of the Microsoft APIs Terms of Use." | A bot (T1, T2) needs no polling. The bot-less connector (T4) polls by the owner's explicit choice, with one small delta read per poll (TEAMS_CHANNEL.md §7), and says so in its settings. |
| Change notifications need an HTTPS webhook, Azure Event Hubs or Azure Event Grid; a Teams subscription lasts at most 60 minutes (renewable); 10,000 Teams subscriptions per organization. | Not needed: the bot receives through ACS. |
| Personal scope RSC: "The only RSC permission relevant to chats in personal scope is `ChatMessageReadReceipt.Read.Chat`." Reading the bot's 1:1 chat needs `Chat.Read.All` (application, administrator consent) or the user's delegated sign-in. | No catch-up of missed messages without one of the two (D-T7). |
| Pinning a chat message: delegated `ChatMessage.Send`, or application `Chat.ReadWrite.All`; no RSC. | The bot cannot pin the fleet board; the owner pins it once by hand (§5). |

## 3. A locked-down tenant (measured 2026-11-18, read-only, as a normal user)

The design must work in a company tenant where a normal user has no administrator and gets none. The facts below were
read in one such tenant through Microsoft Graph with the Azure CLI's delegated token (`az rest`) and the user's Teams
sign-in; nothing was created or changed. Read your own tenant's values the same way.

| Fact | Value in a locked-down tenant | Consequence |
|---|---|---|
| **Users may register applications** | `authorizationPolicy.defaultUserRolePermissions.allowedToCreateApps = false` | `teams app create` (and the Developer Portal's bot management) fail for a normal user. Only an administrator can create a bot (T1, T2). |
| The user's directory roles | only the default user role | No administrator rights. |
| **Users may create teams** | `Group.Unified`: `EnableGroupCreation = False`, with one allowed group that the user is not in | No team of their own. A private channel in a team they belong to still works (T4). |
| User consent | `permissionGrantPoliciesAssigned`: only the two "ManagePermissionGrantsForOwnedResource … dynamically-managed-permissions-for-team / -for-chat" policies | Users can grant RSC permissions for teams and chats they own, and nothing else: a new Graph scope for any app needs an administrator. |
| Apps the tenant already allows | Microsoft's own apps (the Azure CLI); apps that an administrator consented for other work | The only identities a bot-less Agentmaster can use (§15, TEAMS_CHANNEL.md §4). |
| A private channel of the user's own | private, layout `post`, the user its only member | The sandbox of the measurements (§15) and the home of T4. |
| Sideloading, public preview | not read (needs `teams login` or the Teams client) | P0-T17, P0-T4. |

## 4. The shape on Teams

### 4.1 T1 — the bot's 1:1 chat with threads (recommended)

```
The bot's 1:1 chat with the owner (threads: preview)
 ├─ L1 📊 Fleet board (Adaptive Card, refreshed on view; the owner pins it once)
 ├─ L1 🔎 Search ─── L2 requests and answers
 ├─ L1 [🟦 drag crash fix] 🔵 Running since 14:02 · Agentmaster · main · opus · 182K   ← the session card
 │     ├─ L2 👤 You (📱): fix the drag crash        ✍ → 👨‍💻 → 👌 (reactions)
 │     │      └─ plan: ✓ Read ×2 · ● Edit TabDragMath.h · ○ Run tests   [Stop]
 │     └─ L2 Claude: the final reply (extended Markdown, quoting the prompt)
 ├─ L1 [🟩 docs] 🟡 Waiting for you since 13:40 …
 └─ L1 (the owner writes a new top-level message) → L2 new-session form → the session runs in this thread
```

- **Why:** it matches the Telegram layout one to one (one private chat, a thread per session), it needs no team (which
  users of a locked-down tenant cannot create), and the chat is the owner's own.
- **Cost:** threads in 1:1 agent chats are a preview; the whole chat is **one conversation**, so every session shares
  one budget of 1,800 calls per hour (§7); thread roots keep their creation order.
- **T1′ (fallback when the chat has no threads):** the flat 1:1 chat. Each session posts one message per turn with its
  title line and a deep link to its card. It works, but the conversation of many sessions runs together.

### 4.2 T2 — a channel with one thread per session

- A private channel (in a team the owner belongs to) with the bot installed; one post per
  session (the card as the post, the turns as replies); RSC `ChannelMessage.Read.Group` lets the bot receive the owner's
  replies without an @mention.
- **For:** threads in channels are generally available (posts layout; the threads layout is rolling out), a channel's
  notifications are off by default except for threads the user follows and @mentions, so an @mention in the final reply
  is one sound per turn without muting.
- **Against:** it needs a team, the bot's installation in it (a team owner's consent), and channel-level visibility
  rules; a team per folder (MESSAGING.md §22) needs team creation, which a locked-down tenant restricts.

### 4.3 T3 — group chats per session

- Not possible for a bot: a bot cannot create a group chat, and a chat per session would add one more chat to the
  owner's chat list for every session.

### 4.4 T4 — the owner's private channel, written as the owner through Graph (no bot; measured)

- Agentmaster signs in as the owner (a delegated Graph token of its own app registration) and writes into a private
  channel where the owner is the only member: one post per session (the card as the post, its subject the state icon,
  the color square and the title), the turns as replies under it. The owner's replies come back through Graph change
  notifications.
- **For:** no bot, no team to create, and no administrator for the content side. Everything the mirror shows works
  (§15): posts with a subject, replies, edits of the card and its subject, formatting with tables and code blocks,
  reactions, inline pictures, files, Adaptive Cards, deep links, messages up to 80 KB. A catch-up read after a downtime
  (`messages/delta`) loses no reply. Graph allows about 50 sends a minute in one channel, against Telegram's 18.
- **Against:**
  - Teams does not notify a user of their own plain posts, and never shows them as unread. A post that @mentions the
    owner does notify (§15.1 #11), so the alert is an @mention.
  - A card's Submit and imBack buttons have no receiver without a bot; emoji reactions replace them (§15.4).
  - A delete needs `ChannelMessage.ReadWrite`, and an edit with Agentmaster's own app needs it too (administrator
    consent; the tenant blocks user consent, §3). Edits go through the Azure CLI's token.
  - Real-time replies need a public HTTPS endpoint for the change notifications; without one, Agentmaster polls, which
    Graph allows once a day (§2.11). The owner chose polling (TEAMS_CHANNEL.md §7).
  - Every post counts against the owner's own Graph limit of 1 send per second, shared with any other tool that posts
    as the owner.
  - Every post shows as the owner's own message.

## 5. Feature translation

Status: ✅ native or reused unchanged · 🔧 rebuilt with Teams features · ⚠ weaker · ✖ not possible · ❓ unknown until a
test bot measures it (§12). References are to [`SPEC.md`](../claude-code-telegram/SPEC.md).

### 5.1 Setup, identity, settings (SPEC §6)

| Telegram | Teams | Status |
|---|---|---|
| A bot from @BotFather in five minutes; one bot per install | A Teams app with an Entra app and a bot registration per install (`teams app create --messaging-mode socket`); in a locked-down tenant only an administrator can create it (§9) | ⚠ |
| Threaded Mode in the BotFather Mini App | Threads in 1:1 agent chats are a platform preview; the owner's client in public preview | ⚠ |
| Owner = numeric user id; every other sender dropped first (I1) | Owner = Entra object id + tenant id (`from.aadObjectId`, `channelData.tenant.id`); single-tenant app; the same "sender first" rule | ✅ |
| Token in Credential Manager | Client secret in Credential Manager (`Agentmaster:messaging:teams:<install>`); app id and tenant id in settings | ✅ |
| The identity gate (`botId`) | App id + tenant id; a different app resets the thread map | ✅ |
| One poller per token (409) | One app per install + a machine-wide mutex on the app id (§2.1) | ✅ |
| Status line | "Connected (amer, emea, apac ready)", "socket mode not enabled for this bot (403)", "token refused (401)", "Retry-After …" | ✅ |

### 5.2 The chat's layout (SPEC §7)

| Telegram | Teams | Status |
|---|---|---|
| Main view "All" with the pinned directory | The 1:1 chat's top level; the fleet board as an L1 card (§5.12) | 🔧 |
| 🔎 Search topic | An L1 "🔎 Search" root the bot posts once; requests as replies in its thread; `/search <words>` anywhere | 🔧 |
| A topic per session | An L1 session card + its thread | 🔧 ❓ P0-T4 |
| A thread the owner opens starts a session | A top-level message the owner writes starts a session (§5.10) | 🔧 |

### 5.3 Topics are sessions (SPEC §8)

| Telegram | Teams | Status |
|---|---|---|
| Which sessions qualify (non-idle Claude tabs, the idle grace, pins, held tabs) | The same rule decides which sessions get a card and live posts, and which the board lists | ✅ reused |
| Create, rename, Idle card, delete, Rekey (at most 6 per tick) | Create = post the L1 card; rename = edit the card (a thread has no name of its own); Idle = edit; **no deletes** (the thread stays, the board drops the session); Rekey = the same separator line, in the thread | 🔧 |
| Topic name = color square + title, the state as the topic's icon | The card header: state emoji + color square + title | 🔧 |
| Six topic colors, fixed at creation | Cards have six semantic colors (default, accent, good, warning, attention, emphasis) and no hex; the emoji squares stay | ⚠ the same limit |
| The change line after a rename | Not posted (a new message notifies, §6); the card shows the new title | 🔧 |
| The idle notice in the main view | Not needed (no deletes); the board lists idle sessions apart | ✅ simpler |
| MOST RECENT order made by posting; the top-24 re-sort | Thread roots keep their creation order (❓ P0-T6); the board lists sessions in MOST RECENT order with deep links; no re-sort and no re-post cost | 🔧 |
| Recreation, `/recreate` | A new L1 card with the history replayed; the old thread stays | 🔧 |
| The start-up hold | The same rule | ✅ |
| `/pin` (never idle) | The same | ✅ |
| The owner renames or deletes a topic | Nothing to rename; the owner cannot delete the bot's messages. A deleted own message arrives as `messageDelete`, so a queued prompt from it leaves the queue (Telegram cannot see deletes) | ✅ better |

### 5.4 The mirror (SPEC §9)

| Telegram | Teams | Status |
|---|---|---|
| Two modes: conversation only, verbose | The same | ✅ |
| The transcript line classifier | Reused (pure) | ✅ |
| A prompt typed at the PC: the bot's "👤 You" line at the turn's start, its status edited | The card shows "▶ running: «first line»" while the turn runs; the prompt is quoted at the top of the turn's final message — one new message per turn (§6) | 🔧 |
| A prompt from the phone: the owner's message, its status as a reaction | The same, with Teams reactions | ✅ |
| Remarks and the work log (verbose), one message edited every ≥ 15 s | Phone turn: plan steps under the owner's message (one step per tool call, no messages, a native Stop button). PC turn: the card's "now" line, and the turn's tool calls folded into the final message (❓ P0-T5: can a plan bind to the bot's own message?) | 🔧 ❓ |
| The final reply, with sound, quoting its prompt | Extended Markdown in the session's thread, a quoted reply to the prompt; the turn's one ring (§6) | ✅ |
| A message typed mid-turn, a slash command, an answer to a question | Lines in the final message (or plan steps) | 🔧 |
| Secretary mode (the owner's turns as the owner's own messages) | A bot cannot post as the user | ✖ |
| Rich Markdown with tables (`sendRichMessage`, 32,768 characters, 500 blocks) | Extended Markdown, ≤ 80 KB | ✅ better |
| The classic HTML fallback | Not needed | ✅ |
| Title chips (URL buttons to a topic) | Markdown links and card buttons with deep links (`https://teams.microsoft.com/l/message/<chatId>/<messageId>?context={"contextType":"chat"}`); they open in the Teams app on every client (Telegram Android cannot open a topic) | ✅ ❓ P0-T9 |
| `tg-time` (relative times kept by the apps) | No such entity; cards refresh on view, so a card may say "3 min ago"; messages carry clock text | 🔧 |
| The history replay (the whole transcript, ≤ 3,500 characters a message, paced) | The same, packed into ≤ 80 KB messages (about 20 times fewer), paced by the conversation's budget (§7); silent only in a muted chat (§6) | ⚠ ❓ P0-T3 |
| The recap on the card | The same | ✅ |
| Forks, `/clear`, `/resume` | The same rules | ✅ |

### 5.5 The sticky card (SPEC §10)

| Telegram | Teams | Status |
|---|---|---|
| One rich message kept last: deleted and re-posted after other posts; edited at most every 20 s; clock times only | The card is the thread's root: always at the top of its thread and in the chat. An Adaptive Card with `refresh` (`userIds` = the owner) is rebuilt by the bot when viewed, at most once a minute — current with no timed edits and no re-posts | ✅ better |
| The card rings for Done, Error, Needs approval | An activity feed notification with a deep link (§6) | 🔧 |
| Buttons on pages (Main, Session, All Screen), `c:<verb>:<sid>` | `Action.Execute` buttons; the Session and All Screen groups as `Action.ShowCard` (inline, no page-flip edits); JSON data, no 64-byte limit | ✅ |
| Form upgrades | Not needed | ✅ |
| 🖼️ the screen on the card | A card `Image` or an inline image ≤ 1024 × 1024 px and ≤ 1 MB: the screen is scaled or split (❓ P0-T8 legibility) | ⚠ |

### 5.6 Inbound (SPEC §11)

| Telegram | Teams | Status |
|---|---|---|
| The router, sender first; the bot's own service messages deleted | The same check on `aadObjectId`; a bot never receives its own messages, so there is nothing to delete | ✅ |
| Any action marks the session read | The same | ✅ |
| A message is a queued prompt (the one funnel, the delivery rules, the 5 s settle) | Reused | ✅ |
| The status reactions ✍ 👨‍💻 👎 👀 🫡 | Teams reaction IDs for the same emoji; at most 2 reaction calls per second for the whole bot | ✅ ❓ P0-T13 |
| An edit of a queued message replaces it | `messageUpdate` | ✅ |
| `/unqueue n` | Also: delete the message (`messageDelete`) | ✅ better |
| **The backlog rule** (Telegram keeps updates 24 h; older than 10 min → asked) | Teams delivers nothing while Agentmaster is closed (socket mode) or fails the request (HTTPS). The start note says so; a catch-up read needs `Chat.Read.All` or the owner's delegated sign-in (D-T7) | ✖ ⚠ |
| Commands `/word`; the "/" menu by `setMyCommands` | The manifest's slash commands (§2.9); the same router and the same refusal tables | ✅ |
| Buttons: state-reading verbs and single-use tokens | `Action.Execute` `verb` + `data`; the same token rule for confirmations (I5) | ✅ |
| A tap is held until its work is done (≤ 9 s) | The invoke is answered with the updated card within 25 s; longer work answers at once and edits the card later | ✅ |
| Files and photos from the phone are refused | The same (attachments can come later) | ✅ |

### 5.7 Claude Code's dialogs (SPEC §12)

| Telegram | Teams | Status |
|---|---|---|
| `AskUserQuestion` through the PermissionRequest hook and the ask pipe; a card with tabs, a step per tap, `ForceReply` for own words | The same hook and pipe. **One Adaptive Card form:** per question an `Input.ChoiceSet` (radio, or checkboxes for multi-select), an `Input.Text` for own words and one for the note, previews as `CodeBlock`; Submit, Chat about this, Cancel. The same `updatedInput` composer (I14) | ✅ better |
| The 4,096-character card budget and its shortening steps | 80 KB: shortening is rarely needed | ✅ |
| `/add-dir`'s menu card, the default after 60 s | The same, as a card | ✅ |
| The interrupted-turn questions | The same, as card buttons | ✅ |
| Plans and permission prompts are not answered from the phone | The same | ✅ |

### 5.8 Session control (SPEC §13)

| Telegram | Teams | Status |
|---|---|---|
| ▶ Reactivate, 🔄 Restart, ⎋ Esc, 🌙 Deactivate, ⑂ Fork, 🆕 New Session, `/enter`, `/steer` | The same window-hosted actions behind card buttons and slash commands. For a turn started from the phone, the plan's native **Stop** is one Esc (its `409 PlanConflict` tells the bot) | ✅ |
| Every launch is answered | The same | ✅ |

### 5.9 The 🔎 Search topic (SPEC §14)

| Telegram | Teams | Status |
|---|---|---|
| One conversation of the search agent; evidence cards; a pick button per result; pick menus | The same agent; the answer in extended Markdown and one card with a pick button per result; menus as cards | ✅ |
| `/search-tab` moves Search to the top of the list | Not needed: a deep link opens the Search thread | ✅ |
| (none) | Optional later: a search message extension (find a session from any chat's compose box) | ➕ |

### 5.10 A new session (SPEC §15)

| Telegram | Teams | Status |
|---|---|---|
| A wizard of steps (folder lists in pages of ten, model, window), one message edited per step | **One form:** folder `Input.ChoiceSet` with `style: "filtered"` over Recent and Most used (or a dynamic search, `Data.Query`), a typed path, model, window, Launch | ✅ better |
| Creating a missing folder | The same checks behind a Create button | ✅ |
| The opener Telegram Android sends without its thread id | Not applicable | ✅ |
| The bind: the user's thread becomes the session's topic | The owner's top-level message roots the session's thread; the bot's first reply (the form) becomes the session card, or the bot posts a new L1 card and links it (D-T4) | 🔧 |

### 5.11 Pictures and documents (SPEC §17)

| Telegram | Teams | Status |
|---|---|---|
| 🖼 Screen Image (photo ≤ 10 MB, 2,560 px) | Inline image ≤ 1024 × 1024 px, 1 MB: scaled or split in two | ⚠ ❓ P0-T8 |
| 📜 All Screen (tall PNG documents, ≤ 32,000 px a part) | Files through the file consent card (the owner's OneDrive), or the file-link route | 🔧 ❓ P0-T15 |
| The previews' picture (question card) | Previews as `CodeBlock` text in the card; no picture | ✅ |

### 5.12 The directory, `/status`, voice, files (SPEC §16, §18–§20)

| Telegram | Teams | Status |
|---|---|---|
| The pinned directory: stats, tags, recent activity; a dot moves a topic up | The **fleet board** card (refresh on view): stats, tags, recent activity, a deep link per session; the owner pins it once | 🔧 |
| `/status` | The same report, in extended Markdown (its tables render) | ✅ |
| 🎤 Voice notes | Teams mobile records voice messages in chats; the bot receives an attachment (❓ P0-T10: its shape and download); the transcription pipeline is reused | ❓ |
| File links (LAN or hosted) | The same links | ✅ |
| `/download` → 📤 Send as file (≤ 50 MB) | The file consent card (the owner's OneDrive; no 50 MB cap) | ✅ |

### 5.13 Outbox, persistence, lifetime, logs, tests (SPEC §21–§26)

| Telegram | Teams | Status |
|---|---|---|
| One call per 1.05 s; a background budget of 18 calls per minute; long 429s park a method | The per-conversation limits of §2.8 and §7; `Retry-After`; retries of 412, 502, 504 | 🔧 |
| The durable outbox (journal, lanes, breaker) | Reused; Teams answer table | ✅ |
| "message thread not found" → the topic is made again | Not expected: the owner cannot delete the bot's roots (❓ P0-T11: an administrator's retention policy can) | ✅ |
| `state.json` | `<profile>\messaging\teams\state.json`: the app id, the conversation reference (service URL, chat id, the owner's id), the thread map (session → root id), card hashes, plan bindings | ✅ |
| The start note in Search; the run markers | The same; the note also says that messages sent while Agentmaster was closed did not arrive | ✅ |
| `[tg]` transport lines | `[teams]` in `messaging.log` | ✅ |
| A fake Bot API (planned) | A fake ACS: a SignalR socket server and a REST stub (simpler than a fake Bot API) | ✅ |

## 6. One sound per turn on Teams

Telegram sends everything with `disable_notification` except a turn's final message (D5, I4). Teams has no such flag
for a bot (§2.7). Two ways keep the rule:

- **A (recommended): a muted chat and activity-feed pings.** The owner mutes the bot chat once. Agentmaster then sends
  one activity feed notification (Graph `POST /users/{id}/teamwork/sendActivityNotification`, RSC
  `TeamsActivity.Send.User`) per finished turn and per "needs you" moment, with a deep link to the thread and a
  `chainId` per session, so a newer ping replaces the older one. Everything else, the history replay included, arrives
  silently. Cost: the activity-feed limit of 20 per minute per user; the app needs the Graph token of its own app
  registration, which the RSC grant covers (P0-T14).
- **B (fallback, no Graph): one new message per turn.** The chat stays unmuted. Per turn only the final message is new;
  the card (refresh on view), the plan, edits and reactions carry everything else. Each history replay message rings
  too, so the replay runs only on request (`/history`).

## 7. Budgets and pacing

- **T1 is one conversation:** every session shares 60 sends per 30 s and 1,800 per hour (30 per minute on average),
  against Telegram's measured sustained 18 per minute. Edits and plan updates are assumed to count (P0-T7); reactions
  have their own cap, 2 per second for the whole bot.
- **What a turn costs:** 1 final message, 2–4 reactions (phone turns), a few plan updates (phone turns, verbose), 0–1
  card edits (state changes; the refresh on view replaces the rest).
- **The history replay** gets what the live traffic leaves, with a ceiling (for example 600 per hour), and packs 80 KB
  per message.
- **T2 with several channels** multiplies the budget (each channel is a conversation), as MESSAGING.md §22's folder
  groups would in Telegram.
- The pure pacer of the Telegram outbox (call window, background budget) is reused with the Teams numbers.

## 8. Architecture in Agentmaster

### 8.1 Where it runs

- A `TeamsService` beside `MessagingService`: one per process, created by `SharedEngine()`, with the same `ServiceHooks`
  (start, restart, deactivate, Esc, Enter, screen reads, launches) and its own poll, send and tick threads (H1: nothing
  on a UI thread).
- Its own files: `<profile>\messaging\teams\state.json`, `outbox\`, `teams.log` lines in `messaging.log`.
- The cog's Messaging tab gets a Teams section: switch, app id, tenant id, client secret (Credential Manager), owner
  (Entra object id), delivery method, status line.

### 8.2 What is reused

| Part | Files | Reuse |
|---|---|---|
| Platform-neutral decisions | `MessagingSteer.h`, `MessagingSessionControl.h`, `MessagingFromQueue.h`, `MessagingCardPicture.h`, `MessagingFileLinks.h`, `MessagingEnter.h` (3 Telegram-shaped lines of 514) | as they are |
| Models with Telegram renderers | `MessagingModels.h` (the line classifier, the topic sync plan, the command router and its tables, the backlog rule, the card's facts), `AskQuestion.h` (the question model and the `updatedInput` composer), `TuiMenus.h` (the `/add-dir` screen parser), `MessagingNewThreadWizard.h` (folder lists, typed-path and model checks), `MessagingDirectory.h`, `MessagingStatus.h` | the models; new Teams renderers beside the Telegram ones |
| The durable outbox | `MessagingOutbox.h`, `MessagingOutboxJournal.{h,cpp}` | the journal, lanes, back-off and breaker; a Teams answer table |
| Voice, `/status`, pictures, the search agent | `Voice*`, `ClaudeStatus`, `ScreenImage`, `ConversationImage`, `MessagingSearchBridge` | as they are |
| Transport and service | `TelegramApi`, `TelegramFormat.h`, `Messaging.cpp`, `Messaging.Inbound.cpp` | not reused (Telegram only) |

### 8.3 What is new

- `TeamsApi` (WinHTTP): the client-credentials token, the ACS REST calls (send, reply chain, update, delete, reactions,
  plan activities), the Graph call for activity feed notifications, the file consent upload.
- `TeamsSocket` (WinHTTP WebSocket): per region, the connect call, the SignalR negotiation and JSON hub protocol
  (handshake record, `Activity` invocations answered with completions that carry the reply frame, `SocketReady`,
  `SocketClosing`, pings every 15 s, a 30-s server timeout), the replacement before the 60-minute expiry, reconnects
  at 1, 2, 4, 8, then every 15 s with jitter.
- `TeamsRender.h` (pure, tested with goldens): the session card, the fleet board, the question form, the menu card, the
  new-session form, the search answer (Adaptive Card JSON) and the extended Markdown of replies and history; the deep
  links.
- Later, for production: `TeamsEndpoint`, a loopback listener behind the existing Cloudflare route, with the JWT check
  of §2.1.
- Not the external connector contract (MESSAGING.md §12.2): card taps must be answered with a card within 25 s, and the
  rules stay in the process.

## 9. Setup

**The administrator (once):**

1. Registers the bot: `teams app create --name "Agentmaster (<owner>)" --messaging-mode socket` (Teams CLI ≥ 3.1.0), or
   the Developer Portal (Tools → Bot management → New bot; then Configure → Delivery method → Socket mode), or gives the
   owner the Application Developer role so the owner can do it.
2. Makes the app installable for the owner: custom app upload in the owner's app setup policy, or the app uploaded to
   the organization's catalog and allowed by the permission policy.
3. Allows Teams public preview for the owner (threads in 1:1 agent chats, plans).

**The owner:**

4. Edits the manifest: personal scope; `supportsTargetedMessages: true` and `commandLists` with slash triggers; RSC
   `TeamsActivity.Send.User`; `supportsFiles: true`. A developer-preview manifest, if a preview feature needs it, is
   installed by direct upload.
5. Installs the app (the bot's 1:1 chat appears), turns on public preview in the Teams client, mutes the bot chat
   (option A of §6), pins the fleet board when it appears.
6. In Agentmaster's cog: app id, tenant id, client secret, owner id, socket mode, Save. Agentmaster connects in three
   regions; the first message from the owner (or the installation event) gives the conversation reference.

## 10. Security, privacy, compliance

- **The owner rule:** every activity is checked by `from.aadObjectId` and the tenant before anything else of it is read
  (I1). Socket mode authenticates ACS once per socket; the HTTPS option checks the JWT before it parses the body.
- **Secrets:** the client secret in Windows Credential Manager, per install; never in a file, a log or a commit.
- **The data stays in the company tenant.** Every mirrored prompt and reply becomes Microsoft 365 data of the company:
  retention policies, eDiscovery, DLP scanning and administrator access apply. For work sessions this is an
  improvement on Telegram's cloud chats; for a personal session it is not (D-T8). A DLP block of a message is a
  "refused for good" answer in the outbox.
- **A remote prompt is remote code execution on the PC** (sessions run with `--dangerously-skip-permissions`), as with
  Telegram. Redaction (SPEC §24.2) is still recommended.
- **Least privilege:** personal scope only; RSC `TeamsActivity.Send.User` only; no tenant-wide Graph permission unless
  D-T7 asks for `Chat.Read.All`.

## 11. Teams features with no Telegram counterpart (later)

- A search message extension: find a session from any chat's compose box.
- Prompt starters for common requests.
- Streaming of a reply to a message sent from the phone, while it fits the 10-minute stream.
- HTML widgets (preview): an interactive terminal view in the chat, once they work without an HTTPS endpoint.
- Agentic users (preview): an agent with its own Entra account that can join channels and meetings.

## 12. P0 — measure with a test bot

| # | Question | How |
|---|---|---|
| P0-T1 | Socket mode for this tenant: connect in three regions, activity delivery latency, the 60-minute replacement | a probe that logs every frame |
| P0-T2 | Does each bot message in the 1:1 chat notify (desktop, Android, iOS)? Does an edit? Does a reaction? | the probe posts each kind; the owner reports |
| P0-T3 | 100 messages into a muted chat: any notification, any throttling | the probe |
| P0-T4 | Threads in the 1:1 agent chat with public preview on: how the clients show threads; a proactive L1 card with replies under it; replies from the phone | the probe and the owner's phone |
| P0-T5 | Plans: can a plan bind to the bot's own message (a PC turn)? Do plan updates count in the 1,800 per hour? What does Stop send? | the probe |
| P0-T6 | Order of thread roots and unread markers when an older thread gets a reply (1:1 chat; the owner's private channel for T2) | the probe; the owner's clients |
| P0-T7 | 429 behavior: edits and plan updates against 60 / 30 s and 1,800 / h | the probe |
| P0-T8 | The screenshot at 1024 px: legible? scaled or split? | the probe |
| P0-T9 | Deep links into a 1:1 bot-chat thread (`channelData.chat.id`) on Android, iOS, desktop | the probe posts links; the owner taps them |
| P0-T10 | A voice message from Teams mobile: the attachment's shape and its download by the bot | the owner records one |
| P0-T11 | Edits and deletes of the bot's messages at any age; card refresh: frequency and `userIds` format | the probe over days |
| P0-T12 | What the owner sees when writing while Agentmaster is closed | stop the probe, write, start it |
| P0-T13 | The reactions REST route and the IDs of ✍ 👨‍💻 👌 👎 💔 😴 🙈 👀 🫡 | the probe |
| P0-T14 | An activity feed notification with the app's own Graph token under RSC `TeamsActivity.Send.User`; its deep link | the probe |
| P0-T15 | The file consent card on mobile; a 30 MB file | the probe |
| P0-T16 | An 80 KB extended Markdown message with tables and code on mobile | the probe |
| P0-T17 | The owner's permission to upload custom apps | `teams login` (it prints "Sideloading: enabled" or "disabled") |

## 13. Decisions for the owner

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| **D-T1** | Build Teams at all | Yes, as a second connector, after a test bot passes P0 | Telegram only |
| **D-T2** | The shape | T1, the 1:1 chat with threads; T1′ when threads are off | T2, a private channel |
| **D-T3** | One sound per turn | A muted chat + activity feed pings (§6 A) | Chat notifications, one new message per turn (§6 B) |
| **D-T4** | A new session started from a top-level message | The owner's message roots the thread; the form becomes the card | A new bot L1 card, linked |
| **D-T5** | Where the test bot lives | The company tenant, registered by an administrator (§9) | A separate test tenant where the owner is administrator |
| **D-T6** | Socket mode for daily use (preview, "development and testing") | Yes for the first version | An HTTPS endpoint behind the Cloudflare route |
| **D-T7** | Messages sent while Agentmaster is closed | Accept the loss; the start note says so | `Chat.Read.All` (administrator), or the owner's delegated sign-in, for a catch-up read at start |
| **D-T8** | Which sessions go to the company tenant | All Claude tabs, as in Telegram | Only sessions in chosen folders |
| **D-T9** | A bot-less connector in the owner's channel (T4, §15) | **DECIDED 2026-11-18: build it first, with no administrator ever** — [`TEAMS_CHANNEL.md`](TEAMS_CHANNEL.md): emoji reactions as the buttons, a pinned legend, an @mention as the alert, polling for the replies | Wait for a test bot (D-T5) |

## 14. Sources (read 2026-11-18)

- Threads: [Use threads in channels and one-on-one agent chats](https://learn.microsoft.com/microsoftteams/platform/bots/how-to/conversations/agent-chat-threads).
- Socket mode: [Choose how your agent receives activity data](https://learn.microsoft.com/microsoftteams/platform/agents-in-teams/agent-connectivity), [Configure the delivery method](https://learn.microsoft.com/microsoftteams/platform/agents-in-teams/configure-delivery-methods-for-agents), [Connect an agent to Teams without an SDK](https://learn.microsoft.com/microsoftteams/platform/agents-in-teams/connect-agent-without-sdk).
- Authentication: [Authentication with the Bot Connector API](https://learn.microsoft.com/azure/bot-service/rest-api/bot-framework-rest-connector-authentication).
- Limits: [Rate limiting for agents](https://learn.microsoft.com/microsoftteams/platform/bots/how-to/rate-limit), [Microsoft Graph throttling limits](https://learn.microsoft.com/graph/throttling-limits).
- Formatting and media: [Format agent messages](https://learn.microsoft.com/microsoftteams/platform/bots/how-to/format-your-bot-messages), [Send and receive inline images](https://learn.microsoft.com/microsoftteams/platform/bots/how-to/agents-inline-images), [Stream agent messages](https://learn.microsoft.com/microsoftteams/platform/bots/streaming-ux).
- Cards: [Universal Actions for Adaptive Cards](https://learn.microsoft.com/microsoftteams/platform/task-modules-and-cards/cards/universal-actions-for-adaptive-cards/work-with-universal-actions-for-adaptive-cards), [Universal Action Model](https://learn.microsoft.com/adaptive-cards/authoring-cards/universal-action-model).
- Reactions, plans, commands: [Emoji reactions](https://learn.microsoft.com/microsoftteams/platform/agents-in-teams/agent-reactions), [Plans and steps](https://learn.microsoft.com/microsoftteams/platform/agents-in-teams/integrate-ai/agent-plans), [Slash commands](https://learn.microsoft.com/microsoftteams/platform/agents-in-teams/agent-slash-commands), [Developer announcements](https://learn.microsoft.com/microsoftteams/platform/developer-announcements), [New capabilities for building digital teammates (2026-11-17)](https://devblogs.microsoft.com/microsoft365dev/new-capabilities-for-building-digital-teammates-in-microsoft-teams/).
- Notifications: [Activity feed notifications](https://learn.microsoft.com/graph/teams-send-activityfeednotifications), [Best practices](https://learn.microsoft.com/graph/teams-activity-feed-notifications-best-practices).
- Registration and preview: [Registering an agent](https://learn.microsoft.com/microsoftteams/platform/agents-in-teams/registering-agent-teams-platform), [Teams developer CLI](https://learn.microsoft.com/microsoftteams/platform/teams-developer-cli-overview), [Custom app policies](https://learn.microsoft.com/microsoftteams/teams-custom-app-policies-and-settings), [Preview features](https://learn.microsoft.com/microsoftteams/platform/resources/dev-preview/developer-preview-intro).
- Graph: [Teams API overview: polling requirements](https://learn.microsoft.com/graph/api/resources/teams-api-overview#polling-requirements), [Resource-specific consent](https://learn.microsoft.com/microsoftteams/platform/graph-api/rsc/resource-specific-consent), [Pin a message in a chat](https://learn.microsoft.com/graph/api/chat-post-pinnedmessages), [Deep links to chats](https://learn.microsoft.com/microsoftteams/platform/concepts/build-and-test/deep-link-teams).
- Voice messages: the Teams mobile microphone button in chats (Microsoft Q&A threads, 2026; no official support page found).
- The bot-less shape (§15): [Update chatMessage](https://learn.microsoft.com/graph/api/chatmessage-update), [Change notifications for channel messages](https://learn.microsoft.com/graph/teams-changenotifications-chatmessage), [Microsoft Graph permissions reference](https://learn.microsoft.com/graph/permissions-reference), [Incoming webhooks through Workflows](https://learn.microsoft.com/microsoftteams/platform/webhooks-and-connectors/how-to/add-incoming-webhook), [Microsoft 365 connectors: known issues](https://learn.microsoft.com/microsoftteams/m365-custom-connectors).

## 15. Measured without a bot: the owner's private channel (2026-11-18)

The owner asked which features work in their private channel today. This tenant has no Agentmaster bot (§3), so the
test wrote AS THE OWNER through Microsoft Graph, in the private channel of §3 where the owner is the only member, from
12:35 to 12:50 local time. The scripts are in [`tools/teams-proof/`](../../tools/teams-proof/README.md). Two delegated
tokens took part:

- **The Azure CLI's token.** Its `Group.ReadWrite.All` scope lets it post and edit; Graph accepts that scope "for
  backward compatibility only". A product must not depend on it.
- **The token of an existing tenant app registration.** It holds `ChannelMessage.Send`, `ChannelMessage.Read.All` and
  `Files.ReadWrite`, but not `ChannelMessage.ReadWrite` and not `TeamsActivity.Send`. This is the kind of identity that
  Agentmaster would use.

### 15.1 Results

| # | Telegram feature (SPEC) | Teams form in the channel | Result |
|---|---|---|---|
| 1 | A topic per session (§8) | A root post with a `subject` | ✅ 201 in 0.9 s |
| 2 | The topic name: state icon, color square, title (§8) | PATCH of the root's `subject` (with its body) | ✅ 204 in 2.0 s |
| 3 | The sticky card (§10) | The root post's body, edited in place | ✅ 204. The card stays at the top of its thread, and the channel shows it as the post. |
| 4 | The mirror (§9) | Replies under the root | ✅ 201 in 0.5–1.2 s |
| 5 | Formatting (§9) | HTML: heading, bold, italic, strike, underline, inline code, link, quote, both list kinds, a code block with its language, a table | ✅ All kept. Teams stores a code block as its own `<codeblock>` element. |
| 6 | Status reactions on the prompt (§9) | `setReaction` / `unsetReaction` of 👀, 👨‍💻, 🫡, 👌 | ✅ 204 each, ~0.7 s; the app's token is enough (`ChannelMessage.Send`) |
| 7 | Cards with buttons (§10, §12) | An Adaptive Card 1.4 reply: FactSet, ChoiceSet, a link button, imBack and Submit buttons, a button that opens a local URL | ✅ Stored (201). What a click does: O2. |
| 8 | The screen picture (§17) | An inline PNG (`hostedContents`) | ✅ 201 (21.6 KB, 1024 × 320) |
| 9 | Message size (§9, the history replay) | HTML tables of 20, 40, 80 and 120 KB | ✅ 80,065 bytes accepted; ✖ 120,052 bytes refused (400 `InvalidParameter`). Telegram allows 4,096 characters. |
| 10 | Files (§20) | An upload to the owner's OneDrive and a `reference` attachment | ✅ 201 (a file card) |
| 11 | An alert by @mention | `mentions` | ✅ A post that @mentions the owner NOTIFIES the owner (PC and phone), with high importance too (a red label). A post that @mentions the channel does not, and a plain post does not. Measured with three posts; the owner reported which ones rang. |
| 12 | Deep links (§8) | The `webUrl` of each root | ✅ |
| 13 | Topics in recent order (§8, the quiet re-sort) | The list of roots after a reply to the older one of two threads | ✅ The API lists the thread with the newest reply first. The clients' order: O3. |
| 14 | No lost message while Agentmaster is closed (§11) | `messages/delta` at start (roots), then the replies of each thread | ✅ 200 |
| 14b | Inbound by polling (§11) | The channel's delta read, then the newest replies of each changed thread | ✅ An unchanged channel answers in 573 bytes. A NEW REPLY in a thread, and a REACTION on an old reply, each bring that thread's root into the next delta read; a reaction on a root arrives in the root itself. `replies?$top=N` answers newest first. |
| 15 | Inbound by push (§11) | A change-notification subscription on the channel's messages | ✅ For the app token (`ChannelMessage.Read.All`, the documented permission): only the endpoint's validation failed (an address that cannot answer). The Azure CLI token got the same error, but Graph may check the endpoint first, so that proves nothing for it. Needs a public HTTPS endpoint; not used (the owner chose polling). |
| 15b | Read and react with the Azure CLI token | `GET` messages, replies, delta; `setReaction` | ✖ 403: reading needs `ChannelMessage.Read.All` (its `Group.ReadWrite.All` is not accepted for reading, although the docs list it), reacting needs `ChannelMessage.Send`. It can post and edit only. |
| 16 | Delete (§21, idle topics, the outbox) | `softDelete` | ✖ 403 for both tokens, v1.0 and beta: it needs `ChannelMessage.ReadWrite`. Fallback: edit the message down to one dot (✅ 204). |
| 17 | Edit with the app's own token | PATCH | ✖ 403: needs `ChannelMessage.ReadWrite` (or `Group.ReadWrite.All`) |
| 18 | One sound per turn (§6) | An activity feed notification (`systemDefault`) | ✖ 403 for both tokens: needs `TeamsActivity.Send`, and a Teams app installed for the owner. Not needed: an @mention of the owner rings (#11). |
| 19 | The sender | — | Every post shows as the owner's own message, with no app label. |

- **Scopes the owner cannot get alone.** The Azure CLI cannot get `ChannelMessage.ReadWrite`, `ChannelMessage.Edit` or
  `TeamsActivity.Send` (AADSTS65002: Microsoft's own apps need preauthorization). Agentmaster's own app needs them
  granted, and this tenant blocks user consent (§3), so an administrator must grant them. By Microsoft's default,
  `ChannelMessage.Edit` and `TeamsActivity.Send` need no administrator; `ChannelMessage.ReadWrite` does.
- **Limits (Graph, documented).** A post or a reply: 1 per second per channel and 1 per second per user; an edit or a
  delete: 1 per second per channel. A steady load must stay near 83 % of that, so about 50 sends a minute in one
  channel, against Telegram's 18. Every post as the owner shares the owner's second with every other tool that posts
  as the owner.
- **Test traffic.** The size probes and one delete probe were edited down to one line after the test; the delete
  could not remove them.

### 15.2 What only the owner can check (open)

| # | Check | Expected |
|---|---|---|
| O1 | ~~Does any proof post notify the owner?~~ Answered (#11): an @mention of the owner does; a plain post and a channel mention do not. | — |
| O2 | What Teams shows for a click of the card's imBack and Submit buttons; whether its local-URL button opens the PC's page (a listener on 127.0.0.1 waited 3 hours; nobody clicked) | imBack and Submit fail with no bot; the local URL works on the PC only. Not needed by T4: emoji reactions are the buttons (§15.4). |
| O3 | The order of the two threads in the clients after the reply to the older thread | The thread with the newest reply moves to the end of the channel (posts layout). |
| O4 | A reply from the phone in the thread: Agentmaster reads it with one GET | Works (the same read as #14). |
| O5 | A Workflows flow ("when a Teams webhook request is received") that posts as the Workflows bot. Not needed: the @mention rings (#11). | Microsoft's pages disagree about the private channel ("under development" and "supported"); the chat works. |

### 15.3 Verdict

- **Content: yes.** Each thing the Telegram mirror shows has a working form in the channel, as the owner, with no
  administrator.
- **The sound per turn: yes.** A post that @mentions the owner rings; every other post is silent (#11). This is
  Telegram's rule (one sound per turn) with no bot.
- **Not possible as the owner:** unread markers per thread (the owner's own posts are never unread), card buttons that
  act (emoji reactions replace them, §15.4), refresh on view, plans with Stop, the typing indicator and slash commands.
  The last four need a bot (T1, T2).
- **Needs an administrator:** delete, and edit with Agentmaster's own app (`ChannelMessage.ReadWrite`), and the activity
  feed (`TeamsActivity.Send` plus a Teams app). Edits use the Azure CLI's token instead (#4, #15b).
- **Inbound:** by push, a public HTTPS endpoint; by polling, one delta read per poll (#14b). The owner chose polling.
- So T4 gives the extent of the mirror and of the remote control: the alert (an @mention), the buttons (emoji), the
  answers (replies read by polling). TEAMS_CHANNEL.md is its design.

### 15.4 Actions as emoji reactions, with a pinned legend (measured 2026-11-18)

The owner's idea: every action becomes an emoji reaction on the right message, and a pinned post holds the legend,
like the Telegram directory message. Measured in the same channel:

| Check | Result |
|---|---|
| Which emoji Graph takes as a reaction | All 26 legend emoji, the keycaps 1️⃣–🔟 included (204). Graph checks the Teams emoji set: "A", a word, ⏎ and 🖼 without its variation selector (U+FE0F) are refused (400 "Unicode … is not supported"). |
| The owner's reactions, made in the Teams client | Read back with the emoji, its name ("Keycap one") and the time; the reacting user is the owner's id. |
| Act like a button | Agentmaster removed the owner's 🖼️ (it is the caller's own reaction: Agentmaster signs in as the owner), added 👌 and posted the session's screen in its thread: 3 calls, 3.9 s. The same emoji can be pressed again. |
| Answer a question by a number | The owner's 1️⃣ on the question reply was read; the question was edited to "Answered: 1️⃣". |
| The legend post | Posted by Agentmaster, pinned by the owner by hand. Graph has no pin for a channel (the chat-only `pinnedMessages` answers 400 for a channel) and shows no pin field. An edit of the pinned post (its fleet board) kept its subject. |
| The history of a message's reactions | `messageHistory` is empty for channel messages (v1.0 and beta). Agentmaster compares a message's reactions with the last set it read: an emoji added and removed between two reads is not seen. |
| A trap found on the way | A PATCH that sends the body without the subject CLEARED the root's subject (the topic name). Every edit of a root sends its subject and its body together. |

**The legend** (the pinned post holds all of it; each card ends with a line of the actions that fit its state):

| On | Emoji | Action |
|---|---|---|
| A session card | 🖼️ | Send the screen now |
| | 📊 | Status (model, context, usage) |
| | 🛑 | Esc: stop the running turn |
| | ↩️ | Enter: send the prompt waiting in the box |
| | 📜 | The whole conversation as pictures |
| | 🔀 | Fork the session |
| | 🌙 | Park the tab (asks first during a turn) |
| | ☀️ | Wake a parked tab |
| | 🔄 | Restart (always asks first) |
| A question | 1️⃣ … 🔟 | Pick that option (several for a multi-pick) |
| | ✅ / ❌ | Send the picks / cancel; a text reply is "Other" |
| A confirm reply | ✅ | Yes, within 60 s |
| The owner's queued prompt | ❌ | Take it out of the queue |
| The legend post | ➕ | Start a new session (Agentmaster replies with numbered folders) |

**The rules:**
- **Two sets that never meet.** Every reaction shows as the owner's, Agentmaster's status reactions too (👀 👨‍💻 🫡
  👌 ⚠️). The action emoji are never status emoji, and Agentmaster also keeps a journal of the reactions it set itself.
- **Consume.** After it acts, Agentmaster removes the owner's action emoji and adds 👌, or ⚠️ with a reply that says
  why not.
- **Confirm.** 🔄, and 🌙 during a turn, act only after a ✅ on Agentmaster's confirm reply within 60 s. Microsoft warns
  that "user reactions aren't a reliable or consistent indicator of intent"; one person in a private channel with
  confirms is the case where they are usable.
- **Exact forms.** Agentmaster sends each emoji in its fully qualified form and compares without U+FE0F.
- **Detection:** a reaction reaches Agentmaster the same way as a text reply: by polling, a reaction on a reply brings
  its thread's root into the next delta read, and a reaction on a root arrives in the root itself (§15.1 #14b).

**Open checks:**

| # | Check |
|---|---|
| O6 | Does a reaction alone send an "updated" change notification? Only matters for push, which T4 does not use: by polling, a reaction is seen (#14b). |
| O7 | On the phone: how many taps an action emoji takes (long-press, "+", the recent list) against Telegram's one tap; where the pinned legend shows; whether the pin stays after Agentmaster edits the post. |
| O8 | 📜's tall pictures (TEAMS_CHANNEL.md §8): a conversation part is 1,232 px wide and up to 8,000 px tall, posted inline as the post's own hosted content. Its PNG size against the 2.8 MB cap of one post, and how Teams shows it (whole, or a thumbnail that opens full size) on the desktop and on the phone. |
