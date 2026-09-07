# Atomic prompt delivery — the DELIVERY GATE

> **Status:** implemented (engine + window layer) — see §4 for the change list; engine-tested in
> `AgentMaster/tests/`; rides the next deploy cycle.
> **Companions:** `PENDING_INPUT.md` §9 (the draft swap — the delivery mechanism this serializes),
> `DESIGN.md` §10 / `HOOKS.md` (the Tests Autorunner the gate paces), `CLAUDE.md` *Tests Autorunner (M7)*.

## 1. The problem — four actors, no shared fact

Placing "the next prompt" involves four independent actors: the **scheduler's advance**
(`DecideAdvance` → mark `Sent` → `SubmitPrompt`), the **draft swap** (the hosting window's
read→lock→clear→send→await→restore pipeline that protects the user's unsent draft), the
**Enter-retry watchdog** (re-presses a lone `\r` when a submit's CR was eaten), and the
**pending-input scan** (records the box's unsent draft). The only atomic step among them was the
registry's mark-`Sent`; everything after it was *guessed* from side-channels — echo timing, box
reads, state transitions — that each actor interpreted alone.

The recorded incident (dev `hooks.log` + `autorunner.log`, session `1bf4bd83`):

```
07:26:59.055  [send] #5                      ← scheduler marks Sent, hands to SubmitPrompt
07:26:59.301  [draft-swap] holding an unsent draft (chars=12) — input locked, clearing via Ctrl+S
07:27:01.362  [UserPromptSubmit]             ← the send WORKED — claude started the turn
07:27:07.643  [Stop]                         ← the turn COMPLETED normally
07:27:08.153  [send] #6                      ← Stop-seam advance fires the next prompt…
07:27:08.445  [draft-swap] declined: a swap is already in flight (prompt stays Pending)
   …[send] #6 / declined / rollback ×8, every ~750 ms…
07:27:14.216  [nav] autorunner -> Off        ← the USER stopped it by hand
07:27:23.033  [draft-swap] restore DEFERRED: the prompt has not left the input box
```

Prompt #5 demonstrably submitted (echo at +2 s, clean Stop at +8 s), yet the swap's submit-await
read the box as non-empty for its full 18 s budget and held the session's box latch for ~24 s.
The moment the turn completed, the advance was genuinely "ready" (state Waiting; #5's echo already
consumed, so the pickup guard held nothing) — it marked #6 `Sent`, the swap declined it (latch
held), the rollback's own notify re-triggered the advance, and the loop ran at ~750 ms (the 500 ms
throttle + overhead) until the user intervened. A second live episode (`d22ad8e8`, 22:16) shows
the same prompt marked-`Sent` twice around a decline. Restart press-storms are also on record
(11:42 / 16:03 / 17:25 — multiple sessions' first watchdog presses in the same millisecond):
reopen re-observes arm restored `Sent`-but-unechoed prompts (`echoed` is transient), fire 3 blind
Enters into each restored box, then give up — `Failed` + autorunner paused, quietly undoing the
reopen's re-arm.

## 2. Root causes

- **RC1 — readiness ≠ delivery.** The advance's serializer (the pickup guard) keys on the *echo*
  within a 4 s window; the delivery's true extent is the swap's box hold (legitimately up to
  ~21 s: 2.4 s clear + 18 s submit-await). Once the echo lands — or the window lapses — the
  scheduler believes the lane is free while the box is still owned.
- **RC2 — the rollback feeds the trigger.** A declined submission rolls its prompt back to
  `Pending` via an `Update`, whose notify runs `OnObserved` → `RequestAdvance` → another doomed
  attempt: a self-feeding mark/decline/rollback livelock braked only by the throttle.
- **RC3 — the watchdog is blind.** `DecideEnterRetry` consults state + echo + transcript, never
  the box owner: it can inject a lone `\r` into a box mid-swap (submitting the user's partial or
  restored draft — the exact merge §9 exists to prevent), and it storms on restart because a
  loaded `Sent` prompt's transient `echoed=false` reads as an in-flight, unacknowledged send. It
  also pressed into *dormant* (never-started) tabs, whose pre-Connected `WriteInput` drops.
- **RC4 — the swap trusts the box read over the turn evidence.** The submit-await demanded
  `box.empty() && turnStarted`; a box the TUI repopulates after a successful submit (a Ctrl+S
  stash popping back at turn end, a repaint the detector half-reads) held the await to its full
  budget, deferred the draft restore, and kept the latch (and now the gate) held — while the log
  couldn't even say *what* the box read.
- **RC5 — `[send]` logs acceptance, not delivery.** Eight `[send]` lines in the incident; zero
  bytes reached the terminal.
- **RC6 — the echo compare is newline-exact.** The Manager compose `TextBox` stores a typed
  newline as `\r` (the measured UWP quirk `EvaluateDraftPull` already folds for), while
  `BuildPromptFill` folds `\r`→`\n` at inject, so the wire echo carries `\n`:
  `p.text == msg.promptText` never matched a multi-line composed prompt. The echo went unconsumed
  (a phantom unacknowledged send for the pickup guard *and* the watchdog) and the message was
  re-recorded as a duplicate `Typed` row.

## 3. The design — one engine-owned gate per session

A **delivery gate**: a per-session, engine-owned fact meaning *"exactly one operation owns this
session's input box right now"*. It lives as two **transient** fields on `SessionInfo`
(`deliveryPromptId` — the owner tag, non-empty == open — and `deliveryOpenedUnixMs`; never
persisted), maintained by `SessionRegistry` under its lock, and readable by every pure decision
through `DeliveryGateOpen(s, now)` (SessionModels.h).

**Lifecycle.**

- **Opened by `SubmitPrompt`** — the one send seam — *before* it invokes anything, tagged with the
  prompt id (`DeliveryGateTagFor`). A second `SubmitPrompt` while open is declined synchronously,
  before any marshal, and the caller performs its usual Rule-#4 rollback.
- **Closed by whoever resolves the delivery:**
  - the **fallback path** (no submitter bound — tests/CLI/legacy windows): the inline
    `Inject(BuildPromptSubmission(…))` *is* the delivery — close synchronously, right after it;
  - the **submitter path**: the hosting window's swap owns the outcome and closes on **every**
    exit (a `wil::scope_exit` — legal here because `CloseDeliveryGate` is registry-only,
    thread-safe work with no UI affinity, unlike the read-only release which stays an explicit
    UI-thread call);
  - a submitter that **refuses or throws**: `SubmitPrompt` closes before returning `false`.
- **The mail-button clear** (`PENDING_INPUT.md` §8d) opens the same gate under the reserved tag
  `kDeliveryGateClearTag` — it holds the same box, so it must exclude deliveries the same way.
- **The close NOTIFIES** (one `_notify`, `HookEvent::Unknown` — the `Update`/`SetStarted` idiom):
  that notify is the wake-up that re-runs `OnObserved` → `RequestAdvance`, so a held advance
  resumes the instant the box is free instead of polling or flapping. Opens are quiet.

**Consumers.**

- `DecideAdvance` **holds** while the gate is open (reason `"delivery in flight"`, change-deduped
  into `[advance-skip]` like every stall) — the *primary* serializer. The echo-keyed pickup guard
  stays as the pacing belt for the raw-inject fallback path, whose gate closes synchronously.
- `DecideEnterRetry` returns `None` while the gate is open (the close-notify re-arms the watch if
  the send is still unacknowledged), and **never fires for a dormant session**
  (`!started && !external` — the same carve-out `OnObserved`'s idle-start trigger uses).
- The **standby fill** and **§10 restore re-fill** pumps skip a gated session for that tick
  (`DeliveryGateHeld`) — they type into the same box.

**Failure containment (fail-open by construction).**

- **Expiry belt:** `kDeliveryGateTimeoutMs` (≈ 2× the swap's worst case — 45 s originally; 75 s
  since the §11 VERIFIED PLACEMENT grew the worst case to ~36 s). An expired gate
  reads *closed* everywhere; the next open **reclaims** it (logged `[gate] … reclaimed`). A gate
  stamped in the future (clock jump) also reads closed.
- **Owner-checked close:** `CloseDeliveryGate(id, tag)` clears only a matching tag — a stale
  holder (expiry-reclaimed, or its record re-keyed) can never clear a younger delivery's hold.
  A double close is quiet (no phantom wake-ups).
- **`Upsert` re-key:** replacing a record wipes its transient fields → the gate reads closed, the
  late close no-ops on the tag mismatch. Nothing stalls; the UI-side box latch
  (`_draftSwapsInFlight`) remains the second belt for the brief overlap.

## 4. Component changes

| Where | Change |
| --- | --- |
| `SessionModels.h` | `deliveryPromptId` / `deliveryOpenedUnixMs` transient fields; `kDeliveryGateTimeoutMs`; `kDeliveryGateClearTag`; pure `DeliveryGateOpen(s, now)`; `DeliveryGateTagFor(submission)`. |
| `SessionRegistry.{h,cpp}` | `TryOpenDeliveryGate` / `CloseDeliveryGate` (owner-checked, close notifies) / `DeliveryGateHeld`; `SubmitPrompt` (now non-const) opens the gate first, declines synchronously while held, resolves the fallback path synchronously, closes on refusal/throw; `[gate]` + `[delivered]` log lines; **RC6:** the echo consume and `NoteExternalPrompt`'s dedupe compare fold `\r`/`\r\n`→`\n` on both sides (`FoldCrToLf`, the `BuildPromptFill` fold). |
| `Scheduler.h` | `DecideAdvance`: hold on an open gate (before the pickup guard). `DecideEnterRetry`: dormant gate + delivery-gate hold. |
| `Scheduler.cpp` | `_process`'s auto-send resolves its target **by id** from the decided snapshot and mutates by id (an index alone could be redirected onto a different `Pending` prompt by a concurrent queue reorder/delete during the 500 ms throttle gap); the deferred-send log text covers the gate-declined case. |
| `Persistence.cpp` | A loaded `Sent` prompt reads `echoed = true` — the existing comment ("a reloaded Sent prompt's echo already happened in a past run") made executable. Kills the restart press-storms at the source. |
| `TerminalPage.AgentObserver.cpp` | The swap closes the gate on every exit (scope guard); **RC4:** the submit-await succeeds on `turnStarted && (box empty OR box ≠ the sent text)` — only a box still holding *the prompt* defers — and the defer log now prints the box's size, whether it matches the prompt, and its head; `_RestoreDraftAfterSwap` verifies first (a box already reading the draft — e.g. the TUI restored its own stash — is returned untouched: pressing anything could only disturb it); the mail-clear opens/closes the gate around its ladder; both pumps skip a gated session's tick. |

## 5. Invariants (do not regress)

1. **At most one operation owns a session's input box at a time**, and the delivery gate is the
   *shared, engine-visible* fact saying so. Anything that types into the box — a delivery, the
   mail-clear, a pump fill — either holds the gate or checks it.
2. **An advance never marks a prompt `Sent` while the gate is open.** Marking-then-declining is
   the livelock; holding is free (the close notifies).
3. **The watchdog never injects** while the gate is open, into a dormant (`!started && !external`)
   session, or for a prompt whose `Sent` state was loaded from disk.
4. **A close always notifies; a mismatched or double close never clears a younger gate and never
   fires a phantom wake-up.**
5. **Every open has a bounded lifetime** (the expiry belt) — a leaked gate can stall a plan for at
   most `kDeliveryGateTimeoutMs`, never permanently.
6. **The submit-await defers only when the box still holds the sent prompt.** Turn-started plus a
   box reading anything else (empty, the user's draft, a TUI-restored stash) is success; the
   restore path re-verifies before pressing anything.
7. **Echo dedup is newline-fold tolerant** — a `\r`-composed prompt's `\n` echo is one message,
   recorded once.

## 6. Edge cases

- **Draft text == prompt text** (the mail button queued the box's own draft, Shift-kept): after
  the send, a box repopulated with that same text is indistinguishable from an un-submitted
  prompt — the await defers, which lands the *correct* end state (prompt sent; the identical
  draft stays in the box/stash; nothing restored on top of it).
- **Send-now racing an in-flight delivery:** declined synchronously by `SubmitPrompt` → the
  existing rollback → the prompt re-fires on the gate's close notify (autorunner) or the user's
  next click. One mark/rollback pair, bounded — never a loop.
- **Expiry mid-swap:** a second delivery may open while the stale swap still runs; the UI-side
  `_draftSwapsInFlight` latch declines the newcomer's swap (the pre-existing belt), and the stale
  swap's eventual close is an owner-mismatch no-op.
- **Scheduler race on the snapshot:** `DecideAdvance` reads a snapshot; a gate opened between the
  decide and the `SubmitPrompt` (a Send-now race) is caught by the synchronous decline — one
  bounded rollback, then the advance holds.

## 7. Tests (`AgentMaster/tests/`)

`TestDeliveryGate` (tests_spawn_sched.cpp): the pure predicate (closed / open / expired / future
stamp); registry open→decline→owner-close→reclaim→stale-close semantics; close notifies exactly
once (opens and double closes are quiet); `DecideAdvance` holds on an open gate and resumes on
expiry; `DecideEnterRetry` refuses while gated / dormant, still drives an adopted external;
`SubmitPrompt` declines while held, resolves + closes the fallback synchronously, keeps the gate
open across an *accepted* submitter until the window's close, and closes on submitter
refusal/throw; the `\r`-composed / `\n`-echoed prompt is consumed with no duplicate `Typed` row.
`TestPersistence` additionally pins: a persisted `Sent` prompt loads `echoed=true`, a `Pending`
one `false`.

## 8. Explicitly out of scope (known, separate)

- The **question-guard trap — CLOSED** (the `b5f766fc` live report: three mail-queued prompts
  parked by `lastMessageWasQuestion`, autorunner Full, mode-cycling tried blind, the reason
  visible only as a change-deduped `[advance-skip]`). Three-part fix: **(1)** an **explicit
  autorunner re-arm** (Off→Semi/Full, the Manager header toggle *and* the overlay cycle — the
  exact gesture users try) clears the question latch: a human clicking GO is attending, so the
  "don't blindly answer a question" hold no longer applies, and the next question-ending Stop
  re-latches it, keeping the protection for later turns; **(2)** the overlay **mail button**
  queues its draft with `guardPattern = kAnswersQuestionOk` — the session's *own* unsent draft
  is by construction the user's next message to it, never a blind auto-answer; **(3)** the
  overlay's ⏳ row prefixes **"❓ held — answer the question"** (orange) whenever the guard is
  holding the next prompt, with the releases spelled out in its tooltip. Still open: a
  per-prompt opt-out control in the Manager queue UI, and a board-card hold indicator.
- **Paste placeholders** (`[Pasted text #N +M lines]`) queued verbatim by the mail button lose the
  paste content when the queued copy is sent — expansion at queue time is the §8c/§9-consistent
  fix.
- The pending-input detector's **~120-row read window**: a draft taller than the window is
  invisible to the swap's protection entirely. **→ promoted into R5** (the tri-state box read +
  the on-demand raised verification window — [`DELIVERY_PLAN.md`](DELIVERY_PLAN.md) Part 2).
- A fuller **accepted/delivered log split** beyond the added `[delivered]` lines.

## 9. Incident 2 — the duplicate-Stop advance (fixed)

Post-deploy live report (`b5f766fc`, 08:44 local): *"sent two messages on top of each other while
the previous message was just now accepted but the status was yet to update."* The trail:

```
08:44:50.860  [Stop]  question=1            ← the real turn end
08:44:51.710  [delivered] 0906f5ce          ← prompt #1 ("check for best one") delivered
08:44:53.360  [UserPromptSubmit]            ← #1's echo: the new turn starts (Running)
08:44:53.375  [Stop]                        ← 15 ms later — a DUPLICATE of 08:44:50's Stop whose
                                              slow pwsh forwarder stamped its ts ~3.6 s late,
                                              i.e. AFTER the next prompt's UPS ts
08:44:53.884  [send] #6                     ← the advance seam trusted it as an instant complete
08:44:54.778  [delivered] 9a5fccd1          ← prompt #2 pasted into claude's RUNNING turn
```

The transcript proves the damage: claude had opened an `AskUserQuestion` dialog for prompt #1;
prompt #2's 49 characters were consumed by the dialog rendering and **never became a message** —
a silent loss recorded as delivered (the dialog's eventual answer carries only the user's option
pick). The ordered machine's staleness test (`stop.ts < lastPromptUnixMs`) assumes a Stop's `ts`
orders it truthfully; a duplicate Stop process stamped after the next prompt's UPS defeats it and
reads as a 15 ms turn-complete — flipping state to Waiting, re-latching the question bit, and
firing the advance.

**Two-layer fix (both pure, both tested):**

- **The minimum-real-turn-span floor** — `kMinRealTurnSpanMs` (2 s) in `NextSessionStateOrdered`:
  a non-quiescent Stop landing within the floor of the newest prompt's `ts` cannot be that turn's
  real completion (measured anchors: the fastest trivial turn in the incident corpus is 3.4 s; the
  observed duplicate-stamp skew is ≤0.4 s). It reads STALE — state kept, question-bit and advance
  suppressed. Deliberately placed **after** the type-ahead branch (a queued batch's consume
  legitimately lands close behind its type-ahead prompt), and the scanner's quiescent Stop is
  exempt — the self-heal for a genuinely-faster-than-floor turn (~2.5 s extra latency, nothing
  lost, advance included).
- **The evidence-released pickup guard** — `DecideAdvance` no longer expires its hold after a
  naked 4 s (the other half of the same race class: the 07:27 incident's `#6` fired 9 s after `#5`
  through exactly that lapsed window). A Sent-unacknowledged flight prompt now holds the next
  advance until the turn **visibly started** — the echo consumed, a newer `UserPromptSubmit`
  stamped `turns.lastPromptUnixMs` past the send (an echo the text-match missed still proves the
  pickup), or the transcript advanced meaningfully past it (the no-hook fallback, the same margin
  `DecideEnterRetry` trusts) — with `kPickupGuardMaxMs` (30 s) as the lost-evidence belt, sitting
  above the Enter-retry watchdog's ~21 s give-up horizon (which resolves a truly-dead send to
  `Failed` + a paused autorunner first).

Invariant addendum (extends §5): **an advance may fire only off a turn-complete whose turn
demonstrably ran** (the floor), and **never while the previous delivery's turn has not visibly
started** (the evidence-held guard). Time alone releases nothing except the two belts.

## 10. Incident follow-ups — coverage verdicts + residuals (→ DELIVERY_PLAN.md)

Two more suspected behaviors in the same session (`b5f766fc`), checked against the logs:

**A — "the next prompt was sent while a question was asked": the SAME duplicate-Stop race,
recorded twice.** The duplicate Stop carries the question bit of the *old* transcript tail, so its
false turn-complete both re-latches `question=1` and fires the advance in one stroke:

```
08:42:26.325  [UserPromptSubmit]            ← "asdasd"'s echo (turn actually still running)
08:42:26.340  [Stop] question=1             ← duplicate, 15 ms later — advance fired…
                                              …but the OLD 4s pickup guard caught it by ~200 ms
                                              (the send waited for the REAL Stop at 30.791)
08:44:53.360  [UserPromptSubmit]            ← "check for best one"'s echo
08:44:53.375  [Stop] question=1             ← the same duplicate shape; the echo had already
                                              consumed → the guard released → #6 stacked (§9)
```

**Covered** by the §9 floor: both duplicates now read stale — no advance, no state flip, no
question re-latch — and a floor-suppressed *real* Stop's question bit is recovered by the
scanner's quiescent synth, which computes it from the tail (`SessionScanner.cpp` —
`EndsWithQuestion` on the last assistant text; the quiescent Stop is never stale, so the ordered
machine applies its bit). Note the deliberate semantic that remains: a **mail-queued** prompt
(`kAnswersQuestionOk`, §8) still fires on a *genuine* question-ending turn-complete — that is the
opt-out working, not a race.

**B — "the agent finished but the follow-up wasn't prompted in": two phenomena, one residual.**
The 08:11→08:42 park was the §8 question-guard trap (fixed: re-arm release + mail opt-out +
visibility). The deeper one: the stacked `#6` (*"then for another one but spawn subagent…"*)
sits terminally **`Sent`** in the persisted queue while the transcript proves it **never became a
message** (§9 — consumed by the AskUserQuestion dialog rendering). The §9 floor prevents this
*cause*; the three residual gaps R1–R3 are now **IMPLEMENTED** (spec + deviations in
[`DELIVERY_PLAN.md`](DELIVERY_PLAN.md); engine-tested — the harness's `TestLostSendReconciler` +
the rewritten guard/watchdog/ordered suites):

- **R1 — the lost-send reconciler (shipped).** Two halves. **(1) The PULL echo-consume:**
  `NoteExternalPrompt` (fed by the scanner with each user line's OWN timestamp, newly parsed onto
  `TranscriptEvent::lineTsMs`) now marks a fold-matched `Sent`+unechoed Autorun prompt **`echoed`**
  — the transcript line IS the proof the injection became a message — so `echoed` is THE delivery
  fact for hooked and no-hook sessions alike (a replayed OLD identical line, ts before the send,
  never vouches; a late-READ fresh line still consumes — deliberately no recency window; logged
  `[pull-echo]`). **(2) The verdict:** pure `DecideLostSend` (Scheduler.h, `kLostSendSettleMs`
  15 s) — a live session AT REST + gate closed, a `Sent`+unechoed+stamped Autorun prompt settled
  past 15 s ⇒ LOST; the scanner's reconcile pass applies it (freshest-record decide + in-`Update`
  re-verify): **`Failed` + the session's autorunner paused** (`mode → Off`), logged `[lost-send]`
  to both logs, **never auto-resent** (the text may sit in the TUI's type-ahead/box — a resend can
  double it). **Deviation from the plan draft:** the "a turn ran past the send" conjunct was
  DROPPED — a watchdog press refreshes `sentAtUnixMs`, which made that conjunct permanently false
  after one press (stranding the draft-blocked corner `Sent` forever — the very gap R1 closes);
  the press-refresh also naturally serializes the two recoveries (the verdict can only fire once
  the ladder went quiet), and the echo conjunct is the real false-positive protection.
- **R2 — sound pickup evidence (shipped).** Both consumers re-keyed: `DecideEnterRetry`'s
  "picked up" drain and the §9 pickup guard's release **dropped the raw transcript-advance
  clause** (`convLastActivityUnixMs` — a still-running PREVIOUS turn also writes the file; it is
  exactly why the watchdog drained instead of resolving `#6`); started-ness is now `echoed` (push
  or pull) / the state leaving the ready set / a newer prompt-carrying UPS stamp (guard only).
  The retired `kEnterRetryActivityMarginMs` is gone. **Addition the plan lacked — the DRAFT
  GUARD, proven load-bearing on this very log:** at 08:50:07 the box held the user's 17-char
  draft (*"maybe another one"*) while the lost `#6` sat Sent-unechoed — the plan-as-written
  watchdog, re-armed at that Stop, would have pressed a lone Enter and **submitted the user's
  draft** (the RC3 merge). Now a box observed holding text ≠ the watched prompt
  (`DraftMatchesPromptText` — the promoted shared `FoldCrToLf` + trailing-trim) is never pressed
  into (answer `Waiting`; the lost verdict owns the terminal resolution), while a box holding OUR
  prompt still presses — that IS the eaten-CR rescue.
- **R3 — phantom / duplicate `UserPromptSubmit` events (shipped, forensics corrected).** The
  Step-1 instrumentation came first and REDATED the diagnosis: the phantom UPS events
  (08:42:28.904, 08:44:53.902, 08:50:14.515 — the pattern is endemic: a late twin ~0.5–2.6 s
  after each real hook) recorded **no `Typed` rows** and match **no transcript message**, so they
  carried **EMPTY prompt text** — late duplicate deliveries whose payload lost the prompt, NOT
  text-duplicates; and the AskUserQuestion dialog answer rides **`PostToolUse`**, never a UPS
  (proven at 08:48:47), so the plan's dialog-answer caveat is moot. Fix: the ordered machine
  gates BOTH `turns.lastPromptUnixMs` **and the type-ahead count** on a **non-empty prompt** — an
  empty UPS is state-only (→ Running, self-healing if phantom). That kills the real-Stop floor
  suppression (measured: the 08:42 real Stop at +1.9 s of the twin's stamp read stale — now it
  measures against the real prompt and completes cleanly), the phantom type-ahead consume (the
  real Stop read as a batch consume and stranded Running ~2.5 s), and the guard's spurious
  prompt-stamp release; the scanner's recon-run synth (deliberately empty) stops stamping too —
  its reconciliation-timed stamp was itself a floor hazard. The speculative text-dedupe was
  **rejected**: unmotivated by evidence, and it risked eating a real repeat-typed type-ahead
  ("y" twice mid-turn), reintroducing the mid-turn advance. Every UPS now logs one `[ups]`
  disposition line (`chars=N hash=… -> echo|typed|noise|EMPTY`), so any future phantom shape is
  self-evident from hooks.log.

Invariant addendum (extends §5/§9): **a `Sent` prompt is either provably a message (`echoed` —
push or pull), provably dead (`Failed`, surfaced, autorunner paused), or still being watched**;
**no watcher accepts another turn's activity as proof of our prompt's pickup**; and **the
watchdog never presses into a box holding anything but the watched prompt itself**.

## 11. Incident 3 — the invisible-content MERGED send (→ the R4–R8 verified-placement plan)

Live, post-R1–R3 (dev, session `1bf4bd83` — the same experiment session as §1 — 2026-08-12
10:28 local). A 13-char prompt was delivered into a box every read had called empty, and the one
message claude received carried **4205 chars** — pre-existing TUI-internal content concatenated
with our prompt. The user-facing lesson, verbatim spirit: *once we SET text in the edit box,
nothing validated that the exact text is the actual prompt* — the whole pipeline validates the
steps AROUND the placement (clear, restore, discard), never the placement itself.

```
10:24:48–10:25:25  three swap cycles: a ~2.2–2.5K placeholder-bearing draft cleared via Ctrl+S
                   each time, prompt sent + echoed each time — but the RESTORE FAILED all three
                   ("could NOT be put back verbatim … kept in memory and in the kill-ring"),
                   and the box kept REFILLING between cycles (chars=2241 → 2455 → 2466: the
                   stash auto-pop at each submit re-planting it). After three failed channel
                   round-trips the TUI-internal state (stash-slot parity, kill-ring content)
                   is UNKNOWN to us — ~2.4K parked somewhere we cannot read.
10:27:58 / 10:28:01  two mail-queue moves; each DISCARD-ladder clear VERIFIED empty (live read).
10:28:11.077  [Stop]                            ← turn complete → the advance fires
10:28:11.969  [delivered] fb83b0ef (chars=13)   ← "lol again 10s" — via the swap's NO-DRAFT FAST
                                                  PATH: the live double-read (60 ms apart, and
                                                  mutation-id-gated, so a pop between the reads
                                                  would have been seen) answered EMPTY — no
                                                  lock, no clear, paste+CR in ONE write
10:28:12.505  [ups] chars=4205 -> recorded as Typed   ← the submitted message: ~4.2K of
                                                  TUI-internal content + our 13 chars, MERGED
10:28:30.388  [lost-send] fb83b0ef … marked Failed, autorunner paused
```

**Proven:** the box was verified empty at 10:28:01.58 (discard ladder) and again by the fast
path's two live reads ~60 ms before the write; the message claude received carries our prompt
inside 4205 chars; the post-hoc machinery (fold-mismatch → `Typed` row; `DecideLostSend` →
`Failed` + paused) worked exactly as designed — **18 s after claude had already acted on a
message nobody wrote**. **Not pinned:** which channel materialized the content (the loaded
stash slot's auto-pop at turn-end / at our own submit; late fallout of the failed restores'
Ctrl+S/Ctrl+U churn) and where it landed — in the ≤60 ms window between the last read and the
write, between the paste and the CR inside claude's own event interleave, or in a render shape
the detector misses. Phase-0 probes in the plan pin it; every variant is covered by the same
three-layer answer (R4 catches it at the read-back, R6 removes the dominant source, R7 names
the aftermath honestly).

**Root causes (extend §2):**

- **RC7 — the PLACEMENT itself is open-loop.** Every verified cycle guards a *neighboring* step:
  the clear re-reads until confirmed empty, the restore compares against the draft, the discard
  ladder re-reads per round, the submit-await re-reads after the fact. The send is
  `BuildPromptSubmission` = bracketed paste + CR in ONE `WriteInput`: nothing ever reads the box
  between the paste landing and the CR committing. Everything ahead of it is precondition
  checking; the commit point trusts blindly.
- **RC8 — `""` is ambiguous.** The pure detector computes `boxFound`, but
  `ControlCore::ReadPendingInputDraft` drops it at the boundary: "no box visible" (an
  AskUserQuestion menu replacing the box — the §9 damage shape, a draft taller than the 120-row
  window, a mid-repaint frame) and "box present, empty" both read `""` — and every consumer
  treats `""` as the SAFE case ("nothing to protect ⇒ inject").
- **RC9 — a failed channel operation leaves TUI-internal state UNKNOWN, and it detonates
  later.** Ctrl+S is a one-slot toggle that AUTO-POPS at the next submit (measured — the
  PendingInput.h gotcha); Ctrl+U/Ctrl+Y is a ring we share with claude. Three failed restores
  left ~2.4K parked across those channels with parity we cannot inspect; the pipeline carried on
  as if the swap had cleanly resolved, and the parked content re-entered the box at a
  TUI-chosen moment no scan tick was guaranteed to precede.

Invariant addendum (extends §5): **no submit CR is committed into a box that has not been read
back as exactly-the-prompt** (or provably collapse-equal — claude re-collapses a large pasted
fill into a `[Pasted text #N +M lines]` placeholder), and **a box that cannot be read refuses
placement instead of defaulting to "empty"**. The implementing plan — **R4 verified placement
(fill → read-back → commit) · R5 the tri-state box read · R6 TUI-channel hygiene · R7 the merge
classifier · R8 the live-read watchdog press** — is [`DELIVERY_PLAN.md`](DELIVERY_PLAN.md)
Part 2.

**Outcome — R4–R8 are IMPLEMENTED** (deviations flagged inline in the plan; the largest: a
verify-STRIKE ledger so an unverifiable prompt resolves `Failed`+paused instead of rollback-
livelocking, a whitespace-stripped compare so soft wrap can't false-Foreign a wide prompt, the
Phase-0 probes deferred with every probe-gated decision taken conservative, and the send-side
protections extended to ADOPTED sessions — their bind path had registered the injector alone, no
submitter, so no swap and no verify). The send is now `Inject(BuildPromptFill)` → a deep
read-back probe (`ReadInputBoxProbe`, 1000 rows — a filled prompt can outgrow the 120-row scan)
→ the lone CR only on `Verified`/`VerifiedCollapsed`; `Eaten` re-fills (≤2), `Partial` discards
its own text and re-fills, `Foreign` undoes the insertion in verified backspace batches (budget
== the folded prompt length, so foreign text can never be eaten) and records the remainder as
`pendingInput`; the pre-flight declines into `NoBox`/`MenuOpen` (the recorded state holds the
advance; the release notifies); the watchdog's Enter goes through a live-read presser that never
presses into a menu; a merged submit is named `[merge-detected]` at both echo seams and Failed
immediately; the fill pumps content-verify. Off-switch `AppSettings::verifySendBeforeSubmit`
(default ON, cog → TESTS AUTORUNNER). Engine-tested (`TestVerifiedPlacement`, harness
**3214/3214**) + `TerminalAppLib`/`TerminalControlLib`/CLI compile green; live verification
rides the next deploy cycle. Replaying §11's shape against the new pipeline: the double-read
answering Empty is now followed by a LOCKED fill whose read-back sees the 4K materialize →
`Foreign` → no CR, insertion undone, content recorded on the dots — the merge cannot commit.

## 12. Incident 4 — give-up-to-Off fired on FALSE signals and overrode a manual Full (fixed: confirm-and-retry)

**The report (2026-08-12, the same experiment tab `1bf4bd83`, ~3.5h after R4–R8 deployed):**
"the state of a tab moves to **Off** instead of staying **Full** … we are fully capable of
reading the entire string in the prompt box, clearing it, filling it back — all 100% successful —
we just have to keep **confirming** the transaction of inputting a string was successful, and
**if not, retry** — instead of giving up and moving the autorunner to Off." Read as a design
directive: the R4 input transaction (fill → read-back → CR) is trusted; the TERMINAL give-ups
around it are not.

R4–R8 had added FOUR new mode→Off sites on top of the three that pre-dated them. The live log
showed two of them firing **on false signals** within one minute of each other:

**Episode A — the R5 box-not-visible escalation overrode an explicit manual re-arm.**
```
20:43:10.775  [Stop] 1bf4bd83                              turn complete
20:43:14.003  [nav] autorunner 1bf4bd83 -> Semi (overlay)  the human cycling the mode
20:43:14.005  [advance-skip] (input box not visible)       recorded box state was ALREADY NoBox
20:43:16.079  [nav] autorunner 1bf4bd83 -> Full (overlay)  the human's explicit GO
20:44:04.505  [send-verify] 1bf4bd83 autorunner paused (no parseable input box for >60s …)
```
The NoBox clock (`pendingBoxStateUnixMs`) had been running from ~20:43:04 — BEFORE the re-arm —
so the escalation measured "60 s ignored" from a stamp that predated the human's GO and flipped
their Full back to Off 48 s after they set it. Compounding it: a NoBox verdict is **not always a
fault** — the detector's `MenuOpen` verdict keys on the NUMBERED option-row shape (`❯ 1. Yes`),
so a parked NON-numbered menu (the rewind / slash-command class) falls through to NoBox — the
same "legitimately parks for hours" class MenuOpen was always escalation-exempt for. (Whether
Episode A's NoBox was a real parked menu or a render drift is unresolved — the ConPTY probe
harness had an input-side regression that day — but the fix is correct under either truth.)

**Episode B — the lost-send verdict fired ONE SECOND after gate expiry on a delivery that was
still alive, and the "lost" prompt then delivered fine.**
```
20:40:41.7    [rehome] … resume claude 1bf4bd83            a 51-tab window restore, ~85 claudes live
20:40:45.239  [send] 1bf4bd83 #25                          mark-Sent, delivery ACCEPTED
   (the delivery coroutine sits queued behind the restore-wedged UI dispatcher…)
20:42:01.591  [lost-send] … prompt a85dfc20 "lol again 5s" (marked Failed, autorunner paused)
20:42:08.939  [delivered] 1bf4bd83 prompt a85dfc20 (chars=12, verified=exact)
20:42:09.494  [ups] … -> recorded as Typed                 the echo found no Sent prompt (Failed) ⇒ duplicate row
20:42:14.145  [startup] splash: foreground terminal connected (90219ms)
```
Mark-Sent and the INJECTION are separated by the hosting window's dispatcher, and that gap was
measured at **83 s** on a loaded restore — longer than the gate's 75 s expiry belt. The verdict
required only Sent + unechoed + settled + gate-closed, so the expired gate un-held it at 76 s;
the delivery then executed anyway (nothing re-checked the prompt's status), submitted a message
the system had already written off, and the echo — finding no Sent row — double-recorded it as
Typed. Every element of the verdict's premise ("delivered but never became a message") was
false: it had not yet been delivered at all.

### The fix — confirm-and-retry, never override a human, terminal only on real evidence

**Injection evidence (`QueuedPrompt::injectedAtUnixMs`, transient).** Every seam that actually
writes a prompt's bytes to the ConPTY (the verified CR commit, the legacy `plainSend`, the
registry's no-submitter direct inject) stamps it via `SessionRegistry::MarkPromptInjected`;
every mark-Sent/rollback seam resets it. `DecideLostSend` now REQUIRES it, and the settle runs
from `max(sentAt, injectedAt)` — a late injection gets its full echo window. The terminal
verdict (Failed + autorunner paused, unchanged) is thereby reserved for the case it was designed
for: a prompt that demonstrably reached the terminal and demonstrably never became a message
(the `#6` dialog-consumed shape — which still carries injection evidence).

**The undelivered-send RECLAIM (`DecideUndeliveredReclaim` + the scanner's `[send-reclaim]`).**
A Sent prompt with NO injection evidence whose gate claim has lapsed is rolled back to
**Pending** — no Failed, no pause, the advance's notify retries it. Unconditionally safe:
nothing was ever typed, so nothing can double. This is the directive's retry, applied at the one
place a retry is provably harmless. (State-independent on purpose — reclaiming promptly is also
what disarms a zombie delivery before it could fire mid-turn.)

**The zombie-delivery abort (the per-attempt gate tag + the top guard).** `SubmitPrompt` stamps
a monotonic `submitNonce` into the submission, so each delivery ATTEMPT owns a unique gate tag
(`DeliveryGateTagFor` = `promptId#nonce`) — a reclaim + resend of one prompt mints a NEW tag.
`_SubmitPromptWithDraftSwapImpl`'s first act after its dispatcher hop is now a STALE-DELIVERY
GUARD: the prompt must still be Sent and `RevalidateDeliveryGate` must re-assert THIS attempt's
claim (an expired-but-unreclaimed claim revives — the holder was alive, merely starved; a
reclaimed one refuses ⇒ abort, type nothing, touch nothing). The verified injector re-asserts
the claim before each fill and — decisively — immediately before the irreversible CR
(`_InjectPromptVerified` return 3 = stale abort, no rollback: the prompt belongs to whichever
newer attempt owns the claim). Under Episode B's replay: the reclaim rolls the prompt back at
~76 s, the advance resends (new tag), the zombie wakes at 83 s, fails its top guard, and exactly
one carrier delivers.

**The R5 escalation is DEFANGED into a once-per-episode WARNING (`ShouldWarnOnBoxNotVisible`).**
It no longer touches the mode — `DecideAdvance`'s NoBox hold already parks the queue, and
`SetPendingBoxState`'s blocked→unblocked notify already self-resumes it the instant a box
renders, so flipping Off bought nothing except robbing the user's setting and requiring a manual
re-arm for a state that heals itself. The scanner logs the `[send-verify] … queued prompts HELD`
warning once per NoBox episode (deduped on the stamp in `ScanState::boxNotVisibleWarnedStamp`).
**And a manual Off→Semi/Full re-arm now RE-STAMPS `pendingBoxStateUnixMs`** (both arming
surfaces — the Manager header toggle and the overlay cycle, the question-latch-clear precedent),
so no automatic window is ever measured from before a human's explicit GO. The state itself is
deliberately NOT cleared on re-arm: a genuinely unreadable box must keep holding until the scan
sees a box again (~one tick when it was a phantom).

**The R4 verify-strike terminal keeps the MODE.** `kSendVerifyMaxFills` 2 → 3 (more inner
confirm-and-retry — livelock-free, the fill loop never touches the advance trigger), and the
2-strike terminal still marks the prompt Failed (bounded — never the same doomed verify at
advance cadence, the RC2 shape) but no longer pauses the autorunner: a REFUSED merge is the
protection working, not a fault to punish the user's mode for, and a persistent wall is held
upstream by the strike-free pre-flight decline + the recorded box state. The paths that DO still
pause: a POST-COMMIT `[merge-detected]` (a real mangled message reached the transcript), the
Enter-retry give-up (3 verified presses into a readable box that ignored them), stop-on-error,
and the evidence-backed `[lost-send]` — each a real, human-worthy fault, none reachable by a
merely-slow delivery or an unreadable render any more.

Coverage: the §12 conjuncts + reclaim matrix + `MarkPromptInjected` / `RevalidateDeliveryGate` /
nonce-tag uniqueness + the re-arm re-stamp window in `TestLostSendReconciler` /
`TestVerifiedPlacement` (harness **3244/3244**); `TerminalAppLib` compiles green. Live
verification (the Episode-A gesture: Stop → set Full → wait, tab stays Full) rides the next
deploy cycle.

## 13. Incident 5 — the ts-corrupted duplicate Stop double-send (fixed: arrival stamp + MINIMUM SEND SPACING)

**The report (session `ea1dc3dd`, 2026-08-15 11:12): two queued prompts popped and double-sent
~2.6 s apart.** The reconstructed timeline (hooks.log + autorunner.log + the transcript, which
is ground truth — local = UTC+3):

```
11:12:01.5  the running turn's REAL end (transcript end_turn text; stop_hook_summary 02.179Z+3h)
11:12:02.6  [Stop] arrives -> WaitingForInput          (the line's 11:11:47.975 stamp is a
            [send] #1 (11:12:02.654)                    partial-line artifact: [tooltip-wheel]
                                                        wrote the line-head without a newline)
11:12:04.159 [delivered] #1 (61 chars, verified=exact)  the /np-function prompt
11:12:04.169Z+3h the transcript's user line — #1 SUBMITTED, the turn RUNNING
11:12:04.692 [ups] echo consumed -> Running             pickup guard legitimately released
11:12:04.729 [Stop] state=2  ← THE BUG: accepted as a fresh turn-complete 37ms after the UPS
11:12:05.240 [send] #2 (the 500ms throttle after the bogus turn-complete)
11:12:06.378 [delivered] #2 (280 chars, verified=exact) — INTO THE RUNNING TURN; claude queued
             it (transcript queue-operation 06.399Z; the box then read "Press up to edit queued
             messages"), the user's overlay mail-click re-queued + cleared the box, #2's turn
             never started -> enter-retry ×3 -> give-up -> Failed + autorunner paused
```

**Root cause — a duplicate Stop delivery with a corrupted ordering stamp.** No turn ended at
11:12:04.729 (the transcript's next output is 11:13:20), so that Stop is a DUPLICATE delivery of
the 11:12:02 Stop — the §9/§10 twin phenomenon (this very session also shows two `[Unknown]`
twins at 10:10:07.026 and 11:12:02.649: mangled deliveries whose event name didn't parse). §9's
`kMinRealTurnSpanMs` floor should have read a 37 ms "turn" as stale — but with BOTH stamps sane
the acceptance is arithmetically impossible (`stop.ts ≥ ups.ts + 2000` cannot hold when the Stop
ARRIVED 37 ms after the UPS and ts is stamped before delivery), so at least one of the two wire
`ts` fields was lost/garbled: `ParseWireLine` leaves `ts=0` on a truncated tail (the ts is the
LAST field — the first casualty of truncation; the bridge's trailing-flush path even parses a
fragment of a mid-write-dying client), and **`ts==0` DISABLES both ordering defenses** — the
stale check AND the floor are gated on `ts != 0 && lastPromptUnixMs != 0` (a ts-less UPS
likewise skips the `lastPromptUnixMs` stamp, leaving the floor measuring against a prompt an
hour old). Either corruption lets the duplicate fall through to `turnComplete` → WaitingForInput
→ the advance seam → send #2.

**Fix 1 — the bridge stamps ARRIVAL time on a ts-less parse (`HooksBridge.cpp`, both dispatch
sites).** Every current forwarder stamps ts unconditionally (FIRST, before any slow work), so a
missing ts at the bridge means a mangled delivery, not an old forwarder. Arrival can only
OVERSTATE the fire time, which errs toward reading a suspect event as stale/too-fast — the
self-healing direction (a genuinely-completed turn is settled by the scanner's exempt quiescent
Stop ~2.5 s later). Under the incident's replay, whichever field was corrupted now resolves to
stale/floored: state stays Running, no advance.

**Fix 2 — the MINIMUM SEND SPACING (`Scheduler.h kMinSendSpacingMs`, 20 s): never two AUTOMATIC
injections into one session closer than 20 s, measured from OUR OWN registry-clock stamps**
(`sentAtUnixMs` on Sent/Failed Autorun rows + `injectedAtUnixMs` on any Autorun row — NowMs at
the mark/deliver seams, so NO wire corruption can defeat it; the user-prescribed belt). Every
status-correction lane that catches a bogus at-rest state (the scanner's recon-run pass ~2.5 s,
the presence heartbeat ~2 s, the push/pull echo consume) needs only seconds — 20 s outlasts them
all, so a status race can cost a DELAYED send, never a DOUBLE send (in the incident's replay the
hold expires at ~11:12:24 with the session already re-lit Running by recon-run; #2 then rides
the REAL turn end). Deliberately NOT spaced: a Typed row (the human's own prompt — its running
turn already holds the advance via state), a rolled-back Pending row (send-deferred/reclaim —
provably nothing typed; spacing would only slow the legitimate retry), and the human's explicit
paths (Send-now, the SemiAuto Confirm click — they bypass `DecideAdvance`). The hold is a TIMED
None (`AdvancePlan::retryAfterMs`) — unlike every other hold it has no external re-trigger on a
quiet session, so the scheduler schedules a **deferred re-advance** (`_scheduleDeferredAdvance`
→ the worker's wait wakes at the earliest due and promotes it back into the queue): the plan
self-resumes, nothing polls. Placement: after the question-guard (a question-parked plan
schedules no pointless timers) and after the Pending scan (a drained plan still answers PlanDone
immediately); it gates SemiAuto ARMING too (the armed suggestion races status the same way).

**One deliberate behavior change:** "echoed lead → next pending sends immediately" is GONE —
the echo still releases the *pickup guard*, but the next send now waits out the spacing window.
That immediacy is exactly what §13 proved unsafe: between the echo (04.692) and send #2 (05.240)
there was NOTHING left to hold a corrupted status.

Coverage (`tests_spawn_sched.cpp`): the incident replay through the REAL pipe + parser + state
machine (`TestBridgeRoundTrip` — a real-ts UPS then a ts-less duplicate Stop 40 ms later must
stay Running; pre-fix it flips WaitingForInput), the arrival-stamp observable (a ts-less line
now advances the decay anchor), and the `DecideAdvance` spacing matrix (`TestScheduler`: the
2.6 s incident shape held + `retryAfterMs` exact, injection-stamp-only hold, Failed-lead hold,
rolled-back-Pending exempt, Typed exempt, future-stamp skew-safe, PlanDone precedence, SemiAuto
arming spaced, release at exactly 20 s; the pickup-release probes moved to the spacing boundary
with an unechoed contrast so echo-vs-clock stays pinned).

## 14. The INTERRUPT HOLD — Esc parks the autorunner until your next message (fixed)

**The report (2026-09-07):** *"This session was interrupted. Were I to send in Full mode some
queued messages I would have gotten them sent."* Pressing Esc mid-turn is the user taking the
wheel — they stop the agent because they have something to say — yet a Full autorunner treated
the interrupt's turn-end like any other turn-complete and delivered the next queued prompt into
exactly that session (and with a draft already in the box, the §9 swap stashed and restored the
user's half-written message around the send).

**Why nothing existing caught it.** Claude fires NO `Stop` hook on an interrupt. The PULL scanner
sees the `[Request interrupted by user…]` user line, flags `interrupted`, and once the transcript
is ≥2 s quiet synthesizes a quiescent Stop (recon-stop) that lands `WaitingForInput` with
`turnComplete`, so the advance seam fires. Every gate in `DecideAdvance` then reads the session as
READY — it is at rest, the box is empty, no delivery is in flight, the question-guard holds no
question — and the state machine cannot help: an interrupt is not an error and the session
genuinely IS ready for input. The missing fact is *who* should supply that input.

**The rule — three parts, one engine-owned fact (`AutorunnerState::interruptHeldMode` +
`interruptHeldUnixMs`, transient; the pure helpers beside it in `SessionModels.h`):**

1. **PARK.** When the scanner's parse ends on the interrupt marker (`_readDelta`: the marker was
   sighted in this batch AND `st.interrupted` survived to the loop's end — a batch of `[marker,
   the user's next prompt]` parks nothing, since that prompt already fed `NoteExternalPrompt`
   inside the loop and a hold taken afterwards would have nobody left to resume it), it calls
   `SessionRegistry::NoteInterrupt(id, markerLineTs)` — IN the parse, BEFORE the same pass's
   reconciliation can synthesize the Stop. `TakeInterruptHold`: a Semi/Full autorunner goes
   **Off** with the prior mode remembered and the marker's own transcript timestamp kept as the
   resume ANCHOR; an already-Off autorunner (the user's own Off, or an existing hold — a second Esc)
   is untouched. `DecideAdvance` then answers `autorunner off (interrupt hold - resumes on your next
   message)`, so the interrupt's Stop advances into nothing. Logged `[interrupt-hold] <sid8>
   autorunner Full -> Off (...)` on hooks.log + autorunner.log; the take notifies (the header
   toggle / overlay / board repaint).
2. **RESUME on the user's next message.** `ResumeInterruptHold` restores the held mode from the
   two places a real message lands: the push `UserPromptSubmit` record in `OnHookEvent` (the human's
   typed prompt — recorded as Typed — OR the echo of a prompt they **Send-now**'d: while held the
   mode is Off, so the only sends that can produce an echo are the user's own) and the pull twin
   `NoteExternalPrompt` (the transcript's user line, a deduped already-recorded line included — the
   hooked session's normal push-then-pull shape). Never on protocol noise (a teammate delivery is
   not the user talking — `IsNoiseUserPrompt`), never on an EMPTY UPS (the §10 phantom twins / the
   recon-run synth), and never on a message stamped BEFORE the anchor (the push side's wire `ts`,
   the pull side's line timestamp): a late-forwarded UPS of the very turn the user killed, or a
   replayed older transcript line, must not release it; an unstamped pull line (0) cannot prove it
   is the next message and never resumes (the push hook does on a hooked session). Logged
   `[interrupt-resume] <sid8> autorunner Off -> Full (your message arrived …)`.
3. **CANCEL on a user-set mode.** Whatever the user picks on the Auto-Testing header toggle or the
   overlay cycle — Off included — `ClearInterruptHold` runs inside the same registry write, so the
   pending resume is gone and the human's explicit choice stands until the NEXT interrupt arms the
   mechanism again (the `[nav] autorunner … -> X (cancels the interrupt hold)` suffix marks it).

**Precedence — a backstop pause always wins.** Every engine pause (`stop-on-error`,
`enter-retry-giveup`, `lost-send`, the push AND pull merge verdicts) clears the hold beside its
`mode = Off`, so a backstop pause is never silently undone by the next message; the merged-submit
verdict in particular runs BEFORE the resume check on the very message that drew it. stop-on-error
fires on a HELD session too (`mode != Off || InterruptHoldActive`), converting the hold into a
real, sticky pause. The restore seam (`_LaunchClaudeSession`'s reopened-session mode re-stamp)
clears it as well — the field is transient (never persisted), so a record loaded from disk never
carries one, and a this-run-archived record's in-memory remainder is voided explicitly.

**Belts.** The marker is sighted on every read that ends on it — a restored/adopted session's
history replay included — so `NoteInterrupt` refuses a marker older than `kInterruptHoldFreshMs`
(60 s; `InterruptMarkerIsFresh`, unstamped never) and the scanner gates the call on
`primedAtParse` like the CommandWatch feeds; live sessions only; the anchor makes every resume
strictly-after-the-marker. Codex is untouched (no injector, no autorunner, no `❯` marker).

**UI.** An interrupt-held Off is painted distinctly from a user's Off so it never reads as "someone
switched my autorunner off": the Auto-Testing header toggle says `Tests Autorunner: Off → Full on
your next message` with its hollow circle in the held mode's color, the per-tab overlay's row-1
button reads `Off → Full` in that color, the board card's ⚙ queue tooltip names the parked mode,
and every autorunner tooltip states the rule.

**Known edge (accepted):** a prompt the user TYPE-AHEAD'd before pressing Esc already fired its
UPS (at Enter time), so it cannot be the resuming message; the hold then lasts until the next real
message or the toggle. Coverage: `TestInterruptHold` (`tests_spawn_sched.cpp`) — the pure rules,
the registry park (fresh-marker belt, idempotent, live-only, notify), the interrupt's synthesized
Stop advancing into the hold, the push resume matrix (typed / Send-now echo / noise / empty /
pre-anchor), the pull twin (pre-anchor / unstamped / fresh / deduped), the user-set cancel, and the
stop-on-error + push/pull merge precedence.
