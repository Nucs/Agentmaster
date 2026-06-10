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
//   Semantics: case-INsensitive always. (F) fuzzy = the query's non-space characters in order
//   with anything between (`a.*?b.*?c` for rg; a subsequence scan in-process — identical
//   semantics). Both message scopes OFF ⇒ the query matches title + directory only (§1a);
//   📁 matches the session cwd + the DIRECTORY part of every tool-touched path; 📄 matches the
//   LEAF (file name) of every tool-touched path.
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
        std::wstring text; // raw query; empty => window-only listing (no text filter)
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

    // ===== PURE primitives ===================================================================

    // Lowercase fold (towlower per char) — both matchers expect pre-folded inputs.
    std::wstring FoldLower(std::wstring_view s);

    // The rg pattern for a query: regex specials escaped; fuzzy joins the query's non-space
    // characters with a lazy gap (`.*?`). Pairs with `rg -i` (case-insensitivity lives there).
    std::wstring BuildSearchRegex(std::wstring_view text, bool fuzzy);

    // The in-process equivalent of the rg match: substring (non-fuzzy) or in-order subsequence
    // (fuzzy) over PRE-FOLDED haystack/needle. Empty needle matches.
    bool MatchesQueryText(std::wstring_view textLower, std::wstring_view queryLower, bool fuzzy);

    // A display snippet around the first match: ~40 chars of left context + the match + right
    // context, single-line-collapsed, capped at maxChars. Fuzzy (no contiguous match position)
    // returns the collapsed head. `text` is the ORIGINAL (unfolded) string.
    std::wstring MakeSnippet(std::wstring_view text, std::wstring_view queryLower, bool fuzzy, size_t maxChars);

    // Phase-1 fast match over the sidecar index entries. Empty text => every entry (the
    // window-only listing). Haystacks per the toggle semantics above; a title/dir match always
    // applies (the both-scopes-OFF baseline); 👤 additionally tries the cached first prompt
    // (the full user scope is the history accelerator + phase 2). Returns matching sids in
    // input order.
    std::vector<std::wstring> SearchIndexFast(const std::vector<SessionIndexEntry>& entries, const SessionQuery& q);

    // ===== OS-touching =======================================================================

    // The resolved `rg.exe` (PATH search, cached), empty when ripgrep is unavailable —
    // every consumer must fall back to the in-process scan.
    const std::wstring& ResolveRipgrep();

    // The 👤 accelerator: match every typed prompt in `history.jsonl` ({display, sessionId,…}
    // per line — global, prebuilt, never swept) and aggregate per session. rg does the line
    // filter when available; the in-process line scan otherwise. Sessions older than the
    // window simply won't be in the caller's row set — over-returning is harmless. Caveats
    // (SESSIONS.md §6.4): ancient lines without a sessionId are skipped; paste bodies live in
    // paste-cache and are not searched.
    std::unordered_map<std::wstring, SessionHit> SearchHistoryPrompts(const std::wstring& historyPath, const SessionQuery& q, size_t maxSnippetsPerSession);

    // Phase 2: full content search over the window's transcripts. `rg -il` (batched) filters
    // to files with at least one raw match; each survivor is re-scanned in-process and every
    // hit attributed to its scope via ClassifyTranscriptLine (👤 = REAL prompt text, 🤖 =
    // assistant text/thinking + tool inputs/results + system content). Without rg, every ref
    // is scanned. Requires at least one message scope ON (title/dir/file matching is phase 1's
    // job). `cancelled` is polled between files — the UI cancels a stale search on re-type.
    std::vector<SessionHit> SearchTranscriptsSlow(const std::vector<TranscriptRef>& refs, const SessionQuery& q, size_t maxSnippetsPerSession, const std::function<bool()>& cancelled);
}
