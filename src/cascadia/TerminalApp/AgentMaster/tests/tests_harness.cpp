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
//   the pair without which Claude Code never compacts - the retry bound, the effort), the
//   PERMISSION POLICY (bypass always, auto when bypass is not allowed, then dontAsk: the mode
//   words, the step for each mode a process can report, which start failures are a refused mode,
//   the test seam's two settings switches), the child's environment (the harness mark, the
//   removals of a parent session's variables), and the readers of Claude Code's own lines, fed
//   the shapes captured from Claude Code 2.1.291 (init with its whole inventory, a refused tool
//   call, the three compaction lines, a result whose failure flag is is_error). Plus the Fleet
//   Observer's side of the harness mark: ParseClaudeFacts reads AM_HARNESS.
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
        // Measured 2026-10-14: with the variable set, every live effort change answered "success"
        // and the effort stayed where the variable put it. The --effort flag carries the start value.
        CHECK(env && env->Find(L"CLAUDE_CODE_EFFORT_LEVEL") == nullptr, "settings: NO effort variable (it would override every live effort change)");
        CHECK(env && env->StrAt(L"DISABLE_AUTOUPDATER") == L"1" && env->StrAt(L"CLAUDE_CODE_DISABLE_CLAUDE_MDS") == L"1" && env->StrAt(L"CLAUDE_CODE_DISABLE_AUTO_MEMORY") == L"1", "settings: a quiet background child");
        const auto* extraBody = env ? env->Find(L"CLAUDE_CODE_EXTRA_BODY") : nullptr;
        CHECK(extraBody && extraBody->type == json::Value::Type::Str && extraBody->str.empty(), "settings: CLAUDE_CODE_EXTRA_BODY is written EMPTY when no body is wanted (the harness owns the variable)");

        // "Off" on a model that refuses "disabled": the API's own between_tools rides the
        // variable (measured 2026-10-14: Claude Code spreads it over every request body).
        HarnessSettingsSpec off = spec;
        off.extraRequestBody = std::wstring{ kHarnessBetweenToolsRequestBody };
        const std::wstring offText = BuildHarnessSettingsJson(off);
        bool offAscii = true;
        for (const wchar_t c : offText)
        {
            offAscii = offAscii && c >= 0x20 && c < 0x7F;
        }
        const json::Value offSettings = J(offText);
        const auto* offEnv = offSettings.Find(L"env");
        const std::wstring bodyText = offEnv ? offEnv->StrAt(L"CLAUDE_CODE_EXTRA_BODY") : std::wstring{};
        const json::Value body = J(bodyText);
        const auto* bodyThinking = body.Find(L"thinking");
        CHECK(offAscii && bodyThinking && bodyThinking->StrAt(L"type") == L"between_tools" && body.members.size() == 1, "settings: the between_tools body is the variable's value, a JSON object with the one `thinking` field, still plain ASCII");

        // The live patch carries the COMPLETE env object (apply_flag_settings merges shallow: its
        // "env" replaces the whole env object of the --settings layer).
        const json::Value patch = J(BuildHarnessEnvPatchJson(off));
        const auto* patchEnv = patch.Find(L"env");
        CHECK(patch.members.size() == 1 && patchEnv && offEnv && json::Dump(*patchEnv) == json::Dump(*offEnv), "env patch: {\"env\": ...} alone, the same env object the start writes (retry bound and quiet switches included)");
        const json::Value patchBack = J(BuildHarnessEnvPatchJson(spec));
        const auto* patchBackEnv = patchBack.Find(L"env");
        CHECK(patchBackEnv && patchBackEnv->Find(L"CLAUDE_CODE_EXTRA_BODY") && patchBackEnv->StrAt(L"CLAUDE_CODE_EXTRA_BODY").empty() && patchBackEnv->StrAt(L"CLAUDE_CODE_MAX_RETRIES") == L"2", "env patch back to 'on': the variable is written empty (it must overwrite between_tools), the rest unchanged");

        HarnessSettingsSpec none;
        none.apiMaxRetries = 99;
        const json::Value plain = J(BuildHarnessSettingsJson(none));
        CHECK(plain.Find(L"autoCompactEnabled") == nullptr && plain.Find(L"autoCompactWindow") == nullptr, "settings: no window asked, no compaction keys");
        const auto* plainEnv = plain.Find(L"env");
        CHECK(plainEnv && plainEnv->StrAt(L"CLAUDE_CODE_MAX_RETRIES") == L"15", "settings: the retry count is clamped");
        HarnessSettingsSpec tiny;
        tiny.autoCompactTokens = 5;
        CHECK(J(BuildHarnessSettingsJson(tiny)).I64At(L"autoCompactWindow") == 100000, "settings: the window is clamped into Claude Code's range");
        CHECK(ClampHarnessAutoCompactTokens(0) == 0 && ClampHarnessAutoCompactTokens(1) == 100000 && ClampHarnessAutoCompactTokens(500000) == 500000 && ClampHarnessAutoCompactTokens(5000000) == 1000000 && ClampHarnessAutoCompactTokens(-7) == 0, "the auto-compact clamp: 0 (and less) means no window, anything else 100,000..1,000,000");
    }

    // ---- the permission policy: bypass always, auto when bypass is not allowed, then dontAsk ----
    // Every text and every mode word below is what Claude Code 2.1.291 really wrote (the
    // permission matrix of 2026-10-12: sixteen starts of the real claude.exe).
    {
        CHECK(std::wstring{ HarnessPermissionModeWord(HarnessPermissionMode::Bypass) } == L"bypassPermissions" && std::wstring{ HarnessPermissionModeWord(HarnessPermissionMode::Auto) } == L"auto" && std::wstring{ HarnessPermissionModeWord(HarnessPermissionMode::DontAsk) } == L"dontAsk", "permission: the three mode words are Claude Code's own");
        CHECK(NextHarnessPermissionMode(HarnessPermissionMode::Bypass) == HarnessPermissionMode::Auto && NextHarnessPermissionMode(HarnessPermissionMode::Auto) == HarnessPermissionMode::DontAsk && !NextHarnessPermissionMode(HarnessPermissionMode::DontAsk).has_value(), "permission: the order is bypass, auto, dontAsk, and nothing is lower");

        // What the process reports, and what the harness then does.
        CHECK(DecideHarnessPermissionStep(HarnessPermissionMode::Bypass, L"bypassPermissions") == HarnessPermissionStep::Keep, "permission: bypass was asked and is in effect: keep");
        CHECK(DecideHarnessPermissionStep(HarnessPermissionMode::Bypass, L"auto") == HarnessPermissionStep::Keep, "permission: bypass was asked, Claude Code chose auto (a policy forbids bypass): keep - that is the rule's second half");
        CHECK(DecideHarnessPermissionStep(HarnessPermissionMode::Bypass, L"default") == HarnessPermissionStep::SwitchToAuto, "permission: bypass was asked, 'default' is in effect (it can ask): switch to auto");
        CHECK(DecideHarnessPermissionStep(HarnessPermissionMode::Bypass, L"plan") == HarnessPermissionStep::SwitchToAuto && DecideHarnessPermissionStep(HarnessPermissionMode::Bypass, L"acceptEdits") == HarnessPermissionStep::SwitchToAuto && DecideHarnessPermissionStep(HarnessPermissionMode::Bypass, L"someFutureMode") == HarnessPermissionStep::SwitchToAuto, "permission: every other mode word can ask, a word this build does not know included");
        CHECK(DecideHarnessPermissionStep(HarnessPermissionMode::Bypass, L"") == HarnessPermissionStep::Keep && DecideHarnessPermissionStep(HarnessPermissionMode::Auto, L"") == HarnessPermissionStep::Keep, "permission: no report at start (an older Claude Code): keep, and read the mode at the first turn - a blind switch could lower a process that runs in bypass");
        CHECK(DecideHarnessPermissionStep(HarnessPermissionMode::Auto, L"auto") == HarnessPermissionStep::Keep && DecideHarnessPermissionStep(HarnessPermissionMode::Auto, L"default") == HarnessPermissionStep::SwitchToDontAsk, "permission: auto was asked and refused ('default' is in effect): go to dontAsk, auto cannot work");
        CHECK(DecideHarnessPermissionStep(HarnessPermissionMode::DontAsk, L"dontAsk") == HarnessPermissionStep::Keep && DecideHarnessPermissionStep(HarnessPermissionMode::DontAsk, L"default") == HarnessPermissionStep::SwitchToDontAsk, "permission: the last mode keeps itself, and is asked for again when it is not in effect");
        CHECK(HarnessPermissionModeNeverAsks(L"bypassPermissions") && HarnessPermissionModeNeverAsks(L"auto") && HarnessPermissionModeNeverAsks(L"dontAsk") && !HarnessPermissionModeNeverAsks(L"default") && !HarnessPermissionModeNeverAsks(L"manual") && !HarnessPermissionModeNeverAsks(L"acceptEdits") && !HarnessPermissionModeNeverAsks(L"plan") && !HarnessPermissionModeNeverAsks(L"") && !HarnessPermissionModeNeverAsks(L"BypassPermissions"), "permission: three modes never ask a person; the compare is exact (Claude Code's own spelling)");

        // A start that Claude Code REFUSED because of the mode (it ends with an error text).
        CHECK(HarnessStartFailureNamesPermissionMode(L"Claude Code could not be started: The Claude Code process ended. (Error: bypassPermissions not supported in restricted mode)"), "permission: --restricted refuses bypass with an error at start");
        CHECK(HarnessStartFailureNamesPermissionMode(L"Claude Code could not be started: The Claude Code process ended. (error: option '--permission-mode <mode>' argument 'auto' is invalid. Allowed choices are acceptEdits, bypassPermissions, default, plan.)"), "permission: a Claude Code that does not know a mode word refuses it at start");
        CHECK(HarnessStartFailureNamesPermissionMode(L"--dangerously-skip-permissions cannot be used with root/sudo privileges for security reasons") && HarnessStartFailureNamesPermissionMode(L"Bypass Permissions mode is disabled by your organization") && HarnessStartFailureNamesPermissionMode(L"AUTO MODE is not available for this account"), "permission: the other wordings of a refused mode, in any letter case");
        CHECK(!HarnessStartFailureNamesPermissionMode(L"Claude Code (claude.exe) was not found.") && !HarnessStartFailureNamesPermissionMode(L"Claude Code could not be started: Access is denied. (permission denied: C:\\x\\claude.exe)") && !HarnessStartFailureNamesPermissionMode(L"error: unknown option '--autocompact'") && !HarnessStartFailureNamesPermissionMode(L"") && !HarnessStartFailureNamesPermissionMode(L"automatic update failed"), "permission: any other start failure is NOT a refused mode ('permission denied' is a file error) - no second start for it");

        // The test seam writes the two switches of an administrator's policy; a product launch writes none.
        HarnessSettingsSpec product;
        product.autoCompactTokens = 500000;
        CHECK(J(BuildHarnessSettingsJson(product)).Find(L"permissions") == nullptr, "settings: a product launch writes no permissions key (the mode is asked for on the command line)");
        HarnessSettingsSpec noBypass;
        noBypass.disableBypassPermissions = true;
        const json::Value noBypassJson = J(BuildHarnessSettingsJson(noBypass));
        const auto* noBypassPermissions = noBypassJson.Find(L"permissions");
        CHECK(noBypassPermissions && noBypassPermissions->StrAt(L"disableBypassPermissionsMode") == L"disable" && noBypassPermissions->Find(L"disableAutoMode") == nullptr, "settings (test seam): bypass disabled, auto untouched");
        HarnessSettingsSpec neither;
        neither.disableBypassPermissions = true;
        neither.disableAutoMode = true;
        const json::Value neitherJson = J(BuildHarnessSettingsJson(neither));
        const auto* neitherPermissions = neitherJson.Find(L"permissions");
        CHECK(neitherPermissions && neitherPermissions->StrAt(L"disableBypassPermissionsMode") == L"disable" && neitherPermissions->StrAt(L"disableAutoMode") == L"disable", "settings (test seam): both modes disabled");
    }

    // ---- the thinking policy: the levels, the toggle's rotation, the live-change plan, the model's report ----
    {
        using TL = HarnessThinkingLevel;
        const TL all[] = { TL::Off, TL::Low, TL::Medium, TL::High, TL::XHigh, TL::Max };
        bool roundTrip = true;
        for (const TL level : all)
        {
            const auto back = ParseHarnessThinkingLevel(HarnessThinkingLevelWord(level));
            roundTrip = roundTrip && back.has_value() && *back == level;
        }
        CHECK(roundTrip, "thinking: every level's word reads back as that level (the persisted form)");
        CHECK(std::wstring{ HarnessThinkingLevelWord(TL::Off) } == L"off" && std::wstring{ HarnessThinkingLevelWord(TL::XHigh) } == L"xhigh" && std::wstring{ HarnessThinkingLevelWord(TL::Max) } == L"max", "thinking: the words are 'off' and Claude Code's effort words");
        CHECK(ParseHarnessThinkingLevel(L"  HIGH\t") == TL::High && ParseHarnessThinkingLevel(L"Off") == TL::Off, "thinking: a word is read without regard to case and surrounding spaces");
        CHECK(!ParseHarnessThinkingLevel(L"").has_value() && !ParseHarnessThinkingLevel(L"ultra").has_value() && !ParseHarnessThinkingLevel(L"hi gh").has_value(), "thinking: any other word is no level (the caller reads it as off)");
        CHECK(!HarnessThinkingLevelThinks(TL::Off) && HarnessThinkingLevelThinks(TL::Low) && HarnessThinkingLevelThinks(TL::Max), "thinking: only Off runs without thinking");
        CHECK(std::wstring{ HarnessThinkingLevelEffort(TL::Off) } == L"low" && std::wstring{ HarnessThinkingLevelEffort(TL::Medium) } == L"medium" && std::wstring{ HarnessThinkingLevelEffort(TL::Max) } == L"max", "thinking: Off runs at effort low (the fastest), every other level at its own effort");

        // The rotation. An empty list = no process reported yet: every level is offered.
        const std::vector<std::wstring> unknown;
        bool rotation = true;
        TL at = TL::Off;
        for (const TL expected : { TL::Low, TL::Medium, TL::High, TL::XHigh, TL::Max, TL::Off })
        {
            at = NextHarnessThinkingLevel(at, unknown);
            rotation = rotation && at == expected;
        }
        CHECK(rotation, "thinking: the toggle rotates off -> low -> medium -> high -> xhigh -> max -> off");
        const std::vector<std::wstring> noXHighNoMax{ L"low", L"medium", L"high" };
        CHECK(NextHarnessThinkingLevel(TL::High, noXHighNoMax) == TL::Off && NextHarnessThinkingLevel(TL::Medium, noXHighNoMax) == TL::High, "thinking: a level whose effort the model lacks is skipped (Claude Code would step it down anyway)");
        const std::vector<std::wstring> onlyMax{ L"MAX" };
        CHECK(NextHarnessThinkingLevel(TL::Off, onlyMax) == TL::Max && NextHarnessThinkingLevel(TL::Max, onlyMax) == TL::Off, "thinking: Off is always offered, so the rotation ends; the model's words are compared caselessly");
        const std::vector<std::wstring> none{ L"auto" };
        CHECK(NextHarnessThinkingLevel(TL::Off, none) == TL::Off, "thinking: a model with no known effort word leaves only Off");

        // The live-change plan (the process was started --thinking adaptive). St(thinks,
        // betweenTools, effort) builds one side.
        const auto St = [](bool thinks, bool betweenTools, const wchar_t* effort) {
            HarnessReasoningState state;
            state.thinks = thinks;
            state.betweenTools = betweenTools;
            state.effort = effort;
            return state;
        };
        CHECK(PlanHarnessThinkingChange(St(true, false, L"low"), St(true, false, L"low")).Empty(), "plan: nothing differs, nothing is sent (the case before almost every turn)");
        const HarnessThinkingChange off = PlanHarnessThinkingChange(St(true, false, L"high"), St(false, true, L"low"));
        CHECK(off.thinkingOff && off.betweenToolsOn && !off.thinkingOn && !off.betweenToolsOff && off.effort == L"low", "plan: high -> off on Sonnet 5.5 sends set_max_thinking_tokens 0, the between_tools body, and the effort back to low");
        const HarnessThinkingChange on = PlanHarnessThinkingChange(St(false, true, L"low"), St(true, false, L"max"));
        CHECK(on.betweenToolsOff && on.thinkingOn && !on.thinkingOff && !on.betweenToolsOn && on.effort == L"max", "plan: off -> max clears the body and sends set_max_thinking_tokens null (back to adaptive) and effort max");
        const HarnessThinkingChange olderOn = PlanHarnessThinkingChange(St(false, false, L"low"), St(true, false, L"max"));
        CHECK(olderOn.thinkingOn && !olderOn.betweenToolsOff && !olderOn.betweenToolsOn && olderOn.effort == L"max", "plan: on a model that takes 'disabled' (no body), off -> max moves the thinking and the effort only");
        const HarnessThinkingChange olderOff = PlanHarnessThinkingChange(St(true, false, L"low"), St(false, false, L"low"));
        CHECK(olderOff.thinkingOff && !olderOff.betweenToolsOn && olderOff.effort.empty(), "plan: on such a model, 'off' is set_max_thinking_tokens 0 alone (Claude Code sends 'disabled' itself)");
        const HarnessThinkingChange refined = PlanHarnessThinkingChange(St(false, false, L"low"), St(false, true, L"low"));
        CHECK(refined.betweenToolsOn && !refined.thinkingOff && !refined.thinkingOn && refined.effort.empty(), "plan: off stays off, but the resolved model refuses 'disabled': only the body is added");
        CHECK(PlanHarnessThinkingChange(St(true, false, L"low"), St(true, true, L"low")).Empty(), "plan: a wanted body with thinking ON is read as no body (the API refuses that pair)");
        const HarnessThinkingChange deeper = PlanHarnessThinkingChange(St(true, false, L"medium"), St(true, false, L"high"));
        CHECK(!deeper.thinkingOff && !deeper.thinkingOn && !deeper.betweenToolsOn && !deeper.betweenToolsOff && deeper.effort == L"high", "plan: medium -> high changes the effort only");
        CHECK(PlanHarnessThinkingChange(St(true, false, L"high"), St(true, false, L"")).Empty() && PlanHarnessThinkingChange(St(true, false, L"high"), St(true, false, L"ultra")).Empty(), "plan: an empty or unknown wanted effort keeps the applied one (never sent)");
        CHECK(IsHarnessEffortWord(L"xhigh") && !IsHarnessEffortWord(L"High") && !IsHarnessEffortWord(L"off") && !IsHarnessEffortWord(L""), "thinking: only Claude Code's exact effort words go to it");

        // Which models refuse "disabled" — Claude Code 2.1.291's rule (qEe), mirrored.
        bool refuses = true;
        for (const wchar_t* model : { L"claude-sonnet-5-5", L"CLAUDE-SONNET-5-5", L"claude-sonnet-5-5[1m]", L"claude-opus-5-5", L"claude-fable-5-1", L"claude-sonnet-6", L"sonnet", L"" })
        {
            refuses = refuses && HarnessModelRefusesDisabledThinking(model);
        }
        CHECK(refuses, "refuses 'disabled': Sonnet 5.5, Opus 5.5, Fable 5.1, an unlisted future model, an unresolved alias and an empty name (Claude Code's default for what it does not list)");
        bool takes = true;
        for (const wchar_t* model : { L"claude-sonnet-4-6", L"claude-sonnet-4-5-20250929", L"claude-opus-4-1-20250805", L"claude-opus-4-20250514", L"claude-sonnet-4-20250514", L"claude-3-7-sonnet-20250219", L"claude-3-5-haiku-latest", L"claude-haiku-4-5", L"claude-haiku-4-5-20251001", L"claude-opus-5", L"claude-sonnet-5", L"claude-opus-4-8", L"us.anthropic.claude-sonnet-4-5-20250929-v1:0", L"claude-sonnet-4-5@20250929", L"claude-opus-4-6[1m]", L" Claude-Sonnet-4-6 " })
        {
            takes = takes && !HarnessModelRefusesDisabledThinking(model);
        }
        CHECK(takes, "takes 'disabled': every model Claude Code lists, by its dated, Bedrock, Vertex and [1m] names, in any letter case");
        const json::Value betweenTools = J(std::wstring{ kHarnessBetweenToolsRequestBody });
        const auto* betweenToolsThinking = betweenTools.Find(L"thinking");
        CHECK(betweenToolsThinking && betweenToolsThinking->StrAt(L"type") == L"between_tools", "the between_tools body is the API's own off: {\"thinking\": {\"type\": \"between_tools\"}}");

        // The model's report, in the shape Claude Code 2.1.291 answered initialize with (2026-10-14).
        const std::wstring models = LR"([{"value":"default","resolvedModel":"claude-opus-5-5","displayName":"Default","supportsEffort":true,"supportedEffortLevels":["low","medium","high","xhigh","max"]},)"
                                    LR"({"value":"sonnet","resolvedModel":"claude-sonnet-5-5","displayName":"Sonnet","supportsEffort":true,"supportedEffortLevels":["low","medium","high","xhigh","max"],"supportsAdaptiveThinking":true},)"
                                    LR"({"value":"haiku","resolvedModel":"claude-haiku-4-5","supportsEffort":false}])";
        const HarnessModelReasoning sonnet = ReadHarnessModelReasoning(models, L"claude-sonnet-5-5");
        // The expected lists are named outside CHECK: a braced list's commas would split the macro's argument.
        const std::vector<std::wstring> fiveLevels{ L"low", L"medium", L"high", L"xhigh", L"max" };
        CHECK(sonnet.known && sonnet.adaptiveThinking && sonnet.supportedEfforts == fiveLevels, "model report: Sonnet 5.5 is found by its resolved id, with its five effort levels and adaptive thinking");
        const HarnessModelReasoning alias = ReadHarnessModelReasoning(models, L"SONNET");
        CHECK(alias.known && alias.resolvedModel == L"claude-sonnet-5-5", "model report: the alias row's own value matches too, without regard to case, and names the wire id it resolves to");
        CHECK(sonnet.resolvedModel == L"claude-sonnet-5-5" && HarnessModelRefusesDisabledThinking(alias.resolvedModel), "model report: the resolved id is what the 'disabled' rule is asked about (the alias alone names no version)");
        const HarnessModelReasoning haiku = ReadHarnessModelReasoning(models, L"claude-haiku-4-5");
        CHECK(haiku.known && haiku.supportedEfforts.empty() && !haiku.adaptiveThinking, "model report: a model without effort levels lists none (the toggle then offers only off)");
        CHECK(!ReadHarnessModelReasoning(models, L"claude-unknown").known && !ReadHarnessModelReasoning(L"{\"models\":[]}", L"claude-sonnet-5-5").known && !ReadHarnessModelReasoning(L"not json", L"claude-sonnet-5-5").known && !ReadHarnessModelReasoning(models, L"").known, "model report: an unknown model, a text that is not the array, and an empty model name know nothing");
        const HarnessModelReasoning odd = ReadHarnessModelReasoning(LR"([{"value":"x","supportedEffortLevels":["LOW",7,"ultra","max"]}])", L"x");
        const std::vector<std::wstring> lowAndMax{ L"low", L"max" };
        CHECK(odd.supportedEfforts == lowAndMax, "model report: levels are lower-cased; non-strings and unknown words are dropped");
        CHECK(odd.resolvedModel == L"x", "model report: a row without resolvedModel (an older Claude Code) names its value as the wire id");

        // The stream's own sign that a turn thought (the line Claude Code 2.1.291 wrote while
        // Sonnet 5.5 thought at effort high, 2026-10-14).
        const HarnessSystemFacts thought = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"thinking_tokens","estimated_tokens":207,"estimated_tokens_delta":157,"session_id":"2c626760","uuid":"5265d8b7"})");
        CHECK(thought.kind == HarnessSystemKind::ThinkingTokens && thought.thinkingTokens == 207 && thought.thinkingTokensDelta == 157, "system line: thinking_tokens carries Claude Code's estimate of the thinking so far and since the line before");
        const HarnessSystemFacts negative = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"thinking_tokens","estimated_tokens":-1,"estimated_tokens_delta":-5})");
        CHECK(negative.kind == HarnessSystemKind::ThinkingTokens && negative.thinkingTokens == 0 && negative.thinkingTokensDelta == 0, "system line: a negative estimate counts as none");
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
        bool thinkingRemovals = true;
        for (const char* name : { "CLAUDE_CODE_EXTRA_BODY", "CLAUDE_CODE_DISABLE_THINKING", "CLAUDE_CODE_DISABLE_ADAPTIVE_THINKING", "DISABLE_INTERLEAVED_THINKING", "MAX_THINKING_TOKENS" })
        {
            thinkingRemovals = thinkingRemovals && get(name) && get(name)->empty();
        }
        CHECK(thinkingRemovals, "environment: the variables that would change the thinking level are REMOVED too (the request body is set by the harness's own settings)");
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

        // The whole inventory line, as a harness process in bypass mode wrote it (2026-10-12; the
        // working folder, the session id and the socket path replaced): what the model can use.
        const auto full = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"init","cwd":"C:\\w","session_id":"f53e","tools":["mcp__display__show","mcp__sessions__search"],"mcp_servers":[{"name":"display","status":"connected","source":"sdk"},{"name":"sessions","status":"connected","source":"sdk"}],"model":"claude-sonnet-5-5","permissionMode":"bypassPermissions","slash_commands":[],"apiKeySource":"none","claude_code_version":"2.1.291","output_style":"default","agents":["claude","Explore","general-purpose","Plan","statusline-setup"],"skills":[],"plugins":[{"name":"cc-plugin-agents-md","path":"builtin","source":"cc-plugin-agents-md@builtin"},{"name":"cc-plugin-plugin-authoring","path":"builtin","source":"cc-plugin-plugin-authoring@builtin"}],"analytics_disabled":true,"fast_mode_state":"off"})");
        CHECK(full.kind == HarnessSystemKind::Init && full.permissionMode == L"bypassPermissions" && full.apiKeySource == L"none" && full.outputStyle == L"default", "system/init: the permission mode in effect, the credential source, the output style");
        CHECK(full.tools.size() == 2 && full.slashCommands.empty() && full.skills.empty(), "system/init: two tools, no slash command, no skill (the harness's launch policy)");
        CHECK(full.agents.size() == 5 && full.agents[1] == L"Explore", "system/init: the agent types that EXIST (listed even though the model has no tool to start one)");
        CHECK(full.plugins.size() == 2 && full.plugins[0] == L"cc-plugin-agents-md", "system/init: the plugins, by the name of each object");
        const auto mixed = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"init","skills":["review",{"name":"deploy"},{"no":"name"},7],"slash_commands":["compact"]})");
        CHECK(mixed.skills.size() == 2 && mixed.skills[1] == L"deploy" && mixed.slashCommands.size() == 1, "system/init: a list of names is read whether its items are strings or objects with a name; anything else is skipped");

        // Claude Code refused a tool call (both texts are 2.1.291's own).
        const auto deniedByMode = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"permission_denied","tool_name":"mcp__search__find_sessions","tool_use_id":"toolu_01","decision_reason_type":"mode","message":"Permission to use mcp__search__find_sessions has been denied because Claude Code is running in don't ask mode."})");
        CHECK(deniedByMode.kind == HarnessSystemKind::PermissionDenied && deniedByMode.deniedTool == L"mcp__search__find_sessions" && deniedByMode.deniedReason.find(L"don't ask mode") != std::wstring::npos, "system/permission_denied: the tool and Claude Code's reason (dontAsk refuses a tool no rule allows)");
        const auto deniedNoGrant = ClassifyHarnessSystemMessage(LR"({"type":"system","subtype":"permission_denied","tool_name":"mcp__search__find_sessions","tool_use_id":"toolu_02","message":"Claude requested permissions to use mcp__search__find_sessions, but you haven't granted it yet."})");
        CHECK(deniedNoGrant.kind == HarnessSystemKind::PermissionDenied && deniedNoGrant.deniedReason.find(L"haven't granted") != std::wstring::npos, "system/permission_denied: in 'default' a process without a permission handler refuses too (it does not wait)");
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

        // A session that is active this very minute (the one the user works in). The live test of
        // 2026-10-12 showed the model "(now ago)" for it.
        SessionCatalog busy = TestCatalog();
        busy.entries.insert(busy.entries.begin(), Entry(L"eeee5555-0000-4000-8000-000000000006", L"drag work in progress", L"K:\\source\\Agentmaster", 0));
        const json::Value busyResult = J(FormatSessionSearchResult(busy, pPlain, RunSessionSearch(busy, pPlain, kNow, content), kNow));
        const auto* busyRows = busyResult.Find(L"sessions");
        const std::wstring activeNow = (busyRows && !busyRows->arr.empty()) ? busyRows->arr[0].StrAt(L"active") : std::wstring{};
        CHECK(activeNow.size() > 6 && activeNow.compare(activeNow.size() - 6, 6, L" (now)") == 0 && activeNow.find(L"ago") == std::wstring::npos, "result row: a session active within the last minute reads '(now)', not '(now ago)'");

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
        // The user's rule of 2026-10-13: the FIRST step mentions which kinds of tool calls the request can
        // use, out of all the tools, and how in different ways. Pinned by its place (rule 1), its words,
        // and the rule count that SESSIONS.md 9.3 states.
        const size_t ruleOne = prompt.find(L"\n1. Your first step");
        const size_t ruleTwo = prompt.find(L"\n2. ");
        CHECK(ruleOne != std::wstring::npos && ruleTwo != std::wstring::npos && ruleOne < ruleTwo, "toolset: rule 1 of the system prompt is the first step");
        const std::wstring ruleOneText = (ruleOne != std::wstring::npos && ruleTwo != std::wstring::npos && ruleOne < ruleTwo) ? prompt.substr(ruleOne, ruleTwo - ruleOne) : std::wstring{};
        CHECK(ruleOneText.find(L"kinds of tool calls") != std::wstring::npos && ruleOneText.find(L"out of everything your tools can do") != std::wstring::npos && ruleOneText.find(L"in different ways") != std::wstring::npos && ruleOneText.find(L"before your first tool call, in the same message") != std::wstring::npos, "toolset: rule 1 says to mention the kinds of tool calls, out of all the tools, and how in different ways - in the same message as the first calls (no extra model request)");
        CHECK(ruleOneText.find(L"deep:true") != std::wstring::npos && ruleOneText.find(L"top:N") != std::wstring::npos && ruleOneText.find(L"after or before") != std::wstring::npos, "toolset: rule 1 lists every kind of call (words, filters, period, full text, show) so the plan is chosen from all of them");
        CHECK(prompt.find(L"\n8. ") != std::wstring::npos && prompt.find(L"\n9. ") == std::wstring::npos, "toolset: the system prompt has eight rules (the count SESSIONS.md 9.3 states)");
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
