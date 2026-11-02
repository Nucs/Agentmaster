// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the PASTE-CACHE resolver for pending-input drafts (PENDING_INPUT.md section 2b).
//
// Claude Code spills a large paste to disk AT PASTE TIME -- <CLAUDE_CONFIG_DIR | ~/.claude>/
// paste-cache/<16-hex>.txt (content-addressed leaf; observed stable since 2026-01) -- and renders the
// draft's input box with a PLACEHOLDER instead of the content, in one of these forms:
//
//   [Pasted text #N +M lines]                the whole paste COLLAPSED to one marker line
//   [Pasted text #N]                         the same for a paste with NO line break (M == 0): claude
//                                            leaves the count out (2.1.310: M == 0 ? "[Pasted text #N]"
//                                            : "[Pasted text #N +M lines]") -- a long one-line text such
//                                            as a voice note's transcript
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
//     expansion is far worse than a placeholder). The BARE form (M == 0) is never matched against the
//     cache: every one-line file in it would "match" a zero count (PasteCacheCanAnswer), so only the
//     paste journal (DraftCarry.h) can back one.
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
        bool truncated{ false }; // [...Truncated text #N +M lines...] vs [Pasted text #N +M lines] / [Pasted text #N]
        int index{ 0 }; // N -- Claude's per-conversation paste counter (opaque; not interpreted)
        int lines{ 0 }; // M -- collapsed: the paste's total line count (0 for the bare "[Pasted text #N]",
                        // claude's render of a paste with no line break); truncated: the HIDDEN middle
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

    // ---- Claude's own paste rules -------------------------------------------------------------------
    //
    // What claude does with a bracketed paste before its box shows it (read from the 2.1.280 bundle,
    // unchanged in 2.1.310): normalize it, count its line breaks, and collapse it into a placeholder when
    // it is long or has many lines. These live here, under SessionModels.h, so the two readers that must
    // agree with claude share ONE copy: the verified send's read-back (SessionModels.h,
    // PromptFillCollapsesToBareMarker) and the draft carry's journal and piecewise fill (DraftCarry.h).

    // Claude's inline-vs-collapse threshold, CHARACTERS (JS UTF-16 length == wchar_t count). A paste
    // longer than this is always collapsed into a placeholder. A piecewise fill keeps every TYPED
    // chunk at or below it so claude inserts the chunk as visible text, never as a placeholder.
    inline constexpr size_t kClaudePasteInlineMaxChars = 800;
    // Claude's inline-vs-collapse threshold, NEWLINES: min(terminal rows - 10, 2). A Claude tab shorter
    // than 12 rows would collapse even a 2-newline chunk -- not a realistic working setup, and the
    // failure is cosmetic (the content still lands, just collapsed), so the common value is used.
    inline constexpr size_t kClaudePasteInlineMaxNewlines = 2;

    // Normalize a pasted text EXACTLY as it ends up in claude's pastedContents: Windows Terminal's
    // paste filter first (FilterStringForPaste with CarriageReturnNewline|ControlCodes -- CRLF and a
    // lone LF become CR, every C0/C1 control except TAB/CR/LF is dropped), then claude's paste handler
    // (stripANSI -- a no-op once ESC is gone -- CRLF/CR -> LF, TAB -> four spaces). Journal entries are
    // stored in this form so a placeholder's "+M lines" and the entry's newline count agree by
    // construction, and so the expansion is byte-identical to what claude itself would submit.
    //   raw     -- the text as it was pasted (the clipboard text, or the body of one of our fills).
    //   returns -- the normalized text ("" for an all-control input).
    //   throws  -- std::bad_alloc on allocation failure only.
    inline std::wstring NormalizePastedTextLikeClaude(std::wstring_view raw)
    {
        std::wstring out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const wchar_t c = raw[i];
            if (c == L'\r')
            {
                out.push_back(L'\n');
                if (i + 1 < raw.size() && raw[i + 1] == L'\n')
                {
                    ++i; // CRLF -> one LF (WT folds it to CR, claude folds that CR to LF)
                }
                continue;
            }
            if (c == L'\n')
            {
                out.push_back(L'\n');
                continue;
            }
            if (c == L'\t')
            {
                out.append(4, L' '); // claude's replaceAll("\t", "    ")
                continue;
            }
            if (c < 0x20 || (c >= 0x7F && c <= 0x9F))
            {
                continue; // WT's ControlCodes filter: C0 + DEL + C1 never reach claude
            }
            out.push_back(c);
        }
        return out;
    }

    // Claude's "+M lines" arithmetic: the number of newline SEQUENCES (CRLF counts once, a lone CR or
    // LF counts once) -- claude's own gM(). On a normalized text this is simply the LF count.
    //   text    -- any text (normalized or raw).
    //   returns -- the newline-sequence count.
    //   throws  -- nothing.
    inline size_t ClaudePasteNewlineCount(std::wstring_view text) noexcept
    {
        size_t n = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'\r')
            {
                ++n;
                if (i + 1 < text.size() && text[i + 1] == L'\n')
                {
                    ++i;
                }
            }
            else if (text[i] == L'\n')
            {
                ++n;
            }
        }
        return n;
    }

    // Would claude collapse this (already normalized) paste into a placeholder rather than insert it
    // as visible text? Only such pastes can ever back a placeholder, so the journal records only these,
    // the resolver only matches against these (a small inline paste can never be what a
    // "[Pasted text ...]" stands for), and the verified send accepts a bare "[Pasted text #N]" as its
    // own fill only when this holds for that fill.
    //   normalized -- a NormalizePastedTextLikeClaude output.
    //   returns    -- true when claude renders it as a placeholder.
    //   throws     -- nothing.
    inline bool ClaudeWouldCollapsePaste(std::wstring_view normalized) noexcept
    {
        return normalized.size() > kClaudePasteInlineMaxChars || ClaudePasteNewlineCount(normalized) > kClaudePasteInlineMaxNewlines;
    }

    // Can the PASTE-CACHE tier answer this marker at all? Both of its tests need a line count: a
    // collapsed marker is matched by its count alone, a truncated one by its content anchors exactly M
    // segments apart. A zero count gives neither test anything honest to check -- every one-line cache
    // file would "match" a bare "[Pasted text #N]" -- so such a marker is never resolved from the cache
    // (ValidatePasteFile refuses it), and the adapters that would read the whole cache only to be
    // refused skip the read (ClaudeSpawn.cpp). Only the paste journal can back a bare marker.
    //   m       -- a marker from FindPasteMarkers (or one built by DraftCarry.h).
    //   returns -- true when its count is positive, i.e. a cache file could validate against it.
    //   throws  -- nothing.
    inline bool PasteCacheCanAnswer(const PasteMarker& m) noexcept
    {
        return m.lines > 0;
    }

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
        //   [Pasted text #N]               -- the BARE form, read as M == 0 (a paste with no line break)
        //   [...Truncated text #N +M lines...]
        // The bare form matters to every reader of a box: without it a long one-line paste reads as
        // plain typed text -- the verified send called its OWN collapsed fill foreign and refused it,
        // and a draft holding one was re-typed as its literal label.
        //   line -- one '\n'-free line of a draft.
        //   pos  -- the offset of the candidate '['.
        //   m    -- receives truncated/index/lines/charBegin/charEnd on success; untouched otherwise.
        //   returns -- true on an exact match of one of the forms above.
        //   throws  -- nothing.
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
            if (digits == 0)
            {
                return false;
            }
            // The BARE collapsed form ends right after N. Claude writes the count only for a paste that
            // has line breaks, and a truncated marker always carries its count, so only the collapsed kind
            // may close here.
            if (!trunc && p < line.size() && line[p] == L']')
            {
                m.truncated = false;
                m.index = idx;
                m.lines = 0;
                m.charBegin = pos;
                m.charEnd = p + 1;
                return true;
            }
            if (!starts(L" +", p))
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
        if (!PasteCacheCanAnswer(m))
        {
            return false; // a zero count (the bare form included) validates nothing honestly
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

    // Agentmaster (PENDING_INPUT.md §10 -- the restore RE-FILL): a whole-draft expansion verdict.
    // complete == true means `text` holds the draft with EVERY marker expanded to its verified cache
    // content (a marker-free draft is trivially complete, text == draft verbatim); complete == false
    // means the expansion was REFUSED as a whole and `text` is empty -- the caller must NOT re-type
    // the draft (a literal "[Pasted text #N +M lines]" label pasted back into a fresh session is
    // indistinguishable from a real placeholder on screen but submits as junk text, silently dropping
    // the content behind it -- the DRAFT SWAP's §9 refusal rule, applied here for the same reason).
    struct DraftPasteExpansion
    {
        bool complete{ false };
        int expandedCount{ 0 }; // markers actually substituted (0 for a marker-free draft)
        std::wstring text; // the fill text ("" when refused)
    };

    // Expand EVERY marker in a draft against the supplied cache file set, ALL-OR-REFUSE:
    //   * no markers            -> complete, text == draft (the common case costs one scan);
    //   * any marker unresolved / ambiguous (ResolvePasteMarkers refused)      -> refuse the whole;
    //   * two markers sharing ONE draft line (expansion is line-replacing, so expanding either
    //     would eat the other -- vanishingly rare, honest to keep the placeholders)  -> refuse;
    //   * any single expansion refused (ExpandPasteMarker's own rules)         -> refuse the whole.
    // Markers are substituted LAST-first: replacing draft line k rewrites lines >= k only, so every
    // EARLIER marker's recorded draftLine/fragments stay valid against the partially-expanded text
    // (FindPasteMarkers emits them in ascending line order). A degenerate expansion whose result is
    // the empty string (a one-line draft whose paste is a single empty line) reads as a refusal --
    // safe, the placeholder form is kept. PURE (the impure adapter feeds the cache files in).
    inline DraftPasteExpansion ExpandDraftPasteMarkers(std::wstring_view draft, const std::vector<PasteFileText>& files)
    {
        DraftPasteExpansion out;
        const auto markers = FindPasteMarkers(draft);
        if (markers.empty())
        {
            out.complete = true;
            out.text.assign(draft);
            return out;
        }
        const auto res = ResolvePasteMarkers(markers, files);
        for (size_t i = 0; i < markers.size(); ++i)
        {
            if (i >= res.size() || !res[i].resolved)
            {
                return DraftPasteExpansion{}; // any unresolved marker refuses the whole draft
            }
            for (size_t j = i + 1; j < markers.size(); ++j)
            {
                if (markers[j].draftLine == markers[i].draftLine)
                {
                    return DraftPasteExpansion{}; // two markers on one line: expanding either eats the other
                }
            }
        }
        std::wstring cur{ draft };
        for (size_t k = markers.size(); k-- > 0;)
        {
            const PasteFileText* file = nullptr;
            for (const auto& f : files)
            {
                if (f.name == res[k].fileName)
                {
                    file = &f;
                    break;
                }
            }
            if (!file)
            {
                return DraftPasteExpansion{}; // resolution named a file the set no longer carries
            }
            auto expanded = ExpandPasteMarker(cur, markers[k], file->text);
            if (expanded.empty())
            {
                return DraftPasteExpansion{}; // this marker's expansion refused -> refuse the whole
            }
            cur = std::move(expanded);
            ++out.expandedCount;
        }
        out.complete = true;
        out.text = std::move(cur);
        return out;
    }
}
