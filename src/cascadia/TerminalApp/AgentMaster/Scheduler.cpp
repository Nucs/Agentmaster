// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

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
            bool haveAdvance = false;
            {
                std::unique_lock lk{ _mtx };
                const auto ready = [this] { return !_running.load() || !_queue.empty(); };
                if (_pending.empty())
                {
                    _cv.wait(lk, ready);
                }
                else
                {
                    // A Flight send is awaiting pickup — wake on a poll cadence to re-check it (and
                    // re-press Enter when due) even if no advance is queued. (Spurious early wakes
                    // just run an extra cheap sweep; the predicate still gates real work.)
                    _cv.wait_for(lk, std::chrono::milliseconds(kEnterRetryPollMs), ready);
                }
                if (!_running.load() && _queue.empty())
                {
                    return;
                }
                if (!_queue.empty())
                {
                    id = std::move(_queue.front());
                    _queue.pop_front();
                    haveAdvance = true;
                }
            }
            if (haveAdvance)
            {
                try
                {
                    _process(id);
                }
                catch (...)
                {
                }
            }
            // Always sweep the Enter-retry watch list (cheap no-op when empty); a _process above may
            // have just armed a fresh send, and a timed wake lands here with no advance to process.
            try
            {
                _sweepPendingPickups();
            }
            catch (...)
            {
            }
        }
    }

    void Scheduler::_sweepPendingPickups()
    {
        // Snapshot the watch list (cheap id copy), then do all registry I/O OUTSIDE _mtx — Inject /
        // Update reach the ConPTY + fire observers (re-entering OnObserved on this very thread), which
        // must not happen under our lock.
        std::vector<std::wstring> ids;
        {
            std::lock_guard lk{ _mtx };
            ids.assign(_pending.begin(), _pending.end());
        }
        if (ids.empty())
        {
            return;
        }
        const int64_t now = NowMs();
        std::vector<std::wstring> done; // ids to drop from the watch (turn started / gave up / gone)
        for (const auto& id : ids)
        {
            const auto s = _registry->Get(id);
            if (!s)
            {
                done.push_back(id);
                continue;
            }
            const auto plan = DecideEnterRetry(*s, now, _registry->HasInjector(id));
            if (plan.action == EnterRetryAction::None)
            {
                done.push_back(id); // the turn started (or nothing awaits pickup) — stop watching
            }
            else if (plan.action == EnterRetryAction::GiveUp)
            {
                done.push_back(id);
                // Agentmaster (#4): the submit never landed after kEnterRetryMax presses. Don't leave a
                // phantom Sent (it reads as delivered but isn't) and don't let autorunner advance past a
                // broken step: mark the prompt Failed and PAUSE this session's autorunner (mirrors the
                // stopOnError backstop). The user fixes the cause, then Send-now / re-arms autorunner.
                // (Rolling back to Pending would just re-send and be re-eaten — an infinite loop.)
                _registry->Update(id, [&](SessionInfo& ss) {
                    for (auto& p : ss.queue)
                    {
                        if (p.id == plan.promptId && p.status == PromptStatus::Sent && !p.echoed)
                        {
                            p.status = PromptStatus::Failed;
                            break;
                        }
                    }
                    ss.autorunner.mode = AutorunnerMode::Off;
                });
                AppendStateLog(L"autorunner.log",
                               L"[enter-retry-giveup] " + id + L" (turn never started after " +
                                   std::to_wstring(kEnterRetryMax) + L" Enter retries; marked Failed, autorunner paused)\n");
            }
            else if (plan.action == EnterRetryAction::Retry)
            {
                // Re-press a LONE Enter — never the prompt text (it is already typed in Claude's box;
                // resending it would duplicate the message).
                const bool delivered = _registry->Inject(id, L"\r");
                if (!delivered)
                {
                    done.push_back(id); // injector vanished (tab closing) — stop watching
                    continue;
                }
                // Bump the counter + RESTART the echo/pickup window from this Enter: when a late press
                // finally submits, its UserPromptSubmit echo must land within kEchoWindowMs of the
                // (refreshed) sentAt or the registry would mis-record it as a fresh Typed prompt. The
                // still-Sent-and-unechoed guard avoids clobbering a prompt that got picked up between
                // the DecideEnterRetry read above and this Update.
                uint32_t attempt = 0;
                _registry->Update(id, [&](SessionInfo& ss) {
                    for (auto& p : ss.queue)
                    {
                        if (p.id == plan.promptId && p.status == PromptStatus::Sent && !p.echoed)
                        {
                            p.enterRetries += 1;
                            p.sentAtUnixMs = now;
                            attempt = p.enterRetries;
                            break;
                        }
                    }
                });
                AppendStateLog(L"autorunner.log",
                               L"[enter-retry] " + id + L" press " + std::to_wstring(attempt) + L"/" +
                                   std::to_wstring(kEnterRetryMax) + L"\n");
            }
            // EnterRetryAction::Waiting -> keep watching (not yet due)
        }
        if (!done.empty())
        {
            std::lock_guard lk{ _mtx };
            for (const auto& id : done)
            {
                _pending.erase(id);
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
            const auto throttle = s->autorunner.throttleMs;
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
                        p.enterRetries = 0; // fresh send -> reset the Enter-retry watch (Scheduler.h)
                        ss.autorunner.autoSendsThisRun += 1;
                        ss.pendingConfirmPromptId.clear();
                    }
                });
                if (!text.empty())
                {
                    // Inject + submit via a bracketed paste so a multi-line body lands as ONE message
                    // (BuildPromptSubmission, #6) instead of submitting on the first embedded line break.
                    // Idempotent: the prompt is already marked Sent above, so a duplicate advance won't resend.
                    const bool delivered = _registry->Inject(id, BuildPromptSubmission(text));
                    if (delivered)
                    {
                        AppendStateLog(L"autorunner.log", L"[send] " + id + L" #" + std::to_wstring(plan2.promptIndex) + L"\n");
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
                                if (ss.autorunner.autoSendsThisRun > 0)
                                {
                                    ss.autorunner.autoSendsThisRun -= 1;
                                }
                            }
                        });
                        AppendStateLog(L"autorunner.log", L"[send-deferred] " + id + L" (no injector yet)\n");
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
            // LEGACY / unreached: DecideAdvance no longer returns Hold — a pending question now
            // leaves the prompt Pending (it stays queued and waits, like a Running mid-turn).
            // Retained defensively so the switch stays exhaustive; if it ever fired again it would
            // re-create the strand bug this case once caused.
            _registry->Update(id, [&](SessionInfo& ss) {
                if (plan.promptIndex < ss.queue.size() && ss.queue[plan.promptIndex].status == PromptStatus::Pending)
                {
                    ss.queue[plan.promptIndex].status = PromptStatus::Held;
                }
            });
            AppendStateLog(L"autorunner.log", L"[hold] " + id + L" (" + plan.reason + L")\n");
            break;
        case AdvanceAction::AwaitConfirm:
            _registry->Update(id, [&](SessionInfo& ss) {
                if (plan.promptIndex < ss.queue.size())
                {
                    ss.pendingConfirmPromptId = ss.queue[plan.promptIndex].id;
                }
            });
            AppendStateLog(L"autorunner.log", L"[await-confirm] " + id + L"\n");
            break;
        case AdvanceAction::PlanDone:
            AppendStateLog(L"autorunner.log", L"[plan-done] " + id + L"\n");
            break;
        case AdvanceAction::None:
        case AdvanceAction::Send: // (already handled / state changed away from Send)
        default:
            break;
        }
    }

    void Scheduler::OnObserved(const SessionInfo& s)
    {
        // Enter-retry watch (DecideEnterRetry): arm the watch whenever a session has a Flight send
        // awaiting pickup. EVERY send path — the auto-send + SemiAuto confirm below AND the Manager's
        // manual Send-now — marks the prompt Sent through the registry, which notifies this observer,
        // so no send path needs to know about the retry mechanism. Independent of autorunner mode (a
        // manual Send-now with autorunner Off must still submit reliably) and of the early returns
        // below, so it sits first. The worker sweep drains the watch as turns start / give up.
        {
            const auto rp = DecideEnterRetry(s, NowMs(), _registry->HasInjector(s.id));
            if (rp.action == EnterRetryAction::Waiting || rp.action == EnterRetryAction::Retry)
            {
                bool added = false;
                {
                    std::lock_guard lk{ _mtx };
                    added = _pending.insert(s.id).second;
                }
                if (added)
                {
                    _cv.notify_one(); // wake the worker to begin polling this send for pickup
                }
            }
        }

        // stopOnError backstop: a turn that ended in Error pauses the plan.
        if (s.state == SessionState::Error && s.autorunner.stopOnError && s.autorunner.mode != AutorunnerMode::Off)
        {
            _registry->Update(s.id, [](SessionInfo& ss) { ss.autorunner.mode = AutorunnerMode::Off; });
            AppendStateLog(L"autorunner.log", L"[stop-on-error] paused " + s.id + L"\n");
            return;
        }

        // Drive the plan from observed changes, not only the Stop hook: a managed session
        // sitting Idle/WaitingForInput with autorunner on and something Pending (just resumed,
        // or you just enabled autorunner / added a prompt) should START consuming. Without this
        // a resumed-Idle session would wait forever — it never emits a Stop to trigger an
        // advance. DecideAdvance + the pickup guard keep it to one prompt per turn, and
        // RequestAdvance dedups the burst.
        //
        // The gate is CONTROLLABILITY (HasInjector — do we hold this session's stdin?), NOT
        // provenance (s.external — did we launch it?). The two diverge for an ADOPTED session:
        // a claude typed into a `+` tab is external=true yet, once adopted, is bound an injector
        // and is fully drivable. Gating on !s.external used to skip every adopted session here,
        // so toggling Autorunner Off→back (a UI Update that fires no hook, hence no Stop-seam
        // advance) never started consuming its queue — the "switching autorunner off and back,
        // pending messages are not sent" bug. An observe-only external (no injector) and an
        // ARCHIVED session (!live, loads from disk Idle possibly with mode=Full + Pending) both
        // lack an injector, so HasInjector is false and they are still correctly skipped — no
        // churn on the deferred-send path until the user restores/adopts them.
        //
        // Agentmaster (dormant re-homed tab): ALSO require the session to have STARTED (its ConPTY/claude
        // has actually launched) OR be external. A window-restored BACKGROUND tab is live=true +
        // injector-bound but DORMANT — WT starts a background tab's child LAZILY (only on its first
        // layout, i.e. when the tab is first shown) — so injecting a queued prompt now writes into a
        // connection whose claude has NOT launched: the submit is lost, the Enter-retry watchdog then
        // gives up and marks the prompt Failed + PAUSES the plan (a phantom failure on every reopen).
        // Defer until the tab actually starts: SetStarted(true) (fired the instant the control
        // initializes — on focus, or an in-place Activate) _notify's this observer, so the plan resumes
        // exactly then with nothing lost. `started` is MEANINGLESS for an external session (we track no
        // control for it — left false though the adopted claude IS running), so the `|| s.external`
        // carve-out keeps adopted sessions drivable (mirrors the `!started && !external` "dormant"
        // definition the tab-strip status dot uses).
        if (s.live && (s.started || s.external) && s.autorunner.mode != AutorunnerMode::Off &&
            (s.state == SessionState::Idle || s.state == SessionState::WaitingForInput) &&
            _registry->HasInjector(s.id))
        {
            bool hasWork = false;
            for (const auto& p : s.queue)
            {
                // Pending = ready to (re)consider. Held = a question-guard hold persisted by an
                // older build (current builds never park a prompt in Held — the guard keeps it
                // Pending instead): include it so _process's un-hold recovery is reachable and a
                // stranded Held prompt rehabilitates to Pending once the question clears, rather
                // than sitting dead forever (OnObserved is the only re-trigger for an idle session).
                if (p.status == PromptStatus::Pending || p.status == PromptStatus::Held)
                {
                    hasWork = true;
                    break;
                }
            }
            if (hasWork)
            {
                RequestAdvance(s.id);
            }
        }
    }

    void Scheduler::Confirm(const std::wstring& sessionId, bool confirm)
    {
        std::wstring text;
        std::wstring confirmedId;
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
                        confirmedId = p.id; // remember it so a failed inject can roll it back (Rule #4)
                        p.status = PromptStatus::Sent;
                        p.sentAtUnixMs = NowMs();
                        p.attempts += 1;
                        p.echoed = false; // await this injection's UserPromptSubmit echo
                        p.enterRetries = 0; // fresh send -> reset the Enter-retry watch (Scheduler.h)
                        ss.autorunner.autoSendsThisRun += 1;
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
            // Agentmaster: check the inject result and roll back on failure — the SemiAuto
            // confirm path was the mirror-image of the auto-send path in _process above but was MISSING
            // its rollback: it marked the prompt Sent then injected and ignored the bool. If no injector
            // is bound yet (confirming during a restore before the ConPTY is wired), the prompt was
            // stranded as a phantom Sent that was never delivered AND never re-fires (the re-fire keys
            // on Pending), violating Correctness Rule #4. Mirror _process: revert to Pending; the next
            // advance re-decides AwaitConfirm and re-arms the confirm once the injector binds.
            const bool delivered = _registry->Inject(sessionId, BuildPromptSubmission(text));
            if (delivered)
            {
                AppendStateLog(L"autorunner.log", L"[confirm-send] " + sessionId + L"\n");
            }
            else
            {
                _registry->Update(sessionId, [&](SessionInfo& ss) {
                    for (auto& p : ss.queue)
                    {
                        if (p.id == confirmedId && p.status == PromptStatus::Sent)
                        {
                            p.status = PromptStatus::Pending;
                            p.echoed = false;
                            if (p.attempts > 0)
                            {
                                p.attempts -= 1;
                            }
                            if (ss.autorunner.autoSendsThisRun > 0)
                            {
                                ss.autorunner.autoSendsThisRun -= 1;
                            }
                            break;
                        }
                    }
                });
                AppendStateLog(L"autorunner.log", L"[confirm-deferred] " + sessionId + L" (no injector yet)\n");
            }
        }
    }
}
