// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — SessionSearch: the Sessions page's two-phase search over the on-disk Claude
// sessions (SESSIONS.md §1/§6, answers Q1/Q2/Q6).
//
//   Phase 1 — FAST (ms, UI-debounced): in-memory match over the per-session sidecar INDEX
//   entries (titles · cwd · paths-accessed) plus the `history.jsonl` accelerator for the 👤
//   user scope (one global, prebuilt file of every typed prompt). Results render immediately.
//
//   Phase 2 — SLOW (background, cancellable): full transcript content. ripgrep — resolved from
//   PATH — does the heavy file-level filter (`rg -il` over the window's transcripts, batched
//   under the command-line length cap); each matched file is then re-scanned IN-PROCESS
//   (ScanTranscript → ClassifyTranscriptLine) so every hit is attributed to its scope (👤 user
//   text vs 🤖 agent/tool text) exactly — rg can't tell a prompt from a tool dump, our
//   classifier can. No rg on the machine ⇒ the same in-process scan runs over every window
//   file (slower, identical results).
//
//   Semantics: case-INsensitive always. The query parses into whitespace-split TERMS,
//   AND-combined (ParseSessionQuery — every term must match; each may hit a different field /
//   the same message): a "quoted phrase" is ONE term matched as an exact contiguous substring
//   (spaces kept, (F) never applies to it), and a bare whole-GUID token also matches the
//   session's IDENTITY (id + fork-parent id) — see the grammar block below. (F) fuzzy = a plain
//   term's non-space characters in order with anything between (`a.*?b.*?c` for rg; a
//   subsequence scan in-process — identical semantics). The working DIRECTORY (cwd) is ALWAYS a
//   match target; 🏷 (default ON) adds the session TITLE (custom/ai/summary/first-prompt + the
//   runtime liveTitle overlay = an open session's live tab title); 📁 matches the DIRECTORY part
//   of every tool-touched path; 📄 matches the LEAF (file name) of every tool-touched path. With
//   🏷 off and both message scopes off, terms match cwd (+ 📁/📄 paths) only (§1a). DIRECTORY
//   matching (cwd + 📁) is slash-INSENSITIVE — '/' and '\' are equivalent (MatchesPathQuery) — so
//   a path term finds a session regardless of which separator the user or the stored cwd uses.
//
// Plain C++ + Win32, no WinRT (PCH NotUsing; links into the standalone harness). The matchers /
// regex builder / snippet maker are PURE; only the rg invocation + history scan touch the OS.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "TranscriptStore.h" // SessionIndexEntry / TranscriptRef

namespace Agentmaster
{
    // The search bar's state: [ text ] (👤) (🤖) (📁) (📄) (F) — plus the window, which the
    // caller applies at enumeration time (it is a row filter, not a text-match input).
    struct SessionQuery
    {
        std::wstring text; // raw query — ParseSessionQuery's input; NO terms (empty/whitespace/"") => window-only listing
        bool scopeTitle{ true }; // 🏷 match the session TITLE (custom/ai/summary/first-prompt + the runtime liveTitle overlay). DEFAULT ON — a SessionQuery left unset reproduces the old always-on title baseline.
        bool scopeUser{}; // 👤 search user (typed) messages
        bool scopeAgent{}; // 🤖 search agent + tools (everything but user messages)
        bool scopeDirs{}; // 📁 match directories: the cwd + dirs of tool-touched paths
        bool scopeFiles{}; // 📄 match files: the leaves of tool-touched paths
        bool fuzzy{}; // (F) subsequence/fuzzy matching
    };

    // One per-session search result. `hitCount` aggregates matched messages (slow phase) or 1
    // for a metadata match (fast phase); `snippets` carry display-ready match context.
    struct SessionHit
    {
        std::wstring sessionId;
        int hitCount{};
        std::vector<std::wstring> snippets;
    };

    // ===== query grammar =====================================================================
    //
    // The search-bar text parses into TERMS, AND-combined: every term must match for a row /
    // line / message to hit, but each term may hit a DIFFERENT field in the fast phase
    // (order-free — the Archive page's token semantics). Three term shapes:
    //   word          — plain term: substring, or subsequence under (F) fuzzy.
    //   "any phrase"  — EXACT term: one contiguous substring, spaces kept, case-insensitive as
    //                   always; (F) NEVER applies to it. "" (empty) is dropped; an unterminated
    //                   quote runs to the end of the query; quotes open at a term boundary only.
    //   <guid>        — a bare whole-GUID token (8-4-4-4-12 hex, {braces} tolerated) ALSO
    //                   matches the session's IDENTITY: its id and its fork-parent id (paste an
    //                   id from hooks.log / the detail pane → that session + its forks). It
    //                   still matches as literal text too (additive — never worse); QUOTING a
    //                   guid makes it a pure text term. Never fuzzy (a 36-char hex subsequence
    //                   would match almost any hex-ish text).
    // The content phases (history / transcripts) need at least one NON-guid term; a guid term
    // there scopes hits to that session (satisfied by the line's / file's own session id).

    // One parsed search-bar term.
    struct SearchTerm
    {
        std::wstring text; // term text, original case, quotes/braces stripped — feeds the rg regex
        std::wstring textLower; // pre-folded for the in-process matchers
        bool exact{}; // "quoted": contiguous substring even under (F)
        bool isGuid{}; // bare whole-GUID token: also matches the session identity
    };

    // True iff `token` is a whole GUID: 8-4-4-4-12 hex with optional surrounding {braces}. Pure.
    bool IsGuidToken(std::wstring_view token);

    // Parse the raw search-bar text into terms per the grammar above. Empty / whitespace-only /
    // ""-only input => no terms (callers treat that as "no text filter"). Pure.
    std::vector<SearchTerm> ParseSessionQuery(std::wstring_view text);

    // Per-term effective fuzziness: (F) applies to PLAIN terms only — exact ("quoted") and guid
    // terms always match contiguously. Both the rg regex builder and the in-process matcher key
    // on this, keeping the pair contract per term.
    inline bool TermIsFuzzy(const SearchTerm& t, bool queryFuzzy) noexcept
    {
        return queryFuzzy && !t.exact && !t.isGuid;
    }

    // ===== PURE primitives ===================================================================

    // Lowercase fold (towlower per char) — both matchers expect pre-folded inputs.
    std::wstring FoldLower(std::wstring_view s);

    // The rg pattern for a query: regex specials escaped; fuzzy joins the query's non-space
    // characters with a lazy gap (`.*?`). Pairs with `rg -i` (case-insensitivity lives there).
    std::wstring BuildSearchRegex(std::wstring_view text, bool fuzzy);

    // The in-process equivalent of the rg match: substring (non-fuzzy) or in-order subsequence
    // (fuzzy) over PRE-FOLDED haystack/needle. Empty needle matches.
    bool MatchesQueryText(std::wstring_view textLower, std::wstring_view queryLower, bool fuzzy);

    // Slash-INSENSITIVE variant for DIRECTORY / path haystacks (the cwd + the 📁 dir-part of
    // tool-touched paths): '/' and '\' are treated as the same separator — both sides are folded
    // to '/' before the usual MatchesQueryText — so a "src/foo" query finds a Windows "src\foo"
    // cwd and vice versa. Substring/subsequence semantics are otherwise identical. A needle with
    // NO separator cannot be affected by the folding (the separator chars surround, never form,
    // the match), so the call then defers straight to MatchesQueryText — the common plain-word
    // case stays allocation-free. Inputs PRE-folded like MatchesQueryText. Pure.
    bool MatchesPathQuery(std::wstring_view textLower, std::wstring_view queryLower, bool fuzzy);

    // A display snippet around the first match: ~40 chars of left context + the match + right
    // context, single-line-collapsed, capped at maxChars. Fuzzy (no contiguous match position)
    // returns the collapsed head. `text` is the ORIGINAL (unfolded) string.
    std::wstring MakeSnippet(std::wstring_view text, std::wstring_view queryLower, bool fuzzy, size_t maxChars);

    // Phase-1 fast match over the sidecar index entries. No terms => every entry (the
    // window-only listing). EVERY term must match (AND), each against any haystack: cwd ALWAYS
    // applies; the title fields (custom/ai/summary/first-prompt + the runtime liveTitle overlay)
    // apply under q.scopeTitle (DEFAULT ON — the cached first prompt rides there, so the
    // both-scopes-OFF baseline is title + directory; the full user scope is the history
    // accelerator + phase 2); 📁/📄 add the dir/leaf parts of the tool-touched paths; and a GUID
    // term matches the entry's sessionId or fork-parent id outright. Returns matching sids in
    // input order.
    std::vector<std::wstring> SearchIndexFast(const std::vector<SessionIndexEntry>& entries, const SessionQuery& q);

    // ===== OS-touching =======================================================================

    // The resolved `rg.exe` (PATH search, cached), empty when ripgrep is unavailable —
    // every consumer must fall back to the in-process scan.
    const std::wstring& ResolveRipgrep();

    // The 👤 accelerator: match every typed prompt in `history.jsonl` ({display, sessionId,…}
    // per line — global, prebuilt, never swept) and aggregate per session. A line hits when
    // ALL terms match its display text — a guid term is alternatively satisfied by the line's
    // own sessionId (so "<guid> word" = search "word" within that session); a guid-ONLY query
    // returns nothing (identity matching is the fast phase's job). rg does the line filter
    // when available (on the longest text term — one pattern can't AND several; every
    // candidate line is still verified in-process); the in-process line scan otherwise.
    // Sessions older than the window simply won't be in the caller's row set — over-returning
    // is harmless. Caveats (SESSIONS.md §6.4): ancient lines without a sessionId are skipped;
    // paste bodies live in paste-cache and are not searched.
    std::unordered_map<std::wstring, SessionHit> SearchHistoryPrompts(const std::wstring& historyPath, const SessionQuery& q, size_t maxSnippetsPerSession);

    // Phase 2: full content search over the window's transcripts. A MESSAGE hits when ALL
    // terms match its text — a guid term is alternatively satisfied by the file's own session
    // id (so "<guid> word" = search "word" within that session); a guid-ONLY query returns
    // nothing (identity matching is the fast phase's job). `rg -il` (batched, one round per
    // text term, path lists intersected) filters to files raw-containing every text term;
    // each survivor is re-scanned in-process and every hit attributed to its scope via
    // ClassifyTranscriptLine (👤 = REAL prompt text, 🤖 = assistant text/thinking + tool
    // inputs/results + system content). Without rg, every ref is scanned. Requires at least
    // one message scope ON (title/dir/file matching is phase 1's job). `cancelled` is polled
    // between files — the UI cancels a stale search on re-type.
    std::vector<SessionHit> SearchTranscriptsSlow(const std::vector<TranscriptRef>& refs, const SessionQuery& q, size_t maxSnippetsPerSession, const std::function<bool()>& cancelled);
}
