// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 test harness - the AGENT HARNESS's model-free half (SDK_HARNESS.md) and the
// Sessions page's AI search tools (SESSIONS.md section 9).
//
// Nothing here starts claude.exe or makes a model call (that is tests\harness_live.cpp, run by
// hand). These tests pin what must hold whatever the model does:
//
// TestAgentHarnessCore - AgentHarnessCore.{h,cpp}: the conversation RECORD (the item caps, the
//   JSON round trip, a corrupt file reading as "no conversation", the atomic save / load /
//   delete, an empty record deleting its file), the cold-start DIGEST (which turns, which lines,
//   no tool results, the size bound), the --settings text (auto-compaction ON beside its window -
//   the pair without which Claude Code never compacts - the retry bound, the effort), the child's
//   environment (the harness mark, the removals of a parent session's variables), and the readers
//   of Claude Code's own lines, fed the shapes captured from Claude Code 2.1.291 (init, the three
//   compaction lines, a result whose failure flag is is_error). Plus the Fleet Observer's side of
//   the harness mark: ParseClaudeFacts reads AM_HARNESS.
//
// TestSessionSearchTools - SessionSearchTools.{h,cpp}: the search tool's arguments (defaults,
//   clamps, the errors written for the model), ONE search over a hand-built catalog with an
//   injected content search (the scope, the four match tiers and their order, the sorts, the
//   limit, deep search only on request, a cancelled search), the result text (the row fields,
//   the "indexing" note of a catalog that does not reach back far enough), the two tool handlers
//   (short ids and their collisions, show by ids / top / nothing, unknown ids, a cancelled
//   request leaving no trace, no index at all), and GatherSessionCatalog on scratch folders
//   (a whole gather, a gather that stops early and says how far it got, steps that always move
//   forward, a refresh that reads nothing).

#include "m5_tests.h"

#include "../AgentHarness.h" // HarnessToolServer / HarnessToolResult (types only: no SDK)
#include "../AgentHarnessCore.h"
#include "../SessionSearchTools.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // A fixed "now" for every time computation here: 2026-10-10 12:00:00 UTC.
    constexpr int64_t kNow = 1791633600000LL;
    constexpr int64_t kDay = 86400000LL;

    // Parse JSON text that the test itself wrote; a parse failure is a test bug, so it fails loudly.
    //   text    - the JSON.
    //   returns - the value; an empty object when the text does not parse (and one failed check).
    json::Value J(const std::wstring& text)
    {
        auto parsed = json::Parse(text);
        CHECK(parsed.has_value(), "test JSON parses");
        return parsed ? *parsed : json::Value::MkObj();
    }

    // One record item, by kind.
    HarnessItem Item(HarnessItemKind kind, uint64_t turn, const std::wstring& text, int64_t at = 1000)
    {
        HarnessItem it;
        it.kind = kind;
        it.turn = turn;
        it.atUnixMs = at;
        it.text = text;
        return it;
    }

    // A session index entry as a gather would produce it, for the search tests.
    //   id         - the session id (a full uuid, so the 8-character short id means something).
    //   title      - the custom title (the first field of the title precedence).
    //   cwd        - the working folder.
    //   activeDays - how many days before kNow its last message was.
    SessionIndexEntry Entry(const std::wstring& id, const std::wstring& title, const std::wstring& cwd, double activeDays)
    {
        SessionIndexEntry e;
        e.valid = true;
        e.sessionId = id;
        e.path = L"C:\\claude\\projects\\K--proj\\" + id + L".jsonl";
        e.stats.found = true;
        e.stats.customTitle = title;
        e.stats.cwd = cwd;
        e.stats.lastTimestampMs = kNow - static_cast<int64_t>(activeDays * static_cast<double>(kDay));
        e.stats.firstTimestampMs = e.stats.lastTimestampMs - 3600000;
        e.mtimeMs = e.stats.lastTimestampMs + 1000;
        e.birthMs = e.stats.firstTimestampMs;
        e.stats.userPrompts = 3;
        e.stats.toolUses = 7;
        return e;
    }

    // Write `text` as UTF-8 to `path` and give the file the modification time `mtimeUnixMs`.
    //   returns - true when both worked.
    bool WriteFileWithMtime(const std::filesystem::path& path, const std::string& text, int64_t mtimeUnixMs)
    {
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                return false;
            }
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        const HANDLE h = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        // Unix ms -> FILETIME (100 ns ticks since 1601).
        const uint64_t ticks = (static_cast<uint64_t>(mtimeUnixMs) + 11644473600000ULL) * 10000ULL;
        FILETIME ft{};
        ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
        ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
        const BOOL ok = ::SetFileTime(h, nullptr, nullptr, &ft);
        ::CloseHandle(h);
        return ok != FALSE;
    }

    // The ids of the search tests: the first two share their first 8 characters on purpose (the
    // short-id collision case).
    const wchar_t kIdDrag[] = L"aaaa1111-0000-4000-8000-000000000001";
    const wchar_t kIdDragTwin[] = L"aaaa1111-0000-4000-8000-000000000002";
    const wchar_t kIdTags[] = L"bbbb2222-0000-4000-8000-000000000003";
    const wchar_t kIdNum[] = L"cccc3333-0000-4000-8000-000000000004";
    const wchar_t kIdOld[] = L"dddd4444-0000-4000-8000-000000000005";

    // The catalog every search test uses: five sessions, newest first.
    SessionCatalog TestCatalog()
    {
        SessionCatalog c;
        c.sinceUnixMs = kNow - 400 * kDay;
        c.coveredSinceUnixMs = c.sinceUnixMs;
        c.gatheredUnixMs = kNow;
        c.entries.push_back(Entry(kIdDrag, L"Fix tab drag crash", L"K:\\source\\Agentmaster", 1));
        c.entries.push_back(Entry(kIdDragTwin, L"Sessions page polish", L"K:\\source\\Agentmaster", 2));
        c.entries.push_back(Entry(kIdTags, L"Tag chips performance", L"K:\\source\\Agentmaster", 5));
        c.entries.push_back(Entry(kIdNum, L"polyfit for six bases", L"K:\\source\\NumSharp", 20));
        c.entries.push_back(Entry(kIdOld, L"ancient drag experiment", L"K:\\source\\Old", 200));
        c.entries[1].stats.pathsAccessed.push_back(L"K:\\source\\Agentmaster\\src\\TabDragMath.h"); // a PATH match for "drag"
        c.entries[2].stats.gitBranch = L"feature/tags";
        c.entries[3].stats.forkedFromId = kIdOld;
        c.tags[kIdTags] = { L"Release" };
        c.favorites.insert(kIdNum);
        c.openIds.insert(kIdDrag);
        return c;
    }
}

void TestAgentHarnessCore()
{
    std::wprintf(L"TestAgentHarnessCore\n");

    // ---- the record's caps ----
    {
        HarnessRecord r;
        AppendHarnessItem(r, Item(HarnessItemKind::User, 1, std::wstring(kHarnessMaxItemTextChars + 500, L'x'), 5000));
        CHECK(r.items.size() == 1 && r.items[0].text.size() == kHarnessMaxItemTextChars, "record: a long text is cut to the cap");
        CHECK(r.items[0].text.rfind(L"...") == r.items[0].text.size() - 3, "record: a cut text ends in ...");
        CHECK(r.updatedUnixMs == 5000, "record: updated follows the newest item");
        HarnessItem result = Item(HarnessItemKind::ToolResult, 1, std::wstring(kHarnessMaxToolResultChars + 1, L'y'), 4000);
        AppendHarnessItem(r, result);
        CHECK(r.items[1].text.size() == kHarnessMaxToolResultChars, "record: a tool result is cut to its own, smaller cap");
        CHECK(r.updatedUnixMs == 5000, "record: an older item does not move updated back");
        HarnessItem call = Item(HarnessItemKind::ToolCall, 1, L"");
        call.argumentsJson = std::wstring(kHarnessMaxItemTextChars + 1, L'z');
        AppendHarnessItem(r, call);
        CHECK(r.items[2].argumentsJson.size() == kHarnessMaxItemTextChars, "record: tool arguments are cut too");
        HarnessRecord many;
        for (size_t i = 0; i < kHarnessMaxItems + 25; ++i)
        {
            AppendHarnessItem(many, Item(HarnessItemKind::Assistant, 1, L"item " + std::to_wstring(i)));
        }
        CHECK(many.items.size() == kHarnessMaxItems, "record: the item count is capped");
        CHECK(many.items.front().text == L"item 25" && many.items.back().text == L"item " + std::to_wstring(kHarnessMaxItems + 24), "record: the OLDEST items are the ones dropped");
    }

    // ---- the JSON round trip ----
    HarnessRecord record;
    record.name = L"sessions-search";
    record.model = L"claude-sonnet-5-5";
    record.createdUnixMs = 1000;
    record.turns = 2;
    record.compactions = 1;
    record.usage.contextTokens = 12345;
    record.usage.contextWindow = 500000;
    record.usage.inputTokens = 11;
    record.usage.outputTokens = 22;
    record.usage.cacheReadTokens = 33;
    record.usage.cacheCreationTokens = 44;
    record.usage.costUsd = 0.25;
    AppendHarnessItem(record, Item(HarnessItemKind::User, 1, L"find the tab drag crash \x05E9\x05DC\x05D5\x05DD \"quoted\"\nline two", 1100));
    {
        HarnessItem call = Item(HarnessItemKind::ToolCall, 1, L"", 1200);
        call.toolName = L"mcp__sessions__search";
        call.argumentsJson = LR"({"query":"tab drag"})";
        AppendHarnessItem(record, call);
        HarnessItem result = Item(HarnessItemKind::ToolResult, 1, L"SECRET-RESULT-TEXT", 1300);
        result.toolName = L"mcp__sessions__search";
        result.isError = true;
        AppendHarnessItem(record, result);
    }
    AppendHarnessItem(record, Item(HarnessItemKind::Assistant, 1, L"The best match is the tab drag crash session.", 1400));
    AppendHarnessItem(record, Item(HarnessItemKind::User, 2, L"only the open ones", 2000));
    AppendHarnessItem(record, Item(HarnessItemKind::Assistant, 2, L"One open session is shown.", 2100));
    {
        HarnessItem notice = Item(HarnessItemKind::Notice, 0, L"the context was compacted", 2200);
        AppendHarnessItem(record, notice);
    }
    {
        const std::wstring text = SerializeHarnessRecord(record);
        const HarnessRecord back = DeserializeHarnessRecord(text);
        CHECK(back.name == record.name && back.model == record.model && back.turns == 2 && back.compactions == 1, "round trip: the header fields");
        CHECK(back.createdUnixMs == 1000 && back.updatedUnixMs == record.updatedUnixMs, "round trip: the times");
        CHECK(back.usage.contextTokens == 12345 && back.usage.contextWindow == 500000 && back.usage.inputTokens == 11 && back.usage.outputTokens == 22 && back.usage.cacheReadTokens == 33 && back.usage.cacheCreationTokens == 44 && back.usage.costUsd == 0.25, "round trip: the usage");
        CHECK(back.items.size() == record.items.size(), "round trip: every item");
        bool same = back.items.size() == record.items.size();
        for (size_t i = 0; same && i < back.items.size(); ++i)
        {
            const auto& a = back.items[i];
            const auto& b = record.items[i];
            same = a.kind == b.kind && a.turn == b.turn && a.atUnixMs == b.atUnixMs && a.text == b.text && a.toolName == b.toolName && a.argumentsJson == b.argumentsJson && a.isError == b.isError;
        }
        CHECK(same, "round trip: each item field for field (Hebrew, quotes and a line break included)");
        CHECK(DeserializeHarnessRecord(L"").Empty() && DeserializeHarnessRecord(L"not json").Empty() && DeserializeHarnessRecord(L"[1,2]").Empty(), "a file that is not a record reads as no conversation");
        CHECK(DeserializeHarnessRecord(LR"({"version":2,"turns":5,"items":[]})").Empty(), "an unknown record version reads as no conversation");
        CHECK(HarnessRecord{}.Empty() && !record.Empty(), "Empty(): nothing asked yet");
    }

    // ---- the cold-start digest ----
    {
        const std::wstring digest = BuildHarnessDigest(record, 6, 6000);
        CHECK(digest.find(L"user: find the tab drag crash") != std::wstring::npos, "digest: a turn's user message");
        CHECK(digest.find(L"tool mcp__sessions__search {\"query\":\"tab drag\"}") != std::wstring::npos, "digest: a tool call with its arguments");
        CHECK(digest.find(L"assistant: The best match") != std::wstring::npos, "digest: the assistant's text");
        CHECK(digest.find(L"SECRET-RESULT-TEXT") == std::wstring::npos, "digest: tool RESULTS are left out");
        CHECK(digest.find(L"user: only the open ones") != std::wstring::npos && digest.find(L"user: find") < digest.find(L"user: only"), "digest: oldest turn first");
        const std::wstring last = BuildHarnessDigest(record, 1, 6000);
        CHECK(last.find(L"only the open ones") != std::wstring::npos && last.find(L"tab drag crash") == std::wstring::npos, "digest: maxTurns keeps the NEWEST turns");
        CHECK(BuildHarnessDigest(record, 6, 80).size() <= 80, "digest: never longer than maxChars");
        CHECK(BuildHarnessDigest(HarnessRecord{}, 6, 6000).empty(), "digest: nothing for an empty record");
        CHECK(ComposeColdStartMessage(L"", L"hello") == L"hello", "cold start: no digest, the text as it is");
        const std::wstring composed = ComposeColdStartMessage(digest, L"and the other one");
        CHECK(composed.find(digest) != std::wstring::npos && composed.size() > digest.size() && composed.rfind(L"and the other one") == composed.size() - 17, "cold start: the digest in a marked block, then the user's own text last");
    }

    // ---- names and files ----
    {
        CHECK(IsValidHarnessName(L"sessions-search") && IsValidHarnessName(L"a") && IsValidHarnessName(std::wstring(48, L'a')), "names: a-z, 0-9 and '-', 1 to 48 characters");
        CHECK(!IsValidHarnessName(L"") && !IsValidHarnessName(std::wstring(49, L'a')) && !IsValidHarnessName(L"Sessions") && !IsValidHarnessName(L"a b") && !IsValidHarnessName(L"a\\b") && !IsValidHarnessName(L"a.b") && !IsValidHarnessName(L"\x05D0"), "names: nothing that is unsafe in a file name, an environment value or a log tag");
        CHECK(HarnessToolFullName(L"sessions", L"search") == L"mcp__sessions__search", "the model's name of a tool");

        wchar_t temp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, temp);
        const std::wstring dir = std::wstring{ temp } + L"agentmaster-m5-harness";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        const std::wstring path = HarnessRecordPathIn(dir + L"\\nested", L"sessions-search");
        CHECK(path == dir + L"\\nested\\sessions-search.json", "the record's path");
        CHECK(LoadHarnessRecordFrom(path).Empty(), "load: a missing file is no conversation");
        CHECK(SaveHarnessRecordTo(path, record) && std::filesystem::exists(path), "save: creates the folder and the file");
        const HarnessRecord loaded = LoadHarnessRecordFrom(path);
        CHECK(loaded.turns == 2 && loaded.items.size() == record.items.size() && loaded.items[0].text == record.items[0].text, "load: reads the saved record back");
        bool leftovers = false;
        for (const auto& f : std::filesystem::directory_iterator(dir + L"\\nested", ec))
        {
            leftovers = leftovers || f.path().filename() != L"sessions-search.json";
        }
        CHECK(!leftovers, "save: no temp file is left beside the record");
        CHECK(SaveHarnessRecordTo(path, HarnessRecord{}) && !std::filesystem::exists(path), "save: an EMPTY record deletes the file");
        CHECK(SaveHarnessRecordTo(path, record) && DeleteHarnessRecordAt(path) && !std::filesystem::exists(path), "delete: removes the file");
        CHECK(DeleteHarnessRecordAt(path), "delete: a missing file is success");
        std::filesystem::remove_all(dir, ec);
    }

    // ---- the --settings text ----
    {
        HarnessSettingsSpec spec;
        spec.autoCompactTokens = 500000;
        spec.effort = L"low";
        spec.apiMaxRetries = 2;
        const std::wstring text = BuildHarnessSettingsJson(spec);
        const json::Value settings = J(text);
        bool ascii = true;
        for (const wchar_t c : text)
        {
            ascii = ascii && c >= 0x20 && c < 0x7F;
        }
        CHECK(ascii, "settings: plain ASCII on one line (it travels on a command line)");
        CHECK(settings.BoolAt(L"autoCompactEnabled") && settings.I64At(L"autoCompactWindow") == 500000, "settings: auto-compaction is turned ON beside its window (the flag alone never compacts)");
        const auto* verbose = settings.Find(L"verbose");
        CHECK(verbose && verbose->type == json::Value::Type::Bool && !settings.BoolAt(L"verbose"), "settings: verbose is forced off");
        const auto* env = settings.Find(L"env");
        CHECK(env && env->StrAt(L"CLAUDE_CODE_MAX_RETRIES") == L"2" && env->StrAt(L"CLAUDE_CODE_RETRY_WATCHDOG") == L"0", "settings: bounded API retries, no retry watchdog");
        CHECK(env && env->StrAt(L"CLAUDE_CODE_EFFORT_LEVEL") == L"low", "settings: the effort variable (it beats the --effort flag)");
        CHECK(env && env->StrAt(L"DISABLE_AUTOUPDATER") == L"1" && env->StrAt(L"CLAUDE_CODE_DISABLE_CLAUDE_MDS") == L"1" && env->StrAt(L"CLAUDE_CODE_DISABLE_AUTO_MEMORY") == L"1", "settings: a quiet background child");

        HarnessSettingsSpec none;
        none.effort = L"bogus";
        none.apiMaxRetries = 99;
        const json::Value plain = J(BuildHarnessSettingsJson(none));
        CHECK(plain.Find(L"autoCompactEnabled") == nullptr && plain.Find(L"autoCompactWindow") == nullptr, "settings: no window asked, no compaction keys");
        const auto* plainEnv = plain.Find(L"env");
        CHECK(plainEnv && plainEnv->Find(L"CLAUDE_CODE_EFFORT_LEVEL") == nullptr, "settings: an unknown effort sets none");
        CHECK(plainEnv && plainEnv->StrAt(L"CLAUDE_CODE_MAX_RETRIES") == L"15", "settings: the retry count is clamped");
        HarnessSettingsSpec tiny;
        tiny.autoCompactTokens = 5;
        CHECK(J(BuildHarnessSettingsJson(tiny)).I64At(L"autoCompactWindow") == 100000, "settings: the window is clamped into Claude Code's range");
        CHECK(ClampHarnessAutoCompactTokens(0) == 0 && ClampHarnessAutoCompactTokens(1) == 100000 && ClampHarnessAutoCompactTokens(500000) == 500000 && ClampHarnessAutoCompactTokens(5000000) == 1000000 && ClampHarnessAutoCompactTokens(-7) == 0, "the auto-compact clamp: 0 (and less) means no window, anything else 100,000..1,000,000");
    }

    // ---- the child's environment, and the Fleet Observer's side of the mark ----
    {
        const std::map<std::string, std::string> env = BuildHarnessEnvironment(L"sessions-search");
        const auto get = [&env](const char* name) -> const std::string* {
            const auto it = env.find(name);
            return it == env.end() ? nullptr : &it->second;
        };
        CHECK(get("AM_HARNESS") && *get("AM_HARNESS") == "sessions-search", "environment: the harness mark carries the conversation key");
        bool removals = true;
        for (const char* name : { "CLAUDECODE", "CLAUDE_CODE_SESSION_ID", "WT_SESSION", "AM_SESSION", "CCMGR_HOOK_PIPE", "CCMGR_SESSION_ID", "CLAUDE_CODE_EFFORT_LEVEL", "DISABLE_AUTO_COMPACT", "CLAUDE_CODE_AUTO_COMPACT_WINDOW", "DEBUG" })
        {
            removals = removals && get(name) && get(name)->empty();
        }
        CHECK(removals, "environment: a parent session's and tab's variables are REMOVED (an empty value)");
        CHECK(get("CLAUDE_CODE_ENTRYPOINT") == nullptr && get("PATH") == nullptr && get("USERPROFILE") == nullptr, "environment: nothing else is touched");

        std::unordered_map<std::wstring, std::wstring> childEnv;
        childEnv[kHarnessEnvMarker] = L"sessions-search";
        ClaudeProcessFacts facts;
        ParseClaudeFacts(L"claude.exe --output-format stream-json --model claude-sonnet-5-5", childEnv, facts);
        CHECK(facts.harnessName == L"sessions-search" && facts.wtSession.empty() && facts.amSession.empty(), "the Fleet Observer reads the harness mark (it then skips the process)");
        ClaudeProcessFacts plain;
        ParseClaudeFacts(L"claude.exe", {}, plain);
        CHECK(plain.harnessName.empty(), "a claude without the mark is an ordinary one");
    }

    // ---- the readers of Claude Code's lines (shapes captured from 2.1.291) ----
    {
        const auto init = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"init","cwd":"C:\\x","session_id":"1d0e","tools":["ListMcpResourcesTool","mcp__sessions__search","mcp__display__show"],"mcp_servers":[{"name":"sessions","status":"connected","source":"sdk"},{"name":"display","status":"failed"}],"model":"claude-sonnet-5-5","permissionMode":"dontAsk","claude_code_version":"2.1.291"})");
        CHECK(init.kind == HarnessSystemKind::Init && init.model == L"claude-sonnet-5-5" && init.sessionId == L"1d0e" && init.cliVersion == L"2.1.291" && init.permissionMode == L"dontAsk", "system/init: the model, the session id, the version, the permission mode");
        CHECK(init.tools.size() == 3 && init.tools[1] == L"mcp__sessions__search", "system/init: the tools in effect");
        CHECK(init.mcpServers.size() == 2 && init.mcpServers[0].first == L"sessions" && init.mcpServers[0].second == L"connected" && init.mcpServers[1].second == L"failed", "system/init: each MCP server with its status");
        const auto compacting = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"status","status":"compacting"})");
        CHECK(compacting.kind == HarnessSystemKind::Compacting, "system/status compacting");
        const auto finished = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"status","status":null,"compact_result":"success"})");
        CHECK(finished.kind == HarnessSystemKind::CompactFinished && finished.compactOk, "system/status with a compact_result: the compaction ended well");
        const auto failed = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"status","status":null,"compact_result":"failed"})");
        CHECK(failed.kind == HarnessSystemKind::CompactFinished && !failed.compactOk, "system/status with a failed compact_result");
        const auto boundary = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"compact_boundary","compact_metadata":{"trigger":"auto","pre_tokens":70709,"post_tokens":11077,"cumulative_dropped_tokens":59632,"duration_ms":7825}})");
        CHECK(boundary.kind == HarnessSystemKind::CompactBoundary && boundary.compactTrigger == L"auto" && boundary.preTokens == 70709 && boundary.postTokens == 11077 && boundary.compactDurationMs == 7825, "system/compact_boundary: the trigger, the sizes, the duration");
        CHECK(ClassifyHarnessSystemMessage(LR"({"type":"assistant"})").kind == HarnessSystemKind::Other && ClassifyHarnessSystemMessage(L"{").kind == HarnessSystemKind::Other && ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"status","status":"requesting"})").kind == HarnessSystemKind::Other, "any other line is Other");

        const auto ok = ParseHarnessResultMessage(LR"({"type":"result","subtype":"success","is_error":false,"duration_ms":2789,"duration_api_ms":2100,"num_turns":3,"result":"Three sessions are shown.","stop_reason":"end_turn","total_cost_usd":0.0421,"modelUsage":{"claude-sonnet-5-5":{"inputTokens":9,"contextWindow":1000000},"claude-haiku-4-5":{"contextWindow":200000}}})");
        CHECK(ok.valid && !ok.isError && ok.subtype == L"success" && ok.text == L"Three sessions are shown." && ok.stopReason == L"end_turn", "result: a turn that ended well");
        CHECK(ok.numTurns == 3 && ok.durationMs == 2789 && ok.durationApiMs == 2100 && ok.totalCostUsd > 0.042 && ok.totalCostUsd < 0.0422, "result: the counters");
        CHECK(ok.contextWindow == 1000000, "result: the context window is the largest one reported");
        const auto bad = ParseHarnessResultMessage(LR"({"type":"result","subtype":"success","is_error":true,"api_error_status":404,"result":"model not found","errors":["one","two"]})");
        CHECK(bad.valid && bad.isError && bad.subtype == L"success" && bad.apiErrorStatus == 404 && bad.errors.size() == 2, "result: the failure flag is is_error, whatever subtype says");
        CHECK(!ParseHarnessResultMessage(LR"({"type":"assistant"})").valid && !ParseHarnessResultMessage(L"nope").valid, "result: any other line is not one");

        const auto usage = ParseHarnessRequestUsage(LR"({"input_tokens":3,"cache_creation_input_tokens":1500,"cache_read_input_tokens":9000,"output_tokens":120,"service_tier":"standard"})");
        CHECK(usage.inputTokens == 3 && usage.cacheCreationTokens == 1500 && usage.cacheReadTokens == 9000 && usage.outputTokens == 120 && usage.Context() == 10623, "usage: the four token counts and their sum, the context size");
        CHECK(ParseHarnessRequestUsage(L"{}").Context() == 0 && ParseHarnessRequestUsage(L"x").Context() == 0, "usage: missing numbers are 0");
    }
}

void TestSessionSearchTools()
{
    std::wprintf(L"TestSessionSearchTools\n");
    std::wstring error;

    // ---- the search tool's arguments ----
    {
        const auto defaults = ParseSessionSearchParams(J(L"{}"), kNow, error);
        CHECK(defaults && error.empty() && defaults->query.empty() && defaults->days == kSessionSearchDefaultDays && defaults->limit == 15 && defaults->sort == L"relevance" && !defaults->deep && !defaults->openOnly && !defaults->favoriteOnly, "arguments: the defaults");
        CHECK(defaults && SessionSearchSinceMs(*defaults, kNow) == kNow - kSessionSearchDefaultDays * kDay, "arguments: the default period is the last kSessionSearchDefaultDays days");
        const auto full = ParseSessionSearchParams(J(LR"({"query":"  tab drag  ","days":7,"dir":" Agentmaster ","branch":"main","tag":"release","favorite":true,"open":true,"deep":true,"sort":"created","limit":3})"), kNow, error);
        CHECK(full && full->query == L"tab drag" && full->days == 7 && full->dir == L"Agentmaster" && full->branch == L"main" && full->tag == L"release" && full->favoriteOnly && full->openOnly && full->deep && full->sort == L"created" && full->limit == 3, "arguments: every field, trimmed");
        const auto clamped = ParseSessionSearchParams(J(LR"({"days":0,"limit":500,"sort":"upside down"})"), kNow, error);
        CHECK(clamped && clamped->days == 1 && clamped->limit == 50 && clamped->sort == L"relevance", "arguments: out-of-range numbers are clamped, an unknown sort is the default");
        const auto huge = ParseSessionSearchParams(J(LR"({"days":99999,"limit":-4})"), kNow, error);
        CHECK(huge && huge->days == 3650 && huge->limit == 1, "arguments: the upper clamp of days, the lower clamp of limit");
        CHECK(!ParseSessionSearchParams(J(LR"({"query":7})"), kNow, error) && error.find(L"\"query\"") != std::wstring::npos && error.find(L"string") != std::wstring::npos, "arguments: a wrong type is an error that names the argument and what it must be");
        CHECK(!ParseSessionSearchParams(J(LR"({"days":"seven"})"), kNow, error) && error.find(L"\"days\"") != std::wstring::npos, "arguments: days must be a number");
        // Mid-January: no time zone changes its clock then, so the span is the same on every machine.
        const auto dated = ParseSessionSearchParams(J(LR"({"after":"2026-01-13","before":"2026-01-15","days":7})"), kNow, error);
        CHECK(dated && dated->afterUnixMs > 0 && dated->beforeUnixMs > dated->afterUnixMs && dated->beforeUnixMs - dated->afterUnixMs == 3 * kDay - 1, "arguments: after is the day's first millisecond, before the day's last (local time)");
        CHECK(dated && SessionSearchSinceMs(*dated, kNow) == dated->afterUnixMs, "arguments: after overrides days");
        CHECK(!ParseSessionSearchParams(J(LR"({"after":"October 1st"})"), kNow, error) && error.find(L"YYYY-MM-DD") != std::wstring::npos, "arguments: a date in another form is an error that shows the form");
        CHECK(!ParseSessionSearchParams(J(LR"({"after":"2026-10-03","before":"2026-10-01"})"), kNow, error) && error.find(L"swap") != std::wstring::npos, "arguments: before earlier than after says how to fix it");
        CHECK(ParseSessionSearchParams(J(LR"({"query":null,"dir":null,"after":null,"days":null})"), kNow, error).has_value(), "arguments: null means not given");
    }

    // ---- the row facts ----
    {
        SessionIndexEntry e = Entry(kIdNum, L"custom", L"K:\\proj", 3);
        e.stats.aiTitle = L"ai";
        e.stats.firstUserPrompt = L"first prompt";
        CHECK(SessionSearchTitleOf(e) == L"custom", "title: the custom title first");
        e.liveTitle = L"the tab's name";
        CHECK(SessionSearchTitleOf(e) == L"the tab's name", "title: a stored / live tab title wins over the transcript's");
        SessionIndexEntry bare;
        CHECK(SessionSearchTitleOf(bare) == L"(no prompt yet)", "title: a placeholder, never empty");
        CHECK(SessionSearchLastActivityMs(e) == e.stats.lastTimestampMs, "active: the newest message");
        e.stats.lastTimestampMs = 0;
        CHECK(SessionSearchLastActivityMs(e) == e.mtimeMs, "active: the file time when no message has a time");
        CHECK(SessionSearchCreatedMs(e) == e.stats.firstTimestampMs, "created: the first message");
        e.stats.forkedFromId = kIdOld;
        e.birthMs = 777;
        CHECK(SessionSearchCreatedMs(e) == 777, "created: a fork counts from its own file (its copied lines carry the parent's times)");
        CHECK(SessionSearchDirOf(e) == L"K:\\proj", "dir: the working folder its lines carry");
        e.stats.cwd.clear();
        CHECK(SessionSearchDirOf(e) == L"K--proj", "dir: else the name of the transcript's project folder");
        CHECK(SessionSearchHandleOf(kIdDrag) == L"aaaa1111" && SessionSearchHandleOf(L"abc") == L"abc", "the short id: the first 8 characters");
    }

    const SessionCatalog catalog = TestCatalog();
    // The injected content search: "drag" is in a prompt of the Tags session and in the text of
    // the NumSharp one, so the four tiers each have a session.
    int promptCalls = 0;
    int transcriptCalls = 0;
    size_t transcriptRefs = 0;
    SessionContentSearch content;
    content.prompts = [&promptCalls](const SessionQuery& q) {
        ++promptCalls;
        std::unordered_map<std::wstring, SessionHit> hits;
        if (q.text.find(L"drag") != std::wstring::npos)
        {
            hits[kIdTags] = SessionHit{ kIdTags, 2, { L"...the drag gesture\nbroke again...", L"second", L"third" } };
        }
        return hits;
    };
    content.transcripts = [&transcriptCalls, &transcriptRefs](const std::vector<TranscriptRef>& refs, const SessionQuery& q, const std::function<bool()>& cancelled) {
        ++transcriptCalls;
        transcriptRefs = refs.size();
        std::vector<SessionHit> hits;
        if (!(cancelled && cancelled()) && q.text.find(L"drag") != std::wstring::npos)
        {
            hits.push_back(SessionHit{ kIdNum, 4, { L"a drag coefficient" } });
        }
        return hits;
    };
    const auto params = [&error](const wchar_t* json) {
        const auto p = ParseSessionSearchParams(J(json), kNow, error);
        CHECK(p.has_value(), "test arguments parse");
        return p ? *p : SessionSearchParams{};
    };
    const auto idOf = [&catalog](const SessionSearchOutcome& o, size_t row) {
        return row < o.rows.size() ? catalog.entries[o.rows[row].entryIndex].sessionId : std::wstring{};
    };

    // ---- one search ----
    {
        const auto listed = RunSessionSearch(catalog, params(L"{}"), kNow, content);
        CHECK(listed.searched == 4 && listed.matched == 4 && listed.rows.size() == 4, "no query: every session of the period is listed (the 200-day-old one is outside 90 days)");
        CHECK(idOf(listed, 0) == kIdDrag && idOf(listed, 3) == kIdNum && listed.rows[0].tier == SessionMatchTier::Listed, "no query: newest first");
        CHECK(promptCalls == 0 && transcriptCalls == 0, "no query: no content search runs");

        const auto drag = RunSessionSearch(catalog, params(LR"({"query":"drag"})"), kNow, content);
        CHECK(drag.searched == 4 && drag.matched == 3, "query: title, path and prompt matches (the text match needs deep)");
        CHECK(idOf(drag, 0) == kIdDrag && drag.rows[0].tier == SessionMatchTier::Name, "tier 1: the title match");
        CHECK(idOf(drag, 1) == kIdDragTwin && drag.rows[1].tier == SessionMatchTier::Path, "tier 2: a file the session touched");
        CHECK(idOf(drag, 2) == kIdTags && drag.rows[2].tier == SessionMatchTier::Prompt && drag.rows[2].hits == 2 && drag.rows[2].snippets.size() == 2, "tier 3: a typed prompt, with its hit count and two snippets");
        CHECK(promptCalls == 1 && transcriptCalls == 0, "query: the prompts are searched, the transcripts are not");

        const auto deep = RunSessionSearch(catalog, params(LR"({"query":"drag","deep":true})"), kNow, content);
        CHECK(deep.matched == 4 && idOf(deep, 3) == kIdNum && deep.rows[3].tier == SessionMatchTier::Text && deep.rows[3].hits == 4, "deep: the conversation text is searched too, and ranks last");
        CHECK(transcriptCalls == 1 && transcriptRefs == 4, "deep: only the sessions in scope are scanned");

        const auto year = RunSessionSearch(catalog, params(LR"({"query":"drag","days":365})"), kNow, content);
        CHECK(year.searched == 5 && year.matched == 4 && idOf(year, 1) == kIdOld, "a longer period brings the old session in; inside the title tier, newest first");
        const auto byActive = RunSessionSearch(catalog, params(LR"({"query":"drag","days":365,"sort":"active"})"), kNow, content);
        CHECK(idOf(byActive, 0) == kIdDrag && idOf(byActive, 1) == kIdDragTwin && idOf(byActive, 3) == kIdOld, "sort active: by time alone, the tiers ignored");
        const auto limited = RunSessionSearch(catalog, params(LR"({"query":"drag","days":365,"limit":2})"), kNow, content);
        CHECK(limited.matched == 4 && limited.rows.size() == 2, "limit: the count of matches stays, the rows are cut");

        CHECK(RunSessionSearch(catalog, params(LR"({"dir":"numsharp"})"), kNow, content).matched == 1, "filter dir: part of the folder, any case");
        CHECK(RunSessionSearch(catalog, params(LR"({"dir":"source/agentmaster"})"), kNow, content).matched == 3, "filter dir: either slash");
        CHECK(RunSessionSearch(catalog, params(LR"({"branch":"tags"})"), kNow, content).matched == 1, "filter branch");
        CHECK(RunSessionSearch(catalog, params(LR"({"tag":"RELEASE"})"), kNow, content).matched == 1 && RunSessionSearch(catalog, params(LR"({"tag":"rel"})"), kNow, content).matched == 0, "filter tag: the whole name, any case");
        CHECK(idOf(RunSessionSearch(catalog, params(LR"({"favorite":true})"), kNow, content), 0) == kIdNum, "filter favorite");
        CHECK(idOf(RunSessionSearch(catalog, params(LR"({"open":true})"), kNow, content), 0) == kIdDrag, "filter open");
        CHECK(RunSessionSearch(catalog, params(LR"({"query":"drag","dir":"numsharp"})"), kNow, content).matched == 0, "a query never widens the filters");
        CHECK(RunSessionSearch(catalog, params(LR"({"days":3})"), kNow, content).matched == 2, "filter days");
        CHECK(idOf(RunSessionSearch(catalog, params((L"{\"query\":\"" + std::wstring{ kIdTags } + L"\"}").c_str()), kNow, content), 0) == kIdTags, "a whole session id finds that session");

        transcriptCalls = 0;
        const auto cancelledSearch = RunSessionSearch(catalog, params(LR"({"query":"drag","deep":true})"), kNow, content, [] { return true; });
        CHECK(transcriptCalls == 0 && cancelledSearch.matched == 3, "a cancelled search does not start the transcript scan");
    }

    // ---- the result text ----
    {
        const auto p = params(LR"({"query":"drag","days":365})");
        const auto outcome = RunSessionSearch(catalog, p, kNow, content);
        const json::Value result = J(FormatSessionSearchResult(catalog, p, outcome, kNow));
        CHECK(result.I64At(L"searched") == 5 && result.I64At(L"matched") == 4 && result.I64At(L"returned") == 4 && result.StrAt(L"window") == L"last 365 days" && !result.StrAt(L"now").empty(), "result: the window and the counts");
        CHECK(result.Find(L"indexing") == nullptr && result.Find(L"hint") == nullptr, "result: no note when the index is whole and something matched");
        const auto* sessions = result.Find(L"sessions");
        CHECK(sessions && sessions->type == json::Value::Type::Arr && sessions->arr.size() == 4, "result: one row per session");
        if (sessions && sessions->arr.size() == 4)
        {
            const auto& first = sessions->arr[0];
            CHECK(first.StrAt(L"id") == L"aaaa1111" && first.StrAt(L"title") == L"Fix tab drag crash" && first.StrAt(L"dir") == L"K:\\source\\Agentmaster" && first.StrAt(L"match") == L"title" && first.BoolAt(L"open"), "result row: the short id, the title, the folder, why it matched, open");
            CHECK(first.I64At(L"msgs") == 3 && first.I64At(L"tools") == 7 && first.StrAt(L"active").find(L"ago)") != std::wstring::npos && first.StrAt(L"created").size() == 10, "result row: the sizes and the two times");
            CHECK(first.Find(L"fav") == nullptr && first.Find(L"fork") == nullptr && first.Find(L"tags") == nullptr && first.Find(L"snippets") == nullptr, "result row: flags and lists only when they say something");
            const auto& prompt = sessions->arr[3];
            CHECK(prompt.StrAt(L"match") == L"prompt" && prompt.I64At(L"hits") == 2 && prompt.Find(L"snippets") && prompt.Find(L"snippets")->arr.size() == 2 && prompt.Find(L"snippets")->arr[0].str.find(L'\n') == std::wstring::npos, "result row: a prompt match carries its hits and one-line snippets");
            CHECK(prompt.StrAt(L"branch") == L"feature/tags" && prompt.Find(L"tags") && prompt.Find(L"tags")->arr.size() == 1, "result row: the branch and the tags");
        }
        const auto none = RunSessionSearch(catalog, params(LR"({"query":"zebra"})"), kNow, content);
        CHECK(J(FormatSessionSearchResult(catalog, params(LR"({"query":"zebra"})"), none, kNow)).StrAt(L"hint").find(L"deep:true") != std::wstring::npos, "result: nothing matched -> a hint that names the next steps");
        CHECK(J(FormatSessionSearchResult(catalog, params(LR"({"query":"zebra","deep":true})"), none, kNow)).StrAt(L"hint").find(L"deep:true") == std::wstring::npos, "result: ... and does not suggest deep to a deep search");

        // A catalog that stops 10 days back: a 90-day search must say what it lacks.
        SessionCatalog partial = TestCatalog();
        partial.coveredSinceUnixMs = kNow - 10 * kDay;
        CHECK(!partial.Complete() && catalog.Complete(), "Complete(): the catalog reaches as far back as it was asked");
        const auto p90 = params(LR"({"query":"zebra"})");
        const json::Value flagged = J(FormatSessionSearchResult(partial, p90, RunSessionSearch(partial, p90, kNow, content), kNow));
        CHECK(flagged.StrAt(L"indexing").find(L"still being indexed") != std::wstring::npos, "result: a partial index is reported in an indexing field");
        CHECK(flagged.StrAt(L"hint").find(L"longer period") == std::wstring::npos, "result: ... and the hint does not suggest a longer period then");
        const auto p3 = params(LR"({"days":3})");
        CHECK(J(FormatSessionSearchResult(partial, p3, RunSessionSearch(partial, p3, kNow, content), kNow)).Find(L"indexing") == nullptr, "result: no indexing note when the search's own period is covered");

        // A catalog without the stored tags and favorites: a filter on them cannot be answered.
        SessionCatalog noStore = TestCatalog();
        noStore.storeOverlay = false;
        noStore.tags.clear();
        noStore.favorites.clear();
        const auto pTag = params(LR"({"tag":"release"})");
        const json::Value blind = J(FormatSessionSearchResult(noStore, pTag, RunSessionSearch(noStore, pTag, kNow, content), kNow));
        CHECK(blind.I64At(L"matched") == 0 && blind.StrAt(L"indexing").find(L"Tags and favorites are still being loaded") != std::wstring::npos && blind.StrAt(L"hint").find(L"longer period") == std::wstring::npos, "result: a tag filter over a catalog without the stored tags says that they are still being loaded");
        const auto pFav = params(LR"({"favorite":true})");
        CHECK(!J(FormatSessionSearchResult(noStore, pFav, RunSessionSearch(noStore, pFav, kNow, content), kNow)).StrAt(L"indexing").empty(), "result: ... and so does a favorite filter");
        const auto pPlain = params(LR"({"query":"drag"})");
        CHECK(J(FormatSessionSearchResult(noStore, pPlain, RunSessionSearch(noStore, pPlain, kNow, content), kNow)).Find(L"indexing") == nullptr, "result: ... a search without such a filter has no note");

        CHECK(DescribeSessionSearch(params(LR"({"query":"tab drag","days":7,"dir":"Agentmaster","open":true,"deep":true})")) == L"\x201Ctab drag\x201D \x00B7 last 7 days \x00B7 in Agentmaster \x00B7 open \x00B7 full text", "the one-line description of a search, for the user");
        CHECK(DescribeSessionSearch(params(L"{}")).rfind(L"all sessions", 0) == 0 && DescribeSessionSearch(params(LR"({"days":1})")).find(L"last 1 day") != std::wstring::npos, "description: no query, and the singular day");
    }

    // ---- the two tool servers ----
    {
        auto shared = std::make_shared<const SessionCatalog>(TestCatalog());
        std::atomic<uint64_t> generation{ 1 };
        bool giveCatalog = true;
        int64_t askedSince = 0;
        std::vector<std::wstring> searching;
        std::vector<std::vector<std::wstring>> candidates;
        std::vector<std::vector<std::wstring>> shown;
        std::vector<std::wstring> notes;
        SessionSearchToolset::Callbacks callbacks;
        callbacks.catalog = [&](int64_t since, const std::function<bool()>&) {
            askedSince = since;
            return giveCatalog ? shared : std::shared_ptr<const SessionCatalog>{};
        };
        callbacks.generation = [&generation] { return generation.load(); };
        callbacks.onSearching = [&searching](const std::wstring& summary) { searching.push_back(summary); };
        callbacks.onCandidates = [&candidates](const std::vector<std::wstring>& ids) { candidates.push_back(ids); };
        callbacks.onShow = [&shown, &notes](const std::vector<std::wstring>& ids, const std::wstring& note) {
            shown.push_back(ids);
            notes.push_back(note);
        };
        callbacks.content = content;
        callbacks.now = [] { return kNow; };
        SessionSearchToolset toolset{ callbacks };

        bool threw = false;
        try
        {
            SessionSearchToolset::Callbacks missing;
            missing.onShow = callbacks.onShow;
            SessionSearchToolset bad{ missing };
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw, "toolset: the catalog callback is required");

        const auto servers = toolset.Servers();
        CHECK(servers.size() == 2 && servers[0].name == L"sessions" && servers[1].name == L"display", "toolset: two servers, one to search and one to show");
        CHECK(servers[0].tools.size() == 1 && servers[0].tools[0].name == L"search" && servers[0].tools[0].readOnly && servers[1].tools.size() == 1 && servers[1].tools[0].name == L"show" && !servers[1].tools[0].readOnly, "toolset: search is read-only (may run in parallel), show is not");
        CHECK(json::Parse(servers[0].tools[0].inputSchemaJson).has_value() && json::Parse(servers[1].tools[0].inputSchemaJson).has_value(), "toolset: both input schemas are JSON");
        CHECK(servers[0].tools[0].inputSchemaJson.find(L"Default " + std::to_wstring(kSessionSearchDefaultDays)) != std::wstring::npos, "toolset: the schema states the real default period");
        const std::wstring prompt = SessionSearchToolset::SystemPrompt();
        bool promptAscii = !prompt.empty();
        for (const wchar_t c : prompt)
        {
            promptAscii = promptAscii && (c == L'\n' || (c >= 0x20 && c < 0x7F));
        }
        CHECK(promptAscii && prompt.find(L"mcp__sessions__search") != std::wstring::npos && prompt.find(L"mcp__display__show") != std::wstring::npos && prompt.find(L"indexing") != std::wstring::npos, "toolset: the system prompt is ASCII and names both tools and the indexing note");
        const auto& search = servers[0].tools[0].handler;
        const auto& show = servers[1].tools[0].handler;

        const HarnessToolResult found = search(J(LR"({"query":"drag","days":365})"));
        CHECK(!found.isError && askedSince == kNow - 365 * kDay, "search: runs, and asks for a catalog of its own period");
        CHECK(searching.size() == 1 && searching[0].find(L"drag") != std::wstring::npos, "search: the page is told what is searched");
        CHECK(candidates.size() == 1 && candidates[0].size() == 4 && candidates[0][0] == kIdDrag, "search: the page gets the rows as full ids, in result order");
        const json::Value foundJson = J(found.text);
        const auto* rows = foundJson.Find(L"sessions");
        CHECK(rows && rows->arr.size() == 4 && rows->arr[0].StrAt(L"id") == L"aaaa1111", "search: the model reads short ids");
        CHECK(toolset.ResolveHandle(L"aaaa1111") == kIdDrag && toolset.ResolveHandle(L" AAAA1111 ") == kIdDrag && toolset.ResolveHandle(kIdTags) == kIdTags && toolset.ResolveHandle(L"bbbb2222-0000") == kIdTags, "short ids resolve: as given, in any case, as a whole id, as a longer prefix");
        CHECK(toolset.ResolveHandle(L"ffff9999").empty() && toolset.ResolveHandle(L"").empty(), "an id that no search returned resolves to nothing");

        // The twin shares "aaaa1111": its short id must be longer, and both must resolve.
        const HarnessToolResult twins = search(J(LR"({"dir":"Agentmaster","days":3})"));
        const json::Value twinsJson = J(twins.text);
        const auto* twinRows = twinsJson.Find(L"sessions");
        CHECK(twinRows && twinRows->arr.size() == 2 && twinRows->arr[0].StrAt(L"id") == L"aaaa1111" && twinRows->arr[1].StrAt(L"id").size() > 8 && twinRows->arr[1].StrAt(L"id").rfind(L"aaaa1111", 0) == 0, "a second session with the same first 8 characters gets a longer short id");
        CHECK(twinRows && twinRows->arr.size() == 2 && toolset.ResolveHandle(twinRows->arr[1].StrAt(L"id")) == kIdDragTwin && toolset.ResolveHandle(L"aaaa1111") == kIdDrag, "both twins resolve to their own session");

        const HarnessToolResult shownOk = show(J(LR"({"ids":["bbbb2222","aaaa1111","bbbb2222","nope1234"],"note":"  the drag\nsessions "})"));
        CHECK(!shownOk.isError && shown.size() == 1 && shown[0].size() == 2 && shown[0][0] == kIdTags && shown[0][1] == kIdDrag, "show: full ids, the model's order, each session once");
        CHECK(notes.back() == L"the drag sessions" && shownOk.text.find(L"Shown 2 sessions") != std::wstring::npos && shownOk.text.find(L"nope1234") != std::wstring::npos, "show: a one-line caption, and the unknown id is reported back");
        const HarnessToolResult top = show(J(LR"({"top":1})"));
        CHECK(!top.isError && shown.back().size() == 1 && shown.back()[0] == kIdDrag, "show top:N takes the first rows of the LATEST search");
        const HarnessToolResult nothing = show(J(LR"({"ids":[]})"));
        CHECK(!nothing.isError && shown.back().empty() && nothing.text.find(L"Shown 0 sessions") != std::wstring::npos, "show with an empty list shows nothing (the model's \"nothing fits\")");
        const size_t showsBefore = shown.size();
        CHECK(show(J(LR"({"ids":["nope1234","zzzz0000"]})")).isError && shown.size() == showsBefore, "show with only unknown ids is an error and leaves the screen alone");
        CHECK(show(J(L"{}")).isError && show(J(LR"({"top":0})")).isError && shown.size() == showsBefore, "show without ids or top is an error");

        CHECK(search(J(LR"({"limit":"many","query":3})")).isError, "search: unusable arguments are an error result, not an exception");

        // A request that is not wanted any more: the search must leave no trace.
        const size_t candidatesBefore = candidates.size();
        SessionSearchToolset::Callbacks racing = callbacks;
        racing.catalog = [&](int64_t, const std::function<bool()>& cancelled) {
            ++generation; // the user asked something else while the catalog was fetched
            CHECK(cancelled && cancelled(), "the catalog callback is told that the search is cancelled");
            return shared;
        };
        racing.onCandidates = [&candidates](const std::vector<std::wstring>& ids) { candidates.push_back(ids); };
        SessionSearchToolset racingToolset{ racing };
        const HarnessToolResult cancelledResult = racingToolset.Servers()[0].tools[0].handler(J(LR"({"query":"drag"})"));
        CHECK(cancelledResult.isError && cancelledResult.text.find(L"cancelled") != std::wstring::npos && candidates.size() == candidatesBefore, "a cancelled search is an error result and tells the page nothing");
        CHECK(racingToolset.ResolveHandle(L"aaaa1111").empty(), "... and records no short id");

        giveCatalog = false;
        const HarnessToolResult noIndex = search(J(LR"({"query":"drag"})"));
        CHECK(noIndex.isError && noIndex.text.find(L"still being read") != std::wstring::npos, "search without any index says that it is still being read");
        giveCatalog = true;

        toolset.Reset();
        CHECK(toolset.ResolveHandle(L"aaaa1111").empty() && show(J(LR"({"top":3})")).isError == false && shown.back().empty(), "Reset forgets the short ids and the latest search");
    }

    // ---- GatherSessionCatalog on scratch folders ----
    {
        wchar_t temp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path root = std::filesystem::path{ temp } / L"agentmaster-m5-catalog";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        const std::filesystem::path projects = root / L"projects" / L"K--proj";
        const std::filesystem::path index = root / L"index";
        std::filesystem::create_directories(projects, ec);
        std::filesystem::create_directories(index, ec);
        // Six sessions, one per day back from now: file i was last written i days ago.
        constexpr int kCount = 6;
        const int64_t wallNow = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::vector<std::wstring> ids;
        std::vector<int64_t> mtimes;
        // One transcript line: a prompt the user typed.
        const auto userLine = [](const std::wstring& id, const std::string& prompt, const std::string& uuid) {
            return "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"" + prompt +
                   "\"},\"timestamp\":\"2026-10-01T10:00:00.000Z\",\"cwd\":\"K:\\\\proj\",\"sessionId\":\"" + HarnessToUtf8(id) + "\",\"uuid\":\"" + uuid + "\"}\n";
        };
        bool wrote = true;
        for (int i = 0; i < kCount; ++i)
        {
            const std::wstring id = L"0000000" + std::to_wstring(i) + L"-1111-4222-8333-444444444444";
            ids.push_back(id);
            mtimes.push_back(wallNow - i * kDay - 60000);
            wrote = wrote && WriteFileWithMtime(projects / (id + L".jsonl"), userLine(id, "prompt number " + std::to_string(i), "u" + std::to_string(i)), mtimes.back());
        }
        CHECK(wrote, "the scratch transcripts were written");
        SessionCatalogSources sources;
        sources.projectsDir = (root / L"projects").wstring();
        sources.indexDir = index.wstring();
        const int64_t since = wallNow - 30 * kDay;

        // A reader that allows `allowed` sidecar reads, then cancels.
        const auto readsThenCancel = [](int allowed) {
            auto left = std::make_shared<int>(allowed);
            return std::function<bool()>{ [left] { return (*left)-- <= 0; } };
        };

        SessionCatalogGatherOptions two;
        two.cancelled = readsThenCancel(2);
        const auto partial = GatherSessionCatalog(since, sources, two);
        CHECK(partial->entries.size() == 2 && partial->entries[0].sessionId == ids[0] && partial->entries[1].sessionId == ids[1], "a gather that stops early holds the NEWEST sessions");
        CHECK(!partial->Complete() && partial->sinceUnixMs == since, "... and is not complete");
        // The first session left out is #2: everything changed after it is in.
        CHECK(partial->coveredSinceUnixMs > mtimes[2] && partial->coveredSinceUnixMs <= mtimes[1], "... and says how far back it reaches: just after the newest session it left out");

        SessionCatalogGatherOptions none;
        none.cancelled = [] { return true; };
        none.previous = partial;
        const auto reused = GatherSessionCatalog(since, sources, none);
        CHECK(reused->entries.size() == 2 && reused->coveredSinceUnixMs == partial->coveredSinceUnixMs, "what the catalog before had is taken without a read, even by a cancelled gather");

        // Steps with a budget: each reads at least one new session, so they reach the end.
        std::shared_ptr<const SessionCatalog> step = partial;
        int steps = 0;
        bool grew = true;
        while (!step->Complete() && steps < kCount + 2)
        {
            SessionCatalogGatherOptions next;
            next.previous = step;
            next.budgetMs = 1;
            const auto after = GatherSessionCatalog(since, sources, next);
            grew = grew && after->entries.size() > step->entries.size();
            step = after;
            ++steps;
        }
        CHECK(step->Complete() && step->entries.size() == kCount && grew, "steps with a budget each read at least one more session, and end complete");
        CHECK(steps >= 1 && steps <= kCount - 2, "... in no more steps than there were sessions left");
        CHECK(step->coveredSinceUnixMs == since && SessionSearchTitleOf(step->entries[5]) == L"prompt number 5" && SessionSearchDirOf(step->entries[0]) == L"K:\\proj", "a complete catalog covers its whole period, and its entries are real index entries");

        const auto whole = GatherSessionCatalog(since, sources);
        CHECK(whole->Complete() && whole->entries.size() == kCount && whole->entries[0].sessionId == ids[0] && whole->entries[5].sessionId == ids[5], "a gather without a budget reads the whole period, newest first");
        CHECK(whole->storeOverlay, "a gather without a budget always reads the session store (titles, tags, favorites)");
        // That read made the store cheap to read: a gather with a budget takes it too from now on.
        SessionCatalogGatherOptions bounded;
        bounded.previous = whole;
        bounded.budgetMs = 50;
        CHECK(GatherSessionCatalog(since, sources, bounded)->storeOverlay, "once the store was read, a gather with a budget has the stored titles, tags and favorites");
        const auto window = GatherSessionCatalog(wallNow - 2 * kDay - kDay / 2, sources);
        CHECK(window->Complete() && window->entries.size() == 3, "the period bounds what is listed");

        // A transcript that GREW (a transcript is only ever appended to) is read again; the
        // others are still taken from the catalog before.
        wrote = WriteFileWithMtime(projects / (ids[3] + L".jsonl"), userLine(ids[3], "prompt number 3", "u3") + userLine(ids[3], "a second prompt", "n1"), wallNow - 1000);
        SessionCatalogGatherOptions refresh;
        refresh.previous = whole;
        refresh.cancelled = readsThenCancel(1); // exactly the one changed session may be read
        const auto refreshed = GatherSessionCatalog(since, sources, refresh);
        CHECK(wrote && refreshed->Complete() && refreshed->entries.size() == kCount && refreshed->entries[0].sessionId == ids[3] && refreshed->entries[0].stats.userPrompts == 2 && whole->entries[3].stats.userPrompts == 1, "a refresh reads only the session that changed (it is the newest now)");

        // The open sessions: the flag for the search's open filter, and the tab's title.
        SessionCatalogSources withOpen = sources;
        withOpen.openSessions = [&ids] {
            return std::vector<std::pair<std::wstring, std::wstring>>{ { ids[1], L"my tab name" }, { L"not-on-disk", L"x" } };
        };
        const auto open = GatherSessionCatalog(since, withOpen);
        bool titled = false;
        for (const auto& e : open->entries)
        {
            titled = titled || (e.sessionId == ids[1] && SessionSearchTitleOf(e) == L"my tab name");
        }
        CHECK(open->openIds.count(ids[1]) == 1 && titled, "an open session is marked open and shows its tab's title");
        std::filesystem::remove_all(root, ec);
    }
}
