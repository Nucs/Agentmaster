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

        // 1) Read facts (PEB cwd/cmdline/env) + an initial AM_SESSION-based classification for every
        //    claude.exe. The classification is REFINED below: a claude correlated to one of OUR tabs
        //    is ours regardless of AM_SESSION (a `+`-tab claude does not reliably inherit it — WT
        //    regenerates a tab's env), so the roster correlation is the authoritative bind signal and
        //    AM_SESSION is only the secondary signal for the census of NON-rostered claudes.
        std::unordered_map<uint32_t, ClaudeProcessFacts> factsByPid;
        for (const auto& e : snap)
        {
            if (!ImageNameEq(e.image, L"claude.exe"))
            {
                continue;
            }
            ClaudeProcessFacts f = ReadClaudeFacts(e.pid);
            f.parentPid = e.ppid;
            f.runningApp = ClassifyRunningApp(f.amSession, f.wtSession, _amSession);
            factsByPid.emplace(e.pid, std::move(f));
        }

        // 2) Merge every window's roster (one entry per tab; a wtSession lives in exactly one
        //    window). Track the owning windowId per tab (the _rosterByWindow key) — this is the
        //    robust per-window attribution that covers BOTH Launched and hand-typed claudes (§19-Q1).
        std::unordered_map<std::wstring, TabRosterEntry> roster;
        std::unordered_map<std::wstring, std::wstring> ownerByWt;
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
                        ownerByWt.emplace(e.wtSession, win);
                    }
                    else if (e.bound && !it->second.bound)
                    {
                        it->second = e; // prefer the bound view if (impossibly) duplicated
                        ownerByWt[e.wtSession] = win;
                    }
                }
            }
        }

        // 3) Per-tab correlation + activity. A claude correlated to a tab in OUR roster is running
        //    in one of our windows -> it is OURS (this Agentmaster process), regardless of AM_SESSION
        //    (which a `+`-tab claude does not inherit). So the roster correlation IS the bind signal.
        std::vector<CorrelationRow> corr;
        std::vector<TabActivityRow> act;
        std::vector<uint32_t> correlatedPids;
        std::unordered_map<uint32_t, std::wstring> rosteredOwner; // claude pid -> owning windowId (== ours)
        corr.reserve(roster.size());
        act.reserve(roster.size());
        for (const auto& [wtSession, tab] : roster)
        {
            const uint32_t cpid = FindDescendantByImage(snap, tab.shellPid, L"claude.exe");
            if (cpid != 0)
            {
                const auto fit = factsByPid.find(cpid);
                const ClaudeProcessFacts f = (fit != factsByPid.end()) ? fit->second : ReadClaudeFacts(cpid);
                const std::wstring sid = ResolveObservedId(f);
                const std::wstring ownerWin = ownerByWt.count(wtSession) ? ownerByWt[wtSession] : std::wstring{};
                correlatedPids.push_back(cpid);
                rosteredOwner[cpid] = ownerWin; // mark this pid OURS for the census below

                CorrelationRow cr;
                cr.wtSession = wtSession;
                cr.claudePid = cpid;
                cr.cwd = f.cwd;
                cr.sessionId = sid;
                cr.ownerWindowId = ownerWin;
                cr.runningApp = RunningApp::Agentmaster; // rostered == in our tab == ours (not AM_SESSION-gated)
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

                // Feed the registry once this OUR claude has a conversation id (a never-prompted one
                // has none yet, §11d). Idempotent + provenance-safe (ObserveClaude never sets state).
                if (!sid.empty())
                {
                    ObservedClaude o;
                    o.sessionId = sid;
                    o.tabToken = wtSession;
                    o.amSession = f.amSession;
                    o.ownerWindowId = !ownerWin.empty() ? ownerWin : WindowIdFromAmSession(f.amSession);
                    o.cwd = f.cwd;
                    o.pid = cpid;
                    o.runningApp = RunningApp::Agentmaster;
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

        // 4) Census: classify each claude — rostered (in one of our tabs) -> OURS; else by AM_SESSION
        //    (external Windows Terminal / bare Other). Logged on signature change (a Launch/close is
        //    visible immediately) or a slow keepalive (a soak shows the survey is alive).
        int ours = 0, external = 0, other = 0;
        std::vector<uint32_t> oursPids;
        for (const auto& [pid, f] : factsByPid)
        {
            const RunningApp app = rosteredOwner.count(pid) ? RunningApp::Agentmaster : f.runningApp;
            if (app == RunningApp::Agentmaster)
            {
                ++ours;
                oursPids.push_back(pid);
            }
            else if (app == RunningApp::WindowsTerminal)
            {
                ++external;
            }
            else
            {
                ++other;
            }
        }
        std::sort(oursPids.begin(), oursPids.end());
        {
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
                                   L" other=" + std::to_wstring(other) + L" rostered=" + std::to_wstring(correlatedPids.size()) + L"\n");
                // Detail for OUR claudes only (privacy + signal; model/effort aren't secrets, cwd is
                // local). `rostered` == bound to one of our tabs; `stamp` == identified by AM_SESSION.
                for (const auto p : oursPids)
                {
                    const auto& f = factsByPid[p];
                    const bool rostered = rosteredOwner.count(p) != 0;
                    const std::wstring win = rostered ? rosteredOwner[p] : WindowIdFromAmSession(f.amSession);
                    AppendStateLog(L"hooks.log",
                                   L"[observer]   ours pid=" + std::to_wstring(p) + L" " + (rostered ? L"rostered" : L"stamp") +
                                       L" bg=" + (f.background ? L"1" : L"0") + L" model=" + f.model + L" effort=" + f.effort +
                                       L" cwd=" + f.cwd + L" wt=" + f.wtSession + (win.empty() ? L"" : (L" win=" + win)) + L"\n");
                }
            }
        }

        // 5) Publish the snapshots (copy-out readers hold no lock while iterating).
        {
            std::lock_guard lk{ _tableMtx };
            _correlation.swap(corr);
            _activity.swap(act);
            _knownPids.swap(correlatedPids);
        }
    }
}
