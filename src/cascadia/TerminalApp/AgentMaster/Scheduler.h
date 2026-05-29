// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the Autopilot scheduler (DESIGN §10, HOOKS.md tryAdvance). It advances a
// session's Flight Plan when the session reaches turn-complete (a clean Stop ->
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

#include "SessionModels.h"

namespace Agentmaster
{
    class SessionRegistry;

    // Per-item opt-out of the question-guard: a queued prompt whose guardPattern equals this
    // is allowed to fire even when the agent's last message was a question.
    inline constexpr std::wstring_view kAnswersQuestionOk = L"answers-a-question:ok";

    enum class AdvanceAction
    {
        None, // nothing to do (see reason)
        Send, // auto-send queue[promptIndex] (Full)
        Hold, // a guard blocked auto-send (e.g. agent asked a question)
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
    // Encodes, in order: global pause; mode Off; not turn-complete; pause-on-human-input;
    // maxAutoSends; (no Pending => PlanDone); Manual-gate skip; question-guard Hold;
    // SemiAuto => AwaitConfirm; Full => Send.
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
        if (s.autopilot.mode == AutopilotMode::Off)
        {
            plan.reason = L"autopilot off";
            return plan;
        }
        if (s.state != SessionState::WaitingForInput)
        {
            plan.reason = L"not turn-complete";
            return plan;
        }
        if (s.autopilot.pauseOnHumanInput &&
            lastHumanInputUnixMs != 0 &&
            (nowUnixMs - lastHumanInputUnixMs) >= 0 &&
            (nowUnixMs - lastHumanInputUnixMs) < 1500)
        {
            plan.reason = L"human typing";
            return plan;
        }
        if (s.autopilot.autoSendsThisRun >= s.autopilot.maxAutoSends)
        {
            plan.reason = L"maxAutoSends reached";
            return plan;
        }

        // First Pending prompt that isn't blocked by an unmet dependency.
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

        const bool overridesQuestionGuard = (item.guardPattern == kAnswersQuestionOk);
        if (s.lastMessageWasQuestion && !overridesQuestionGuard)
        {
            plan.action = AdvanceAction::Hold;
            plan.reason = L"question-guard (agent asked a question)";
            return plan;
        }

        if (s.autopilot.mode == AutopilotMode::SemiAuto)
        {
            plan.action = AdvanceAction::AwaitConfirm;
            plan.reason = L"semi-auto: awaiting confirm";
            return plan;
        }

        plan.action = AdvanceAction::Send;
        plan.reason = L"full: auto-send";
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

        std::shared_ptr<SessionRegistry> _registry;
        std::thread _thread;
        std::mutex _mtx;
        std::condition_variable _cv;
        std::deque<std::wstring> _queue;
        std::atomic<bool> _running{ false };
        std::atomic<bool> _globalPause{ false };
    };
}
