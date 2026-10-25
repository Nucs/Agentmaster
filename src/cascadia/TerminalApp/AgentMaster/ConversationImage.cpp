// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - ConversationImage: the layout and the pipeline (ConversationImage.h says what is drawn, how it imitates
// Claude Code, and why the transcript is the source and not the terminal). Plain C++ + Win32 (no WinRT, no PCH).

#include "ConversationImage.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <string_view>
#include <utility>

#include "ClaudeInvisibleChars.h" // ClaudeCellWidth: a code point's width in Claude Code's TUI
#include "ClaudeSpawn.h" // LogSwallowedException (Rule #18)

namespace Agentmaster
{
    namespace
    {
        // ----- the glyphs Claude Code draws (its Windows set: "●" where macOS shows "⏺", so the picture matches this PC's screen) -----
        constexpr wchar_t kGlyphBullet = L'\x25CF'; // before Claude's text and each tool call
        constexpr wchar_t kGlyphPrompt = L'\x276F'; // before a user's message (Claude Code's own prompt mark)
        constexpr wchar_t kGlyphResult = L'\x23BF'; // before what a tool returned
        constexpr wchar_t kGlyphStar = L'\x273B'; // before a compaction
        constexpr wchar_t kGlyphRule = L'\x2500'; // a turn's rule
        constexpr wchar_t kGlyphHeavyRule = L'\x2501'; // a session's rule
        constexpr wchar_t kGlyphQuoteBar = L'\x2502'; // a quote inside a message
        constexpr wchar_t kGlyphEllipsis = L'\x22EF'; // the note of rows left out
        constexpr wchar_t kGlyphCross = L'\x2717'; // how TranscriptTurns marks a failed call ("✗ exit 1", "✗ error")
        constexpr wchar_t kGlyphMessage = L'\x2709'; // a teammate's or another session's message
        // The two emoji the turns view starts remarks and compactions with (TranscriptTurns EventLines), as their surrogate
        // pairs: one shared high half, a distinct low half.
        constexpr wchar_t kAstralHigh = L'\xD83D';
        constexpr wchar_t kRemarkLow = L'\xDCAC'; // U+1F4AC, a remark between calls
        constexpr wchar_t kCompactionLow = L'\xDDDC'; // U+1F5DC, a compaction in the middle of a turn

        /// <summary>The narrowest sheet laid out: every prefix (an agent's indent, a nested list's markers) still leaves room for words.</summary>
        constexpr int kMinSheetCols = 20;
        /// <summary>
        /// The deepest container prefix drawn, in columns. A list nested ten deep inside a quote would leave no room for its text;
        /// past this the prefix is cut (its inner end kept) so a row always holds text.
        /// </summary>
        constexpr int kMaxPrefixCols = 40;

        /// <summary>A stretch of text in one style, as the writer lays it out.</summary>
        struct Span
        {
            std::wstring text; // may hold '\n' (a hard line break: the next text starts a new row)
            uint32_t fg{ kConvText }; // the text's color as 0xRRGGBB (the background is the block's: TextStyle::bg)
            uint8_t flags{ 0 }; // kScreenCell* bits
        };

        /// <summary>The cells a block's rows start with: its first row's (a bullet, a marker) and every other row's (the hanging indent).</summary>
        struct Prefix
        {
            std::vector<ScreenCell> first; // before the text of the block's FIRST row: "● ", "❯ ", a list marker, a quote bar
            std::vector<ScreenCell> rest; // before the text of every later row: blanks as wide as `first`, or the quote bars again
        };

        /// <summary>How a block's text is laid out.</summary>
        struct TextStyle
        {
            uint32_t bg{ kConvBg }; // every cell's background (the prefix's too: a band covers the whole row)
            bool fill{ false }; // pad every row to the sheet's width in `bg` (a band: a prompt, a code block)
            bool wrapWords{ true }; // break at spaces; false = keep every space and break at the edge (code)
            uint8_t cut{ kTallCutBlock }; // the cut value of the block's FIRST row (its other rows: kTallCutAny)
        };

        /// <summary>One cell.</summary>
        /// <param name="text">Its text (" " = blank).</param>
        /// <param name="fg">Its foreground.</param>
        /// <param name="bg">Its background.</param>
        /// <param name="flags">kScreenCell* bits.</param>
        /// <param name="width">1, or 2 for a wide glyph.</param>
        /// <returns>The cell.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be copied.</exception>
        ScreenCell MakeCell(std::wstring text, uint32_t fg, uint32_t bg, uint8_t flags = 0, uint8_t width = 1)
        {
            ScreenCell c;
            c.text = std::move(text);
            c.fg = fg;
            c.bg = bg;
            c.flags = flags;
            c.width = width;
            return c;
        }

        /// <summary>The columns a run of cells covers.</summary>
        /// <param name="cells">The cells.</param>
        /// <returns>The sum of their widths (a wide cell counts 2).</returns>
        int CellsWidth(const std::vector<ScreenCell>& cells) noexcept
        {
            int w = 0;
            for (const auto& c : cells)
            {
                w += c.width == 2 ? 2 : 1;
            }
            return w;
        }

        /// <summary>Reads one code point at `i` (a surrogate pair as one; a lone half as itself) and says how many units it took.</summary>
        /// <param name="text">The text.</param>
        /// <param name="i">Where it starts (below text.size()).</param>
        /// <param name="units">1 or 2.</param>
        /// <returns>The code point.</returns>
        char32_t CodePointAt(std::wstring_view text, size_t i, size_t& units) noexcept
        {
            const wchar_t c = text[i];
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
            {
                units = 2;
                return 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) + (static_cast<char32_t>(text[i + 1]) - 0xDC00);
            }
            units = 1;
            return static_cast<char32_t>(c);
        }

        /// <summary>
        /// The sheet as it is written: rows of cells with their cut value and their turn, bounded to the LAST maxRows rows
        /// (an older row is dropped as a newer one comes, so memory stays bounded however long the conversation is).
        /// </summary>
        class SheetWriter
        {
        public:
            /// <summary>A writer for a sheet of `cols` columns that keeps at most `maxRows` rows (0 = all).</summary>
            /// <param name="cols">The width.</param>
            /// <param name="maxRows">The row cap (0 = none; else at least 3, room for the note of rows left out).</param>
            SheetWriter(int cols, int maxRows) noexcept :
                _cols(cols), _maxRows(maxRows <= 0 ? 0 : (std::max)(maxRows, 3))
            {
            }

            /// <summary>The width in cells: a block's room on a row is this less its prefix.</summary>
            /// <returns>The sheet's columns.</returns>
            int Cols() const noexcept
            {
                return _cols;
            }

            /// <summary>Whether any row was written yet (a heading at the very top gets no blank row above it).</summary>
            /// <returns>True before the first row (a row dropped by the cap counts as written).</returns>
            bool Empty() const noexcept
            {
                return _rows.empty() && _dropped == 0;
            }

            /// <summary>The turn the rows written from now on belong to.</summary>
            /// <param name="turn">Its number (0 = before the first turn).</param>
            void SetTurn(int turn) noexcept
            {
                _turn = turn;
                _highestTurn = (std::max)(_highestTurn, turn);
            }

            /// <summary>One empty row; a background other than the sheet's paints the whole width (a band's gap).</summary>
            /// <param name="bg">The row's background.</param>
            /// <exception cref="std::bad_alloc">If the row cannot be stored.</exception>
            void Blank(uint32_t bg)
            {
                std::vector<ScreenCell> row;
                if ((bg & 0xFFFFFFu) != kConvBg)
                {
                    row.reserve(static_cast<size_t>(_cols));
                    for (int i = 0; i < _cols; ++i)
                    {
                        row.push_back(MakeCell(L" ", kConvText, bg));
                    }
                }
                _Push(std::move(row), kTallCutAny);
            }

            /// <summary>
            /// Lays styled text out after a prefix: words wrap at the sheet's width (or, without wrapWords, every cell goes where
            /// it falls and a row breaks at the edge), '\n' starts a new row, and every row after the first starts with the
            /// prefix's `rest` cells. At least one row is written, so an empty block still shows its prefix (a list item's marker).
            /// </summary>
            /// <param name="spans">The text, in order.</param>
            /// <param name="prefix">The cells each row starts with.</param>
            /// <param name="style">The background, the band, the wrapping and the first row's cut value.</param>
            /// <exception cref="std::bad_alloc">If a row cannot be built or stored.</exception>
            void Text(const std::vector<Span>& spans, const Prefix& prefix, const TextStyle& style)
            {
                std::vector<ScreenCell> row = prefix.first;
                int used = CellsWidth(row); // columns taken on this row, its prefix included
                int textStart = used; // where this row's text begins (TAB stops count from here)
                bool rowHasText = false; // a text cell is on the row (the blanks at a wrap are then dropped)
                bool firstRow = true;
                bool emittedAny = false;
                std::vector<ScreenCell> word; // the word being read: it moves to the row whole, or wraps whole
                int wordW = 0;
                std::vector<ScreenCell> gap; // the blanks between the row's text and the next word
                int gapW = 0;

                const auto emitRow = [&]() {
                    if (style.fill)
                    {
                        while (used < _cols)
                        {
                            row.push_back(MakeCell(L" ", kConvText, style.bg));
                            ++used;
                        }
                    }
                    _Push(std::move(row), firstRow ? style.cut : kTallCutAny);
                    firstRow = false;
                    emittedAny = true;
                    row = prefix.rest;
                    used = CellsWidth(row);
                    textStart = used;
                    rowHasText = false;
                };
                // One cell where it falls: on this row when it fits, else on the next (a row with nothing but its prefix takes
                // it anyway - the only way a too-deep prefix cannot loop forever).
                const auto placeCell = [&](ScreenCell&& cell) {
                    const int w = cell.width == 2 ? 2 : 1;
                    if (used + w > _cols && rowHasText)
                    {
                        emitRow();
                    }
                    row.push_back(std::move(cell));
                    used += w;
                    rowHasText = true;
                };
                const auto flushWord = [&]() {
                    if (word.empty())
                    {
                        return;
                    }
                    if (used + gapW + wordW > _cols)
                    {
                        // The word does not fit after the blanks: it goes to the next row, and the blanks at the break go.
                        gap.clear();
                        gapW = 0;
                        if (rowHasText)
                        {
                            emitRow();
                        }
                    }
                    if (used + gapW + wordW <= _cols)
                    {
                        for (auto& c : gap)
                        {
                            row.push_back(std::move(c));
                        }
                        used += gapW;
                        for (auto& c : word)
                        {
                            row.push_back(std::move(c));
                        }
                        used += wordW;
                        rowHasText = true;
                    }
                    else
                    {
                        // A word longer than a whole row (a path, a URL, a hash): split it cell by cell across rows.
                        for (auto& c : word)
                        {
                            placeCell(std::move(c));
                        }
                    }
                    gap.clear();
                    gapW = 0;
                    word.clear();
                    wordW = 0;
                };

                for (const Span& span : spans)
                {
                    const std::wstring_view text{ span.text };
                    for (size_t i = 0; i < text.size();)
                    {
                        size_t units = 1;
                        const char32_t cp = CodePointAt(text, i, units);
                        const std::wstring_view piece = text.substr(i, units);
                        i += units;
                        if (cp == U'\n')
                        {
                            // A hard break: what is pending ends this row (its trailing blanks go), the next text starts a new one.
                            flushWord();
                            gap.clear();
                            gapW = 0;
                            emitRow();
                            continue;
                        }
                        if (cp == U'\t')
                        {
                            // To the next multiple of 4 columns of the text (Claude Code's own expansion is the terminal's).
                            const int column = used - textStart + gapW + wordW;
                            const int blanks = 4 - (column % 4);
                            if (style.wrapWords)
                            {
                                flushWord();
                                for (int k = 0; k < blanks; ++k)
                                {
                                    gap.push_back(MakeCell(L" ", span.fg, style.bg, span.flags));
                                }
                                gapW += blanks;
                            }
                            else
                            {
                                for (int k = 0; k < blanks; ++k)
                                {
                                    placeCell(MakeCell(L" ", span.fg, style.bg, span.flags));
                                }
                            }
                            continue;
                        }
                        const int w = ClaudeCellWidth(cp);
                        if (w == 0)
                        {
                            // A combining mark, a joiner or a variation selector rides on the cell before it (its cluster);
                            // with nothing before it on the row it has nothing to ride on and is dropped.
                            ScreenCell* host = !word.empty() ? &word.back() : (!style.wrapWords && rowHasText && !row.empty() ? &row.back() : nullptr);
                            if (host && host->text.size() + piece.size() <= kScreenCellMaxChars)
                            {
                                host->text.append(piece);
                            }
                            continue;
                        }
                        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
                        {
                            continue; // a stray control character (a lone CR, an escape): nothing a reader could see
                        }
                        ScreenCell cell = MakeCell(std::wstring{ piece }, span.fg, style.bg, span.flags, static_cast<uint8_t>(w));
                        if (!style.wrapWords)
                        {
                            placeCell(std::move(cell));
                        }
                        else if (cp == U' ')
                        {
                            flushWord();
                            gap.push_back(std::move(cell));
                            gapW += 1;
                        }
                        else
                        {
                            word.push_back(std::move(cell));
                            wordW += w;
                        }
                    }
                }
                flushWord();
                if (rowHasText || !emittedAny)
                {
                    // The last row (a block that ended with a hard break has written its rows already, and an empty
                    // continuation row after it would only add a blank).
                    if (style.fill)
                    {
                        while (used < _cols)
                        {
                            row.push_back(MakeCell(L" ", kConvText, style.bg));
                            ++used;
                        }
                    }
                    _Push(std::move(row), firstRow ? style.cut : kTallCutAny);
                }
            }

            /// <summary>
            /// Extends the LAST row to the full width with `glyph` (a rule after a heading's words), one blank first so the rule
            /// never touches the last word. The blank is added here because Text drops a row's trailing blanks.
            /// </summary>
            /// <param name="glyph">The rule's character (one column wide).</param>
            /// <param name="fg">Its color.</param>
            /// <exception cref="std::bad_alloc">If the row cannot grow.</exception>
            void FillLastRow(wchar_t glyph, uint32_t fg)
            {
                if (_rows.empty())
                {
                    return;
                }
                auto& row = _rows.back();
                int used = CellsWidth(row);
                if (!row.empty() && row.back().text != L" " && used < _cols)
                {
                    row.push_back(MakeCell(L" ", kConvText, kConvBg));
                    ++used;
                }
                for (; used < _cols; ++used)
                {
                    row.push_back(MakeCell(std::wstring(1, glyph), fg, kConvBg));
                }
            }

            /// <summary>
            /// The finished sheet. When rows were dropped at the top, the first two rows become the note saying how many (and
            /// through which turn) plus a blank, so the sheet never exceeds its cap.
            /// </summary>
            /// <returns>The sheet (at least one row).</returns>
            /// <exception cref="std::bad_alloc">If the sheet cannot be allocated.</exception>
            ConversationSheet Finish()
            {
                ConversationSheet sheet;
                if (_dropped > 0)
                {
                    // Room for the note and a blank under it: two more of the oldest rows go.
                    for (int k = 0; k < 2 && _rows.size() > 1; ++k)
                    {
                        _DropFront();
                    }
                    const int firstTurn = _turns.empty() ? 0 : _turns.front();
                    std::wstring note(1, kGlyphEllipsis);
                    note += L" " + std::to_wstring(_dropped) + (_dropped == 1 ? L" earlier row" : L" earlier rows");
                    if (_droppedThroughTurn > 0)
                    {
                        note += L", through turn " + std::to_wstring(_droppedThroughTurn) + L",";
                    }
                    note += L" left out: the conversation is longer than the pictures hold";
                    std::vector<ScreenCell> noteRow;
                    int used = 0;
                    for (size_t i = 0; i < note.size();)
                    {
                        size_t units = 1;
                        const char32_t cp = CodePointAt(note, i, units);
                        const int w = (std::max)(1, ClaudeCellWidth(cp));
                        if (used + w > _cols)
                        {
                            break; // the note is one row: a narrow sheet shows its start
                        }
                        noteRow.push_back(MakeCell(note.substr(i, units), kConvDim, kConvBg, kScreenCellItalic, static_cast<uint8_t>(w)));
                        used += w;
                        i += units;
                    }
                    _rows.push_front({});
                    _cuts.push_front(kTallCutAny);
                    _turns.push_front(firstTurn);
                    _rows.push_front(std::move(noteRow));
                    _cuts.push_front(kTallCutTurn);
                    _turns.push_front(firstTurn);
                }
                if (_rows.empty())
                {
                    _rows.emplace_back();
                    _cuts.push_back(kTallCutTurn);
                    _turns.push_back(0);
                }
                sheet.snapshot.cols = _cols;
                sheet.snapshot.rows = static_cast<int>(_rows.size());
                sheet.snapshot.defaultFg = kConvText;
                sheet.snapshot.defaultBg = kConvBg;
                sheet.snapshot.lines.reserve(_rows.size());
                for (auto& row : _rows)
                {
                    sheet.snapshot.lines.push_back(std::move(row));
                }
                sheet.cuts.assign(_cuts.begin(), _cuts.end());
                sheet.rowTurn.assign(_turns.begin(), _turns.end());
                sheet.turns = _highestTurn;
                sheet.droppedRows = _dropped;
                sheet.droppedThroughTurn = _droppedThroughTurn;
                return sheet;
            }

        private:
            /// <summary>Stores a row; past the cap the oldest row goes (counted, with its turn).</summary>
            /// <param name="row">The row.</param>
            /// <param name="cut">Its cut value.</param>
            /// <exception cref="std::bad_alloc">If the row cannot be stored.</exception>
            void _Push(std::vector<ScreenCell>&& row, uint8_t cut)
            {
                _rows.push_back(std::move(row));
                _cuts.push_back(cut);
                _turns.push_back(_turn);
                if (_maxRows > 0 && _rows.size() > static_cast<size_t>(_maxRows))
                {
                    _DropFront();
                }
            }

            /// <summary>Drops the oldest row, counting it and the turn it belonged to.</summary>
            void _DropFront() noexcept
            {
                _droppedThroughTurn = (std::max)(_droppedThroughTurn, _turns.front());
                _rows.pop_front();
                _cuts.pop_front();
                _turns.pop_front();
                ++_dropped;
            }

            int _cols; // the sheet's width in cells
            int _maxRows; // the row cap (0 = none): past it the oldest row goes as a new one comes
            int _turn{ 0 }; // the turn the rows written now belong to (SetTurn)
            int _highestTurn{ 0 }; // the highest turn number seen (ConversationSheet::turns)
            int _dropped{ 0 }; // rows dropped at the top by the cap
            int _droppedThroughTurn{ 0 }; // the highest turn a dropped row belonged to
            std::deque<std::vector<ScreenCell>> _rows; // the rows kept, oldest first (a deque: the cap drops at the front)
            std::deque<uint8_t> _cuts; // one kTallCut* per kept row
            std::deque<int> _turns; // one turn number per kept row
        };

        // ----- reading the blocks -----

        /// <summary>A block's first visible UTF-16 unit, the unit after it, and the style of the run that holds it.</summary>
        struct Lead
        {
            wchar_t unit{ 0 }; // 0 = the block shows no text
            wchar_t next{ 0 }; // the unit after it in the same run (a surrogate pair's low half)
            uint8_t style{ 0 }; // the run's kMd* bits
            size_t run{ 0 }; // the run's index
        };

        /// <summary>Finds a block's first visible unit (skipping blanks and line breaks).</summary>
        /// <param name="b">The block.</param>
        /// <returns>The lead (unit 0 when the block has no visible text).</returns>
        Lead LeadOf(const MdBlock& b) noexcept
        {
            Lead lead;
            for (size_t r = 0; r < b.runs.size(); ++r)
            {
                const std::wstring& text = b.runs[r].text;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    const wchar_t c = text[i];
                    if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r')
                    {
                        continue;
                    }
                    lead.unit = c;
                    lead.next = i + 1 < text.size() ? text[i + 1] : L'\0';
                    lead.style = b.runs[r].style;
                    lead.run = r;
                    return lead;
                }
            }
            return lead;
        }

        /// <summary>A block's text: its runs joined (a code block's code).</summary>
        /// <param name="b">The block.</param>
        /// <returns>The text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring BlockText(const MdBlock& b)
        {
            if (b.kind == MdBlockKind::Code)
            {
                return b.code;
            }
            std::wstring text;
            for (const auto& run : b.runs)
            {
                text += run.text;
            }
            return text;
        }

        /// <summary>Whether every visible run of a block is italic (a note: "_Before this turn: ..._").</summary>
        /// <param name="b">The block.</param>
        /// <returns>True for an all-italic paragraph with some text.</returns>
        bool AllItalic(const MdBlock& b) noexcept
        {
            bool any = false;
            for (const auto& run : b.runs)
            {
                if (run.text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
                {
                    continue;
                }
                if ((run.style & kMdItalic) == 0)
                {
                    return false;
                }
                any = true;
            }
            return any;
        }

        /// <summary>The number in a turn heading's "Turn N", or -1 for a heading that is not a turn ("Work before the first prompt" is 0).</summary>
        /// <param name="b">A top-level level-3 heading.</param>
        /// <returns>The turn's number, 0, or -1.</returns>
        /// <exception cref="std::bad_alloc">If the heading's text cannot be read.</exception>
        int TurnNumberOf(const MdBlock& b)
        {
            const std::wstring text = BlockText(b);
            if (text.rfind(L"Work before the first prompt", 0) == 0)
            {
                return 0;
            }
            if (text.rfind(L"Turn ", 0) != 0)
            {
                return -1;
            }
            int n = 0;
            size_t i = 5;
            for (; i < text.size() && text[i] >= L'0' && text[i] <= L'9' && n < 10000000; ++i)
            {
                n = n * 10 + (text[i] - L'0');
            }
            return i > 5 ? n : -1;
        }

        /// <summary>Base colors and bits for a block's runs: what plain, bold, code and link text become.</summary>
        struct RunColors
        {
            uint32_t text{ kConvText }; // a plain run
            uint32_t bold{ kConvBright }; // a **strong** run (it also gets kScreenCellBold)
            uint32_t code{ kConvCode }; // an `inline code` run
            uint32_t link{ kConvLink }; // a link's text (it also gets kScreenCellUnderline)
            uint8_t flags{ 0 }; // bits every run gets (italic for a note)
        };

        /// <summary>
        /// A block's runs as spans: **bold** bright and bold, *italic* italic, `code` lavender, a link underlined in its color.
        /// A block cut at kConversationBlockMaxChars says so at its end.
        /// </summary>
        /// <param name="b">The block.</param>
        /// <param name="colors">The colors.</param>
        /// <returns>The spans.</returns>
        /// <exception cref="std::bad_alloc">If the spans cannot be allocated.</exception>
        std::vector<Span> SpansOf(const MdBlock& b, const RunColors& colors)
        {
            std::vector<Span> spans;
            spans.reserve(b.runs.size() + 1);
            for (const auto& run : b.runs)
            {
                Span s;
                s.text = run.text;
                s.fg = colors.text;
                s.flags = colors.flags;
                if (run.style & kMdBold)
                {
                    s.flags |= kScreenCellBold;
                    s.fg = colors.bold;
                }
                if (run.style & kMdItalic)
                {
                    s.flags |= kScreenCellItalic;
                }
                if (run.style & kMdCode)
                {
                    s.fg = colors.code;
                }
                if (run.style & kMdLink)
                {
                    s.fg = colors.link;
                    s.flags |= kScreenCellUnderline;
                }
                spans.push_back(std::move(s));
            }
            if (b.truncated)
            {
                spans.push_back(Span{ L" \x2026 (cut here: the block is longer than " + std::to_wstring(kConversationBlockMaxChars) + L" characters)", kConvDim, kScreenCellItalic });
            }
            return spans;
        }

        /// <summary>
        /// Removes the first `units` UTF-16 units of the visible text from spans (after any blanks before them), and the blank
        /// right after them: a remark's "💬 " or a compaction's "🗜 ", whose meaning the bullet takes over.
        /// </summary>
        /// <param name="spans">The spans (changed in place).</param>
        /// <param name="units">How many units the mark has.</param>
        void DropLeadMark(std::vector<Span>& spans, size_t units) noexcept
        {
            bool dropped = false;
            for (auto& s : spans)
            {
                size_t i = 0;
                if (!dropped)
                {
                    while (i < s.text.size() && (s.text[i] == L' ' || s.text[i] == L'\t'))
                    {
                        ++i;
                    }
                    if (i >= s.text.size())
                    {
                        s.text.clear();
                        continue;
                    }
                    const size_t take = (std::min)(units, s.text.size() - i);
                    i += take;
                    units -= take;
                    if (units > 0)
                    {
                        s.text.erase(0, i);
                        continue; // the mark goes on in the next span
                    }
                    dropped = true;
                }
                if (i < s.text.size() && s.text[i] == L' ')
                {
                    ++i; // the blank between the mark and the words
                }
                s.text.erase(0, i);
                return;
            }
        }

        /// <summary>Blank cells.</summary>
        /// <param name="cells">Where they go.</param>
        /// <param name="count">How many.</param>
        /// <param name="bg">Their background.</param>
        /// <exception cref="std::bad_alloc">If the cells cannot be stored.</exception>
        void AppendBlanks(std::vector<ScreenCell>& cells, int count, uint32_t bg)
        {
            for (int i = 0; i < count; ++i)
            {
                cells.push_back(MakeCell(L" ", kConvText, bg));
            }
        }

        /// <summary>A glyph and a blank: a bullet or a mark before a block's text.</summary>
        /// <param name="cells">Where they go.</param>
        /// <param name="glyph">The glyph (one column).</param>
        /// <param name="fg">Its color.</param>
        /// <param name="bg">The background.</param>
        /// <param name="flags">kScreenCell* bits of the glyph.</param>
        /// <exception cref="std::bad_alloc">If the cells cannot be stored.</exception>
        void AppendMark(std::vector<ScreenCell>& cells, wchar_t glyph, uint32_t fg, uint32_t bg, uint8_t flags = 0)
        {
            cells.push_back(MakeCell(std::wstring(1, glyph), fg, bg, flags));
            cells.push_back(MakeCell(L" ", kConvText, bg));
        }

        /// <summary>
        /// The prefix the containers [from, end) of a block add: a bar for each quote, a marker for each list item the block
        /// STARTS (on the first row; the indent under it on every other row), the indent alone for an item the block only
        /// continues. Every marker column is as wide as its list's widest marker plus a blank, so wrapped text lines up.
        /// </summary>
        /// <param name="b">The block.</param>
        /// <param name="from">The first container to draw (the ones before it belong to the speaker's quote or a tool item).</param>
        /// <param name="prefix">The prefix to extend.</param>
        /// <param name="bg">The background of the cells.</param>
        /// <exception cref="std::bad_alloc">If the cells cannot be stored.</exception>
        void AppendContainerPrefix(const MdBlock& b, size_t from, Prefix& prefix, uint32_t bg)
        {
            for (size_t k = from; k < b.containers.size(); ++k)
            {
                const MdContainer& c = b.containers[k];
                if (c.kind == MdContainerKind::Quote)
                {
                    AppendMark(prefix.first, kGlyphQuoteBar, kConvRule, bg);
                    AppendMark(prefix.rest, kGlyphQuoteBar, kConvRule, bg);
                    continue;
                }
                const int width = static_cast<int>(c.markerChars) + 1;
                int markerW = 0;
                if (!c.marker.empty())
                {
                    for (size_t i = 0; i < c.marker.size();)
                    {
                        size_t units = 1;
                        const char32_t cp = CodePointAt(c.marker, i, units);
                        const int w = (std::max)(1, ClaudeCellWidth(cp));
                        prefix.first.push_back(MakeCell(c.marker.substr(i, units), kConvMarker, bg, 0, static_cast<uint8_t>(w)));
                        markerW += w;
                        i += units;
                    }
                }
                AppendBlanks(prefix.first, (std::max)(1, width - markerW), bg);
                AppendBlanks(prefix.rest, (std::max)(1, width), bg);
            }
        }

        /// <summary>Keeps a prefix within kMaxPrefixCols columns (its inner end: the marker next to the text).</summary>
        /// <param name="cells">The prefix's cells (changed in place; erasing never allocates, so this never throws).</param>
        void ClampPrefix(std::vector<ScreenCell>& cells)
        {
            while (CellsWidth(cells) > kMaxPrefixCols && !cells.empty())
            {
                cells.erase(cells.begin());
            }
        }

        /// <summary>What a list item under a speaker is, by how the turns view starts it (TranscriptTurns EventLines).</summary>
        enum class ItemKind
        {
            ToolCall, // "**Bash** - ... · ok": a bold tool name
            Remark, // "💬 ...": what Claude said between calls
            Compaction, // "🗜 ...": the context was compacted mid-turn
            Message, // "✉ **name** ...": a message that reached the agent
            Prompt, // "❯ ...": a brief or a message typed while it worked
            Empty, // "(no tool calls)" / "(no remarks)"
            Other,
        };

        /// <summary>Reads what a tool area's item is.</summary>
        /// <param name="b">The item's first block.</param>
        /// <returns>Its kind.</returns>
        /// <exception cref="std::bad_alloc">If the block's text cannot be read.</exception>
        ItemKind ItemKindOf(const MdBlock& b)
        {
            const Lead lead = LeadOf(b);
            if (lead.unit == kAstralHigh && lead.next == kRemarkLow)
            {
                return ItemKind::Remark;
            }
            if (lead.unit == kAstralHigh && lead.next == kCompactionLow)
            {
                return ItemKind::Compaction;
            }
            if (lead.unit == kGlyphMessage)
            {
                return ItemKind::Message;
            }
            if (lead.unit == kGlyphPrompt)
            {
                return ItemKind::Prompt;
            }
            if (lead.unit != 0 && (lead.style & kMdBold) != 0)
            {
                return ItemKind::ToolCall;
            }
            const std::wstring text = BlockText(b);
            if (text.find_first_not_of(L" \t") != std::wstring::npos && text.substr(text.find_first_not_of(L" \t")).rfind(L"(no ", 0) == 0)
            {
                return ItemKind::Empty;
            }
            return ItemKind::Other;
        }

        /// <summary>
        /// The bullet color of a tool call, from how the turns view says it ended (its "· <outcome>" part): red when it failed
        /// ("✗ exit 1", "✗ error"), gray when no result came ("no result"), green otherwise - the colors of Claude Code's own
        /// bullet for a finished, a failed and an unfinished call.
        /// </summary>
        /// <param name="b">The call's line.</param>
        /// <returns>The color.</returns>
        /// <exception cref="std::bad_alloc">If the block's text cannot be read.</exception>
        uint32_t ToolCallColor(const MdBlock& b)
        {
            const std::wstring text = BlockText(b);
            if (text.find(kGlyphCross) != std::wstring::npos)
            {
                return kConvFail;
            }
            if (text.find(L"\x00B7 no result") != std::wstring::npos)
            {
                return kConvPending;
            }
            return kConvOk;
        }

        /// <summary>The spans of the lines AFTER a block's first line (a label's folded stats line at the Max level).</summary>
        /// <param name="b">The block.</param>
        /// <returns>The spans; empty when the block is one line.</returns>
        /// <exception cref="std::bad_alloc">If the spans cannot be allocated.</exception>
        std::vector<Span> SpansAfterFirstLine(const MdBlock& b)
        {
            std::vector<Span> spans;
            bool past = false;
            for (const auto& run : b.runs)
            {
                std::wstring_view text{ run.text };
                if (!past)
                {
                    const size_t nl = text.find(L'\n');
                    if (nl == std::wstring_view::npos)
                    {
                        continue;
                    }
                    text.remove_prefix(nl + 1);
                    past = true;
                }
                if (!text.empty())
                {
                    spans.push_back(Span{ std::wstring{ text }, kConvDim, kScreenCellItalic });
                }
            }
            return spans;
        }

        /// <summary>The spans of a block's FIRST line only (a label's line).</summary>
        /// <param name="b">The block.</param>
        /// <param name="fg">Their color.</param>
        /// <param name="flags">Their bits.</param>
        /// <returns>The spans.</returns>
        /// <exception cref="std::bad_alloc">If the spans cannot be allocated.</exception>
        std::vector<Span> SpansOfFirstLine(const MdBlock& b, uint32_t fg, uint8_t flags)
        {
            std::vector<Span> spans;
            for (const auto& run : b.runs)
            {
                std::wstring_view text{ run.text };
                const size_t nl = text.find(L'\n');
                spans.push_back(Span{ std::wstring{ nl == std::wstring_view::npos ? text : text.substr(0, nl) }, fg, flags });
                if (nl != std::wstring_view::npos)
                {
                    break;
                }
            }
            return spans;
        }

        /// <summary>Who the blocks being laid out belong to: set by a label or a header, cleared by a heading.</summary>
        enum class Speaker
        {
            None, // outside any message (the legend, the session chain, a note)
            User, // the user's message ("❯ User")
            Peer, // a teammate's or another session's message ("✉ name")
            Claude, // Claude's part of a turn ("● Claude")
            Agent, // a subagent's or teammate's entry ("● name · ...")
        };

        /// <summary>
        /// Lays out one block of a message, a reply or a note: a paragraph or a heading as wrapped text, a code block on its band
        /// keeping its spaces, a rule as a line across the room left.
        /// </summary>
        /// <param name="w">The writer.</param>
        /// <param name="b">The block.</param>
        /// <param name="prefix">The cells its rows start with.</param>
        /// <param name="colors">The colors of its runs.</param>
        /// <param name="style">The background and the band (a code block outside a band gets the code band).</param>
        /// <exception cref="std::bad_alloc">If a row cannot be built.</exception>
        void LayoutContentBlock(SheetWriter& w, const MdBlock& b, Prefix prefix, const RunColors& colors, TextStyle style)
        {
            ClampPrefix(prefix.first);
            ClampPrefix(prefix.rest);
            if (b.kind == MdBlockKind::Code)
            {
                // A code block keeps its spaces and breaks at the edge; it sits on the code band unless it is inside a band
                // already (a code block in a user's message keeps the message's band).
                TextStyle code = style;
                code.wrapWords = false;
                code.fill = true;
                if (!style.fill)
                {
                    code.bg = kConvCodeBand;
                }
                std::vector<Span> spans{ Span{ b.code, kConvCodeText, 0 } };
                if (b.truncated)
                {
                    spans.push_back(Span{ L"\n\x2026 (cut here: the block is longer than " + std::to_wstring(kConversationBlockMaxChars) + L" characters)", kConvDim, kScreenCellItalic });
                }
                w.Text(spans, prefix, code);
                return;
            }
            if (b.kind == MdBlockKind::Rule)
            {
                const int room = (std::max)(1, w.Cols() - CellsWidth(prefix.first));
                TextStyle rule = style;
                rule.wrapWords = false;
                w.Text({ Span{ std::wstring(static_cast<size_t>(room), kGlyphRule), kConvRule, 0 } }, prefix, rule);
                return;
            }
            RunColors runColors = colors;
            if (b.rawHtml)
            {
                // Raw HTML kept as text (MarkdownBlocks.h): dimmed, so markup reads as markup.
                runColors.text = kConvDim;
                runColors.bold = kConvDim;
            }
            if (b.kind == MdBlockKind::Heading)
            {
                // A heading inside a message: bold and bright, a first-level one underlined (Claude Code draws them so).
                runColors.text = kConvBright;
                runColors.flags |= kScreenCellBold | (b.headingLevel <= 1 ? kScreenCellUnderline : 0);
            }
            w.Text(SpansOf(b, runColors), prefix, style);
        }
    }

    ConversationSheet BuildConversationSheet(const std::vector<MdBlock>& blocks, const ConversationSheetOptions& options)
    {
        const int cols = std::clamp(options.cols, kMinSheetCols, kScreenSnapshotMaxCols);
        SheetWriter w(cols, options.maxRows);
        // Which blocks are a speaker's tool-call list (the list right under "● Claude" or an agent's header).
        std::vector<int> areaOf(blocks.size(), -1);
        {
            const auto areas = MdFindToolAreas(blocks);
            for (size_t a = 0; a < areas.size(); ++a)
            {
                for (size_t i = areas[a].first; i < areas[a].end && i < blocks.size(); ++i)
                {
                    areaOf[i] = static_cast<int>(a);
                }
            }
        }
        Speaker speaker = Speaker::None;
        bool inAgent = false; // inside an agent's entry: its list and its reply are indented under its header
        uint32_t speakerQuote = 0; // the id of the quote that holds the current message or reply (0 = none started yet)
        bool skipGap = false; // the block before was left out (a "(no tool calls)" line): its gap is the next block's
        const MdBlock* prev = nullptr;
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            const MdBlock& b = blocks[i];
            const bool top = b.containers.empty();

            // ----- headings: a turn's rule, a session's rule, any other heading as a bold line -----
            if (top && b.kind == MdBlockKind::Heading)
            {
                if (!w.Empty())
                {
                    w.Blank(kConvBg);
                }
                speaker = Speaker::None;
                inAgent = false;
                speakerQuote = 0;
                const int turn = b.headingLevel == 3 ? TurnNumberOf(b) : -1;
                const bool session = b.headingLevel <= 2 && MdSessionHeadingNumber(b) > 0;
                TextStyle rule;
                rule.cut = kTallCutTurn;
                Prefix prefix;
                if (turn >= 0)
                {
                    // "── Turn 3 · Oct 17 16:27 → last reply 16:44 ─────": the number in Claude's orange, the times dimmed.
                    w.SetTurn(turn);
                    AppendMark(prefix.first, kGlyphRule, kConvRule, kConvBg);
                    prefix.first.insert(prefix.first.begin(), MakeCell(std::wstring(1, kGlyphRule), kConvRule, kConvBg));
                    AppendBlanks(prefix.rest, 3, kConvBg); // a heading too long for one row goes on under its words, not under the rule
                    std::wstring text = BlockText(b);
                    const size_t dot = text.find(L" \x00B7 ");
                    std::vector<Span> spans{ Span{ text.substr(0, dot), kConvClaude, kScreenCellBold } };
                    if (dot != std::wstring::npos)
                    {
                        spans.push_back(Span{ text.substr(dot), kConvDim, 0 });
                    }
                    w.Text(spans, prefix, rule);
                    w.FillLastRow(kGlyphRule, kConvRule);
                }
                else if (session)
                {
                    // "━━ Session 2 of 3 · `f48cd627` · forked from session 1 ... ━━━": a heavier rule, all orange.
                    AppendMark(prefix.first, kGlyphHeavyRule, kConvClaude, kConvBg);
                    prefix.first.insert(prefix.first.begin(), MakeCell(std::wstring(1, kGlyphHeavyRule), kConvClaude, kConvBg));
                    AppendBlanks(prefix.rest, 3, kConvBg);
                    RunColors colors;
                    colors.text = kConvClaude;
                    colors.bold = kConvClaude;
                    colors.code = kConvClaude;
                    colors.flags = kScreenCellBold;
                    w.Text(SpansOf(b, colors), prefix, rule);
                    w.FillLastRow(kGlyphHeavyRule, kConvClaude);
                }
                else
                {
                    RunColors colors;
                    colors.text = kConvBright;
                    colors.flags = kScreenCellBold;
                    w.Text(SpansOf(b, colors), prefix, rule);
                }
                prev = &b;
                continue;
            }

            // ----- the gap above: a blank row where the Markdown has paragraph spacing (MdGapAbove's 6+ px), none in a tight list -----
            // After a block that was left out, the blank row above IT already separates what comes next.
            const bool gapTaken = skipGap;
            skipGap = false;
            if (prev && !gapTaken && MdGapAbove(prev, b) >= 6.0)
            {
                // Between two blocks of ONE user message the gap is part of the message's band.
                const bool inBand = (speaker == Speaker::User || speaker == Speaker::Peer) && speakerQuote != 0 && !top && !prev->containers.empty() &&
                                    b.containers[0].kind == MdContainerKind::Quote && b.containers[0].id == speakerQuote && prev->containers[0].id == speakerQuote;
                w.Blank(inBand ? kConvPromptBand : kConvBg);
            }
            prev = &b;
            const Lead lead = LeadOf(b);

            // ----- a speaker's label: it says whose blocks follow; "❯ User" and "● Claude" draw no line of their own -----
            if (top && MdIsLabelParagraph(b))
            {
                speakerQuote = 0;
                if (lead.unit == kGlyphPrompt)
                {
                    speaker = Speaker::User;
                    inAgent = false;
                    // "❯ User, typed while it worked (16:30)" says more than the band does: that line stays, dimmed.
                    const std::wstring label = BlockText(b);
                    if (label.find(L"typed while it worked") != std::wstring::npos)
                    {
                        Prefix none;
                        w.Text(SpansOfFirstLine(b, kConvDim, kScreenCellItalic), none, TextStyle{});
                    }
                }
                else if (lead.unit == kGlyphBullet)
                {
                    speaker = Speaker::Claude;
                    inAgent = false;
                    // At the Full / Max levels the turn's stats line is folded into the label's paragraph: it stays, dimmed.
                    if (auto stats = SpansAfterFirstLine(b); !stats.empty())
                    {
                        Prefix indent;
                        AppendBlanks(indent.first, 2, kConvBg);
                        indent.rest = indent.first;
                        w.Text(stats, indent, TextStyle{});
                    }
                }
                else if (lead.unit == kGlyphMessage)
                {
                    speaker = Speaker::Peer;
                    inAgent = false;
                    Prefix none;
                    w.Text(SpansOfFirstLine(b, kConvPeer, kScreenCellBold), none, TextStyle{});
                }
                else
                {
                    Prefix none;
                    w.Text(SpansOf(b, RunColors{ kConvDim, kConvDim, kConvDim, kConvDim, 0 }), none, TextStyle{});
                }
                continue;
            }

            // ----- an agent's header ("● finder-a · teammate · ...") or a peer's label with a note ("✉ name · note") -----
            if (top && b.kind == MdBlockKind::Paragraph && !b.rawHtml && (lead.style & kMdBold) != 0 && (lead.unit == kGlyphBullet || lead.unit == kGlyphMessage))
            {
                speakerQuote = 0;
                std::vector<Span> spans = SpansOf(b, RunColors{ kConvDim, kConvBright, kConvCode, kConvLink, 0 });
                Prefix prefix;
                if (lead.unit == kGlyphBullet)
                {
                    speaker = Speaker::Agent;
                    inAgent = true;
                    // The header's own "●" becomes the bullet, in Claude's orange: an agent is Claude at work too.
                    DropLeadMark(spans, 1);
                    AppendMark(prefix.first, kGlyphBullet, kConvClaude, kConvBg);
                    AppendBlanks(prefix.rest, 2, kConvBg);
                }
                else
                {
                    speaker = Speaker::Peer;
                    inAgent = false;
                    for (auto& s : spans)
                    {
                        if (s.flags & kScreenCellBold)
                        {
                            s.fg = kConvPeer;
                        }
                    }
                }
                w.Text(spans, prefix, TextStyle{});
                continue;
            }

            // ----- a message (on its band) or a reply (after Claude's bullet): the quote under a label -----
            if (!top && b.containers[0].kind == MdContainerKind::Quote && speaker != Speaker::None)
            {
                const uint32_t quote = b.containers[0].id;
                const bool firstOfQuote = quote != speakerQuote;
                speakerQuote = quote;
                Prefix prefix;
                TextStyle style;
                RunColors colors;
                if (speaker == Speaker::User || speaker == Speaker::Peer)
                {
                    // Claude Code's prompt: "❯ " and the text on a gray band across the whole row.
                    style.bg = kConvPromptBand;
                    style.fill = true;
                    colors.text = kConvBright;
                    if (firstOfQuote)
                    {
                        AppendMark(prefix.first, kGlyphPrompt, speaker == Speaker::Peer ? kConvPeer : kConvPromptMark, kConvPromptBand, kScreenCellBold);
                    }
                    else
                    {
                        AppendBlanks(prefix.first, 2, kConvPromptBand);
                    }
                    AppendBlanks(prefix.rest, 2, kConvPromptBand);
                }
                else
                {
                    // Claude's text: "● " before its first block, the rest hanging under it (an agent's one step further in).
                    if (inAgent)
                    {
                        AppendBlanks(prefix.first, 2, kConvBg);
                        AppendBlanks(prefix.rest, 2, kConvBg);
                    }
                    if (firstOfQuote)
                    {
                        AppendMark(prefix.first, kGlyphBullet, kConvBright, kConvBg);
                    }
                    else
                    {
                        AppendBlanks(prefix.first, 2, kConvBg);
                    }
                    AppendBlanks(prefix.rest, 2, kConvBg);
                    // "(no reply)" / "(no text - the turn ended on a tool call)" is the turns view's note, not Claude's words.
                    const std::wstring text = BlockText(b);
                    if (b.containers.size() == 1 && b.kind == MdBlockKind::Paragraph && text.rfind(L"(no ", 0) == 0 && !text.empty() && text.back() == L')')
                    {
                        colors.text = kConvDim;
                        colors.flags = kScreenCellItalic;
                    }
                }
                AppendContainerPrefix(b, 1, prefix, style.bg);
                LayoutContentBlock(w, b, std::move(prefix), colors, style);
                continue;
            }

            // ----- a speaker's tool-call list: "● Bash - ... · ok" in the call's color, "⎿ " before what it returned -----
            if (!top && areaOf[i] >= 0 && b.containers[0].kind == MdContainerKind::Item)
            {
                Prefix prefix;
                if (inAgent)
                {
                    AppendBlanks(prefix.first, 2, kConvBg);
                    AppendBlanks(prefix.rest, 2, kConvBg);
                }
                RunColors colors;
                const bool nested = b.containers.size() >= 2 && b.containers[1].kind == MdContainerKind::Item;
                size_t from = 1;
                if (!nested)
                {
                    if (!b.containers[0].marker.empty())
                    {
                        // The item's first block: its bullet says what it is.
                        const ItemKind kind = ItemKindOf(b);
                        if (kind == ItemKind::Empty && b.containers.size() == 1)
                        {
                            // "(no tool calls)" / "(no remarks)" is the turns view's note for an empty timeline. Claude Code's
                            // screen shows nothing there - the reply follows the prompt - and a row per such turn adds up.
                            skipGap = true;
                            continue;
                        }
                        std::vector<Span> spans;
                        uint32_t bullet = kConvDim;
                        wchar_t glyph = kGlyphBullet;
                        switch (kind)
                        {
                        case ItemKind::ToolCall:
                            bullet = ToolCallColor(b);
                            break;
                        case ItemKind::Remark:
                            bullet = kConvBright;
                            break;
                        case ItemKind::Compaction:
                            glyph = kGlyphStar;
                            bullet = kConvClaude;
                            colors.text = kConvDim;
                            colors.flags = kScreenCellItalic;
                            break;
                        case ItemKind::Message:
                            bullet = kConvPeer;
                            break;
                        case ItemKind::Prompt:
                            glyph = kGlyphPrompt;
                            bullet = kConvPromptMark;
                            colors.text = kConvBright;
                            break;
                        case ItemKind::Empty:
                            colors.text = kConvDim;
                            colors.flags = kScreenCellItalic;
                            break;
                        case ItemKind::Other:
                        default:
                            break;
                        }
                        AppendMark(prefix.first, glyph, bullet, kConvBg);
                        AppendBlanks(prefix.rest, 2, kConvBg);
                        if (kind == ItemKind::Remark || kind == ItemKind::Compaction || kind == ItemKind::Prompt)
                        {
                            // The bullet says it now: the emoji or the mark the turns view starts the line with goes.
                            spans = SpansOf(b, colors);
                            DropLeadMark(spans, kind == ItemKind::Prompt ? 1 : 2);
                            ClampPrefix(prefix.first);
                            ClampPrefix(prefix.rest);
                            w.Text(spans, prefix, TextStyle{});
                            continue;
                        }
                    }
                    else
                    {
                        // A later block of the item (a full Bash command, a Write's content): under the item's text.
                        AppendBlanks(prefix.first, 2, kConvBg);
                        AppendBlanks(prefix.rest, 2, kConvBg);
                    }
                }
                else
                {
                    // An item nested under a call (its first output line, a background task's end, the user's answer):
                    // "  ⎿  " and dimmed, as Claude Code shows what a call returned.
                    AppendBlanks(prefix.first, 2, kConvBg);
                    AppendBlanks(prefix.rest, 2, kConvBg);
                    if (!b.containers[1].marker.empty())
                    {
                        prefix.first.push_back(MakeCell(std::wstring(1, kGlyphResult), kConvDim, kConvBg));
                        AppendBlanks(prefix.first, 2, kConvBg);
                    }
                    else
                    {
                        AppendBlanks(prefix.first, 3, kConvBg);
                    }
                    AppendBlanks(prefix.rest, 3, kConvBg);
                    colors.text = kConvDim;
                    colors.bold = kConvText;
                    from = 2;
                }
                AppendContainerPrefix(b, from, prefix, kConvBg);
                LayoutContentBlock(w, b, std::move(prefix), colors, TextStyle{});
                continue;
            }

            // ----- anything else: plain Markdown, dimmed outside a message (the legend, the session chain, the notes) -----
            {
                Prefix prefix;
                if (inAgent)
                {
                    AppendBlanks(prefix.first, 2, kConvBg);
                    AppendBlanks(prefix.rest, 2, kConvBg);
                }
                AppendContainerPrefix(b, 0, prefix, kConvBg);
                RunColors colors;
                if (speaker == Speaker::None || top)
                {
                    colors.text = kConvDim;
                    colors.bold = kConvText;
                }
                if (b.kind == MdBlockKind::Paragraph && AllItalic(b))
                {
                    colors.text = kConvDim;
                    colors.flags = kScreenCellItalic;
                }
                LayoutContentBlock(w, b, std::move(prefix), colors, TextStyle{});
            }
        }
        return w.Finish();
    }

    ConversationImageResult RenderConversationImages(const std::filesystem::path& transcript, const ConversationImageOptions& options) noexcept
    {
        ConversationImageResult out;
        try
        {
            const auto now = []() noexcept { return static_cast<int64_t>(::GetTickCount64()); };
            const int64_t started = now();
            // 1. The conversation turn by turn (the chain it continues included), as Copy > Transcript renders it.
            const auto markdown = RenderTranscriptTurnsFile(transcript, TurnsOptionsForLevel(options.level), options.utc);
            out.turnsMs = now() - started;
            if (!markdown || markdown->empty())
            {
                out.error = L"the transcript could not be read";
                return out;
            }
            out.markdownChars = markdown->size();

            // 2. How many rows fit: the font found decides the cell height, so the cap on rows comes from it.
            const int cols = std::clamp(options.cols, kMinSheetCols, kScreenSnapshotMaxCols);
            const ScreenImageGrid grid = MeasureTallScreenImageGrid(L"", cols, options.image, out.fontFamily, out.error);
            if (grid.cellHeightPx <= 0)
            {
                return out; // `error` says why (no monospace font, DirectWrite unavailable)
            }
            const size_t maxParts = (std::max)(size_t{ 1 }, options.maxParts);
            const int padPx = (std::max)(0, options.image.paddingPx);
            const int rowsPerPart = (std::max)(1, ((std::max)(options.image.maxPartHeightPx, 0) - 2 * padPx) / grid.cellHeightPx);
            const int64_t rowCap = static_cast<int64_t>(rowsPerPart) * static_cast<int64_t>(maxParts);

            // 3. The blocks, laid out on the grid. A block of more than kConversationBlockMaxChars is cut (and says so).
            const int64_t layoutStarted = now();
            MdParseOptions parse;
            parse.softBreaksAsLineBreaks = true;
            parse.maxBlockChars = kConversationBlockMaxChars;
            const MdParseResult parsed = ParseMarkdownBlocks(*markdown, parse);
            if (parsed.failed && parsed.blocks.empty())
            {
                out.error = L"the conversation's Markdown could not be parsed";
                return out;
            }
            ConversationSheetOptions sheetOptions;
            sheetOptions.cols = cols;
            sheetOptions.maxRows = static_cast<int>((std::min)(rowCap, static_cast<int64_t>(kTallSnapshotMaxRows)));
            const ConversationSheet sheet = BuildConversationSheet(parsed.blocks, sheetOptions);
            out.layoutMs = now() - layoutStarted;
            out.rows = sheet.snapshot.rows;
            out.droppedRows = sheet.droppedRows;
            out.droppedThroughTurn = sheet.droppedThroughTurn;
            out.turns = sheet.turns;

            // 4. The pictures. Each part is described by the turns its first and last rows belong to (a caption's "turns 3-7").
            const int64_t drawStarted = now();
            const auto turnOf = [&sheet](int row) noexcept {
                return row >= 0 && static_cast<size_t>(row) < sheet.rowTurn.size() ? sheet.rowTurn[static_cast<size_t>(row)] : 0;
            };
            std::vector<ConversationImagePart> parts;
            const auto describe = [&](TallImagePart&& picture, size_t index, size_t count) {
                ConversationImagePart part;
                part.index = index;
                part.count = count;
                part.firstTurn = turnOf(picture.firstRow);
                part.lastTurn = turnOf(picture.firstRow + picture.rowCount - 1);
                part.picture = std::move(picture);
                if (options.onPart)
                {
                    // The caller takes the bytes now; the result keeps what a log line needs.
                    ConversationImagePart kept;
                    kept.index = part.index;
                    kept.count = part.count;
                    kept.firstTurn = part.firstTurn;
                    kept.lastTurn = part.lastTurn;
                    kept.picture.firstRow = part.picture.firstRow;
                    kept.picture.rowCount = part.picture.rowCount;
                    kept.picture.widthPx = part.picture.widthPx;
                    kept.picture.heightPx = part.picture.heightPx;
                    options.onPart(std::move(part));
                    parts.push_back(std::move(kept));
                }
                else
                {
                    parts.push_back(std::move(part));
                }
            };
            const TallScreenImage drawn = RenderTallScreenSnapshotPngs(sheet.snapshot, sheet.cuts, maxParts, options.image, describe);
            out.drawMs = now() - drawStarted;
            out.parts = std::move(parts);
            out.grid = drawn.grid;
            if (!drawn.fontFamily.empty())
            {
                out.fontFamily = drawn.fontFamily;
            }
            out.error = drawn.error;
            return out;
        }
        catch (...)
        {
            // Rule #18: the throw site goes to hooks.log; the caller gets a reason (and keeps nothing half-made).
            LogSwallowedException(L"RenderConversationImages");
            ConversationImageResult failed;
            failed.error = L"an exception while drawing the conversation";
            return failed;
        }
    }
}
