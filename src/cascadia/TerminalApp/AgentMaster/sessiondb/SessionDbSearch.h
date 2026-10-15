// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's SEARCH (SESSIONS_DB.md §6): one request in, the matching
// sessions out, ranked in tiers, with hit counts and snippets.
//
// Semantics kept from today's Sessions page (§6.1, SessionSearch.h):
//   - the query parses into whitespace-split AND terms (ParseSessionQuery): "quoted phrase" = one
//     exact term, a whole session id also matches the session's identity (its id or fork parent);
//   - SESSION-level AND for the names and places (T0 / T1): every term must hit SOME field — a title,
//     the id, the branch, the working folder (always), a touched folder (📁) or file name (📄) — and
//     the fields are matched exactly as today's fast phase matches them (FoldLower + MatchesQueryText /
//     MatchesPathQuery), so a session the page finds by name today is found here;
//   - MESSAGE-level AND for the content (T2 prompts, T3 everything else): a block hits when every text
//     term matches THAT block; a whole-id term then only scopes the hits to that session;
//   - the result is the union, ranked Name (0) > Path (1) > Prompt (2) > Text (3), then by the
//     requested sort (BM25 inside the content tiers for "relevance").
//
// The content tiers: T2 scans the small pre-folded prompt tier (+ history.jsonl's prompts); T3 runs the
// index plan of §6.3 (rule 1: a term without separators is answered exactly by the dictionary terms that
// contain it; rule 2: a term with separators is narrowed by its tokens and verified on the candidates'
// text) and scans the hot tier (blocks written since the last merge). D8: terms of 1-2 characters skip
// T3 (they would expand to most of the dictionary). D9: fuzzy (F) over T3 expands to the dictionary terms
// that hold the term's characters in order; T2 and the hot tier keep today's subsequence rule.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Agentmaster::SessionDb
{
    class Connection;
    class Store;

    // One session as the search, the list and the tools see it (cached; rebuilt after a write).
    struct SessionCard
    {
        int32_t sidKey{ 0 };
        std::wstring sessionId; // lower-case UUID
        std::wstring shortId;
        std::wstring title; // the display title: stored > custom > AI > legacy summary > first prompt
        std::wstring storedTitle, customTitle, aiTitle, legacySummary, firstPrompt;
        std::wstring cwd, cwdKey, inferredDir, branch, forkedFrom, model;
        int64_t createdMs{ 0 }, activeMs{ 0 }, lastTypedMs{ 0 }, lastTurnEndMs{ 0 };
        int32_t msgs{ 0 }, tools{ 0 };
        int64_t bytes{ 0 }, contextTokens{ 0 };
        bool favorite{ false };
        bool present{ true }; // the transcript is still on disk (D7)
        std::vector<std::wstring> tags;
        // Folded haystacks (FoldLower — today's fast-phase fold, so T0 / T1 match what the page matches).
        std::wstring idF, forkF, cwdF, branchF;
        std::wstring titleF[5]; // stored, custom, AI, legacy summary, first prompt
        std::vector<std::wstring> dirsF, leavesF; // distinct directory parts / file names of touched paths
    };

    // The cached card set (immutable once published).
    struct CardSet
    {
        std::vector<SessionCard> cards;
        std::unordered_map<int32_t, size_t> bySid;
        int64_t version{ -1 }; // the store's write stamp it was built at
    };

    // A search request: the Sessions page's parameters and the tools' (§8.4.3 `search`), merged.
    struct SearchRequest
    {
        std::wstring query; // the raw query text (ParseSessionQuery's input); empty = list by the filters only
        bool scopeTitle{ true }; // 🏷 names: titles, the id, the branch
        bool scopeDirs{ true }; // 📁 touched folders (the working folder is always a match target)
        bool scopeFiles{ true }; // 📄 touched file names
        bool scopePrompt{ true }; // 👤 typed prompts (and messages typed while Claude worked, history.jsonl)
        bool scopeReply{ true }; // 🤖 Claude's replies, thinking and system text
        bool scopeTool{ false }; // 🤖 tool calls and their output (broad)
        bool scopeSubagent{ false }; // also read subagents' transcripts (the roles chosen above, there too)
        bool fuzzy{ false }; // (F)
        int64_t afterMs{ 0 }, beforeMs{ 0 }; // last-activity window (0 = unbounded)
        std::wstring dir, branch, tag; // row filters: folder part, branch part, a whole tag (any case)
        int favorite{ -1 }, open{ -1 }; // -1 any, 0 no, 1 yes
        enum class Sort
        {
            Relevance, // tiers, then BM25 (content) / activity
            Active, // newest activity first
            Created, // newest first
        } sort{ Sort::Relevance };
        size_t offset{ 0 }, limit{ 15 };
        size_t snippetsPerRow{ 2 };
        size_t snippetChars{ 160 };
        // The time this search may spend reading TOOL text (verification and snippets of tool calls / results).
        // That text is ZSTD (doc_tool): each scattered block costs a vector's decompression (~2-5 ms), so a
        // broad tool-scope search verifies the best-scored candidates until the budget is spent and reports
        // `partial`. Conversation text has no budget (its reads are ~0.05 ms per block).
        int64_t toolTextBudgetMs{ 600 };
        std::function<bool()> cancel; // polled between the steps
    };

    struct SearchSnippet
    {
        int32_t fileId{ 0 }; // the block's source file (0 for history.jsonl); a subagent's file names its agent
        int32_t lineNo{ 0 }; // the block's line in its file (0 for history.jsonl / spills)
        uint16_t block{ 0 };
        uint8_t role{ 0 };
        bool subagent{ false };
        bool history{ false }; // from ~/.claude/history.jsonl (no ref)
        std::wstring text;
    };

    struct SearchRow
    {
        int32_t sidKey{ 0 };
        uint8_t tier{ 4 }; // 0 name, 1 path, 2 prompt, 3 text, 4 listed (no query)
        int64_t hits{ 0 }; // matched blocks (content tiers)
        double score{ 0 }; // BM25 (content tiers)
        std::vector<SearchSnippet> snippets;
    };

    struct SearchResult
    {
        std::vector<SearchRow> rows; // the requested page
        int64_t searched{ 0 }; // sessions inside the window and filters
        int64_t matched{ 0 }; // sessions matching the query
        bool partial{ false }; // a cap or the budget cut the content tiers short
        bool hasMore{ false }; // rows beyond this page
        std::wstring note; // a remark for the caller ("1-2 character terms ...")
        std::wstring timings; // the steps' times ("cards 1.2 ms, names 0.3 ms, ..."), for the service log and diagnostics
        std::shared_ptr<const CardSet> cards; // the card set the rows index into
    };

    // The tool texts read recently, decompressed. doc_tool is ZSTD, so a scattered read costs ~2-5 ms; typing a
    // query letter by letter re-reads mostly the same snippet blocks, which this cache answers. A doc id's text
    // never changes (a replaced transcript gets new ids), so entries leave only to make room. Thread-safe.
    class DocTextCache
    {
    public:
        // budgetBytes — the most text (UTF-16 bytes) kept; the least recently used entries leave first.
        explicit DocTextCache(size_t budgetBytes);
        // Copy the cached text of `docId` into `text` and mark it used. Returns false when it is not cached.
        bool Get(uint32_t docId, std::wstring& text);
        // Cache a text (a text larger than a quarter of the budget is not kept).
        void Put(uint32_t docId, const std::wstring& text);

    private:
        std::mutex _mtx;
        size_t _budget;
        size_t _bytes{ 0 };
        std::list<std::pair<uint32_t, std::wstring>> _lru; // most recent first
        std::unordered_map<uint32_t, std::list<std::pair<uint32_t, std::wstring>>::iterator> _at;
    };

    // One block as the search's snippets and verification, and the `messages` / `grep` tools, read it.
    struct BlockText
    {
        int32_t fileId{ 0 }; // the block's source file (a subagent's transcript names its agent)
        int32_t line{ 0 }; // its line in that file (1-based)
        uint16_t block{ 0 }; // its index among the line's blocks
        uint8_t role{ 0 }; // BlockRole (SessionDbExtract.h)
        uint8_t flags{ 0 }; // DocFlag bits
        int64_t tsMs{ 0 }; // the line's timestamp (0 = none)
        std::wstring tool; // a tool block's tool name
        std::wstring text; // the original text (not folded)
    };

    // Read the blocks named by `ids` and hand each one whose text was read to `fn`.
    //   ids      — doc ids, best first (duplicates allowed): when the deadline cuts the tool texts short, the
    //              best candidates were read. Fetched in chunks of at most 1,000, so each chunk is an index
    //              scan (DuckDB uses doc_by_id only below MAX(2,048 rows, 0.1% of the table)).
    //   withHot  — also look in doc_hot (blocks not merged yet; a small table, scanned).
    //   cache    — the tool-text cache: doc_tool reads go through it.
    //   deadline — tool texts are read from doc_tool (ZSTD: ~2-5 ms per scattered block) until this time; the
    //              tool blocks left are not handed out. Conversation text (doc.text, doc_hot.text) is always
    //              read: ~0.05 ms per block.
    //   fn       — called once per block read (in no particular order); it may move the text out.
    // Returns false when the deadline left tool blocks unread. Throws DbError.
    bool ReadBlockTexts(Connection& con, const std::vector<int64_t>& ids, bool withHot, DocTextCache& cache,
                        std::chrono::steady_clock::time_point deadline, const std::function<void(uint32_t, BlockText&)>& fn);

    // The search engine: the card cache and the planner. Thread-safe (each call brings its own connection).
    class SearchEngine
    {
    public:
        explicit SearchEngine(Store& store);
        // The current card set (rebuilt when the store wrote since the last build). Throws DbError.
        std::shared_ptr<const CardSet> Cards(Connection& con);
        // Forget the cached cards (Agentmaster's facts changed: titles, tags, favorites).
        void Invalidate();
        // Run one search. Throws DbError (an interrupted query reports "INTERRUPT").
        SearchResult Search(Connection& con, const SearchRequest& req);
        // The tool-text cache (the `messages` / `grep` tools read through it too).
        DocTextCache& ToolTexts()
        {
            return _toolTexts;
        }

    private:
        Store& _store;
        std::mutex _mtx;
        std::shared_ptr<const CardSet> _cards;
        int64_t _builtAt{ -1 };
        bool _invalid{ true };
        DocTextCache _toolTexts{ 32u * 1024u * 1024u };
    };

    // The display title of a card's fields (PickDisplayTitle's order with Agentmaster's title first).
    std::wstring CardTitle(const SessionCard& c);
}
