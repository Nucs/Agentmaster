# Agentmaster — Claude Code Hooks Bridge

Session **state** (Idle / Running / Waiting-for-you / Needs-approval / Error / Done) is
derived from **Claude Code hooks**, not by parsing the terminal. Hooks are the authoritative
**PUSH** path: low-latency and exact, where the Ink TUI's redraw stream is neither
(Correctness Rule #1/#7 — "state is hook-derived, never screen-scraped").

This is the floor's *fast* layer. Beneath it sit two **PULL** layers that never set state on
their own: the `SessionScanner` transcript-tail reconciler (a backstop that synthesizes a
`Stop` from the on-disk transcript) and the **Fleet Observer** (out-of-band PEB/transcript
survey — see [`OBSERVER.md`](OBSERVER.md)). The observer *enriches* facts but **NEVER** writes
`SessionState` (Rule #13); hooks remain authoritative for state. A hooked session and an
un-hooked one converge on the same record — the difference is latency.

## Session correlation

When the Manager spawns a session it sets two environment variables on the child `claude.exe`
(`ClaudeSpawn.cpp`, `BuildClaudeSpec`):

```
CCMGR_SESSION_ID=<guid>     # == Claude's own --session-id  (the correlation key)
CCMGR_HOOK_PIPE=\\.\pipe\agentmaster.<pid>   # where to post (the live bridge)
```

`CCMGR_SESSION_ID` is the GUID passed to claude as `--session-id`, so for a session's **first**
conversation the hook payload's `session_id`, the wire `sessionId`, and the `SessionInfo.id` in
the registry are **one value**. The forwarder resolves the id **payload-first**: the hook
payload's `session_id` (Claude's *current* conversation) → else `CCMGR_SESSION_ID` (the
launch-time fallback, and the only id a session we did **not** launch has when its payload lacks
one). Payload-first is load-bearing because Claude's conversation id can **diverge** from the
launch id — `/resume` (into another conversation), `/clear`, and `/compact` all switch the active
`session_id`, while the env (and the cmdline `--session-id`) stay **pinned at spawn**. Env-FIRST
stranded such a session: every post-divergence hook fed the dead launch id, which owns no
transcript of its own, so the missed-`Stop` reconciler (it reads a terminal `stop_reason` from
the transcript tail) could never release it — it stuck in the last mis-attributed state (a
permission `NeedsApproval` that never cleared, or `Idle` while the real conversation ran). For a
normal spawn the first conversation's payload id == our `--session-id` == the env, so this is a
no-op until a real divergence. `cwd` rides every record as a secondary key (the `M` working
directory).

For sessions the Manager did not spawn (a hand-typed `claude` in a `+` tab), two discovery
fallbacks make it self-wire:
- **Pipe:** if `CCMGR_HOOK_PIPE` wasn't inherited, the forwarder reads `~/.agentmaster/bridge.json`
  (`{ "pid", "pipe" }`, written by `WriteBridgeDiscovery` at engine start) to find the live bridge.
- **Adoption key:** the forwarder echoes `$env:WT_SESSION` as the wire's **`tabToken`** field, so
  the app can match the hook back to a live ConPTY (`ITerminalConnection::SessionId()`) and bind a
  stdin injector — promoting an observed session to full observe **+ control** (Rule #9).

> ⚠️ **The hook fast-path is degraded for `+`-tab claudes.** WT regenerates a `+` tab's child env
> from the registry (`til::env::regenerate`), dropping the runtime-only `CCMGR_*` vars and the PATH
> shim — so a bare `claude` typed there fires **zero** hooks. A Manager-*Launched* session is
> unaffected (it sets env explicitly via `spec.env`). This degradation is *why* the Fleet Observer
> exists: it keys binding on the roster correlation, not on the shim/pipe env (Rule #13).

## Transport

There is **no JSON on the wire** and no network. One hook invocation produces one **UTF-8,
newline-terminated, TAB-separated** record posted to a **local named pipe** the app hosts
(`\\.\pipe\agentmaster.<pid>`, `HookPipeName()`). Keeping the wire format flat means the native
bridge needs no JSON dependency; the **PowerShell forwarder** — which *does* have
`ConvertFrom-Json` — parses Claude's hook JSON from stdin and emits the few flat fields.

- **Forwarder:** `agentmaster-hook.ps1` (authored by `BuildForwarderScript`, written by
  `MaterializeSharedHookFiles`). Pure-ASCII PowerShell, best-effort throughout — any failure is
  swallowed so a hook never breaks the Claude turn. A bounded `Connect(1000)` means a stale
  discovery file fails in ~1 s rather than stalling the turn; with the app running the local pipe
  answers in <50 ms.
- **Bridge:** `HooksBridge` (`HooksBridge.{h,cpp}`) — a named-pipe server (`Start`/`Stop`/`_Worker`,
  4 worker instances) created in `Engine.cpp` with `SessionRegistry::OnHookEvent` as its sink;
  it logs `[engine] bridge listening on \\.\pipe\agentmaster.<pid>` at startup. **Exactly one
  bridge per process** (M9 `SharedEngine` — the `<pid>` pipe is unambiguous *because* there is one
  bridge shared by every window).

### The wire line

```
event \t sessionId \t cwd \t isQuestion \t permission \t tool \t tabToken \t prompt \t ts \n
```

| # | Field | Notes |
| --- | --- | --- |
| 1 | `event` | `SessionStart` / `UserPromptSubmit` / `Notification` / `Stop` / `SubagentStop` / `SessionEnd` |
| 2 | `sessionId` | Claude's **current** conversation id (payload `session_id`; == `--session-id` == `CCMGR_SESSION_ID` until `/resume`/`/clear`/`/compact` diverge it) (**required**) |
| 3 | `cwd` | the session's working directory |
| 4 | `isQuestion` | `1`/`0` — set on `Stop`: did the agent's last message end in `?` (question-guard) |
| 5 | `permission` | `1`/`0` — a `Notification` requesting tool permission (vs. an idle notice) |
| 6 | `tool` | associated tool name, when applicable |
| 7 | `tabToken` | the hosting `WT_SESSION` GUID (for adopting a hand-typed `claude`) |
| 8 | `prompt` | **escaped**; set ONLY on `UserPromptSubmit` (so the Flight Plan records *every* message a session got) |
| 9 | `ts` | the hook's **fire time** (unix ms, UTC) — the event-ORDERING key (see *State machine*) |

Fields 1–2 are required; the rest are optional (a tolerant parser accepts older/edge forwarders
that omit the tail). `cwd`, `tool`, and `tabToken` are assumed free of TAB/newline (true for
Windows paths, Claude tool names, and a plain `WT_SESSION` GUID). The **`prompt`** is the
one field that can carry TAB/newline, so it is **escaped** — `\` → `\\` first, then tab/CR/LF →
`\t \r \n` — by **both** the forwarder and `BuildWireLine`, and un-escaped on parse. The
PowerShell escape (`-replace '\\','\\'` first, then the whitespace replaces) must mirror
`WireEscape` byte-for-byte; this is the one seam the C++ tests can't cover (verify with a parity
check — see Gotchas in `CLAUDE.md`).

The trailing **`ts`** is the hook's **fire time** (`[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()`,
stamped FIRST in the forwarder — before the stdin read and the Stop path's transcript work — and
mirroring C++ `NowMs()`). It exists because hooks are independent fire-and-forget processes whose
arrival order is NOT their fire order: the `Stop` forwarder tails the transcript before connecting,
so turn N's `Stop` can land *after* turn N+1's `UserPromptSubmit` — the ordered state machine
(below) uses `ts` to detect that. Appended LAST deliberately: an old 8-field line parses with
`ts == 0` (the registry then falls back to arrival order), and an old parser ignores the extra
field — both skew directions across a deploy stay compatible. `ts == 0` also keeps a non-numeric /
overlong value out (the parser is digits-only + bounded). The canonical format + the
(de)serializers live in **`HookWire.h`**: `BuildWireLine` / `ParseWireLine` / `WireEscape` /
`WireUnescape` — shared verbatim by the bridge and the unit tests so the format has one source of
truth.

## Events consumed

The materialized `hooks-settings.json` (`BuildHooksSettingsJson`) registers the forwarder for
**six** events:

| Hook | Meaning | Effect in Agentmaster (`NextSessionState`) |
| --- | --- | --- |
| `SessionStart` | session began | create/confirm `SessionInfo`, state → `Idle` |
| `UserPromptSubmit` | a prompt was submitted (by user or by us) | state → `Running`; record the prompt in the Flight Plan; recognize + suppress the echo of a prompt **we** injected (idempotency) |
| `Notification` (permission) | tool-permission prompt | state → `NeedsApproval`; run **Approval Policy** (NOT the prompt queue). A plain idle notice leaves state unchanged |
| `Stop` | main agent finished the turn | state → `WaitingForInput`; **Autopilot.tryAdvance()**; carries `isQuestion` for the question-guard |
| `SubagentStop` | a subagent finished | informational only (state unchanged) |
| `SessionEnd` | session ended | state → `Done`; stop Autopilot |

**`PreToolUse` / `PostToolUse` are defined in the protocol but intentionally NOT registered**
(`HookEvents.h`): the session is already `Running` between `UserPromptSubmit` and `Stop`, and
hooking every tool call would spawn a forwarder *per tool* (latency). `ParseHookEvent` /
`NextSessionState` still handle them (both → `Running`) so they can be enabled later for activity
heartbeats or `PreToolUse`-based auto-approval, but the shipped config does not emit them.

## State machine

`NextSessionState(current, msg)` (`HookEvents.h`) is **pure** — it depends only on the current
state and the incoming event, so the single most correctness-critical piece is unit-tested
standalone. It encodes Correctness Rule #1 — "waiting" is three distinct states, and only a clean
`Stop` produces `WaitingForInput`:

```
SessionStart      -> Idle
UserPromptSubmit  -> Running
Pre/PostToolUse   -> Running          # if ever enabled (not registered today)
Notification      -> NeedsApproval if permissionRequest, else unchanged
Stop              -> WaitingForInput  # question-guard (M7) Holds the next prompt if isQuestion
SubagentStop      -> unchanged
SessionEnd        -> Done
Unknown           -> unchanged
```

**The ordered layer — `NextSessionStateOrdered(current, msg, turns)`** (`HookEvents.h`, applied by
`OnHookEvent`; `TurnAccounting` lives on `SessionInfo.turns`, transient). The base machine is pure
on (state, event) and cannot see two real-world facts, both of which used to leave a session
showing `WaitingForInput`/`Idle` for an ENTIRE turn (the "second turn never shows Running" bug):

- **Stale `Stop`** — hooks are independent processes; the `Stop` forwarder does transcript work
  first, so turn N's `Stop` can arrive after turn N+1's `UserPromptSubmit` and would flip the
  Running turn back to `WaitingForInput`. A non-quiescent `Stop` whose wire `ts` predates the
  newest `UserPromptSubmit` (`turns.lastPromptUnixMs`) is **stale**: state is kept, and its
  question bit + Autopilot advance are suppressed (it described an older turn).
- **Type-ahead** — Claude Code fires `UserPromptSubmit` at **Enter-time** for a prompt typed while
  a turn is still running (measured live: 21 `UserPromptSubmit` vs 4 `Stop` on one heavy session),
  queues it, then consumes the queued batch as the next turn with **no further hook**. A
  `UserPromptSubmit` landing while `Running`/`NeedsApproval` increments `turns.queuedPrompts`; the
  next non-quiescent `Stop` **consumes** the batch (`queuedPrompts -> 0`) and stays `Running` —
  no `turnComplete`, so Autopilot does not inject into the already-starting turn (Rule #1's one
  prompt per turn).
- **Quiescent `Stop`** — the scanner's synthesized missed-Stop (`HookMessage::quiescentStop`,
  never on the wire) comes from a ≥2 s-quiet transcript whose tail carries a **terminal**
  stop_reason (`IsTerminalStopReason`: `end_turn` / `stop_sequence` / `max_tokens` / `refusal`
  — gated on `end_turn` alone, a turn ended any other way stayed Running forever), so it is
  authoritative "idle NOW": it always lands `WaitingForInput` and zeroes the accounting,
  regardless of `ts` or a recorded type-ahead (consumed or canceled by then). The synthesized
  `Stop` now also fires on a **user-interrupt** tail (`IsUserInterruptMarker` — Esc kills a turn
  with no clean `Stop`, and the marker clears the stop_reason) and from **`NeedsApproval` as well
  as `Running`** (`ShouldSynthesizeStop`), so an approved/answered session whose post-turn `Stop`
  was dropped is released to `WaitingForInput` instead of stranding in `NeedsApproval`.
- **Blocked-on-user (synthesized `NeedsApproval`)** — an UNANSWERED **interactive** tool_use
  (`AskUserQuestion`, surfaced by `ParseTranscriptDelta`'s `toolName` + `IsInteractiveTool`) on a
  quiescent transcript is the agent waiting for *you*, not working — so `ShouldSynthesizeBlockedOnUser`
  synthesizes a permission-style `Notification` → `NeedsApproval` (`[recon-block]`), from `Running`
  only (idempotent). A pending **non-interactive** tool (a long `Bash`) stays `Running`. A
  `ToolResult` marker (the question was answered) or a new prompt clears the block; the turn's
  terminal/interrupt tail then exits it (above).

Self-healing by construction: every applied `Stop` zeroes `queuedPrompts` (drift cannot
accumulate; a phantom `UserPromptSubmit` — e.g. a slash command that never starts an API turn —
costs at most one wrong-`Running` interval, which the scanner's quiescence reconciliation ends),
`SessionStart`/`SessionEnd` reset the accounting, and `ts == 0` events (an old forwarder) degrade
to plain arrival order. Only a `turnComplete` transition fires the Autopilot advance; the wire
`ts` also feeds `lastActivityUnixMs` (monotonic — a stale event can't regress the
WaitingForInput→Idle decay anchor, which real hooks previously never refreshed at all).

On any hook `OnHookEvent` also sets `s.hookWired = true` and `s.lastHookUnixMs` (provenance for
the observer — see below), remembers the `tabToken`, and — on `SessionStart` for an id it doesn't
know — creates a minimal `external`, `live` record and fires the **adoption** handler (which tries
to bind it to its ConPTY). Any *other* event for an unknown id is ignored (there is no connection
to bind).

### Forwarder-side enrichment

Two fields are computed in the forwarder, not just relayed:

- **`isQuestion`** (on `Stop`): the forwarder tails the transcript (`$j.transcript_path`, last
  ~60 lines), finds the last `assistant` text message, trims it, and sets `1` if it ends with `?`.
  This feeds the **question-guard**: Autopilot **Holds** the next queued prompt rather than
  auto-answering a clarifying question.
- **`permission`** (on `Notification`): set `1` when the notification `message` matches
  `(?i)permission|approve|allow|grant` — distinguishing a blocking tool-permission prompt from an
  idle notification.

## How a session gets the hook config

- **Manager-Launched** (`BuildClaudeSpec`): claude is spawned with
  `--settings <~/.agentmaster/hooks-settings.json>` plus `CCMGR_SESSION_ID` / `CCMGR_HOOK_PIPE` in
  `spec.env`. The settings file (`BuildHooksSettingsJson`) carries the six hook blocks **and** the
  cog's optional Claude-session settings (`model`, `includeCoAuthoredBy`, `permissions.defaultMode`)
  — each emitted only when it differs from Claude's default, so an all-defaults config stays
  byte-for-byte the prior hooks-only file.
- **Hand-typed `claude`** (a `+` tab): a transparent **PATH shim** (`~/.agentmaster/shim/claude.cmd`
  + a POSIX `claude`, authored by `MaterializeClaudeShim`) injects `--settings <ours>` then execs
  the real claude (resolved *before* the PATH prepend so it never finds itself). Combined with the
  `bridge.json` pipe-discovery and the payload `session_id` fallback, a bare `claude` self-wires —
  **when** it inherits our process env. See the degradation caveat above; the Fleet Observer is the
  reliable detection/bind path that needs none of this.

## Scheduler trigger (see IMPLEMENTATION.md / M7, `Scheduler.cpp`)

```
onHook(sid, ev):
  s = registry[sid]
  Stop:                 s.state = WaitingForInput; tryAdvance(s)
  Notification(perm):   s.state = NeedsApproval;   applyApprovalPolicy(s)   // not the queue

tryAdvance(s):
  if s.autopilot.mode == Off:            return
  if s.state not in {WaitingForInput, Idle}: return   // Rule #1: Idle is also "ready"
  if humanTypedWithin(s, 1500ms):        return        // pauseOnHumanInput
  item = s.queue.firstPending(); if !item: notifyPlanDone(s); return
  if !passesGuard(s, item):  item.status = Held; flagCard(s); return  // e.g. lastMessageIsQuestion
  if s.autopilot.mode == SemiAuto:       askConfirm(s, item); return
  markSentAtomically(item); persist(s)                 // idempotent BEFORE inject (Rule #4)
  s.connection.WriteInput(item.text + L"\r");          // inject + submit
```

Advances fire on **two** triggers — the `Stop` seam *and* observed changes (`OnObserved`) — so a
freshly launched / just-resumed session sitting `Idle` (which never emits a `Stop`) still starts
consuming its queue; a time-bounded pickup guard holds it to one prompt per turn.

## Files

All under **`%USERPROFILE%\.agentmaster\`** (deliberately *not* `%LOCALAPPDATA%` — MSIX
virtualizes a packaged app's LocalCache, but the external `claude.exe` resolves the real path;
they must agree — see Gotchas in `CLAUDE.md`):

| File | Written by | Purpose |
| --- | --- | --- |
| `agentmaster-hook.ps1` | `MaterializeSharedHookFiles` | the PowerShell forwarder Claude invokes |
| `hooks-settings.json` | `MaterializeSharedHookFiles` | the `--settings` config wiring the six hooks |
| `bridge.json` | `WriteBridgeDiscovery` | live-bridge discovery (`{ pid, pipe }`) for the shim |
| `shim/claude.cmd`, `shim/claude` | `MaterializeClaudeShim` | the transparent `claude` PATH shim |
| `hooks.log` | engine | runtime traces (`[engine] bridge listening …`, `[SessionStart]`, `[Stop]`, …) |
