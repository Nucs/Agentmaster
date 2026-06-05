// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing). The
// S-lane: a heartbeat survey that reads claude PEBs out-of-band and feeds the registry. See
// ProcessObserver.h / OBSERVER.md §8.
#define NOMINMAX
#include "ProcessObserver.h"

#include "ClaudeSpawn.h" // AppendStateLog
#include "ProcessInspect.h" // SnapshotProcesses / ReadClaudeFacts / FindDescendantByImage / ResolveSessionId / classify
#include "SessionRegistry.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace
{
    using namespace Agentmaster;

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Map a tab's shell image to a coarse activity. (The full taxonomy — busy detection, shell cwd,
    // [activity] transition events — lands in O6; O4 just populates the table.)
    TabActivity ClassifyShellActivity(std::wstring_view image)
    {
        if (ImageNameEq(image, L"pwsh.exe") || ImageNameEq(image, L"powershell.exe"))
        {
            return TabActivity::Powershell;
        }
        if (ImageNameEq(image, L"cmd.exe"))
        {
            return TabActivity::Cmd;
        }
        if (image.empty())
        {
            return TabActivity::Unknown;
        }
        return TabActivity::Other;
    }

    // A 36-char hyphenated UUID (8-4-4-4-12). Used to decide whether a claude's --session-id /
    // --resume argument is an authoritative conversation id (prefer it over cwd->transcript
    // resolution, which can collide when several claudes share an encoded cwd dir).
    bool LooksLikeGuid(std::wstring_view s)
    {
        if (s.size() != 36)
        {
            return false;
        }
        for (size_t i = 0; i < 36; ++i)
        {
            const wchar_t c = s[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != L'-')
                {
                    return false;
                }
            }
            else
            {
                const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
                if (!hex)
                {
                    return false;
                }
            }
        }
        return true;
    }

    // The authoritative conversation id for a correlated claude: an explicit --session-id (fresh) or
    // --resume <guid> wins (collision-free, known even before the transcript exists); otherwise fall
    // back to cwd -> newest transcript (empty until the first prompt — §11d). (§8b)
    std::wstring ResolveObservedId(const ClaudeProcessFacts& f)
    {
        if (LooksLikeGuid(f.sessionIdArg))
        {
            return f.sessionIdArg;
        }
        if (LooksLikeGuid(f.resumeTarget))
        {
            return f.resumeTarget;
        }
        return ResolveSessionId(f.cwd, f.startUnixMs);
    }
}

namespace Agentmaster
{
    ProcessObserver::ProcessObserver(std::shared_ptr<SessionRegistry> registry, std::wstring amSession) :
        _registry{ std::move(registry) },
        _amSession{ std::move(amSession) }
    {
    }

    ProcessObserver::~ProcessObserver()
    {
        Stop();
    }

    void ProcessObserver::Start()
    {
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            return;
        }
        _thread = std::thread([this]() noexcept { _worker(); });
    }

    void ProcessObserver::Stop() noexcept
    {
        if (_running.exchange(false))
        {
            {
                std::lock_guard lk{ _mtx };
                _woken = true;
            }
            _cv.notify_all();
            if (_thread.joinable())
            {
                _thread.join();
            }
        }
    }

    void ProcessObserver::Wake() noexcept
    {
        {
            std::lock_guard lk{ _mtx };
            _woken = true;
        }
        _cv.notify_one();
    }

    void ProcessObserver::PublishRoster(const std::wstring& windowId, std::vector<TabRosterEntry> roster)
    {
        if (windowId.empty())
        {
            return;
        }
        bool changed = false;
        {
            std::lock_guard lk{ _rosterMtx };
            auto& cur = _rosterByWindow[windowId];
            if (cur.size() != roster.size())
            {
                changed = true;
            }
            else
            {
                for (size_t i = 0; i < cur.size(); ++i)
                {
                    if (cur[i].wtSession != roster[i].wtSession || cur[i].shellPid != roster[i].shellPid || cur[i].bound != roster[i].bound)
                    {
                        changed = true;
                        break;
                    }
                }
            }
            cur = std::move(roster);
        }
        if (changed)
        {
            Wake(); // a roster change -> survey now (bind latency ~one survey, not one heartbeat)
        }
    }

    void ProcessObserver::UnpublishWindow(const std::wstring& windowId)
    {
        {
            std::lock_guard lk{ _rosterMtx };
            _rosterByWindow.erase(windowId);
        }
        Wake();
    }

    std::vector<CorrelationRow> ProcessObserver::Correlation() const
    {
        std::lock_guard lk{ _tableMtx };
        return _correlation;
    }

    std::vector<TabActivityRow> ProcessObserver::Activity() const
    {
        std::lock_guard lk{ _tableMtx };
        return _activity;
    }

    void ProcessObserver::_worker() noexcept
    {
        for (;;)
        {
            try
            {
                _surveyOnce();
            }
            catch (...)
            {
            }
            std::unique_lock lk{ _mtx };
            if (!_running.load())
            {
                return;
            }
            // Always heartbeat (unlike the scanner's sleep-forever-when-idle): a claude can be BORN
            // in a tab whose roster never changed (the user typed `claude` into an existing shell),
            // and only a periodic survey catches that. Wake() collapses the wait on a roster change.
            _cv.wait_for(lk, std::chrono::milliseconds(kObserverHeartbeatMs), [this] { return !_running.load() || _woken; });
            _woken = false;
            if (!_running.load())
            {
                return;
            }
        }
    }

    void ProcessObserver::_surveyOnce()
    {
        const int64_t now = NowMs();
        const auto snap = SnapshotProcesses(); // the one ~10 ms call; reused for census + every tab tree

        // 1) Census: read facts + classify ownership for every claude.exe.
        std::unordered_map<uint32_t, ClaudeProcessFacts> factsByPid;
        int ours = 0;
        int external = 0; // a real Windows Terminal's claude (WT_SESSION, no AM_SESSION)
        int other = 0;
        for (const auto& e : snap)
        {
            if (!ImageNameEq(e.image, L"claude.exe"))
            {
                continue;
            }
            ClaudeProcessFacts f = ReadClaudeFacts(e.pid);
            f.parentPid = e.ppid;
            f.runningApp = ClassifyRunningApp(f.amSession, f.wtSession, _amSession);
            switch (f.runningApp)
            {
            case RunningApp::Agentmaster:
                ++ours;
                break;
            case RunningApp::WindowsTerminal:
                ++external;
                break;
            default:
                ++other;
                break;
            }
            factsByPid.emplace(e.pid, std::move(f));
        }

        // Census log: on-change (so a Launch/close is visible immediately) or a slow keepalive (so a
        // soak shows the survey is alive). Signature folds the counts + our claude pids so a swap
        // that keeps the counts equal still logs.
        {
            std::vector<uint32_t> oursPids;
            for (const auto& [pid, f] : factsByPid)
            {
                if (f.runningApp == RunningApp::Agentmaster)
                {
                    oursPids.push_back(pid);
                }
            }
            std::sort(oursPids.begin(), oursPids.end());
            std::wstring sig = std::to_wstring(factsByPid.size()) + L":" + std::to_wstring(ours) + L":" +
                               std::to_wstring(external) + L":" + std::to_wstring(other);
            for (const auto p : oursPids)
            {
                sig += L"," + std::to_wstring(p);
            }
            if (sig != _lastCensusSig || (now - _lastCensusLogMs) >= kObserverCensusKeepaliveMs)
            {
                _lastCensusSig = sig;
                _lastCensusLogMs = now;
                AppendStateLog(L"hooks.log",
                               L"[observer] census claudes=" + std::to_wstring(factsByPid.size()) +
                                   L" ours=" + std::to_wstring(ours) + L" wt=" + std::to_wstring(external) +
                                   L" other=" + std::to_wstring(other) + L"\n");
                // Detail for OUR claudes only (usually few; external ones are just counted — privacy
                // + signal). model/effort are not secrets; cwd is a local path.
                for (const auto p : oursPids)
                {
                    const auto& f = factsByPid[p];
                    AppendStateLog(L"hooks.log",
                                   L"[observer]   ours pid=" + std::to_wstring(p) + L" bg=" + (f.background ? L"1" : L"0") +
                                       L" model=" + f.model + L" effort=" + f.effort + L" cwd=" + f.cwd +
                                       L" wt=" + f.wtSession + L"\n");
                }
            }
        }

        // 2) Merge every window's roster (one entry per tab; a wtSession lives in exactly one window).
        std::unordered_map<std::wstring, TabRosterEntry> roster;
        {
            std::lock_guard lk{ _rosterMtx };
            for (const auto& [win, entries] : _rosterByWindow)
            {
                for (const auto& e : entries)
                {
                    if (e.wtSession.empty())
                    {
                        continue;
                    }
                    auto it = roster.find(e.wtSession);
                    if (it == roster.end())
                    {
                        roster.emplace(e.wtSession, e);
                    }
                    else if (e.bound && !it->second.bound)
                    {
                        it->second = e; // prefer the bound view if (impossibly) duplicated
                    }
                }
            }
        }

        // 3) Per-tab correlation + activity. (Empty until a window publishes a roster — that wiring
        //    is the UI lane in O5; O4 builds the full survey so O5 is just publish + read + bind.)
        std::vector<CorrelationRow> corr;
        std::vector<TabActivityRow> act;
        std::vector<uint32_t> correlatedPids;
        corr.reserve(roster.size());
        act.reserve(roster.size());
        for (const auto& [wtSession, tab] : roster)
        {
            const uint32_t cpid = FindDescendantByImage(snap, tab.shellPid, L"claude.exe");
            if (cpid != 0)
            {
                const auto fit = factsByPid.find(cpid);
                ClaudeProcessFacts f = (fit != factsByPid.end()) ? fit->second : ReadClaudeFacts(cpid);
                if (fit == factsByPid.end())
                {
                    f.runningApp = ClassifyRunningApp(f.amSession, f.wtSession, _amSession);
                }
                const std::wstring sid = ResolveObservedId(f);

                CorrelationRow cr;
                cr.wtSession = wtSession;
                cr.claudePid = cpid;
                cr.cwd = f.cwd;
                cr.sessionId = sid;
                cr.runningApp = f.runningApp;
                cr.alive = true;
                cr.observedUnixMs = now;
                corr.push_back(std::move(cr));

                TabActivityRow ar;
                ar.wtSession = wtSession;
                ar.shellPid = tab.shellPid;
                ar.activity = TabActivity::ClaudeCode;
                ar.image = L"claude.exe";
                ar.cwd = f.cwd;
                ar.busy = false; // refined in O6 (claude mid-turn / has a tool child)
                ar.sessionId = sid;
                ar.observedUnixMs = now;
                act.push_back(std::move(ar));

                correlatedPids.push_back(cpid);

                // Feed the registry ONLY for OUR claudes with a known id (an external WT claude is
                // observe-only — never a managed session; a known-id-less ours waits for its
                // transcript, §11d). ObserveClaude is idempotent + provenance-safe (never sets state).
                if (f.runningApp == RunningApp::Agentmaster && !sid.empty())
                {
                    ObservedClaude o;
                    o.sessionId = sid;
                    o.tabToken = wtSession;
                    o.amSession = f.amSession;
                    o.cwd = f.cwd;
                    o.pid = cpid;
                    o.runningApp = f.runningApp;
                    o.background = f.background;
                    o.model = f.model;
                    o.effort = f.effort;
                    o.permissionMode = f.permissionMode;
                    o.sessionName = f.sessionName;
                    o.observedUnixMs = now;
                    _registry->ObserveClaude(o);
                }
                continue;
            }

            // No claude under this tab: a Codex acknowledge, else classify the shell image.
            TabActivityRow ar;
            ar.wtSession = wtSession;
            ar.shellPid = tab.shellPid;
            ar.observedUnixMs = now;
            if (FindDescendantByImage(snap, tab.shellPid, L"codex.exe") != 0)
            {
                ar.activity = TabActivity::Codex;
                ar.image = L"codex.exe";
            }
            else
            {
                std::wstring shellImage;
                for (const auto& e : snap)
                {
                    if (e.pid == tab.shellPid)
                    {
                        shellImage = e.image;
                        break;
                    }
                }
                ar.activity = ClassifyShellActivity(shellImage);
                ar.image = shellImage;
                // shell cwd + busy: an O6 refinement (a pwsh process cwd is stale anyway).
            }
            act.push_back(std::move(ar));
        }

        // 4) Publish the snapshots (copy-out readers hold no lock while iterating).
        {
            std::lock_guard lk{ _tableMtx };
            _correlation.swap(corr);
            _activity.swap(act);
            _knownPids.swap(correlatedPids);
        }
    }
}
