// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the SUMMARY-PANEL JUMP resolver (see doc/agentmaster/SUMMARY_JUMP.md).
//
// The per-tab summary panel (AgentTabOverlay) numbers the conversation's user prompts. A "jump"
// affordance on each one should scroll the session's TERMINAL VIEW to where that prompt is rendered
// (and, by neighbor-derivation, to a turn's end). But the summary text comes from the TRANSCRIPT
// (.jsonl) while "scroll the view" means moving the rendered ConPTY text BUFFER — two different
// representations with NO shared coordinate system. Claude Code's Ink TUI also repaints/scrolls/
// reflows constantly, so any resolved location ages, and the SAME prompt may be sent (and rendered)
// more than once.
//
// This unit is the PURE bridge between the two: given the linearized terminal buffer (the "haystack")
// and the conversation's prompt texts, it produces, for each prompt, the best buffer location to jump
// to. It is deliberately free of WinRT / TextBuffer / ICU so it can be unit-tested AND benchmarked
// standalone (AgentMaster/tests), and HEADER-ONLY (like ProfileBootstrap.h / Updater.h) so it can be
// shared by the TerminalApp overlay AND ControlCore (a separate Microsoft.Terminal.Control DLL) with
// no cross-project source/link. The buffer/control layer is a THIN adapter: it linearizes the
// TextBuffer into a haystack (rows concatenated; a hard line-break contributes one '\n', a soft wrap
// contributes nothing — so a wrapped sentence stays continuous, matching TextBuffer::SearchText's
// own haystack) + a parallel offset->row index, calls ResolvePromptAnchors once, then maps the
// returned haystack offsets back to buffer rows for a centered scroll + flash highlight.
//
// Design (SUMMARY_JUMP.md §2-§4):
//  - NEEDLE: the first non-trivial line of the prompt, whitespace-collapsed + ASCII-lowercased, up to
//    maxNeedle chars. Matching is whitespace-tolerant (a wrap or a re-indent doesn't defeat it) and
//    case-insensitive, and naturally skips a render prefix like "> " (it's a substring search).
//  - PARTIAL / "match as much as possible": after a needle hits, the match is EXTENDED character by
//    character against the whole (normalized) prompt; quality = matched-fraction. A render that
//    truncates/reflows the prompt still resolves, at quality < 1 (flagged `partial`). If the full
//    needle is absent, it BACKS OFF to shorter prefixes (down to minNeedle) before giving up.
//  - DUPLICATES: all prompts are resolved together in ONE pass with an ORDER-PRESERVING greedy
//    assignment — prompt order == buffer order — so the i-th send of a repeated text maps to the i-th
//    surviving on-screen occurrence (a send scrolled off the top is simply gone; the rest still line
//    up). When no in-order candidate exists, it falls back to the LAST (most-recent) global occurrence,
//    flagged `outOfOrder` (lower confidence) rather than guessing silently.
//  - STALENESS / PERF: the caller gates work by buffer mutation-id (unchanged => cached spans are
//    bit-for-bit valid, zero work). When the id moved, ValidatePromptAnchor is the CHEAP per-click
//    re-check at the cached offset (O(needle)); a full ResolvePromptAnchors (O(haystack)) runs only on
//    a validate miss or a gated idle refresh — never per render frame. The dominant cost is normalizing
//    the haystack (index-written, not push_back'd); a true-absence membership pre-check short-circuits a
//    "scrolled-off" prompt in one scan; a LAZY FLOOR-HIT INDEX (detail::FloorHitIndex) caps the miss
//    CASCADE — a floor-present prompt whose longer prefixes are absent pays ~2 full scans instead of one
//    per backoff length x probe family (the release-0.6.8 UI-freeze unit cost, tests_summary_anchor's
//    cascade bench); and the adapter caps the haystack to a recent window
//    (kAnchorRecentWindowChars) so cost is bounded regardless of total scrollback. See SUMMARY_JUMP.md §4.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // The adapter caps the linearized haystack to (at most) this many trailing chars — the recent
    // scrollback. A prompt older than this has scrolled out of practical reach anyway; capping keeps a
    // batch resolve bounded (~tens of ms) no matter how deep the buffer is. ~2 MB of wchar text covers
    // a typical full scrollback (WT default ~9k lines). Tunable; not a hard correctness limit.
    inline constexpr size_t kAnchorRecentWindowChars = 1'200'000;

    // Agentmaster (SUMMARY_JUMP.md §5): the glyph(s) Claude Code renders at the START of a SENT user
    // prompt's line in its TUI. U+276F (the heavy right-angle prompt ornament) is the primary marker;
    // U+203A (single right-angle quote) a secondary variant. Both are rare + specific -- unlike plain '>',
    // which pervades markdown quotes, shell prompts, redirections and diffs and would false-positive -- so
    // a prompt-text match that sits right after one of these is almost certainly the REAL user-prompt
    // render, not an assistant echo of the same words. The ControlCore adapter passes this as
    // AnchorOptions::promptMarkers; PromptAnchor.h's own default is empty (no enforcement), so the pure
    // resolver + its existing tests are unchanged unless a caller opts in. Plain '>' is deliberately
    // EXCLUDED (it is the synthetic marker the unit tests use, precisely because it is too common to
    // validate against in a real buffer). NOTE: \u escapes, not raw glyphs -- this TU compiles without
    // /utf-8, so a raw multibyte literal would mojibake-decode (the same reason the tests build non-ASCII
    // text from code points).
    inline constexpr std::wstring_view kClaudePromptMarkers = L"\u276F\u203A";

    // Tunables for the resolver. Defaults are the shipping values (SUMMARY_JUMP.md §2/§3).
    struct AnchorOptions
    {
        size_t maxNeedle = 64; // chars of the prompt's first non-trivial line used as the primary needle
        size_t minNeedle = 8; // backoff floor; never search a needle shorter than this (avoid matching "the")
        int backoffSteps = 4; // max needle-shrink attempts (full, then halving, down to minNeedle)
        double partialThreshold = 0.85; // quality below this flags the match `partial`

        // Agentmaster (SUMMARY_JUMP.md §5): prompt-marker VALIDATION. When non-empty, a forward candidate
        // occurrence is accepted only if one of these marker chars appears within `markerLookback` chars
        // immediately before the match start (in the normalized haystack) — so a match binds to a real
        // user-prompt render (which Claude prefixes with the marker) and not to an incidental echo of the
        // same text in assistant output / a tool result / a diff. Empty => legacy, marker-agnostic matching.
        // SAFETY: if markers are set but NONE occur anywhere in the haystack (a Claude build/theme that
        // renders prompts without the glyph), enforcement auto-disables for that resolve (never regresses).
        std::wstring promptMarkers = {}; // e.g. kClaudePromptMarkers; empty => off
        size_t markerLookback = 4; // normalized chars before a match to scan for a marker (covers the U+276F glyph + a space and a box/indent char)
    };

    // The resolved buffer location for one prompt. Offsets are into the SAME haystack passed in
    // (raw, NOT normalized) so the caller can map them straight back to buffer rows/columns.
    struct AnchorMatch
    {
        bool found = false; // a location was resolved (possibly partial / out-of-order)
        size_t offset = 0; // start of the matched region, as a char index into the input haystack
        size_t length = 0; // length of the matched region (the part that matched), in haystack chars
        double quality = 0.0; // 0..1: fraction of the normalized prompt that matched contiguously from `offset`
        bool partial = false; // quality < AnchorOptions::partialThreshold (a truncated/reflowed render)
        bool outOfOrder = false; // no in-order candidate; fell back to the last global occurrence (lower confidence)
        size_t needleLen = 0; // the needle length that hit after backoff (diagnostic; <= maxNeedle)
    };

    namespace detail
    {
        // Deterministic, locale-free ASCII lowercasing. Prompts + terminal text are overwhelmingly
        // ASCII; this avoids towlower()'s locale dependence (and a full-Unicode fold's cost) on the
        // hot path. Non-ASCII passes through verbatim (still matches itself).
        inline constexpr wchar_t LowerAscii(wchar_t c) noexcept
        {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }

        inline constexpr bool IsWs(wchar_t c) noexcept
        {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v';
        }

        // Normalize `s` AND record, for every normalized char, the raw index it came from. `map` ends
        // with a sentinel (== s.size()) so a normalized [a,b) span maps to raw [map[a], map[b]). An
        // inserted run-collapse space maps to the raw index of the first non-ws char that follows it.
        //
        // PERF (SUMMARY_JUMP.md §4): this is the dominant cost of a batch resolve (it touches the whole
        // haystack). Both buffers are pre-sized to the raw length (norm <= raw) and written BY INDEX —
        // no per-char push_back capacity checks/branches — then trimmed to the true length once.
        inline void NormalizeWithMap(std::wstring_view s, std::wstring& norm, std::vector<uint32_t>& map)
        {
            const size_t n = s.size();
            norm.resize(n);
            map.resize(n + 1);

            size_t w = 0; // write cursor into norm/map
            bool pendingSpace = false; // saw whitespace; emit one space before the next real char
            bool started = false; // suppress leading whitespace entirely
            for (uint32_t i = 0; i < n; ++i)
            {
                const auto c = s[i];
                if (IsWs(c))
                {
                    pendingSpace = started; // only meaningful once content has begun
                    continue;
                }
                if (pendingSpace)
                {
                    norm[w] = L' ';
                    map[w] = i; // the collapsed space stands in front of this char
                    ++w;
                    pendingSpace = false;
                }
                norm[w] = LowerAscii(c);
                map[w] = i;
                ++w;
                started = true;
            }
            norm.resize(w);
            map.resize(w + 1);
            map[w] = static_cast<uint32_t>(n); // end sentinel
        }

        // Candidate needle lengths for backoff: full, then halving, down to >= minNeedle, with a final
        // exact-minNeedle attempt. Deduplicated, descending (longest/most-specific first).
        inline std::vector<size_t> BackoffLengths(size_t full, const AnchorOptions& opts)
        {
            std::vector<size_t> lens;
            const size_t floorLen = (std::min)(opts.minNeedle, full);
            size_t len = full;
            for (int step = 0; step < opts.backoffSteps && len > floorLen; ++step)
            {
                lens.push_back(len);
                len = (std::max)(floorLen, len / 2);
            }
            if (lens.empty() || lens.back() != floorLen)
            {
                lens.push_back(floorLen);
            }
            return lens;
        }

        // Longest common prefix of norm[pos..] and nmsg — the "match as much as possible" extension.
        inline size_t CommonRun(const std::wstring& norm, size_t pos, const std::wstring& nmsg)
        {
            size_t q = 0;
            const size_t cap = (std::min)(norm.size() - pos, nmsg.size());
            while (q < cap && norm[pos + q] == nmsg[q])
            {
                ++q;
            }
            return q;
        }

        // Score + map a hit at normalized position `pos` into an AnchorMatch (raw offsets via `map`).
        inline AnchorMatch ScoreHit(const std::wstring& norm,
                                    const std::vector<uint32_t>& map,
                                    size_t pos,
                                    const std::wstring& nmsg,
                                    size_t needleLen,
                                    const AnchorOptions& opts)
        {
            AnchorMatch m;
            m.found = true;
            const size_t run = (std::max)(needleLen, CommonRun(norm, pos, nmsg));
            m.offset = map[pos];
            m.length = map[pos + run] - map[pos];
            m.quality = nmsg.empty() ? 0.0 : static_cast<double>(run) / static_cast<double>(nmsg.size());
            m.partial = m.quality < opts.partialThreshold;
            m.needleLen = needleLen;
            return m;
        }

        // Does `s` contain a strong right-to-left character? A terminal renders an RTL line in VISUAL
        // (reversed) order while the transcript stores it in LOGICAL order — so a Hebrew/Arabic prompt
        // appears character-reversed in the buffer. We only attempt the reversed orientation for prompts
        // that actually contain RTL text, so LTR matching is never perturbed. Ranges: Hebrew/Arabic/Syriac/
        // Thaana/NKo/Samaritan/Mandaic/Arabic-Ext (U+0590..U+08FF) + Hebrew/Arabic presentation forms.
        inline bool ContainsRtl(std::wstring_view s) noexcept
        {
            for (const auto c : s)
            {
                if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFEFC))
                {
                    return true;
                }
            }
            return false;
        }

        // Agentmaster (SUMMARY_JUMP.md §5) — PROMPT-MARKER VALIDATION. Claude Code prefixes a SENT user
        // prompt's rendered line with a marker glyph (kClaudePromptMarkers). A match that sits right after
        // such a marker is the REAL user-prompt render; the same text appearing elsewhere (an assistant
        // echo, a tool result, a diff) carries no marker. The helpers below are pure + generic — the marker
        // SET is injected by the caller via AnchorOptions::promptMarkers, so PromptAnchor.h stays free of
        // app-specific knowledge.

        // Any of `markers` within [pos-lookback, pos) of `norm`? (A real render starts right after a marker;
        // an echoed/incidental occurrence does not.) `lookback` is tiny — a marker glyph, a space, maybe a
        // box/indent char. O(lookback).
        inline bool MarkerBefore(const std::wstring& norm, size_t pos, std::wstring_view markers, size_t lookback)
        {
            if (markers.empty() || pos == 0)
            {
                return false;
            }
            const size_t start = pos > lookback ? pos - lookback : 0;
            for (size_t i = start; i < pos; ++i)
            {
                if (markers.find(norm[i]) != std::wstring_view::npos)
                {
                    return true;
                }
            }
            return false;
        }

        // Is marker validation USABLE in this haystack? When markers are configured but NONE occur anywhere
        // (a Claude build/theme that renders SENT prompts without the glyph, or an alt-screen session with no
        // scrollback), the resolver disables enforcement for that resolve so matching degrades to the legacy,
        // marker-agnostic behavior instead of failing every prompt. One pass over `norm` (markers is tiny).
        inline bool AnyMarkerPresent(const std::wstring& norm, std::wstring_view markers)
        {
            return !markers.empty() && norm.find_first_of(markers) != std::wstring::npos;
        }

        // Find an occurrence of `pat` in `norm` that satisfies the marker gate. `global`=false: the EARLIEST
        // at/after `from`; true: the LAST anywhere (rfind). When `enforceMarker`, only an occurrence preceded
        // by a marker (MarkerBefore) qualifies — the scan skips an unmarked occurrence (e.g. an assistant
        // echo) and keeps looking. Returns npos if none qualifies. (When !enforceMarker this is exactly the
        // old single norm.find / norm.rfind.)
        inline size_t FindAcceptable(const std::wstring& norm,
                                     std::wstring_view pat,
                                     size_t from,
                                     bool global,
                                     bool enforceMarker,
                                     std::wstring_view markers,
                                     size_t lookback)
        {
            if (pat.empty())
            {
                return std::wstring::npos;
            }
            if (!global)
            {
                for (size_t scan = from;;)
                {
                    const auto p = norm.find(pat, scan);
                    if (p == std::wstring::npos)
                    {
                        return std::wstring::npos;
                    }
                    if (!enforceMarker || MarkerBefore(norm, p, markers, lookback))
                    {
                        return p;
                    }
                    scan = p + 1; // unmarked occurrence (echo) — keep scanning forward
                }
            }
            for (size_t end = std::wstring::npos;;)
            {
                const auto p = norm.rfind(pat, end);
                if (p == std::wstring::npos)
                {
                    return std::wstring::npos;
                }
                if (!enforceMarker || MarkerBefore(norm, p, markers, lookback))
                {
                    return p;
                }
                if (p == 0)
                {
                    return std::wstring::npos;
                }
                end = p - 1; // unmarked — keep scanning backward (toward the start)
            }
        }

        // Agentmaster (SUMMARY_JUMP.md §4, perf) — the LAZY FLOOR-HIT INDEX, the miss-cascade cost cap.
        // Every backoff needle is a PREFIX of the same normalized first line, so every occurrence of ANY
        // backoff length starts at an occurrence of the SHORTEST (floor) prefix. When a prompt heads into
        // the miss cascade (its longest needle absent — up to ~4 probe families x ~4 lengths, each a full
        // O(haystack) scan: in-order + global-rfind, marker-enforced + the soft-fallback repeat), ONE
        // O(haystack) pass collects the floor-prefix hit positions and every remaining probe becomes a
        // walk of those candidates with an O(needle) verify — capping a pathological prompt at ~2 full
        // scans instead of ~10-20. Built LAZILY on the first legacy miss so the common on-screen case
        // (longest needle hits in-order immediately) never pays the collection. Occurrences are collected
        // with a +1 step so OVERLAPPING occurrences are candidates too (the set must be a true superset
        // of every longer needle's occurrence set). A haystack DENSE in the floor prefix
        // (> kFloorIndexMaxHits candidates — e.g. a repeated-character needle over a repeated-character
        // buffer) aborts to Dense and every probe stays on the legacy vectorized scan, which handles
        // dense-hit inputs well (matches are found early). This cascade was the release-0.6.8 UI freeze's
        // UNIT cost — 19 panels x a 200+-prompt conversation x ~10-20 scans per scrolled-off prompt (the
        // caller-side epoch cache / focus gate / interval floor bound how OFTEN a resolve runs; this
        // bounds the resolve ITSELF).
        struct FloorHitIndex
        {
            uint8_t state = 0; // 0 = not built, 1 = built (hits valid), 2 = dense (use the legacy scans)
            std::vector<size_t> hits; // ascending normalized offsets of the floor prefix (state 1 only)
        };
        inline constexpr size_t kFloorIndexMaxHits = 512;

        inline void EnsureFloorIndex(const std::wstring& norm, const std::wstring& needle, size_t floorLen, FloorHitIndex& idx)
        {
            if (idx.state != 0)
            {
                return;
            }
            const auto f = std::wstring_view{ needle }.substr(0, (std::min)(floorLen, needle.size()));
            idx.state = 1;
            if (f.empty())
            {
                return; // no needle => no candidates (mirrors FindAcceptable's empty-pat npos)
            }
            for (size_t scan = 0;;)
            {
                const auto p = norm.find(f, scan);
                if (p == std::wstring::npos)
                {
                    return;
                }
                if (idx.hits.size() >= kFloorIndexMaxHits)
                {
                    idx.hits.clear();
                    idx.state = 2; // dense — the legacy scans stay cheap + equivalent here
                    return;
                }
                idx.hits.push_back(p);
                scan = p + 1; // +1, not +len: overlapping occurrences must be candidates too
            }
        }

        // FindAcceptable over the candidate index: the same result as the legacy scan by construction —
        // the candidate set is a superset of pat's occurrences (pat extends the floor prefix), walked in
        // the same direction with the same marker gate; a candidate qualifies iff pat matches there.
        inline size_t FindAcceptableIndexed(const std::wstring& norm,
                                            std::wstring_view pat,
                                            size_t from,
                                            bool global,
                                            bool enforceMarker,
                                            std::wstring_view markers,
                                            size_t lookback,
                                            const FloorHitIndex& idx)
        {
            if (pat.empty())
            {
                return std::wstring::npos;
            }
            const auto matchesAt = [&](size_t p) noexcept {
                return p + pat.size() <= norm.size() && norm.compare(p, pat.size(), pat) == 0;
            };
            if (!global)
            {
                for (auto it = std::lower_bound(idx.hits.begin(), idx.hits.end(), from); it != idx.hits.end(); ++it)
                {
                    if (matchesAt(*it) && (!enforceMarker || MarkerBefore(norm, *it, markers, lookback)))
                    {
                        return *it;
                    }
                }
                return std::wstring::npos;
            }
            for (auto it = idx.hits.rbegin(); it != idx.hits.rend(); ++it)
            {
                if (matchesAt(*it) && (!enforceMarker || MarkerBefore(norm, *it, markers, lookback)))
                {
                    return *it;
                }
            }
            return std::wstring::npos;
        }

        // Locate `nmsg` — and, when `tryRev`, its char-reversal `rmsg` (the visual form an RTL line takes
        // in the buffer) — in `norm`, longest needle first. `global`=false searches at/after `cursor`
        // (earliest occurrence); true searches the LAST occurrence (rfind). Forward is always tried first
        // so an LTR hit never reaches the reversed pass. On an in-order hit, *cursorEnd (if non-null) is
        // set to the norm offset to advance the greedy cursor past this match. Returns found=false if none.
        //
        // `enforceMarker` (SUMMARY_JUMP.md §5) gates the FORWARD (LTR) orientation to occurrences preceded by
        // a prompt marker — rejecting an assistant echo of the prompt text. It is NOT applied to the reversed
        // (RTL) orientation: a marker's position under bidi reversal is unreliable, and RTL is already
        // best-effort, so a reversed hit is accepted on text alone (preserving today's RTL behavior).
        inline AnchorMatch LocateOriented(const std::wstring& norm,
                                          const std::vector<uint32_t>& map,
                                          const std::wstring& nmsg,
                                          const std::wstring& rmsg,
                                          bool tryRev,
                                          size_t cursor,
                                          const std::vector<size_t>& lens,
                                          const AnchorOptions& opts,
                                          bool global,
                                          bool enforceMarker,
                                          size_t* cursorEnd,
                                          FloorHitIndex* fwdIdx = nullptr,
                                          FloorHitIndex* revIdx = nullptr)
        {
            // One probe of one orientation at one needle length. Legacy-scan until a first MISS proves
            // the backoff cascade has begun, then build the floor-hit index ONCE (per orientation; the
            // caller owns it across the in-order / global / soft-fallback probe families of one prompt)
            // and answer every later probe from the candidate walk. idx == nullptr, or Dense => legacy.
            const auto probe = [&](const std::wstring& msg, size_t len, bool enforce, FloorHitIndex* idx) -> size_t {
                const auto pat = std::wstring_view{ msg }.substr(0, len);
                if (idx && idx->state == 1)
                {
                    return FindAcceptableIndexed(norm, pat, cursor, global, enforce, opts.promptMarkers, opts.markerLookback, *idx);
                }
                const auto p = FindAcceptable(norm, pat, cursor, global, enforce, opts.promptMarkers, opts.markerLookback);
                if (idx && idx->state == 0 && p == std::wstring::npos)
                {
                    EnsureFloorIndex(norm, msg, lens.back(), *idx); // first miss — the cascade begins; index the rest
                }
                return p;
            };
            for (const auto len : lens)
            {
                const auto fpos = probe(nmsg, len, enforceMarker, fwdIdx);
                if (fpos != std::wstring::npos)
                {
                    if (cursorEnd)
                    {
                        *cursorEnd = fpos + (std::max)(len, CommonRun(norm, fpos, nmsg));
                    }
                    return ScoreHit(norm, map, fpos, nmsg, len, opts);
                }
                if (tryRev)
                {
                    const auto rpos = probe(rmsg, len, /*enforce*/ false, revIdx);
                    if (rpos != std::wstring::npos)
                    {
                        if (cursorEnd)
                        {
                            *cursorEnd = rpos + (std::max)(len, CommonRun(norm, rpos, rmsg));
                        }
                        return ScoreHit(norm, map, rpos, rmsg, len, opts);
                    }
                }
            }
            return {};
        }

        // True-absence membership pre-check across BOTH orientations: shortest prefix nowhere => skip.
        inline bool PresentEither(const std::wstring& norm, const std::wstring& nmsg, const std::wstring& rmsg, bool tryRev, size_t floorLen)
        {
            if (norm.find(std::wstring_view{ nmsg }.substr(0, floorLen)) != std::wstring::npos)
            {
                return true;
            }
            return tryRev && norm.find(std::wstring_view{ rmsg }.substr(0, floorLen)) != std::wstring::npos;
        }
    }

    // ASCII-lowercase + collapse every run of whitespace (space/tab/CR/LF/FF/VT) to a single space +
    // trim leading/trailing whitespace. The canonical match form for both haystack and needle.
    inline std::wstring NormalizeForMatch(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        bool pendingSpace = false;
        bool started = false;
        for (const auto c : s)
        {
            if (detail::IsWs(c))
            {
                pendingSpace = started;
                continue;
            }
            if (pendingSpace)
            {
                out.push_back(L' ');
                pendingSpace = false;
            }
            out.push_back(detail::LowerAscii(c));
            started = true;
        }
        return out;
    }

    // The needle for a prompt: its first NON-TRIVIAL line (skipping leading blank lines), normalized,
    // truncated to maxLen. Empty if the prompt has no non-whitespace content.
    inline std::wstring PickAnchorNeedle(std::wstring_view message, size_t maxLen)
    {
        size_t i = 0;
        const size_t n = message.size();
        while (i < n)
        {
            size_t lineEnd = message.find(L'\n', i);
            if (lineEnd == std::wstring_view::npos)
            {
                lineEnd = n;
            }
            const auto line = message.substr(i, lineEnd - i);
            auto norm = NormalizeForMatch(line);
            if (!norm.empty())
            {
                if (norm.size() > maxLen)
                {
                    norm.resize(maxLen);
                }
                return norm;
            }
            i = lineEnd + 1;
        }
        return {};
    }

    // Resolve ONE prompt against `haystack`, searching at or after `fromOffset` (the greedy cursor).
    // Backs off to shorter needles, and to the last global occurrence (outOfOrder) when nothing is
    // in-order. The caller advances its own cursor by the returned match end for the next prompt.
    //
    // NOT the production path (SUMMARY_JUMP.md §3): the jump (▸), alt+up/down nav, and icon eligibility ALL
    // resolve the WHOLE prompt list via ResolvePromptAnchors in one order-preserving pass — duplicate
    // disambiguation places each anchor relative to the others, so a per-anchor resolve would mis-bind a
    // repeated prompt. This single-message helper is for tests / a genuinely degenerate one-prompt caller
    // only; do NOT wire it into the scan paths (keep "guarantee full batch resolve only").
    inline AnchorMatch ResolveOnePromptAnchor(std::wstring_view haystack,
                                              std::wstring_view message,
                                              size_t fromOffset,
                                              const AnchorOptions& opts = {})
    {
        std::wstring norm;
        std::vector<uint32_t> map;
        detail::NormalizeWithMap(haystack, norm, map);

        size_t cursor = 0;
        if (fromOffset > 0)
        {
            const auto it = std::lower_bound(map.begin(), map.end(), static_cast<uint32_t>(fromOffset));
            cursor = (std::min)(static_cast<size_t>(it - map.begin()), norm.size());
        }

        const auto nmsg = NormalizeForMatch(message);
        AnchorMatch out;
        if (nmsg.empty() || norm.empty())
        {
            return out;
        }

        // Marker validation (SUMMARY_JUMP.md §5) is enforced only when markers are configured AND actually
        // present in this haystack; otherwise the resolve is legacy/marker-agnostic (never regresses).
        const bool enforceMarker = !opts.promptMarkers.empty() && detail::AnyMarkerPresent(norm, opts.promptMarkers);

        // RTL prompts render character-reversed in the terminal buffer (visual order); also try the
        // reversal for those. Forward is tried first inside LocateOriented, so LTR is never perturbed.
        const bool tryRev = detail::ContainsRtl(nmsg);
        std::wstring rmsg;
        if (tryRev)
        {
            rmsg.assign(nmsg.rbegin(), nmsg.rend());
        }

        const size_t full = (std::min)(opts.maxNeedle, nmsg.size());
        const auto lens = detail::BackoffLengths(full, opts);

        // True-absence short-circuit (PERF): if even the shortest prefix is nowhere (either orientation),
        // no longer needle can be either — skip the full backoff + global fallback.
        if (!detail::PresentEither(norm, nmsg, rmsg, tryRev, lens.back()))
        {
            return out;
        }

        // Marker-PREFERRED (SUMMARY_JUMP.md §5): in-order, then last-global — accepting only MARKED
        // occurrences when enforceMarker. (Single-prompt helper; not the production batch path.)
        if (auto m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, cursor, lens, opts, /*global*/ false, enforceMarker, nullptr); m.found)
        {
            return m;
        }
        if (auto m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, 0, lens, opts, /*global*/ true, enforceMarker, nullptr); m.found)
        {
            m.outOfOrder = true;
            return m;
        }
        // SOFT FALLBACK: no marked occurrence -> degrade to the legacy marker-agnostic resolve so a prompt
        // that legacy would have found never vanishes under enforcement (mirrors the batch path).
        if (enforceMarker)
        {
            if (auto m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, cursor, lens, opts, /*global*/ false, /*enforceMarker*/ false, nullptr); m.found)
            {
                return m;
            }
            if (auto m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, 0, lens, opts, /*global*/ true, /*enforceMarker*/ false, nullptr); m.found)
            {
                m.outOfOrder = true;
                return m;
            }
        }
        return out;
    }

    // Resolve ALL prompts (in conversation order) against `haystack` in one pass, with the
    // order-preserving greedy assignment that handles duplicate prompt texts (SUMMARY_JUMP.md §3).
    // Returns one AnchorMatch per input message (same order); `found==false` where nothing resolved.
    // `haystack` is the raw linearized buffer; returned offsets index into it.
    inline std::vector<AnchorMatch> ResolvePromptAnchors(std::wstring_view haystack,
                                                         const std::vector<std::wstring>& messages,
                                                         const AnchorOptions& opts = {})
    {
        std::vector<AnchorMatch> results(messages.size());

        // Normalize the haystack ONCE for the whole batch (the dominant cost; SUMMARY_JUMP.md §4).
        std::wstring norm;
        std::vector<uint32_t> map;
        detail::NormalizeWithMap(haystack, norm, map);
        if (norm.empty())
        {
            return results;
        }

        // Marker validation (SUMMARY_JUMP.md §5): compute ONCE for the whole batch — enforced only when
        // markers are configured AND present in this haystack (else legacy/marker-agnostic; never regresses).
        const bool enforceMarker = !opts.promptMarkers.empty() && detail::AnyMarkerPresent(norm, opts.promptMarkers);

        size_t cursor = 0; // greedy: the lowest normalized offset the next prompt may occupy
        for (size_t mi = 0; mi < messages.size(); ++mi)
        {
            const auto nmsg = NormalizeForMatch(messages[mi]);
            if (nmsg.empty())
            {
                continue;
            }
            // RTL prompts render character-reversed in the buffer; also try the reversal for those
            // (forward first inside LocateOriented, so LTR is never perturbed).
            const bool tryRev = detail::ContainsRtl(nmsg);
            std::wstring rmsg;
            if (tryRev)
            {
                rmsg.assign(nmsg.rbegin(), nmsg.rend());
            }

            const size_t full = (std::min)(opts.maxNeedle, nmsg.size());
            const auto lens = detail::BackoffLengths(full, opts);

            // True-absence short-circuit (PERF): shortest prefix nowhere (either orientation) => skip.
            if (!detail::PresentEither(norm, nmsg, rmsg, tryRev, lens.back()))
            {
                continue; // results[mi] stays not-found; cursor unchanged
            }

            // Lazy floor-hit index (see detail::FloorHitIndex), shared by ALL FOUR probe families below
            // (in-order + global, marker-enforced + soft-fallback) so a miss-cascade prompt pays ~2 full
            // scans instead of ~10-20. Per prompt — the floor prefix differs per needle.
            detail::FloorHitIndex fwdIdx;
            detail::FloorHitIndex revIdx;
            detail::FloorHitIndex* const revIdxPtr = tryRev ? &revIdx : nullptr;

            // Marker-PREFERRED, order-preserving resolve (SUMMARY_JUMP.md §3/§5). When enforceMarker, the
            // first two probes accept ONLY a marked occurrence (a real user-prompt render), so a match binds
            // to the prompt and not to an assistant echo of the same words: in-order first (advances the
            // greedy cursor); else the last global marked occurrence (out-of-order). If NO marked occurrence
            // exists for this prompt (its render scrolled off, or — defensively — the marker glyph isn't on
            // sent-prompt lines in this build), a SOFT FALLBACK repeats the same two probes WITHOUT the
            // marker, degrading to exactly the legacy result, so enforcement can NEVER make a prompt that
            // legacy would resolve vanish. (When !enforceMarker the first probes already ARE the legacy ones,
            // so the fallback is a no-op and is skipped.)
            size_t cend = cursor;
            AnchorMatch m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, cursor, lens, opts, /*global*/ false, enforceMarker, &cend, &fwdIdx, revIdxPtr);
            if (m.found)
            {
                cursor = cend;
            }
            else if (m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, 0, lens, opts, /*global*/ true, enforceMarker, nullptr, &fwdIdx, revIdxPtr); m.found)
            {
                m.outOfOrder = true;
            }
            else if (enforceMarker)
            {
                // No marked hit -> legacy (marker-agnostic) resolve: in-order (advance) else global (oo-order).
                size_t cend2 = cursor;
                m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, cursor, lens, opts, /*global*/ false, /*enforceMarker*/ false, &cend2, &fwdIdx, revIdxPtr);
                if (m.found)
                {
                    cursor = cend2;
                }
                else if (m = detail::LocateOriented(norm, map, nmsg, rmsg, tryRev, 0, lens, opts, /*global*/ true, /*enforceMarker*/ false, nullptr, &fwdIdx, revIdxPtr); m.found)
                {
                    m.outOfOrder = true;
                }
            }
            results[mi] = m;
        }

        // SECOND PASS — COLLISION resolution (SUMMARY_JUMP.md §5b). A prompt's needle is its first line — a
        // PREFIX of its text — and matching is a substring search, so SEVERAL prompts can land on ONE rendered
        // line: a strict PREFIX ("deploy dev please" vs "deploy dev please, fast mode") matches the START of
        // the longer one's render; a SUFFIX / substring ("deploy dev" inside "please deploy dev") matches the
        // MIDDLE of it; and a longer prompt whose OWN render scrolled off can BACK OFF to a shorter prefix that
        // matches a shorter sibling's render (same offset, same length, lower quality). In every shape the two
        // matches occupy OVERLAPPING regions of the same render — and the prompt the render actually shows owns
        // it. Decide the owner by REGION CONTAINMENT: if prompt j's matched region [off, off+len) strictly
        // CONTAINS prompt i's, then i is a prefix/suffix/substring of j's render and is unresolved (its real
        // render isn't on screen — dimming beats mis-jumping onto a sibling, the "(4) and (5) both jump to (5)"
        // report). For an IDENTICAL region the fuller match wins (greater quality — so a longer prompt that
        // merely backed off to this prefix yields to the sibling whose whole text the render shows). Identical
        // region AND equal quality is left untouched — exact-duplicate texts sharing one surviving render, the
        // legitimate case the order-preserving greedy already spreads across distinct occurrences. O(n^2) over
        // the (small) prompt count; order-independent — interval containment is transitive + asymmetric, so the
        // true owner (the outermost / fullest region) is never the one unresolved.
        for (size_t i = 0; i < results.size(); ++i)
        {
            if (!results[i].found)
            {
                continue;
            }
            const size_t iEnd = results[i].offset + results[i].length;
            for (size_t j = 0; j < results.size(); ++j)
            {
                if (j == i || !results[j].found)
                {
                    continue;
                }
                const size_t jEnd = results[j].offset + results[j].length;
                if (results[j].offset <= results[i].offset && jEnd >= iEnd) // j's region contains i's
                {
                    const bool sameRegion = results[j].offset == results[i].offset && jEnd == iEnd;
                    if (!sameRegion || results[j].quality > results[i].quality)
                    {
                        results[i] = {}; // i is a prefix/suffix/substring of j's render (or a backed-off tie) -> unresolve
                        break;
                    }
                }
            }
        }
        return results;
    }

    // CHEAP per-click re-validation: is `message`'s needle still present at/around `offset` in
    // `haystack`? O(needle) — reads only a small window at the cached offset, no full scan. Returns
    // false when the buffer shifted out from under the cached span (=> the caller does a full resolve).
    inline bool ValidatePromptAnchor(std::wstring_view haystack,
                                     std::wstring_view message,
                                     size_t offset,
                                     const AnchorOptions& opts = {})
    {
        if (offset > haystack.size())
        {
            return false;
        }
        const auto needle = PickAnchorNeedle(message, opts.maxNeedle);
        if (needle.empty())
        {
            return false;
        }
        const size_t window = needle.size() * 3 + 16;
        const auto slice = haystack.substr(offset, (std::min)(window, haystack.size() - offset));
        const auto nslice = NormalizeForMatch(slice);
        if (const auto at = nslice.find(needle); at != std::wstring::npos && at <= 2)
        {
            return true;
        }
        // RTL: the buffer holds the visual (reversed) form, so the cached span starts with the reversal.
        if (detail::ContainsRtl(needle))
        {
            const std::wstring rneedle(needle.rbegin(), needle.rend());
            const auto at = nslice.find(rneedle);
            return at != std::wstring::npos && at <= 2;
        }
        return false;
    }
}
