// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the PENDING-INPUT detector (see doc/agentmaster/PENDING_INPUT.md).
//
// Claude Code renders an INPUT BOX at the bottom of its TUI where the user types the next prompt.
// Until the user presses Enter that text is a DRAFT -- it has not been submitted, so NO hook fires
// for it (UserPromptSubmit fires on submit). The draft is therefore the ONE session fact the
// hook/transcript pipeline can never carry: the only way to know a tab is holding an unsent message
// is to read it out of the rendered terminal buffer. This unit is the PURE brain that, given the
// bottom region of the buffer, extracts that draft.
//
// It is deliberately free of WinRT / TextBuffer / ICU so it can be unit-tested standalone
// (AgentMaster/tests) and HEADER-ONLY (like PromptAnchor.h / ProfileBootstrap.h) so the ControlCore
// adapter (a separate Microsoft.Terminal.Control DLL) and the test harness share it with no
// cross-project source/link. The buffer/control layer is a THIN adapter: it copies the last N row
// texts under the read-lock into a vector and calls DetectPendingInput once.
//
// SOURCE STAYS PURE-ASCII (the PromptAnchor.h convention): markers/box chars are written as \u
// escapes and referred to by code point in comments -- this header is included by TUs that compile
// WITHOUT /utf-8, where a raw multibyte literal would mojibake-decode (and a raw glyph anywhere would
// trip C4828).
//
// NOTE ON STATE vs FACT (Correctness Rule #7/#13): the project never screen-scrapes for session
// STATE (Running/Waiting/...) -- the Ink TUI repaints constantly, so state is hook/transcript-driven.
// This is NOT state: it is a transient DRAFT fact (analogous to Claude's presence heartbeat), read
// out-of-band and never authoritative for anything but "this tab has an unsent message". The read is
// strictly read-only -- it never writes to the shell.
//
// HOW THE BOX IS IDENTIFIED (PENDING_INPUT.md section 2). The input box renders as a U+276F prompt
// line WRAPPED by U+2500 horizontal rules. With ASCII stand-ins (> for U+276F, --- for the U+2500
// rule):
//
//     ----------------------------------------------------------   (top rule)
//     > first line of the draft, possibly long and soft-wrapped
//       a continuation line (2-space indent aligning under "> ")
//     ----------------------------------------------------------   (bottom rule)
//
// Two facts pin it down (both required, per the user's guidance):
//   1. it is the BOTTOM-MOST line whose first non-space glyph is the prompt marker U+276F that ALSO
//      satisfies fact 2 (candidates failing fact 2 -- e.g. a U+276F line the user PASTED into the
//      draft body, or a menu cursor -- are skipped and the scan continues UPWARD), and
//   2. it is WRAPPED by rule rows (a plain rule directly above the prompt line, and one below the
//      body). A box rule is ANCHORED: it starts at/near column 0 and spans the width -- which is what
//      separates it from a floating right-column pane border (see the menu note below).
// Fact 2 is what separates the input box from (a) a SENT prompt -- also U+276F-prefixed, but rendered
// INLINE in the scrollback with no surrounding box -- and (b) a MENU selection cursor (Claude marks
// the highlighted choice with U+276F too, e.g. "> 1. Yes", but a menu is not rule-wrapped; the line
// above it is the question text, not a rule). Without fact 2 both would false-positive.
//
// CROSS-VERSION FACTS (measured live, 2026-07-23 -- see PENDING_INPUT.md section 2a):
//   * The separator after the marker is U+00A0 NBSP, not a plain space ("❯ text"). Verified
//     on Claude Code 2.1.217 + 2.1.218 live buffers and on 1,088 historical [pending] log lines
//     spanning both profiles -- 100% NBSP. The post-marker strip therefore accepts ANY single
//     whitespace (IsWs), never just L' '.
//   * The box rules are FULL-WIDTH and FLUSH-LEFT (column 0) in every observed render. The
//     AskUserQuestion side-by-side PREVIEW menu, by contrast, floats its preview pane's border
//     (e.g. "╭─..╮") mid-row in the right column -- which satisfies the box-drawing
//     density test but NOT the column anchor. Six live false positives ("❯ 2. <option label>
//     <pad> │ <preview>" extracted as a 3,474-char "draft") came from exactly that; the anchor
//     requirement + the menu-shape rejector below kill the class.
//   * A menu OPTION row reads "N. <label>" right after the marker and carries the U+2502 column
//     separator with content after it. A row shaped like that is rejected as a candidate even when
//     rule-wrapped (a real draft starting "2. " is only rejected if it ALSO carries a mid-row U+2502
//     column separator on its FIRST line -- deliberately narrow, documented in the tests).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // The glyph Claude Code renders at the start of the input line (and a SENT prompt). U+276F is the
    // primary heavy right-angle ornament; U+203A a secondary single right-angle quote.
    inline constexpr wchar_t kPendingPromptMarkerA = L'\u276F';
    inline constexpr wchar_t kPendingPromptMarkerB = L'\u203A';

    // The resolved draft for one session's input box.
    struct PendingInputDraft
    {
        bool boxFound{ false }; // the bottom-most U+276F input box (rule-wrapped) was located at all
        std::wstring text; // the UNSENT draft (empty => the box is empty, or no box was found)
        // Indices INTO the rows passed to DetectPendingInput -- diagnostic, and the seam for an
        // attribute-aware refinement (reading the prompt line's cells to skip a DIM placeholder).
        int caretRow{ -1 }; // the U+276F prompt line
        int bottomRuleRow{ -1 }; // the box's bottom rule
    };

    namespace pending_detail
    {
        // A box-drawing code point (U+2500..U+257F): light/heavy/double rules, corners, junctions.
        inline constexpr bool IsBoxDrawing(wchar_t c) noexcept
        {
            return c >= 0x2500 && c <= 0x257F;
        }

        inline constexpr bool IsWs(wchar_t c) noexcept
        {
            // ASCII whitespace + the common INVISIBLE Unicode spaces. The latter matter because a focused
            // input box's cursor / its padding can be a non-ASCII space (e.g. NBSP U+00A0) rather than a
            // plain space — without trimming those, an EMPTY box reads as a draft (a false "pending").
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v' ||
                   c == 0x00A0 || // no-break space
                   c == 0x1680 || // ogham space mark
                   (c >= 0x2000 && c <= 0x200A) || // en quad .. hair space
                   c == 0x2028 || c == 0x2029 || // line / paragraph separator
                   c == 0x202F || c == 0x205F || // narrow no-break space, medium math space
                   c == 0x3000 || // ideographic space
                   c == 0x200B || c == 0x200C || c == 0x200D || // zero-width space / non-joiner / joiner
                   c == 0xFEFF; // zero-width no-break space / BOM
        }

        // A focused terminal renders its CURSOR as a block, which Claude Code's Ink input box emits into
        // the cell under the cursor — so the "white box" after "❯ " in an EMPTY box, and the glyph at the
        // tail of a non-empty draft, is the CURSOR, not typed text. Treat the block-element glyphs
        // (U+2580..U+259F — full block █ U+2588, the shade blocks U+2591..U+2593, the half blocks, etc.)
        // as a cursor artifact so a lone trailing block doesn't read as content. (A real draft's text
        // sits to the LEFT of its trailing cursor; only the trailing run is stripped.)
        inline constexpr bool IsCursorArtifact(wchar_t c) noexcept
        {
            return c >= 0x2580 && c <= 0x259F;
        }

        inline constexpr bool IsIgnorable(wchar_t c) noexcept
        {
            return IsWs(c) || IsCursorArtifact(c);
        }

        // Right-trim trailing whitespace (rows arrive padded to the buffer width).
        inline std::wstring_view RTrim(std::wstring_view s) noexcept
        {
            size_t e = s.size();
            while (e > 0 && IsWs(s[e - 1]))
            {
                --e;
            }
            return s.substr(0, e);
        }

        inline size_t LeadingSpaces(std::wstring_view s) noexcept
        {
            size_t i = 0;
            while (i < s.size() && s[i] == L' ')
            {
                ++i;
            }
            return i;
        }

        // Leading run of ANY whitespace (space, NBSP, ...). The caret/rule scans use this rather than
        // LeadingSpaces so a render that pads with a non-ASCII space (the NBSP separator is already
        // proven live) can never hide the marker or un-anchor a rule.
        inline size_t LeadingWs(std::wstring_view s) noexcept
        {
            size_t i = 0;
            while (i < s.size() && IsWs(s[i]))
            {
                ++i;
            }
            return i;
        }

        // A VERTICAL border/frame glyph (U+2502 light, U+2503 heavy, U+2551 double). Two uses:
        //   * a future FRAMED input box ("│ ❯ text │") -- the caret scan skips ONE leading vertical
        //     border so the marker is still found (extraction of the side borders stays best-effort);
        //   * the AskUserQuestion PREVIEW menu's column separator ("❯ 1. label  │ preview") -- the
        //     menu-shape rejector keys on a U+2502 with content after it (IsMenuOptionCaret).
        inline constexpr bool IsVerticalBorder(wchar_t c) noexcept
        {
            return c == 0x2502 || c == 0x2503 || c == 0x2551;
        }

        // Menu-shape rejector (the live false-positive class, PENDING_INPUT.md section 2a): a caret
        // candidate whose post-marker text reads like a NUMBERED OPTION ("N. label") AND whose raw row
        // carries a U+2502 COLUMN SEPARATOR (a vertical border with at least one non-whitespace char
        // after it) is an AskUserQuestion side-by-side preview menu's selection cursor, not the input
        // box. Both cues are required: a real draft may start "2. fix the tests" (no separator), and a
        // framed box's TRAILING "│" has nothing after it (not a column separator).
        //   postMarker = the candidate row's text AFTER the marker glyph (separator not yet stripped)
        //   rawRow     = the full (right-trimmed) row
        inline bool IsMenuOptionCaret(std::wstring_view rawRow, std::wstring_view postMarker) noexcept
        {
            // "N. " (1-3 digits, a dot, then whitespace-or-end) right after the marker + separator.
            size_t p = 0;
            while (p < postMarker.size() && IsWs(postMarker[p]))
            {
                ++p;
            }
            size_t digits = 0;
            while (p < postMarker.size() && postMarker[p] >= L'0' && postMarker[p] <= L'9' && digits < 4)
            {
                ++p;
                ++digits;
            }
            if (digits == 0 || digits > 3 || p >= postMarker.size() || postMarker[p] != L'.')
            {
                return false;
            }
            ++p;
            if (p < postMarker.size() && !IsWs(postMarker[p]))
            {
                return false; // "2.5x" etc. -- not an option number
            }
            // A column-separator U+2502-family glyph with real content after it on the same row.
            for (size_t i = 0; i < rawRow.size(); ++i)
            {
                if (IsVerticalBorder(rawRow[i]))
                {
                    for (size_t j = i + 1; j < rawRow.size(); ++j)
                    {
                        if (!IsWs(rawRow[j]))
                        {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        inline bool AllWhitespace(std::wstring_view s) noexcept
        {
            for (const auto c : s)
            {
                if (!IsWs(c))
                {
                    return false;
                }
            }
            return true;
        }
    }

    // Is `row` one of the input box's U+2500 rule borders? A rule is DOMINATED by box-drawing
    // characters: at least kMinRuleRun of them AND >= 80% of the row's visible (non-space) characters
    // are box-drawing. The 80% floor rejects ordinary text and a LABELED divider (e.g. "--- 3 files
    // ---", which Claude uses elsewhere) while accepting a plain rule or a corner-framed one.
    inline bool IsPendingRuleRow(std::wstring_view row) noexcept
    {
        using namespace pending_detail;
        const auto t = RTrim(row);
        size_t box = 0;
        size_t nonSpace = 0;
        for (const auto c : t)
        {
            if (c == L' ')
            {
                continue;
            }
            ++nonSpace;
            if (IsBoxDrawing(c))
            {
                ++box;
            }
        }
        constexpr size_t kMinRuleRun = 6; // a real border spans the width; a few stray rule chars is not a rule
        return box >= kMinRuleRun && nonSpace > 0 && box * 5 >= nonSpace * 4; // box >= 0.8 * nonSpace
    }

    // Is `row` one of the INPUT BOX's rule borders specifically? The box's rules are FLUSH-LEFT
    // (column 0 in every observed render, 2.1.217/2.1.218 + the whole [pending] log history); a small
    // indent tolerance absorbs a future padded render. The anchor is what rejects a FLOATING rule
    // fragment -- the AskUserQuestion preview pane's border ("<~30 columns of spaces>╭─..╮")
    // is box-drawing-dominated, so IsPendingRuleRow alone reads it as a rule and the option row below
    // it as the caret (the live 3,474-char false-draft class). A floating fragment starts mid-row;
    // the box's rules never do.
    inline constexpr size_t kPendingRuleAnchorMaxIndent = 4;
    inline bool IsAnchoredPendingRuleRow(std::wstring_view row) noexcept
    {
        return pending_detail::LeadingWs(row) <= kPendingRuleAnchorMaxIndent && IsPendingRuleRow(row);
    }

    // Detect the UNSENT draft in Claude Code's input box, given the BOTTOM region of the terminal
    // buffer (`rows`, top-to-bottom -- each entry is one buffer row's text). The adapter passes a
    // bounded window of the last rows (the box always sits at the buffer bottom, independent of the
    // user's scroll position). Returns the draft text (empty => empty box or no box). PURE.
    inline PendingInputDraft DetectPendingInput(const std::vector<std::wstring>& rows)
    {
        using namespace pending_detail;
        PendingInputDraft out;
        const int n = static_cast<int>(rows.size());
        if (n == 0)
        {
            return out;
        }

        // (1) The BOTTOM-MOST line whose first non-whitespace glyph (after at most ONE leading
        // vertical border char, for a future framed render) is the prompt marker U+276F / U+203A --
        // that ALSO passes the box checks below. A candidate that fails (a menu cursor, a U+276F line
        // the user PASTED into the draft body, a sent prompt in scrollback) does NOT abort detection:
        // the scan CONTINUES UPWARD to the next marker row, so a draft whose body itself contains a
        // "❯"-leading line (a pasted transcript snippet) still resolves to the true caret above it.
        constexpr int kMaxBodyRows = 200;
        int caret = -1;
        int bottom = -1;
        size_t markerPos = 0; // index of the marker glyph within the caret row (after ws/border skip)
        for (int i = n - 1; i >= 0; --i)
        {
            const auto t = RTrim(rows[i]);
            size_t pos = LeadingWs(t);
            if (pos < t.size() && IsVerticalBorder(t[pos]))
            {
                ++pos; // a framed box's left border ("│ ❯ ..."); one ws after it may pad
                if (pos < t.size() && IsWs(t[pos]))
                {
                    ++pos;
                }
            }
            if (pos >= t.size())
            {
                continue;
            }
            const auto c = t[pos];
            if (c != kPendingPromptMarkerA && c != kPendingPromptMarkerB)
            {
                continue;
            }

            // (2a) A rule DIRECTLY above the prompt line (within 2 rows, tolerating one intervening
            // blank row). Bail on a NON-blank, NON-rule line above -- that is a menu's question text
            // (or the draft body above a pasted "❯" line), not a box top. The rule must be ANCHORED
            // (flush-left): a preview pane's floating border fragment does not count.
            bool hasTop = false;
            for (int j = i - 1; j >= 0 && j >= i - 2; --j)
            {
                if (IsAnchoredPendingRuleRow(rows[j]))
                {
                    hasTop = true;
                    break;
                }
                if (!RTrim(rows[j]).empty())
                {
                    break; // a real line (not a rule, not blank) above => not the input box
                }
            }
            if (!hasTop)
            {
                continue; // not the box -- keep scanning upward
            }

            // The menu-shape rejector: "❯ N. label │ preview" is an AskUserQuestion side-by-side
            // preview menu's selection cursor even when a (mis-)anchored rule sits above it.
            if (IsMenuOptionCaret(t, t.substr(pos + 1)))
            {
                continue;
            }

            // (2b) A rule BELOW the body (scan down from the prompt line, capped so a pathological
            // buffer can't run away -- the visible box is at most a viewport tall anyway).
            int b = -1;
            for (int j = i + 1; j < n && j <= i + kMaxBodyRows; ++j)
            {
                if (IsAnchoredPendingRuleRow(rows[j]))
                {
                    b = j;
                    break;
                }
            }
            if (b < 0)
            {
                continue; // an open prompt line with no closing rule => not the input box
            }

            caret = i;
            bottom = b;
            markerPos = pos;
            break;
        }
        if (caret < 0)
        {
            return out; // no rule-wrapped input box visible at all
        }

        out.boxFound = true;
        out.caretRow = caret;
        out.bottomRuleRow = bottom;

        // (3) Extract the body rows [caret, bottom): strip the marker (+ ONE following whitespace --
        // Claude 2.1.x renders U+00A0 NBSP there, not a plain space; accepting any single IsWs char is
        // the cross-version-safe form of "the single space after the marker") from the first line and
        // the 2-char continuation indent (space or NBSP) from the rest, then join with '\n'. (This is
        // a best-effort reconstruction for display/preview -- exact fidelity is not load-bearing; the
        // load-bearing output is "is there any non-whitespace content".)
        std::vector<std::wstring> lines;
        lines.reserve(static_cast<size_t>(bottom - caret));
        for (int k = caret; k < bottom; ++k)
        {
            const auto t = RTrim(rows[k]);
            if (k == caret)
            {
                size_t p = markerPos + 1; // past leading ws (+ a framed border) + the marker glyph
                if (p < t.size() && IsWs(t[p]))
                {
                    ++p; // the single separator Claude puts after the marker (NBSP on 2.1.x)
                }
                lines.emplace_back(t.substr(p));
            }
            else
            {
                size_t p = 0;
                while (p < 2 && p < t.size() && (t[p] == L' ' || t[p] == 0x00A0))
                {
                    ++p; // the 2-char continuation indent (space; NBSP-tolerant)
                }
                lines.emplace_back(t.substr(p));
            }
        }

        // Drop trailing blank lines (the box reserves a little vertical slack below the text).
        while (!lines.empty() && AllWhitespace(lines.back()))
        {
            lines.pop_back();
        }

        std::wstring text;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (i != 0)
            {
                text.push_back(L'\n');
            }
            text += lines[i];
        }
        // Strip the trailing CURSOR (+ any trailing whitespace): an EMPTY focused box renders the block/
        // space cursor right after "> " (the "white box" the user sees), and a non-empty draft carries
        // the cursor at its very tail too — neither is real content. Stripping the trailing ignorable run
        // makes an empty box collapse to "" (no false "pending"), while a real draft keeps its text (its
        // content sits to the LEFT of the cursor). IsIgnorable = whitespace (incl. non-ASCII spaces) + a
        // block-element cursor glyph.
        while (!text.empty() && IsIgnorable(text.back()))
        {
            text.pop_back();
        }
        out.text = std::move(text);
        return out;
    }
}
