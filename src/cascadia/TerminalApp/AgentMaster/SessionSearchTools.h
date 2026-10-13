// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionSearchTools: what the model can DO in the Sessions browser's AI search
// (SESSIONS.md §9, SDK_HARNESS.md §7).
//
// The AI search is a hosted conversation (AgentHarness.h) with two tool servers:
//
//   sessions.search  — "which sessions match?"  Runs the Sessions page's own search (the sidecar
//                      index, the history.jsonl accelerator and, on request, the transcript text
//                      scan) over a CATALOG of sessions, with the page's filters as arguments.
//   display.show     — "put these on the screen." Hands an ordered list of session ids to the
//                      Sessions page, which shows exactly those rows in that order.
//
// The model understands the request, calls search (once, or a few times with other words), picks
// what fits, calls show, and writes one sentence. The split is the point: the search tool never
// changes what the user sees, so the model can look around freely, and the show tool is the ONE
// place where the screen changes.
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

    // ===== the tool servers =================================================================

    // The two tool servers of the Sessions AI search, and the state they share between calls of
    // one conversation: which short id means which session, and the rows of the latest search
    // (so `show` can take "the first N of that").
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
            // caption. Required: it is what changes the screen.
            std::function<void(const std::vector<std::wstring>& sessionIds, const std::wstring& note)> onShow;
            SessionContentSearch content; // empty members = DefaultSessionContentSearch()
            std::function<int64_t()> now; // empty = the system clock
        };

        //   throws — std::invalid_argument when a required callback is missing; std::bad_alloc.
        explicit SessionSearchToolset(Callbacks callbacks);

        // The servers to put into HarnessOptions::toolServers: "sessions" (search) and "display"
        // (show). The handlers refer to this object: it must outlive the conversation's process.
        //   throws — std::bad_alloc.
        std::vector<HarnessToolServer> Servers();

        // Forget the short ids and the latest search (a conversation reset).
        void Reset();

        // The full session id a short id of this conversation stands for, or empty.
        //   throws — std::bad_alloc.
        std::wstring ResolveHandle(const std::wstring& handle) const;

        // The system prompt of the Sessions AI search: the role, the two tools, and eight rules.
        // Rule 1 makes the model's FIRST step a short mention of which kinds of tool calls it can use
        // for the request, out of everything the tools can do, and how it will use them in different
        // ways — written in the same message as its first calls, so it costs output tokens in that
        // step but no extra model request. The other rules keep it fast: act at once, one show call,
        // one sentence. ASCII (it travels through a file and the API unchanged).
        //   returns — a static, NUL-terminated string; never null.
        // ⚠ Its text is part of every request's prompt and of the prompt cache's key: a change costs a
        //   new cache entry, and the m5 suite pins rule 1 and the count of rules.
        static const wchar_t* SystemPrompt() noexcept;

        // The servers' and tools' names (the model calls mcp__sessions__search and mcp__display__show).
        static constexpr wchar_t kSearchServer[] = L"sessions";
        static constexpr wchar_t kSearchTool[] = L"search";
        static constexpr wchar_t kDisplayServer[] = L"display";
        static constexpr wchar_t kDisplayTool[] = L"show";

    private:
        HarnessToolResult Search(const json::Value& arguments);
        HarnessToolResult Show(const json::Value& arguments);
        int64_t Now() const;

        Callbacks _callbacks;
        mutable std::mutex _mutex; // guards the two members below
        std::unordered_map<std::wstring, std::wstring> _handles; // short id (lower case) -> full session id
        std::vector<std::wstring> _lastSearchIds; // the latest search's rows, full ids, result order
    };
}
