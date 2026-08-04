# Delivery hardening — implementer plan (R1–R3)

> **Status: PLAN — not yet implemented.** Companion to [`DELIVERY.md`](DELIVERY.md) (read §9–§10
> first: the incidents, the shipped fixes, and where these residuals were carved off). Everything
> here is engine-side and pure-testable unless marked otherwise; follow the house rules — pure
> decision functions with the `DecideAdvance` pattern, harness coverage in
> `AgentMaster/tests/`, `[tag]`-logged observability, Rule #18 on every catch, and amend
> `DELIVERY.md` + `CLAUDE.md` when done.

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
   - turn evidence exists PAST the send (`turns.lastPromptUnixMs > sentAt` or a real Stop landed
     after it): a turn ran and our text still never showed;
   - a settle margin elapsed (`kLostSendSettleMs`, suggest 15 000 — must exceed the Enter-retry
     give-up horizon ~21 s? No: independent watchers; pick ≥ the echo window 15 s so a slow push
     echo can never be beaten to the verdict);
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

**Decision point (flagged, needs a corpus check before committing):** the scanner's pull-consume
cadence is ~2.5 s and transcript-write-lagged; on a no-hook ADOPTED session a genuinely-started
turn may not mark `echoed` for a few seconds. Both consumers already tolerate that (the guard
holds a little longer — correct direction; the watchdog's first press is at 3 s and a press into
a running turn's box is an empty-box no-op). Verify against a no-hook session live before
shipping; if the watchdog presses prove noisy, gate its Retry (not its Waiting) on one extra
poll.

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

**Step 2 — the likely hardenings (pick per evidence):**
- an **empty-prompt** UPS never bumps `turns.lastPromptUnixMs` (a promptless submit is not a new
  turn's start — but FIRST verify the dialog-answer UPS is the empty one, because that submit
  DOES resume API work and may deserve the stamp);
- a UPS whose folded text equals an already-`echoed` recorded message within the echo window is
  a DUPLICATE: apply the state effect (Running is harmless and self-heals) but skip the stamp,
  so it can never floor-suppress the real Stop that follows.

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
an extensive commit.

Adjacent, still-open items (from DELIVERY.md §8 — separate scope, listed so the implementer sees
the whole board): a per-prompt question-guard opt-out control in the Manager queue UI + a
board-card hold indicator; **Failed-row visibility** on the overlay's ⏳ line (natural rider on
R1 — a lost/failed prompt should be as visible as a held one); paste-placeholder expansion at
mail-queue time; the pending-input detector's ~120-row read window; the fuller
accepted/delivered log split.
