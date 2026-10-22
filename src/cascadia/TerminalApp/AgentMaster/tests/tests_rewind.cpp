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

// ===== HOOKS.md "Interrupted turns": the user-interrupt rewind, the resume classifier, the switch, the fork stop watch =====
//
// The transcript shapes below are the ones the 2026-10-22 live probes wrote with the real claude.exe 2.1.291 against the e2e
// mock (tools/e2e-spike): an Esc during a reply (e1), during the FIRST and the SECOND tool call of a turn (e2), a fork of a
// session waiting on its second request and stopped by one Esc (e3 / e4), a fork of a session inside a running Bash call
// (e3b). Each probe proved the live behavior these plans model (a resume with the planned line continued the turn; without
// it, nothing); the rows here keep the probes' parent links and kinds with dummy text.
namespace
{
    std::string InterruptRow(const std::string& uuid, const std::string& parent, bool forToolUse, bool shutdown = false)
    {
        const std::string text = forToolUse ? "[Request interrupted by user for tool use]" : "[Request interrupted by user]";
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":" + Q(text) + "}]}" +
               (shutdown ? ",\"interruptedByShutdown\":true" : "") + Common(uuid, "2026-10-22T10:00:00.000Z") + "}";
    }

    // The result Claude Code writes for a tool call an Esc cut (the probes' "The user doesn't want to proceed ..." row).
    std::string RejectedResult(const std::string& uuid, const std::string& parent, const std::string& toolId, const std::string& extra = "")
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"content\":" +
               "\"The user doesn't want to proceed with this tool use. The tool use was rejected (eg. if it was a file edit, the new_string was NOT written to the file). STOP what you are doing and wait for the user to tell you how to proceed.\"" +
               ",\"is_error\":true,\"tool_use_id\":" + Q(toolId) + "}]},\"toolUseResult\":\"User rejected tool use\"" + extra + Common(uuid, "2026-10-22T10:00:01.000Z") + "}";
    }

    // A tool_use row with the given tool name (ToolUse above always names Bash).
    std::string ToolCall(const std::string& uuid, const std::string& parent, const std::string& msgId, const std::string& toolId, const std::string& name)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"message\":{\"model\":\"claude-opus-5-5\",\"id\":" + Q(msgId) +
               ",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":" + Q(toolId) +
               ",\"name\":" + Q(name) + ",\"input\":{}}],\"stop_reason\":null},\"type\":\"assistant\"" + Common(uuid, "2026-10-22T10:00:02.000Z") + "}";
    }

    std::string LastPromptRewound(const std::string& leaf)
    {
        return "{\"type\":\"last-prompt\",\"lastPrompt\":\"x\",\"leafUuid\":" + Q(leaf) + ",\"explicit\":true,\"rewound\":true,\"sessionId\":" + Q(kSid) + "}";
    }

    std::string FileHistorySnapshot()
    {
        return "{\"type\":\"file-history-snapshot\",\"messageId\":\"m\",\"snapshot\":{},\"isSnapshotUpdate\":false}";
    }

    // e1: an Esc in the middle of a reply (streamed text, then the interrupt row), then claude's exit bookkeeping.
    std::string EscDuringReply()
    {
        return Join({
            UserPrompt("u0000001", "", "e1 prompt: refactor the parser"),
            Attachment("g0000001", "u0000001", "date"),
            Attachment("g0000002", "g0000001", "prompt_snapshot"),
            LastPrompt("g0000002", "e1 prompt: refactor the parser"),
            Partial("p0000001", "g0000002", "msg_1", false),
            InterruptRow("i0000001", "p0000001", false),
            FileHistorySnapshot(),
            CostState(),
            LastPrompt("i0000001", "e1 prompt: refactor the parser"),
            CostState(),
        });
    }

    // e2 (second run): an Esc during the SECOND tool call — the first call's result "one" is the step to continue from.
    std::string EscDuringSecondTool()
    {
        return Join({
            UserPrompt("u0000001", "", "e2 prompt"),
            Attachment("g0000001", "u0000001", "prompt_snapshot"),
            ToolUse("a0000001", "g0000001", "msg_1", "toolu_1"),
            ToolResult("r0000001", "a0000001", "toolu_1", "one"),
            Attachment("g0000002", "r0000001", "total_tokens_reminder"),
            ToolUse("a0000002", "g0000002", "msg_2", "toolu_2"),
            LastPrompt("a0000002", "e2 prompt"),
            RejectedResult("x0000001", "a0000002", "toolu_2"),
            InterruptRow("i0000001", "x0000001", true),
            FileHistorySnapshot(),
            CostState(),
            LastPrompt("i0000001", "e2 prompt"),
        });
    }

    void TestInterruptPlanner()
    {
        const std::wstring sid = L"5e551011-0000-4000-8000-000000000001";

        // e1: rewound to the prompt (Claude Code sends it again)
        {
            const auto p = PlanInterruptRewind(EscDuringReply(), true, sid);
            CHECK(p.verdict == InterruptRewindVerdict::Rewind, "interrupt e1: an Esc during a reply is planned");
            CHECK(p.targetUuid == L"u0000001" && p.targetKind == L"prompt", "interrupt e1: the target is the prompt below the interrupted reply");
            CHECK(p.targetText == L"e1 prompt: refactor the parser", "interrupt e1: the prompt's first line is kept for the question");
            CHECK(!p.duringToolUse && p.partialRows == 1 && p.discardedResults == 0, "interrupt e1: one reply row stepped over, during a reply");
            CHECK(p.interruptUuid == L"i0000001" && p.rowsAfterInterrupt == 0, "interrupt e1: the interrupt row is identified");
            const auto m = ParseMarker(p.markerLine);
            CHECK(m && m->StrAt(L"type") == L"last-prompt" && m->StrAt(L"leafUuid") == L"u0000001", "interrupt e1: the marker moves the leaf to the prompt");
            CHECK(m && m->BoolAt(L"explicit") && m->Find(L"rewound") == nullptr, "interrupt e1: the marker is explicit and never 'rewound'");
            CHECK(m && m->StrAt(L"sessionId") == sid && m->StrAt(L"lastPrompt") == L"e1 prompt: refactor the parser", "interrupt e1: the session id and the newest lastPrompt");
            CHECK(p.markerLine.find('\n') == std::string::npos, "interrupt e1: no newline in the marker line");
        }

        // e2: rewound to the first call's result (Claude Code continues the turn)
        {
            const auto p = PlanInterruptRewind(EscDuringSecondTool(), true, sid);
            CHECK(p.verdict == InterruptRewindVerdict::Rewind && p.targetUuid == L"r0000001" && p.targetKind == L"tool result", "interrupt e2: an Esc in the second tool call goes back to the first call's result");
            CHECK(p.duringToolUse && p.discardedResults == 1 && p.partialRows == 1, "interrupt e2: the cut call's result and its tool_use are stepped over");
        }

        // e2 (first run): an Esc during the FIRST call — the cut batch, then the prompt
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "e2 prompt"),
                Attachment("g1", "u1", "prompt_snapshot"),
                ToolUse("a1", "g1", "msg_1", "toolu_1"),
                RejectedResult("x1", "a1", "toolu_1"),
                Attachment("g2", "x1", "prompt_snapshot"),
                InterruptRow("i1", "g2", true),
                LastPrompt("i1", "e2 prompt"),
            });
            const auto p = PlanInterruptRewind(bytes, true, sid);
            CHECK(p.verdict == InterruptRewindVerdict::Rewind && p.targetUuid == L"u1" && p.targetKind == L"prompt" && p.discardedResults == 1, "interrupt: an Esc in the first call rewinds to the prompt");
        }

        // e3/e4: a fork stopped by one Esc — its own "Continue from where you left off." row, then the source's tool result
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "e3 prompt"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                ToolResult("r1", "a1", "toolu_1", "one"),
                Attachment("g1", "r1", "total_tokens_reminder"),
                MetaUser("c1", "g1", "Continue from where you left off."),
                InterruptRow("i1", "c1", false),
                FileHistorySnapshot(),
                LastPrompt("i1", "e3 prompt"),
            });
            const auto p = PlanInterruptRewind(bytes, true, sid);
            CHECK(p.verdict == InterruptRewindVerdict::Rewind && p.targetUuid == L"r1" && p.resumePromptRows == 1, "interrupt e4: a stopped fork rewinds past its continuation row to the tool result");
        }

        // a retried request the Esc ended: the transient error row is stepped over
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "x"),
                ApiError("e1", "u1", "API Error: Overloaded", false),
                InterruptRow("i1", "e1", false),
            });
            const auto p = PlanInterruptRewind(bytes, true, sid);
            CHECK(p.verdict == InterruptRewindVerdict::Rewind && p.targetUuid == L"u1" && p.errorRows == 1, "interrupt: a transient API error above the interrupt is stepped over");
            const auto refused = Join({ UserPrompt("u1", "", "x"), ApiError("e1", "u1", "API Error: refused", false, "refusal"), InterruptRow("i1", "e1", false) });
            CHECK(PlanInterruptRewind(refused, true, sid).verdict == InterruptRewindVerdict::UnknownShape, "interrupt: a refused response above the interrupt is never resent");
        }

        // nothing to take back
        {
            const auto shutdown = Join({ UserPrompt("u1", "", "x"), Partial("p1", "u1", "m1", false), InterruptRow("i1", "p1", false, true) });
            CHECK(PlanInterruptRewind(shutdown, true, sid).verdict == InterruptRewindVerdict::NotApplicable, "interrupt: a SHUTDOWN interrupt is Claude Code's own (it continues it)");
            const auto wentOn = EscDuringReply() + Join({ UserPrompt("u2", "i0000001", "never mind"), Reply("a2", "u2", "msg_2", "ok"), LastPrompt("a2", "never mind") });
            CHECK(PlanInterruptRewind(wentOn, true, sid).verdict == InterruptRewindVerdict::NotApplicable, "interrupt: the conversation went on after the Esc");
            const auto healthy = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m", "ok"), LastPrompt("a1") });
            const auto hp = PlanInterruptRewind(healthy, true, sid);
            CHECK(hp.verdict == InterruptRewindVerdict::NotApplicable && hp.reason.empty(), "interrupt: a healthy transcript stops at the byte scan");
            CHECK(PlanInterruptRewind({}, true, sid).verdict == InterruptRewindVerdict::NotApplicable, "interrupt: an empty transcript");
            // an interrupt OLDER than the tail (the user went on, and the marker text sits in the history)
            const auto older = EscDuringReply() + Join({ UserPrompt("u3", "i0000001", "again"), Reply("a3", "u3", "msg_3", "done") });
            CHECK(PlanInterruptRewind(older, true, sid).verdict == InterruptRewindVerdict::NotApplicable, "interrupt: an older interrupt in the history is not the tail");
        }

        // idempotent: the marker's own line plans nothing
        {
            const auto first = PlanInterruptRewind(EscDuringReply(), true, sid);
            const auto again = PlanInterruptRewind(EscDuringReply() + first.markerLine + "\n", true, sid);
            CHECK(again.verdict == InterruptRewindVerdict::NotApplicable, "interrupt: a rewound transcript plans nothing (idempotent)");
            // ...and the classifier then sees a prompt it RESENDS (what the probe's resume did)
            const auto c = ClassifyResumeContinuation(EscDuringReply() + first.markerLine + "\n", true);
            CHECK(c.verdict == ResumeContinuation::ContinuesTurn || c.verdict == ResumeContinuation::ResendsPrompt, "interrupt: after the line, a resume continues the turn");
            const auto c2 = ClassifyResumeContinuation(EscDuringSecondTool() + PlanInterruptRewind(EscDuringSecondTool(), true, sid).markerLine + "\n", true);
            CHECK(c2.verdict == ResumeContinuation::ContinuesTurn && c2.tailKind == L"tool result", "interrupt: after the line, a tool-result tail continues the turn");
        }

        // torn, shapes, windows
        {
            auto torn = EscDuringReply();
            torn.pop_back();
            CHECK(PlanInterruptRewind(torn, true, sid).verdict == InterruptRewindVerdict::TornTail, "interrupt: a torn last line is never appended to");
            const auto twoReplies = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "msg_A", "done"), Partial("p1", "a1", "msg_B", false), InterruptRow("i1", "p1", false) });
            CHECK(PlanInterruptRewind(twoReplies, true, sid).verdict == InterruptRewindVerdict::UnknownShape, "interrupt: two replies above the interrupt");
            const auto all = EscDuringSecondTool();
            const auto cut = all.find("{\"parentUuid\":\"r0000001\""); // from the reminder attachment on
            CHECK(PlanInterruptRewind(all.substr(cut), false, sid).verdict == InterruptRewindVerdict::NeedMoreHistory, "interrupt: a short window asks for more history");
            CHECK(PlanInterruptRewind(all.substr(cut), true, sid).verdict == InterruptRewindVerdict::UnknownShape, "interrupt: the same bytes as a whole file are an unknown shape");
            const auto twoInterrupts = Join({ UserPrompt("u1", "", "x"), InterruptRow("i0", "u1", false), Partial("p1", "i0", "m1", false), InterruptRow("i1", "p1", false) });
            CHECK(PlanInterruptRewind(twoInterrupts, true, sid).verdict == InterruptRewindVerdict::UnknownShape, "interrupt: an earlier interrupt above this one");
        }

        // targets Claude Code never continues
        {
            const auto meta = Join({
                UserPrompt("u1", "", "<command-name>/handover</command-name>"),
                MetaUser("m1", "u1", "When the user types /handover, write the briefing"),
                Partial("p1", "m1", "msg_1", false),
                InterruptRow("i1", "p1", false),
            });
            const auto pm = PlanInterruptRewind(meta, true, sid);
            CHECK(pm.verdict == InterruptRewindVerdict::WouldNotContinue && pm.targetKind == L"meta" && pm.markerLine.empty(), "interrupt: a meta target is not offered");
            const auto local = Join({ UserPrompt("u1", "", "<local-command-stdout>ok</local-command-stdout>"), Partial("p1", "u1", "m1", false), InterruptRow("i1", "p1", false) });
            CHECK(PlanInterruptRewind(local, true, sid).targetKind == L"local command", "interrupt: a local command's echo is not offered");
            const auto denied = Join({
                UserPrompt("u1", "", "x"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                RejectedResult("x1", "a1", "toolu_1"),
                Partial("p1", "x1", "msg_2", false),
                InterruptRow("i1", "p1", false),
            });
            const auto pd = PlanInterruptRewind(denied, true, sid);
            CHECK(pd.verdict == InterruptRewindVerdict::WouldNotContinue && pd.targetKind == L"denied tool result", "interrupt: a denied tool result is not offered");
            const auto denialEnds = Join({
                UserPrompt("u1", "", "x"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                "{\"parentUuid\":\"a1\",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"content\":\"no\",\"tool_use_id\":\"toolu_1\"}]},\"toolUseResult\":{\"stdout\":\"no\"},\"toolDenialEndsTurn\":true,\"uuid\":\"r1\",\"sessionId\":\"x\"}",
                Partial("p1", "r1", "msg_2", false),
                InterruptRow("i1", "p1", false),
            });
            CHECK(PlanInterruptRewind(denialEnds, true, sid).verdict == InterruptRewindVerdict::WouldNotContinue, "interrupt: a toolDenialEndsTurn result is not offered");
        }
    }

    void TestResumeClassifier()
    {
        // e3: the source waits on its second request (a tool result, then a reminder attachment): a fork continues
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "e3 prompt"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                ToolResult("r1", "a1", "toolu_1", "one"),
                Attachment("g1", "r1", "total_tokens_reminder"),
                LastPrompt("g1", "e3 prompt"),
            });
            const auto c = ClassifyResumeContinuation(bytes, true);
            CHECK(c.verdict == ResumeContinuation::ContinuesTurn && c.tailKind == L"tool result" && c.droppedTool.empty(), "classify e3: a tool-result tail continues the turn");
        }
        // e3b: the source is INSIDE a Bash call (no result yet): the call is dropped and run again
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "e3b prompt"),
                Attachment("g1", "u1", "prompt_snapshot"),
                ToolCall("a1", "g1", "msg_1", "toolu_1", "Bash"),
            });
            const auto c = ClassifyResumeContinuation(bytes, true);
            CHECK(c.verdict == ResumeContinuation::ContinuesTurn && c.droppedTool == L"Bash", "classify e3b: a call in flight is dropped and its tool named");
            const auto thinking = Join({
                UserPrompt("u1", "", "think first"),
                "{\"parentUuid\":\"u1\",\"isSidechain\":false,\"message\":{\"id\":\"msg_1\",\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"x\"}]},\"type\":\"assistant\",\"uuid\":\"t1\",\"sessionId\":\"x\"}",
                ToolCall("a1", "t1", "msg_1", "toolu_1", "Edit"),
            });
            const auto ct = ClassifyResumeContinuation(thinking, true);
            CHECK(ct.verdict == ResumeContinuation::ResendsPrompt && ct.droppedTool == L"Edit" && ct.promptText == L"think first", "classify: a call's orphan thinking goes with it, and the prompt is resent");
            const auto text = Join({ UserPrompt("u1", "", "x"), Partial("p1", "u1", "msg_1", false), ToolCall("a1", "p1", "msg_1", "toolu_1", "Bash") });
            CHECK(ClassifyResumeContinuation(text, true).verdict == ResumeContinuation::None, "classify: a reply's text above a dropped call ends the turn");
        }
        // finished turns
        {
            const auto reply = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m1", "done"), TurnDuration("t1", "a1"), LastPrompt("t1") });
            const auto c = ClassifyResumeContinuation(reply, true);
            CHECK(c.verdict == ResumeContinuation::None && c.tailKind == L"reply", "classify: a finished reply");
            CHECK(ClassifyResumeContinuation(EscDuringReply(), true).verdict == ResumeContinuation::None, "classify: a user interrupt is a finished turn");
            const auto shutdown = Join({ UserPrompt("u1", "", "x"), Partial("p1", "u1", "m1", false), InterruptRow("i1", "p1", false, true) });
            CHECK(ClassifyResumeContinuation(shutdown, true).verdict == ResumeContinuation::ContinuesTurn, "classify: a shutdown interrupt continues");
            const auto meta = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m1", "ok"), MetaUser("m2", "a1", "some meta") });
            CHECK(ClassifyResumeContinuation(meta, true).verdict == ResumeContinuation::None, "classify: a meta tail waits");
            const auto metaUnderAttachment = meta + Join({ Attachment("g9", "m2", "date") });
            CHECK(ClassifyResumeContinuation(metaUnderAttachment, true).verdict == ResumeContinuation::ContinuesTurn, "classify: a meta row under an attachment tail continues (bfo's attachment branch)");
            const auto compact = Join({ "{\"parentUuid\":null,\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"summary\"},\"isCompactSummary\":true,\"uuid\":\"cs1\",\"sessionId\":\"x\"}" });
            CHECK(ClassifyResumeContinuation(compact, true).verdict == ResumeContinuation::None, "classify: a compaction summary tail waits");
            const auto denied = Join({ UserPrompt("u1", "", "x"), ToolUse("a1", "u1", "msg_1", "toolu_1"), RejectedResult("x1", "a1", "toolu_1") });
            CHECK(ClassifyResumeContinuation(denied, true).verdict == ResumeContinuation::None, "classify: a rejected tool result waits");
            const auto local = Join({ UserPrompt("u1", "", "<command-name>/model</command-name>") });
            CHECK(ClassifyResumeContinuation(local, true).verdict == ResumeContinuation::None, "classify: a local command waits");
        }
        // continued turns
        {
            const auto prompt = Join({ UserPrompt("u1", "", "Fix the bug\\nsecond line") }); // a JSON "\n" escape (one JSONL line)
            const auto c = ClassifyResumeContinuation(prompt, true);
            CHECK(c.verdict == ResumeContinuation::ResendsPrompt && c.promptText == L"Fix the bug", "classify: a bare prompt is resent (its first line kept)");
            const auto transient = Join({ UserPrompt("u1", "", "x"), ToolUse("a1", "u1", "m1", "toolu_1"), ToolResult("r1", "a1", "toolu_1"), ApiError("e1", "r1", "API Error: Overloaded", false) });
            CHECK(ClassifyResumeContinuation(transient, true).verdict == ResumeContinuation::ContinuesTurn, "classify: a transient API error tail is stepped over");
            const auto refused = Join({ UserPrompt("u1", "", "x"), ApiError("e1", "u1", "API Error: refused", false, "refusal") });
            CHECK(ClassifyResumeContinuation(refused, true).verdict == ResumeContinuation::None, "classify: a refusal is a finished turn");
        }
        // the loader's leaf rules
        {
            const auto rewound = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m1", "ok"), UserPrompt("u2", "a1", "y"), LastPromptRewound("u2") });
            const auto c = ClassifyResumeContinuation(rewound, true);
            CHECK(c.verdict == ResumeContinuation::None && c.tailKind == L"rewound", "classify: Claude Code's own rewind continues nothing");
            // a partial last line (a running session's file mid-write) is ignored
            auto midWrite = Join({ UserPrompt("u1", "", "x"), ToolUse("a1", "u1", "m1", "toolu_1"), ToolResult("r1", "a1", "toolu_1") });
            midWrite += "{\"parentUuid\":\"r1\",\"isSidechain\":false,\"type\":\"assist";
            CHECK(ClassifyResumeContinuation(midWrite, true).verdict == ResumeContinuation::ContinuesTurn, "classify: a partial last line is ignored");
            CHECK(ClassifyResumeContinuation({}, true).verdict == ResumeContinuation::None, "classify: an empty transcript");
        }
    }

    // A scoped process-environment override (restored on scope exit), for the resolution tests.
    struct EnvOverride
    {
        std::wstring name;
        std::wstring old;
        bool had{ false };
        EnvOverride(const wchar_t* n, const std::wstring& value) :
            name(n)
        {
            wchar_t buf[32768];
            const DWORD got = ::GetEnvironmentVariableW(n, buf, 32768);
            had = got > 0 && got < 32768;
            if (had)
            {
                old.assign(buf, got);
            }
            ::SetEnvironmentVariableW(n, value.empty() ? nullptr : value.c_str());
        }
        ~EnvOverride()
        {
            ::SetEnvironmentVariableW(name.c_str(), had ? old.c_str() : nullptr);
        }
    };

    void TestAutoContinueSetting()
    {
        CHECK(IsClaudeEnvTruthy(L"1") && IsClaudeEnvTruthy(L"true") && IsClaudeEnvTruthy(L"TRUE") && IsClaudeEnvTruthy(L" yes ") && IsClaudeEnvTruthy(L"On"), "switch: Claude Code's truthy words");
        CHECK(!IsClaudeEnvTruthy(L"0") && !IsClaudeEnvTruthy(L"false") && !IsClaudeEnvTruthy(L"") && !IsClaudeEnvTruthy(L"2") && !IsClaudeEnvTruthy(L"truee") && !IsClaudeEnvTruthy(L"yes please"), "switch: everything else is off");
        CHECK(ClassifyAutoContinueAtRestart(true, true) == AutoContinueAtRestart::On && ClassifyAutoContinueAtRestart(true, false) == AutoContinueAtRestart::On, "switch: on is on wherever it came from");
        CHECK(ClassifyAutoContinueAtRestart(false, false) == AutoContinueAtRestart::TurnedOnForThisRestart && ClassifyAutoContinueAtRestart(false, true) == AutoContinueAtRestart::OffInSettings, "switch: off in a settings file cannot be overridden");

        // The precedence, against a scratch config dir + project (the machine's managed policy, if it sets the key, decides
        // instead: then only the "managed decides" outcome is checked).
        const auto root = ScratchDir() / L"autocontinue";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / L"config", ec);
        std::filesystem::create_directories(root / L"project" / L".claude", ec);
        EnvOverride configDir(L"CLAUDE_CONFIG_DIR", (root / L"config").wstring());
        EnvOverride processEnv(L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN", L"");
        const std::wstring project = (root / L"project").wstring();
        const auto managed = ResolveClaudeAutoContinue(L"", {});
        if (managed.fromSettingsFile)
        {
            std::wprintf(L"  [note] a managed / user policy file sets the switch here (%s) - precedence checks skipped\n", managed.source.c_str());
            return;
        }
        CHECK(!managed.on && managed.source == L"nowhere", "switch: set nowhere = off");
        {
            EnvOverride on(L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN", L"1");
            const auto s = ResolveClaudeAutoContinue(project, {});
            CHECK(s.on && s.source == L"process env" && !s.fromSettingsFile, "switch: the process env");
        }
        const std::vector<std::pair<std::wstring, std::wstring>> spawnOff{ { L"claude_code_resume_interrupted_turn", L"0" } };
        {
            EnvOverride on(L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN", L"1");
            const auto s = ResolveClaudeAutoContinue(project, spawnOff);
            CHECK(!s.on && s.source == L"spawn env", "switch: the spawn env outranks the process env (names ignore case)");
        }
        WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"1\"}}");
        {
            const auto s = ResolveClaudeAutoContinue(project, spawnOff);
            CHECK(s.on && s.fromSettingsFile && s.source.find(L"settings.json") != std::wstring::npos, "switch: the user's settings file outranks the environment");
        }
        WriteBytes(root / L"project" / L".claude" / L"settings.json", "\xEF\xBB\xBF{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":false}}");
        {
            const auto s = ResolveClaudeAutoContinue(project, {});
            CHECK(!s.on && s.fromSettingsFile && s.value == L"false", "switch: the project file (with a BOM, a JSON boolean) outranks the user's");
        }
        WriteBytes(root / L"project" / L".claude" / L"settings.local.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"yes\"}}");
        {
            const auto s = ResolveClaudeAutoContinue(project, {});
            CHECK(s.on && s.source.find(L"settings.local.json") != std::wstring::npos, "switch: the local project file outranks the shared one");
        }
        WriteBytes(root / L"project" / L".claude" / L"settings.local.json", "{\"env\":{\"OTHER\":\"1\"}}");
        {
            const auto s = ResolveClaudeAutoContinue(project, {});
            CHECK(!s.on && s.source.find(L"settings.local.json") == std::wstring::npos, "switch: a file without the key does not decide");
        }

        // The session-level front doors against the scratch config dir: a transcript of a fork source in a running tool call.
        {
            const std::wstring source = L"5e551011-0000-4000-8000-0000000000b1";
            std::filesystem::create_directories(root / L"config" / L"projects" / L"K--x", ec);
            WriteBytes(root / L"config" / L"projects" / L"K--x" / (source + L".jsonl"),
                       Join({ UserPrompt("u1", "", "e3b prompt"), ToolCall("a1", "u1", "msg_1", "toolu_1", "Bash") }));
            WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"1\"}}");
            std::filesystem::remove(root / L"project" / L".claude" / L"settings.local.json", ec);
            std::filesystem::remove(root / L"project" / L".claude" / L"settings.json", ec);
            const auto c = ClassifyResumeContinuationForSession(source);
            CHECK(c.verdict == ResumeContinuation::ResendsPrompt && c.droppedTool == L"Bash", "fork outlook: the source's transcript is classified through the session id");
            const auto o = AssessForkContinuation(source, false, project, {});
            CHECK(o.ask && o.tail.droppedTool == L"Bash", "fork outlook: a source in a running call is asked about");
            WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"0\"}}");
            CHECK(!AssessForkContinuation(source, true, project, {}).ask, "fork outlook: with the switch off a fork continues nothing - never asked");
            WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"1\"}}");
            CHECK(!AssessForkContinuation(L"5e551011-0000-4000-8000-0000000000b2", true, project, {}).ask, "fork outlook: a source with no transcript forks fresh - never asked");
            const std::wstring finished = L"5e551011-0000-4000-8000-0000000000b3";
            WriteBytes(root / L"config" / L"projects" / L"K--x" / (finished + L".jsonl"), Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m1", "ok") }));
            CHECK(!AssessForkContinuation(finished, false, project, {}).ask, "fork outlook: a finished source at rest is not asked about");
            CHECK(AssessForkContinuation(finished, true, project, {}).ask, "fork outlook: a source the registry says runs is asked about (its tail changes every second)");

            // the restart's front doors: a session interrupted during a reply, planned and rewound through its id
            const std::wstring interrupted = L"5e551011-0000-4000-8000-0000000000b4";
            const auto path = root / L"config" / L"projects" / L"K--x" / (interrupted + L".jsonl");
            WriteBytes(path, EscDuringReply());
            const auto plan = PlanInterruptRewindForSession(interrupted);
            CHECK(plan.verdict == InterruptRewindVerdict::Rewind && plan.targetUuid == L"u0000001", "restart: the plan through the session id");
            const auto r = RewindInterruptTailForSession(interrupted);
            CHECK(r.outcome == ApiErrorRewindOutcome::Appended && ReadBytes(path) == EscDuringReply() + r.plan.markerLine + "\n", "restart: one line appended at the end, nothing else touched");
            CHECK(RewindInterruptTailForSession(interrupted).outcome == ApiErrorRewindOutcome::NotNeeded, "restart: a second rewind writes nothing");
            const auto line = DescribeInterruptRewind(r, interrupted, L"restart");
            CHECK(line.find(L"[interrupt-rewind] 5e551011 restart: took back") == 0 && line.find(L"sends the prompt again") != std::wstring::npos, "restart: the log line");
            CHECK(RewindInterruptTailForSession(L"5e551011-0000-4000-8000-0000000000ff").outcome == ApiErrorRewindOutcome::NotNeeded, "restart: no transcript needs nothing");
        }
        std::filesystem::remove_all(root, ec);
    }

    void TestForkTurnStopDecision()
    {
        ForkTurnStopFacts f;
        CHECK(DecideForkTurnStop(f, 1000) == ForkTurnStopAction::Wait, "stop watch: a dormant fork waits (unbounded)");
        f.connectionGone = true;
        CHECK(DecideForkTurnStop(f, 1000) == ForkTurnStopAction::Gone, "stop watch: a gone connection ends the watch");
        f = {};
        f.started = true;
        f.startedAtMs = 1000;
        CHECK(DecideForkTurnStop(f, 1100) == ForkTurnStopAction::Wait, "stop watch: started, nothing known yet");
        f.heartbeatKnown = true;
        f.turnRunning = true;
        CHECK(DecideForkTurnStop(f, 5900) == ForkTurnStopAction::PressEsc, "stop watch: busy -> Esc at once");
        f.presses = 1;
        f.lastPressMs = 5900;
        CHECK(DecideForkTurnStop(f, 6000) == ForkTurnStopAction::Wait, "stop watch: still busy right after the press -> wait");
        f.turnRunning = false;
        CHECK(DecideForkTurnStop(f, 6000) == ForkTurnStopAction::Stopped, "stop watch: idle after a press -> stopped");
        f.turnRunning = true;
        CHECK(DecideForkTurnStop(f, 5900 + kForkTurnStopVerifyMs) == ForkTurnStopAction::PressEsc, "stop watch: still busy after the verify window -> press again");
        f.presses = kForkTurnStopMaxPresses;
        CHECK(DecideForkTurnStop(f, 5900 + kForkTurnStopVerifyMs) == ForkTurnStopAction::GaveUp, "stop watch: presses spent -> give up");
        // no heartbeat: the registry stands in, believed only after the verify window
        f = {};
        f.started = true;
        f.startedAtMs = 1000;
        f.registryRunning = true;
        CHECK(DecideForkTurnStop(f, 2000) == ForkTurnStopAction::PressEsc, "stop watch: no heartbeat, the registry says Running -> Esc");
        f.presses = 1;
        f.lastPressMs = 2000;
        f.registryRunning = false;
        CHECK(DecideForkTurnStop(f, 2500) == ForkTurnStopAction::Wait && DecideForkTurnStop(f, 2000 + kForkTurnStopVerifyMs) == ForkTurnStopAction::Stopped, "stop watch: the late registry is believed after the verify window");
        // nothing to stop
        f = {};
        f.started = true;
        f.startedAtMs = 1000;
        f.heartbeatKnown = true;
        f.idleBoxSinceMs = 3000;
        CHECK(DecideForkTurnStop(f, 3000 + kForkTurnStopIdleSettleMs - 1) == ForkTurnStopAction::Wait && DecideForkTurnStop(f, 3000 + kForkTurnStopIdleSettleMs) == ForkTurnStopAction::NothingToStop, "stop watch: idle with the box up for the settle -> nothing to stop");
        f.idleBoxSinceMs = 0;
        CHECK(DecideForkTurnStop(f, 1000 + kForkTurnStopStartCapMs) == ForkTurnStopAction::NothingToStop, "stop watch: the start cap ends a watch that never saw a turn");
        CHECK(std::wstring{ ForkTurnStopActionName(ForkTurnStopAction::PressEsc) } == L"press-esc" && std::wstring{ ForkTurnChoiceName(ForkTurnChoice::Interrupted) } == L"interrupted" && std::wstring{ RestartTurnChoiceName(RestartTurnChoice::Uninterrupt) } == L"uninterrupt", "stop watch: the log words");
        CHECK(RestartTurnChoice{} == RestartTurnChoice::KeepInterrupted && ForkTurnChoice{} == ForkTurnChoice::Uninterrupted, "choices: zero is today's behavior");
    }

    void TestTurnQuestionTexts()
    {
        const auto r = UninterruptQuestionBody(L"Parser work", false, true, L"refactor the parser", AutoContinueAtRestart::On, L"");
        CHECK(r.find(L"\x201CParser work\x201D") != std::wstring::npos && r.find(L"refactor the parser") != std::wstring::npos && r.find(L"sent again") != std::wstring::npos, "texts: the restart question quotes the title and the prompt");
        CHECK(r.find(L"Keep it interrupted") != std::wstring::npos && r.find(L"Nothing is deleted") != std::wstring::npos, "texts: both answers are explained");
        const auto t = UninterruptQuestionBody(L"", true, false, L"", AutoContinueAtRestart::TurnedOnForThisRestart, L"");
        CHECK(t.find(L"this session") != std::wstring::npos && t.find(L"during a tool call") != std::wstring::npos && t.find(L"last step that finished") != std::wstring::npos && t.find(L"this one restart sets it") != std::wstring::npos, "texts: a tool-call interrupt, the switch set for the restart");
        const auto o = UninterruptQuestionBody(L"x", false, false, L"", AutoContinueAtRestart::OffInSettings, L"C:\\cfg\\settings.json");
        CHECK(o.find(L"is off (in C:\\cfg\\settings.json)") != std::wstring::npos && o.find(L"next message continues it") != std::wstring::npos, "texts: a switch off in settings says what happens instead");
        const auto f = ForkTurnQuestionBody(L"Build", false, L"Bash", true);
        CHECK(f.find(L"\x201C" L"Build\x201D is in the middle of a turn") == 0 && f.find(L"Bash call it was running runs again") != std::wstring::npos, "texts: the fork question names the re-run tool");
        CHECK(f.find(L"Fork interrupted") != std::wstring::npos && f.find(L"presses Esc") != std::wstring::npos && f.find(L"beside the original") != std::wstring::npos, "texts: both fork answers are explained");
        const auto g = ForkTurnQuestionBody(L"", true, L"", false);
        CHECK(g.find(L"This session stopped in the middle of a turn") == 0 && g.find(L"the last prompt is sent again") != std::wstring::npos, "texts: a turn left unfinished on disk");
    }
}

void TestInterruptedTurns()
{
    TestInterruptPlanner();
    TestResumeClassifier();
    TestAutoContinueSetting();
    TestForkTurnStopDecision();
    TestTurnQuestionTexts();
}
