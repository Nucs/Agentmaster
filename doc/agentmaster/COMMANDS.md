# Slash-command bindings + the /handover integration (CommandWatch)

> Bind to `/commands` the user types into a managed Claude session, and AWAIT the session's
> follow-up activity — asynchronously, bounded, and without touching the state machine.
> First integration: **`/handover <handover-context-or-filepath>`** → await the handover
> markdown the command instructs Claude to write → open a successor tab named
> `"<origin title> (handover)"` in the same working dir, whose FIRST USER MESSAGE is that
> document's content injected verbatim. Its IN-PLACE twin
> **`/handover-here <handover-context-or-filepath>`** (§5a) reuses the whole pipeline but
> REPLACES the origin tab instead of opening a new one — the Restart-session swap into a
> fresh "New Session Here → Default" conversation, the origin archived (resumable).

Engine: `AgentMaster/CommandWatch.{h,cpp}` (+ parser/scanner seams in `SessionScanner.{h,cpp}`,
wiring in `Engine.{h,cpp}`); UI action: `TerminalPage.AgentEngine.cpp` (sink) +
`TerminalPage.AgentSessions.cpp` (`_HandleCommandHandover`). Tests: `tests/tests_commands.cpp` —
three suites (§8): `TestCommandWatch` (units + safeguard belts), `TestCommandHandoverE2E` (the
fabricated end-to-end /handover session), `TestCommandEchoRealCorpus` (real-transcript replay) —
plus the refined `/model`-echo invariant in `tests_transcript.cpp`. Hardening map: §7.

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
handler)`) — **MULTI-FILE: one command consumes EVERY qualifying markdown written after it.**
The oldest UNSEALED pending of the session COLLECTS, from each assistant message's Write/Edit
batch, every markdown whose LEAF contains the binding's hint (`"handover"` → the command's own
`HANDOVER-*.md` family; an incidental doc edit in the same turn never rides along), in write
order, case-insensitively deduped (a Write-then-Edit of one file collects once); while the
pending has collected NOTHING, a batch's first markdown is the fallback pick (the legacy
mis-named-single-file tolerance). A relative tool path resolves against the session's working
dir. **The collection SEALS at the first turn end after a match** (the definitions end the turn
right after writing, so everything belonging to one command lands in ONE turn — sealing is also
what keeps a SECOND command's writes from bleeding into the first, and what makes the
/handover-here swap happen only after the origin's turn completed), with a
`kCommandMatchSettleMs` (20s) write-silence fallback when no turn end ever arrives (a session
killed mid-turn). **The tool_use lines only prove the REQUEST** — a write may still be pending
a permission approval — so the fire is gated on the DISK: a sealed pending fires once EVERY
collected file exists non-empty (probed at seal, then per `Tick`), delivering the whole path
set to the handler. The file probe is injectable (`SetFileProbe`) so the whole machine
unit-tests offline.

**Every await is bounded** (state can never leak):

* an UNMATCHED sighting expires after `kCommandAwaitMaxTurnEnds` (2) turn boundaries — the
  command's own turn + one clarification round. Scoping the await to the command's vicinity is
  what keeps an unrelated `.md` edit three turns later from spawning a spurious tab (worse than
  a missed one — the user just re-runs the command). A MATCHED (sealed) pending never ages by
  turns (approval can span boundaries) and is bounded by the deadline alone;
* `kCommandAwaitDeadlineMs` (15 min) hard-caps everything;
* `kCommandMaxPendingPerSession` (4) caps memory, oldest evicted; FIFO across sightings (a
  second `/handover` arms its own pending; the turn-end seal pairs each command's writes with
  its own turn);
* **SAME-FAMILY SUPERSEDE — /handover and /handover-here can never race each other's files.**
  Bindings sharing a leaf hint (both use `"handover"` — one `HANDOVER-*` contract) await the
  same indistinguishable file family, so they are treated as ONE logical operation with
  different HANDLING paths, not two queued FIFO awaits: a new family sighting **re-aims** the
  await — an older UNSEALED family pending with no collected paths is **superseded** (removed,
  its durable marker retired, logged `[cmd] /handover superseded by /handover-here …`; the
  newest handling path takes the next write — a `/handover` left hanging on a clarification and
  then pivoted to `/handover-here` fires the in-place path, never a stale new-tab spawn); an
  older unsealed pending that already collected paths (structurally rare — a user echo implies
  the prior turn closed, which seals) is defensively **sealed** so it fires with its own files;
  SEALED pendings are untouched (distinct resolved operations — repeatability upheld: every
  satisfied command still fires). At most ONE unsealed family pending exists per session, so a
  family write has exactly one possible owner. A same-command re-run supersedes too (a retry is
  one operation, not two successors);
* pendings themselves stay transient in-memory — but the session's PROGRESS is durable (§3a),
  so a restart neither double-processes a fired command nor loses one that was mid-await.

Logs (hooks.log): `[cmd] /handover sighted <sid8> args="…"` (`(revived after restart, …)` when
§3a revived it) → `[cmd] /handover md matched … (N files)` → `[cmd] /handover sealed … files=N
(turn end|settle)` → `[cmd-fire] /handover <sid8> md=<p1>|<p2>`, with `[cmd-expire] …` for the
bounded ends. Unbound commands (`/model`, `/compact`, …) produce no state and no logs.

## 3a. Durable per-session progress — restart resilience without double-processing

The watch persists a tiny per-session PROGRESS record through an injectable store seam
(`SetProgressStore(load, save)`; the engine wires it to the **SessionStore KV** —
`session-store/<sid>.json`, key `cmdProgress`, value `EncodeCommandProgress`:
`"v1;p=<processedMs>;a=<cmd>@<ts>,…"`, empty ⇒ key removed, the store stays sparse). Two facts,
both keyed by the echo line's OWN transcript timestamp (the durable identity of a command
occurrence):

* **The fired watermark (`processedMs`)** — advanced (and saved) INSIDE the fire path, BEFORE
  the handler runs (at-most-once across a restart: a crash after the mark but before the spawn
  loses that fire — the user re-runs — it never doubles). A replayed echo at/under the
  watermark **never re-arms**, which closes the real double-processing holes the 60s freshness
  gate could not: **resuming a just-handed-over origin session** (its tail replays the echo +
  write fresh enough to pass the gate — previously a second successor / a second in-place swap),
  a **crash-restart within the freshness window of a fire**, and a truncation-rewind re-feed
  (also guarded in-memory by same-echo idempotence).
* **Armed markers (`armed[]`)** — one `(command, lineTs)` per sighting that armed but has not
  resolved. A replayed echo matching a marker **REVIVES** its await even past the freshness
  window (the app restarted / the session was closed mid-await and later resumed), with the
  deadline anchored at the ORIGINAL timestamp — so the 15-min bound stays absolute across the
  restart, and a marker older than it is pruned at load (garbage-collected) and never revives.
  Markers retire on fire, on turn-end/deadline expiry, and on cap eviction. A marker also
  overrides the watermark (an out-of-order fire — a later command fired while an earlier one
  still awaited its approval-held file — must not orphan the earlier await).

An echo that is neither fresh nor marked never arms — an adopted/foreign transcript's deep
history stays inert exactly as before (including a `/handover` typed into the same conversation
from OUTSIDE Agentmaster while the app was closed: no marker ⇒ no ghost successor).
`DropSession` drops only the in-memory pendings + cache — the persisted progress deliberately
survives archiving, because it is what makes the later resume/replay safe. No store wired
(harness default) ⇒ the exact pre-persistence semantics.

## 4. Fan-out — the command-action sinks

The engine's `/handover` binding (registered in `Engine.cpp` init, before the scanner starts)
does one thing: `RaiseCommandActionInWindows(sessionId, L"handover", mdPath)`; the
`/handover-here` binding is its twin with the action name `L"handover-here"`, and the
`/handover-standby` binding (§5b) its third sibling with `L"handover-standby"` (the watch's
binding lookup is name-EXACT, so no two family commands can ever cross-fire — no prefix
aliasing — and the §3 same-family supersede means they can never race each other's FILES
either: back to back, the newest one owns the await).
That is a per-window sink family (`Engine::CommandActionSink` — the `activateSinks` idiom
verbatim: registered at engine init, token-detached in `~TerminalPage` (Rule #10),
snapshot-under-lock / invoke-outside). Unlike Activate there is NO source window to exclude —
the fire originates on the scanner thread — so every window's sink runs; each hops onto its own
UI dispatcher and only the (single) window whose `_claudeTabs` hosts the origin session acts
(the sink dispatches both action names into `_HandleCommandHandover`, `inPlace` distinguishing
them).

## 5. /handover end-to-end

1. User types **`/handover <context-or-filepath>`** in a managed Claude session. The command
   DEFINITION (§6) instructs Claude: read the arg if it names a file, then **use the Write tool**
   to create ONE `HANDOVER-<topic>.md` in the cwd carrying everything a successor needs, then end
   the turn.
2. The scanner's next delta surfaces the echo → `[cmd] /handover sighted`, await armed.
3. Claude's Write tool_use(s) land in later deltas → COLLECTED (leaf preference `handover`;
   one command may write several `HANDOVER-*.md` files — §3); the turn's end SEALS the set and
   the disk probe confirms every file → `[cmd-fire] … md=<p1>|<p2>` → engine fan-out (the
   '|'-joined path set, `JoinWatchPaths`) → the hosting window's UI thread.
4. `TerminalPage::_HandleCommandHandover` (miss ⇒ no-op; archived/vanished ⇒ drop):
   * **dir** — `EffectiveWorkingDir(tabColorMode, *s)`: the same "here" every *Open New Session
     Here* uses (falls back to the launch cwd inside the resolver);
   * **title** — `DeriveSuffixedTitle(origin, L"handover")` → `"T (handover)"`, chaining bumps
     (`(handover 2)`, …) — the generalized `DeriveForkTitle` (which now delegates to it) — then
     bumped past any title already in the registry (live or archived), so sibling handovers from
     one origin never collide;
   * **FAN-OUT — one successor PER collected file** (`_HandleCommandHandover` splits the payload
     via `SplitWatchPaths`, re-asserts sanity + existence PER PATH — a vanished/insane subset is
     dropped with a log, only an empty survivor set drops the whole action): one typed
     `/handover` writing N files hands off to **N parallel successor sessions**, in write order,
     each file becoming ITS successor's **first user message** ("as if the user typed it") —
     VERBATIM, **IN FULL**, **NEVER truncated**. Titles chain per successor (`(handover)` →
     `(handover 2)` → … — each registers before the next derives);
   * **placement** — sequential slots starting at `originTab.TabViewIndex()+1`, so write order
     reads left-to-right on the strip (the *New Session Here* placement, generalized);
   * **per-file delivery** — each file goes through `ReadHandoverDocumentPrompt` (4 MiB sanity
     cap per file — a ceiling on absurdity, not a message bound; UTF-8 BOM stripped, CRLF → LF,
     stray C0 controls except `\n`/`\t` dropped — which also makes the paste framing below
     injection-proof, since ESC can never survive into the content — outer whitespace trimmed),
     then tiers on ITS OWN `PsEscapedCost` (the PsDoubleQuote cost model — ` `` ` `"` `$` cost
     2) against `kHandoverPromptEscapedBudget` (11,500 — the size math in step 5):
       - **fits** → that successor's launch commandline positional prompt (the zero-race channel);
       - **over budget** → that file rides the **ConPTY stdin instead — a streamed pipe with NO
         CreateProcessW ceiling**: parked as a Pending prompt at the FRONT of ITS successor's
         queue (the durable, visible carrier — Auto Testing lists it, Send-now can deliver it
         manually, a restart keeps it) and paste-injected by `_PumpHandoverInjections` (below)
         the moment that session actually starts;
       - **unreadable / whitespace-only / beyond-cap** (a rewrite-in-flight race past the §7
         re-asserts) → that successor gets the pointer-style prompt naming its file, so its
         handover still functions and nothing was silently cut.
     `handover-done` carries parallel per-file lists: `new=<sid8>,<sid8> …
     inject=content,paste` (position i == file i; a failed spawn logs `(failed)` at its position).
   * **the paste pump** (`TerminalPage::_PumpHandoverInjections`, ticked by the scanner's
     liveness probe ~2.5s): waits for `SessionInfo.started` (the control initialized and
     `Start()` ran — a pre-Connected `WriteInput` is SILENTLY dropped by ConPTY, so injecting
     earlier would lose the text) plus a 1.5s settle, then delivers via the **Send-now recipe**:
     mark `Sent` → `Inject(BuildPromptSubmission(text))` — the existing bracketed-paste submit
     (`ESC[200~ … ESC[201~` + one trailing CR), which Claude's TUI treats as ONE pasted block
     regardless of embedded newlines — → roll back to `Pending` on failure (Rule #4). The echo
     dedup marks it consumed when its `UserPromptSubmit` lands, and the scheduler's
     **Enter-retry watchdog** re-presses a swallowed submit CR — the full backstop set of any
     flight prompt, for free. Give-up deadline 10 min (a dormant never-focused tab): the entry
     drops but the prompt STAYS Pending in the queue — visible, Send-now-able, never lost.
5. The spawn is a normal fresh `_LaunchClaudeSession` — Default model, hooks wired, registry
   card, per-dir color — plus the **initial prompt on the commandline**:
   `BuildClaudeCommandline(..., initialPrompt)` appends it LAST as a positional arg claude
   submits as the session's first turn. Zero-race by construction: no stdin injection, no
   TUI-init Enter-eaten window (the Enter-retry class of problem does not exist here — a
   MULTI-LINE body is safe precisely because nothing is typed into the TUI: the whole document
   is ONE argv), and the prompt fires a real `UserPromptSubmit`, so state (`Running`) and the
   Typed record ride the normal push path. PS-quoted via `PsDoubleQuote` (`` ` ``→` `` ``,
   `"`→`` `" ``, `$`→`` `$ ``) because the managed commandline is invoked by the pwsh host's `&`
   operator (`BuildPwshHostedCommandline`) — PowerShell parsing, not CreateProcessW, governs its
   args (a claude launch is never the `cmd /c` batch form: native-exe-only policy), and a PS
   double-quoted string legally spans newlines. **The size ceiling that forces the budget:** the
   prompt crosses TWO CreateProcessW hops capped at 32,767 chars each, and the binding one is the
   outer `-EncodedCommand` wrap — base64(UTF-16LE) costs ≈2.67× the inner commandline, so the
   inner must stay ≤ ~12,200 chars; minus launch overhead that leaves ~11,500 POST-escape chars
   for the prompt (the constant, with margin; ` `` ` `"` `$` each cost 2 — why the budget is
   escape-aware, not raw-length). FRESH launches only — `_LaunchClaudeSession` structurally
   drops the prompt on any restore/resume (its turn already ran and lives in the transcript).
6. Nav trail: `[nav] handover-begin <sid8> md=…` ↔ `[nav] handover-done new=<sid8'> from=<sid8>`
   (the fork-managed begin/end convention — an unpaired begin means a crash mid-action).
7. **Repeatable by design** — every `/handover` in a conversation arms its own await and spawns
   its own successor; chains title naturally (`(handover)` → `(handover 2)` when run from the
   successor, registry-bumped when run again from the origin).

## 5a. /handover-here — the IN-PLACE twin (replace the origin tab)

**`/handover-here <context-or-filepath>`** reuses the ENTIRE /handover pipeline — the same
markdown await (same `"handover"` leaf preference; its definition instructs the same
`HANDOVER-<topic>.md` name), the same §5 guards/title/dir resolution, the same per-file
delivery tiers and fan-out — and differs in exactly ONE step: **where the FIRST successor
lives**. The FIRST collected file's successor **REPLACES the origin tab in place**
(`TerminalPage::_RestartTabIntoFreshSession`, dispatched by `_HandleCommandHandover`'s
`inPlace` flag); each ADDITIONAL file's successor opens as a new tab beside it (the §5
fan-out):

* **The spawn is "Open New Session Here → Default"** — a brand-new FRESH conversation:
  `BuildClaudeSpawn` with a newly minted id, NO resume/fork, NO model override (the settings
  model decides), hooks wired, the same effective working dir — with the handover content
  riding the commandline's positional prompt when the §5 commandline tier fits (the paste /
  pointer tiers behave identically, keyed on the successor's id).
* **The swap is the Restart-session mechanism** (`_RestartManagedSession`'s recipe): build the
  new ConPTY connection (`inheritCursor` — the origin conversation's scrollback stays readable
  above the successor), `HardResetWithoutErase`, `Connection(newConn)`, `Start()` — with
  deliberately NO explicit `oldConn.Close()` first (the swap revokes the output handlers and
  THEN closes the old connection, so the dying origin claude's teardown banner never prints
  into the successor's pane). The swap targets the ORIGIN SESSION's pane specifically —
  resolved by its connection's `WT_SESSION` == the record's `tabToken` (a user split's focused
  sibling shell is never the victim), first-terminal-pane fallback. The
  `_restartPaneConnection` NotConnected guard applies (never swap a never-initialized
  control); `SuppressAutoClose` is (re-)applied so an ADOPTED origin's plain profile pane
  keeps the readable-dead-pane behavior.
* **The bookkeeping is ONE `_BindClaudeSessionToTab` call** — its re-home block (the
  in-session `/resume` machinery, reused verbatim) archives the ORIGIN record (`live=false`,
  injector cleared, the `[rehome]` + `tab-swap` nav trail; **the origin stays resumable from
  the Sessions browser** — Close-semantics, nothing deleted), re-keys
  `_claudeTabs`/`_claudeOverlays` onto the successor, binds the successor's stdin injector
  (Rule #3), pins the successor's own title (Rule #11 — `"<origin> (handover)"`, same bumping
  as /handover), re-colors, re-attaches the overlay, and marks it **started** (the control is
  already initialized and `Start()` ran → `SetStarted(true)` immediately — exactly what the
  §5 paste pump gates on, so the over-budget tier works unchanged). The successor's fresh
  registration carries the cog's Autorunner defaults plus the restore-fresh CONTINUITY seeds
  (the origin's `inferredWorkingDir` + Individual-mode `tabColorHex` — same tab, same work,
  best first guess).
* **Degrades, never loses the handover**: a refused/failed swap (origin tab torn down in the
  fire → UI-hop window, a dormant control, a failed connection build, no claude.exe) falls
  back to the classic §5 new-tab spawn, logged
  `[handover-here] … in-place restart unavailable - falling back …` + `(fallback=new-tab)` on
  the done line.
* Nav trail: `[nav] handover-here-begin <sid8> md=…` ↔ `[nav] handover-here-done new=<sid8'>
  from=<sid8> inject=content|paste|pointer` (the same pairing rule — an unpaired begin means a
  crash mid-action), plus the mechanism line `[handover-here] <new> replaced <old> in place …`
  and the re-home's own `[rehome]`/`tab-swap`.
* A crash between the swap and the debounced `WindowRecord` save reopens the ARCHIVED origin
  ref on next launch (the pre-existing in-session `/resume` re-home characteristic; the
  successor is still in `sessions.json`, resumable) — `_ScheduleWindowRecordSave` is kicked
  right after the swap to shrink that window.

## 5b. /handover-standby — the FILL-NOT-SEND member (one Enter away)

**`/handover-standby <context-or-filepath>`** is /handover with the SUBMIT withheld: each
collected file still opens its OWN successor tab (the §5 fan-out, titles, dir, model — all
identical), but the briefing is **TYPED into that session's input box and NEVER sent** — the
user reviews the pre-filled message in the successor tab and presses Enter themselves. "A
handover in standby": nothing runs until they do. Its own definition `handover-standby.md`
(`EnsureHandoverStandbyCommandFile` / `ShippedHandoverStandbyCommandHashes` + `…Text()`, the
same §6 digest write policy through the ONE shared core; the three histories are asserted
pairwise DISJOINT), the same markdown await on the same `"handover"` leaf — so all three
commands are ONE §3 supersede family (a /handover pivoted to /handover-standby fires the
standby path, never the stale spawn) — and the canonical fan-out action `L"handover-standby"`
dispatched to `_HandleCommandHandover(…, standby=true)`.

**The delivery is a fourth tier — FILL — and it is the ONLY tier standby uses:**

* **The commandline tier is structurally excluded** — claude auto-submits a positional prompt
  at launch, the exact submit standby exists to prevent. Every standby file (readable content
  AND the unreadable-file pointer text alike) rides the pump.
* **The fill is `BuildPromptFill`** (SessionModels.h) — byte-for-byte
  `BuildPromptSubmission` minus its ONE trailing submit CR (the submit builder now delegates:
  `fill + L"\r"`, so the two channels can provably never drift): the same bracketed-paste wrap
  keeps embedded newlines literal, the same CR/CRLF→LF normalization, and NO carriage return
  anywhere in the emitted bytes — nothing this builder produces can ever submit.
* **Deliberately NOT a queue row.** A `Pending` row could be auto-SENT by a Full-mode
  autorunner (fresh successors are stamped with the cog's default mode) and a `Sent` row would
  arm the scheduler's Enter-retry watchdog, whose whole job is to PRESS ENTER — either would
  defeat standby. The document lives only in the pump entry
  (`PendingHandoverInjection.standbyText`); the briefing FILE is the durable copy (below).
* **The pump's standby lane VERIFIES the fill** (`_PumpHandoverInjectionsImpl`): no echo ever
  confirms a fill (nothing is submitted), so after started + the same 1.5s settle it injects
  the fill and then READS THE INPUT BOX BACK (`ControlCore::ReadPendingInputDraft` — the
  PENDING_INPUT.md primitive) on the following ticks: a non-empty box == **verified** (logged
  `draft VERIFIED in the input box … - one Enter away`); a box still empty after
  `kStandbyVerifyMs` (12s) means the TUI ate the paste pre-raw-mode (the Enter-retry gotcha's
  text-eaten sibling) → **re-fill**, at most `kStandbyMaxAttempts` (2) times, then give up
  with the file kept. Pre-fill, a box already holding ANY text means the user is typing —
  the fill holds off entirely (never append to a human draft; the 10-min deadline caps). And a
  session the USER DROVE is never touched — the pure `StandbySessionTakenOver` latch
  (SessionModels.h, unit-tested), checked in BOTH phases: a turn in flight NOW (state off
  Idle/Waiting), or proof one EVER ran (`turns.lastPromptUnixMs` push-side /
  `convLastActivityUnixMs` pull-side — both 0 on a fresh standby successor until a real
  submit). The latch is what closes the fast-turn hole: a prompt submitted AND completed
  between pump ticks lands the state back at rest with an empty box, which a state check alone
  would misread as an eaten paste and RE-FILL the already-delivered briefing; pre-fill it also
  keeps a fill out of a session the user claimed with their own prompt during the settle.
  Hands off always keeps the file. The pending-input monitor then lights the
  "3 dots" unsent-draft indicator on the standby tab for free (the draft IS a pending input).
* **Delete-after arms only on a VERIFIED fill** — never in `_HandleCommandHandover`. An
  unverified/undelivered draft always leaves its `HANDOVER-*.md` on disk (it is standby's only
  durable copy — the pump entry is transient, and a restart before the fill simply leaves the
  successor resumable with the file intact); the pointer text NAMES the file, so a
  pointer-standby never arms at all.
* **Full settings suite:** its own Commands-tab section (enable + rename + status line +
  Successor-model combo — `commandHandoverStandbyName`/`…Enabled`/`…MaterializedName`/
  `…SuccessorModel`, the §6a name heal generalized to `ResolveCommandNameTriple`), and it
  participates in every family-wide §6b/§6c rule: the title rewrite, the file-match regex, the
  write location, delete-after, and the per-message model + title hints
  (`/handover-standby [fable] …`, `/handover-standby [my title] …`).
* Nav trail: `[nav] handover-standby-begin <sid8> md=…` ↔ `[nav] handover-standby-done
  new=<sid8'> from=<sid8> inject=standby|pointer-standby`, plus the mechanism lines
  `[handover-standby] … briefing armed for the FILL …` → `… briefing FILLED into the input box
  (chars=N, attempt k/2) - NOT submitted, verifying` → `… draft VERIFIED … - one Enter away`
  (or the give-up/took-over lines, every one of which says the md was KEPT).

**Threading note (a pre-existing race this work fixed):** `_PumpHandoverInjections` and
`_SweepHandoverDeletes` used to run SYNCHRONOUSLY on the scanner's liveness-probe thread while
`_HandleCommandHandover` wrote the same UI-thread-only maps from the UI thread — a genuine
cross-thread `unordered_map` race (narrow, never observed, real). Both are now
`fire_and_forget` + `Impl` coroutines that marshal to the UI thread first (the
`_ScanPendingInput` idiom, terminate-nets included) — which is also what makes the standby
lane's `ReadPendingInputDraft` (a UI-affine TermControl call) legal.

## 6. The shipped command definition — the ONE write outside the profile

A typed `/handover` must BE a command (Claude Code rejects unknown slash commands client-side —
nothing would ever reach the transcript), so engine init materializes
**`<claude-config>/commands/handover.md`** (`EnsureHandoverCommandFile`; `CLAUDE_CONFIG_DIR` >
`~/.claude`, the `ClaudeProjectsDir` resolution) — logged `[engine] handover command: <path>` —
**and its in-place twin `handover-here.md`** (`EnsureHandoverHereCommandFile`, logged
`[engine] handover-here command: <path>`; shipped history `ShippedHandoverHereCommandHashes`).
Both route through the ONE shared core `EnsureShippedCommandFileIn(configDir, leaf,
shippedHashes, currentText, label)`, so the write policy below can never drift between the two
files.

**Create-if-absent + a version-aware UPGRADE, gated on SHA-256:** each command carries a
**shipped-version history of digests** (`ShippedHandoverCommandHashes` /
`ShippedHandoverHereCommandHashes` — the SHA-256, lowercase hex, of each version's UTF-8 bytes
exactly as `WriteFileUtf8` lays them on disk; oldest first, the LAST entry being the digest of
the CURRENT text, `ShippedHandoverCommandText()`). On every engine init the on-disk file is read
(bounded 64 KiB) and hashed (`AgentMaster/Sha256.h` — pure, header-only, hand-rolled like
`Base64Encode` so no bcrypt/crypt32 has to be threaded through the lib + harness + CLI builds):
a digest matching **any** entry (after undoing the §6a name and §6c write-location renderings —
`MatchShippedCommandVersion`) means the file is a pristine OURS, untouched by the user, and it is
rewritten **iff its bytes are not already exactly what we would write** — an older version
upgrades, and a current version re-renders when the configured name or write location changed
(logged `[engine] handover command upgraded (shipped vN -> vM)` / `(shipped vN re-rendered)`),
while a byte-identical file is a no-op (no rewrite, no log line). Anything else — the user's own
`/handover`, or an edited copy of ours — is NEVER overwritten (the `ApplyEnvDefaults` discipline:
a user edit sticks forever; §6c's Reinstall button is the one deliberate, confirmed exception).

*Why digests and not the texts:* the rule only ever asks "are these bytes something WE shipped,
unmodified?" — a content-IDENTITY question a 64-char digest answers completely — so a superseded
version costs ONE line instead of a permanently frozen 2–3 KB literal (the retired texts stay in
git history; `git log -S<digest>` lands on the commit that retired one). The list is
**APPEND-ONLY and frozen**: editing or dropping an entry makes every install still carrying that
version read as user-owned, and it would never auto-upgrade again. **To ship a new version:**
edit the text, rename the literal to `kHandoverCommandV<N+1>`, run the harness — the failing
`last entry == sha256(current text)` check PRINTS the digest to append — and append it. That
gate is what makes the digest history self-maintaining: a text edit that forgot its digest fails
the suite instead of silently orphaning the upgrade rule. (The engine harness additionally pins
`Sha256.h` to the NIST vectors + the 55/56/63/64/65-byte padding edges, and drives the whole
create/upgrade/never-overwrite policy over a SYNTHETIC command — the real histories keep digests
only, so a prior version's bytes no longer exist to lay on disk.) This is deliberately called out as **the one place
Agentmaster writes outside its active profile** (a global `~/.claude` config mutation — additive
and inert until the user actually types `/handover`; the same product-decision class the
Codex-C3 `~/.codex` hooks change was deferred over, shipped here because the feature IS the
ask). The definition's load-bearing lines, which the await depends on: *use the Write tool* (a
shell-redirect write is invisible to the transcript's tool_use stream) and *name it
`HANDOVER-<topic>.md`* (the leaf preference). **V2 (content injection)** additionally briefs
Claude that the file's content is injected VERBATIM as the successor's first user message — so
the document must be written AS a direct, self-contained briefing TO the successor (imperative,
second person). **V3 (never truncate)** drops V2's "long documents truncate to a pointer"
caution — the document is delivered IN FULL whatever its size (the paste tier, §5), so
thoroughness is encouraged, not traded against delivery. **V4 (multi-file)** permitted a
split briefing delivered joined; `handover-here.md` **V2** carried the same line. **V6 /
handover-here V4 (FAN-OUT — the current semantics)** supersede that: "EACH `HANDOVER-*.md`
file you write in this turn starts its OWN successor tab, in write order" — one command
writing N files fans out N parallel successors, so every file must be a SELF-CONTAINED
briefing to a DIFFERENT session ("never write 'continue in file B'" — the live multi-file
test's origin wrote exactly that cross-reference under the join semantics, which the fan-out
text now prevents); handover-here's first file replaces the origin tab, additional files open
beside it.
**V7 / handover-here V5 (the CONFIGURABLE WRITE LOCATION)** lift the target
folder out of the prose onto its own rendered `WRITE IT IN: <phrase>` line, defaulting to the
session scratchpad instead of the working directory — see §6c, which also documents the marker
contract every future version must keep and the identity inverse that keeps the digest history
valid.
**V8 / handover-here V6 / handover-standby V2 (the ARGUMENT-HINT — the current texts)** add the
official Claude Code **`argument-hint:` frontmatter field** (shown beside the command in the `/`
autocomplete menu; `.claude/commands/*.md` files take the same frontmatter as skills) spelling the
§6b per-message syntax right where the user types it:
`[model] [title] <context-or-filepath> - brackets optional + typed literally, e.g. /handover
[fable] [my title] finish the tests; a [x] naming no model becomes the title` (each command's
example names ITS OWN token — rename-render-safe, and standby's example deliberately shows the
TITLE-ONLY fallback form). The value is **single-QUOTED YAML** — a plain scalar starting `[`
parses as a flow sequence and would break the whole frontmatter block — and the harness pins the
quoted prefix as a sentinel, so a future edit can't silently unquote it.
**V5 / handover-here V3 (the SELF-INVOCATION guard — found by a live skill-creator review):**
Claude Code lists the commands as invocable SKILLS, but a MODEL-initiated Skill invocation
writes **no `<command-name>` transcript echo** (proven empirically against a live transcript —
the invocation is an assistant `tool_use`, not the typed-command expansion), so the
CommandWatch never arms for it and the prior texts' closing "Agentmaster is watching" promise
was FALSE on that path: the model would write the file, end its turn, and nothing would ever
pick it up (silently confusing; conversely it also means Claude can never trigger the
tab-replacing /handover-here on its own — that safety property is preserved). The guard
paragraph tells a self-invoked model to write NOTHING and redirect the user to TYPE the
command instead.

## 6a. Customizable names + disable — the cog's "Commands" tab

All three family commands (/handover · /handover-here · /handover-standby §5b) are
user-customizable from the Settings cog's **Commands** tab: each can be
**RENAMED** (the word typed after `/` — which is also the definition's file leaf, `<name>.md`)
and **DISABLED** (no definition file materialized, no binding registered — the command simply
does not exist). **Everything applies at the NEXT START only**: the definition files and the
CommandWatch bindings are set up once at engine init and deliberately never re-bound mid-run
(the scanner worker reads the binding set unsynchronized — engine-init registration before
`Start()` is the happens-before), so the tab's per-command status lines stage a pending change
explicitly (`Active as /handover → becomes /ho after restart` / `DISABLED after restart` — the
PROFILE row's idiom, live state read from the disk markers below).

* **Settings model** (`AppSettings`, settings.json): `commandHandoverName`/`Enabled` +
  `commandHandoverHereName`/`Enabled` + `commandHandoverStandbyName`/`Enabled` (cog-owned),
  stored NORMALIZED
  (`NormalizeCommandName` — lowercase ASCII slug `[a-z0-9-_]`, `/`-stripped, ≤64 chars; a
  hand-edited junk value self-heals on load) and **collision-healed**
  (`ResolveCommandNameTriple` — the watch's binding lookup is name-exact and
  `BindMarkdownAwait` is last-wins, so no two commands may share a name: priority handover >
  here > standby, the LATER command yields to its own default on a collision, and an earlier
  name squatting a later default is evicted to ITS OWN default — the heal loop provably lands
  three distinct slugs). Absent keys reproduce the shipped `/handover` + `/handover-here` +
  `/handover-standby` exactly.
* **The RENDER / IDENTITY pair** (`RenderShippedCommandText` /
  `NormalizeCommandBytesForIdentity`, ClaudeSpawn): a renamed command's definition must SAY
  the new name (the self-invocation guard tells the user what to TYPE), so materializing
  substitutes every `"/<default>"` token — **word-boundary bounded**, so a `/handover`
  substitution can never corrupt a `/handover-here` mention — while the §6 histories stay
  digests of the DEFAULT-name texts: identity questions about a custom-named file substitute
  the name BACK over the raw UTF-8 bytes and hash that (names are ASCII slugs, so the byte
  substitution can never split a multi-byte char). One trick, and every shipped version is
  recognizable under ANY name — no per-name digest freezing, and a custom-named pristine file
  still auto-UPGRADES when a new version ships.
* **The engine-init reconcile** (`ReconcileHandoverCommandFiles` →
  `ReconcileShippedCommandFileIn` per command): the durable **materialized-name markers**
  (`commandHandoverMaterializedName`/`…HereMaterializedName`/`…StandbyMaterializedName` —
  engine-owned AppSettings fields,
  RMW'd at init, preserved from disk by BOTH cog-Save preserve blocks; absent keys read as the
  DEFAULT names so a pre-feature install's on-disk files migrate correctly — for standby, whose
  file may never have existed, the migration's remove-of-nothing is a no-op — `""` == nothing
  materialized) record which file the LAST init wrote. When marker ≠ wanted (rename, or
  disable ⇒ wanted = none), the old file is **deleted ONLY when its (name-normalized) digest
  matches ANY shipped version — including the current one** (`RemoveShippedCommandFileNamedIn`;
  unlike the upgrade walk, which excludes the last entry): pristine-ours migrates away so
  Claude stops offering a dead name, while **a user-edited file is NEVER touched** (their
  content, their command — a disabled command's edited file keeps working as THEIR command,
  just unwatched). Then the wanted name materializes (`EnsureShippedCommandFileNamedIn`,
  create-if-absent + upgrade) and the markers RMW to the new reality. A failed write leaves the
  marker empty — the next init just tries again (self-healing); logs:
  `[engine] handover command: <path>` / `… command removed (renamed/disabled; pristine ours)` /
  `… command disabled (no definition materialized)`.
* **Bindings**: engine init registers `BindMarkdownAwait(<configuredName>, "handover", …)` only
  for ENABLED commands; the fan-out **action names stay the canonical
  `L"handover"`/`L"handover-here"`/`L"handover-standby"`** whatever the typed names are, so the
  per-window sinks and
  `_HandleCommandHandover` never see a rename (zero UI-layer changes). The same-family
  supersede (§3) keys on the shared LEAF HINT, not the names — renamed commands still
  supersede each other correctly. An old `/handover` echo replayed after a rename finds no
  binding and stays inert.
* **Caveat (pre-existing seam, §gap-3 class)**: the dev and release installs share
  `~/.claude/commands` but keep separate settings — a rename/disable in one install can be
  "undone" by the other's next init re-materializing ITS configured names. Run one install
  primarily, or configure both alike.

## 6b. Successor shaping — model · title rewrite · file match · delete-after

The Commands tab's second half configures what a handover PRODUCES. Unlike the names/enables
above, most of it is consumed at ACTION time (`_HandleCommandHandover` reads the live
`_appSettings` when a handover fires), so it applies to the **next handover right after Save —
no restart**. The ONE exception is the file-match pattern, which rides the engine-init binding.

**The shared regex component** (`AgentMaster/RegexUtil.h`, header-only + pure — the
`PromptAnchor.h` idiom, so the engine, the UI layer, and the harness share one definition).
Every user-typed pattern in Agentmaster goes through it instead of touching `std::wregex`,
because raw `std::wregex` is the wrong shape for user input three ways: **construction THROWS**
on an invalid pattern (and a pattern being edited is invalid on most keystrokes), cost is
**unbounded** (a pathological pattern can backtrack for seconds), and semantics would drift per
call site. `RegexIsValid` / `RegexSearch` / `RegexReplace` never throw (invalid ⇒ no-match /
input unchanged), cap the pattern (512) and input (4096) lengths, and fix ONE flavor —
ECMAScript, `regex_search` semantics (anchor with `^`/`$` for whole-string), optional
case-insensitivity, `$1` backrefs, replace-ALL. `RegexReplace`'s `applied` out-param is the
"configured AND it did something" signal the title fallback keys on. Its catches are the
documented Rule #18 *expected control flow* exemption (an invalid pattern mid-edit is the normal
state; the cog surfaces invalidity to the user instead of flooding hooks.log).

* **Successor model** — `commandHandoverSuccessorModel` / `commandHandoverHereSuccessorModel`,
  **per command** (`commandHandoverSuccessorModel` / `…Here…` / `…Standby…`): `""` ==
  **Default** (the settings `model` decides — the shipped behavior),
  else a model id threaded as this launch's `--model <id>` through the EXISTING launch-model
  picker seam (`BuildClaudeCommandline`'s `modelOverride`) — for the new-tab path via
  `_LaunchClaudeSession`, for the in-place path via `_RestartTabIntoFreshSession`'s new
  `modelOverride` param. Every file of one command's fan-out launches with that command's pick.
  The cog offers **Default** + the `launchModels` list, rebuilt at each cog open; a stored id no
  longer in the list is listed as `(custom) <id>` so it round-trips instead of silently resetting.
* **The per-MESSAGE model + title hints** (`ParseHandoverArgsHints`, pure + tested;
  `PickModelFromArgsHint` is its model-only view — one parser, so the two can never drift) — the
  typed command's LEADING words may pick the model and/or pin an explicit successor TITLE for
  THAT handover alone, overriding the combo above / the title naming below:
  `/handover [fable] do a b c`, `/handover fable 5: fix the tests`,
  `/handover [fable] [my title] do a b c`, `/handover-standby [my title] fix x`.
  **Model** matching is **partial + caseless + characters-only**: the hint and BOTH sides of
  every `launchModels` entry (display name AND model id) fold to lowercase `[a-z0-9]`, and the
  hint hits when it is a **substring** of either side (`fable` ⊂ `fable5`/`claudefable5`;
  `sonnet` ⊂ `claudesonnet5`); the first list entry wins a tie. Only the args' FIRST LINE's
  leading portion is consulted: the **bracketed** form `[hint]` is explicit (any non-empty fold;
  an unterminated FIRST bracket reads as plain context — no hints at all), the **bare** form
  requires the FIRST word to hit on its own (folded ≥ 3 chars — a stray `a`/`do` can never pick
  a model) and then greedily extends word-by-word (≤ 4) while the longer fold still matches,
  longest hit winning (`fable 5 do x` → `fable5`, stopping before `do`).
  **Title** is bracketed ONLY, in one of two positions: the FIRST bracket **when its body
  matches no model** (or none are configured) **falls back to being the title** —
  `/handover-standby [my title] …` titles without picking — or the very NEXT token after a
  recognized model hint, bracketed or bare (`[fable] [my title] …`, `fable 5 [my title] …`); a
  bracket deeper in the sentence is never consulted. The body is **verbatim** (any characters —
  `[my asd \n !!_ title]` keeps its punctuation and its literal backslash-n), edge-trimmed,
  unusable when blank, degenerate-capped at 255 like every title path; the explicit title
  outranks the §6b find/replace rewrite AND the classic `"(handover)"` naming, and the per-file
  registry uniqueness bump still applies (a fan-out's later files walk `<title> (handover)` /
  `(handover 2)` …).
  Plumbing: the echo's `<command-args>` rides the fire verbatim — `CommandActionSink` /
  `RaiseCommandActionInWindows` / the per-window sink / `_HandleCommandHandover` all carry an
  `args` leg — and the hints are resolved at ACTION time against the live `launchModels`.
  Nothing is stripped anywhere: the origin already received the full text as `$ARGUMENTS`
  (harmless context) and the successor's first message is the FILE content. Logged
  `[handover] <sid8> successor model from the message hint: <id>` /
  `[handover] <sid8> successor title from the message hint: <title>`.
  **The syntax is SURFACED in the `/` autocomplete** by the definitions' `argument-hint:`
  frontmatter (V8 / here V6 / standby V2 — see §6's version narrative), so the user sees
  `[model] [title] <context-or-filepath>` while typing the command instead of having to know it.
**The defaults are REAL VALUES, not hidden code paths.** All three regex settings ship
**seeded** (`kDefaultCommandTitleFindRegex` / `…TitleReplace` / `…FileMatchRegex` in
SessionModels.h) and **presence-gated** on load (the `launchModels` idiom): an absent key seeds
the shipped default so the cog's boxes show the ACTUAL rule ready to edit, while a **present
empty string** is a deliberate "fall back to the built-in behavior" the consumers still honor.
Nothing about the default rule is invisible any more — and the **Reset** button (below) puts a
box back to it.

* **Title rewrite** — `commandHandoverTitleFindRegex` + `commandHandoverTitleReplace`, ONE pair
  for the whole family (all three commands name successors alike). The pure, guarded
  `DeriveHandoverSuccessorTitle(originTitle, find, replace)` returns a candidate, or `""`
  whenever the rewrite does not apply — **cleared · invalid · matches nowhere · blank result** —
  and the caller then falls back to the built-in `DeriveSuffixedTitle` `"(handover)"` naming. So
  the rewrite can only ever IMPROVE a title, never lose one (Rule #11's never-empty invariant);
  the result is trimmed and capped at 255 (252 + `...`) like `DeriveSessionTitle`, and the
  caller's uniqueness bump past registry titles applies exactly as before. `$1` backrefs make
  the common shapes one-liners: append `^(.*)$` → `$1 - continued`, bump `\d+` → `4`, strip a
  prefix, etc.
  **The shipped default pair reproduces the classic naming EXACTLY, chaining included** —
  find `^(.*?)(?: \(handover(?: \d+)?\))?$`, replace `$1 (handover)`. It is deliberately NOT the
  naive `^(.*)$` → `$1 (handover)`: that would STACK `"(handover) (handover)"` on a repeat, the
  exact bug `DeriveForkTitle` exists to prevent. Instead the pattern optionally EATS an existing
  `" (handover)"` / `" (handover N)"` ending and re-adds it, so a chained handover resolves to
  its origin's own title — which the caller's uniqueness bump then walks to `(handover 2)`,
  `(handover 3)`, … Unit-tested against `DeriveSuffixedTitle` for byte-parity on a plain title
  and for the chain/interior-mention cases.
* **File match** — `commandHandoverFileMatchRegex`, shared by BOTH commands (they are one await
  family — a per-command pattern would split the §3 supersede family, whose key stays the leaf
  HINT). It ships seeded with `HANDOVER\-` — the regex spelling of the shipped definitions' own
  `HANDOVER-<topic>.md` naming contract — and a markdown qualifies when the pattern regex-SEARCHES
  its file NAME (case-insensitive, so `handover-notes.md` matches too; anchor for a whole-name
  match, e.g. `^BRIEF-.*\.md$`). The default is deliberately **tighter than the contains-"handover"
  leaf hint** it spells out: a repo doc merely MENTIONING handover in its name (`handover.md`,
  `HANDOVER_NOTES.md`, `old-handover.md`) is not collected as a briefing — such a file still rides
  the nothing-collected-yet tolerance below when it is the turn's only markdown, so a lone
  mis-named briefing keeps working. **Clearing** the box falls back to the (looser) built-in
  contains-hint. A **valid pattern is authoritative** — a
  `HANDOVER-*.md` no longer qualifies unless the pattern says so — and a **CUSTOMIZED** one (≠ the
  shipped default) additionally **suppresses the legacy nothing-collected-yet fallback** (the
  batch's first markdown: tolerance for a mis-named single briefing file, which only makes sense
  under the shipped rule — with it live, a first batch writing an unrelated `notes.md` would be
  collected as the briefing and spawn a successor from an incidental doc edit). That policy is
  **caller-owned** (`BindMarkdownAwait`'s `allowFirstMarkdownFallback`, set by Engine.cpp to
  `pattern == kDefaultCommandFileMatchRegex || pattern.empty()`) precisely BECAUSE the default is
  now a real setting value — the watch itself can no longer tell "default" from "customized".
  An **invalid** one falls back to the hint (fallback included) —
  belted twice: the engine validates once at bind time and logs
  `[engine] handover file-match regex INVALID - using the built-in 'handover' leaf hint: …`, and
  the watch re-checks per leaf. **Restart-applied** (`BindMarkdownAwait`'s new optional
  `leafMatchRegex`; bindings register once at init). NOTE: the shipped definitions still tell
  Claude to write `HANDOVER-<topic>.md` — which is exactly what the default pattern spells — so a
  custom pattern normally pairs with an edited definition (which the §6 policy then treats as
  user-owned — by design). (The seed is presence-gated and never re-seeded, so an install that
  already SAVED the earlier `handover` seed keeps it — and, differing from the shipped default, it
  now reads as CUSTOMIZED; clear the box or retype `HANDOVER\-` to adopt the new rule. Only
  unreleased builds can be in that state — the seeding never shipped in a tagged release.)
* **Delete after launch AND successful start** — `commandHandoverDeleteFileAfterLaunch` (default
  **OFF**; deleting user-visible files is opt-in). The delete is **DEFERRED, not fired at spawn**:
  `_HandleCommandHandover` only ARMS an entry (successor id → md path) and
  `TerminalPage::_SweepHandoverDeletes` — ticked on the same liveness pass as the paste pump —
  removes the file once the successor has **actually STARTED** (`SessionInfo.started`, i.e. its
  ConPTY/claude really launched). Why deferred: a successor opened in a **background tab starts
  LAZILY** (WT builds the control on first layout, so claude.exe may not run for minutes), and a
  launch that never comes up must leave the briefing on disk to re-run. The four outcomes:
  **started** ⇒ delete (delivery is secured — the **content** tier put the whole document on the
  launch commandline, the **paste** tier parked it durably at the FRONT of the successor's queue,
  `sessions.json`: restart-safe, Send-now-able); **gone/archived before starting** ⇒ drop the
  entry and **KEEP** the file (the successor died — the md is the only copy the user can act on);
  **past the 10-min deadline** (the pump's) ⇒ give up, keep the file; **delete failed** (locked)
  ⇒ logged, file left in place. The **pointer** tier never arms an entry at all (its successor's
  first message NAMES the file), and a **failed spawn** never arms one. Logs:
  `[handover] deleted md after successful hand-off (successor=<sid8> started): …` ·
  `[handover] <sid8> successor gone before start - md KEPT: …` ·
  `[handover] <sid8> successor never started within 10 min - md KEPT: …` ·
  `[handover] delete-after-hand-off FAILED (le=…), file left in place: …`.
  This is the answer to `HANDOVER-*.md` litter accumulating at repo roots (§gap-8).

**Per-tab Reset (the cog footer).** A **Reset** button sits LEFT of Cancel and is shown only
while the active tab supplies a reset handler — today just **Commands**; every other tab passes
`nullptr` to `addSettingsTab` and simply doesn't offer it. The mechanism is deliberately generic
(`_settingsTabResets`, index-aligned with the tab buttons/panels; `_SwitchSettingsTab`
re-evaluates visibility), so wiring another tab is ONE lambda at its `addSettingsTab` call and
nothing else. A handler restores that tab's CONTROLS from a default-constructed `AppSettings` —
one source of truth, no hand-copied literals to drift — and touches **nothing on disk**: Save
commits the reset, Cancel discards it. That is also why it needs no confirm dialog (a
`ContentDialog` over this in-content modal is the XAML-Islands keypress trap). The engine-owned
materialized-name markers are deliberately NOT reset — they are disk reality, not a preference.
Logged `[nav] settings-reset tab=Commands …`.

## 6c. Where the briefing is WRITTEN — the handover file location

Until v7/v5 of the definitions, the target folder was a hard-coded phrase in the command text:
*"create ONE new markdown file **in the current working directory** named
`HANDOVER-<short-topic>.md`"*. That is the whole reason `HANDOVER-*.md` files accumulate at repo
roots — and it was never load-bearing, because a briefing's **content** is what reaches the
successor (§5's delivery tiers); the file is a courier. So the folder is now a **setting**,
`AppSettings::commandHandoverWritePath`, and the shipped default is the session **scratchpad**.

**Folder-only, family-wide.** The setting moves the DIRECTORY and nothing else: the file NAME
stays the `HANDOVER-<topic>.md` contract every other stage keys on (the §6b file-match regex
matches the **name**, the §5 fan-out gives one successor per file, delete-after acts on the
absolute path the tool_use reported), so no other stage changes at all. It is one value for both
commands, like the file-match pattern — the two are one await family (§3). Values:

| value | meaning |
|---|---|
| `scratchpad` (default) or `""` | the session's own temp scratchpad — Claude Code names it in the session's instructions; the definition falls back to the system temp folder if it has none |
| `./` | the session's working directory — the pre-§6c behavior, kept as a preset |
| `./docs`, `./handovers`, … | a folder relative to the working directory, created if missing |
| `D:\briefings` | an absolute folder, created if missing |

`NormalizeCommandWritePath` (SessionModels.h) trims the value, drops a trailing separator (but
never on the degenerate roots `./` `/` `C:\`), caps the length, and **drops** control characters,
newlines, backticks, quotes and `|` — the value is inlined into ONE line of a markdown
instruction, so anything that could break out of that line is removed rather than escaped. It
runs on the cog's Save AND on the Persistence load, so a hand-edited `settings.json` self-heals.

**How it reaches Claude: a rendered span, like the custom NAME (§6a).** Both shipped texts carry
exactly one line

```
   WRITE IT IN: your session scratchpad directory (the temp scratchpad folder your own instructions name; if you have none, use the system temp folder)
```

`RenderShippedCommandWritePath` swaps that phrase for `CommandWritePathPhrase(writePath)` when
materializing; `NormalizeCommandWritePathBytesForIdentity` swaps whatever is there BACK before
hashing. The two are exact inverses, so **every shipped version stays recognizable under any
location** and a pristine file still auto-upgrades. Two deliberate differences from the name seam:

* the inverse is **delimited** (marker → end of line), not value-driven, so it needs **no
  per-install marker** and still recognizes a location someone changed by hand;
* identity therefore tries **two candidates** — the name-normalized bytes (which match any
  HISTORICAL version verbatim; pre-§6c texts carry no marker at all, so the location
  normalization is a no-op on them) and the additionally location-normalized bytes (which match
  the CURRENT version rendered under a custom folder). `MatchShippedCommandVersion` is the one
  place that asks "are these bytes a version we shipped?", shared by the upgrade, the
  rename/disable removal, and the cog's status line.

⚠️ **Contract for future versions: keep the `WRITE IT IN: ` marker, and keep the phrase on ONE
line.** A text that drops the marker silently stops following the setting (it still upgrades —
the marker-less bytes match their own digest — it just renders nothing).

**Applies on Save, not at restart.** The location only affects the definition TEXT, never a
binding, so the cog's Save calls `RefreshHandoverCommandWritePath(settings)` right after the
settings sink persisted — the next `/handover` writes to the new folder with no restart. That
refresh re-renders the **LIVE** definition files (keyed on the engine-owned *materialized* names,
`LiveHandoverCommandNames`) and never renames, migrates or deletes anything: a **rename stays
restart-applied** (§6a), because the file and its CommandWatch binding must never disagree
mid-run. The write policy is unchanged — a file we don't recognize is never touched — which is
why the rewrite rule is now *"ours **and** not already exactly what we would write"*: that one
predicate covers a version upgrade, a name re-render and a location re-render, while a
byte-identical file stays a no-op (no write, no log).

**Delete-after pairs with it.** A briefing in a temp scratchpad has no reason to linger once its
successor holds the content, so a **fresh install** now seeds `commandHandoverDeleteFileAfterLaunch`
**ON** (the absent-key default; an install that already stored `false` keeps it), and picking the
**Scratchpad** preset in the cog ticks the toggle for you. Everything else about delete-after is
unchanged (§6b — deferred to a started successor, never the pointer tier, never a failed spawn).

**The definition files, said out loud + the Reinstall escape hatch.** Everything on this tab is
delivered by writing `<claude-config>\commands\<name>.md`, and Agentmaster **never overwrites a
definition it does not recognize**. That rule is right (a user edit sticks forever) but it has a
consequence worth stating: an edited definition **stops following these settings** — it keeps
whatever folder its own text names. So the Commands tab now carries a **COMMAND DEFINITION FILES**
section: a state line per file (`up to date` · `managed — updates on Save` · `EDITED BY YOU — left
alone` · `not installed yet`, from `InspectHandoverCommandFiles`, sampled at cog open and after a
reinstall — not per keystroke) and a **Reinstall definition files…** button. That button is the
ONE path that overwrites regardless of digest (`ForceReinstallShippedCommandFileNamedIn`), behind
a confirm that says the edits are lost, and it writes the shipped text rendered with the configured
name + location. Logged `[engine] <label> command REINSTALLED (user-requested overwrite): …` and
`[nav] commands reinstall-definitions …`.

## 7. Hardening & safeguards

The feature crosses three threads (scanner worker → engine fan-out → per-window UI dispatchers)
and consumes transcript fields we do not control, so every seam is belt-and-suspenders — under
the repo's *never-lose-a-swallowed-exception* policy (each catch logs through
`LogSwallowedException` / `AgentLogCaughtException` with the VEH throw-stack ring, so a hooks.log
line alone pins the throw site later):

* **The watch is SELF-CONTAINED** — every public feed (`OnCommandSighting` / `OnFileToolWrite` /
  `OnTurnEnd` / `Tick` / `DropSession`) runs as a function-try-block: a watch bug, a throwing
  bound handler, or a throwing injected probe can never cost the SCANNER a reconcile pass (nor
  the process its worker thread). The scanner's feed sites therefore carry no guards of their
  own (documented on the class).
* **A bound handler is caught PER FIRE** (`_fire`) — one bad handler cannot block a later fire.
* **A throwing file probe reads as "file absent"** (`_probeFile`) — the pending survives and is
  re-probed next Tick / swept by the deadline, instead of unwinding into the feed.
* **The sane-path gate** (`IsSaneWatchPath`, pure + tested): a matched path flows into a log
  line, the disk probe, and the successor's single-line launch prompt — so a resolved path
  carrying a control character, a double quote, or absurd length (malformed or adversarial tool
  input; none is a legal Windows path) is **rejected at the match** (logged
  `md match REJECTED`), leaving the pending unmatched for a later sane write.
* **Fan-out survives a dead window** — `RaiseCommandActionInWindows` catches per sink, so one
  window's tearing-down dispatcher cannot stop the fan-out from reaching the host window.
* **The UI dispatch lambda is guarded** (the command-action sink in `TerminalPage.AgentEngine.cpp`)
  — a background-originated action landing on the UI thread must never unwind unhandled (an
  uncaught throw there terminates the app); `AgentLogCaughtException` records it.
* **`_HandleCommandHandover` is a guarded function-try** and RE-ASSERTS its inputs at action
  time: `IsSaneWatchPath(mdPath)` again (the prompt/log belt), and the **md's continued
  existence on disk** (non-empty, non-directory) — the file can vanish in the fire → UI-hop
  window, and a successor pointed at nothing is worse than none (logged
  `[handover] … md vanished before spawn`, user re-runs the command). A spawn-path failure logs
  and leaves the nav trail's unpaired `handover-begin` as the forensics marker.
* **The content read degrades, never blocks — and never truncates** —
  `ReadHandoverDocumentPrompt` is itself a guarded function-try with a bounded read; an
  unreadable / whitespace-only / beyond-sanity-cap result (a rewrite race past the re-asserts)
  falls back to the pointer-style prompt (`inject=pointer` on `handover-done`), and an
  over-budget document switches CHANNEL (the §5 paste tier) instead of being cut or busting the
  CreateProcessW ceiling. The paste pump inherits the queue machinery's own belts: Send-now-
  recipe rollback on a failed inject, echo dedup, the Enter-retry watchdog, a give-up deadline
  that leaves the prompt Pending (visible + manually sendable) rather than lost.
* **The in-place path carries its own belts** (§5a): `_RestartTabIntoFreshSession` runs inside
  `_HandleCommandHandover`'s guarded function-try; the `_restartPaneConnection` **NotConnected
  guard** (never `HardResetWithoutErase` a never-initialized control — a null state machine is
  an AV) is re-applied even though the origin just ran the command (the fire → UI-hop window is
  real); the swap targets the origin session's OWN pane (tabToken-matched — a user split's
  focused sibling shell is never the victim); and every refusal/failure **degrades to the
  classic new-tab spawn** (`(fallback=new-tab)` on the done line) — the handover itself is
  never lost to the in-place nicety.
* **`EnsureHandoverCommandFileIn` is best-effort** — a failure logs (`[persist-fail]` /
  swallowed-exception) and engine init proceeds; the feature stays dormant until a later launch
  succeeds. (`EnsureHandoverHereCommandFileIn` rides the same shared, guarded core.)
* Pre-existing bounds double as safeguards: the parser's own whole-chunk catch
  (`ParseTranscriptDelta` never throws), the freshness + turn-scope + deadline + per-session-cap
  bounds (§3), and the structural fresh-launch-only gate on the initial prompt (§5).

## 8. Test coverage

`tests/tests_commands.cpp`, three suites in the standalone harness (`run-m5-tests.bat`):

* **`TestCommandWatch` — the units.** `ParseCommandEcho` across both real strata (current
  name-first user line, June message-first, system/`local_command`), the markdown/path
  predicates + the leaf preference, the parser's `Command` + `fileWritePaths` events (real
  corpus shapes, verbatim), the full watch machine against a fabricated clock + injected probe
  (arm / match / disk-await / FIFO / relative-path resolve / turn-end + deadline expiry /
  freshness replay guard / per-session cap / DropSession / never-fires-twice),
  `DeriveSuffixedTitle` (+ `DeriveForkTitle` parity), `PsDoubleQuote`, the initial-prompt
  commandline arg (empty == byte-identical to the pre-parameter form), and
  `EnsureHandoverCommandFileIn` under a **temp** config dir — never the real `~/.claude`:
  create-if-absent, the content-injection sentinel, and the digest history's sanity —
  `ShippedHandoverCommandHashes` entries all 64-char lowercase hex + unique, the V3
  never-truncate promise ("whatever its size", no truncation caution), and the **version gate**
  `last entry == sha256(ShippedHandoverCommandText())` (whose failure message prints the digest
  to append). The **upgrade policy itself** is exercised by its own suite over a SYNTHETIC
  command through the shared `EnsureShippedCommandFileIn` core — absent→create, a pristine
  OLDEST version upgrades, the version right before current upgrades, an already-current file is
  left alone, a user-owned file and a one-byte-off copy are never overwritten, an empty file is
  left alone, bytes absent from THIS command's history never rewrite (no cross-command upgrade),
  and the degenerate inputs (no config dir / empty history / empty current text) refuse outright
  — because the real histories carry digests only, so a prior version's bytes no longer exist to
  lay on disk. `Sha256.h` itself is pinned to the NIST vectors ("", "abc", the 448-bit vector,
  1,000,000×'a') plus the 55/56/63/64/65-byte padding edges. Content-injection units: `PsEscapedCost` (the tier check's
  cost model, asserted against the REAL `PsDoubleQuote` output size so the two can't drift;
  budget-sized plain text fits the commandline tier, escape-heavy text of the same raw length
  overflows to the paste tier) and `ReadHandoverDocumentPrompt` (BOM strip + CRLF→LF +
  control-char drop + trim, verbatim read-back; an oversized document comes back **IN FULL** —
  no truncation tail, last line intact, cost over the budget == the paste-tier decision;
  whitespace-only/missing → empty for the fallback). Plus the safeguard belts: `IsSaneWatchPath`
  (incl. `|`), `Join/SplitWatchPaths` round-trip,
  an insane matched path rejected at the match (a later sane write still satisfies), a throwing
  handler swallowed per fire (the watch keeps firing), a throwing probe reading as absent then
  firing on recovery. **Multi-file + seal units (§3):** several hint-matching writes across
  batches collect into ONE fire in write order (deduped; an incidental non-hint md excluded),
  no fire before the seal, the settle fallback fires without a turn end, FIFO pairs each
  command's writes with its own turn. **Durable-progress units (§3a):**
  `Encode/DecodeCommandProgress` round-trip + garbage tolerance, arming persists a marker, a
  fire advances the watermark + retires the marker, a SECOND watch instance over the same store
  never re-fires the replayed echo, a mid-await marker REVIVES a stale echo and completes, a
  past-deadline marker prunes at load, same-echo idempotence never double-arms.
  **Family-supersede units (§3):** the exact reported race (/handover unmatched → /handover-here
  → write fires ONLY the newer path) in both directions, a SEALED predecessor untouched (both
  fire with their own files), a same-command re-run collapsing to one fire with the newest args,
  the defensive seal of a matched-but-unsealed predecessor (no cross-steal either way), plus E2E
  scenario H (the fabricated pivot transcript through the real parser + both bindings).
  **/handover-here units (§5a):** the hyphenated echo parses whole
  (`ParseCommandEcho` keeps the hyphen — bindings key on the exact name), a `/handover-here`
  sighting beside a registered `/handover` binding fires ONLY its own handler (name-exact
  lookup, no prefix aliasing), and `EnsureHandoverHereCommandFileIn` under the same temp-config
  discipline: its OWN `handover-here.md` created beside `handover.md`, carrying the load-bearing
  signal (Write tool + `HANDOVER-`), the content-injection + never-truncate briefing, AND the
  in-place sentinels ("REPLACES" / "RESTARTS THIS TAB"); `ShippedHandoverHereCommandHashes`
  sanity + its own `last == sha256(current text)` gate, and that the three commands' histories are
  DISJOINT (a shared digest would cross-upgrade the files); a user-edited file never overwritten
  (the shared `EnsureShippedCommandFileIn` core).
  **§5b STANDBY units:** the `handover-standby.md` definition (its own file + digest gate + the
  fill-not-send sentinels "PRE-TYPED VERBATIM"/"NOT submitted"/"presses Enter" + the `WRITE IT
  IN:` marker + no bare sibling token — a rename can never leave a stale `/handover` mention);
  the THREE-way binding isolation (a `/handover-standby` sighting fires only its own binding —
  note "handover" is a PREFIX of "handover-standby", the exact aliasing hazard) + the family
  supersede pivot (/handover → /handover-standby fires the standby path); `BuildPromptFill`
  (bracketed paste, CR/CRLF→LF, NO trailing CR ever — and `BuildPromptSubmission == fill +
  "\r"`, so the two channels provably share every byte up to the submit); the
  `StandbySessionTakenOver` hands-off latch (fresh-fillable, a turn in flight, and the
  fast-completed-turn push/pull evidence cases); the standby
  settings' round-trip (name/enable/marker/successor-model) + the triple heal. The §6c
  render↔identity inverse below runs over ALL THREE shipped texts, so a phrase drift that
  would freeze a command's custom-location files is caught per text.
  **§6a CUSTOMIZATION units:** `NormalizeCommandName` (slug rules, `/`-strip, path chars dropped,
  the 64-cap) + `ResolveCommandNameTriple` (defaults, every collision direction incl. the
  squatter-eviction cascade); the
  render↔identity INVERSE over a synthetic text with a boundary hazard (`/am-cmd` vs
  `/am-cmd-here`) AND the REAL texts (a custom render carries only the custom token, the
  await's load-bearing signals survive, a custom-named CURRENT render digests back onto the
  history's last entry, a default-name render is byte-identical); the named
  ensure/remove/reconcile policy over a synthetic 2-version history (create rendered, upgrade a
  custom-named prior version, never overwrite a user edit, remove pristine-any-version vs leave
  user-owned, and the full reconcile story: first materialize → rename-migrate → rename away
  from a user-EDITED file (left in place) → disable-delete → re-enable → enabled-but-blank
  fallback); plus the AppSettings round-trip incl. marker semantics (a PRESENT-empty marker ==
  disabled vs an absent key == the default), the load-time collision heal, and a hand-edited
  marker normalizing.
  **§6b SHAPING units:** `RegexUtil` end to end (valid/invalid/empty classification, the
  pattern + input caps, case-insensitivity, unanchored search semantics, replace-ALL, `$1`
  backrefs, and — the contract that matters — an invalid pattern reading as no-match /
  unchanged with `applied=false`); `DeriveHandoverSuccessorTitle`'s five fallback paths (unset ·
  invalid · no-match · blank result · the >255 cap) beside the real rewrites (plain, `$1`
  append, numeric bump, trimming); the CommandWatch **leaf-match regex** (a valid pattern is
  authoritative — `BRIEF-*` collected case-insensitively while a hint-named `HANDOVER-old.md`
  is EXCLUDED, and it SUPPRESSES the first-markdown fallback so an unrelated `notes.md` never
  becomes the briefing, while the shipped-hint case KEEPS that fallback — and an invalid pattern
  falls back to the shipped hint); and the shaping AppSettings
  round-trip (models, the find/replace pair + file pattern stored VERBATIM with regex chars and
  `$` backrefs unmangled, the toggle, absent keys == shipped behavior, and an invalid stored
  pattern degrading at USE time).
  **§6c WRITE-LOCATION units:** `NormalizeCommandWritePath` (trim, the trailing-separator drop
  that spares the degenerate roots, the line-breaking chars dropped, the length cap) +
  `CommandWritePathIsScratchpad` (blank == the token, case-insensitively); the rendered phrase
  per shape (default · `./` · relative · absolute); the render↔identity INVERSE over ALL THREE
  REAL texts — the marker is present, the shipped default renders BYTE-IDENTICALLY, a configured
  folder lands on the marker line while the `HANDOVER-<topic>.md` name contract is untouched, the
  phrase stays ONE line (line count unchanged), a location-rendered current text digests back
  onto the history's last entry, a marker-less (pre-§6c) text is returned verbatim so those
  versions still upgrade, and a RENAMED command with a CUSTOM location still digests onto the
  history (the two spans invert independently); the write policy through the location seam over a
  synthetic 2-version history (a changed location RE-RENDERS the same shipped version, re-running
  is a no-op, changing back re-renders back, a PRIOR version written under a custom location still
  upgrades, a user-edited file is STILL never overwritten, and `ForceReinstall…` is the one thing
  that overwrites it); `InspectShippedCommandFileNamedIn`'s four states; and the AppSettings
  round-trip incl. the presence gate (absent == the scratchpad default, a hand-edited value
  normalizing on load) and the delete-after pairing (absent == ON for a fresh install, a stored
  `false` kept).
* **`TestCommandHandoverE2E` — the FABRICATED session** (the design's expected transcript,
  fabricated with real ISO timestamps and replayed through the REAL `ParseTranscriptDelta` + a
  feed mapping kept in lockstep with `_readDelta`'s): scenario A the happy path — echo →
  assistant `Write(HANDOVER-*.md)` → tool_result → `end_turn`, asserted event shape
  (1 Command + 2 Assistant + 1 ToolResult, **0 UserPrompt**), fired once via the **DEFAULT
  GetFileAttributesExW probe against a real on-disk md** (the deployed code path), args riding
  the fire, and the fired path's content reading back VERBATIM through
  `ReadHandoverDocumentPrompt` (exactly what `_HandleCommandHandover` injects as the
  successor's first user message); plus a chunked scanner-style parse (awkward split points,
  partial-line re-reads) proving event-sequence equivalence with the whole-parse. Scenario B: a clarification round
  still fires (the 2-turn window). Scenario C: no md within 2 turns → expired, no fire, no
  leak. Scenario D: **two /handovers in one conversation → two fires**, each with its own
  md + args (the explicit repeatability requirement). Scenario E: the same session replayed
  with 2-hour-old timestamps (the restart/adopt history read) never arms — no ghost successor
  tabs on reopen. Scenario F: **one /handover writing TWO real HANDOVER files → ONE fire
  carrying both in write order** (default disk probe; each file reads back as ITS OWN
  successor's first message — the action fans out one successor per file). Scenario G: **the
  restart story over a durable store** — run 1 fires; the
  restarted instance replays the same still-fresh history and never re-fires (the watermark);
  and an armed-but-unresolved command from run 1 REVIVES in run 2's replay (echo now stale) and
  completes exactly once. Scenario H: **the family-race pivot** — /handover answered with a
  clarifying question, the user pivots to /handover-here, the write fires ONLY the in-place
  path (the stale /handover superseded, never fed the newer command's file).
* **`TestCommandEchoRealCorpus` — the REAL corpus** (guarded; `[info]`-skips on a machine
  without `~/.claude`): the newest ~120 on-disk transcripts (2 MB heads, whole-line-safe)
  line-scanned for genuine command echoes and Write-tool lines, each replayed through the real
  parser — asserting **every** echo parses to a `Command` event, **zero** echoes leak a turn
  event (the /model false-Running invariant, corpus-wide), timestamps parse (the freshness
  guard's input), and every real `Write` tool_use yields its `file_path`. On this machine's
  corpus at authoring time: 54 user-echo + 22 system-echo + 60 Write lines, all green.

The pre-existing `/model`-echo test in `tests_transcript.cpp` pins the §2 invariant from the
state-machine side (no TURN event; the echo reads back as the non-turn `Command` event).

## 9. What was deliberately NOT changed

* **State machine untouched** — a command echo remains a non-event (§2); the successor's
  `Running` comes from its own real `UserPromptSubmit`.
* **No injection into the ORIGIN session** — the origin is read via the transcript only
  (Rule #13-adjacent); the action spawns a NEW session. (/handover-here upholds the same rule:
  nothing is ever written to the origin's stdin — the swap CLOSES its connection, which is the
  Restart-session mechanism, and archives the record like a Close.)
* **Codex excluded** — rollouts carry no command echoes; `/handover` is Claude-only (the
  successor is a Claude session).
* **Observe-only externals excluded** — the scanner reconciles registry sessions only; an
  external claude's `/handover` never fires (we host no tab to anchor the successor to).

## 10. Follow-ups (non-blocking)

* A push fast-path (the `UserPromptSubmit` hook's prompt field) behind the same feed, dedup by
  sighting identity, for sub-second latency.
* More bindings on the same infra (the generic await shapes beyond markdown: "next turn-complete",
  "next file matching \<glob\>", `/command`-driven queue operations).
* ~~A cog off-switch (`commandBindingsEnabled`) if a user ever wants the echoes ignored.~~
  **SHIPPED, per-command** — §6a: the cog's Commands tab disables (and renames) each command
  individually, applied at the next start.
* Surfacing an armed await on the origin tab's overlay (a dim `⏳ handover pending` row).
* A **fan-out cap** — a definition-following Claude writing, say, 10 files opens 10 tabs. The
  §6b file-match regex narrows WHAT counts and delete-after cleans up, but nothing bounds the
  COUNT yet.
* Live-applying the §6b **file-match** pattern (re-binding mid-run) — deliberately restart-only
  today, because the scanner worker reads the binding set unsynchronized.
* First-prompt `/handover` in a brand-new session is a degenerate no-op by construction (there
  is nothing to hand over); the freshness gate also skips a `/handover` typed moments before an
  app restart — the user re-runs it. Documented, accepted.
