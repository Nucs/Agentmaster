// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — ProcessObserver: the Fleet Observer's S-lane (doc/agentmaster/OBSERVER.md §8).
//
// The PULL census/correlation worker. One per process (owned by SharedEngine, next to the
// SessionScanner), it runs a slow heartbeat (~2 s) + on-event survey that: takes ONE Toolhelp
// snapshot, reads each claude.exe's PEB out-of-band (ProcessInspect), classifies ownership via
// AM_SESSION (ours vs an external Windows Terminal), and — for each tab a window publishes in its
// roster — correlates the tab's shell to its claude, resolves the conversation id, and feeds the
// one SessionRegistry (ObserveClaude). It is the always-correct floor beneath the lossy hook push:
// it detects + correlates a claude even when no hook fires, with no shim, no settings, and nothing
// the user can feel (all reads are out-of-band; it NEVER writes to any shell — the invisibility
// invariant).
//
// Thread/condvar shape mirrors SessionScanner. Plain C++ + Win32, no WinRT (the engine lanes never
// touch XAML; the UI lane reads the published snapshots and marshals via the DispatcherQueue).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Activity.h" // CorrelationRow, TabActivityRow, TabRosterEntry, ObservedClaude
#include "TranscriptStore.h" // SessionPresenceRow (the validated presence table the S-lane publishes)

namespace Agentmaster
{
    class SessionRegistry;

    inline constexpr int64_t kObserverHeartbeatMs = 2000; // §8c FULL-survey cadence (the birth-detection floor; preserved by O7's debounce)
    inline constexpr int64_t kObserverFastTickMs = 1000; // O7: cheap (µs) liveness cadence BETWEEN full surveys
    inline constexpr int64_t kObserverCensusKeepaliveMs = 15000; // re-log an unchanged census at most this often

    class ProcessObserver
    {
    public:
        // `amSession` is the engine's per-process ownership stamp (Engine::amSession, O2); the
        // observer classifies a claude RunningApp::Agentmaster iff its AM_SESSION matches this.
        ProcessObserver(std::shared_ptr<SessionRegistry> registry, std::wstring amSession);
        ~ProcessObserver();

        ProcessObserver(const ProcessObserver&) = delete;
        ProcessObserver& operator=(const ProcessObserver&) = delete;

        void Start();
        void Stop() noexcept;
        void Wake() noexcept; // event-driven survey (a roster changed / a known PID died)

        // UI lane -> observer (per window, each probe tick). Thread-safe; REPLACES that window's
        // set, and Wake()s the survey when the set actually changed (so binding latency is ~one
        // survey, not one heartbeat). UnpublishWindow drops a closing window's set (~TerminalPage).
        void PublishRoster(const std::wstring& windowId, std::vector<TabRosterEntry> roster);
        void UnpublishWindow(const std::wstring& windowId);

        // observer -> UI lane (snapshots; copied under the table lock so readers iterate lock-free).
        std::vector<CorrelationRow> Correlation() const;
        std::vector<TabActivityRow> Activity() const;
        // The external (RunningApp::WindowsTerminal) claude census — observe-only, surfaced by the
        // Manager as an "External (N)" group (O6 / §11c). Copy-under-lock like the other tables.
        std::vector<ExternalClaudeRow> External() const;
        // Live-session presence (~/.claude/sessions/<pid>.json), VALIDATED each full survey against
        // the same process snapshot (a stale file — its pid dead or no longer a claude — is dropped;
        // crash leftovers linger on disk). The separation contract (SESSIONS.md §7-Q5): the
        // TranscriptStore owns the raw read; everything live/changing reaches the app through THIS
        // published table (and the per-session `presenceStatus` enrichment via ObserveClaude) —
        // never an ad-hoc UI read. Copy-under-lock like the other tables.
        std::vector<SessionPresenceRow> Presence() const;

    private:
        void _worker() noexcept; // heartbeat + Wake() loop (mirrors SessionScanner::_worker)
        // ONE pass: snapshot -> census -> classify -> correlate roster -> publish. `forcedByWake` (a
        // roster change / pid-death Wake, or the very first survey) bypasses the O7 debounce so the
        // pass always does the full Toolhelp snapshot; an unforced fast tick may cheap-skip (§8c).
        void _surveyOnce(bool forcedByWake);

        std::shared_ptr<SessionRegistry> _registry;
        std::wstring _amSession;

        std::thread _thread;
        std::mutex _mtx; // guards _woken (the condvar wait predicate)
        std::condition_variable _cv;
        std::atomic<bool> _running{ false };
        bool _woken{ false };

        mutable std::mutex _rosterMtx; // guards _rosterByWindow (written by N UI lanes, read by the survey)
        std::unordered_map<std::wstring, std::vector<TabRosterEntry>> _rosterByWindow;

        mutable std::mutex _tableMtx; // guards the published tables (written by the survey, read by UI lanes)
        std::vector<CorrelationRow> _correlation;
        std::vector<TabActivityRow> _activity;
        std::vector<ExternalClaudeRow> _external; // external (WindowsTerminal) claudes (O6)
        std::vector<SessionPresenceRow> _presence; // pid-validated live-session presence (§7-Q5)
        std::vector<uint32_t> _knownPids; // claude pids that correlated this pass (M-lane liveness cross-check)

        // Worker-thread-only census-log throttle (no lock — touched only inside _surveyOnce).
        std::wstring _lastCensusSig;
        int64_t _lastCensusLogMs{ 0 };
        // Worker-thread-only last-seen activity per tab (no lock) — drives the [activity] transition
        // events (log a tab's pwsh -> ClaudeCode -> pwsh moves; O6). Keyed by wtSession, rebuilt each
        // survey so a closed tab's entry drops.
        std::unordered_map<std::wstring, TabActivity> _lastActivityByWt;

        // Worker-thread-only cache (no lock) of an EXTERNAL session's transcript-derived title +
        // gitBranch, keyed by the resolved conversation id, so the per-external transcript head-read
        // happens once (not every survey — the title doesn't change). Grows only with the distinct
        // external sessions seen this run.
        struct ExtInfo
        {
            std::wstring title;
            std::wstring gitBranch;
        };
        std::unordered_map<std::wstring, ExtInfo> _extInfoCache;

        // Worker-thread-only cache (no lock) of a Codex session's resolved id + rollout path + the
        // rollout-derived facts (title / model / effort / sandbox / approval / git branch / timing),
        // keyed by codex PID — so the date-sharded resolution + rollout head-read happen ONCE per
        // codex process (only the mtime is re-stat'd each survey). Pruned to live codex pids.
        // (Phase C1, OBSERVER.md §19-Q3.)
        struct CodexInfo
        {
            std::wstring sessionId;
            std::wstring rolloutPath;
            std::wstring title;
            std::wstring model;
            std::wstring effort;
            std::wstring sandbox;
            std::wstring approvalMode;
            std::wstring gitBranch;
            int64_t createdUnixMs{};
            int64_t lastActivityUnixMs{};
            bool resolved{}; // the rollout was found + head-read once (don't re-resolve a known one)
            // Phase C2 turn-state: a byte cursor into the rollout + the last-derived state. The cursor
            // advances past complete lines each survey (ReadCodexStateDelta), so steady-state is a few
            // KB read per active codex; gated on a changed mtime so an idle codex does zero IO.
            CodexState state{ CodexState::Unknown };
            int64_t rolloutOffset{ 0 };
        };
        std::unordered_map<uint32_t, CodexInfo> _codexInfoByPid;
        std::vector<std::pair<uint32_t, int64_t>> _lastCodexAlive; // (pid,start) of codex seen last full survey — folded into the O7 liveness set so a codex birth/exit forces a full survey

        // Debounce (O7, worker-thread-only, no lock). The FULL Toolhelp survey runs at most every
        // kObserverHeartbeatMs; between full surveys the worker ticks at kObserverFastTickMs and, when
        // the roster is byte-identical AND every correlated (pid,start) pair is still alive, SKIPS the
        // snapshot (a µs liveness check) — steady state is µs. A Wake or a dead correlated claude
        // forces a full survey. Keeping the FULL cadence at the heartbeat preserves birth detection.
        int64_t _lastFullSurveyMs{ 0 };
        std::wstring _lastRosterSig; // (wtSession:shellPid) of the last full survey's merged roster
        std::vector<std::pair<uint32_t, int64_t>> _lastCorrelated; // (pid, startUnixMs) correlated last full survey
        std::unordered_set<uint32_t> _pebDeniedLogged; // pids whose PEB read was denied — logged once (O7)
        std::unordered_set<uint32_t> _guiExcludedLogged; // GUI claude pids (the desktop Electron app) skipped — logged once
        std::unordered_set<uint32_t> _orphanLogged; // orphaned claude pids (host terminal exited) skipped — logged once
        std::unordered_map<std::wstring, std::wstring> _shellCwdCache; // wtSession -> last TRUSTWORTHY shell cwd (survives idle gaps when a pwsh has no child this tick)
    };
}
