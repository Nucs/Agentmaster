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

#include "SessionModels.h"

namespace Agentmaster
{
    class SessionRegistry;

    // Per-item opt-out of the question-guard: a queued prompt whose guardPattern equals this
    // is allowed to fire even when the agent's last message was a question.
    inline constexpr std::wstring_view kAnswersQuestionOk = L"answers-a-question:ok";

    // After injecting a prompt the session lingers Idle/WaitingForInput for a beat until
    // Claude's UserPromptSubmit moves it to Running. DecideAdvance refuses to fire another
    // prompt while a Flight prompt is Sent-but-not-yet-echoed within this window — so a
    // change-driven advance (the observer trigger that lets idle plans START) can't drain the
    // whole queue at once. Bounded in time so a lost echo can't permanently stall the plan.
    inline constexpr int64_t kPickupGuardMs = 4000;

    // Enter-retry (the "the TUI ate my Enter" backstop). The ConPTY can deliver an injected
    // `prompt + CR` faster than Claude's Ink UI initializes its input handler, so the submit Enter
    // is absorbed as a NEWLINE instead of sending — the prompt sits typed-but-not-submitted and the
    // turn never starts (no UserPromptSubmit, no transcript write, state stuck Idle/WaitingForInput).
    // The scheduler watches every just-sent Flight prompt; if the turn has not started within
    // kEnterRetryFirstMs (then kEnterRetryIntervalMs for later presses) it re-presses a LONE Enter
    // (never the text again — that would duplicate it), up to kEnterRetryMax extra presses, then gives
    // up: the prompt is marked Failed and the session's autorunner is PAUSED (it never landed — don't
    // strand a phantom Sent nor advance past a broken step; the user Send-nows / re-arms). kEnterRetry-
    // PollMs is how often the worker re-checks while a send awaits pickup. kEnterRetryActivityMarginMs
    // guards the transcript-advanced "it started" signal against a send fired sub-second after the
    // prior turn ended (so a real conversation write — not the prior turn's tail — is what clears it).
    inline constexpr int64_t kEnterRetryFirstMs = 3000; // first re-press fires fast — rescue the common eaten-CR case without a long stall
    inline constexpr int64_t kEnterRetryIntervalMs = 6000; // subsequent re-presses: slower (the turn may be genuinely starting)
    inline constexpr uint32_t kEnterRetryMax = 3; // never re-press Enter more than this many times
    inline constexpr int64_t kEnterRetryPollMs = 1000; // worker re-check cadence while a send awaits pickup
    inline constexpr int64_t kEnterRetryActivityMarginMs = 1000; // transcript must advance at least this far past the send to count as "started"

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
    };

    // PURE decision (DESIGN §10 "the loop" + the three-states table). Inputs are explicit so
    // tests pass deterministic values:
    //   nowUnixMs            — current time
    //   lastHumanInputUnixMs — when the human last typed into this session (0 if never)
    //   globalPause          — the scheduler's global Pause-all backstop
    // Encodes, in order: global pause; mode Off; not-ready (only Idle or WaitingForInput are
    // ready — see below); pause-on-human-input; maxAutoSends; awaiting-injection-pickup guard;
    // (no Pending => PlanDone); Manual-gate skip; question-guard (None — the prompt stays queued
    // and waits, exactly like a Running mid-turn); SemiAuto => AwaitConfirm; Full => Send.
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

        // Awaiting-injection-pickup guard: if a Flight prompt we just injected is Sent but has
        // not yet echoed back (Claude hasn't moved to Running), hold off — otherwise an advance
        // driven by an observed change in that brief window would send the NEXT prompt too,
        // draining the queue. Time-bounded (kPickupGuardMs) so a lost echo can't stall forever,
        // and ignores un-timestamped Sent prompts (e.g. a restored plan) via sentAtUnixMs != 0.
        for (const auto& q : s.queue)
        {
            if (q.origin == PromptOrigin::Autorun && q.status == PromptStatus::Sent && !q.echoed &&
                q.sentAtUnixMs != 0 && (nowUnixMs - q.sentAtUnixMs) >= 0 &&
                (nowUnixMs - q.sentAtUnixMs) < kPickupGuardMs)
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
    // ANY of: the prompt's UserPromptSubmit echo arrived (`echoed`, the hook path); the session left
    // the ready set (state advanced past Idle/WaitingForInput, e.g. Running — covers a hook-less
    // adopted session driven by the transcript tail); or the conversation transcript advanced past the
    // send (`convLastActivityUnixMs`, the no-hook fast-turn fallback). Otherwise the most-recently-sent
    // un-acknowledged Flight prompt is watched: Waiting until kEnterRetryIntervalMs elapses, then Retry
    // (until kEnterRetryMax presses), then GiveUp.
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
        // The most-recently-sent Flight prompt still awaiting its pickup (Sent, not echoed, and the
        // transcript hasn't advanced past it). A later send supersedes an earlier one.
        const QueuedPrompt* best = nullptr;
        for (const auto& p : s.queue)
        {
            if (p.origin != PromptOrigin::Autorun || p.status != PromptStatus::Sent || p.echoed || p.sentAtUnixMs == 0)
            {
                continue;
            }
            // Transcript advanced meaningfully past this send => Claude picked it up (started a turn).
            if (s.convLastActivityUnixMs != 0 && s.convLastActivityUnixMs > p.sentAtUnixMs + kEnterRetryActivityMarginMs)
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
        std::atomic<bool> _running{ false };
        std::atomic<bool> _globalPause{ false };
    };
}
