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

            /// <summary>
            /// The turn the rows written from now on belong to. The FIRST real turn heading also claims every row kept above it: the
            /// path legend, the chain block and the notes before the first turn of a whole conversation (which the captions count
            /// as turn 1 anyway), and - §21.39's Last Turn - the newest turn's own "_Before this turn" note, which RenderTurn writes
            /// above its heading: without the claim a Last Turn picture of turn 31 would start at "turn 0" and be captioned
            /// "turns 1-31".
            /// </summary>
            /// <param name="turn">Its number (0 = the pseudo-turn of a transcript that opens mid-turn, which claims nothing).</param>
            void SetTurn(int turn) noexcept
            {
                if (_highestTurn == 0 && turn > 0)
                {
                    // Every row kept so far was written before any real turn (turn 0); rows the cap dropped already were counted
                    // with the turn they had then.
                    for (int& kept : _turns)
                    {
                        kept = turn;
                    }
                }
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
            /// One row built whole by its caller (a table's border or one of its text lines, LayoutTableBlock): stored as it is,
            /// with no wrapping. The caller keeps it within the sheet's width — a wider row would be drawn past the picture's
            /// right edge, since the tall renderer sizes every part by the sheet's columns.
            /// </summary>
            /// <param name="row">The row's cells, its prefix included (and its band's padding, when the block has one).</param>
            /// <param name="cut">Its cut value (kTallCut*): kTallCutBlock or the block's own for a block's first row, else kTallCutAny.</param>
            /// <exception cref="std::bad_alloc">If the row cannot be stored.</exception>
            void Row(std::vector<ScreenCell>&& row, uint8_t cut)
            {
                _Push(std::move(row), cut);
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

        /// <summary>Reads a unified-diff hunk header ("@@ -12,3 +14,5 @@ any text"): where the hunk starts in the old and the new file.</summary>
        /// <param name="line">The header line (it starts with "@@").</param>
        /// <param name="oldStart">The old file's first line of the hunk (unchanged when the header does not parse).</param>
        /// <param name="newStart">The new file's first line of the hunk (unchanged when the header does not parse).</param>
        /// <returns>True when both numbers were read; false for "@@ @@" or a malformed header (the hunk is then drawn without numbers).</returns>
        bool ParseHunkHeader(std::wstring_view line, int& oldStart, int& newStart) noexcept
        {
            // At most 9 digits a number: a line number never needs more, and the bound keeps the int far from overflow.
            const auto readNumber = [line](size_t& i, int& value) noexcept {
                int v = 0;
                size_t digits = 0;
                while (i < line.size() && line[i] >= L'0' && line[i] <= L'9' && digits < 9)
                {
                    v = v * 10 + (line[i] - L'0');
                    ++i;
                    ++digits;
                }
                if (digits == 0)
                {
                    return false;
                }
                value = v;
                return true;
            };
            if (line.substr(0, 2) != L"@@")
            {
                return false;
            }
            size_t i = line.find(L'-', 2);
            int oldValue = 0;
            if (i == std::wstring_view::npos || !readNumber(++i, oldValue))
            {
                return false;
            }
            i = line.find(L'+', i);
            int newValue = 0;
            if (i == std::wstring_view::npos || !readNumber(++i, newValue))
            {
                return false;
            }
            oldStart = oldValue;
            newStart = newValue;
            return true;
        }

        /// <summary>
        /// A ```diff block as Claude Code draws a file change (TranscriptTurns' editDiffs hunks, or a diff quoted in a message): one
        /// row per line (a long line breaks at the edge, under the text), led by a gutter with the line's number (the new file's
        /// for an added or context line, the old file's for a removed one, counted from the hunk headers; blank without them)
        /// and its mark. An ADDED line sits on Claude Code's green band, a REMOVED one on its red band, a context line plain; the
        /// hunk headers draw nothing, a second hunk is announced by a dim vertical ellipsis. A quoted git diff's file header -
        /// before a file's first hunk, a "diff" or "index" line and a "--- a/x" line together with the "+++ b/x" line under it -
        /// is dim, not a change; alone, "--- x" is a REMOVED line "-- x" (a diff without hunk headers, as the turns view's
        /// old_string / new_string fallback writes it, can remove a SQL comment).
        /// </summary>
        /// <param name="w">The writer.</param>
        /// <param name="b">The code block (info "diff").</param>
        /// <param name="outer">The cells before the gutter: the block's containers (a call's result corner, an agent's indent).</param>
        /// <exception cref="std::bad_alloc">If a row cannot be built.</exception>
        void LayoutDiffBlock(SheetWriter& w, const MdBlock& b, const Prefix& outer)
        {
            std::vector<std::wstring_view> lines;
            {
                std::wstring_view rest{ b.code };
                for (;;)
                {
                    const size_t nl = rest.find(L'\n');
                    lines.push_back(rest.substr(0, nl));
                    if (nl == std::wstring_view::npos)
                    {
                        break;
                    }
                    rest.remove_prefix(nl + 1);
                }
            }
            const auto startsWith = [](std::wstring_view line, std::wstring_view head) noexcept { return line.substr(0, head.size()) == head; };
            // What each line is, decided once for both passes below (they must agree, or the gutter is sized for numbers the
            // rows never draw).
            enum class DiffLine : uint8_t
            {
                Hunk, // "@@ -a,b +c,d @@": where the next lines sit in the two files; draws no row
                Header, // a quoted git diff's file header: dim, no number
                Added, // "+...": the new file's line
                Removed, // "-...": the old file's line
                Note, // "\ No newline at end of file": about the line above, not a line of either file
                Context, // " ...": a line of both files (or an empty line: the turns view's block writes a blank one trimmed)
            };
            std::vector<DiffLine> kinds;
            kinds.reserve(lines.size());
            {
                // Before a file's first hunk, where a quoted git diff names its files. A "diff " line can start no change, so it
                // opens the next file of a multi-file diff, whose "---" / "+++" pair is then a header again.
                bool beforeHunk = true;
                for (size_t i = 0; i < lines.size(); ++i)
                {
                    const auto line = lines[i];
                    if (startsWith(line, L"@@"))
                    {
                        kinds.push_back(DiffLine::Hunk);
                        beforeHunk = false;
                    }
                    else if (startsWith(line, L"diff "))
                    {
                        kinds.push_back(DiffLine::Header);
                        beforeHunk = true;
                    }
                    else if (beforeHunk && startsWith(line, L"index "))
                    {
                        kinds.push_back(DiffLine::Header);
                    }
                    else if (beforeHunk && startsWith(line, L"--- ") && i + 1 < lines.size() && startsWith(lines[i + 1], L"+++ "))
                    {
                        // Only as a pair: a lone "--- x" is the removed line "-- x".
                        kinds.push_back(DiffLine::Header);
                        kinds.push_back(DiffLine::Header);
                        ++i;
                    }
                    else if (!line.empty() && line[0] == L'+')
                    {
                        kinds.push_back(DiffLine::Added);
                    }
                    else if (!line.empty() && line[0] == L'-')
                    {
                        kinds.push_back(DiffLine::Removed);
                    }
                    else if (!line.empty() && line[0] == L'\\')
                    {
                        kinds.push_back(DiffLine::Note);
                    }
                    else
                    {
                        kinds.push_back(DiffLine::Context);
                    }
                }
            }
            // Each line's number, counted from its hunk's header: the new file's for an added or context line, the old file's for
            // a removed one, 0 (drawn blank) for a header or a note and in a hunk whose header gave no numbers (a start of 0
            // stays 0). The largest decides the gutter's width, so every row lines up (3 digits at least, as Claude Code's own).
            std::vector<int> numbers(lines.size(), 0);
            int widest = 0;
            {
                int oldLine = 0;
                int newLine = 0;
                for (size_t i = 0; i < lines.size(); ++i)
                {
                    switch (kinds[i])
                    {
                    case DiffLine::Hunk:
                        if (!ParseHunkHeader(lines[i], oldLine, newLine))
                        {
                            oldLine = 0;
                            newLine = 0;
                        }
                        break;
                    case DiffLine::Added:
                        numbers[i] = newLine;
                        newLine += newLine > 0 ? 1 : 0;
                        break;
                    case DiffLine::Removed:
                        numbers[i] = oldLine;
                        oldLine += oldLine > 0 ? 1 : 0;
                        break;
                    case DiffLine::Context:
                        numbers[i] = newLine;
                        newLine += newLine > 0 ? 1 : 0;
                        oldLine += oldLine > 0 ? 1 : 0;
                        break;
                    case DiffLine::Header:
                    case DiffLine::Note:
                        break;
                    }
                    widest = (std::max)(widest, numbers[i]);
                }
            }
            int digits = 1;
            for (int v = widest; v >= 10; v /= 10)
            {
                ++digits;
            }
            digits = (std::max)(digits, 3);
            // The rows.
            bool firstRow = true; // the block's first row takes the containers' first-row cells (a result corner)
            bool sawHunk = false; // a hunk of this file was drawn: the next one is announced by the ellipsis
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (kinds[i] == DiffLine::Hunk)
                {
                    if (sawHunk)
                    {
                        // Between two hunks: the lines in between are not shown, and a dim vertical ellipsis says so.
                        Prefix gap;
                        gap.first = firstRow ? outer.first : outer.rest;
                        gap.rest = outer.rest;
                        AppendBlanks(gap.first, digits + 3, kConvBg);
                        AppendBlanks(gap.rest, digits + 3, kConvBg);
                        TextStyle style;
                        style.cut = kTallCutAny;
                        w.Text({ Span{ std::wstring(1, L'\x22EE'), kConvDim, 0 } }, gap, style);
                        firstRow = false;
                    }
                    sawHunk = true;
                    continue;
                }
                uint32_t bg = kConvBg;
                uint32_t fg = kConvText;
                uint8_t flags = 0;
                wchar_t mark = L' ';
                std::wstring_view text = lines[i];
                switch (kinds[i])
                {
                case DiffLine::Header:
                    fg = kConvDim; // a quoted git diff's file header: not a change
                    sawHunk = false; // the next hunk is this file's first: no ellipsis before it
                    break;
                case DiffLine::Added:
                    bg = kConvDiffAddedBg;
                    fg = kConvBright;
                    mark = L'+';
                    text.remove_prefix(1);
                    break;
                case DiffLine::Removed:
                    bg = kConvDiffRemovedBg;
                    fg = kConvBright;
                    mark = L'-';
                    text.remove_prefix(1);
                    break;
                case DiffLine::Note:
                    fg = kConvDim; // "\ No newline at end of file": a note about the line above, not a line
                    flags = kScreenCellItalic;
                    break;
                case DiffLine::Context:
                case DiffLine::Hunk: // (handled above)
                    // The leading blank is the diff's mark. Only a blank is taken: a line without one (a hand-written diff in a
                    // message) is drawn whole rather than losing its first character.
                    if (!text.empty() && text[0] == L' ')
                    {
                        text.remove_prefix(1);
                    }
                    break;
                }
                Prefix prefix;
                prefix.first = firstRow ? outer.first : outer.rest;
                prefix.rest = outer.rest;
                // The gutter: the number right-aligned, then the mark between blanks - on the line's band, so a change reads as
                // one colored row from its number to the sheet's edge.
                const std::wstring numberText = numbers[i] > 0 ? std::to_wstring(numbers[i]) : std::wstring{};
                AppendBlanks(prefix.first, (std::max)(0, digits - static_cast<int>(numberText.size())), bg);
                for (const wchar_t c : numberText)
                {
                    prefix.first.push_back(MakeCell(std::wstring(1, c), kConvDim, bg));
                }
                prefix.first.push_back(MakeCell(L" ", kConvText, bg));
                prefix.first.push_back(MakeCell(std::wstring(1, mark), kConvBright, bg));
                prefix.first.push_back(MakeCell(L" ", kConvText, bg));
                AppendBlanks(prefix.rest, digits + 3, bg);
                TextStyle style;
                style.bg = bg;
                style.fill = true;
                style.wrapWords = false;
                style.cut = firstRow ? kTallCutBlock : kTallCutAny;
                w.Text({ Span{ std::wstring{ text }, fg, flags } }, prefix, style);
                firstRow = false;
            }
            if (b.truncated)
            {
                Prefix note;
                note.first = firstRow ? outer.first : outer.rest;
                note.rest = outer.rest;
                TextStyle style;
                style.cut = kTallCutAny;
                w.Text({ Span{ L"\x2026 (cut here: the change is longer than " + std::to_wstring(kConversationBlockMaxChars) + L" characters)", kConvDim, kScreenCellItalic } }, note, style);
            }
        }

        // ----- tables: Claude Code's own table (MESSAGING.md §21.39.1) -----
        //
        // A GitHub pipe table (MdBlockKind::Table, parsed with MdParseOptions::tables) is drawn the way Claude Code's TUI draws one
        // — its bundle's table renderer, read in 2.1.322 — so a picture shows the table Claude wrote as the user saw it:
        //   ┌───────┬──────┐
        //   │ Name  │ Size │   the header row centered (bold only where the Markdown is),
        //   ├───────┼──────┤
        //   │ alpha │   12 │   a body cell by its column's alignment (":-" / "-:" / ":-:"; left by default),
        //   ├───────┼──────┤   a rule between every two body rows,
        //   │ beta  │    7 │   a cell shorter than its row centered top to bottom.
        //   └───────┴──────┘
        // The column widths are Claude Code's: every column at its whole text when the table fits; else at least its longest word,
        // the room left shared in proportion to what each column still lacks; else the longest words scaled down and long words
        // broken. Each cell wraps as Claude Code wraps it (wrap-ansi without trimming: WrapTableCell). A table that still does not
        // fit, or whose wrapped row would be more than kTableMaxRowLines lines tall, is drawn as Claude Code's VERTICAL records
        // instead: "Header: value" per cell, a short rule between rows. Before the table's
        // layout existed here the table reached the sheet as its raw "| a | b |" lines — the pipes never lined up (nothing pads
        // a cell, the bold and code marks of a cell are not drawn, a long row wrapped): the owner's report of 2026-11-17.

        // Claude Code 2.1.322's constants (its bundle: `De=3`, `Be=4`, `hs=4`, `_r=200`).
        constexpr int kTableMinColWidth = 3; // the narrowest column (De)
        constexpr int kTableMarginCols = 4; // the terminal's width less this is the widest table drawn as a box (Be)
        constexpr int kTableMaxRowLines = 4; // a row that wraps into more lines than this sends the table to the vertical records (hs)
        constexpr size_t kTableMaxBodyRows = 200; // the body rows drawn; the rest are counted in a note (_r)
        constexpr int kTableRecordRuleMax = 40; // the vertical records' rule: at most this long (Claude Code: min(width - 1, 40))
        // The box's pieces: the top, middle and bottom border's left corner / tee / right corner, and the two lines.
        constexpr wchar_t kBoxTopLeft = L'\x250C';
        constexpr wchar_t kBoxTopTee = L'\x252C';
        constexpr wchar_t kBoxTopRight = L'\x2510';
        constexpr wchar_t kBoxMidLeft = L'\x251C';
        constexpr wchar_t kBoxCross = L'\x253C';
        constexpr wchar_t kBoxMidRight = L'\x2524';
        constexpr wchar_t kBoxBottomLeft = L'\x2514';
        constexpr wchar_t kBoxBottomTee = L'\x2534';
        constexpr wchar_t kBoxBottomRight = L'\x2518';
        constexpr wchar_t kBoxVertical = L'\x2502';
        constexpr wchar_t kBoxHorizontal = L'\x2500';

        /// <summary>
        /// A table cell's Markdown as styled cells on ONE line, before any wrapping: its runs styled as a paragraph's (SpansOf: bold
        /// bright, code lavender, a link underlined), a line break or a TAB as one blank (a cell is one line in GFM), control
        /// characters dropped, a combining mark riding on the cell before it, and the blanks at both ends trimmed.
        /// </summary>
        /// <param name="runs">The cell's runs (MdTableCell).</param>
        /// <param name="colors">The colors of the block's runs.</param>
        /// <param name="bg">The cells' background (the block's: a band inside a prompt).</param>
        /// <returns>The cells (empty for an empty cell).</returns>
        /// <exception cref="std::bad_alloc">If the cells cannot be allocated.</exception>
        std::vector<ScreenCell> TableCellLine(const MdTableCell& runs, const RunColors& colors, uint32_t bg)
        {
            MdBlock holder; // SpansOf reads a block's runs: the cell's are lent to an empty one (no truncation note: it is not truncated)
            holder.runs = runs;
            std::vector<ScreenCell> cells;
            for (const Span& span : SpansOf(holder, colors))
            {
                const std::wstring_view text{ span.text };
                for (size_t i = 0; i < text.size();)
                {
                    size_t units = 1;
                    const char32_t cp = CodePointAt(text, i, units);
                    const std::wstring_view piece = text.substr(i, units);
                    i += units;
                    if (cp == U'\n' || cp == U'\r' || cp == U'\t')
                    {
                        cells.push_back(MakeCell(L" ", span.fg, bg, span.flags));
                        continue;
                    }
                    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
                    {
                        continue; // checked before the width: a control character must never ride on the cell before it
                    }
                    const int width = ClaudeCellWidth(cp);
                    if (width == 0)
                    {
                        // A combining mark, a joiner or a variation selector belongs to the cluster of the cell before it.
                        if (!cells.empty() && cells.back().text.size() + piece.size() <= kScreenCellMaxChars)
                        {
                            cells.back().text.append(piece);
                        }
                        continue;
                    }
                    cells.push_back(MakeCell(std::wstring{ piece }, span.fg, bg, span.flags, static_cast<uint8_t>(width)));
                }
            }
            const auto blank = [](const ScreenCell& c) { return c.text == L" "; };
            while (!cells.empty() && blank(cells.back()))
            {
                cells.pop_back();
            }
            const auto lead = std::find_if_not(cells.begin(), cells.end(), blank);
            cells.erase(cells.begin(), lead);
            return cells;
        }

        /// <summary>The widest word of a cell, in columns (words are split at blanks): the narrowest a column can be without breaking a word.</summary>
        /// <param name="cells">The cell (TableCellLine).</param>
        /// <returns>The width (0 for an empty cell).</returns>
        int LongestWordWidth(const std::vector<ScreenCell>& cells) noexcept
        {
            int longest = 0;
            int current = 0;
            for (const auto& c : cells)
            {
                if (c.text == L" ")
                {
                    longest = (std::max)(longest, current);
                    current = 0;
                    continue;
                }
                current += c.width == 2 ? 2 : 1;
            }
            return (std::max)(longest, current);
        }

        /// <summary>
        /// Wraps a cell's line to `width` columns exactly as Claude Code wraps a table cell: its `xe`, which is wrap-ansi with
        /// `{ hard, trim: false, wordWrap: true }` then drops the empty lines. Words are split at every blank (two blanks in a row
        /// hold an empty word between them) and kept whole; a word that does not fit starts the next line. Two wrap-ansi
        /// behaviors of `trim: false` show in the box, and are kept so a picture matches the terminal:
        /// - the blank at a break stays at the END of the line before it — it pads that line like any blank, so a right-aligned
        ///   or centered line sits a column further left than the cell's last line;
        /// - after a line filled to the column, the next line STARTS with that blank (a column further right).
        /// With `hard` (the widths were scaled below the longest words) a word wider than the column is broken across lines, its
        /// first piece on the current line when that costs no extra line (wrap-ansi's breaksStartingThisLine /
        /// breaksStartingNextLine), each piece filled to the column; a wide glyph that would straddle the edge starts the next
        /// piece. Without `hard` such a word stays whole (Claude Code's widths never let that happen). At least one line: an
        /// empty cell is one empty line.
        /// </summary>
        /// <param name="cells">The cell (TableCellLine: no blanks at its ends).</param>
        /// <param name="width">The column's width (raised to 1).</param>
        /// <param name="hard">Break words wider than the column.</param>
        /// <returns>The lines, top first.</returns>
        /// <exception cref="std::bad_alloc">If the lines cannot be allocated.</exception>
        std::vector<std::vector<ScreenCell>> WrapTableCell(const std::vector<ScreenCell>& cells, int width, bool hard)
        {
            width = (std::max)(1, width);
            // The words as wrap-ansi's `split(" ")` sees them, each with the blank cell before it (its colors kept: a blank
            // inside a code span or a bold run is drawn as that run's blank).
            struct Word
            {
                const ScreenCell* blank = nullptr; // the blank before the word (none before the first)
                std::vector<ScreenCell> cells;
                int columns = 0; // its visible width
            };
            std::vector<Word> words(1);
            for (const auto& c : cells)
            {
                if (c.text == L" ")
                {
                    Word next;
                    next.blank = &c;
                    words.push_back(std::move(next));
                    continue;
                }
                words.back().cells.push_back(c);
                words.back().columns += c.width == 2 ? 2 : 1;
            }
            std::vector<std::vector<ScreenCell>> rows(1);
            int rowW = 0; // the visible width of rows.back()
            const auto newRow = [&]() {
                rows.emplace_back();
                rowW = 0;
            };
            for (size_t i = 0; i < words.size(); ++i)
            {
                Word& word = words[i];
                if (i != 0)
                {
                    // trim:false - a row already full makes a new one, and the blank goes on the row either way: at the end of
                    // a row the next word then leaves, or at the start of the new one.
                    if (rowW >= width)
                    {
                        newRow();
                    }
                    rows.back().push_back(*word.blank);
                    rowW += 1;
                }
                if (hard && word.columns > width)
                {
                    // Start the word on this row when that does not add a break (both counts are wrap-ansi's; the operands are
                    // never negative here — rowW <= width < word.columns — so integer division is its Math.floor).
                    const int remaining = width - rowW;
                    const int breaksStartingHere = 1 + (word.columns - remaining - 1) / width;
                    const int breaksStartingNext = (word.columns - 1) / width;
                    if (breaksStartingNext < breaksStartingHere)
                    {
                        newRow();
                    }
                    for (size_t k = 0; k < word.cells.size(); ++k)
                    {
                        const int cw = word.cells[k].width == 2 ? 2 : 1;
                        if (rowW + cw > width)
                        {
                            newRow(); // a glyph that would pass the edge starts the next piece
                        }
                        rows.back().push_back(std::move(word.cells[k]));
                        rowW += cw;
                        if (rowW == width && k + 1 < word.cells.size())
                        {
                            newRow(); // a filled row: the rest of the word goes on below
                        }
                    }
                    continue;
                }
                if (rowW + word.columns > width && rowW > 0 && word.columns > 0)
                {
                    newRow(); // the word does not fit after what the row holds: it starts the next row
                }
                rows.back().insert(rows.back().end(), std::make_move_iterator(word.cells.begin()), std::make_move_iterator(word.cells.end()));
                rowW += word.columns;
            }
            // Claude Code keeps only the lines that hold something (`filter(line => line.length > 0)`), and at least one.
            rows.erase(std::remove_if(rows.begin(), rows.end(), [](const std::vector<ScreenCell>& row) { return row.empty(); }), rows.end());
            if (rows.empty())
            {
                rows.emplace_back();
            }
            return rows;
        }

        /// <summary>
        /// Claude Code's "… 1,234 more rows not shown" (its `lt`): the count with thousands grouped, "row" for one. Said under a
        /// table whose body has more than kTableMaxBodyRows rows.
        /// </summary>
        /// <param name="hidden">How many body rows are left out (at least 1).</param>
        /// <returns>The note.</returns>
        /// <exception cref="std::bad_alloc">If the note cannot be allocated.</exception>
        std::wstring TableHiddenRowsNote(size_t hidden)
        {
            std::wstring digits = std::to_wstring(hidden);
            std::wstring grouped;
            for (size_t i = 0; i < digits.size(); ++i)
            {
                if (i > 0 && (digits.size() - i) % 3 == 0)
                {
                    grouped += L',';
                }
                grouped += digits[i];
            }
            return L"\x2026 " + grouped + (hidden == 1 ? L" more row not shown" : L" more rows not shown");
        }

        /// <summary>
        /// Lays a pipe table out (the section comment above): Claude Code's box when it fits, its vertical records when it does not.
        /// Each line of the table is one sheet row after the block's prefix (the reply's "● " on the first, the quote bars and the
        /// hanging indent on the rest), on the block's background — a table inside a prompt keeps the prompt's band. The borders
        /// are in the block's text color, as Claude Code prints them unstyled.
        /// </summary>
        /// <param name="w">The writer.</param>
        /// <param name="b">The table block (its header row first; at least one row and one column).</param>
        /// <param name="prefix">The cells its rows start with (clamped already).</param>
        /// <param name="colors">The colors of its runs.</param>
        /// <param name="style">The background, the band and the first row's cut value.</param>
        /// <exception cref="std::bad_alloc">If a row cannot be built or stored.</exception>
        void LayoutTableBlock(SheetWriter& w, const MdBlock& b, const Prefix& prefix, const RunColors& colors, const TextStyle& style)
        {
            const size_t cols = b.tableAlign.size();
            if (cols == 0 || b.tableRows.empty())
            {
                return; // the parser never makes one: nothing to draw
            }
            // The room after the prefix, counted as Claude Code counts its terminal: its widths take kTableMarginCols off the
            // terminal's width, two of them for the "● " indent a reply is drawn after — which the sheet took off the room already.
            const int room = (std::max)(1, w.Cols() - CellsWidth(prefix.first));
            const int terminalCols = room + 2;
            const uint32_t border = colors.text;
            bool firstRow = true;
            const auto emit = [&](std::vector<ScreenCell>&& cells) {
                std::vector<ScreenCell> row = firstRow ? prefix.first : prefix.rest;
                const uint8_t cut = firstRow ? style.cut : kTallCutAny;
                firstRow = false;
                row.insert(row.end(), std::make_move_iterator(cells.begin()), std::make_move_iterator(cells.end()));
                if (style.fill)
                {
                    for (int used = CellsWidth(row); used < w.Cols(); ++used)
                    {
                        row.push_back(MakeCell(L" ", kConvText, style.bg));
                    }
                }
                w.Row(std::move(row), cut);
            };
            // The rows drawn: the header and at most kTableMaxBodyRows body rows (Claude Code's widths read only those too).
            const size_t shown = (std::min)(b.tableRows.size(), kTableMaxBodyRows + 1); // (std::min): the <windows.h> macro
            const size_t hidden = b.tableRows.size() - shown;
            std::vector<std::vector<std::vector<ScreenCell>>> text(shown); // [row][column] -> the cell on one line
            for (size_t r = 0; r < shown; ++r)
            {
                text[r].reserve(cols);
                for (size_t c = 0; c < cols; ++c)
                {
                    text[r].push_back(c < b.tableRows[r].cells.size() ? TableCellLine(b.tableRows[r].cells[c], colors, style.bg) : std::vector<ScreenCell>{});
                }
            }
            // The widths (Claude Code's): `ideal` = a column's longest cell on one line, `least` = its longest word, both at least
            // kTableMinColWidth; the content room = the terminal less the borders and padding (1 + 3 per column) and the margin.
            std::vector<int> ideal(cols, kTableMinColWidth);
            std::vector<int> least(cols, kTableMinColWidth);
            for (size_t r = 0; r < shown; ++r)
            {
                for (size_t c = 0; c < cols; ++c)
                {
                    ideal[c] = (std::max)(ideal[c], CellsWidth(text[r][c]));
                    least[c] = (std::max)(least[c], LongestWordWidth(text[r][c]));
                }
            }
            const int columns = static_cast<int>(cols);
            const int chrome = 1 + columns * 3;
            const int contentRoom = (std::max)(terminalCols - chrome - kTableMarginCols, columns * kTableMinColWidth);
            int idealSum = 0;
            int leastSum = 0;
            for (size_t c = 0; c < cols; ++c)
            {
                idealSum += ideal[c];
                leastSum += least[c];
            }
            std::vector<int> widths(cols, kTableMinColWidth);
            bool hard = false;
            if (idealSum <= contentRoom)
            {
                widths = ideal; // the whole table fits on single lines
            }
            else if (leastSum <= contentRoom)
            {
                // Every word fits whole: each column gets its longest word, plus a share of the room left in proportion to how much
                // more its longest cell would take (Claude Code floors each share, as here, so a column or two may stay a cell short).
                const int extra = contentRoom - leastSum;
                int lackSum = 0;
                for (size_t c = 0; c < cols; ++c)
                {
                    lackSum += ideal[c] - least[c];
                }
                for (size_t c = 0; c < cols; ++c)
                {
                    const int share = lackSum == 0 ? 0 : static_cast<int>(static_cast<double>(ideal[c] - least[c]) / lackSum * extra);
                    widths[c] = least[c] + share;
                }
            }
            else
            {
                // Not even the longest words fit: they are scaled down together, and words are broken where they must.
                hard = true;
                const double scale = static_cast<double>(contentRoom) / leastSum;
                for (size_t c = 0; c < cols; ++c)
                {
                    widths[c] = (std::max)(static_cast<int>(least[c] * scale), kTableMinColWidth);
                }
            }
            // Every cell wrapped to its column; a row taller than kTableMaxRowLines, or a box wider than the room, is drawn as the
            // vertical records instead.
            std::vector<std::vector<std::vector<std::vector<ScreenCell>>>> wrapped(shown); // [row][column] -> lines
            size_t tallest = 1;
            for (size_t r = 0; r < shown; ++r)
            {
                wrapped[r].reserve(cols);
                for (size_t c = 0; c < cols; ++c)
                {
                    wrapped[r].push_back(WrapTableCell(text[r][c], widths[c], hard));
                    tallest = (std::max)(tallest, wrapped[r].back().size());
                }
            }
            int boxWidth = chrome;
            for (size_t c = 0; c < cols; ++c)
            {
                boxWidth += widths[c];
            }
            const bool vertical = tallest > static_cast<size_t>(kTableMaxRowLines) || boxWidth > terminalCols - kTableMarginCols;
            if (!vertical)
            {
                const auto rule = [&](wchar_t left, wchar_t tee, wchar_t right) {
                    std::vector<ScreenCell> cells;
                    cells.push_back(MakeCell(std::wstring(1, left), border, style.bg));
                    for (size_t c = 0; c < cols; ++c)
                    {
                        for (int k = 0; k < widths[c] + 2; ++k)
                        {
                            cells.push_back(MakeCell(std::wstring(1, kBoxHorizontal), border, style.bg));
                        }
                        cells.push_back(MakeCell(std::wstring(1, c + 1 < cols ? tee : right), border, style.bg));
                    }
                    emit(std::move(cells));
                };
                const auto rowLines = [&](size_t r, bool header) {
                    size_t height = 1;
                    for (const auto& lines : wrapped[r])
                    {
                        height = (std::max)(height, lines.size());
                    }
                    for (size_t line = 0; line < height; ++line)
                    {
                        std::vector<ScreenCell> cells;
                        cells.push_back(MakeCell(std::wstring(1, kBoxVertical), border, style.bg));
                        for (size_t c = 0; c < cols; ++c)
                        {
                            const auto& lines = wrapped[r][c];
                            // A cell shorter than its row sits in the middle of it (Claude Code: floor((height - lines) / 2) above).
                            const size_t above = (height - lines.size()) / 2;
                            const bool has = line >= above && line - above < lines.size();
                            const std::vector<ScreenCell> empty;
                            const std::vector<ScreenCell>& content = has ? lines[line - above] : empty;
                            const int pad = (std::max)(0, widths[c] - CellsWidth(content));
                            const MdAlign align = header ? MdAlign::Center : b.tableAlign[c];
                            const int left = align == MdAlign::Center ? pad / 2 : (align == MdAlign::Right ? pad : 0);
                            cells.push_back(MakeCell(L" ", kConvText, style.bg));
                            AppendBlanks(cells, left, style.bg);
                            cells.insert(cells.end(), content.begin(), content.end());
                            AppendBlanks(cells, pad - left, style.bg);
                            cells.push_back(MakeCell(L" ", kConvText, style.bg));
                            cells.push_back(MakeCell(std::wstring(1, kBoxVertical), border, style.bg));
                        }
                        emit(std::move(cells));
                    }
                };
                rule(kBoxTopLeft, kBoxTopTee, kBoxTopRight);
                rowLines(0, true);
                rule(kBoxMidLeft, kBoxCross, kBoxMidRight);
                for (size_t r = 1; r < shown; ++r)
                {
                    rowLines(r, false);
                    if (r + 1 < shown)
                    {
                        rule(kBoxMidLeft, kBoxCross, kBoxMidRight);
                    }
                }
                rule(kBoxBottomLeft, kBoxBottomTee, kBoxBottomRight);
            }
            else
            {
                // Claude Code's vertical records: per body row, "Header: value" for each cell that holds anything (the header bold),
                // a long value going on under it two columns in, and a short rule between two rows.
                std::vector<std::wstring> headers(cols);
                for (size_t c = 0; c < cols; ++c)
                {
                    for (const auto& cell : text[0][c])
                    {
                        headers[c] += cell.text;
                    }
                }
                const int ruleLength = (std::min)(kTableRecordRuleMax, (std::max)(1, room - 1));
                const auto recordRule = [&]() {
                    std::vector<ScreenCell> cells;
                    for (int k = 0; k < ruleLength; ++k)
                    {
                        cells.push_back(MakeCell(std::wstring(1, kBoxHorizontal), border, style.bg));
                    }
                    emit(std::move(cells));
                };
                // One record line (Claude Code's `Kt`): "Header: value" — the header and its colon bold, as its
                // `${Dr}${T}:${Fr}` — or the value alone when there is no header. The value keeps its Markdown styles, its runs
                // of blanks read as one (TableCellLine already joined its line breaks and trimmed it), and it wraps to the room
                // with its next lines two columns in (Claude Code's "  " before them).
                const auto writeRecord = [&](const std::wstring& header, const std::vector<ScreenCell>& value) {
                    std::vector<Span> spans;
                    if (!header.empty())
                    {
                        spans.push_back(Span{ header + L":", colors.bold, static_cast<uint8_t>(colors.flags | kScreenCellBold) });
                        spans.push_back(Span{ L" ", colors.text, colors.flags });
                    }
                    bool lastBlank = false;
                    for (const auto& cell : value)
                    {
                        const bool isBlank = cell.text == L" ";
                        if (isBlank && lastBlank)
                        {
                            continue;
                        }
                        lastBlank = isBlank;
                        spans.push_back(Span{ cell.text, cell.fg, cell.flags });
                    }
                    Prefix record;
                    record.first = firstRow ? prefix.first : prefix.rest;
                    record.rest = prefix.rest;
                    AppendBlanks(record.rest, 2, style.bg);
                    TextStyle line = style;
                    line.cut = firstRow ? style.cut : kTallCutAny;
                    firstRow = false;
                    w.Text(spans, record, line);
                };
                bool wroteAny = false; // a record line is on the sheet already (the next row's lines then follow a rule)
                if (shown == 1)
                {
                    // A table with only its header row (Claude Code's records then have no headers and the header cells as their
                    // one row): each header cell that holds anything on a line of its own, its Markdown styles kept.
                    for (size_t c = 0; c < cols; ++c)
                    {
                        if (text[0][c].empty())
                        {
                            continue;
                        }
                        writeRecord(std::wstring{}, text[0][c]);
                        wroteAny = true;
                    }
                }
                for (size_t r = 1; r < shown; ++r)
                {
                    bool rowWrote = false;
                    for (size_t c = 0; c < cols; ++c)
                    {
                        if (headers[c].empty() && text[r][c].empty())
                        {
                            continue; // Claude Code skips a cell with no header and no value; an empty value under a header stays
                        }
                        if (!rowWrote && wroteAny)
                        {
                            recordRule(); // between two rows that drew something (a row of empty cells draws neither)
                        }
                        rowWrote = true;
                        writeRecord(headers[c], text[r][c]);
                        wroteAny = true;
                    }
                }
                if (hidden > 0 && wroteAny)
                {
                    recordRule(); // Claude Code rules the hidden rows' note off from the records above it
                }
            }
            if (hidden > 0)
            {
                // Claude Code's own count of the rows it leaves out (a note: dim, like the sheet's other notes).
                Prefix note;
                note.first = firstRow ? prefix.first : prefix.rest;
                note.rest = prefix.rest;
                TextStyle line = style;
                line.cut = firstRow ? style.cut : kTallCutAny;
                firstRow = false;
                w.Text({ Span{ TableHiddenRowsNote(hidden), kConvDim, kScreenCellItalic } }, note, line);
            }
            if (b.truncated)
            {
                // The parser cut the table at kConversationBlockMaxChars (its header and the rows that fit are drawn): said under it.
                Prefix note;
                note.first = firstRow ? prefix.first : prefix.rest;
                note.rest = prefix.rest;
                TextStyle line = style;
                line.cut = firstRow ? style.cut : kTallCutAny;
                firstRow = false;
                w.Text({ Span{ L"\x2026 (cut here: the table is longer than " + std::to_wstring(kConversationBlockMaxChars) + L" characters)", kConvDim, kScreenCellItalic } },
                       note, line);
            }
        }

        /// <summary>
        /// Lays out one block of a message, a reply or a note: a paragraph or a heading as wrapped text, a code block on its band
        /// keeping its spaces (a ```diff block as Claude Code draws a file change: LayoutDiffBlock), a rule as a line across the
        /// room left, a pipe table as Claude Code's box (LayoutTableBlock).
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
            if (b.kind == MdBlockKind::Table)
            {
                // A pipe table Claude (or the user) wrote: Claude Code's own box, or its vertical records when the box does not fit.
                LayoutTableBlock(w, b, prefix, colors, style);
                return;
            }
            if (b.kind == MdBlockKind::Code && b.info == L"diff")
            {
                // A file change (§21.39 "File diffs") or a diff in a message: Claude Code's own diff look, numbers and bands.
                LayoutDiffBlock(w, b, prefix);
                return;
            }
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
                            // A level whose call lines state no outcome (Compact's reason lines) gets the neutral bullet:
                            // ToolCallColor reads green for "no failure written", which there would only mean "not written".
                            bullet = options.callOutcomes ? ToolCallColor(b) : kConvDim;
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
            // 1. The conversation turn by turn (the chain it continues included), as Copy > Transcript renders it - plus, with
            // "File diffs", every file change as a diff (TurnsOptions::editDiffs: Agentmaster's own option, which no level sets),
            // and with "Last Turn" only the newest turn (TurnsOptions::lastTurnOnly, Agentmaster's own as well).
            TurnsOptions turns = TurnsOptionsForLevel(options.level);
            turns.editDiffs = turns.editDiffs || options.editDiffs;
            turns.lastTurnOnly = turns.lastTurnOnly || options.lastTurnOnly;
            const auto markdown = RenderTranscriptTurnsFile(transcript, turns, options.utc);
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
            // §21.39.1: pipe tables become Table blocks, drawn as Claude Code's box (LayoutTableBlock) — without it a table reached
            // the sheet as its raw "| a | b |" lines, whose pipes never line up.
            parse.tables = true;
            const MdParseResult parsed = ParseMarkdownBlocks(*markdown, parse);
            if (parsed.failed && parsed.blocks.empty())
            {
                out.error = L"the conversation's Markdown could not be parsed";
                return out;
            }
            ConversationSheetOptions sheetOptions;
            sheetOptions.cols = cols;
            sheetOptions.maxRows = static_cast<int>((std::min)(rowCap, static_cast<int64_t>(kTallSnapshotMaxRows)));
            // Compact lists its calls as reason lines with no outcome (the call list, ops, is off there): neutral bullets.
            sheetOptions.callOutcomes = turns.ops;
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
