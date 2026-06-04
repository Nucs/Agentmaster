// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and
// the standalone test harness compiles it directly). Includes only what it needs.
#include "SessionRegistry.h"

#include <algorithm>
#include <chrono>

namespace
{
    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // A short single-line label for a captured prompt (mirrors the UI's Add-prompt rule).
    std::wstring MakeLabel(const std::wstring& text)
    {
        std::wstring label = text.substr(0, 56);
        std::replace(label.begin(), label.end(), L'\n', L' ');
        std::replace(label.begin(), label.end(), L'\r', L' ');
        return label;
    }

    // A Flight prompt we injected echoes back as a UserPromptSubmit with the same text. We
    // only treat an incoming prompt as that echo when a matching Flight prompt was Sent very
    // recently (so a stale never-echoed prompt, or a reloaded old one, can't swallow a fresh
    // human message that happens to repeat the text).
    constexpr int64_t kEchoWindowMs = 15000;
}

namespace Agentmaster
{
    void SessionRegistry::_notify(const SessionInfo& snapshot, HookEvent cause)
    {
        std::vector<RegistryObserver> observers;
        {
            std::lock_guard guard{ _mtx };
            observers.reserve(_observers.size());
            for (const auto& o : _observers)
            {
                observers.push_back(o.second);
            }
        }
        for (auto& ob : observers)
        {
            if (ob)
            {
                try
                {
                    ob(snapshot, cause);
                }
                catch (...)
                {
                }
            }
        }
    }

    std::wstring SessionRegistry::Upsert(SessionInfo info)
    {
        auto id = info.id;
        SessionInfo snapshot;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                _order.push_back(id);
            }
            _sessions[id] = std::move(info);
            snapshot = _sessions[id];
        }
        _notify(snapshot, HookEvent::Unknown);
        return id;
    }

    void SessionRegistry::Remove(const std::wstring& id)
    {
        {
            std::lock_guard guard{ _mtx };
            _sessions.erase(id);
            _injectors.erase(id);
            _lastHumanInput.erase(id);
            _order.erase(std::remove(_order.begin(), _order.end(), id), _order.end());
        }
    }

    bool SessionRegistry::Contains(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        return _sessions.find(id) != _sessions.end();
    }

    std::optional<SessionInfo> SessionRegistry::Get(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<SessionInfo> SessionRegistry::Snapshot() const
    {
        std::lock_guard guard{ _mtx };
        std::vector<SessionInfo> out;
        out.reserve(_order.size());
        for (const auto& id : _order)
        {
            const auto it = _sessions.find(id);
            if (it != _sessions.end())
            {
                out.push_back(it->second);
            }
        }
        return out;
    }

    size_t SessionRegistry::Count() const
    {
        std::lock_guard guard{ _mtx };
        return _sessions.size();
    }

    void SessionRegistry::OnHookEvent(const HookMessage& msg)
    {
        SessionInfo snapshot;
        bool found = false;
        bool triggerAdvance = false;

        {
            std::lock_guard guard{ _mtx };
            auto it = _sessions.find(msg.sessionId);
            if (it == _sessions.end())
            {
                // A SessionStart may race ahead of our own Upsert (or arrive for a session
                // we restored without re-registering). Create a minimal record rather than
                // dropping authoritative state. Any other event for an unknown session is
                // ignored (we have no connection to bind it to).
                if (msg.event != HookEvent::SessionStart)
                {
                    return;
                }
                SessionInfo created;
                created.id = msg.sessionId;
                created.workingDir = msg.cwd;
                created.state = SessionState::Idle;
                // A hook means a real claude is running, so this session is OPEN (live), not
                // archived — it belongs on the Triage Board immediately.
                created.live = true;
                // Mark it external/observe-only; the adoption handler (fired below, outside
                // the lock) tries to bind it to its ConPTY for full control.
                created.external = true;
                _order.push_back(msg.sessionId);
                it = _sessions.emplace(msg.sessionId, std::move(created)).first;
            }

            auto& s = it->second;
            // Remember the hosting ConPTY (WT_SESSION) from EVERY hook — the stable tab identity the
            // app reconciles against (survives an in-session /resume that changes the session id).
            if (!msg.tabToken.empty())
            {
                s.tabToken = msg.tabToken;
            }
            const auto next = NextSessionState(s.state, msg);
            s.state = next;
            if (msg.ts != 0)
            {
                s.lastActivityUnixMs = msg.ts;
            }
            if (s.workingDir.empty() && !msg.cwd.empty())
            {
                s.workingDir = msg.cwd;
            }
            if (msg.event == HookEvent::Stop)
            {
                s.lastMessageWasQuestion = msg.lastMessageIsQuestion;
            }

            // The Flight Plan reflects EVERY message a session received. A UserPromptSubmit is
            // either the echo of a prompt WE just injected (suppress it — it is already in the
            // queue as Sent), or a prompt the human typed straight into the ConPTY (record it
            // as a Sent/Typed entry so the Flight Plan's "sent" summary is complete).
            if (msg.event == HookEvent::UserPromptSubmit && !msg.promptText.empty())
            {
                const int64_t now = NowMs();
                bool isEcho = false;
                for (auto& p : s.queue)
                {
                    if (p.origin == PromptOrigin::Flight && p.status == PromptStatus::Sent && !p.echoed &&
                        p.text == msg.promptText && p.sentAtUnixMs != 0 && (now - p.sentAtUnixMs) >= 0 &&
                        (now - p.sentAtUnixMs) < kEchoWindowMs)
                    {
                        p.echoed = true; // consume exactly one echo per injected prompt
                        isEcho = true;
                        break;
                    }
                }
                if (!isEcho)
                {
                    QueuedPrompt typed;
                    typed.id = L"typed-" + std::to_wstring(now) + L"-" + std::to_wstring(_typedSeq++);
                    typed.label = MakeLabel(msg.promptText);
                    typed.text = msg.promptText;
                    typed.status = PromptStatus::Sent;
                    typed.origin = PromptOrigin::Typed;
                    typed.echoed = true; // it IS the message; no further echo expected
                    typed.attempts = 1;
                    typed.sentAtUnixMs = now;
                    s.queue.push_back(std::move(typed));
                }
            }

            snapshot = s;
            found = true;
            // Only a clean turn-complete advances the Flight Plan (Correctness Rule #1).
            triggerAdvance = (msg.event == HookEvent::Stop && next == SessionState::WaitingForInput);
        }

        if (found)
        {
            _notify(snapshot, msg.event);
        }
        // Agentmaster: fire the bind/adoption handler on EVERY SessionStart — not only when this
        // call CREATED the record — so the app layer can reconcile the tab<->session binding by the
        // STABLE WT_SESSION tabToken. This covers (a) adopting a hand-typed `+`-tab claude (a new,
        // unknown id), AND (b) RE-HOMING a tab whose claude switched conversation id via the
        // in-session `/resume` (the session id changes; the ConPTY / tabToken does not — and the new
        // id may even be a previously-known/archived one, which never created a record here). The
        // reconcile is idempotent: a SessionStart for an already-bound session fast-returns.
        if (msg.event == HookEvent::SessionStart)
        {
            // Fan out to every window's bind handler (M9: the registry is a process-wide singleton).
            // Snapshot under the lock, invoke outside it; whichever window hosts this session's tab
            // binds / re-homes it, the rest no-op.
            std::vector<AdoptionHandler> adopters;
            {
                std::lock_guard guard{ _mtx };
                adopters.reserve(_adopters.size());
                for (const auto& a : _adopters)
                {
                    adopters.push_back(a.second);
                }
            }
            for (auto& adopt : adopters)
            {
                if (adopt)
                {
                    try
                    {
                        adopt(msg.sessionId, msg.cwd, msg.tabToken);
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
        if (triggerAdvance && _advance)
        {
            try
            {
                _advance(msg.sessionId);
            }
            catch (...)
            {
            }
        }
    }

    bool SessionRegistry::Update(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate)
    {
        SessionInfo snapshot;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return false;
            }
            mutate(it->second);
            snapshot = it->second;
        }
        _notify(snapshot, HookEvent::Unknown);
        return true;
    }

    void SessionRegistry::UpdateQuiet(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate)
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end())
        {
            return;
        }
        mutate(it->second);
        // Deliberately NO _notify: this path exists precisely to avoid the persist / UI / advance
        // cascade for transient, high-frequency fields (e.g. streamed assistant text).
    }

    void SessionRegistry::NoteExternalPrompt(const std::wstring& id, const std::wstring& text)
    {
        if (text.empty())
        {
            return;
        }
        SessionInfo snapshot;
        bool changed = false;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return;
            }
            auto& s = it->second;
            // Idempotent: a message already recorded (status Sent — a Typed capture or an injected
            // Flight prompt's echo, which also shows up as a user line in the transcript) must not
            // be duplicated. Pending plan items are NOT "recorded messages", so they don't suppress
            // recording a human message that happens to match a queued prompt's text.
            for (const auto& p : s.queue)
            {
                if (p.status == PromptStatus::Sent && p.text == text)
                {
                    return;
                }
            }
            const int64_t now = NowMs();
            QueuedPrompt typed;
            typed.id = L"recon-" + std::to_wstring(now) + L"-" + std::to_wstring(_typedSeq++);
            typed.label = MakeLabel(text);
            typed.text = text;
            typed.status = PromptStatus::Sent;
            typed.origin = PromptOrigin::Typed;
            typed.echoed = true; // it IS the message; no further echo expected
            typed.attempts = 1;
            typed.sentAtUnixMs = now;
            s.queue.push_back(std::move(typed));
            snapshot = s;
            changed = true;
        }
        if (changed)
        {
            _notify(snapshot, HookEvent::UserPromptSubmit);
        }
    }

    ObserverToken SessionRegistry::AddObserver(RegistryObserver observer)
    {
        std::lock_guard guard{ _mtx };
        const ObserverToken token = _nextObserverId++;
        _observers.emplace_back(token, std::move(observer));
        return token;
    }

    void SessionRegistry::RemoveObserver(ObserverToken token)
    {
        std::lock_guard guard{ _mtx };
        _observers.erase(
            std::remove_if(_observers.begin(), _observers.end(), [token](const auto& o) { return o.first == token; }),
            _observers.end());
    }

    void SessionRegistry::SetAdvanceHandler(AdvanceHandler handler)
    {
        std::lock_guard guard{ _mtx };
        _advance = std::move(handler);
    }

    AdoptionToken SessionRegistry::AddAdoptionHandler(AdoptionHandler handler)
    {
        std::lock_guard guard{ _mtx };
        const AdoptionToken token = _nextAdopterId++;
        _adopters.emplace_back(token, std::move(handler));
        return token;
    }

    void SessionRegistry::RemoveAdoptionHandler(AdoptionToken token)
    {
        std::lock_guard guard{ _mtx };
        _adopters.erase(
            std::remove_if(_adopters.begin(), _adopters.end(), [token](const auto& a) { return a.first == token; }),
            _adopters.end());
    }

    void SessionRegistry::SetInjector(const std::wstring& id, Injector injector)
    {
        std::lock_guard guard{ _mtx };
        if (injector)
        {
            _injectors[id] = std::move(injector);
        }
        else
        {
            _injectors.erase(id);
        }
    }

    bool SessionRegistry::Inject(const std::wstring& id, const std::wstring& text) const
    {
        Injector fn;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _injectors.find(id);
            if (it == _injectors.end())
            {
                return false;
            }
            fn = it->second; // copy so we call it outside the lock
        }
        if (!fn)
        {
            return false;
        }
        try
        {
            fn(text);
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    bool SessionRegistry::HasInjector(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        return _injectors.find(id) != _injectors.end();
    }

    void SessionRegistry::NoteHumanInput(const std::wstring& id, int64_t unixMs)
    {
        std::lock_guard guard{ _mtx };
        _lastHumanInput[id] = unixMs;
    }

    int64_t SessionRegistry::LastHumanInputUnixMs(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _lastHumanInput.find(id);
        return it == _lastHumanInput.end() ? 0 : it->second;
    }
}
