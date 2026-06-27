// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

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

    // Stat a file's creation + last-write times as Unix ms (Codex rollout timing refresh — the
    // mtime ticks as codex appends, so the per-pid cache re-stats cheaply each survey). false if
    // the file is absent/a directory. (Phase C1.)
    bool FileTimesOf(const std::wstring& path, int64_t& createdMs, int64_t& lastMs)
    {
        createdMs = 0;
        lastMs = 0;
        if (path.empty())
        {
            return false;
        }
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            return false;
        }
        const auto toMs = [](const FILETIME& ft) -> int64_t {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            constexpr uint64_t kEpoch = 116444736000000000ull; // 1601 -> 1970 in 100ns
            return u.QuadPart < kEpoch ? 0 : static_cast<int64_t>((u.QuadPart - kEpoch) / 10000ull);
        };
        createdMs = toMs(fad.ftCreationTime);
        lastMs = toMs(fad.ftLastWriteTime);
        return true;
    }

    // A short label for a TabActivity (the [activity] event + any logging). (O6)
    const wchar_t* ActivityName(TabActivity a)
    {
        switch (a)
        {
        case TabActivity::Powershell:
            return L"pwsh";
        case TabActivity::Cmd:
            return L"cmd";
        case TabActivity::ClaudeCode:
            return L"claude";
        case TabActivity::Codex:
            return L"codex";
        case TabActivity::Other:
            return L"other";
        case TabActivity::Unknown:
        default:
            return L"unknown";
        }
    }

    // First label of a WT_SESSION GUID for terse logs ("e9c916dd-...") — full ids are long + noisy.
    std::wstring ShortWt(const std::wstring& wt)
    {
        const auto dash = wt.find(L'-');
        return dash == std::wstring::npos ? wt : wt.substr(0, dash);
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

    std::vector<ExternalClaudeRow> ProcessObserver::External() const
    {
        std::lock_guard lk{ _tableMtx };
        return _external;
    }

    std::vector<SessionPresenceRow> ProcessObserver::Presence() const
    {
        std::lock_guard lk{ _tableMtx };
        return _presence;
    }

    void ProcessObserver::_worker() noexcept
    {
        bool forced = true; // the first survey is always a full one (no prior state to debounce against)
        for (;;)
        {
            try
            {
                _surveyOnce(forced);
            }
            catch (...)
            {
            }
            std::unique_lock lk{ _mtx };
            if (!_running.load())
            {
                return;
            }
            // Tick on the FAST cadence (cheap µs liveness between full surveys; O7). The FULL survey
            // still runs at the heartbeat — a claude can be BORN in a tab whose roster never changed
            // (the user typed `claude` into an existing shell), and only a periodic full survey catches
            // that. A Wake (roster change / a known PID died) collapses the wait AND forces the next
            // pass to be full, so a fresh claude / a closed tab is caught within ~one fast tick.
            _cv.wait_for(lk, std::chrono::milliseconds(kObserverFastTickMs), [this] { return !_running.load() || _woken; });
            forced = _woken; // woke early == something changed -> force a full survey
            _woken = false;
            if (!_running.load())
            {
                return;
            }
        }
    }

    int64_t ProcessObserver::_LineDerivedLastActivity(std::wstring_view cwd, const std::wstring& sid, int64_t mtimeMs)
    {
        auto& c = _lineActivityBySid[sid];
        if (c.mtime != mtimeMs) // transcript grew (or first sight) -> re-derive from the tail; else reuse
        {
            c.mtime = mtimeMs;
            c.lastActivityMs = ReadTranscriptLastActivityTail(cwd, sid);
        }
        // 0 == no timestamped conversation line in the window (a never-prompted / unreadable transcript)
        // -> fall back to the mtime, preserving the old behavior for that degenerate case.
        return c.lastActivityMs != 0 ? c.lastActivityMs : mtimeMs;
    }

    void ProcessObserver::_surveyOnce(bool forcedByWake)
    {
        const int64_t now = NowMs();

        // O7 debounce. Compute the merged-roster signature (cheap — roster lock only) and decide if
        // this pass needs the FULL Toolhelp snapshot: forced (a Wake / the first pass), the roster
        // changed, or kObserverHeartbeatMs elapsed since the last full survey (the birth-detection
        // floor — catches a claude typed into a stable tab). Otherwise, on a fast tick, if every
        // correlated (pid,start) is still alive, SKIP the snapshot (steady-state µs); if one died or
        // its PID was reused, fall through to a full survey to reclassify.
        std::wstring rosterSig;
        {
            std::lock_guard lk{ _rosterMtx };
            std::vector<std::wstring> entries;
            for (const auto& [win, tabs] : _rosterByWindow)
            {
                for (const auto& t : tabs)
                {
                    if (!t.wtSession.empty())
                    {
                        entries.push_back(t.wtSession + L":" + std::to_wstring(t.shellPid));
                    }
                }
            }
            std::sort(entries.begin(), entries.end());
            for (const auto& e : entries)
            {
                rosterSig += e;
                rosterSig += L";";
            }
        }
        const bool dueFull = forcedByWake || rosterSig != _lastRosterSig || (now - _lastFullSurveyMs) >= kObserverHeartbeatMs;
        if (!dueFull)
        {
            bool allAlive = true;
            for (const auto& [pid, start] : _lastCorrelated)
            {
                if (start == 0 || ProcessStartUnixMs(pid) != start) // dead (-> 0) or PID reused (-> different start)
                {
                    allAlive = false;
                    break;
                }
            }
            // Codex liveness too (Phase C1): a codex exit/birth must reclassify its External row.
            for (const auto& [pid, start] : _lastCodexAlive)
            {
                if (!allAlive)
                {
                    break;
                }
                if (start == 0 || ProcessStartUnixMs(pid) != start)
                {
                    allAlive = false;
                }
            }
            if (allAlive)
            {
                return; // nothing changed -> keep the last published tables; µs cost
            }
        }

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
            // The Claude DESKTOP app (an Electron GUI binary ALSO named Claude.exe) and its
            // renderer/gpu/utility/crashpad children are NOT Claude Code sessions — they run with cwd
            // C:\WINDOWS\system32 and would otherwise surface in the External group as bogus "system32
            // sessions". Tell them apart by PE subsystem (console CLI vs GUI app) and skip the GUI ones
            // entirely (out of factsByPid -> no External row, no census, never correlated). (OBSERVER.md §5a)
            if (IsClaudeDesktopGuiApp(f))
            {
                if (_guiExcludedLogged.insert(e.pid).second)
                {
                    AppendStateLog(L"hooks.log", L"[observer] skipping GUI claude (desktop app, not a CLI session) pid=" + std::to_wstring(e.pid) + L" cwd=" + f.cwd + L"\n");
                }
                continue;
            }
            f.runningApp = ClassifyRunningApp(f.amSession, f.wtSession, _amSession);
            // O7: a fully-empty PEB read (cwd AND commandline) means a denied / elevated /
            // cross-integrity (or WOW64) target — it can't be correlated/bound, so it stays
            // observe-only. Log ONCE per pid so a soak shows why an elevated claude never binds,
            // without spamming every survey. (ProcessInspect already guards the read -> empty, not garbage.)
            if (f.cwd.empty() && f.commandline.empty() && _pebDeniedLogged.insert(e.pid).second)
            {
                AppendStateLog(L"hooks.log", L"[observer] PEB read denied pid=" + std::to_wstring(e.pid) + L" (elevated/cross-integrity/WOW64?) -> observe-only\n");
            }
            factsByPid.emplace(e.pid, std::move(f));
        }

        // 1a) Codex (Phase C1, OBSERVER.md §19-Q3): read every codex.exe's facts out-of-band (no
        //     GUI-desktop sibling to exclude, unlike claude). Classified ours/external by AM_SESSION
        //     like claude — but Codex is OBSERVE-ONLY, so it is never fed to ObserveClaude and never
        //     bound; it is surfaced as an External row, enriched from its rollout. `getCodexInfo`
        //     resolves the date-sharded rollout (+ reads model/effort/sandbox/approval/title) ONCE per
        //     codex PID (cached, mtime re-stat'd each survey) — shared by the per-tab badge + the census.
        std::unordered_map<uint32_t, CodexProcessFacts> codexByPid;
        for (const auto& e : snap)
        {
            if (!ImageNameEq(e.image, L"codex.exe"))
            {
                continue;
            }
            CodexProcessFacts f = ReadCodexFacts(e.pid);
            f.parentPid = e.ppid;
            f.runningApp = ClassifyRunningApp(f.amSession, f.wtSession, _amSession);
            codexByPid.emplace(e.pid, std::move(f));
        }
        const auto getCodexInfo = [this](const CodexProcessFacts& f) -> CodexInfo {
            auto it = _codexInfoByPid.find(f.pid);
            // PID-reuse guard: only reuse a RESOLVED entry when it was resolved for THIS exact process
            // (same start time). The map is pruned only to live codex pids, so a recycled pid can carry a
            // dead codex's resolved entry here; trusting it would serve the wrong rollout uuid/state.
            // start==0 (a failed read) leaves the entry's start at 0 and matches the same failed read, so
            // it degrades to the prior pid-only behavior rather than thrashing. (An UNRESOLVED entry is
            // not taken by this branch — it re-resolves every survey and overwrites below, self-healing.)
            if (it != _codexInfoByPid.end() && it->second.resolved && it->second.startUnixMs == f.startUnixMs)
            {
                // Known rollout: refresh the mtime (codex appends as it works); no re-resolve. When the
                // file GREW since last survey, advance the turn-state cursor over the new delta (Phase
                // C2 — gated on the mtime so a quiescent codex does zero IO).
                if (!it->second.rolloutPath.empty())
                {
                    int64_t c = 0, l = 0;
                    if (FileTimesOf(it->second.rolloutPath, c, l))
                    {
                        if (c)
                        {
                            it->second.createdUnixMs = c;
                        }
                        const bool grew = (l != 0 && l != it->second.lastActivityUnixMs);
                        if (l)
                        {
                            it->second.lastActivityUnixMs = l;
                        }
                        if (grew)
                        {
                            it->second.state = ReadCodexStateDelta(it->second.rolloutPath, it->second.rolloutOffset, it->second.state);
                        }
                    }
                }
                return it->second;
            }
            // Resolve once: an explicit `codex resume <guid>` is authoritative; else cwd+start
            // discovery over the date-sharded rollouts. A codex with no rollout yet (never prompted,
            // §11d) stays unresolved and is retried next survey until its first turn writes the file.
            CodexInfo ci;
            ci.startUnixMs = f.startUnixMs; // stamp the process identity so a future PID reuse invalidates this entry (resolved or not)
            const std::wstring home = !f.codexHome.empty() ? f.codexHome : CodexDefaultHome();
            CodexSession sess;
            if (!f.resumeTarget.empty())
            {
                sess.sessionId = f.resumeTarget;
                sess.rolloutPath = ResolveCodexRolloutPathIn(home, f.resumeTarget);
            }
            else
            {
                sess = ResolveCodexSessionIn(home, f.cwd, f.startUnixMs);
            }
            ci.sessionId = sess.sessionId;
            ci.rolloutPath = sess.rolloutPath;
            ci.createdUnixMs = sess.createdUnixMs;
            ci.lastActivityUnixMs = sess.lastActivityUnixMs;
            if (!ci.rolloutPath.empty())
            {
                const auto ri = ReadCodexRolloutInfo(ci.rolloutPath, 131072, 1); // 128 KB head: model + first prompt
                ci.title = ri.title;
                ci.model = ri.model;
                ci.effort = ri.effort;
                ci.sandbox = ri.sandbox;
                ci.approvalMode = ri.approvalMode;
                ci.gitBranch = ri.gitBranch;
                if (ri.createdUnixMs)
                {
                    ci.createdUnixMs = ri.createdUnixMs;
                }
                if (ri.lastActivityUnixMs)
                {
                    ci.lastActivityUnixMs = ri.lastActivityUnixMs;
                }
                ci.resolved = true;
                // Phase C2: seed the turn state on first sight — offset 0 makes ReadCodexStateDelta
                // SEEK to the tail window, so a just-discovered (possibly multi-MB) rollout reports
                // Running/Waiting immediately instead of catching up over many surveys.
                ci.state = ReadCodexStateDelta(ci.rolloutPath, ci.rolloutOffset, ci.state);
            }
            // Command-line facts fill any gaps (model/sandbox/approval can be on the line when not
            // carried by config.toml). They never override the rollout's authoritative values.
            if (ci.model.empty())
            {
                ci.model = f.model;
            }
            if (ci.sandbox.empty())
            {
                ci.sandbox = f.sandbox;
            }
            if (ci.approvalMode.empty())
            {
                ci.approvalMode = f.approvalMode;
            }
            _codexInfoByPid[f.pid] = ci; // cache (resolved=false => re-resolve next survey until the rollout appears)
            return ci;
        };

        // 1b) Live-session presence (SESSIONS.md §7-Q5): the store's RAW read of
        //     ~/.claude/sessions/<pid>.json, validated against THIS snapshot — a row whose pid is
        //     gone, or no longer a claude.exe (stale crash leftovers, PID reuse), is dropped. The
        //     surviving rows are published as a table AND feed the per-session `presenceStatus`
        //     enrichment below (a display FACT — never SessionState, Rule #13).
        std::vector<SessionPresenceRow> presence = ReadSessionPresence();
        presence.erase(std::remove_if(presence.begin(), presence.end(), [&factsByPid](const SessionPresenceRow& r) { return factsByPid.find(r.pid) == factsByPid.end(); }),
                       presence.end());
        std::unordered_map<std::wstring, std::wstring> presenceBySid;
        // pid -> the conversation id Claude is CURRENTLY on (its own heartbeat). Used to BIND a
        // correlated tab to the live conversation rather than the launch-time --session-id on the
        // cmdline, which goes stale when the user /resume / /clear / /compact-s a managed session
        // into a different conversation (the cmdline still shows the spawn id; presence does not).
        // Bind-grade trust is STRICTER than the status enrichment above: the row must belong to the
        // SAME live process — pid alive (already filtered) AND its recorded startedAt within an init
        // window of the OS process-create time — so a stale presence file left by a REUSED pid can
        // never mis-bind. Presence is pid-keyed, so it is immune to the cwd-density mis-bind Rule #14
        // warns of. (See ResolveObservedId + the rostered bind below.)
        std::unordered_map<uint32_t, std::wstring> presenceByPid;
        for (const auto& r : presence)
        {
            presenceBySid[r.sessionId] = r.status;
            if (r.sessionId.empty())
            {
                continue;
            }
            const auto fit = factsByPid.find(r.pid);
            if (fit == factsByPid.end())
            {
                continue; // pid not a live claude this snapshot (already filtered, belt+suspenders)
            }
            const int64_t procStart = fit->second.startUnixMs;
            if (procStart > 0 && (r.startedAtMs < procStart - 5000 || r.startedAtMs > procStart + 60000))
            {
                continue; // presence predates / long-postdates this process -> not its file (PID reuse)
            }
            presenceByPid[r.pid] = r.sessionId;
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
                std::wstring sid = ResolveObservedId(f);
                // Claude's OWN presence heartbeat is the authoritative record of the conversation
                // this pid is CURRENTLY on — it follows /resume, /clear and /compact, which the
                // launch-time --session-id on the cmdline does NOT. When they disagree (a managed
                // session the user resumed/cleared/compacted into a different conversation), the
                // presence id wins, so the tab binds to — and displays the state of — the
                // conversation actually running, not the stale launch id stuck as a phantom record.
                // A brand-new spawn that hasn't written presence yet (or a denied PEB start time)
                // is absent from presenceByPid and falls back to ResolveObservedId (== our launch
                // id, which is correct until a divergence). Matches the CLI's bind precedence.
                if (const auto pit = presenceByPid.find(cpid); pit != presenceByPid.end() && LooksLikeGuid(pit->second))
                {
                    sid = pit->second;
                }
                const std::wstring ownerWin = ownerByWt.count(wtSession) ? ownerByWt[wtSession] : std::wstring{};
                correlatedPids.push_back(cpid);
                rosteredOwner[cpid] = ownerWin; // mark this pid OURS for the census below

                // Conversation timing from the transcript — only once the id is resolved. Drives the
                // Manager's per-session timing adornment. createdUnixMs is the file ctime (file birth ==
                // conversation start — honest). lastActivity is LINE-DERIVED (the last real conversation
                // line), NOT the file mtime: `claude --resume` + mode/permission/cwd changes append
                // untimestamped state lines that bump mtime without being activity, so a restored tab
                // focused after a restart would read "active just now" (it only resumed). The tail read
                // is mtime-gated (a quiet/just-resumed session costs only this stat after it settles).
                int64_t convCreated = 0, convMtime = 0, convLast = 0;
                if (!sid.empty())
                {
                    TranscriptTimes(f.cwd, sid, convCreated, convMtime); // ctime=created, mtime=the gate
                    convLast = _LineDerivedLastActivity(f.cwd, sid, convMtime);
                }

                CorrelationRow cr;
                cr.wtSession = wtSession;
                cr.claudePid = cpid;
                cr.cwd = f.cwd;
                cr.sessionId = sid;
                cr.ownerWindowId = ownerWin;
                cr.runningApp = RunningApp::Agentmaster; // rostered == in our tab == ours (not AM_SESSION-gated)
                cr.alive = true;
                cr.observedUnixMs = now;
                cr.createdUnixMs = convCreated;
                cr.lastActivityUnixMs = convLast;
                corr.push_back(std::move(cr));

                TabActivityRow ar;
                ar.wtSession = wtSession;
                ar.shellPid = tab.shellPid;
                ar.activity = TabActivity::ClaudeCode;
                ar.image = L"claude.exe";
                ar.cwd = f.cwd;
                ar.busy = HasActiveChild(snap, cpid); // O6: claude with a live tool child == mid-turn
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
                    // Git branch — the live writer for SessionInfo::branch (the per-tab overlay's
                    // row-2 "<workdir folder>/<branch>"). Read LIVE from the working dir's .git/HEAD,
                    // i.e. the CURRENT branch — NOT the transcript's recorded gitBranch, which is a
                    // per-line historical snapshot (first-seen) that reads stale, or "HEAD", when the
                    // repo was momentarily detached as that line was written. A tiny filesystem read.
                    o.gitBranch = ReadGitBranchForDir(f.cwd);
                    if (const auto pit = presenceBySid.find(sid); pit != presenceBySid.end())
                    {
                        o.presenceStatus = pit->second; // claude's own heartbeat (busy/idle/waiting/shell)
                    }
                    o.observedUnixMs = now;
                    o.createdUnixMs = convCreated;
                    o.lastActivityUnixMs = convLast;
                    _registry->ObserveClaude(o);
                }
                continue;
            }

            // No claude under this tab: a Codex acknowledge, else classify the shell image.
            std::wstring shellImage;
            for (const auto& e : snap)
            {
                if (e.pid == tab.shellPid)
                {
                    shellImage = e.image;
                    break;
                }
            }
            TabActivityRow ar;
            ar.wtSession = wtSession;
            ar.shellPid = tab.shellPid;
            ar.observedUnixMs = now;
            if (const uint32_t xpid = FindDescendantByImage(snap, tab.shellPid, L"codex.exe"))
            {
                ar.activity = TabActivity::Codex;
                ar.image = L"codex.exe";
                ar.busy = HasActiveChild(snap, xpid); // codex with a live tool child == mid-turn
                // Enrich the observe badge with the model + cwd (Phase C1). getCodexInfo is cached per
                // pid, so this rollout read is amortized. Observe-only — no ObserveClaude, no bind.
                if (const auto cf = codexByPid.find(xpid); cf != codexByPid.end())
                {
                    ar.cwd = cf->second.cwd;
                    const CodexInfo ci = getCodexInfo(cf->second); // resolved + state-advanced once per pid (cached)
                    ar.model = ci.model;
                    ar.codexState = ci.state; // Phase C2: enrich the badge with the turn state
                    ar.sessionId = ci.sessionId; // the resolved rollout uuid -> the UI lane fills a MANAGED codex record's codexSessionId (Codex-launch)
                }
            }
            else
            {
                ar.activity = ClassifyShellActivity(shellImage);
                ar.image = shellImage;
                // O6: a shell running a non-shell foreground command is busy.
                ar.busy = HasNonShellChild(snap, tab.shellPid);
            }
            // Resolve this tab's working dir out-of-band so a reopened shell tab restores where it was
            // (PERSISTENCE.md): cmd's own PEB tracks `cd`; pwsh/powershell freeze their process cwd but
            // pass the live $PWD to native children, so ResolveShellCwd reads the newest child. Cache a
            // TRUSTWORTHY reading per tab so an IDLE pwsh (no child this tick) keeps its last real cwd
            // rather than regressing to the launch dir. Read-only — never writes to a shell (Rule #13).
            {
                const auto sc = ResolveShellCwd(snap, tab.shellPid, shellImage);
                if (sc.reliable && !sc.cwd.empty())
                {
                    _shellCwdCache[wtSession] = sc.cwd;
                    ar.cwd = sc.cwd;
                }
                else if (const auto it = _shellCwdCache.find(wtSession); it != _shellCwdCache.end())
                {
                    ar.cwd = it->second; // idle pwsh: reuse the last trustworthy cwd
                }
                else
                {
                    ar.cwd = sc.cwd; // best-effort (a launch dir) — still better than nothing
                }
            }
            act.push_back(std::move(ar));
        }

        // Drop cached shell cwds for tabs no longer in any window's roster (closed tabs).
        for (auto it = _shellCwdCache.begin(); it != _shellCwdCache.end();)
        {
            it = (roster.find(it->first) == roster.end()) ? _shellCwdCache.erase(it) : std::next(it);
        }

        // 4) Census: classify each claude — rostered (in one of our tabs) -> OURS; else by AM_SESSION
        //    (external Windows Terminal / bare Other). Logged on signature change (a Launch/close is
        //    visible immediately) or a slow keepalive (a soak shows the survey is alive).
        int ours = 0, external = 0, other = 0, orphan = 0;
        std::vector<uint32_t> oursPids;
        std::vector<ExternalClaudeRow> externalRows; // published for the Manager's "External" group (O6)
        // Resolve a claude's host shell leaf from the snapshot (its parent's image) — labels a
        // cmd-/console-hosted external ("cmd.exe", "pwsh.exe", ...). Pure scan over `snap`.
        const auto parentImageOf = [&snap](uint32_t childPid) -> std::wstring {
            uint32_t ppid = 0;
            for (const auto& e : snap)
            {
                if (e.pid == childPid)
                {
                    ppid = e.ppid;
                    break;
                }
            }
            if (ppid == 0)
            {
                return {};
            }
            for (const auto& e : snap)
            {
                if (e.pid == ppid)
                {
                    return e.image;
                }
            }
            return {};
        };
        for (const auto& [pid, f] : factsByPid)
        {
            const RunningApp app = rosteredOwner.count(pid) ? RunningApp::Agentmaster : f.runningApp;
            if (app == RunningApp::Agentmaster)
            {
                ++ours;
                oursPids.push_back(pid);
                continue;
            }
            // Skip ORPHANED claudes: their hosting terminal/ConPTY has EXITED (the parent process is
            // gone), so they are dead, non-interactable sessions — not live externals. Without this
            // they pile up as phantom External rows after dev close→relaunch cycles (a claude can
            // outlive its window — "3 pids for 1 tab"). PID-reuse guard: a still-present parent must
            // have started no later than the claude. (OBSERVER.md §11c)
            bool parentLive = false;
            for (const auto& e : snap)
            {
                if (e.pid == f.parentPid)
                {
                    parentLive = (f.startUnixMs == 0) || (ProcessStartUnixMs(f.parentPid) <= f.startUnixMs + 2000);
                    break;
                }
            }
            if (!parentLive)
            {
                ++orphan;
                if (_orphanLogged.insert(pid).second)
                {
                    AppendStateLog(L"hooks.log", L"[observer] skipping orphaned claude (host terminal exited) pid=" + std::to_wstring(pid) + L" cwd=" + f.cwd + L"\n");
                }
                continue;
            }
            // External (observe-only): a WT-hosted claude (WindowsTerminal) OR a bare-console /
            // cmd-hosted one (Other / Unknown). Both are surfaced in the Manager's External group,
            // enriched out-of-band with the conversation id + transcript timing + a title.
            if (app == RunningApp::WindowsTerminal)
            {
                ++external;
            }
            else
            {
                ++other;
            }
            ExternalClaudeRow ex;
            ex.pid = pid;
            ex.hostPid = f.parentPid ? f.parentPid : pid; // the host shell (claude's parent) — the UI's "same window/tab" color key (fall back to self when the parent is unknown)
            ex.wtSession = f.wtSession;
            ex.cwd = f.cwd;
            ex.model = f.model;
            ex.effort = f.effort;
            ex.background = f.background;
            ex.startUnixMs = f.startUnixMs;
            ex.observedUnixMs = now;
            ex.host = app; // WindowsTerminal vs Other/Unknown (cmd / bare console)
            if (app != RunningApp::WindowsTerminal)
            {
                ex.hostImage = parentImageOf(pid); // cmd.exe / pwsh.exe / ... (WT rows leave this empty)
            }
            // Resolve a CLEAR host display label by the hosting terminal's identity (package family /
            // image path), so an external claude in another Agentmaster instance reads "Agentmaster" /
            // "Agentmaster Dev" instead of "WindowsTerminal" (our fork's exe leaf), and the real
            // Windows Terminal reads "Windows Terminal" — not lumped together. (OBSERVER.md §11c)
            ex.hostLabel = ResolveExternalHostLabel(snap, pid, !f.amSession.empty());
            const std::wstring sid = ResolveObservedId(f);
            ex.sessionId = sid;
            if (!sid.empty())
            {
                // extMtime = the raw transcript mtime — the gate for the recap re-read below AND the
                // fallback for the line-derived last-activity. ex.lastActivityUnixMs itself is set to
                // the LINE-DERIVED value at the end of this block (not the mtime), for the same reason
                // as the managed path: `claude --resume` / mode / cwd changes bump mtime without being
                // activity, so an idle external would otherwise read "active just now".
                int64_t extMtime = 0;
                TranscriptTimes(f.cwd, sid, ex.createdUnixMs, extMtime);
                auto cached = _extInfoCache.find(sid);
                if (cached == _extInfoCache.end())
                {
                    // One-time transcript head read (128 KB) for the title + gitBranch — both are
                    // write-once facts, so they are cached for the session's life. The ONE precedence
                    // (TranscriptDisplayTitle): the user-SET custom-title > the picker's ai-title >
                    // legacy summary > the first REAL prompt — the custom title is the user's own label,
                    // and typically what a renamed hosting tab says too. (A retitle past the 128 KB head
                    // is missed — best-effort; the Bring-Window-To-Front worker re-reads deeper for its
                    // tab match.)
                    const auto ti = ReadTranscriptInfo(f.cwd, sid, 131072, 1);
                    cached = _extInfoCache.emplace(sid, ExtInfo{ TranscriptDisplayTitle(ti), ti.gitBranch, L"", 0 }).first;
                }
                ex.title = cached->second.title;
                ex.gitBranch = cached->second.gitBranch;

                // Agentmaster (the observer as RECAP PROVIDER for externals): the idle RECAP
                // (away_summary) is NOT a write-once fact — a fresh one is appended each time the session
                // re-idles — so unlike title/branch it is re-read whenever the transcript GREW (mtime
                // advanced). We pull it from the transcript TAIL (ReadTranscriptRecapTail), the SAME
                // REGION the SessionScanner's byte-cursor delta pulls a MANAGED session's recap from; an
                // external has no scanner cursor, so the observer is its recap provider. mtime-gated: an
                // IDLE external (no growth) costs ONE stat (TranscriptTimes, above) and ZERO content reads;
                // an empty tail (no away_summary) never clears a captured recap — the scanner's "empty
                // never clears" rule, here inside RecapFromTranscriptChunk. ObserveClaude is never used for
                // this (it sets no SessionState, Rule #13); the recap rides ExternalClaudeRow as a
                // transient display fact.
                //
                // Window: FIRST sight reads a DEEP tail (4 MiB) to catch a recap written BEFORE we started
                // watching — a long, multi-recap session can bury its last recap far above EOF (measured
                // over the live corpus: median last-recap sits ~1 KiB from EOF, but ~9% are >128 KiB deep,
                // max ~1.4 MiB). Steady-state reads (mtime already seen once) use the shallow 128 KiB tail
                // — the agentmaster-cli `show` window — because a FRESH recap always lands at the very tail,
                // so the shallow read catches every NEW one while a buried prior recap stays cached. So the
                // deep read is one-time-per-external (like the title head read), not per tick.
                if (cached->second.recapMtime != extMtime)
                {
                    const size_t window = (cached->second.recapMtime == 0) ? (4u << 20) : 131072;
                    cached->second.recapMtime = extMtime;
                    if (std::wstring r = ReadTranscriptRecapTail(f.cwd, sid, window); !r.empty())
                    {
                        cached->second.recap = std::move(r);
                    }
                }
                ex.recap = cached->second.recap;

                // Line-derived last-activity (NOT the lying mtime). mtime-gated via _lineActivityBySid,
                // so an idle external costs only the TranscriptTimes stat above after the one settling read.
                ex.lastActivityUnixMs = _LineDerivedLastActivity(f.cwd, sid, extMtime);
            }
            externalRows.push_back(std::move(ex));
        }

        // 4c) Codex census (Phase C1, OBSERVER.md §19-Q3): EVERY codex.exe is observe-only, so it is
        //     surfaced as an External row (kind=Codex) regardless of host — even one in OUR own tab
        //     (Codex is never adopted/driven in C1). Same orphan skip + host labeling as the claude
        //     census; enriched from its rollout via getCodexInfo. NOT fed to ObserveClaude.
        // Managed Codex (launched / restored / adopted BY US) carry a registry record (kind=Codex,
        // live) whose tabToken == their WT_SESSION; they surface as managed Triage-Board cards, so
        // EXCLUDE them from the observe-only External census — else a launched codex DOUBLE-shows (a
        // managed card AND an external row). A hand-typed codex in our tab has no such record and stays
        // observe-only (External). (Codex-launch.)
        std::unordered_set<std::wstring> managedCodexTokens;
        for (const auto& s : _registry->Snapshot())
        {
            if (s.kind == AgentKind::Codex && s.live && !s.external && !s.tabToken.empty())
            {
                managedCodexTokens.insert(s.tabToken);
            }
        }
        int codexCount = 0;
        for (const auto& [pid, f] : codexByPid)
        {
            if (!f.wtSession.empty() && managedCodexTokens.count(f.wtSession))
            {
                continue; // managed -> a Triage-Board card, not an observe-only External row
            }
            // Orphan skip (mirror the claude census): a codex whose host shell/terminal has exited is
            // a dead session, not a live external. PID-reuse guard: a present parent started no later.
            bool parentLive = false;
            for (const auto& e : snap)
            {
                if (e.pid == f.parentPid)
                {
                    parentLive = (f.startUnixMs == 0) || (ProcessStartUnixMs(f.parentPid) <= f.startUnixMs + 2000);
                    break;
                }
            }
            if (!parentLive)
            {
                if (_orphanLogged.insert(pid).second)
                {
                    AppendStateLog(L"hooks.log", L"[observer] skipping orphaned codex (host exited) pid=" + std::to_wstring(pid) + L" cwd=" + f.cwd + L"\n");
                }
                continue;
            }
            ++codexCount;
            const CodexInfo ci = getCodexInfo(f);
            ExternalClaudeRow ex;
            ex.kind = AgentKind::Codex;
            ex.pid = pid;
            ex.hostPid = f.parentPid ? f.parentPid : pid; // the host shell (codex's parent) — the "same window/tab" color key
            ex.wtSession = f.wtSession;
            ex.cwd = f.cwd;
            ex.model = ci.model;
            ex.effort = ci.effort;
            ex.sandbox = ci.sandbox;
            ex.approvalMode = ci.approvalMode;
            ex.startUnixMs = f.startUnixMs;
            ex.observedUnixMs = now;
            ex.host = f.runningApp; // WindowsTerminal / Agentmaster(-Dev) / Other (cmd-hosted)
            if (f.runningApp != RunningApp::WindowsTerminal)
            {
                ex.hostImage = parentImageOf(pid);
            }
            ex.hostLabel = ResolveExternalHostLabel(snap, pid, !f.amSession.empty());
            ex.sessionId = ci.sessionId;
            ex.rolloutPath = ci.rolloutPath; // date-sharded — carried so the read-only plan + "open rollout" need no re-resolve
            ex.title = ci.title;
            ex.gitBranch = ci.gitBranch;
            ex.createdUnixMs = ci.createdUnixMs;
            ex.lastActivityUnixMs = ci.lastActivityUnixMs;
            ex.codexState = ci.state; // Phase C2: rollout-tail-derived turn state -> the row's state dot
            externalRows.push_back(std::move(ex));
        }

        std::sort(externalRows.begin(), externalRows.end(), [](const ExternalClaudeRow& a, const ExternalClaudeRow& b) { return a.pid < b.pid; });
        std::sort(oursPids.begin(), oursPids.end());
        {
            // Census log gating — signal over noise. The re-log fires ONLY when OUR fleet changes:
            // the set of our claudes OR any of their identifying facts (rostered / bg / model /
            // effort / cwd / wt / win — exactly what the detail block below prints). EXTERNAL-world
            // churn — unrelated claudes/codex starting and dying (`wt`/`other`/`orphan`/`codex`/the
            // total) — NO LONGER triggers a re-log: it is observe-only UI, not signal worth a line,
            // and on a busy box (dozens of unrelated claudes) it was the bulk of the spam, together
            // with a too-short keepalive. The summary line still PRINTS the full live counts as
            // context (they're just not what TRIGGERS the re-log), and the keepalive still snapshots
            // the whole fleet periodically. (Was: ANY count change + a 15 s keepalive re-emitted the
            // whole summary+detail block — ~34% of a 65 MB log was this one census.)
            std::wstring sig;
            for (const auto p : oursPids)
            {
                const auto& f = factsByPid[p];
                const bool rostered = rosteredOwner.count(p) != 0;
                const std::wstring win = rostered ? rosteredOwner[p] : WindowIdFromAmSession(f.amSession);
                sig += std::to_wstring(p) + L"|" + (rostered ? L"r" : L"s") + L"|" + (f.background ? L"1" : L"0") +
                       L"|" + f.model + L"|" + f.effort + L"|" + f.cwd + L"|" + f.wtSession + L"|" + win + L";";
            }
            if (sig != _lastCensusSig || (now - _lastCensusLogMs) >= kObserverCensusKeepaliveMs)
            {
                _lastCensusSig = sig;
                _lastCensusLogMs = now;
                AppendStateLog(L"hooks.log",
                               L"[observer] census claudes=" + std::to_wstring(factsByPid.size()) +
                                   L" ours=" + std::to_wstring(ours) + L" wt=" + std::to_wstring(external) +
                                   L" other=" + std::to_wstring(other) + L" orphan=" + std::to_wstring(orphan) +
                                   L" codex=" + std::to_wstring(codexCount) +
                                   L" rostered=" + std::to_wstring(correlatedPids.size()) + L"\n");
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

        // 4b) [activity] transition events (O6): log a tab whose activity KIND changed since last
        //     survey (pwsh -> ClaudeCode -> pwsh). Keyed by wtSession; `busy` rides the line as info
        //     but does NOT trigger an event (it flips fast as claude spawns tools). The map is
        //     rebuilt each survey so a closed tab's entry drops. Worker-thread-only (no lock).
        {
            std::unordered_map<std::wstring, TabActivity> nextActivity;
            nextActivity.reserve(act.size());
            for (const auto& a : act)
            {
                nextActivity[a.wtSession] = a.activity;
                const auto prev = _lastActivityByWt.find(a.wtSession);
                const bool isNew = (prev == _lastActivityByWt.end());
                const bool changed = isNew ? (a.activity != TabActivity::Unknown) : (prev->second != a.activity);
                if (changed)
                {
                    const std::wstring from = isNew ? std::wstring{ L"-" } : std::wstring{ ActivityName(prev->second) };
                    AppendStateLog(L"hooks.log",
                                   L"[activity] wt=" + ShortWt(a.wtSession) + L" " + from + L" -> " + ActivityName(a.activity) +
                                       L" image=" + a.image + L" busy=" + (a.busy ? L"1" : L"0") + L"\n");
                }
            }
            _lastActivityByWt.swap(nextActivity);
        }

        // O7 debounce bookkeeping: remember this FULL survey's roster signature + correlated
        // (pid, start) so the next fast tick can cheap-skip when nothing changed. (Captured BEFORE the
        // publish swap empties correlatedPids.)
        _lastFullSurveyMs = now;
        _lastRosterSig = rosterSig;
        _lastCorrelated.clear();
        _lastCorrelated.reserve(correlatedPids.size());
        for (const auto pid : correlatedPids)
        {
            const auto it = factsByPid.find(pid);
            _lastCorrelated.emplace_back(pid, it != factsByPid.end() ? it->second.startUnixMs : 0);
        }
        // Codex liveness set + prune the per-pid codex cache to live codex pids (Phase C1; bounds the
        // cache across a long run, and lets a codex birth/exit collapse the fast-tick skip above).
        _lastCodexAlive.clear();
        _lastCodexAlive.reserve(codexByPid.size());
        for (const auto& [pid, f] : codexByPid)
        {
            _lastCodexAlive.emplace_back(pid, f.startUnixMs);
        }
        for (auto it = _codexInfoByPid.begin(); it != _codexInfoByPid.end();)
        {
            it = (codexByPid.find(it->first) == codexByPid.end()) ? _codexInfoByPid.erase(it) : std::next(it);
        }

        // 5) Publish the snapshots (copy-out readers hold no lock while iterating).
        {
            std::lock_guard lk{ _tableMtx };
            _correlation.swap(corr);
            _activity.swap(act);
            _external.swap(externalRows);
            _presence.swap(presence);
            _knownPids.swap(correlatedPids);
        }
    }
}
