// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
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
// Plus GrepTranscripts — not a page phase but the AI search's evidence finder (its `grep` tool):
// WHERE a pattern occurs, with context, who wrote it and when (the grep section below).
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

    // ===== grep: WHERE a pattern occurs (the AI search's evidence, SESSIONS.md §9.12) =========
    //
    // SearchTranscriptsSlow answers WHICH sessions hold some words; GrepTranscripts answers WHERE:
    // per session, how many messages match and the first few matches in their context, with whose
    // words they are (the user's or Claude's) and when they were written. The Sessions AI search's
    // `grep` tool runs it (SessionSearchTools.cpp) to find evidence that a session is the one the
    // user asks about: words the user remembers, an error text, a version, a file name.
    //
    // What it reads is the content search's text (ClassifyTranscriptLine): the user's REAL typed
    // prompts on one side; Claude's text and thinking, every tool call (its name and its input as
    // JSON), every tool result and the system lines on the other — each message cut to
    // kGrepTextBudgetChars. Not read: a message the user typed while Claude worked (Claude Code
    // records it as a queued_command attachment, which the content search does not read either),
    // and the subagents' own transcripts.
    //
    // Matching is case-insensitive. A literal pattern matches as one contiguous piece of text. A
    // regex is ECMAScript (std::wregex, icase): `.` does not cross a line break and `[\s\S]` does;
    // `^` and `$` are the start and the end of one message's text.
    //
    // Speed: ripgrep first drops the files that cannot match, and the reader then parses only the
    // lines that hold a REQUIRED LITERAL of the pattern (GrepRequiredLiteral). A pattern with no
    // such literal (a regex like `\d+`) skips both, so every candidate is parsed line by line —
    // the deep search's cost. A time budget bounds a call; the outcome says when it ran out.

    // How much of one message's text a grep reads, in UTF-16 units: the head of a long tool result
    // or file read. 16K was measured cheap for std::wregex on MSVC (a 64K-character message took
    // under 31 ms for every probe pattern, with no stack-depth error), and it is four times the
    // content search's per-message head.
    inline constexpr size_t kGrepTextBudgetChars = 16384;

    // At most this much of a match is quoted in an excerpt (a `[\s\S]*` regex can match a whole
    // message): the excerpt then ends with "…" inside the match.
    inline constexpr size_t kGrepMaxQuotedMatchChars = 200;

    // What a grep looks for, and how much it brings back.
    struct GrepOptions
    {
        std::wstring pattern; // the text (or regex) to find; must not be empty
        bool regex{}; // false: `pattern` is literal text; true: an ECMAScript regex (RegexUtil.h's caps: 512 characters)
        bool user{ true }; // read the user's typed prompts
        bool agent{ true }; // read Claude's side: its text, thinking, tool calls and results, the system lines
        size_t contextChars{ 120 }; // characters kept on each side of a match in an excerpt
        size_t maxExcerpts{ 3 }; // excerpts kept per session (every match still counts)
        // 0 = no time limit. Else stop reading files this many milliseconds after the call
        // started: the outcome then has complete == false and the sessions read so far.
        int64_t budgetMs{};
    };

    // One match, as the model reads it.
    struct GrepExcerpt
    {
        bool user{}; // true: in a prompt the user typed; false: on Claude's side
        int64_t timestampMs{}; // the message's own time; 0 when its line carries none
        std::wstring text; // one line: the match with its context, "…" where the message goes on
    };

    // One session's matches.
    struct GrepSessionHit
    {
        std::wstring sessionId;
        int hits{}; // matching messages (each message counts once, however often it matches)
        int userHits{}; // of those, prompts the user typed
        std::vector<GrepExcerpt> excerpts; // the first GrepOptions::maxExcerpts matches, in file order
    };

    // What a grep found, and how much of the work it did.
    struct GrepOutcome
    {
        std::vector<GrepSessionHit> sessions; // the sessions with at least one match, in the order of the refs
        size_t candidates{}; // refs left after ripgrep's file filter (all of them without one), of the batches it reached
        size_t scanned{}; // of those, read to their end (a file deleted since the listing is not)
        size_t notReached{}; // refs of the batches it never got to (neither filtered nor read) when it stopped early
        bool complete{ true }; // false: the budget ran out, or the caller cancelled, before every candidate was read
        bool prefiltered{}; // ripgrep filtered the files (on the required literal)
        int tooComplex{}; // messages where the regex gave up (std::regex_error: complexity / stack); they count as no match
        std::wstring error; // non-empty: the pattern cannot be used (empty, too long, or not a valid regex); nothing was read
    };

    // The longest piece of text that EVERY match of `pattern` contains and that a transcript's raw
    // JSON line holds verbatim whenever a message's decoded text does: no quote, no backslash, no
    // control character, no unpaired surrogate (the characters JSON escapes). The prefilter key of
    // ripgrep and of the line reader. Empty when there is none — a regex whose matches share no
    // literal (`\d+`, `a|b`), or a construct this reader does not follow; the grep then reads
    // every candidate in full, which is slow but never misses a match.
    //   pattern — the grep's pattern.
    //   regex   — true: an ECMAScript regex. Only top-level plain characters count: a group, a
    //             class, `.`, an anchor or a class escape ends a run; a quantifier that allows
    //             zero ends it before its character; a top-level `|` gives no literal at all.
    //   returns — the literal (original case), or empty.
    //   throws  — std::bad_alloc.
    std::wstring GrepRequiredLiteral(std::wstring_view pattern, bool regex);

    // One excerpt: `contextChars` of `text` on each side of the match at [matchPos, matchPos +
    // matchLen), whitespace collapsed to single spaces, "…" where `text` goes on; a match longer
    // than kGrepMaxQuotedMatchChars is quoted up to that length. Never splits a surrogate pair.
    // Pure.
    //   text      — the message's text (original case).
    //   matchPos  — where the match starts (clamped to the text).
    //   matchLen  — its length.
    //   throws    — std::bad_alloc.
    std::wstring MakeGrepExcerpt(std::wstring_view text, size_t matchPos, size_t matchLen, size_t contextChars);

    // Grep the transcripts of `refs`, in their order (a caller that wants the newest sessions
    // first when the budget runs out passes them newest first).
    //   refs      — the transcripts to read.
    //   options   — the pattern and the limits.
    //   cancelled — empty = never; else asked between files and every megabyte read: true ends
    //               the grep with what it found (complete == false).
    //   returns   — the outcome; `error` set (and nothing read) for an unusable pattern.
    //   throws    — std::bad_alloc. A file that cannot be opened is skipped (it was deleted after
    //               the listing); ripgrep that cannot start, times out or fails is replaced by
    //               reading every file of its batch.
    GrepOutcome GrepTranscripts(const std::vector<TranscriptRef>& refs, const GrepOptions& options, const std::function<bool()>& cancelled);
}
