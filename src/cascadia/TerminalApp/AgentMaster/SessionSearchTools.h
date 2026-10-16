// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionSearchTools: what the model can DO in the Sessions browser's AI search
// (SESSIONS.md §9, SDK_HARNESS.md §7). The Telegram 🔎 Search topic runs the same tools in its own
// conversation (MESSAGING.md §5.4).
//
// The AI search is a hosted conversation (AgentHarness.h) with two tool servers:
//
//   sessions.search  — "which sessions match?"  Runs the Sessions page's own search (the sidecar
//                      index, the history.jsonl accelerator and, on request, the transcript text
//                      scan) over a CATALOG of sessions, with the page's filters as arguments.
//   sessions.grep    — "where exactly?"  Finds a text or a regex in the conversations' full text
//                      and returns each match in its context, with who wrote it and when
//                      (GrepTranscripts): the evidence for a candidate (SESSIONS.md §9.12).
//   sessions.read    — "is it really about that?"  Reads up to eight sessions as the compact
//                      Markdown of Copy ▸ Transcript (TranscriptTurns), a long one cut to its first
//                      and last turns, so the model confirms every session before it shows it.
//   display.show     — "present these." Hands an ordered list of session ids — each with the one
//                      line of evidence the model saw (`why`) — to the surface: the Sessions page
//                      shows exactly those rows in that order, Telegram one card per session.
//
// The model understands the request, calls search (once, or a few times with other words), reads
// the candidates (and greps for the user's own words), keeps what fits, calls show, and writes one
// sentence. The split is the point: search, grep and read never change what the user sees, so the
// model can look around freely, and the show tool is the ONE place where the result changes.
//
// With the session database On (SESSIONS_DB.md §8.2; Callbacks::database), the first server is
// replaced by the database's read tools (SessionDbToolDefs.h) as the server `agentmaster`: search
// (with scopes instead of `deep`), session, messages, grep, files, who_touched, timeline, usage,
// related and sql, each call forwarded to the database service — plus this file's own `read`, so a
// candidate is confirmed the same way in both modes. No catalog is read then (no sidecar gather,
// no index keeper); `show` and its short ids work the same way, and a short id the conversation's
// own searches never printed is resolved through the service.
//
// This file needs neither the SDK nor nlohmann/json (it uses AgentHarness.h only for the tool
// types), so the standalone test harness compiles it and calls the tool handlers directly.
// Plain C++ + Win32, no WinRT (the .cpp is <PrecompiledHeader>NotUsing</PrecompiledHeader>).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AgentHarness.h" // HarnessToolServer / HarnessToolResult
#include "Json.h"
#include "SessionSearch.h" // SessionQuery / SessionHit
#include "TranscriptStore.h" // SessionIndexEntry / TranscriptRef

namespace Agentmaster
{
    // ===== the catalog ======================================================================

    // The sessions a search can see: one immutable snapshot, shared by pointer between the tool
    // calls that use it. Built by GatherSessionCatalog, or handed over by the Sessions page from
    // the rows it gathered anyway (so the first search of a conversation reads no file).
    struct SessionCatalog
    {
        int64_t sinceUnixMs{}; // the period it was asked for: sessions whose transcript changed at or after this instant
        // The instant from which it is COMPLETE. Equal to sinceUnixMs for a whole gather; LATER
        // when the gather stopped early (its time budget ran out, or it was cancelled): then the
        // sessions between sinceUnixMs and this instant are missing, newest ones are all there.
        int64_t coveredSinceUnixMs{};
        int64_t gatheredUnixMs{}; // when it was built (a catalog older than kSessionCatalogMaxAgeMs is rebuilt)
        std::vector<SessionIndexEntry> entries; // newest transcript first; liveTitle carries a stored / live tab title
        std::unordered_map<std::wstring, std::vector<std::wstring>> tags; // session id -> its bookmark tags
        std::unordered_set<std::wstring> favorites; // session ids with the star
        std::unordered_set<std::wstring> openIds; // session ids that have a tab open in an Agentmaster window now
        // False when the stored titles, the tags and the favorites are NOT in this catalog: the
        // session store was not read yet in this process, and the gather had a budget or a
        // cancel check. (The store's FIRST read of a process opens every record file: seconds,
        // and 16 s were measured for 2,181 files that Windows had not cached.) A session then
        // shows its transcript's own title, and a tag or favorite filter matches nothing — a
        // search result says so. The caller reads the store (WarmSessionStore) and gathers again.
        bool storeOverlay{ true };

        // True when it holds every session of the period it was asked for. A search over a
        // catalog that is not complete must say so (FormatSessionSearchResult does).
        bool Complete() const noexcept { return coveredSinceUnixMs <= sinceUnixMs; }
    };

    // A catalog is trusted this long. Short, because a search must find the session the user
    // worked in a minute ago; long enough that the few searches of one request share one gather.
    inline constexpr int64_t kSessionCatalogMaxAgeMs = 20000;

    // The period a search covers when the model names none: the last 90 days. Longer than the
    // Sessions page's own "1 month", because a request like "the session where I fixed X" names
    // no period, and a second search with a longer one costs a model round trip (about a second).
    // The search tool's schema text and whoever reads the index ahead of a search use it.
    inline constexpr int kSessionSearchDefaultDays = 90;

    // The name a session is shown by: the stored / live tab title when there is one, else the
    // transcript's own (custom title > AI title > legacy summary > first prompt), else a
    // placeholder. The same precedence the Sessions page uses for its Title column.
    //   throws — std::bad_alloc.
    std::wstring SessionSearchTitleOf(const SessionIndexEntry& entry);

    // When the session last said something: the newest message's timestamp, else the file's
    // modification time. The Sessions page's "Active" column.
    int64_t SessionSearchLastActivityMs(const SessionIndexEntry& entry) noexcept;

    // When the conversation was created: its first message, or — for a fork, whose copied lines
    // carry the parent's times — the file's creation time. The Sessions page's "Created" column.
    int64_t SessionSearchCreatedMs(const SessionIndexEntry& entry) noexcept;

    // The folder the session was launched in: the working directory its lines carry, else the
    // (lossy) name of the transcript's project folder. The Sessions page's "Directory" column.
    //   throws — std::bad_alloc.
    std::wstring SessionSearchDirOf(const SessionIndexEntry& entry);

    // What GatherSessionCatalog needs from the app, each optional.
    struct SessionCatalogSources
    {
        // The sessions that have a tab open right now, as (id, tab title). Empty function = none
        // (the test harness; a process without a registry).
        std::function<std::vector<std::pair<std::wstring, std::wstring>>()> openSessions;
        // Where the transcripts and the sidecar files are. Empty = the real ones:
        // <claude home>\projects and <profile>\sessions-index. A test gives both a scratch
        // folder, so a gather is the same on every machine.
        std::wstring projectsDir;
        std::wstring indexDir;
    };

    // How a gather may save time, or be stopped.
    struct SessionCatalogGatherOptions
    {
        // An older catalog, or null. A session whose transcript has the same path, size and
        // modification time as in `previous` is taken from it without reading its sidecar file,
        // so a refresh costs the directory listing plus the sessions that changed. (Each taken
        // entry is COPIED: a refresh of a few thousand sessions copies tens of megabytes.)
        std::shared_ptr<const SessionCatalog> previous;
        // 0 = read the whole period. Else stop reading sidecar files this many milliseconds
        // after the FIRST one was opened, and return what is there: the NEWEST sessions (the
        // listing is newest first), with coveredSinceUnixMs saying how far back it got. The
        // clock starts at the first read, not at the call, so a gather with a budget always
        // reads at least one new sidecar: a caller that gathers again and again with `previous`
        // (the way to build a large index in steps) always moves forward.
        int64_t budgetMs{ 0 };
        // Empty = never. Else asked before each sidecar read; true stops the gather like a spent
        // budget does (but with no guaranteed read).
        std::function<bool()> cancelled;
    };

    // Build a catalog from disk: every transcript changed at or after `sinceUnixMs`, each through
    // its sidecar index (LoadOrRefreshSessionIndex — a cached entry costs one small file read; a
    // new or grown transcript is parsed), plus the stored titles, tags and favorites of the
    // session store and the open sessions of `sources`.
    //   returns — the catalog; never null. Check Complete() when a budget or a cancel was given.
    //   throws  — std::bad_alloc.
    // ⚠ The FIRST gather of a long period on a machine with many sessions parses every transcript
    //   whose sidecar is missing or out of date: MINUTES for a year of work (measured: 3 minutes
    //   for 3,600 sessions). Later gathers read one small file per session, or nothing with
    //   `previous`. Never call it on a UI thread, and give a budget when a person waits for it.
    std::shared_ptr<const SessionCatalog> GatherSessionCatalog(int64_t sinceUnixMs, const SessionCatalogSources& sources, const SessionCatalogGatherOptions& options = {});

    // ===== one search =======================================================================

    // The search tool's arguments, validated and with their defaults filled in.
    struct SessionSearchParams
    {
        std::wstring query; // words (all must match), "quoted phrases", or a session id; empty = no text filter
        int days{ kSessionSearchDefaultDays }; // sessions active in the last N days; ignored when afterUnixMs is set
        int64_t afterUnixMs{}; // 0 = use `days`; else sessions active at or after this instant
        int64_t beforeUnixMs{}; // 0 = until now; else sessions active at or before this instant
        std::wstring dir; // part of the working directory's path ('/' and '\' are the same)
        std::wstring branch; // part of the git branch name
        std::wstring tag; // a bookmark tag (case-insensitive, the whole name)
        bool favoriteOnly{};
        bool openOnly{};
        bool deep{}; // also read the conversations' full text: slow, and broad
        std::wstring sort{ L"relevance" }; // "relevance" | "active" | "created"
        int limit{ 15 }; // rows to return, 1..50
    };

    // Read the tool's arguments.
    //   arguments — the model's argument object.
    //   nowUnixMs — the current time (a parameter, so tests are deterministic).
    //   error     — receives what is wrong when the arguments cannot be used; it is written FOR
    //               THE MODEL (it becomes the tool's error result), so it says how to fix the call.
    //   returns   — the parameters, or nullopt with `error` set.
    //   throws    — std::bad_alloc.
    // Out-of-range numbers are clamped, not refused: a search that runs with limit 50 instead of
    // 500 is more useful to the model than an error.
    std::optional<SessionSearchParams> ParseSessionSearchParams(const json::Value& arguments, int64_t nowUnixMs, std::wstring& error);

    // The instant `params` searches from (the lower time bound): afterUnixMs, else now - days.
    int64_t SessionSearchSinceMs(const SessionSearchParams& params, int64_t nowUnixMs) noexcept;

    // Why a row matched, best reason first. The order is the relevance ranking.
    enum class SessionMatchTier
    {
        Name = 0, // every word matches the title, the branch or the id — "found it by name"
        Path = 1, // matches the working directory or a file the session touched
        Prompt = 2, // only a message the user typed matches
        Text = 3, // only the conversation's other text matches (deep search)
        Listed = 4, // no query: the row is listed because it passed the filters
    };

    // One row of a search result.
    struct SessionSearchRow
    {
        size_t entryIndex{}; // index into the catalog's `entries`
        SessionMatchTier tier{ SessionMatchTier::Listed };
        int hits{}; // matched messages (content tiers); 0 for a name / path match
        std::vector<std::wstring> snippets; // up to 2 pieces of matching text
    };

    // A search's answer.
    struct SessionSearchOutcome
    {
        size_t searched{}; // sessions inside the time window and the filters
        size_t matched{}; // of those, how many match the query
        std::vector<SessionSearchRow> rows; // the best `limit` of the matches, in result order
    };

    // The content search a query may need beyond the index: the user's typed prompts, and the
    // transcripts' text. Injected so the search core is pure in tests; the app uses
    // DefaultSessionContentSearch.
    struct SessionContentSearch
    {
        // sid -> hit over the typed prompts (history.jsonl). May be empty (no prompt search).
        std::function<std::unordered_map<std::wstring, SessionHit>(const SessionQuery&)> prompts;
        // Hits over the transcripts of `refs`. `cancelled` is asked between files; true ends the
        // scan with the hits so far. May be empty (deep search finds nothing).
        std::function<std::vector<SessionHit>(const std::vector<TranscriptRef>&, const SessionQuery&, const std::function<bool()>& cancelled)> transcripts;
    };

    // The real content search: SearchHistoryPrompts over <claude home>\history.jsonl and
    // SearchTranscriptsSlow (ripgrep-prefiltered) over the given transcripts.
    //   throws — std::bad_alloc.
    SessionContentSearch DefaultSessionContentSearch();

    // Run one search over a catalog. The semantics are the Sessions page's own: every word of the
    // query must match (each may match a different field); the index answers title / folder /
    // touched-path / id matches at once; the typed prompts are always searched too; the
    // transcripts' text only with params.deep.
    //   catalog   — what to search.
    //   params    — the validated arguments.
    //   nowUnixMs — the current time.
    //   content   — the prompt / transcript search (see SessionContentSearch).
    //   cancelled — empty = never; else handed to the transcript scan, which stops when it
    //               answers true (the outcome then holds what was found so far).
    //   returns   — the outcome; rows are ranked by tier, then by params.sort inside a tier
    //               (with sort "active" or "created" the tier is ignored).
    //   throws    — std::bad_alloc; whatever `content` throws.
    SessionSearchOutcome RunSessionSearch(const SessionCatalog& catalog, const SessionSearchParams& params, int64_t nowUnixMs, const SessionContentSearch& content, const std::function<bool()>& cancelled = {});

    // The SHORT id the model sees for a session: the first 8 characters of its id. Short on
    // purpose — the model must write every id it wants shown, and output tokens are what a
    // search waits for (a full id costs about five times as many).
    std::wstring SessionSearchHandleOf(const std::wstring& sessionId);

    // A search result as the JSON text the model reads: the window it covered, the counts, and
    // one compact object per row (short id, title, folder, branch, times, sizes, why it matched).
    //   handleOf — the short id to print for a session id; empty = SessionSearchHandleOf. (The
    //              toolset passes its own, which lengthens a short id that two sessions share.)
    //   throws   — std::bad_alloc; whatever `handleOf` throws.
    std::wstring FormatSessionSearchResult(const SessionCatalog& catalog, const SessionSearchParams& params, const SessionSearchOutcome& outcome, int64_t nowUnixMs, const std::function<std::wstring(const std::wstring&)>& handleOf = {});

    // A one-line description of a search for the user ("tab drag crash" · last 30 days · full text).
    //   throws — std::bad_alloc.
    std::wstring DescribeSessionSearch(const SessionSearchParams& params);

    // ===== grep and read: confirming a candidate (SESSIONS.md §9.12) ========================

    // How long one grep call may read before it answers with what it has. The model waits for
    // it, and while a tool runs the SDK's reader thread can deliver nothing else — not even the
    // interrupt of a request the user replaced (SessionSearchToolset's cancel check covers that).
    inline constexpr int64_t kSessionGrepBudgetMs = 20000;

    // The grep tool's arguments, validated and with their defaults filled in.
    struct SessionGrepParams
    {
        std::wstring pattern; // the text to find (required); a regex when `regex`
        bool regex{};
        std::vector<std::wstring> ids; // the ids as the model wrote them (short or whole); empty = every session of the period
        int days{ kSessionSearchDefaultDays }; // the period when no ids are given (like search)
        int64_t afterUnixMs{}; // 0 = use `days`
        int64_t beforeUnixMs{}; // 0 = until now
        std::wstring dir; // part of the working folder's path ('/' and '\' are the same)
        bool user{ true }; // read the user's typed prompts ("in": "all" or "user")
        bool agent{ true }; // read Claude's side ("in": "all" or "claude")
        int context{ 120 }; // characters on each side of a match, 40..400
        int limit{ 10 }; // sessions to return, 1..20
    };

    // Read the grep tool's arguments.
    //   arguments — the model's argument object.
    //   error     — receives what is wrong, written for the model (it becomes the error result).
    //   returns   — the parameters, or nullopt with `error` set.
    //   throws    — std::bad_alloc.
    // Numbers out of range are clamped; more than 20 ids are cut to the first 20.
    std::optional<SessionGrepParams> ParseSessionGrepParams(const json::Value& arguments, std::wstring& error);

    // What `read` renders: the Copy ▸ Transcript levels a confirming read needs.
    enum class SessionReadLevel
    {
        Minimal = 0, // each turn's user message and Claude's final reply
        Compact = 1, // + compactions, the user's answers, slash commands (the default)
        Default = 2, // + every tool call on one line and the remarks between them
    };

    // At most this many sessions per read call: one model step reads the usual handful of
    // candidates at once, and a call stays well under the context a request may use.
    inline constexpr size_t kSessionReadMaxIds = 8;
    // A session's share of a read result, in characters, when the model names none: the whole of a
    // median compact transcript (6,277 characters measured over 26 sessions, SESSIONS_AGENT.md
    // §4.2), the first and last turns of a longer one. The model may ask for 1,000 to 40,000.
    inline constexpr size_t kSessionReadDefaultChars = 6000;
    // How long one read call waits for its renders (they run in parallel, one thread each).
    inline constexpr int64_t kSessionReadBudgetMs = 20000;

    // The read tool's arguments.
    struct SessionReadParams
    {
        std::vector<std::wstring> ids; // as the model wrote them, each once; 1..kSessionReadMaxIds
        size_t extraIds{}; // ids past kSessionReadMaxIds: not read (the result tells the model to read them in another call)
        SessionReadLevel level{ SessionReadLevel::Compact };
        size_t chars{ kSessionReadDefaultChars }; // per session
    };

    // Read the read tool's arguments.
    //   arguments — the model's argument object.
    //   error     — receives what is wrong, written for the model.
    //   returns   — the parameters, or nullopt with `error` set (no ids, or ids not an array).
    //   throws    — std::bad_alloc.
    // More than kSessionReadMaxIds ids are cut to the first ones; `chars` is clamped.
    std::optional<SessionReadParams> ParseSessionReadParams(const json::Value& arguments, std::wstring& error);

    // A shown session's `why` is cut to this many characters (the prompt asks for under 120): a
    // surface shows it as one line under the title, or as a quote on a phone card.
    inline constexpr size_t kSessionWhyMaxChars = 300;

    // A rendered transcript cut to a budget, and what was left out.
    struct SessionReadCut
    {
        std::wstring text; // what the model reads
        size_t turns{}; // the transcript's turns (each "### " heading starts one)
        size_t omittedTurns{}; // turns left out in the middle (a "[... N turns left out ...]" line stands for them)
        bool cutInside{}; // a kept turn was itself cut short ("[... cut ...]")
    };

    // Cut a TranscriptTurns Markdown document to about `budgetChars`: everything when it fits;
    // else what comes before the first turn, the FIRST turns (about two fifths of the budget —
    // what the session was about) and the LAST turns (the rest — how it ended), with one line
    // naming how many turns were left out between them. A turn longer than its share is cut at
    // its end. Pure.
    //   markdown    — RenderTranscriptTurnsFile's output (turn headings start with "### ").
    //   budgetChars — the target size; at least 1,000 is used.
    //   throws      — std::bad_alloc.
    SessionReadCut CutRenderedTranscript(std::wstring_view markdown, size_t budgetChars);

    // ===== the tool servers =================================================================

    // The two tool servers of the Sessions AI search, and the state they share between calls of
    // one conversation: which short id means which session, the rows of the latest search (so
    // `show` can take "the first N of that"), and where each session's transcript is (so `read`
    // and `grep` with ids open the file a result named).
    //
    // Thread-safe: the handlers run on the SDK's reader thread, and read-only search calls of one
    // model turn may run at the same time.
    class SessionSearchToolset
    {
    public:
        struct Callbacks
        {
            // The catalog for a search from `sinceUnixMs` on. Required. Called on the tool-call
            // thread, with the model waiting: it should answer within a few seconds, with a
            // catalog that is not Complete() when it has to (the result then says so).
            // `cancelled` answers true when the search is not wanted any more.
            std::function<std::shared_ptr<const SessionCatalog>(int64_t sinceUnixMs, const std::function<bool()>& cancelled)> catalog;
            // A number that changes whenever the running request is not wanted any more (a newer
            // request, an interrupt, a reset). A search reads it when it starts and stops when it
            // changed. Optional: without it a search always runs to its end.
            std::function<uint64_t()> generation;
            // A search is starting: `summary` is DescribeSessionSearch's line. Optional.
            std::function<void(const std::wstring& summary)> onSearching;
            // A search returned these sessions (full ids, result order). Optional.
            std::function<void(const std::vector<std::wstring>& sessionIds)> onCandidates;
            // The model asked to show these sessions (full ids, the model's order) with this
            // caption, and with `whys` index-aligned to the ids: the one line of evidence the
            // model gave each session ("" where it gave none). Required: it is what changes what
            // the user sees.
            std::function<void(const std::vector<std::wstring>& sessionIds, const std::wstring& note, const std::vector<std::wstring>& whys)> onShow;
            SessionContentSearch content; // empty members = DefaultSessionContentSearch()
            std::function<int64_t()> now; // empty = the system clock
            // Where a session's transcript is, by its whole id, for a `read` or a `grep` with ids
            // of a session no search of this conversation returned (in the database mode, every
            // session). "" = not found. Called on the tool-call thread. Optional: empty =
            // ResolveClaudeTranscriptPath (<claude home>\projects\*\<id>.jsonl); a test points it
            // at its scratch folder.
            std::function<std::wstring(const std::wstring& sessionId)> transcriptPath;

            // The session database (SESSIONS_DB.md §8.2), optional. When `call` is set, the toolset
            // serves the DATABASE's tools (SessionDbToolDefs.h: search, session, messages, grep,
            // files, who_touched, timeline, usage, related, sql) as the in-process server
            // "agentmaster" INSTEAD of its own search, and `catalog` is never asked (it may be
            // empty). `show` stays the one tool that changes the screen. The app sets it when
            // AppSettings::sessionDbMode is On (SharedSessionSearchAgent).
            struct DatabaseTools
            {
                // Run one database tool with the model's arguments and return its result text
                // and error flag (the app forwards it to the service's `toolCall`). Called on the
                // tool-call thread with the model waiting; must be thread-safe (read-only calls of
                // one model step may run at the same time). May throw (the harness answers the
                // model with the message).
                std::function<HarnessToolResult(const std::string& tool, const json::Value& arguments)> call;
                // The whole session ids behind short ids, in order ("" where a handle is unknown
                // or shared by two sessions), in one round trip (the service's `resolveIds`).
                // Same thread rules. Optional: without it `show` knows only the ids of searches.
                std::function<std::vector<std::wstring>(const std::vector<std::wstring>& handles)> resolve;
            } database;
        };

        //   throws — std::invalid_argument when a required callback is missing (catalog — unless
        //            database.call is set — and onShow); std::bad_alloc.
        explicit SessionSearchToolset(Callbacks callbacks);

        // The servers to put into HarnessOptions::toolServers: "sessions" (search, grep, read)
        // and "display" (show); with Callbacks::database.call set, "agentmaster" (the database's
        // tools, then this toolset's own read) and "display" instead. The handlers refer to this
        // object: it must outlive the conversation's process.
        //   throws — std::bad_alloc.
        std::vector<HarnessToolServer> Servers();

        // True when the toolset serves the session database's tools (Callbacks::database.call).
        bool UsesDatabase() const noexcept
        {
            return static_cast<bool>(_callbacks.database.call);
        }

        // Forget the short ids, the latest search and the transcripts the results named (a
        // conversation reset).
        void Reset();

        // The full session id a short id of this conversation stands for, or empty.
        //   throws — std::bad_alloc.
        std::wstring ResolveHandle(const std::wstring& handle) const;

        // The system prompt of the Sessions AI search: the role, the four tools, and nine rules.
        // Rule 1 makes the model's FIRST step a short mention of which kinds of tool calls it can use
        // for the request, out of everything the tools can do, and how it will use them in different
        // ways — written in the same message as its first calls, so it costs output tokens in that
        // step but no extra model request. Rule 4 makes it READ every session it intends to show
        // (and grep for the user's own words), rule 5 give each a `why`, rule 6 answer in ONE
        // sentence that says what it found — never "on the screen", since the answer is also read
        // on a phone (the user's choice of 2026-10-17, SESSIONS.md §9.12). ASCII (it travels
        // through a file and the API unchanged).
        //   returns — a static, NUL-terminated string; never null.
        // ⚠ Its text is part of every request's prompt and of the prompt cache's key: a change costs a
        //   new cache entry, and the m5 suite pins rule 1 and the count of rules.
        static const wchar_t* SystemPrompt() noexcept;

        // The system prompt of the AI search on the SESSION DATABASE (Callbacks::database.call): the
        // same role and the same nine rules, for the database's tools (mcp__agentmaster__search with
        // `scopes` where the page search had `deep`; grep, who_touched, timeline, session, messages,
        // files, related, usage and sql to look further), this toolset's mcp__agentmaster__read to
        // confirm a candidate, and mcp__display__show. ASCII.
        //   returns — a static, NUL-terminated string; never null.
        // ⚠ Like SystemPrompt(): part of every request's prompt and of the prompt cache's key; the m5
        //   suite pins its rule count, that it names every database tool, and that it never says
        //   `deep` (the database has scopes).
        static const wchar_t* DatabaseSystemPrompt() noexcept;

        // A one-line description of a database tool call for the user (the page's "Searching: ..."
        // line): `"tab drag crash" · last 365 days` for search, `grep "foo"`, `reading 1e0f7ab2`, ...
        //   tool      — a SessionDbToolDefs.h name.
        //   arguments — the model's argument object.
        //   throws    — std::bad_alloc.
        static std::wstring DescribeDatabaseCall(const std::string& tool, const json::Value& arguments);

        // The servers' and tools' names (the model calls mcp__sessions__search, mcp__sessions__grep,
        // mcp__sessions__read and mcp__display__show).
        static constexpr wchar_t kSearchServer[] = L"sessions";
        static constexpr wchar_t kSearchTool[] = L"search";
        static constexpr wchar_t kGrepTool[] = L"grep";
        static constexpr wchar_t kReadTool[] = L"read";
        static constexpr wchar_t kDisplayServer[] = L"display";
        static constexpr wchar_t kDisplayTool[] = L"show";
        // The database's server (the model calls mcp__agentmaster__<tool>): the same name as the
        // stdio MCP server `agentmaster-sessiondb.exe mcp`, so a tool reads the same everywhere.
        static constexpr wchar_t kDatabaseServer[] = L"agentmaster";

    private:
        // What the conversation's own results told it about a session: enough for `read` and a
        // `grep` with ids to open its transcript and name it without asking the catalog again.
        struct KnownSession
        {
            std::wstring path; // the transcript
            std::wstring title; // SessionSearchTitleOf, cut for a header line
            int64_t activeMs{}; // SessionSearchLastActivityMs
            int64_t sizeBytes{}; // the transcript's size when it was listed (a grep batch's weight)
        };

        HarnessToolResult Search(const json::Value& arguments);
        // The grep tool: GrepTranscripts over the sessions the ids name, or over the catalog of
        // the period (with `dir`), newest first, within kSessionGrepBudgetMs. Its rows get short
        // ids like a search's (show and read take them); it is not a "latest search" (top:N) and
        // previews nothing on the page.
        //   returns — the result JSON (FormatGrep...), or an error result written for the model.
        //   throws  — std::bad_alloc; what the catalog callback throws.
        HarnessToolResult Grep(const json::Value& arguments);
        // Grep's second half: the outcome as the JSON the model reads (the ordered, cut rows with their
        // excerpts; why it stopped early; the indexing note; unknown ids), the rows' short ids assigned
        // and their transcripts recorded. A cancelled call answers "cancelled" and records nothing.
        //   searched     — how many sessions the grep was given (the ids' or the period's).
        //   window       — what the period / id list was, in words.
        //   partial      — the catalog does not reach back to the period's start (coveredSince).
        //   unknown      — ids that named no session.
        //   facts        — title / activity / transcript of the sessions that matched.
        //   throws       — std::bad_alloc.
        HarnessToolResult FinishGrep(const SessionGrepParams& params, GrepOutcome outcome, size_t searched, const std::wstring& window, bool partial, int64_t coveredSince, const std::vector<std::wstring>& unknown, const std::unordered_map<std::wstring, KnownSession>& facts, int64_t now, const std::function<bool()>& cancelled);
        // The read tool: each id's transcript rendered at the asked level (chain and subagents
        // left out), in parallel, cut to the asked size (CutRenderedTranscript).
        //   returns — one text with a "## Session <id> ..." part per session, or an error result.
        //   throws  — std::bad_alloc.
        HarnessToolResult Read(const json::Value& arguments);
        // The read tool's description, schema and handler (both modes' servers carry it).
        //   throws — std::bad_alloc.
        HarnessTool MakeReadTool();
        HarnessToolResult Show(const json::Value& arguments);
        // The short id for `id`: its first 8 characters, or a longer prefix while another session
        // of this conversation already holds the shorter one, or the whole id. Recorded in _handles.
        // The caller holds _mutex.
        //   throws — std::bad_alloc.
        std::wstring AssignHandleLocked(const std::wstring& id);
        // Resolve what the model wrote to whole ids, asking the database (Callbacks::database.
        // resolve) for the ones no result of this conversation printed.
        //   written — the ids as the model wrote them.
        //   unknown — receives the ones that name no session (cut for a message), at most 5.
        //   returns — the whole ids, each once, in the model's order.
        //   throws  — std::bad_alloc; what database.resolve throws.
        std::vector<std::wstring> ResolveWrittenIds(const std::vector<std::wstring>& written, std::vector<std::wstring>& unknown);
        // The transcript of a whole id: what a result of this conversation recorded, else
        // Callbacks::transcriptPath, else ResolveClaudeTranscriptPath. "" = not found.
        //   throws — std::bad_alloc; what transcriptPath throws.
        std::wstring TranscriptPathOf(const std::wstring& sessionId) const;
        // One database tool call (Callbacks::database.call), with the page's reports: onSearching
        // before it; for `search`, the rows' short ids are resolved and recorded (show's ids, top:N,
        // onCandidates). A call whose request was replaced meanwhile answers "cancelled".
        //   tool      — a SessionDbToolDefs.h name.
        //   arguments — the model's argument object.
        //   returns   — the database's result (text + error flag), or the cancelled error.
        //   throws    — what database.call / database.resolve throw; std::bad_alloc.
        HarnessToolResult DatabaseCall(const std::string& tool, const json::Value& arguments);
        // Record handle -> full id pairs, index-aligned (an empty full id is skipped). Takes _mutex.
        //   throws — std::bad_alloc.
        void RememberHandles(const std::vector<std::wstring>& handles, const std::vector<std::wstring>& fullIds);
        int64_t Now() const;

        Callbacks _callbacks;
        mutable std::mutex _mutex; // guards the three members below
        std::unordered_map<std::wstring, std::wstring> _handles; // short id (lower case) -> full session id
        std::vector<std::wstring> _lastSearchIds; // the latest search's rows, full ids, result order
        std::unordered_map<std::wstring, KnownSession> _known; // full id -> what a search / grep row said about it (Reset clears it)
    };
}
