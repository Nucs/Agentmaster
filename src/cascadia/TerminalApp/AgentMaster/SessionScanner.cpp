// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). The transcript tail is raw Win32 file I/O.
#define NOMINMAX
#include "SessionScanner.h"

#include "ClaudeSpawn.h" // ResolveClaudeTranscriptPath, AppendStateLog
#include "CommandWatch.h" // ParseCommandEcho + the slash-command binding/await feeds (COMMANDS.md)
#include "Json.h"
#include "ProcessInspect.h" // SubagentActivityUnixMs — subagent/Task side-file activity (the parent transcript stays quiescent while a subagent runs)
#include "Scheduler.h" // DecideLostSend — the lost-send verdict this reconcile pass applies (DELIVERY_PLAN.md R1)
#include "SessionRegistry.h"
#include "TranscriptStore.h" // IsNoiseUserPrompt — keep control markers out of the Auto-Testing back-fill
#include "TranscriptWatcher.h" // HOOKS.md "Transcript watcher" — the ReadDirectoryChangesW change feed that replaces per-pass polling

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

    // Agentmaster (COMMANDS.md): the file-WRITING tool_use paths of this message — each
    // Write/Edit (and legacy MultiEdit) block's input.file_path, in block order. Feeds the
    // CommandWatch's markdown await (the /handover "look for the .md being written" signal).
    // A read-only tool (Read/Grep/Glob/Bash/…) never collects; a missing/empty file_path is
    // skipped. Cheap: the line's JSON DOM is already parsed — this is a field walk, no re-parse.
    std::vector<std::wstring> CollectFileWritePaths(const Agentmaster::json::Value* content)
    {
        using Agentmaster::json::Value;
        std::vector<std::wstring> paths;
        if (!content || content->type != Value::Type::Arr)
        {
            return paths;
        }
        for (const auto& blk : content->arr)
        {
            if (blk.type != Value::Type::Obj || blk.StrAt(L"type") != L"tool_use")
            {
                continue;
            }
            const std::wstring name = blk.StrAt(L"name");
            if (name != L"Write" && name != L"Edit" && name != L"MultiEdit")
            {
                continue;
            }
            if (const auto* input = blk.Find(L"input"); input && input->type == Value::Type::Obj)
            {
                if (std::wstring fp = input->StrAt(L"file_path"); !fp.empty())
                {
                    paths.push_back(std::move(fp));
                }
            }
        }
        return paths;
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
            // Forensics: OutputDebugString needs a debugger ATTACHED AT THE TIME to be seen, so on a
            // user's machine this was effectively silent. An empty delta = "no state change this
            // pass", which looks exactly like a quiet session — the state machine degrades invisibly.
            // Throttled per context (~2s), so a persistently-malformed transcript can't flood the log.
            LogSwallowedException(L"ParseTranscriptDelta");
            return {};
        }
    }
    static TranscriptParse ParseTranscriptDeltaImpl(std::wstring_view chunk)
    {
        TranscriptParse out;
        // Agentmaster: emit a Node (lineage-only) event for a non-turn BOOKKEEPING line so the scanner can
        // follow the API-error's forward descendant chain (the error -> a system/turn_duration child -> an
        // away_summary; the post-error last-prompt marker names the turn_duration child). Only when BOTH
        // uuid + parentUuid are present — a mode/permission-mode config line carries neither, so it is
        // skipped and never enters the chain.
        const auto emitNode = [&out](const json::Value& o) {
            std::wstring u = o.StrAt(L"uuid");
            std::wstring p = o.StrAt(L"parentUuid");
            if (u.empty() || p.empty())
            {
                return;
            }
            TranscriptEvent ev;
            ev.kind = TranscriptEvent::Kind::Node;
            ev.uuid = std::move(u);
            ev.parentUuid = std::move(p);
            out.events.push_back(std::move(ev));
        };
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
                ev.uuid = obj.StrAt(L"uuid"); // the error line's OWN uuid -> seeds the descendant frontier (errorBranchUuids)
                // The model that produced this message — the CURRENT model of the conversation (the
                // newest line wins downstream). The API-error line's "<synthetic>" is carried as-is
                // here (the parser stays a plain extractor); the fold skips it.
                ev.model = msg->StrAt(L"model");
                const auto* content = msg->Find(L"content");
                ev.text = CollectText(content);
                ev.toolName = CollectInteractiveToolName(content); // "" unless an interactive tool_use is present
                ev.fileWritePaths = CollectFileWritePaths(content); // Write/Edit file_path values (COMMANDS.md markdown await)
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
                // Agentmaster (HOOKS.md "Compaction"): a usage-bearing assistant line AFTER a compact
                // boundary in this chunk is the newer context truth — drop the boundary's pending
                // postTokens so the fold keeps this line's value (file order decides, never the kind).
                if (ev.tokens > 0)
                {
                    out.compactPostTokens = 0;
                }
                out.events.push_back(std::move(ev));
            }
            else if (type == L"user")
            {
                // Skip meta / synthetic user lines (command echoes, injected reminders, etc.).
                // isCompactSummary == the synthetic "This session is being continued…" bridge a
                // /compact (or auto-compaction) injects — machine text, not a human turn, and it is
                // HUGE (the whole summary): emitting it as a UserPrompt back-filled it into the
                // Typed record (91 rows ≈ 1.36 MB of machine text found persisted in the prod
                // registry, rewritten on every autosave) and read as a turn START. Every other
                // transcript reader (ReadTranscriptInfo / AnalyzeSessionTranscript /
                // ClassifyTranscriptLine) already skips the flag exactly like isMeta — this parser
                // was the lone exception. The post-compact auto-continuation (which fires NO
                // UserPromptSubmit hook) still lights recon-run off its ASSISTANT lines one pass
                // later, the same pattern as the wake wrappers.
                if (obj.BoolAt(L"isMeta") || obj.BoolAt(L"isCompactSummary"))
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
                    // Agentmaster (the /model false-Running fix): a machine-injected CONTROL MARKER
                    // masquerading as a user line is NOT a human turn — skip it like an isMeta line.
                    // A LOCAL slash command (`/model`, `/config`, …) writes a NON-meta
                    // "<command-name>…" echo + a "<local-command-stdout>…" result and starts NO API
                    // turn at all (and fires NO UserPromptSubmit hook — verified live), yet emitting
                    // them as UserPrompt turn events made the reconciler treat a mere `/model` as a
                    // turn START: recon-run lit an Idle/Waiting session Running within one tick
                    // (proven live: session de4fcb12 — `/model` echo at 05:29:26.392Z, [recon-run]
                    // at .952), the wiped tail facts (the prior turn's TERMINAL stop_reason, a
                    // pending AskUserQuestion, an unrecovered API error) also released NeedsApproval
                    // (ShouldSynthesizeResumed) and Error (recon-run's recovery edge), and the now-
                    // cleared stop_reason left the phantom turn releasable only by the 30s
                    // presence-idle floor — landing WaitingForInput, never the original state. The
                    // shared prefix filter (IsNoiseUserPrompt, TranscriptStore) also covers the `!`
                    // bash passthrough echoes, injected reminders/caveats, and the teammate/agent/
                    // task-notification WAKE wrappers — the wakes lose nothing: their turn is REAL,
                    // so a wired session's state rides the real UserPromptSubmit hook (push), and a
                    // hookless one lights recon-run off the wake turn's ASSISTANT lines one pass
                    // later. The interrupt marker is in the noise list too but is a real turn-ENDER
                    // the reconciler must keep seeing (IsUserInterruptMarker → recon-stop), so it is
                    // exempted and still flows through as a UserPrompt event.
                    if (IsNoiseUserPrompt(prompt) && !IsUserInterruptMarker(prompt))
                    {
                        // Agentmaster (COMMANDS.md): a slash-command ECHO stays noise for the state
                        // machine (dropped as a prompt, never a turn event — the /model false-Running
                        // fix is untouched) but is surfaced as an ORDERED Command event so CommandWatch
                        // bindings can key on it (the /handover integration). Current Claude Code
                        // writes even built-ins as this user-line shape; the tag order varies across
                        // strata, so the extractor is order-agnostic. The line's own timestamp rides
                        // along as the watch's replay guard.
                        if (SlashCommand sc; ParseCommandEcho(prompt, sc))
                        {
                            TranscriptEvent ev;
                            ev.kind = TranscriptEvent::Kind::Command;
                            ev.commandName = std::move(sc.name);
                            ev.commandArgs = std::move(sc.args);
                            ev.lineTsMs = ParseTranscriptTimestamp(obj.StrAt(L"timestamp"));
                            out.events.push_back(std::move(ev));
                        }
                        continue;
                    }
                    TranscriptEvent ev;
                    ev.kind = TranscriptEvent::Kind::UserPrompt;
                    ev.text = std::move(prompt);
                    // The line's own timestamp rides along for the PULL echo-consume's staleness
                    // filter (DELIVERY_PLAN.md R1 — NoteExternalPrompt's observedUnixMs): an old
                    // replayed identical line must never vouch for a NEW send's delivery.
                    ev.lineTsMs = ParseTranscriptTimestamp(obj.StrAt(L"timestamp"));
                    out.events.push_back(std::move(ev));
                }
            }
            else if (type == L"system")
            {
                // Agentmaster (COMMANDS.md): the OLDER slash-command echo shape — a
                // `system/local_command` line carrying the same <command-name>/<command-args> tags
                // (the June-2026 strata; current Claude Code writes the user-line shape above).
                // Emitted as a Command event exactly like that shape; the line still ALSO feeds
                // emitNode below (lineage is orthogonal).
                if (obj.StrAt(L"subtype") == L"local_command")
                {
                    if (SlashCommand sc; ParseCommandEcho(obj.StrAt(L"content"), sc))
                    {
                        TranscriptEvent ev;
                        ev.kind = TranscriptEvent::Kind::Command;
                        ev.commandName = std::move(sc.name);
                        ev.commandArgs = std::move(sc.args);
                        ev.lineTsMs = ParseTranscriptTimestamp(obj.StrAt(L"timestamp"));
                        out.events.push_back(std::move(ev));
                    }
                }
                if (obj.StrAt(L"subtype") == L"away_summary")
                {
                    // Agentmaster: the Claude Code idle RECAP. NOT a turn event (it never touches the state
                    // machine) — captured out-of-band so the scanner can mirror it onto SessionInfo.recap and
                    // the CLI can surface it. Last recap in the chunk wins; an empty body never clears it.
                    if (std::wstring r = NormalizeRecapText(obj.StrAt(L"content")); !r.empty())
                    {
                        out.recap = std::move(r);
                    }
                }
                if (obj.StrAt(L"subtype") == L"compact_boundary")
                {
                    // Agentmaster (HOOKS.md "Compaction"): the conversation was just summarized IN PLACE —
                    // its context is now compactMetadata.postTokens (Claude Code stamps it before writing
                    // the line; absent on older builds -> 0 -> the fold leaves the prior value). NOT a turn
                    // event: a compaction neither starts nor ends a turn, and the boundary's parentUuid is
                    // null, so emitNode below skips it (it is a chain ROOT, not an error bookkeeping child).
                    if (const auto* md = obj.Find(L"compactMetadata"); md && md->type == json::Value::Type::Obj)
                    {
                        if (const int64_t post = md->I64At(L"postTokens"); post > 0)
                        {
                            out.compactPostTokens = post;
                        }
                    }
                }
                // A system line (turn_duration child, away_summary, …) is on the API-error's bookkeeping
                // chain — emit its lineage so the descendant frontier can follow it (errorBranchUuids).
                emitNode(obj);
            }
            else if (type == L"last-prompt")
            {
                // Agentmaster: the ACTIVE-LEAF pointer. Claude writes {"type":"last-prompt","leafUuid":…}
                // naming the conversation's current branch head whenever the active leaf moves — most
                // importantly a double-ESC REWIND repoints it BACKWARD. Emitted as an ordered (non-turn)
                // event so the scanner sees it relative to an API-error line and can tell the error is no
                // longer the tail (the leaf moved off it) -> leave Error (ShouldReleaseErrorOnLeafMove).
                // A last-prompt with no leafUuid (e.g. the trust-dialog prompt) carries no leaf — skip it
                // so it never clobbers the tracked leaf to empty.
                if (std::wstring leaf = obj.StrAt(L"leafUuid"); !leaf.empty())
                {
                    TranscriptEvent ev;
                    ev.kind = TranscriptEvent::Kind::LeafMarker;
                    ev.text = std::move(leaf);
                    out.events.push_back(std::move(ev));
                }
            }
            else
            {
                // Any other line type (e.g. "progress" — also an API-error child in the corpus) — emit
                // lineage only, for the same error descendant-frontier tracking as the system child above.
                emitNode(obj);
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
            // are the initial HISTORY REPLAY (a restored/adopted session's first read: its tail
            // window — FirstSightReadOffset — or the whole file when it fits) — not a live append.
            // A window closed mid-turn leaves that history
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
        // HOOKS.md "Transcript watcher": the change feed comes up BEFORE the worker, so the worker's very first
        // pass can already ask it (it answers "not healthy yet" until its first index lands a moment later,
        // and the pass polls meanwhile). Its callback is Wake(): an append becomes a pass NOW instead of on the
        // next poll tick. A watcher that cannot start costs nothing but the old polling.
        if (const std::wstring projects = ClaudeProjectsDir(); !projects.empty())
        {
            try
            {
                auto watcher = std::make_unique<TranscriptWatcher>(projects);
                watcher->Start([this]() { Wake(); });
                _watcher = std::move(watcher);
            }
            catch (...)
            {
                // Forensics (Rule #18): the OS refused the watcher's thread or event. Keep scanning without it.
                LogSwallowedException(L"SessionScanner::Start (transcript watcher)");
            }
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
        // The worker is gone, so nothing reads the watcher any more: stop it (it cancels + drains its pending
        // read before closing) and release it. Outside the _running guard so a watcher whose worker failed to
        // start is still torn down.
        if (_watcher)
        {
            _watcher->Stop();
            _watcher.reset();
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
                // Forensics: the scanner worker's top-level net. A throw costs a WHOLE reconcile pass
                // (every session's missed-Stop / blocked-on-user / API-error synthesis), and if it
                // recurs the PULL state engine is effectively dead while the app looks healthy.
                LogSwallowedException(L"SessionScanner::_worker (_scanOnce)");
                sleepMs = kScanLiveIdleMs;
            }

            std::unique_lock lk{ _mtx };
            if (!_running.load())
            {
                return;
            }
            // COALESCE (HOOKS.md "Transcript watcher"): never start the next pass sooner than kScanMinPassGapMs
            // after this one, even when woken. The registry notifies on every hook of every session and the
            // watcher on every append batch; with dozens of live sessions those wakes used to chain passes back
            // to back. Only Stop() cuts the gap short; a Wake() landing inside it is kept (_woken) and makes the
            // wait below return at once.
            _cv.wait_for(lk, std::chrono::milliseconds(kScanMinPassGapMs), [this] { return !_running.load(); });
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
                // The gap already counted toward the cadence, so the adaptive tick (300 ms Running / 2.5 s idle)
                // is unchanged.
                const int64_t rest = sleepMs > kScanMinPassGapMs ? sleepMs - kScanMinPassGapMs : 0;
                _cv.wait_for(lk, std::chrono::milliseconds(rest), [this] { return !_running.load() || _woken; });
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

        // LIVE sessions only (SnapshotLive): every use below — the reconcile loop, the cursor cleanup, the
        // turn-in-flight probe — looks at live sessions alone, and the full Snapshot() deep-copied every
        // record the profile ever held (1,348 on the 2026-09-25 release fleet, each with its prompt history)
        // on every pass, i.e. several times a second while anything ran.
        const auto sessions = _registry->SnapshotLive();

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
                if (!live)
                {
                    if (_commandWatch)
                    {
                        _commandWatch->DropSession(it->first); // COMMANDS.md: a gone/archived session's pendings go with its cursor
                    }
                    it = _scan.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // COMMANDS.md: the per-pass sweep — deadline expiry + the disk poll that fires a matched
        // await the moment its file lands (a write held behind a permission approval materializes
        // between ticks). Steady state (no pendings) is one empty-check, no I/O.
        if (_commandWatch)
        {
            // TURN-IN-FLIGHT probe for the settle-seal (CommandWatch.h kCommandMatchSettleMs):
            // the settle must not seal a matched collection MID-TURN — the transcript is
            // byte-SILENT while Claude streams the next briefing's content, so one /handover's
            // several writes are routinely MINUTES apart (the 2026-07-26 clip: 2m09s between two
            // HANDOVER writes sealed at 20s with only file 1). The turn is still in flight when
            // this pass's own facts say so: the tracked tail is neither terminal nor interrupted
            // (else the boundary already fed OnTurnEnd) AND the process is live — a known pid
            // answers with kernel ground truth (a dead claude must settle exactly as before, and
            // stale presence enrichment must not outvote a dead pid), an unknown pid falls back
            // to claude's own heartbeat as positive work evidence. Consulted lazily (only for a
            // matched-unsealed pending whose settle window elapsed), µs when it runs at all.
            const auto turnInFlight = [&](const std::wstring& sid) -> bool {
                const auto sc = _scan.find(sid);
                if (sc == _scan.end() || !sc->second.primed)
                {
                    return false; // no caught-up cursor -> no tail judgment -> classic settle
                }
                const ScanState& st = sc->second;
                if (st.interrupted || IsTerminalStopReason(st.lastStopReason))
                {
                    return false; // the turn is over (the boundary itself fed OnTurnEnd)
                }
                for (const auto& s : sessions)
                {
                    if (s.live && s.id == sid)
                    {
                        return s.pid != 0 ? ProcessAlive(s.pid) : PresenceIsWorking(s.presenceStatus);
                    }
                }
                return false; // gone/archived -> DropSession territory, settle freely
            };
            _commandWatch->Tick(now, turnInFlight);
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
        const int64_t passNow = NowMs();
        // HOOKS.md "Transcript watcher": what the change feed knows about this session. Taken ONCE, BEFORE any
        // stat below, so the sequences a refresh records describe a state no later than what it reads. A
        // change landing mid-pass bumps them again and the next pass refreshes. No watcher (or not yet
        // indexed) reads as unhealthy, which is the classic poll-everything behavior.
        const TranscriptWatchStamp watch = _watcher ? _watcher->Stamp(s.id) : TranscriptWatchStamp{};
        if (st.path.empty())
        {
            if (watch.healthy)
            {
                // The index answers from memory. "" is AUTHORITATIVE (no transcript exists yet: never prompted);
                // the transcript's creation arrives as a notification that re-wakes this scanner.
                st.path = _watcher->Resolve(s.id);
                if (st.path.empty() && passNow >= st.nextResolveMs)
                {
                    // Belt, not a hot path: once a minute per still-unresolved session, check the index against
                    // the classic walk. A hit means the index missed a file, so log it for the next investigation.
                    st.nextResolveMs = passNow + kScanIndexCrossCheckMs;
                    if (std::wstring walked = ResolveClaudeTranscriptPath(s.id); !walked.empty())
                    {
                        AppendStateLog(L"scanner.log", L"[watch] index missed " + s.id + L" (found by the folder walk: " + walked + L")\n");
                        st.path = std::move(walked);
                    }
                }
            }
            else if (passNow >= st.nextResolveMs)
            {
                // No healthy watcher: the classic walk over every project folder, but BACKED OFF for a session
                // that has no transcript yet. It used to run every 300 ms pass for each never-prompted session
                // and was 28% of the scanner's time on the release fleet.
                st.path = ResolveClaudeTranscriptPath(s.id);
                if (st.path.empty())
                {
                    st.nextResolveMs = passNow + kScanResolveRetryMs;
                }
            }
            if (st.path.empty())
            {
                return; // no transcript yet (never prompted) — nothing to tail
            }
            st.statValid = false; // a newly resolved path: nothing cached describes it
            st.sideValid = false;
        }

        // HOT sessions keep the classic per-pass stat (their tail can demote them); COLD ones reuse the cached
        // stat until the watcher reports a change or the slow resync comes due (ShouldRefreshTranscriptStat).
        const bool hot = ScanNeedsFreshTail(s);
        if (ShouldRefreshTranscriptStat(watch.healthy, st.statValid, watch.epoch != st.seenStatEpoch, watch.transcriptSeq != st.seenTranscriptSeq, hot, passNow - st.lastStatMs))
        {
            st.seenStatEpoch = watch.epoch;
            st.seenTranscriptSeq = watch.transcriptSeq;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(st.path.c_str(), GetFileExInfoStandard, &fad))
            {
                st.path.clear(); // vanished / rotated -> re-resolve next pass
                st.statValid = false;
                st.sideValid = false;
                return;
            }
            st.statSize = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | static_cast<int64_t>(fad.nFileSizeLow);
            st.statWriteUnixMs = FiletimeToUnixMs(fad.ftLastWriteTime);
            st.statValid = true;
            st.lastStatMs = passNow;
        }
        const int64_t size = st.statSize;
        // The transcript's last write (the stat above, or the cached one while nothing changed): every quiet-time
        // and freshness computation below reads THIS, exactly as it used to read the per-pass stat.
        const int64_t lastWriteMs = st.statWriteUnixMs;

        if (size < st.offset)
        {
            // Truncated / rewritten under us (shouldn't happen for an append-only transcript, but
            // be safe): restart the tail from the top. The re-read is a fresh HISTORY REPLAY, so
            // the primed flag drops with it — its events must not feed the run-repair either.
            // A rewritten file is also a fresh FIRST SIGHT: re-arm the tail seek (a big rewrite
            // starts at its tail window like any other first sight) and mark the facts not-current
            // until that read lands.
            st.offset = 0;
            st.lastSize = -1;
            st.primed = false;
            st.readReachedEnd = false;
            st.seekDecided = false;
            st.alignSeek = false;
        }
        // Captured BEFORE the read: events consumed by a not-yet-primed cursor are the initial
        // backlog replay (the tail window -> end on a restored/adopted session, or offset 0 -> end on
        // a transcript that fits one window / the alignment fallback), and the pass that
        // FINISHES the replay sets primed for the NEXT pass — so replayed history can never count
        // as the live append the run-repair keys on (no matter how fresh the file mtime is).
        const bool wasPrimed = st.primed;
        bool consumedTurnEvent = false;
        st.answeredInteractive = false; // per-pass scratch (see ScanState) — only THIS pass's reads count
        // Agentmaster (the 2026-09-25 CATCH-UP STALL, session d1f3f198): read on a size change OR while
        // the previous read left complete bytes unread (a window-capped chunk, a failed open, a short
        // grown re-read). The old `size != lastSize` gate alone froze the cursor mid-history the moment
        // the file went quiet: the stuck session's 3.8 MB transcript got one 1 MiB chunk per append,
        // its last append arrived as the aborted turn ended, and the tail facts stayed parked on a
        // two-day-old `tool_use`, which no pull release will ever end (see ShouldReadTranscriptDelta).
        if (ShouldReadTranscriptDelta(size, st.lastSize, st.readReachedEnd))
        {
            consumedTurnEvent = _readDelta(st, s, size); // advances st.offset past the complete lines it consumed
            st.lastSize = size;
        }
        // The TAIL-FACTS-CURRENT gate: the fold (lastStopReason / interrupted / pendingInteractiveTool /
        // lastWasApiError / the active leaf) describes the file's END only once a read has reached the
        // stat'd end. While a backlog of complete lines is still unread (a multi-pass catch-up, the
        // alignment fallback's replay from 0, a retried I/O failure) it describes some line mid-file.
        // So every synth that JUDGES the tail (recon-error / error-release / recon-stop /
        // recon-stop-idle / recon-block / the lost-send verdict) holds until the catch-up lands. The
        // alternative is a false Error off a historical API error, a false turn-end off a historical
        // end_turn, or a false lost-send off a not-yet-read echo. Holding costs at most the few 300 ms
        // passes a catch-up takes; acting on stale facts costs a wrong state that may never be undone.
        // The consumption-driven repairs (recon-run / recon-resume) key on freshly appended events
        // behind the primed-cursor gate, and the heartbeat-driven ones (presence block/resume,
        // recon-subagent) key on current facts. They need no hold.
        const bool tailCurrent = st.readReachedEnd;

        // Agentmaster (DEACTIVATE == the window-restored state — ScanSessionIsDormant): a DORMANT session (a
        // tab of ours whose connection has not started — a window-restored tab nobody entered, or one parked
        // by "Deactivate ▸") HOLDS every promotion to Running below (recon-run / recon-resume /
        // recon-resume-presence / recon-subagent). No process of ours can be running a turn in it; what
        // those synths would read is residue of the claude a Deactivate just closed — its last transcript
        // lines, consumed by this cursor that was primed while it ran, or its subagents' final side-file
        // flushes, which keep `subagentActive` lit for kScanSubagentFreshMs. Without the hold a tab parked
        // mid-turn re-lit Running on the very next pass and stayed Running until it was entered, where the
        // resumed claude's SessionStart then made a false "completed" edge. The cursor still READS (so it is
        // caught up the moment the tab is entered) and the at-rest synths still run, exactly as for a
        // window-restored tab. Computed once from the pass snapshot; a wake that lands mid-pass is picked up
        // by the next pass.
        const bool dormant = ScanSessionIsDormant(s);
        if (!dormant)
        {
            st.dormantHoldLogged = false; // the tab started — a later park is a new episode, worth its own log line
        }
        // Log a held promotion ONCE per dormancy episode (ScanState::dormantHoldLogged), so the evidence that
        // the hold fired is in scanner.log without a line per pass while the residue lasts.
        //   synth — the synth's log tag (e.g. L"recon-run"), named in the line.
        // Never throws past AppendStateLog's own best-effort contract.
        const auto holdDormant = [&](const wchar_t* synth) {
            if (!st.dormantHoldLogged)
            {
                st.dormantHoldLogged = true;
                AppendStateLog(L"scanner.log",
                               L"[recon-dormant] " + s.id + L" held " + synth +
                                   L" -> Running (the tab is dormant: deactivated, or restored and not yet entered \x2014 no process of ours can be running a turn)\n");
            }
        };

        // Agentmaster (Error triage dismissal — expiry): the manual "Move to Idle/Done" on an Error
        // card acknowledged THE error that was standing when the user clicked; the ack lasts only
        // until the conversation MOVES. A freshly consumed TURN event is that move — a retry (clears
        // lastWasApiError -> recon-run below re-lights Running), or a NEW error line (itself a turn
        // event) that must re-fire recon-error — so retire the flag HERE, at consumption time, not in
        // the synth gate: the new error's own pass fails the 2s quiescence and only a LATER quiet pass
        // fires it, by which point a per-pass "saw a turn event" bypass would have expired. Quiet — the
        // flag is display-independent bookkeeping; the visible state comes from the synths below.
        // (Bookkeeping-only appends — turn_duration / away_summary lineage — are NOT turn events, so a
        // post-dismissal idle recap can never resurrect the acknowledged error.)
        if (consumedTurnEvent && s.errorDismissed)
        {
            _registry->UpdateQuiet(s.id, [](SessionInfo& ss) { ss.errorDismissed = false; });
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
        // EMPTY promptText (no Auto-Testing side effects — NoteExternalPrompt in _readDelta owns the
        // prompt back-fill, and the echo bookkeeping stays push-owned, so a late real
        // UserPromptSubmit lands on Running -> Running, a no-op). The re-Get mirrors the
        // missed-Stop's freshest-state re-check: a real hook that landed mid-pass wins.
        // Agentmaster: held for a DORMANT session (ScanSessionIsDormant — the deactivate residue).
        const bool runRepair = ShouldSynthesizeRunning(s.state, consumedTurnEvent, wasPrimed, st.lastStopReason, NowMs() - lastWriteMs);
        if (runRepair && dormant)
        {
            holdDormant(L"recon-run");
        }
        else if (runRepair)
        {
            const auto fresh = _registry->Get(s.id);
            // Agentmaster (the stuck-Error report — session 2b34b07f, 2026-08-07): Error is one of the
            // pure gate's recoverable states (the documented "come out of Error on the first change" PULL
            // edge, pinned by the run-repair unit tests) — but this freshest-state re-check listed only
            // Idle/WaitingForInput, so the recovery could never actually FIRE. A session whose Error had
            // been re-asserted over the push's fresh Running (the recon-error stale-snapshot race, now
            // guarded in ShouldSynthesizeError below) therefore stayed a red card for the WHOLE retry turn
            // (35 min live: Error re-asserted 14:59:51.190, released only by the real Stop at 15:35:14)
            // while every pass's consumed transcript append said "recover". Keep this re-check in lockstep
            // with ShouldSynthesizeRunning's state set.
            if (fresh && (fresh->state == SessionState::Idle || fresh->state == SessionState::WaitingForInput || fresh->state == SessionState::Error))
            {
                const bool fromError = fresh->state == SessionState::Error;
                HookMessage run;
                run.event = HookEvent::UserPromptSubmit;
                run.sessionId = s.id;
                run.cwd = s.workingDir;
                run.ts = NowMs();
                _registry->OnHookEvent(run);
                AppendStateLog(L"scanner.log", L"[recon-run] " + s.id + (fromError ? L" (user retried after an API error — Error -> Running)" : L" (turn in progress, prompt hook missed/folded)") + L"\n");
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
        // Agentmaster: held for a DORMANT session (ScanSessionIsDormant — the deactivate residue).
        const bool resumeRepair = ShouldSynthesizeResumed(s.state, consumedTurnEvent, wasPrimed, st.pendingInteractiveTool, st.lastStopReason, st.interrupted, NowMs() - lastWriteMs, st.answeredInteractive);
        if (resumeRepair && dormant)
        {
            holdDormant(L"recon-resume");
        }
        else if (resumeRepair)
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

        // Agentmaster (presence-driven blocked-on-user EXIT — the third belt). The transcript-based
        // recon-resume above cannot fire off the ANSWER itself (a ToolResult is not a turn event), so
        // it waits for the agent's NEXT assistant append — measured 19.8s (ed5a48a4) / 45.3s
        // (d2e77e90) of a card reading orange "needs you" while the agent was demonstrably working,
        // and that is the ONLY exit for the answer-and-keep-working shape (the other three exits ride
        // a real Stop hook). claude's own heartbeat leaving "waiting" for "busy"/"shell" IS the
        // answer, is written at the transition instant, and needs neither the transcript nor a hook.
        // Runs BEFORE the latch is refreshed below so it reads the PREVIOUS pass's "waiting".
        // Same synthesis as recon-resume (PostToolUse -> Running through the ONE state machine, never
        // a UserPromptSubmit which would inflate ++queuedPrompts), same freshest-state re-Get, so
        // whichever belt is first wins and the loser is a no-op.
        // Agentmaster: held for a DORMANT session (ScanSessionIsDormant — a heartbeat it shows is the dead
        // claude's last word, never a resumed turn of ours).
        const bool presenceResume = ShouldSynthesizeResumedFromPresence(s.state, s.presenceStatus, st.presenceWasWaiting, st.lastStopReason, st.interrupted);
        if (presenceResume && dormant)
        {
            holdDormant(L"recon-resume-presence");
        }
        else if (presenceResume)
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
                AppendStateLog(L"scanner.log",
                               L"[recon-resume-presence] " + s.id + L" (heartbeat waiting -> " + s.presenceStatus +
                                   L", NeedsApproval -> Running)\n");
            }
        }

        // Agentmaster (presence-driven blocked-on-user ENTRY — the third belt). recon-block below
        // needs the AskUserQuestion tool_use LINE, measured still unwritten 10+ minutes into a live
        // question (7b40d6cf); the Notification hook covers that today but is droppable, and with
        // BOTH missed the session shows Running forever with a question on screen. claude's own
        // "waiting" closes that hole and lands ~6.4s earlier than the hook besides. Synthesizes the
        // SAME permission-style Notification recon-block does, so the two are interchangeable and a
        // duplicate is a no-op through the one state machine.
        const bool presenceAwaitingUser = PresenceIsAwaitingUser(s.presenceStatus);
        if (ShouldSynthesizeBlockedFromPresence(s.state, s.presenceStatus, st.interrupted))
        {
            const auto fresh = _registry->Get(s.id);
            // Re-run the SAME predicate against the freshest state (a real hook may have landed
            // mid-pass), rather than a hand-written subset that could drift from it.
            if (fresh && ShouldSynthesizeBlockedFromPresence(fresh->state, s.presenceStatus, st.interrupted))
            {
                HookMessage block;
                block.event = HookEvent::Notification;
                block.permissionRequest = true; // -> NeedsApproval via the one state machine
                block.sessionId = s.id;
                block.cwd = s.workingDir;
                block.ts = NowMs();
                _registry->OnHookEvent(block);
                AppendStateLog(L"scanner.log",
                               L"[recon-block-presence] " + s.id + L" (heartbeat waiting" +
                                   (s.presenceWaitingFor.empty() ? std::wstring{} : (L": " + s.presenceWaitingFor)) +
                                   L" -> needs you)\n");
            }
        }
        // Refresh the EDGE latch for the next pass: armed while claude reports "waiting", dropped the
        // moment it leaves (the resume synth above already consumed the transition; a move to "idle"
        // needs no release here — that is recon-stop's -> Waiting).
        st.presenceWasWaiting = presenceAwaitingUser;

        // Agentmaster (subagent/fork activity): the parent <id>.jsonl can sit QUIESCENT while the
        // turn's real work happens elsewhere — a Task/Agent SUBAGENT writing <id>/subagents/*.jsonl
        // (the parent does not grow until the subagent returns), or, after a live /fork|/clear|
        // /resume, a NEW conversation id the tab has not yet re-bound to (a /compact is in-place — same
        // id, same file — and its silent window is held by CompactionInFlight below). Two out-of-band
        // signals recover "still working" without screen-scraping: the newest subagent/tool-result
        // side-file write (SubagentActivityUnixMs), and claude's OWN presence heartbeat ("busy",
        // pid-validated by the S-lane — Rule #13: a FACT the scanner may consume as a state input).
        // Fold both into the quiescence clock so the missed-Stop / blocked-on-user synths below do
        // NOT demote a working session: a fresh subagent write counts as a recent transcript write,
        // and a "busy" heartbeat forces quietForMs to 0 (claude says it is working — never synthesize
        // a turn-end). With neither signal present this is exactly the prior parent-only value, so a
        // genuinely idle session reconciles unchanged.
        const int64_t parentWriteMs = lastWriteMs;
        const int64_t parentQuietMs = NowMs() - parentWriteMs;
        // HOOKS.md "Transcript watcher": the side-folder walk (two directory enumerations) runs only when the
        // watcher saw a change under this session's subagents/ or tool-results/, or the belt comes due
        // (ShouldRefreshSideActivity). Before, every live session paid it on every pass.
        if (ShouldRefreshSideActivity(watch.healthy, st.sideValid, watch.epoch != st.seenSideEpoch, watch.sideSeq != st.seenSideSeq, hot, passNow - st.lastSideScanMs))
        {
            st.seenSideEpoch = watch.epoch;
            st.seenSideSeq = watch.sideSeq;
            st.subagentActivityMs = SubagentActivityUnixMs(st.path);
            st.sideValid = true;
            st.lastSideScanMs = passNow;
        }
        const int64_t subagentActivityMs = st.subagentActivityMs;
        const bool presenceWorking = PresenceIsWorking(s.presenceStatus); // "busy" OR a live "shell" job (claude's own heartbeat)
        const bool subagentActive = subagentActivityMs > 0 && (NowMs() - subagentActivityMs) <= kScanSubagentFreshMs;
        // Agentmaster (work that OUTLIVES the turn — teammates / background agents / shells): external
        // work postdating the parent transcript's last write by MORE than kScanExternalWorkGraceMs has
        // provably outlived the turn's end — an in-process TEAMMATE or run_in_background Agent keeps
        // writing subagents/*.jsonl minutes-hours after the lead's end_turn, and a live shell job keeps
        // claude's heartbeat on "shell"/"busy" — as opposed to turn-tail RESIDUE (a final side-file
        // flush lands us before end_turn; "busy" lingers a ~2s tick; post-Esc dying subagents flush up
        // to kScanSubagentFreshMs — all under the margin). ONE expression, computed ONCE, drives BOTH
        // the terminal-tail promotion (ShouldSynthesizeRunningFromExternalWork) AND the recon-stop hold
        // (ShouldSynthesizeStop's externalWorkOngoing) so the two gates can never disagree/oscillate:
        // "a shell or agent or teammate still running means Running, not idle/done/waiting-for-you".
        const bool externalOutlivesTurn =
            (subagentActive && subagentActivityMs > parentWriteMs + kScanExternalWorkGraceMs) ||
            (presenceWorking && parentQuietMs > kScanExternalWorkGraceMs);
        int64_t quietForMs = parentQuietMs;
        if (subagentActivityMs > 0)
        {
            const int64_t subQuietMs = NowMs() - subagentActivityMs;
            if (subQuietMs < quietForMs)
            {
                quietForMs = subQuietMs; // the conversation IS being written (just not the main file)
            }
        }
        if (presenceWorking)
        {
            quietForMs = 0; // claude self-reports working (a turn OR a live shell job) — the transcript may be momentarily quiet, the work is not over
        }
        // Agentmaster (the COMPACTION WINDOW — HOOKS.md "Compaction"): a PreCompact opened a window the
        // closing hook has not ended yet — claude is summarizing the conversation, which writes NOTHING to
        // the transcript for 1-3+ minutes while the tail still shows the previous turn's TERMINAL
        // stop_reason. That is exactly the missed-Stop shape, so without this every quiescence-gated synth
        // (recon-stop, the presence-idle release, recon-block, recon-error, the error release) would
        // demote the compacting session within seconds — before the S-lane's ~2 s survey even publishes
        // claude's "busy". Forcing the clock to 0 stands them ALL down together, the same way "busy" does;
        // CompactionInFlight ends the hold on the closing hook, claude's own settled "idle" (a cancelled
        // or failed compaction), or the hard cap (a lost closing hook).
        if (CompactionInFlight(s.compactingSinceUnixMs, NowMs(), s.presenceStatus))
        {
            quietForMs = 0;
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
        // Agentmaster: held for a DORMANT session (ScanSessionIsDormant) — the side files a parked tab's
        // closed claude left behind stay "fresh" for kScanSubagentFreshMs and would re-light it otherwise.
        const bool externalWorkRun = ShouldSynthesizeRunningFromExternalWork(s.state, subagentActive, presenceWorking, st.lastStopReason, st.interrupted, externalOutlivesTurn);
        if (externalWorkRun && dormant)
        {
            holdDormant(L"recon-subagent");
        }
        else if (externalWorkRun)
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && (fresh->state == SessionState::Idle || fresh->state == SessionState::WaitingForInput))
            {
                HookMessage act;
                act.event = HookEvent::PostToolUse;
                act.sessionId = s.id;
                act.cwd = s.workingDir;
                act.ts = NowMs();
                // EXTERNAL work (a subagent/teammate side file, the busy/shell heartbeat) — drives
                // Running through the one state machine but must NOT stamp the ⚡ turn evidence: a
                // teammate's/subagent's API call runs in its own context, never this conversation's
                // prefix cache (IsApiTurnEvidence excludes the flagged PostToolUse).
                act.externalWorkActivity = true;
                _registry->OnHookEvent(act);
                AppendStateLog(L"scanner.log",
                               L"[recon-subagent] " + s.id + L" (" + (presenceWorking ? L"presence=" + s.presenceStatus : L"subagent side-files active") +
                                   (externalOutlivesTurn ? L", outlived turn-end" : L"") +
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
        const bool errorIsActiveLeaf = ApiErrorIsActiveLeaf(st.lastWasApiError, st.activeLeafUuid, st.errorEpochLeaf, st.errorBranchUuids);
        // The newest transcript byte THIS pass's parse can possibly reflect (the attribute read the
        // delta was taken against). A REAL UserPromptSubmit stamped AFTER it proves the parsed tail is
        // superseded — the user already retried and the new turn just hasn't hit the file yet — so the
        // error must not (re-)enter. The live race (session 2b34b07f, 2026-08-07): the push hook flipped
        // Error -> Running at 14:59:51.152 and this arm — state read fresh (no longer Error, so the
        // idempotence arm passed) but transcript evidence still ending at the 14:58 error line — re-fired
        // recon-error 38ms later, re-asserting a red card over a genuinely running retry turn.
        const int64_t tailWriteMs = lastWriteMs;
        // tailCurrent: an error line consumed MID-catch-up is history, not the tail — never Error off it.
        // NowMs() bounds the supersession hold (kScanPromptLandingGraceMs): a retry that never reached the
        // transcript re-asserts Error once the grace passes, instead of being held forever.
        if (tailCurrent && ShouldSynthesizeError(s.state, errorIsActiveLeaf, quietForMs, s.turns.lastPromptUnixMs, tailWriteMs, NowMs()))
        {
            const auto fresh = _registry->Get(s.id);
            // Manual dismissal gate (the triage "Move to Idle/Done" on an Error card): the user
            // acknowledged THIS error, so don't re-derive Error off the UNCHANGED tail — without this,
            // Error being level-derived would bounce the flip to Idle back within one pass. The flag
            // was retired at consumption time above the moment any turn event moved the conversation
            // (and a fresh Error entry clears it in OnHookEvent), so a genuinely NEW error re-fires
            // here normally. Still early-return: the (acknowledged) errored tail owns this pass.
            // NOTE the dismissal ack lands in **Idle**, not Done — so errorDismissed (not a Done arm) is
            // what protects it, which is why Done is no longer excluded here or in ShouldSynthesizeError:
            // a live session in Done with an unrecovered API-error tail is an ABORTED session whose claude
            // is still up, and it must read Error (see the DONE FIRES TOO note on the predicate).
            // The lastPromptUnixMs re-check is the supersession guard's FRESH belt: the pure gate above
            // tested the pass-start snapshot's stamp, and a real prompt can land in the window between
            // that snapshot and this re-Get — the exact interleave that re-asserted 2b34b07f's Error.
            // Same shared, bounded predicate as the gate (NewerPromptNotYetInTranscript), so the belt
            // and the gate can never disagree about the grace.
            if (fresh && !fresh->errorDismissed && fresh->state != SessionState::Error &&
                !NewerPromptNotYetInTranscript(fresh->turns.lastPromptUnixMs, tailWriteMs, NowMs()))
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

        // Error RELEASE on a leaf move (double-ESC REWIND past the error): the error is still the
        // physical tail (lastWasApiError true), but a later last-prompt repointed the active leaf OFF it
        // (activeLeafUuid != errorEpochLeaf) — the user rewound and is back at an earlier prompt, so the
        // error is no longer "the last message". No turn event fired (recon-run / push own the user-RETRIED
        // case, which clears lastWasApiError -> Running), so release HERE to WaitingForInput. Quiescence-
        // gated like recon-stop; early-returns so recon-stop/idle/block don't also fire this pass.
        if (tailCurrent && ShouldReleaseErrorOnLeafMove(s.state, st.lastWasApiError, st.activeLeafUuid, st.errorEpochLeaf, st.errorBranchUuids, quietForMs))
        {
            const auto fresh = _registry->Get(s.id);
            if (fresh && fresh->state == SessionState::Error)
            {
                HookMessage stop;
                stop.event = HookEvent::Stop;
                stop.sessionId = s.id;
                stop.cwd = s.workingDir;
                stop.ts = NowMs();
                stop.quiescentStop = true; // settled: the ordered machine lands WaitingForInput unconditionally
                stop.lastMessageIsQuestion = false; // the leaf moved off the error; there is no question to surface
                _registry->OnHookEvent(stop);
                AppendStateLog(L"scanner.log", L"[recon-error-release] " + s.id + L" (rewound past error -> Waiting)\n");
            }
            return; // handled this pass — don't let recon-stop / recon-idle also fire
        }

        // Missed/forced-Stop reconciliation: the turn is OVER — either the transcript's last
        // assistant message ended it (a TERMINAL stop_reason — end_turn / stop_sequence /
        // max_tokens / refusal) or the user INTERRUPTED it (Esc -> no clean Stop hook) — the file
        // has gone quiescent, yet we are STILL Running (or blocked in NeedsApproval whose
        // post-approval / post-answer Stop was dropped). Synthesize a Stop identical to the real
        // one (-> WaitingForInput + the question-guard + the Autorunner advance). The state gate
        // (re-checked against the freshest state right before firing) makes a real Stop that
        // already landed win, so this never double-fires.
        // externalOutlivesTurn HOLDS the Running->Waiting demotion while a teammate / background agent /
        // live shell provably outlives the finished turn — the exact expression the promotion above
        // fires on, so the two stay mutually exclusive (never oscillate); see ShouldSynthesizeStop.
        // tailCurrent (both arms): a mid-catch-up fold can read a historical end_turn as "the turn is
        // over", or a historical tool_use as "still mid-tool"; only the caught-up tail may end a turn.
        // Agentmaster (the TURN-START RACE — NewerPromptNotYetInTranscript, HOOKS.md "State machine"):
        // both arms also HOLD while the user has submitted a prompt the transcript has not recorded yet.
        // The prompt's UserPromptSubmit Wake()s this pass ~6 ms after setting Running, before claude writes
        // the prompt line (it does so only after its hooks finish), so the tail here is still the PREVIOUS
        // turn's end. Firing on it flipped Running -> Waiting -> Running in ~220 ms on about 29% of prompts.
        // The hold ends when ANY byte lands after the prompt (normally its own line, ~0.2-0.5 s), or
        // kScanPromptLandingGraceMs after it for a prompt claude never records.
        const int64_t promptCheckNowMs = NowMs();
        const bool stopFromTail = tailCurrent && ShouldSynthesizeStop(s.state, st.lastStopReason, st.interrupted, quietForMs, externalOutlivesTurn,
                                                                      s.turns.lastPromptUnixMs, tailWriteMs, promptCheckNowMs);
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
        const bool stopFromPresenceIdle = tailCurrent && !stopFromTail && st.pendingInteractiveTool.empty() &&
                                          ShouldSynthesizeStopFromPresenceIdle(s.state, s.presenceStatus, st.lastStopReason, st.interrupted, quietForMs,
                                                                               s.turns.lastPromptUnixMs, tailWriteMs, promptCheckNowMs);
        // Evidence that the guard did its job: re-ask both predicates WITHOUT the prompt stamps (their
        // classic, guard-inert defaults). A synth that would have fired is exactly one false Waiting
        // prevented. Logged ONCE per prompt (promptHoldLoggedFor), because the hold repeats every pass
        // until the prompt line lands.
        if (tailCurrent && st.promptHoldLoggedFor != s.turns.lastPromptUnixMs &&
            NewerPromptNotYetInTranscript(s.turns.lastPromptUnixMs, tailWriteMs, promptCheckNowMs) &&
            (ShouldSynthesizeStop(s.state, st.lastStopReason, st.interrupted, quietForMs, externalOutlivesTurn) ||
             (st.pendingInteractiveTool.empty() &&
              ShouldSynthesizeStopFromPresenceIdle(s.state, s.presenceStatus, st.lastStopReason, st.interrupted, quietForMs))))
        {
            st.promptHoldLoggedFor = s.turns.lastPromptUnixMs;
            AppendStateLog(L"scanner.log",
                           L"[recon-stop-hold] " + s.id + L" (a prompt submitted " + std::to_wstring(promptCheckNowMs - s.turns.lastPromptUnixMs) +
                               L" ms ago is not in the transcript yet; its last write was " + std::to_wstring(promptCheckNowMs - tailWriteMs) +
                               L" ms ago - the tail is the previous turn's, held)\n");
        }
        if (stopFromTail || stopFromPresenceIdle)
        {
            const auto fresh = _registry->Get(s.id);
            // The prompt-stamp re-check is the guard's FRESH belt, the recon-error twin. The predicates
            // above tested the pass-start snapshot, and a prompt can arrive between that snapshot and this
            // re-Get. With the old turn's missed Stop still pending, the session is Running both before and
            // after that prompt, so the state check alone cannot tell them apart. Same shared, bounded
            // predicate, so the belt never outlives the grace.
            const int64_t freshNowMs = NowMs();
            const bool freshPromptNotLanded = fresh && NewerPromptNotYetInTranscript(fresh->turns.lastPromptUnixMs, tailWriteMs, freshNowMs);
            if (freshPromptNotLanded && st.promptHoldLoggedFor != fresh->turns.lastPromptUnixMs)
            {
                st.promptHoldLoggedFor = fresh->turns.lastPromptUnixMs;
                AppendStateLog(L"scanner.log",
                               L"[recon-stop-hold] " + s.id + L" (a prompt arrived during this pass, " +
                                   std::to_wstring(freshNowMs - fresh->turns.lastPromptUnixMs) + L" ms ago - the tail is the previous turn's, held)\n");
            }
            if (fresh && (fresh->state == SessionState::Running || fresh->state == SessionState::NeedsApproval) && !freshPromptNotLanded)
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
                // COMMANDS.md: a presence-idle release ends a turn the TRANSCRIPT never showed
                // ending (no terminal tail / no interrupt line -> the parser fed no OnTurnEnd for
                // it), so feed the boundary here: a matched command await SEALS on the scanner's
                // own settled judgment instead of riding to its deadline now that the settle-seal
                // holds while the tail still reads mid-turn. The TAIL-derived stop must NOT feed —
                // its terminal/interrupt line already fed OnTurnEnd at parse, and a second feed
                // would double-age unmatched pendings (halving their clarification window).
                if (stopFromPresenceIdle && _commandWatch && st.primed)
                {
                    _commandWatch->OnTurnEnd(s.id);
                }
                // A prompt still newer than every transcript byte, yet the synth fired: the guard let go
                // only because kScanPromptLandingGraceMs passed. Claude never recorded that prompt (an Esc
                // during its hook phase, or a re-submit). Say so, so the log never reads like the race again.
                const bool promptNeverLanded = tailWriteMs > 0 && fresh->turns.lastPromptUnixMs > tailWriteMs;
                AppendStateLog(L"scanner.log",
                               (stopFromPresenceIdle ? L"[recon-stop-idle] " : L"[recon-stop] ") + s.id +
                                   L" q=" + (stop.lastMessageIsQuestion ? L"1" : L"0") +
                                   (st.interrupted ? L" interrupted" : L"") +
                                   (stopFromPresenceIdle ? (L" presence=" + s.presenceStatus) : std::wstring{}) +
                                   (promptNeverLanded ? (L" (a prompt submitted " + std::to_wstring(freshNowMs - fresh->turns.lastPromptUnixMs) +
                                                         L" ms ago never reached the transcript)")
                                                      : std::wstring{}) +
                                   L"\n");
            }
        }

        // Blocked-on-user reconciliation: the latest assistant message is an UNANSWERED interactive
        // tool_use (AskUserQuestion) and the transcript has gone quiescent — the session is blocked
        // waiting for the user to answer, NOT working, so it must not show Running. Synthesize a
        // permission-style Notification (-> NeedsApproval, the "needs you" column). Idempotent: only
        // from Running, so once it lands NeedsApproval it stays until the answer + the turn's end
        // release it via the missed-Stop reconciliation above. (Autorunner treats NeedsApproval as
        // NOT ready — Rule #1 — so it won't auto-answer the question with a queued prompt.)
        if (tailCurrent && ShouldSynthesizeBlockedOnUser(s.state, st.pendingInteractiveTool, st.interrupted, quietForMs))
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

        // Agentmaster (DELIVERY_PLAN.md R1 — the LOST-SEND verdict). A delivered prompt that never
        // became a message used to be terminally invisible: its queue row read `Sent` exactly like a
        // success (the recorded `#6` — pasted into a running turn, consumed by an AskUserQuestion
        // dialog's rendering, discovered only by a human diffing the transcript). This pass owns
        // transcript truth and has JUST caught the cursor up (any user line on disk fed the pull
        // echo-consume above via NoteExternalPrompt), so an at-rest, settled, still-unechoed Sent
        // prompt is provably not a message: mark it Failed and PAUSE the session's autorunner
        // (mirrors the Enter-retry give-up — never advance past a vanished step, and NEVER resend:
        // the text may sit in the TUI's type-ahead or box, where a resend doubles it). Decided on
        // the FRESHEST record (the synths above may have just settled the state this very pass) and
        // re-verified inside the Update against the live row — an echo or a watchdog press landing
        // between the decide and the apply wins (the press refreshes sentAtUnixMs, un-settling it).
        {
            // Steady-state pre-filter on the PASS SNAPSHOT: only a session showing a Sent-unechoed
            // Autorun prompt pays the fresh Get (the overwhelming majority of passes see none). The
            // snapshot may be a pass stale in EITHER direction (a consume just marked it echoed, or
            // a send just landed) — the fresh decide + the in-Update re-verify are the truth.
            bool lostCandidate = false;
            for (const auto& p : s.queue)
            {
                if (p.origin == PromptOrigin::Autorun && p.status == PromptStatus::Sent && !p.echoed && p.sentAtUnixMs != 0)
                {
                    lostCandidate = true;
                    break;
                }
            }
            const auto fresh = lostCandidate ? _registry->Get(s.id) : std::nullopt;
            if (fresh)
            {
                const int64_t verdictNow = NowMs();
                // tailCurrent: the verdict's whole premise is "this pass just caught the cursor up, so
                // any user line on disk already fed the pull echo-consume". While a backlog is unread,
                // the delivered prompt's own echo line may still sit past the cursor, and a verdict now
                // would Fail a prompt that DID become a message (and pause the autorunner for it). Hold
                // until the catch-up lands. The undelivered reclaim below never reads the transcript,
                // so it stays ungated.
                const auto lostIds = tailCurrent ? DecideLostSend(*fresh, verdictNow) : std::vector<std::wstring>{};
                for (const auto& lostId : lostIds)
                {
                    bool applied = false;
                    std::wstring label;
                    _registry->Update(s.id, [&](SessionInfo& live) {
                        for (auto& p : live.queue)
                        {
                            if (p.id == lostId && p.status == PromptStatus::Sent && !p.echoed &&
                                p.sentAtUnixMs != 0 && (verdictNow - p.sentAtUnixMs) >= kLostSendSettleMs &&
                                p.injectedAtUnixMs != 0 && (verdictNow - p.injectedAtUnixMs) >= kLostSendSettleMs)
                            {
                                p.status = PromptStatus::Failed;
                                label = p.label;
                                live.autorunner.mode = AutorunnerMode::Off;
                                ClearInterruptHold(live.autorunner); // DELIVERY.md §14: a backstop pause is sticky — no resume on the next message
                                applied = true;
                            }
                        }
                    });
                    if (applied)
                    {
                        const std::wstring line = L"[lost-send] " + ShortId(s.id) + L" prompt " + ShortId(lostId) +
                                                  L" \"" + label + L"\" (delivered but never became a message - marked Failed, autorunner paused; not resent)\n";
                        AppendStateLog(L"autorunner.log", line);
                        AppendStateLog(L"hooks.log", line);
                    }
                }

                // Agentmaster (DELIVERY.md §12 — the UNDELIVERED-SEND reclaim, the lost verdict's
                // retryable sibling): a Sent prompt whose delivery was ACCEPTED but provably never
                // INJECTED (injectedAtUnixMs == 0) and whose gate claim has lapsed. Nothing was ever
                // typed, so a re-send cannot double anything — roll it back to Pending and let the
                // advance retry (the Update's notify IS the retry trigger). This replaces the old
                // behavior where the lost verdict — blind to injection — failed such a prompt and
                // paused the autorunner ONE SECOND after the gate expired, while its delivery was
                // still queued behind a starved dispatcher (the 20:40:45→20:42:08 incident: the
                // "lost" prompt then delivered verified=exact and became a duplicate Typed row).
                // The stale delivery itself aborts at its top guard (the §12 nonce gate tags), so
                // the reclaim and the zombie can never both deliver.
                const auto undeliveredIds = DecideUndeliveredReclaim(*fresh, verdictNow);
                for (const auto& undeliveredId : undeliveredIds)
                {
                    bool reclaimed = false;
                    std::wstring label;
                    _registry->Update(s.id, [&](SessionInfo& live) {
                        if (DeliveryGateOpen(live, NowMs()))
                        {
                            return; // a delivery re-validated its claim between the decide and this apply — it is alive
                        }
                        for (auto& p : live.queue)
                        {
                            if (p.id == undeliveredId && p.status == PromptStatus::Sent && !p.echoed &&
                                p.sentAtUnixMs != 0 && p.injectedAtUnixMs == 0 &&
                                (verdictNow - p.sentAtUnixMs) >= kLostSendSettleMs)
                            {
                                p.status = PromptStatus::Pending;
                                p.echoed = false;
                                p.enterRetries = 0;
                                if (p.attempts > 0)
                                {
                                    p.attempts -= 1;
                                }
                                label = p.label;
                                reclaimed = true;
                            }
                        }
                    });
                    if (reclaimed)
                    {
                        const std::wstring line = L"[send-reclaim] " + ShortId(s.id) + L" prompt " + ShortId(undeliveredId) +
                                                  L" \"" + label + L"\" (accepted for delivery but never injected and the gate lapsed - rolled back to Pending for a retry; autorunner keeps its mode)\n";
                        AppendStateLog(L"autorunner.log", line);
                        AppendStateLog(L"hooks.log", line);
                    }
                }
            }

            // Agentmaster (DELIVERY_PLAN.md R5, defanged by DELIVERY.md §12 — the box-not-visible
            // WARNING): DecideAdvance holds queued work while the box reads NoBox, and that hold is
            // deliberately quiet (an [advance-skip] line). A NoBox that PERSISTS (a detector/render
            // drift, a modal parked over the box) must not park a plan invisibly forever — WARN
            // loudly, once per NoBox episode. The R5 cut paused the autorunner (mode → Off) here,
            // and the live incident showed why that is wrong: the NoBox clock ran from BEFORE the
            // user's explicit Off→Full re-arm, so the escalation overrode a human's GO 48s after
            // they gave it — for a verdict that is not even always a fault (a parked non-numbered
            // menu reads NoBox, the same legitimately-parks-for-hours class MenuOpen was always
            // exempt for). The hold already stops any send and self-releases the moment a box
            // renders (SetPendingBoxState's blocked→unblocked notify), so the mode is never robbed.
            if (ShouldWarnOnBoxNotVisible(s, NowMs()) && st.boxNotVisibleWarnedStamp != s.pendingBoxStateUnixMs)
            {
                st.boxNotVisibleWarnedStamp = s.pendingBoxStateUnixMs;
                const std::wstring line = L"[send-verify] " + ShortId(s.id) +
                                          L" queued prompts HELD: no parseable input box for >" +
                                          std::to_wstring(kBoxNotVisibleEscalateMs / 1000) +
                                          L"s (a menu/modal may be parked over it, or the render drifted) - the advance stays held and self-resumes when a box renders; the autorunner keeps its mode\n";
                AppendStateLog(L"autorunner.log", line);
                AppendStateLog(L"hooks.log", line);
            }
        }
    }

    bool SessionScanner::_readDelta(ScanState& st, const SessionInfo& s, int64_t size)
    {
        // Agentmaster (FIRST-SIGHT TAIL SEEK — FirstSightReadOffset): a fresh cursor (a restored /
        // resumed / adopted / forked session, or a truncation rewind) on a transcript bigger than one
        // window starts at the tail window instead of replaying the whole history. The tail window
        // carries every latest-wins fact the reconcilers judge, so the facts are right on the very first
        // pass, at the same ≤ one-window cost the first pass always paid. Decided exactly ONCE per cursor
        // (seekDecided), so the alignment fallback to offset 0 below can never bounce back to the tail.
        if (!st.seekDecided)
        {
            st.seekDecided = true;
            if (st.offset == 0 && !st.primed)
            {
                if (const int64_t seek = FirstSightReadOffset(size); seek > 0)
                {
                    st.offset = seek;
                    st.alignSeek = true; // the seek point is an arbitrary byte — drop the leading fragment below
                }
            }
        }

        int64_t avail = size - st.offset; // non-const: the first-sight alignment below trims the leading fragment off it
        if (avail <= 0)
        {
            st.primed = true; // nothing past the cursor: it IS the file end — caught up
            st.readReachedEnd = true;
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
            // A transient open failure (an AV scanner / backup holding the file) must be RETRIED next
            // pass. Under the old size-only gate it was retried only if the file grew again, so a turn
            // that had just ended never got its end_turn read. readReachedEnd=false re-arms the read.
            st.readReachedEnd = false;
            return false;
        }
        LARGE_INTEGER li{};
        li.QuadPart = st.offset;
        if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        {
            ::CloseHandle(h);
            st.readReachedEnd = false; // retry next pass (see the open failure above)
            return false;
        }
        std::string bytes;
        bytes.resize(want);
        DWORD got = 0;
        const BOOL ok = ::ReadFile(h, bytes.data(), want, &got, nullptr);
        if (!ok || got == 0)
        {
            ::CloseHandle(h);
            st.readReachedEnd = false; // retry next pass (see the open failure above)
            return false;
        }
        bytes.resize(got);

        // Agentmaster (FIRST-SIGHT TAIL SEEK — alignment): the seek landed mid-line, so drop the leading
        // fragment and start at the window's first COMPLETE line. The fragment is garbage to the parser,
        // and a fragment of an embedded escaped object must never pass for a real line. A window with no
        // usable boundary lies entirely inside one line that began before the seek point (a >1 MiB final
        // line; rare, since a transcript normally ends in small trailer lines). There we fall back to the
        // classic replay from byte 0: correct, only slower. ShouldReadTranscriptDelta keeps it moving one
        // window per pass, and the tailCurrent gate keeps its mid-history facts from judging anything
        // until it lands.
        if (st.alignSeek)
        {
            const size_t start = FirstCompleteLineStart(std::string_view{ bytes.data(), bytes.size() });
            if (start == std::string_view::npos)
            {
                ::CloseHandle(h);
                st.offset = 0;
                st.alignSeek = false;
                st.readReachedEnd = false; // re-read from the top on the next pass
                return false;
            }
            // FirstCompleteLineStart guarantees start < bytes.size(), so something complete remains.
            bytes.erase(0, start);
            st.offset += static_cast<int64_t>(start);
            got -= static_cast<DWORD>(start);
            avail -= static_cast<int64_t>(start);
            st.alignSeek = false;
        }

        // Consume only up to the last newline; a trailing partial line is an append in flight and
        // is re-read whole next tick. (Splitting on the '\n' BYTE is UTF-8-safe: 0x0A is never a
        // continuation byte.)
        size_t nl = bytes.rfind('\n');
        if (nl == std::string::npos &&
            DecideDeltaNoNewline(static_cast<int64_t>(got), avail) == DeltaNoNewline::Grow)
        {
            // The FIRST unconsumed line alone outgrew the window (a huge single-line tool_result —
            // the 2026-08-14 stuck-Running wedge, session 6434e5f8: two PDF-read user lines of
            // 1.51 MiB and 2.0 MiB froze the cursor at one offset for 17+ minutes and every pull
            // release was structurally dead — see DecideDeltaNoNewline). RE-READ with the window
            // grown to the force-consume cap so a 1-4 MiB line parses like any other line.
            const DWORD wantBig = static_cast<DWORD>(avail < kScanForceConsumeBytes ? avail : kScanForceConsumeBytes);
            LARGE_INTEGER liBig{};
            liBig.QuadPart = st.offset;
            if (::SetFilePointerEx(h, liBig, nullptr, FILE_BEGIN))
            {
                std::string bigBytes;
                bigBytes.resize(wantBig);
                DWORD bigGot = 0;
                if (::ReadFile(h, bigBytes.data(), wantBig, &bigGot, nullptr) && bigGot >= got)
                {
                    bigBytes.resize(bigGot);
                    bytes.swap(bigBytes);
                    got = bigGot;
                    nl = bytes.rfind('\n');
                }
            }
        }
        ::CloseHandle(h);
        if (nl == std::string::npos)
        {
            switch (DecideDeltaNoNewline(static_cast<int64_t>(got), avail))
            {
            case DeltaNoNewline::SkipRead:
                // A run PROVEN newline-less past the force-consume cap: corrupt/binary. Advance the
                // cursor past the VERIFIED bytes only — never teleport to the file end, which would
                // silently drop every complete line written beyond the run (they parse next pass).
                st.offset += static_cast<int64_t>(got);
                // SkipRead implies got < avail, so complete lines past the run still wait. Keep reading
                // next pass even if the file never grows again (ShouldReadTranscriptDelta).
                st.readReachedEnd = false;
                break;
            case DeltaNoNewline::PartialInFlight:
                st.primed = true; // the whole remainder is one partial line in flight — caught up on complete lines
                st.readReachedEnd = true; // the in-flight line completes with its own append, which is a size change
                break;
            default:
                // Grow — the bigger re-read failed or came back short (an I/O hiccup): change
                // nothing; the next pass retries from the same cursor. readReachedEnd=false is what
                // actually MAKES it retry: the old size-only gate skipped every later pass on an
                // unchanged file, so the promised retry never came.
                st.readReachedEnd = false;
                break;
            }
            return false;
        }
        const size_t completeBytes = nl + 1;
        const std::wstring wide = Utf8ToUtf16(bytes.data(), static_cast<int>(completeBytes));
        st.offset += static_cast<int64_t>(completeBytes);
        // Did THIS read cover every byte up to the stat'd size? True even when a trailing partial line
        // stays unconsumed (it completes with its own append, which the size gate sees). False for a
        // window-capped chunk: complete lines remain past the cursor, and ShouldReadTranscriptDelta then
        // makes the next pass continue WITHOUT waiting for the file to grow. That wait is the 2026-09-25
        // stall: a finished turn's quiet file never grows again, so the catch-up froze mid-history.
        st.readReachedEnd = static_cast<int64_t>(got) == avail;
        if (st.readReachedEnd)
        {
            // This read reached the file's current end (everything available fit the window —
            // the 1 MiB default or the grown big-line one): the cursor is caught up — anything
            // consumed on a LATER pass is a live append, so the run-repair may key on it. (A
            // capped backlog chunk — avail > the read — is still mid-replay and does NOT prime;
            // the pass that finishes the replay primes for the NEXT one.)
            st.primed = true;
        }

        const auto parsed = ParseTranscriptDelta(wide);
        // Agentmaster: mirror the Claude Code idle RECAP (away_summary) onto the registry record so the
        // Triage-Board card tooltip can show it. QUIET (display-only, like lastAssistantText below) and
        // only when THIS chunk carried a recap — away_summary lines are rare, so this is a no-op on
        // virtually every tick (no lock churn). On a first-sight read (the tail window — FirstSightReadOffset
        // — or the whole file when it fits one window) the last recap in that read wins; a live recap lands
        // in its own delta. Never clears an existing recap (empty is ignored).
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
        // Agentmaster (COMMANDS.md): CommandWatch feeds are gated on the cursor being CAUGHT UP at
        // parse time (st.primed reflects THIS read — a caught-up small read, including a fresh
        // session's very first prompt, feeds; a capped mid-replay backlog chunk does not). The
        // watch's own line-timestamp freshness gate is the second belt — an adopted session's
        // one-shot history read primes during the same pass, but its old commands carry old stamps
        // and never arm. Feeds happen IN the event loop so transcript order (arm -> match -> turn
        // end) is preserved within a batch.
        const bool primedAtParse = st.primed;
        const bool feedWatch = primedAtParse && _commandWatch != nullptr;
        // Agentmaster (the INTERRUPT HOLD — DELIVERY.md §14): did THIS batch carry the user's turn-abort
        // marker? Captured in the loop, applied AFTER it — only if the marker is still the batch's
        // newest turn line (st.interrupted survives to the end: no later user prompt / assistant line
        // superseded it). A batch of [marker, the user's next prompt] must NOT park anything: that
        // prompt already fed NoteExternalPrompt inside the loop, so a hold taken afterwards would have
        // nobody left to resume it. Gated on primedAtParse like the CommandWatch feeds (a mid-replay
        // backlog chunk never parks); the registry's marker-freshness belt is the second guard.
        bool sawInterruptMarker = false;
        int64_t interruptMarkerTs = 0;
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
                    // Snapshot the ACTIVE LEAF as of the error (the last-prompt leaf seen up to this
                    // point). The error is "the last message" while the leaf stays here OR advances onto the
                    // error's OWN descendant chain (errorBranchUuids); a later last-prompt naming a leaf that
                    // is NEITHER is a double-ESC rewind PAST the error, which ApiErrorIsActiveLeaf /
                    // ShouldReleaseErrorOnLeafMove use to leave Error. "" when no marker has been seen yet
                    // (positional lastWasApiError then governs).
                    st.errorEpochLeaf = st.activeLeafUuid;
                    // Seed the forward descendant frontier with the error's OWN uuid. Claude appends
                    // bookkeeping children (a system/turn_duration line, then an away_summary) parented to
                    // the error, and the post-error last-prompt marker names that turn_duration CHILD; the
                    // Node branch below extends the frontier through those children so ApiErrorIsActiveLeaf
                    // reads the advanced leaf as still the error tail rather than a rewind off it.
                    st.errorUuid = ev.uuid;
                    st.errorBranchUuids.clear();
                    if (!ev.uuid.empty())
                    {
                        st.errorBranchUuids.insert(ev.uuid);
                    }
                }
                else
                {
                    // A NON-error assistant line appended after an error is the session resuming —
                    // lastWasApiError just cleared, so drop the now-stale descendant frontier too.
                    st.errorUuid.clear();
                    st.errorBranchUuids.clear();
                }
                // The latest assistant block sets / clears the "blocked on the user" flag: an
                // interactive tool_use (AskUserQuestion) parks the turn on the user until answered;
                // a text / non-interactive-tool message means the agent moved on (clear it).
                st.pendingInteractiveTool = ev.toolName; // "" unless this message is an interactive tool_use
                if (!ev.text.empty())
                {
                    st.lastAssistantText = ev.text;
                    // Mirror into the model for a future Auto-Testing "peek". QUIET: streamed text
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
                // CURRENT model: the newest REAL assistant line's message.model wins. The synthetic
                // API-error line's "<synthetic>" pseudo-model is skipped (it is not a model the
                // session runs on — and it must not shadow the real one across an error). Mirrored via
                // the NOTIFYING Update, unlike the quiet per-line mirrors above: it changes at most a
                // handful of times per session (first reply / a `/model` switch, which is otherwise
                // invisible — no cmdline/env/trailer trace), and the board card / per-tab overlay /
                // tab tooltip should repaint when it does. Change-gated so steady state stays silent.
                if (!ev.apiError && !ev.model.empty() && ev.model != L"<synthetic>" && ev.model != st.currentModel)
                {
                    st.currentModel = ev.model;
                    const std::wstring mdl = ev.model;
                    _registry->Update(s.id, [&mdl](SessionInfo& ss) { ss.currentModel = mdl; });
                }
                // COMMANDS.md feeds: this message's file-writing tool_use paths (the markdown
                // await's match signal) and — when its stop_reason is TERMINAL — a turn boundary
                // (unmatched sightings age a turn; see kCommandAwaitMaxTurnEnds). Order matters:
                // the write feed precedes the boundary feed of the SAME message, so a handover md
                // written by the turn's last message matches before the boundary ages the await.
                if (feedWatch)
                {
                    if (!ev.fileWritePaths.empty())
                    {
                        _commandWatch->OnFileToolWrite(s.id, ev.fileWritePaths, s.workingDir, NowMs());
                    }
                    if (IsTerminalStopReason(ev.stopReason))
                    {
                        _commandWatch->OnTurnEnd(s.id);
                    }
                }
            }
            else if (ev.kind == TranscriptEvent::Kind::ToolResult)
            {
                // A tool produced a result -> a pending interactive tool_use (the question) was
                // ANSWERED. (Deliberately NOT a turn event for the run-repair: a bare tool_result
                // never synthesized Running before — preserve that.)
                // Agentmaster: but a tool_result that clears a NON-EMPTY pendingInteractiveTool is
                // exactly "the user answered the question" — the one line that proves it — so record
                // it for the NeedsApproval-only resume gate (ShouldSynthesizeResumed's
                // answeredPendingQuestion). The empty->empty case (an ordinary Bash/Read result) is
                // NOT recorded, which is what keeps ShouldSynthesizeRunning's "a bare tool_result
                // never lights Running" invariant exactly as it was.
                if (!st.pendingInteractiveTool.empty())
                {
                    st.answeredInteractive = true;
                }
                st.pendingInteractiveTool.clear();
                st.lastWasApiError = false; // activity past any error -> the tail is no longer that error
                st.errorUuid.clear();
                st.errorBranchUuids.clear();
            }
            else if (ev.kind == TranscriptEvent::Kind::LeafMarker)
            {
                // The active branch head moved (a new prompt's anchor, the error's bookkeeping child, or a
                // double-ESC REWIND). NOT a turn event: it never sets consumedTurnEvent and never clears
                // lastWasApiError (the error stays physically last). If this moves the leaf off the error
                // AND off its descendant frontier while the error is still positionally last, the
                // reconciler releases Error (ShouldReleaseErrorOnLeafMove); a forward advance onto a
                // turn_duration child (in errorBranchUuids) keeps the error the active leaf.
                st.activeLeafUuid = ev.text;
            }
            else if (ev.kind == TranscriptEvent::Kind::Node)
            {
                // A bookkeeping line (system/turn_duration, away_summary, progress, …). Extend the API
                // error's forward DESCENDANT frontier: a line whose parent is already on the frontier is
                // itself on it. This is what lets the post-error last-prompt marker — which names the
                // error's turn_duration CHILD — be recognized as the error still being the active leaf,
                // not a rewind off it. No-op unless an API error is currently the tail (lastWasApiError).
                if (st.lastWasApiError && !ev.uuid.empty() && !ev.parentUuid.empty() &&
                    st.errorBranchUuids.count(ev.parentUuid) != 0)
                {
                    st.errorBranchUuids.insert(ev.uuid);
                }
            }
            else if (ev.kind == TranscriptEvent::Kind::Command)
            {
                // A slash-command ECHO (COMMANDS.md). Deliberately NOT a turn event and NOT a tail
                // mutation — the echo was pure noise before (dropped in the parser), and the state
                // machine must see byte-identical behavior (the /model false-Running fix). Its ONLY
                // consumer is the CommandWatch: arm a binding's await (the watch's own freshness
                // gate rejects a replayed old command by the line's timestamp).
                if (feedWatch)
                {
                    _commandWatch->OnCommandSighting(s.id, SlashCommand{ ev.commandName, ev.commandArgs }, ev.lineTsMs, NowMs());
                }
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
                st.errorUuid.clear();
                st.errorBranchUuids.clear();
                if (IsUserInterruptMarker(ev.text))
                {
                    // The user hit Esc: NO clean Stop hook fires, and the marker clears the
                    // stop_reason — so without this flag the turn's end goes unseen and the
                    // session stays Running forever. Flag it; recon-stop releases it to Waiting.
                    st.interrupted = true;
                    sawInterruptMarker = true; // DELIVERY.md §14 — parked after the loop if still the tail
                    interruptMarkerTs = ev.lineTsMs;
                    if (feedWatch)
                    {
                        _commandWatch->OnTurnEnd(s.id); // an interrupt ends the command's turn too (COMMANDS.md)
                    }
                }
                else
                {
                    st.interrupted = false;
                    // Control markers that masquerade as user lines — command echoes, task
                    // notifications (STATE.md §8 bug-2) — must not be back-filled into the Flight
                    // Plan as Typed prompts. The parser now drops them before they ever get here
                    // (the /model false-Running fix: a noise line is NOT a turn event and must not
                    // clear the tail facts above either), so this gate is belt+suspenders for the
                    // back-fill seam should the parser's filter ever be relaxed.
                    if (!IsNoiseUserPrompt(ev.text))
                    {
                        // Idempotent by text — back-fills a dropped hook; a fold-match against a
                        // Sent+unechoed Autorun prompt is additionally CONSUMED as that send's PULL
                        // echo (DELIVERY_PLAN.md R1), the line's own timestamp guarding a replay.
                        _registry->NoteExternalPrompt(s.id, ev.text, ev.lineTsMs);
                    }
                }
            }
        }
        // Agentmaster (HOOKS.md "Compaction" — the stale "ctx N"): a compact_boundary that is this chunk's
        // LAST context-bearing line reset the conversation's context to its postTokens (the parser zeroes
        // the field when a usage-bearing assistant line follows the boundary, so the loop's per-line value
        // above already won in that case). Mirrored QUIETLY like the per-line value — a display fact — so
        // the card reads the compacted size right away instead of the pre-compaction one until the next
        // assistant line (after a manual /compact at rest: the user's next prompt, possibly never).
        if (parsed.compactPostTokens > 0 && parsed.compactPostTokens != st.contextTokens)
        {
            st.contextTokens = parsed.compactPostTokens;
            const int64_t tok = parsed.compactPostTokens;
            _registry->UpdateQuiet(s.id, [tok](SessionInfo& ss) { ss.contextTokens = tok; });
        }
        // Agentmaster (the INTERRUPT HOLD — DELIVERY.md §14): the user pressed Esc and has NOT sent
        // anything since (the marker is still the tail) — park the Tests Autorunner NOW, in the parse,
        // BEFORE this pass's reconciliation synthesizes the interrupt's Stop (recon-stop ->
        // WaitingForInput -> the advance seam): a Full autorunner would otherwise auto-send the next
        // queued prompt straight into the session the user just stopped to talk to. The registry
        // refuses a stale/unstamped marker (history replay) and an already-Off autorunner.
        if (sawInterruptMarker && st.interrupted && primedAtParse)
        {
            _registry->NoteInterrupt(s.id, interruptMarkerTs);
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
    // PresenceIsWorking is folded in so a long Task/Agent subagent ("busy" heartbeat, quiescent parent
    // transcript — the recon-subagent promotion's target) or a live shell job ("shell") is never raced
    // to Idle. Like the synthesized
    // missed-Stop above, this is a deliberate TIME-derived transition layered on the hook-derived machine
    // (Rule #7-adjacent — never screen-scraped): the mutator RE-CHECKS the full gate UNDER the registry
    // lock, so a hook / a visit (read stamp) landing between our snapshot and the update wins. Autorunner
    // semantics are unchanged: DecideAdvance treats Idle as ready exactly like WaitingForInput (Rule #1).
    void SessionScanner::_maybeDecayWaiting(const SessionInfo& s, int64_t nowMs)
    {
        const uint32_t minutes = _waitingDecayMinutes.load();
        const bool working = PresenceIsWorking(s.presenceStatus);
        if (!ShouldDecayWaitingToIdle(s.state, s.lastActivityUnixMs, s.readUnixMs, s.manualUnread, working, minutes, nowMs))
        {
            return;
        }
        bool decayed = false;
        _registry->Update(s.id, [&](SessionInfo& live) {
            // Re-check the full gate under the lock against the LIVE record (a visit may have just
            // stamped readUnixMs / cleared manualUnread, or a fresh turn may have moved the state).
            if (ShouldDecayWaitingToIdle(live.state, live.lastActivityUnixMs, live.readUnixMs, live.manualUnread, PresenceIsWorking(live.presenceStatus), minutes, nowMs))
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
                    // Forensics: the per-window liveness probe fan-out — it ticks _ObserverProbe,
                    // _ReconcileClaudeTabs, _SweepClaudeLiveness and _ScanPendingInput. A throw here
                    // silently stops THAT window binding new tabs at all.
                    LogSwallowedException(L"SessionScanner liveness probe");
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
