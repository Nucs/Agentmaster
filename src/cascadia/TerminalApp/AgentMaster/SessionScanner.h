// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — SessionScanner: the interval reconciler (the PULL half of the design; the
// HooksBridge is the PUSH half). Hooks give low-latency, authoritative state, but they can be
// DROPPED (a missed `Stop` strands a session in Running forever) and they carry no assistant
// output text. A low-priority worker thread tails each LIVE session's transcript .jsonl and
// reconciles what the push path missed — capturing the latest assistant message, back-filling a
// human prompt whose UserPromptSubmit hook never arrived, and synthesizing a missed Stop — and
// it fans out a periodic liveness sweep so a dead claude.exe (crash / `/exit` with no SessionEnd)
// gets archived.
//
// SMART + PERFORMANT (the explicit ask): scope to live, non-archived sessions only; a cheap
// size stat gates any read; only the byte DELTA past a saved offset is parsed; one coalesced
// pass per tick; cadence is ADAPTIVE (fast while a turn is Running, slow while Idle/Waiting) and
// the loop SLEEPS on a condvar at zero cost when nothing is live — a registry observer Wake()s
// it the instant a session goes live.
//
// Plain C++ + Win32 (no WinRT), like the rest of AgentMaster/, so it is unit-testable standalone.
// The transcript parse is a PURE function (ParseTranscriptDelta); the thread wraps it with the
// filesystem read, the registry writes, and the liveness fan-out. The liveness CHECK itself is
// WinRT (it walks XAML tabs), so it is delegated to app-layer probes the scanner merely TICKS.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SessionModels.h" // SessionInfo (passed to the per-session reconcile)

namespace Agentmaster
{
    class SessionRegistry;

    // --- cadence + bound tunables (see DecideAdvance-style separation: values are explicit) ---
    inline constexpr int64_t kScanRunningMs = 300; // a turn is in progress: poll quickly
    inline constexpr int64_t kScanLiveIdleMs = 2500; // live but Idle/Waiting: poll lazily
    inline constexpr int64_t kScanSweepMs = 2500; // min interval between liveness-probe ticks
    inline constexpr int64_t kScanStopQuiescenceMs = 2000; // transcript must be this quiet before a synthesized Stop
    inline constexpr int64_t kScanMaxDeltaBytes = 1 << 20; // read at most 1 MiB of new transcript per tick
    inline constexpr int64_t kScanForceConsumeBytes = 4 << 20; // a 4 MiB run with no newline -> skip it (corrupt/binary guard)

    // One reconciled record extracted from a transcript .jsonl line (the PURE parser's output).
    struct TranscriptEvent
    {
        enum class Kind
        {
            UserPrompt, // a human message (content is a plain string / pure-text block; NOT a tool_result)
            Assistant, // an assistant message (carries its concatenated text + message.stop_reason)
        };
        Kind kind{ Kind::Assistant };
        std::wstring text; // UserPrompt: the prompt body. Assistant: concatenated text blocks (may be empty).
        std::wstring stopReason; // Assistant only: message.stop_reason ("end_turn" / "tool_use" / ...).
    };

    struct TranscriptParse
    {
        std::vector<TranscriptEvent> events;
        size_t consumed{ 0 }; // wide-char count up to and INCLUDING the last '\n' (a partial tail is left)
    };

    // PURE + total (never throws): parse a UTF-16 chunk of NEW transcript text into ordered
    // reconciled events. Each complete (newline-terminated) line is one JSON object (Claude Code
    // transcript JSONL). A trailing line WITHOUT a newline is an append in flight — it is left
    // unconsumed (reflected in `consumed`) so the next read re-sees it whole. User-prompt
    // extraction is deliberately CONSERVATIVE (top-level string / pure-text content only, and
    // `isMeta` lines skipped) so a tool_result or meta line is never mistaken for a typed prompt;
    // the UserPromptSubmit hook is the primary path and this only back-fills a dropped one.
    TranscriptParse ParseTranscriptDelta(std::wstring_view chunk);

    // Ticked on the scanner thread on the slow cadence; the probe marshals to ITS OWN UI thread
    // and archives any of its claude tabs whose ConPTY connection has Closed. One per window (M9).
    using LivenessProbe = std::function<void()>;
    using LivenessToken = uint64_t;

    class SessionScanner
    {
    public:
        explicit SessionScanner(std::shared_ptr<SessionRegistry> registry);
        ~SessionScanner();

        SessionScanner(const SessionScanner&) = delete;
        SessionScanner& operator=(const SessionScanner&) = delete;

        void Start();
        void Stop() noexcept;

        // Break the idle wait early (e.g. a session just went live). Wired to a registry observer
        // in Engine so a newly-launched/adopted session is picked up immediately, not after a
        // timeout. Cheap + idempotent.
        void Wake() noexcept;

        // Register / detach an app-layer liveness probe (one per window; M9 fan-out, mirroring the
        // registry's adoption handlers). Tokens are monotonic and never reused, so removing a stale
        // one is a no-op. The window detaches via RemoveLivenessProbe on teardown.
        LivenessToken AddLivenessProbe(LivenessProbe probe);
        void RemoveLivenessProbe(LivenessToken token);

    private:
        // Per-session tail cursor (owned solely by the worker thread — no lock needed).
        struct ScanState
        {
            std::wstring path; // resolved transcript path ("" until found / after it vanishes)
            int64_t offset{ 0 }; // BYTE offset already consumed (only complete lines advance it)
            int64_t lastSize{ -1 }; // last observed file size (the cheap change-gate)
            std::wstring lastAssistantText; // latest assistant text seen (for the missed-Stop question)
            std::wstring lastStopReason; // latest assistant stop_reason ("end_turn" => turn complete)
        };

        void _worker() noexcept;
        int64_t _scanOnce(); // one coalesced pass; returns the next sleep ms (<0 == sleep until woken)
        void _reconcileSession(const SessionInfo& s);
        void _readDelta(ScanState& st, const SessionInfo& s, int64_t size);
        void _maybeSweepLiveness(int64_t nowMs, bool anyLive);

        std::shared_ptr<SessionRegistry> _registry;
        std::thread _thread;
        std::mutex _mtx; // guards _woken + _probes (NOT _scan: worker-confined)
        std::condition_variable _cv;
        std::atomic<bool> _running{ false };
        bool _woken{ false };

        std::unordered_map<std::wstring, ScanState> _scan; // sessionId -> tail cursor (worker thread only)
        std::vector<std::pair<LivenessToken, LivenessProbe>> _probes;
        uint64_t _nextProbeId{ 1 };
        int64_t _lastSweepMs{ 0 };
    };
}
