# Slash-command bindings + the /handover integration (CommandWatch)

> Bind to `/commands` the user types into a managed Claude session, and AWAIT the session's
> follow-up activity — asynchronously, bounded, and without touching the state machine.
> First integration: **`/handover <handover-context-or-filepath>`** → await the handover
> markdown the command instructs Claude to write → open a successor tab named
> `"<origin title> (handover)"` in the same working dir, first-prompted at that file.

Engine: `AgentMaster/CommandWatch.{h,cpp}` (+ parser/scanner seams in `SessionScanner.{h,cpp}`,
wiring in `Engine.{h,cpp}`); UI action: `TerminalPage.AgentEngine.cpp` (sink) +
`TerminalPage.AgentSessions.cpp` (`_HandleCommandHandover`). Tests: `tests/tests_commands.cpp`
(the `TestCommandWatch` suite) + the refined `/model`-echo invariant in `tests_transcript.cpp`.

## 1. Why the transcript, and what a command looks like there

The Fleet's PULL lane already tails every live session's transcript
(`SessionScanner::_readDelta` → the pure `ParseTranscriptDelta`), and a typed slash command
leaves a deterministic echo there. Corpus-grounded (the real `~/.claude/projects` history on
this machine), there are TWO strata:

* **Current (2026-07, the shape /handover takes)** — a `type:"user"` line whose `message.content`
  is a plain STRING, name tag first:

  ```json
  {"type":"user","message":{"role":"user","content":"<command-name>/model</command-name>\n            <command-message>model</command-message>\n            <command-args></command-args>"},"timestamp":"2026-07-20T…Z",…}
  ```

* **Older (≤ 2026-06)** — custom commands as a user line with `<command-message>` FIRST, and
  built-ins as `type:"system","subtype":"local_command"` lines carrying the same tags.

Both strata carry the same three tags, so the extractor (`ParseCommandEcho`, pure) is
**order-agnostic**: it takes the `<command-name>` body (leading `/` stripped, ASCII-lowered)
plus the optional `<command-args>` body verbatim (outer-trimmed; interior newlines kept — a
`/compact` in the corpus carries `\r\n` args). No `<command-name>` tag ⇒ not a command.

Why PULL-only (no hook lane): the transcript is the always-correct floor — it works for hooked
AND hookless (observer-bound) sessions alike, one lane means no cross-lane double-fire dedup,
and the ~2s scanner cadence is irrelevant next to the seconds-to-minutes the command's own turn
takes. A push fast-path can be added later behind the same `CommandWatch` feed (dedup by
sighting identity) without changing any consumer.

## 2. Parser surface — new events, ZERO state-machine change

`ParseTranscriptDelta` previously **dropped** command echoes at the `IsNoiseUserPrompt` gate
(the /model false-Running fix). It still does — for the state machine — but now also emits:

* **`TranscriptEvent::Kind::Command`** — `commandName` + `commandArgs` + **`lineTsMs`** (the
  line's own `timestamp` → Unix ms via `ParseTranscriptTimestamp`; the replay guard below).
  Emitted for both strata (the user-line shape at the noise gate; `system/local_command` in the
  system branch — which still also feeds `emitNode`, lineage being orthogonal).
* **`TranscriptEvent::fileWritePaths`** (assistant events) — each `Write`/`Edit` (+ legacy
  `MultiEdit`) tool_use block's `input.file_path`, in block order (`CollectFileWritePaths`; the
  line's JSON DOM is already parsed, so this is a field walk, not a re-parse).

**Invariant kept:** a `Command` event is NOT a turn event. `_readDelta` handles it in its own
branch that touches nothing — no `consumedTurnEvent`, no tail-fact wipes (`lastStopReason` /
`pendingInteractiveTool` / API-error tracking), no `NoteExternalPrompt`. A `/model` still cannot
light recon-run, release NeedsApproval, or clear Error (`tests_transcript.cpp` pins exactly
this: no TURN event, and the echo now reads back as the non-turn Command event).

## 3. CommandWatch — bind + await, bounded

One process-wide instance (`Engine::commandWatch`), fed by the scanner worker in transcript
order, mutex-guarded, handlers fired on the scanner thread (consumers marshal — §5).

**Feeds** (`SessionScanner::_readDelta` / `_scanOnce`):

| Feed | Source | Meaning |
|---|---|---|
| `OnCommandSighting` | `Kind::Command` events | arm a bound command's await |
| `OnFileToolWrite` | assistant `fileWritePaths` | match the oldest unmatched pending |
| `OnTurnEnd` | terminal `stop_reason` / interrupt marker | unmatched pendings age one turn |
| `Tick` | every scanner pass | deadline sweep + the disk poll |
| `DropSession` | cursor cleanup (session gone/archived) | drop its pendings |

**Two gates keep a replay from ever re-firing an old command:**

1. **Caught-up cursor** — feeds run only when the cursor was primed at parse time (`st.primed`
   after the read): a capped mid-replay backlog chunk never feeds. A fresh session's very first
   small read DOES feed (its "history" is seconds old and live).
2. **Line-timestamp freshness** (`kCommandSightingFreshMs`, 60s) — an adopted/restored session's
   one-shot history read primes during the same pass, but its old commands carry old stamps and
   never arm. An absent stamp (0) never arms either.

**The markdown await** (v1's one binding shape, `BindMarkdownAwait(name, preferLeafContains,
handler)`): after the command, the next `.md` `Write`/`Edit` is the match —
`PickMarkdownWritePath` prefers, within a message's batch, a path whose LEAF contains the
binding's hint (`"handover"` → the command's own `HANDOVER-*.md` outranks an incidental doc
edit in the same message), else the first markdown. A relative tool path resolves against the
session's working dir. **The tool_use line only proves the REQUEST** — the write may still be
pending a permission approval — so the fire is gated on the DISK: the pending fires when the
file actually exists non-empty (probed at match time, then per `Tick`). The file probe is
injectable (`SetFileProbe`) so the whole machine unit-tests offline.

**Every await is bounded** (state can never leak):

* an UNMATCHED sighting expires after `kCommandAwaitMaxTurnEnds` (2) turn boundaries — the
  command's own turn + one clarification round. Scoping the await to the command's vicinity is
  what keeps an unrelated `.md` edit three turns later from spawning a spurious tab (worse than
  a missed one — the user just re-runs the command). A MATCHED-but-not-yet-on-disk pending is
  exempt (approval can span boundaries) and is bounded by the deadline alone;
* `kCommandAwaitDeadlineMs` (15 min) hard-caps everything;
* `kCommandMaxPendingPerSession` (4) caps memory, oldest evicted; FIFO across sightings (a
  second `/handover` before the first resolves waits for the NEXT write);
* pure v1 semantics: pendings are transient (never persisted — a deliberate Rule-#16-adjacent
  choice like `pendingInput`: an await does not survive an app restart).

Logs (hooks.log): `[cmd] /handover sighted <sid8> args="…"` → `[cmd] /handover md matched …` →
`[cmd-fire] /handover <sid8> md=…`, with `[cmd-expire] …` for the bounded ends. Unbound
commands (`/model`, `/compact`, …) produce no state and no logs.

## 4. Fan-out — the command-action sinks

The engine's `/handover` binding (registered in `Engine.cpp` init, before the scanner starts)
does one thing: `RaiseCommandActionInWindows(sessionId, L"handover", mdPath)`. That is a new
per-window sink family (`Engine::CommandActionSink` — the `activateSinks` idiom verbatim:
registered at engine init, token-detached in `~TerminalPage` (Rule #10), snapshot-under-lock /
invoke-outside). Unlike Activate there is NO source window to exclude — the fire originates on
the scanner thread — so every window's sink runs; each hops onto its own UI dispatcher and only
the (single) window whose `_claudeTabs` hosts the origin session acts.

## 5. /handover end-to-end

1. User types **`/handover <context-or-filepath>`** in a managed Claude session. The command
   DEFINITION (§6) instructs Claude: read the arg if it names a file, then **use the Write tool**
   to create ONE `HANDOVER-<topic>.md` in the cwd carrying everything a successor needs, then end
   the turn.
2. The scanner's next delta surfaces the echo → `[cmd] /handover sighted`, await armed.
3. Claude's Write tool_use lands in a later delta → matched (leaf preference `handover`); the
   disk probe confirms the file → `[cmd-fire]` → engine fan-out → the hosting window's UI thread.
4. `TerminalPage::_HandleCommandHandover` (miss ⇒ no-op; archived/vanished ⇒ drop):
   * **dir** — `EffectiveWorkingDir(tabColorMode, *s)`: the same "here" every *Open New Session
     Here* uses (falls back to the launch cwd inside the resolver);
   * **title** — `DeriveSuffixedTitle(origin, L"handover")` → `"T (handover)"`, chaining bumps
     (`(handover 2)`, …) — the generalized `DeriveForkTitle` (which now delegates to it) — then
     bumped past any title already in the registry (live or archived), so sibling handovers from
     one origin never collide;
   * **placement** — inserted at `originTab.TabViewIndex()+1`, beside the origin (the *New
     Session Here* placement);
   * **the handover message** — the md is delivered BY REFERENCE: the successor's launch
     commandline carries ONE positional prompt, `Read the handover document at <mdPath> and
     continue the work it describes.` A multi-line md body injected as a prompt would be hostage
     to quoting and to the known multi-line-injection limitation; the file outlives the prompt
     and the successor Reads it as its first action.
5. The spawn is a normal fresh `_LaunchClaudeSession` — Default model, hooks wired, registry
   card, per-dir color — plus the **initial prompt on the commandline**:
   `BuildClaudeCommandline(..., initialPrompt)` appends it LAST as a positional arg claude
   submits as the session's first turn. Zero-race by construction: no stdin injection, no
   TUI-init Enter-eaten window (the Enter-retry class of problem does not exist here), and the
   prompt fires a real `UserPromptSubmit`, so state (`Running`) and the Typed record ride the
   normal push path. PS-quoted via `PsDoubleQuote` (`` ` ``→` `` ``, `"`→`` `" ``, `$`→`` `$ ``)
   because the managed commandline is invoked by the pwsh host's `&` operator
   (`BuildPwshHostedCommandline`) — PowerShell parsing, not CreateProcessW, governs its args (a
   claude launch is never the `cmd /c` batch form: native-exe-only policy). FRESH launches only —
   `_LaunchClaudeSession` structurally drops the prompt on any restore/resume (its turn already
   ran and lives in the transcript).
6. Nav trail: `[nav] handover-begin <sid8> md=…` ↔ `[nav] handover-done new=<sid8'> from=<sid8>`
   (the fork-managed begin/end convention — an unpaired begin means a crash mid-action).
7. **Repeatable by design** — every `/handover` in a conversation arms its own await and spawns
   its own successor; chains title naturally (`(handover)` → `(handover 2)` when run from the
   successor, registry-bumped when run again from the origin).

## 6. The shipped command definition — the ONE write outside the profile

A typed `/handover` must BE a command (Claude Code rejects unknown slash commands client-side —
nothing would ever reach the transcript), so engine init materializes
**`<claude-config>/commands/handover.md`** (`EnsureHandoverCommandFile`; `CLAUDE_CONFIG_DIR` >
`~/.claude`, the `ClaudeProjectsDir` resolution) — logged `[engine] handover command: <path>`.

**Strictly create-if-absent:** a file that exists — the user's own `/handover`, or an edited
copy of ours — is NEVER overwritten; from the first materialization the user owns the wording.
This is deliberately called out as **the one place Agentmaster writes outside its active
profile** (a global `~/.claude` config mutation — additive and inert until the user actually
types `/handover`; the same product-decision class the Codex-C3 `~/.codex` hooks change was
deferred over, shipped here because the feature IS the ask). The definition's load-bearing
lines, which the await depends on: *use the Write tool* (a shell-redirect write is invisible to
the transcript's tool_use stream) and *name it `HANDOVER-<topic>.md`* (the leaf preference).

## 7. What was deliberately NOT changed

* **State machine untouched** — a command echo remains a non-event (§2); the successor's
  `Running` comes from its own real `UserPromptSubmit`.
* **No injection into the ORIGIN session** — the origin is read via the transcript only
  (Rule #13-adjacent); the action spawns a NEW session.
* **Codex excluded** — rollouts carry no command echoes; `/handover` is Claude-only (the
  successor is a Claude session).
* **Observe-only externals excluded** — the scanner reconciles registry sessions only; an
  external claude's `/handover` never fires (we host no tab to anchor the successor to).

## 8. Follow-ups (non-blocking)

* A push fast-path (the `UserPromptSubmit` hook's prompt field) behind the same feed, dedup by
  sighting identity, for sub-second latency.
* More bindings on the same infra (the generic await shapes beyond markdown: "next turn-complete",
  "next file matching \<glob\>", `/command`-driven queue operations).
* A cog off-switch (`commandBindingsEnabled`) if a user ever wants the echoes ignored.
* Surfacing an armed await on the origin tab's overlay (a dim `⏳ handover pending` row).
* First-prompt `/handover` in a brand-new session is a degenerate no-op by construction (there
  is nothing to hand over); the freshness gate also skips a `/handover` typed moments before an
  app restart — the user re-runs it. Documented, accepted.
