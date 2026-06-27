// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). The transcript tail is raw Win32 file I/O.
#define NOMINMAX
#include "SessionScanner.h"

#include "ClaudeSpawn.h" // ResolveClaudeTranscriptPath, AppendStateLog
#include "Json.h"
#include "ProcessInspect.h" // SubagentActivityUnixMs — subagent/Task side-file activity (the parent transcript stays quiescent while a subagent runs)
#include "SessionRegistry.h"
#include "TranscriptStore.h" // IsNoiseUserPrompt — keep control markers out of the Flight-Plan back-fill

#include <windows.h>

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

    // FILETIME (100ns ticks since 1601-01-01) -> Unix epoch milliseconds.
    int64_t FiletimeToUnixMs(const FILETIME& ft)
    {
        ULARGE_INTEGER u{};
        u.HighPart = ft.dwHighDateTime;
        u.LowPart = ft.dwLowDateTime;
        // 11644473600 s between 1601 and 1970, in 100ns ticks == 116444736000000000.
        if (u.QuadPart < 116444736000000000ULL)
        {
            return 0;
        }
        return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    }

    // UTF-8 byte span -> UTF-16 (mirrors HooksBridge's helper). Empty on failure.
    std::wstring Utf8ToUtf16(const char* data, int len)
    {
        if (len <= 0)
        {
            return {};
        }
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, data, len, nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        const int written = ::MultiByteToWideChar(CP_UTF8, 0, data, len, out.data(), needed);
        if (written <= 0)
        {
            return {};
        }
        out.resize(static_cast<size_t>(written));
        return out;
    }

    bool EndsWithQuestion(std::wstring_view s)
    {
        size_t b = s.size();
        while (b > 0 && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n'))
        {
            --b;
        }
        return b > 0 && s[b - 1] == L'?';
    }

    // The name of an INTERACTIVE (blocks-on-user) tool_use block in this message's content, else "".
    // The scanner uses it to flag a session blocked waiting for the user to answer (AskUserQuestion)
    // — non-interactive tool_use blocks (Bash, Read, …) are ignored: those are genuinely working.
    std::wstring CollectInteractiveToolName(const Agentmaster::json::Value* content)
    {
        using Agentmaster::json::Value;
        if (!content || content->type != Value::Type::Arr)
        {
            return {};
        }
        for (const auto& blk : content->arr)
        {
            if (blk.type == Value::Type::Obj && blk.StrAt(L"type") == L"tool_use" &&
                Agentmaster::IsInteractiveTool(blk.StrAt(L"name")))
            {
                return blk.StrAt(L"name");
            }
        }
        return {};
    }

    // Concatenate the text blocks of a Claude message `content` (string, or array of blocks).
    std::wstring CollectText(const Agentmaster::json::Value* content)
    {
        using Agentmaster::json::Value;
        if (!content)
        {
            return {};
        }
        if (content->type == Value::Type::Str)
        {
            return content->str;
        }
        if (content->type == Value::Type::Arr)
        {
            std::wstring text;
            for (const auto& blk : content->arr)
            {
                if (blk.type == Value::Type::Obj && blk.StrAt(L"type") == L"text")
                {
                    text += blk.StrAt(L"text");
                }
            }
            return text;
        }
        return {};
    }
}

namespace Agentmaster
{
    static TranscriptParse ParseTranscriptDeltaImpl(std::wstring_view chunk);
    // Agentmaster (extra-safe): the scanner WORKER THREAD calls this every tick; an uncaught parse throw
    // here (a malformed/partial .jsonl chunk, std::bad_alloc) would unwind off the worker with no catch
    // and std::terminate the whole app. Contain it -> return an empty delta, so the reconciler simply
    // makes no state change this pass (the file byte-cursor is advanced by bytes READ, independent of
    // parse success, so a bad chunk is skipped, not retried forever). See AnalyzeSessionTranscript.
    TranscriptParse ParseTranscriptDelta(std::wstring_view chunk)
    {
        try
        {
            return ParseTranscriptDeltaImpl(chunk);
        }
        catch (...)
        {
            OutputDebugStringW(L"[Agentmaster] ParseTranscriptDelta: swallowed parse exception (no crash)\n");
            return {};
        }
    }
    static TranscriptParse ParseTranscriptDeltaImpl(std::wstring_view chunk)
    {
        TranscriptParse out;
        size_t lineStart = 0;
        for (size_t i = 0; i < chunk.size(); ++i)
        {
            if (chunk[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line = chunk.substr(lineStart, i - lineStart);
            out.consumed = i + 1; // a partial trailing line (after the last '\n') is left unconsumed
            lineStart = i + 1;

            // Trim a trailing CR (CRLF transcripts) and skip blank lines cheaply.
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            bool blank = true;
            for (const wchar_t c : line)
            {
                if (c != L' ' && c != L'\t')
                {
                    blank = false;
                    break;
                }
            }
            if (blank)
            {
                continue;
            }

            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue; // not a JSON object line (tolerate anything)
            }
            const auto& obj = *parsed;
            // Skip subagent/sidechain lines. A Task/Agent subagent's messages carry their OWN
            // message.usage and turn structure — NOT the main session's — so counting them would
            // corrupt contextTokens (it would jump to the subagent's context) and the state machine.
            // In practice subagents stream to a separate subagents/*.jsonl, but this matches the
            // defensiveness ReadTranscriptInfo already applies, in case a flow/version inlines them.
            if (obj.BoolAt(L"isSidechain"))
            {
                continue;
            }
            const std::wstring type = obj.StrAt(L"type");

            if (type == L"assistant")
            {
                const auto* msg = obj.Find(L"message");
                if (!msg || msg->type != json::Value::Type::Obj)
                {
                    continue;
                }
                TranscriptEvent ev;
                ev.kind = TranscriptEvent::Kind::Assistant;
                ev.stopReason = msg->StrAt(L"stop_reason");
                // Agentmaster: the synthetic API-error turn-ender carries a TOP-LEVEL
                // isApiErrorMessage:true (model "<synthetic>", content = the "API Error: …" text). The
                // scanner turns this into SessionState::Error (ShouldSynthesizeError) instead of letting
                // its terminal stop_reason read as a clean turn-complete.
                ev.apiError = obj.BoolAt(L"isApiErrorMessage");
                ev.apiErrorStatus = static_cast<int>(obj.I64At(L"apiErrorStatus")); // top-level HTTP code (429/529/…); 0 when none
                const auto* content = msg->Find(L"content");
                ev.text = CollectText(content);
                ev.toolName = CollectInteractiveToolName(content); // "" unless an interactive tool_use is present
                // Context occupancy from message.usage (≈ the size of the request that produced this
                // message). cache_read carries the whole conversation forward, so this single block is
                // the current context size; the newest assistant line wins downstream.
                if (const auto* usage = msg->Find(L"usage"); usage && usage->type == json::Value::Type::Obj)
                {
                    ev.tokens = usage->I64At(L"input_tokens") +
                                usage->I64At(L"cache_creation_input_tokens") +
                                usage->I64At(L"cache_read_input_tokens") +
                                usage->I64At(L"output_tokens");
                }
                out.events.push_back(std::move(ev));
            }
            else if (type == L"user")
            {
                // Skip meta / synthetic user lines (command echoes, injected reminders, etc.).
                if (obj.BoolAt(L"isMeta"))
                {
                    continue;
                }
                const auto* msg = obj.Find(L"message");
                if (!msg || msg->type != json::Value::Type::Obj)
                {
                    continue;
                }
                const auto* content = msg->Find(L"content");
                if (!content)
                {
                    continue;
                }
                std::wstring prompt;
                if (content->type == json::Value::Type::Str)
                {
                    prompt = content->str;
                }
                else if (content->type == json::Value::Type::Arr)
                {
                    // A pure-text user message is a human prompt; ANY tool_result block means this
                    // is a tool turn, not something the human typed -> emit a ToolResult marker
                    // (a tool completed — it ANSWERS a pending interactive tool_use) instead.
                    bool hasToolResult = false;
                    std::wstring text;
                    for (const auto& blk : content->arr)
                    {
                        if (blk.type != json::Value::Type::Obj)
                        {
                            continue;
                        }
                        const std::wstring bt = blk.StrAt(L"type");
                        if (bt == L"tool_result")
                        {
                            hasToolResult = true;
                            break;
                        }
                        if (bt == L"text")
                        {
                            text += blk.StrAt(L"text");
                        }
                    }
                    if (hasToolResult)
                    {
                        TranscriptEvent ev;
                        ev.kind = TranscriptEvent::Kind::ToolResult;
                        out.events.push_back(std::move(ev));
                        continue;
                    }
                    prompt = text;
                }
                if (!prompt.empty())
                {
                    TranscriptEvent ev;
                    ev.kind = TranscriptEvent::Kind::UserPrompt;
                    ev.text = std::move(prompt);
                    out.events.push_back(std::move(ev));
                }
            }
            else if (type == L"system" && obj.StrAt(L"subtype") == L"away_summary")
            {
                // Agentmaster: the Claude Code idle RECAP. NOT a turn event (it never touches the state
                // machine) — captured out-of-band so the scanner can mirror it onto SessionInfo.recap and
                // the CLI can surface it. Last recap in the chunk wins; an empty body never clears it.
                if (std::wstring r = NormalizeRecapText(obj.StrAt(L"content")); !r.empty())
                {
                    out.recap = std::move(r);
                }
            }
        }
        return out;
    }

    bool ShouldSynthesizeRunning(SessionState state, bool consumedTurnEvent, bool primedBeforePass, std::wstring_view lastStopReason, int64_t sinceWriteMs) noexcept
    {
        if (!consumedTurnEvent)
        {
            return false; // nothing new this pass — never re-fire on a quiet transcript
        }
        if (!primedBeforePass)
        {
            // The cursor had NOT caught up with the file before this pass, so the consumed events
            // are the initial HISTORY REPLAY (a restored/adopted session reads its whole transcript
            // from offset 0) — not a live append. A window closed mid-turn leaves that history
            // ending "turn in progress" with a FRESH mtime (and `--resume` can touch the file), so
            // the freshness check below cannot catch this case: without this gate a just-resumed,
            // actually-idle claude lit up Running and STUCK (recon-stop needs a terminal tail).
            return false;
        }
        if (IsTerminalStopReason(lastStopReason))
        {
            return false; // the tail says the turn COMPLETED — missed-Stop territory, not Running
        }
        if (sinceWriteMs > kScanRunRepairFreshMs)
        {
            return false; // an old write surfacing late (stalled scan) — not a live turn
        }
        // The states a missed UserPromptSubmit strands a session in: Idle / WaitingForInput (Running
        // needs no repair) PLUS Error — a fresh turn event after an API error is the user retrying, so
        // this is the PULL half of "come out of Error on the first change" (the push half is a real
        // UserPromptSubmit -> Running). NeedsApproval / Done stay excluded — "needs you / ended" states a
        // mere transcript line must never clear (a real hook still can, through the state machine).
        return state == SessionState::Idle || state == SessionState::WaitingForInput || state == SessionState::Error;
    }

    SessionScanner::SessionScanner(std::shared_ptr<SessionRegistry> registry) :
        _registry{ std::move(registry) }
    {
    }

    SessionScanner::~SessionScanner()
    {
        Stop();
    }

    void SessionScanner::Start()
    {
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            return;
        }
        _thread = std::thread([this]() noexcept { _worker(); });
    }

    void SessionScanner::Stop() noexcept
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

    void SessionScanner::Wake() noexcept
    {
        {
            std::lock_guard lk{ _mtx };
            _woken = true;
        }
        _cv.notify_one();
    }

    LivenessToken SessionScanner::AddLivenessProbe(LivenessProbe probe)
    {
        std::lock_guard lk{ _mtx };
        const LivenessToken token = _nextProbeId++;
        _probes.emplace_back(token, std::move(probe));
        return token;
    }

    void SessionScanner::RemoveLivenessProbe(LivenessToken token)
    {
        std::lock_guard lk{ _mtx };
        _probes.erase(
            std::remove_if(_probes.begin(), _probes.end(), [token](const auto& p) { return p.first == token; }),
            _probes.end());
    }

    void SessionScanner::_worker() noexcept
    {
        for (;;)
        {
            int64_t sleepMs;
            try
            {
                sleepMs = _scanOnce();
            }
            catch (...)
            {
                sleepMs = kScanLiveIdleMs;
            }

            std::unique_lock lk{ _mtx };
            if (!_running.load())
            {
                return;
            }
            if (sleepMs < 0)
            {
                // Nothing live: sleep at zero cost until Wake()/Stop().
                _cv.wait(lk, [this] { return !_running.load() || _woken; });
            }
            else
            {
                _cv.wait_for(lk, std::chrono::milliseconds(sleepMs), [this] { return !_running.load() || _woken; });
            }
            _woken = false;
            if (!_running.load())
            {
                return;
            }
        }
    }

    int64_t SessionScanner::_scanOnce()
    {
        const int64_t now = NowMs();

        // `armed` (set once at engine init) keeps this worker TICKING even with nothing live, so each
        // window's liveness probe — which also drives the Fleet Observer's per-window roster publish
        // (_ObserverProbe) — keeps running and can correlate a hand-typed `claude` in a fresh tab that
        // has no managed session yet. (The transcript-ENUMERATION sweep this flag used to gate is
        // retired: the Fleet Observer's out-of-band PEB correlation subsumes it — O7.)
        const bool armed = _discoverArmed.load();

        const auto sessions = _registry->Snapshot();

        bool anyLive = false;
        bool anyRunning = false;
        for (const auto& s : sessions)
        {
            if (!s.live)
            {
                continue; // archived: no running claude, the transcript is static -> skip
            }
            anyLive = true;
            if (s.state == SessionState::Running)
            {
                anyRunning = true;
            }
            _reconcileSession(s);
            _maybeDecayWaiting(s, now);
        }

        // Drop tail cursors for sessions that are gone / archived (bounded memory).
        if (!_scan.empty())
        {
            for (auto it = _scan.begin(); it != _scan.end();)
            {
                bool live = false;
                for (const auto& s : sessions)
                {
                    if (s.live && s.id == it->first)
                    {
                        live = true;
                        break;
                    }
                }
                it = live ? std::next(it) : _scan.erase(it);
            }
        }

        // Run the app-layer probes when armed even with nothing live, so each window's probe can
        // correlate a freshly discovered transcript to one of its tabs (the probe alone knows tabs).
        _maybeSweepLiveness(now, anyLive || armed);

        if (!anyLive)
        {
            // Keep ticking on the discovery cadence while armed (so new transcripts are noticed);
            // otherwise sleep at zero cost until a registry observer Wake()s us.
            return armed ? kScanDiscoverMs : -1;
        }
        return anyRunning ? kScanRunningMs : kScanLiveIdleMs;
    }

    void SessionScanner::_reconcileSession(const SessionInfo& s)
    {
        ScanState& st = _scan[s.id];
        if (st.path.empty())
        {
            st.path = ResolveClaudeTranscriptPath(s.id);
            if (st.path.empty())
            {
                return; // no transcript yet (never prompted) — nothing to tail
            }
        }

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(st.path.c_str(), GetFileExInfoStandard, &fad))
        {
            st.path.clear(); // vanished / rotated -> re-resolve next pass
            return;
        }
        const int64_t size = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | static_cast<int64_t>(fad.nFileSizeLow);

        if (size < st.offset)
        {
            // Truncated / rewritten under us (shouldn't happen for an append-only transcript, but
            // be safe): restart the tail from the top. The re-read is a fresh HISTORY REPLAY, so
            // the primed flag drops with it — its events must not feed the run-repair either.
            st.offset = 0;
            st.lastSize = -1;
            st.primed = false;
        }
        // Captured BEFORE the read: events consumed by a not-yet-primed cursor are the initial
        // backlog replay (offset 0 -> end on a restored/adopted session), and the pass that
        // FINISHES the replay sets primed for the NEXT pass — so replayed history can never count
        // as the live append the run-repair keys on (no matter how fresh the file mtime is).
        const bool wasPrimed = st.primed;
        bool consumedTurnEvent = false;
        if (size != st.lastSize)
        {
            consumedTurnEvent = _readDelta(st, s, size); // advances st.offset past the complete lines it consumed
            st.lastSize = size;
        }

        // Missed/folded-UserPromptSubmit reconciliation — the Running MIRROR of the missed-Stop
        // synthesis below. A turn whose UserPromptSubmit was dropped (the forwarder is
        // fire-and-forget) or FOLDED (the prompt was typed/queued mid-turn, so its hook landed
        // while already Running and the prior turn's Stop then flipped the session back to
        // WaitingForInput mid-new-turn) used to run start-to-finish showing WaitingForInput/Idle —
        // and, never being Running, it disarmed the missed-Stop backstop below, so the turn's END
        // went unnoticed too (until the decay shuffled the card to Idle). The PURE gate
        // (ShouldSynthesizeRunning, unit-tested) fires only off a freshly-appended turn event —
        // consumed by an already-PRIMED cursor (wasPrimed: the initial history replay of a
        // restored/adopted transcript must never light a just-resumed idle claude Running) — whose
        // tail says a turn is in progress; the synthesized event goes through the ONE state machine
        // (OnHookEvent) exactly like the missed-Stop: ts stamped (also refreshes the decay anchor),
        // EMPTY promptText (no Flight-Plan side effects — NoteExternalPrompt in _readDelta owns the
        // prompt back-fill, and the echo bookkeeping stays push-owned, so a late real
        // UserPromptSubmit lands on Running -> Running, a no-op). The re-Get mirrors the
        // missed-Stop's freshest-state re-check: a real hook that landed mid-pass wins.
        if (ShouldSynthesizeRunning(s.state, consumedTurnEvent, wasPrimed, st.lastStopReason, NowMs() - FiletimeToUnixMs(fad.ftLastWriteTime)))
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && (fresh->state == SessionState::Idle || fresh->state == SessionState::WaitingForInput))
            {
                HookMessage run;
                run.event = HookEvent::UserPromptSubmit;
                run.sessionId = s.id;
                run.cwd = s.workingDir;
                run.ts = NowMs();
                _registry->OnHookEvent(run);
                AppendStateLog(L"scanner.log", L"[recon-run] " + s.id + L" (turn in progress, prompt hook missed/folded)\n");
            }
        }

        // Resumed-from-NeedsApproval reconciliation — the NeedsApproval MIRROR of recon-run above.
        // The user ANSWERED the blocking question / permission and the agent is WORKING AGAIN (fresh
        // assistant output, the pending interactive tool cleared, the turn not yet over). NeedsApproval's
        // only OTHER pull-side exit is -> WaitingForInput at the turn's END (recon-stop), so without this
        // a session answered MID-turn kept showing "needs you" (orange) for the rest of the turn even
        // with new lines streaming in. The PURE gate (ShouldSynthesizeResumed) keys on a freshly-appended
        // turn event consumed by an already-PRIMED cursor whose tail is in-progress, and its interrupt +
        // terminal-stop guards make it mutually exclusive with recon-stop below (a finished/aborted turn
        // is recon-stop's -> Waiting). Synthesized as tool ACTIVITY (PostToolUse -> Running through the
        // ONE state machine), NOT a UserPromptSubmit, which from NeedsApproval would wrongly inflate the
        // type-ahead queue accounting (++queuedPrompts) and could strand the next real Stop as Running.
        // The re-Get mirrors the other synths' freshest-state re-check, so a real hook that landed
        // mid-pass wins.
        if (ShouldSynthesizeResumed(s.state, consumedTurnEvent, wasPrimed, st.pendingInteractiveTool, st.lastStopReason, st.interrupted, NowMs() - FiletimeToUnixMs(fad.ftLastWriteTime)))
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && fresh->state == SessionState::NeedsApproval)
            {
                HookMessage resume;
                resume.event = HookEvent::PostToolUse;
                resume.sessionId = s.id;
                resume.cwd = s.workingDir;
                resume.ts = NowMs();
                _registry->OnHookEvent(resume);
                AppendStateLog(L"scanner.log", L"[recon-resume] " + s.id + L" (answered -> working again, NeedsApproval -> Running)\n");
            }
        }

        // Agentmaster (subagent/fork activity): the parent <id>.jsonl can sit QUIESCENT while the
        // turn's real work happens elsewhere — a Task/Agent SUBAGENT writing <id>/subagents/*.jsonl
        // (the parent does not grow until the subagent returns), or, after a live /fork|/clear|
        // /compact|/resume, a NEW conversation id the tab has not yet re-bound to. Two out-of-band
        // signals recover "still working" without screen-scraping: the newest subagent/tool-result
        // side-file write (SubagentActivityUnixMs), and claude's OWN presence heartbeat ("busy",
        // pid-validated by the S-lane — Rule #13: a FACT the scanner may consume as a state input).
        // Fold both into the quiescence clock so the missed-Stop / blocked-on-user synths below do
        // NOT demote a working session: a fresh subagent write counts as a recent transcript write,
        // and a "busy" heartbeat forces quietForMs to 0 (claude says it is working — never synthesize
        // a turn-end). With neither signal present this is exactly the prior parent-only value, so a
        // genuinely idle session reconciles unchanged.
        const int64_t parentQuietMs = NowMs() - FiletimeToUnixMs(fad.ftLastWriteTime);
        const int64_t subagentActivityMs = SubagentActivityUnixMs(st.path);
        const bool presenceBusy = PresenceIsBusy(s.presenceStatus);
        const bool subagentActive = subagentActivityMs > 0 && (NowMs() - subagentActivityMs) <= kScanSubagentFreshMs;
        int64_t quietForMs = parentQuietMs;
        if (subagentActivityMs > 0)
        {
            const int64_t subQuietMs = NowMs() - subagentActivityMs;
            if (subQuietMs < quietForMs)
            {
                quietForMs = subQuietMs; // the conversation IS being written (just not the main file)
            }
        }
        if (presenceBusy)
        {
            quietForMs = 0; // claude self-reports working — the transcript may be momentarily quiet, the turn is not over
        }

        // Subagent/fork activity reconciliation — the EXTERNAL-WORK mirror of recon-run above.
        // recon-run keys on a PARENT-transcript append; this fires when the work is OUTSIDE the
        // parent transcript: a subagent is writing <id>/subagents/*.jsonl, or presence says "busy"
        // (which also follows a live /fork to a new id before the observer re-binds the tab). A
        // session sitting Idle / WaitingForInput while demonstrably WORKING must read Running.
        // Synthesized as tool ACTIVITY (PostToolUse -> Running through the ONE state machine), NOT a
        // UserPromptSubmit (which from these states would inflate the type-ahead queue accounting).
        // The pure gate guards BOTH arms on a NON-terminal tail, so neither a stale "busy" lingering
        // right after a real Stop NOR a subagent's final write (which lands µs before the parent's
        // end_turn) can bounce a just-Waiting session back to Running. The re-Get mirrors the other
        // synths' freshest-state re-check (a real hook landing mid-pass wins), and — gated to
        // Idle/Waiting — it self-limits: once it lands Running it stops re-firing.
        if (ShouldSynthesizeRunningFromExternalWork(s.state, subagentActive, presenceBusy, st.lastStopReason, st.interrupted))
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && (fresh->state == SessionState::Idle || fresh->state == SessionState::WaitingForInput))
            {
                HookMessage act;
                act.event = HookEvent::PostToolUse;
                act.sessionId = s.id;
                act.cwd = s.workingDir;
                act.ts = NowMs();
                _registry->OnHookEvent(act);
                AppendStateLog(L"scanner.log",
                               L"[recon-subagent] " + s.id + L" (" + (presenceBusy ? L"presence=busy" : L"subagent active") +
                                   L", " + (fresh->state == SessionState::Idle ? L"Idle" : L"Waiting") + L" -> Running)\n");
            }
        }

        // API-error reconciliation: the turn DIED with an API error — Claude Code wrote a synthetic
        // assistant message with isApiErrorMessage:true (a rate/usage limit, "Prompt is too long", a
        // 4xx/5xx, a dropped connection, an overloaded server, …) and it is still the tail. That line
        // carries a TERMINAL stop_reason, so WITHOUT this the missed-Stop backstop below would read it as
        // a clean turn-complete and land WaitingForInput — hiding the failure. Synthesize Error INSTEAD,
        // through the ONE state machine (the engine-internal apiError flag), and check it BEFORE recon-stop
        // so it takes precedence. The early return keeps recon-stop / recon-idle from also firing this
        // pass. The session LEAVES Error on the first new turn event: a real UserPromptSubmit (push) lands
        // Running, and recon-run (ShouldSynthesizeRunning, which now includes Error) is the pull backstop —
        // both keyed off the parser clearing st.lastWasApiError the instant a later turn event supersedes
        // the error. The re-Get mirrors the other synths' freshest-state re-check (a real hook wins).
        if (ShouldSynthesizeError(s.state, st.lastWasApiError, quietForMs))
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && fresh->state != SessionState::Error && fresh->state != SessionState::Done)
            {
                HookMessage err;
                err.event = HookEvent::Notification; // neutral carrier; the apiError flag drives the transition
                err.apiError = true;
                err.sessionId = s.id;
                err.cwd = s.workingDir;
                err.ts = NowMs();
                // Preserve the failure reason for the Triage-Board Error card: the message + HTTP status,
                // both captured DIRECTLY from the error line when it was consumed (st.lastApiError*).
                err.errorMessage = st.lastApiErrorMessage;
                err.errorStatus = st.lastApiErrorStatus;
                _registry->OnHookEvent(err);
                std::wstring why = st.lastApiErrorMessage; // the "API Error: …" text, captured from the error line
                for (auto& c : why)
                {
                    if (c == L'\r' || c == L'\n')
                    {
                        c = L' '; // keep the log a single line
                    }
                }
                if (why.size() > 160)
                {
                    why.resize(160);
                    why += L"…";
                }
                AppendStateLog(L"scanner.log", L"[recon-error] " + s.id + L" (" + why + L")\n");
            }
            return; // the errored turn owns this pass — don't let recon-stop / recon-idle also fire
        }

        // Missed/forced-Stop reconciliation: the turn is OVER — either the transcript's last
        // assistant message ended it (a TERMINAL stop_reason — end_turn / stop_sequence /
        // max_tokens / refusal) or the user INTERRUPTED it (Esc -> no clean Stop hook) — the file
        // has gone quiescent, yet we are STILL Running (or blocked in NeedsApproval whose
        // post-approval / post-answer Stop was dropped). Synthesize a Stop identical to the real
        // one (-> WaitingForInput + the question-guard + the Autopilot advance). The state gate
        // (re-checked against the freshest state right before firing) makes a real Stop that
        // already landed win, so this never double-fires.
        const bool stopFromTail = ShouldSynthesizeStop(s.state, st.lastStopReason, st.interrupted, quietForMs);
        // Agentmaster (presence-idle release): the missing IDLE half of the presence signal. The
        // tail-based backstop above needs a TERMINAL stop_reason or an interrupt — but a turn can end
        // with NEITHER: its last transcript line is a bare user prompt that cleared the tracked
        // stop_reason (_readDelta) and then produced NO assistant output / fired NO Stop hook (a
        // dropped Stop, or a no-op turn), stranding the session Running forever while claude is
        // demonstrably at rest. claude's OWN pid-validated heartbeat saying "idle" is the authority
        // that the turn is OVER (PresenceIsBusy's release mirror). Gated on no pending interactive tool
        // so it never pre-empts recon-block (the blocked-on-question -> NeedsApproval path below), and
        // on the (subagent/busy-folded) quietForMs, so a live subagent / "busy" heartbeat — which
        // already forces that clock small — can never trip it mid-work. For a RUNNING cleared-tail turn
        // the predicate additionally demands a MUCH longer quiescence (kScanPresenceIdleRunningQuiescenceMs)
        // than NeedsApproval: that shape is indistinguishable from a turn paused behind a "No response
        // from API · Retrying" backoff, so only sustained rest (no append AND no "busy" for the long
        // window — both reset quietForMs just below) releases it (the idle<->running flap fix).
        const bool stopFromPresenceIdle = !stopFromTail && st.pendingInteractiveTool.empty() &&
                                          ShouldSynthesizeStopFromPresenceIdle(s.state, s.presenceStatus, st.lastStopReason, st.interrupted, quietForMs);
        if (stopFromTail || stopFromPresenceIdle)
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && (fresh->state == SessionState::Running || fresh->state == SessionState::NeedsApproval))
            {
                HookMessage stop;
                stop.event = HookEvent::Stop;
                stop.sessionId = s.id;
                stop.cwd = s.workingDir;
                stop.ts = NowMs();
                // Synthesized from a >=2s-QUIESCENT transcript: turn identity is settled, so
                // the ordered machine (NextSessionStateOrdered) lands WaitingForInput
                // unconditionally — never held Running by a recorded type-ahead (already
                // consumed or canceled), never treated as stale.
                stop.quiescentStop = true;
                // Only a TAIL-derived stop reflects a real last assistant message; a presence-idle
                // release ends a turn that produced no assistant output, so it carries no question.
                stop.lastMessageIsQuestion = stopFromTail && !st.interrupted && EndsWithQuestion(st.lastAssistantText);
                _registry->OnHookEvent(stop);
                AppendStateLog(L"scanner.log",
                               (stopFromPresenceIdle ? L"[recon-stop-idle] " : L"[recon-stop] ") + s.id +
                                   L" q=" + (stop.lastMessageIsQuestion ? L"1" : L"0") +
                                   (st.interrupted ? L" interrupted" : L"") +
                                   (stopFromPresenceIdle ? (L" presence=" + s.presenceStatus) : std::wstring{}) + L"\n");
            }
        }

        // Blocked-on-user reconciliation: the latest assistant message is an UNANSWERED interactive
        // tool_use (AskUserQuestion) and the transcript has gone quiescent — the session is blocked
        // waiting for the user to answer, NOT working, so it must not show Running. Synthesize a
        // permission-style Notification (-> NeedsApproval, the "needs you" column). Idempotent: only
        // from Running, so once it lands NeedsApproval it stays until the answer + the turn's end
        // release it via the missed-Stop reconciliation above. (Autopilot treats NeedsApproval as
        // NOT ready — Rule #1 — so it won't auto-answer the question with a queued prompt.)
        if (ShouldSynthesizeBlockedOnUser(s.state, st.pendingInteractiveTool, st.interrupted, quietForMs))
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && fresh->state == SessionState::Running)
            {
                HookMessage block;
                block.event = HookEvent::Notification;
                block.permissionRequest = true; // -> NeedsApproval via the one state machine
                block.sessionId = s.id;
                block.cwd = s.workingDir;
                block.ts = NowMs();
                _registry->OnHookEvent(block);
                AppendStateLog(L"scanner.log",
                               L"[recon-block] " + s.id + L" (unanswered " + st.pendingInteractiveTool + L" -> needs you)\n");
            }
        }
    }

    bool SessionScanner::_readDelta(ScanState& st, const SessionInfo& s, int64_t size)
    {
        const int64_t avail = size - st.offset;
        if (avail <= 0)
        {
            st.primed = true; // nothing past the cursor: it IS the file end — caught up
            return false;
        }
        const DWORD want = static_cast<DWORD>(avail < kScanMaxDeltaBytes ? avail : kScanMaxDeltaBytes);

        // Share everything: claude has the file open for append; never block its writes.
        const HANDLE h = ::CreateFileW(st.path.c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_SEQUENTIAL_SCAN,
                                       nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER li{};
        li.QuadPart = st.offset;
        if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        {
            ::CloseHandle(h);
            return false;
        }
        std::string bytes;
        bytes.resize(want);
        DWORD got = 0;
        const BOOL ok = ::ReadFile(h, bytes.data(), want, &got, nullptr);
        ::CloseHandle(h);
        if (!ok || got == 0)
        {
            return false;
        }
        bytes.resize(got);

        // Consume only up to the last newline; a trailing partial line is an append in flight and
        // is re-read whole next tick. (Splitting on the '\n' BYTE is UTF-8-safe: 0x0A is never a
        // continuation byte.)
        const size_t nl = bytes.rfind('\n');
        if (nl == std::string::npos)
        {
            // No complete line in this window. Guard against a corrupt/binary run wedging the
            // cursor forever: if the unterminated run is pathologically large, skip past it.
            if (avail > kScanForceConsumeBytes)
            {
                st.offset = size;
                st.primed = true; // skipped to the end — caught up
            }
            else if (avail <= kScanMaxDeltaBytes && static_cast<int64_t>(got) == avail)
            {
                st.primed = true; // the whole remainder is one partial line in flight — caught up on complete lines
            }
            return false;
        }
        const size_t completeBytes = nl + 1;
        const std::wstring wide = Utf8ToUtf16(bytes.data(), static_cast<int>(completeBytes));
        st.offset += static_cast<int64_t>(completeBytes);
        if (avail <= kScanMaxDeltaBytes && static_cast<int64_t>(got) == avail)
        {
            // This read reached the file's current end (everything available fit the window):
            // the cursor is caught up — anything consumed on a LATER pass is a live append, so
            // the run-repair may key on it. (A capped backlog chunk — avail > the window — is
            // still mid-replay and does NOT prime; the pass that finishes the replay primes for
            // the NEXT one.)
            st.primed = true;
        }

        const auto parsed = ParseTranscriptDelta(wide);
        // Agentmaster: mirror the Claude Code idle RECAP (away_summary) onto the registry record so the
        // Triage-Board card tooltip can show it. QUIET (display-only, like lastAssistantText below) and
        // only when THIS chunk carried a recap — away_summary lines are rare, so this is a no-op on
        // virtually every tick (no lock churn). On a first-sight replay-from-0 the last recap in the file
        // wins; a live recap lands in its own delta. Never clears an existing recap (empty is ignored).
        if (!parsed.recap.empty())
        {
            const std::wstring recap = parsed.recap;
            _registry->UpdateQuiet(s.id, [&recap](SessionInfo& ss) {
                if (ss.recap != recap)
                {
                    ss.recap = recap;
                }
            });
        }
        bool consumedTurnEvent = false; // a human prompt / assistant line (NOT a bare tool_result)
        for (const auto& ev : parsed.events)
        {
            if (ev.kind == TranscriptEvent::Kind::Assistant)
            {
                consumedTurnEvent = true;
                st.lastStopReason = ev.stopReason; // latest assistant line wins (tool_use -> not done)
                st.interrupted = false; // fresh assistant output: the turn is progressing, not aborted
                // API-error tail tracking: the synthetic isApiErrorMessage line sets this; ANY other
                // (non-error) assistant line clears it — so it always reflects whether the NEWEST event
                // is an unrecovered API error (-> ShouldSynthesizeError). A non-error assistant line
                // appended after an error is the session resuming, which clears Error.
                st.lastWasApiError = ev.apiError;
                if (ev.apiError)
                {
                    // Capture the reason DIRECTLY from the error event (not via lastAssistantText, whose
                    // mirror below skips an empty-text line) so the card never shows a stale message.
                    st.lastApiErrorMessage = ev.text;
                    st.lastApiErrorStatus = ev.apiErrorStatus;
                }
                // The latest assistant block sets / clears the "blocked on the user" flag: an
                // interactive tool_use (AskUserQuestion) parks the turn on the user until answered;
                // a text / non-interactive-tool message means the agent moved on (clear it).
                st.pendingInteractiveTool = ev.toolName; // "" unless this message is an interactive tool_use
                if (!ev.text.empty())
                {
                    st.lastAssistantText = ev.text;
                    // Mirror into the model for a future Flight-Plan "peek". QUIET: streamed text
                    // must not trigger the persist / UI / scheduler cascade on every line.
                    const std::wstring text = ev.text;
                    _registry->UpdateQuiet(s.id, [&text](SessionInfo& ss) { ss.lastAssistantText = text; });
                }
                // Context occupancy: the newest assistant usage wins. Mirror QUIETLY (like
                // lastAssistantText) — the board recomputes the % on its next rebuild (a turn in
                // progress already polls fast), so this must not drive its own notify cascade.
                if (ev.tokens > 0 && ev.tokens != st.contextTokens)
                {
                    st.contextTokens = ev.tokens;
                    const int64_t tok = ev.tokens;
                    _registry->UpdateQuiet(s.id, [tok](SessionInfo& ss) { ss.contextTokens = tok; });
                }
            }
            else if (ev.kind == TranscriptEvent::Kind::ToolResult)
            {
                // A tool produced a result -> a pending interactive tool_use (the question) was
                // ANSWERED. (Deliberately NOT a turn event for the run-repair: a bare tool_result
                // never synthesized Running before — preserve that.)
                st.pendingInteractiveTool.clear();
                st.lastWasApiError = false; // activity past any error -> the tail is no longer that error
            }
            else // UserPrompt: a new human turn began, OR a turn-abort interrupt marker
            {
                consumedTurnEvent = true;
                // CRITICAL: a new user message starts a fresh turn, so the PRIOR assistant
                // end_turn no longer marks the CURRENT turn complete. Clear the tracked
                // stop_reason, else the missed-Stop backstop could fire on that stale end_turn
                // while claude is mid-(new-)turn — declaring turn-complete and draining the plan
                // into a running turn. (tool_result user lines don't reach here — they're a
                // ToolResult event above — so this only resets on a genuine human line.)
                st.lastStopReason.clear();
                st.pendingInteractiveTool.clear(); // a human line supersedes any pending question
                st.lastWasApiError = false; // a new human turn supersedes a prior API error -> recovery (Error -> Running)
                if (IsUserInterruptMarker(ev.text))
                {
                    // The user hit Esc: NO clean Stop hook fires, and the marker clears the
                    // stop_reason — so without this flag the turn's end goes unseen and the
                    // session stays Running forever. Flag it; recon-stop releases it to Waiting.
                    st.interrupted = true;
                }
                else
                {
                    st.interrupted = false;
                    // Control markers that masquerade as user lines — command echoes, task
                    // notifications (STATE.md §8 bug-2) — must not be back-filled into the Flight
                    // Plan as Typed prompts. They still clear the stop_reason above.
                    if (!IsNoiseUserPrompt(ev.text))
                    {
                        _registry->NoteExternalPrompt(s.id, ev.text); // idempotent by text — back-fills a dropped hook
                    }
                }
            }
        }
        // ≥1 human prompt / assistant line consumed -> the caller may synthesize a missed
        // UserPromptSubmit off it (a bare tool_result / meta / garbage line never counts).
        return consumedTurnEvent;
    }

    // Agentmaster (Waiting-for-you "unread" model): a session in WaitingForInput is the Triage Board's
    // "answer me" inbox. It demotes to Idle (the card moves to "Idle / Done") only once the timeout has
    // elapsed AND the user has READ it (visited its tab since the last turn) — so it persists for the
    // FULL timeout regardless of reading, and past the timeout it keeps waiting while still unread; a
    // manual "Mark Unread" never time-decays at all. The pure gate ShouldDecayWaitingToIdle decides;
    // PresenceIsBusy is folded in so a long Task/Agent subagent (busy heartbeat, quiescent parent
    // transcript — the recon-subagent promotion's target) is never raced to Idle. Like the synthesized
    // missed-Stop above, this is a deliberate TIME-derived transition layered on the hook-derived machine
    // (Rule #7-adjacent — never screen-scraped): the mutator RE-CHECKS the full gate UNDER the registry
    // lock, so a hook / a visit (read stamp) landing between our snapshot and the update wins. Autopilot
    // semantics are unchanged: DecideAdvance treats Idle as ready exactly like WaitingForInput (Rule #1).
    void SessionScanner::_maybeDecayWaiting(const SessionInfo& s, int64_t nowMs)
    {
        const uint32_t minutes = _waitingDecayMinutes.load();
        const bool busy = PresenceIsBusy(s.presenceStatus);
        if (!ShouldDecayWaitingToIdle(s.state, s.lastActivityUnixMs, s.readUnixMs, s.manualUnread, busy, minutes, nowMs))
        {
            return;
        }
        bool decayed = false;
        _registry->Update(s.id, [&](SessionInfo& live) {
            // Re-check the full gate under the lock against the LIVE record (a visit may have just
            // stamped readUnixMs / cleared manualUnread, or a fresh turn may have moved the state).
            if (ShouldDecayWaitingToIdle(live.state, live.lastActivityUnixMs, live.readUnixMs, live.manualUnread, PresenceIsBusy(live.presenceStatus), minutes, nowMs))
            {
                live.state = SessionState::Idle;
                decayed = true;
            }
        });
        if (decayed)
        {
            AppendStateLog(L"scanner.log",
                           L"[decay-waiting] " + s.id + L" read + waited " + std::to_wstring((nowMs - s.lastActivityUnixMs) / 60000) +
                               L"m (>= " + std::to_wstring(minutes) + L"m) -> Idle\n");
        }
    }

    void SessionScanner::_maybeSweepLiveness(int64_t nowMs, bool anyLive)
    {
        if (!anyLive)
        {
            return; // nothing to probe
        }
        if (_lastSweepMs != 0 && (nowMs - _lastSweepMs) < kScanSweepMs)
        {
            return; // rate-limited: a sweep marshals to the UI thread, so don't do it every fast tick
        }
        _lastSweepMs = nowMs;

        std::vector<LivenessProbe> probes;
        {
            std::lock_guard lk{ _mtx };
            probes.reserve(_probes.size());
            for (const auto& p : _probes)
            {
                probes.push_back(p.second);
            }
        }
        for (auto& pr : probes)
        {
            if (pr)
            {
                try
                {
                    pr();
                }
                catch (...)
                {
                }
            }
        }
    }

    void SessionScanner::ArmDiscovery()
    {
        // Retained (despite the now-historical name) to keep the worker TICKING even with nothing
        // live, so each window's liveness probe — and with it the Fleet Observer's per-window roster
        // publish (_ObserverProbe) — keeps running. The transcript-discovery ENUMERATION this used to
        // also start is retired (O7): the observer's out-of-band PEB correlation subsumes it.
        _discoverArmed.store(true);
        Wake(); // break the idle wait so the probe starts ticking immediately
    }
}
