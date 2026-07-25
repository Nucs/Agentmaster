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
(`ClaudeSpawn.cpp` — `BuildClaudeSpawn`, via `AppendManagedClaudeEnv`):

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
- **Pipe:** if `CCMGR_HOOK_PIPE` wasn't inherited, the forwarder reads `<profile>\bridge.json`
  (`{ "pid", "pipe" }`, written by `WriteBridgeDiscovery` at engine start) to find the live bridge.
  The discovery path is baked into the generated forwarder per-profile (`BuildForwarderScript(stateDir)`) —
  a hardcoded `~/.agentmaster` literal would route a dev-profile session's hooks to the release
  instance's bridge (Rule #15).
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
| 8 | `prompt` | **escaped**; set ONLY on `UserPromptSubmit` (so the Auto Testing records *every* message a session got) |
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
| `UserPromptSubmit` | a prompt was submitted (by user or by us) | state → `Running`; record the prompt in the Auto Testing; recognize + suppress the echo of a prompt **we** injected (idempotency) |
| `Notification` (permission) | tool-permission prompt | state → `NeedsApproval`; run **Approval Policy** (NOT the prompt queue). A plain idle notice leaves state unchanged |
| `Stop` | main agent finished the turn | state → `WaitingForInput`; **Tests Autorunner.tryAdvance()**; carries `isQuestion` for the question-guard |
| `SubagentStop` | a subagent finished | informational only (state unchanged) |
| `SessionEnd` | session ended | state → `Done`; stop Tests Autorunner |

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
  question bit + Tests Autorunner advance are suppressed (it described an older turn).
- **Type-ahead** — Claude Code fires `UserPromptSubmit` at **Enter-time** for a prompt typed while
  a turn is still running (measured live: 21 `UserPromptSubmit` vs 4 `Stop` on one heavy session),
  queues it, then consumes the queued batch as the next turn with **no further hook**. A
  `UserPromptSubmit` landing while `Running`/`NeedsApproval` increments `turns.queuedPrompts`; the
  next non-quiescent `Stop` **consumes** the batch (`queuedPrompts -> 0`) and stays `Running` —
  no `turnComplete`, so Tests Autorunner does not inject into the already-starting turn (Rule #1's one
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
to plain arrival order. Only a `turnComplete` transition fires the Tests Autorunner advance; the wire
`ts` also feeds `lastActivityUnixMs` (monotonic — a stale event can't regress the
WaitingForInput→Idle decay anchor, which real hooks previously never refreshed at all).

On any hook `OnHookEvent` also sets `s.hookWired = true` and `s.lastHookUnixMs` (provenance for
the observer — see below) and remembers the `tabToken` (the stable ConPTY identity it reconciles
against, kept fresh on *every* hook so it survives an in-session `/resume`). A `SessionStart` for
an id it doesn't know creates a minimal `external`, `live` record; any *other* event for an unknown
id is ignored (there is no connection to bind). The **adoption** handler then fires on **every**
`SessionStart` — not only when this call created the record — so the app can (a) bind a newly-seen
hand-typed `claude` and (b) **re-home** a tab whose claude switched conversation id via `/resume`
(the id changes, the `tabToken` does not); the reconcile is idempotent (an already-bound session
fast-returns).

### Forwarder-side enrichment

Two fields are computed in the forwarder, not just relayed:

- **`isQuestion`** (on `Stop`): the forwarder tails the transcript (`$j.transcript_path`, last
  ~60 lines), finds the last `assistant` text message, trims it, and sets `1` if it ends with `?`.
  This feeds the **question-guard**: Tests Autorunner **Holds** the next queued prompt rather than
  auto-answering a clarifying question.
- **`permission`** (on `Notification`): set `1` when the notification `message` matches
  `(?i)permission|approve|allow|grant` — distinguishing a blocking tool-permission prompt from an
  idle notification.

## How a session gets the hook config

- **Manager-Launched** (`BuildClaudeSpawn`): claude is spawned with
  `--settings <~/.agentmaster/hooks-settings.json>` plus `CCMGR_SESSION_ID` / `CCMGR_HOOK_PIPE` in
  `spec.env`. The settings file (`BuildHooksSettingsJson`) carries the six hook blocks **and** the
  cog's optional Claude-session settings (`model`, `includeCoAuthoredBy`, `permissions.defaultMode`)
  — each emitted only when it differs from Claude's default, so an all-defaults config stays
  byte-for-byte the prior hooks-only file.
- **Hand-typed `claude`** (a `+` tab): a transparent **PATH shim** (`~/.agentmaster/shim/claude.cmd`
  + a POSIX `claude`, authored by `MaterializeClaudeShim`) prepends `--dangerously-skip-permissions --settings <ours>` (only when you didn't already pass `--settings`) then execs
  the real claude (resolved *before* the PATH prepend so it never finds itself). Combined with the
  `bridge.json` pipe-discovery and the payload `session_id` fallback, a bare `claude` self-wires —
  **when** it inherits our process env. See the degradation caveat above; the Fleet Observer is the
  reliable detection/bind path that needs none of this.

## Workspace trust — how a session gets past the startup modal

Before any of the above can happen, claude has to actually **reach its prompt**. On an untrusted
working directory it doesn't: it blocks on a modal —

```
 Accessing workspace:  C:\Users\ELI
 Quick safety check: Is this a project you created or one you trust? ...
 ❯ 1. Yes, I trust this folder
   2. No, exit
```

**An unattended ConPTY tab cannot answer that**, and the failure is silent-by-design in our UI: the
session submits no prompt ⇒ fires no `UserPromptSubmit` ⇒ writes no transcript ⇒ has **no
conversation id**. The Fleet Observer can therefore only classify it as a
[§11d](OBSERVER.md) *correlated-but-no-transcript* claude — the dim `○ claude · unlinked` observe
badge. No Triage card, no state dot, no Tests Autorunner, and a `/handover` content injection is
typed straight into the menu. It reads like "tabs don't auto-activate; every one needs a manual
click", which is why it looks like an Agentmaster bug and is not one.

### What claude actually gates on (2.1.220, read from its own bundle + PTY-verified)

```
trusted  =  env CLAUDE_CODE_SANDBOXED (bool-coerced: 1/true/yes/on)
         || sessionTrustAccepted            // IN-MEMORY only: non-interactive -p, background-agent
                                            //   mode, or accepting the dialog this run
         || backgroundAgentMode
         || ~/.claude.json  projects[<key>].hasTrustDialogAccepted === true
                                            //   for the cwd's key OR ANY ANCESTOR of the cwd
```

`<key>` = the **git toplevel** of the cwd (else the cwd), `path.normalize`d, `\` → `/`, no trailing
separator, **case-sensitive**. The whole step is additionally skipped by the internal **`CLAUBBIT`**
env var. Even when trusted the dialog *re-appears* if the workspace has project-scoped allow-rules /
`additionalDirectories` that are still un-granted — unless that **exact** key is trusted.

Three things that are **not** true, each of which cost time to establish:

- ⚠ **`--dangerously-skip-permissions` does NOT skip it.** The permission mode is never consulted by
  the trust gate. Our own comments claimed the opposite for a long time (`the dialog block is gated
  on mode !== "bypassPermissions"`); launching the flag in an untrusted dir on a real PTY shows the
  dialog. Corrected in `ClaudeSpawn.cpp`, `SessionModels.h` and `CLAUDE.md`.
- There is **no `settings.json` / managed-policy knob** — the CLI's entire trust-related string table
  was enumerated. Seeding the key is what Claude Code itself prints as the remedy.
- Accepting in your **home directory never persists**: the accept handler branches
  `if (isHomeDir) setSessionTrustAccepted(true) /* memory */ else persistProjectFlag()`. Since an
  empty `defaultLaunchDir` falls back to `%USERPROFILE%`, that is our worst case — it re-asks on
  *every* launch, forever.

### What we do — `EnsureClaudeWorkspaceTrusted` (`ClaudeSpawn.{h,cpp}`)

A shared spawn **prelude** (`PrepareManagedClaudeWorkspace`) runs from **both** builders
(`BuildClaudeSpawn` + `BuildClaudeRestartSpec`), so fresh launch / resume / fork / restore /
window-restore / restart / handover all seed the workspace *before* the process starts. Gated on
`AppSettings::trustWorkspaceOnLaunch` (Settings cog → Sessions → **"Trust the working directory
automatically"**, default **ON**; OFF ⇒ `~/.claude.json` is never touched).

It writes `projects[<repo-or-dir>].hasTrustDialogAccepted = true` — and it is a **surgical splice,
never a re-serialize**. That is a hard requirement, not a preference: this file holds the user's
oauth account, ~60 project entries and float stats, and `Json.h` prints numbers through `%g`
(6 significant digits), so a parse/reprint round-trip would silently truncate `lastCost` /
`lastFpsAverage`. `SpliceWorkspaceTrust` (pure + unit-tested) uses `Json.h` only to **validate** and
to escape the key, and edits the raw text with a string/brace-aware scanner (strings skipped as
units, so a brace inside a member *name* can't unbalance the walk):

| on disk | action |
| --- | --- |
| flag present, `true` | **no write at all** — the steady state after the first launch |
| flag present, `false` | replace that value token in place |
| entry present, no flag | insert after the entry's `{`, reusing the surrounding indentation |
| no entry | insert one under `projects` |
| no `projects` at all | add it as the root's first member |

Guarantees, in the order they matter: an empty / unparseable / non-object config is **refused, never
rebuilt** (the `Updater.h` no-clobber rule — an empty file would race claude's own first write); the
spliced text is **re-parsed and re-read as trusted** before anything is written; it writes **only**
when the flag is missing or false (once per workspace, ever); it takes **claude's OWN config lock**
(`~/.claude.json.lock` — proper-lockfile's atomic-`mkdir` primitive, so `CreateDirectoryW` is the
same lock), re-reads **inside** it, and writes atomically; contention is bounded at 12 × 25 ms and
**skips** the seed if claude holds the lock (fail-open — the dialog shows once more, next launch
retries); a lock abandoned by a killed process is broken only far past proper-lockfile's own 10 s
staleness horizon; and the whole entry point is `try`-wrapped to `LogSwallowedException` (Rule #18)
with "not trusted" as the recovery, so a failure costs one click and never the launch. Steady-state
cost at the launch seam is one ~1 ms read; only the first launch in a workspace can wait at all.

Keying the **repo** (nearest `FindGitRootForDir`, else the dir) is what claude does natively, so one
entry covers every subdirectory. A git *worktree* answers itself here while claude's canonical key
answers the main repo root — harmless, because the worktree is still an **ancestor** of the cwd,
which is the second thing claude's gate checks.

Traces: `[trust] pre-trusted <key>` / `[trust] skipped <key> (claude holds the config lock)` /
`[trust] skipped <key> (config unreadable or unspliceable: <path>)` / `[trust] FAILED to write …`.

### Verifying it (the PTY probe)

The dialog is interactive-only, so a piped run can never reproduce it (`-p` and a non-TTY stdout
both mark the session non-interactive, which trusts it outright). Drive a **real** pty:

```python
from winpty import PtyProcess          # pip install pywinpty
p = PtyProcess.spawn([r"%USERPROFILE%\.local\bin\claude.exe", "--model", "sonnet"],
                     cwd=r"<a directory with no trusted ancestor>", dimensions=(45, 160))
# read for ~15 s, then look for "Quick safety check" in the output
```

Use a directory whose **ancestors** are untrusted too (check `~/.claude.json` — e.g. `K:/source` is
trusted, so nothing under it will ever prompt).

**In the deployed app**, the check is: Launch a session into a folder that has never been trusted
(the fastest is a brand-new folder under `%TEMP%`, or simply the **home dir** — the historical worst
case). Expect:

1. the tab goes straight to claude's prompt — no "Quick safety check" screen, no keypress needed;
2. `hooks.log` gains `[trust] pre-trusted <key>` **once** (subsequent launches in that workspace log
   nothing — the already-trusted path writes nothing and takes no lock);
3. `~/.claude.json` gains exactly `projects["<repo-or-dir>"] = { "hasTrustDialogAccepted": true }`,
   with every other entry byte-identical;
4. the session binds normally — Triage card, state dot, autorunner — instead of sitting as
   `○ claude · unlinked`.

Toggling **Settings → Sessions → "Trust the working directory automatically"** OFF must restore the
old behavior exactly (no write, dialog returns). A `[trust] skipped …` line is not a failure — it
means claude held its config lock, or the config could not be parsed; the seed retries next launch.

⚠ If you isolate the test with `CLAUDE_CONFIG_DIR`,
be aware it also relocates the **user-memory root**, which reclassifies `~/.claude/CLAUDE.md`'s
`@imports` as *project* external includes and raises a second, unrelated dialog ("Allow external
CLAUDE.md file imports?"). That is a harness artifact — with the real config all live project entries
have both of its keys `false` and it never fires.

## Scheduler trigger (see IMPLEMENTATION.md / M7, `Scheduler.cpp`)

```
onHook(sid, ev):
  s = registry[sid]
  Stop:                 s.state = WaitingForInput; tryAdvance(s)
  Notification(perm):   s.state = NeedsApproval;   applyApprovalPolicy(s)   // not the queue

tryAdvance(s):
  if s.autorunner.mode == Off:            return
  if s.state not in {WaitingForInput, Idle}: return   // Rule #1: Idle is also "ready"
  if humanTypedWithin(s, 1500ms):        return        // pauseOnHumanInput
  item = s.queue.firstPending(); if !item: notifyPlanDone(s); return
  if !passesGuard(s, item):  item.status = Held; flagCard(s); return  // e.g. lastMessageIsQuestion
  if s.autorunner.mode == SemiAuto:       askConfirm(s, item); return
  markSentAtomically(item); persist(s)                 // idempotent BEFORE inject (Rule #4)
  s.connection.WriteInput(item.text + L"\r");          // inject + submit
```

Advances fire on **two** triggers — the `Stop` seam *and* observed changes (`OnObserved`) — so a
freshly launched / just-resumed session sitting `Idle` (which never emits a `Stop`) still starts
consuming its queue; a time-bounded pickup guard holds it to one prompt per turn.

## Files

All under the **ACTIVE PROFILE** dir (`AgentmasterStateDir()` — default `%USERPROFILE%\.agentmaster\`
for the release package + unpackaged runs, `%USERPROFILE%\.agentmaster-dev\` for the dev package;
resolved once at startup — see [`PROFILES.md`](PROFILES.md)). Deliberately *not* `%LOCALAPPDATA%` —
MSIX virtualizes a packaged app's LocalCache, but the external `claude.exe` resolves the real path;
they must agree — see Gotchas in `CLAUDE.md`:

| File | Written by | Purpose |
| --- | --- | --- |
| `agentmaster-hook.ps1` | `MaterializeSharedHookFiles` | the PowerShell forwarder Claude invokes |
| `hooks-settings.json` | `MaterializeSharedHookFiles` | the `--settings` config wiring the six hooks |
| `bridge.json` | `WriteBridgeDiscovery` | live-bridge discovery (`{ pid, pipe }`) for the shim |
| `forwarder-errors.log` | the forwarder | local trace of a hook delivery that never reached the bridge (no sid / no pipe / a dead-pipe connect timeout) — otherwise invisible, since the bridge-side `hooks.log` only sees lines that arrived |
| `shim/claude.cmd`, `shim/claude` | `MaterializeClaudeShim` | the transparent `claude` PATH shim |
| `hooks.log` | engine | runtime traces (`[engine] bridge listening …`, `[SessionStart]`, `[Stop]`, …) |

**Outside the profile** — the only two files we ever touch, both under Claude's own config, both
additive and both gated:

| File | Written by | Purpose |
| --- | --- | --- |
| `<claude-config>/commands/handover*.md` | `EnsureShippedCommandFileIn` | the shipped `/handover` family definitions (create-if-absent + SHA-256-gated upgrade; a user-edited file is never touched) — see [`COMMANDS.md`](COMMANDS.md) §6 |
| `~/.claude.json` (one flag) | `EnsureClaudeWorkspaceTrusted` | `projects[<repo-or-dir>].hasTrustDialogAccepted = true`, so the startup trust modal never parks a managed tab — see *Workspace trust* above; surgical splice, once per workspace, cog-gated |
