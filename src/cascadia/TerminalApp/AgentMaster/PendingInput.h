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

    // Agentmaster (DELIVERY.md sect. 11 RC8 / DELIVERY_PLAN.md R5 -- the TRI-STATE box read): the
    // detector's VERDICT, kept distinct from the draft text because "" is fatally ambiguous -- "box
    // present and empty" (safe to paste into) and "no box visible at all" (a menu/dialog replaced it,
    // the box is taller than the read window, a mid-repaint frame) used to both read as an empty
    // string, and every consumer treated the empty string as the SAFE case. Incident 3 (a 13-char
    // prompt merged with ~4.2K of invisible content) and the sect. 9 dialog-consumed delivery are both
    // ""-misread shapes. Int-typed values are a wire contract: the verdict crosses the ControlCore DLL
    // boundary as an Int32 (ReadPendingInputBoxState / ReadInputBoxProbe) and rides
    // SessionInfo::pendingBoxState -- never renumber, only append.
    //
    //   Unknown  -- no read has happened (a dormant tab, a not-yet-initialized terminal). Consumers
    //               must treat it as "no information", never as a hold.
    //   NoBox    -- rows were read and NO rule-wrapped U+276F input box was found. At rest this means
    //               a modal is parked over the box (the workspace-trust dialog), the render drifted,
    //               or the box's caret line is above the read window (a very tall draft).
    //   Empty    -- the box was found and holds no text: VERIFIED empty, safe to place into.
    //   Draft    -- the box was found and holds an unsent draft (PendingInputDraft::text).
    //   MenuOpen -- no input box, and the bottom-most caret row reads as a MENU selection cursor
    //               (an AskUserQuestion / permission menu). A paste now would feed the MENU, and a
    //               lone Enter would SELECT the highlighted option -- both must refuse.
    enum class InputBoxState : int32_t
    {
        Unknown = 0,
        NoBox = 1,
        Empty = 2,
        Draft = 3,
        MenuOpen = 4,
    };

    // The resolved draft for one session's input box.
    struct PendingInputDraft
    {
        bool boxFound{ false }; // the bottom-most U+276F input box (rule-wrapped) was located at all
        InputBoxState state{ InputBoxState::NoBox }; // the tri-state verdict (see above); callers with no rows at all report Unknown themselves
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
        // "N. " (1-3 digits, a dot, then whitespace-or-end) right after the marker + separator --
        // the NUMBERED-OPTION shape every Claude menu row carries ("> 1. Yes"). On its own this is
        // only a VERDICT signal (a caret candidate that fails the box checks AND reads like this is
        // most plausibly an open menu -- InputBoxState::MenuOpen); candidate REJECTION additionally
        // requires the preview menu's column separator (IsMenuOptionCaret below), because a real
        // draft may legitimately start "2. fix the tests".
        inline bool IsNumberedOptionShape(std::wstring_view postMarker) noexcept
        {
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
            return p >= postMarker.size() || IsWs(postMarker[p]); // "2.5x" etc. -- not an option number
        }

        inline bool IsMenuOptionCaret(std::wstring_view rawRow, std::wstring_view postMarker) noexcept
        {
            if (!IsNumberedOptionShape(postMarker))
            {
                return false;
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
        bool sawMenuCaret = false; // a rejected candidate READ like a menu row -> the MenuOpen verdict when no real box exists
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
                // A rejected caret whose post-marker text reads "N. <label>" is a PLAIN menu's
                // selection cursor (question text above, no rule): the AskUserQuestion / permission
                // menu shape. Only a VERDICT signal (R5 -- MenuOpen when no real box exists below);
                // rejection itself stays exactly as narrow as before.
                if (IsNumberedOptionShape(t.substr(pos + 1)))
                {
                    sawMenuCaret = true;
                }
                continue; // not the box -- keep scanning upward
            }

            // The menu-shape rejector: "❯ N. label │ preview" is an AskUserQuestion side-by-side
            // preview menu's selection cursor even when a (mis-)anchored rule sits above it.
            if (IsMenuOptionCaret(t, t.substr(pos + 1)))
            {
                sawMenuCaret = true;
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
            // No rule-wrapped input box visible at all. The verdict tells the two shapes apart (R5):
            // a menu-shaped caret among the rejected candidates => a menu is (most plausibly) open
            // and would EAT a paste / SELECT on a lone Enter; anything else is a bare NoBox (modal /
            // render drift / box taller than the window). Both are ADVISORY -- consumers act on them
            // only for a session at rest (a running turn legitimately scrolls menu echoes around).
            out.state = sawMenuCaret ? InputBoxState::MenuOpen : InputBoxState::NoBox;
            return out;
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
        out.state = out.text.empty() ? InputBoxState::Empty : InputBoxState::Draft;
        return out;
    }

    // ---- The "current prompt" PICK: a live read vs the remembered draft (PENDING_INPUT.md sect. 8) ----
    //
    // Every "Copy Current Prompt" menu item has TWO possible sources for a session's unsent draft, and
    // this is the ONE rule that chooses between them, so the three copy menus (the per-tab overlay's,
    // the Triage Board / Explorer-tree Copy submenu, the WT tab menu's "Copy >") can never drift:
    //
    //   * LIVE -- DetectPendingInput run against that tab's terminal buffer RIGHT NOW
    //     (ControlCore::ReadPendingInputDraft, via the hosting window's TermControl). Authoritative
    //     whenever it yields text: it is the box exactly as rendered this instant, with none of the
    //     scan lane's tick lag. It is simply UNAVAILABLE in several ordinary cases -- a session whose
    //     tab lives in ANOTHER window (a different UI thread), a dormant window-restored tab whose
    //     claude has not started (no buffer), a closed session, or a read that threw -- and the caller
    //     passes "" for all of them (the read is wrapped; a failure is never fatal, just empty).
    //   * REMEMBERED -- SessionInfo::pendingInput: what the observer's scan lane last recorded (at most
    //     one liveness tick old) or, across a restart, the PERSISTED staleness-labeled memory (sect. 5).
    //
    // The rule: a non-empty LIVE read wins; otherwise fall back to the remembered value. A live read
    // that comes back EMPTY deliberately does NOT erase the fallback -- the "3 dots" indicator is
    // driven by the remembered value through the 2-tick clear debounce, so for as long as the tab/card
    // still says "this session holds an unsent message" the copy must hand over that message instead
    // of silently copying nothing. Whitespace-only counts as empty on both sides (a focused empty box
    // can render as padding alone; DetectPendingInput already strips its cursor).
    struct CurrentPromptPick
    {
        std::wstring text; // the chosen draft ("" => neither source had one; the copy is then a no-op)
        bool fromLive{ false }; // true => text came from the live buffer read (else remembered/persisted)
    };

    inline CurrentPromptPick PickCurrentPromptText(std::wstring_view live, std::wstring_view remembered)
    {
        CurrentPromptPick pick;
        if (!pending_detail::AllWhitespace(live))
        {
            pick.text.assign(live);
            pick.fromLive = true;
            return pick;
        }
        if (!pending_detail::AllWhitespace(remembered))
        {
            pick.text.assign(remembered);
        }
        return pick;
    }

    // ---- The DRAFT vs THE COMPOSE BOX (PENDING_INPUT.md sect. 8b) -----------------------------
    //
    // Sect. 8a pulls a session's unsent draft into the Manager's Auto-Testing compose box when that
    // box is EMPTY. The follow-on case is what happens once it ISN'T: the user pulls a prompt in
    // here, goes BACK to the terminal tab, keeps editing it THERE, then refocuses this box. Now two
    // texts exist and the UI has to decide -- without ever destroying something the user typed --
    // which of them is "the prompt", so this is the pure rule for comparing them.
    //
    // Both sides are normalized for the comparison ONLY (NormalizeForCompare): newline flavors are
    // folded (a UWP TextBox stores '\r' for a typed newline while the detector emits '\n', so a
    // multi-line draft would otherwise NEVER compare equal to the same text in the box) and trailing
    // whitespace is dropped (render slack / a trailing cursor cell is never intent). Interior text is
    // untouched -- leading indentation IS content.
    //
    //   NoDraft       the session holds nothing unsent            -> no offer
    //   BoxEmpty      the box is empty (whitespace-only counts)   -> the sect. 8a pull
    //   Same          the two are the same text                   -> nothing to do
    //   Continuation  the draft STARTS WITH the box, and is longer -> the box is a strict PREFIX of
    //                 the draft: the user kept typing in the terminal, so extending the box to the
    //                 full draft is NON-DESTRUCTIVE (nothing in the box is lost). The one relation
    //                 that may be applied automatically (autoExtend).
    //   BoxAhead      the box STARTS WITH the draft, and is longer -> the box already contains
    //                 everything the draft has plus the user's own addition. NEVER offered: taking
    //                 the draft here would DELETE that addition, and gain nothing.
    //   Divergent     anything else                               -> offered only when the two are
    //                 demonstrably the same prompt evolved (below).
    //
    // OFFERING A DIVERGENT DRAFT IS DESTRUCTIVE (it replaces composed text), so it is gated on the
    // two being RELATED, per the user's rule -- "only if it [is] contained within the new prompt or
    // similar by at least 80% (and >50 chars)":
    //   * the box appears VERBATIM somewhere inside the draft (containment, not just a prefix --
    //     e.g. the user prepended "please " in the terminal): nothing in the box is lost, so this
    //     needs no length floor; or
    //   * both texts are longer than kDraftSimilarityMinChars AND score at least
    //     kDraftSimilarityPercent similar. The length floor is what keeps a short string's ratio
    //     from being noise ("fix it" vs "fix up" would score high on nothing but shortness).
    // Everything else stays hidden: an UNRELATED draft is not an alternative version of what is in
    // the box, so silently offering to overwrite it would be the wrong question to ask.
    enum class DraftVsBox
    {
        NoDraft, // the session holds no unsent draft at all
        BoxEmpty, // the compose box is empty (whitespace-only counts as empty)
        Same, // the box already holds exactly the draft
        Continuation, // the draft starts with the box and is longer (a strict prefix -> safe to extend)
        BoxAhead, // the box starts with the draft and is longer (the box is ahead -> never overwrite)
        Divergent, // neither is a prefix of the other
    };

    namespace pending_detail
    {
        // Normalize for COMPARISON only: fold every newline flavor (\r\n, a lone \r, \n) to '\n' and
        // drop trailing whitespace. The newline fold is load-bearing, not cosmetic: a UWP TextBox
        // reports a typed newline as '\r' while DetectPendingInput joins body rows with '\n', so
        // without it every multi-line draft would read as Divergent from an identical box.
        inline std::wstring NormalizeForCompare(std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == L'\r')
                {
                    if (i + 1 < s.size() && s[i + 1] == L'\n')
                    {
                        ++i; // a CRLF pair is ONE newline
                    }
                    out.push_back(L'\n');
                }
                else
                {
                    out.push_back(s[i]);
                }
            }
            out.resize(RTrim(out).size()); // (the view is consumed before the resize)
            return out;
        }

        // The relation, over ALREADY-NORMALIZED texts (so a caller that normalized once for the
        // similarity/containment tests does not pay for it twice).
        inline DraftVsBox RelationOfNormalized(std::wstring_view box, std::wstring_view draft)
        {
            if (AllWhitespace(draft))
            {
                return DraftVsBox::NoDraft; // "no draft" outranks "empty box": there is nothing to offer
            }
            if (AllWhitespace(box))
            {
                return DraftVsBox::BoxEmpty;
            }
            if (box == draft)
            {
                return DraftVsBox::Same;
            }
            if (draft.size() > box.size() && draft.compare(0, box.size(), box) == 0)
            {
                return DraftVsBox::Continuation;
            }
            if (box.size() > draft.size() && box.compare(0, draft.size(), draft) == 0)
            {
                return DraftVsBox::BoxAhead;
            }
            return DraftVsBox::Divergent;
        }
    }

    // The similarity floor for offering a DIVERGENT draft, and the minimum length either text must
    // exceed before a ratio is even consulted (the user's "similar by at least 80% (and >50 chars)").
    inline constexpr size_t kDraftSimilarityMinChars = 50;
    inline constexpr int kDraftSimilarityPercent = 80;

    // How similar are two texts, as a percentage of the LONGER one: the share they agree on at their
    // EDGES -- their common prefix plus the common suffix of what is left after it.
    //
    // Deliberately NOT an edit distance, for two reasons that both matter here:
    //   * COST. This runs on the compose box's TextChanged, i.e. once per keystroke, and a
    //     Levenshtein matrix over two multi-KB prompts is milliseconds of UI-thread work per key.
    //     This is one linear pass, no allocation.
    //   * DIRECTION OF ERROR. Editing only the middle of a text costs (prefix + suffix) exactly, so
    //     this value is a LOWER BOUND on the edit-distance ratio: distance <= max - (prefix + suffix),
    //     hence edge-share >= 80% IMPLIES 80% similar by edit distance. It can therefore never
    //     over-report, only under-report -- and under-reporting merely HIDES the button (the
    //     conservative direction: we never silently offer to overwrite composed text on a weak
    //     signal). The case it under-reports is an edit at BOTH ends of the text.
    // Two empty texts score 100; one empty scores 0. prefix + suffix can never exceed the shorter
    // text by construction (the suffix scan stops at the prefix), so the result is always 0..100.
    inline int DraftSimilarityPercent(std::wstring_view a, std::wstring_view b)
    {
        const size_t la = a.size();
        const size_t lb = b.size();
        if (la == 0 || lb == 0)
        {
            return (la == 0 && lb == 0) ? 100 : 0;
        }
        const size_t maxLen = la > lb ? la : lb;
        const size_t minLen = la > lb ? lb : la;
        size_t pre = 0;
        while (pre < minLen && a[pre] == b[pre])
        {
            ++pre;
        }
        size_t suf = 0;
        while (suf < minLen - pre && a[la - 1 - suf] == b[lb - 1 - suf])
        {
            ++suf;
        }
        return static_cast<int>(((pre + suf) * 100) / maxLen);
    }

    // Cost bound for the CONTAINMENT probe (the Divergent gate's first half). std::wstring::find is a
    // naive O(needle * haystack) scan, and EvaluateDraftPull runs on the compose box's TextChanged --
    // once per KEYSTROKE. Two 20 KB texts would be ~4e8 character comparisons per key, i.e. a visible
    // stall; unbounded per-tick work is precisely what froze the app once already (SUMMARY_JUMP.md
    // sect. 4a). The product cap keeps the probe to a few ms at worst while still admitting every
    // realistic case -- a short box inside a long draft (100 x 20,000 = 2e6) is exactly the shape
    // containment is for. Over the cap the probe is SKIPPED and the (linear) similarity gate below
    // decides alone, so the failure mode is the conservative one: the button is not offered.
    inline constexpr size_t kDraftContainmentMaxProduct = 4000000;

    struct DraftPullVerdict
    {
        DraftVsBox relation{ DraftVsBox::NoDraft };
        bool offer{ false }; // show the "pull it in" button (the draft is worth offering AND safe-ish to take)
        bool autoExtend{ false }; // the box is a strict PREFIX of the draft -> extending in place loses nothing
        int similarityPercent{ 0 }; // only computed (and only meaningful) when the Divergent ratio gate was consulted
    };

    // The ONE decision behind both compose-box behaviours (sect. 8b): the relation, whether to offer
    // the draft at all, and whether it may be taken AUTOMATICALLY. PURE. `box` is the compose box's
    // text and `draft` the session's unsent input-box draft (already picked by PickCurrentPromptText).
    inline DraftPullVerdict EvaluateDraftPull(std::wstring_view box, std::wstring_view draft)
    {
        DraftPullVerdict v;
        const auto nb = pending_detail::NormalizeForCompare(box);
        const auto nd = pending_detail::NormalizeForCompare(draft);
        v.relation = pending_detail::RelationOfNormalized(nb, nd);
        switch (v.relation)
        {
        case DraftVsBox::NoDraft: // nothing to offer
        case DraftVsBox::Same: // already in sync
        case DraftVsBox::BoxAhead: // the box is ahead -- taking the draft would delete the user's addition
            return v;
        case DraftVsBox::BoxEmpty:
            v.offer = true; // an empty box loses nothing (this is the sect. 8a click-pull, made explicit)
            return v;
        case DraftVsBox::Continuation:
            v.offer = true;
            v.autoExtend = true; // a strict prefix: the draft is the box plus what was typed in the terminal
            return v;
        default:
            break; // Divergent -- the gated case below
        }
        if (nb.size() * nd.size() <= kDraftContainmentMaxProduct && nd.find(nb) != std::wstring::npos)
        {
            v.offer = true; // the box appears verbatim INSIDE the draft: nothing in it would be lost
            return v;
        }
        if (nb.size() > kDraftSimilarityMinChars && nd.size() > kDraftSimilarityMinChars)
        {
            v.similarityPercent = DraftSimilarityPercent(nb, nd);
            v.offer = v.similarityPercent >= kDraftSimilarityPercent;
        }
        return v;
    }

    // Does `text` still BEGIN WITH `seed` (under the same normalization)? The compose box uses this to
    // ask "is what is in this box still the thing WE put there, possibly with more typed onto it" --
    // the provenance guard on the automatic extend. Nothing seeded (an empty `seed`) is always false:
    // text the user typed themselves is never something we may rewrite unasked, even when the draft
    // happens to be a superset of it. Equality counts as continuing.
    inline bool TextContinuesSeed(std::wstring_view text, std::wstring_view seed)
    {
        const auto ns = pending_detail::NormalizeForCompare(seed);
        if (ns.empty())
        {
            return false;
        }
        const auto nt = pending_detail::NormalizeForCompare(text);
        return nt.size() >= ns.size() && nt.compare(0, ns.size(), ns) == 0;
    }

    // The 6-way relation on its own (the same rule EvaluateDraftPull classifies with), for callers /
    // tests that only need "how do these two texts relate".
    inline DraftVsBox ClassifyDraftAgainstBox(std::wstring_view box, std::wstring_view draft)
    {
        return pending_detail::RelationOfNormalized(pending_detail::NormalizeForCompare(box),
                                                    pending_detail::NormalizeForCompare(draft));
    }

    // ---- The DRAFT SWAP (PENDING_INPUT.md sect. 9) -------------------------------------------
    //
    // Submitting a queued prompt into a box that already holds the user's UNSENT draft used to
    // MERGE the two: a bracketed paste lands at the cursor and the submit CR then sends draft +
    // prompt as one message the user never wrote (and never pressed Enter on). The swap empties
    // the box first, sends, and puts the draft back.
    //
    // THE PRIMARY CHANNEL IS CLAUDE'S OWN STASH -- Ctrl+S (0x13), a TOGGLE over one stash slot:
    //
    //   box has text  ->  Ctrl+S STASHES it and empties the box (replacing any earlier stash)
    //   box is empty  ->  Ctrl+S RESTORES the stash back into the box
    //
    // That is exactly the shape the swap needs, and both of our presses land on the right side of
    // it by construction: we only ever press to clear when the box HAS text, and only ever press to
    // restore when the box has been VERIFIED empty. It handles the whole box (not one line) and is
    // cursor-position independent, which is why it replaced the kill-ring pair as the default.
    //
    //   ! ONE SLOT, so a stash the USER had already made is discarded by ours. Unavoidable (the
    //     slot is not inspectable) and worth knowing; their live draft is never at risk, only a
    //     previously stashed one.
    //
    //   ! THE STASH IS NOT A DISCARD: claude AUTO-RESTORES a stashed draft into the input box
    //     ~0.4s after the NEXT message submission, whoever submits it (measured 2026-08-07 on a
    //     live PTY; it survives idle indefinitely, and the status line reads "stashed" while the
    //     slot is loaded). The swap is safe because it always CONSUMES the slot — its restore
    //     pops it back deliberately, or the auto-pop beats it and the verify-first restore leaves
    //     it be. But a clear that stashes and WALKS AWAY plants a scheduled re-paste: the mail
    //     button's MOVE-clear did exactly that, and the moved draft reappeared in the box seconds
    //     after its own queued copy was delivered (the "Second pass please" incident, session
    //     d373b992). A MOVE-style clear must use the DISCARD ladder below (DecideDraftDiscard),
    //     never this rung.
    //
    // THE FALLBACK CHANNEL is the line-editor kill-ring, used when the Ctrl+S rung is switched off
    // (AppSettings::draftSwapUseCtrlS) or does not empty the box:
    //
    //   Ctrl+U (0x15) -- kill INTO the kill-ring. Line-scoped and cursor-relative, so it can need a
    //                    press per line and may kill only part of a draft -- the reason it is no
    //                    longer the default.
    //   Ctrl+Y (0x19) -- yank the kill-ring back into the (now empty) box.
    //   DEL    (0x7F) -- the last-resort erase, one per remaining character, used only when a kill
    //                    press does not shrink the box at all. 0x7F rather than 0x08: xterm-family
    //                    terminals send DEL for the Backspace KEY, which is what a TUI line editor
    //                    binds; 0x08 is Ctrl+H and is commonly bound elsewhere.
    //
    // Whichever rung cleared the box is the one the UI lane restores through, and the restore is
    // VERIFIED against the draft that was read before clearing; a mismatch falls back to re-pasting
    // that text with BuildPromptFill (no submit CR). So every half degrades.
    //
    // NOTHING here is assumed to have worked: every rung is VERIFIED by re-reading the box through
    // DetectPendingInput, and a swap that cannot confirm an EMPTY box ABORTS without sending. An
    // unsupported keybinding therefore costs a deferred prompt, never a mangled message. PURE --
    // these only build the bytes and decide the next rung; the injecting, the re-reads and the
    // read-only window live in the UI lane (TerminalPage::_SubmitPromptWithDraftSwap).
    inline constexpr wchar_t kInputStashChar = L'\x13'; // Ctrl+S -- Claude's stash/unstash toggle
    inline constexpr wchar_t kInputKillLineChar = L'\x15'; // Ctrl+U -- kill the line into the kill-ring
    inline constexpr wchar_t kInputYankChar = L'\x19'; // Ctrl+Y -- yank it back
    inline constexpr wchar_t kInputBackspaceChar = L'\x7f'; // DEL -- the fallback erase

    // The SAME byte both stashes and restores -- it is one toggle, and which way it goes is decided
    // by whether the box has text. Callers must therefore never press it "just in case": pressing it
    // on an empty box un-stashes, and pressing it on a full box discards the stash.
    inline std::wstring BuildInputStash()
    {
        return std::wstring(1, kInputStashChar);
    }

    inline std::wstring BuildInputKill()
    {
        return std::wstring(1, kInputKillLineChar);
    }

    inline std::wstring BuildInputYank()
    {
        return std::wstring(1, kInputYankChar);
    }

    // A bounded run of backspaces. Capped so a mis-measured box (or a detector reporting a pasted
    // PLACEHOLDER far shorter than the content behind it) can never spray unbounded erases at a
    // live TUI; the ladder re-reads and re-decides between rounds anyway.
    inline constexpr size_t kMaxBackspacesPerRound = 4096;

    inline std::wstring BuildBackspaces(size_t count)
    {
        return std::wstring((count < kMaxBackspacesPerRound ? count : kMaxBackspacesPerRound), kInputBackspaceChar);
    }

    // How many times each rung may be tried before the swap gives up.
    //
    // The stash cap is 1 and MUST STAY 1: Ctrl+S is a toggle, so a second press on a box the first
    // press emptied would UN-stash and put the draft straight back. One press either works or the
    // ladder moves on.
    //
    // The kill cap is 3 because Ctrl+U is line-scoped -- a multi-line draft can need a press per
    // line -- while needing more than that already means the binding is not doing what we expect
    // and the erase rung should take over; two backspace rounds then cover a box whose length the
    // first round under-measured.
    inline constexpr uint32_t kMaxDraftStashPresses = 1;
    inline constexpr uint32_t kMaxDraftKillPresses = 3;
    inline constexpr uint32_t kMaxDraftBackspaceRounds = 2;

    enum class DraftClearAction
    {
        Done, // the box reads empty -- the swap may send
        Stash, // press Ctrl+S (stash the whole box aside) -- the default first rung
        Kill, // press Ctrl+U (kill into the kill-ring)
        Backspace, // erase `backspaces` characters, then re-read
        GiveUp, // the box will not empty -- ABORT the swap (send nothing, leave the draft alone)
    };

    struct DraftClearPlan
    {
        DraftClearAction action{ DraftClearAction::Done };
        size_t backspaces{ 0 }; // valid for Backspace
    };

    // What the caller has spent on this box so far. A struct rather than a parameter list so a new
    // rung cannot silently reorder an existing call site.
    struct DraftClearProgress
    {
        uint32_t stashPresses{ 0 };
        uint32_t killPresses{ 0 };
        uint32_t backspaceRounds{ 0 };
        bool shrank{ false }; // did the LAST action shrink the box? (false on the first call)
        bool useStash{ true }; // AppSettings::draftSwapUseCtrlS -- off => start at the kill rung
    };

    // PURE decision (the DecideAdvance pattern) for ONE step of the clear ladder. Its inputs are
    // the box as RE-READ right now plus what has already been spent, so the caller is a plain
    // "read -> decide -> act -> read again" loop holding no hidden state:
    //
    //   * empty box                     -> Done. Whitespace-only counts as empty (a focused empty
    //                                      box can render as padding alone), the same rule
    //                                      PickCurrentPromptText applies.
    //   * stash rung enabled + unspent  -> Stash. One press, whole box, cursor-independent.
    //   * kill presses left, and either
    //     none pressed yet or the last
    //     action SHRANK the box         -> Kill. A multi-line draft can take a press per line.
    //   * an action that did NOT shrink -> fall through to the erase rung: that binding is not
    //                                      doing what we assumed, so stop pressing it.
    //   * backspace rounds left         -> Backspace, one per remaining character plus a small
    //                                      margin for a trailing cursor cell in the read.
    //   * otherwise                     -> GiveUp.
    //
    // The ladder only ever moves FORWARD -- a spent rung is pinned by its own counter -- so no two
    // rungs can alternate and the loop always terminates.
    inline DraftClearPlan DecideDraftClear(std::wstring_view box, const DraftClearProgress& spent)
    {
        DraftClearPlan plan;
        if (pending_detail::AllWhitespace(box))
        {
            plan.action = DraftClearAction::Done;
            return plan;
        }
        if (spent.useStash && spent.stashPresses < kMaxDraftStashPresses &&
            spent.killPresses == 0 && spent.backspaceRounds == 0)
        {
            plan.action = DraftClearAction::Stash;
            return plan;
        }
        if (spent.backspaceRounds == 0 && spent.killPresses < kMaxDraftKillPresses &&
            (spent.killPresses == 0 || spent.shrank))
        {
            // The FIRST kill press is always allowed, whatever the stash rung did: a Ctrl+S that
            // changed nothing just means the binding is absent here, and Ctrl+U is the fair next
            // thing to try. `shrank` gates only the REPEAT presses (a multi-line draft), so a kill
            // that does nothing hands over to the erase rung instead of burning its budget.
            plan.action = DraftClearAction::Kill;
            return plan;
        }
        if (spent.backspaceRounds < kMaxDraftBackspaceRounds)
        {
            plan.action = DraftClearAction::Backspace;
            plan.backspaces = box.size() + 8; // + margin: a trailing cursor cell / a wide glyph
            return plan;
        }
        plan.action = DraftClearAction::GiveUp;
        return plan;
    }

    // ---- The MOVE-clear DISCARD ladder (PENDING_INPUT.md 8d) ---------------------------------
    //
    // The overlay MAIL button's plain click MOVES the box's draft into the Auto-Testing queue, so
    // its clear must DESTROY the box copy — which the swap's ladder above cannot promise: its
    // first rung is Ctrl+S, and the stash is not a discard (the auto-restore caution above). The
    // discard ladder never touches the stash. Its rungs, each verified live on a PTY (2026-08-07):
    //
    //   End + Ctrl+U      -- End (CSI F) first: Ctrl+U kills only caret -> line-start, so a caret
    //                        parked mid-line leaves the right-hand tail without it (measured).
    //                        Then one kill eats the line. A multi-line draft clears a line per
    //                        effective round, but the presses BETWEEN lines can legitimately
    //                        change nothing (measured on a 3-line draft: kill x2/x4 were no-ops),
    //                        so ONE stalled round is tolerated and only TWO consecutive no-change
    //                        rounds judge the rung dead.
    //   End + Backspaces  -- the binding-free fallback, End first each round: BLIND backspaces
    //                        stall permanently once the caret reaches position 0 with lines still
    //                        below it (measured — two full rounds changed nothing). Per-round End
    //                        + length+margin erases converge at ~a line per round; a
    //                        [Pasted text #N] placeholder deletes atomically on a single press.
    //
    // Both rungs are TRUE discards — a killed draft never returns across a submit (the kill-ring
    // is manual-only, Ctrl+Y) — and both are MID-TURN SAFE (measured: a streaming turn survives
    // untouched; Esc, by contrast, INTERRUPTS a running turn even with text in the box, which is
    // why it is not a rung). Same contract as the swap's ladder: pure decisions over a re-read
    // box, forward-only rungs pinned by their own counters, bounded termination.
    inline constexpr wchar_t kInputEscapeChar = L'\x1b';

    // End = CSI F -- the sequence ConPTY/xterm-family terminals send for the End key, and what
    // Claude's line editor binds (measured: it re-anchors the caret so the erase rungs can eat
    // the whole line; CSI 1;5F Ctrl+End is NOT recognized).
    inline std::wstring BuildInputEnd()
    {
        std::wstring s;
        s.push_back(kInputEscapeChar);
        s.push_back(L'[');
        s.push_back(L'F');
        return s;
    }

    // Round caps. Kill rounds run ~2 per line worst case (the measured no-op between lines) and
    // the detector's read window bounds the box at ~120 rows; backspace rounds eat ~a line each.
    // The stall limit is the real bail — the caps are the runaway backstop under the caller's
    // wall-clock budget.
    inline constexpr uint32_t kMaxDiscardKillRounds = 48;
    inline constexpr uint32_t kMaxDiscardBackspaceRounds = 24;
    inline constexpr uint32_t kDiscardStallLimit = 2; // consecutive NO-CHANGE rounds => the rung is dead

    enum class DraftDiscardAction
    {
        Done, // the box reads empty -- the move is complete
        EndKill, // press End then Ctrl+U (kill the caret's line, whole once End landed)
        EndBackspace, // press End then erase `backspaces` characters, then re-read
        GiveUp, // the box will not empty -- leave the draft (it is already queued; a Shift+Click end state)
    };

    struct DraftDiscardPlan
    {
        DraftDiscardAction action{ DraftDiscardAction::Done };
        size_t backspaces{ 0 }; // valid for EndBackspace
    };

    // What the caller has spent. The stall counters are PER RUNG and caller-maintained: +1 after
    // a round that left the box byte-identical, reset to 0 on any change — the measured kill
    // alternation (change / no-op / change) must keep its rung, while two consecutive dead rounds
    // must advance it.
    struct DraftDiscardProgress
    {
        uint32_t killRounds{ 0 };
        uint32_t killStalls{ 0 };
        uint32_t backspaceRounds{ 0 };
        uint32_t backspaceStalls{ 0 };
    };

    // PURE decision (the DecideDraftClear pattern) for ONE round of the discard ladder. The
    // ladder only ever moves FORWARD -- once a backspace round has run, the kill rung is never
    // revisited -- so no two rungs can alternate and the loop always terminates.
    inline DraftDiscardPlan DecideDraftDiscard(std::wstring_view box, const DraftDiscardProgress& spent)
    {
        DraftDiscardPlan plan;
        if (pending_detail::AllWhitespace(box))
        {
            plan.action = DraftDiscardAction::Done;
            return plan;
        }
        if (spent.backspaceRounds == 0 &&
            spent.killRounds < kMaxDiscardKillRounds &&
            spent.killStalls < kDiscardStallLimit)
        {
            plan.action = DraftDiscardAction::EndKill;
            return plan;
        }
        if (spent.backspaceRounds < kMaxDiscardBackspaceRounds &&
            spent.backspaceStalls < kDiscardStallLimit)
        {
            plan.action = DraftDiscardAction::EndBackspace;
            plan.backspaces = box.size() + 8; // + margin: a trailing cursor cell / a wide glyph
            return plan;
        }
        plan.action = DraftDiscardAction::GiveUp;
        return plan;
    }
}
