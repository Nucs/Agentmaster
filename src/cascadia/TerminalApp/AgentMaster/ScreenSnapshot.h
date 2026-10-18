// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - ScreenSnapshot: a terminal's live screen as a STYLED snapshot (MESSAGING.md section 21.24).
//
// What it is for: the Telegram card's "Screen Image" button sends a session's terminal as a PICTURE that looks the way
// the terminal draws it (colors, bold, box drawing), where "Screen Text" can only send its characters. The pixels of a
// tab cannot be captured: a background tab draws nothing (RENDERING.md section 4c: hidden terminals stop painting and
// release their GPU device) and XAML cannot capture a swap chain. So the picture is drawn again from the TEXT BUFFER:
// ControlCore::ReadScreenSnapshot reads the active buffer's live viewport under the terminal's read lock (each cell's
// glyph, its width, and its colors resolved exactly as the renderer resolves them), serializes it with
// SerializeScreenSnapshot, and hands the text across the WinRT boundary (TermControl lives in another DLL). The Telegram
// service parses it back with ParseScreenSnapshot and draws it off the UI thread (ScreenImage.h).
//
// Header-only and pure (no Win32, no WinRT): the PendingInput.h idiom, so ControlCore.cpp (TerminalControl) and the
// engine (TerminalApp, the m5 harness) share ONE copy of the format and can never disagree on it. Pure-ASCII source.
//
// The text format, version 1 (numbers are decimal unless they say hex; every line ends with '\n'):
//   AMSS1 <cols> <rows> <defaultFg hex6> <defaultBg hex6> <faceLen>:<face>
//   <attrCount>
//   <fg hex6><bg hex6><flags hex2>                 one line per distinct cell style, attrCount lines
//   <cells>                                        one line per row (rows lines): its cells left to right, each written
//                                                  as <attr>,<width>,<len>:<text> with no separator between cells
// A cell's text is EXACTLY <len> UTF-16 code units (the grapheme cluster the buffer stores), so any character can be in it
// - a ':' or a digit or even a line break - without escaping. A wide cell (width 2) covers two columns; its right half is
// not listed. The cells a row leaves out at its END are blank (a space in the default colors): trailing blank cells are
// never written, which keeps a mostly empty screen to a few hundred characters.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Agentmaster
{
    // ----- a cell's style bits (ScreenCell::flags) -----
    // The colors of a cell are RESOLVED already when the snapshot is taken (RenderSettings::GetAttributeColors: the color
    // table, bright-for-intense, faint, reverse video, invisible), so only what changes the DRAWING of a glyph is a flag.

    /// <summary>The glyph is drawn with a bold face (the buffer's "intense" attribute).</summary>
    inline constexpr uint8_t kScreenCellBold = 0x01;
    /// <summary>The glyph is drawn with an italic face.</summary>
    inline constexpr uint8_t kScreenCellItalic = 0x02;
    /// <summary>A line under the cell (every underline style of the buffer - single, double, curly, dotted, dashed - draws one).</summary>
    inline constexpr uint8_t kScreenCellUnderline = 0x04;
    /// <summary>A line through the cell.</summary>
    inline constexpr uint8_t kScreenCellStrikethrough = 0x08;
    /// <summary>Every bit a version-1 snapshot may carry: a parsed style with any other bit is refused (a newer, unknown format).</summary>
    inline constexpr uint8_t kScreenCellFlagsMask = 0x0F;

    // ----- limits (a snapshot is internet-bound pixels and crosses a DLL boundary: every size is bounded) -----

    /// <summary>The widest snapshot (columns). A real terminal is rarely past 300; the bound only refuses garbage.</summary>
    inline constexpr int kScreenSnapshotMaxCols = 1000;
    /// <summary>The tallest snapshot (rows).</summary>
    inline constexpr int kScreenSnapshotMaxRows = 500;
    /// <summary>
    /// A cell's text is cut at this many UTF-16 code units (never inside a surrogate pair). A grapheme cluster is normally
    /// 1-11 units (an emoji ZWJ sequence is the long case); only a run of stacked combining marks gets near the bound, and the
    /// picture of such a cell loses nothing a reader could see.
    /// </summary>
    inline constexpr size_t kScreenCellMaxChars = 64;
    /// <summary>A serialized snapshot longer than this (UTF-16 code units) is refused by the parser before it reads it.</summary>
    inline constexpr size_t kScreenSnapshotMaxChars = 8u * 1024u * 1024u;

    /// <summary>One cell of the screen, as the renderer will draw it.</summary>
    struct ScreenCell
    {
        std::wstring text; // the cell's grapheme cluster; "" or " " = blank (only its background is drawn)
        uint8_t width{ 1 }; // 1, or 2 for a wide glyph (CJK, most emoji): the cell covers two columns
        uint32_t fg{ 0 }; // the foreground as 0xRRGGBB, resolved (see the flags' note above)
        uint32_t bg{ 0 }; // the background as 0xRRGGBB, resolved
        uint8_t flags{ 0 }; // kScreenCell* bits
    };

    /// <summary>A terminal's screen: its size, its default colors, its font, and every row's cells.</summary>
    struct ScreenSnapshot
    {
        int cols{ 0 }; // the screen's width in columns (1..kScreenSnapshotMaxCols)
        int rows{ 0 }; // the screen's height in rows (1..kScreenSnapshotMaxRows)
        uint32_t defaultFg{ 0xCCCCCC }; // 0xRRGGBB: the color of text that names no color of its own
        uint32_t defaultBg{ 0x0C0C0C }; // 0xRRGGBB: the screen's background (what the picture is filled with)
        std::wstring fontFace; // the terminal's font family ("" = unknown: the renderer picks a monospace family)
        std::vector<std::vector<ScreenCell>> lines; // exactly `rows` rows; each row's cells left to right (trailing blanks may be absent)
    };

    namespace detail
    {
        /// <summary>Appends `value` as exactly `digits` lowercase hex digits (the high bits beyond them are dropped).</summary>
        /// <param name="out">The text to append to.</param>
        /// <param name="value">The number.</param>
        /// <param name="digits">How many hex digits (1..8).</param>
        /// <exception cref="std::bad_alloc">If the text cannot grow.</exception>
        inline void SnapAppendHex(std::wstring& out, uint32_t value, int digits)
        {
            static constexpr wchar_t kHex[] = L"0123456789abcdef";
            for (int i = digits - 1; i >= 0; --i)
            {
                out += kHex[(value >> (i * 4)) & 0xFu];
            }
        }

        /// <summary>
        /// Whether a cell draws nothing but the default background: a space (or nothing) in one column, the default
        /// background, and no line under or through it. Such cells at a row's END are not written (the reader fills them).
        /// </summary>
        /// <param name="cell">The cell.</param>
        /// <param name="defaultBg">The screen's default background.</param>
        /// <returns>True for a blank cell.</returns>
        inline bool SnapIsBlank(const ScreenCell& cell, uint32_t defaultBg) noexcept
        {
            return (cell.text.empty() || cell.text == L" ") && cell.width == 1 && (cell.bg & 0xFFFFFFu) == (defaultBg & 0xFFFFFFu) &&
                   (cell.flags & (kScreenCellUnderline | kScreenCellStrikethrough)) == 0;
        }

        /// <summary>
        /// Cuts a cell's text to kScreenCellMaxChars code units without splitting a surrogate pair (a high surrogate left
        /// alone at the cut would be an invalid character the renderer cannot draw).
        /// </summary>
        /// <param name="text">The cell's text.</param>
        /// <returns>The text, at most kScreenCellMaxChars units long.</returns>
        inline std::wstring_view SnapCellText(std::wstring_view text) noexcept
        {
            if (text.size() <= kScreenCellMaxChars)
            {
                return text;
            }
            size_t cut = kScreenCellMaxChars;
            if (cut > 0 && text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF)
            {
                --cut; // a high surrogate whose low half is past the cut: leave the pair out whole
            }
            return text.substr(0, cut);
        }

        /// <summary>The parser's cursor over a serialized snapshot: each read either advances or reports failure.</summary>
        struct SnapReader
        {
            std::wstring_view text;
            size_t pos{ 0 };

            /// <summary>Consumes `c` when it is the next character.</summary>
            /// <param name="c">The expected character.</param>
            /// <returns>True when it was there (and is now consumed).</returns>
            bool Take(wchar_t c) noexcept
            {
                if (pos < text.size() && text[pos] == c)
                {
                    ++pos;
                    return true;
                }
                return false;
            }

            /// <summary>
            /// Reads a decimal number of 1..9 digits (a longer one is refused: no field of the format needs more, and the bound
            /// keeps the value far from overflow).
            /// </summary>
            /// <param name="value">The number read (unchanged on failure).</param>
            /// <returns>True when a number was read.</returns>
            bool Decimal(size_t& value) noexcept
            {
                size_t v = 0;
                size_t digits = 0;
                while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9')
                {
                    if (++digits > 9)
                    {
                        return false;
                    }
                    v = v * 10 + static_cast<size_t>(text[pos] - L'0');
                    ++pos;
                }
                if (digits == 0)
                {
                    return false;
                }
                value = v;
                return true;
            }

            /// <summary>Reads exactly `digits` hex digits (either case).</summary>
            /// <param name="digits">How many (1..8).</param>
            /// <param name="value">The number read (unchanged on failure).</param>
            /// <returns>True when all of them were hex digits.</returns>
            bool Hex(int digits, uint32_t& value) noexcept
            {
                if (text.size() - pos < static_cast<size_t>(digits))
                {
                    return false;
                }
                uint32_t v = 0;
                for (int i = 0; i < digits; ++i)
                {
                    const wchar_t c = text[pos + static_cast<size_t>(i)];
                    uint32_t d = 0;
                    if (c >= L'0' && c <= L'9')
                    {
                        d = static_cast<uint32_t>(c - L'0');
                    }
                    else if (c >= L'a' && c <= L'f')
                    {
                        d = static_cast<uint32_t>(c - L'a' + 10);
                    }
                    else if (c >= L'A' && c <= L'F')
                    {
                        d = static_cast<uint32_t>(c - L'A' + 10);
                    }
                    else
                    {
                        return false;
                    }
                    v = (v << 4) | d;
                }
                pos += static_cast<size_t>(digits);
                value = v;
                return true;
            }

            /// <summary>Reads exactly `count` code units as they are (a length-prefixed text).</summary>
            /// <param name="count">How many.</param>
            /// <param name="out">The text read (unchanged on failure).</param>
            /// <returns>True when that many were left.</returns>
            bool Raw(size_t count, std::wstring_view& out) noexcept
            {
                if (text.size() - pos < count)
                {
                    return false;
                }
                out = text.substr(pos, count);
                pos += count;
                return true;
            }
        };
    }

    /// <summary>
    /// Writes a snapshot in the text format of this header's top comment (version 1). The rows are written as given, with
    /// each row's trailing blank cells left out (detail::SnapIsBlank) and every cell's text cut to kScreenCellMaxChars; a
    /// width other than 2 is written as 1, the colors keep their low 24 bits and the flags their known bits.
    /// </summary>
    /// <param name="snapshot">The snapshot (its `lines` should hold `rows` rows; missing rows are written empty, extra rows are dropped).</param>
    /// <returns>The text ParseScreenSnapshot reads back.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    /// <remarks>
    /// Pure and deterministic: the same snapshot always gives the same text (the style table is in first-use order), which
    /// the m5 suite's round trips rely on. A face name's line breaks are written as spaces (the face sits on the header line;
    /// its length prefix would carry them, but a face name never legitimately holds one).
    /// </remarks>
    inline std::wstring SerializeScreenSnapshot(const ScreenSnapshot& snapshot)
    {
        const int rows = snapshot.rows < 0 ? 0 : snapshot.rows;
        std::wstring face = snapshot.fontFace;
        for (auto& c : face)
        {
            if (c == L'\n' || c == L'\r')
            {
                c = L' ';
            }
        }
        // The style table, in first-use order: a key packs fg (24 bits), bg (24 bits) and the flags (8 bits).
        std::vector<uint64_t> styles;
        std::unordered_map<uint64_t, size_t> styleIndex;
        const auto keyOf = [](const ScreenCell& c) {
            return (static_cast<uint64_t>(c.fg & 0xFFFFFFu) << 32) | (static_cast<uint64_t>(c.bg & 0xFFFFFFu) << 8) | static_cast<uint64_t>(c.flags & kScreenCellFlagsMask);
        };
        std::vector<size_t> rowEnds(static_cast<size_t>(rows), 0);
        for (int y = 0; y < rows; ++y)
        {
            if (static_cast<size_t>(y) >= snapshot.lines.size())
            {
                break;
            }
            const auto& line = snapshot.lines[static_cast<size_t>(y)];
            size_t end = line.size();
            while (end > 0 && detail::SnapIsBlank(line[end - 1], snapshot.defaultBg))
            {
                --end; // the reader fills a row's missing end with blanks
            }
            rowEnds[static_cast<size_t>(y)] = end;
            for (size_t i = 0; i < end; ++i)
            {
                const uint64_t key = keyOf(line[i]);
                if (styleIndex.emplace(key, styles.size()).second)
                {
                    styles.push_back(key);
                }
            }
        }
        std::wstring out;
        out.reserve(64 + styles.size() * 15 + static_cast<size_t>(rows) * 16);
        out += L"AMSS1 ";
        out += std::to_wstring(snapshot.cols < 0 ? 0 : snapshot.cols);
        out += L' ';
        out += std::to_wstring(rows);
        out += L' ';
        detail::SnapAppendHex(out, snapshot.defaultFg, 6);
        out += L' ';
        detail::SnapAppendHex(out, snapshot.defaultBg, 6);
        out += L' ';
        out += std::to_wstring(face.size());
        out += L':';
        out += face;
        out += L'\n';
        out += std::to_wstring(styles.size());
        out += L'\n';
        for (const uint64_t key : styles)
        {
            detail::SnapAppendHex(out, static_cast<uint32_t>(key >> 32), 6);
            detail::SnapAppendHex(out, static_cast<uint32_t>((key >> 8) & 0xFFFFFFu), 6);
            detail::SnapAppendHex(out, static_cast<uint32_t>(key & 0xFFu), 2);
            out += L'\n';
        }
        for (int y = 0; y < rows; ++y)
        {
            if (static_cast<size_t>(y) < snapshot.lines.size())
            {
                const auto& line = snapshot.lines[static_cast<size_t>(y)];
                for (size_t i = 0; i < rowEnds[static_cast<size_t>(y)]; ++i)
                {
                    const auto& cell = line[i];
                    const std::wstring_view text = detail::SnapCellText(cell.text);
                    out += std::to_wstring(styleIndex[keyOf(cell)]);
                    out += L',';
                    out += cell.width == 2 ? L'2' : L'1';
                    out += L',';
                    out += std::to_wstring(text.size());
                    out += L':';
                    out += text;
                }
            }
            out += L'\n';
        }
        return out;
    }

    /// <summary>
    /// Reads a snapshot written by SerializeScreenSnapshot (version 1). Every size is checked against the limits above and
    /// the screen's own width (a row's cells may not cover more than `cols` columns), so a damaged or hostile text is refused
    /// whole instead of producing a picture of garbage.
    /// </summary>
    /// <param name="text">The serialized snapshot ("" = the terminal could not be read: refused).</param>
    /// <returns>The snapshot, or nullopt when the text is not a valid version-1 snapshot.</returns>
    /// <exception cref="std::bad_alloc">If the snapshot cannot be allocated (it is bounded by kScreenSnapshotMaxChars).</exception>
    /// <remarks>
    /// The rows come back exactly as written: a row ends where its last non-blank cell ended, and the caller (the renderer)
    /// treats the columns after it as blank. Nothing is normalized beyond that - a cell of width 2 in the last column is
    /// refused (it would draw past the screen's edge), but a row that covers fewer columns than `cols` is valid.
    /// </remarks>
    inline std::optional<ScreenSnapshot> ParseScreenSnapshot(std::wstring_view text)
    {
        if (text.empty() || text.size() > kScreenSnapshotMaxChars)
        {
            return std::nullopt;
        }
        detail::SnapReader r{ text, 0 };
        ScreenSnapshot s;
        size_t cols = 0;
        size_t rows = 0;
        size_t faceLen = 0;
        std::wstring_view magic; // the format's name and version: another version is refused, never half-read
        std::wstring_view face;
        if (!(r.Raw(6, magic) && magic == L"AMSS1 " && r.Decimal(cols) && r.Take(L' ') && r.Decimal(rows) && r.Take(L' ') &&
              r.Hex(6, s.defaultFg) && r.Take(L' ') && r.Hex(6, s.defaultBg) && r.Take(L' ') && r.Decimal(faceLen) && r.Take(L':')))
        {
            return std::nullopt;
        }
        if (cols == 0 || rows == 0 || cols > static_cast<size_t>(kScreenSnapshotMaxCols) || rows > static_cast<size_t>(kScreenSnapshotMaxRows) || faceLen > 256)
        {
            return std::nullopt;
        }
        if (!(r.Raw(faceLen, face) && r.Take(L'\n')))
        {
            return std::nullopt;
        }
        s.cols = static_cast<int>(cols);
        s.rows = static_cast<int>(rows);
        s.fontFace = std::wstring{ face };
        size_t styleCount = 0;
        if (!(r.Decimal(styleCount) && r.Take(L'\n')) || styleCount > cols * rows)
        {
            return std::nullopt; // more styles than cells cannot come from a real screen
        }
        struct Style
        {
            uint32_t fg;
            uint32_t bg;
            uint8_t flags;
        };
        std::vector<Style> styles;
        styles.reserve(styleCount);
        for (size_t i = 0; i < styleCount; ++i)
        {
            uint32_t fg = 0;
            uint32_t bg = 0;
            uint32_t flags = 0;
            if (!(r.Hex(6, fg) && r.Hex(6, bg) && r.Hex(2, flags) && r.Take(L'\n')) || (flags & ~static_cast<uint32_t>(kScreenCellFlagsMask)) != 0)
            {
                return std::nullopt;
            }
            styles.push_back({ fg, bg, static_cast<uint8_t>(flags) });
        }
        s.lines.resize(rows);
        for (size_t y = 0; y < rows; ++y)
        {
            auto& line = s.lines[y];
            size_t covered = 0; // the columns this row's cells cover so far
            while (!r.Take(L'\n'))
            {
                size_t style = 0;
                size_t width = 0;
                size_t len = 0;
                std::wstring_view cellText;
                if (!(r.Decimal(style) && r.Take(L',') && r.Decimal(width) && r.Take(L',') && r.Decimal(len) && r.Take(L':') && r.Raw(len, cellText)))
                {
                    return std::nullopt; // a cut-off or malformed cell (this also ends the loop at the end of the text)
                }
                if (style >= styles.size() || (width != 1 && width != 2) || len > kScreenCellMaxChars || covered + width > cols)
                {
                    return std::nullopt;
                }
                ScreenCell cell;
                cell.text = std::wstring{ cellText };
                cell.width = static_cast<uint8_t>(width);
                cell.fg = styles[style].fg;
                cell.bg = styles[style].bg;
                cell.flags = styles[style].flags;
                line.push_back(std::move(cell));
                covered += width;
            }
        }
        if (r.pos != text.size())
        {
            return std::nullopt; // trailing bytes: not a snapshot this version wrote
        }
        return s;
    }
}
