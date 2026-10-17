// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 test harness - the SESSION DATABASE's parts that the app and the service share
// (doc/agentmaster/SESSIONS_DB.md). The service itself (sessiondb\, DuckDB) is not in this suite: it is
// checked end to end by its one-shot modes on a scratch profile (SESSIONS_DB.md section 12).
//
// TestSessionDb pins:
//   * SessionDbProtocol.h: one set of names per profile whatever the path's case, slashes and trailing
//     separators (the CLI must find the service the GUI started); two profiles never share a pipe or a mutex;
//     the files under <profile>\sessiondb; the frame codec (4-byte little-endian length, then the payload);
//   * SessionDbTokenizer.h: tokens and identifier parts (camelCase, snake_case, acronyms, digits), the token and
//     block caps (D4), the query side (rule 1 / rule 2, separators only), FoldText;
//   * SessionDbToolDefs.h: the ten tools in their order, unique names, ASCII text (section 8.4.2), every schema
//     a JSON object schema with additionalProperties false, positive budgets, FindTool;
//   * the clamps of the database's three numeric settings (refresh 1..60 s, threads 1..16, memory 256..16384 MB);
//   * the transcript watcher's CHANGE SET (D16, TranscriptIndex::SetChangeTracking / TakeChanged / ClearChanged):
//     off by default, each kind of change recorded once per id, handed out and emptied, cleared, dropped when
//     tracking stops;
//   * the FACTS EPOCH (SessionFactsEpoch): it moves on a changed title, favorite or tag list, and not on an
//     unchanged write or on another key;
//   * the AI search's DATABASE MODE (SessionSearchToolset with Callbacks::database, section 8.2): the servers
//     and their tools, the database system prompt (eight rules, every database tool named), a search recording
//     its rows (show by ids and by top:N, the candidates), show resolving an id that another tool printed, an
//     unknown id refused, a replaced request answering "cancelled" and leaving no trace, DescribeDatabaseCall.

#include "m5_tests.h"

#include "../AgentHarness.h" // HarnessToolServer / HarnessToolResult (types only: no SDK)
#include "../SessionDbProtocol.h"
#include "../SessionDbTokenizer.h"
#include "../SessionDbToolDefs.h"
#include "../SessionSearchTools.h"
#include "../SessionStore.h"
#include "../TranscriptWatcher.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // The tool `name` of the server `server` among `servers`, or nullptr.
    const HarnessTool* FindHarnessTool(const std::vector<HarnessToolServer>& servers, const std::wstring& server, const std::wstring& name)
    {
        for (const auto& s : servers)
        {
            if (s.name != server)
            {
                continue;
            }
            for (const auto& t : s.tools)
            {
                if (t.name == name)
                {
                    return &t;
                }
            }
        }
        return nullptr;
    }

    // Parse JSON text that the test wrote itself (a parse failure is a test bug, so it is a failed check).
    json::Value Args(const std::wstring& text)
    {
        auto parsed = json::Parse(text);
        CHECK(parsed.has_value(), "test JSON parses");
        return parsed ? *parsed : json::Value::MkObj();
    }

    // ===== SessionDbProtocol.h =====

    void TestProtocolNames()
    {
        using namespace SessionDbProtocol;
        const std::wstring a = L"C:\\Users\\Someone\\.agentmaster";
        CHECK(FoldProfilePath(L"C:/Users/SOMEONE/.agentmaster\\\\") == FoldProfilePath(a), "the profile folds case, slashes and trailing separators");
        CHECK(FoldProfilePath(L"C:\\") == L"c:\\", "a drive root keeps its separator");
        CHECK(PipeName(L"c:\\users\\someone\\.AGENTMASTER") == PipeName(a), "one pipe per profile, whatever the spelling");
        CHECK(MutexName(L"C:/Users/someone/.agentmaster/") == MutexName(a), "one service mutex per profile, whatever the spelling");
        CHECK(StartMutexName(a) != MutexName(a), "the start mutex is not the service mutex");
        CHECK(PipeName(a) != PipeName(L"C:\\Users\\Someone\\.agentmaster-dev"), "two profiles never share a pipe");
        CHECK(PipeName(a).rfind(L"\\\\.\\pipe\\agentmaster.sessiondb.", 0) == 0, "the pipe lives under \\\\.\\pipe\\agentmaster.sessiondb.");
        const std::wstring hash = ProfileHash(a); // one string: begin() and end() must belong to the same object
        CHECK(hash.size() == 16, "the profile hash is 16 hex digits");
        CHECK(std::all_of(hash.begin(), hash.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }),
              "the profile hash is lowercase hex");
        CHECK(ServiceDir(a + L"\\") == a + L"\\sessiondb", "the service folder is <profile>\\sessiondb");
        CHECK(DatabasePath(a) == a + L"\\sessiondb\\sessions.duckdb", "the database file");
        CHECK(TempDir(a) == a + L"\\sessiondb\\tmp", "the spill folder");
        CHECK(BinDir(a) == a + L"\\sessiondb\\bin", "the copied-out service folder");
        CHECK(LogPath(a) == a + L"\\sessiondb\\sessiondb.log", "the service log");
        CHECK(DiscoveryPath(a) == a + L"\\sessiondb\\service.json", "the discovery file");

        // The frame codec: little-endian length, then the payload, both ways.
        const std::string payload = "{\"id\":1,\"method\":\"hello\"}";
        const std::string frame = EncodeFrame(payload);
        CHECK(frame.size() == 4 + payload.size(), "a frame is 4 bytes + the payload");
        CHECK(DecodeFrameLength(reinterpret_cast<const unsigned char*>(frame.data())) == payload.size(), "the length decodes");
        CHECK(frame.substr(4) == payload, "the payload follows the header");
        const std::string big(70000, 'x');
        const std::string bigFrame = EncodeFrame(big);
        CHECK(static_cast<unsigned char>(bigFrame[0]) == (70000 & 0xFF) && static_cast<unsigned char>(bigFrame[1]) == ((70000 >> 8) & 0xFF) &&
                  static_cast<unsigned char>(bigFrame[2]) == ((70000 >> 16) & 0xFF) && bigFrame[3] == 0,
              "the length is little-endian");
        CHECK(DecodeFrameLength(reinterpret_cast<const unsigned char*>(EncodeFrame("").data())) == 0, "an empty payload is a 4-byte frame");

        // Method names the client and the service must agree on (a rename here is a protocol change).
        CHECK(kPageSearch == "pageSearch" && kResolveIds == "resolveIds" && kToolCall == "toolCall" && kToolList == "toolList",
              "the method names the app uses");
        CHECK(kErrTimeout == 408 && kErrCancelled == 499 && kErrBadRequest == 400, "the error codes");
    }

    // ===== SessionDbToolDefs.h =====

    void TestToolDefs()
    {
        using namespace SessionDbToolDefs;
        const char* const expected[] = { "search", "session", "messages", "grep", "files", "who_touched", "timeline", "usage", "related", "sql" };
        CHECK(kToolCount == std::size(expected), "ten database tools");
        std::set<std::string> names;
        for (size_t i = 0; i < kToolCount && i < std::size(expected); ++i)
        {
            const ToolDef& t = kTools[i];
            CHECK(std::strcmp(t.name, expected[i]) == 0, "the tools in their order");
            names.insert(t.name);
            CHECK(t.budgetMs > 0, "every tool has a budget");
            const auto ascii = [](const char* s) {
                for (; *s; ++s)
                {
                    if (static_cast<unsigned char>(*s) > 0x7F)
                    {
                        return false;
                    }
                }
                return true;
            };
            CHECK(ascii(t.description) && ascii(t.inputSchema), "tool text is ASCII (section 8.4.2)");
            CHECK(std::strlen(t.description) > 40, "every tool has a real description");
            // The schema parses as a JSON object schema that refuses unknown arguments.
            std::wstring schema;
            for (const char* p = t.inputSchema; *p; ++p)
            {
                schema.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
            }
            const auto parsed = json::Parse(schema);
            CHECK(parsed && parsed->type == json::Value::Type::Obj, "every schema parses as a JSON object");
            if (parsed)
            {
                CHECK(parsed->StrAt(L"type") == L"object", "every schema is an object schema");
                const auto* extra = parsed->Find(L"additionalProperties");
                CHECK(extra && extra->type == json::Value::Type::Bool && !extra->boolean, "every schema sets additionalProperties false");
                const auto* props = parsed->Find(L"properties");
                CHECK(props && props->type == json::Value::Type::Obj && !props->members.empty(), "every schema names its properties");
            }
            CHECK(FindTool(t.name) == &t, "FindTool finds each tool");
        }
        CHECK(names.size() == kToolCount, "the tool names are unique");
        CHECK(FindTool("show") == nullptr, "show is not a database tool (it changes the screen: the app's)");
        CHECK(FindTool(nullptr) == nullptr && FindTool("") == nullptr && FindTool("searc") == nullptr && FindTool("searchx") == nullptr,
              "FindTool matches whole names only");
    }

    // ===== the tokenizer (section 6.3; SessionDbTokenizer.h, shared by the service's index and its query side) =====

    void TestTokenizer()
    {
        using namespace SessionDbTokenizer;
        const Classifier ascii = BasicClassify;
        // A path: '.' separates tokens; each camelCase token also yields its parts, folded.
        auto tf = TermFrequencies(L"TerminalPage.AgentSessionsPage.cpp", ascii);
        CHECK(tf.count(L"terminalpage") && tf.count(L"agentsessionspage") && tf.count(L"cpp"), "a path's tokens, folded");
        CHECK(tf.count(L"terminal") && tf.count(L"agent") && tf.count(L"sessions"), "camelCase parts");
        CHECK(tf[L"page"] == 2, "a part counts each occurrence");
        // snake_case: '_' is a word character (the token stays whole) and splits the parts.
        CHECK(IdentifierParts(L"pending_input_draft", ascii) == std::vector<std::wstring>({ L"pending", L"input", L"draft" }), "snake_case parts");
        CHECK(IdentifierParts(L"HTMLParser", ascii) == std::vector<std::wstring>({ L"html", L"parser" }), "an acronym's last capital starts the next part");
        CHECK(IdentifierParts(L"aValue", ascii).empty(), "a one-letter part is dropped, and one part left is the token itself");
        CHECK(IdentifierParts(L"renderSessions2", ascii) == std::vector<std::wstring>({ L"render", L"sessions2" }), "digits continue a part");
        CHECK(IdentifierParts(L"plain", ascii).empty(), "a plain word has no parts");
        // The caps: a token keeps kMaxTokenChars; a block is tokenized up to kMaxIndexedChars.
        tf = TermFrequencies(std::wstring(300, L'x'), ascii);
        CHECK(tf.size() == 1 && tf.begin()->first.size() == kMaxTokenChars, "a long token is cut to kMaxTokenChars");
        std::wstring big(kMaxIndexedChars, L' ');
        big += L"beyond";
        tf = TermFrequencies(L"start " + big, ascii);
        CHECK(tf.count(L"start") && !tf.count(L"beyond"), "only the first kMaxIndexedChars characters of a block are indexed (D4)");
        // The query side: a word as typed (no parts), separators decide the rule.
        auto q = TokenizeQueryTerm(L"Tab", ascii);
        CHECK(q.tokens == std::vector<std::wstring>({ L"tab" }) && !q.hasSeparator, "one query word: rule 1");
        q = TokenizeQueryTerm(L"foo.BarBaz", ascii);
        CHECK(q.tokens == std::vector<std::wstring>({ L"foo", L"barbaz" }) && q.hasSeparator, "a dotted query term: rule 2, no identifier parts");
        q = TokenizeQueryTerm(L"::=", ascii);
        CHECK(q.tokens.empty() && q.hasSeparator, "separators only: verified on the other terms' candidates");
        CHECK(FoldText(L"AbC.dE", ascii) == L"abc.de", "FoldText folds and keeps the separators");
    }

    // ===== the settings' clamps =====

    void TestSettingsClamps()
    {
        CHECK(ClampSessionDbRefreshSeconds(0) == 5 && ClampSessionDbRefreshSeconds(1) == 1 && ClampSessionDbRefreshSeconds(60) == 60 &&
                  ClampSessionDbRefreshSeconds(61) == 60,
              "the refresh period: 0 -> 5, then 1..60");
        CHECK(ClampSessionDbThreads(0) == 4 && ClampSessionDbThreads(1) == 1 && ClampSessionDbThreads(17) == 16, "the threads: 0 -> 4, then 1..16");
        CHECK(ClampSessionDbMemoryMb(0) == 1024 && ClampSessionDbMemoryMb(100) == 256 && ClampSessionDbMemoryMb(2048) == 2048 &&
                  ClampSessionDbMemoryMb(100000) == 16384,
              "the memory limit: 0 -> 1024, then 256..16384");
        AppSettings defaults;
        CHECK(defaults.sessionDbMode == SessionDbMode::Off, "the database is Off by default");
        CHECK(defaults.sessionDbRefreshSeconds == 5 && defaults.sessionDbThreads == 4 && defaults.sessionDbMemoryMb == 1024, "the defaults");
    }

    // ===== the transcript watcher's change set (D16) =====

    void TestChangeSet()
    {
        TranscriptIndex index;
        const std::wstring idA = L"aaaaaaaa-1111-2222-3333-444444444444";
        const std::wstring idB = L"BBBBBBBB-1111-2222-3333-444444444444";
        index.AddTranscript(L"K--proj", idA, L"C:\\p\\K--proj\\" + idA + L".jsonl");
        CHECK(index.TakeChanged().empty(), "tracking is off by default: no set is kept");

        index.SetChangeTracking(true);
        index.TouchTranscript(idA);
        index.TouchTranscript(idA); // twice: one entry
        index.TouchSide(idB);
        auto changed = index.TakeChanged();
        std::sort(changed.begin(), changed.end());
        CHECK(changed.size() == 2, "each changed session once");
        CHECK(std::find(changed.begin(), changed.end(), idA) != changed.end(), "a modified transcript is a change");
        CHECK(std::find(changed.begin(), changed.end(), L"bbbbbbbb-1111-2222-3333-444444444444") != changed.end(),
              "a side file is a change of its session, and ids are lowercase");
        CHECK(index.TakeChanged().empty(), "a take empties the set");

        index.AddTranscript(L"K--proj", idB, L"C:\\p\\K--proj\\" + idB + L".jsonl");
        index.RemoveTranscript(L"K--proj", idA);
        changed = index.TakeChanged();
        CHECK(changed.size() == 2, "an added and a removed transcript are changes");

        index.TouchTranscript(idA);
        index.ClearChanged();
        CHECK(index.TakeChanged().empty(), "ClearChanged drops the set (a re-index made it meaningless)");

        index.TouchTranscript(idA);
        index.SetChangeTracking(false);
        CHECK(index.TakeChanged().empty(), "turning tracking off drops the set");
        index.TouchTranscript(idA);
        CHECK(index.TakeChanged().empty(), "nothing is recorded while tracking is off");
    }

    // ===== the facts epoch =====

    void TestFactsEpoch()
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"agentmaster-m5-facts-" + std::to_wstring(::GetCurrentProcessId());
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring sid = L"cccccccc-1111-2222-3333-444444444444";

        uint64_t e0 = SessionFactsEpoch();
        CHECK(SetSessionStoreFieldIn(dir, sid, kSessionStoreTitleKey, L"A title"), "a title write");
        uint64_t e1 = SessionFactsEpoch();
        CHECK(e1 > e0, "a changed title moves the facts epoch");
        CHECK(SetSessionStoreFieldIn(dir, sid, kSessionStoreTitleKey, L"A title"), "the same title again");
        CHECK(SessionFactsEpoch() == e1, "an unchanged write does not move it");
        CHECK(SetSessionStoreFieldIn(dir, sid, kSessionStoreFavoriteKey, L"1"), "a favorite write");
        const uint64_t e2 = SessionFactsEpoch();
        CHECK(e2 > e1, "a changed favorite moves it");
        CHECK(SetSessionStoreFieldIn(dir, sid, kSessionStoreTagsKey, L"[\"x\"]"), "a tags write");
        const uint64_t e3 = SessionFactsEpoch();
        CHECK(e3 > e2, "a changed tag list moves it");
        CHECK(SetSessionStoreFieldIn(dir, sid, L"draft", L"some unsent text"), "a draft write");
        CHECK(SessionFactsEpoch() == e3, "another key (the draft) does not move it");
        CHECK(SetSessionStoreFieldIn(dir, sid, kSessionStoreFavoriteKey, L""), "a favorite removed");
        CHECK(SessionFactsEpoch() > e3, "removing a fact moves it");

        // Clean up the scratch store (its one record file and the folder).
        WIN32_FIND_DATAW fd{};
        const HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    ::DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        ::RemoveDirectoryW(dir.c_str());
    }

    // ===== the AI search's database mode (section 8.2) =====

    void TestDatabaseMode()
    {
        // A fake service: the search tool answers two rows, grep one row; resolve knows three short ids.
        const std::wstring full1 = L"aaaaaaaa-0000-0000-0000-000000000001";
        const std::wstring full2 = L"bbbbbbbb-0000-0000-0000-000000000002";
        const std::wstring full3 = L"cccccccc-0000-0000-0000-000000000003";
        std::vector<std::string> calls;
        std::vector<std::vector<std::wstring>> resolves;
        std::vector<std::wstring> candidates;
        std::vector<std::wstring> shown;
        std::wstring searching;
        std::atomic<uint64_t> generation{ 1 };
        bool bumpDuringCall = false;

        SessionSearchToolset::Callbacks cb;
        cb.onShow = [&](const std::vector<std::wstring>& ids, const std::wstring&, const std::vector<std::wstring>&) { shown = ids; };
        cb.onCandidates = [&](const std::vector<std::wstring>& ids) { candidates = ids; };
        cb.onSearching = [&](const std::wstring& s) { searching = s; };
        cb.generation = [&] { return generation.load(); };
        cb.database.call = [&](const std::string& tool, const json::Value& args) -> HarnessToolResult {
            calls.push_back(tool);
            if (bumpDuringCall)
            {
                ++generation; // a newer request arrived while this call ran
            }
            if (tool == "search")
            {
                CHECK(args.StrAt(L"query") == L"tab drag", "the model's arguments reach the service as they are");
                return { L"{\"now\":\"2026-10-15 13:00\",\"rows\":[{\"id\":\"aaaaaaaa\",\"title\":\"one\"},{\"id\":\"bbbbbbbb\",\"title\":\"two\"}]}", false };
            }
            if (tool == "grep")
            {
                return { L"{\"hits\":[{\"id\":\"cccccccc\",\"ref\":\"cccccccc:12\"}]}", false };
            }
            return { L"{\"error\":\"nope\"}", true };
        };
        cb.database.resolve = [&](const std::vector<std::wstring>& handles) {
            resolves.push_back(handles);
            std::vector<std::wstring> out;
            for (const auto& h : handles)
            {
                out.push_back(h == L"aaaaaaaa" ? full1 : h == L"bbbbbbbb" ? full2 : h == L"cccccccc" ? full3 : std::wstring{});
            }
            return out;
        };
        // No catalog: the database mode never asks for one.
        SessionSearchToolset toolset{ cb };
        CHECK(toolset.UsesDatabase(), "the toolset serves the database");

        const auto servers = toolset.Servers();
        CHECK(servers.size() == 2 && servers[0].name == L"agentmaster" && servers[1].name == L"display", "the servers: agentmaster and display");
        CHECK(servers.size() == 2 && servers[0].tools.size() == SessionDbToolDefs::kToolCount + 1 && servers[0].tools.back().name == L"read",
              "every database tool is offered, and the toolset's own read after them (SESSIONS.md 9.12)");
        CHECK(FindHarnessTool(servers, L"sessions", L"search") == nullptr, "the page search is not offered beside the database");
        bool allReadOnly = !servers.empty();
        for (const auto& t : servers[0].tools)
        {
            allReadOnly = allReadOnly && t.readOnly && !t.description.empty() && !t.inputSchemaJson.empty();
        }
        CHECK(allReadOnly, "every database tool is read-only and described");
        const HarnessTool* search = FindHarnessTool(servers, L"agentmaster", L"search");
        const HarnessTool* grep = FindHarnessTool(servers, L"agentmaster", L"grep");
        const HarnessTool* show = FindHarnessTool(servers, L"display", L"show");
        CHECK(search && grep && show, "search, grep and show are there");
        if (!search || !grep || !show)
        {
            return;
        }

        // The database prompt (SESSIONS.md 9.14's structure, pinned in full by tests_tabtools.cpp): the behaviors and the
        // workflows, every database tool named as the model calls it, its own read before a show.
        const std::wstring prompt = SessionSearchToolset::DatabaseSystemPrompt();
        CHECK(prompt.find(L"# Instructional Behaviors") != std::wstring::npos && prompt.find(L"## Tools") != std::wstring::npos && prompt.find(L"## Workflow Guidelines") != std::wstring::npos,
              "the database prompt has the behaviors, the tools and the workflows");
        CHECK(prompt.find(L"mcp__agentmaster__read") != std::wstring::npos && prompt.find(L"Read a session before you show it") != std::wstring::npos, "the database prompt confirms candidates with its read");
        bool allNamed = true;
        for (const auto& t : SessionDbToolDefs::kTools)
        {
            std::wstring name;
            for (const char* p = t.name; *p; ++p)
            {
                name.push_back(static_cast<wchar_t>(*p));
            }
            allNamed = allNamed && prompt.find(L"mcp__agentmaster__" + name) != std::wstring::npos;
        }
        CHECK(allNamed, "the database prompt names every database tool");
        CHECK(prompt.find(L"mcp__display__show") != std::wstring::npos && prompt.find(L"deep") == std::wstring::npos,
              "the database prompt names show, and no `deep` switch (the database has scopes)");
        CHECK(std::wstring{ SessionSearchToolset::SystemPrompt() } != prompt, "the page search keeps its own prompt");

        // A search: forwarded, its rows resolved, recorded and previewed.
        auto r = search->handler(Args(L"{\"query\":\"tab drag\",\"days\":365}"));
        CHECK(!r.isError && r.text.find(L"aaaaaaaa") != std::wstring::npos, "the service's result reaches the model as it is");
        CHECK(calls.size() == 1 && calls[0] == "search", "one forwarded call");
        CHECK(resolves.size() == 1 && resolves[0].size() == 2, "the search's two short ids resolved in one round trip");
        CHECK(candidates == std::vector<std::wstring>({ full1, full2 }), "the candidates are the whole ids, in result order");
        CHECK(searching.find(L"\"tab drag\"") != std::wstring::npos && searching.find(L"last 365 days") != std::wstring::npos,
              "the page's line describes the search");

        // show by ids, by top:N.
        r = show->handler(Args(L"{\"ids\":[\"bbbbbbbb\",\"AAAAAAAA\"],\"note\":\"x\"}"));
        CHECK(!r.isError && shown == std::vector<std::wstring>({ full2, full1 }), "show takes the search's short ids (any case), in the model's order");
        CHECK(resolves.size() == 1, "known ids need no round trip");
        r = show->handler(Args(L"{\"top\":1}"));
        CHECK(!r.isError && shown == std::vector<std::wstring>({ full1 }), "top:N shows the latest search's first rows");

        // An id that another tool printed: resolved when shown.
        r = grep->handler(Args(L"{\"pattern\":\"exact text\"}"));
        CHECK(!r.isError && calls.back() == "grep", "grep is forwarded");
        CHECK(candidates == std::vector<std::wstring>({ full1, full2 }), "only a search previews candidates");
        r = show->handler(Args(L"{\"ids\":[\"cccccccc\",\"zzzzzzzz\"]}"));
        CHECK(!r.isError && shown == std::vector<std::wstring>({ full3 }), "show resolves an id grep printed");
        CHECK(r.text.find(L"zzzzzzzz") != std::wstring::npos, "an unknown id is named in the answer");
        r = show->handler(Args(L"{\"ids\":[\"yyyyyyyy\"]}"));
        CHECK(r.isError && shown == std::vector<std::wstring>({ full3 }), "only unknown ids: refused, the screen untouched");
        CHECK(r.text.find(L"names a session") != std::wstring::npos, "the database mode's refusal wording");

        // A request replaced while its call ran: cancelled, and the latest search stays the earlier one.
        bumpDuringCall = true;
        r = search->handler(Args(L"{\"query\":\"tab drag\"}"));
        bumpDuringCall = false;
        CHECK(r.isError && r.text.find(L"cancelled") != std::wstring::npos, "a replaced request answers cancelled");
        r = show->handler(Args(L"{\"top\":2}"));
        CHECK(shown == std::vector<std::wstring>({ full1, full2 }), "a cancelled search leaves no trace (top:N still the earlier rows)");

        // A tool error passes through (the service's message tells the model how to fix the call).
        const HarnessTool* sql = FindHarnessTool(servers, L"agentmaster", L"sql");
        r = sql ? sql->handler(Args(L"{\"query\":\"DELETE FROM x\"}")) : HarnessToolResult{ L"", false };
        CHECK(r.isError, "a tool error reaches the model as an error");

        // The page's lines for the other tools.
        CHECK(SessionSearchToolset::DescribeDatabaseCall("grep", Args(L"{\"pattern\":\"foo\"}")) == L"grep \"foo\"", "grep's line");
        CHECK(SessionSearchToolset::DescribeDatabaseCall("messages", Args(L"{\"id\":\"1e0f7ab2\"}")) == L"reading 1e0f7ab2", "messages' line");
        CHECK(SessionSearchToolset::DescribeDatabaseCall("who_touched", Args(L"{\"path\":\"Tab.cpp\"}")) == L"who touched \"Tab.cpp\"", "who_touched's line");
        CHECK(SessionSearchToolset::DescribeDatabaseCall("search", Args(L"{}")).find(L"all sessions") == 0, "a search without words lists sessions");

        // Without the database, the catalog is required.
        SessionSearchToolset::Callbacks none;
        none.onShow = [](const std::vector<std::wstring>&, const std::wstring&, const std::vector<std::wstring>&) {};
        bool threw = false;
        try
        {
            SessionSearchToolset t{ none };
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw, "the page search still needs its catalog");
    }
}

void TestSessionDb()
{
    TestProtocolNames();
    TestToolDefs();
    TestTokenizer();
    TestSettingsClamps();
    TestChangeSet();
    TestFactsEpoch();
    TestDatabaseMode();
}
