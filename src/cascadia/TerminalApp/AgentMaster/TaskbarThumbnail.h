// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster -- TaskbarThumbnail: the RED FRAME on the taskbar / Alt+Tab preview of a window that holds unread tabs
// (doc/agentmaster/NOTIFICATIONS.md section 9).
//
// WHY AN "ICONIC" THUMBNAIL. Windows draws a window's taskbar preview itself, as a live copy of the window, and it
// has NO API to tint or outline one window's preview. The documented escape hatch is the iconic representation
// (DWMWA_FORCE_ICONIC_REPRESENTATION + DWMWA_HAS_ICONIC_BITMAP): the app hands DWM its OWN bitmap with
// DwmSetIconicThumbnail, and DWM shows that bitmap instead of the live preview -- in the taskbar hover flyout and in
// Alt+Tab (both verified). So a framed window gives DWM a capture of itself with a frame painted on it.
// Measured on a disposable Windows 11 25H2 (26200) desktop, 2026-10-06 (the probe is tools/taskbar-thumbnail-probe):
//   * Flashing (FlashWindowEx), a red progress state and an overlay icon change only the COMBINED taskbar button;
//     no per-window preview marks the window. The iconic bitmap is the only per-window frame there is.
//   * PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) captures a XAML-Islands window with its swap-chain terminal
//     text (Windows Terminal: 1113x627 in 19-49 ms under WARP); without PW_RENDERFULLCONTENT the capture is black.
//     It works from a thread-pool thread of the window's own process too.
//   * DWM REJECTS a bitmap larger than its maximum preview size with E_INVALIDARG -- and it tells the app that size
//     only in WM_DWMSENDICONICTHUMBNAIL (a 704x381 window asked for at most 200x108 at 100% scaling: width 200,
//     height from the window's aspect). Hence the LEARNED box, and an ESTIMATE kept under it until then.
//   * Alt+Tab shows the window it pre-selects BLACK when DWM holds no stored bitmap at the moment Alt+Tab opens, and
//     never asks again for that window during that Alt+Tab. So the stored bitmap must never be left empty: every
//     refresh PUSHES an unsolicited bitmap right after DwmInvalidateIconicBitmaps (an invalidate-and-wait design
//     showed the pre-selected window black every time).
//   * DwmInvalidateIconicBitmaps is still needed: without it DWM keeps the FIRST peek (live-preview) bitmap forever.
//   * DWM asks for bitmaps only while something displays the preview, so a framed window costs nothing at rest
//     beyond its own refreshes.
//
// This header is the PURE half (sizes, the frame, the box-filter downscale, the blank check, the job coalescer) plus
// a small Win32 half (the capture, the DIB). Header-only and plain Win32 -- the TaskbarBadge.h idiom -- so the
// WindowsTerminal EXE (IslandWindow, which owns the DWM calls) and the standalone harness (tests_taskbar.cpp) can
// both include it. Every std::min/std::max is written `(std::min)(...)` for the harness's <windows.h> without
// NOMINMAX.

#pragma once

// No WIN32_LEAN_AND_MEAN here (the StartupTiming.h rule): a define in a header would strip <windows.h> sub-headers
// from whichever TU includes this first. wingdi.h (the capture half) is included either way.
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// CreateCompatibleDC / CreateDIBSection / DrawIconEx / PrintWindow: gdi32 + user32. The EXE links both already; the
// pragmas keep the header self-sufficient for the test harness.
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace Agentmaster::TaskbarThumbnail
{
    /// <summary>
    /// A width and height in physical pixels. A non-positive dimension means "no size" (unknown, or nothing to draw).
    /// </summary>
    struct PixelSize
    {
        int width{ 0 };
        int height{ 0 };

        /// <summary>True when either dimension is zero or negative: callers treat it as "unknown / nothing".</summary>
        /// <returns>Whether the size is empty.</returns>
        constexpr bool Empty() const noexcept
        {
            return width <= 0 || height <= 0;
        }

        /// <summary>Exact equality of both dimensions.</summary>
        /// <param name="other">The size to compare with.</param>
        /// <returns>True when width and height are both equal.</returns>
        constexpr bool operator==(const PixelSize& other) const noexcept
        {
            return width == other.width && height == other.height;
        }
    };

    /// <summary>
    /// The preview box at 96 DPI that the ESTIMATE starts from, before DWM has asked once. 200 is the width Windows 11
    /// 25H2 asked for at 100% scaling; 120 is a height cap for tall windows that was NOT measured -- an estimate DWM
    /// finds too large only costs one E_INVALIDARG, after which the window waits for DWM to ask (and learns the box).
    /// </summary>
    inline constexpr int kEstimateBoxWidth96 = 200;

    /// <summary>The height half of <see cref="kEstimateBoxWidth96"/>'s box; unmeasured, see there.</summary>
    inline constexpr int kEstimateBoxHeight96 = 120;

    /// <summary>
    /// The share of the scaled estimate box actually used, in percent. The estimate is a guess: keeping it a few
    /// percent under the expected maximum makes a DWM rounding difference land inside the box instead of earning an
    /// E_INVALIDARG. A preview 4% smaller than the maximum is invisible to the user.
    /// </summary>
    inline constexpr int kEstimatePercent = 96;

    /// <summary>
    /// The largest edge of the remembered capture (the minimized-window snapshot). Bounds its memory at about 1.6 MB
    /// per framed window while staying larger than any preview DWM asked for in testing (200x108 at 100%).
    /// </summary>
    inline constexpr int kCacheMaxEdge = 640;

    /// <summary>
    /// How old the remembered capture may be and still answer a WM_DWMSENDICONICTHUMBNAIL without a new capture, in
    /// milliseconds. A refresh that invalidates while a preview is on screen is followed by DWM asking at once; this
    /// lets that request reuse the capture the refresh just made instead of capturing the window twice.
    /// </summary>
    inline constexpr uint64_t kReuseCaptureMs = 1500;

    /// <summary>
    /// The interval of the periodic refresh while a window is framed, in milliseconds. The preview is about 200 px
    /// wide, so terminal text in it is unreadable and a 20-second-old picture is as good as a live one; the cost is
    /// one capture per framed window per interval, on a thread-pool thread.
    /// </summary>
    inline constexpr uint32_t kRefreshPeriodMs = 20000;

    /// <summary>
    /// The debounce after a resize, restore or DPI change before the next refresh, in milliseconds: a drag-resize
    /// raises WM_SIZE dozens of times a second, and one capture after it settles is enough.
    /// </summary>
    inline constexpr uint32_t kSettleDelayMs = 400;

    /// <summary>
    /// The fill of the PLACEHOLDER card, as 0xRRGGBB: shown when there is nothing to capture (the window was minimized
    /// before it was ever captured) or the capture came back black. Windows 11's dark window background, so the card
    /// reads as "a window" with the frame and the app icon on it rather than as a broken preview.
    /// </summary>
    inline constexpr uint32_t kCardFillRgb = 0x202020;

    /// <summary>
    /// The lowest frame opacity used, as an alpha byte. The frame takes AppSettings::flashRingColor, whose alpha is
    /// user-picked; a near-transparent pick must not make the frame vanish while the feature still costs captures
    /// (the Settings switch is the way to turn it off).
    /// </summary>
    inline constexpr uint8_t kMinFrameAlpha = 0x80;

    /// <summary>
    /// Scales <paramref name="source"/> down (or up) to fit inside <paramref name="box"/> with its aspect ratio kept,
    /// rounding DOWN so the result never exceeds the box in either dimension -- the property DWM enforces with
    /// E_INVALIDARG. Each dimension is at least 1.
    /// </summary>
    /// <param name="source">The size of the image to fit (the window's client area).</param>
    /// <param name="box">The largest allowed size.</param>
    /// <returns>The fitted size; empty when either argument is empty.</returns>
    /// <remarks>
    /// Integer math in 64 bits, so no window size can overflow. Matches what DWM itself computed in the probe: a
    /// 704x381 source in a 200x108 box gives 199x108, and 1113x627 gives 191x108.
    /// </remarks>
    inline PixelSize FitInside(PixelSize source, PixelSize box) noexcept
    {
        if (source.Empty() || box.Empty())
        {
            return {};
        }
        const int64_t sw = source.width;
        const int64_t sh = source.height;
        const int64_t bw = box.width;
        const int64_t bh = box.height;
        PixelSize out;
        // Compare the aspect ratios without division: sw/sh >= bw/bh  <=>  sw*bh >= sh*bw. The wider side binds.
        if (sw * bh >= sh * bw)
        {
            out.width = box.width;
            out.height = static_cast<int>((std::max)(int64_t{ 1 }, (sh * bw) / sw));
        }
        else
        {
            out.height = box.height;
            out.width = static_cast<int>((std::max)(int64_t{ 1 }, (sw * bh) / sh));
        }
        return out;
    }

    /// <summary>
    /// The preview box to push into before DWM has told the window its real one: the measured 96-DPI box scaled to
    /// <paramref name="dpi"/> and kept <see cref="kEstimatePercent"/> percent of it.
    /// </summary>
    /// <param name="dpi">The window's DPI (GetDpiForWindow). 0, an unknown DPI, is treated as 96; anything above 960
    /// (1000% scaling) is clamped there.</param>
    /// <returns>The estimated box; never empty.</returns>
    /// <remarks>
    /// The estimate exists so a window framed BEFORE anything displayed its preview still has a stored bitmap the
    /// first time Alt+Tab opens (see the header comment). It may be wrong both ways: too large costs one rejected push,
    /// too small leaves the preview slightly small until DWM asks once.
    /// </remarks>
    inline PixelSize EstimatedBox(uint32_t dpi) noexcept
    {
        const int64_t d = dpi == 0 ? 96 : (std::min)(static_cast<int64_t>(dpi), int64_t{ 960 });
        const auto scale = [d](int base) {
            return static_cast<int>((std::max)(int64_t{ 1 }, (base * d * kEstimatePercent) / (96 * 100)));
        };
        return PixelSize{ scale(kEstimateBoxWidth96), scale(kEstimateBoxHeight96) };
    }

    /// <summary>
    /// How thick the frame is on a preview of this size: about 1/24 of the shorter side (4 px on the 200x108 preview
    /// at 100% scaling, 6 px at 150%), at least 3 px so it reads as a frame, at most a quarter of the shorter side so a
    /// tiny preview is never all frame.
    /// </summary>
    /// <param name="thumb">The preview bitmap size.</param>
    /// <returns>The frame thickness in pixels; 0 for an empty size.</returns>
    inline int FrameThickness(PixelSize thumb) noexcept
    {
        if (thumb.Empty())
        {
            return 0;
        }
        const int shorter = (std::min)(thumb.width, thumb.height);
        const int quarter = (std::max)(1, shorter / 4);
        return (std::min)((std::max)(3, shorter / 24), quarter);
    }

    /// <summary>
    /// The frame color as 0xAARRGGBB: the given color with its alpha raised to at least <see cref="kMinFrameAlpha"/>.
    /// </summary>
    /// <param name="argb">The user's color (AppSettings::flashRingColor, parsed), 0xAARRGGBB.</param>
    /// <returns>The color the frame is blended with.</returns>
    inline uint32_t FrameColor(uint32_t argb) noexcept
    {
        const uint32_t alpha = (argb >> 24) & 0xFFu;
        return alpha >= kMinFrameAlpha ? argb : ((argb & 0x00FFFFFFu) | (static_cast<uint32_t>(kMinFrameAlpha) << 24));
    }

    /// <summary>
    /// Sets the alpha byte of every pixel to 255. GDI (PrintWindow, StretchBlt, DrawIconEx into a DIB) leaves alpha at
    /// 0, and DWM treats a 0-alpha pixel of an iconic bitmap as transparent -- the preview would show nothing.
    /// </summary>
    /// <param name="bgra">Top-down BGRA pixels; its size is the pixel count times 4 (a trailing partial pixel is
    /// ignored).</param>
    inline void ForceOpaque(std::vector<uint8_t>& bgra) noexcept
    {
        for (size_t i = 3; i < bgra.size(); i += 4)
        {
            bgra[i] = 0xFF;
        }
    }

    /// <summary>
    /// Shrinks (or grows) a top-down BGRA image with a box filter: each destination pixel is the rounded average of
    /// the source pixels its footprint covers. The output is fully opaque.
    /// </summary>
    /// <param name="src">The source pixels, srcSize.width * srcSize.height * 4 bytes.</param>
    /// <param name="srcSize">The source size.</param>
    /// <param name="dstSize">The wanted size.</param>
    /// <returns>dstSize.width * dstSize.height * 4 bytes; EMPTY when a size is empty or the buffer is too short for
    /// srcSize.</returns>
    /// <remarks>
    /// Footprints are whole source pixels: when shrinking they tile the source exactly, so every source pixel is read
    /// once (a 4K window is about 8 M pixels, a few milliseconds); when growing (a window smaller than the preview)
    /// each footprint is a single pixel, i.e. nearest-neighbour. Runs on the capture worker, never the UI thread.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the output buffer cannot be allocated.</exception>
    inline std::vector<uint8_t> DownscaleBgra(const std::vector<uint8_t>& src, PixelSize srcSize, PixelSize dstSize)
    {
        if (srcSize.Empty() || dstSize.Empty())
        {
            return {};
        }
        const int64_t sw = srcSize.width;
        const int64_t sh = srcSize.height;
        const int64_t dw = dstSize.width;
        const int64_t dh = dstSize.height;
        if (src.size() < static_cast<size_t>(sw * sh * 4))
        {
            return {};
        }
        std::vector<uint8_t> out(static_cast<size_t>(dw * dh * 4));
        for (int64_t dy = 0; dy < dh; ++dy)
        {
            const int64_t y0 = (dy * sh) / dh;
            const int64_t y1 = (std::max)(y0 + 1, ((dy + 1) * sh) / dh);
            for (int64_t dx = 0; dx < dw; ++dx)
            {
                const int64_t x0 = (dx * sw) / dw;
                const int64_t x1 = (std::max)(x0 + 1, ((dx + 1) * sw) / dw);
                uint64_t b = 0;
                uint64_t g = 0;
                uint64_t r = 0;
                for (int64_t y = y0; y < y1; ++y)
                {
                    const uint8_t* px = src.data() + static_cast<size_t>((y * sw + x0) * 4);
                    for (int64_t x = x0; x < x1; ++x, px += 4)
                    {
                        b += px[0];
                        g += px[1];
                        r += px[2];
                    }
                }
                const uint64_t n = static_cast<uint64_t>((x1 - x0) * (y1 - y0));
                uint8_t* const o = out.data() + static_cast<size_t>((dy * dw + dx) * 4);
                o[0] = static_cast<uint8_t>((b + n / 2) / n);
                o[1] = static_cast<uint8_t>((g + n / 2) / n);
                o[2] = static_cast<uint8_t>((r + n / 2) / n);
                o[3] = 0xFF;
            }
        }
        return out;
    }

    /// <summary>
    /// Paints the frame: a band <paramref name="thickness"/> pixels wide along all four edges, the color blended over
    /// the image with the color's own alpha. The pixels inside the band are untouched; every band pixel ends opaque.
    /// </summary>
    /// <param name="bgra">Top-down BGRA pixels of <paramref name="size"/>, modified in place.</param>
    /// <param name="size">The image size.</param>
    /// <param name="thickness">The band width (normally <see cref="FrameThickness"/>); clamped to half the shorter
    /// side, so the band can at most cover the whole image.</param>
    /// <param name="argb">The color as 0xAARRGGBB (normally <see cref="FrameColor"/>).</param>
    /// <remarks>A buffer shorter than the size says, an empty size or a non-positive thickness leaves it untouched.</remarks>
    inline void DrawFrame(std::vector<uint8_t>& bgra, PixelSize size, int thickness, uint32_t argb) noexcept
    {
        if (size.Empty() || thickness <= 0 ||
            bgra.size() < static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4)
        {
            return;
        }
        const int t = (std::min)(thickness, ((std::min)(size.width, size.height) + 1) / 2);
        const uint32_t a = (argb >> 24) & 0xFFu;
        const uint32_t cr = (argb >> 16) & 0xFFu;
        const uint32_t cg = (argb >> 8) & 0xFFu;
        const uint32_t cb = argb & 0xFFu;
        for (int y = 0; y < size.height; ++y)
        {
            const bool rowInBand = y < t || y >= size.height - t;
            for (int x = 0; x < size.width; ++x)
            {
                if (!rowInBand && x >= t && x < size.width - t)
                {
                    x = size.width - t - 1; // skip the untouched interior of this row in one step
                    continue;
                }
                uint8_t* const px = bgra.data() + (static_cast<size_t>(y) * static_cast<size_t>(size.width) + static_cast<size_t>(x)) * 4;
                // Straight-alpha "color over pixel", rounded: out = (src*(255-a) + c*a) / 255.
                px[0] = static_cast<uint8_t>((px[0] * (255u - a) + cb * a + 127u) / 255u);
                px[1] = static_cast<uint8_t>((px[1] * (255u - a) + cg * a + 127u) / 255u);
                px[2] = static_cast<uint8_t>((px[2] * (255u - a) + cr * a + 127u) / 255u);
                px[3] = 0xFF;
            }
        }
    }

    /// <summary>
    /// True when a capture came back without content: every pixel is black (each channel at most 2). That is what
    /// PrintWindow returns when the window's content was not rendered for it -- the case the capture must not show.
    /// </summary>
    /// <param name="bgra">Top-down BGRA pixels.</param>
    /// <param name="size">The image size.</param>
    /// <returns>True for an all-black image, an empty size, or a buffer too short for the size.</returns>
    /// <remarks>
    /// Scans until the first non-black pixel, which a real capture yields within its first rows (the tab strip), so a
    /// good capture costs almost nothing; a blank one is scanned whole (a few milliseconds for 4K, on the worker). A
    /// terminal's own background is #0C0C0C or lighter, so a dark theme never reads as blank.
    /// </remarks>
    inline bool LooksBlank(const std::vector<uint8_t>& bgra, PixelSize size) noexcept
    {
        if (size.Empty())
        {
            return true;
        }
        const size_t bytes = static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4;
        if (bgra.size() < bytes)
        {
            return true;
        }
        for (size_t i = 0; i < bytes; i += 4)
        {
            if (bgra[i] > 2 || bgra[i + 1] > 2 || bgra[i + 2] > 2)
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>
    /// A plain opaque card of <paramref name="size"/> filled with <paramref name="rgb"/>: the base of the placeholder
    /// preview (the frame and the app icon go on top).
    /// </summary>
    /// <param name="size">The card size.</param>
    /// <param name="rgb">The fill as 0xRRGGBB (normally <see cref="kCardFillRgb"/>).</param>
    /// <returns>size.width * size.height * 4 bytes of top-down BGRA; empty for an empty size.</returns>
    /// <exception cref="std::bad_alloc">If the buffer cannot be allocated.</exception>
    inline std::vector<uint8_t> SolidCard(PixelSize size, uint32_t rgb)
    {
        if (size.Empty())
        {
            return {};
        }
        std::vector<uint8_t> out(static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4);
        for (size_t i = 0; i < out.size(); i += 4)
        {
            out[i] = static_cast<uint8_t>(rgb & 0xFFu);
            out[i + 1] = static_cast<uint8_t>((rgb >> 8) & 0xFFu);
            out[i + 2] = static_cast<uint8_t>((rgb >> 16) & 0xFFu);
            out[i + 3] = 0xFF;
        }
        return out;
    }

    /// <summary>
    /// What a capture worker has been asked to do, merged across requests. Combined flags are done in one pass with
    /// one capture.
    /// </summary>
    struct WorkOrder
    {
        /// <summary>A refresh: capture, then invalidate DWM's stored bitmaps and store a new preview at once.</summary>
        bool push{ false };
        /// <summary>DWM asked for a preview (WM_DWMSENDICONICTHUMBNAIL); answer at <see cref="answerBox"/>.</summary>
        bool answer{ false };
        /// <summary>The largest preview DWM accepts, from the newest request; meaningful only with <see cref="answer"/>.</summary>
        PixelSize answerBox;
        /// <summary>DWM asked for the full-size peek bitmap (WM_DWMSENDICONICLIVEPREVIEWBITMAP).</summary>
        bool peek{ false };

        /// <summary>True when any work is pending.</summary>
        /// <returns>Whether the order holds work.</returns>
        constexpr bool Any() const noexcept
        {
            return push || answer || peek;
        }
    };

    /// <summary>
    /// The per-window job book that keeps AT MOST ONE capture worker in flight and folds every request that arrives
    /// meanwhile into its next pass. A drag-resize or a burst of DWM requests therefore costs one capture per pass,
    /// never a queue of stale captures. Pure and NOT thread-safe by itself: the owner calls it under its own lock.
    /// </summary>
    /// <remarks>
    /// Protocol: the requester calls <see cref="RequestPush"/>, <see cref="RequestAnswer"/> or
    /// <see cref="RequestPeek"/>; when it returns true the requester must start a worker (and call
    /// <see cref="Abandon"/> if it cannot). The worker loops on <see cref="Take"/> until it returns an empty
    /// order, which also marks the book idle -- so a request racing the worker's last Take either lands before it
    /// (and is taken) or after it (and starts a new worker). <see cref="Clear"/> drops pending work when the frame
    /// turns off; a running worker then finds nothing and stops.
    /// </remarks>
    class JobBook
    {
    public:
        /// <summary>Adds a refresh (capture + invalidate + push).</summary>
        /// <returns>True when no worker runs and the caller must start one.</returns>
        bool RequestPush() noexcept
        {
            _pending.push = true;
            return _Claim();
        }

        /// <summary>Adds an answer to DWM at <paramref name="box"/>; a newer request's box replaces an older one's.</summary>
        /// <param name="box">The largest preview DWM accepts (from the request's lParam).</param>
        /// <returns>True when no worker runs and the caller must start one.</returns>
        bool RequestAnswer(PixelSize box) noexcept
        {
            _pending.answer = true;
            _pending.answerBox = box;
            return _Claim();
        }

        /// <summary>Adds a peek (full-size live-preview) answer.</summary>
        /// <returns>True when no worker runs and the caller must start one.</returns>
        bool RequestPeek() noexcept
        {
            _pending.peek = true;
            return _Claim();
        }

        /// <summary>
        /// The worker's next pass: everything pending, cleared from the book. An EMPTY order means "stop", and the
        /// book is marked idle in the same call, so the next request starts a new worker.
        /// </summary>
        /// <returns>The work to do now; empty when there is none.</returns>
        WorkOrder Take() noexcept
        {
            if (!_pending.Any())
            {
                _running = false;
                return {};
            }
            const WorkOrder order = _pending;
            _pending = {};
            return order;
        }

        /// <summary>Drops all pending work (the frame turned off, or the window closed). A running worker stops at its
        /// next <see cref="Take"/>.</summary>
        void Clear() noexcept
        {
            _pending = {};
        }

        /// <summary>
        /// Undoes the claim of a worker that could not be started (the thread-pool submission failed), so the next
        /// request tries again instead of waiting forever on a worker that does not exist. Pending work is kept.
        /// </summary>
        void Abandon() noexcept
        {
            _running = false;
        }

        /// <summary>True while a worker owns the book (between a claiming request and the worker's empty Take).</summary>
        /// <returns>Whether a worker is in flight.</returns>
        bool Running() const noexcept
        {
            return _running;
        }

        /// <summary>The work waiting for the next pass (for tests and logging).</summary>
        /// <returns>A copy of the pending order.</returns>
        WorkOrder Pending() const noexcept
        {
            return _pending;
        }

    private:
        /// <summary>Marks a worker as needed; true only for the request that must start it.</summary>
        /// <returns>True when the caller must start a worker.</returns>
        bool _Claim() noexcept
        {
            if (_running)
            {
                return false;
            }
            _running = true;
            return true;
        }

        WorkOrder _pending;
        bool _running{ false };
    };

    namespace detail
    {
        /// <summary>
        /// A 32-bpp TOP-DOWN DIB section selected into a memory DC, so byte row y is pixel row y. Releases both handles
        /// when destroyed. Plain Win32; never throws.
        /// </summary>
        class DibCanvas
        {
        public:
            /// <summary>Creates the DC and the DIB; check <see cref="Valid"/> before use.</summary>
            /// <param name="size">The canvas size; an empty or absurd size (an edge over 16384) yields an invalid canvas.</param>
            explicit DibCanvas(PixelSize size) noexcept :
                _size{ size }
            {
                if (size.Empty() || size.width > 16384 || size.height > 16384)
                {
                    return;
                }
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = size.width;
                bmi.bmiHeader.biHeight = -size.height; // negative == top-down
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                _dc = ::CreateCompatibleDC(nullptr);
                if (!_dc)
                {
                    return;
                }
                _dib = ::CreateDIBSection(_dc, &bmi, DIB_RGB_COLORS, &_bits, nullptr, 0);
                if (!_dib || !_bits)
                {
                    return;
                }
                _old = ::SelectObject(_dc, _dib);
            }

            ~DibCanvas()
            {
                if (_dc && _old)
                {
                    ::SelectObject(_dc, _old);
                }
                if (_dib)
                {
                    ::DeleteObject(_dib);
                }
                if (_dc)
                {
                    ::DeleteDC(_dc);
                }
            }

            DibCanvas(const DibCanvas&) = delete;
            DibCanvas& operator=(const DibCanvas&) = delete;

            /// <summary>True when the DC, the DIB and its pixel memory all exist.</summary>
            /// <returns>Whether the canvas can be drawn on.</returns>
            bool Valid() const noexcept
            {
                return _dc && _dib && _bits && _old;
            }

            /// <summary>The memory DC the DIB is selected into.</summary>
            /// <returns>The DC (null when invalid).</returns>
            HDC Dc() const noexcept
            {
                return _dc;
            }

            /// <summary>The DIB's pixel memory (top-down BGRA), owned by the canvas.</summary>
            /// <returns>The pixel pointer (null when invalid).</returns>
            uint8_t* Bits() const noexcept
            {
                return static_cast<uint8_t*>(_bits);
            }

            /// <summary>The canvas size in pixels.</summary>
            /// <returns>The size the canvas was created with.</returns>
            PixelSize Size() const noexcept
            {
                return _size;
            }

            /// <summary>The pixel memory's length in bytes.</summary>
            /// <returns>width * height * 4.</returns>
            size_t ByteCount() const noexcept
            {
                return static_cast<size_t>(_size.width) * static_cast<size_t>(_size.height) * 4;
            }

        private:
            PixelSize _size;
            HDC _dc{ nullptr };
            HBITMAP _dib{ nullptr };
            HGDIOBJ _old{ nullptr };
            void* _bits{ nullptr };
        };
    }

    /// <summary>
    /// Captures a window's CLIENT AREA as DWM composes it -- XAML, swap chains and all -- into top-down opaque BGRA.
    /// </summary>
    /// <param name="hwnd">The window. Any thread may call this for any window (verified from a thread-pool thread of
    /// the window's own process).</param>
    /// <param name="bgra">Receives the pixels on success; cleared on failure.</param>
    /// <param name="size">Receives the client size on success; empty on failure.</param>
    /// <returns>True when PrintWindow reported success and the pixels were copied; FALSE for a minimized window (a 0x0
    /// client area), a destroyed window, or a GDI failure. A "successful" capture may still be all black -- check
    /// <see cref="LooksBlank"/>.</returns>
    /// <remarks>
    /// PW_RENDERFULLCONTENT (0x2) makes DWM render the window's composition tree into the bitmap; without it a
    /// DirectComposition window captures black. PW_CLIENTONLY (0x1) leaves out the invisible resize borders, which
    /// would otherwise frame the preview in black. Takes tens of milliseconds for a large window, so callers keep it
    /// off the UI thread.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the pixel buffer cannot be allocated (before any GDI handle exists).</exception>
    inline bool CaptureClient(HWND hwnd, std::vector<uint8_t>& bgra, PixelSize& size)
    {
        bgra.clear();
        size = {};
        constexpr UINT kClientOnly = 0x00000001; // PW_CLIENTONLY
        constexpr UINT kRenderFullContent = 0x00000002; // PW_RENDERFULLCONTENT (WinUser.h names it only in newer SDKs)
        RECT rc{};
        if (!hwnd || !::GetClientRect(hwnd, &rc))
        {
            return false;
        }
        const PixelSize client{ rc.right - rc.left, rc.bottom - rc.top };
        if (client.Empty() || client.width > 16384 || client.height > 16384)
        {
            return false;
        }
        // Allocated FIRST, so a bad_alloc can never strand the GDI handles below (the TaskbarBadge.h rule).
        std::vector<uint8_t> pixels(static_cast<size_t>(client.width) * static_cast<size_t>(client.height) * 4);
        detail::DibCanvas canvas{ client };
        if (!canvas.Valid())
        {
            return false;
        }
        const BOOL printed = ::PrintWindow(hwnd, canvas.Dc(), kClientOnly | kRenderFullContent);
        // GDI batches drawing; the DIB memory holds the picture only after a flush.
        ::GdiFlush();
        if (!printed)
        {
            return false;
        }
        std::memcpy(pixels.data(), canvas.Bits(), canvas.ByteCount());
        ForceOpaque(pixels);
        bgra = std::move(pixels);
        size = client;
        return true;
    }

    /// <summary>
    /// Draws <paramref name="icon"/> centered on a BGRA image (the placeholder card's app icon). The image stays fully
    /// opaque.
    /// </summary>
    /// <param name="bgra">Top-down BGRA pixels of <paramref name="size"/>, modified in place.</param>
    /// <param name="size">The image size.</param>
    /// <param name="icon">The icon (a shared, LR_SHARED handle is fine: it is only drawn, never destroyed here).
    /// Null leaves the image untouched.</param>
    /// <param name="edge">The drawn icon edge in pixels; clamped to the shorter side of the image.</param>
    /// <returns>True when the icon was drawn.</returns>
    /// <remarks>DrawIconEx blends a 32-bpp icon by its own alpha; GDI clears the alpha bytes it writes, so the image
    /// is forced opaque afterwards.</remarks>
    inline bool DrawIconCentered(std::vector<uint8_t>& bgra, PixelSize size, HICON icon, int edge) noexcept
    {
        if (!icon || size.Empty() || edge <= 0 ||
            bgra.size() < static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4)
        {
            return false;
        }
        detail::DibCanvas canvas{ size };
        if (!canvas.Valid())
        {
            return false;
        }
        std::memcpy(canvas.Bits(), bgra.data(), canvas.ByteCount());
        const int e = (std::min)(edge, (std::min)(size.width, size.height));
        const BOOL drawn = ::DrawIconEx(canvas.Dc(), (size.width - e) / 2, (size.height - e) / 2, icon, e, e, 0, nullptr, DI_NORMAL);
        ::GdiFlush();
        if (!drawn)
        {
            return false;
        }
        std::memcpy(bgra.data(), canvas.Bits(), canvas.ByteCount());
        ForceOpaque(bgra);
        return true;
    }

    /// <summary>
    /// Wraps BGRA pixels in a new 32-bpp top-down DIB section: the bitmap DwmSetIconicThumbnail and
    /// DwmSetIconicLivePreviewBitmap take.
    /// </summary>
    /// <param name="bgra">The pixels, size.width * size.height * 4 bytes.</param>
    /// <param name="size">The image size.</param>
    /// <returns>A new HBITMAP the CALLER must DeleteObject (DWM keeps its own copy, so right after the call is fine);
    /// null for an empty size, a short buffer or a GDI failure.</returns>
    inline HBITMAP CreateTopDownDib(const std::vector<uint8_t>& bgra, PixelSize size) noexcept
    {
        if (size.Empty() || size.width > 16384 || size.height > 16384 ||
            bgra.size() < static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4)
        {
            return nullptr;
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size.width;
        bmi.bmiHeader.biHeight = -size.height; // top-down, the order the pixels are in
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HBITMAP dib = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits)
        {
            if (dib)
            {
                ::DeleteObject(dib);
            }
            return nullptr;
        }
        std::memcpy(bits, bgra.data(), static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4);
        return dib;
    }
}
