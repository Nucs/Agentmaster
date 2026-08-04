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

- **Expiry belt:** `kDeliveryGateTimeoutMs` (45 s ≈ 2× the swap's worst case). An expired gate
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

- The **question-guard opt-out** for a mail-queued own-draft (`kAnswersQuestionOk` is reachable
  only from tests today) — a moved "answer" can park until a manual Send-now.
- **Paste placeholders** (`[Pasted text #N +M lines]`) queued verbatim by the mail button lose the
  paste content when the queued copy is sent — expansion at queue time is the §8c/§9-consistent
  fix.
- The pending-input detector's **~120-row read window**: a draft taller than the window is
  invisible to the swap's protection entirely.
- A fuller **accepted/delivered log split** beyond the added `[delivered]` lines.
