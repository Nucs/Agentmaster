// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - ScreenImage: draws a terminal's screen (a ScreenSnapshot) as a PNG that looks the way a terminal draws it
// (MESSAGING.md section 21.24 - the Telegram card's "Screen Image" button).
//
// Why it is drawn again rather than captured: a background tab draws nothing (RENDERING.md section 4c) and XAML cannot
// capture a swap chain, so the only screen every session has is its TEXT BUFFER. ControlCore::ReadScreenSnapshot reads
// it with every cell's resolved colors (ScreenSnapshot.h); this unit lays the cells on a monospace grid and draws them
// with Direct2D + DirectWrite into a WIC bitmap, then encodes the bitmap as PNG.
//
// How it stays close to the terminal's own look:
//   - The cell is a WHOLE number of pixels wide and the font size is chosen so the font's advance is exactly that width
//     (ComputeScreenImageGrid), so columns never drift and horizontal box-drawing lines meet without a seam.
//   - Box-drawing and block characters (U+2500-U+259F, IsCellStretchGlyph - Claude Code's frames and rules are made of
//     them) are drawn stretched to fill their cell exactly, the way the terminal's built-in glyphs draw them, so vertical
//     lines join from row to row even when the cell is a pixel taller than the font's line.
//   - Every other glyph is drawn alone in its cell (one or two columns wide) with DirectWrite's system font fallback, so a
//     symbol the terminal font lacks still shows, and color emoji keep their colors.
//   - Grayscale antialiasing: a picture is viewed at any scale and recompressed by the messenger, where ClearType's color
//     fringes would only smear.
//
// Plain C++ + Win32/COM (no WinRT, no PCH): the Telegram service calls it on a worker thread of its own, and the m5
// harness renders and decodes real pictures with it (tests_messaging_card.cpp). It links d2d1, dwrite, windowscodecs and
// ole32 through its own #pragma comment lines.
//
// TALL pictures (MESSAGING.md section 21.39 - the card's "All Screen"): a sheet of any height (a whole conversation laid out
// on the grid, ConversationImage.h) is drawn with a FIXED cell into one or more PNG parts of a bounded height
// (RenderTallScreenSnapshotPngs). It goes through a strip of a few thousand pixels, so memory stays the same however tall the
// picture is, and each part is PNG-encoded strip by strip as it is drawn. Runs of plain ASCII in one style are drawn with one
// DirectWrite call each (every candidate font is monospace, so the run's glyphs land on the cells exactly): a conversation is
// hundreds of thousands of cells, and one call per cell would take seconds.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ScreenSnapshot.h"

namespace Agentmaster
{
    /// <summary>How big the picture of a screen may be. The defaults fit a phone and the messenger's photo limits.</summary>
    struct ScreenImageOptions
    {
        int maxWidthPx{ 2400 }; // the widest picture (a messenger shows a photo up to ~2560 px on its long side and scales a wider one down)
        int minCellWidthPx{ 8 }; // a narrower cell is unreadable even zoomed in: a very wide terminal makes a wider picture instead
        int maxCellWidthPx{ 16 }; // a wider cell only blows a small terminal's picture up (80 columns -> 1,312 px wide)
        int paddingPx{ 16 }; // the margin around the grid, in the screen's background color (the terminal's own padding)
    };

    /// <summary>The grid a screen is drawn on: the cell size, the font size that fits it, and the picture's size.</summary>
    struct ScreenImageGrid
    {
        int cellWidthPx{ 0 }; // whole pixels: every column starts on a pixel boundary (no drift, no seams between box lines)
        int cellHeightPx{ 0 }; // whole pixels: the font's line height rounded up
        float fontPx{ 0.0f }; // the em size that makes the font's advance exactly cellWidthPx
        float lineHeightPx{ 0.0f }; // the font's own line height at fontPx (ascent + descent + line gap): cellHeightPx minus under a pixel
        int widthPx{ 0 }; // the picture: cols x cellWidthPx plus the padding on both sides
        int heightPx{ 0 }; // the picture: rows x cellHeightPx plus the padding on both sides
    };

    /// <summary>The picture of a screen, or why there is none.</summary>
    struct ScreenImage
    {
        std::string png; // the PNG file's bytes; "" when there is no picture (error says why)
        ScreenImageGrid grid; // the grid it was drawn on (its widthPx / heightPx are the PNG's size)
        std::wstring fontFamily; // the family the text was drawn in: the snapshot's own font, or the first fallback the system has
        std::wstring error; // "" on success; else what failed, with its HRESULT ("CreateWicBitmapRenderTarget failed (0x88990012)")
    };

    /// <summary>
    /// Lays a screen of `cols` x `rows` cells on a grid: the widest whole-pixel cell that keeps the picture within
    /// options.maxWidthPx (clamped to [minCellWidthPx, maxCellWidthPx]), the font size whose advance is exactly that cell,
    /// and the cell height that holds the font's line. Pure (the font's proportions come in as em fractions), so the m5 suite
    /// checks the arithmetic without a font.
    /// </summary>
    /// <param name="cols">The screen's columns (at least 1).</param>
    /// <param name="rows">The screen's rows (at least 1).</param>
    /// <param name="advanceEm">The font's advance of a narrow glyph as a fraction of its em (Cascadia Mono: 1200 / 2048 = 0.586).</param>
    /// <param name="lineHeightEm">The font's line height (ascent + descent + line gap) as a fraction of its em (Cascadia Mono: 2620 / 2048 = 1.279).</param>
    /// <param name="options">The size limits.</param>
    /// <returns>The grid; every field 0 when an input is not usable (no columns, no rows, a font with no advance or no line).</returns>
    /// <remarks>
    /// The cell height is the line height rounded UP with a 1/100 pixel tolerance (a line height of 24.0000001 from float
    /// arithmetic stays 24, not 25). The picture can exceed maxWidthPx only when the minimum cell forces it (more than ~290
    /// columns at the defaults); the messenger then scales it down or the caller sends it as a file.
    /// </remarks>
    inline ScreenImageGrid ComputeScreenImageGrid(int cols, int rows, double advanceEm, double lineHeightEm, const ScreenImageOptions& options) noexcept
    {
        ScreenImageGrid g;
        if (cols <= 0 || rows <= 0 || !(advanceEm > 0.0) || !(lineHeightEm > 0.0) || !std::isfinite(advanceEm) || !std::isfinite(lineHeightEm))
        {
            return g;
        }
        const int minCell = options.minCellWidthPx < 1 ? 1 : options.minCellWidthPx;
        const int maxCell = options.maxCellWidthPx < minCell ? minCell : options.maxCellWidthPx;
        const int padding = options.paddingPx < 0 ? 0 : options.paddingPx;
        int cell = (options.maxWidthPx - 2 * padding) / cols;
        cell = cell < minCell ? minCell : (cell > maxCell ? maxCell : cell);
        g.cellWidthPx = cell;
        g.fontPx = static_cast<float>(static_cast<double>(cell) / advanceEm);
        g.lineHeightPx = static_cast<float>(static_cast<double>(g.fontPx) * lineHeightEm);
        g.cellHeightPx = static_cast<int>(std::ceil(static_cast<double>(g.lineHeightPx) - 0.01));
        if (g.cellHeightPx < 1)
        {
            g.cellHeightPx = 1;
        }
        g.widthPx = cols * g.cellWidthPx + 2 * padding;
        g.heightPx = rows * g.cellHeightPx + 2 * padding;
        return g;
    }

    /// <summary>
    /// Whether a cell's glyph is drawn STRETCHED to fill its cell: a box-drawing character (U+2500-U+257F) or a block element
    /// (U+2580-U+259F). Their strokes must touch the cell's edges for a frame to join from cell to cell and row to row; the
    /// terminal draws them itself for the same reason (its built-in glyphs). One code unit only: these are all in the BMP.
    /// </summary>
    /// <param name="text">The cell's text.</param>
    /// <returns>True for one of those characters.</returns>
    inline bool IsCellStretchGlyph(std::wstring_view text) noexcept
    {
        return text.size() == 1 && text[0] >= 0x2500 && text[0] <= 0x259F;
    }

    /// <summary>
    /// Draws a screen as a PNG (see the top comment for how it imitates the terminal). Blocks for the drawing - measured on the
    /// owner's PC (2026-10-18, optimized and Debug builds alike): 88-137 ms for a 200 x 60 screen (2232 x 1352 px, ~0.5 MB of
    /// PNG), 113-183 ms for 300 x 80, the first call of a process the slowest - so it is called on a worker thread, never on a
    /// UI thread.
    /// </summary>
    /// <param name="snapshot">The screen (ParseScreenSnapshot's output; 1..kScreenSnapshotMaxCols x 1..kScreenSnapshotMaxRows).</param>
    /// <param name="options">The size limits.</param>
    /// <returns>The picture with its grid and font, or an empty `png` and the reason in `error`. Never throws.</returns>
    /// <remarks>
    /// COM is initialized on the calling thread for the call's length (multithreaded; a thread that already chose the
    /// single-threaded model keeps it, which WIC and Direct2D accept) and uninitialized again only when this call initialized
    /// it. The font is the snapshot's own family when the system has it, else Cascadia Mono, Cascadia Code, Consolas, Lucida
    /// Console or Courier New, in that order. A failure is reported, never thrown (Rule #18: an exception is logged with its
    /// throw site and reported as "an exception while drawing the screen").
    /// </remarks>
    ScreenImage RenderScreenSnapshotPng(const ScreenSnapshot& snapshot, const ScreenImageOptions& options = {}) noexcept;

    // ===== TALL pictures (MESSAGING.md section 21.39): a sheet of any height, drawn as PNG parts of a bounded height =====

    /// <summary>
    /// How a TALL sheet is drawn: a FIXED cell (a tall sheet is laid out at a chosen width, so there is no width budget to fit)
    /// and pictures of a bounded height. The defaults suit a phone: 100 columns of 12 px make a 1,232 px wide picture, about a
    /// phone's own width, so it reads at 1:1 without zooming.
    /// </summary>
    /// <remarks>
    /// The part height is a compromise, not a hard limit of PNG. A taller picture is still a valid file, but viewers decode a
    /// picture whole (1,232 x 32,000 px is ~158 MB of pixels), and many of them scale a very tall one down until it is
    /// unreadable. 32,000 px is about 1,330 rows (Cascadia Mono's row is 24 px at a 12 px cell), roughly 25 phone screens.
    /// </remarks>
    struct TallScreenImageOptions
    {
        int cellWidthPx{ 12 }; // every cell this wide; the font is sized so its advance is exactly this (ComputeScreenImageGrid)
        int paddingPx{ 16 }; // the margin around each part, in the sheet's background
        int maxPartHeightPx{ 32000 }; // a part is at most this tall, its padding included; a taller sheet becomes several parts
        int stripHeightPx{ 2048 }; // the drawing's working height: memory is width x this x 4 bytes, however tall the part is
    };

    /// <summary>The tallest sheet RenderTallScreenSnapshotPngs draws (rows). A conversation's sheet is capped far below it; the bound only refuses garbage.</summary>
    inline constexpr int kTallSnapshotMaxRows = 200000;

    // ----- where a part may END: one value per sheet row, for the row a part would start with (PlanTallImageParts) -----

    /// <summary>A part starting at this row would cut a block in two: used only when nothing better is near.</summary>
    inline constexpr uint8_t kTallCutAny = 0;
    /// <summary>A block starts at this row: a part may start here cleanly.</summary>
    inline constexpr uint8_t kTallCutBlock = 1;
    /// <summary>A turn starts at this row: the best place for a part to start.</summary>
    inline constexpr uint8_t kTallCutTurn = 2;

    /// <summary>
    /// Splits a tall sheet into parts of at most `rowsPerPart` rows, each part starting where it reads best: at a TURN within
    /// the last 40% of the window a part may fill, else at a BLOCK within its last 15%, else exactly where the window ends.
    /// Pure, so the m5 suite checks it without drawing.
    /// </summary>
    /// <param name="totalRows">The sheet's rows (0 = no parts).</param>
    /// <param name="rowsPerPart">The most rows one part holds (a value below 1 counts as 1).</param>
    /// <param name="cuts">kTallCut* for each row: the row a part would START with (shorter than the sheet = kTallCutAny for the rest).</param>
    /// <param name="maxParts">The most parts wanted (0 = any number). When cutting at good places would need more, every part is filled instead.</param>
    /// <returns>(first row, row count) per part, in order; together they cover [0, totalRows) exactly once.</returns>
    /// <exception cref="std::bad_alloc">If the plan cannot be allocated.</exception>
    /// <remarks>
    /// The preference costs at most 40% of a part's room, so a plan can need more parts than a filled one. When `maxParts` is
    /// given and the preferring plan exceeds it, the parts are filled to rowsPerPart instead, which needs exactly
    /// ceil(totalRows / rowsPerPart) of them. Making totalRows fit maxParts x rowsPerPart is the caller's job.
    /// </remarks>
    inline std::vector<std::pair<int, int>> PlanTallImageParts(int totalRows, int rowsPerPart, const std::vector<uint8_t>& cuts, size_t maxParts = 0)
    {
        const int perPart = rowsPerPart < 1 ? 1 : rowsPerPart;
        const auto cutAt = [&cuts](int row) noexcept -> uint8_t {
            return row >= 0 && static_cast<size_t>(row) < cuts.size() ? cuts[static_cast<size_t>(row)] : kTallCutAny;
        };
        const auto plan = [&](bool prefer) {
            std::vector<std::pair<int, int>> parts;
            int start = 0;
            while (start < totalRows)
            {
                if (totalRows - start <= perPart)
                {
                    parts.emplace_back(start, totalRows - start);
                    break;
                }
                const int limit = start + perPart; // a part ending right before this row is exactly full
                int next = limit;
                if (prefer)
                {
                    // Search back from the full window: first for a turn in its last 40%, then for a block in its last 15%.
                    // `row > start` keeps every part at least one row long, so the loop always moves on.
                    const int turnFloor = start + (perPart * 6) / 10;
                    const int blockFloor = start + (perPart * 85) / 100;
                    int found = -1;
                    for (int row = limit; row > start && row >= turnFloor; --row)
                    {
                        if (cutAt(row) >= kTallCutTurn)
                        {
                            found = row;
                            break;
                        }
                    }
                    for (int row = limit; found < 0 && row > start && row >= blockFloor; --row)
                    {
                        if (cutAt(row) >= kTallCutBlock)
                        {
                            found = row;
                        }
                    }
                    if (found > start)
                    {
                        next = found;
                    }
                }
                parts.emplace_back(start, next - start);
                start = next;
            }
            return parts;
        };
        auto parts = plan(true);
        if (maxParts > 0 && parts.size() > maxParts)
        {
            parts = plan(false); // the good places cost too many parts: fill each one instead
        }
        return parts;
    }

    /// <summary>One PNG of a tall sheet: the sheet's rows [firstRow, firstRow + rowCount), with the padding around them.</summary>
    struct TallImagePart
    {
        std::string png; // the PNG file's bytes ("" once the part was handed to the caller's onPart)
        int firstRow{ 0 }; // the sheet row the part starts with
        int rowCount{ 0 }; // how many sheet rows it shows
        int widthPx{ 0 }; // the picture's width (every part of one sheet has the same)
        int heightPx{ 0 }; // the picture's height: rowCount x the cell height plus the padding on both sides
    };

    /// <summary>A tall sheet's pictures, or why there are none.</summary>
    struct TallScreenImage
    {
        std::vector<TallImagePart> parts; // in order; empty when `error` says why
        ScreenImageGrid grid; // the cell and the font size (widthPx = every part's width; heightPx = 0: each part has its own)
        std::wstring fontFamily; // the family the text was drawn in
        std::wstring error; // "" on success; else what failed. Parts drawn before a failure are still in `parts` (or were handed over).
    };

    /// <summary>
    /// The grid a tall sheet of `cols` columns would be drawn on: the font the system has (the face asked for, else the
    /// monospace fallbacks of RenderScreenSnapshotPng), the fixed cell of options.cellWidthPx, the font size whose advance is
    /// exactly that cell, and the cell height. Nothing is drawn. The caller needs the cell HEIGHT before it lays a sheet out:
    /// it decides how many rows fit into one part, so how much of a long conversation fits into the parts it may send.
    /// </summary>
    /// <param name="fontFace">The family to try first ("" = the fallbacks only).</param>
    /// <param name="cols">The sheet's columns (at least 1).</param>
    /// <param name="options">The cell and the padding.</param>
    /// <param name="fontFamily">The family that will be used.</param>
    /// <param name="error">Why there is no grid (the grid is then all 0).</param>
    /// <returns>The grid (its heightPx is one row's picture: the cell height plus the padding); every field 0 on failure. Never throws.</returns>
    ScreenImageGrid MeasureTallScreenImageGrid(const std::wstring& fontFace, int cols, const TallScreenImageOptions& options, std::wstring& fontFamily, std::wstring& error) noexcept;

    /// <summary>
    /// Draws a TALL sheet as PNG parts (PlanTallImageParts cuts it; see the top comment for how it is drawn). Blocks for the
    /// drawing and the encoding - seconds for a long conversation - so it is called on a worker thread, never on a UI thread.
    /// </summary>
    /// <param name="sheet">The sheet: 1..kScreenSnapshotMaxCols columns, 1..kTallSnapshotMaxRows rows (each row's cells left to right, trailing blanks may be absent).</param>
    /// <param name="cuts">kTallCut* per row (PlanTallImageParts); empty = cut anywhere.</param>
    /// <param name="maxParts">The most parts wanted (PlanTallImageParts; 0 = any number).</param>
    /// <param name="options">The cell, the padding and the part height.</param>
    /// <param name="onPart">
    /// Called with each part as soon as it is encoded (its PNG MOVED in), with the part's index and the plan's part count, so a
    /// caller can send part 1 while part 2 is drawn; `parts` then keeps the metadata with an empty `png`. Empty = keep every
    /// part's bytes in `parts`. It runs on the calling thread; an exception it throws ends the drawing (reported in `error`).
    /// </param>
    /// <returns>The parts, the grid and the font, or the reason in `error`. Never throws.</returns>
    /// <remarks>
    /// COM is initialized for the call's length as RenderScreenSnapshotPng does it. A row is drawn in every strip it touches,
    /// with a cell of margin for glyphs that reach past their row (an italic overhang, a taller fallback glyph); a strip starts
    /// on a whole pixel, so a row drawn in two strips gets the same pixels in both, and no seam shows where they meet.
    /// </remarks>
    TallScreenImage RenderTallScreenSnapshotPngs(const ScreenSnapshot& sheet,
                                                 const std::vector<uint8_t>& cuts,
                                                 size_t maxParts,
                                                 const TallScreenImageOptions& options,
                                                 const std::function<void(TallImagePart&& part, size_t index, size_t count)>& onPart = {}) noexcept;
}
