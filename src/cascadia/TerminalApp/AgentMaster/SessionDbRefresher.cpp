// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the app's half of the session database (see SessionDbRefresher.h).

#include "SessionDbRefresher.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <optional>
#include <vector>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "Json.h"
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8 / Utf8ToWide
#include "SessionDbClient.h"
#include "SessionDbProtocol.h"
#include "SessionRegistry.h"
#include "SessionScanner.h"
#include "SessionStore.h" // SessionFactsEpoch
#include "TranscriptWatcher.h" // ChangedSessions

namespace Agentmaster
{
    namespace
    {
        namespace Proto = SessionDbProtocol;

        // How long the tick waits for a service it had to start, and for one answer.
        constexpr uint32_t kConnectWaitMs = 20000;
        constexpr uint32_t kCallTimeoutMs = 120000;
        // A sweep may be the cold build (minutes); its call waits this long before the next tick retries.
        constexpr uint32_t kSweepTimeoutMs = 60u * 60u * 1000u;
        // While the watch is unhealthy, a sweep at most this often (D16: "an unhealthy watcher (every 30 s)").
        constexpr int64_t kUnhealthySweepMs = 30000;
        // The facts are re-sent at least this often: the /clear and /handover links live in sessions.json, which
        // moves no epoch (titles, favorites and tags move the session store's facts epoch at once).
        constexpr int64_t kFactsEveryMs = 5 * 60 * 1000;

        int64_t TickMs()
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        void Log(const std::wstring& line)
        {
            AppendStateLog(L"hooks.log", line + L"\n");
        }

        std::wstring Lower(std::wstring s)
        {
            for (auto& c : s)
            {
                c = static_cast<wchar_t>(towlower(c));
            }
            return s;
        }

        std::wstring ExeDir()
        {
            std::wstring buf(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0)
                {
                    return {};
                }
                if (n < buf.size())
                {
                    buf.resize(n);
                    break;
                }
                buf.resize(buf.size() * 2);
            }
            const size_t cut = buf.find_last_of(L"\\/");
            return cut == std::wstring::npos ? std::wstring{} : buf.substr(0, cut);
        }

        // An answer envelope parsed (the service writes compact UTF-8 JSON). nullopt for anything else.
        std::optional<json::Value> ParseAnswer(const std::string& text)
        {
            return json::Parse(Profiles::detail::Utf8ToWide(text));
        }

        // The `error.message` of an answer, or "" when it carries a result.
        std::wstring ErrorOf(const json::Value& env)
        {
            if (const json::Value* e = env.Find(L"error"))
            {
                if (const json::Value* m = e->Find(L"message"); m && m->type == json::Value::Type::Str)
                {
                    return m->str;
                }
                return L"failed";
            }
            return {};
        }
    }

    struct SessionDbRefresher::SweepState
    {
        explicit SweepState(const std::wstring& profile) :
            client(profile)
        {
        }
        SessionDb::ServiceClient client;
        std::atomic<bool> done{ false };
        std::atomic<bool> succeeded{ false };
    };

    SessionDbRefresher::SessionDbRefresher(std::shared_ptr<SessionScanner> scanner, std::shared_ptr<SessionRegistry> registry, std::wstring profileDir) :
        _scanner(std::move(scanner)), _registry(std::move(registry)), _profile(std::move(profileDir))
    {
    }

    SessionDbRefresher::~SessionDbRefresher()
    {
        Stop();
    }

    void SessionDbRefresher::Apply(const AppSettings& settings) noexcept
    {
        try
        {
            _refreshSeconds.store(ClampSessionDbRefreshSeconds(settings.sessionDbRefreshSeconds));
            const SessionDbMode mode = settings.sessionDbMode;
            if (mode == SessionDbMode::Off)
            {
                if (_running.load())
                {
                    Log(L"[sessdb] the session database is off: the refresh stops (the service exits by itself after 10 minutes)");
                }
                Stop();
                _mode.store(static_cast<int>(SessionDbMode::Off));
                return;
            }
            _mode.store(static_cast<int>(mode));
            if (_running.exchange(true))
            {
                _cv.notify_all(); // a new interval applies from the next tick
                return;
            }
            _stop.store(false);
            _resyncWanted.store(true);
            _factsDirty.store(true);
            Log(L"[sessdb] the session database is " + std::wstring{ mode == SessionDbMode::On ? L"on" : L"in shadow mode" } + L": refreshing every " +
                std::to_wstring(_refreshSeconds.load()) + L" s");
            _thread = std::thread([this] { _run(); });
        }
        catch (...)
        {
            LogSwallowedException(L"SessionDbRefresher::Apply");
            _running.store(false);
        }
    }

    void SessionDbRefresher::Stop() noexcept
    {
        _stop.store(true);
        _cv.notify_all();
        // A tick waiting on an answer, and a sweep in flight, return at once.
        {
            std::lock_guard lk{ _ptrMtx };
            if (_client)
            {
                _client->Abort();
            }
            if (_sweep)
            {
                _sweep->client.Abort();
            }
        }
        if (_thread.joinable())
        {
            try
            {
                _thread.join();
            }
            catch (...)
            {
                LogSwallowedException(L"SessionDbRefresher::Stop (join)");
            }
        }
        _running.store(false);
        _ready.store(false);
    }

    void SessionDbRefresher::NoteFactsChanged() noexcept
    {
        _factsDirty.store(true);
    }

    void SessionDbRefresher::RequestResync() noexcept
    {
        _resyncWanted.store(true);
        _cv.notify_all();
    }

    std::wstring SessionDbRefresher::StatusText() const
    {
        std::lock_guard lk{ _mtx };
        return _status;
    }

    void SessionDbRefresher::_setStatus(std::wstring text)
    {
        std::lock_guard lk{ _mtx };
        _status = std::move(text);
    }

    void SessionDbRefresher::_run() noexcept
    {
        ::SetThreadDescription(::GetCurrentThread(), L"sessiondb refresh");
        try
        {
            _scanner->SetTranscriptChangeTracking(true);
            // §4.2: the service runs from a copy in the profile, so a long-lived MCP server never pins the package.
            std::wstring err;
            if (!SessionDb::EnsureServiceBinaries(_profile, ExeDir(), err))
            {
                Log(L"[sessdb] copying the service out of the package: " + err + L" (it runs from beside the app instead)");
            }
        }
        catch (...)
        {
            LogSwallowedException(L"SessionDbRefresher start");
        }
        while (!_stop.load())
        {
            try
            {
                _tick();
            }
            catch (...)
            {
                LogSwallowedException(L"SessionDbRefresher tick");
                std::lock_guard lk{ _ptrMtx };
                _client.reset(); // the next tick reconnects
            }
            std::unique_lock lk{ _mtx };
            _cv.wait_for(lk, std::chrono::seconds(_refreshSeconds.load()), [this] { return _stop.load(); });
        }
        try
        {
            _scanner->SetTranscriptChangeTracking(false);
            std::lock_guard lk{ _ptrMtx };
            _client.reset();
        }
        catch (...)
        {
            LogSwallowedException(L"SessionDbRefresher stop");
        }
    }

    void SessionDbRefresher::_startSweep()
    {
        if (_sweep && !_sweep->done.load())
        {
            return; // one sweep at a time: the running one covers everything up to its start; later changes are upserts
        }
        auto state = std::make_shared<SweepState>(_profile);
        {
            std::lock_guard lk{ _ptrMtx };
            _sweep = state;
        }
        try
        {
            // The thread owns its state: Stop() aborts the call and lets the thread end by itself.
            std::thread([state] {
                ::SetThreadDescription(::GetCurrentThread(), L"sessiondb sweep");
                try
                {
                    std::wstring err;
                    std::string answer;
                    const int64_t t0 = TickMs();
                    if (state->client.Connect(false, 5000, err) && state->client.Call(Proto::kResync, "{}", answer, kSweepTimeoutMs, err))
                    {
                        const auto env = ParseAnswer(answer);
                        const std::wstring failure = env ? ErrorOf(*env) : L"an unreadable answer";
                        if (failure.empty())
                        {
                            const json::Value* r = env->Find(L"result");
                            Log(L"[sessdb] sweep: " + std::to_wstring(r ? r->I64At(L"sessionsUpserted") : 0) + L" session(s), " +
                                std::to_wstring(r ? r->I64At(L"linesIngested") : 0) + L" line(s) in " + std::to_wstring(TickMs() - t0) + L" ms");
                            state->succeeded.store(true);
                        }
                        else
                        {
                            Log(L"[sessdb] sweep failed: " + failure);
                        }
                    }
                    else
                    {
                        Log(L"[sessdb] sweep failed: " + err);
                    }
                    state->client.Close();
                }
                catch (...)
                {
                    LogSwallowedException(L"SessionDbRefresher sweep");
                }
                state->done.store(true);
            }).detach();
        }
        catch (...)
        {
            LogSwallowedException(L"SessionDbRefresher sweep thread");
            state->done.store(true);
        }
    }

    void SessionDbRefresher::_tick()
    {
        // 1. The connection (it also keeps the service alive: it exits after 10 minutes without a client).
        if (!_client || !_client->Connected())
        {
            {
                std::lock_guard lk{ _ptrMtx };
                _client = std::make_unique<SessionDb::ServiceClient>(_profile);
            }
            std::wstring err;
            if (!_client->Connect(true, kConnectWaitMs, err))
            {
                std::lock_guard lk{ _ptrMtx };
                _client.reset();
                if (TickMs() - _lastConnectFailTick >= 60000)
                {
                    Log(L"[sessdb] the service did not answer: " + err);
                    _lastConnectFailTick = TickMs();
                }
                _setStatus(L"Waiting for the service: " + err);
                return;
            }
            Log(L"[sessdb] connected to the session database service");
            _resyncWanted.store(true); // anything may have changed while nobody listened
            _factsDirty.store(true);
            _epochKnown = false;
            _lastOpen.clear();
        }
        if (_sweep && _sweep->done.load() && _sweep->succeeded.load() && !_ready.exchange(true))
        {
            Log(L"[sessdb] the database covers every session (first sweep done)");
        }

        // 2. What changed since the last tick (the transcript watcher's set, D16).
        ChangedSessions changed;
        _scanner->TakeChangedTranscripts(changed);
        if (!changed.healthy)
        {
            if (TickMs() - _lastUnhealthySweepTick >= kUnhealthySweepMs)
            {
                _resyncWanted.store(true); // the watch is down: only a sweep sees the changes
                _lastUnhealthySweepTick = TickMs();
            }
        }
        else
        {
            if (_epochKnown && changed.epoch != _lastEpoch)
            {
                _resyncWanted.store(true); // the watcher re-indexed (an overflow, a lost watch): sweep
            }
            _lastEpoch = changed.epoch;
            _epochKnown = true;
        }
        for (auto& [id, path] : changed.sessions)
        {
            _pending[id] = path;
        }

        // 3. The sweep, on its own connection and thread.
        if (_resyncWanted.exchange(false))
        {
            _startSweep();
        }

        std::wstring err;
        std::string answer;
        // 4. Agentmaster's own facts (titles, tags, favorites, links): the service re-reads them from the profile.
        const uint64_t factsEpoch = SessionFactsEpoch();
        if (factsEpoch != _lastFactsEpoch || TickMs() - _lastFactsTick >= kFactsEveryMs)
        {
            _factsDirty.store(true);
        }
        if (_factsDirty.exchange(false))
        {
            if (!_client->Call(Proto::kFactsChanged, "{}", answer, kCallTimeoutMs, err))
            {
                _factsDirty.store(true);
                std::lock_guard lk{ _ptrMtx };
                _client.reset();
                return;
            }
            _lastFactsEpoch = factsEpoch;
            _lastFactsTick = TickMs();
        }

        // 5. The open sessions (only when the set changed).
        std::unordered_set<std::wstring> open;
        for (const auto& b : _registry->LiveTabBindings())
        {
            open.insert(Lower(b.id));
        }
        if (open != _lastOpen)
        {
            json::Value ids = json::Value::MkArr();
            for (const auto& id : open)
            {
                ids.Push(json::Value::MkStr(id));
            }
            json::Value params = json::Value::MkObj();
            params.Set(L"ids", std::move(ids));
            if (!_client->Call(Proto::kOpenChanged, Profiles::detail::WideToUtf8(json::Dump(params)), answer, kCallTimeoutMs, err))
            {
                std::lock_guard lk{ _ptrMtx };
                _client.reset();
                return;
            }
            _lastOpen = std::move(open);
        }

        // 6. The upserts: every session the watcher counted since the last good tick, in ONE request.
        if (!_pending.empty())
        {
            json::Value list = json::Value::MkArr();
            for (const auto& [id, path] : _pending)
            {
                json::Value s = json::Value::MkObj();
                s.Set(L"id", json::Value::MkStr(id));
                s.Set(L"path", json::Value::MkStr(path));
                list.Push(std::move(s));
            }
            json::Value params = json::Value::MkObj();
            params.Set(L"sessions", std::move(list));
            const int64_t t0 = TickMs();
            if (!_client->Call(Proto::kUpsertSessions, Profiles::detail::WideToUtf8(json::Dump(params)), answer, kCallTimeoutMs, err))
            {
                // The ids stay pending: the next tick (on a new connection) sends them again; the cursors make a
                // repeat harmless.
                _setStatus(L"The last refresh failed: " + err);
                std::lock_guard lk{ _ptrMtx };
                _client.reset();
                return;
            }
            const auto env = ParseAnswer(answer);
            const std::wstring failure = env ? ErrorOf(*env) : L"an unreadable answer";
            if (!failure.empty())
            {
                _setStatus(L"The last refresh failed: " + failure);
                return; // kept pending
            }
            int64_t lines = 0;
            size_t failed = 0;
            std::unordered_map<std::wstring, std::wstring> keep;
            if (const json::Value* r = env->Find(L"result"))
            {
                if (const json::Value* rows = r->Find(L"sessions"); rows && rows->type == json::Value::Type::Arr)
                {
                    for (const auto& row : rows->arr)
                    {
                        lines += row.I64At(L"lines");
                        if (!row.BoolAt(L"ok", true))
                        {
                            // A session whose own transaction failed is retried; a bad id is not.
                            const std::wstring id = row.StrAt(L"id");
                            if (row.StrAt(L"error") != L"not a session id")
                            {
                                if (const auto it = _pending.find(id); it != _pending.end())
                                {
                                    keep.insert(*it);
                                }
                            }
                            ++failed;
                        }
                    }
                }
            }
            const size_t sent = _pending.size();
            _pending = std::move(keep);
            _setStatus(L"Last refresh: " + std::to_wstring(sent) + L" session(s), " + std::to_wstring(lines) + L" new line(s) in " +
                       std::to_wstring(TickMs() - t0) + L" ms" + (failed ? L" (" + std::to_wstring(failed) + L" failed, retried next tick)" : L""));
        }
        else if (StatusText().empty())
        {
            _setStatus(L"Connected; nothing changed yet.");
        }
    }
}
