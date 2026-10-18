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

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

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
}
