// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
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
    inline constexpr int64_t kObserverCensusKeepaliveMs = 300000; // re-log an UNCHANGED census at most this often (5 min): a periodic fleet snapshot, NOT a per-tick drip (was 15 s — the bulk of the log spam alongside external-churn-triggered re-logs, now gated to OUR fleet only)

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
        // happens once (not every survey — the title doesn't change). Pruned at each full survey's
        // tail to the external sids that survey published — a vanished external's cache is dead
        // weight (and a momentarily-unresolved sid just re-reads once), so it no longer grows with
        // every external ever seen this run.
        //
        // Agentmaster: the cache also holds the idle RECAP (away_summary) — but unlike title/gitBranch
        // the recap is NOT a write-once fact (a new one appears each time the session re-idles), so it
        // is re-read from the transcript TAIL whenever the file mtime advances (recapMtime tracks the
        // mtime the recap was last read at). This is the external analog of the SessionScanner mirroring
        // a MANAGED session's recap from its delta cursor — same region (the tail), but mtime-gated here
        // since the observer keeps no per-session byte cursor. recapMtime 0 == not yet read.
        struct ExtInfo
        {
            std::wstring title;
            std::wstring gitBranch;
            std::wstring recap; // last away_summary seen in the tail ("empty never clears")
            // Agentmaster (current-model adornment): the CURRENT model — the tail's newest real
            // assistant message.model, read from the SAME mtime-gated tail pull as the recap
            // (ReadTranscriptTailFacts: one read, two facts). "empty never clears": a shallow
            // steady-state tail that carries no assistant line (all tool_results / user lines)
            // keeps the captured model. Feeds ExternalClaudeRow.currentModel.
            std::wstring model;
            int64_t recapMtime{ 0 }; // transcript mtime the recap+model tail was last re-read at
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
            // PID-reuse guard: the process start time (ProcessStartUnixMs) this entry was resolved
            // for. The cache is pid-keyed and pruned only to live codex pids, so a dead codex's pid
            // can be reused by a NEW codex before the prune drops it — the resolved fast path below
            // would then serve the dead one's rollout uuid/state (and _ReconcileManagedCodex would
            // stamp the wrong codexSessionId -> a later `codex resume <wrong-uuid>`). getCodexInfo
            // requires this to equal the current facts' startUnixMs before reusing a resolved entry;
            // a mismatch (PID reused) falls through to re-resolve. Mirrors the (pid,start) pairing the
            // O7 liveness skip already uses in _lastCodexAlive.
            int64_t startUnixMs{};
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
        // The three logged-once pid sets are pruned to still-alive pids at each full survey's tail
        // (they were insert-only for the process lifetime); a recycled pid IS a new process, so its
        // fresh one-line log after a prune is correct, not spam.
        std::unordered_set<uint32_t> _pebDeniedLogged; // pids whose PEB read was denied — logged once (O7)
        std::unordered_set<uint32_t> _guiExcludedLogged; // GUI claude pids (the desktop Electron app) skipped — logged once
        std::unordered_set<uint32_t> _orphanLogged; // orphaned claude pids (host terminal exited) skipped — logged once
        std::unordered_map<std::wstring, std::wstring> _shellCwdCache; // wtSession -> last TRUSTWORTHY shell cwd (survives idle gaps when a pwsh has no child this tick)

        // Worker-thread-only cache (no lock) of a session's LINE-DERIVED last-activity, mtime-gated.
        // The observer feeds SessionInfo.convLastActivityUnixMs from the transcript's last REAL
        // conversation line (ReadTranscriptLastActivityTail), NOT the file mtime — `claude --resume` +
        // /model / permission-mode / shell-cwd changes APPEND untimestamped state lines that bump the
        // mtime without being activity, so a restored tab focused after a restart would otherwise read
        // "active just now" (it only resumed). Keyed by session id; the tail is re-read only when the
        // transcript mtime advances, so an idle/just-resumed session costs ONLY the TranscriptTimes
        // stat after the one read that settles it. Pruned at each full survey's tail to the sids that
        // survey consulted (_lineActivitySeenThisSurvey below) — an archived/closed session's gate
        // drops instead of accumulating for the process lifetime. See _LineDerivedLastActivity.
        struct LineActivity
        {
            int64_t mtime{ 0 }; // the transcript mtime the value was last derived at (the gate)
            int64_t lastActivityMs{ 0 }; // line-derived last-activity (0 == none found -> caller uses mtime)
            int64_t apiActivityMs{ 0 }; // the PRE-FOLD parent-line value (the ⚡ API-activity half; 0 == none — never the mtime)
        };
        std::unordered_map<std::wstring, LineActivity> _lineActivityBySid;
        std::unordered_set<std::wstring> _lineActivitySeenThisSurvey; // the sids _LineDerivedLastActivity served THIS full survey — the prune's keep-set (worker-thread-only)
        // Worker-thread-only: the line-derived last-activity for `sid` (the value to publish as
        // convLastActivityUnixMs), gated by _lineActivityBySid against the transcript `mtimeMs` (from
        // TranscriptTimes) — which is also the fallback when no timestamped conversation line is found.
        // `apiOut` (optional): the PRE-FOLD parent-line value for convApiActivityUnixMs — the newest
        // real line of the PARENT conversation itself (never the subagent fold, never the mtime): a
        // teammate's/subagent's own-context turn must not light the ⚡ cache hint.
        int64_t _LineDerivedLastActivity(std::wstring_view cwd, const std::wstring& sid, int64_t mtimeMs, int64_t* apiOut = nullptr);
    };
}
