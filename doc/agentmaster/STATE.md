# Agentmaster — Observer-Owned Session State (the PULL state engine)

> **Status: DESIGN (pre-implementation).** This doc captures *what a Claude session's state is*,
> *how the Fleet Observer can derive all of it from disk + process facts alone* (no hooks), and
> *how that becomes the single source of truth* as we phase hooks out. Grounded in a survey of
> **4,837 real transcripts** on this machine (see §9 evidence).
> Companions: [`OBSERVER.md`](./OBSERVER.md) · [`HOOKS.md`](./HOOKS.md) · [`DESIGN.md`](./DESIGN.md) ·
> [`TAB_OVERLAY.md`](./TAB_OVERLAY.md).

---

## 0. Why this exists

Today `SessionState` is **hook-derived**: `OnHookEvent → NextSessionState` is the *only* path that
sets `Running`/`WaitingForInput`/etc. The Fleet Observer (`ObserveClaude`) deliberately **never**
sets state (Rule #13), and the transcript tail (`SessionScanner::_reconcileSession`) only ever
synthesizes a *missed Stop* — and only if the session is **already** `Running`.

**Consequence (the bug that started this):** a hook-less session — a hand-typed `claude` in a `+`
tab, which WT's env regeneration strips of the hook pipe/shim — has **no Idle→Running edge** and is
pinned at `Idle` for its entire life, even mid-turn. Live example: session `3f98f88e` in
`C:\Users\ELI\.claude` (the **live registry** at dig time read `external:true`, `lastHookUnixMs:0`
— don't look for these in `sessions.json`: `lastHookUnixMs` is transient and `external` is
force-cleared on restore-load, `TerminalPage.AgentSessions.cpp:282`) whose transcript clearly shows
`stop_reason:"end_turn"` (truly *WaitingForInput*) while the registry says `Idle`.

**Strategic direction (locked):** we are **removing hooks slowly**. The PULL side — the transcript
tail + the process observer — becomes the **sole authority** for state. Hooks survive only as an
optional low-latency *hint* during migration, then get deleted. Rule #13 already names this owner
("push hooks **+ the transcript tail** own state"); we are finishing the half that was never built.

## 1. Decisions (locked — from design review)

| # | Decision | Choice |
| --- | --- | --- |
| 1 | **Authority model** | **Tail is sole authority.** Transcript tail + process observer derive state for ALL sessions; hooks demoted to a migration-only early hint, reconciled by evidence-timestamp (§6), then removed. |
| 2 | **Taxonomy this pass** | **Core 6 + `Compacting` + `Starting`.** (`Interrupted` / `Retrying` / thinking·tool sub-states deferred to annotations — §3b.) |
| 3 | **NeedsApproval (hook-less)** | **Heuristic fallback** — dangling `tool_use` + permission-mode gate + no tool child + quiescence (§4, grounded in §9). |
| 4 | **Scope** | **Design doc first** (this file) before any code. |

## 2. What the PULL side can see (signal inventory)

Every Claude session continuously writes a structured JSONL transcript at
`<~/.claude>/projects/<enc-cwd>/<id>.jsonl` plus side files (`debug/<id>.txt`, `session-env/<id>`,
`todos/<id>-agent-*.json`). The observer already reads the PEB (cwd/cmdline/env), the process tree
(Toolhelp), and transcript ctime/mtime + head. The **state-bearing** signals (all verified present
in real data — §9):

| Signal | Where | Tells us |
| --- | --- | --- |
| assistant `message.stop_reason` ∈ `tool_use` / `null` (latest main line) | transcript | turn **in progress** → Running |
| assistant `stop_reason` ∈ `end_turn` / `stop_sequence` (latest main-chain line) | transcript | turn **complete** → WaitingForInput (no quiescence wait — §3a) |
| latest line is a `user` `tool_result` | transcript | claude is **digesting tool output** → Running |
| assistant `tool_use` **id with no matching `tool_result`** + quiescent | transcript | **blocked** (approval or stall) → NeedsApproval (heuristic) |
| `tool_use` named **`ExitPlanMode`**, no result yet | transcript | **plan awaiting approval** → NeedsApproval |
| `isApiErrorMessage:true` / system `subtype:"api_error"` (latest, unsuperseded) | transcript | API failure → **Error** |
| system `subtype:"compact_boundary"` / `isCompactSummary:true` (in flight) | transcript | context **compaction** → **Compacting** |
| `interruptedMessageId` / user `"[Request interrupted by user…]"` | transcript | user hit **Esc** → (maps to WaitingForInput this pass) |
| `retryAttempt` / `retryInMs` present | transcript | API **auto-retry** (annotation; Running) |
| `permission-mode` line → `permissionMode` | transcript | posture (`default`/`acceptEdits`/`bypassPermissions`/`plan`) — gates the approval heuristic |
| `isSidechain:true` activity | transcript | a **subagent (Task)** is running — main session is Running, *not* turn-complete |
| `pendingBackgroundAgentCount` / system `away_summary`,`scheduled_task_fire`,`turn_duration` | transcript | **background** noise — must NOT be read as a user turn (bumps mtime!) |
| process **alive**, transcript **absent/empty** | PEB + fs | never-prompted (§11d) → **Starting** |
| process **dead** (known pid only) | PEB | exited → **Done** (clean if last line was `end_turn`, else crashed) |
| a live **tool child** (Bash spawns `bash/pwsh/cmd`) under the claude pid | Toolhelp tree | a tool is genuinely **executing** (≠ blocked on approval). Already computed: `HasActiveChild` → `TabActivityRow::busy` (O6) |

The `debug/<id>.txt` is even more explicit (`[API REQUEST] … source=repl_main_thread` = real turn
start; `source=away_summary` = background; `[skills] idle`). **But it is debug output** — possibly
flag-gated and format-unstable — so it is at most an *optional accelerator*, never the authority.
**The JSONL tail is the stable oracle.**

> **mtime is a liar.** Background agents (`away_summary`), scheduled tasks, and snapshot lines all
> append to the same transcript and bump its mtime *without a user turn* (verified: session
> `3f98f88e` finished at 20:10:33 but mtime advanced to 20:13 from an `away_summary` fork). State
> MUST come from **parsing the tail's last *main-chain* event**, never from mtime. (mtime stays
> only the "last-activity-ago" adornment.) The same goes for **quiescence**: the §4 heuristic's
> `quietMs` is measured from the latest main-chain line's own `timestamp` field — a background fork
> appending every few seconds would hold an mtime-based quiet clock hot indefinitely and starve
> NeedsApproval.

## 3. State taxonomy

### 3a. The lifecycle states (Core 6 + 2)

| State | Glyph · color (Triage Board / overlay) | PULL definition |
| --- | --- | --- |
| **Starting** *(new)* | `◌` Gray-dim | process alive, **no transcript yet** — for a session that HAS a registry record before its transcript: Manager-**Launched** (we mint the id up front) or hook-`SessionStart`-adopted. (A hook-less external claude before its first prompt has **no record to hold any state** — `ObserveClaude` early-returns on an empty id, `SessionRegistry.cpp:304` — so it stays OBSERVER §11d's badge-only "ClaudeCode (starting…)" until the transcript appears.) Replaces the misuse of `Idle` for "just launched". |
| **Idle** | `○` Gray | alive, transcript exists, but no turn has ever begun (rare — usually a session opens straight into Starting→Running). Kept for back-compat / resumed-but-untouched. |
| **Running** | `●` DodgerBlue | latest main-chain event is an unfinished assistant (`tool_use`/`null`), a fresh user prompt, a `tool_result` being digested, or a live subagent. |
| **WaitingForInput** | `◐` Goldenrod | latest main-chain assistant ended the turn (`end_turn`/`stop_sequence`) — **no quiescence gate**: `end_turn` is final for the turn, an mtime-quiet wait can be starved forever by background forks (§2), and the hook race the old synth's 2 s quiet debounced is the §6 precedence's job now (a Stop-hook *block* that makes claude continue just supersedes on its next line). Carries `lastMessageWasQuestion` (from `EndsWithQuestion(lastAssistantText)`) for the Autopilot question-guard. |
| **NeedsApproval** | `⚠` OrangeRed | dangling `tool_use` (incl. `ExitPlanMode`) + permission-mode requires consent + no tool child + quiescent (§4). |
| **Compacting** *(new)* | `↻` MediumPurple | a `compact_boundary` / `isCompactSummary` is the live tail with no newer turn — context is being summarized. Transient; resolves to Running/WaitingForInput on the next real line. |
| **Error** | `✕` Crimson | latest unsuperseded marker is `isApiErrorMessage` / system `api_error`. Distinct from a *tool* failure (`tool_result.is_error` — claude usually continues, so that stays Running). |
| **Done** | `✓` MediumSeaGreen | process dead — gated on a KNOWN pid: `pid != 0 && !ProcessAlive(pid)`. A record with **no correlated pid** (just-restored, or hook-created before the S-lane's first survey) must fall through to the tail, never read Done. (Process signal, not tail — the tail just stops.) Clean vs crashed = annotation. Note this is also what finally covers an **adopted hand-typed** claude that exits back to its shell: its ConPTY connection is the SHELL, so the connection-Closed archive sweep (`_SweepClaudeLiveness`) never fires for it today — see §10-Q6. |

`Starting` and `Compacting` are the two new enum values (decision #2). They slot into
`SessionModels.h::SessionState` and the three palette tables (`AgentManagerContent` `StateColor`/
`StateGlyph`/`StateLabel`, mirrored in `AgentTabOverlay.cpp`).

### 3b. Deferred — annotations, not states (this pass)

Surfaced as **enrichment fields / badges**, not lifecycle states, to keep the enum small:
- **Interrupted** (`interruptedMessageId` / `[Request interrupted by user]`) → maps to `WaitingForInput`; optional "interrupted" tag.
- **Retrying** (`retryAttempt`/`retryInMs`) → stays `Running` with a "retrying (n)" tag; escalates to `Error` only when retries are exhausted.
- **thinking / tool-running** sub-state of Running (a `thinking` block streaming vs a `tool_use` executing).
- **background-busy** (`pendingBackgroundAgentCount>0`, away/scheduled) → a small "bg" adornment; never the primary state.
- **permission posture** (`permissionMode`) → already an adornment; also gates §4.

## 4. NeedsApproval without hooks — the grounded heuristic

This is the only state with no unambiguous *live* transcript line (the y/n block in the TUI isn't
written until the user decides). Evidence model from §9:

- **Live "blocked" signature:** the latest main-chain assistant message has `stop_reason:"tool_use"`,
  and **at least one of its `tool_use` ids has no matching `tool_result`** further down the file.
- **Post-decision confirmations** (used to detect the *exit* from approval, and to validate the model):
  - **Denied:** a `tool_result` with `is_error:true` and the canonical string *"The user doesn't want
    to proceed with this tool use. The tool use was rejected … STOP what you are doing and wait …"*
  - **Interrupted-during-tool:** a `user` line `"[Request interrupted by user for tool use]"`.
  - **Plan approval:** assistant `tool_use` name `ExitPlanMode` (`{allowedPrompts, plan}`) — dangling = waiting on the plan decision.

**Decision rule (per scan), only when the dangling-`tool_use` signature holds:**

```
mode      = session.permissionMode             # transcript `permission-mode` line (live — tracks a
                                               #   mid-session Shift+Tab), else cmdline --permission-mode;
                                               #   EMPTY/unknown ⇒ treat as "default" (the conservative read)
hasChild  = HasActiveChild(snap, claudePid)    # the EXISTING primitive (ProcessInspect.h) — Bash spawns
                                               #   bash/pwsh/cmd; in-process tools have none. ≤ ~2 s stale
                                               #   (refreshed each S-lane heartbeat survey)
quietMs   = now - ts(latest main-chain line)   # the line's own `timestamp` field — NEVER file mtime (§2:
                                               #   a background fork holds mtime hot indefinitely)

if name == "ExitPlanMode":           return NeedsApproval # the plan gate prompts regardless of mode — FIRST
if mode == "bypassPermissions":      return Running      # nothing is ever gated → it's executing
if hasChild:                         return Running      # a Bash tool is genuinely running
if mode in {"", "default", "plan"} or (mode=="acceptEdits" and pendingToolIsNotAnEdit):
    if quietMs >= APPROVAL_QUIET_MS: return NeedsApproval # blocked on y/n (in-proc tools finish in ms)
    else:                            return Running       # result imminent
else:                                return Running
```

(`ExitPlanMode` is checked first so the "independent of mode" claim holds unconditionally — modes
are exclusive in practice, plan vs bypass, but the ordering costs nothing and removes the edge. An
empty `mode` MUST map into the gated branch: a session with no `permission-mode` line and no
cmdline flag IS in `default` mode, and routing it to the final `else` would pin a genuinely
blocked session at Running forever.)

`APPROVAL_QUIET_MS` ≈ **3 s** — it must be ≥ one S-lane heartbeat (`kObserverHeartbeatMs` = 2 s),
so at least one full child-survey lands inside the window (`hasChild` refreshes per heartbeat; the
O7 fast-tick skip never starves it past one, since the heartbeat always forces a full survey). On
first entering the dangling-quiet window the scanner additionally `Wake()`s the observer so the
child read is fresh, not up to a heartbeat stale. Rationale: **in-process tools** (Read/Edit/Grep/
Glob/…) spawn no child and complete in milliseconds, so a dangling `tool_use` quiet for seconds ⇒
almost certainly a permission block. **Bash** is the one tool that spawns an observable child, so
`hasChild` cleanly separates "running a command" from "waiting on you". When a hook *is* present
(`Notification(permission)`), it remains the precise, instant signal during migration (§6); the
heuristic is the floor for hook-less sessions.

> **Known limitation (accepted):** a **slow in-process tool** — WebFetch awaiting the network, a
> long MCP tool call — is indistinguishable from awaiting-approval once approved: dangling
> `tool_use`, no child, quiet. The heuristic reads it **NeedsApproval while it is actually
> executing**, for the tool's duration, self-correcting the moment the `tool_result` lands. Same
> for an **allowlisted** (`permissions.allow`) in-proc tool that never prompted at all. Bounded,
> visually mild (an amber card that resolves itself), and absent on hooked sessions (the
> `Notification` hook is exact); per-tool duration knowledge (Read fast / WebFetch slow) is a
> possible refinement, deferred with the §3b annotations.

## 5. The derivation algorithm (tail + process)

Runs in the **PULL state engine** — `SessionScanner::_reconcileSession` (which already owns the
per-session transcript cursor, `lastStopReason`, `lastAssistantText`) extended from "synthesize a
missed Stop" to "compute the full derived state". `ObserveClaude` stays **facts-only** (Rule #13).

```
DeriveState(session, tailCursor, proc):
  if session.pid != 0 and not proc.alive:     return Done              # NEVER on pid==0 (uncorrelated /
                                              #   just-restored) — fall through to the tail instead
  if tailCursor.transcriptMissingOrEmpty:     return Starting          # alive, never prompted (§11d)

  e = latest MAIN-CHAIN, non-background event # skip isSidechain, away_summary, scheduled_task_fire,
                                              #   turn_duration, snapshot, isMeta
  if liveErrorMarker(e):                      return Error             # isApiErrorMessage / api_error
  if liveCompaction(e):                       return Compacting        # compact_boundary / isCompactSummary

  switch e.kind:
    assistant, stop_reason in {end_turn, stop_sequence}:
        session.lastMessageWasQuestion = EndsWithQuestion(e.text)
        return WaitingForInput
    assistant, stop_reason in {tool_use, null}:          # null is the JSON literal — our parser's
                                                          #   StrAt reads it as the empty string
        if danglingToolUse(e): return ApprovalOrRunning(e, session, proc)   # §4
        return Running                                                       # results in, mid-turn
    user prompt (no later assistant):          return Running          # turn just started
    user tool_result:                          return Running          # digesting tool output
    default:                                   return WaitingForInput  # safe fallback
```

The result is applied via a new **tail-native setter** (§6), which fires observers + the Autopilot
advance seam (so turn-complete still drives the Flight Plan — fired only on a *transition into*
WaitingForInput, like today's `triggerAdvance`) and is a **no-op when unchanged** (so background
mtime ticks never cause churn — Rule #13's quiet discipline). Unlike today's `_readDelta`, the
derive runs **every reconcile tick, not only when the file size changed** — the quiescence- and
process-driven transitions (Running→NeedsApproval, →Done) happen precisely while the transcript is
NOT growing.

**Process inputs** come free from the S-lane's existing single Toolhelp snapshot — and the
primitive already exists: **`HasActiveChild(snap, pid)`** (`ProcessInspect.h`, the O6 `busy`
heuristic: a non-infra direct child, conhost/OpenConsole ignored) is exactly §4's `hasChild`; no
new `ClaudeHasToolChild` needed. **The channel:** the scanner never touches the observer's tables —
the S-lane enriches `SessionInfo` via `ObserveClaude` with a transient `hasToolChild` (refreshed
**silently**, like `lastObservedUnixMs`: it flips on every tool call and must not drive the change
cascade), and the scanner reads it off the registry snapshot it already iterates. `ProcessAlive(pid)`
the scanner calls directly (`ProcessInspect` links into the same plain-C++ engine). No extra
enumeration cost (O7 budget preserved).

## 6. Precedence & the hook phase-out

End state: **tail only.** During migration, both paths can produce state; we reconcile by **newest
evidence**, so deleting hooks is a no-op:

- Each derived value carries an **evidence timestamp** = the `timestamp` field of the main-chain
  line it was read from (wall-clock, same clock domain as the hook `ts`), or `now` for
  process-fact states (Done/Starting).
- A hook still sets state through `OnHookEvent` and stamps `lastHookUnixMs` (already exists).
- The tail-native `ApplyDerivedState(id, state, evidenceTs, annotations)` writes state **iff**
  `evidenceTs >= lastHookUnixMs` **or** the session is hook-less (`!hookWired`). So a *fresh* hook
  (e.g. instant `Notification→NeedsApproval`) wins for its recency window; the tail confirms /
  overrides once its own evidence is at least as new.
- **Remove hooks →** `lastHookUnixMs` stays 0, `hookWired` false, and the tail *always* wins.
  `NextSessionState`/`OnHookEvent` become dead code to delete; `ApplyDerivedState` is the sole writer.

This keeps hooked sessions correct mid-migration (no regression) while making the tail authoritative
and the removal mechanical.

## 7. Where it plugs into the code

| Area | Change |
| --- | --- |
| `AgentMaster/SessionModels.h` (+ `Activity.h`) | add `Starting`, `Compacting` to `SessionState`; a transient `hasToolChild` enrichment field on `SessionInfo` + `ObservedClaude` (the §4/§5 channel); (optional) annotation fields (`retrying`, `interrupted`, `toolRunning`, `bgBusy`). |
| `AgentMaster/SessionScanner.{h,cpp}` | extend `_reconcileSession` → `DeriveState` (§5), run **every tick** (not only on size change); **filter `isSidechain`** in `ParseTranscriptDelta` (§8 bug-1); detect error/compaction/dangling-tool-use lines + parse the **`permission-mode` line** from the tail it already reads (the LIVE mode — tracks a mid-session Shift+Tab; the S-lane's cmdline `--permission-mode` is just the seed) (today it only reads assistant `stop_reason` + user prompts). |
| `AgentMaster/SessionRegistry.{h,cpp}` | add `ApplyDerivedState(id, state, evidenceTs, …)` with the §6 precedence; keep `ObserveClaude` facts-only; route the Autopilot advance off `ApplyDerivedState` too. |
| `AgentMaster/ProcessInspect.{h,cpp}` | **no new primitive** — `HasActiveChild(snap, pid)` already exists and already feeds `TabActivityRow::busy` (`ProcessObserver.cpp` O6); §4 reuses it. |
| `AgentMaster/ProcessObserver.cpp` (S-lane) | enrich `SessionInfo` with `hasToolChild` via `ObserveClaude` (**silently**, like `lastObservedUnixMs` — it flips per tool call and must not drive the change cascade); the pid it already feeds gives the scanner its `ProcessAlive` input for Done. |
| `AgentMaster/Engine.cpp` | hand the scanner a wake-the-observer callback (it owns both workers) — §4's fresh-child read on entering the dangling-quiet window. |
| `AgentManagerContent.cpp` | extend `StateColor`/`StateGlyph`/`StateLabel` for the 2 new states; the board already renders whatever it's given. |
| `AgentTabOverlay.cpp` | mirror the 2 new states in its (hand-synced) palette + labels. |
| `AgentMaster/HookEvents.h`, `HooksBridge`, the shim/forwarder | **migration-only**, then deletion: hooks become the optional hint, then removed (§6). |
| `doc/agentmaster/HOOKS.md`, `CLAUDE.md` | re-document state ownership as PULL/tail once shipped. |

## 8. Side-bugs found during the dig (fold into the implementation)

1. **Sidechain pollutes the tail.** `ParseTranscriptDelta` does **not** filter `isSidechain:true`,
   so a *subagent's* `end_turn` can satisfy the missed-Stop synth and declare the **main** turn
   complete (draining the plan into a running turn). Fix: ignore `isSidechain` lines for state; count
   a live sidechain as *Running* instead.
2. **Interrupts recorded as prompts.** `NoteExternalPrompt` (transcript back-fill) records
   `"[Request interrupted by user]"` / `"[Request interrupted by user for tool use]"` as **Typed
   Flight-Plan prompts** (seen in `sessions.json` for the `.claude` session). These are control
   markers, not human prompts — filter them out of the queue back-fill.
3. **An Esc-interrupt strands even a HOOKED session in `Running`.** The `Stop` hook does not fire
   on a user interrupt; the prior assistant line is `tool_use`/`null` (not `end_turn`), so the
   missed-Stop synth can't fire either — and the interrupt marker is parsed as a *UserPrompt*
   (bug 2), which `_readDelta` treats as a fresh turn (`lastStopReason.clear()`), re-arming
   nothing. The session reads `Running` until the next real prompt. §3b's Interrupted →
   WaitingForInput mapping fixes this for hooked and hook-less sessions alike.

## 9. Evidence appendix (real data, this machine)

Survey of **4,837** transcripts; taxonomy over the 80–700 most recent:

- **Top-level `type`:** `assistant`, `user`, `permission-mode`, `last-prompt`, `attachment`, `mode`,
  `system`, `file-history-snapshot`, `queue-operation`, `agent-setting`, `worktree-state`,
  `ai-title`, `custom-title`, `fork-context-ref`.
- **assistant `stop_reason`:** `tool_use` 29 357 · `end_turn` 1 997 · `null` 163 · `stop_sequence` 17.
- **`system.subtype`:** `turn_duration` · `away_summary` · `scheduled_task_fire` · `stop_hook_summary`
  · `compact_boundary` · `api_error` · `local_command` · `informational`.
- **state-flags:** `isSidechain` (dominant) · `isMeta` · `isSnapshotUpdate` · `isCompactSummary` ·
  `isVisibleInTranscriptOnly` · `preventedContinuation` · `isApiErrorMessage` · plus keys
  `apiErrorStatus`, `retryAttempt`/`maxRetries`/`retryInMs`, `interruptedMessageId`,
  `pendingBackgroundAgentCount`, `permissionMode`, `compactMetadata`, `summarizeMetadata`.
- **Approval (over 700 recent):** 65 denial `tool_result`s (canonical *"The user doesn't want to
  proceed…"*), 99 `[Request interrupted by user…]`, 17 `ExitPlanMode` tool_uses.
- **API error shape:** `assistant` line, `model:"<synthetic>"`, content `"API Error: …"`,
  `isApiErrorMessage:true`, `apiErrorStatus`.
- **Live confirmation:** stuck session `3f98f88e` tail = `assistant stop_reason:end_turn` (truly
  WaitingForInput) while registry = `Idle`; mtime advanced past the real turn via an `away_summary`
  fork (proves §2's mtime caveat).

## 10. Open questions (for review before coding)

1. **`APPROVAL_QUIET_MS`** — the floor is now fixed at ≥ `kObserverHeartbeatMs` (2 s, the
   `hasChild` refresh cadence — §4); the open part is only how far above it to sit (3 s
   recommended; higher is safer against slow in-proc tools / API stalls, at the cost of approval
   latency).
2. **Keep `Idle` at all?** With `Starting` covering "alive, no transcript", `Idle` only means
   "resumed but untouched" — and even that derives as WaitingForInput (a resumed transcript's last
   main-chain line is its old `end_turn`), which is what Autopilot treats as ready anyway (Rule
   #1 already equates them). Fold into `Starting`, or keep for resumed sessions?
3. **`debug/<id>.txt` as an optional accelerator** for sub-second turn-start latency, or ignore it
   entirely (transcript-only) for stability?
4. **Error transience** — auto-clear `Error` to `Running` when a retry/next line lands, or hold until
   a human looks (sticky)?
5. **Annotation surface** — do we add the deferred annotations (retrying/interrupted/bg) now as
   fields (cheap) even though the UI uses them later?
6. **Done → auto-archive for adopted externals?** A Done derived for an adopted hand-typed claude
   (its claude exited; the hosting shell tab lives on, so the connection-Closed sweep never fires —
   §3a) leaves a green ✓ card on the board indefinitely. Auto-archive after a grace period, or keep
   the card until the user acts?
