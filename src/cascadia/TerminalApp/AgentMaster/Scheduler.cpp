// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
#include "Scheduler.h"

#include "ClaudeSpawn.h" // AppendStateLog, NewSessionId-not-needed here
#include "SessionRegistry.h"

#include <chrono>

namespace
{
    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
}

namespace Agentmaster
{
    Scheduler::Scheduler(std::shared_ptr<SessionRegistry> registry) :
        _registry{ std::move(registry) }
    {
    }

    Scheduler::~Scheduler()
    {
        Stop();
    }

    void Scheduler::Start()
    {
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            return;
        }
        _thread = std::thread([this]() noexcept { _worker(); });
    }

    void Scheduler::Stop() noexcept
    {
        if (_running.exchange(false))
        {
            _cv.notify_all();
            if (_thread.joinable())
            {
                _thread.join();
            }
        }
    }

    void Scheduler::RequestAdvance(const std::wstring& sessionId)
    {
        {
            std::lock_guard lk{ _mtx };
            // Dedup: one pending advance per session is enough — _process always re-reads the
            // latest state, so collapsing a burst of observed changes (the idle-plan trigger
            // can fire on every mutation) loses nothing and keeps the queue from piling up.
            for (const auto& q : _queue)
            {
                if (q == sessionId)
                {
                    return;
                }
            }
            _queue.push_back(sessionId);
        }
        _cv.notify_one();
    }

    void Scheduler::_worker() noexcept
    {
        for (;;)
        {
            std::wstring id;
            {
                std::unique_lock lk{ _mtx };
                _cv.wait(lk, [this] { return !_running.load() || !_queue.empty(); });
                if (!_running.load() && _queue.empty())
                {
                    return;
                }
                id = std::move(_queue.front());
                _queue.pop_front();
            }
            try
            {
                _process(id);
            }
            catch (...)
            {
            }
        }
    }

    void Scheduler::_process(const std::wstring& id)
    {
        auto s = _registry->Get(id);
        if (!s)
        {
            return;
        }

        // The question-guard is transient: once a turn ends WITHOUT a trailing question,
        // un-hold anything the guard parked so the plan resumes.
        if (!s->lastMessageWasQuestion)
        {
            bool changed = false;
            _registry->Update(id, [&](SessionInfo& ss) {
                for (auto& p : ss.queue)
                {
                    if (p.status == PromptStatus::Held)
                    {
                        p.status = PromptStatus::Pending;
                        changed = true;
                    }
                }
            });
            if (changed)
            {
                s = _registry->Get(id);
                if (!s)
                {
                    return;
                }
            }
        }

        auto plan = DecideAdvance(*s, NowMs(), _registry->LastHumanInputUnixMs(id), _globalPause.load());

        if (plan.action == AdvanceAction::Send)
        {
            // Throttle off the bridge: this runs on the scheduler thread, so sleeping is
            // fine. Re-decide afterwards in case state changed (human typed, paused, etc.).
            const auto throttle = s->autopilot.throttleMs;
            if (throttle > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(throttle));
            }
            auto s2 = _registry->Get(id);
            if (!s2)
            {
                return;
            }
            auto plan2 = DecideAdvance(*s2, NowMs(), _registry->LastHumanInputUnixMs(id), _globalPause.load());
            if (plan2.action == AdvanceAction::Send)
            {
                std::wstring text;
                _registry->Update(id, [&](SessionInfo& ss) {
                    if (plan2.promptIndex < ss.queue.size() && ss.queue[plan2.promptIndex].status == PromptStatus::Pending)
                    {
                        auto& p = ss.queue[plan2.promptIndex];
                        text = p.text;
                        p.status = PromptStatus::Sent;
                        p.sentAtUnixMs = NowMs();
                        p.attempts += 1;
                        p.echoed = false; // await this injection's UserPromptSubmit echo
                        ss.autopilot.autoSendsThisRun += 1;
                        ss.pendingConfirmPromptId.clear();
                    }
                });
                if (!text.empty())
                {
                    // Inject + submit (null-terminated by std::wstring). Idempotent: the
                    // prompt is already marked Sent above, so a duplicate advance won't resend.
                    const bool delivered = _registry->Inject(id, text + L"\r");
                    if (delivered)
                    {
                        AppendStateLog(L"autopilot.log", L"[send] " + id + L" #" + std::to_wstring(plan2.promptIndex) + L"\n");
                    }
                    else
                    {
                        // No stdin injector bound yet — can happen if a restored session's plan
                        // bootstraps from Idle before its ConPTY is wired. Roll the prompt back to
                        // Pending (Rule #4: never strand a phantom Sent) so it re-fires once the
                        // injector binds (the SessionStart hook re-triggers an advance).
                        _registry->Update(id, [&](SessionInfo& ss) {
                            if (plan2.promptIndex < ss.queue.size() && ss.queue[plan2.promptIndex].status == PromptStatus::Sent)
                            {
                                auto& p = ss.queue[plan2.promptIndex];
                                p.status = PromptStatus::Pending;
                                p.echoed = false;
                                if (p.attempts > 0)
                                {
                                    p.attempts -= 1;
                                }
                                if (ss.autopilot.autoSendsThisRun > 0)
                                {
                                    ss.autopilot.autoSendsThisRun -= 1;
                                }
                            }
                        });
                        AppendStateLog(L"autopilot.log", L"[send-deferred] " + id + L" (no injector yet)\n");
                    }
                }
                return;
            }
            // fall through to handle the re-decided plan
            plan = plan2;
        }

        switch (plan.action)
        {
        case AdvanceAction::Hold:
            _registry->Update(id, [&](SessionInfo& ss) {
                if (plan.promptIndex < ss.queue.size() && ss.queue[plan.promptIndex].status == PromptStatus::Pending)
                {
                    ss.queue[plan.promptIndex].status = PromptStatus::Held;
                }
            });
            AppendStateLog(L"autopilot.log", L"[hold] " + id + L" (" + plan.reason + L")\n");
            break;
        case AdvanceAction::AwaitConfirm:
            _registry->Update(id, [&](SessionInfo& ss) {
                if (plan.promptIndex < ss.queue.size())
                {
                    ss.pendingConfirmPromptId = ss.queue[plan.promptIndex].id;
                }
            });
            AppendStateLog(L"autopilot.log", L"[await-confirm] " + id + L"\n");
            break;
        case AdvanceAction::PlanDone:
            AppendStateLog(L"autopilot.log", L"[plan-done] " + id + L"\n");
            break;
        case AdvanceAction::None:
        case AdvanceAction::Send: // (already handled / state changed away from Send)
        default:
            break;
        }
    }

    void Scheduler::OnObserved(const SessionInfo& s)
    {
        // stopOnError backstop: a turn that ended in Error pauses the plan.
        if (s.state == SessionState::Error && s.autopilot.stopOnError && s.autopilot.mode != AutopilotMode::Off)
        {
            _registry->Update(s.id, [](SessionInfo& ss) { ss.autopilot.mode = AutopilotMode::Off; });
            AppendStateLog(L"autopilot.log", L"[stop-on-error] paused " + s.id + L"\n");
            return;
        }

        // Drive the plan from observed changes, not only the Stop hook: a managed session
        // sitting Idle/WaitingForInput with autopilot on and something Pending (just resumed,
        // or you just enabled autopilot / added a prompt) should START consuming. Without this
        // a resumed-Idle session would wait forever — it never emits a Stop to trigger an
        // advance. DecideAdvance + the pickup guard keep it to one prompt per turn, and
        // RequestAdvance dedups the burst. External/observe-only sessions are skipped: they have
        // no injector to write to until adopted (and would just churn the deferred-send path).
        // ARCHIVED sessions (!live) are skipped too: they have no live claude/injector — an
        // archived plan with autopilot on must not churn the deferred-send path (it loads from
        // disk Idle, possibly with mode=Full + Pending prompts) until the user restores it.
        if (s.live && !s.external && s.autopilot.mode != AutopilotMode::Off &&
            (s.state == SessionState::Idle || s.state == SessionState::WaitingForInput))
        {
            bool hasPending = false;
            for (const auto& p : s.queue)
            {
                if (p.status == PromptStatus::Pending)
                {
                    hasPending = true;
                    break;
                }
            }
            if (hasPending)
            {
                RequestAdvance(s.id);
            }
        }
    }

    void Scheduler::Confirm(const std::wstring& sessionId, bool confirm)
    {
        std::wstring text;
        _registry->Update(sessionId, [&](SessionInfo& ss) {
            if (ss.pendingConfirmPromptId.empty())
            {
                return;
            }
            for (auto& p : ss.queue)
            {
                if (p.id == ss.pendingConfirmPromptId)
                {
                    if (confirm && p.status == PromptStatus::Pending)
                    {
                        text = p.text;
                        p.status = PromptStatus::Sent;
                        p.sentAtUnixMs = NowMs();
                        p.attempts += 1;
                        p.echoed = false; // await this injection's UserPromptSubmit echo
                        ss.autopilot.autoSendsThisRun += 1;
                    }
                    else if (!confirm)
                    {
                        p.status = PromptStatus::Skipped;
                    }
                    break;
                }
            }
            ss.pendingConfirmPromptId.clear();
        });
        if (confirm && !text.empty())
        {
            _registry->Inject(sessionId, text + L"\r");
            AppendStateLog(L"autopilot.log", L"[confirm-send] " + sessionId + L"\n");
        }
    }
}
