# Delivery hardening — implementer plan (R1–R3)

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
mail-queue time; the pending-input detector's ~120-row read window; the fuller
accepted/delivered log split.
