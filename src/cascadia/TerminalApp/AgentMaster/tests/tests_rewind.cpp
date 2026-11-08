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
#include "../ConnectionLostRestart.h" // TestConnectionLostRestart — the automatic restart after a lost connection

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
            // ENV_VARS.md 8, v3 (2026-11-06: "Check if env var for auto resume is coming with agentmaster"): the shipped Global
            // env default reaches every managed spawn's env, so with no settings file of the user's the switch is on there (a
            // settings file that sets it still decides: the precedence checks below).
            const auto seeded = MergeSessionEnv(ApplyEnvDefaults(L"", 0).first, L"");
            const auto s = ResolveClaudeAutoContinue(project, seeded);
            CHECK(s.on && s.source == L"spawn env" && !s.fromSettingsFile, "switch: the shipped default (ENV_VARS.md 8, v3) turns it on through the spawn env");
        }
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

            // The RESTART outlook (the third question, 2026-11-06): the fork rule on the conversation the restart resumes — the
            // session's own transcript, else (a never-messaged fork, re-forked) its source's, else a fresh start.
            {
                const auto own = AssessRestartContinuation(source, L"", false, project, {});
                CHECK(own.ask && !own.reforks && own.conversationId == source && own.tail.droppedTool == L"Bash" && own.tail.verdict == ResumeContinuation::ResendsPrompt,
                      "restart outlook: a session whose tail is a call in flight is asked about (its own transcript, the call named)");
                CHECK(!AssessRestartContinuation(finished, L"", false, project, {}).ask && AssessRestartContinuation(finished, L"", true, project, {}).ask,
                      "restart outlook: a finished turn at rest is not asked about; a turn the registry says runs is (the restart's shutdown cuts it)");
                const std::wstring neverMessaged = L"5e551011-0000-4000-8000-0000000000b5"; // no transcript of its own
                const auto fresh = AssessRestartContinuation(neverMessaged, L"", true, project, {});
                CHECK(!fresh.ask && fresh.why.find(L"starts it fresh") != std::wstring::npos, "restart outlook: no transcript and no fork source -> a fresh start, never asked (even 'running')");
                const auto refork = AssessRestartContinuation(neverMessaged, source, false, project, {});
                CHECK(refork.ask && refork.reforks && refork.conversationId == source && refork.tail.droppedTool == L"Bash",
                      "restart outlook: a never-messaged fork re-forks -> its SOURCE's tail decides (a call in flight there: asked)");
                const auto reforkDone = AssessRestartContinuation(neverMessaged, finished, false, project, {});
                CHECK(!reforkDone.ask && reforkDone.reforks && reforkDone.why.find(L"fork source's turn is finished") != std::wstring::npos,
                      "restart outlook: a re-fork of a finished source at rest is not asked about, and says why");
                CHECK(AssessRestartContinuation(neverMessaged, finished, true, project, {}).ask, "restart outlook: ...unless the source's turn runs right now (the caller passes the source's state)");
                const auto orphan = AssessRestartContinuation(neverMessaged, L"5e551011-0000-4000-8000-0000000000b6", true, project, {});
                CHECK(!orphan.ask && orphan.why.find(L"neither the fork nor its source") != std::wstring::npos, "restart outlook: a fork whose source has no transcript either starts fresh - never asked");
                WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"0\"}}");
                const auto off = AssessRestartContinuation(source, L"", true, project, {});
                CHECK(!off.ask && off.why.find(L"does not continue a turn by itself") != std::wstring::npos, "restart outlook: with the switch off the restarted claude waits - never asked, even for a running turn");
                WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"1\"}}");
            }

            // SITUATION B (2026-11-06, the session stuck at "API Error: Server error mid-response. The response above may be
            // incomplete."): a turn that died on an API error after output began. The real shape: a tool result, a THINKING-ONLY
            // half-response, the error (server_error, truncatedAfterOutput) and its turn_duration row.
            {
                const std::wstring errored = L"5e551011-0000-4000-8000-0000000000b7";
                const auto erroredPath = root / L"config" / L"projects" / L"K--x" / (errored + L".jsonl");
                const std::string erroredTail = Join({
                    UserPrompt("u0000001", "", "Finish this"),
                    ToolUse("a0000001", "u0000001", "msg_1", "toolu_1"),
                    ToolResult("r0000001", "a0000001", "toolu_1"),
                    Partial("p0000001", "r0000001", "msg_2", true),
                    ApiError("e0000001", "p0000001", "API Error: Server error mid-response. The response above may be incomplete.", true),
                    TurnDuration("t0000001", "e0000001"),
                });
                WriteBytes(erroredPath, erroredTail);
                const auto tail = ClassifyResumeContinuationForSession(errored);
                CHECK(tail.verdict == ResumeContinuation::None && tail.tailKind == L"reply", "api error: the thinking-only half-response reads as a finished turn to the classifier (situation A says no)");
                CHECK(!AssessRestartContinuation(errored, L"", false, project, {}).ask && !AssessForkContinuation(errored, false, project, {}).ask,
                      "api error: without considerApiError (resumeApiErrorTurns off) neither question is asked");
                const auto r = AssessRestartContinuation(errored, L"", false, project, {}, true);
                CHECK(r.ask && r.apiError && !r.reforks && r.apiErrorPlan.verdict == ApiErrorRewindVerdict::Rewind && r.apiErrorPlan.targetKind == L"tool result" &&
                          r.apiErrorPlan.errorText.find(L"API Error: Server error mid-response") == 0 && r.apiErrorPlan.errorKind == L"server_error",
                      "api error: the restart offers the take-back (onto the tool result: it carries on), with the error quoted");
                const auto f = AssessForkContinuation(errored, false, project, {}, true);
                CHECK(f.ask && f.apiError && ApiErrorTakeBackContinues(f.apiErrorPlan), "api error: the fork offers the take-back too");
                WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"0\"}}");
                const auto offB = AssessRestartContinuation(errored, L"", false, project, {}, true);
                CHECK(offB.ask && offB.apiError && !offB.autoContinue.on && offB.autoContinue.fromSettingsFile && AssessForkContinuation(errored, false, project, {}, true).ask,
                      "api error: asked with the switch off too (the question then says the next message continues it)");
                WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"1\"}}");
                const auto refork = AssessRestartContinuation(L"5e551011-0000-4000-8000-0000000000b8", errored, false, project, {}, true);
                CHECK(refork.ask && refork.apiError && refork.reforks && refork.conversationId == errored, "api error: a never-messaged fork's re-fork offers its SOURCE's take-back");
                CHECK(!ApiErrorTakeBackContinues(ApiErrorRewindPlan{}), "api error: a plan that is not Rewind offers nothing");

                // The fork's take-back beside a LIVE holder: a heartbeat of THIS process (the harness) naming the conversation with
                // its exact creation stamp, so the holder check opens the pid and matches procStart as it does for a real claude.
                FILETIME created{}, exited{}, kernel{}, user{};
                ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
                const uint64_t procStart = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                std::string erroredNarrow;
                for (const wchar_t c : errored)
                {
                    erroredNarrow += static_cast<char>(c); // an ASCII GUID
                }
                std::filesystem::create_directories(root / L"config" / L"sessions", ec);
                const auto beat = root / L"config" / L"sessions" / (std::to_wstring(::GetCurrentProcessId()) + L".json");
                WriteBytes(beat, "{\"pid\":" + std::to_string(::GetCurrentProcessId()) + ",\"sessionId\":\"" + erroredNarrow + "\",\"procStart\":\"" + std::to_string(procStart) + "\",\"status\":\"idle\"}");
                CHECK(LiveClaudeProcessesForSession(errored).size() == 1, "fork take-back: the heartbeat makes this process a live holder of the source");
                const auto refused = RewindApiErrorTailForSession(errored);
                CHECK(refused.outcome == ApiErrorRewindOutcome::Skipped && refused.detail.find(L"still runs this conversation") != std::wstring::npos && ReadBytes(erroredPath) == erroredTail,
                      "a RESUME's rewind refuses beside a live holder (its exit would undo the line): nothing written");
                const auto taken = RewindApiErrorTailForForkSource(errored);
                CHECK(taken.outcome == ApiErrorRewindOutcome::Appended && ReadBytes(erroredPath) == erroredTail + taken.plan.markerLine + "\n" && taken.detail.find(L"still runs the source") != std::wstring::npos &&
                          taken.plan.targetUuid == L"r0000001",
                      "fork take-back: ONE line appended beside the live holder (aimed at the tool result), and the result names the holder");
                const auto line = DescribeApiErrorRewind(taken, errored, L"fork");
                CHECK(line.find(L"[api-error-rewind] 5e551011 fork: rewound past") == 0 && line.find(L"still runs the source") != std::wstring::npos, "fork take-back: the log line names the live source claude");
                std::filesystem::remove(beat, ec);
                CHECK(RewindApiErrorTailForForkSource(errored).outcome == ApiErrorRewindOutcome::NotNeeded && ReadBytes(erroredPath) == erroredTail + taken.plan.markerLine + "\n",
                      "fork take-back: a second one writes nothing (the error is taken back already)");
                const auto afterA = AssessForkContinuation(errored, false, project, {}, true);
                CHECK(afterA.ask && !afterA.apiError && afterA.tail.verdict == ResumeContinuation::ContinuesTurn,
                      "fork take-back: once taken back, the next fork's question is situation A (Claude Code continues the turn by itself)");
                CHECK(RewindApiErrorTailForForkSource(L"5e551011-0000-4000-8000-0000000000fe").outcome == ApiErrorRewindOutcome::NotNeeded, "fork take-back: a source with no transcript needs nothing");
            }

            // SITUATION C (2026-11-06: "a restart session (and fork session) that picks uninterrupted removes error and interrupt
            // so auto-resume works"): a fork of a source whose last turn the USER interrupted. A user interrupt is a FINISHED turn
            // to Claude Code's resume (situation A says no), so only the interactive seams' considerUserInterrupt asks — and the
            // take-back goes into the SOURCE's file, beside a live source claude, like B's.
            {
                const std::wstring userStopped = L"5e551011-0000-4000-8000-0000000000b9";
                const auto userStoppedPath = root / L"config" / L"projects" / L"K--x" / (userStopped + L".jsonl");
                WriteBytes(userStoppedPath, EscDuringReply());
                CHECK(!AssessForkContinuation(userStopped, false, project, {}, true).ask, "user interrupt: without considerUserInterrupt (a caller that never heard of C) the fork is not asked about");
                const auto c = AssessForkContinuation(userStopped, false, project, {}, true, true);
                CHECK(c.ask && c.userInterrupt && !c.apiError && c.interruptPlan.verdict == InterruptRewindVerdict::Rewind && c.interruptPlan.targetUuid == L"u0000001" &&
                          c.interruptPlan.targetKind == L"prompt" && !c.interruptPlan.duringToolUse,
                      "user interrupt: the fork offers the take-back of the user's interrupt (onto the prompt: it is sent again)");
                WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"0\"}}");
                const auto offC = AssessForkContinuation(userStopped, false, project, {}, true, true);
                CHECK(offC.ask && offC.userInterrupt && !offC.autoContinue.on && offC.autoContinue.fromSettingsFile,
                      "user interrupt: asked with the switch off too (the question then says the next message continues it)");
                WriteBytes(root / L"config" / L"settings.json", "{\"env\":{\"CLAUDE_CODE_RESUME_INTERRUPTED_TURN\":\"1\"}}");

                // The take-back beside a LIVE source claude (this process, as above): a resume's rewind refuses, the fork's appends.
                FILETIME created{}, exited{}, kernel{}, user{};
                ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
                const uint64_t procStart = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                std::string sidNarrow;
                for (const wchar_t ch : userStopped)
                {
                    sidNarrow += static_cast<char>(ch); // an ASCII GUID
                }
                std::filesystem::create_directories(root / L"config" / L"sessions", ec);
                const auto beat = root / L"config" / L"sessions" / (std::to_wstring(::GetCurrentProcessId()) + L".json");
                WriteBytes(beat, "{\"pid\":" + std::to_string(::GetCurrentProcessId()) + ",\"sessionId\":\"" + sidNarrow + "\",\"procStart\":\"" + std::to_string(procStart) + "\",\"status\":\"idle\"}");
                const auto refused = RewindInterruptTailForSession(userStopped);
                CHECK(refused.outcome == ApiErrorRewindOutcome::Skipped && ReadBytes(userStoppedPath) == EscDuringReply(),
                      "user interrupt: a RESUME's interrupt rewind refuses beside a live holder (its exit would undo the line): nothing written");
                const auto taken = RewindInterruptTailForForkSource(userStopped);
                CHECK(taken.outcome == ApiErrorRewindOutcome::Appended && ReadBytes(userStoppedPath) == EscDuringReply() + taken.plan.markerLine + "\n" &&
                          taken.plan.targetUuid == L"u0000001" && taken.detail.find(L"still runs the source") != std::wstring::npos,
                      "fork take-back of a user interrupt: ONE line appended beside the live holder (aimed at the prompt), and the result names the holder");
                const auto line = DescribeInterruptRewind(taken, userStopped, L"fork");
                CHECK(line.find(L"[interrupt-rewind] 5e551011 fork: took back") == 0 && line.find(L"still runs the source") != std::wstring::npos &&
                          line.find(L"the fork sends the prompt again") != std::wstring::npos && line.find(L"restarted claude") == std::wstring::npos,
                      "fork take-back of a user interrupt: the log line names the live source claude, and says the FORK goes on (not a restarted claude)");
                std::filesystem::remove(beat, ec);
                CHECK(RewindInterruptTailForForkSource(userStopped).outcome == ApiErrorRewindOutcome::NotNeeded && ReadBytes(userStoppedPath) == EscDuringReply() + taken.plan.markerLine + "\n",
                      "fork take-back of a user interrupt: a second one writes nothing (taken back already)");
                const auto afterC = AssessForkContinuation(userStopped, false, project, {}, true, true);
                CHECK(afterC.ask && !afterC.userInterrupt && !afterC.apiError && afterC.tail.verdict == ResumeContinuation::ResendsPrompt,
                      "fork take-back of a user interrupt: once taken back, the next fork's question is situation A (Claude Code sends the prompt again by itself)");
                CHECK(RewindInterruptTailForForkSource(L"5e551011-0000-4000-8000-0000000000fd").outcome == ApiErrorRewindOutcome::NotNeeded, "fork take-back of a user interrupt: a source with no transcript needs nothing");
            }

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
        // The third question's two answers (2026-11-06): appended after Ask, so 0 / 1 / 2 keep their meaning, and logged by name.
        CHECK(static_cast<int32_t>(RestartTurnChoice::StopContinuedTurn) == 3 && static_cast<int32_t>(RestartTurnChoice::LetTurnContinue) == 4 &&
                  std::wstring{ RestartTurnChoiceName(RestartTurnChoice::StopContinuedTurn) } == L"stop-continued-turn" && std::wstring{ RestartTurnChoiceName(RestartTurnChoice::LetTurnContinue) } == L"let-turn-continue" &&
                  std::wstring{ RestartTurnChoiceName(static_cast<RestartTurnChoice>(9)) } == L"unknown",
              "choices: the restart's interrupted / uninterrupted answers come after Ask, with their own log words");
        // The fork's take-back answers (2026-11-06, situations B and C): appended after Ask too.
        CHECK(static_cast<int32_t>(ForkTurnChoice::TakeBackAndContinue) == 3 && std::wstring{ ForkTurnChoiceName(ForkTurnChoice::TakeBackAndContinue) } == L"take-back-and-continue" &&
                  static_cast<int32_t>(ForkTurnChoice::TakeBackAndInterrupt) == 4 && std::wstring{ ForkTurnChoiceName(ForkTurnChoice::TakeBackAndInterrupt) } == L"take-back-and-interrupt" &&
                  std::wstring{ ForkTurnChoiceName(static_cast<ForkTurnChoice>(9)) } == L"unknown",
              "choices: the fork's take-back answers come after Ask, with their own log words");
        // What each fork answer does: the take-back (B and C's "uninterrupted", B's "interrupted") and the stop watch (A's and
        // B's "interrupted") — the user's rule "interrupted keeps but replaces error with regular interrupt" is the one answer
        // that does both.
        CHECK(!ForkTurnTakesBack(ForkTurnChoice::Uninterrupted) && !ForkTurnTakesBack(ForkTurnChoice::Interrupted) && !ForkTurnTakesBack(ForkTurnChoice::Ask) &&
                  ForkTurnTakesBack(ForkTurnChoice::TakeBackAndContinue) && ForkTurnTakesBack(ForkTurnChoice::TakeBackAndInterrupt),
              "choices: only the two take-back answers take the source's stop back");
        CHECK(!ForkTurnArmsStopWatch(ForkTurnChoice::Uninterrupted) && ForkTurnArmsStopWatch(ForkTurnChoice::Interrupted) && !ForkTurnArmsStopWatch(ForkTurnChoice::Ask) &&
                  !ForkTurnArmsStopWatch(ForkTurnChoice::TakeBackAndContinue) && ForkTurnArmsStopWatch(ForkTurnChoice::TakeBackAndInterrupt),
              "choices: Interrupted and TakeBackAndInterrupt arm the stop watch, nothing else");
        // The pass-throughs (another window's request, the Telegram bot's launch) carry every decided answer and fork a stray Ask
        // or an unknown value as before.
        CHECK(ForkTurnChoiceForLaunch(ForkTurnChoice::Interrupted) == ForkTurnChoice::Interrupted && ForkTurnChoiceForLaunch(ForkTurnChoice::TakeBackAndContinue) == ForkTurnChoice::TakeBackAndContinue &&
                  ForkTurnChoiceForLaunch(ForkTurnChoice::TakeBackAndInterrupt) == ForkTurnChoice::TakeBackAndInterrupt && ForkTurnChoiceForLaunch(ForkTurnChoice::Uninterrupted) == ForkTurnChoice::Uninterrupted &&
                  ForkTurnChoiceForLaunch(ForkTurnChoice::Ask) == ForkTurnChoice::Uninterrupted && ForkTurnChoiceForLaunch(static_cast<ForkTurnChoice>(9)) == ForkTurnChoice::Uninterrupted,
              "choices: a pass-through carries every decided answer; Ask and unknown values fork as before");
        // The [nav] done line's words: the old " (fork interrupted)" kept for Interrupted, the take-backs named, a plain fork bare.
        CHECK(ForkTurnNavSuffix(ForkTurnChoice::Interrupted, false) == L" (fork interrupted)" && ForkTurnNavSuffix(ForkTurnChoice::Uninterrupted, false).empty() &&
                  ForkTurnNavSuffix(ForkTurnChoice::TakeBackAndContinue, true).find(L"fork uninterrupted, after the take-back") != std::wstring::npos &&
                  ForkTurnNavSuffix(ForkTurnChoice::TakeBackAndInterrupt, true).find(L"a regular interrupt replaces it") != std::wstring::npos &&
                  ForkTurnNavSuffix(ForkTurnChoice::TakeBackAndContinue, false).find(L"nothing to take back") != std::wstring::npos,
              "choices: the fork's [nav] words");
    }

    void TestTurnQuestionTexts()
    {
        const auto r = UninterruptQuestionBody(L"Parser work", false, true, L"refactor the parser", AutoContinueAtRestart::On, L"");
        CHECK(r.find(L"\x201CParser work\x201D") != std::wstring::npos && r.find(L"refactor the parser") != std::wstring::npos && r.find(L"sent again") != std::wstring::npos, "texts: the restart question quotes the title and the prompt");
        CHECK(r.find(L"Restart uninterrupted: before the restart") != std::wstring::npos && r.find(L"Restart interrupted: the restarted session waits for you") != std::wstring::npos &&
                  r.find(L"Nothing is deleted") != std::wstring::npos && r.find(L"Keep it interrupted") == std::wstring::npos && r.find(L"Continue the turn") == std::wstring::npos,
              "texts: both answers are explained, by the buttons' names (the words of every restart and fork question since 2026-11-06)");
        const auto t = UninterruptQuestionBody(L"", true, false, L"", AutoContinueAtRestart::TurnedOnForThisRestart, L"");
        CHECK(t.find(L"this session") != std::wstring::npos && t.find(L"during a tool call") != std::wstring::npos && t.find(L"last step that finished") != std::wstring::npos && t.find(L"this one restart sets it") != std::wstring::npos, "texts: a tool-call interrupt, the switch set for the restart");
        const auto o = UninterruptQuestionBody(L"x", false, false, L"", AutoContinueAtRestart::OffInSettings, L"C:\\cfg\\settings.json");
        CHECK(o.find(L"is off (in C:\\cfg\\settings.json)") != std::wstring::npos && o.find(L"next message continues it") != std::wstring::npos, "texts: a switch off in settings says what happens instead");
        const auto f = ForkTurnQuestionBody(L"Build", false, L"Bash", true);
        CHECK(f.find(L"\x201C" L"Build\x201D is in the middle of a turn") == 0 && f.find(L"Bash call it was running runs again") != std::wstring::npos, "texts: the fork question names the re-run tool");
        CHECK(f.find(L"Fork interrupted") != std::wstring::npos && f.find(L"presses Esc") != std::wstring::npos && f.find(L"beside the original") != std::wstring::npos, "texts: both fork answers are explained");
        const auto g = ForkTurnQuestionBody(L"", true, L"", false);
        CHECK(g.find(L"This session stopped in the middle of a turn") == 0 && g.find(L"the last prompt is sent again") != std::wstring::npos, "texts: a turn left unfinished on disk");
        // The third question (2026-11-06): the restart of a turn that would go on, in the fork question's words.
        const auto h = RestartTurnQuestionBody(L"Build", false, L"Bash", true);
        CHECK(h.find(L"\x201C" L"Build\x201D is in the middle of a turn") == 0 && h.find(L"resumes the conversation in a new Claude") != std::wstring::npos &&
                  h.find(L"Bash call it was running runs again") != std::wstring::npos,
              "texts: the restart question names the session, what a restart does, and the re-run tool");
        CHECK(h.find(L"Restart interrupted") != std::wstring::npos && h.find(L"presses Esc") != std::wstring::npos && h.find(L"Restart uninterrupted") != std::wstring::npos &&
                  h.find(L"carries on with the turn") != std::wstring::npos,
              "texts: both restart answers are explained, by the buttons' names");
        const auto i = RestartTurnQuestionBody(L"", true, L"", false);
        CHECK(i.find(L"This session stopped in the middle of a turn") == 0 && i.find(L"the last prompt is sent again") != std::wstring::npos && i.find(L"\x2014 the ") != std::wstring::npos,
              "texts: a restart of a turn left unfinished on disk that resends its prompt");
        const auto j = RestartTurnQuestionBody(L"x", false, L"", true);
        CHECK(j.find(L"\x2014") == std::wstring::npos, "texts: with no tool and no prompt to resend, no dash clause");
        // Situation B (2026-11-06): a turn that died on an API error, for the restart and for the fork.
        const auto k = ApiErrorTurnQuestionBody(false, L"open bugs", L"API Error: Server error mid-response.", false, AutoContinueAtRestart::On, L"");
        CHECK(k.find(L"The last turn of \x201Copen bugs\x201D stopped on an API error: \x201C" L"API Error: Server error mid-response.\x201D") == 0 && k.find(L"Restart uninterrupted: Agentmaster takes the error back") != std::wstring::npos &&
                  k.find(L"continues the turn by itself") != std::wstring::npos && k.find(L"from the last step that finished") != std::wstring::npos &&
                  k.find(L"Restart interrupted: Agentmaster takes the error back too, then presses Esc") != std::wstring::npos && k.find(L"a regular interrupt in place of the error") != std::wstring::npos &&
                  k.find(L"waits at the error for you") == std::wstring::npos,
              "texts: the restart's API-error question quotes the error and explains both answers - interrupted takes the error back too and replaces it with a regular interrupt");
        const auto l = ApiErrorTurnQuestionBody(true, L"", L"", true, AutoContinueAtRestart::TurnedOnForThisRestart, L"");
        CHECK(l.find(L"The last turn of this session stopped on an API error.") == 0 && l.find(L"Fork uninterrupted") != std::wstring::npos && l.find(L"the source stays where it is") != std::wstring::npos &&
                  l.find(L"your prompt is sent again") != std::wstring::npos && l.find(L"this one fork sets it") != std::wstring::npos &&
                  l.find(L"Fork interrupted: Agentmaster takes the error back for the fork too, then locks the fork's tab and presses Esc") != std::wstring::npos,
              "texts: the fork's API-error question - no title, no error text, the prompt resent, the switch set for this fork, interrupted = take-back + Esc");
        const auto m = ApiErrorTurnQuestionBody(false, L"x", L"e", false, AutoContinueAtRestart::OffInSettings, L"C:\\cfg\\settings.json");
        CHECK(m.find(L"is off (in C:\\cfg\\settings.json)") != std::wstring::npos && m.find(L"your next message continues it") != std::wstring::npos && m.find(L"by itself") == std::wstring::npos &&
                  m.find(L"Restart interrupted: Agentmaster takes the error back too, and the restarted Claude waits for you.") != std::wstring::npos && m.find(L"presses Esc") == std::wstring::npos,
              "texts: a switch off in settings says what happens instead of promising an automatic continuation (no Esc: nothing starts to stop)");
        // Situation C (2026-11-06): the fork of a source whose turn the USER interrupted.
        const auto n = ForkInterruptQuestionBody(L"Parser work", false, true, L"refactor the parser", AutoContinueAtRestart::On, L"");
        CHECK(n.find(L"You interrupted \x201CParser work\x201D in the middle of a reply.") == 0 && n.find(L"would wait at your interrupt too") != std::wstring::npos &&
                  n.find(L"Fork uninterrupted: Agentmaster takes the interrupt back for the fork") != std::wstring::npos && n.find(L"the source stays where it is") != std::wstring::npos &&
                  n.find(L"your prompt \x201Crefactor the parser\x201D is sent again") != std::wstring::npos && n.find(L"Fork interrupted: the fork waits for you, interrupted like its source.") != std::wstring::npos,
              "texts: the fork's user-interrupt question quotes the title and the prompt and explains both answers");
        const auto p = ForkInterruptQuestionBody(L"", true, false, L"", AutoContinueAtRestart::TurnedOnForThisRestart, L"");
        CHECK(p.find(L"You interrupted this session during a tool call.") == 0 && p.find(L"from the last step that finished") != std::wstring::npos && p.find(L"this one fork sets it") != std::wstring::npos,
              "texts: a tool-call interrupt, the switch set for this fork");
        const auto q = ForkInterruptQuestionBody(L"x", false, false, L"", AutoContinueAtRestart::OffInSettings, L"C:\\cfg\\settings.json");
        CHECK(q.find(L"is off (in C:\\cfg\\settings.json)") != std::wstring::npos && q.find(L"your next message there continues it") != std::wstring::npos && q.find(L"by itself") == std::wstring::npos,
              "texts: the user-interrupt fork question with the switch off in settings promises nothing automatic");
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

// ===== the AUTOMATIC RESTART after a lost connection (ConnectionLostRestart.h; HOOKS.md "Connection lost mid-response — the
// automatic restart"; the user's request of 2026-11-08) =====
namespace
{
    // A system row Claude Code writes after a turn (its away_summary minutes later, …), parented like its other bookkeeping.
    std::string SystemRow(const std::string& uuid, const std::string& parent, const std::string& subtype)
    {
        return "{\"parentUuid\":" + Parent(parent) + ",\"isSidechain\":false,\"type\":\"system\",\"subtype\":" + Q(subtype) + ",\"isMeta\":false" + Common(uuid, "2026-11-08T09:12:02.775Z") + "}";
    }

    // The 2.1.310 shape of the user's session d42f0aea (2026-11-08, the session that prompted the feature; dummy text): a tool
    // result, the NON-explicit last-prompt claude wrote before the reply, the cut half-response (thinking + text, one message
    // id), the error row with its streamCutCause, the turn_duration and the away_summary written minutes later.
    std::string ConnectionLostTail310()
    {
        return Join({
            UserPrompt("u0000001", "", "dedupe the eateries"),
            ToolUse("a0000001", "u0000001", "msg_1", "toolu_1"),
            ToolResult("r0000001", "a0000001", "toolu_1"),
            LastPrompt("r0000001", "dedupe the eateries"),
            Partial("p0000001", "r0000001", "msg_2", true),
            Partial("p0000002", "p0000001", "msg_2", false),
            ApiError("e0000001", "p0000002", "API Error: Connection lost mid-response. The response above may be incomplete.", true, "stop_sequence", ",\"streamCutCause\":\"stale_connection\""),
            TurnDuration("t0000001", "e0000001"),
            SystemRow("s0000001", "t0000001", "away_summary"),
        });
    }

    // The facts of a session the registry gate restarts: every other case below changes ONE of them.
    ConnectionLostRestartFacts GoFacts()
    {
        ConnectionLostRestartFacts f;
        f.settingOn = true;
        f.takeBackOn = true;
        f.isClaude = true;
        f.live = true;
        f.hostedHere = true;
        f.started = true;
        f.external = false;
        f.state = SessionState::Error;
        f.errorDismissed = false;
        f.errorMessage = L"API Error: Connection lost mid-response. The response above may be incomplete."; // a literal: the view never dangles
        return f;
    }

    void TestConnectionLostText()
    {
        CHECK(IsConnectionLostMidResponseError(L"API Error: Connection lost mid-response. The response above may be incomplete."), "conn-lost text: Claude Code's own row text matches");
        CHECK(IsConnectionLostMidResponseError(L"api error: CONNECTION LOST MID-RESPONSE."), "conn-lost text: the match ignores ASCII case");
        CHECK(IsConnectionLostMidResponseError(L"\x05E9\x05DC Connection lost mid-response"), "conn-lost text: a non-ASCII prefix does not hide the words");
        CHECK(IsConnectionLostMidResponseError(L"Connection lost mid-response"), "conn-lost text: the words alone match (at the very start and end of the text)");
        CHECK(!IsConnectionLostMidResponseError(L"API Error: Server error mid-response. The response above may be incomplete."), "conn-lost text: a server error mid-response is another error");
        CHECK(!IsConnectionLostMidResponseError(L"API Error: Connection lost before a response was produced. Try again."), "conn-lost text: the before-any-output variant is not matched (any resume continues it)");
        CHECK(!IsConnectionLostMidResponseError(L"API Error: The response stopped arriving. The response above may be incomplete."), "conn-lost text: a stalled stream is another error");
        CHECK(!IsConnectionLostMidResponseError(L""), "conn-lost text: an empty message never matches");
        CHECK(!IsConnectionLostMidResponseError(L"Connection lost mid-respons"), "conn-lost text: a text shorter than the words never matches");
    }

    void TestConnectionLostRegistryGate()
    {
        using G = ConnectionLostRestartGate;
        const auto go = DecideConnectionLostRestart(GoFacts());
        CHECK(go.gate == G::Go && go.reason.empty(), "gate: a started Claude session hosted here, in an unacknowledged lost-connection Error, restarts");
        // Ignore: not this feature's business (nothing is logged for these).
        {
            auto f = GoFacts();
            f.isClaude = false;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: a Codex session is ignored");
        }
        {
            auto f = GoFacts();
            f.live = false;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: an archived session is ignored");
        }
        {
            auto f = GoFacts();
            f.hostedHere = false;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: a session another window hosts is that window's to restart");
        }
        {
            auto f = GoFacts();
            f.state = SessionState::Running;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: a session that runs again (a retry) is ignored");
            f.state = SessionState::WaitingForInput;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: a session out of Error (a rewind) is ignored");
        }
        {
            auto f = GoFacts();
            f.errorDismissed = true;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: an error the user acknowledged (Move to Idle/Done) is ignored");
        }
        {
            auto f = GoFacts();
            f.errorMessage = L"API Error: Server error mid-response. The response above may be incomplete.";
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: another API error is ignored");
            f.errorMessage = L"The last turn stopped before it finished.";
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: a cut-off turn's Error is ignored");
        }
        {
            auto f = GoFacts();
            f.started = false;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate: a dormant tab is ignored (its own start takes the error back)");
        }
        // Decline: a lost connection this window will not restart (logged once per episode).
        {
            auto f = GoFacts();
            f.settingOn = false;
            const auto r = DecideConnectionLostRestart(f);
            CHECK(r.gate == G::Decline && r.reason.find(L"Settings") != std::wstring_view::npos, "gate: the switch off declines, naming the setting");
        }
        {
            auto f = GoFacts();
            f.external = true;
            const auto r = DecideConnectionLostRestart(f);
            CHECK(r.gate == G::Decline && r.reason.find(L"adopted") != std::wstring_view::npos, "gate: an adopted session declines (its ConPTY is the user's shell)");
        }
        {
            auto f = GoFacts();
            f.takeBackOn = false;
            CHECK(DecideConnectionLostRestart(f).gate == G::Decline, "gate: without the API-error take-back a restart heals nothing - declined");
        }
        {
            auto f = GoFacts();
            f.stopIntent = true;
            const auto r = DecideConnectionLostRestart(f);
            CHECK(r.gate == G::Decline && r.reason.find(L"interrupted") != std::wstring_view::npos,
                  "gate: a recorded stop intent (Fork / Restart interrupted) declines - \"uninterrupted\" is the answer the user did not give");
            f.draftInBox = true;
            CHECK(DecideConnectionLostRestart(f).gate == G::Decline, "gate order: a stop intent stays a decline whatever holds");
        }
        // Hold: restarted once the reason passes.
        {
            auto f = GoFacts();
            f.draftInBox = true;
            const auto r = DecideConnectionLostRestart(f);
            CHECK(r.gate == G::Hold && r.reason.find(L"draft") != std::wstring_view::npos, "gate: an unsent draft holds the restart (it would wipe the draft)");
        }
        {
            auto f = GoFacts();
            f.boxBusy = true;
            CHECK(DecideConnectionLostRestart(f).gate == G::Hold, "gate: a delivery / draft fill / stop watch owning the box holds the restart");
        }
        {
            auto f = GoFacts();
            f.backgroundWork = true;
            CHECK(DecideConnectionLostRestart(f).gate == G::Hold, "gate: background work (a shell, an agent) holds the restart");
        }
        // The order: Ignore before Decline before Hold.
        {
            auto f = GoFacts();
            f.settingOn = false;
            f.draftInBox = true;
            CHECK(DecideConnectionLostRestart(f).gate == G::Decline, "gate order: a declined restart stays declined whatever holds");
        }
        {
            auto f = GoFacts();
            f.state = SessionState::Idle;
            f.settingOn = false;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate order: a session out of Error is ignored, never declined");
        }
        {
            auto f = GoFacts();
            f.started = false;
            f.external = true;
            f.settingOn = false;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate order: a dormant tab is ignored before any decline");
        }
        {
            auto f = GoFacts();
            f.errorMessage = L"API Error: Overloaded";
            f.draftInBox = true;
            CHECK(DecideConnectionLostRestart(f).gate == G::Ignore, "gate order: another error never holds");
        }
    }

    void TestConnectionLostTranscriptGate()
    {
        const std::wstring sid = L"5e551011-0000-4000-8000-000000000001";
        const auto plan = PlanApiErrorRewind(ConnectionLostTail310(), true, sid);
        CHECK(plan.verdict == ApiErrorRewindVerdict::Rewind && plan.targetUuid == L"r0000001" && plan.targetKind == L"tool result" && plan.partialRows == 2 && plan.errorUuid == L"e0000001",
              "transcript gate: the 2.1.310 shape (streamCutCause, a non-explicit marker before the reply, an away_summary) plans the take-back onto the tool result");
        const auto go = DecideConnectionLostRestartFromPlan(plan, L"", 0);
        CHECK(go.verdict == ConnectionLostPlanVerdict::Restart && go.reason.empty(), "transcript gate: a new error row with budget left restarts");
        CHECK(DecideConnectionLostRestartFromPlan(plan, L"e0000001", 1).verdict == ConnectionLostPlanVerdict::AlreadyRestarted,
              "transcript gate: the error row a restart was already made for is never restarted again (that restart did not take it back)");
        CHECK(DecideConnectionLostRestartFromPlan(plan, L"e0000000", kConnectionLostRestartBudget - 1).verdict == ConnectionLostPlanVerdict::Restart,
              "transcript gate: another error row restarts while under the budget");
        const auto over = DecideConnectionLostRestartFromPlan(plan, L"e0000000", kConnectionLostRestartBudget);
        CHECK(over.verdict == ConnectionLostPlanVerdict::OverBudget && over.reason.find(L"30 minutes") != std::wstring::npos, "transcript gate: the budget caps the restarts and says over which time");
        // Already taken back (the line is in the file): nothing left to heal.
        const auto rewound = PlanApiErrorRewind(ConnectionLostTail310() + plan.markerLine + "\n", true, sid);
        CHECK(DecideConnectionLostRestartFromPlan(rewound, L"", 0).verdict == ConnectionLostPlanVerdict::NothingToTakeBack, "transcript gate: a transcript already taken back has nothing to heal");
        // Another error at the active leaf.
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "go"),
                ToolUse("a1", "u1", "msg_1", "toolu_1"),
                ToolResult("r1", "a1", "toolu_1"),
                Partial("p1", "r1", "msg_2", false),
                ApiError("e1", "p1", "API Error: Server error mid-response. The response above may be incomplete.", true, "stop_sequence", ",\"streamCutCause\":\"server_error\""),
                TurnDuration("t1", "e1"),
            });
            const auto r = DecideConnectionLostRestartFromPlan(PlanApiErrorRewind(bytes, true, sid), L"", 0);
            CHECK(r.verdict == ConnectionLostPlanVerdict::NotThisError && r.reason.find(L"Server error") != std::wstring::npos, "transcript gate: the transcript's error is another one - not restarted, and the log quotes it");
        }
        // The planner leaves the error alone: a refusal, a torn last line, two responses above the error.
        {
            const auto refusal = Join({ UserPrompt("u1", "", "x"), Partial("p1", "u1", "msg_1", false), ApiError("e1", "p1", "API Error: Connection lost mid-response.", true, "refusal"), TurnDuration("t1", "e1") });
            CHECK(DecideConnectionLostRestartFromPlan(PlanApiErrorRewind(refusal, true, sid), L"", 0).verdict == ConnectionLostPlanVerdict::NotTakenBack, "transcript gate: a refused response is never taken back, so never restarted");
            auto torn = ConnectionLostTail310();
            torn.pop_back();
            CHECK(DecideConnectionLostRestartFromPlan(PlanApiErrorRewind(torn, true, sid), L"", 0).verdict == ConnectionLostPlanVerdict::NotTakenBack, "transcript gate: a torn last line is not restarted");
            const auto two = Join({
                UserPrompt("u1", "", "x"),
                Reply("a1", "u1", "msg_A", "complete answer"),
                Partial("p1", "a1", "msg_B", false),
                ApiError("e1", "p1", "API Error: Connection lost mid-response.", true),
                TurnDuration("t1", "e1"),
            });
            const auto r = DecideConnectionLostRestartFromPlan(PlanApiErrorRewind(two, true, sid), L"", 0);
            CHECK(r.verdict == ConnectionLostPlanVerdict::NotTakenBack && !r.reason.empty(), "transcript gate: an unknown shape is not restarted, and the planner's reason is logged");
        }
        // The take-back lands on a row Claude Code would not continue by itself (a slash command's skill text).
        {
            const auto bytes = Join({
                UserPrompt("u1", "", "<command-name>/np-function</command-name>"),
                MetaUser("m1", "u1", "When user requests /np-function, follow these instructions"),
                Partial("p1", "m1", "msg_1", false),
                ApiError("e1", "p1", "API Error: Connection lost mid-response. The response above may be incomplete.", true),
                TurnDuration("t1", "e1"),
            });
            const auto r = DecideConnectionLostRestartFromPlan(PlanApiErrorRewind(bytes, true, sid), L"", 0);
            CHECK(r.verdict == ConnectionLostPlanVerdict::WouldNotContinue && r.reason.find(L"meta") != std::wstring::npos, "transcript gate: a meta target would not continue - not restarted");
        }
        // A healthy transcript (the race where the user retried meanwhile).
        {
            const auto healthy = Join({ UserPrompt("u1", "", "x"), Reply("a1", "u1", "m", "ok"), TurnDuration("t1", "a1") });
            CHECK(DecideConnectionLostRestartFromPlan(PlanApiErrorRewind(healthy, true, sid), L"", 0).verdict == ConnectionLostPlanVerdict::NothingToTakeBack, "transcript gate: no error at the leaf - nothing to heal");
        }
        CHECK(std::wstring{ ConnectionLostPlanVerdictName(ConnectionLostPlanVerdict::Restart) } == L"restart" &&
                  std::wstring{ ConnectionLostPlanVerdictName(ConnectionLostPlanVerdict::OverBudget) } == L"over-budget" &&
                  std::wstring{ ConnectionLostPlanVerdictName(static_cast<ConnectionLostPlanVerdict>(99)) } == L"unknown",
              "transcript gate: the verdicts' log words (a stray value reads unknown)");
    }

    void TestConnectionLostOutcome()
    {
        using O = ConnectionLostRestartOutcome;
        CHECK(DecideConnectionLostRestartOutcome(false, SessionState::Running, true, 0) == O::Gone, "outcome: a closed session is gone, whatever its state");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Running, false, 4000) == O::Healed, "outcome: the turn runs again - healed");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::NeedsApproval, false, 9000) == O::Healed, "outcome: the continuation asks for a permission - healed too");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Error, false, 5000) == O::Waiting,
              "outcome: Error again during the held start (the error stays the leaf until the take-back line is written) is not a failure");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Idle, false, kConnectionLostRestartConfirmMs - 1) == O::Waiting, "outcome: still waiting a millisecond before the confirm window ends");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Idle, false, kConnectionLostRestartConfirmMs) == O::NotHealed, "outcome: the confirm window passed without the turn going on - not healed");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Error, false, kConnectionLostRestartConfirmMs) == O::NotHealed, "outcome: still in Error after the window - not healed");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::WaitingForInput, false, 3 * kConnectionLostRestartConfirmMs) == O::NotHealed,
              "outcome: the error taken back but nothing continued (a settings file turned the switch off) - not healed, the user is told");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Done, false, -5) == O::Waiting, "outcome: a negative clock reads as no time passed");
        // A turn the new claude ENDED after the restart (a real Stop / a terminal reply line).
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::WaitingForInput, true, 3000) == O::Healed,
              "outcome: a continuation answered so fast that no scan saw it run still counts as healed (the e2e mock's case, flows/06 INT-01)");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Idle, true, 3000) == O::Healed, "outcome: ...in Idle too");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Error, true, 3000) == O::NotHealed,
              "outcome: a turn that ended with the session in Error again - the continuation failed, not healed (and decided at once, not after the window)");
        CHECK(DecideConnectionLostRestartOutcome(true, SessionState::Running, true, 3000) == O::Healed, "outcome: running again outranks an older turn end");
    }

    void TestConnectionLostRunnerGiveBack()
    {
        AutorunnerState a;
        a.mode = AutorunnerMode::Off;
        a.stopOnErrorPausedMode = AutorunnerMode::Full;
        a.stopOnErrorPausedEpisodeMs = 1000;
        CHECK(ShouldGiveRunnerBackAfterRestart(a, 1000), "give-back: the mode the stop-on-error pause took goes back once that episode is healed");
        CHECK(!ShouldGiveRunnerBackAfterRestart(a, 2000), "give-back: a pause of another Error episode is not given back");
        {
            auto b = a;
            b.mode = AutorunnerMode::Full;
            CHECK(!ShouldGiveRunnerBackAfterRestart(b, 1000), "give-back: a runner that runs already is left as it is");
        }
        {
            auto b = a;
            b.stopOnErrorPausedMode = AutorunnerMode::Off;
            CHECK(!ShouldGiveRunnerBackAfterRestart(b, 1000), "give-back: a pause that took nothing gives nothing back");
        }
        {
            auto b = a;
            b.stopOnErrorPausedEpisodeMs = 0;
            CHECK(!ShouldGiveRunnerBackAfterRestart(b, 0), "give-back: an unstamped pause never matches, not even an episode of 0");
        }
        {
            auto b = a;
            ClearInterruptHold(b);
            CHECK(!ShouldGiveRunnerBackAfterRestart(b, 1000) && b.stopOnErrorPausedMode == AutorunnerMode::Off && b.stopOnErrorPausedEpisodeMs == 0 && b.mode == AutorunnerMode::Off,
                  "give-back: every mode decision (ClearInterruptHold) forgets the pause and leaves the mode alone");
        }
    }

    void TestConnectionLostLedger()
    {
        constexpr int64_t kMin = 60 * 1000;
        ConnectionLostRestartLedger ledger;
        auto e = ledger.Look(L"s1", 0);
        CHECK(e.lastErrorUuid.empty() && e.restartsInWindow == 0, "ledger: a session never restarted has no history");
        ledger.Note(L"s1", L"e1", 1000);
        e = ledger.Look(L"s1", 2000);
        CHECK(e.lastErrorUuid == L"e1" && e.restartsInWindow == 1, "ledger: a restart is remembered with its error row");
        CHECK(ledger.Look(L"s2", 2000).restartsInWindow == 0 && ledger.Look(L"s2", 2000).lastErrorUuid.empty(), "ledger: sessions are counted apart");
        ledger.Note(L"s1", L"e2", 10 * kMin);
        ledger.Note(L"s1", L"e3", 20 * kMin);
        e = ledger.Look(L"s1", 25 * kMin);
        CHECK(e.restartsInWindow == 3 && e.lastErrorUuid == L"e3", "ledger: three restarts in the window, the newest error row last");
        CHECK(ledger.Look(L"s1", 1000 + kConnectionLostRestartWindowMs - 1).restartsInWindow == 3, "ledger: a restart still counts a millisecond before its window ends");
        CHECK(ledger.Look(L"s1", 1000 + kConnectionLostRestartWindowMs).restartsInWindow == 2, "ledger: ...and leaves the count when it does");
        ledger.Note(L"s1", L"e4", 60 * kMin);
        e = ledger.Look(L"s1", 60 * kMin);
        CHECK(e.restartsInWindow == 1 && e.lastErrorUuid == L"e4", "ledger: an old stretch of restarts no longer counts against a new one");
        ConnectionLostRestartLedger back;
        back.Note(L"s", L"e", 10 * kMin);
        CHECK(back.Look(L"s", 5 * kMin).restartsInWindow == 1, "ledger: a restart stamped after the asked time (a clock that went back) still counts - the budget only gets stricter");
        CHECK(&SharedConnectionLostRestartLedger() == &SharedConnectionLostRestartLedger(), "ledger: one shared ledger per process");
    }
}

void TestConnectionLostRestart()
{
    TestConnectionLostText();
    TestConnectionLostRegistryGate();
    TestConnectionLostTranscriptGate();
    TestConnectionLostOutcome();
    TestConnectionLostRunnerGiveBack();
    TestConnectionLostLedger();
}
