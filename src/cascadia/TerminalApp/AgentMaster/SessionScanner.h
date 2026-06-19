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
    inline constexpr int64_t kScanPresenceIdleQuiescenceMs = 5000; // presence-idle release BASE floor (ShouldSynthesizeStopFromPresenceIdle): the transcript must be quiet THIS long — longer than kScanStopQuiescenceMs, to outlast the ~2s S-lane presence-refresh lag — before claude's at-rest ("idle") heartbeat releases a NON-terminal-tail turn. Used directly for NeedsApproval; a RUNNING cleared-tail turn needs the longer kScanPresenceIdleRunningQuiescenceMs below (its shape is ambiguous with an API-retry pause)
    inline constexpr int64_t kScanPresenceIdleRunningQuiescenceMs = 30000; // RUNNING-state presence-idle release floor (the idle<->running flap fix): a Running session with a CLEARED (empty) stop_reason must be quiet — NO transcript append AND NO "busy" heartbeat, BOTH of which reset quietForMs in _reconcileSession — for THIS long before its "idle" heartbeat releases it. FAR longer than the base floor, because a cleared-tail Running turn is AMBIGUOUS: a genuine no-op/finished turn is indistinguishable at an instant from a turn merely PAUSED behind a "No response from API · Retrying" backoff / slow first token / streaming stall (claude reports "idle" in both). The long floor lets a real pause ride — it appends or flips "busy" within the window — while still releasing a session truly at rest, killing the Running<->WaitingForInput flap vs recon-run (the reported "card bg" flap)
    inline constexpr int64_t kScanMaxDeltaBytes = 1 << 20; // read at most 1 MiB of new transcript per tick
    inline constexpr int64_t kScanForceConsumeBytes = 4 << 20; // a 4 MiB run with no newline -> skip it (corrupt/binary guard)
    inline constexpr int64_t kScanDiscoverMs = 1500; // idle keep-ticking cadence (drives each window's observer probe + liveness sweep when nothing is live)
    inline constexpr int64_t kScanRunRepairFreshMs = 15000; // a consumed turn event must be this FRESH (file mtime) to synthesize a missed UserPromptSubmit — a stalled/late scan must not revive an old write (belt+suspenders BEHIND the primed-cursor gate below, which is what actually blocks the history replay: mtime alone cannot — a window closed mid-turn and reopened within the window replays a FRESH file)
    inline constexpr int64_t kScanSubagentFreshMs = 15000; // a SUBAGENT/tool-result side file written within this window == the turn is actively working inside a Task/Agent subagent (the parent <id>.jsonl is quiescent) -> hold/synthesize Running (SubagentActivityUnixMs, ProcessInspect)

    // One reconciled record extracted from a transcript .jsonl line (the PURE parser's output).
    struct TranscriptEvent
    {
        enum class Kind
        {
            UserPrompt, // a human message (content is a plain string / pure-text block; NOT a tool_result)
            Assistant, // an assistant message (carries its concatenated text + message.stop_reason)
            ToolResult, // a user message carrying a tool_result block — a tool completed (NOT a typed prompt)
        };
        Kind kind{ Kind::Assistant };
        std::wstring text; // UserPrompt: the prompt body. Assistant: concatenated text blocks (may be empty).
        std::wstring stopReason; // Assistant only: message.stop_reason ("end_turn" / "tool_use" / ...).
        // Assistant only: the name of an INTERACTIVE tool_use block in this message (one that blocks
        // on the user — AskUserQuestion), else "". Lets the scanner tell a session that is BLOCKED
        // waiting for the user to answer (-> NeedsApproval) from one genuinely working (a long Bash).
        std::wstring toolName;
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

    // PURE: does this assistant stop_reason mark the TURN as complete? "end_turn" is the common
    // case, but "stop_sequence" / "max_tokens" / "refusal" equally end the turn (nothing further
    // is coming without new input) — gating the reconcilers on end_turn ONLY left a turn that
    // ended any other way stuck Running forever (the missed-Stop backstop never fired) and let
    // the run-repair read its tail as "turn in progress". "tool_use" (mid-turn), "" (a user line
    // cleared it / none seen yet), and any UNKNOWN future reason read as in-flight — that is the
    // pre-existing default (everything != end_turn), so a new reason degrades to the old
    // behavior instead of inventing a turn boundary.
    inline bool IsTerminalStopReason(std::wstring_view reason) noexcept
    {
        return reason == L"end_turn" || reason == L"stop_sequence" || reason == L"max_tokens" || reason == L"refusal";
    }

    // PURE + total: should the reconciler synthesize a missed/folded UserPromptSubmit (-> Running)?
    // The Running MIRROR of the missed-Stop synthesis. A dropped UserPromptSubmit hook used to be
    // half-repaired: the scanner back-filled the PROMPT (NoteExternalPrompt) but never the STATE, so
    // the whole turn ran showing WaitingForInput/Idle — and with the session never Running, the
    // missed-Stop backstop (gated on Running) was disarmed too, so the turn's END also went
    // unnoticed. Same for the FOLDED variant (the next prompt's UserPromptSubmit lands mid-turn /
    // before the prior turn's late Stop, which then flips Running back to Waiting mid-turn).
    // Fires only when: this pass consumed ≥1 turn event (a human prompt or an assistant line — the
    // repair piggybacks on transcript appends, never on a quiet file); the cursor was PRIMED before
    // this pass (`primedBeforePass`: the tail had already caught up with the file end, so the
    // consumed events are a LIVE append — the initial history replay of a restored/adopted session
    // reads the WHOLE transcript from offset 0, and a window closed mid-turn leaves that history
    // ending "turn in progress" with a FRESH mtime, so without this gate a just-resumed, idle
    // claude lit up Running and STUCK there — recon-stop needs a terminal-stop tail to clear
    // it); the tail says a turn is IN PROGRESS (!IsTerminalStopReason(lastStopReason) — a user
    // line cleared it / an assistant line is mid-turn; a terminal tail is the missed-Stop's
    // territory); the write is FRESH
    // (sinceWriteMs <= kScanRunRepairFreshMs — a stalled scan must not revive an old write); and
    // the session sits in one of the two states a missed prompt strands it in (Idle /
    // WaitingForInput — Running needs no repair, and NeedsApproval / Error / Done are "needs you /
    // ended" states a mere transcript line must never clear).
    bool ShouldSynthesizeRunning(SessionState state, bool consumedTurnEvent, bool primedBeforePass, std::wstring_view lastStopReason, int64_t sinceWriteMs) noexcept;

    // PURE: is this user-message text the Claude Code marker for a turn the user ABORTED (Esc)?
    // An interrupt fires NO clean Stop hook, and the marker line clears the tracked stop_reason —
    // so without recognizing it, a Running session whose turn the user killed stayed Running
    // forever (the missed-Stop backstop, gated on a terminal stop_reason, could never fire). The
    // marker is authoritative "the turn is over NOW" (no more assistant output is coming), so it
    // is treated as a turn-ender. Matches both "[Request interrupted by user]" and the
    // "…for tool use]" variant via the stable prefix.
    inline bool IsUserInterruptMarker(std::wstring_view text) noexcept
    {
        constexpr std::wstring_view kPrefix = L"[Request interrupted by user";
        return text.size() >= kPrefix.size() && text.substr(0, kPrefix.size()) == kPrefix;
    }

    // PURE: does this tool_use name BLOCK on the user (the agent cannot proceed until the user
    // answers)? AskUserQuestion is the built-in interactive question tool. An unanswered one with a
    // quiescent transcript means the session is "needs you", NOT working — distinct from a pending
    // NON-interactive tool (a long Bash / web fetch) whose transcript is ALSO momentarily quiescent
    // but is genuinely running. Conservative allowlist (evidence-based): only names we KNOW block.
    inline bool IsInteractiveTool(std::wstring_view toolName) noexcept
    {
        return toolName == L"AskUserQuestion";
    }

    // PURE + total: should the reconciler synthesize a missed/forced Stop (-> WaitingForInput)? The
    // missed-Stop backstop, generalized. Fires from Running OR NeedsApproval — a session blocked on
    // the user (a real permission approval, OR a synthesized AskUserQuestion block below) whose turn
    // then ENDED must also be released; recon-stop being Running-ONLY left an approved session stuck
    // in NeedsApproval when its post-approval Stop hook was dropped. The turn is OVER when the tail
    // is a TERMINAL stop_reason (end_turn / …) or the user INTERRUPTED it. Requires the transcript
    // to have gone quiescent first (a mid-turn pause is not the end).
    inline bool ShouldSynthesizeStop(SessionState state, std::wstring_view lastStopReason, bool interrupted, int64_t quietForMs) noexcept
    {
        if (state != SessionState::Running && state != SessionState::NeedsApproval)
        {
            return false; // only a turn-in-progress / blocked-on-user state has a turn to end
        }
        if (quietForMs < kScanStopQuiescenceMs)
        {
            return false; // not quiet long enough — could be a mid-turn pause
        }
        return interrupted || IsTerminalStopReason(lastStopReason);
    }

    // PURE + total: should the reconciler synthesize a "needs you" state (-> NeedsApproval) because
    // the latest assistant message is an UNANSWERED interactive tool_use (AskUserQuestion) and the
    // transcript has gone quiescent? The session is blocked waiting for the user to answer — it must
    // NOT show as Running (the Triage Board's whole job is to surface who needs you). Only from
    // Running (idempotent — once NeedsApproval it stays until the answer + the turn's end release it
    // via ShouldSynthesizeStop). An interrupt takes precedence (the question is gone -> a Stop).
    inline bool ShouldSynthesizeBlockedOnUser(SessionState state, std::wstring_view pendingInteractiveTool, bool interrupted, int64_t quietForMs) noexcept
    {
        if (state != SessionState::Running)
        {
            return false; // entered once from Running; never re-fire while already NeedsApproval
        }
        if (interrupted)
        {
            return false; // an interrupt ends the turn (ShouldSynthesizeStop owns it), not a block
        }
        if (quietForMs < kScanStopQuiescenceMs)
        {
            return false; // the user may answer within the settle window — don't flicker
        }
        return IsInteractiveTool(pendingInteractiveTool);
    }

    // PURE + total: should the reconciler release a NeedsApproval session back to Running because the
    // user ANSWERED the blocking question / approval and the agent has RESUMED working? NeedsApproval
    // is entered by a real permission Notification OR the synthesized AskUserQuestion block above, and
    // until now its ONLY pull-side exit was -> WaitingForInput at the turn's END (ShouldSynthesizeStop)
    // — so a session that was answered and KEPT WORKING (more assistant output, not yet end-of-turn)
    // showed "needs you" (orange) for the whole rest of the turn. It has RESUMED when a fresh
    // assistant/user line appended THIS pass (consumedTurnEvent) on an already-PRIMED cursor (the
    // initial history replay never counts — exactly as ShouldSynthesizeRunning), the blocking
    // interactive tool is no longer pending (answered / moved on; a NEW AskUserQuestion keeps it set ->
    // stay blocked), the user did NOT interrupt, and the tail is NOT a terminal stop_reason (a finished
    // turn is ShouldSynthesizeStop's job -> Waiting, never a Running blip). The interrupt + terminal
    // guards make this MUTUALLY EXCLUSIVE with ShouldSynthesizeStop (no single pass satisfies both), so
    // the two never fight regardless of the fresh/quiescent window overlap. It is the NeedsApproval
    // mirror of ShouldSynthesizeRunning (which stays Idle/Waiting-ONLY — Running needs no repair); the
    // caller synthesizes it as tool ACTIVITY (PostToolUse), NOT a UserPromptSubmit, because a
    // UserPromptSubmit from NeedsApproval would wrongly ++queuedPrompts (the type-ahead accounting).
    inline bool ShouldSynthesizeResumed(SessionState state, bool consumedTurnEvent, bool primedBeforePass, std::wstring_view pendingInteractiveTool, std::wstring_view lastStopReason, bool interrupted, int64_t sinceWriteMs) noexcept
    {
        if (state != SessionState::NeedsApproval)
        {
            return false; // only NeedsApproval lacks a resume edge; Idle/Waiting -> ShouldSynthesizeRunning
        }
        if (!consumedTurnEvent || !primedBeforePass)
        {
            return false; // no live append this pass (or still replaying history) -> not a resume
        }
        if (!pendingInteractiveTool.empty())
        {
            return false; // still blocked on a (possibly NEW) question -> stay NeedsApproval
        }
        if (interrupted || IsTerminalStopReason(lastStopReason))
        {
            return false; // the turn ENDED -> ShouldSynthesizeStop's job (-> Waiting), not a resume
        }
        return sinceWriteMs <= kScanRunRepairFreshMs; // a live, fresh turn (not an old write surfacing late)
    }

    // PURE: is Claude's presence heartbeat reporting this session as actively WORKING? The presence
    // file (~/.claude/sessions/<pid>.json "status") is claude's OWN self-report, and it (a) FOLLOWS
    // the live conversation across /fork, /clear, /compact, /resume — it is pid-keyed, so it names
    // the id the process is ACTUALLY in, not the stale launch id — and (b) stays "busy" for the WHOLE
    // turn, including while a Task/Agent subagent runs and the main transcript is quiescent. So it is
    // the one signal that covers BOTH spawn cases. The S-lane validates it against pid liveness before
    // publishing it onto SessionInfo.presenceStatus (Rule #13: a display FACT — the OBSERVER never
    // sets state), which is exactly why the SCANNER — the engine's state authority — may read it as a
    // state INPUT here without violating that rule. "idle"/"waiting"/"shell"/"" are NOT "working".
    inline bool PresenceIsBusy(std::wstring_view presenceStatus) noexcept
    {
        return presenceStatus == L"busy";
    }

    // PURE: is Claude's presence heartbeat reporting this session AT REST — "idle" (no turn in
    // progress, nothing pending)? The RELEASE mirror of PresenceIsBusy. claude's pid-keyed self-report
    // flips to "busy" the instant a turn starts and back to "idle" when it fully ends, so an explicit
    // "idle" is authoritative "no turn is running". Deliberately NARROW — only "idle": "waiting" (the
    // user is being asked to answer/approve) overlaps the NeedsApproval / recon-block "needs you"
    // semantics and is left to those paths; "shell" / "" are not at-rest claude-turn signals (a shell
    // job, or no live presence file at all). The S-lane validates the backing pid is a live claude.exe
    // before publishing this onto SessionInfo.presenceStatus (Rule #13: a FACT the scanner may consume).
    inline bool PresenceIsAtRest(std::wstring_view presenceStatus) noexcept
    {
        return presenceStatus == L"idle";
    }

    // PURE + total: should the reconciler synthesize a missed Stop because Claude's OWN presence
    // heartbeat says the session is at rest ("idle") while we are STILL Running / NeedsApproval, even
    // though the transcript tail is NON-terminal? The missing IDLE half of the presence signal
    // (PresenceIsBusy is the BUSY-hold half — it only ever KEPT a session Running; nothing released one
    // on the strength of claude's own "I'm idle"). The plain missed-Stop backstop (ShouldSynthesizeStop)
    // needs a TERMINAL stop_reason or an interrupt in the tail — but a turn can end with NEITHER: the
    // last transcript line is a bare user prompt whose UserPromptSubmit cleared the tracked stop_reason
    // (_readDelta) and which then produced NO assistant output and fired NO Stop hook (dropped, or a
    // no-op turn), stranding the session Running forever while claude is demonstrably idle. claude's
    // pid-validated heartbeat is the authority here: "idle" => the turn is OVER. Fires only in exactly
    // that GAP — a NON-terminal, NON-interrupted tail (a terminal/interrupted tail is
    // ShouldSynthesizeStop's territory, so the two are mutually exclusive and never double-fire) — from
    // Running / NeedsApproval, and only after the transcript has been quiet LONGER than the normal stop
    // quiescence (kScanPresenceIdleQuiescenceMs). The longer window closes the only race: right after a
    // turn STARTS claude's file is already "busy" but the ~2s S-lane survey may not have refreshed
    // SessionInfo.presenceStatus off the prior "idle" yet — by the time the parent transcript has been
    // quiet kScanPresenceIdleQuiescenceMs with presence STILL "idle", the survey has re-read it, and a
    // genuine in-flight turn (incl. an extended-thinking pause, which keeps the heartbeat "busy") reads
    // "busy". The caller additionally gates this on no pending interactive tool so it never pre-empts
    // recon-block's blocked-on-question -> NeedsApproval path.
    //
    // Agentmaster (idle<->running flap fix, TWO-part): a RUNNING session's "idle" heartbeat must clear
    // TWO hurdles, because "idle heartbeat + non-terminal tail + quiet transcript" ALSO describes a turn
    // merely PAUSED behind a "No response from API · Retrying" backoff / a slow first token / a streaming
    // stall — claude stops generating, so it reports "idle" and the file sits quiet, yet the turn is NOT
    // over. (1) A PENDING tool_use tail (non-empty, non-terminal stop_reason) is NEVER released: claude
    // is mid-tool (a long Bash/build) or waiting on that tool's API call. (2) A CLEARED (empty)
    // stop_reason — a bare user prompt with no assistant output yet — is the no-op-turn case this
    // backstop was built for, BUT it is ALSO exactly what a turn looks like while its FIRST API call is
    // retrying (the original fix wrongly assumed "No response from API" only ever coincides with a
    // pending tool_use — the live "card bg" flap proved it fires with a CLEARED tail too). The two are
    // indistinguishable at an instant, so a cleared-tail Running turn is released only after the LONG
    // quiescence floor (kScanPresenceIdleRunningQuiescenceMs): any transcript append OR any "busy"
    // heartbeat inside the window resets quietForMs (see _reconcileSession), so a real retry — which
    // does at least one of those repeatedly — never reaches the floor, while a genuinely finished turn
    // (no writes, no "busy") does. Together these kill the Running<->WaitingForInput oscillation against
    // recon-run (the reported "card bg" flap + "jumps to idle/waiting while the API is retrying" bug).
    // NeedsApproval is unaffected: it keeps the SHORT base floor (its pending tool_use is the
    // AskUserQuestion — once answered + idle, that turn really is settled, with no API-retry ambiguity).
    inline bool ShouldSynthesizeStopFromPresenceIdle(SessionState state, std::wstring_view presenceStatus, std::wstring_view lastStopReason, bool interrupted, int64_t quietForMs) noexcept
    {
        if (state != SessionState::Running && state != SessionState::NeedsApproval)
        {
            return false; // only a turn-in-progress / blocked-on-user state has a turn to end
        }
        if (interrupted || IsTerminalStopReason(lastStopReason))
        {
            return false; // a terminal / interrupted tail is the plain missed-Stop's job (no double-fire)
        }
        if (state == SessionState::Running && !lastStopReason.empty())
        {
            return false; // hurdle 1: a PENDING tool_use tail == mid-tool / waiting on its API call — never a turn-end
        }
        // hurdle 2 — the quiescence floor. NeedsApproval uses the short base; a Running cleared-tail turn
        // uses the LONG floor, because that shape is indistinguishable from an in-flight API-retry /
        // streaming pause (which appends or flips "busy" within the window, resetting quietForMs, so it
        // never reaches the floor). Only a session truly at rest for the WHOLE long window is released.
        const int64_t quiescenceFloor = (state == SessionState::Running) ? kScanPresenceIdleRunningQuiescenceMs : kScanPresenceIdleQuiescenceMs;
        if (quietForMs < quiescenceFloor)
        {
            return false;
        }
        return PresenceIsAtRest(presenceStatus);
    }

    // PURE + total: should the reconciler synthesize Running because EXTERNAL work is active while the
    // session sits Idle / WaitingForInput? The SUBAGENT/FORK mirror of ShouldSynthesizeRunning (which
    // keys on a PARENT-transcript append): a tab whose turn delegated to a subagent — so the parent
    // <id>.jsonl is quiescent while <id>/subagents/*.jsonl grows — or whose live conversation forked to
    // a new id we have not yet re-bound, must read Running, not idle. Two signals of "still working":
    //   * subagentActive — a subagent/tool-result side file was written within kScanSubagentFreshMs
    //     (SubagentActivityUnixMs). Fires regardless of cursor priming — it is a CURRENT filesystem
    //     fact, not a transcript-replay artifact, so the primed-cursor gate the parent-append repairs
    //     need does not apply.
    //   * presenceBusy — claude's heartbeat self-reports "busy" (it follows a live /fork to a new id).
    // BOTH arms are gated on a NON-TERMINAL tail: a TERMINAL stop_reason (end_turn / stop_sequence /
    // max_tokens / refusal) means the turn is OVER, and recent side-file activity / a lingering "busy"
    // is then the TAIL END of the turn that just finished — the subagent's last write lands µs BEFORE
    // the parent's end_turn (so it is always "fresh" at turn-end), and "busy" lingers a tick after a
    // real Stop — NOT new work, so it must never bounce a settled (WaitingForInput) session back to
    // Running. During a LIVE subagent the parent tail is the pending Task/Agent tool_use (non-terminal),
    // so genuine in-flight work is unaffected. Only from the two states a missed turn-start strands a
    // session in (Idle / WaitingForInput); Running needs no repair, and NeedsApproval / Error / Done are
    // "needs you / ended" a mere activity signal must not clear. The caller synthesizes it as tool
    // ACTIVITY (PostToolUse -> Running), NOT a UserPromptSubmit (which would inflate the type-ahead
    // queue accounting, ++queuedPrompts).
    inline bool ShouldSynthesizeRunningFromExternalWork(SessionState state, bool subagentActive, bool presenceBusy, std::wstring_view lastStopReason) noexcept
    {
        if (state != SessionState::Idle && state != SessionState::WaitingForInput)
        {
            return false;
        }
        if (IsTerminalStopReason(lastStopReason))
        {
            return false; // a COMPLETED turn — recent subagent activity / lingering "busy" is its tail end, not new work
        }
        return subagentActive || presenceBusy;
    }

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

        // Call once at engine init to keep the worker TICKING even with no live sessions, so each
        // window's liveness probe + the Fleet Observer's per-window roster publish keep running (a
        // hand-typed `claude` in a fresh tab is then correlated out-of-band by the observer). The
        // transcript-discovery enumeration this used to also start is retired (O7). Name kept for now.
        void ArmDiscovery();

        // Agentmaster (cache-aware Waiting decay): how long a session may sit in WaitingForInput
        // before the scanner demotes it to Idle (the Triage Board's "Waiting-for-you" column should
        // only surface sessions still inside Claude's ~5-minute server-side prompt-cache window —
        // past it, answering costs a full cache re-read either way). 0 == never decay. Seeded from
        // AppSettings at engine init and re-pushed when the Settings cog saves. Atomic — the cog
        // writes from a UI thread while the worker reads.
        void SetWaitingDecayMinutes(uint32_t minutes) noexcept
        {
            _waitingDecayMinutes.store(minutes);
        }

    private:
        // Per-session tail cursor (owned solely by the worker thread — no lock needed).
        struct ScanState
        {
            std::wstring path; // resolved transcript path ("" until found / after it vanishes)
            int64_t offset{ 0 }; // BYTE offset already consumed (only complete lines advance it)
            int64_t lastSize{ -1 }; // last observed file size (the cheap change-gate)
            std::wstring lastAssistantText; // latest assistant text seen (for the missed-Stop question)
            std::wstring lastStopReason; // latest assistant stop_reason (IsTerminalStopReason => turn complete)
            // Has the cursor ever CAUGHT UP with the file end? False while the initial backlog
            // (a restored/adopted session's whole history, read from offset 0 in 1 MiB chunks) is
            // still being consumed; true from the first pass that reached the current end. Only
            // events consumed AFTER priming are a live append — the run-repair gate
            // (ShouldSynthesizeRunning) reads the value from BEFORE the pass, so the pass that
            // finishes the replay cannot itself synthesize. Reset with the truncation rewind.
            bool primed{ false };
            // Tail facts that disambiguate "blocked on the user" / "interrupted" from "working" —
            // the missed-Stop backstop can't read either from stop_reason alone (HookEvents.h notes).
            std::wstring pendingInteractiveTool; // an UNANSWERED interactive tool_use (AskUserQuestion) is the latest assistant block; "" once answered / moved on
            bool interrupted{ false }; // the latest user line is a turn-abort marker (Esc) — treat as a turn-ender
        };

        void _worker() noexcept;
        int64_t _scanOnce(); // one coalesced pass; returns the next sleep ms (<0 == sleep until woken)
        void _reconcileSession(const SessionInfo& s);
        // Returns true when ≥1 turn event (user prompt / assistant line) was consumed this call —
        // the trigger for the missed-UserPromptSubmit repair in _reconcileSession.
        bool _readDelta(ScanState& st, const SessionInfo& s, int64_t size);
        void _maybeSweepLiveness(int64_t nowMs, bool anyLive);
        void _maybeDecayWaiting(const SessionInfo& s, int64_t nowMs); // WaitingForInput older than the decay window -> Idle

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

        // Keeps the worker TICKING with nothing live (so each window's observer probe + liveness
        // sweep keep running). Set once via ArmDiscovery at engine init; the transcript-ENUMERATION
        // it used to also drive is retired (O7) — the Fleet Observer's PEB correlation subsumes it.
        std::atomic<bool> _discoverArmed{ false };

        // Cache-aware Waiting decay window in minutes (see SetWaitingDecayMinutes). The default
        // mirrors AppSettings::waitingDecayMinutes so the behavior holds even before the seed lands.
        std::atomic<uint32_t> _waitingDecayMinutes{ 5 };
    };
}
