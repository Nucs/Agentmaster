# Delivery hardening — implementer plan

> **Part 1 (R1–R3): IMPLEMENTED. Part 2 (R4–R8, verified placement): PLANNED** — motivated by
> Incident 3 (DELIVERY.md §11), the invisible-content merged send.

# Part 1 — R1–R3 (implemented)

> **Status: IMPLEMENTED — with three evidence-driven deviations** (each flagged `⚠ DEVIATION`
> inline; the outcomes live in [`DELIVERY.md`](DELIVERY.md) §10). The pre-implementation log
> re-analysis materially corrected the R3 forensics (the phantom UPS events are EMPTY late
> twins, not text-duplicates — no `Typed` rows exist for them; the AskUserQuestion dialog answer
> rides `PostToolUse`, never a UPS) and proved R2-as-written unsafe on the very incident log (the
> re-armed watchdog would have submitted the user's draft at 08:50:07 — the draft guard below is
> mandatory, not hardening). Engine-tested: `TestLostSendReconciler` + the rewritten
> guard/watchdog/ordered suites (harness 3130/3130); `TerminalAppLib` compiles green.

**Target invariant (extends DELIVERY.md §5/§9):** a `Sent` prompt is either *provably a message*
(echo — push or pull), *provably dead* (`Failed`, surfaced), or *still being watched*. No prompt
may sit `Sent` forever without having become a message; no watcher may accept another turn's
activity as proof of our prompt's pickup.

---

## R1 — the lost-send reconciler (the `b5f766fc` `#6` gap)

**Problem.** A delivered prompt that never became a message is terminally invisible. Live case
(DELIVERY.md §9): *"then for another one but spawn subagent…"* was pasted into a running turn,
consumed by an AskUserQuestion dialog's rendering, never appeared in the transcript — and its
queue row reads `Sent`, identical to a success. The §9 floor removes the *recorded* cause (the
bogus mid-turn advance), but a user racing the autorunner with a manual submit reproduces the
shape at any time.

**Design — two halves, both in the scanner lane (it owns transcript truth and already parses
user messages per pass):**

1. **The PULL echo-consume.** When the scanner's per-pass transcript delta yields a USER message
   whose newline-folded text equals a `Sent`+`!echoed` Autorun prompt's folded text (the
   `FoldCrToLf` rule, `SessionRegistry.cpp`) with `timestamp ≥ sentAtUnixMs`, mark that prompt
   `echoed` (quietly — `UpdateQuiet`; this is bookkeeping, not a state change). This unifies
   delivery evidence across hooked AND no-hook sessions: `echoed` becomes THE "it became a
   message" fact, fed by push (the hook echo) or pull (this). It also makes R2 possible.
   - Note the existing near-miss: `NoteExternalPrompt`'s dedupe already *finds* these matches
     (it declines to double-record) but never marks `echoed` — the consume belongs right there
     or beside it in the scanner's back-fill path.
2. **The lost verdict.** Pure `DecideLostSend(const SessionInfo& s, int64_t nowMs)` → the set of
   `Sent`+`!echoed`+`sentAtUnixMs != 0` Autorun prompts that are LOST, defined as ALL of:
   - the session is AT REST (`Idle`/`WaitingForInput`) — never judge mid-turn;
   - ~~turn evidence exists PAST the send (`turns.lastPromptUnixMs > sentAt` or a real Stop landed
     after it): a turn ran and our text still never showed;~~ **⚠ DEVIATION — conjunct DROPPED
     as implemented:** a watchdog press REFRESHES `sentAtUnixMs`, making this permanently false
     after one press — the draft-blocked corner would strand `Sent` forever, the very gap R1
     closes; it also never guarded the false-positive it appeared to (a fold-miss echo produces
     turn evidence too — the echo conjunct is the real protection), and eaten-vs-lost
     disambiguation is already serialized by that same press-refresh (the verdict structurally
     fires only after the ladder went quiet);
   - a settle margin elapsed (`kLostSendSettleMs` = 15 000 — must exceed the Enter-retry
     give-up horizon ~21 s? No: independent watchers; pick ≥ the echo window 15 s so a slow push
     echo can never be beaten to the verdict — and the press-refresh above means an ACTIVE ladder
     always postpones it anyway);
   - the delivery gate for it is closed (never race an in-flight swap).

**Recovery (decision made — mirror the Enter-retry give-up, `Scheduler.cpp`):** mark the prompt
`Failed`, log `[lost-send] <sid8> prompt <pid8> "<label>" (delivered but never became a message)`
to `autorunner.log` + `hooks.log`, and **pause the session's autorunner** (`mode → Off`) — the
queue past a vanished step is suspect, and silently continuing is how `#6`'s loss went unnoticed
until a human diffed the transcript. Do **NOT** auto-resend: the text may sit in the TUI's
type-ahead or box; a resend can double it. The user re-arms / Send-nows (both already release
paths).

**Files.** `SessionScanner.cpp` (the per-pass feed + the consume + the verdict application),
`Scheduler.h` or `SessionModels.h` (the pure `DecideLostSend` + constants — put it where
`DecideEnterRetry` lives for symmetry), `SessionRegistry.h/.cpp` only if the consume needs a new
quiet accessor.

**Tests.** Pure verdict cases (each conjunct's negation holds the verdict); the pull-consume
(fold-matched user message marks `echoed`, non-matching does not, a pre-send timestamp does
not); the `#6` replay shape end-to-end through the scanner feed (fabricated transcript: deliver →
no user message → next turn runs → at-rest ⇒ `Failed` + paused); a slow-push-echo race (echo at
14 s ⇒ NOT lost).

**Acceptance.** The `#6` scenario replayed against the fixed build ends with the prompt `Failed`,
the autorunner paused, and a `[lost-send]` line — within ~one scanner tick of the session coming
to rest.

---

## R2 — sound pickup evidence (stop trusting another turn's writes)

**Problem.** Both `DecideEnterRetry`'s "picked up, stop watching" filter and the §9 pickup
guard's release accept `convLastActivityUnixMs > sentAt + margin` as proof our prompt started its
turn. A still-running PREVIOUS turn also advances the transcript — which is exactly why the
watchdog drained instead of rescuing `#6`, and why the guard would release in the same shape.

**Design.** Once R1's pull echo-consume exists, `echoed` is delivery-evidence for hooked and
no-hook sessions alike. Re-key both consumers:

- `DecideEnterRetry`: drop the raw transcript-advance clause; "the turn started" =
  `p.echoed` (push or pull) OR the state left the ready set. Keep the give-up ladder unchanged.
- The §9 pickup guard (`DecideAdvance`): release on `p.echoed` OR
  `turns.lastPromptUnixMs > sentAt` (a submit stamp is still direct evidence); drop the
  transcript-advance clause. `kPickupGuardMaxMs` stays as the lost-evidence belt.

**⚠ DEVIATION — the DRAFT GUARD is a mandatory third leg, proven on the incident log itself.**
Dropping the drain clause RE-ARMS the watchdog in the lost shape (the prompt stays a watch
candidate once the session returns to rest) — and at 08:50:07 the live log shows exactly that
moment with the box holding the user's 17-char draft (*"maybe another one"*): the plan-as-written
watchdog would have pressed a lone Enter there and SUBMITTED it (the RC3 merge). As implemented,
`DecideEnterRetry` never presses while `s.pendingInput` (the pending-input monitor's box read) is
non-empty and ≠ the watched prompt's text (`DraftMatchesPromptText` — the shared `FoldCrToLf`,
promoted to SessionModels.h, + trailing-trim): a box holding OUR prompt still presses (that IS
the eaten-CR rescue), a foreign draft answers `Waiting` (never press; the R1 verdict owns the
terminal resolution — its settle clock, keyed on the un-refreshed `sentAtUnixMs`, keeps running
while presses are refused). An empty observed box presses (a lone Enter into an empty claude box
is a no-op; the pending scan's eager-show bounds the race to ~one tick).

**Decision point (flagged, needs a corpus check before committing):** the scanner's pull-consume
cadence is ~2.5 s and transcript-write-lagged; on a no-hook ADOPTED session a genuinely-started
turn may not mark `echoed` for a few seconds. Both consumers already tolerate that (the guard
holds a little longer — correct direction; the watchdog's first press is at 3 s and a press into
a running turn's box is an empty-box no-op — and the transcript-derived state leaves the ready
set within a tick, ending the watch). Live no-hook verification rides the next deploy cycle with
the rest; the draft guard bounds the worst case to a no-op press either way.

**Files.** `Scheduler.h` (both pure functions + comments), tests in `tests_spawn_sched.cpp`
(replace the transcript-advance release/drain cases with echoed-keyed ones; add the
"previous turn still streaming ⇒ NOT released" case that `#6` exercised).

**Acceptance.** A fabricated "delivery while another turn streams" state neither releases the
guard nor drains the watchdog; a pull-echoed prompt releases both.

---

## R3 — phantom / duplicate `UserPromptSubmit` hardening

**Problem.** The log shows UPS events matching no delivery and no transcript message
(08:42:28.904, 08:44:53.902 — DELIVERY.md §10). Each stamps `turns.lastPromptUnixMs` late, which
can floor-suppress a REAL Stop (benign — the quiescent synth heals in ~2.5 s — but it adds
latency and re-lights Running on a settled session until the scanner settles it).

**Step 1 — instrument before deciding (do not guess claude's semantics):** add `chars=N` (and a
short fold-hash) of the prompt to the `[UserPromptSubmit]` hook log line, and capture one live
repro (likely candidates: the AskUserQuestion answer-submit, the stash-pop auto-submit, or a
duplicate forwarder like the §9 Stops). The wire already carries the prompt — this is log-only.

**⚠ DEVIATION — Step 1's questions were answerable from the EXISTING record, and the answers
redirected Step 2.** The persisted queue holds NO `Typed` rows at any phantom instant (a
non-empty non-echo UPS always records one; noise shapes are real turns) and the transcript holds
no user message there ⇒ the phantoms carried **EMPTY prompt text** — late duplicate deliveries
(~0.5–2.6 s after each real hook, the same twin pattern as the §9 duplicate Stops, degraded to a
promptless payload; a third instance found at 08:50:14.515). And the dialog-answer question is
moot: the AskUserQuestion answer fires **`PostToolUse`** (proven live at 08:48:47), never a UPS.
(The earlier "duplicate asdasd entries" reading was itself wrong — those are two separate
legitimate sends, 08:11 and 08:42.)

**Step 2 — as implemented (per that evidence):**
- an **empty-prompt** UPS is STATE-ONLY in the ordered machine: it never bumps
  `turns.lastPromptUnixMs` **and never counts type-ahead** (`++queuedPrompts` — a phantom count
  made the next REAL Stop read as a batch consume and strand Running for a quiescent-heal cycle);
  the → Running state effect stays (self-healing if phantom). This also covers the scanner's
  recon-run synth (deliberately empty), whose reconciliation-timed stamp was itself a
  floor-suppression hazard.
- ~~the already-`echoed` text-DUPLICATE dedupe~~ **⚠ DEVIATION — REJECTED:** no evidence
  motivates it (the phantoms are empty, so the text path never sees them), and it carries a real
  false-positive: a human repeat-typing the same short prompt ("y") as mid-turn type-ahead within
  the window would lose its type-ahead count — making the current turn's Stop read turn-complete
  and fire the advance into the repeat's turn, the exact mid-turn stacking this whole effort
  kills. Do not add it without a recorded non-empty duplicate.
- the `[ups]` disposition line ships regardless: one line per UserPromptSubmit —
  `chars=N hash=<fnv1a of the folded text> -> echo consumed | recorded as Typed | noise | EMPTY`
  — so any future phantom shape is self-evident from hooks.log (two deliveries of one message
  share a hash; the empty twins read `chars=0`).

**Files.** `HooksBridge.cpp`/`Engine.cpp` (wherever the `[UserPromptSubmit]` trace prints),
`SessionRegistry.cpp` (`OnHookEvent` — the stamp gate), `HookEvents.h` only if the stamp rule
moves into the ordered machine (preferred: keep the ordered machine pure and gate in
`OnHookEvent` where the queue is visible).

**Tests.** Ordered/registry cases: duplicate UPS (same folded text, already echoed) does not move
`lastPromptUnixMs` and the following real Stop still clears the floor; an empty-prompt UPS per
the Step-1 verdict.

**Acceptance.** Replaying the 08:42 window (UPS echo → dup Stop → dup UPS → real Stop) through
the registry yields exactly one turn: the dup Stop stale (floor), the dup UPS stamp-inert, the
real Stop a clean turn-complete with its true question bit.

---

## Sequencing + adjacent backlog

Order: **R1 → R2** (R2 consumes R1's pull echo), **R3** independent (Step 1 can start
immediately). Each lands with: harness green (`run-m5-tests.bat`), `TerminalAppLib`
compile-check, `DELIVERY.md` §10 updated with the outcome, the `CLAUDE.md` M7 paragraph amended,
an extensive commit. **(As landed: the three interlock — R2's guard consumes R1's `echoed`, R3's
stamp gate is what makes R2's remaining prompt-stamp release sound — so they shipped as ONE
change set, validated together: harness 3130/3130 + lib green; deploy verification rides the
next cycle.)**

Adjacent, still-open items (from DELIVERY.md §8 — separate scope, listed so the implementer sees
the whole board): a per-prompt question-guard opt-out control in the Manager queue UI + a
board-card hold indicator; **Failed-row visibility** on the overlay's ⏳ line (natural rider on
R1 — a lost/failed prompt should be as visible as a held one); paste-placeholder expansion at
mail-queue time; the pending-input detector's ~120-row read window (**→ now R5**, Part 2); the
fuller accepted/delivered log split.

---

# Part 2 — VERIFIED PLACEMENT (R4–R8): close the loop between "set" and "submit"

> **Status: PLANNED — not implemented.** Motivated by **Incident 3** (DELIVERY.md §11, session
> `1bf4bd83`, 2026-08-12 10:28): a 13-char prompt delivered through the no-draft fast path merged
> with ~4.2K of TUI-internal content the reads never saw — `[ups] chars=4205 -> recorded as
> Typed`, then `[lost-send] … Failed` 18 s after claude had acted on the mangled message. The
> R1–R3 machinery detected the loss *after the fact*, exactly as designed — the gap is that the
> placement itself has no read-back.
>
> **The organizing principle (the user's lesson, adopted as doctrine):** every automated WRITE
> into claude's input box — a fill, a submission, a lone Enter, a clear rung — must be followed
> by a READ-BACK against expectation through the fullest read we have, before any irreversible
> commit; and a read that cannot SEE the box must refuse rather than default to "empty ⇒ safe".
> The clear/restore/discard cycles already live by this; the send and the press do not.

**Target invariant (extends DELIVERY.md §5/§9/§11):** no submit CR is committed into a box that
has not been read back as exactly-the-prompt (or provably collapse-equal); an unreadable box
refuses placement; a failed TUI-channel round-trip never ends with content parked where no read
can find it; a merged submit is *named* (`[merge-detected]`), not just "lost".

## Phase 0 — probes before code (the R3 discipline: instrument before deciding)

All four run in the existing harnesses (`tests/pending_probe.cpp` + `_run-pending-probe.bat`
AttachConsole oracle; the HOOKS.md pywinpty PTY recipe in a scratchpad dir) — no product code.

- **P1 — tall-draft render shape.** Type/paste a draft that renders >120 rows: does the Ink TUI
  render the WHOLE box (the `❯` line scrolls past any bounded window) or viewport it internally
  (the `❯` stays within ~a viewport of the bottom)? Decides R5's window fix: a raised on-demand
  read window vs a tail-anchored partial verify.
- **P2 — injected-paste collapse threshold.** At what size does a bracketed-paste FILL collapse
  to `[Pasted text #N +M lines]`, and what does a large NON-collapsed fill render like? Decides
  R4's verify predicate for the handover paste tier (those injections are precisely the huge
  ones) — and whether "unreadable after fill" is even reachable for collapse-eligible sizes.
- **P3 — kill-ring placeholder fidelity.** Does Ctrl+U → Ctrl+Y round-trip a
  `[Pasted text #N]` placeholder's paste-cache binding intact (as the stash does)? Decides
  R6(c) — whether the ring can replace the auto-popping stash as the default clear rung.
- **P4 — the stash auto-pop trigger, precisely.** Submit+~0.4 s vs turn-end; can a pop land
  inside a ≤60 ms read→write window; what does the pop render as mid-frame? Pins Incident 3's
  exact variant and sizes R4's residual window honestly.

## R4 — verified placement: the send becomes FILL → READ-BACK → COMMIT

**Problem.** `BuildPromptSubmission` = `BuildPromptFill + "\r"` in ONE write (RC7). Both the
swap's post-clear send and the two fast paths (`draft.empty()` — the Incident-3 path — and
`!preserveDraftOnSend`) commit blind. The clear's "confirmed empty" is a *pre*condition read; by
commit time it is 60 ms – 2.4 s stale, and claude's own channels (RC9) can repopulate the box in
between.

**Design.**

- **New pure verdict** (PendingInput.h, beside `DecideDraftClear`):
  `VerifyFillAgainstPrompt(boxRead, promptText) -> Verified | VerifiedCollapsed | Eaten |
  Foreign` over fold+trim text (`FoldCrToLf` + the trailing-trim — the `DraftMatchesPromptText`
  normalization). `VerifiedCollapsed` accepts a box whose `[Pasted text #N +M lines]` markers
  make it collapse-consistent with the prompt (literal lines + Σ(M) line-arithmetic, the §8c
  resolver's counting — pure, no disk; claude re-collapses a big pasted fill, so a naive
  equality would refuse every handover paste-tier delivery). `Eaten` = verified-empty box
  (R5's verdict, not a bare `""`). `Foreign` = anything else — including "our prompt PLUS more",
  the merge shape.
- **The send sequence** (the swap Impl's step 5, and BOTH fast paths whenever a readable control
  exists): LOCK read-only (now also on the empty-box path — user keystrokes excluded for the
  ~2–6 polls; the TUI's own inserts are what the verify catches) → `Inject(BuildPromptFill)` →
  settle (the clear ladder's change-detecting read loop; budget ~1.5 s, **progress-extends**
  while the box is still growing toward the target) → verify:
  - `Verified` / `VerifiedCollapsed` ⇒ `Inject(L"\r")` — **the commit** — then the unchanged §6
    submit-await / echo / watchdog machinery.
  - `Eaten` ⇒ re-fill (≤2 attempts, the standby lane's constant) ⇒ then rollback to `Pending` +
    `[send-verify] giveup` — never a blind CR.
  - `Foreign` ⇒ **the merge, caught pre-commit.** Recovery: backspace-UNDO our insertion in
    verified batches (`BuildBackspaces(64)` → re-read → repeat while the box still ENDS WITH our
    prompt's folded tail; a collapsed fill deletes atomically in one press — the measured
    PendingInput.h fact; never a blind count), verify the box no longer carries our text,
    rollback to `Pending`, **record what remains as `SetPendingInput`** (the "3 dots" + the draft
    memory now honestly show the content the reads had missed), log `[send-verify] FOREIGN`.
    The foreign text is the user's until proven otherwise — it is never destroyed (Rule #16's
    spirit; the ABORT philosophy: a late prompt is recoverable, a mangled message is not).
  - Unreadable post-fill (R5's `NoBox` after a fill) ⇒ P1/P2-gated policy: collapse-eligible
    sizes re-read at the raised window expecting the collapsed marker; a truly unverifiable fill
    fails CLOSED — backspace-undo best-effort, rollback, loud log.
- **Unchanged contracts:** the no-control fallback (dormant / hosted in another window / tests /
  the registry's no-submitter inline path) keeps the atomic `BuildPromptSubmission` — there is
  no buffer to read, and pretending otherwise would just add latency. Logged distinctly
  (`[delivered] … (unverified: no readable buffer)`).
- **Settings composition.** New `AppSettings::verifySendBeforeSubmit` (default ON; cog → TESTS
  AUTORUNNER beside `preserveDraftOnSend`). The two switches are orthogonal:
  `preserveDraftOnSend` governs whether an existing draft is RESCUED (clear + restore);
  `verifySendBeforeSubmit` governs whether the commit may be blind. Preservation OFF + verify ON
  + a Foreign read ⇒ still refuse (a merge is never OK); the verify switch itself is the escape
  hatch — see the fail-direction analysis under R5.
- **Constants.** The added worst case ≈ 2×(fill + settle) ≈ 3–4 s on top of the swap's existing
  ~21 s worst hold — still under `kDeliveryGateTimeoutMs` (45 s) with ≥1.6× margin; keep 45 s,
  re-assert the margin in the constant's comment. A normal empty-box send costs one settle
  (~60–300 ms) extra.
- **⚠ The residual, stated honestly:** verify→CR remains ONE ~60 ms poll of blind window — an
  async TUI insert can still land inside it (no atomic read+write exists on a PTY). The verify
  shrinks the exposure from "unbounded trust in a 60 ms–2.4 s-stale precondition" to one poll;
  R6 removes the dominant insert *source*; R7 names the aftermath. Defense in depth — no single
  silver bullet, and the doc must not claim one.

**Files.** `PendingInput.h` (the verdict + tests beside the ladder), `SessionModels.h` (the
`AppSettings` field), `Persistence.cpp` (round-trip), `TerminalPage.AgentObserver.cpp` (the swap
Impl's step 5 + both fast paths + the batched undo helper), `AgentManagerContent.Settings.cpp`
(the cog row), `SessionRegistry.cpp` (the `[delivered]`-line unverified annotation only).

**Tests.** Pure verdict table (exact / CR-composed / collapsed-marker arithmetic / partial /
foreign-prefix / foreign-suffix / empty); the Incident-3 replay (fabricated: verified-empty
read → foreign content lands → fill → `Foreign` → batched undo → rollback; assert NO CR ever
injected, prompt back to `Pending`, `pendingInput` records the foreign text); the handover
paste tier E2E with a collapse fixture (P2's measured shape); `Eaten` → re-fill → `Verified` →
CR; give-up after 2.

**Acceptance.** Replaying the 10:28 shape ends with no CR fired, the prompt `Pending` (or
`Failed` after attempts), the foreign text visible as pending input, and a `[send-verify]`
trail; a plain empty-box send is ≤ ~360 ms slower; the handover paste tier still delivers
(via `VerifiedCollapsed`).

## R5 — the tri-state box read: plumb `boxFound` (+ the menu verdict) through the boundary

**Problem (RC8).** `PendingInputDraft.boxFound` exists in the pure detector and dies at
`ControlCore::ReadPendingInputDraft`, which returns only text. Every consumer therefore treats
"cannot see a box" as "empty box": the swap's READ step fast-paths into `plainSend`, the pumps'
`draftKnown && draft.empty()` reads as verified-empty, and the watchdog's guard admits a press.
The §9 damage (49 chars consumed by an AskUserQuestion dialog) and Incident 3 are both
"" -misread shapes. The detector even *recognizes* the menu shape (`IsMenuOptionCaret`) — and
then throws that knowledge away by answering plain-empty.

**Design.**

- **Detector verdict** (pure): `InputBoxState { NoBox, Empty, Draft, MenuOpen }` surfaced from
  the existing detection walk (`boxFound` false ⇒ `NoBox`; the menu-shape rejector that today
  silently skips a candidate additionally reports `MenuOpen` when the bottom-most caret row IS a
  menu option and no true box exists below it). `DetectPendingInput`'s signature keeps the text;
  the struct gains `state`.
- **Boundary:** the ControlCore scan cache (`_pendingInputScanResult` + the mutation-id gate)
  additionally stores the verdict; expose `ReadPendingInputBoxState()` (int32 over IDL) beside
  `ReadPendingInputDraft()` — one shared cached scan, two views. `TermControl` passthrough;
  `_ReadLiveDraftForSession` grows a state-returning sibling.
- **Consumers:**
  - the swap's READ pre-flight: `NoBox`/`MenuOpen` (persisting across the existing one re-read)
    ⇒ **DECLINE the delivery** — rollback to `Pending`, `[send-verify] box not visible
    (state=menu|none)`, the advance re-fires later. This kills both incident shapes at the
    cheapest point: a paste while a menu is up feeds the MENU (§9's silent 49-char loss), and an
    invisible box is exactly where Incident 3 lived.
  - R4's verify reads distinguish `Eaten` (verified `Empty`) from unreadable (`NoBox`).
  - the standby + §10 pumps' `draftKnown` becomes verdict-aware (`Empty` == safe-to-fill;
    `NoBox`/`MenuOpen` == wait, deadline-capped as today).
  - the scan records a transient `SessionInfo::pendingBoxState` (never persisted — a FACT like
    `pendingInput`) so the pure deciders can consult it: `DecideAdvance` holds with reason
    `"input box not visible (menu open?)"` instead of marking `Sent` into a menu;
    `DecideEnterRetry` refuses (see R8).
- **The read window:** per P1 — either `ReadPendingInputDraft` gains an on-demand `rows`
  parameter (the verification reads pass a raised bound, e.g. 1000; the 2.5 s scan keeps 120 —
  the cost is per-CALL, and verification calls are human-cadence), or, if the TUI viewports
  internally, the tail-anchored partial verify (match the visible tail against the prompt's
  tail, accept-with-log). The window gap graduates out of §8's "known, separate" list either
  way.
- **⚠ Fail-direction analysis (the deliberate part).** Today a detector break (a TUI render
  drift) fails OPEN — no protection, sends flow blind. With R4+R5 gating it fails CLOSED — sends
  refuse, loudly. That is the correct default for a system whose failure mode is *submitting
  text nobody wrote*, but it converts render drift into a delivery outage, so it must surface
  fast and have an exit: M consecutive `NoBox` refusals for one prompt ⇒ mark it `Failed` +
  pause the session's autorunner (the lost-send idiom — a paused plan + a log line, never a
  silent stall), and `verifySendBeforeSubmit` OFF restores the historical blind send while a
  detector fix ships. The pending-probe suite is the drift canary (it already sweeps live
  versions 2.1.211–218).

**Files.** `PendingInput.h` (+ tests), `ControlCore.{h,cpp,idl}`, `TermControl.{h,cpp,idl}`,
`TerminalPage.AgentObserver.cpp` (pre-flight + pumps + scan recording), `SessionModels.h`
(`pendingBoxState`), `Scheduler.h` (the two deciders' new holds + tests).

**Tests.** Detector verdict table over synthetic row sets (empty box / draft / menu with
preview pane / no box at all / bottom-rule-only tall-box shape); pre-flight decline replay;
pump verdict-aware waits; `DecideAdvance` hold + release.

## R6 — TUI-channel hygiene: a failed round-trip must not leave a loaded landmine

**Problem (RC9).** Incident 3's fuel: three failed stash-restores left ~2.4K in channels we
cannot read, and the auto-pop re-planted it at TUI-chosen moments (the box refilled between
swaps — 2241 → 2455 → 2466 — and finally into the 10:28 send). The swap's justification for
keeping the Ctrl+S rung ("its restore always CONSUMES the slot") is disproven by its own
failure paths.

**Design.**

- **(a) Progress-extending restore settle.** `kDraftSwapRestoreBudgetMs` (1.2 s) is a fixed
  wall; a 2.4K pop's repaint can outrun it, turning a *slow success* into a recorded *failure*
  (and an unknown slot). The settle loop extends while the box is still CHANGING toward the
  wanted text (the clear ladder's change-detection idiom), capped at ~4 s.
- **(b) Never end a swap with the slot believed-loaded.** On a failed stash-restore, one final
  recovery rung: press Ctrl+S once on a VERIFIED-EMPTY box (the deterministic un-stash; a no-op
  when the slot is empty), read what pops, LEAVE IT IN THE BOX and `SetPendingInput` it — the
  content becomes visible, scan-tracked, and the next swap's clear handles it, instead of a
  landmine detonating into a later send. If nothing pops, nothing changed.
- **(c) Rung order, P3/P4-gated.** If the kill-ring round-trips placeholders (P3), flip the
  default clear rung to Ctrl+U (`draftSwapUseCtrlS` default OFF): the ring never auto-pops —
  the stash's whole-box convenience is not worth an auto-fire channel. The Ctrl+S path stays
  available (the setting), and the mail-clear/discard ladder is already stash-free.
- **(d) The slot ledger in the log.** Every stash/kill press already logs; add the swap-end
  line's channel disposition (`slot=consumed|left-loaded|popped-back`) so a future incident's
  ledger is readable straight off hooks.log — Incident 3's could not be reconstructed with
  certainty.

**Files.** `TerminalPage.AgentObserver.cpp` (`_RestoreDraftAfterSwap` + swap-end logging),
`SessionModels.h` (default flip, if P3 confirms), `PendingInput.h` comment updates.

**Tests.** Restore-settle extension (growing box ⇒ budget extends; static wrong box ⇒ fails at
cap); the pop-back rung (loaded slot ⇒ content lands in box + recorded; empty slot ⇒ no-op).

## R7 — the merge classifier: name a mangled message at the echo seam

**Problem.** Incident 3's merge surfaced as `[lost-send]` 18 s later; the operator learned the
truth only by diffing the transcript. The evidence was in-process the whole time: a non-echo
UPS whose text CONTAINS the in-flight `Sent`+unechoed prompt.

**Design.** In `OnHookEvent`'s UPS handling (and `NoteExternalPrompt`, the pull twin): when a
prompt-carrying UPS fails the fold-match but its folded text **ends with** the watched prompt's
folded text (the physical merge shape — the paste appended at a cursor sitting after the
foreign content) and is strictly longer, with a min-length floor on the prompt (≥8 folded chars
— never let a bare "y" classify), verdict **MERGED**: log `[merge-detected] <sid8> prompt
<pid8> swallowed into a <N>-char message`, mark the prompt `Failed` immediately (the transcript
row IS the proof — no reason to wait out `DecideLostSend`'s 15 s settle), pause the session's
autorunner (the stop-on-error idiom), and record the `Typed` row annotated as the merged
message. The `[ups]` disposition line gains the `merged` verdict.

**Files.** `SessionRegistry.cpp` (+ the pure suffix-match helper beside `FoldCrToLf`),
`Scheduler`-side none (the pause rides the existing seam). **Tests:** suffix hit / prefix
non-hit / short-prompt floor / exact-echo unaffected / the Incident-3 replay asserting
`[merge-detected]` fires on the 4205-char UPS.

## R8 — the watchdog press goes live-read (and menu-refusing)

**Problem.** The lone-Enter press trusts the scan-stale `s.pendingInput` (≤ ~2.5 s + debounce) —
the R2 draft guard's acknowledged residual race. Worse, found in this review: with an
**AskUserQuestion menu up** and the state still `WaitingForInput`/`Idle` (recon-block can lag —
a pending question's `tool_use` line was measured still unwritten 10+ minutes in, HOOKS.md), an
empty `pendingInput` ADMITS the press — and a lone Enter into a menu **selects the highlighted
option**: a real answer claude acts on, fabricated by the rescue mechanism. The delivery gate
and the dormant guard do not cover this.

**Design.** Route the press through a window-registered **verified presser** (the
`SetPromptSubmitter` idiom — same registration lifetime, same detached teardown): at press time,
live-read the box STATE (R5): `Draft` fold-matching the watched prompt ⇒ press (the eaten-CR
rescue, unchanged); verified `Empty` ⇒ press (a no-op for claude, unchanged semantics);
`Foreign` ⇒ answer `Waiting` + `SetPendingInput` the live text (the guard now sees truth, not a
tick-old echo); **`NoBox`/`MenuOpen` ⇒ never press** — the menu hole closed. The no-presser
fallback (tests/CLI) keeps today's `Inject("\r")` + the existing scan-stale guard, so the harness
contract is unchanged.

**Files.** `SessionRegistry.{h,cpp}` (the presser registration beside the submitter),
`Scheduler.cpp` (`_sweepPendingPickups` resolves through it), `TerminalPage.AgentObserver.cpp`
(register + implement), `Scheduler.h` tests (menu-refusal, foreign-refresh, empty-press).

## Sequencing + interlocks

**Phase 0 → R5 → R4 → R6/R7/R8** (any order after R4; R7 is independent enough to ship first if
a quick win is wanted). R4 without R5 is unsound — its verify cannot distinguish `Eaten` from
"cannot see the box", which is exactly how a re-fill would DOUBLE a paste into an invisible-box
session. R5 without R4 already pays: the pre-flight refusals alone kill both recorded damage
shapes. Each lands with the R1–R3 protocol: harness green (`run-m5-tests.bat`), `TerminalAppLib`
compile-check, DELIVERY.md §11 outcome recorded, the CLAUDE.md paragraph amended, an extensive
commit.

**Explicitly out of scope here:** paste-placeholder expansion at mail-queue time (§8, separate);
the compose-box/read-only UX of a longer verified send (nothing new is visible — the lock now
also spans the ~300 ms verified fast path); CLI P3's `send-now` (it will inherit the seam).
