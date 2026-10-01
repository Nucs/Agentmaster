// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: TRANSCRIPT TURNS — the session copy menus' "Copy > Transcript >
// Default / Max / Full / Compact / Minimal" (AgentCopyActions.h codes 9-13). TranscriptTurns.{h,cpp} is a C++
// port of the am-agentmaster-cli skill's transcript_turns.py (v9), so its contract is the reference's own
// output: every fixture in turns\ (copies of the skill's synthetic test transcripts — a session with a rewind
// and a background job, a file that opens with a compaction, junk tool arguments, every way an AskUserQuestion
// ends, a compaction a parentUuid-only walk can't cross, a fork + plan-restart family, a team lead with its
// subagents) is rendered at each of the five levels and compared BYTE FOR BYTE with turns\expected\, which
// turns\make_goldens.py writes by running the reference script itself. A mismatch prints the first differing
// line of each side. Plus focused checks of the level ladder, the timestamp reader and the JSONL reader.
// Shared CHECK/fixtures/decls live in m5_tests.h.
#include "m5_tests.h"

#include "../TranscriptTurns.h"

#include <cwchar> // std::swprintf
#include <fstream>
#include <iterator> // std::size
#include <map>
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
    std::wprintf(L"\n[TranscriptTurns] Copy > Transcript levels vs the transcript_turns.py v9 goldens\n");

    // ---- the level ladder: what each menu item turns on (the reference's LEVELS table) ----
    {
        const TurnsOptions latest{}; // the reference's latest preset at its default level
        const TurnsOptions def = TurnsOptionsForLevel(TurnsLevel::Default);
        CHECK(def.notes == latest.notes && def.times == latest.times && def.ops == latest.ops && def.shortPaths == latest.shortPaths &&
                  def.fullBash == latest.fullBash && def.stats == latest.stats && def.opMeta == latest.opMeta && def.opDuration == latest.opDuration &&
                  def.opOutput == latest.opOutput && def.bgEvents == latest.bgEvents && def.narration == latest.narration &&
                  def.compactions == latest.compactions && def.answers == latest.answers && def.nestedBlocks == latest.nestedBlocks &&
                  def.compactedHistory == latest.compactedHistory && def.chain == latest.chain && def.agents == latest.agents &&
                  def.peerMessages == latest.peerMessages && def.fullWrite == latest.fullWrite,
              "TranscriptTurns: Default is exactly the latest preset's own defaults (the steps level)");
        CHECK(def.ops && def.narration && def.opMeta && !def.opDuration && !def.opOutput && !def.fullBash && !def.fullWrite && !def.stats,
              "TranscriptTurns: Default = calls on one line with their outcome, no durations / output / blocks / stats");
        const TurnsOptions minimal = TurnsOptionsForLevel(TurnsLevel::Minimal);
        CHECK(!minimal.notes && !minimal.ops && !minimal.narration && !minimal.compactions && !minimal.answers && !minimal.agents &&
                  !minimal.fullBash && !minimal.opOutput && !minimal.opDuration && !minimal.fullWrite && !minimal.stats,
              "TranscriptTurns: Minimal turns every ladder feature off");
        CHECK(minimal.times && minimal.chain && minimal.peerMessages && minimal.liveBranch && minimal.compactedHistory,
              "TranscriptTurns: Minimal keeps the non-ladder features and every rule");
        const TurnsOptions compact = TurnsOptionsForLevel(TurnsLevel::Compact);
        CHECK(compact.notes && compact.compactions && compact.answers && compact.agents && !compact.ops && !compact.narration,
              "TranscriptTurns: Compact = notes + compactions + answers + agents, no calls or remarks");
        const TurnsOptions full = TurnsOptionsForLevel(TurnsLevel::Full);
        CHECK(full.ops && full.fullBash && full.opOutput && full.opDuration && !full.fullWrite && !full.stats,
              "TranscriptTurns: Full = Default + Bash blocks, first output lines, per-call durations");
        const TurnsOptions max = TurnsOptionsForLevel(TurnsLevel::Max);
        CHECK(max.fullBash && max.opOutput && max.opDuration && max.fullWrite && max.stats,
              "TranscriptTurns: Max = Full + Write contents + a stats line per turn");
        const TurnsOptions bogus = TurnsOptionsForLevel(static_cast<TurnsLevel>(42));
        CHECK(bogus.ops == def.ops && bogus.fullBash == def.fullBash && bogus.notes == def.notes, "TranscriptTurns: an out-of-range level reads as Default");
        CHECK(std::wstring{ TurnsLevelName(TurnsLevel::Minimal) } == L"minimal" && std::wstring{ TurnsLevelName(TurnsLevel::Default) } == L"default" &&
                  std::wstring{ TurnsLevelName(TurnsLevel::Max) } == L"max",
              "TranscriptTurns: level names");
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
