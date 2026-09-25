// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster -- TaskbarBadge: the UNREAD-session count drawn on the app's taskbar button
// (doc/agentmaster/NOTIFICATIONS.md section 8).
//
// WHY AN OVERLAY ICON, NOT A BADGE NOTIFICATION. Windows has two ways to put a number on a taskbar button:
//   * Badge notifications (Windows.UI.Notifications.BadgeUpdateManager, `<badge value="3"/>`): the SHELL draws
//     the number. They need PACKAGE IDENTITY -- CreateBadgeUpdaterForApplication() throws 0x80070490 ("Element
//     not found") in an unpackaged process, i.e. every PORTABLE Agentmaster; they have open taskbar bugs for
//     XAML-hosted desktop windows (WindowsAppSDK #3928: the badge shows only once the window closes); and a
//     badge OUTLIVES the app on a pinned icon (a stale "3" on a closed app is a lie nobody is left to correct).
//   * Overlay icons (ITaskbarList3::SetOverlayIcon): the app hands the shell a small icon, which it paints over
//     the lower-right corner of the button. Works with or without identity, dies with the window, and is what
//     Discord / Slack / classic Teams do. Windows Terminal already holds an ITaskbarList3 per window for its
//     progress indicator (IslandWindow::_taskbar), so the overlay rides existing plumbing.
// Both honor the user's Settings > Personalization > Taskbar > "Show badges on taskbar apps" switch, and
// neither can draw while the taskbar uses small buttons.
//
// THE OVERLAY IS PER GROUP, NOT PER WINDOW. Every Agentmaster window shares one AUMID, so the taskbar combines
// them into one button, and SetOverlayIcon's contract is "the last overlay received is the overlay shown". A
// per-window count would therefore show whichever window spoke last. So the count is PROCESS-WIDE (Engine.h's
// unread census) and every window paints the SAME number on its own button: right when combined, merely
// repeated when the user un-combines taskbar buttons.
//
// This header is the RENDERING half: pure helpers (the label, the accessibility text, the icon size, the
// disc + digit composition) and the GDI functions that turn a count into an HICON. Header-only, pure Win32
// (the ProfileBootstrap.h / StartupTiming.h idiom), so the WindowsTerminal EXE -- which links TerminalApp.dll,
// not the engine lib -- and the standalone test harness (tests_taskbar.cpp) can both include it.
//
// Every std::min/std::max below is written `(std::min)(...)`: the harness includes <windows.h> WITHOUT
// NOMINMAX, and a bare call hits the min/max macros (the 7ac3d50e6 TabDragMath.h break).

#pragma once

// No WIN32_LEAN_AND_MEAN here (the StartupTiming.h rule): a define in a header would silently strip <windows.h>
// sub-headers from whichever TU includes this first. wingdi.h (the GDI half) is included either way.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// CreateCompatibleDC / CreateDIBSection / CreateFontW / TextOutW / CreateBitmap live in gdi32 and
// CreateIconIndirect in user32. The EXE links both already; the pragmas make the header self-sufficient for the
// test harness, whose cl command line names only the libraries the engine TUs need.
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace Agentmaster::TaskbarBadge
{
    /// <summary>
    /// The largest count drawn as digits; anything above renders "99+". The same cap the shell's own numeric badges
    /// use, and three glyphs is already the most a 16 px disc can hold legibly.
    /// </summary>
    inline constexpr uint32_t kMaxShownCount = 99;

    /// <summary>
    /// The disc fill as 0xRRGGBB: Fluent's "red" (#D13438). White bold digits on it clear 4.5:1 contrast, and a red
    /// disc reads as "unread" on both the light (#F3F3F3) and the dark (#202020) Windows 11 taskbar.
    /// </summary>
    /// <remarks>
    /// Deliberately NOT the tab flash ring's AppSettings::flashRingColor, although both mean "unread": that one is
    /// user-picked WITH an alpha channel, and a yellow or 30%-alpha pick would leave white digits unreadable.
    /// </remarks>
    inline constexpr uint32_t kDiscFillRgb = 0xD13438;

    /// <summary>
    /// Supersampling factor per axis for the disc edge and the digit mask. 4 gives 17 coverage levels (smooth enough
    /// at 16-32 px) and makes the digits independent of GDI's per-size anti-aliasing decisions.
    /// </summary>
    inline constexpr int kSuperSample = 4;

    /// <summary>
    /// The smallest overlay edge rendered, in physical pixels: the documented 16 px small-icon size at 96 DPI.
    /// </summary>
    inline constexpr int kMinIconSize = 16;

    /// <summary>
    /// The largest overlay edge rendered, in physical pixels (400% scaling). Bounds the transient GDI canvases, which
    /// grow with the square of this; the shell scales an icon for anything beyond.
    /// </summary>
    inline constexpr int kMaxIconSize = 64;

    /// <summary>
    /// The text drawn inside the disc: nothing for 0, the decimal digits up to <see cref="kMaxShownCount"/>, and
    /// "99+" above it.
    /// </summary>
    /// <param name="count">The unread-session count.</param>
    /// <returns>The label; an empty string means "no badge".</returns>
    /// <exception cref="std::bad_alloc">Only if the string allocation fails.</exception>
    inline std::wstring Label(uint32_t count)
    {
        if (count == 0)
        {
            return {};
        }
        if (count > kMaxShownCount)
        {
            return L"99+";
        }
        return std::to_wstring(count);
    }

    /// <summary>
    /// The overlay's accessibility text (SetOverlayIcon's pszDescription, which Narrator reads for the taskbar
    /// button). Carries the EXACT count, never the visual "99+" cap: a screen reader has no pixel budget to respect.
    /// </summary>
    /// <param name="count">The unread-session count.</param>
    /// <returns>E.g. "3 unread Agentmaster sessions"; empty for 0.</returns>
    /// <exception cref="std::bad_alloc">Only if the string allocation fails.</exception>
    inline std::wstring Description(uint32_t count)
    {
        if (count == 0)
        {
            return {};
        }
        return std::to_wstring(count) + (count == 1 ? L" unread Agentmaster session" : L" unread Agentmaster sessions");
    }

    /// <summary>
    /// The overlay edge in physical pixels for a DPI: the 16 px small icon the SetOverlayIcon docs ask for, scaled
    /// linearly the way GetSystemMetricsForDpi(SM_CXSMICON) scales it (20 at 120 DPI, 24 at 144, 32 at 192), clamped
    /// to [<see cref="kMinIconSize"/>, <see cref="kMaxIconSize"/>].
    /// </summary>
    /// <param name="dpi">The window's DPI (GetDpiForWindow). 0, an unknown DPI, is treated as 96.</param>
    /// <returns>The icon edge to render at.</returns>
    /// <remarks>
    /// The taskbar can sit on a monitor whose DPI differs from the window's; the shell rescales a mismatched icon, so
    /// the window's DPI is the best single guess (the caller re-renders on WM_DPICHANGED).
    /// </remarks>
    inline int IconSizeForDpi(uint32_t dpi) noexcept
    {
        // Clamp first so 16 * d can never overflow, then round to the nearest pixel.
        const uint32_t d = dpi == 0 ? 96u : (std::min)(dpi, 960u);
        const int size = static_cast<int>((16u * d + 48u) / 96u);
        return (std::max)(kMinIconSize, (std::min)(size, kMaxIconSize));
    }

    /// <summary>
    /// How much of pixel (x, y) the badge disc covers, by <see cref="kSuperSample"/> x <see cref="kSuperSample"/>
    /// point sampling. The disc is centered and full-bleed (diameter == size): the overlay slot IS the badge.
    /// </summary>
    /// <param name="size">The icon edge in pixels.</param>
    /// <param name="x">The pixel column, 0-based.</param>
    /// <param name="y">The pixel row, 0-based (top-down).</param>
    /// <returns>The covered fraction in [0, 1]; 0 for any out-of-range argument.</returns>
    inline double DiscCoverage(int size, int x, int y) noexcept
    {
        if (size <= 0 || x < 0 || y < 0 || x >= size || y >= size)
        {
            return 0.0;
        }
        const double center = size / 2.0;
        const double radiusSquared = center * center;
        int inside = 0;
        for (int sy = 0; sy < kSuperSample; ++sy)
        {
            for (int sx = 0; sx < kSuperSample; ++sx)
            {
                // Sample at each sub-cell's CENTER, so the pattern is symmetric about the disc center.
                const double px = x + (sx + 0.5) / kSuperSample - center;
                const double py = y + (sy + 0.5) / kSuperSample - center;
                if (px * px + py * py <= radiusSquared)
                {
                    ++inside;
                }
            }
        }
        return static_cast<double>(inside) / (kSuperSample * kSuperSample);
    }

    /// <summary>
    /// Composes the badge's pixels: the disc in <paramref name="fillRgb"/>, blended toward white wherever the text
    /// covers it. Pure, so the harness can pin exact values without GDI.
    /// </summary>
    /// <param name="size">The icon edge in pixels, 1..256.</param>
    /// <param name="textCoverage">size*size text coverages (0 = no ink, 255 = full ink), row-major top-down. Any other
    /// length, empty included, draws the disc alone.</param>
    /// <param name="fillRgb">The disc color as 0xRRGGBB.</param>
    /// <returns>size*size*4 bytes of top-down BGRA with STRAIGHT (non-premultiplied) alpha, the convention 32-bpp icon
    /// images use (the shell premultiplies when it draws); empty for an out-of-range size.</returns>
    /// <remarks>
    /// Text outside the disc is dropped with the disc's own alpha, so a too-wide label can never paint a white
    /// rectangle past the circle. A fully transparent pixel stays all-zero, which is also what the AND mask keys on.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the pixel buffer cannot be allocated.</exception>
    inline std::vector<uint8_t> ComposePixels(int size, const std::vector<uint8_t>& textCoverage, uint32_t fillRgb)
    {
        std::vector<uint8_t> out;
        if (size <= 0 || size > 256)
        {
            return out;
        }
        const size_t pixelCount = static_cast<size_t>(size) * static_cast<size_t>(size);
        out.assign(pixelCount * 4, 0);
        const double fillR = static_cast<double>((fillRgb >> 16) & 0xFFu);
        const double fillG = static_cast<double>((fillRgb >> 8) & 0xFFu);
        const double fillB = static_cast<double>(fillRgb & 0xFFu);
        const bool haveText = textCoverage.size() == pixelCount;
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                const double disc = DiscCoverage(size, x, y);
                if (disc <= 0.0)
                {
                    continue;
                }
                const size_t i = static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x);
                const double ink = haveText ? textCoverage[i] / 255.0 : 0.0;
                uint8_t* const px = out.data() + i * 4;
                px[0] = static_cast<uint8_t>(std::lround(fillB + (255.0 - fillB) * ink));
                px[1] = static_cast<uint8_t>(std::lround(fillG + (255.0 - fillG) * ink));
                px[2] = static_cast<uint8_t>(std::lround(fillR + (255.0 - fillR) * ink));
                px[3] = static_cast<uint8_t>(std::lround(255.0 * disc));
            }
        }
        return out;
    }

    namespace detail
    {
        /// <summary>
        /// The bounding box of the inked pixels of a mask, inclusive on both ends.
        /// </summary>
        struct InkBox
        {
            int x0{ 0 };
            int y0{ 0 };
            int x1{ -1 };
            int y1{ -1 };

            /// <summary>True when no pixel is inked (the default-constructed box).</summary>
            /// <returns>Whether the box is empty.</returns>
            bool Empty() const noexcept
            {
                return x1 < x0 || y1 < y0;
            }
        };

        /// <summary>
        /// Finds the ink bounding box of a row-major mask (any non-zero byte counts as ink).
        /// </summary>
        /// <param name="mask">width*height bytes.</param>
        /// <param name="width">The mask width.</param>
        /// <param name="height">The mask height.</param>
        /// <returns>The box; Empty() when nothing is inked or the arguments don't describe the buffer.</returns>
        inline InkBox FindInk(const std::vector<uint8_t>& mask, int width, int height) noexcept
        {
            if (width <= 0 || height <= 0 || mask.size() < static_cast<size_t>(width) * static_cast<size_t>(height))
            {
                return {};
            }
            InkBox box{ width, height, -1, -1 };
            for (int y = 0; y < height; ++y)
            {
                const uint8_t* const row = mask.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
                for (int x = 0; x < width; ++x)
                {
                    if (row[x] != 0)
                    {
                        box.x0 = (std::min)(box.x0, x);
                        box.y0 = (std::min)(box.y0, y);
                        box.x1 = (std::max)(box.x1, x);
                        box.y1 = (std::max)(box.y1, y);
                    }
                }
            }
            return box.x1 < 0 ? InkBox{} : box;
        }

        /// <summary>
        /// Draws <paramref name="label"/> in white on black, Segoe UI Bold at an em of <paramref name="emPx"/>, with
        /// the text cell's top-left at (<paramref name="originX"/>, <paramref name="originY"/>) on a canvas x canvas
        /// 32-bpp DIB, and returns which pixels were inked.
        /// </summary>
        /// <param name="label">The text to draw.</param>
        /// <param name="emPx">The font em height in pixels (a negative lfHeight).</param>
        /// <param name="canvas">The square canvas edge, 1..4096.</param>
        /// <param name="originX">The text cell's left edge on the canvas.</param>
        /// <param name="originY">The text cell's top edge on the canvas.</param>
        /// <returns>canvas*canvas bytes, row-major top-down, 1 = inked and 0 = not; EMPTY on any GDI failure.</returns>
        /// <remarks>
        /// Glyphs are drawn NONANTIALIASED (bilevel): ClearType would paint colored fringes, and GDI's grayscale
        /// anti-aliasing switches itself off at some sizes per the font's "gasp" table. RenderLabelCoverage's box
        /// filter turns the bilevel supersampled glyphs into smooth coverage instead, identically at every size.
        /// </remarks>
        /// <exception cref="std::bad_alloc">Only from the up-front mask allocation, i.e. before any GDI handle is
        /// created, so no handle can leak on a throw.</exception>
        inline std::vector<uint8_t> DrawLabelMask(const std::wstring& label, int emPx, int canvas, int originX, int originY)
        {
            if (label.empty() || emPx <= 0 || canvas <= 0 || canvas > 4096)
            {
                return {};
            }
            // Allocated FIRST: everything below is plain Win32 that cannot throw, so the DC / DIB / font handles
            // are always released on the paths that follow.
            std::vector<uint8_t> out(static_cast<size_t>(canvas) * static_cast<size_t>(canvas), 0);

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = canvas;
            bmi.bmiHeader.biHeight = -canvas; // negative == top-down, so byte row y IS pixel row y
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            const HDC dc = ::CreateCompatibleDC(nullptr);
            if (!dc)
            {
                return {};
            }
            void* bits = nullptr;
            const HBITMAP dib = ::CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!dib || !bits)
            {
                if (dib)
                {
                    ::DeleteObject(dib);
                }
                ::DeleteDC(dc);
                return {};
            }
            const HGDIOBJ oldBitmap = ::SelectObject(dc, dib);
            std::memset(bits, 0, static_cast<size_t>(canvas) * static_cast<size_t>(canvas) * 4); // black

            bool drawn = false;
            const HFONT font = ::CreateFontW(-emPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            if (font)
            {
                const HGDIOBJ oldFont = ::SelectObject(dc, font);
                ::SetBkMode(dc, TRANSPARENT);
                ::SetTextColor(dc, RGB(255, 255, 255));
                ::SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
                drawn = ::TextOutW(dc, originX, originY, label.c_str(), static_cast<int>(label.size())) != FALSE;
                // GDI batches drawing calls: the DIB's memory holds the text only after a flush.
                ::GdiFlush();
                ::SelectObject(dc, oldFont);
                ::DeleteObject(font);
            }
            if (drawn)
            {
                // White text on black: any channel is the ink. Green is read (BGRX byte 1); the threshold only
                // matters if a future quality flag ever reintroduced intermediate levels.
                const auto* const px = static_cast<const uint8_t*>(bits);
                for (size_t i = 0; i < out.size(); ++i)
                {
                    out[i] = px[i * 4 + 1] >= 128 ? 1 : 0;
                }
            }
            ::SelectObject(dc, oldBitmap);
            ::DeleteObject(dib);
            ::DeleteDC(dc);
            if (!drawn)
            {
                out.clear(); // clear() never throws; an empty mask is the documented failure value
            }
            return out;
        }
    }

    /// <summary>
    /// Renders <paramref name="label"/> as a size*size text-coverage mask, sized to fit the disc and optically
    /// centered on it: the input <see cref="ComposePixels"/> expects.
    /// </summary>
    /// <param name="label">The text (normally <see cref="Label"/>(count)).</param>
    /// <param name="size">The icon edge, <see cref="kMinIconSize"/>..<see cref="kMaxIconSize"/>.</param>
    /// <returns>size*size coverages 0..255, row-major top-down; EMPTY when the label is empty, the size out of range,
    /// or GDI failed (the caller then draws the disc without digits).</returns>
    /// <remarks>
    /// Up to four GDI passes: a trial at an em equal to the disc diameter measures the font's real ink extents, then
    /// fitted ems are drawn and re-measured until the ink fits its target box, because hinting makes glyph size
    /// non-linear in em (each correction rounds down, so the fit converges from the too-big side and the label is not
    /// clipped by the disc). The target box shrinks with the label length (one digit big, "99+" smallest), and every
    /// box's corners sit inside the disc. The INK box's center (not the font cell's) is then moved onto the disc
    /// center: digits have no descenders, so a cell-centered "3" sits visibly low.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If a mask buffer cannot be allocated.</exception>
    inline std::vector<uint8_t> RenderLabelCoverage(const std::wstring& label, int size)
    {
        if (label.empty() || size < kMinIconSize || size > kMaxIconSize)
        {
            return {};
        }
        const int hi = size * kSuperSample; // the supersampled badge square; the disc's diameter
        const int canvas = hi * 3; // room for the trial run of "99+" at em == hi, side bearings included
        const int origin = hi / 2; // off the edges, so a negative left bearing stays on the canvas

        double targetHeight = 0.58;
        double targetWidth = 0.62;
        if (label.size() == 2)
        {
            targetHeight = 0.50;
            targetWidth = 0.76;
        }
        else if (label.size() >= 3)
        {
            targetHeight = 0.40;
            targetWidth = 0.84;
        }

        // Fit the em until the ink lands inside the target box. Pass 0 measures at em == hi; each later pass corrects by
        // the measured overshoot, rounding DOWN so the correction converges from the too-big side (hinting makes ink
        // size jumpy in em, so one proportional step can still overflow by a pixel). It stops once the ink fits without
        // being more than 15% under the target, when the em stops moving, or after the fourth draw at the latest; the
        // mask + ink left in the variables always belong to the em that was drawn last.
        std::vector<uint8_t> mask;
        detail::InkBox ink;
        int em = hi;
        for (int pass = 0;; ++pass)
        {
            mask = detail::DrawLabelMask(label, em, canvas, origin, origin);
            ink = detail::FindInk(mask, canvas, canvas);
            if (ink.Empty())
            {
                return {};
            }
            const double inkWidth = ink.x1 - ink.x0 + 1.0;
            const double inkHeight = ink.y1 - ink.y0 + 1.0;
            const double fit = (std::min)(targetHeight * hi / inkHeight, targetWidth * hi / inkWidth);
            if (pass >= 3 || (pass > 0 && fit >= 1.0 && fit <= 1.15))
            {
                break;
            }
            const int next = (std::max)(4, static_cast<int>(std::floor(em * fit)));
            if (next == em)
            {
                break;
            }
            em = next;
        }

        const long dx = std::lround(hi / 2.0 - (ink.x0 + ink.x1 + 1) / 2.0);
        const long dy = std::lround(hi / 2.0 - (ink.y0 + ink.y1 + 1) / 2.0);
        std::vector<uint8_t> hiMask(static_cast<size_t>(hi) * static_cast<size_t>(hi), 0);
        for (int y = ink.y0; y <= ink.y1; ++y)
        {
            for (int x = ink.x0; x <= ink.x1; ++x)
            {
                if (mask[static_cast<size_t>(y) * static_cast<size_t>(canvas) + static_cast<size_t>(x)] == 0)
                {
                    continue;
                }
                const long tx = x + dx;
                const long ty = y + dy;
                if (tx < 0 || ty < 0 || tx >= hi || ty >= hi)
                {
                    continue; // ink pushed past the badge square: ComposePixels would clip it to the disc anyway
                }
                hiMask[static_cast<size_t>(ty) * static_cast<size_t>(hi) + static_cast<size_t>(tx)] = 1;
            }
        }

        // Box-filter down to the icon grid: each pixel's coverage is the share of its kSuperSample^2 sub-pixels
        // that are inked, rounded to the nearest 0..255 step.
        constexpr int samples = kSuperSample * kSuperSample;
        std::vector<uint8_t> coverage(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                int sum = 0;
                for (int sy = 0; sy < kSuperSample; ++sy)
                {
                    const size_t row = static_cast<size_t>(y * kSuperSample + sy) * static_cast<size_t>(hi);
                    for (int sx = 0; sx < kSuperSample; ++sx)
                    {
                        sum += hiMask[row + static_cast<size_t>(x * kSuperSample + sx)];
                    }
                }
                coverage[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] =
                    static_cast<uint8_t>((sum * 255 + samples / 2) / samples);
            }
        }
        return coverage;
    }

    /// <summary>
    /// Builds the overlay icon for <paramref name="count"/>: a red disc with the white count, or "99+".
    /// </summary>
    /// <param name="count">The unread-session count; 0 means "no badge" and returns nullptr.</param>
    /// <param name="size">The icon edge (normally <see cref="IconSizeForDpi"/>),
    /// <see cref="kMinIconSize"/>..<see cref="kMaxIconSize"/>.</param>
    /// <returns>A new HICON the CALLER must DestroyIcon (SetOverlayIcon copies it, so the caller may destroy it right
    /// after the call); nullptr for count 0, an out-of-range size, or a GDI failure.</returns>
    /// <remarks>
    /// A failed digit render still yields the plain red disc: "something is unread" beats no badge, and the
    /// accessibility description still carries the number. The color bitmap is a 32-bpp DIB with an explicit alpha
    /// mask (BITMAPV5HEADER + BI_BITFIELDS, the documented recipe for an alpha icon via CreateIconIndirect); the
    /// monochrome AND mask marks the fully transparent pixels for renderers that ignore alpha.
    /// </remarks>
    /// <exception cref="std::bad_alloc">From the pixel / mask buffers, all allocated before any GDI handle exists.</exception>
    inline HICON CreateBadgeIcon(uint32_t count, int size)
    {
        if (count == 0 || size < kMinIconSize || size > kMaxIconSize)
        {
            return nullptr;
        }
        const std::vector<uint8_t> text = RenderLabelCoverage(Label(count), size);
        const std::vector<uint8_t> pixels = ComposePixels(size, text, kDiscFillRgb);
        if (pixels.empty())
        {
            return nullptr;
        }
        // CreateBitmap's monochrome contract: 1 bit per pixel, most significant bit first, every row padded to a
        // 16-bit boundary. A 1 bit is transparent.
        const int maskStride = ((size + 15) / 16) * 2;
        std::vector<uint8_t> maskBits(static_cast<size_t>(maskStride) * static_cast<size_t>(size), 0);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                if (pixels[(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4 + 3] == 0)
                {
                    maskBits[static_cast<size_t>(y) * static_cast<size_t>(maskStride) + static_cast<size_t>(x / 8)] |= static_cast<uint8_t>(0x80u >> (x % 8));
                }
            }
        }

        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(BITMAPV5HEADER);
        bi.bV5Width = size;
        bi.bV5Height = -size; // top-down, matching ComposePixels' row order
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;

        void* bits = nullptr;
        // DIB_RGB_COLORS needs no device palette, so no DC is required.
        const HBITMAP color = ::CreateDIBSection(nullptr, reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!color || !bits)
        {
            if (color)
            {
                ::DeleteObject(color);
            }
            return nullptr;
        }
        std::memcpy(bits, pixels.data(), pixels.size());

        HICON icon = nullptr;
        const HBITMAP mask = ::CreateBitmap(size, size, 1, 1, maskBits.data());
        if (mask)
        {
            ICONINFO info{};
            info.fIcon = TRUE;
            info.hbmMask = mask;
            info.hbmColor = color;
            icon = ::CreateIconIndirect(&info); // copies both bitmaps; ours are released below either way
            ::DeleteObject(mask);
        }
        ::DeleteObject(color);
        return icon;
    }
}
