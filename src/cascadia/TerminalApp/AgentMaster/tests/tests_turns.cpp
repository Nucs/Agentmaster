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
// Plus the summary panel's WHOLE-CONVERSATION view, which reuses this port's chain walk (section 8):
// TestSummaryConversation (SummaryConversation.h's numbered list, pure) and TestConversationUpstream
// (CollectConversationUpstream end to end on synthetic transcripts — every kind of link, never downstream,
// the cache rules). Shared CHECK/fixtures/decls live in m5_tests.h.
#include "m5_tests.h"

#include "../SummaryConversation.h" // BuildSummaryConversation / CodexSummaryConversation (TestSummaryConversation, TestConversationUpstream)
#include "../TranscriptTurns.h"

#include <algorithm> // std::find (the conversation-chain tests)
#include <cwchar> // std::swprintf
#include <fstream>
#include <iterator> // std::size
#include <map>
#include <stdexcept> // std::runtime_error (a throwing recorded-predecessor lookup)
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
// misattributes; and the Codex rows.
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

        // ---- the whole numbered list for G, end to end ----
        const auto titleOf = [&](const std::wstring& sid) { return sid == idP ? std::wstring{ L"Title of P" } : std::wstring{}; };
        const auto rows = BuildSummaryConversation(aG, upG, /*showHistory*/ true, titleOf);
        CHECK(rows.size() == 7, "upstream/rows: G's conversation = 5 prompts + a separator after P and after F");
        if (rows.size() == 7)
        {
            CHECK(rows[0].text == L"study the market data" && rows[1].text == L"backtest the strategy" && rows[2].text == L"try a stop loss" &&
                      rows[4].text == L"fork: try MSFT" && rows[6].text == L"fork again: tune the stop",
                  "upstream/rows: the prompts in conversation order (the renderer numbers them 1..5)");
            CHECK(rows[3].sessionId == idP && rows[3].continuedBy == L"fork" && rows[3].sessionTitle == L"Title of P" &&
                      rows[5].sessionId == idF && rows[5].continuedBy == L"fork",
                  "upstream/rows: a separator closes P's part and F's part");
            CHECK(rows[0].headIndex == -1 && rows[1].headIndex == -1 && rows[2].headIndex == 0 && rows[4].headIndex == 1 && rows[6].headIndex == 2,
                  "upstream/rows: the prompts G's terminal holds keep their jump index; the rest have none");
        }
        const auto classic = BuildSummaryConversation(aG, upG, /*showHistory*/ false, titleOf);
        CHECK(classic.size() == 3 && classic[0].text == L"try a stop loss" && classic[2].text == L"fork again: tune the stop",
              "upstream/rows: the toggle OFF lists G's own file, as before the chain existed");
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
