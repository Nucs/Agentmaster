// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the Autorunner scheduler (DESIGN §10, HOOKS.md tryAdvance). It advances a
// session's Auto Testing when the session reaches turn-complete (a clean Stop ->
// WaitingForInput, surfaced via SessionRegistry's advance seam), honoring the correctness
// rules and backstops.
//
// The decision is a PURE function (DecideAdvance) so every branch is unit-testable without
// threads or wall-clock. The Scheduler wraps it with its own worker thread (so throttle
// sleeps never block the hooks bridge), the inject, and the global pause/backstops.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SessionModels.h"

namespace Agentmaster
{
    class SessionRegistry;

    // Per-item opt-out of the question-guard: a queued prompt whose guardPattern equals this
    // is allowed to fire even when the agent's last message was a question.
    inline constexpr std::wstring_view kAnswersQuestionOk = L"answers-a-question:ok";

    // After injecting a prompt the session lingers Idle/WaitingForInput for a beat until
    // Claude's UserPromptSubmit moves it to Running. DecideAdvance refuses to fire another
    // prompt while a Flight prompt is Sent-but-not-yet-acknowledged — so a change-driven
    // advance (the observer trigger that lets idle plans START) can't drain the whole queue.
    //
    // Agentmaster (DELIVERY.md §9): the guard is EVIDENCE-released, no longer a naked 4s window.
    // The old expiry meant "a lost echo shouldn't stall the plan", but 4s is routinely beaten by
    // real forwarder latency — the 07:27 incident's #6 fired 9s after #5 exactly because the
    // guard had lapsed while the delivery was still being accepted, stacking the next prompt on
    // top. Now the guard holds until the TURN visibly started — the echo consumed (`echoed`, fed
    // by the push hook OR the scanner's pull consume, DELIVERY_PLAN.md R1/R2), or a newer
    // prompt-carrying UserPromptSubmit stamped turns.lastPromptUnixMs past the send (an echo the
    // fold couldn't match still proves the pickup; an EMPTY phantom twin no longer stamps — R3).
    // The former raw transcript-advance release is GONE (R2): a still-running PREVIOUS turn also
    // writes the transcript, so "the file grew past sentAt" never proved OUR prompt started — it
    // is exactly how the watchdog drained instead of rescuing the lost `#6`. This cap is only the
    // lost-evidence belt: it sits ABOVE the Enter-retry watchdog's give-up horizon (~21s: 3s + 6s
    // + 6s presses + the poll), which resolves a truly-dead send to Failed (and pauses the
    // autorunner) long before the cap — so in practice the cap only ever releases a session whose
    // watchdog could not run (no injector mid-rebind, or draft-guarded presses; the lost-send
    // verdict below then owns the terminal resolution).
    inline constexpr int64_t kPickupGuardMaxMs = 30000;

    // Agentmaster (the ea1dc3dd double-send, 2026-08-15 — MINIMUM SEND SPACING): never two AUTOMATIC
    // injections into one session closer than this, measured from OUR OWN registry-clock stamps
    // (QueuedPrompt::sentAtUnixMs / injectedAtUnixMs, stamped NowMs at the mark/deliver seams) —
    // deliberately NEVER from any wire-derived time. The incident this closes: prompt #1 delivered
    // 11:12:04.159 + its UserPromptSubmit echo consumed 04.692 (pickup guard legitimately released —
    // the turn HAD started), then a DUPLICATE delivery of the ALREADY-PROCESSED previous Stop arrived
    // 04.729 with its trailing wire ts lost to truncation — and ts==0 DISABLES both of
    // NextSessionStateOrdered's ordering defenses (the stale check AND the kMinRealTurnSpanMs floor,
    // HookEvents.h — both are gated on `ts != 0 && lastPromptUnixMs != 0`), so a 37ms "turn-complete"
    // flipped the state WaitingForInput and the advance delivered prompt #2 into the RUNNING turn
    // 2.6s after #1 (claude queued it as type-ahead, the user's box-clear then orphaned it,
    // enter-retry gave up, the plan derailed). The HooksBridge now stamps ARRIVAL time on a ts==0
    // parse (the root fix), and THIS spacing is the wire-independent belt: every status-correction
    // lane that would catch a bogus at-rest state (the scanner's recon-run pass ~2.5s cadence, the
    // presence heartbeat ~2s, the push/pull echo consume) needs only seconds, so 20s of spacing
    // outlasts them all — a status race can cost a DELAYED send now, never a DOUBLE send. The hold
    // is TIMED (AdvancePlan::retryAfterMs → the scheduler's deferred re-advance), so a spaced plan
    // self-resumes with no external notify needed.
    inline constexpr int64_t kMinSendSpacingMs = 20000;

    // Enter-retry (the "the TUI ate my Enter" backstop). The ConPTY can deliver an injected
    // `prompt + CR` faster than Claude's Ink UI initializes its input handler, so the submit Enter
    // is absorbed as a NEWLINE instead of sending — the prompt sits typed-but-not-submitted and the
    // turn never starts (no UserPromptSubmit, no transcript write, state stuck Idle/WaitingForInput).
    // The scheduler watches every just-sent Flight prompt; if the turn has not started within
    // kEnterRetryFirstMs (then kEnterRetryIntervalMs for later presses) it re-presses a LONE Enter
    // (never the text again — that would duplicate it), up to kEnterRetryMax extra presses, then gives
    // up: the prompt is marked Failed and the session's autorunner is PAUSED (it never landed — don't
    // strand a phantom Sent nor advance past a broken step; the user Send-nows / re-arms). kEnterRetry-
    // PollMs is how often the worker re-checks while a send awaits pickup. (The former
    // kEnterRetryActivityMarginMs transcript-advance "it started" signal is GONE — DELIVERY_PLAN.md
    // R2: a still-running PREVIOUS turn also advances the transcript, so it drained the watch in
    // exactly the shape that needed rescuing; started-ness now keys on `echoed` — push or pull —
    // and on the state leaving the ready set.)
    inline constexpr int64_t kEnterRetryFirstMs = 3000; // first re-press fires fast — rescue the common eaten-CR case without a long stall
    inline constexpr int64_t kEnterRetryIntervalMs = 6000; // subsequent re-presses: slower (the turn may be genuinely starting)
    inline constexpr uint32_t kEnterRetryMax = 3; // never re-press Enter more than this many times
    inline constexpr int64_t kEnterRetryPollMs = 1000; // worker re-check cadence while a send awaits pickup

    // Agentmaster (DELIVERY_PLAN.md R1 — the LOST-SEND verdict's settle): a Sent Autorun prompt with
    // NO echo (push hook or the scanner's pull consume) this long after its send, on a session AT
    // REST with the delivery gate closed, provably never became a message — the transcript would
    // have shown its user line by now and the pull consume would have marked it. >= the push echo
    // window (SessionRegistry's kEchoWindowMs, 15s), so a slow hook echo can never be beaten to the
    // verdict; and because every Enter-retry press REFRESHES sentAtUnixMs, the verdict structurally
    // waits out an active watchdog ladder (it can only fire once the presses stopped — gave up
    // [already Failed, verdict moot], draft-blocked, or uncontrollable). The recorded gap this
    // closes (DELIVERY.md §9/§10): prompt `#6` pasted into a running turn was consumed by an
    // AskUserQuestion dialog's rendering, never became a message, and sat terminally `Sent` —
    // indistinguishable from a success until a human diffed the transcript.
    inline constexpr int64_t kLostSendSettleMs = 15000;

    enum class AdvanceAction
    {
        None, // nothing to do (see reason)
        Send, // auto-send queue[promptIndex] (Full)
        Hold, // LEGACY — no longer produced. A pending question / "needs you" state now leaves the
              // prompt Pending (treated like a Running mid-turn: stay queued, wait for a status
              // change), NOT parked in a separate Held status. Kept only so _process can still
              // rehabilitate Held prompts persisted by older builds. See the question-guard below.
        AwaitConfirm, // SemiAuto: arm queue[promptIndex] for one-click confirm
        PlanDone, // no Pending prompts remain
    };

    struct AdvancePlan
    {
        AdvanceAction action{ AdvanceAction::None };
        size_t promptIndex{ 0 };
        std::wstring reason;
        // Non-zero ONLY when action==None is a TIMED hold (today: the minimum send spacing) — how
        // long until the hold self-releases. The scheduler schedules a deferred re-advance for it:
        // unlike every other None reason, a spacing hold has NO external re-trigger of its own (a
        // quiet session may see no registry notify for minutes — OnObserved fires on changes only).
        int64_t retryAfterMs{ 0 };
    };

    // PURE decision (DESIGN §10 "the loop" + the three-states table). Inputs are explicit so
    // tests pass deterministic values:
    //   nowUnixMs            — current time
    //   lastHumanInputUnixMs — when the human last typed into this session (0 if never)
    //   globalPause          — the scheduler's global Pause-all backstop
    // Encodes, in order: global pause; mode Off; not-ready (only Idle or WaitingForInput are
    // ready — see below); pause-on-human-input; maxAutoSends; awaiting-injection-pickup guard;
    // (no Pending => PlanDone); Manual-gate skip; question-guard (None — the prompt stays queued
    // and waits, exactly like a Running mid-turn); minimum SEND SPACING (kMinSendSpacingMs — a
    // TIMED hold, retryAfterMs set); SemiAuto => AwaitConfirm; Full => Send.
    inline AdvancePlan DecideAdvance(const SessionInfo& s,
                                     int64_t nowUnixMs,
                                     int64_t lastHumanInputUnixMs,
                                     bool globalPause)
    {
        AdvancePlan plan;

        if (globalPause)
        {
            plan.reason = L"global pause";
            return plan;
        }
        if (s.autorunner.mode == AutorunnerMode::Off)
        {
            plan.reason = L"autorunner off";
            return plan;
        }
        // Ready for a prompt = turn-complete (a clean Stop -> WaitingForInput) OR sitting Idle
        // with no turn in progress (freshly launched / just resumed). An Idle session never
        // emits a Stop, so gating on WaitingForInput alone would leave a queued plan + autorunner
        // waiting forever; allowing Idle lets the plan START. Running / NeedsApproval / Error /
        // Done are NOT ready (mid-turn, awaiting you, or ended).
        if (s.state != SessionState::WaitingForInput && s.state != SessionState::Idle)
        {
            plan.reason = L"not ready (mid-turn / needs-approval / ended)";
            return plan;
        }
        if (s.autorunner.pauseOnHumanInput &&
            lastHumanInputUnixMs != 0 &&
            (nowUnixMs - lastHumanInputUnixMs) >= 0 &&
            (nowUnixMs - lastHumanInputUnixMs) < 1500)
        {
            plan.reason = L"human typing";
            return plan;
        }
        if (s.autorunner.autoSendsThisRun >= s.autorunner.maxAutoSends)
        {
            plan.reason = L"maxAutoSends reached";
            return plan;
        }

        // Agentmaster (DELIVERY.md): a delivery (or the mail-button box-clear) currently OWNS this
        // session's input box — the hosting window is mid draft-swap. Advancing now could only mark
        // a prompt Sent to be synchronously declined and rolled back (the recorded 07:27 livelock:
        // 8 mark/decline/rollback cycles at ~750ms while ONE swap legitimately held the box for
        // 24s — the echo-keyed pickup guard below cannot see a delivery, only its echo). HOLD; the
        // gate's CLOSE notifies, which re-requests this advance, so nothing polls. The expiry belt
        // (kDeliveryGateTimeoutMs, checked inside DeliveryGateOpen) bounds a leaked gate, so this
        // can never stall a plan permanently.
        if (DeliveryGateOpen(s, nowUnixMs))
        {
            plan.reason = L"delivery in flight";
            return plan;
        }

        // Agentmaster (DELIVERY_PLAN.md R5 — the tri-state box read): the scan lane (or a send's
        // pre-flight decline) last saw NO parseable input box on this session's screen. A MENU means
        // a paste now would feed the MENU (the §9 dialog that silently consumed a delivered prompt);
        // a bare NoBox means an unreadable/replaced box (the §11 invisible-content merge lived
        // exactly there). HOLD — the scan re-reads every liveness tick and SetPendingBoxState
        // notifies on the blocked→unblocked release, so the held advance re-fires the moment a box
        // is visible again; a NoBox that persists with queued work escalates loudly instead
        // (ShouldWarnOnBoxNotVisible below — a warning since §12, never a mode change). Unknown (no read yet — a dormant tab) never holds.
        if (s.pendingBoxState == InputBoxState::MenuOpen)
        {
            plan.reason = L"input box not visible (a menu/dialog is open)";
            return plan;
        }
        if (s.pendingBoxState == InputBoxState::NoBox)
        {
            plan.reason = L"input box not visible";
            return plan;
        }

        // Awaiting-injection-pickup guard: if a Flight prompt we just injected is Sent but the
        // turn it should start has not visibly begun, hold off — otherwise an advance driven by
        // an observed change in that window would send the NEXT prompt on top of it (stacking two
        // messages into one box). EVIDENCE-released (DELIVERY.md §9), not time-expired: the turn
        // started when the echo was consumed (`echoed` — the push hook OR the scanner's pull
        // consume, DELIVERY_PLAN.md R1), or a newer prompt-carrying UserPromptSubmit stamped
        // turns.lastPromptUnixMs past the send (an echo the fold couldn't match still proves the
        // pickup; empty phantom twins no longer stamp — R3). The former transcript-advance release
        // is GONE (R2): a still-running PREVIOUS turn also writes the file, so raw growth past
        // sentAt proved nothing about OUR prompt — it is the clause that released the guard while
        // `#6` was being consumed by a dialog. kPickupGuardMaxMs is the lost-evidence belt only;
        // the Enter-retry watchdog resolves a truly-dead send to Failed well before it, and the
        // lost-send verdict (DecideLostSend) owns the case the watchdog can't reach. Ignores
        // un-timestamped Sent prompts (a restored plan) via sentAtUnixMs != 0; a restored Sent
        // prompt also loads echoed=true (Persistence), so it can never re-arm this guard.
        for (const auto& q : s.queue)
        {
            if (q.origin != PromptOrigin::Autorun || q.status != PromptStatus::Sent || q.echoed || q.sentAtUnixMs == 0)
            {
                continue;
            }
            const int64_t sentAge = nowUnixMs - q.sentAtUnixMs;
            if (sentAge < 0 || sentAge >= kPickupGuardMaxMs)
            {
                continue;
            }
            const bool turnStarted = s.turns.lastPromptUnixMs > q.sentAtUnixMs;
            if (!turnStarted)
            {
                plan.reason = L"awaiting injection pickup";
                return plan;
            }
        }

        // First Pending prompt, by queue order. (`dependsOn` / `delayMs` / `maxAttempts` are
        // data-model-only today — carried, persisted, but NOT evaluated here; the Auto-Testing UI
        // queues everything at the OnTurnComplete default. Do not describe them as enforced.)
        bool found = false;
        size_t idx = 0;
        for (size_t i = 0; i < s.queue.size(); ++i)
        {
            if (s.queue[i].status == PromptStatus::Pending)
            {
                idx = i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            plan.action = AdvanceAction::PlanDone;
            plan.reason = L"queue drained";
            return plan;
        }

        const auto& item = s.queue[idx];
        plan.promptIndex = idx;

        if (item.gate == PromptGate::Manual)
        {
            plan.reason = L"manual gate";
            return plan; // None: only "Send now" fires a Manual item
        }

        // Question-guard: the agent ended its turn asking the user something (a clarifying
        // question / a "needs you" state). Treat it EXACTLY like a mid-turn Running state for the
        // queue — leave the next prompt PENDING and wait for the next status change (a later,
        // non-question turn-complete) to fire it. We deliberately do NOT react by parking it in a
        // separate Held status: a "needs you" status must not be reacted to beyond how Running is
        // (the message just stays queued), and a Held prompt could otherwise strand — OnObserved's
        // idle-start re-trigger only watches Pending prompts. A per-prompt opt-out (guardPattern
        // == kAnswersQuestionOk) marks a prompt that is itself the answer, so it still fires through.
        const bool overridesQuestionGuard = (item.guardPattern == kAnswersQuestionOk);
        if (s.lastMessageWasQuestion && !overridesQuestionGuard)
        {
            plan.reason = L"question pending — stay queued and wait (treated like running)";
            return plan; // None: the prompt stays Pending; a later non-question turn-complete fires it
        }

        // MINIMUM SEND SPACING (kMinSendSpacingMs above — the ea1dc3dd double-send): never fire the
        // next automatic injection within the spacing window of the previous one, measured from OUR
        // OWN stamps so no wire-ts corruption can defeat it. What counts as "the previous send":
        //   * injectedAtUnixMs on ANY Autorun row — real keystrokes demonstrably reached the ConPTY
        //     then (a wedged dispatcher can inject long after the mark — DELIVERY.md §12 measured
        //     83s — so the LATEST evidence wins);
        //   * sentAtUnixMs on a Sent or Failed Autorun row — an accepted send whose injection may
        //     still be materializing (Sent), or one that failed AFTER real Enter presses went in
        //     (Failed: the enter-retry ladder refreshes sentAtUnixMs per press).
        // Deliberately NOT counted: a Typed row (the human's own prompt never spaces OUR queue — its
        // running turn already holds the advance via state), and a rolled-back Pending row (the
        // send-deferred / undelivered-reclaim paths keep the stale stamp but provably typed NOTHING —
        // spacing it would just slow the legitimate retry). A negative gap (clock skew / a future
        // stamp) reads as expired, the pickup guard's idiom — never a permanent hold. Sits AFTER the
        // question-guard so a question-parked plan doesn't schedule pointless timed re-checks, and
        // AFTER the Pending scan so a drained plan still answers PlanDone immediately; it gates the
        // SemiAuto arming too (the armed suggestion races status exactly like an auto-send would).
        // The human's explicit paths (Send-now, a SemiAuto Confirm click) bypass DecideAdvance
        // entirely and are deliberately NOT spaced.
        int64_t lastSendMs = 0;
        for (const auto& q : s.queue)
        {
            if (q.origin != PromptOrigin::Autorun)
            {
                continue;
            }
            if (q.injectedAtUnixMs > lastSendMs)
            {
                lastSendMs = q.injectedAtUnixMs;
            }
            if ((q.status == PromptStatus::Sent || q.status == PromptStatus::Failed) && q.sentAtUnixMs > lastSendMs)
            {
                lastSendMs = q.sentAtUnixMs;
            }
        }
        if (lastSendMs != 0)
        {
            const int64_t sinceSend = nowUnixMs - lastSendMs;
            if (sinceSend >= 0 && sinceSend < kMinSendSpacingMs)
            {
                // Fixed literal (no countdown in the text) so _noteAdvanceSkip's change-dedup works.
                plan.reason = L"send spacing (a prompt was sent/injected moments ago)";
                plan.retryAfterMs = kMinSendSpacingMs - sinceSend;
                return plan;
            }
        }

        if (s.autorunner.mode == AutorunnerMode::SemiAuto)
        {
            plan.action = AdvanceAction::AwaitConfirm;
            plan.reason = L"semi-auto: awaiting confirm";
            return plan;
        }

        plan.action = AdvanceAction::Send;
        plan.reason = L"full: auto-send";
        return plan;
    }

    enum class EnterRetryAction
    {
        None, // nothing to watch — the turn started, or no Flight prompt awaits pickup (stop watching)
        Waiting, // a send awaits pickup but the retry interval hasn't elapsed yet (keep watching)
        Retry, // re-press Enter now (the turn still hasn't started after kEnterRetryIntervalMs)
        GiveUp, // exhausted kEnterRetryMax presses — stop watching (and log)
    };

    struct EnterRetryPlan
    {
        EnterRetryAction action{ EnterRetryAction::None };
        std::wstring promptId; // the watched prompt (valid for Waiting / Retry / GiveUp)
        uint32_t attempt{ 0 }; // how many Enter re-presses have already been made for it
    };

    // PURE decision (the DecideAdvance pattern): given a session snapshot + now, should the scheduler
    // re-press Enter for a Flight prompt whose submit Enter the TUI may have eaten? See the
    // kEnterRetry* constants above. The turn is considered STARTED — so no retry, stop watching — when
    // ANY of: the prompt's UserPromptSubmit echo arrived (`echoed` — the push hook, OR the scanner's
    // PULL consume when the transcript shows the message, DELIVERY_PLAN.md R1); or the session left
    // the ready set (state advanced past Idle/WaitingForInput, e.g. Running — covers a hook-less
    // adopted session driven by the transcript tail, and doubles as the mid-turn SAFETY gate: never
    // press into a running turn). The former transcript-advance drain is GONE (R2): a still-running
    // PREVIOUS turn also advances `convLastActivityUnixMs`, so it drained the watch in exactly the
    // shape that needed the watchdog — a prompt delivered into (and consumed by) another turn.
    // Otherwise the most-recently-sent un-acknowledged Flight prompt is watched: Waiting until
    // kEnterRetryIntervalMs elapses, then Retry (until kEnterRetryMax presses), then GiveUp.
    //
    // DRAFT GUARD (R2 — mandatory, proven on the live log): before pressing, the box's observed
    // unsent draft (s.pendingInput, the PENDING_INPUT.md monitor) is compared to the watched
    // prompt's text. A box holding OUR prompt (fold-matched) is the eaten-CR shape — press, that IS
    // the rescue. A box holding ANYTHING ELSE is (or may be) the human's draft — a lone Enter would
    // SUBMIT it, the exact RC3 merge: in the recorded incident the box held the user's 17-char
    // draft at 08:50:07 while the lost `#6` sat Sent-unechoed, and without this guard the re-armed
    // watch would have pressed right there. Answer Waiting (keep watching, never press); the
    // draft-blocked prompt is then resolved by the lost-send verdict (DecideLostSend below) once
    // settled. An EMPTY pendingInput allows the press: the pending scan's eager-show records a
    // draft within ~one tick, and a press into a truly empty box is a no-op for claude — the
    // narrow race (a draft begun sub-tick before the press) is bounded by that no-op window.
    //
    // `controllable` == "we hold a bound stdin injector for this session, so a re-pressed Enter can
    // actually reach it". The caller passes SessionRegistry::HasInjector(s.id). This is CONTROLLABILITY
    // (do we have stdin?), NOT provenance (s.external = did we launch it): an ADOPTED session — typed
    // into a `+` tab, external=true — is bound an injector on adoption and IS drivable, so it must NOT
    // be excluded the way an observe-only external (no injector) is. Gating on s.external here used to
    // wrongly skip adopted sessions (Agentmaster: the autorunner-on-adopted bug).
    inline EnterRetryPlan DecideEnterRetry(const SessionInfo& s, int64_t nowUnixMs, bool controllable = true)
    {
        EnterRetryPlan plan;
        if (!s.live || !controllable)
        {
            return plan; // observe-only (no injector) / archived — no bound stdin to re-press Enter on
        }
        if (s.state != SessionState::WaitingForInput && s.state != SessionState::Idle)
        {
            return plan; // the turn started (Running / NeedsApproval / Error / Done) — nothing to retry
        }
        // Agentmaster (DELIVERY.md RC3): never press into a DORMANT session — a window-restored
        // background tab is live + injector-bound but its claude has not launched (WT starts the
        // child lazily on first show), so a pre-Connected WriteInput silently drops: the presses
        // could only burn the retry budget and end in a spurious Failed + autorunner-paused ~15s
        // after reopen (the recorded restart press-storms). `started` is meaningless for an
        // external (adopted) session — the same carve-out OnObserved's idle-start trigger uses.
        if (!s.started && !s.external)
        {
            return plan;
        }
        // Agentmaster (DELIVERY.md RC3): a delivery/clear OWNS the box right now — a lone Enter
        // injected mid-swap would submit whatever the box holds (the user's partially-cleared
        // draft on the ladder, or the just-restored draft), the exact merge the swap exists to
        // prevent. Drop the watch; the gate's close notifies OnObserved, which re-arms it if the
        // send is still unacknowledged then.
        if (DeliveryGateOpen(s, nowUnixMs))
        {
            return plan;
        }
        // The most-recently-sent Flight prompt still awaiting its pickup (Sent, not echoed — push
        // OR pull evidence both mark `echoed` now, DELIVERY_PLAN.md R1/R2). A later send supersedes
        // an earlier one. (The raw transcript-advance drain that used to sit here is gone — R2.)
        const QueuedPrompt* best = nullptr;
        for (const auto& p : s.queue)
        {
            if (p.origin != PromptOrigin::Autorun || p.status != PromptStatus::Sent || p.echoed || p.sentAtUnixMs == 0)
            {
                continue;
            }
            if (!best || p.sentAtUnixMs > best->sentAtUnixMs)
            {
                best = &p;
            }
        }
        if (!best)
        {
            return plan; // no un-acknowledged Flight send — stop watching
        }
        plan.promptId = best->id;
        plan.attempt = best->enterRetries;
        if (best->enterRetries >= kEnterRetryMax)
        {
            plan.action = EnterRetryAction::GiveUp;
            return plan;
        }
        // DRAFT GUARD (R2, see the contract above): a box observed holding text that is NOT our
        // watched prompt is never pressed into — a lone Enter would submit the human's draft (the
        // RC3 merge). Keep watching instead; the lost-send verdict owns the terminal resolution.
        if (!s.pendingInput.empty() && !DraftMatchesPromptText(s.pendingInput, best->text))
        {
            plan.action = EnterRetryAction::Waiting;
            return plan;
        }
        // BOX-STATE GUARD (DELIVERY_PLAN.md R5/R8): the scan last saw NO parseable input box — a
        // MENU (an AskUserQuestion / permission list, where a lone Enter SELECTS the highlighted
        // option: an answer fabricated by the rescue mechanism) or a bare NoBox (nothing readable
        // to press into). Never press; keep watching — the lost-send verdict / the give-up ladder
        // own the terminal resolution, and the scan clears the state the moment a box re-renders.
        // Unknown (no read yet) deliberately passes: a dormant tab is already excluded above, and
        // an un-scanned live box must not strand the eaten-CR rescue.
        if (s.pendingBoxState == InputBoxState::NoBox || s.pendingBoxState == InputBoxState::MenuOpen)
        {
            plan.action = EnterRetryAction::Waiting;
            return plan;
        }
        // First re-press fires after kEnterRetryFirstMs (snappy rescue); later presses space out by
        // kEnterRetryIntervalMs. sentAtUnixMs is refreshed on each press, so this is "since last press".
        const int64_t due = (best->enterRetries == 0) ? kEnterRetryFirstMs : kEnterRetryIntervalMs;
        if ((nowUnixMs - best->sentAtUnixMs) >= due)
        {
            plan.action = EnterRetryAction::Retry;
            return plan;
        }
        plan.action = EnterRetryAction::Waiting;
        return plan;
    }

    // Agentmaster (DELIVERY_PLAN.md R1 — the LOST-SEND verdict; tightened by DELIVERY.md §12). PURE
    // (the DecideAdvance pattern): which of this session's Sent Autorun prompts are provably LOST —
    // delivered to the terminal yet never became a message? The recorded gap (DELIVERY.md §9/§10): a
    // prompt pasted into a running turn was consumed by an AskUserQuestion dialog's rendering; its
    // queue row read `Sent`, indistinguishable from a success, forever. A prompt is LOST when ALL hold:
    //   * Sent + UNECHOED (`echoed` is THE became-a-message fact, fed by the push hook echo OR the
    //     scanner's pull consume — NoteExternalPrompt marking a fold-matched transcript user line)
    //     with a real send stamp (sentAtUnixMs != 0; a restored plan's Sent rows load echoed=true);
    //   * INJECTED (injectedAtUnixMs != 0 — the §12 conjunct): the prompt's bytes demonstrably
    //     reached the ConPTY (the [delivered] seams stamp it). WITHOUT this the verdict fired on a
    //     delivery still QUEUED behind a backlogged UI dispatcher — measured live: mark-Sent
    //     20:40:45, gate expired at 75s, the verdict fired at 76s, and the SAME prompt then
    //     delivered `verified=exact` at 83s and became a real message (recorded as a duplicate
    //     Typed row, the flight row stranded Failed, the autorunner robbed to Off). A Sent-but-
    //     never-injected prompt is DecideUndeliveredReclaim's case below (retry, not a fault);
    //   * the session is AT REST (Idle / WaitingForInput) — never judged mid-turn: while a turn is
    //     in flight our text may still be queued type-ahead the next turn will consume;
    //   * kLostSendSettleMs elapsed since BOTH the send and the injection — past the push echo
    //     window, past multiple scanner passes (so the pull consume had every chance), and —
    //     because each Enter-retry press REFRESHES sentAtUnixMs — structurally AFTER the watchdog's
    //     ladder went quiet (an actively-pressing watchdog resets this clock, so the two recoveries
    //     never race: the rescue always gets to finish; a rescued prompt echoes and leaves this set);
    //   * the delivery gate is CLOSED (never race an in-flight swap, whose box hold is legitimate).
    // NO "a turn ran past the send" conjunct, deliberately (a deviation from the first plan draft):
    // (a) a watchdog press refreshing sentAtUnixMs made `lastPromptUnixMs > sentAt` PERMANENTLY
    // false afterward, stranding the draft-blocked corner Sent forever — the very gap R1 closes;
    // (b) it never guarded the false-positive it appeared to (a fold-miss echo produces turn
    // evidence too — the echo conjunct is the real protection); (c) eaten-vs-lost disambiguation is
    // already serialized by the press refresh above. The caller (the SessionScanner's reconcile
    // pass — the transcript-truth lane, its cursor caught up by construction) marks each returned
    // prompt Failed and PAUSES the session's autorunner (mode -> Off), mirroring the Enter-retry
    // give-up: an INJECTED prompt that vanished is a real unaccounted-for transmission — the queue
    // past it is suspect, and silently continuing is how the loss went unnoticed until a human
    // diffed the transcript. NEVER auto-resend — the text may sit in the TUI's type-ahead or box,
    // and a resend can double it; the user Send-nows / re-arms.
    inline std::vector<std::wstring> DecideLostSend(const SessionInfo& s, int64_t nowUnixMs)
    {
        std::vector<std::wstring> lost;
        if (!s.live)
        {
            return lost; // an archived record has no live terminal to have lost a send into
        }
        if (s.state != SessionState::WaitingForInput && s.state != SessionState::Idle)
        {
            return lost; // mid-turn / needs-you / ended — never judge while a turn may yet consume it
        }
        if (DeliveryGateOpen(s, nowUnixMs))
        {
            return lost; // a delivery/clear owns the box right now — judge after it resolves
        }
        for (const auto& p : s.queue)
        {
            if (p.origin != PromptOrigin::Autorun || p.status != PromptStatus::Sent || p.echoed || p.sentAtUnixMs == 0)
            {
                continue;
            }
            if (p.injectedAtUnixMs == 0)
            {
                continue; // never injected — DecideUndeliveredReclaim's case, not a loss
            }
            const int64_t sentAge = nowUnixMs - p.sentAtUnixMs;
            const int64_t injectedAge = nowUnixMs - p.injectedAtUnixMs;
            if (sentAge >= kLostSendSettleMs && injectedAge >= kLostSendSettleMs)
            {
                lost.push_back(p.id);
            }
        }
        return lost;
    }

    // Agentmaster (DELIVERY.md §12 — the UNDELIVERED-SEND reclaim, DecideLostSend's retryable
    // sibling). PURE: which of this session's Sent Autorun prompts were ACCEPTED for delivery but
    // provably NEVER INJECTED (injectedAtUnixMs == 0) and whose delivery claim has lapsed (the gate
    // is closed — resolved, expired, or reclaimed)? Nothing was ever typed into the terminal for
    // these, so rolling them back to Pending and letting the advance RE-SEND is unconditionally
    // safe — no text exists that a resend could double, which is exactly the hazard that makes the
    // LOST verdict terminal. This is the confirm-and-retry half the live incident demanded: a
    // delivery that sat queued behind a wedged dispatcher past the gate expiry used to be judged
    // LOST (Failed + autorunner paused) one second after the expiry; now it is reclaimed and
    // retried, and the stale delivery itself aborts at its top guard (the submitNonce gate tag).
    // Same settle as the lost verdict (kLostSendSettleMs since the send) so an in-flight-but-slow
    // accept is never snatched back mid-marshal, and the gate-closed conjunct keeps it ordered
    // AFTER the expiry belt (a live delivery refreshes/holds its gate; only a lapsed one reclaims).
    // Deliberately NO at-rest conjunct: with nothing ever typed, the rollback is state-independent
    // — and reclaiming promptly is what disarms a zombie delivery before it can fire mid-turn.
    // The caller logs [send-reclaim] and rolls back via RollbackPromptToPending (refund=false: the
    // retry legitimately spends a fresh auto-send slot).
    inline std::vector<std::wstring> DecideUndeliveredReclaim(const SessionInfo& s, int64_t nowUnixMs)
    {
        std::vector<std::wstring> undelivered;
        if (!s.live)
        {
            return undelivered;
        }
        if (DeliveryGateOpen(s, nowUnixMs))
        {
            return undelivered; // the delivery still owns its claim — it is alive; judge after it lapses
        }
        for (const auto& p : s.queue)
        {
            if (p.origin != PromptOrigin::Autorun || p.status != PromptStatus::Sent || p.echoed ||
                p.sentAtUnixMs == 0 || p.injectedAtUnixMs != 0)
            {
                continue;
            }
            const int64_t sentAge = nowUnixMs - p.sentAtUnixMs;
            if (sentAge >= kLostSendSettleMs)
            {
                undelivered.push_back(p.id);
            }
        }
        return undelivered;
    }

    // Agentmaster (DELIVERY_PLAN.md R5 — the box-not-visible escalation; DEFANGED by DELIVERY.md
    // §12 into a WARNING): a NoBox that persists is either a detector/render drift or a modal
    // parked over the box, and DecideAdvance's hold on it would otherwise park a queued plan
    // silently forever (the §8 question-guard lesson: an invisible park is a bug report). Once
    // NoBox has stood this long on a LIVE, at-rest session with an ACTIVE autorunner and QUEUED
    // work, the scanner WARNS loudly (once per NoBox episode — ScanState-deduped) — it no longer
    // pauses the autorunner. The R5 cut flipped mode → Off here, and the live incident showed why
    // that is wrong: the NoBox clock had been running from BEFORE the user's explicit Off→Full
    // re-arm, so 48s after a human said GO the escalation overrode them — and a NoBox verdict is
    // not always a fault (a parked non-numbered menu reads NoBox too: the detector's MenuOpen
    // verdict keys on the numbered-option row shape, so the rewind/slash-style menus fall through
    // to NoBox — the same "legitimately parks for hours" class MenuOpen was always exempt for).
    // The HOLD (DecideAdvance) already stops any send while NoBox stands, and SetPendingBoxState's
    // blocked→unblocked release notifies, so the plan self-resumes the moment a box renders — the
    // user's mode setting is never robbed for a state that heals itself. MenuOpen deliberately
    // never even warns: a menu legitimately parks for hours (an unanswered AskUserQuestion is the
    // question-guard's domain, not a fault). PURE.
    inline constexpr int64_t kBoxNotVisibleEscalateMs = 60'000;
    inline bool ShouldWarnOnBoxNotVisible(const SessionInfo& s, int64_t nowUnixMs)
    {
        if (!s.live || s.autorunner.mode == AutorunnerMode::Off)
        {
            return false;
        }
        if (s.state != SessionState::WaitingForInput && s.state != SessionState::Idle)
        {
            return false; // only an at-rest session consults the box state (the advisory contract)
        }
        if (s.pendingBoxState != InputBoxState::NoBox || s.pendingBoxStateUnixMs == 0)
        {
            return false;
        }
        if ((nowUnixMs - s.pendingBoxStateUnixMs) < kBoxNotVisibleEscalateMs)
        {
            return false;
        }
        for (const auto& p : s.queue)
        {
            if (p.status == PromptStatus::Pending)
            {
                return true; // queued work is being silently held — surface it
            }
        }
        return false;
    }

    class Scheduler
    {
    public:
        explicit Scheduler(std::shared_ptr<SessionRegistry> registry);
        ~Scheduler();

        Scheduler(const Scheduler&) = delete;
        Scheduler& operator=(const Scheduler&) = delete;

        void Start();
        void Stop() noexcept;

        // Enqueue a turn-complete advance (wired to SessionRegistry::SetAdvanceHandler).
        // Returns immediately; the work runs on the scheduler thread.
        void RequestAdvance(const std::wstring& sessionId);

        // Backstop observer (wired via SessionRegistry::AddObserver): pauses a session's plan
        // when its turn ends in Error and stopOnError is set.
        void OnObserved(const SessionInfo& s);

        // SemiAuto one-click confirm (from the UI): send the armed prompt (confirm=true) or
        // skip it (confirm=false), then clear the pending-confirm flag.
        void Confirm(const std::wstring& sessionId, bool confirm);

        void SetGlobalPause(bool paused) { _globalPause.store(paused); }
        bool GlobalPaused() const { return _globalPause.load(); }

    private:
        void _worker() noexcept;
        void _process(const std::wstring& id);
        // Re-press Enter for any watched session whose just-sent prompt the TUI never submitted
        // (DecideEnterRetry). Runs on the worker thread each poll tick; drains _pending as sessions
        // start their turn / give up. Does its registry I/O OUTSIDE _mtx.
        void _sweepPendingPickups();
        // Agentmaster (no-silent-stall): a DecideAdvance that answers None used to leave NO trace, so
        // a queue held by the question-guard / global pause / maxAutoSends / a manual gate was
        // indistinguishable in autorunner.log from a dead scheduler (the "stuck in Pending, log shows
        // nothing" report). _noteAdvanceSkip logs `[advance-skip] <id> (<reason>)` change-deduped per
        // session (the reasons are fixed literals, so equality is exact); _clearAdvanceSkip forgets
        // the session's last reason whenever its plan makes progress (a send / an armed confirm /
        // plan-done), so the NEXT stall logs again instead of being swallowed by the dedup.
        void _noteAdvanceSkip(const std::wstring& id, const std::wstring& reason);
        void _clearAdvanceSkip(const std::wstring& id);
        // Agentmaster (send spacing): re-run a session's advance once a TIMED hold expires
        // (AdvancePlan::retryAfterMs). Keeps the EARLIEST scheduled due per session; the worker's
        // wait wakes for it and promotes it into _queue. Without this a spacing-held plan on a
        // quiet session would stall until some unrelated registry notify happened along.
        void _scheduleDeferredAdvance(const std::wstring& id, int64_t delayMs);

        std::shared_ptr<SessionRegistry> _registry;
        std::thread _thread;
        std::mutex _mtx;
        std::condition_variable _cv;
        std::deque<std::wstring> _queue;
        // Session ids whose latest Flight send is awaiting pickup — the Enter-retry watch list.
        // Armed in OnObserved (every send path marks the prompt Sent via the registry, which
        // notifies this observer), drained in _sweepPendingPickups. Guarded by _mtx.
        std::unordered_set<std::wstring> _pending;
        // sessionId -> the last [advance-skip] reason logged for it (the change-dedup state; see
        // _noteAdvanceSkip). Guarded by _mtx; erased on plan progress; wholesale-reset past a
        // generous cap (the Engine.cpp [Unknown]-dedup precedent) so a long run can't grow it.
        std::unordered_map<std::wstring, std::wstring> _lastSkipReason;
        // sessionId -> due NowMs for a deferred re-advance (the send-spacing hold's self-release;
        // _scheduleDeferredAdvance). Guarded by _mtx; one-shot — an entry is erased the moment the
        // worker promotes it into _queue, so it is bounded by the sessions currently spacing-held.
        std::unordered_map<std::wstring, int64_t> _deferred;
        std::atomic<bool> _running{ false };
        std::atomic<bool> _globalPause{ false };
    };
}
