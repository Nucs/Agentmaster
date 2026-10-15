# The session link — Agentmaster ⇄ Claude Code through a mod

> **Status: DESIGN (2026-10-16). Nothing is built.** The Phase 0 probes in §3 ran on this machine against
> Claude Code 2.1.291; their tools are in `tools/mod-link-probe/`. The open measurements are §11, the
> decisions for the user are §12.
>
> **The user's ask (2026-10-16):** "design a new integration between agentmaster to claude code sessions via
> mods. Supporting 100+ sessions on a machine efficiently and low-cost. Features like queue and the status can
> all be done through mods. We will ensure claude sessions running in agentmaster are loaded with mods and are
> connected to the agentmaster."
>
> Companions: [`HOOKS.md`](HOOKS.md) (the push path this replaces per session) · [`DELIVERY.md`](DELIVERY.md) +
> [`DELIVERY_PLAN.md`](DELIVERY_PLAN.md) (the typed delivery this replaces) · [`PENDING_INPUT.md`](PENDING_INPUT.md)
> (drafts) · [`COMMANDS.md`](COMMANDS.md) (/handover) · [`OBSERVER.md`](OBSERVER.md) · [`STATE.md`](STATE.md)
> (the direction: hooks phased out) · [`UI_THREAD.md`](UI_THREAD.md) (fleet-scale cost) · the mods in
> `~/.claude/mods` — `prompt-queue/SPEC.md` §14 is the live-verified behavior of `$.prompt.submit` this design
> leans on. The mod API itself: the `plugin-authoring` skill and the `claude-code.d.ts` its engine writes per build.

## 0. Summary

**What it is.** A small mod, `agentmaster-link`, loaded into every Claude Code session Agentmaster launches. It
runs inside claude.exe and sees the session's own events exactly: turns and why they ended, every prompt and who
sent it, questions, permission dialogs, compactions, background work, usage, the unsent draft. It reports them to
Agentmaster over one local HTTP connection. Agentmaster sends back prompts to submit, drafts to fill, turns to
abort and commands to run, and the mod executes them through Claude Code's own API (`$.prompt.submit`,
`$.prompt.fill`, `$.turn.abort`, `$.command.run`) instead of typing into the terminal.

**Why.** Today each session costs Agentmaster work that grows with the fleet, and much of it reconstructs facts
that exist inside the process:
- a PowerShell process per hook, at least two per turn;
- screen reads of every started tab's input box on every scan tick;
- transcript-tail parsing for state, plus a dozen reconcilers for what hooks miss;
- a delivery pipeline that types into a TUI: delivery gate, draft swap, read-back, Enter rescue, lost-send verdict,
  invisible-character mirror.

Inside the process every one of those is an exact event or a direct call.

```
Agentmaster.exe (one per profile)
 ├─ engine: registry · scheduler · scanner · observer · persistence      (exists)
 ├─ LinkServer (new): HTTP/1.1 on <profile>\link\am.sock (AF_UNIX), fallback 127.0.0.1:<port>
 │    per session: token → record · event intake (seq-ordered) · command outbox · held poll
 └─ HooksBridge (exists): named pipe — stays for sessions without the link
        ▲ events (POST, batched)          │ commands (piggybacked replies + a held GET)
        │                                 ▼
claude.exe ×N  ── agentmaster-link mod (in-process, observe-only hooks; acts through $.prompt / $.turn / $.command)
        └─ ConPTY tab (unchanged: the user still types into the real TUI)
```

**What it is not.** It does not replace the transcripts as the record (the Sessions page, summaries, the transcript
overlay and the session database keep reading them). It does not replace the observer's census (external claudes,
orphans), launch, persistence, windows or presets. It does not apply to Codex, which has no mods. And it does not
remove the classic hooks: a session without the link keeps today's behavior.

## 1. What a session costs today

| Mechanism | Per session | Source |
|---|---|---|
| Hook forwarders | A Windows PowerShell 5.1 process per hook event: UserPromptSubmit + Stop every turn, Notification, SessionStart/End, Pre/PostCompact, SubagentStop. 0.31–0.52 s wall each in exec form. | HOOKS.md *Hook invocation* |
| Draft detection | A read of the last ~120 rows of every bound, started tab's buffer, under its read lock, each scanner tick. | PENDING_INPUT.md, CLAUDE.md *Pending-input monitor* |
| State reconciliation | Transcript tail reads for hot sessions every pass, a 30 s resync for cold ones, presence-heartbeat reads, ~12 recon synths. | HOOKS.md *State machine* |
| Delivery | Gate, draft swap (stash, verify, restore), fill → read-back → commit, Enter-retry watchdog, lost-send verdict. | DELIVERY.md |
| Per-tab host shell | `pwsh -NoExit` around claude.exe: ~35 MB commit per tab, 2.5 GB at 72 tabs. | catalog open item, 2026-10-03 |
| claude.exe | ~400–430 MB private / ~225–230 MB working set with no mod; ~535 MB / ~293 MB with the user's three mods (§3). | measured |

The live release fleet at the time of writing: **44 open sessions, 41 of them idle**, 665 tab refs across saved
windows. Memory is dominated by idle claude.exe processes; CPU churn by forwarder spawns and per-tab reads; and
complexity by the typed delivery and the pull heuristics.

## 2. Goals and non-goals

**Goals.**
- **G1 — exact status from inside the session.** A linked session needs no screen read and no transcript-tail
  judgment for its state.
- **G2 — delivery without typing.** Queue sends, Send now and handover briefings go through `$.prompt.submit`;
  standby and restore fills through `$.prompt.fill`; the draft comes from the editor, not the screen.
- **G3 — 100+ sessions.** A linked idle session costs ~0 CPU: no process spawn per turn, one held request every few
  seconds. It adds no measurable memory on top of the mods already loaded (§3, F8).
- **G4 — fail-safe.** A session without the link (an old Claude Code, a policy refusal, a crashed module) behaves as
  today. The pull floor stays.
- **G5 — no surprises for the user.** Their own hooks and mods are untouched, nothing types into a box behind them,
  and their terminal still works as before.
- **G6 — upgradeable** without restarting running sessions, versioned on both sides.
- **G7 — local and authenticated:** a per-launch token, no network exposure.

**Non-goals.** Codex. Claudes running in other terminals (observe-only, as today). Remote, cloud and SDK-hosted
sessions. Replacing transcripts.

## 3. Measured facts (Phase 0, 2026-10-16)

Claude Code 2.1.291 (its host reports `User-Agent: Bun/1.4.3`), headless `-p`, Haiku, `--setting-sources project`
from an empty folder. The probes: `tools/mod-link-probe/`.

| # | Fact | Consequence |
|---|---|---|
| F1 | `$.http.fetch` to `http://127.0.0.1:<port>` works: **4 ms** cold, **3 ms** warm; Bun keeps the connection alive. | TCP loopback is a working transport. |
| F2 | `socketPath` to an **AF_UNIX** socket works on Windows: **6 ms**. The path is capped near 100 bytes. | A socket file in the profile is a working transport; a long profile path needs the TCP fallback. |
| F3 | `socketPath` to a **named pipe** is refused: `$.http.fetch: socketPath \\.\pipe\… refused: a network location is not reached from here`. | The existing `HooksBridge` pipe cannot carry the link; a new listener is needed. |
| F4 | A **12 s** fetch held inside `session.start` returned at 12,019 ms. In-flight time is outside the hook's 10 s budget, but `session.start` is awaited before the first prompt, so the prompt waited 12 s. | Long-polls work. Never await the link inside `session.start`. |
| F5 | A loop started in `session.start` and left running keeps calling `$.http.fetch` and `$.clock.sleep` after the hook returned (a tick every 0.3–0.4 s through the whole turn). | The link's background loop needs no hook to live in. |
| F6 | A plugin from `--plugin-dir` and one from `CLAUDE_CODE_PLUGIN_DIRS` **both load**. | Adding `--plugin-dir` never displaces the user's `claudemd-once`. |
| F7 | Payloads as documented: `turn.complete` {reason `answer`, duration 1,911 ms, usage with the model}; `classic.Stop` {`background_tasks: []`, `session_crons: []`}; `prompt.submit` {origin `sdk` under `-p`}; `turn.start` {turnId}; `session.end` {reason `other`, budget 1,499 ms}. | The state table of §4.4 rests on real fields. |
| F8 | **Memory per claude.exe** (median private / working set over a steady 12 s window, 2–3 runs each): no mod 403–434 / 225–229 MB; one link-like mod 486–517 / 262–278 MB; the same mod with no network 489–509 / 278 MB; the user's 3 mods 535–538 / 292–294 MB; those 3 + the link-like mod 525–550 / 293–299 MB. | The mod runtime costs **~+85 MB private / +50 MB working set** on a process's first mod, network or not. One more mod is noise (+1 MB median). The user's sessions already pay it. |
| F9 | **An open fetch holds a headless session's end until it answers.** In 5 of 5 runs with a 25 s long-poll the process exited ~13–14 s later (~52 s vs 35–40 s), and `session.end` arrived ~40 ms after a poll answered; the runs without a poll exited on time. `HttpInit` has no abort signal. | The held poll must be short and released by the server (§4.3). Whether an *interactive* exit is held too is Q1. |
| F10 | A plugin's `$.store` is one JSON file per plugin id (`<name>@inline` for every `--plugin-dir` folder), shared machine-wide, 4 MiB in all. | The link keeps no per-session data in `$.store`; Agentmaster is the durable store. |

Verified earlier, live, by `prompt-queue` (SPEC §14, Claude Code 2.1.287 and 2.1.296):
- `$.prompt.submit({ text, asUser: true })` starts a turn of its own after the running turn **and its Stop hooks**
  (0.12–0.21 s after a 3 s Stop hook ended). The model reads the text bare.
- The engine draws `› Prompt from the <plugin> plugin` above such a row, and skips the sender's own hooks on it.
- `$.command.run` is refused inside a `prompt.submit` hook ("it would wait on the turn this hook is holding").
- A `/clear` runs `session.end` with the **old** id and fires no `session.start`. The new id shows ~15 ms after the
  chain settles. On 2.1.287 `$.state` starts empty in the new conversation.
- A large paste reads back from `$.prompt.read` only as its `[Pasted text #N +M lines]` stand-in and raises no
  `prompt.edit`; only claude's own submit expands it.
- `$.prompt.fill` under a dialog answers `isFilled: false`.

## 4. Design

### 4.1 Pieces

- **`agentmaster-link`** — one hooks module, observe-only. Every hook returns `next(e)`, none gates anything, and
  it draws no UI unless a feature asks (§4.5, the band). Shipped inside the Agentmaster package, materialized at
  engine init into an immutable `<profile>\mods\agentmaster-link\<version>-<hash>\` (the `launch-settings`
  idiom).
- **`LinkServer`** (engine, one per process like `HooksBridge`) — HTTP/1.1 over an AF_UNIX socket
  `<profile>\link\am.sock`, and over `127.0.0.1:<ephemeral port>` when the socket path is too long (F2) or the bind
  fails. A small Winsock IOCP server: request line, headers, `Content-Length` bodies, keep-alive. It writes
  `<profile>\link.json` (`{pid, socket, port}`), the `bridge.json` idiom.
- **The registry** gains per-session `linkState` (`absent · connecting · live · lost`), the link's token and
  version, and `OnLinkEvent`. For a live link the link is the state source; otherwise hooks + pull, as today.

### 4.2 Connection and identity

**At launch** (`BuildClaudeSpawn` / `BuildClaudeRestartSpec` / `BuildClaudeCommandline` /
`AppendManagedClaudeEnv`, every seam: launch, resume, fork, restart, deactivate-wake, handover, preset):
- the command line gains `--plugin-dir "<profile>\mods\agentmaster-link\<version>-<hash>"`;
- the ConPTY env gains `AM_LINK=<socket path or http://127.0.0.1:port>` and `AM_LINK_TOKEN=<128-bit random>`.

The token is minted per launch, kept transient on `SessionInfo`, never persisted or logged. `AM_SESSION`,
`CCMGR_SESSION_ID` and `WT_SESSION` stay as they are.

**Hello.** In `session.start` the mod starts its loop *detached* (F4) and returns. The loop posts `hello`
{protocol, token, mod version, Claude Code version (`$.session.version()`), session id, cwd, root, model,
isInteractive, surfaces, `WT_SESSION`, `AM_SESSION`, `CCMGR_SESSION_ID`}. Agentmaster checks the token, maps it to
the launch's record, cross-checks `WT_SESSION` against that tab's token, and answers with its settings (hold
length, which features are on) plus any commands already waiting (a restore re-fill, a handover's first message).

**The conversation id moves; the token does not.** After `/clear` or an in-process `/resume`, the mod sends
`switch {from, to, via}`. The new id comes from `classic.SessionStart`'s payload (source `clear` / `resume`), with
short timed looks at `$.session.id()` as the belt, the prompt-queue method. Agentmaster re-keys from the token. This
replaces pairing `SessionEnd(clear)` with the next `SessionStart` on the same ConPTY (`_recentClearEnds`). A fork's
first `SessionStart`, which echoes its source id (CLAUDE.md Gotchas), cannot mislead it: the token belongs to the
fork's launch.

**One conversation in two tabs** (OBSERVER.md §20) stops being ambiguous: each tab's mod has its own token, so every
event names its tab. `PickConversationHost` keeps deciding which tab the Manager shows, but the observer no longer has
to infer which process an event came from.

**Hand-typed claudes in `+` tabs** inherit none of our env (WT regenerates it), so `--plugin-dir` from the shim does
not reach them. That is decision D3:
- leave them on the pull floor, as today; or
- install the mod at user scope as well (the `claude-mods` marketplace idiom). With no token in its env, it reads
  `%USERPROFILE%\.agentmaster*\link.json` and asks each live instance to claim it by `WT_SESSION`
  (`POST /v1/claim`); the instance whose roster holds that tab answers with a token, and others answer 404. In a
  terminal that is not ours it finds no owner and stays dormant: one attempt, no polling.

### 4.3 Transport

**Upstream: events.** `POST /v1/events` with a batch. Each event is `{seq, t, kind, …}`, with `seq` monotonic per
link. The mod flushes state edges at once and coalesces the rest over ~50 ms. It keeps an outbox of 256 events
(oldest non-edge events dropped first) and resends from the last acked `seq`. Agentmaster applies events in `seq`
order and drops duplicates.

**Downstream: commands**, by two routes:
1. **Piggybacked** on the reply to `hello` and to every event POST. The turn-end auto-advance lives here:
   Agentmaster runs `DecideAdvance` on the fresh state inside the handler of the `turn.complete` POST and answers
   with the `submit`. The prompt is queued within milliseconds of the turn's end, and claude starts it right after
   the Stop hooks (§3).
2. **A held poll** `GET /v1/commands?after=<id>&hold=<ms>` for what originates in Agentmaster's UI (Send now,
   abort, fill, compact). It answers the moment a command is queued, else at `hold` with nothing.
   - Because of F9, the default hold is **5 s**: a session's end can be held at most that long, at a fleet-wide
     cost of ~20 tiny requests a second at 100 sessions.
   - **Server-side release:** before Agentmaster closes, parks or restarts a session it answers that session's poll
     with `release`. The mod also stops polling on `command.run` for `/exit` and `/quit`, posting `release` before it
     passes the command on, so the server answers at once. In a non-interactive session it stops after its last
     `turn.complete`. A `/clear` keeps polling: the process and the token live on.
   - If Q1 shows that an interactive exit is not held, the hold rises to 25 s.

**The poll is the link's heartbeat.** A link with a poll pending, or one answered within `hold + 2 s`, is `live`.
The poll's socket closing (claude.exe ended or crashed) marks it `lost` at once, faster than the observer's 2 s
survey.

**Why not the pipe:** F3. **Why AF_UNIX first:** no port to allocate or scan, access by the profile folder's ACL, no
firewall or AV surface. TCP is the fallback for a profile path over ~100 bytes (portable installs) and for a failed
bind.

### 4.4 Status from inside the session

The mod derives the state with a pure, tested reducer over exact facts, and sends edges: `state {state, why,
turnId, since}` plus the facts behind them. For a live link, Agentmaster applies them as given. Events arrive in
`seq` order, so none of `NextSessionStateOrdered`'s stale / too-fast / type-ahead / empty-twin heuristics apply.
Turn events carry `agentId` inside a subagent's loop, and the session's state follows the main loop's alone
(`agentId` absent); subagents count as background work.

| Agentmaster state | Exact signal in the mod |
|---|---|
| **Running** | `turn.start` → `turn.complete`; also after `turn.complete` while `classic.Stop`'s `background_tasks` is non-empty (shells, subagents, monitors, workflows, i.e. work that outlives the turn) |
| **Waiting for you** | `turn.complete` reason `answer` and no background work |
| **Needs approval** | `tool.call` on `AskUserQuestion` / `ExitPlanMode` in flight (before `next(e)` = asked, after = answered), carrying the questions; `classic.PermissionRequest` until that tool's `PostToolUse` / `PostToolUseFailure` / `PermissionDenied` (Q4) |
| **Error** | `turn.complete` reason `error` or `refusal`; `classic.StopFailure` names the kind (rate limit, overloaded, auth, …) and its details |
| interrupted | `turn.complete` reason `aborted`, which feeds the interrupt hold (DELIVERY.md §14) directly |
| compacting | `session.compact` before `next(e)` → after it, with trigger and tokens before/after |
| **Done** | `session.end` with its reason |
| **Idle** | `session.start`, before the first turn |

Facts sent beside the state:
- every prompt with its origin (`composer`, `peer`, `task-notification`, `plugin`, …), so Typed rows are exact, with
  no echo matching;
- the question bit (the reply's last character, the forwarder's rule) from `turn.complete`'s answer;
- usage pushed by `session.measure` (context fill, rate limits, cost), throttled to 1 point of change or 30 s;
- model and effort (`turn.step`, `classic.PostModelSwitch`);
- `session_crons` from `classic.Stop` (a session that will wake itself, §4.8);
- subagents from `$.agent.list()` on demand.

**What Agentmaster keeps:** the registry, the Waiting → Idle decay (it needs "has the user seen it", which is the
app's), the flash and toast rules (now fed a state that does not flap), and the restore-time cut-off rule
(`midTurnAtRestore`). For that last one the mod adds one exact fact: at `classic.SessionStart` with source `resume`,
whether `$.session.messages()` ends on an answered turn.

**What a linked session stops costing:** the scanner's tail-judging synths (`recon-*`) and its hot-session tail
reads, the pending-input screen reads, and the observer's per-session enrichment. The scanner and the observer keep
running for unlinked sessions, the census and the Sessions page's index.

### 4.5 The queue through the mod

**Ownership (D2): Agentmaster owns the queue** — its contents, order, mode (Paused / Auto), the global pause,
templates, persistence and the Prompt Queue pane. The mod is its delivery agent and its eyes. `DecideAdvance` stays
the decision; its inputs become exact.

**A send:**
1. Agentmaster queues `submit {commandId, promptId, text}`, piggybacked or on the held poll.
2. The mod's background loop calls `$.prompt.submit({ text, asUser: true })` (never inside a hook) and remembers
   the `promptId`.
3. The call resolves as that turn starts (§3). The mod pairs it with the `turn.start` whose text is the submitted
   text and sends `ack {commandId, started, turnId}`. Agentmaster marks the prompt `Sent` and echoed in one step.
   The `prompt` event for it carries origin `{kind: 'plugin', name: 'agentmaster-link', asUser: true}`, so it is
   never recorded as Typed.
4. A submit another plugin drops resolves `{drop}` → `ack refused` → the prompt goes back to `Pending` and that
   session's runner pauses (prompt-queue's `dropped` rule). A rejection → `ack failed`, the same handling.

**What a linked session no longer needs:** the delivery gate (a per-session in-flight flag remains), the draft swap,
fill → read-back → commit, the Enter-retry watchdog, the lost-send and reclaim verdicts, and the pickup guard (the
ack replaces it). A plugin's submit never touches the input box, so the user's draft is never at risk.

**The settle window (D6).** The 40 s hold of DELIVERY.md §16 exists because heuristic states flapped. Linked state
does not flap, so a linked session's default becomes 3 s; unlinked sessions keep 40 s.

**Kinds of entries:**
- a `/command` line → `$.command.run`;
- a prompt with images → stays on the typed path (a plugin's submit carries text only);
- **Send now while a turn runs** (D11): `$.prompt.submit` never folds into a running turn, so Send now from the
  Manager queues the prompt as the next turn. An explicit *steer* (fold into the running turn) stays on today's typed
  path. The Manager offers both labels.

**The row label (D5).** Claude Code draws `› Prompt from the agentmaster-link plugin` above each queued prompt.
chat-view already covers prompt-queue's label (it lifts its own drawing over it) and needs one more plugin name
in its list. Alternatively, accept the label as honest provenance. The transcript records the origin either way,
which also makes "queued vs typed" exact in Agentmaster's own summaries.

**prompt-queue inside Agentmaster (D7).** Two queues in one session would confuse. Recommended: prompt-queue stands
down when `AM_LINK` is set, and the link draws the band above the prompt (`+N ▸ next`, resume, ctrl+↑) from
Agentmaster's queue, sent down as `queue` commands. Ctrl+X Enter then queues into Agentmaster's queue
(`enqueue {text}`). Outside Agentmaster prompt-queue works as before.

### 4.6 Drafts and fills

- **The draft.** The mod passes through a `ui.render` hook on `PromptHint`, whose `isDraft` prop flips when the box
  gains or loses text (terminal and desktop surfaces; its cost is Q7). It reports each flip at once. While a
  draft exists, the background loop reads `$.prompt.read()` every 10 s and sends the text when it changed.
  Copy Current Prompt sends a `snapshot` command and takes the draft from the reply. Large pastes read back as their stand-in (§3),
  and the paste journal stays for expanding `[Pasted text #N]`. This replaces `ReadPendingInputDraft` and its clear
  debounce for linked sessions.
  ⚠ The mod does **not** hook `prompt.edit`: that would put a round trip into the mod in the path of every
  keystroke the user types.
- **Fills.** `fill {text, mode}` → `$.prompt.fill` → `ack {isFilled}`. A `false` (a dialog holds the keys) is retried
  on today's 150 / 400 / 1,000 / 2,500 ms ladder. This replaces the bracketed paste plus read-back plus 12 s re-fill
  of the `/handover-standby` lane and the §10 restore re-fill. The fork carry's piecewise fill becomes one fill of
  the expanded text.

### 4.7 /handover through the mod

The mod sees the typed command exactly: `command.run {command: handover | handover-here | handover-standby, args,
origin composer}`, under the names the Commands tab gave them (`hello`'s reply carries them). It also sees each `Write` of a `HANDOVER-*.md` resolve, by
watching `tool.call` for `Write` with a matcher on the file name. At `turn.complete` it posts `handover {command,
args, files[]}`, and `_HandleCommandHandover` runs unchanged from there.

This replaces `CommandWatch`'s transcript-echo parsing, its seal-at-turn-end and 20 s settle, its in-flight probe and
its replay watermark. The successor's first message goes as a `submit` after the successor's `hello`: no 11,500-char
command-line budget and no paste pump. Successors without a link keep today's tiers. A later step (D8) can give the
model a registered tool, `mcp__agentmaster-link__hand_over`, in place of the files.

### 4.8 Fleet economics at 100+ sessions

| Lever | Effect |
|---|---|
| **Per-turn forwarder hooks dropped for linked launches** (D4) | A linked launch's `--settings` keeps SessionStart, SessionEnd, PreCompact and PostCompact (rare events: a canary and the fallback's lifecycle) and drops UserPromptSubmit, Stop, Notification and SubagentStop. **0 PowerShell processes per turn.** If no `hello` arrives within 10 s of the SessionStart canary, the session is `absent` and the pull floor carries it, as it carries a hookless `+`-tab claude today. |
| **Per-tab reads skipped for linked sessions** | No input-box screen reads, no tail judgment, no per-session observer enrichment. |
| **Fewer UI notifies** | The UI thread is the fleet-scale bottleneck (UI_THREAD.md). The link raises one registry notify per state edge; usage is throttled; drafts notify only on the flip (today's rule). |
| **Exact parking** (D9) | The memory lever, since an idle claude.exe holds ~0.4–0.5 GB. A session may be Deactivated automatically when: its last turn ended longer ago than the prompt-cache lifetime (`serverCacheMinutes`, the `ServerCacheStillWarm` predicate), no background tasks, no `session_crons` (parking would kill its wakeups), no question, no dialog, not the focused tab of the active window, and not opted out. Resuming then costs the same re-cache that continuing would; `classic.SessionStart` on resume carries `prompt_cache_likely_expired` and `estimated_cache_write_usd` to show it. At today's 41 idle sessions that is ~20 GB of commit. |
| **No pwsh host per tab** (D10) | ~35 MB × 100 = 3.5 GB. Separate decision: launch `claude.exe` as the ConPTY client and pass a preset's token in that process's env, with no shell prompt left in which to leak it. |

**Steady cost of a linked idle session:** no spawns, no reads; one held request per 5 s (3–6 ms each, F1/F2); no
extra memory beyond the mod runtime the user's sessions already load. A session that loads no other mod pays the
runtime's ~90 MB (F8).

### 4.9 Failure and fallback

| What fails | What happens |
|---|---|
| The mod does not load (Claude Code < 2.1.287, a managed `plugin.register` refusal, a bad folder) | No `hello`; after 10 s `absent`. Hooks (the canaries) + pull carry state; the typed path delivers. |
| `$.http.fetch` refused (an organization's web-fetch policy) | `hello` never arrives → `absent`, as above. |
| The module throws in a hook | Observe-only hooks fail open: the hook is skipped, the session runs on. The mod's heartbeat goes stale → `lost` → pull floor; on reload it re-hellos with the same token and sends a full snapshot. |
| Agentmaster restarts | Its tabs' claudes end with it (ConPTY), so links end with them. On reopen every session gets a new token at its launch. |
| A hot reload of the mod | Versioned, immutable folders mean an update never reloads a running session's module. A reload for any other reason re-hellos; `$.state` keeps the last command id, so nothing is applied twice. |
| Two installs (release and dev) | Each launch's env names its own profile's endpoint; each `link.json` is per profile. |

### 4.10 Security

- **Local only:** AF_UNIX in the profile folder (the user's ACL) or `127.0.0.1`. Nothing listens on another
  interface.
- **A per-launch token** in `x-am-token` on every request. Unknown tokens get 404. The token is never logged,
  persisted or put on a command line.
- **The same-user boundary as today:** the token is in the session's env, so its children (the Bash tool) can read
  it, exactly as any same-user process can write a fake line to today's hook pipe.
- **Commands are only what the user can already do** in that tab: submit, fill, command, abort, compact, toast.

## 5. The mod

- **One module, observe-only.** Hooks: `session.start`, `session.end`, `classic.SessionStart`, `classic.Stop`,
  `classic.StopFailure`, `classic.PermissionRequest`, `classic.PermissionDenied`, `classic.PostModelSwitch`,
  `prompt.submit`, `turn.start`, `turn.complete`, `session.compact`, `session.measure`, `command.run`, and
  `tool.call` only on `AskUserQuestion` / `ExitPlanMode` / `Write` (matchers, so other tool calls never enter the
  mod).
- **Permission dialogs** are rare here (managed launches run with `--dangerously-skip-permissions` unless the cog
  says otherwise). A dialog's end is taken from `classic.PostToolUse` / `PostToolUseFailure` / `PermissionDenied`.
  Those hooks cost one crossing per tool call, so they return at once unless a dialog is open (Q4, Q7).
- **Never in the typing or streaming path:** no `prompt.edit`, and no `turn.step` (a generator there would carry
  every chunk of every response through the mod).
- **No UI by default.** `ui.render` on `PromptHint` (observe `isDraft`) and on `AbovePrompt` (the band) only when
  those features are on.
- **The background loop:** started detached from `session.start` (F4, F5). It flushes the outbox, runs the held poll,
  applies commands, and backs off on failures (1, 2, 4 … 30 s).
- **No `$.store`** (F10). Per-session values in `$.state`: last applied command id, seq, the draft's last reported
  hash. The durable record is Agentmaster's.
- **Version and protocol** in `hello`. The server speaks protocol N and N-1. The mod checks
  `$.session.version()` and disables features its build lacks.
- **Tests** with `claude plugin test`: the reducer's table, the outbox, command application through a mocked
  `$` (the kit's `mock.clock`), and the band mounted on `terminal` and `desktop`.

## 6. Protocol v1

| Route | Body → reply |
|---|---|
| `POST /v1/hello` | identity (§4.2) → `{hold, coalesceMs, features, commands[]}` |
| `POST /v1/events` | `{events: [{seq, t, kind, …}]}` → `{acked, commands[]}` |
| `GET /v1/commands?after=&hold=` | — → `{commands[]}` at once when queued, else empty at `hold`; `{release}` to stop |
| `POST /v1/claim` | `{wtSession, sessionId, cwd}` (D3's user-scope install only) → `{token}` or 404 |

**Events:** `state`, `turn.start`, `turn.complete`, `prompt`, `question`, `permission`, `background`, `compact`,
`usage`, `draft`, `model`, `switch`, `handover`, `enqueue`, `ack`, `snapshot`, `bye`.

**Commands:** `submit`, `fill`, `command`, `abort`, `compact`, `toast`, `queue` (band contents), `snapshot` (send
everything again), `release`.

## 7. Changes in Agentmaster

| Where | Change |
|---|---|
| `AgentMaster/LinkServer.{h,cpp}` (new) | The listener (AF_UNIX + TCP fallback), HTTP parsing, token table, per-session outbox and held polls, `link.json`. |
| `Engine.cpp` | Start it beside `HooksBridge`; materialize the mod folder; `[link]` log lines. |
| `ClaudeSpawn.{h,cpp}` | `--plugin-dir` + `AM_LINK` / `AM_LINK_TOKEN` at every launch seam; a "linked" variant of `BuildHooksSettingsJson` (D4); the shim adds `--plugin-dir` too. |
| `SessionRegistry` | `OnLinkEvent`; `linkState`; link-authoritative state; `switch` re-keying; Typed records from `prompt` events; acks → Sent / rollback. |
| `Scheduler` | Unchanged decision; a link submitter registered per linked session (`SetPromptSubmitter`); the piggyback path for turn-end advances; per-session settle (D6). |
| `SessionScanner` / `TerminalPage._ScanPendingInputImpl` / `ProcessObserver` | Skip linked sessions' tail judgment, screen reads and enrichment; census unchanged. |
| `CommandWatch` | Accept `handover` events from the link; transcript detection stays for unlinked sessions. |
| Settings cog | SESSION LINK: Off / Shadow / On (the session database's idiom); hold; parking (off, after N minutes); band on/off. |
| CLI | `show` gains `link: live · absent · lost (mod <v>, protocol <n>)`. |

## 8. Rollout

| Phase | Scope | Done when |
|---|---|---|
| **P0** | The remaining probes (§11). No product code. | Each Q answered with a number. |
| **P1 — Shadow** | LinkServer, `hello`, `state`, `prompt`, `draft`, `usage` events. Agentmaster logs `[link-diff]` wherever the link's state differs from today's; behavior unchanged, hooks unchanged. | A week on the dev install with every diff explained. |
| **P2 — State** | Link-authoritative state for live links; the per-tab skips of §4.4; linked launches drop the per-turn forwarders (D4). | HOOKS.md's live checks pass; ~0 forwarder spawns per linked turn. |
| **P3 — Delivery** | `submit`, `fill`, `command`, `abort` through the link; the piggybacked advance; linked settle. | DELIVERY.md's incidents replayed on linked sessions: each impossible or passing. |
| **P4 — Handover, band, usage** | §4.7; the band (D7); context and cost on cards. | /handover family live checks pass through the link. |
| **P5 — Fleet** | Parking (D9); the pwsh host (D10); a 100-session soak. | CPU, memory and UI-thread budgets of §11 Q8 met. |

## 9. Tests

- **Mod:** `claude plugin test` (above); `claude plugin validate --strict` in CI against the pinned Claude Code.
- **Engine harness:** HTTP parsing (split reads, keep-alive, bad lengths), tokens, outbox ordering and resend dedupe,
  held poll and release, state application, the 10 s absent timer, `switch` re-keying, ack → Sent / rollback.
- **Live:** the `claude-wrap-ConPTY` driver against a real TUI for each event row of §4.4, and a 100-session run on a
  test profile.

## 10. Risks

| Risk | Mitigation |
|---|---|
| The mod API is early access and moves between releases. | Minimum version gate; `validate` + `test` on every Claude Code update (the cli-patcher check's moment); feature detection; the hooks + pull floor stays. |
| An open poll holds an exit (F9). | 5 s hold, server release, mod-side release on exit commands; Q1. |
| The engine changes `$.prompt.submit`'s label or timing. | The typed path stays available per session; acks show it at once. |
| A session that loads no other mod pays ~90 MB. | Already paid by every session on this machine; parking dwarfs it. |
| A policy refuses user-tier mods (the work machine's managed policy bypasses some `classic.*` events for user-tier mods). | `absent` → today's behavior; the link only ever adds. |
| A silent mod failure leaves stale state. | The poll heartbeat marks `lost` within `hold + 2 s`; the pull floor resumes. |

## 11. Open measurements (P0)

- **Q1** — Is an **interactive** exit held by an open poll: `/exit`, Ctrl+C twice, closing the tab (CTRL_CLOSE, 5 s
  to the OS kill), and an Agentmaster Deactivate? This sets the hold length.
- **Q2** — The idle cost over an hour, linked vs unlinked: claude.exe CPU time and wake-ups.
- **Q3** — `$.prompt.submit` limits: 100 KB and 1 MB texts, invisible characters, CRLF; the label on 2.1.291.
- **Q4** — Permission-dialog events in a non-bypass session: `classic.PermissionRequest`'s timing and what resolves
  it.
- **Q5** — `/clear`, `/resume` and fork: `classic.SessionStart`'s `session_id` and the order against `session.end`.
- **Q6** — `$.prompt.fill` with large and multi-line text, under a dialog, and in the fullscreen renderer.
- **Q7** — The cost of an observe-only `ui.render` on `PromptHint` (a crossing per draw).
- **Q8** — 100 linked sessions on a test profile. Budgets to meet:
  - LinkServer under 2% of a core;
  - zero forwarder spawns;
  - no UI-thread stall over 250 ms caused by the link;
  - the request rate at most hold-bound plus events.
- **Q9** — Every run of §3 again on the build being shipped (`tools/mod-link-probe`).

## 12. Decisions for the user

| # | Decision | Recommendation |
|---|---|---|
| D1 | Transport | AF_UNIX in the profile, TCP loopback fallback (F1–F3). |
| D2 | Who owns the queue | Agentmaster; the mod delivers and reports (§4.5). |
| D3 | `+`-tab claudes | Start with the pull floor; add the user-scope install + `claim` later. |
| D4 | Hooks for linked launches | From P2: keep SessionStart / SessionEnd / Pre/PostCompact, drop the per-turn four. |
| D5 | The `› Prompt from …` label | Cover it in chat-view (one name more); else accept it. |
| D6 | Settle for linked sessions | 3 s (unlinked keep 40 s). |
| D7 | prompt-queue inside Agentmaster | Stands down on `AM_LINK`; the link draws the band from Agentmaster's queue. |
| D8 | Handover by tool | Later; P4 observes command + writes. |
| D9 | Automatic parking | Off by default; when on, after the cache lifetime, with the §4.8 conditions. |
| D10 | The per-tab pwsh host | A separate decision (P5). |
| D11 | Send now during a turn | "Send next" through the link by default; "Steer now" stays typed. |
| D12 | Settings | Off / Shadow / On; ship as Shadow first. |

## 13. What stays the same

Transcripts and everything that reads them; the Sessions page, summaries and the transcript overlay; the session
database; the observer's census, External rows and the orphan reaper; launch, presets, resume, fork, restart,
windows and persistence; Codex; the CLI's offline reads; and every unlinked session, which keeps exactly today's
hooks, scanner and typed delivery.
