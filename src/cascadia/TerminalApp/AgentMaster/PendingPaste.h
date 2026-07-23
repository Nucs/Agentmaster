// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the PASTE-CACHE resolver for pending-input drafts (PENDING_INPUT.md section 2b).
//
// Claude Code spills a large paste to disk AT PASTE TIME -- <CLAUDE_CONFIG_DIR | ~/.claude>/
// paste-cache/<16-hex>.txt (content-addressed leaf; observed stable since 2026-01) -- and renders the
// draft's input box with a PLACEHOLDER instead of the content, in one of two forms:
//
//   [Pasted text #N +M lines]                the whole paste COLLAPSED to one marker line
//   head[...Truncated text #N +M lines...]tail   the paste EXPANDED with its MIDDLE elided in place
//
// The pending-input detector (PendingInput.h) therefore extracts a draft whose text names content it
// does not contain. This unit resolves those markers back to the real cached file -- CONTENT-ANCHORED,
// never by mtime (the cache is global across sessions, so a time window races) -- and validates the
// match by ARITHMETIC before ever claiming it:
//
//   * COLLAPSED:  the marker's M equals the candidate file's line count. Both observed counting
//     conventions are accepted -- M == newline count (proven live: a 274-segment file whose last
//     segment is unterminated carried "+273 lines") or M == segment count -- and the match must be
//     UNIQUE across the cache under whichever convention hit; ambiguity refuses (a wrong 273-line
//     expansion is far worse than a placeholder).
//   * TRUNCATED:  the draft itself carries real paste content around the marker. The text just
//     before the marker is a PREFIX of file segment i (the head-cut line), the text just after is a
//     SUFFIX of segment j (the tail-resume line), and j - i == M exactly. Proven live on the fixture
//     session: head "## The user" = prefix of segment 8, tail "es)." = suffix of segment 266,
//     266 - 8 == 258 == the marker's "+258 lines". Two content anchors + an exact offset make a
//     false match practically impossible.
//
// PURE + HEADER-ONLY (the PendingInput.h / PromptAnchor.h idiom): no filesystem, no WinRT, pure-ASCII
// source -- the impure adapter (ClaudeSpawn.cpp: ClaudePasteCacheDir / ResolvePendingPasteRefs) reads
// the cache directory and feeds file texts in, so the whole brain is unit-testable standalone.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // One paste/truncation marker found in a draft's text.
    struct PasteMarker
    {
        bool truncated{ false }; // [...Truncated text #N +M lines...] vs [Pasted text #N +M lines]
        int index{ 0 }; // N -- Claude's per-conversation paste counter (opaque; not interpreted)
        int lines{ 0 }; // M -- collapsed: the paste's total line count; truncated: the HIDDEN middle
        size_t draftLine{ 0 }; // which draft line (0-based, '\n'-split) carries the marker
        size_t charBegin{ 0 }; // the marker's [ .. ] span within that line (begin inclusive)
        size_t charEnd{ 0 }; // (end exclusive)
        std::wstring headFragment; // text on the marker line BEFORE '[' (truncated: the head cut's last visible text)
        std::wstring tailFragment; // text on the marker line AFTER ']' (truncated: the tail resume's first visible text)
    };

    // One cached paste file's content, fed by the impure adapter.
    struct PasteFileText
    {
        std::wstring name; // the cache leaf, e.g. "bf8eefafa3e80676.txt"
        std::wstring text; // full UTF-8-decoded content
    };

    // The per-marker resolution verdict. resolved implies verified -- an unverifiable candidate is
    // REFUSED, never half-claimed.
    struct PasteResolution
    {
        bool resolved{ false };
        std::wstring fileName; // the winning cache leaf ("" when unresolved)
        int fileSegments{ 0 }; // the winning file's segment count (diagnostic)
    };

    namespace paste_detail
    {
        // '\n'-split segments. A file ending WITH a newline contributes no trailing empty segment;
        // CR is tolerated (CRLF-normalized per segment). Claude's own "+M lines" has been observed
        // under BOTH conventions (newline count vs segment count) -- see the header comment -- so
        // callers compare against both.
        inline std::vector<std::wstring> SplitSegments(std::wstring_view text)
        {
            std::vector<std::wstring> segs;
            size_t start = 0;
            for (size_t i = 0; i <= text.size(); ++i)
            {
                if (i == text.size() || text[i] == L'\n')
                {
                    if (i == text.size() && i == start)
                    {
                        break; // trailing '\n' => no empty final segment
                    }
                    auto seg = text.substr(start, i - start);
                    if (!seg.empty() && seg.back() == L'\r')
                    {
                        seg.remove_suffix(1);
                    }
                    segs.emplace_back(seg);
                    start = i + 1;
                }
            }
            return segs;
        }

        inline size_t NewlineCount(std::wstring_view text) noexcept
        {
            size_t n = 0;
            for (const auto c : text)
            {
                if (c == L'\n')
                {
                    ++n;
                }
            }
            return n;
        }

        // Parse ONE marker starting at `pos` (which must point at '['). Returns true + fills m
        // (except draftLine/fragments -- the caller owns line context) on an exact grammar match:
        //   [Pasted text #N +M lines]      /  [Pasted text #N +M line]
        //   [...Truncated text #N +M lines...]
        inline bool ParseMarkerAt(std::wstring_view line, size_t pos, PasteMarker& m) noexcept
        {
            const auto starts = [&](std::wstring_view what, size_t at) noexcept {
                return line.size() - at >= what.size() && line.substr(at, what.size()) == what;
            };
            size_t p = pos + 1;
            bool trunc = false;
            if (starts(L"...Truncated text #", p))
            {
                trunc = true;
                p += 19;
            }
            else if (starts(L"Pasted text #", p))
            {
                p += 13;
            }
            else
            {
                return false;
            }
            int idx = 0;
            size_t digits = 0;
            while (p < line.size() && line[p] >= L'0' && line[p] <= L'9' && digits < 7)
            {
                idx = idx * 10 + (line[p] - L'0');
                ++p;
                ++digits;
            }
            if (digits == 0 || !starts(L" +", p))
            {
                return false;
            }
            p += 2;
            int cnt = 0;
            digits = 0;
            while (p < line.size() && line[p] >= L'0' && line[p] <= L'9' && digits < 8)
            {
                cnt = cnt * 10 + (line[p] - L'0');
                ++p;
                ++digits;
            }
            if (digits == 0)
            {
                return false;
            }
            if (starts(L" lines", p))
            {
                p += 6;
            }
            else if (starts(L" line", p))
            {
                p += 5;
            }
            else
            {
                return false;
            }
            if (trunc)
            {
                if (!starts(L"...]", p))
                {
                    return false;
                }
                p += 4;
            }
            else
            {
                if (p >= line.size() || line[p] != L']')
                {
                    return false;
                }
                p += 1;
            }
            m.truncated = trunc;
            m.index = idx;
            m.lines = cnt;
            m.charBegin = pos;
            m.charEnd = p;
            return true;
        }
    }

    // Find every paste/truncation marker in a draft ('\n'-joined, as DetectPendingInput emits it).
    inline std::vector<PasteMarker> FindPasteMarkers(std::wstring_view draft)
    {
        using namespace paste_detail;
        std::vector<PasteMarker> out;
        const auto lines = SplitSegments(draft);
        for (size_t li = 0; li < lines.size(); ++li)
        {
            const auto& line = lines[li];
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] != L'[')
                {
                    continue;
                }
                PasteMarker m;
                if (!ParseMarkerAt(line, i, m))
                {
                    continue;
                }
                m.draftLine = li;
                m.headFragment = line.substr(0, m.charBegin);
                m.tailFragment = line.substr(m.charEnd);
                out.push_back(std::move(m));
                i = m.charEnd - 1; // continue after the marker (multiple markers per line tolerated)
            }
        }
        return out;
    }

    // Validate ONE candidate file against ONE marker (the arithmetic described in the header).
    // For a truncated marker, fills ioHeadSeg with the matched head-cut segment index (1-based)
    // when validation succeeds (the expansion uses it).
    inline bool ValidatePasteFile(const PasteMarker& m, std::wstring_view fileText, size_t* ioHeadSeg = nullptr)
    {
        using namespace paste_detail;
        if (m.lines <= 0)
        {
            return false;
        }
        if (!m.truncated)
        {
            // COLLAPSED: M == newline count OR M == segment count. Only sensible when the marker is
            // the whole visible content of its line (fragments around a collapsed marker are typed
            // text, not paste content -- they carry no signal about THIS file).
            const size_t nl = NewlineCount(fileText);
            const auto segs = SplitSegments(fileText);
            return nl == static_cast<size_t>(m.lines) || segs.size() == static_cast<size_t>(m.lines);
        }
        // TRUNCATED: head fragment = PREFIX of segment i; tail fragment = SUFFIX of segment i+M.
        // Require enough anchor material that a coincidence is implausible.
        const auto& head = m.headFragment;
        const auto& tail = m.tailFragment;
        if (head.size() + tail.size() < 6 || head.empty() || tail.empty())
        {
            return false; // too little anchor to validate honestly -> refuse
        }
        const auto segs = SplitSegments(fileText);
        const size_t off = static_cast<size_t>(m.lines);
        for (size_t i = 0; i + off < segs.size(); ++i)
        {
            const auto& hs = segs[i];
            if (hs.size() < head.size() || hs.compare(0, head.size(), head) != 0)
            {
                continue;
            }
            const auto& ts = segs[i + off];
            if (ts.size() >= tail.size() && ts.compare(ts.size() - tail.size(), tail.size(), tail) == 0)
            {
                if (ioHeadSeg)
                {
                    *ioHeadSeg = i + 1; // 1-based
                }
                return true;
            }
        }
        return false;
    }

    // Resolve every marker against the supplied cache file set. A marker resolves ONLY when exactly
    // one file validates (collapsed ambiguity -- several same-length files -- refuses; a truncated
    // marker's two-anchor + exact-offset test makes multi-file matches practically impossible, but
    // ambiguity still refuses on principle).
    inline std::vector<PasteResolution> ResolvePasteMarkers(const std::vector<PasteMarker>& markers,
                                                            const std::vector<PasteFileText>& files)
    {
        using namespace paste_detail;
        std::vector<PasteResolution> out;
        out.reserve(markers.size());
        for (const auto& m : markers)
        {
            PasteResolution r;
            size_t hits = 0;
            for (const auto& f : files)
            {
                if (ValidatePasteFile(m, f.text))
                {
                    ++hits;
                    if (hits == 1)
                    {
                        r.resolved = true;
                        r.fileName = f.name;
                        r.fileSegments = static_cast<int>(SplitSegments(f.text).size());
                    }
                    else
                    {
                        r = PasteResolution{}; // ambiguous -> refuse (never guess between files)
                        break;
                    }
                }
            }
            out.push_back(std::move(r));
        }
        return out;
    }

    // Expand ONE VERIFIED marker inside a draft: the marker line is replaced by the real cached
    // content. COLLAPSED requires the marker to be its line's only visible content (fragments would
    // be typed text we must not eat); the whole file substitutes the line. TRUNCATED substitutes the
    // marker line with segments i..i+M verbatim (they subsume the visible head/tail fragments).
    // Returns the expanded draft, or "" when expansion is REFUSED (validation failed, fragments
    // around a collapsed marker, line out of range) -- the caller keeps the placeholder.
    inline std::wstring ExpandPasteMarker(std::wstring_view draft, const PasteMarker& m, std::wstring_view fileText)
    {
        using namespace paste_detail;
        size_t headSeg = 0;
        if (!ValidatePasteFile(m, fileText, &headSeg))
        {
            return {};
        }
        const auto draftLines = SplitSegments(draft);
        if (m.draftLine >= draftLines.size())
        {
            return {};
        }
        const auto segs = SplitSegments(fileText);
        std::vector<std::wstring> repl;
        if (!m.truncated)
        {
            // Only a whole-line marker expands (surrounding ws tolerated).
            const auto& line = draftLines[m.draftLine];
            for (size_t i = 0; i < line.size(); ++i)
            {
                if ((i < m.charBegin || i >= m.charEnd) && line[i] != L' ' && line[i] != L'\t')
                {
                    return {};
                }
            }
            repl.assign(segs.begin(), segs.end());
        }
        else
        {
            const size_t first = headSeg - 1; // 0-based head-cut segment
            const size_t last = first + static_cast<size_t>(m.lines); // 0-based tail-resume segment
            if (last >= segs.size())
            {
                return {};
            }
            repl.assign(segs.begin() + first, segs.begin() + last + 1);
        }
        std::wstring outText;
        for (size_t i = 0; i < draftLines.size(); ++i)
        {
            if (i)
            {
                outText.push_back(L'\n');
            }
            if (i == m.draftLine)
            {
                for (size_t k = 0; k < repl.size(); ++k)
                {
                    if (k)
                    {
                        outText.push_back(L'\n');
                    }
                    outText += repl[k];
                }
            }
            else
            {
                outText += draftLines[i];
            }
        }
        return outText;
    }
}
