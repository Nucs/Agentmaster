// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 harness — the API-ERROR REWIND (HOOKS.md "API-error turns", ApiErrorRewind.{h,cpp}).
//
// What is pinned here:
//   * the planner (PlanApiErrorRewind) on the transcript shapes Claude Code 2.1.291 writes — the 2026-10-17
//     incident's mid-response drop (half-streamed thinking + text above the synthetic error, then its
//     turn_duration, cost-state and last-prompt lines), an error before any output, the first response to
//     a prompt, a resume that died again (its "Continue from where you left off." row stepped over),
//     hook attachments of later resumes, a slash-command skill row as the target;
//   * every refusal: refusal / DLP / safety blocks, a conversation that went on, a user rewind, an already
//     rewound file (idempotent), a cleared conversation, a torn last line, two different responses above
//     the error, a window too short (NeedMoreHistory) and the same bytes as a whole file (UnknownShape);
//   * the marker line itself: valid JSON, Claude Code's own rewind keys, explicit, no "rewound", the
//     newest lastPrompt copied (non-ASCII intact), the session id;
//   * the file-level apply (RewindApiErrorTail) on temp files: appended byte for byte at the end, a second
//     run changes nothing, a torn / healthy / refusal file is left untouched, the 256 KiB -> 16 MiB window
//     widening, a missing file; the session-id front door and the holder wait for an id nobody runs.
// The live behaviour this models (Claude Code continuing the turn after the line) was proven on a
// throwaway session; see the commit that added this file.

#include "m5_tests.h"

#include "../ApiErrorRewind.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>

namespace
{
    // ---- transcript row builders (UTF-8 JSON lines, Claude Code 2.1.291's shapes, dummy text) ----
    constexpr const char* kSid = "5e551011-0000-4000-8000-000000000001";

    std::string Q(const std::string& s)
    {
        return "\"" + s + "\"";
    }

    std::string Parent(const std::string& parent)
    {
        return parent.empty() ? std::string{ "null" } : Q(parent);
    }

    std::string Common(const std::string& uuid, const std::string& ts)
    {
        return ",\"uuid\":" + Q(uuid) + ",\"timestamp\":" + Q(ts) + ",\"sessionId\":" + Q(kSid) + ",\"version\":\"2.1.291\"";
    }

    std::string UserPrompt(const std::string& uuid, const std::string& parent, const std::string& text)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":" + Q(text) + "}" + Common(uuid, "2026-10-17T15:50:00.000Z") + "}";
    }

    std::string MetaUser(const std::string& uuid, const std::string& parent, const std::string& text)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":" + Q(text) + "}]},\"isMeta\":true" + Common(uuid, "2026-10-17T15:50:01.000Z") + "}";
    }

    std::string ToolUse(const std::string& uuid, const std::string& parent, const std::string& msgId, const std::string& toolId)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"message\":{\"model\":\"claude-opus-5-5\",\"id\":" + Q(msgId) +
               ",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":" + Q(toolId) +
               ",\"name\":\"Bash\",\"input\":{\"command\":\"echo hi\"}}],\"stop_reason\":\"tool_use\"},\"requestId\":\"req_" + msgId + "\",\"type\":\"assistant\"" + Common(uuid, "2026-10-17T15:51:00.000Z") + "}";
    }

    std::string ToolResult(const std::string& uuid, const std::string& parent, const std::string& toolId, const std::string& output = "hi")
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"tool_use_id\":" + Q(toolId) +
               ",\"type\":\"tool_result\",\"content\":" + Q(output) + "}]},\"toolUseResult\":{\"stdout\":" + Q(output) + "}" + Common(uuid, "2026-10-17T15:52:00.000Z") + "}";
    }

    std::string Attachment(const std::string& uuid, const std::string& parent, const std::string& attType)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"attachment\":{\"type\":" + Q(attType) + "},\"type\":\"attachment\"" + Common(uuid, "2026-10-17T15:52:01.000Z") + "}";
    }

    std::string Partial(const std::string& uuid, const std::string& parent, const std::string& msgId, bool thinking)
    {
        const std::string block = thinking ? std::string{ "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"c2ln\"}" } :
                                             std::string{ "{\"type\":\"text\",\"text\":\"I write the batch-1 benchmark twins:\"}" };
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"message\":{\"model\":\"claude-opus-5-5\",\"id\":" + Q(msgId) +
               ",\"type\":\"message\",\"role\":\"assistant\",\"content\":[" + block + "],\"stop_reason\":\"end_turn\"},\"requestId\":\"req_" + msgId + "\",\"type\":\"assistant\"" + Common(uuid, "2026-10-17T15:56:36.000Z") + "}";
    }

    std::string Reply(const std::string& uuid, const std::string& parent, const std::string& msgId, const std::string& text)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"message\":{\"model\":\"claude-opus-5-5\",\"id\":" + Q(msgId) +
               ",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":" + Q(text) + "}],\"stop_reason\":\"end_turn\"},\"type\":\"assistant\"" + Common(uuid, "2026-10-17T16:00:00.000Z") + "}";
    }

    // The synthetic API-error row. `extra` adds top-level members (",\"apiError\":\"…\"").
    std::string ApiError(const std::string& uuid, const std::string& parent, const std::string& text, bool truncatedAfterOutput, const std::string& stopReason = "stop_sequence", const std::string& extra = "")
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"assistant\",\"message\":{\"id\":\"70a0f90f\",\"model\":\"<synthetic>\",\"role\":\"assistant\",\"stop_reason\":" + Q(stopReason) +
               ",\"stop_sequence\":\"\",\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":" + Q(text) + "}]},\"error\":\"server_error\"" +
               (truncatedAfterOutput ? ",\"truncatedAfterOutput\":true" : "") + ",\"isApiErrorMessage\":true" + extra + Common(uuid, "2026-10-17T15:56:48.131Z") + "}";
    }

    std::string TurnDuration(const std::string& uuid, const std::string& parent)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"system\",\"subtype\":\"turn_duration\",\"durationMs\":2410825,\"isMeta\":false" + Common(uuid, "2026-10-17T15:56:48.160Z") + "}";
    }

    std::string LastPrompt(const std::string& leaf, const std::string& lastPrompt = "Finish this", bool isExplicit = false)
    {
        return "{\"type\":\"last-prompt\",\"lastPrompt\":" + Q(lastPrompt) + ",\"leafUuid\":" + Q(leaf) + (isExplicit ? ",\"explicit\":true" : "") + ",\"sessionId\":" + Q(kSid) + "}";
    }

    std::string CostState()
    {
        return std::string{ "{\"type\":\"cost-state\",\"sessionId\":\"" } + kSid + "\",\"totalCostUSD\":20.5}";
    }

    std::string Join(std::initializer_list<std::string> lines)
    {
        std::string out;
        for (const auto& l : lines)
        {
            out += l;
            out += "\n";
        }
        return out;
    }

    // The incident's shape (0391efeb, 2026-10-17): a tool result, a token reminder, the half-streamed
    // response (thinking + text, one message id), the synthetic error, its turn_duration and the
    // bookkeeping lines claude writes after it.
    std::string IncidentTail()
    {
        return Join({
            UserPrompt("u0000001", "", "Finish this"),
            ToolUse("a0000001", "u0000001", "msg_1", "toolu_1"),
            ToolResult("r0000001", "a0000001", "toolu_1"),
            Attachment("g0000001", "r0000001", "total_tokens_reminder"),
            Partial("p0000001", "g0000001", "msg_2", true),
            Partial("p0000002", "p0000001", "msg_2", false),
            ApiError("e0000001", "p0000002", "API Error: Connection lost mid-response. The response above may be incomplete.", true),
            TurnDuration("t0000001", "e0000001"),
            CostState(),
            LastPrompt("t0000001"),
            CostState(),
            CostState(),
        });
    }

    std::optional<json::Value> ParseMarker(const std::string& line)
    {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0'); // (not std::max: windows.h's max macro is live in this TU)
        if (n > 0)
        {
            ::MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), w.data(), n);
        }
        return json::Parse(w);
    }

    // The temp transcripts live in the run's SCRATCH profile (run-m5-tests.bat points AGENTMASTER_PROFILE at a
    // wiped %TEMP% folder; an isolated run at its own), so two harness runs never write the same files.
    std::filesystem::path ScratchDir()
    {
        auto dir = std::filesystem::path{ AgentmasterStateDir() } / L"api-error-rewind";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    void WriteBytes(const std::filesystem::path& p, const std::string& bytes)
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::string ReadBytes(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    void TestPlanner()
    {
        const std::wstring sid = L"5e551011-0000-4000-8000-000000000001";

        // 1. the incident: rewound past the half-response + error to the tool result
        {
            const auto p = PlanApiErrorRewind(IncidentTail(), true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind, "rewind: the incident's mid-response drop is rewound");
            CHECK(p.targetUuid == L"r0000001", "rewind: the target is the tool result below the failed response");
            CHECK(p.targetKind == L"tool result", "rewind: the target kind is a tool result (Claude Code continues the turn)");
            CHECK(p.partialRows == 2, "rewind: both half-response rows (thinking + text) are stepped over");
            CHECK(p.rowsAfterError == 1, "rewind: the turn_duration row after the error is counted");
            CHECK(p.errorUuid == L"e0000001", "rewind: the error row is identified");
            CHECK(p.errorKind == L"server_error", "rewind: the error kind is the row's `error` field");
            CHECK(p.errorText == L"API Error: Connection lost mid-response. The response above may be incomplete.", "rewind: the error text is the row's text");
            const auto m = ParseMarker(p.markerLine);
            CHECK(m && m->type == json::Value::Type::Obj, "rewind: the marker line is a JSON object");
            if (m)
            {
                CHECK(m->StrAt(L"type") == L"last-prompt", "rewind: the marker is a last-prompt line");
                CHECK(m->StrAt(L"leafUuid") == L"r0000001", "rewind: the marker's leaf is the target");
                CHECK(m->BoolAt(L"explicit") == true, "rewind: the marker is explicit (the loader keeps it as the leaf)");
                CHECK(m->Find(L"rewound") == nullptr, "rewind: the marker carries no 'rewound' (that would suppress the auto-continue)");
                CHECK(m->StrAt(L"sessionId") == sid, "rewind: the marker names the session");
                CHECK(m->StrAt(L"lastPrompt") == L"Finish this", "rewind: the newest lastPrompt is copied");
                CHECK(m->members.size() == 5 && m->members[0].first == L"type" && m->members[2].first == L"leafUuid" && m->members[3].first == L"explicit",
                      "rewind: Claude Code's own key order (type, lastPrompt, leafUuid, explicit, sessionId)");
            }
            CHECK(p.markerLine.find('\n') == std::string::npos, "rewind: the marker line has no newline of its own");
        }

        // 2. an error before any output (no half-response): still rewound, uniformly
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "go"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                ToolResult("r1", "a1", "toolu_1"),
                Attachment("g1", "r1", "total_tokens_reminder"),
                ApiError("e1", "g1", "API Error: Connection lost before a response was produced. Try again.", false),
                TurnDuration("t1", "e1"),
                LastPrompt("t1"),
            });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind && p.targetUuid == L"r1" && p.partialRows == 0, "rewind: an error before any output is rewound to the tool result");
        }

        // 3. the first response to a prompt failed: the prompt is the target (Claude Code resends it)
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "Fix the bug"),
                ApiError("e1", "u1", "API Error: Overloaded", false),
                TurnDuration("t1", "e1"),
                LastPrompt("t1", "Fix the bug"),
            });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind && p.targetUuid == L"u1" && p.targetKind == L"prompt", "rewind: the first response to a prompt failing rewinds to the prompt");
        }

        // 4. refusal / DLP / safety blocks are never rewound
        {
            const auto refusal = Join({ UserPrompt("u1", "", "x"), ApiError("e1", "u1", "API Error: refused", false, "refusal"), TurnDuration("t1", "e1") });
            CHECK(PlanApiErrorRewind(refusal, true, sid).verdict == ApiErrorRewindVerdict::RefusalError, "refusal: stop_reason refusal is never rewound");
            const auto dlp = Join({ UserPrompt("u1", "", "x"), ApiError("e1", "u1", "API Error: dlp", false, "stop_sequence", ",\"apiError\":\"dlp_request_denied\""), TurnDuration("t1", "e1") });
            CHECK(PlanApiErrorRewind(dlp, true, sid).verdict == ApiErrorRewindVerdict::RefusalError, "refusal: a DLP denial is never rewound");
            const auto safety = Join({ UserPrompt("u1", "", "x"), ApiError("e1", "u1", "API Error: blocked", false, "stop_sequence", ",\"apiError\":\"safety_monitor_blocked\""), TurnDuration("t1", "e1") });
            CHECK(PlanApiErrorRewind(safety, true, sid).verdict == ApiErrorRewindVerdict::RefusalError, "refusal: a safety-monitor block is never rewound");
            const auto rate = Join({ UserPrompt("u1", "", "x"), ApiError("e1", "u1", "API Error: Rate limited", false, "stop_sequence", ",\"apiError\":\"rate_limit\""), TurnDuration("t1", "e1") });
            CHECK(PlanApiErrorRewind(rate, true, sid).verdict == ApiErrorRewindVerdict::Rewind, "rewind: any other apiError kind (rate_limit) is rewound");
        }

        // 5. the conversation went on after the error: nothing to do
        {
            const auto bytes = IncidentTail() + Join({ UserPrompt("u2", "t0000001", "continue"), Reply("a2", "u2", "msg_3", "Done."), LastPrompt("a2", "continue") });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::NotApplicable, "not applicable: the user went on after the error");
            // ...even with an explicit marker pointing back at the error's own turn_duration (the safety check)
            const auto odd = bytes + Join({ LastPrompt("t0000001", "x", true) });
            CHECK(PlanApiErrorRewind(odd, true, sid).verdict == ApiErrorRewindVerdict::NotApplicable, "not applicable: newer messages exist after the error, whatever the marker says");
        }

        // 6. idempotent: the incident plus our own marker plans nothing
        {
            const auto first = PlanApiErrorRewind(IncidentTail(), true, sid);
            const auto again = PlanApiErrorRewind(IncidentTail() + first.markerLine + "\n", true, sid);
            CHECK(again.verdict == ApiErrorRewindVerdict::NotApplicable, "idempotent: a rewound transcript plans nothing");
        }

        // 7. a user rewind to before the error (an explicit marker to an earlier row)
        {
            const auto p = PlanApiErrorRewind(IncidentTail() + Join({ LastPrompt("u0000001", "Finish this", true) }), true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::NotApplicable, "not applicable: the user rewound off the error");
        }

        // 8. a torn last line
        {
            auto bytes = IncidentTail();
            bytes.pop_back(); // the final "\n"
            CHECK(PlanApiErrorRewind(bytes, true, sid).verdict == ApiErrorRewindVerdict::TornTail, "torn: no newline at the end is refused");
            const std::string healthy = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m", "ok") });
            CHECK(PlanApiErrorRewind(healthy.substr(0, healthy.size() - 1), true, sid).verdict == ApiErrorRewindVerdict::NotApplicable, "torn: a healthy file mid-write is not reported (the fast exit runs first)");
        }

        // 9. a window too short: NeedMoreHistory; the same bytes as a whole file: UnknownShape
        {
            const auto all = IncidentTail();
            const auto cut = all.find("{\"parentUuid\":\"r0000001\""); // the attachment's line start
            const std::string window = all.substr(cut);
            CHECK(PlanApiErrorRewind(window, false, sid).verdict == ApiErrorRewindVerdict::NeedMoreHistory, "window: the walk leaving the bytes read asks for more history");
            CHECK(PlanApiErrorRewind(window, true, sid).verdict == ApiErrorRewindVerdict::UnknownShape, "window: the same bytes as a whole file are an unknown shape");
        }

        // 10. a resume that died again: Claude Code's continuation row is stepped over too
        {
            const auto first = PlanApiErrorRewind(IncidentTail(), true, sid);
            const auto bytes = IncidentTail() + first.markerLine + "\n" +
                               Join({
                                   MetaUser("c0000001", "g0000001", "Continue from where you left off."),
                                   Partial("p0000003", "c0000001", "msg_4", false),
                                   ApiError("e0000002", "p0000003", "API Error: Connection lost mid-response. The response above may be incomplete.", true),
                                   TurnDuration("t0000002", "e0000002"),
                                   LastPrompt("t0000002"),
                               });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind && p.targetUuid == L"r0000001", "rewind again: a second failure rewinds to the same tool result");
            CHECK(p.resumePromptRows == 1 && p.partialRows == 1, "rewind again: the continuation row and the new half-response are stepped over");
        }

        // 11. hook attachments of later resumes hang off the turn_duration
        {
            const auto bytes = IncidentTail() + Join({ Attachment("h0000001", "t0000001", "hook_success"), Attachment("h0000002", "h0000001", "hook_additional_context"), CostState() });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind && p.targetUuid == L"r0000001" && p.rowsAfterError == 3, "rewind: hook attachments after the error are tolerated");
        }

        // 12. two different responses above the error: left alone
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "x"),
                Reply("a1", "u1", "msg_A", "complete answer"),
                Partial("p1", "a1", "msg_B", false),
                ApiError("e1", "p1", "API Error: Connection lost mid-response.", true),
                TurnDuration("t1", "e1"),
            });
            CHECK(PlanApiErrorRewind(bytes, true, sid).verdict == ApiErrorRewindVerdict::UnknownShape, "unknown: two responses above the error with no user row between");
        }

        // 13. sidechain rows are ignored; 14. no marker at all: the last row is the leaf
        {
            auto bytes = IncidentTail();
            bytes += "{\"parentUuid\":\"zz\",\"isSidechain\":true,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"subagent\"},\"uuid\":\"s1\"}\n";
            CHECK(PlanApiErrorRewind(bytes, true, sid).verdict == ApiErrorRewindVerdict::Rewind, "sidechain: a subagent row after the error changes nothing");
            const auto noMarker = Join({
                UserPrompt("u1", "", "x"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                ToolResult("r1", "a1", "toolu_1"),
                Partial("p1", "r1", "msg_2", false),
                ApiError("e1", "p1", "API Error: Connection lost mid-response.", true),
                TurnDuration("t1", "e1"),
            });
            const auto p = PlanApiErrorRewind(noMarker, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind && p.targetUuid == L"r1", "no marker: the last row is the leaf and the rewind still applies");
            const auto m = ParseMarker(p.markerLine);
            CHECK(m && m->Find(L"lastPrompt") == nullptr, "no marker: no lastPrompt to copy, so none is written");
        }

        // 15. a healthy transcript: the fast exit
        {
            const auto bytes = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m", "ok"), TurnDuration("t1", "a1"), LastPrompt("t1") });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::NotApplicable && p.reason.empty() && p.errorUuid.empty(), "healthy: nothing to do, nothing reported");
            CHECK(PlanApiErrorRewind({}, true, sid).verdict == ApiErrorRewindVerdict::NotApplicable, "healthy: an empty transcript is nothing to do");
        }

        // 16. the target is a slash command's skill text (meta): rewound, reported as meta
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "<command-name>/np-function</command-name>"),
                MetaUser("m1", "u1", "When user requests /np-function, follow these instructions"),
                Attachment("g1", "m1", "environment"),
                ApiError("e1", "g1", "API Error: Connection lost before a response was produced.", false),
                TurnDuration("t1", "e1"),
            });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind && p.targetUuid == L"m1" && p.targetKind == L"meta", "rewind: a meta target is rewound and reported as meta");
        }

        // 17. a conversation cleared to empty (a null explicit marker at the end)
        {
            const auto bytes = IncidentTail() + "{\"type\":\"last-prompt\",\"leafUuid\":null,\"explicit\":true,\"sessionId\":\"x\"}\n";
            CHECK(PlanApiErrorRewind(bytes, true, sid).verdict == ApiErrorRewindVerdict::NotApplicable, "cleared: an explicitly emptied conversation plans nothing");
        }

        // 18. a non-ASCII lastPrompt survives (Hebrew + an emoji, written as UTF-8 bytes)
        {
            const std::string hebrew = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xF0\x9F\x98\x80"; // shalom + U+1F600
            const auto bytes = Join({
                UserPrompt("u1", "", hebrew),
                ApiError("e1", "u1", "API Error: Overloaded", false),
                TurnDuration("t1", "e1"),
                LastPrompt("t1", hebrew),
            });
            const auto p = PlanApiErrorRewind(bytes, true, sid);
            const auto m = ParseMarker(p.markerLine);
            const std::wstring expected = L"\x05E9\x05DC\x05D5\x05DD \xD83D\xDE00";
            CHECK(p.verdict == ApiErrorRewindVerdict::Rewind && m && m->StrAt(L"lastPrompt") == expected, "marker: a Hebrew + emoji lastPrompt round-trips through the UTF-8 line");
            CHECK(p.markerLine.find(hebrew) != std::string::npos, "marker: the lastPrompt is written as UTF-8, not escaped");
        }

        // 19. the session id falls back to the error row's own when none is given
        {
            const auto p = PlanApiErrorRewind(IncidentTail(), true, L"");
            const auto m = ParseMarker(p.markerLine);
            CHECK(m && m->StrAt(L"sessionId") == L"5e551011-0000-4000-8000-000000000001", "marker: the error row's sessionId is used when none is passed");
        }
    }

    void TestFileApply()
    {
        const auto dir = ScratchDir();
        const std::wstring sid = L"5e551011-0000-4000-8000-0000000000aa"; // no claude runs this id

        // appended at the end, byte for byte; a second run changes nothing
        {
            const auto path = dir / L"incident.jsonl";
            const auto before = IncidentTail();
            WriteBytes(path, before);
            const auto r = RewindApiErrorTail(path.wstring(), sid);
            CHECK(r.outcome == ApiErrorRewindOutcome::Appended, "apply: the incident file gets the marker");
            CHECK(r.appendedAtByte == before.size(), "apply: the marker's offset is the old end of the file");
            const auto after = ReadBytes(path);
            CHECK(after == before + r.plan.markerLine + "\n", "apply: the file is the old bytes plus exactly one line");
            const auto again = RewindApiErrorTail(path.wstring(), sid);
            CHECK(again.outcome == ApiErrorRewindOutcome::NotNeeded && ReadBytes(path) == after, "apply: a second run writes nothing");
            const auto line = DescribeApiErrorRewind(r, sid, L"restart");
            CHECK(line.find(L"[api-error-rewind] 5e551011 restart: rewound past") == 0 && line.find(L"r0000001") != std::wstring::npos, "apply: the log line names the session, the seam and the target");
        }

        // untouched: a torn file, a healthy file, a refusal
        {
            const auto torn = dir / L"torn.jsonl";
            auto bytes = IncidentTail();
            bytes.pop_back();
            WriteBytes(torn, bytes);
            const auto r = RewindApiErrorTail(torn.wstring(), sid);
            CHECK(r.outcome == ApiErrorRewindOutcome::Skipped && ReadBytes(torn) == bytes, "apply: a torn file is skipped and left untouched");

            const auto healthy = dir / L"healthy.jsonl";
            const auto ok = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m", "ok") });
            WriteBytes(healthy, ok);
            CHECK(RewindApiErrorTail(healthy.wstring(), sid).outcome == ApiErrorRewindOutcome::NotNeeded && ReadBytes(healthy) == ok, "apply: a healthy file is not touched");

            const auto refusal = dir / L"refusal.jsonl";
            const auto rf = Join({ UserPrompt("u1", "", "x"), ApiError("e1", "u1", "API Error: refused", false, "refusal"), TurnDuration("t1", "e1") });
            WriteBytes(refusal, rf);
            const auto rr = RewindApiErrorTail(refusal.wstring(), sid);
            CHECK(rr.outcome == ApiErrorRewindOutcome::Skipped && ReadBytes(refusal) == rf, "apply: a refusal is skipped and left untouched");
        }

        // the window widening: a tool result bigger than the first 256 KiB window
        {
            const auto big = dir / L"big.jsonl";
            const std::string output(300 * 1024, 'x');
            const auto bytes = Join({
                UserPrompt("u1", "", "x"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                ToolResult("r1", "a1", "toolu_1", output),
                Partial("p1", "r1", "msg_2", false),
                ApiError("e1", "p1", "API Error: Connection lost mid-response.", true),
                TurnDuration("t1", "e1"),
                LastPrompt("t1"),
            });
            WriteBytes(big, bytes);
            const auto r = RewindApiErrorTail(big.wstring(), sid);
            CHECK(r.outcome == ApiErrorRewindOutcome::Appended && r.plan.targetUuid == L"r1", "apply: a target beyond the first window is reached by the wider read");
        }

        // a missing file; an id with no transcript; the holder wait for an id nobody runs
        {
            CHECK(RewindApiErrorTail((dir / L"missing.jsonl").wstring(), sid).outcome == ApiErrorRewindOutcome::Failed, "apply: a missing file is a failure, not a write");
            CHECK(RewindApiErrorTailForSession(L"5e551011-0000-4000-8000-0000000000ff").outcome == ApiErrorRewindOutcome::NotNeeded, "apply: a session with no transcript needs nothing");
            CHECK(PlanApiErrorRewindForSession(L"5e551011-0000-4000-8000-0000000000ff").verdict == ApiErrorRewindVerdict::NotApplicable, "plan: a session with no transcript plans nothing");
            auto w = ProcessExitWait::ForSession(sid);
            CHECK(w.Count() == 0 && w.WaitAll(0), "wait: nobody runs the id, so the wait is over at once");
            auto empty = ProcessExitWait::ForSession(L"");
            CHECK(empty.Count() == 0 && empty.WaitAll(0), "wait: an empty id holds nothing");
            CHECK(LiveClaudeProcessesForSession(sid).empty(), "wait: no live claude for an unused id");
            ProcessExitWait moved = std::move(w);
            CHECK(moved.Count() == 0 && moved.Pids().empty(), "wait: a moved-from waiter hands over its (empty) set");
        }
    }
}

void TestApiErrorRewind()
{
    TestPlanner();
    TestFileApply();
}
