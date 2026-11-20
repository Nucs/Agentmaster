// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: TRANSCRIPT TURNS — the session copy menus' "Copy > Transcript >
// Default / Max / Full / Compact / Minimal" (AgentCopyActions.h codes 9-13). TranscriptTurns.{h,cpp} is a C++
// port of the am-agentmaster-cli skill's transcript_turns.py (v11), so its contract is the reference's own
// output: every fixture in turns\ (copies of the skill's synthetic test transcripts — a session with a rewind
// and a background job, a file that opens with a compaction, junk tool arguments, every way an AskUserQuestion
// ends, a compaction a parentUuid-only walk can't cross, a fork + plan-restart family, a team lead with its
// subagents) is rendered at each of the five levels and compared BYTE FOR BYTE with turns\expected\, which
// turns\make_goldens.py writes by running the reference script itself. A mismatch prints the first differing
// line of each side. Plus focused checks of the level ladder, the timestamp reader and the JSONL reader.
// Plus the summary panel's WHOLE-CONVERSATION view, which reuses this port's chain walk (section 8):
// TestSummaryConversation (SummaryConversation.h's numbered list, pure) and TestConversationUpstream
// (CollectConversationUpstream end to end on synthetic transcripts — every kind of link, never downstream,
// the cache rules). Plus the transcript overlay's DIFF tab (TestTranscriptDiff: TranscriptDiff.h's pure half and the
// RenderTranscriptDiff document on a synthetic session). Shared CHECK/fixtures/decls live in m5_tests.h.
#include "m5_tests.h"

#include "../SummaryConversation.h" // BuildSummaryConversation / CodexSummaryConversation / SummarySeparatorLabel (TestSummaryConversation, TestConversationUpstream)
#include "../TranscriptLevelGuide.h" // kTranscriptLevelGuide / TranscriptLevelTip (TestTranscriptLevelGuide)
#include "../TranscriptTurns.h"
#include "../TranscriptDiff.h" // the overlay's Diff tab: unified diffs, the net change per turn, the cards' arithmetic (TestTranscriptDiff)
#include "../TranscriptViews.h" // the overlay's box tabs: names, boxes, options, ticks, tips (TestTranscriptViews)

#include <algorithm> // std::find (the conversation-chain tests), std::any_of (the level guide's coverage)
#include <set> // TestTranscriptViews: the names seen
#include <cwchar> // std::swprintf
#include <fstream>
#include <iterator> // std::size
#include <map>
#include <stdexcept> // std::runtime_error (a throwing recorded-predecessor lookup)
#include <string_view> // the level guide's em-dash scan
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // The folder m5_tests.exe lives in (the harness's tests folder). The fixtures are found relative to it,
    // not to the current directory, so the suite works however the exe is started.
    // Returns the folder; "." when the module path can't be read.
    fs::path TurnsHarnessDir()
    {
        wchar_t buffer[MAX_PATH * 4]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (n == 0 || n >= std::size(buffer))
        {
            return fs::path{ L"." };
        }
        return fs::path{ buffer }.parent_path();
    }

    // A whole file's bytes, or "" when it can't be read (the caller's compare then fails visibly).
    std::string ReadTurnsFileBytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // UTF-8 bytes -> UTF-16 ("" for empty input or invalid UTF-8).
    std::wstring WidenTurnsUtf8(const std::string& bytes)
    {
        if (bytes.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
        return out;
    }

    // A line for the console: every non-ASCII character as \uXXXX, so the CRT's C-locale conversion
    // can't cut a diagnostic short at the first emoji.
    std::wstring EscapeForConsole(const std::wstring& s)
    {
        std::wstring out;
        for (const wchar_t c : s)
        {
            if (c >= 0x20 && c < 0x7F)
            {
                out += c;
            }
            else
            {
                wchar_t buf[8];
                std::swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                out += buf;
            }
        }
        return out;
    }

    // Prints the first line where `got` and `want` differ (1-based), both sides escaped.
    void PrintFirstTurnsDifference(const std::wstring& got, const std::wstring& want)
    {
        const auto split = [](const std::wstring& s) {
            std::vector<std::wstring> lines;
            size_t start = 0;
            for (;;)
            {
                const size_t nl = s.find(L'\n', start);
                lines.push_back(s.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
                if (nl == std::wstring::npos)
                {
                    break;
                }
                start = nl + 1;
            }
            return lines;
        };
        const auto gotLines = split(got);
        const auto wantLines = split(want);
        const size_t count = gotLines.size() > wantLines.size() ? gotLines.size() : wantLines.size();
        for (size_t i = 0; i < count; ++i)
        {
            const std::wstring g = i < gotLines.size() ? gotLines[i] : L"<end of render>";
            const std::wstring w = i < wantLines.size() ? wantLines[i] : L"<end of golden>";
            if (g != w)
            {
                std::wprintf(L"    first difference at line %zu\n      want: %ls\n      got:  %ls\n", i + 1, EscapeForConsole(w).c_str(), EscapeForConsole(g).c_str());
                return;
            }
        }
        std::wprintf(L"    (the texts differ only in their final newline)\n");
    }

    // births.json: each chain fixture's pinned creation time (stem -> ISO stamp), as epoch seconds — a
    // checkout gives every file the same arbitrary creation time, which would scramble the fork order.
    std::map<std::wstring, double> LoadTurnsBirths(const fs::path& file)
    {
        std::map<std::wstring, double> births;
        const auto parsed = json::Parse(WidenTurnsUtf8(ReadTurnsFileBytes(file)));
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return births;
        }
        for (const auto& member : parsed->members)
        {
            if (const auto micros = TranscriptTimestampToUnixMicros(member.second.AsStr()))
            {
                births[member.first] = static_cast<double>(*micros) / 1e6;
            }
        }
        return births;
    }

    // make_goldens.py's golden_name(): "<fixture stem>-<level>.md", or "<folder>-<id's first block>-<level>.md"
    // for a fixture inside a folder ("chain/a4a4a4a4-0000-...jsonl" -> "chain-a4a4a4a4-default.md").
    std::wstring TurnsGoldenName(const std::wstring& fixture, const std::wstring& level)
    {
        const fs::path path{ fixture };
        const std::wstring folder = path.parent_path().filename().wstring();
        std::wstring stem = path.stem().wstring();
        if (folder.empty())
        {
            return stem + L"-" + level + L".md";
        }
        stem = stem.substr(0, stem.find(L'-'));
        return folder + L"-" + stem + L"-" + level + L".md";
    }

    std::string NarrowAsciiForCheck(const std::wstring& s)
    {
        std::string out;
        for (const wchar_t c : s)
        {
            out += (c > 0 && c < 0x80) ? static_cast<char>(c) : '?';
        }
        return out;
    }
}

void TestTranscriptTurns()
{
    std::wprintf(L"\n[TranscriptTurns] Copy > Transcript levels vs the transcript_turns.py v11 goldens\n");

    // ---- the level ladder: what each menu item turns on (the reference's LEVELS table) ----
    {
        const TurnsOptions latest{}; // the reference's latest preset at its default level
        const TurnsOptions def = TurnsOptionsForLevel(TurnsLevel::Default);
        CHECK(def.notes == latest.notes && def.times == latest.times && def.ops == latest.ops && def.shortPaths == latest.shortPaths &&
                  def.fullBash == latest.fullBash && def.stats == latest.stats && def.opMeta == latest.opMeta && def.opDuration == latest.opDuration &&
                  def.opOutput == latest.opOutput && def.bgEvents == latest.bgEvents && def.narration == latest.narration &&
                  def.compactions == latest.compactions && def.answers == latest.answers && def.nestedBlocks == latest.nestedBlocks &&
                  def.compactedHistory == latest.compactedHistory && def.chain == latest.chain && def.agents == latest.agents &&
                  def.peerMessages == latest.peerMessages && def.fullWrite == latest.fullWrite && def.reasons == latest.reasons,
              "TranscriptTurns: Default is exactly the latest preset's own defaults (the steps level)");
        CHECK(def.ops && def.narration && def.opMeta && !def.opDuration && !def.opOutput && !def.fullBash && !def.fullWrite && !def.stats,
              "TranscriptTurns: Default = calls on one line with their outcome, no durations / output / blocks / stats");
        const TurnsOptions minimal = TurnsOptionsForLevel(TurnsLevel::Minimal);
        CHECK(!minimal.notes && !minimal.ops && !minimal.narration && !minimal.compactions && !minimal.answers && !minimal.agents &&
                  !minimal.fullBash && !minimal.opOutput && !minimal.opDuration && !minimal.fullWrite && !minimal.stats && !minimal.reasons,
              "TranscriptTurns: Minimal turns every ladder feature off");
        CHECK(minimal.times && minimal.chain && minimal.peerMessages && minimal.liveBranch && minimal.compactedHistory,
              "TranscriptTurns: Minimal keeps the non-ladder features and every rule");
        const TurnsOptions compact = TurnsOptionsForLevel(TurnsLevel::Compact);
        CHECK(compact.notes && compact.compactions && compact.answers && compact.agents && compact.reasons && !compact.ops && !compact.narration,
              "TranscriptTurns: Compact = notes + compactions + answers + agents + each call's reason line (v10), no full call lines or remarks");
        const TurnsOptions full = TurnsOptionsForLevel(TurnsLevel::Full);
        CHECK(full.ops && full.fullBash && full.opOutput && full.opDuration && !full.fullWrite && !full.stats,
              "TranscriptTurns: Full = Default + Bash blocks, first output lines, per-call durations");
        const TurnsOptions max = TurnsOptionsForLevel(TurnsLevel::Max);
        CHECK(max.fullBash && max.opOutput && max.opDuration && max.fullWrite && max.stats,
              "TranscriptTurns: Max = Full + Write contents + a stats line per turn");
        const TurnsOptions bogus = TurnsOptionsForLevel(static_cast<TurnsLevel>(42));
        CHECK(bogus.ops == def.ops && bogus.fullBash == def.fullBash && bogus.notes == def.notes, "TranscriptTurns: an out-of-range level reads as Default");
        // Agentmaster's own editDiffs (All Screen's "File diffs", TestTranscriptTurnsEditDiffs) belongs to no level: every level
        // still renders the reference's text, so the goldens below stay the contract.
        bool anyLevelDiffs = latest.editDiffs;
        for (const TurnsLevel level : { TurnsLevel::Minimal, TurnsLevel::Compact, TurnsLevel::Default, TurnsLevel::Full, TurnsLevel::Max })
        {
            anyLevelDiffs = anyLevelDiffs || TurnsOptionsForLevel(level).editDiffs;
        }
        CHECK(!anyLevelDiffs, "TranscriptTurns: neither the preset nor any level turns on Agentmaster's editDiffs");
        // The same for lastTurnOnly (All Screen's "Last Turn", TestTranscriptTurnsLastTurn).
        bool anyLevelLast = latest.lastTurnOnly;
        for (const TurnsLevel level : { TurnsLevel::Minimal, TurnsLevel::Compact, TurnsLevel::Default, TurnsLevel::Full, TurnsLevel::Max })
        {
            anyLevelLast = anyLevelLast || TurnsOptionsForLevel(level).lastTurnOnly;
        }
        CHECK(!anyLevelLast, "TranscriptTurns: neither the preset nor any level turns on Agentmaster's lastTurnOnly");
        CHECK(std::wstring{ TurnsLevelName(TurnsLevel::Minimal) } == L"minimal" && std::wstring{ TurnsLevelName(TurnsLevel::Default) } == L"default" &&
                  std::wstring{ TurnsLevelName(TurnsLevel::Max) } == L"max",
              "TranscriptTurns: level names");
        CHECK(def.plainDashes == latest.plainDashes && minimal.plainDashes && TurnsOptionsForLevel(TurnsLevel::Max).plainDashes,
              "TranscriptTurns: plain dashes (v11) are on at every level - not a ladder feature");
    }

    // ---- plain dashes (v11): the view's OWN separators become hyphens; the session's own text keeps its em dashes ----
    {
        const std::string bytes =
            R"J({"type":"user","uuid":"p01","parentUuid":null,"isSidechain":false,"sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:00.000Z","message":{"role":"user","content":"Run it \u2014 now"}})J"
            "\n"
            R"J({"type":"assistant","uuid":"p02","parentUuid":"p01","isSidechain":false,"sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:01.000Z","message":{"id":"pm1","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"pb1","name":"Bash","input":{"command":"ls","description":"List \u2014 quickly"}}]}})J"
            "\n"
            R"J({"type":"user","uuid":"p03","parentUuid":"p02","isSidechain":false,"sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:02.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"pb1","content":"a.txt"}]}})J"
            "\n"
            R"J({"type":"assistant","uuid":"p04","parentUuid":"p03","isSidechain":false,"sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:03.000Z","message":{"id":"pm2","role":"assistant","model":"claude-test-1","stop_reason":"end_turn","content":[{"type":"text","text":"Done \u2014 1 file."}]}})J"
            "\n";
        const auto lines = ParseTranscriptJsonl(bytes);
        const std::wstring plain = RenderTranscriptTurns(lines, TurnsOptionsForLevel(TurnsLevel::Default), /*utc*/ true);
        TurnsOptions legacy = TurnsOptionsForLevel(TurnsLevel::Default);
        legacy.plainDashes = false;
        const std::wstring dashed = RenderTranscriptTurns(lines, legacy, /*utc*/ true);
        CHECK(plain.find(L"- **Bash** - List \x2014 quickly: `ls` \x00B7 ok") != std::wstring::npos,
              "TranscriptTurns: v11 writes its own separator as a hyphen and keeps the description's em dash");
        CHECK(plain.find(L"> Run it \x2014 now") != std::wstring::npos && plain.find(L"> Done \x2014 1 file.") != std::wstring::npos,
              "TranscriptTurns: v11 keeps the em dashes of the user's prompt and Claude's reply");
        CHECK(dashed.find(L"- **Bash** \x2014 List \x2014 quickly: `ls` \x00B7 ok") != std::wstring::npos,
              "TranscriptTurns: plainDashes off writes v10's em dash separator");
    }

    // ---- the timestamp reader (aware_ts) ----
    {
        CHECK(TranscriptTimestampToUnixMicros(L"1970-01-01T00:00:01.5Z") == std::optional<int64_t>{ 1'500'000 }, "TranscriptTurns: a fractional Z stamp");
        CHECK(TranscriptTimestampToUnixMicros(L"2026-01-10T08:00:00Z") == std::optional<int64_t>{ int64_t{ 1768032000 } * 1'000'000 }, "TranscriptTurns: a births.json stamp");
        CHECK(TranscriptTimestampToUnixMicros(L"1970-01-01T02:00:00+02:00") == std::optional<int64_t>{ 0 }, "TranscriptTurns: an explicit offset is applied");
        CHECK(TranscriptTimestampToUnixMicros(L"1970-01-01T00:00:00.1234567Z") == std::optional<int64_t>{ 123'456 }, "TranscriptTurns: fraction digits past 6 are dropped");
        CHECK(!TranscriptTimestampToUnixMicros(L"").has_value(), "TranscriptTurns: an empty stamp is unknown");
        CHECK(!TranscriptTimestampToUnixMicros(L"yesterday").has_value(), "TranscriptTurns: a malformed stamp is unknown");
        CHECK(!TranscriptTimestampToUnixMicros(L"2026-02-30T00:00:00Z").has_value(), "TranscriptTurns: an impossible date is unknown");
    }

    // ---- the JSONL reader (parse_jsonl): objects only, a torn or junk line skipped, never fatal ----
    {
        const std::string bytes = "{\"a\":1}\n"
                                  "\xEF\xBB\xBF{\"b\":2}\r\n" // a UTF-8 BOM, a CRLF ending
                                  "[1,2]\n" // valid JSON, but not an object
                                  "{\"c\":3} trailing\n" // "Extra data" in Python
                                  "\xFF\xFE\n" // not UTF-8
                                  "{\"torn\":";
        const auto lines = ParseTranscriptJsonl(bytes);
        CHECK(lines.size() == 2, "TranscriptTurns: ParseTranscriptJsonl keeps exactly the two object lines");
        CHECK(lines.size() == 2 && lines[0].Find(L"a") != nullptr && lines[1].Find(L"b") != nullptr, "TranscriptTurns: ...in file order, the BOM'd one included");
    }

    // ---- an unreadable transcript: nothing rendered, nothing thrown ----
    {
        const auto missing = RenderTranscriptTurnsFile(TurnsHarnessDir() / L"turns" / L"no-such-transcript.jsonl", TurnsOptionsForLevel(TurnsLevel::Default), true);
        CHECK(!missing.has_value(), "TranscriptTurns: a missing transcript renders as nullopt");
    }

    // ---- the goldens ----
    const fs::path root = TurnsHarnessDir() / L"turns";
    if (!fs::is_directory(root / L"expected"))
    {
        CHECK(false, "TranscriptTurns: tests\\turns\\expected is missing (the fixtures ship beside m5_tests.exe)");
        return;
    }
    // make_goldens.py's FIXTURES, in its order.
    static const wchar_t* const kFixtures[] = {
        L"fixture_session.jsonl",
        L"fixture_compaction.jsonl",
        L"fixture_malformed.jsonl",
        L"fixture_questions.jsonl",
        L"fixture_preserved.jsonl",
        L"chain/a4a4a4a4-0000-4000-8000-000000000004.jsonl",
        L"chain/a1a1a1a1-0000-4000-8000-000000000001.jsonl",
        L"chain/b2b2b2b2-0000-4000-8000-000000000002.jsonl",
        L"chain/b3b3b3b3-0000-4000-8000-000000000003.jsonl",
        L"agents/c1c1c1c1-0000-4000-8000-000000000001.jsonl",
        L"agents/c2c2c2c2-0000-4000-8000-000000000002.jsonl",
    };
    static const std::pair<const wchar_t*, TurnsLevel> kLevels[] = {
        { L"minimal", TurnsLevel::Minimal },
        { L"compact", TurnsLevel::Compact },
        { L"default", TurnsLevel::Default },
        { L"full", TurnsLevel::Full },
        { L"max", TurnsLevel::Max },
    };
    int matched = 0;
    int total = 0;
    for (const wchar_t* fixture : kFixtures)
    {
        const fs::path file = root / fs::path{ fixture };
        const fs::path birthsFile = file.parent_path() / L"births.json";
        // A fixture in a folder with births.json is part of a chain: render it from its FILE, so the chain and
        // the agents are searched for (with the pinned creation times); any other fixture from its lines alone,
        // exactly as the reference's test renders it.
        const bool chained = fs::is_regular_file(birthsFile);
        const auto births = chained ? LoadTurnsBirths(birthsFile) : std::map<std::wstring, double>{};
        const TurnsBirthFn born = [births](const fs::path& path) -> std::optional<double> {
            const auto it = births.find(path.stem().wstring());
            if (it == births.end())
            {
                return std::nullopt;
            }
            return it->second;
        };
        for (const auto& [levelName, level] : kLevels)
        {
            ++total;
            const TurnsOptions opts = TurnsOptionsForLevel(level);
            std::wstring got;
            if (chained)
            {
                got = RenderTranscriptTurnsFile(file, opts, /*utc*/ true, born).value_or(L"<the render failed>");
            }
            else
            {
                got = RenderTranscriptTurns(ParseTranscriptJsonl(ReadTurnsFileBytes(file)), opts, /*utc*/ true);
            }
            const std::wstring golden = TurnsGoldenName(fixture, levelName);
            const std::wstring want = WidenTurnsUtf8(ReadTurnsFileBytes(root / L"expected" / golden));
            const bool ok = !want.empty() && got == want;
            const std::string label = "TranscriptTurns golden " + NarrowAsciiForCheck(golden);
            CHECK(ok, label.c_str());
            if (ok)
            {
                ++matched;
            }
            else if (want.empty())
            {
                std::wprintf(L"    the golden %ls is missing or empty (run turns\\make_goldens.py)\n", golden.c_str());
            }
            else
            {
                PrintFirstTurnsDifference(got, want);
            }
        }
    }
    std::wprintf(L"  %d of %d goldens match\n", matched, total);
}

namespace
{
    // ---- File diffs (TurnsOptions::editDiffs): a synthetic transcript ----

    // Two turns of file changes, every way Claude Code records one: a Read (no change), an Edit with Claude Code's own
    // hunks (toolUseResult.structuredPatch), a Write that created its file (no hunks: the content), an Edit refused with an
    // error, an Edit and a MultiEdit whose results carry no hunks (an older Claude Code: the call's own old_string /
    // new_string), a NotebookEdit (its new_source) and a closing reply; then a turn whose Edit never got a result. The
    // paths use forward slashes, so no JSON escape doubles a backslash. The raw strings use the delimiter J: the JSON holds
    // a close parenthesis right before a quote (the end of print('hi')), which would end a raw string without one.
    // Returns the transcript's bytes (18 lines).
    std::string EditDiffsTranscript()
    {
        return R"J({"type":"user","uuid":"d01","parentUuid":null,"isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:00.000Z","message":{"role":"user","content":"Fix the greeting"}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d02","parentUuid":"d01","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:01.000Z","message":{"id":"m1","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"r1","name":"Read","input":{"file_path":"C:/w/app.py"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d03","parentUuid":"d02","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:02.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"r1","content":"1\timport os"}]},"toolUseResult":{"type":"text","file":{"filePath":"C:/w/app.py","content":"import os","numLines":1,"startLine":1,"totalLines":1}}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d04","parentUuid":"d03","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:03.000Z","message":{"id":"m2","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e1","name":"Edit","input":{"file_path":"C:/w/app.py","old_string":"print('hi')","new_string":"print('hello')\nprint('bye')"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d05","parentUuid":"d04","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:04.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e1","content":"The file C:/w/app.py has been updated."}]},"toolUseResult":{"filePath":"C:/w/app.py","oldString":"print('hi')","newString":"print('hello')\nprint('bye')","structuredPatch":[{"oldStart":3,"oldLines":2,"newStart":3,"newLines":3,"lines":[" import sys","-print('hi')","+print('hello')","+print('bye')"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d06","parentUuid":"d05","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:05.000Z","message":{"id":"m3","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"w1","name":"Write","input":{"file_path":"C:/w/notes.txt","content":"one\ntwo\n"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d07","parentUuid":"d06","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:06.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"w1","content":"File created successfully at: C:/w/notes.txt"}]},"toolUseResult":{"type":"create","filePath":"C:/w/notes.txt","content":"one\ntwo\n","structuredPatch":[]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d08","parentUuid":"d07","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:07.000Z","message":{"id":"m4","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e2","name":"Edit","input":{"file_path":"C:/w/locked.py","old_string":"x = 1","new_string":"x = 2"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d09","parentUuid":"d08","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:08.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e2","content":"<tool_use_error>File has not been read yet. Read it first before writing to it.</tool_use_error>","is_error":true}]},"toolUseResult":"Error: File has not been read yet. Read it first before writing to it."})J"
               "\n"
               R"J({"type":"assistant","uuid":"d10","parentUuid":"d09","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:09.000Z","message":{"id":"m5","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e3","name":"Edit","input":{"file_path":"C:/w/old.py","old_string":"a = 1","new_string":"a = 2"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d11","parentUuid":"d10","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:10.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e3","content":"The file C:/w/old.py has been updated."}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d12","parentUuid":"d11","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:11.000Z","message":{"id":"m6","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e4","name":"MultiEdit","input":{"file_path":"C:/w/multi.py","edits":[{"old_string":"p = 1","new_string":"p = 2"},{"old_string":"q = 1","new_string":"q = 3"}]}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d13","parentUuid":"d12","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:12.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e4","content":"Applied 2 edits to C:/w/multi.py"}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d14","parentUuid":"d13","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:13.000Z","message":{"id":"m7","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"n1","name":"NotebookEdit","input":{"notebook_path":"C:/w/nb.ipynb","cell_id":"c1","new_source":"x = 1\ny = 2"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d15","parentUuid":"d14","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:14.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"n1","content":"Updated cell c1"}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d16","parentUuid":"d15","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:00:15.000Z","message":{"id":"m8","role":"assistant","model":"claude-test-1","stop_reason":"end_turn","content":[{"type":"text","text":"Greeting fixed."}]}})J"
               "\n"
               R"J({"type":"user","uuid":"d17","parentUuid":"d16","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:05:00.000Z","message":{"role":"user","content":"Now the farewell"}})J"
               "\n"
               R"J({"type":"assistant","uuid":"d18","parentUuid":"d17","isSidechain":false,"sessionId":"dddddddd-0000-4000-8000-0000000000d1","timestamp":"2026-01-02T10:05:01.000Z","message":{"id":"m9","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e5","name":"Edit","input":{"file_path":"C:/w/app.py","old_string":"bye","new_string":"farewell"}}]}})J"
               "\n";
    }

    // A render's lines, split at '\n' (a trailing newline gives an empty last line, on both sides of a compare alike).
    std::vector<std::wstring> SplitTurnsLines(const std::wstring& text)
    {
        std::vector<std::wstring> lines;
        size_t start = 0;
        for (;;)
        {
            const size_t nl = text.find(L'\n', start);
            lines.push_back(text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
            if (nl == std::wstring::npos)
            {
                return lines;
            }
            start = nl + 1;
        }
    }

    // Whether every line of `base` is in `grown`, in the same order: lines may be ADDED between them, none removed or
    // changed - "the option only adds to the level's own render". (A greedy walk decides a subsequence exactly.)
    bool TurnsLinesKeptInOrder(const std::wstring& base, const std::wstring& grown)
    {
        const auto want = SplitTurnsLines(base);
        size_t found = 0;
        for (const auto& line : SplitTurnsLines(grown))
        {
            if (found < want.size() && line == want[found])
            {
                ++found;
            }
        }
        return found == want.size();
    }

    // How many times `part` occurs in `text` (non-overlapping).
    size_t CountTurnsOccurrences(const std::wstring& text, std::wstring_view part)
    {
        size_t count = 0;
        for (size_t at = text.find(part); at != std::wstring::npos; at = text.find(part, at + part.size()))
        {
            ++count;
        }
        return count;
    }
}

// Agentmaster (TurnsOptions::editDiffs - 📜 All Screen's "File diffs", MESSAGING.md section 21.39): NOT part of the port, so
// no golden covers it; this pins what it adds on a synthetic transcript and on the reference's session fixture. Off, every
// level renders exactly its own text (no diff anywhere). On, each file change that went through is a ```diff block under its
// call: Claude Code's own hunks with their header, a created file's content as added lines from 1, the call's own strings
// when no hunks were recorded (Edit, each edit of a MultiEdit), a NotebookEdit's new source; a refused or unanswered call
// gets none. Below Max the option only ADDS lines; at Minimal and Compact it lists the file-changing calls the level hides
// (and nothing else, never an empty timeline); at Max a Write's diff takes the place of its content block.
void TestTranscriptTurnsEditDiffs()
{
    std::wprintf(L"\n[TranscriptTurns] File diffs (editDiffs, All Screen): every file change as a diff, at every level\n");
    const auto lines = ParseTranscriptJsonl(EditDiffsTranscript());
    CHECK(lines.size() == 18, "TurnsEditDiffs: the synthetic transcript parses to its 18 lines");
    const auto render = [&lines](TurnsLevel level, bool diffs) {
        TurnsOptions opts = TurnsOptionsForLevel(level);
        opts.editDiffs = diffs;
        return RenderTranscriptTurns(lines, opts, /*utc*/ true);
    };
    const auto has = [](const std::wstring& text, std::wstring_view part) { return text.find(part) != std::wstring::npos; };
    // A failed check prints the render it judged, line by line and escaped, so the failure reads without a debugger.
    const auto expect = [](bool ok, const char* label, const std::wstring& text) {
        CHECK(ok, label);
        if (!ok)
        {
            for (const auto& line : SplitTurnsLines(text))
            {
                std::wprintf(L"      | %ls\n", EscapeForConsole(line).c_str());
            }
        }
    };

    // The blocks, as each level with calls writes them (the call's line, then its diff as a nested item).
    const std::wstring editDiff = L"- **Edit** `C:/w/app.py` \x00B7 ok\n"
                                  L"  - ```diff\n"
                                  L"    @@ -3,2 +3,3 @@\n"
                                  L"     import sys\n"
                                  L"    -print('hi')\n"
                                  L"    +print('hello')\n"
                                  L"    +print('bye')\n"
                                  L"    ```\n";
    const std::wstring writeDiff = L"- **Write** `C:/w/notes.txt` (2 lines) \x00B7 created\n"
                                   L"  - ```diff\n"
                                   L"    @@ -0,0 +1,2 @@\n"
                                   L"    +one\n"
                                   L"    +two\n"
                                   L"    ```\n";
    const std::wstring fallbackDiff = L"- **Edit** `C:/w/old.py` \x00B7 ok\n"
                                      L"  - ```diff\n"
                                      L"    -a = 1\n"
                                      L"    +a = 2\n"
                                      L"    ```\n";
    const std::wstring multiDiff = L"- **MultiEdit** `C:/w/multi.py` \x00B7 ok\n"
                                   L"  - ```diff\n"
                                   L"    -p = 1\n"
                                   L"    +p = 2\n"
                                   L"    -q = 1\n"
                                   L"    +q = 3\n"
                                   L"    ```\n";
    const std::wstring notebookDiff = L"  - ```diff\n"
                                      L"    +x = 1\n"
                                      L"    +y = 2\n"
                                      L"    ```\n";

    // ---- off: every level is its own text; on: below Max, only lines added ----
    static const std::pair<const char*, TurnsLevel> kLevels[] = {
        { "minimal", TurnsLevel::Minimal },
        { "compact", TurnsLevel::Compact },
        { "default", TurnsLevel::Default },
        { "full", TurnsLevel::Full },
        { "max", TurnsLevel::Max },
    };
    for (const auto& [name, level] : kLevels)
    {
        const std::wstring off = render(level, false);
        const std::wstring on = render(level, true);
        expect(!off.empty() && !has(off, L"```diff"), ("TurnsEditDiffs: off, the " + std::string{ name } + " render has no diff").c_str(), off);
        expect(CountTurnsOccurrences(on, L"```diff") == 5, ("TurnsEditDiffs: on, the " + std::string{ name } + " render has the 5 diffs (not the refused or the unanswered Edit)").c_str(), on);
        if (level != TurnsLevel::Max)
        {
            expect(TurnsLinesKeptInOrder(off, on), ("TurnsEditDiffs: on, the " + std::string{ name } + " render only adds lines to the level's own").c_str(), on);
        }
    }

    // ---- Default: each way a change is recorded ----
    {
        const std::wstring on = render(TurnsLevel::Default, true);
        expect(has(on, editDiff), "TurnsEditDiffs: an Edit shows Claude Code's own hunk (header, context, removed, added)", on);
        expect(has(on, writeDiff), "TurnsEditDiffs: a Write that created its file shows the content as added lines from line 1", on);
        expect(has(on, L"- **Edit** `C:/w/locked.py` \x00B7 \x2717 error\n- **Edit** `C:/w/old.py` \x00B7 ok\n"), "TurnsEditDiffs: a refused Edit (an error result) shows no diff", on);
        expect(has(on, fallbackDiff), "TurnsEditDiffs: an Edit without recorded hunks shows its old_string / new_string, no header", on);
        expect(has(on, multiDiff), "TurnsEditDiffs: a MultiEdit without recorded hunks shows each edit's strings in order", on);
        expect(has(on, notebookDiff), "TurnsEditDiffs: a NotebookEdit shows its new source as added lines", on);
        expect(has(on, L"- **Edit** `C:/w/app.py` \x00B7 no result\n\n> (no text"), "TurnsEditDiffs: an Edit that never got a result shows no diff", on);
    }

    // ---- Minimal: the file changes the level hides are listed, nothing else is ----
    {
        const std::wstring off = render(TurnsLevel::Minimal, false);
        const std::wstring on = render(TurnsLevel::Minimal, true);
        expect(!has(off, L"**Edit**") && !has(off, L"**Write**"), "TurnsEditDiffs: off, Minimal lists no calls (the level's own)", off);
        expect(has(on, editDiff) && has(on, writeDiff) && has(on, fallbackDiff) && has(on, multiDiff) && has(on, notebookDiff),
               "TurnsEditDiffs: on, Minimal lists each file change that went through, with its diff", on);
        expect(!has(on, L"**Read**") && !has(on, L"locked.py") && !has(on, L"no result"), "TurnsEditDiffs: on, Minimal lists no other call, no refused or unanswered Edit", on);
        expect(!has(on, L"(no remarks)") && has(on, L"**\x25CF Claude**\n> (no text"), "TurnsEditDiffs: on, a Minimal turn with no change to show opens no timeline", on);
    }

    // ---- Compact (transcript_turns v10): each call already has its reason line, so a diff nests under THAT line ----
    {
        const std::wstring off = render(TurnsLevel::Compact, false);
        const std::wstring on = render(TurnsLevel::Compact, true);
        expect(has(off, L"- **Edit**: `app.py`\n") && !has(off, L"```diff"), "TurnsEditDiffs: off, Compact lists the Edit as its reason line, no diff", off);
        expect(has(on, L"- **Edit**: `app.py`\n  - ```diff\n    @@ -3,2 +3,3 @@\n"), "TurnsEditDiffs: on, Compact nests the Edit's diff under its reason line", on);
        expect(has(on, L"- **Write**: `notes.txt`\n  - ```diff\n    @@ -0,0 +1,2 @@\n"), "TurnsEditDiffs: on, Compact nests a created file's diff under its reason line", on);
        expect(!has(on, L"- **Edit** `C:/w/app.py` \x00B7 ok"), "TurnsEditDiffs: on, Compact never repeats a call as its full line (the reason line names it)", on);
    }

    // ---- Max: a Write's diff takes the place of its content block ----
    {
        const std::wstring plainWrite = L"  - ```\n    one\n    two\n    ```\n";
        const std::wstring writeBlock = L"  - ```diff\n    @@ -0,0 +1,2 @@\n    +one\n    +two\n    ```\n";
        const std::wstring off = render(TurnsLevel::Max, false);
        const std::wstring on = render(TurnsLevel::Max, true);
        expect(has(off, plainWrite), "TurnsEditDiffs: off, Max shows the Write's content block (the level's own)", off);
        expect(has(on, writeBlock) && !has(on, plainWrite), "TurnsEditDiffs: on, Max shows the Write once, as its diff", on);
    }

    // ---- the reference's session fixture: its Write (a created scan.py) as a diff, the golden otherwise kept ----
    {
        const fs::path root = TurnsHarnessDir() / L"turns";
        const std::string bytes = ReadTurnsFileBytes(root / L"fixture_session.jsonl");
        const std::wstring golden = WidenTurnsUtf8(ReadTurnsFileBytes(root / L"expected" / L"fixture_session-default.md"));
        CHECK(!bytes.empty() && !golden.empty(), "TurnsEditDiffs: tests\\turns holds fixture_session.jsonl and its Default golden");
        if (!bytes.empty() && !golden.empty())
        {
            TurnsOptions opts = TurnsOptionsForLevel(TurnsLevel::Default);
            opts.editDiffs = true;
            const std::wstring on = RenderTranscriptTurns(ParseTranscriptJsonl(bytes), opts, /*utc*/ true);
            expect(has(on, L"- **Write** `scratchpad\\scan.py` (2 lines) \x00B7 created\n  - ```diff\n    @@ -0,0 +1,2 @@\n    +print('scan')\n    +print('done')\n    ```\n"),
                   "TurnsEditDiffs: the fixture's created scan.py is drawn as two added lines", on);
            expect(TurnsLinesKeptInOrder(golden, on) && CountTurnsOccurrences(on, L"```diff") == 1, "TurnsEditDiffs: the fixture's render is its golden plus that one diff", on);
        }
    }
}

namespace
{
    // ---- Last Turn (TurnsOptions::lastTurnOnly): what a golden says its last turn is ----

    // The golden's text from its LAST turn on: from the last turn heading ("### Turn N", or "### Work before the first prompt"
    // for a transcript that opens mid-turn) - or from the "_Before this turn: ..._" note RenderTurn writes right above that
    // heading, after a blank line - to the end. A session heading above the turn ("## Session 3 of 3 ...") is NOT in it: the
    // whole chain's headings are what Last Turn leaves out.
    // Returns the tail, or "" when the golden holds no turn heading.
    std::wstring GoldenLastTurnTail(const std::wstring& golden)
    {
        size_t at = std::wstring::npos;
        for (const std::wstring head : { std::wstring{ L"### Turn " }, std::wstring{ L"### Work before the first prompt" } })
        {
            // A heading starts a line: at the very start, or after a '\n' (a quoted or indented "### Turn" never does).
            size_t start = golden.rfind(L"\n" + head);
            start = start == std::wstring::npos ? (golden.rfind(head, 0) == 0 ? 0 : std::wstring::npos) : start + 1;
            if (start != std::wstring::npos && (at == std::wstring::npos || start > at))
            {
                at = start;
            }
        }
        if (at == std::wstring::npos)
        {
            return {};
        }
        // "_Before this turn: X._\n\n### Turn 3": step back over the blank line to the note's own line.
        if (at >= 3 && golden.compare(at - 2, 2, L"\n\n") == 0)
        {
            const size_t previousEnd = golden.rfind(L'\n', at - 3);
            const size_t lineStart = previousEnd == std::wstring::npos ? 0 : previousEnd + 1;
            if (golden.compare(lineStart, 19, L"_Before this turn: ") == 0)
            {
                at = lineStart;
            }
        }
        return golden.substr(at);
    }

    // Takes one head paragraph off the front of a Last Turn render when it starts with `lead` ("Paths like ...", "Before the first
    // turn: ..."): the paragraph and the blank line after it. A paragraph taken must also be in the golden (the whole render
    // writes it at its head too): `inGolden` says so.
    // Returns true when a paragraph was taken.
    bool TakeHeadParagraph(std::wstring& render, const std::wstring& golden, std::wstring_view lead, bool& inGolden)
    {
        if (render.compare(0, lead.size(), lead) != 0)
        {
            return false;
        }
        const size_t end = render.find(L"\n\n");
        if (end == std::wstring::npos)
        {
            return false;
        }
        const std::wstring paragraph = render.substr(0, end + 1);
        inGolden = inGolden && golden.find(paragraph) != std::wstring::npos;
        render.erase(0, end + 2);
        return true;
    }
}

// Agentmaster (TurnsOptions::lastTurnOnly - 📜 All Screen's "Last Turn", MESSAGING.md section 21.39, the user's request of
// 2026-10-29): NOT part of the port, so no golden covers it directly - but every golden ENDS with its last turn, so each of the
// 11 reference fixtures at each of the 5 levels pins it byte for byte: the Last Turn render is the golden's text from the last
// turn on (its "_Before this turn" note included, its session heading not), after the head paragraphs Last Turn keeps (the path
// legend, which follows what the turn shows; the notes before the first turn, for a one-turn conversation) - each of which the
// golden writes too. Then the synthetic transcript: the newest turn alone, and File Diffs adding nothing from the turns it
// leaves out.
void TestTranscriptTurnsLastTurn()
{
    std::wprintf(L"\n[TranscriptTurns] Last Turn (lastTurnOnly, All Screen): the newest turn only, numbered as in the whole conversation\n");

    // ---- the goldens' last turns ----
    const fs::path root = TurnsHarnessDir() / L"turns";
    if (!fs::is_directory(root / L"expected"))
    {
        CHECK(false, "TurnsLastTurn: tests\\turns\\expected is missing (the fixtures ship beside m5_tests.exe)");
        return;
    }
    // make_goldens.py's FIXTURES, as TestTranscriptTurns lists them.
    static const wchar_t* const kFixtures[] = {
        L"fixture_session.jsonl",
        L"fixture_compaction.jsonl",
        L"fixture_malformed.jsonl",
        L"fixture_questions.jsonl",
        L"fixture_preserved.jsonl",
        L"chain/a4a4a4a4-0000-4000-8000-000000000004.jsonl",
        L"chain/a1a1a1a1-0000-4000-8000-000000000001.jsonl",
        L"chain/b2b2b2b2-0000-4000-8000-000000000002.jsonl",
        L"chain/b3b3b3b3-0000-4000-8000-000000000003.jsonl",
        L"agents/c1c1c1c1-0000-4000-8000-000000000001.jsonl",
        L"agents/c2c2c2c2-0000-4000-8000-000000000002.jsonl",
    };
    static const std::pair<const wchar_t*, TurnsLevel> kLevels[] = {
        { L"minimal", TurnsLevel::Minimal },
        { L"compact", TurnsLevel::Compact },
        { L"default", TurnsLevel::Default },
        { L"full", TurnsLevel::Full },
        { L"max", TurnsLevel::Max },
    };
    int matched = 0;
    int total = 0;
    for (const wchar_t* fixture : kFixtures)
    {
        const fs::path file = root / fs::path{ fixture };
        const fs::path birthsFile = file.parent_path() / L"births.json";
        // As TestTranscriptTurns renders it: a chained fixture from its FILE with the pinned creation times, any other from its
        // lines alone.
        const bool chained = fs::is_regular_file(birthsFile);
        const auto births = chained ? LoadTurnsBirths(birthsFile) : std::map<std::wstring, double>{};
        const TurnsBirthFn born = [births](const fs::path& path) -> std::optional<double> {
            const auto it = births.find(path.stem().wstring());
            if (it == births.end())
            {
                return std::nullopt;
            }
            return it->second;
        };
        for (const auto& [levelName, level] : kLevels)
        {
            ++total;
            TurnsOptions opts = TurnsOptionsForLevel(level);
            opts.lastTurnOnly = true;
            std::wstring got = chained ? RenderTranscriptTurnsFile(file, opts, /*utc*/ true, born).value_or(L"<the render failed>") :
                                         RenderTranscriptTurns(ParseTranscriptJsonl(ReadTurnsFileBytes(file)), opts, /*utc*/ true);
            const std::wstring golden = WidenTurnsUtf8(ReadTurnsFileBytes(root / L"expected" / TurnsGoldenName(fixture, levelName)));
            const std::wstring want = GoldenLastTurnTail(golden);
            // The head paragraphs Last Turn keeps, in the order Document writes them; each must be in the golden as well.
            bool headsInGolden = true;
            TakeHeadParagraph(got, golden, L"Before the first turn: ", headsInGolden);
            TakeHeadParagraph(got, golden, L"Paths like ", headsInGolden);
            const bool ok = !want.empty() && headsInGolden && got == want;
            const std::string label = "TurnsLastTurn: the last turn of " + NarrowAsciiForCheck(TurnsGoldenName(fixture, levelName)) + " is the golden's tail";
            CHECK(ok, label.c_str());
            if (ok)
            {
                ++matched;
            }
            else if (want.empty())
            {
                std::wprintf(L"    the golden has no turn heading (or is missing)\n");
            }
            else
            {
                PrintFirstTurnsDifference(got, want);
            }
        }
    }
    std::wprintf(L"  %d of %d last turns match their golden's tail\n", matched, total);

    // ---- a chain: the newest turn keeps its number in the whole chain, and nothing of the chain is drawn ----
    {
        TurnsOptions opts = TurnsOptionsForLevel(TurnsLevel::Default);
        opts.lastTurnOnly = true;
        const fs::path chain = root / L"chain" / L"a4a4a4a4-0000-4000-8000-000000000004.jsonl";
        const auto births = LoadTurnsBirths(root / L"chain" / L"births.json");
        const TurnsBirthFn born = [births](const fs::path& path) -> std::optional<double> {
            const auto it = births.find(path.stem().wstring());
            return it == births.end() ? std::nullopt : std::optional<double>{ it->second };
        };
        const std::wstring last = RenderTranscriptTurnsFile(chain, opts, /*utc*/ true, born).value_or(L"");
        CHECK(last.rfind(L"### Turn 5 ", 0) == 0 && last.find(L"### Turn 4") == std::wstring::npos && last.find(L"## Session") == std::wstring::npos &&
                  last.find(L"Session chain") == std::wstring::npos,
              "TurnsLastTurn: a three-session chain's newest turn is Turn 5 (its number in the whole chain), with no session heading and no chain block");
    }

    // ---- the synthetic transcript (two turns): the newest alone, and File Diffs adds nothing from the turn left out ----
    {
        const auto lines = ParseTranscriptJsonl(EditDiffsTranscript());
        TurnsOptions opts = TurnsOptionsForLevel(TurnsLevel::Default);
        opts.lastTurnOnly = true;
        const std::wstring last = RenderTranscriptTurns(lines, opts, /*utc*/ true);
        CHECK(last.rfind(L"### Turn 2 ", 0) == 0 && last.find(L"Now the farewell") != std::wstring::npos && last.find(L"### Turn 1") == std::wstring::npos &&
                  last.find(L"Fix the greeting") == std::wstring::npos && last.find(L"Greeting fixed.") == std::wstring::npos,
              "TurnsLastTurn: two turns -> turn 2 alone, its prompt in, turn 1's prompt and reply out");
        opts.editDiffs = true;
        const std::wstring lastWithDiffs = RenderTranscriptTurns(lines, opts, /*utc*/ true);
        CHECK(CountTurnsOccurrences(lastWithDiffs, L"```diff") == 0 && lastWithDiffs.find(L"- **Edit** `C:/w/app.py` \x00B7 no result") != std::wstring::npos,
              "TurnsLastTurn: with File Diffs too, turn 1's five diffs stay out; turn 2's unanswered Edit has none");
        TurnsOptions minimal = TurnsOptionsForLevel(TurnsLevel::Minimal);
        minimal.lastTurnOnly = true;
        const std::wstring lastMinimal = RenderTranscriptTurns(lines, minimal, /*utc*/ true);
        CHECK(lastMinimal.rfind(L"### Turn 2 ", 0) == 0 && lastMinimal.find(L"**\x25CF Claude**\n> (no text") != std::wstring::npos,
              "TurnsLastTurn: at Minimal, turn 2's prompt and its ending, nothing else");
        const std::wstring none = RenderTranscriptTurns({}, opts, /*utc*/ true);
        CHECK(none.find(L"### Turn") == std::wstring::npos, "TurnsLastTurn: a transcript with no turn renders no turn");
    }
}

void TestTranscriptLevelGuide()
{
    std::wprintf(L"\n[TranscriptLevelGuide] the transcript overlay's (i) panel and tab cheat sheets vs the levels\n");
    constexpr TurnsLevel kLevels[] = { TurnsLevel::Minimal, TurnsLevel::Compact, TurnsLevel::Default, TurnsLevel::Full, TurnsLevel::Max };

    // ---- every row agrees with the level ladder: its option is on from its first level up, and off below ----
    // This is the whole point of the guide being one table: a level that changes without its row (or the
    // reverse) fails here instead of shipping a cheat sheet that lies.
    {
        for (const auto& row : kTranscriptLevelGuide)
        {
            for (const TurnsLevel level : kLevels)
            {
                const bool promised = TranscriptLevelShows(row, level);
                const bool rendered = row.feature == nullptr ? true : (TurnsOptionsForLevel(level).*row.feature);
                const std::string label = std::string{ "LevelGuide: \"" } + NarrowAsciiForCheck(row.brief) + "\" at " +
                                          NarrowAsciiForCheck(TurnsLevelName(level)) + (promised ? " is shown" : " is left out");
                CHECK(promised == rendered, label.c_str());
            }
        }
        // A row with no option is one that EVERY level shows, so it must start at Minimal.
        bool optionlessRowsStartAtMinimal = true;
        for (const auto& row : kTranscriptLevelGuide)
        {
            optionlessRowsStartAtMinimal = optionlessRowsStartAtMinimal && (row.feature != nullptr || row.firstLevel == TurnsLevel::Minimal);
        }
        CHECK(optionlessRowsStartAtMinimal, "LevelGuide: a row no option decides is shown at every level (it starts at Minimal)");
    }

    // ---- the table reads as a staircase, and covers every switch the levels flip ----
    {
        bool ordered = true;
        int previous = static_cast<int>(TurnsLevel::Minimal);
        for (const auto& row : kTranscriptLevelGuide)
        {
            ordered = ordered && static_cast<int>(row.firstLevel) >= previous;
            previous = static_cast<int>(row.firstLevel);
        }
        CHECK(ordered, "LevelGuide: rows run from the least detailed level to the most (the panel's check marks rise as a staircase)");
        // The twelve LADDER features (the ones a level sets, TurnsOptionsForLevel): each must have a row,
        // or the guide would hide a difference between two levels.
        const std::pair<bool TurnsOptions::*, const char*> ladder[] = {
            { &TurnsOptions::reasons, "reasons" },
            { &TurnsOptions::notes, "notes" },
            { &TurnsOptions::ops, "ops" },
            { &TurnsOptions::fullBash, "fullBash" },
            { &TurnsOptions::stats, "stats" },
            { &TurnsOptions::opDuration, "opDuration" },
            { &TurnsOptions::opOutput, "opOutput" },
            { &TurnsOptions::narration, "narration" },
            { &TurnsOptions::compactions, "compactions" },
            { &TurnsOptions::answers, "answers" },
            { &TurnsOptions::agents, "agents" },
            { &TurnsOptions::fullWrite, "fullWrite" },
        };
        for (const auto& [feature, name] : ladder)
        {
            const bool covered = std::any_of(std::begin(kTranscriptLevelGuide), std::end(kTranscriptLevelGuide), [feature](const TranscriptLevelGuideRow& row) {
                return row.feature == feature;
            });
            const std::string label = std::string{ "LevelGuide: the ladder feature " } + name + " has a row";
            CHECK(covered, label.c_str());
        }
        // Every level shows at least one row the level below it does not - else two tabs would look alike in
        // the panel, and a tooltip's "plus:" list would be empty.
        for (const TurnsLevel level : kLevels)
        {
            const bool adds = std::any_of(std::begin(kTranscriptLevelGuide), std::end(kTranscriptLevelGuide), [level](const TranscriptLevelGuideRow& row) {
                return row.firstLevel == level;
            });
            const std::string label = std::string{ "LevelGuide: " } + NarrowAsciiForCheck(TurnsLevelName(level)) + " is the first level of at least one row";
            CHECK(adds, label.c_str());
        }
    }

    // ---- the guide's own words: no em dash, every list non-empty ----
    {
        bool plainDashes = true;
        const auto noEmDash = [&plainDashes](const wchar_t* text) {
            plainDashes = plainDashes && std::wstring_view{ text }.find(L'\x2014') == std::wstring_view::npos;
        };
        for (const auto& row : kTranscriptLevelGuide)
        {
            noEmDash(row.text);
            noEmDash(row.brief);
        }
        for (const wchar_t* item : kTranscriptLevelGuideEveryLevel)
        {
            noEmDash(item);
        }
        for (const wchar_t* item : kTranscriptLevelGuideNoLevel)
        {
            noEmDash(item);
        }
        noEmDash(kTranscriptLevelGuideCodexNote);
        CHECK(plainDashes, "LevelGuide: every text uses the plain hyphen, never an em dash");
        CHECK(std::size(kTranscriptLevelGuideEveryLevel) > 0 && std::size(kTranscriptLevelGuideNoLevel) > 0,
              "LevelGuide: the panel has an 'Also in every level' and an 'In no level' list");
    }

    // ---- each tab's cheat sheet: its key, what it adds, what it leaves out and where each of those shows ----
    {
        const std::wstring compact = TranscriptLevelTip(TurnsLevel::Compact, 4);
        CHECK(compact.rfind(L"Compact (key 4)\nEverything in Minimal, plus:\n", 0) == 0, "LevelGuide: Compact's tip opens with its key and what it builds on");
        CHECK(compact.find(L"\n  \x2713 slash commands you ran outside a turn") != std::wstring::npos &&
                  compact.find(L"\n  \x2713 each agent's brief and final report") != std::wstring::npos,
              "LevelGuide: Compact's tip checks the rows Compact adds");
        CHECK(compact.find(L"\x2713 your messages and each turn's final reply") == std::wstring::npos,
              "LevelGuide: Compact's tip lists only what it ADDS (Minimal's row is in Minimal's tip)");
        CHECK(compact.find(L"\n  \x2713 every tool call, with the reason Claude gave") != std::wstring::npos,
              "LevelGuide: Compact's tip checks its calls' reason lines (transcript_turns v10)");
        CHECK(compact.find(L"\nLeaves out:\n  \x00B7 each call's arguments and outcome (from Default)") != std::wstring::npos &&
                  compact.find(L"\x00B7 a stats line per turn (tokens, model, context) (from Max)") != std::wstring::npos,
              "LevelGuide: Compact's tip lists what it leaves out, each with the level that first shows it");
        CHECK(compact.size() >= 4 && compact.substr(compact.size() - std::wstring_view{ L"compares every level." }.size()) == L"compares every level.",
              "LevelGuide: every tip ends by pointing at the (i) panel");

        const std::wstring minimal = TranscriptLevelTip(TurnsLevel::Minimal, 5);
        CHECK(minimal.rfind(L"Minimal (key 5)\nShows:\n  \x2713 your messages and each turn's final reply\nLeaves out:\n", 0) == 0,
              "LevelGuide: Minimal's tip says what it shows, then leaves out every other row");

        const std::wstring max = TranscriptLevelTip(TurnsLevel::Max, 2);
        CHECK(max.rfind(L"Max (key 2)\nEverything in Full, plus:\n", 0) == 0 && max.find(L"Leaves out:") == std::wstring::npos &&
                  max.find(L"\nLeaves out nothing another level shows.") != std::wstring::npos,
              "LevelGuide: Max's tip adds its rows and leaves nothing out");

        // Every tip names every row exactly once: as an addition of this level, an earlier level's row (not
        // listed: it is in that level's tip), or something left out.
        for (const TurnsLevel level : kLevels)
        {
            const std::wstring tip = TranscriptLevelTip(level, 0);
            bool rowsPlaced = tip.find(L"(key ") == std::wstring::npos; // shortcut 0 leaves the key out
            for (const auto& row : kTranscriptLevelGuide)
            {
                const bool added = row.firstLevel == level;
                const bool leftOut = static_cast<int>(row.firstLevel) > static_cast<int>(level);
                const bool listed = tip.find(std::wstring{ L"  \x2713 " } + row.brief) != std::wstring::npos;
                const bool listedOut = tip.find(std::wstring{ L"  \x00B7 " } + row.brief) != std::wstring::npos;
                rowsPlaced = rowsPlaced && listed == added && listedOut == leftOut;
            }
            const std::string label = std::string{ "LevelGuide: " } + NarrowAsciiForCheck(TurnsLevelName(level)) + "'s tip places every row once (added / left out / earlier)";
            CHECK(rowsPlaced, label.c_str());
        }
        CHECK(TranscriptLevelTip(static_cast<TurnsLevel>(42), 1).rfind(L"Default (key 1)\n", 0) == 0, "LevelGuide: an out-of-range level's tip reads as Default's");
        CHECK(std::wstring{ TranscriptLevelLabel(TurnsLevel::Compact) } == L"Compact" && std::wstring{ TranscriptLevelLabel(static_cast<TurnsLevel>(-3)) } == L"Default",
              "LevelGuide: level labels (an out-of-range one reads Default)");
    }
}

namespace
{
    // ---- the summary's conversation chain: synthetic transcripts ----
    // The reference's chain fixtures (turns\chain) write their user lines WITHOUT "userType":"external",
    // which every real Claude Code user line carries and the summary's prompt rule (SummaryPromptOfLine,
    // the analyzer's own) requires — so they hold no prompt the summary would list. These helpers write
    // the same shapes the way Claude Code does.

    // Writes one synthetic transcript (UTF-8 bytes), creating its folder.
    //   path  — the .jsonl to (over)write; bytes — its whole content.
    // Returns false when it could not be written (the caller's CHECKs then fail visibly). Never throws
    // beyond std::bad_alloc.
    bool WriteUpstreamTranscript(const fs::path& path, const std::string& bytes)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(file);
    }

    // A human prompt line: type "user", userType "external", a parent link (empty = null) and a stamp.
    //   jsonText — the prompt, ALREADY JSON-escaped.
    std::string UpUserLine(const std::string& uuid, const std::string& parent, const std::string& ts, const std::string& jsonText)
    {
        return "{\"type\":\"user\",\"userType\":\"external\",\"uuid\":\"" + uuid + "\",\"parentUuid\":" +
               (parent.empty() ? std::string{ "null" } : "\"" + parent + "\"") + ",\"timestamp\":\"" + ts +
               "\",\"message\":{\"role\":\"user\",\"content\":\"" + jsonText + "\"}}\n";
    }

    // A finished assistant reply line (one text block, stop_reason end_turn).
    //   jsonText — the reply, ALREADY JSON-escaped.
    std::string UpAssistantLine(const std::string& uuid, const std::string& parent, const std::string& ts, const std::string& jsonText)
    {
        return "{\"type\":\"assistant\",\"uuid\":\"" + uuid + "\",\"parentUuid\":\"" + parent + "\",\"timestamp\":\"" + ts +
               "\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"" + jsonText +
               "\"}],\"stop_reason\":\"end_turn\"}}\n";
    }

    // A manual /compact as Claude Code writes it: the boundary (a new root whose logicalParentUuid names
    // the last line it summarized) followed by the summary bridge, a userType "external" user line that is
    // NOT a prompt (isCompactSummary).
    std::string UpCompactionLines(const std::string& boundaryUuid, const std::string& logicalParent, const std::string& summaryUuid, const std::string& ts, const std::string& summaryTs)
    {
        return "{\"type\":\"system\",\"subtype\":\"compact_boundary\",\"uuid\":\"" + boundaryUuid +
               "\",\"parentUuid\":null,\"logicalParentUuid\":\"" + logicalParent + "\",\"timestamp\":\"" + ts +
               "\",\"content\":\"Conversation compacted\",\"compactMetadata\":{\"trigger\":\"manual\",\"preTokens\":409000,\"postTokens\":5000}}\n" +
               "{\"type\":\"user\",\"userType\":\"external\",\"isCompactSummary\":true,\"uuid\":\"" + summaryUuid + "\",\"parentUuid\":\"" +
               boundaryUuid + "\",\"timestamp\":\"" + summaryTs +
               "\",\"message\":{\"role\":\"user\",\"content\":\"This session is being continued from a previous conversation that ran out of context.\"}}\n";
    }

    // The prompts of every segment of an upstream, in order (a downstream-leak probe).
    std::vector<std::wstring> AllUpstreamPrompts(const ConversationUpstream& up)
    {
        std::vector<std::wstring> all;
        for (const auto& seg : up.segments)
        {
            all.insert(all.end(), seg.userMsgs.begin(), seg.userMsgs.end());
        }
        return all;
    }

    // The current UTC time: as epoch seconds (a "just created" birth) and as a transcript stamp.
    std::pair<double, std::string> UpstreamNow()
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER t{};
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        const double epoch = (static_cast<double>(t.QuadPart) - 116444736000000000.0) / 1e7;
        SYSTEMTIME st{};
        ::FileTimeToSystemTime(&ft, &st);
        char buf[40]{};
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.000Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return { epoch, std::string{ buf } };
    }
}

// Agentmaster (the summary's conversation chain — SummaryConversation.h): BuildSummaryConversation, pure, on
// hand-built inputs. The rules the whole-conversation view promises: the earlier sessions first, oldest
// first; ONE separator after each earlier session's LAST segment (a session of the chain with nothing
// listed still gets one); its title from titleOf, else the transcript's own; every prompt listed ONCE —
// a fork's copy of its parent's prompt under the parent, carrying its ▸ jump index; the session's own
// pre-/compact parts with no jump index; the toggle OFF = the classic list; a short uuid list never
// misattributes; the Codex rows; and the separator's label (SummarySeparatorLabel): "fork <id>" after a
// fork's parent, the bare id after every other link.
void TestSummaryConversation()
{
    std::wprintf(L"\n[SummaryConversation] the summary's numbered conversation list (BuildSummaryConversation, pure)\n");

    SessionSummary a;
    a.userMsgs = { L"copied prompt", L"own prompt" };
    a.userMsgUuids = { L"u-copied", L"u-own" };
    {
        ConversationSegment before;
        before.label = L"compacted";
        before.userMsgs = { L"before the compaction" };
        before.uuids = { L"u-before" };
        a.previousSegments = { before };
    }
    ConversationUpstream up;
    {
        ConversationSegment s1; // session A, its part before its own /compact
        s1.sessionId = L"aaaaaaaa-0000-4000-8000-000000000001";
        s1.sessionTitle = L"Parent title";
        s1.label = L"compacted";
        s1.userMsgs = { L"parent one", L"parent two" };
        s1.uuids = { L"u-p1", L"u-p2" };
        ConversationSegment s2; // session A, its last part — the prompt the fork copied
        s2.sessionId = s1.sessionId;
        s2.sessionTitle = s1.sessionTitle;
        s2.userMsgs = { L"copied prompt" };
        s2.uuids = { L"u-copied" };
        s2.continuedBy = L"fork";
        ConversationSegment s3; // session B, a link of the chain with nothing listed
        s3.sessionId = L"bbbbbbbb-0000-4000-8000-000000000002";
        s3.continuedBy = L"clear";
        up.segments = { s1, s2, s3 };
        up.uuids = { L"u-p1", L"u-p2", L"u-copied" };
    }
    const auto titleOf = [](const std::wstring& sid) {
        return sid == L"bbbbbbbb-0000-4000-8000-000000000002" ? std::wstring{ L"Agentmaster title" } : std::wstring{};
    };

    const auto rows = BuildSummaryConversation(a, up, /*showHistory*/ true, titleOf);
    CHECK(rows.size() == 7, "summary-conversation: 5 prompts + 2 separators");
    if (rows.size() == 7)
    {
        CHECK(rows[0].text == L"parent one" && rows[0].headIndex == -1 && rows[0].sessionId.empty() &&
                  rows[1].text == L"parent two" && rows[1].headIndex == -1,
              "summary-conversation: the earliest session's prompts come first, with no jump index");
        CHECK(rows[2].text == L"copied prompt" && rows[2].headIndex == 0 && rows[2].sessionId.empty(),
              "summary-conversation: a fork's copy is listed under the session that typed it, keeping its jump index");
        CHECK(rows[3].sessionId == L"aaaaaaaa-0000-4000-8000-000000000001" && rows[3].continuedBy == L"fork" &&
                  rows[3].sessionTitle == L"Parent title" && rows[3].text.empty(),
              "summary-conversation: ONE separator after the session's LAST segment, the transcript's title when titleOf has none");
        CHECK(rows[4].sessionId == L"bbbbbbbb-0000-4000-8000-000000000002" && rows[4].continuedBy == L"clear" &&
                  rows[4].sessionTitle == L"Agentmaster title",
              "summary-conversation: a chain session with nothing listed still gets its separator; titleOf wins");
        CHECK(rows[5].text == L"before the compaction" && rows[5].headIndex == -1,
              "summary-conversation: the session's own pre-/compact part follows, with no jump index");
        CHECK(rows[6].text == L"own prompt" && rows[6].headIndex == 1,
              "summary-conversation: the current part comes last, each prompt with its index in the jump list");
    }
    int prompts = 0;
    int copies = 0;
    for (const auto& row : rows)
    {
        if (row.sessionId.empty())
        {
            ++prompts;
            copies += row.text == L"copied prompt" ? 1 : 0;
        }
    }
    CHECK(prompts == 5 && copies == 1, "summary-conversation: every prompt listed exactly once (numbered 1..5 by the renderer)");

    // The toggle OFF: the classic list — the current part alone, the fork's copy included, index == position.
    const auto classic = BuildSummaryConversation(a, up, /*showHistory*/ false, titleOf);
    CHECK(classic.size() == 2 && classic[0].text == L"copied prompt" && classic[0].headIndex == 0 &&
              classic[1].text == L"own prompt" && classic[1].headIndex == 1 && classic[0].sessionId.empty() && classic[1].sessionId.empty(),
          "summary-conversation: showHistory OFF lists the part after the last compaction, nothing else");

    // No chain: the session's own parts only, no separator.
    const auto alone = BuildSummaryConversation(a, ConversationUpstream{}, true, {});
    CHECK(alone.size() == 3 && alone[0].text == L"before the compaction" && alone[1].text == L"copied prompt" && alone[1].headIndex == 0 &&
              alone[2].headIndex == 1,
          "summary-conversation: a session that continues nothing lists its own parts, oldest first");

    // A short uuid list (a defensive bound): the uuid-less prompt is never treated as owned upstream.
    {
        SessionSummary shortA;
        shortA.userMsgs = { L"x", L"y" };
        shortA.userMsgUuids = { L"u-x" };
        ConversationUpstream shortUp;
        shortUp.uuids = { L"" };
        const auto shortRows = BuildSummaryConversation(shortA, shortUp, true, {});
        CHECK(shortRows.size() == 2 && shortRows[1].text == L"y" && shortRows[1].headIndex == 1,
              "summary-conversation: a uuid list shorter than its prompts never drops or misattributes a prompt");
    }

    // Codex: one row per rollout prompt, no jump index, no separator.
    const auto codexRows = CodexSummaryConversation({ L"one", L"two" });
    CHECK(codexRows.size() == 2 && codexRows[0].text == L"one" && codexRows[1].text == L"two" && codexRows[0].headIndex == -1 &&
              codexRows[1].headIndex == -1 && codexRows[0].sessionId.empty(),
          "summary-conversation: Codex rows are the rollout prompts, unjumpable");

    // The separator's label (SummarySeparatorLabel) on the rows built above: the fork's parent (rows[3]) leads
    // its id with "fork", the /clear predecessor (rows[4]) keeps the bare id; 8 characters for the panel's
    // rule, the whole id (0) for the copied Summary; a prompt row has none.
    if (rows.size() == 7)
    {
        CHECK(SummarySeparatorLabel(rows[3], 8) == L"fork aaaaaaaa",
              "summary-separator-label: a fork's parent reads \"fork\" + the id's first 8 characters (the panel's rule)");
        CHECK(SummarySeparatorLabel(rows[3], 0) == L"fork aaaaaaaa-0000-4000-8000-000000000001",
              "summary-separator-label: idChars 0 = the whole id (the copied Summary's rule), still led by \"fork\"");
        CHECK(SummarySeparatorLabel(rows[4], 8) == L"bbbbbbbb", "summary-separator-label: a /clear predecessor keeps the bare id");
        CHECK(SummarySeparatorLabel(rows[0], 8).empty(), "summary-separator-label: a prompt row has no label");
    }
    // The other links and the edges, on a hand-built separator row.
    {
        SummaryConversationRow sep;
        sep.sessionId = L"cccccccc-0000-4000-8000-000000000003";
        sep.continuedBy = L"plan";
        const bool plan = SummarySeparatorLabel(sep, 8) == L"cccccccc";
        sep.continuedBy = L"handover";
        const bool handover = SummarySeparatorLabel(sep, 8) == L"cccccccc";
        sep.continuedBy.clear();
        const bool unknown = SummarySeparatorLabel(sep, 8) == L"cccccccc";
        sep.continuedBy = L"Fork"; // the exact token only: a near miss must never read as a fork
        const bool nearMiss = SummarySeparatorLabel(sep, 8) == L"cccccccc";
        CHECK(plan && handover && unknown && nearMiss,
              "summary-separator-label: a plan-mode restart, a /handover, an unknown link and a near-miss token keep the bare id");
        sep.sessionId = L"abc";
        sep.continuedBy = L"fork";
        CHECK(SummarySeparatorLabel(sep, 8) == L"fork abc", "summary-separator-label: an id shorter than idChars shows whole");
    }

    // The engine's box renderer with a CONVERSATION (RenderSessionSummaryBox's `conversation`: the rich hover card's
    // Messages, 2026-11-20). The owner's case first: a session auto-compacted AFTER its last prompt has an EMPTY
    // current part — the classic box listed no message at all; the conversation lists every earlier part's prompt,
    // numbered 1..N straight through, with no separator between /compact parts of the one session.
    {
        SessionSummary compacted;
        compacted.found = true;
        ConversationSegment first;
        first.label = L"compacted";
        first.userMsgs = { L"rebrand KvNode", L"create GrpcAsm" };
        first.uuids = { L"u-1", L"u-2" };
        ConversationSegment second;
        second.label = L"compacted";
        second.userMsgs = { L"update docs and skills" };
        second.uuids = { L"u-3" };
        compacted.previousSegments = { first, second };
        const auto classicBox = RenderSessionSummaryBox(compacted, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false);
        CHECK(classicBox.find(L" 1. ") == std::wstring::npos,
              "summary-box: WITHOUT a conversation, a session compacted since its last prompt lists no message (the bug's shape)");
        const auto convo = BuildSummaryConversation(compacted, ConversationUpstream{}, /*showHistory*/ true, {});
        const auto box = RenderSessionSummaryBox(compacted, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false, false, true, &convo);
        CHECK(box.find(L" 1. rebrand KvNode") != std::wstring::npos && box.find(L" 2. create GrpcAsm") != std::wstring::npos &&
                  box.find(L" 3. update docs and skills") != std::wstring::npos,
              "summary-box: WITH the conversation, every compacted part's prompt is listed, numbered 1..3 straight through");
        CHECK(box.find(kSummarySessionSepMark) == std::wstring::npos,
              "summary-box: no session separator between /compact parts of ONE session");
        const auto offRows = BuildSummaryConversation(compacted, ConversationUpstream{}, /*showHistory*/ false, {});
        const auto offBox = RenderSessionSummaryBox(compacted, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false, false, true, &offRows);
        CHECK(offBox.find(L" 1. ") == std::wstring::npos,
              "summary-box: the history toggle OFF keeps the classic view (the part after the last /compact only)");
    }
    // The rows built above (a fork's parent, a /clear predecessor, a pre-/compact part, the current part): the numbers
    // keep counting across sessions, and each separator row is a lone `kSummarySessionSepMark + <its sessionId>` line
    // right after the earlier session's last prompt — the field written verbatim (the hover card stores its LABEL there).
    if (rows.size() == 7)
    {
        const auto box = RenderSessionSummaryBox(a, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false, false, true, &rows);
        const std::wstring sepA = std::wstring{ kSummarySessionSepMark } + L"aaaaaaaa-0000-4000-8000-000000000001\n";
        const std::wstring sepB = std::wstring{ kSummarySessionSepMark } + L"bbbbbbbb-0000-4000-8000-000000000002\n";
        const size_t p3 = box.find(L" 3. copied prompt\n");
        const size_t pA = box.find(sepA);
        const size_t pB = box.find(sepB);
        const size_t p4 = box.find(L" 4. before the compaction\n");
        const size_t p5 = box.find(L" 5. own prompt");
        CHECK(box.find(L" 1. parent one\n") != std::wstring::npos && box.find(L" 2. parent two\n") != std::wstring::npos && p3 != std::wstring::npos &&
                  p4 != std::wstring::npos && p5 != std::wstring::npos,
              "summary-box: every prompt of the whole conversation is numbered 1..5 across sessions");
        CHECK(p3 != std::wstring::npos && pA == p3 + std::wstring{ L" 3. copied prompt\n" }.size() && pB == pA + sepA.size() &&
                  p4 == pB + sepB.size(),
              "summary-box: each earlier session is closed by its own separator line, right after its last prompt, in row order");
        auto labelled = rows;
        labelled[3].sessionId = SummarySeparatorLabel(rows[3], 8);
        const auto labelledBox = RenderSessionSummaryBox(a, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false, false, true, &labelled);
        CHECK(labelledBox.find(std::wstring{ kSummarySessionSepMark } + L"fork aaaaaaaa\n") != std::wstring::npos,
              "summary-box: a separator line carries the row's sessionId verbatim (the hover card's label: \"fork aaaaaaaa\")");
        const auto classic = RenderSessionSummaryBox(a, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false);
        CHECK(classic.find(L" 1. copied prompt") != std::wstring::npos && classic.find(L" 2. own prompt") != std::wstring::npos &&
                  classic.find(kSummarySessionSepMark) == std::wstring::npos && classic.find(L"parent one") == std::wstring::npos,
              "summary-box: with no conversation (null, the default) the box is the classic current-part list, unchanged");
    }

    // ---- the summary's compaction lines (2026-11-20, the owner's pick for the summary panel and the hover card) ----
    // The label pieces, spelled once: " · " between fields, " ×" before a count, "→" between the token counts.
    const std::wstring dot = L" \x00B7 ";
    const std::wstring times = L" \x00D7";
    const std::wstring arrow = L"\x2192";
    // A compaction's own label, the way SeFormatCompactionLabel writes it: "compacted[ · <trigger>][ · <pre>→<post>]".
    const auto compactionLabel = [&](const std::wstring& trigger, const std::wstring& pre, const std::wstring& post) {
        std::wstring l = L"compacted";
        if (!trigger.empty())
        {
            l += dot + trigger;
        }
        if (!pre.empty())
        {
            l += dot + pre + arrow + post;
        }
        return l;
    };
    const std::wstring autoA = compactionLabel(L"auto", L"917k", L"23k");
    const std::wstring autoB = compactionLabel(L"auto", L"920k", L"25k");
    const std::wstring manualC = compactionLabel(L"manual", L"30k", L"5k");

    // SummaryCompactionLabel: one label as it is; several merged into one line.
    CHECK(SummaryCompactionLabel({}).empty() && SummaryCompactionLabel({ autoA }) == autoA,
          "summary-compaction-label: no label reads empty; ONE compaction's line is its own label, unchanged");
    CHECK(SummaryCompactionLabel({ autoA, autoB }) == L"compacted" + dot + L"auto" + times + L"2" + dot + L"917k" + arrow + L"23k, 920k" + arrow + L"25k",
          "summary-compaction-label: two of one trigger merge as \"compacted · auto ×2 · 917k→23k, 920k→25k\" (the owner's mockup)");
    CHECK(SummaryCompactionLabel({ autoA, manualC }) == L"compacted" + times + L"2" + dot + L"auto" + dot + L"917k" + arrow + L"23k, manual" + dot + L"30k" + arrow + L"5k",
          "summary-compaction-label: mixed triggers keep each compaction's trigger beside its numbers");
    {
        const std::wstring noTrigger = compactionLabel(L"", L"917k", L"23k"); // "compacted · 917k→23k"
        CHECK(SummaryCompactionLabel({ noTrigger, noTrigger }) == L"compacted" + times + L"2" + dot + L"917k" + arrow + L"23k, 917k" + arrow + L"23k",
              "summary-compaction-label: a label with no trigger never reads its numbers as a trigger");
        CHECK(SummaryCompactionLabel({ L"compacted", L"compacted" }) == L"compacted" + times + L"2",
              "summary-compaction-label: bare labels (no metadata) merge to the count alone");
    }
    {
        std::vector<std::wstring> three;
        std::vector<std::wstring> five;
        for (int k = 1; k <= 5; ++k)
        {
            const std::wstring l = compactionLabel(L"auto", std::to_wstring(900 + k) + L"k", std::to_wstring(20 + k) + L"k");
            five.push_back(l);
            if (k <= static_cast<int>(kSummaryCompactionsListed))
            {
                three.push_back(l);
            }
        }
        CHECK(SummaryCompactionLabel(three) == L"compacted" + dot + L"auto" + times + L"3" + dot + L"901k" + arrow + L"21k, 902k" + arrow + L"22k, 903k" + arrow + L"23k",
              "summary-compaction-label: up to kSummaryCompactionsListed compactions every number pair is listed");
        CHECK(SummaryCompactionLabel(five) == L"compacted" + dot + L"auto" + times + L"5" + dot + L"901k" + arrow + L"21k, \x2026, 905k" + arrow + L"25k",
              "summary-compaction-label: past it only the first and the last, around \"…\" — the line stays short (×5 keeps the count)");
    }

    // SummaryLineIsListHeader: the "<Label>:" lines whose "* " items draw one line each (the panel and the hover card).
    CHECK(SummaryLineIsListHeader(L"Files Read:") && SummaryLineIsListHeader(L"Files Created:") && SummaryLineIsListHeader(L"Skills Loaded:"),
          "summary-list-header: the box's list sections are list headers");
    CHECK(!SummaryLineIsListHeader(L" 1. do this:") && !SummaryLineIsListHeader(L"Recap: where we are") &&
              !SummaryLineIsListHeader(L"Tasks:  3 done / 0 pending") && !SummaryLineIsListHeader(L"Files2:") && !SummaryLineIsListHeader(L":") &&
              !SummaryLineIsListHeader(L"") && !SummaryLineIsListHeader(std::wstring{ kSummaryCompactionMark } + L"compacted:") &&
              !SummaryLineIsListHeader(std::wstring{ kSummarySessionSepMark } + L"abc:"),
          "summary-list-header: a numbered prompt, text after the colon, a digit, a lone colon and a marked line are not");

    // BuildSummaryConversation's COMPACTION rows: after a segment's prompts, before its session's separator; a boundary
    // this session only COPIED (by uuid, in ConversationUpstream::uuids) is drawn once, in the earlier session's part.
    {
        SessionSummary c;
        c.userMsgs = { L"now" };
        c.userMsgUuids = { L"u-now" };
        ConversationSegment own1; // this session's part before its first own /compact, then a copied boundary carried onto it
        own1.userMsgs = { L"first" };
        own1.uuids = { L"u-first" };
        own1.compactions = { SegmentCompaction{ autoA, L"c-own" }, SegmentCompaction{ autoB, L"c-copied" } };
        ConversationSegment own2; // a part of copies only, closed by a copied boundary: no row at all
        own2.userMsgs = { L"parent prompt" };
        own2.uuids = { L"u-pp" };
        own2.compactions = { SegmentCompaction{ manualC, L"c-copied-2" } };
        c.previousSegments = { own1, own2 };
        ConversationUpstream cu;
        {
            ConversationSegment parent;
            parent.sessionId = L"dddddddd-0000-4000-8000-000000000004";
            parent.userMsgs = { L"parent prompt" };
            parent.uuids = { L"u-pp" };
            parent.compactions = { SegmentCompaction{ manualC, L"c-parent" } };
            parent.continuedBy = L"fork";
            cu.segments = { parent };
            cu.uuids = { L"u-pp", L"c-parent", L"c-copied", L"c-copied-2" };
        }
        const auto rowsC = BuildSummaryConversation(c, cu, /*showHistory*/ true, {});
        CHECK(rowsC.size() == 6, "summary-compaction-rows: 3 prompts + the parent's compaction + its separator + one own compaction");
        if (rowsC.size() == 6)
        {
            CHECK(rowsC[0].text == L"parent prompt" && rowsC[1].compactions == std::vector<std::wstring>{ manualC } && rowsC[1].text.empty() &&
                      rowsC[1].sessionId.empty() && rowsC[1].headIndex == -1 && rowsC[2].sessionId == L"dddddddd-0000-4000-8000-000000000004",
                  "summary-compaction-rows: an earlier session's compaction follows its prompts and comes BEFORE its separator");
            CHECK(rowsC[3].text == L"first" && rowsC[4].compactions == std::vector<std::wstring>{ autoA } && rowsC[5].text == L"now" && rowsC[5].headIndex == 0,
                  "summary-compaction-rows: this session's own compaction follows its part; the copied ones are left out");
        }
        size_t compactionRows = 0;
        for (const auto& row : rowsC)
        {
            compactionRows += row.compactions.empty() ? 0 : 1;
        }
        CHECK(compactionRows == 2, "summary-compaction-rows: a part with nothing but copies adds no compaction row");
        const auto offC = BuildSummaryConversation(c, cu, /*showHistory*/ false, {});
        CHECK(offC.size() == 1 && offC[0].text == L"now" && offC[0].compactions.empty(),
              "summary-compaction-rows: the history toggle OFF lists no compaction (only the part after the last one)");

        // The engine's renderer: a compaction row is a lone `kSummaryCompactionMark + <label>` line that takes NO
        // number — the prompt after it continues the count.
        if (rowsC.size() == 6)
        {
            const auto box = RenderSessionSummaryBox(c, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false, false, true, &rowsC);
            const std::wstring want = L" 1. parent prompt\n" + std::wstring{ kSummaryCompactionMark } + manualC + L"\n" + std::wstring{ kSummarySessionSepMark } +
                                      L"dddddddd-0000-4000-8000-000000000004\n 2. first\n" + std::wstring{ kSummaryCompactionMark } + autoA + L"\n 3. now";
            CHECK(box.find(want) != std::wstring::npos,
                  "summary-box: each compaction is its own marked line between the prompts, and the numbers run straight past it");
            auto merged = rowsC;
            merged[4].compactions = { autoA, autoB };
            const auto mergedBox = RenderSessionSummaryBox(c, L"", L"K:\\x", L"t.jsonl", L"", L"", L"", L"", /*full*/ false, false, true, &merged);
            CHECK(mergedBox.find(std::wstring{ kSummaryCompactionMark } + SummaryCompactionLabel({ autoA, autoB }) + L"\n 3. now") != std::wstring::npos,
                  "summary-box: a row of several compactions is ONE line with the merged label");
        }
    }
}

// Agentmaster (the summary's conversation chain — TranscriptTurns.h section 8): CollectConversationUpstream
// on synthetic transcripts in a scratch project tree, with pinned creation times. Covers each kind of link
// (a fork family three deep, a plan-mode restart, a recorded /clear predecessor, a recorded /handover origin
// in another project folder), that nothing DOWNSTREAM is ever listed, the analyzer's uuid lockstep the
// attribution relies on, the whole numbered list for a fork end to end, the gate + cache rules (an
// unsettled head and a young head are never cached; a settled one is), and the never-throw contract.
void TestConversationUpstream()
{
    std::wprintf(L"\n[ConversationUpstream] the summary's whole conversation: CollectConversationUpstream end to end\n");

    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    const fs::path root = fs::path{ tmp } / (L"am_upstream_" + std::to_wstring(::GetCurrentProcessId()));
    const fs::path folder = root / L"projects" / L"K--am-upstream";
    const fs::path otherFolder = root / L"projects" / L"K--am-upstream-other";
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // The sessions (transcript stems) and their creation times. A fork's parent is found by creation ORDER
    // (and the gate reads a copied first line against the birth), so every transcript gets an explicit one.
    const std::wstring idP = L"11111111-0000-4000-8000-000000000001"; // the root of a fork family
    const std::wstring idF = L"11111111-0000-4000-8000-000000000002"; // a fork of P
    const std::wstring idG = L"11111111-0000-4000-8000-000000000003"; // a fork of F (the head of the family)
    const std::wstring idQ = L"22222222-0000-4000-8000-000000000001"; // a plan-mode parent
    const std::wstring idR = L"22222222-0000-4000-8000-000000000002"; // its restart
    const std::wstring idX = L"33333333-0000-4000-8000-000000000001"; // a /clear predecessor
    const std::wstring idY = L"33333333-0000-4000-8000-000000000002"; // its successor
    const std::wstring idX2 = L"44444444-0000-4000-8000-000000000001"; // a /handover origin (another project folder)
    const std::wstring idY2 = L"44444444-0000-4000-8000-000000000002"; // its successor
    const std::wstring idZ = L"55555555-0000-4000-8000-000000000001"; // a transcript caught before its first prompt
    const std::wstring idW = L"55555555-0000-4000-8000-000000000002"; // a transcript created just now
    const std::wstring idR2 = L"22222222-0000-4000-8000-000000000003"; // a second restart of Q, asked about after Q is gone
    const auto at = [](const wchar_t* iso) { return static_cast<double>(TranscriptTimestampToUnixMicros(iso).value_or(0)) / 1e6; };
    std::map<std::wstring, double> births;
    births[idP] = at(L"2026-01-10T08:00:00Z");
    births[idF] = at(L"2026-01-10T11:00:00Z");
    births[idG] = at(L"2026-01-10T12:00:00Z");
    births[idQ] = at(L"2026-01-11T08:00:00Z");
    births[idR] = at(L"2026-01-11T08:06:30Z");
    births[idR2] = at(L"2026-01-11T08:06:40Z");
    births[idZ] = at(L"2026-01-11T08:06:50Z");
    births[idX] = at(L"2026-01-12T08:00:00Z");
    births[idY] = at(L"2026-01-12T08:20:00Z");
    births[idX2] = at(L"2026-01-12T10:00:00Z");
    births[idY2] = at(L"2026-01-12T10:30:00Z");
    // (idW's birth is set right before W is used: "created just now" must still hold when it is asked about.)
    const TurnsBirthFn born = [&births](const fs::path& path) -> std::optional<double> {
        const auto it = births.find(path.stem().wstring());
        if (it == births.end())
        {
            return std::nullopt;
        }
        return it->second;
    };
    const auto pathOf = [&](const fs::path& dir, const std::wstring& id) { return dir / (id + L".jsonl"); };
    const auto n = [](const std::wstring& id) { return NarrowAsciiForCheck(id); };

    // ---- the transcripts ----
    // P: two prompts, a manual /compact, a prompt, then — AFTER F forked it — more of its own work.
    const std::string pCopied = UpCompactionLines("p-c1", "p-a2", "p-s1", "2026-01-10T08:55:00.000Z", "2026-01-10T08:55:01.000Z") +
                                UpUserLine("p-u3", "p-s1", "2026-01-10T09:30:00.000Z", "try a stop loss") +
                                UpAssistantLine("p-a3", "p-u3", "2026-01-10T09:30:10.000Z", "Stop loss done.");
    bool written = WriteUpstreamTranscript(pathOf(folder, idP),
                                           UpUserLine("p-u1", "", "2026-01-10T08:00:00.000Z", "study the market data") +
                                               UpAssistantLine("p-a1", "p-u1", "2026-01-10T08:00:10.000Z", "Studied.") +
                                               UpUserLine("p-u2", "p-a1", "2026-01-10T08:30:00.000Z", "backtest the strategy") +
                                               UpAssistantLine("p-a2", "p-u2", "2026-01-10T08:30:05.000Z", "Backtested.") + pCopied +
                                               UpUserLine("p-u4", "p-a3", "2026-01-10T11:30:00.000Z", "P keeps going after the fork") +
                                               UpAssistantLine("p-a4", "p-u4", "2026-01-10T11:30:05.000Z", "P's own later work."));
    // F: a fork of P — P's context from its last /compact on, copied with the SAME uuids — then its own
    // prompt, then (after G forked it) more of its own work.
    const std::string fCopied = pCopied + UpUserLine("f-u1", "p-a3", "2026-01-10T11:05:00.000Z", "fork: try MSFT") +
                                UpAssistantLine("f-a1", "f-u1", "2026-01-10T11:05:05.000Z", "MSFT done.");
    written = WriteUpstreamTranscript(pathOf(folder, idF),
                                      fCopied + UpUserLine("f-u2", "f-a1", "2026-01-10T12:30:00.000Z", "F keeps going after its fork") +
                                          UpAssistantLine("f-a2", "f-u2", "2026-01-10T12:30:05.000Z", "F's own later work.")) &&
              written;
    // G: a fork of F.
    written = WriteUpstreamTranscript(pathOf(folder, idG),
                                      fCopied + UpUserLine("g-u1", "f-a1", "2026-01-10T12:05:00.000Z", "fork again: tune the stop") +
                                          UpAssistantLine("g-a1", "g-u1", "2026-01-10T12:05:10.000Z", "Tuned.")) &&
              written;
    // Q + R: a plan-mode restart — R's first prompt names Q's transcript; Q went on after R began.
    written = WriteUpstreamTranscript(pathOf(folder, idQ),
                                      UpUserLine("q-u1", "", "2026-01-11T08:00:00.000Z", "plan the export feature") +
                                          UpAssistantLine("q-a1", "q-u1", "2026-01-11T08:05:00.000Z", "Here is the plan.") +
                                          UpUserLine("q-u2", "q-a1", "2026-01-11T09:00:00.000Z", "a later prompt in the parent") +
                                          UpAssistantLine("q-a2", "q-u2", "2026-01-11T09:00:05.000Z", "A later answer.")) &&
              written;
    const auto planPromptFor = [&](const std::wstring& parentId) {
        return "Implement the following plan:\\n\\n# Plan\\n1. Add an export command\\n\\nIf you need specific details from before "
               "exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: "
               "C:\\\\fake\\\\" +
               n(parentId) + ".jsonl";
    };
    written = WriteUpstreamTranscript(pathOf(folder, idR),
                                      UpUserLine("r-u1", "", "2026-01-11T08:06:31.000Z", planPromptFor(idQ)) +
                                          UpAssistantLine("r-a1", "r-u1", "2026-01-11T08:07:00.000Z", "Export added.")) &&
              written;
    // X + Y: a /clear — no transcript links them; X was resumed later, after Y began.
    written = WriteUpstreamTranscript(pathOf(folder, idX),
                                      UpUserLine("x-u1", "", "2026-01-12T08:00:00.000Z", "first X prompt") +
                                          UpAssistantLine("x-a1", "x-u1", "2026-01-12T08:00:05.000Z", "X one.") +
                                          UpUserLine("x-u2", "x-a1", "2026-01-12T08:10:00.000Z", "second X prompt") +
                                          UpAssistantLine("x-a2", "x-u2", "2026-01-12T08:10:05.000Z", "X two.") +
                                          UpUserLine("x-u3", "x-a2", "2026-01-12T09:00:00.000Z", "X resumed later") +
                                          UpAssistantLine("x-a3", "x-u3", "2026-01-12T09:00:05.000Z", "X three.")) &&
              written;
    written = WriteUpstreamTranscript(pathOf(folder, idY),
                                      UpUserLine("y-u1", "", "2026-01-12T08:20:05.000Z", "Y prompt") +
                                          UpAssistantLine("y-a1", "y-u1", "2026-01-12T08:20:10.000Z", "Y one.")) &&
              written;
    // X2 + Y2: a /handover across project folders.
    written = WriteUpstreamTranscript(pathOf(otherFolder, idX2),
                                      UpUserLine("h-u1", "", "2026-01-12T10:00:00.000Z", "origin work") +
                                          UpAssistantLine("h-a1", "h-u1", "2026-01-12T10:00:05.000Z", "Origin done.")) &&
              written;
    written = WriteUpstreamTranscript(pathOf(folder, idY2),
                                      UpUserLine("k-u1", "", "2026-01-12T10:30:05.000Z", "continue from the briefing") +
                                          UpAssistantLine("k-a1", "k-u1", "2026-01-12T10:30:10.000Z", "Continued.")) &&
              written;
    CHECK(written, "upstream: the synthetic transcripts were written");

    // The recorded links (SessionInfo::continuedFromId), as the registry would answer them.
    const RecordedPredecessorFn recorded = [&](const std::wstring& sid) -> std::optional<RecordedPredecessor> {
        if (sid == idY)
        {
            return RecordedPredecessor{ idX, L"clear" };
        }
        if (sid == idY2)
        {
            return RecordedPredecessor{ idX2, L"handover" };
        }
        return std::nullopt;
    };

    // ---- a fork family: G <- F <- P ----
    const auto upG = CollectConversationUpstream(pathOf(folder, idG), {}, born);
    CHECK(upG.segments.size() == 3, "upstream/fork: G continues P (two parts around its /compact) and F");
    if (upG.segments.size() == 3)
    {
        const auto& s0 = upG.segments[0];
        const auto& s1 = upG.segments[1];
        const auto& s2 = upG.segments[2];
        CHECK(s0.sessionId == idP && s1.sessionId == idP && s2.sessionId == idF, "upstream/fork: oldest session first, every segment tagged with its session");
        CHECK(s0.userMsgs == (std::vector<std::wstring>{ L"study the market data", L"backtest the strategy" }) && !s0.label.empty() && s0.continuedBy.empty(),
              "upstream/fork: P's part before its /compact, closed by the compaction's label");
        CHECK(s1.userMsgs == std::vector<std::wstring>{ L"try a stop loss" } && s1.continuedBy == L"fork",
              "upstream/fork: P's last part, up to where F forked it, carries how F continued it");
        CHECK(s2.userMsgs == std::vector<std::wstring>{ L"fork: try MSFT" } && s2.continuedBy == L"fork",
              "upstream/fork: F's own part, up to where G forked it");
        CHECK(s0.uuids == (std::vector<std::wstring>{ L"p-u1", L"p-u2" }) && s1.uuids == std::vector<std::wstring>{ L"p-u3" } &&
                  s2.uuids == std::vector<std::wstring>{ L"f-u1" },
              "upstream/fork: each prompt's transcript-line uuid, in lockstep");
        // The summary's compaction lines: P's /compact is recorded on the part it ended, with the boundary's uuid —
        // once, for P, although F and G each hold a copy of that line.
        CHECK(s0.compactions.size() == 1 && s0.compactions[0].uuid == L"p-c1" && s0.compactions[0].label == s0.label &&
                  s0.label == L"compacted \x00B7 manual \x00B7 409k\x2192" L"5k" && s1.compactions.empty() && s2.compactions.empty(),
              "upstream/fork: P's compaction is on P's part it ended (label + uuid); F's copy of it adds none to F");
    }
    CHECK(upG.uuids.count(L"p-u1") == 1 && upG.uuids.count(L"p-c1") == 1 && upG.uuids.count(L"p-u3") == 1 && upG.uuids.count(L"f-u1") == 1 &&
              upG.uuids.count(L"g-u1") == 0 && upG.uuids.count(L"g-a1") == 0,
          "upstream/fork: the earlier sessions own their lines (the copies included); the head's own lines are not in the set");
    {
        const auto all = AllUpstreamPrompts(upG);
        const bool downstream = std::find(all.begin(), all.end(), L"P keeps going after the fork") != all.end() ||
                                std::find(all.begin(), all.end(), L"F keeps going after its fork") != all.end();
        CHECK(!downstream, "upstream/fork: nothing a parent did AFTER its fork is listed (that is downstream of the fork)");
    }
    CHECK(CollectConversationUpstream(pathOf(folder, idP), recorded, born).segments.empty(), "upstream: the root of the family continues nothing");

    // ---- the analyzer's uuid lockstep (what attributes a fork's copies) ----
    {
        const auto aG = AnalyzeSessionTranscript(pathOf(folder, idG).wstring(), 0);
        CHECK(aG.userMsgs.size() == aG.userMsgUuids.size(), "upstream/analyzer: userMsgUuids runs in lockstep with userMsgs");
        CHECK(aG.userMsgUuids == (std::vector<std::wstring>{ L"p-u3", L"f-u1", L"g-u1" }),
              "upstream/analyzer: a fork's copied prompts keep their parent's uuids");
        const auto aP = AnalyzeSessionTranscript(pathOf(folder, idP).wstring(), 0);
        std::vector<std::wstring> prevMsgs;
        std::vector<std::wstring> prevUuids;
        bool lockstep = true;
        for (const auto& seg : aP.previousSegments)
        {
            lockstep = lockstep && seg.userMsgs.size() == seg.uuids.size();
            prevMsgs.insert(prevMsgs.end(), seg.userMsgs.begin(), seg.userMsgs.end());
            prevUuids.insert(prevUuids.end(), seg.uuids.begin(), seg.uuids.end());
        }
        CHECK(lockstep && prevMsgs == (std::vector<std::wstring>{ L"study the market data", L"backtest the strategy" }) &&
                  prevUuids == (std::vector<std::wstring>{ L"p-u1", L"p-u2" }),
              "upstream/analyzer: the pre-/compact segments carry their prompts' uuids in lockstep");
        CHECK(aP.userMsgUuids == (std::vector<std::wstring>{ L"p-u3", L"p-u4" }), "upstream/analyzer: the current part's uuids");
        CHECK(aP.previousSegments.size() == 1 && aP.previousSegments[0].compactions.size() == 1 && aP.previousSegments[0].compactions[0].uuid == L"p-c1" &&
                  aP.previousSegments[0].compactions[0].label == aP.previousSegments[0].label,
              "upstream/analyzer: an in-file /compact is recorded on the part it ended, with its boundary's uuid");
        // G's file OPENS with its copy of P's boundary: that empty first part is dropped and its compaction carried
        // nowhere (nothing of G's comes before it) — P's part draws it.
        CHECK(aG.compacted && aG.previousSegments.empty(), "upstream/analyzer: a fork's copied boundary at the top of its file adds no part of its own");

        // ---- the whole numbered list for G, end to end ----
        const auto titleOf = [&](const std::wstring& sid) { return sid == idP ? std::wstring{ L"Title of P" } : std::wstring{}; };
        const auto rows = BuildSummaryConversation(aG, upG, /*showHistory*/ true, titleOf);
        CHECK(rows.size() == 8, "upstream/rows: G's conversation = 5 prompts + P's compaction + a separator after P and after F");
        if (rows.size() == 8)
        {
            CHECK(rows[0].text == L"study the market data" && rows[1].text == L"backtest the strategy" && rows[3].text == L"try a stop loss" &&
                      rows[5].text == L"fork: try MSFT" && rows[7].text == L"fork again: tune the stop",
                  "upstream/rows: the prompts in conversation order (the renderer numbers them 1..5)");
            CHECK(rows[2].compactions == std::vector<std::wstring>{ L"compacted \x00B7 manual \x00B7 409k\x2192" L"5k" } && rows[2].text.empty() &&
                      rows[2].sessionId.empty(),
                  "upstream/rows: P's /compact is a compaction row between the prompts it separated — ONE, though G's file holds a copy");
            CHECK(rows[4].sessionId == idP && rows[4].continuedBy == L"fork" && rows[4].sessionTitle == L"Title of P" &&
                      rows[6].sessionId == idF && rows[6].continuedBy == L"fork",
                  "upstream/rows: a separator closes P's part and F's part");
            // The label the panel draws on those separators, from the link the real chain walk found.
            CHECK(SummarySeparatorLabel(rows[4], 8) == L"fork 11111111" && SummarySeparatorLabel(rows[6], 0) == L"fork " + idF,
                  "upstream/rows: each separator of a fork family reads \"fork <id>\" (8 characters on the panel, the whole id in the copy)");
            CHECK(rows[0].headIndex == -1 && rows[1].headIndex == -1 && rows[3].headIndex == 0 && rows[5].headIndex == 1 && rows[7].headIndex == 2,
                  "upstream/rows: the prompts G's terminal holds keep their jump index; the rest have none");
        }
        const auto classic = BuildSummaryConversation(aG, upG, /*showHistory*/ false, titleOf);
        CHECK(classic.size() == 3 && classic[0].text == L"try a stop loss" && classic[2].text == L"fork again: tune the stop",
              "upstream/rows: the toggle OFF lists G's own file, as before the chain existed");
    }

    // ---- the summary's compaction lines, end to end (2026-11-20) ----
    // The one label every boundary below carries (UpCompactionLines' metadata), and the merged label of two in a row.
    const std::wstring manualLabel = L"compacted \x00B7 manual \x00B7 409k\x2192" L"5k";
    const std::wstring twoManual = L"compacted \x00B7 manual \x00D7" L"2 \x00B7 409k\x2192" L"5k, 409k\x2192" L"5k";
    CHECK(SummaryCompactionLabel({ manualLabel, manualLabel }) == twoManual, "upstream/compactions: the merged label of two manual compactions");
    // N <- M: M compacts TWICE in a row, then a prompt, then N forks it (copying M's context from its LAST compaction
    // on, that boundary included). The upstream walk carries the dropped empty part's compaction onto the part before
    // it — ONE "×2" row — and N's own file, which opens with the copied boundary, adds no second one.
    {
        const std::wstring idM = L"66666666-0000-4000-8000-000000000001";
        const std::wstring idN = L"66666666-0000-4000-8000-000000000002";
        births[idM] = at(L"2026-01-13T08:00:00Z");
        births[idN] = at(L"2026-01-13T10:00:00Z");
        const std::string mCopied = UpCompactionLines("m-c2", "m-s1", "m-s2", "2026-01-13T09:00:00.000Z", "2026-01-13T09:00:01.000Z") +
                                    UpUserLine("m-u2", "m-s2", "2026-01-13T09:10:00.000Z", "build the importer") +
                                    UpAssistantLine("m-a2", "m-u2", "2026-01-13T09:10:05.000Z", "Built.");
        bool ok = WriteUpstreamTranscript(pathOf(folder, idM),
                                          UpUserLine("m-u1", "", "2026-01-13T08:00:00.000Z", "plan the importer") +
                                              UpAssistantLine("m-a1", "m-u1", "2026-01-13T08:00:05.000Z", "Planned.") +
                                              UpCompactionLines("m-c1", "m-a1", "m-s1", "2026-01-13T08:30:00.000Z", "2026-01-13T08:30:01.000Z") + mCopied);
        ok = WriteUpstreamTranscript(pathOf(folder, idN),
                                     mCopied + UpUserLine("n-u1", "m-a2", "2026-01-13T10:05:00.000Z", "fork: add a CSV mode") +
                                         UpAssistantLine("n-a1", "n-u1", "2026-01-13T10:05:05.000Z", "CSV added.")) &&
             ok;
        CHECK(ok, "upstream/compactions: the M and N transcripts were written");
        const auto upN = CollectConversationUpstream(pathOf(folder, idN), {}, born);
        CHECK(upN.segments.size() == 2 && upN.segments[0].sessionId == idM && upN.segments[0].userMsgs == std::vector<std::wstring>{ L"plan the importer" } &&
                  upN.segments[0].compactions.size() == 2 && upN.segments[0].compactions[0].uuid == L"m-c1" &&
                  upN.segments[0].compactions[1].uuid == L"m-c2" && upN.segments[1].compactions.empty() &&
                  upN.segments[1].userMsgs == std::vector<std::wstring>{ L"build the importer" } && upN.segments[1].continuedBy == L"fork",
              "upstream/compactions: two compactions in a row are BOTH kept, on the part before them (the empty part between is dropped)");
        const auto aN = AnalyzeSessionTranscript(pathOf(folder, idN).wstring(), 0);
        CHECK(aN.previousSegments.empty() && aN.userMsgs == (std::vector<std::wstring>{ L"build the importer", L"fork: add a CSV mode" }),
              "upstream/compactions: the fork's own file opens with the copied boundary and keeps no part before it");
        const auto rowsN = BuildSummaryConversation(aN, upN, /*showHistory*/ true, {});
        CHECK(rowsN.size() == 5, "upstream/compactions: N's conversation = 3 prompts + ONE compaction row + M's separator");
        if (rowsN.size() == 5)
        {
            CHECK(rowsN[0].text == L"plan the importer" && rowsN[1].compactions == (std::vector<std::wstring>{ manualLabel, manualLabel }) &&
                      rowsN[2].text == L"build the importer" && rowsN[2].headIndex == 0 && rowsN[3].sessionId == idM && rowsN[3].continuedBy == L"fork" &&
                      rowsN[4].text == L"fork: add a CSV mode" && rowsN[4].headIndex == 1,
                  "upstream/compactions: the two compactions are one row between the prompts they separated, before M's separator");
            const auto boxN = RenderSessionSummaryBox(aN, L"", L"K:\\x", pathOf(folder, idN).wstring(), L"", L"", L"", L"", /*full*/ false, false, true, &rowsN);
            CHECK(boxN.find(L" 1. plan the importer\n" + std::wstring{ kSummaryCompactionMark } + twoManual + L"\n 2. build the importer\n" +
                            std::wstring{ kSummarySessionSepMark } + idM + L"\n 3. fork: add a CSV mode") != std::wstring::npos,
                  "upstream/compactions: rendered as ONE \"×2\" line, the numbers running straight past it");
        }
    }
    // The owner's shape (session 8da2a63d): compacted after a prompt, then TWICE after the last prompt — the current part
    // is empty, and the last two compactions both follow the last prompt, as one merged line.
    {
        const fs::path compactFolder = root / L"projects" / L"K--am-upstream-compact";
        const fs::path k = compactFolder / L"77777777-0000-4000-8000-000000000001.jsonl";
        const bool ok = WriteUpstreamTranscript(k, UpUserLine("k-u1", "", "2026-01-14T08:00:00.000Z", "first") +
                                                       UpAssistantLine("k-a1", "k-u1", "2026-01-14T08:00:05.000Z", "One.") +
                                                       UpCompactionLines("k-c1", "k-a1", "k-s1", "2026-01-14T08:30:00.000Z", "2026-01-14T08:30:01.000Z") +
                                                       UpUserLine("k-u2", "k-s1", "2026-01-14T09:00:00.000Z", "second") +
                                                       UpAssistantLine("k-a2", "k-u2", "2026-01-14T09:00:05.000Z", "Two.") +
                                                       UpCompactionLines("k-c2", "k-a2", "k-s2", "2026-01-14T09:30:00.000Z", "2026-01-14T09:30:01.000Z") +
                                                       UpCompactionLines("k-c3", "k-s2", "k-s3", "2026-01-14T10:00:00.000Z", "2026-01-14T10:00:01.000Z"));
        const auto aK = AnalyzeSessionTranscript(k.wstring(), 0);
        CHECK(ok && aK.userMsgs.empty() && aK.previousSegments.size() == 2 && aK.previousSegments[0].compactions.size() == 1 &&
                  aK.previousSegments[1].compactions.size() == 2 && aK.previousSegments[1].compactions[0].uuid == L"k-c2" &&
                  aK.previousSegments[1].compactions[1].uuid == L"k-c3",
              "summary-compactions/analyzer: the compactions after the last prompt are all carried onto its part");
        const auto rowsK = BuildSummaryConversation(aK, ConversationUpstream{}, /*showHistory*/ true, {});
        const auto boxK = RenderSessionSummaryBox(aK, L"", L"K:\\x", k.wstring(), L"", L"", L"", L"", /*full*/ false, false, true, &rowsK);
        CHECK(rowsK.size() == 4 && boxK.find(L" 1. first\n" + std::wstring{ kSummaryCompactionMark } + manualLabel + L"\n 2. second\n" +
                                             std::wstring{ kSummaryCompactionMark } + twoManual) != std::wstring::npos,
              "summary-compactions/box: each prompt, then where the conversation was compacted — the last line says it happened twice");
    }

    // ---- a plan-mode restart: R <- Q ----
    {
        const auto upR = CollectConversationUpstream(pathOf(folder, idR), {}, born);
        CHECK(upR.segments.size() == 1 && upR.segments[0].sessionId == idQ &&
                  upR.segments[0].userMsgs == std::vector<std::wstring>{ L"plan the export feature" } && upR.segments[0].continuedBy == L"plan" &&
                  upR.segments[0].uuids == std::vector<std::wstring>{ L"q-u1" },
              "upstream/plan: the restart continues its plan-mode parent, up to the moment it began");
        CHECK(upR.uuids.count(L"q-u1") == 1 && upR.uuids.count(L"q-u2") == 0 && upR.uuids.count(L"r-u1") == 0,
              "upstream/plan: the parent's later work is not part of this conversation");
    }

    // ---- recorded links: a /clear predecessor, a /handover origin ----
    {
        CHECK(CollectConversationUpstream(pathOf(folder, idY), {}, born).segments.empty(),
              "upstream/recorded: without the record a /clear successor continues nothing (no transcript records the link)");
        const auto upY = CollectConversationUpstream(pathOf(folder, idY), recorded, born);
        CHECK(upY.segments.size() == 1 && upY.segments[0].sessionId == idX &&
                  upY.segments[0].userMsgs == (std::vector<std::wstring>{ L"first X prompt", L"second X prompt" }) && upY.segments[0].continuedBy == L"clear",
              "upstream/recorded: a /clear successor lists the conversation it cleared");
        const auto all = AllUpstreamPrompts(upY);
        CHECK(std::find(all.begin(), all.end(), L"X resumed later") == all.end(),
              "upstream/recorded: the predecessor's lines written AFTER the successor began are not part of the conversation");
        const auto upY2 = CollectConversationUpstream(pathOf(folder, idY2), recorded, born);
        CHECK(upY2.segments.size() == 1 && upY2.segments[0].sessionId == idX2 &&
                  upY2.segments[0].userMsgs == std::vector<std::wstring>{ L"origin work" } && upY2.segments[0].continuedBy == L"handover",
              "upstream/recorded: a /handover origin in ANOTHER project folder is found and followed");
        const RecordedPredecessorFn selfRef = [&](const std::wstring& sid) -> std::optional<RecordedPredecessor> {
            return sid == idY ? std::optional<RecordedPredecessor>{ RecordedPredecessor{ idY, L"clear" } } : std::nullopt;
        };
        CHECK(CollectConversationUpstream(pathOf(folder, idY), selfRef, born).segments.empty(), "upstream/recorded: a record naming the session itself is ignored");
        const RecordedPredecessorFn gone = [&](const std::wstring& sid) -> std::optional<RecordedPredecessor> {
            return sid == idY ? std::optional<RecordedPredecessor>{ RecordedPredecessor{ L"99999999-0000-4000-8000-000000000009", L"clear" } } : std::nullopt;
        };
        CHECK(CollectConversationUpstream(pathOf(folder, idY), gone, born).segments.empty(),
              "upstream/recorded: a predecessor whose transcript is gone adds nothing");
        const RecordedPredecessorFn throwing = [](const std::wstring&) -> std::optional<RecordedPredecessor> {
            throw std::runtime_error("lookup failed");
        };
        CHECK(CollectConversationUpstream(pathOf(folder, idX), throwing, born).segments.empty(),
              "upstream/recorded: a throwing lookup is contained (logged) and reads as none");
    }

    // ---- the cache keeps only FINAL answers ----
    {
        // Z: caught before its first prompt (only a bookkeeping line) — answered "none", NOT cached...
        written = WriteUpstreamTranscript(pathOf(folder, idZ), "{\"type\":\"permission-mode\",\"permissionMode\":\"default\"}\n");
        CHECK(written && CollectConversationUpstream(pathOf(folder, idZ), {}, born).segments.empty(),
              "upstream/settle: a transcript without its first prompt yet continues nothing (yet)");
        // ...so once the first prompt lands — a plan-mode restart of Q — the SAME key answers anew.
        written = WriteUpstreamTranscript(pathOf(folder, idZ),
                                          "{\"type\":\"permission-mode\",\"permissionMode\":\"default\"}\n" +
                                              UpUserLine("z-u1", "", "2026-01-11T08:07:00.000Z", planPromptFor(idQ)) +
                                              UpAssistantLine("z-a1", "z-u1", "2026-01-11T08:07:05.000Z", "On it."));
        const auto upZ = CollectConversationUpstream(pathOf(folder, idZ), {}, born);
        CHECK(written && upZ.segments.size() == 1 && upZ.segments[0].sessionId == idQ,
              "upstream/settle: the 'none' given before the first prompt was not cached");

        // W: created just now — its answer is never cached while it is young (its opening lines may still be
        // mid-write): an ordinary first prompt answers none, and a rewrite into a restart answers anew.
        const auto now = UpstreamNow();
        births[idW] = now.first;
        written = WriteUpstreamTranscript(pathOf(folder, idW), UpUserLine("w-u1", "", now.second, "hello"));
        CHECK(written && CollectConversationUpstream(pathOf(folder, idW), {}, born).segments.empty(), "upstream/settle: a young ordinary session continues nothing");
        written = WriteUpstreamTranscript(pathOf(folder, idW), UpUserLine("w-u1", "", now.second, planPromptFor(idQ)));
        const auto upW = CollectConversationUpstream(pathOf(folder, idW), {}, born);
        CHECK(written && upW.segments.size() == 1 && upW.segments[0].sessionId == idQ && upW.segments[0].continuedBy == L"plan",
              "upstream/settle: a young head's answer was not cached");

        // A settled answer IS cached: with Q gone, R still answers from the cache, while a restart of Q asked
        // about for the first time finds no parent.
        written = WriteUpstreamTranscript(pathOf(folder, idR2),
                                          UpUserLine("r2-u1", "", "2026-01-11T08:06:41.000Z", planPromptFor(idQ)) +
                                              UpAssistantLine("r2-a1", "r2-u1", "2026-01-11T08:06:50.000Z", "Second try."));
        std::error_code ec;
        fs::remove(pathOf(folder, idQ), ec);
        const auto upRCached = CollectConversationUpstream(pathOf(folder, idR), {}, born);
        CHECK(upRCached.segments.size() == 1 && upRCached.segments[0].sessionId == idQ, "upstream/cache: a settled answer is served from the cache");
        CHECK(written && CollectConversationUpstream(pathOf(folder, idR2), {}, born).segments.empty(),
              "upstream/cache: ...while a head asked about for the first time reads the disk (its parent is gone)");
    }

    // ---- never throws ----
    {
        const auto missing = CollectConversationUpstream(pathOf(folder, L"no-such-session"), recorded, born);
        CHECK(missing.segments.empty() && missing.uuids.empty(), "upstream: a missing transcript (no birth) answers none");
        const auto missingDefaultBirth = CollectConversationUpstream(pathOf(folder, L"no-such-session"), {});
        CHECK(missingDefaultBirth.segments.empty(), "upstream: a missing transcript under the real file times answers none");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

namespace
{
    // ---- the transcript overlay's box tabs (TranscriptViews.h) and the one-sided switches (TurnsOptions::userInputs /
    // peerInputs / finalReplies, TRANSCRIPT_OVERLAY.md section 3j) ----

    // make_goldens.py's FIXTURES, as TestTranscriptTurns lists them, and the five levels with the names their goldens carry.
    constexpr const wchar_t* kViewFixtures[] = {
        L"fixture_session.jsonl",
        L"fixture_compaction.jsonl",
        L"fixture_malformed.jsonl",
        L"fixture_questions.jsonl",
        L"fixture_preserved.jsonl",
        L"chain/a4a4a4a4-0000-4000-8000-000000000004.jsonl",
        L"chain/a1a1a1a1-0000-4000-8000-000000000001.jsonl",
        L"chain/b2b2b2b2-0000-4000-8000-000000000002.jsonl",
        L"chain/b3b3b3b3-0000-4000-8000-000000000003.jsonl",
        L"agents/c1c1c1c1-0000-4000-8000-000000000001.jsonl",
        L"agents/c2c2c2c2-0000-4000-8000-000000000002.jsonl",
    };
    const std::pair<const wchar_t*, TurnsLevel> kViewLevels[] = {
        { L"minimal", TurnsLevel::Minimal },
        { L"compact", TurnsLevel::Compact },
        { L"default", TurnsLevel::Default },
        { L"full", TurnsLevel::Full },
        { L"max", TurnsLevel::Max },
    };

    // Renders one reference fixture under `opts` exactly as TestTranscriptTurns renders it for its golden: a chained fixture
    // (a folder with births.json) from its FILE with the pinned creation times, so the chain and the agents are found; any
    // other from its lines alone.
    // Parameters: root - the harness's turns folder; fixture - a kViewFixtures entry; opts - the render's options.
    // Returns the render, "<the render failed>" when the file render failed. Throws std::bad_alloc only.
    std::wstring RenderTurnsFixture(const fs::path& root, const wchar_t* fixture, const TurnsOptions& opts)
    {
        const fs::path file = root / fs::path{ fixture };
        const fs::path birthsFile = file.parent_path() / L"births.json";
        if (!fs::is_regular_file(birthsFile))
        {
            return RenderTranscriptTurns(ParseTranscriptJsonl(ReadTurnsFileBytes(file)), opts, /*utc*/ true);
        }
        const auto births = LoadTurnsBirths(birthsFile);
        const TurnsBirthFn born = [births](const fs::path& path) -> std::optional<double> {
            const auto it = births.find(path.stem().wstring());
            return it == births.end() ? std::nullopt : std::optional<double>{ it->second };
        };
        return RenderTranscriptTurnsFile(file, opts, /*utc*/ true, born).value_or(L"<the render failed>");
    }

    // Whether two option sets are the same in EVERY member - the reference's features and rules and Agentmaster's own. The
    // members are listed by hand; TestTranscriptViews checks sizeof(TurnsOptions), so a member added later fails loudly
    // there instead of being skipped here.
    bool SameTurnsOptions(const TurnsOptions& a, const TurnsOptions& b)
    {
        return a.notes == b.notes && a.times == b.times && a.ops == b.ops && a.shortPaths == b.shortPaths && a.fullBash == b.fullBash &&
               a.stats == b.stats && a.opMeta == b.opMeta && a.opDuration == b.opDuration && a.opOutput == b.opOutput && a.bgEvents == b.bgEvents &&
               a.narration == b.narration && a.compactions == b.compactions && a.answers == b.answers && a.nestedBlocks == b.nestedBlocks &&
               a.compactedHistory == b.compactedHistory && a.chain == b.chain && a.agents == b.agents && a.peerMessages == b.peerMessages &&
               a.reasons == b.reasons && a.plainDashes == b.plainDashes && a.fullWrite == b.fullWrite && a.editDiffs == b.editDiffs &&
               a.lastTurnOnly == b.lastTurnOnly && a.userInputs == b.userInputs && a.peerInputs == b.peerInputs && a.finalReplies == b.finalReplies &&
               a.liveBranch == b.liveBranch && a.skipTaskNotices == b.skipTaskNotices && a.skipSidechains == b.skipSidechains &&
               a.skipSynthetic == b.skipSynthetic && a.keepPrePromptWork == b.keepPrePromptWork;
    }
    // How many TurnsOptions members SameTurnsOptions compares (all of them bools, so this is also its size in bytes).
    constexpr size_t kTurnsOptionsMembers = 31;

    // The ticked list that names exactly the boxes whose features `opts` turns on - what a box tab ticks to show a level.
    std::vector<std::wstring> TicksFor(const TurnsOptions& opts)
    {
        std::vector<std::wstring> ticks;
        for (const auto& box : kTranscriptViewBoxes)
        {
            if (opts.*box.feature)
            {
                ticks.emplace_back(box.name);
            }
        }
        return ticks;
    }

    // How many lines of `text` start with `prefix`.
    size_t CountLinesStartingWith(const std::wstring& text, std::wstring_view prefix)
    {
        size_t count = 0;
        for (const auto& line : SplitTurnsLines(text))
        {
            if (line.size() >= prefix.size() && std::wstring_view{ line }.substr(0, prefix.size()) == prefix)
            {
                ++count;
            }
        }
        return count;
    }
}

// Agentmaster (TurnsOptions::userInputs / peerInputs / finalReplies - the transcript overlay's User Only and Assistant Only
// tabs, TRANSCRIPT_OVERLAY.md section 3j): NOT part of the port, so no golden covers them directly - but each only REMOVES
// parts of a turn, so every one of the 11 reference fixtures at each of the 5 levels pins them: a one-sided render's lines
// are the golden's lines in order with some left out, and what is left out is exactly its side. User Only (peerInputs and
// finalReplies off) keeps every message of the user's and no teammate's; Assistant Only (userInputs and peerInputs off)
// keeps every turn and every "● Claude" part and no message at all. Then the synthetic cases: turns a teammate started are
// skipped with their numbers kept; Claude's part without its reply keeps the boxes' lines; a level never switches a side off.
void TestTranscriptTurnsSides()
{
    std::wprintf(L"\n[TranscriptTurns] one side of each turn (userInputs / peerInputs / finalReplies: the overlay's User Only, Assistant Only)\n");

    // ---- no level and no preset switches a side off: every level is still the reference's text ----
    {
        const TurnsOptions latest{};
        bool allOn = latest.userInputs && latest.peerInputs && latest.finalReplies;
        for (const auto& [name, level] : kViewLevels)
        {
            const TurnsOptions opts = TurnsOptionsForLevel(level);
            allOn = allOn && opts.userInputs && opts.peerInputs && opts.finalReplies;
        }
        CHECK(allOn, "TurnsSides: neither the preset nor any level switches a side off (userInputs, peerInputs, finalReplies all on)");
    }

    const fs::path root = TurnsHarnessDir() / L"turns";
    if (!fs::is_directory(root / L"expected"))
    {
        CHECK(false, "TurnsSides: tests\\turns\\expected is missing (the fixtures ship beside m5_tests.exe)");
        return;
    }

    // ---- every golden: a one-sided render only REMOVES lines, and removes what its side says ----
    int matched = 0;
    int total = 0;
    for (const wchar_t* fixture : kViewFixtures)
    {
        for (const auto& [levelName, level] : kViewLevels)
        {
            ++total;
            const std::wstring golden = WidenTurnsUtf8(ReadTurnsFileBytes(root / L"expected" / TurnsGoldenName(fixture, levelName)));
            TurnsOptions userOnly = TurnsOptionsForLevel(level);
            userOnly.peerInputs = false;
            userOnly.finalReplies = false;
            TurnsOptions assistantOnly = TurnsOptionsForLevel(level);
            assistantOnly.userInputs = false;
            assistantOnly.peerInputs = false;
            const std::wstring user = RenderTurnsFixture(root, fixture, userOnly);
            const std::wstring assistant = RenderTurnsFixture(root, fixture, assistantOnly);
            const std::string tag = NarrowAsciiForCheck(TurnsGoldenName(fixture, levelName));
            // User Only: the golden's lines in order; every "❯ User" label kept, no "✉ <sender>" label; at Minimal (where
            // Claude's part is its reply alone) no "● Claude" label left at all.
            const bool userOk = !golden.empty() && TurnsLinesKeptInOrder(user, golden) &&
                                CountLinesStartingWith(user, L"**\x276F User") == CountLinesStartingWith(golden, L"**\x276F User") &&
                                CountLinesStartingWith(user, L"**\x2709 ") == 0 &&
                                (level != TurnsLevel::Minimal || CountLinesStartingWith(user, L"**\x25CF Claude**") == 0);
            // Assistant Only: the golden's lines in order; no message label and no mid-turn opening note; every turn heading
            // (and the session headings with them) and every "● Claude" part kept.
            const bool assistantOk = !golden.empty() && TurnsLinesKeptInOrder(assistant, golden) &&
                                     CountLinesStartingWith(assistant, L"**\x276F User") == 0 && CountLinesStartingWith(assistant, L"**\x2709 ") == 0 &&
                                     assistant.find(L"_This transcript opens mid-turn") == std::wstring::npos &&
                                     CountLinesStartingWith(assistant, L"#") == CountLinesStartingWith(golden, L"#") &&
                                     CountLinesStartingWith(assistant, L"**\x25CF Claude**") == CountLinesStartingWith(golden, L"**\x25CF Claude**");
            CHECK(userOk, ("TurnsSides: User Only of " + tag + " is the golden less Claude's replies and the teammates' messages").c_str());
            CHECK(assistantOk, ("TurnsSides: Assistant Only of " + tag + " is the golden less every message that reached a turn").c_str());
            if (userOk && assistantOk)
            {
                ++matched;
            }
        }
    }
    std::wprintf(L"  %d of %d goldens hold both one-sided renders\n", matched, total);

    // ---- a team lead (agents/c1): User Only skips the turns teammates started, and the others keep their numbers ----
    {
        TurnsOptions userOnly = TurnsOptionsForLevel(TurnsLevel::Minimal);
        userOnly.peerInputs = false;
        userOnly.finalReplies = false;
        const std::wstring user = RenderTurnsFixture(root, L"agents/c1c1c1c1-0000-4000-8000-000000000001.jsonl", userOnly);
        CHECK(user.rfind(L"### Turn 1 ", 0) == 0 && user.find(L"\n### Turn 5 ") != std::wstring::npos && user.find(L"### Turn 2 ") == std::wstring::npos &&
                  user.find(L"### Turn 3 ") == std::wstring::npos && user.find(L"### Turn 4 ") == std::wstring::npos,
              "TurnsSides: User Only of a team lead shows turns 1 and 5 (the user's) and skips 2-4 (finder-a, finder-b, system started them)");
        CHECK(user.find(L"> review the commit with a team") != std::wstring::npos && user.find(L"finder-a") == std::wstring::npos &&
                  user.find(L"Got finder-a's findings") == std::wstring::npos,
              "TurnsSides: ...with the user's messages, no teammate's message and no reply");
        TurnsOptions assistantOnly = TurnsOptionsForLevel(TurnsLevel::Minimal);
        assistantOnly.userInputs = false;
        assistantOnly.peerInputs = false;
        const std::wstring assistant = RenderTurnsFixture(root, L"agents/c1c1c1c1-0000-4000-8000-000000000001.jsonl", assistantOnly);
        CHECK(CountLinesStartingWith(assistant, L"### Turn ") == 5 && assistant.find(L"> Got finder-a's findings: 1 leak.") != std::wstring::npos &&
                  assistant.find(L"review the commit with a team") == std::wstring::npos && assistant.find(L"is idle (available)") == std::wstring::npos,
              "TurnsSides: Assistant Only of the team lead keeps all 5 turns with their replies and none of the messages");
    }

    // ---- a box from the other side: Claude's part without its reply keeps the boxes' lines ----
    {
        TurnsOptions userAnswers = TurnsOptionsForLevel(TurnsLevel::Minimal);
        userAnswers.answers = true;
        userAnswers.peerInputs = false;
        userAnswers.finalReplies = false;
        const std::wstring answers = RenderTurnsFixture(root, L"fixture_questions.jsonl", userAnswers);
        CHECK(answers.find(L"> no need, ask me again\n\n**\x25CF Claude**\n- \x276F Which island first? \x2192 **A**\n- \x276F How many trials? \x2192 **8**\n") !=
                      std::wstring::npos &&
                  answers.find(L"Stopping here until you say how to proceed.") == std::wstring::npos &&
                  answers.find(L"What would you like to clarify?") == std::wstring::npos,
              "TurnsSides: User Only + answers keeps Claude's part as the user's answers, without its replies");
        TurnsOptions userReasons = TurnsOptionsForLevel(TurnsLevel::Minimal);
        userReasons.reasons = true;
        userReasons.peerInputs = false;
        userReasons.finalReplies = false;
        const std::wstring reasons = RenderTurnsFixture(root, L"fixture_session.jsonl", userReasons);
        CHECK(reasons.find(L"> also check the tests\n\n**\x25CF Claude**\n- **Bash**: List files\n") != std::wstring::npos &&
                  reasons.find(L"Done: 2 files") == std::wstring::npos && reasons.find(L"> second try question\n\n### Turn 3") != std::wstring::npos,
              "TurnsSides: User Only + call reasons lists Claude's calls under the user's messages, no reply, and a turn with no call shows no Claude part");
        TurnsOptions assistantReasons = TurnsOptionsForLevel(TurnsLevel::Minimal);
        assistantReasons.reasons = true;
        assistantReasons.userInputs = false;
        assistantReasons.peerInputs = false;
        const std::wstring claude = RenderTurnsFixture(root, L"fixture_session.jsonl", assistantReasons);
        CHECK(claude.find(L"\n\n**\x25CF Claude**\n- **Bash**: List files\n") != std::wstring::npos && claude.find(L"> Done: 2 files") != std::wstring::npos &&
                  claude.find(L"List the files and run the scan") == std::wstring::npos && claude.find(L"also check the tests") == std::wstring::npos,
              "TurnsSides: Assistant Only + call reasons lists the calls and the replies, no message");
    }

    // ---- a three-session chain: every session heading stays over its turns ----
    {
        TurnsOptions userOnly = TurnsOptionsForLevel(TurnsLevel::Minimal);
        userOnly.peerInputs = false;
        userOnly.finalReplies = false;
        const std::wstring user = RenderTurnsFixture(root, L"chain/a4a4a4a4-0000-4000-8000-000000000004.jsonl", userOnly);
        CHECK(user.find(L"## Session 2 of 3 \x00B7 `a3a3a3a3`") != std::wstring::npos && user.find(L"\n\n### Turn 4 ") != std::wstring::npos &&
                  user.find(L"**Session chain**") != std::wstring::npos && user.find(L"MSFT: Sharpe 0.9.") == std::wstring::npos,
              "TurnsSides: User Only of a chain keeps the chain list and each session's heading, without the replies");
    }
}

// Agentmaster - the transcript overlay's box tabs (TranscriptViews.h, TRANSCRIPT_OVERLAY.md section 3j): the views' names
// (what settings.json stores), the twelve boxes against the level ladder, the options each tab renders with, the ticks a
// click leaves, the greyed boxes, the tips - and, end to end over every golden, that Custom with a level's own boxes ticked
// renders EXACTLY that level (so "every box ticked is Max" is a fact, not a promise).
void TestTranscriptViews()
{
    std::wprintf(L"\n[TranscriptViews] the overlay's box tabs: names, boxes, options, ticks, tips, Custom == each level\n");
    const auto custom = static_cast<int>(TranscriptView::Custom);
    const auto userOnly = static_cast<int>(TranscriptView::UserOnly);
    const auto assistantOnly = static_cast<int>(TranscriptView::AssistantOnly);

    // ---- the views: names, labels, which ones are box tabs ----
    {
        bool roundTrip = true;
        std::set<std::wstring> seen;
        bool unique = true;
        for (int view = 0; view < kTranscriptViewCount; ++view)
        {
            const std::wstring name = TranscriptViewName(view);
            roundTrip = roundTrip && TranscriptViewFromName(name) == view;
            unique = unique && seen.insert(name).second;
        }
        CHECK(roundTrip, "TranscriptViews: every view's settings name reads back as that view");
        CHECK(unique, "TranscriptViews: no two views share a settings name");
        bool levelsKeepTheirNames = true;
        for (const auto& [name, level] : kViewLevels)
        {
            levelsKeepTheirNames = levelsKeepTheirNames && std::wstring{ TranscriptViewName(static_cast<int>(level)) } == name &&
                                   std::wstring{ TranscriptViewLabel(static_cast<int>(level)) } == TranscriptLevelLabel(level);
        }
        CHECK(levelsKeepTheirNames, "TranscriptViews: a level's view keeps the level's own name (TurnsLevelName) and label - an older remembered level still opens");
        CHECK(std::wstring{ TranscriptViewName(custom) } == L"custom" && std::wstring{ TranscriptViewName(userOnly) } == L"user-only" &&
                  std::wstring{ TranscriptViewName(assistantOnly) } == L"assistant-only",
              "TranscriptViews: the box tabs' settings names");
        CHECK(std::wstring{ TranscriptViewLabel(custom) } == L"Custom" && std::wstring{ TranscriptViewLabel(userOnly) } == L"User Only" &&
                  std::wstring{ TranscriptViewLabel(assistantOnly) } == L"Assistant Only",
              "TranscriptViews: the box tabs' labels");
        CHECK(TranscriptViewFromName(L"USER-ONLY") == userOnly && TranscriptViewFromName(L"Compact") == static_cast<int>(TurnsLevel::Compact),
              "TranscriptViews: a stored name is read without case (a hand-edited settings.json)");
        CHECK(TranscriptViewFromName(L"") == -1 && TranscriptViewFromName(L"user only") == -1 && TranscriptViewFromName(L"everything") == -1,
              "TranscriptViews: an empty or unknown name finds no view (the overlay opens at Default)");
        CHECK(IsTranscriptBoxView(custom) && IsTranscriptBoxView(userOnly) && IsTranscriptBoxView(assistantOnly) && !IsTranscriptBoxView(static_cast<int>(TurnsLevel::Max)) &&
                  !IsTranscriptBoxView(-1) && !IsTranscriptBoxView(kTranscriptViewCount),
              "TranscriptViews: exactly the three tabs after the levels are box tabs");
        // The Diff tab (TRANSCRIPT_OVERLAY.md 3l): the last view, after the box tabs, and no box tab itself.
        const auto diff = static_cast<int>(TranscriptView::Diff);
        CHECK(diff == kTranscriptViewCount - 1 && std::wstring{ TranscriptViewName(diff) } == L"diff" && std::wstring{ TranscriptViewLabel(diff) } == L"Diff" &&
                  TranscriptViewFromName(L"DIFF") == diff && IsTranscriptDiffView(diff) && !IsTranscriptBoxView(diff) && !IsTranscriptDiffView(assistantOnly) &&
                  !IsTranscriptDiffView(kTranscriptViewCount),
              "TranscriptViews: the Diff tab - the last view, its settings name and label, no box tab");
    }

    // ---- the boxes: one per ladder feature, each in the group of the level that first shows it ----
    {
        // The twelve LADDER features (TurnsOptionsForLevel sets them; TestTranscriptLevelGuide lists the same).
        const std::pair<bool TurnsOptions::*, const char*> ladder[] = {
            { &TurnsOptions::reasons, "reasons" },
            { &TurnsOptions::notes, "notes" },
            { &TurnsOptions::ops, "ops" },
            { &TurnsOptions::fullBash, "fullBash" },
            { &TurnsOptions::stats, "stats" },
            { &TurnsOptions::opDuration, "opDuration" },
            { &TurnsOptions::opOutput, "opOutput" },
            { &TurnsOptions::narration, "narration" },
            { &TurnsOptions::compactions, "compactions" },
            { &TurnsOptions::answers, "answers" },
            { &TurnsOptions::agents, "agents" },
            { &TurnsOptions::fullWrite, "fullWrite" },
        };
        CHECK(std::size(kTranscriptViewBoxes) == std::size(ladder), "TranscriptViews: twelve boxes, one per ladder feature");
        for (const auto& [feature, name] : ladder)
        {
            const auto count = std::count_if(std::begin(kTranscriptViewBoxes), std::end(kTranscriptViewBoxes), [feature](const TranscriptViewBox& box) {
                return box.feature == feature;
            });
            // The box's stored name is the member's own name: what settings.json keeps must never drift from the option.
            const bool named = std::any_of(std::begin(kTranscriptViewBoxes), std::end(kTranscriptViewBoxes), [feature, name](const TranscriptViewBox& box) {
                return box.feature == feature && NarrowAsciiForCheck(box.name) == name;
            });
            const std::string label = std::string{ "TranscriptViews: the ladder feature " } + name + " has exactly one box, named after it";
            CHECK(count == 1 && named, label.c_str());
        }
        std::set<std::wstring> names;
        bool uniqueNames = true;
        bool ordered = true;
        int previous = static_cast<int>(TurnsLevel::Compact);
        for (const auto& box : kTranscriptViewBoxes)
        {
            uniqueNames = uniqueNames && names.insert(box.name).second;
            ordered = ordered && static_cast<int>(box.level) >= previous;
            previous = static_cast<int>(box.level);
            // The box's group is the FIRST level that turns its feature on.
            int first = -1;
            for (const auto& [levelName, level] : kViewLevels)
            {
                if (first < 0 && TurnsOptionsForLevel(level).*box.feature)
                {
                    first = static_cast<int>(level);
                }
            }
            const std::string label = "TranscriptViews: the box " + NarrowAsciiForCheck(box.name) + " sits in the group of the level that first shows it";
            CHECK(first == static_cast<int>(box.level), label.c_str());
        }
        CHECK(uniqueNames, "TranscriptViews: no two boxes share a name");
        CHECK(ordered, "TranscriptViews: the boxes run Compact, Default, Full, Max (the row reads like the level guide)");
        // The gates: the full call lines' box; what prints only under them; the reason line they replace.
        bool gates = true;
        for (const auto& box : kTranscriptViewBoxes)
        {
            const std::wstring name = box.name;
            const bool needsCalls = name == L"fullBash" || name == L"opOutput" || name == L"opDuration" || name == L"fullWrite";
            const auto want = needsCalls ? TranscriptBoxGate::NeedsCalls : name == L"reasons" ? TranscriptBoxGate::WithoutCalls : TranscriptBoxGate::Always;
            gates = gates && box.gate == want;
        }
        CHECK(gates && std::wstring{ kTranscriptCallsBoxName } == L"ops",
              "TranscriptViews: the call details wait for the full call lines (\"ops\"), and the reason line gives way to them");
    }

    // ---- the options each tab renders with ----
    {
        CHECK(sizeof(TurnsOptions) == kTurnsOptionsMembers, "TranscriptViews: SameTurnsOptions compares every TurnsOptions member (a new member: add it there)");
        const auto minimal = TurnsOptionsForLevel(TurnsLevel::Minimal);
        const auto max = TurnsOptionsForLevel(TurnsLevel::Max);
        CHECK(SameTurnsOptions(TranscriptViewOptions(custom, {}), minimal), "TranscriptViews: Custom with no box is exactly Minimal");
        std::vector<std::wstring> every;
        for (const auto& box : kTranscriptViewBoxes)
        {
            every.emplace_back(box.name);
        }
        CHECK(SameTurnsOptions(TranscriptViewOptions(custom, every), max), "TranscriptViews: Custom with every box is exactly Max");
        bool eachLevel = true;
        for (const auto& [name, level] : kViewLevels)
        {
            const auto options = TurnsOptionsForLevel(level);
            eachLevel = eachLevel && SameTurnsOptions(TranscriptViewOptions(custom, TicksFor(options)), options);
        }
        CHECK(eachLevel, "TranscriptViews: Custom with a level's own boxes is exactly that level's options");
        auto userWant = minimal;
        userWant.peerInputs = false;
        userWant.finalReplies = false;
        CHECK(SameTurnsOptions(TranscriptViewOptions(userOnly, {}), userWant), "TranscriptViews: User Only = Minimal without the replies and the teammates' messages");
        auto assistantWant = minimal;
        assistantWant.userInputs = false;
        assistantWant.peerInputs = false;
        CHECK(SameTurnsOptions(TranscriptViewOptions(assistantOnly, {}), assistantWant), "TranscriptViews: Assistant Only = Minimal without any message");
        auto assistantStats = assistantWant;
        assistantStats.stats = true;
        CHECK(SameTurnsOptions(TranscriptViewOptions(assistantOnly, { L"stats" }), assistantStats), "TranscriptViews: a box adds its feature to its tab");
        CHECK(SameTurnsOptions(TranscriptViewOptions(static_cast<int>(TurnsLevel::Default), { L"stats", L"fullWrite" }), TurnsOptionsForLevel(TurnsLevel::Default)),
              "TranscriptViews: a level's view ignores ticks (a level is its own options)");
        CHECK(SameTurnsOptions(TranscriptViewOptions(custom, { L"futureBox" }), minimal) && TranscriptViewOptions(custom, { L"OPS" }).ops,
              "TranscriptViews: an unknown stored name ticks nothing, and a known one ticks without case");
        auto diffWant = minimal;
        diffWant.agents = true;
        CHECK(SameTurnsOptions(TranscriptViewOptions(static_cast<int>(TranscriptView::Diff), { L"ops", L"stats" }), diffWant),
              "TranscriptViews: the Diff tab is Minimal's turns with the agents' edits, whatever a box tab ticked (its ops off: nothing folds)");
    }

    // ---- the ticks a click leaves, and which boxes are greyed ----
    {
        auto ticks = TranscriptViewWithBox({}, L"ops", true);
        CHECK(ticks == std::vector<std::wstring>{ L"ops" }, "TranscriptViews: ticking a box adds its name");
        ticks = TranscriptViewWithBox(ticks, L"ops", true);
        CHECK(ticks == std::vector<std::wstring>{ L"ops" }, "TranscriptViews: ticking a ticked box adds nothing");
        CHECK(TranscriptViewWithBox({ L"OPS", L"futureBox", L"ops" }, L"ops", false) == std::vector<std::wstring>{ L"futureBox" },
              "TranscriptViews: clearing a box removes every copy of its name, in any case, and keeps the rest");
        CHECK(TranscriptViewWithBox({ L"futureBox", L"stats" }, L"ops", true) == std::vector<std::wstring>({ L"futureBox", L"stats", L"ops" }),
              "TranscriptViews: a tick keeps a newer build's name and the order, and goes last");
        CHECK(TranscriptViewTicks({ L"Narration" }, L"narration") && !TranscriptViewTicks({ L"narrations" }, L"narration"),
              "TranscriptViews: ticks compare whole names without case");
        const auto box = [](const wchar_t* name) -> const TranscriptViewBox& {
            for (const auto& entry : kTranscriptViewBoxes)
            {
                if (std::wstring{ entry.name } == name)
                {
                    return entry;
                }
            }
            return kTranscriptViewBoxes[0];
        };
        CHECK(!TranscriptViewBoxTakesEffect(box(L"fullBash"), {}) && TranscriptViewBoxTakesEffect(box(L"fullBash"), { L"ops" }),
              "TranscriptViews: the exact Bash commands take effect only with the full call lines");
        CHECK(TranscriptViewBoxTakesEffect(box(L"reasons"), {}) && !TranscriptViewBoxTakesEffect(box(L"reasons"), { L"ops" }),
              "TranscriptViews: the reason lines take effect only without the full call lines");
        CHECK(TranscriptViewBoxTakesEffect(box(L"stats"), {}) && TranscriptViewBoxTakesEffect(box(L"stats"), { L"ops" }),
              "TranscriptViews: a box with no gate always takes effect");
    }

    // ---- the tips ----
    {
        bool plainDashes = true;
        const auto noEmDash = [&plainDashes](const std::wstring& text) {
            plainDashes = plainDashes && text.find(L'\x2014') == std::wstring::npos;
        };
        for (const auto& box : kTranscriptViewBoxes)
        {
            const std::wstring tip = TranscriptViewBoxTip(box);
            noEmDash(tip);
            noEmDash(box.label);
            const std::string label = "TranscriptViews: the box " + NarrowAsciiForCheck(box.name) + "'s tip says what it shows and where it comes from";
            CHECK(tip.find(std::wstring{ L"\nFrom " } + TranscriptLevelLabel(box.level) + L".") != std::wstring::npos && tip.size() > 40, label.c_str());
        }
        CHECK(TranscriptViewBoxTip(kTranscriptViewBoxes[5]).rfind(L"Every tool call's main arguments", 0) == 0 &&
                  TranscriptViewBoxTip(kTranscriptViewBoxes[5]).find(L"\nWhen each background task finished") != std::wstring::npos,
              "TranscriptViews: the full call lines' tip is the guide's own rows for the feature, both of them");
        const std::wstring user = TranscriptViewTip(userOnly, 7);
        const std::wstring all = TranscriptViewTip(custom, 6);
        const std::wstring claude = TranscriptViewTip(assistantOnly, 8);
        noEmDash(user);
        noEmDash(all);
        noEmDash(claude);
        noEmDash(kTranscriptViewsGuideNote);
        CHECK(user.rfind(L"User Only (key 7)\n", 0) == 0 && all.rfind(L"Custom (key 6)\n", 0) == 0 && claude.rfind(L"Assistant Only (key 8)\n", 0) == 0,
              "TranscriptViews: a box tab's tip opens with its label and its key");
        CHECK(all.find(L"Every box ticked shows exactly what Max shows.") != std::wstring::npos &&
                  user.find(L"a turn with nothing of yours is skipped") != std::wstring::npos && claude.find(L"Leaves out every message") != std::wstring::npos,
              "TranscriptViews: each box tab's tip says which side it shows");
        CHECK(TranscriptViewTip(static_cast<int>(TurnsLevel::Default), 1) == TranscriptLevelTip(TurnsLevel::Default, 1),
              "TranscriptViews: a level's tab keeps the level's own cheat sheet");
        const std::wstring diffTip = TranscriptViewTip(static_cast<int>(TranscriptView::Diff), 9);
        noEmDash(diffTip);
        noEmDash(kTranscriptDiffGuideNote);
        CHECK(diffTip.rfind(L"Diff (key 9)\n", 0) == 0 && diffTip.find(L"MultiEdit") != std::wstring::npos &&
                  std::wstring{ kTranscriptDiffGuideNote }.find(L"as it is now") != std::wstring::npos,
              "TranscriptViews: the Diff tab's tip opens with its label and its key and names the edits it shows; the guide says where Expand reads");
        CHECK(plainDashes, "TranscriptViews: every text uses the plain hyphen, never an em dash");
    }

    // ---- end to end: Custom with a level's own boxes renders EXACTLY that level's golden ----
    const fs::path root = TurnsHarnessDir() / L"turns";
    if (!fs::is_directory(root / L"expected"))
    {
        CHECK(false, "TranscriptViews: tests\\turns\\expected is missing (the fixtures ship beside m5_tests.exe)");
        return;
    }
    int matched = 0;
    int total = 0;
    for (const wchar_t* fixture : kViewFixtures)
    {
        for (const auto& [levelName, level] : kViewLevels)
        {
            ++total;
            const std::wstring golden = WidenTurnsUtf8(ReadTurnsFileBytes(root / L"expected" / TurnsGoldenName(fixture, levelName)));
            const std::wstring got = RenderTurnsFixture(root, fixture, TranscriptViewOptions(custom, TicksFor(TurnsOptionsForLevel(level))));
            const bool ok = !golden.empty() && got == golden;
            const std::string label = "TranscriptViews: Custom with the boxes of " + NarrowAsciiForCheck(TurnsGoldenName(fixture, levelName)) + " renders that golden";
            CHECK(ok, label.c_str());
            if (ok)
            {
                ++matched;
            }
            else if (!golden.empty())
            {
                PrintFirstTurnsDifference(got, golden);
            }
        }
    }
    std::wprintf(L"  %d of %d goldens rendered by Custom with their level's boxes\n", matched, total);
}

namespace
{
    // ---- the Diff tab (TRANSCRIPT_OVERLAY.md 3l): a synthetic transcript ----

    // Three turns in the folder C:/w (so its files are named relative to it). Turn 1: two Bash calls with the same reason and
    // one with only a command, two Edits of app.py (the second changes a line the first added: the net is one hunk), a Write
    // that creates notes.txt. Turn 2: an Edit that adds a line to other.py and one that removes it again (no net change).
    // Turn 3: two Edits of lib.py that do not fit together (the file changed between them, as a shell command would change
    // it: shown one by one), and an Edit of a file whose name holds a backtick (its fence must be tildes). The raw strings
    // use the delimiter J (a close parenthesis right before a quote would end a raw string without one).
    // Returns the transcript's bytes (28 lines).
    std::string DiffTabTranscript()
    {
        return R"J({"type":"user","uuid":"t01","parentUuid":null,"isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:00.000Z","message":{"role":"user","content":"Fix the greeting"}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t02","parentUuid":"t01","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:01.000Z","message":{"id":"m1","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"b1","name":"Bash","input":{"command":"pytest -q","description":"Run the tests"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t03","parentUuid":"t02","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:02.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"b1","content":"2 passed"}]},"toolUseResult":{"stdout":"2 passed","stderr":"","interrupted":false}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t04","parentUuid":"t03","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:03.000Z","message":{"id":"m2","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e1","name":"Edit","input":{"file_path":"C:/w/app.py","old_string":"print('hi')","new_string":"print('hello')\nprint('bye')"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t05","parentUuid":"t04","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:04.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e1","content":"The file C:/w/app.py has been updated."}]},"toolUseResult":{"filePath":"C:/w/app.py","structuredPatch":[{"oldStart":3,"oldLines":2,"newStart":3,"newLines":3,"lines":[" import sys","-print('hi')","+print('hello')","+print('bye')"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t06","parentUuid":"t05","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:05.000Z","message":{"id":"m3","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e2","name":"Edit","input":{"file_path":"C:/w/app.py","old_string":"print('bye')","new_string":"print('farewell')"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t07","parentUuid":"t06","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:06.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e2","content":"The file C:/w/app.py has been updated."}]},"toolUseResult":{"filePath":"C:/w/app.py","structuredPatch":[{"oldStart":4,"oldLines":2,"newStart":4,"newLines":2,"lines":[" print('hello')","-print('bye')","+print('farewell')"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t08","parentUuid":"t07","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:07.000Z","message":{"id":"m4","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"w1","name":"Write","input":{"file_path":"C:/w/notes.txt","content":"one\ntwo\n"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t09","parentUuid":"t08","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:08.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"w1","content":"File created successfully at: C:/w/notes.txt"}]},"toolUseResult":{"type":"create","filePath":"C:/w/notes.txt","content":"one\ntwo\n","structuredPatch":[]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t10","parentUuid":"t09","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:09.000Z","message":{"id":"m5","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"b2","name":"Bash","input":{"command":"pytest -q","description":"Run the tests"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t11","parentUuid":"t10","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:10.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"b2","content":"2 passed"}]},"toolUseResult":{"stdout":"2 passed","stderr":"","interrupted":false}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t12","parentUuid":"t11","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:11.000Z","message":{"id":"m6","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"b3","name":"Bash","input":{"command":"git status"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t13","parentUuid":"t12","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:12.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"b3","content":"clean"}]},"toolUseResult":{"stdout":"clean","stderr":"","interrupted":false}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t14","parentUuid":"t13","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:00:13.000Z","message":{"id":"m7","role":"assistant","model":"claude-test-1","stop_reason":"end_turn","content":[{"type":"text","text":"Greeting fixed."}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t15","parentUuid":"t14","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:01:00.000Z","message":{"role":"user","content":"Try a temporary line"}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t16","parentUuid":"t15","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:01:01.000Z","message":{"id":"m8","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e3","name":"Edit","input":{"file_path":"C:/w/other.py","old_string":"a\nb","new_string":"a\ntmp\nb"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t17","parentUuid":"t16","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:01:02.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e3","content":"The file C:/w/other.py has been updated."}]},"toolUseResult":{"filePath":"C:/w/other.py","structuredPatch":[{"oldStart":1,"oldLines":2,"newStart":1,"newLines":3,"lines":[" a","+tmp"," b"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t18","parentUuid":"t17","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:01:03.000Z","message":{"id":"m9","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e4","name":"Edit","input":{"file_path":"C:/w/other.py","old_string":"a\ntmp\nb","new_string":"a\nb"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t19","parentUuid":"t18","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:01:04.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e4","content":"The file C:/w/other.py has been updated."}]},"toolUseResult":{"filePath":"C:/w/other.py","structuredPatch":[{"oldStart":1,"oldLines":3,"newStart":1,"newLines":2,"lines":[" a","-tmp"," b"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t20","parentUuid":"t19","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:01:05.000Z","message":{"id":"m10","role":"assistant","model":"claude-test-1","stop_reason":"end_turn","content":[{"type":"text","text":"Reverted."}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t21","parentUuid":"t20","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:00.000Z","message":{"role":"user","content":"Edit after a shell change"}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t22","parentUuid":"t21","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:01.000Z","message":{"id":"m11","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e5","name":"Edit","input":{"file_path":"C:/w/lib.py","old_string":"x = 1","new_string":"x = 2"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t23","parentUuid":"t22","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:02.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e5","content":"The file C:/w/lib.py has been updated."}]},"toolUseResult":{"filePath":"C:/w/lib.py","structuredPatch":[{"oldStart":1,"oldLines":1,"newStart":1,"newLines":1,"lines":["-x = 1","+x = 2"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t24","parentUuid":"t23","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:03.000Z","message":{"id":"m12","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e6","name":"Edit","input":{"file_path":"C:/w/lib.py","old_string":"x = 5","new_string":"x = 6"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t25","parentUuid":"t24","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:04.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e6","content":"The file C:/w/lib.py has been updated."}]},"toolUseResult":{"filePath":"C:/w/lib.py","structuredPatch":[{"oldStart":1,"oldLines":1,"newStart":1,"newLines":1,"lines":["-x = 5","+x = 6"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t26","parentUuid":"t25","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:05.000Z","message":{"id":"m13","role":"assistant","model":"claude-test-1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"e7","name":"Edit","input":{"file_path":"C:/w/we`ird.txt","old_string":"a","new_string":"b"}}]}})J"
               "\n"
               R"J({"type":"user","uuid":"t27","parentUuid":"t26","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:06.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"e7","content":"The file C:/w/we`ird.txt has been updated."}]},"toolUseResult":{"filePath":"C:/w/we`ird.txt","structuredPatch":[{"oldStart":1,"oldLines":1,"newStart":1,"newLines":1,"lines":["-a","+b"]}]}})J"
               "\n"
               R"J({"type":"assistant","uuid":"t28","parentUuid":"t27","isSidechain":false,"cwd":"C:/w","sessionId":"eeeeeeee-0000-4000-8000-0000000000e1","timestamp":"2026-01-03T10:02:07.000Z","message":{"id":"m14","role":"assistant","model":"claude-test-1","stop_reason":"end_turn","content":[{"type":"text","text":"Done."}]}})J"
               "\n";
    }

    // The line of `text` that starts with `prefix` ("" when none): a turn heading's whole line, wherever its times put its end.
    std::wstring DiffLineStartingWith(const std::wstring& text, std::wstring_view prefix)
    {
        for (const auto& line : SplitTurnsLines(text))
        {
            if (line.rfind(prefix, 0) == 0)
            {
                return line;
            }
        }
        return {};
    }
}

// Agentmaster (TRANSCRIPT_OVERLAY.md 3l - the transcript overlay's DIFF tab): NOT part of the port, so no golden covers it.
// Pins its pure half (TranscriptDiff.h: unified-diff text in and out, the numbered rows, the per-turn NET change composed from
// a turn's edits, the Bash reasons once each with their count, where a hunk sits in the file now, the cards' expand arithmetic,
// labels, display lines, fence info, counts, squares and titles) and its document (RenderTranscriptDiff) on a synthetic
// transcript: the Bash rows, a net diff of two edits, a created file, a turn that changed a file back, edits that do not fit
// together shown one by one, a file name a backtick fence could not carry, the turn headings' counts.
void TestTranscriptDiff()
{
    std::wprintf(L"\n[TranscriptDiff] the overlay's Diff tab: unified diffs, the net change per turn, the cards' arithmetic, the document\n");
    const auto has = [](const std::wstring& text, std::wstring_view part) { return text.find(part) != std::wstring::npos; };
    const auto expect = [](bool ok, const char* label, const std::wstring& text) {
        CHECK(ok, label);
        if (!ok)
        {
            for (const auto& line : SplitTurnsLines(text))
            {
                std::wprintf(L"      | %ls\n", EscapeForConsole(line).c_str());
            }
        }
    };
    const auto hunksOf = [](std::wstring_view text) { return ParseUnifiedDiff(text).hunks; };
    // A composer's net change as diff text (no file lines), "" when it has none.
    const auto netOf = [](const TurnFileComposer& composer) { return UnifiedDiffText(L"", L"", composer.NetHunks(3)); };

    // ---- reading and writing unified diff text ----
    {
        const auto parsed = ParseUnifiedDiff(L"--- C:/w/a.py\n+++ C:/w/a.py\n@@ -3,2 +3,3 @@ def f():\n import sys\n-print('hi')\n+print('hello')\n+print('bye')\n");
        CHECK(parsed.oldPath == L"C:/w/a.py" && parsed.newPath == L"C:/w/a.py" && !parsed.unnumbered && parsed.added == 2 && parsed.removed == 1,
              "Diff: a unified diff's paths and its +/- counts");
        CHECK(parsed.hunks.size() == 1 && parsed.hunks[0].oldStart == 3 && parsed.hunks[0].oldLines == 2 && parsed.hunks[0].newStart == 3 &&
                  parsed.hunks[0].newLines == 3 && parsed.hunks[0].section == L"def f():" && parsed.hunks[0].rows.size() == 4,
              "Diff: a hunk's four numbers, its section and its rows");
        const auto shortHeader = ParseUnifiedDiff(L"@@ -5 +7 @@\n-a\n+b\n");
        CHECK(shortHeader.hunks.size() == 1 && shortHeader.hunks[0].oldLines == 1 && shortHeader.hunks[0].newLines == 1 && shortHeader.hunks[0].newStart == 7,
              "Diff: an omitted count is 1 (unified diff's rule)");
        const auto crlf = ParseUnifiedDiff(L"@@ -1,2 +1,2 @@\r\n-a\r\n+b\r\n\r\n");
        CHECK(crlf.hunks.size() == 1 && crlf.hunks[0].rows == std::vector<std::wstring>({ L"-a", L"+b", L" " }),
              "Diff: CRLF line ends are read, and an empty line inside a hunk is an empty context row");
        const auto unnumbered = ParseUnifiedDiff(L"-a = 1\n+a = 2\n");
        CHECK(unnumbered.unnumbered && unnumbered.hunks.size() == 1 && unnumbered.hunks[0].oldStart == 0 && unnumbered.hunks[0].newStart == 0 &&
                  unnumbered.added == 1 && unnumbered.removed == 1,
              "Diff: change rows with no header are one unnumbered hunk (an older Claude Code's old_string / new_string)");
        DiffHunk header;
        CHECK(!Agentmaster::detail::ParseHunkHeader(L"@@ -a,1 +1 @@", header) && !Agentmaster::detail::ParseHunkHeader(L"@@ -1,1 +1,1", header) && !Agentmaster::detail::ParseHunkHeader(L"@ -1 +1 @@", header),
              "Diff: a line that only looks like a hunk header is no header");
        CHECK(FormatHunkHeader(parsed.hunks[0]) == L"@@ -3,2 +3,3 @@ def f():", "Diff: a hunk header is written back with its section");
        const std::wstring written = UnifiedDiffText(L"/dev/null", L"C:/w/b.py", parsed.hunks);
        CHECK(written.rfind(L"--- /dev/null\n+++ C:/w/b.py\n@@ -3,2 +3,3 @@ def f():\n import sys\n", 0) == 0 && written.back() == L'\n',
              "Diff: diff text is written with both paths, every line ended");
        const auto again = ParseUnifiedDiff(written);
        CHECK(again.hunks.size() == 1 && again.hunks[0].rows == parsed.hunks[0].rows && again.newPath == L"C:/w/b.py", "Diff: written diff text reads back as the same hunks");
    }

    // ---- a hunk's rows with their numbers, as a card draws them ----
    {
        const auto rows = NumberHunkRows(hunksOf(L"@@ -3,2 +3,3 @@\n import sys\n-print('hi')\n+print('hello')\n+print('bye')\n").front());
        CHECK(rows.size() == 4 && rows[0].kind == L' ' && rows[0].oldNo == 3 && rows[0].newNo == 3 && rows[0].text == L"import sys",
              "Diff: a context row carries both numbers, its text without its mark");
        CHECK(rows.size() == 4 && rows[1].oldNo == 4 && rows[1].newNo == 0 && rows[2].oldNo == 0 && rows[2].newNo == 4 && rows[3].newNo == 5,
              "Diff: a removed row carries its old number, an added one its new number");
        const auto created = NumberHunkRows(hunksOf(L"@@ -0,0 +1,2 @@\n+one\n+two\n").front());
        CHECK(created.size() == 2 && created[0].newNo == 1 && created[1].newNo == 2 && created[0].oldNo == 0, "Diff: a created file's lines are numbered from 1");
        const auto plain = NumberHunkRows(ParseUnifiedDiff(L"-a\n+b\n").hunks.front());
        CHECK(plain.size() == 2 && plain[0].oldNo == 0 && plain[1].newNo == 0, "Diff: an unnumbered hunk numbers nothing");
    }

    // ---- the turn's NET change to a file, composed from its edits (the user's pick "Net diff per file") ----
    {
        const std::wstring edit1 = L"@@ -3,2 +3,3 @@\n import sys\n-print('hi')\n+print('hello')\n+print('bye')\n";
        TurnFileComposer single(nullptr, false);
        CHECK(single.Apply(hunksOf(edit1)) && netOf(single) == edit1, "Diff: one edit's net change is that edit");

        TurnFileComposer apart(nullptr, false);
        const bool apartOk = apart.Apply(hunksOf(edit1)) && apart.Apply(hunksOf(L"@@ -40,3 +40,3 @@\n a\n-b\n+B\n c\n"));
        CHECK(apartOk && netOf(apart) == edit1 + L"@@ -39,3 +40,3 @@\n a\n-b\n+B\n c\n",
              "Diff: two edits far apart are two hunks - the later one's old side counted in the file before the turn");

        TurnFileComposer onTop(nullptr, false);
        const bool onTopOk = onTop.Apply(hunksOf(L"@@ -1,3 +1,4 @@\n a\n b\n+x\n c\n")) && onTop.Apply(hunksOf(L"@@ -2,3 +2,3 @@\n b\n-x\n+y\n c\n"));
        CHECK(onTopOk && netOf(onTop) == L"@@ -1,3 +1,4 @@\n a\n b\n+y\n c\n", "Diff: an edit of a line an earlier edit added shows only the line as it ends up");

        TurnFileComposer undone(nullptr, false);
        const bool undoneOk = undone.Apply(hunksOf(L"@@ -1,3 +1,4 @@\n a\n b\n+x\n c\n")) && undone.Apply(hunksOf(L"@@ -2,3 +2,2 @@\n b\n-x\n c\n"));
        CHECK(undoneOk && !undone.IsVoid() && undone.NetHunks(3).empty(), "Diff: a line added and removed again in one turn is no change at all");

        TurnFileComposer created(nullptr, true);
        const bool createdOk = created.Apply(hunksOf(L"@@ -0,0 +1,2 @@\n+one\n+two\n")) && created.Apply(hunksOf(L"@@ -1,2 +1,2 @@\n-one\n+uno\n two\n"));
        CHECK(createdOk && netOf(created) == L"@@ -0,0 +1,2 @@\n+uno\n+two\n", "Diff: a file the turn created is all added lines, as they end up");

        TurnFileComposer misfit(nullptr, false);
        const bool firstFits = misfit.Apply(hunksOf(L"@@ -1,2 +1,2 @@\n a\n-b\n+B\n"));
        const bool secondFits = misfit.Apply(hunksOf(L"@@ -1,2 +1,2 @@\n zzz\n-B\n+C\n"));
        CHECK(firstFits && !secondFits && misfit.IsVoid() && misfit.NetHunks(3).empty() && !misfit.Apply(hunksOf(edit1)),
              "Diff: an edit whose lines differ from what an earlier edit showed (the file changed between them) voids the composition");

        TurnFileComposer noNumbers(nullptr, false);
        CHECK(!noNumbers.Apply(ParseUnifiedDiff(L"-a = 1\n+a = 2\n").hunks) && noNumbers.IsVoid(), "Diff: an edit without line numbers cannot be placed: void");

        std::wstring original;
        for (int line = 1; line <= 10; ++line)
        {
            original += L"l" + std::to_wstring(line) + L"\n";
        }
        TurnFileComposer known(&original, false);
        CHECK(known.Apply(hunksOf(L"@@ -5,1 +5,1 @@\n-l5\n+L5\n")) && netOf(known) == L"@@ -2,7 +2,7 @@\n l2\n l3\n l4\n-l5\n+L5\n l6\n l7\n l8\n",
              "Diff: with the original file known, the net change gets its full 3 lines of context even when an edit carried none");

        TurnFileComposer twoHunks(nullptr, false);
        CHECK(twoHunks.Apply(hunksOf(L"@@ -2,1 +2,2 @@\n a\n+x\n@@ -10,1 +11,1 @@\n-b\n+c\n")) && netOf(twoHunks) == L"@@ -2,1 +2,2 @@\n a\n+x\n@@ -10,1 +11,1 @@\n-b\n+c\n",
              "Diff: an edit's later hunk is placed by the line-count change of its earlier ones");

        TurnFileComposer classic(nullptr, false);
        TurnFileComposer jsdiff(nullptr, false);
        const bool insertsOk = classic.Apply(hunksOf(L"@@ -5,0 +6,2 @@\n+x\n+y\n")) && jsdiff.Apply(hunksOf(L"@@ -6,0 +6,2 @@\n+x\n+y\n"));
        CHECK(insertsOk && netOf(classic) == L"@@ -5,0 +6,2 @@\n+x\n+y\n" && netOf(jsdiff) == netOf(classic),
              "Diff: an insertion with no context lands in the same place whether its empty old side is numbered the unified or the jsdiff way");
        const std::wstring empty;
        TurnFileComposer emptyFile(&empty, false);
        CHECK(emptyFile.Apply(hunksOf(L"@@ -1,0 +1,2 @@\n+x\n+y\n")) && netOf(emptyFile) == L"@@ -0,0 +1,2 @@\n+x\n+y\n",
              "Diff: lines written into an empty file are numbered from 1");
    }

    // ---- the Bash reasons once each, and where a hunk sits in the file now ----
    {
        const auto rows = DedupeInOrder({ L"Run the tests", L"Build", L"Run the tests", L"Run the tests", L"`git status`" });
        // Named first: CHECK is a macro, and the comma inside a template's arguments would split its arguments.
        const std::vector<std::pair<std::wstring, int>> wantRows{ { L"Run the tests", 3 }, { L"Build", 1 }, { L"`git status`", 1 } };
        CHECK(rows == wantRows, "Diff: each different reason once, in first-seen order, with how many times it ran");
        const std::vector<std::wstring> file{ L"a", L"b", L"c", L"x", L"y", L"a", L"b", L"c" };
        const auto hunkAt = [&hunksOf](int newStart) {
            auto hunk = hunksOf(L"@@ -1,3 +1,3 @@\n a\n b\n+c\n").front();
            hunk.newStart = newStart;
            return hunk;
        };
        CHECK(LocateHunkNewSide(file, hunkAt(6)) == 5 && LocateHunkNewSide(file, hunkAt(1)) == 0,
              "Diff: of two places that hold a hunk's lines, the one nearest its own number wins");
        CHECK(LocateHunkNewSide(file, hunksOf(L"@@ -1,2 +1,2 @@\n q\n+r\n").front()) == -1, "Diff: a hunk the file no longer holds is not found");
        CHECK(LocateHunkNewSide({ L"a\r", L"b" }, hunksOf(L"@@ -1,2 +1,2 @@\n a\n b\n").front()) == 0, "Diff: a CRLF file's lines match");
        CHECK(LocateHunkNewSide(file, hunksOf(L"@@ -1,1 +0,0 @@\n-a\n").front()) == -1, "Diff: a hunk with no new side has no place in the file");
    }

    // ---- the cards' expand arithmetic: what a row can still show and how many lines one click shows ----
    {
        const auto hunks = hunksOf(L"@@ -3,2 +3,3 @@\n import sys\n-print('hi')\n+print('hello')\n+print('bye')\n@@ -20,3 +21,3 @@\n a\n-b\n+B\n c\n");
        CHECK(hunks.size() == 2 && DiffNewFirst(hunks[0]) == 3 && DiffOldFirst(hunks[1]) == 20 && DiffSideCount(hunks[0], L'+') == 3 && DiffSideCount(hunks[0], L'-') == 2,
              "Diff: a hunk's first numbers and each side's row count");
        const auto emptySides = hunksOf(L"@@ -0,0 +1,2 @@\n+one\n+two\n@@ -4,2 +3,0 @@\n-p\n-q\n");
        CHECK(DiffOldFirst(emptySides[0]) == 1 && DiffNewFirst(emptySides[1]) == 4, "Diff: an empty side's first line is one past the line its change follows");
        CHECK(DiffLinesAboveHunk(hunks, 0) == 2 && DiffLinesAboveHunk(hunks, 1) == 15 && DiffLinesAboveHunk(hunks, 2) == 0,
              "Diff: the lines above a hunk, by its own numbers - from the file's start, or from the end of the hunk before it");
        CHECK(DiffRemainingAbove(hunks, { 10, 30 }, { 0, 0 }, 0) == 2 && DiffRemainingAbove(hunks, { 10, 30 }, { 0, 0 }, 1) == 15,
              "Diff: a row shows no more than the hunks' numbers say lay there, however much the file holds now");
        CHECK(DiffRemainingAbove(hunks, { 10, 20 }, { 0, 0 }, 1) == 7, "Diff: ...and no more than the file now holds there");
        CHECK(DiffRemainingAbove(hunks, { 10, 30 }, { 2, 5 }, 0) == 0 && DiffRemainingAbove(hunks, { 10, 30 }, { 2, 5 }, 1) == 10,
              "Diff: what was revealed already is taken off");
        CHECK(DiffRemainingAbove(hunks, { -1, 30 }, { 0, 0 }, 1) == 15 && DiffRemainingAbove(hunks, { 10, -1 }, { 0, 0 }, 1) == 0 &&
                  DiffRemainingAbove(hunks, { 10 }, { 0, 0 }, 1) == 0,
              "Diff: a hunk the file no longer holds has nothing to show (its neighbour then counts from the file's start)");
        const auto& last = hunks[1];
        CHECK(DiffRemainingBelow(100, 30, last, 0) == 67 && DiffRemainingBelow(100, 30, last, 60) == 7 && DiffRemainingBelow(100, 30, last, 70) == 0 &&
                  DiffRemainingBelow(100, -1, last, 0) == 0 && DiffRemainingBelow(20, 30, last, 0) == 0,
              "Diff: the lines below the last hunk: the rest of the file now, never below 0");
        CHECK(DiffExpandTake(5, false) == 5 && DiffExpandTake(kDiffExpandStep, false) == kDiffExpandStep && DiffExpandTake(kDiffExpandStep + 1, false) == kDiffExpandStep &&
                  DiffExpandTake(35, true) == 35 && DiffExpandTake(kDiffGapExpandAll, true) == kDiffGapExpandAll && DiffExpandTake(kDiffGapExpandAll + 1, true) == kDiffExpandStep &&
                  DiffExpandTake(0, true) == 0,
              "Diff: one click shows a step, a short gap between hunks whole, or what is left");
    }

    // ---- what the cards say and draw ----
    {
        CHECK(DiffExpandLabel(true, false, -1) == L"Show more lines below (from the file now)" && DiffExpandLabel(true, false, 4) == L"Show the last 4 lines (from the file now)" &&
                  DiffExpandLabel(true, false, 1) == L"Show the last line (from the file now)" && DiffExpandLabel(true, false, 50) == L"Show 20 more lines below (from the file now)",
              "Diff: the row below the last hunk says what a click shows");
        CHECK(DiffExpandLabel(false, true, 2) == L"Show the first 2 lines (from the file now)" && DiffExpandLabel(false, true, 30) == L"Show 20 more lines above (from the file now)" &&
                  DiffExpandLabel(false, false, 12) == L"Show the 12 lines hidden here (from the file now)" &&
                  DiffExpandLabel(false, false, 99) == L"Show 20 more lines (99 hidden) (from the file now)" && DiffExpandLabel(false, false, 1) == L"Show the line hidden here (from the file now)",
              "Diff: the rows above a hunk say what a click shows");
        CHECK(DiffDisplayLine(L"a\tb\r\r") == L"a\tb" && DiffDisplayLine(L"x\ny") == L"x\x2424y" && DiffDisplayLine(L"x\ry") == L"x\x240Dy" &&
                  DiffDisplayLine(L"p\x2028q\vr") == L"p\x2424q\x240Br",
              "Diff: a drawn line keeps its tabs, drops its trailing CRs, and shows every break inside it as a symbol");
        const std::wstring longLine(kDiffMaxLineChars + 500, L'a');
        const std::wstring cutLine = DiffDisplayLine(longLine);
        std::wstring pairAtCut(kDiffMaxLineChars - 1, L'a');
        pairAtCut += L"\xD83D\xDE00tail";
        const std::wstring cutPair = DiffDisplayLine(pairAtCut);
        CHECK(cutLine.size() == kDiffMaxLineChars + 1 && cutLine.back() == L'\x2026' && cutPair.size() == kDiffMaxLineChars && cutPair[kDiffMaxLineChars - 2] == L'a' &&
                  cutPair.back() == L'\x2026',
              "Diff: a very long line is cut with an ellipsis, never in the middle of a surrogate pair");
        // A line's numbers cell (the cards wrap their code, so each line is a grid row: this cell beside the code).
        CHECK(DiffNumberDigits(0) == 3 && DiffNumberDigits(999) == 3 && DiffNumberDigits(1000) == 4 && DiffNumberDigits(123456) == 6 &&
                  DiffNumberDigits(-5) == 3,
              "Diff: a card's numbers are padded to the digits of its biggest number, at least three");
        DiffRow context;
        context.kind = L' ';
        context.oldNo = 39;
        context.newNo = 39;
        DiffRow removedRow;
        removedRow.kind = L'-';
        removedRow.oldNo = 40;
        DiffRow addedRow;
        addedRow.kind = L'+';
        addedRow.newNo = 40;
        DiffRow marker;
        marker.kind = L'\\';
        // The blanks spelled out piece by piece (adjacent literals join): two between the numbers, one before the mark.
        CHECK(DiffNumbersText(context, 3) == L" 39" L"  " L" 39" L" " L" " && DiffNumbersText(removedRow, 3) == L" 40" L"  " L"   " L" " L"-" &&
                  DiffNumbersText(addedRow, 3) == L"   " L"  " L" 40" L" " L"+" && DiffNumbersText(marker, 3) == std::wstring(10, L' '),
              "Diff: a line's numbers cell - both numbers right-aligned, then its mark; blanks where a side has none");
        CHECK(DiffNumbersText(context, 4).size() == 12 && DiffNumbersText(removedRow, 4) == L"  40" L"  " L"    " L" " L"-",
              "Diff: the cell is 2 * digits + 4 characters, so every line of a card lines up");
        std::wstring path;
        std::wstring meta;
        SplitDiffInfo(L"src/app.py \x00B7 2 edits \x00B7 by finder-a", path, meta);
        CHECK(path == L"src/app.py" && meta == L"2 edits \x00B7 by finder-a", "Diff: the fence info is the file, then what the block is");
        SplitDiffInfo(L"notes.txt", path, meta);
        CHECK(path == L"notes.txt" && meta.empty(), "Diff: a fence info with no description is the file alone");
        std::wstring reason = L"Run the tests \x00D7" L"5";
        std::wstring words = L"Run \x00D7 tests";
        std::wstring huge = L"x \x00D7" L"1234567";
        CHECK(TakeDiffRepeatCount(reason) == 5 && reason == L"Run the tests" && TakeDiffRepeatCount(words) == 1 && words == L"Run \x00D7 tests" &&
                  TakeDiffRepeatCount(huge) == 1 && huge == L"x \x00D7" L"1234567",
              "Diff: a Bash row's count comes off its end; text that only looks like one stays");
        // A named pair type: CHECK is a macro, and "std::pair<int, int>" written inside it would split its arguments.
        using Squares = std::pair<int, int>;
        CHECK(DiffBarSquares(0, 0) == Squares(0, 0) && DiffBarSquares(3, 1) == Squares(3, 1) && DiffBarSquares(100, 0) == Squares(5, 0) &&
                  DiffBarSquares(0, 100) == Squares(0, 5) && DiffBarSquares(99, 1) == Squares(4, 1) && DiffBarSquares(1, 99) == Squares(1, 4) &&
                  DiffBarSquares(50, 50) == Squares(3, 2),
              "Diff: the five squares - one per line up to five, else in proportion, each side with lines keeps one");
        const std::wstring title = DiffOutlineTitle(L"  Fix   the\tgreeting \nand then the farewell");
        const std::wstring longTitle = DiffOutlineTitle(std::wstring(300, L'w'));
        CHECK(title == L"Fix the greeting" && longTitle.size() == 121 && longTitle.back() == L'\x2026' && DiffOutlineTitle(L"   ").empty(),
              "Diff: an outline title is the message's first line, its blanks collapsed, cut at 120 characters");
    }

    // ---- the document: a synthetic session, every kind of change ----
    const auto lines = ParseTranscriptJsonl(DiffTabTranscript());
    CHECK(lines.size() == 28, "Diff: the synthetic transcript parses to its 28 lines");
    const std::wstring doc = RenderTranscriptDiff(lines, /*utc*/ true);
    expect(!doc.empty() && CountTurnsOccurrences(doc, L"### Turn ") == 3, "Diff: the document has the session's three turns", doc);
    const std::wstring turn1 = DiffLineStartingWith(doc, L"### Turn 1");
    const std::wstring turn2 = DiffLineStartingWith(doc, L"### Turn 2");
    const std::wstring turn3 = DiffLineStartingWith(doc, L"### Turn 3");
    expect(turn1.ends_with(L" \x00B7 2 files \x00B7 +4 -1") && turn3.ends_with(L" \x00B7 2 files \x00B7 +3 -3") && !turn2.empty() && !has(turn2, L" file"),
           "Diff: a turn heading ends with the files it changed and its net +/- (none for a turn that changed nothing)", doc);
    expect(has(doc, L"- `$` Run the tests \x00D7" L"2\n- `$` `git status`\n"),
           "Diff: the Bash calls are one row per different reason, the repeated one with its count, a call without a reason as its command", doc);
    const std::wstring app = L"```diff app.py \x00B7 2 edits\n--- C:/w/app.py\n+++ C:/w/app.py\n@@ -3,2 +3,3 @@\n import sys\n-print('hi')\n+print('hello')\n+print('farewell')\n```\n";
    const std::wstring notes = L"```diff notes.txt \x00B7 new file\n--- /dev/null\n+++ C:/w/notes.txt\n@@ -0,0 +1,2 @@\n+one\n+two\n```\n";
    expect(has(doc, app), "Diff: two edits of one file are its net change, the file named inside the session's folder", doc);
    expect(has(doc, notes), "Diff: a file the turn created is a new file from /dev/null", doc);
    const size_t asked = doc.find(L"Fix the greeting");
    const size_t ran = doc.find(L"- `$` Run the tests");
    const size_t changed = doc.find(app);
    const size_t created = doc.find(notes);
    const size_t replied = doc.find(L"Greeting fixed.");
    expect(asked < ran && ran < changed && changed < created && created < replied && replied != std::wstring::npos,
           "Diff: a turn reads its message, its Bash rows, its files in the order they were first changed, then the reply", doc);
    expect(!has(doc, L"**Bash**") && !has(doc, L"**Edit**") && !has(doc, L"**Write**"), "Diff: no call lines - Minimal's view with the changes", doc);
    expect(has(doc, L"_other.py: changed and changed back in this turn - no net change._"), "Diff: a file changed back says so instead of an empty card", doc);
    expect(has(doc, L"```diff lib.py \x00B7 edit 1 of 2\n--- C:/w/lib.py\n+++ C:/w/lib.py\n") && has(doc, L"-x = 1\n+x = 2\n") &&
               has(doc, L"```diff lib.py \x00B7 edit 2 of 2\n") && has(doc, L"-x = 5\n+x = 6\n"),
           "Diff: edits that do not fit together are shown one by one, as Claude Code recorded them", doc);
    expect(has(doc, L"~~~diff we`ird.txt\n--- C:/w/we`ird.txt\n+++ C:/w/we`ird.txt\n@@ -1,1 +1,1 @@\n-a\n+b\n~~~\n"),
           "Diff: a file name with a backtick gets a tilde fence (a backtick fence's info may hold none)", doc);
    expect(CountTurnsOccurrences(doc, L"```diff ") == 4 && CountTurnsOccurrences(doc, L"~~~diff ") == 1, "Diff: one block per file, one per edit where they do not fit", doc);
    const auto appDiff = ParseUnifiedDiff(L"--- C:/w/app.py\n+++ C:/w/app.py\n@@ -3,2 +3,3 @@\n import sys\n-print('hi')\n+print('hello')\n+print('farewell')\n");
    CHECK(appDiff.newPath == L"C:/w/app.py" && appDiff.added == 2 && appDiff.removed == 1, "Diff: a card reads its block back: the file Expand opens and its counts");

    // ---- the editDiffs transcript: refused and unanswered edits change nothing; unnumbered ones show as recorded ----
    const std::wstring older = RenderTranscriptDiff(ParseTranscriptJsonl(EditDiffsTranscript()), /*utc*/ true);
    expect(!has(older, L"locked.py") && has(older, L"```diff C:/w/old.py\n--- C:/w/old.py\n+++ C:/w/old.py\n-a = 1\n+a = 2\n```\n"),
           "Diff: a refused Edit shows no card; an Edit without hunks shows its own strings (no folder known: the whole path)", older);
    expect(has(older, L"```diff C:/w/nb.ipynb") && has(older, L"```diff C:/w/multi.py"), "Diff: a NotebookEdit and a MultiEdit are cards too", older);
    const std::wstring olderTurn2 = DiffLineStartingWith(older, L"### Turn 2");
    expect(!olderTurn2.empty() && !has(olderTurn2, L" file"), "Diff: an Edit that never got a result changed nothing", older);
}
