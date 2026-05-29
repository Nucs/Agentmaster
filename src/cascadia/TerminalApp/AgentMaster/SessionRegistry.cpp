// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and
// the standalone test harness compiles it directly). Includes only what it needs.
#include "SessionRegistry.h"

#include <algorithm>

namespace Agentmaster
{
    void SessionRegistry::_notify(const SessionInfo& snapshot, HookEvent cause)
    {
        std::vector<RegistryObserver> observers;
        {
            std::lock_guard guard{ _mtx };
            observers = _observers;
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
                _order.push_back(msg.sessionId);
                it = _sessions.emplace(msg.sessionId, std::move(created)).first;
            }

            auto& s = it->second;
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

            snapshot = s;
            found = true;
            // Only a clean turn-complete advances the Flight Plan (Correctness Rule #1).
            triggerAdvance = (msg.event == HookEvent::Stop && next == SessionState::WaitingForInput);
        }

        if (found)
        {
            _notify(snapshot, msg.event);
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

    void SessionRegistry::AddObserver(RegistryObserver observer)
    {
        std::lock_guard guard{ _mtx };
        _observers.push_back(std::move(observer));
    }

    void SessionRegistry::SetAdvanceHandler(AdvanceHandler handler)
    {
        std::lock_guard guard{ _mtx };
        _advance = std::move(handler);
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
