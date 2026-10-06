// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: the TASKBAR UNREAD BADGE (doc/agentmaster/NOTIFICATIONS.md section 8)
// and the TASKBAR PREVIEW FRAME (section 9).
// Three parts: TaskbarBadge.h, the renderer that turns a count into the red-disc overlay icon (the pure label /
// description / size / composition helpers, then the real GDI glyph pass and CreateBadgeIcon, verified by
// DRAWING the icon the way the shell does); the engine's process-wide unread census (Engine.h), driven on a
// LOCAL Engine through the `...In` seams — never SharedEngine(), whose first access would start the bridge; and
// TaskbarThumbnail.h, the per-window preview frame's pixel work (the fitted size DWM accepts, the frame band, the
// box-filter downscale, the blank-capture check, the worker coalescer) plus its GDI half on real DCs (the icon on
// the placeholder card, the top-down DIB handed to DWM). The DWM calls themselves live in IslandWindow (the EXE)
// and were measured on a disposable Windows desktop (tools/taskbar-thumbnail-probe), not here.
// Shared CHECK/fixtures/decls live in m5_tests.h.
#include "m5_tests.h"

#include "../Engine.h"
#include "../TaskbarBadge.h"
#include "../TaskbarThumbnail.h"

#include <cstdlib>
#include <map>
#include <stdexcept>
#include <vector>

namespace
{
    // One drawn pixel, unpacked from the BGRX DIB word.
    struct Rgb
    {
        int r{ 0 };
        int g{ 0 };
        int b{ 0 };
    };

    // Draws `icon` with DrawIconEx(DI_NORMAL) onto a size x size 32-bpp DIB pre-filled with pure GREEN — what the
    // taskbar does with an overlay, minus the compositor — and returns the resulting pixels row-major, top-down.
    // Green is the one color the badge never contains (red disc, white digits), so any green left over is
    // transparency and any non-green is badge. Empty on a GDI failure.
    std::vector<Rgb> DrawIconOnGreen(HICON icon, int size)
    {
        std::vector<Rgb> out;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        const HDC dc = ::CreateCompatibleDC(nullptr);
        if (!dc)
        {
            return out;
        }
        void* bits = nullptr;
        const HBITMAP dib = ::CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits)
        {
            const HGDIOBJ old = ::SelectObject(dc, dib);
            auto* const px = static_cast<uint32_t*>(bits);
            for (int i = 0; i < size * size; ++i)
            {
                px[i] = 0x0000FF00u; // BGRX: pure green
            }
            ::DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
            ::GdiFlush();
            out.reserve(static_cast<size_t>(size) * static_cast<size_t>(size));
            for (int i = 0; i < size * size; ++i)
            {
                out.push_back(Rgb{ static_cast<int>((px[i] >> 16) & 0xFF), static_cast<int>((px[i] >> 8) & 0xFF), static_cast<int>(px[i] & 0xFF) });
            }
            ::SelectObject(dc, old);
        }
        if (dib)
        {
            ::DeleteObject(dib);
        }
        ::DeleteDC(dc);
        return out;
    }

    bool IsGreen(const Rgb& p)
    {
        return p.g > 200 && p.r < 60 && p.b < 60;
    }
    bool IsWhite(const Rgb& p)
    {
        return p.r > 200 && p.g > 200 && p.b > 200;
    }
    bool IsBadgeRed(const Rgb& p)
    {
        return p.r > 150 && p.g < 120 && p.b < 120;
    }

    // The ink-weighted centroid of a size*size coverage mask (x, y in pixel-edge coordinates; center of the icon
    // == size / 2). Returns false when nothing is inked.
    bool CoverageCentroid(const std::vector<uint8_t>& cov, int size, double& cx, double& cy)
    {
        double sum = 0.0;
        double sx = 0.0;
        double sy = 0.0;
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                const double w = cov[static_cast<size_t>(y) * size + x];
                sum += w;
                sx += w * (x + 0.5);
                sy += w * (y + 0.5);
            }
        }
        if (sum <= 0.0)
        {
            return false;
        }
        cx = sx / sum;
        cy = sy / sum;
        return true;
    }
}

void TestTaskbarBadge()
{
    std::wprintf(L"TaskbarBadge (the taskbar-button unread count overlay):\n");
    namespace TB = Agentmaster::TaskbarBadge;

    // --- the label + the accessibility text ---
    CHECK(TB::Label(0).empty(), "label: 0 => empty (no badge)");
    CHECK(TB::Label(1) == L"1", "label: 1");
    CHECK(TB::Label(9) == L"9", "label: 9");
    CHECK(TB::Label(10) == L"10", "label: two digits");
    CHECK(TB::Label(99) == L"99", "label: 99 is the last number drawn as digits");
    CHECK(TB::Label(100) == L"99+", "label: 100 => 99+ (the shell's own badge cap)");
    CHECK(TB::Label(0xFFFFFFFFu) == L"99+", "label: a huge count still reads 99+");
    CHECK(TB::Description(0).empty(), "description: 0 => empty");
    CHECK(TB::Description(1) == L"1 unread Agentmaster session", "description: singular");
    CHECK(TB::Description(3) == L"3 unread Agentmaster sessions", "description: plural");
    CHECK(TB::Description(150) == L"150 unread Agentmaster sessions", "description: the EXACT count past the visual 99+ cap");

    // --- the icon size follows SM_CXSMICON's DPI scaling ---
    CHECK(TB::IconSizeForDpi(96) == 16, "size: 96 DPI => 16 px");
    CHECK(TB::IconSizeForDpi(120) == 20, "size: 125% => 20 px");
    CHECK(TB::IconSizeForDpi(144) == 24, "size: 150% => 24 px");
    CHECK(TB::IconSizeForDpi(168) == 28, "size: 175% => 28 px");
    CHECK(TB::IconSizeForDpi(192) == 32, "size: 200% => 32 px");
    CHECK(TB::IconSizeForDpi(0) == 16, "size: unknown DPI (0) => the 96 DPI size");
    CHECK(TB::IconSizeForDpi(48) == TB::kMinIconSize, "size: clamped up to the 16 px minimum");
    CHECK(TB::IconSizeForDpi(100000) == TB::kMaxIconSize, "size: a hostile DPI clamps (and cannot overflow)");

    // --- the disc: centered, full-bleed, symmetric ---
    CHECK(TB::DiscCoverage(16, 7, 7) == 1.0 && TB::DiscCoverage(16, 8, 8) == 1.0, "disc: the center pixels are fully covered");
    CHECK(TB::DiscCoverage(16, 0, 0) == 0.0 && TB::DiscCoverage(16, 15, 15) == 0.0, "disc: the corners are transparent");
    {
        const double edge = TB::DiscCoverage(16, 0, 8);
        CHECK(edge > 0.0 && edge < 1.0, "disc: the rim pixel mid-left is partially covered (anti-aliased edge)");
        CHECK(TB::DiscCoverage(16, 0, 8) == TB::DiscCoverage(16, 15, 7), "disc: point-symmetric about the center");
        CHECK(TB::DiscCoverage(16, 3, 5) == TB::DiscCoverage(16, 5, 3), "disc: symmetric about the diagonal");
    }
    CHECK(TB::DiscCoverage(16, -1, 3) == 0.0 && TB::DiscCoverage(16, 3, 16) == 0.0 && TB::DiscCoverage(0, 0, 0) == 0.0, "disc: out-of-range arguments are 0, never UB");

    // --- the composition: straight-alpha BGRA, fill blended toward white under the text ---
    {
        const int s = 16;
        const auto noText = TB::ComposePixels(s, {}, TB::kDiscFillRgb);
        CHECK(noText.size() == static_cast<size_t>(s) * s * 4, "compose: size*size*4 bytes");
        const auto at = [&](const std::vector<uint8_t>& px, int x, int y) { return px.data() + (static_cast<size_t>(y) * s + x) * 4; };
        const uint8_t* c = at(noText, 8, 8);
        CHECK(c[0] == 0x38 && c[1] == 0x34 && c[2] == 0xD1 && c[3] == 255, "compose: a covered pixel is the opaque disc fill (BGRA)");
        const uint8_t* k = at(noText, 0, 0);
        CHECK(k[0] == 0 && k[1] == 0 && k[2] == 0 && k[3] == 0, "compose: a corner is all-zero (transparent, and what the AND mask keys on)");
        const uint8_t* e = at(noText, 0, 8);
        CHECK(e[3] > 0 && e[3] < 255 && e[2] == 0xD1, "compose: a rim pixel keeps the FULL fill color with partial alpha (straight, not premultiplied)");

        std::vector<uint8_t> text(static_cast<size_t>(s) * s, 0);
        text[static_cast<size_t>(8) * s + 8] = 255; // full ink at the center
        text[static_cast<size_t>(8) * s + 7] = 128; // half ink beside it
        text[0] = 255; // ink in the transparent corner
        const auto withText = TB::ComposePixels(s, text, TB::kDiscFillRgb);
        const uint8_t* w = at(withText, 8, 8);
        CHECK(w[0] == 255 && w[1] == 255 && w[2] == 255 && w[3] == 255, "compose: full ink is opaque white");
        const uint8_t* h = at(withText, 7, 8);
        CHECK(h[2] > 0xD1 && h[2] < 255 && h[1] > 0x34 && h[1] < 255, "compose: half ink blends between the fill and white");
        CHECK(at(withText, 0, 0)[3] == 0, "compose: ink OUTSIDE the disc is dropped with the disc's alpha (no white corner)");

        std::vector<uint8_t> wrongSize(10, 255);
        CHECK(TB::ComposePixels(s, wrongSize, TB::kDiscFillRgb) == noText, "compose: a mis-sized text mask draws the disc alone");
        CHECK(TB::ComposePixels(0, {}, TB::kDiscFillRgb).empty() && TB::ComposePixels(257, {}, TB::kDiscFillRgb).empty(), "compose: an out-of-range size is empty");
    }

    // --- FindInk ---
    {
        std::vector<uint8_t> m(5 * 4, 0);
        CHECK(Agentmaster::TaskbarBadge::detail::FindInk(m, 5, 4).Empty(), "ink: a blank mask is empty");
        m[1 * 5 + 2] = 1;
        m[3 * 5 + 4] = 1;
        const auto b = Agentmaster::TaskbarBadge::detail::FindInk(m, 5, 4);
        CHECK(!b.Empty() && b.x0 == 2 && b.y0 == 1 && b.x1 == 4 && b.y1 == 3, "ink: the inclusive bounding box of the inked pixels");
        CHECK(Agentmaster::TaskbarBadge::detail::FindInk(m, 6, 4).Empty(), "ink: dimensions that overrun the buffer read as empty");
    }

    // --- the real GDI glyph pass: digits present, fitted, and optically centered ---
    for (const int size : { 16, 20, 24, 32 })
    {
        for (const uint32_t count : { 1u, 8u, 42u, 250u })
        {
            const auto cov = TB::RenderLabelCoverage(TB::Label(count), size);
            const bool rendered = cov.size() == static_cast<size_t>(size) * size;
            CHECK(rendered, "glyphs: RenderLabelCoverage yields size*size coverages (Segoe UI present)");
            if (!rendered)
            {
                continue;
            }
            double cx = 0.0;
            double cy = 0.0;
            const bool inked = CoverageCentroid(cov, size, cx, cy);
            CHECK(inked, "glyphs: the label actually inks pixels");
            // One digit centers tightly; "42" / "99+" have asymmetric glyphs, so allow a little more.
            const double tolerance = count < 10 ? 1.5 : 2.5;
            CHECK(inked && std::abs(cx - size / 2.0) <= tolerance && std::abs(cy - size / 2.0) <= tolerance, "glyphs: the ink is optically centered on the disc");
            // Every inked pixel must fall inside the disc, or the label would be clipped.
            bool insideDisc = true;
            for (int y = 0; y < size; ++y)
            {
                for (int x = 0; x < size; ++x)
                {
                    if (cov[static_cast<size_t>(y) * size + x] > 0 && TB::DiscCoverage(size, x, y) <= 0.0)
                    {
                        insideDisc = false;
                    }
                }
            }
            CHECK(insideDisc, "glyphs: the fitted label stays inside the disc");
        }
    }
    CHECK(TB::RenderLabelCoverage(L"", 16).empty(), "glyphs: an empty label renders nothing");
    CHECK(TB::RenderLabelCoverage(L"3", 8).empty() && TB::RenderLabelCoverage(L"3", 65).empty(), "glyphs: out-of-range sizes render nothing");

    // --- the icon, drawn the way the shell draws an overlay ---
    CHECK(TB::CreateBadgeIcon(0, 16) == nullptr, "icon: count 0 => no icon (the overlay is removed instead)");
    CHECK(TB::CreateBadgeIcon(3, 15) == nullptr && TB::CreateBadgeIcon(3, 65) == nullptr, "icon: out-of-range sizes => no icon");
    for (const int size : { 16, 24, 32 })
    {
        for (const uint32_t count : { 3u, 57u, 1000u })
        {
            const HICON icon = TB::CreateBadgeIcon(count, size);
            CHECK(icon != nullptr, "icon: CreateBadgeIcon returns an HICON");
            if (!icon)
            {
                continue;
            }
            ICONINFO info{};
            if (::GetIconInfo(icon, &info))
            {
                BITMAP bm{};
                const bool gotColor = info.hbmColor && ::GetObjectW(info.hbmColor, sizeof(bm), &bm) == sizeof(bm);
                CHECK(gotColor && bm.bmWidth == size && std::abs(bm.bmHeight) == size && bm.bmBitsPixel == 32, "icon: a 32-bpp color bitmap of the requested size");
                CHECK(info.fIcon == TRUE, "icon: an icon, not a cursor");
                if (info.hbmColor)
                {
                    ::DeleteObject(info.hbmColor);
                }
                if (info.hbmMask)
                {
                    ::DeleteObject(info.hbmMask);
                }
            }
            else
            {
                CHECK(false, "icon: GetIconInfo succeeds");
            }

            const auto drawn = DrawIconOnGreen(icon, size);
            ::DestroyIcon(icon);
            CHECK(drawn.size() == static_cast<size_t>(size) * size, "icon: drawn onto the green test canvas");
            if (drawn.size() != static_cast<size_t>(size) * size)
            {
                continue;
            }
            const auto px = [&](int x, int y) { return drawn[static_cast<size_t>(y) * size + x]; };
            CHECK(IsGreen(px(0, 0)) && IsGreen(px(size - 1, 0)) && IsGreen(px(0, size - 1)) && IsGreen(px(size - 1, size - 1)), "icon: the four corners stay transparent (the canvas shows through)");
            int white = 0;
            int red = 0;
            int greenInsideDisc = 0;
            for (int y = 0; y < size; ++y)
            {
                for (int x = 0; x < size; ++x)
                {
                    const Rgb p = px(x, y);
                    white += IsWhite(p) ? 1 : 0;
                    red += IsBadgeRed(p) ? 1 : 0;
                    if (TB::DiscCoverage(size, x, y) == 1.0 && IsGreen(p))
                    {
                        ++greenInsideDisc;
                    }
                }
            }
            CHECK(white > 0, "icon: white digit pixels are drawn");
            CHECK(red > size, "icon: the red disc is drawn");
            CHECK(greenInsideDisc == 0, "icon: the fully covered disc area is opaque (alpha honored, nothing shows through)");
        }
    }
}

void TestUnreadCensus()
{
    std::wprintf(L"Unread census (the process-wide count behind the taskbar badge):\n");
    using Agentmaster::Engine;

    // --- the pure union ---
    {
        std::map<std::wstring, std::vector<std::wstring>> m;
        CHECK(Agentmaster::CountDistinctUnreadSessions(m) == 0, "union: no windows => 0");
        m[L"{a}"] = { L"s1", L"s2" };
        m[L"{b}"] = { L"s3" };
        CHECK(Agentmaster::CountDistinctUnreadSessions(m) == 3, "union: disjoint windows add up");
        m[L"{b}"] = { L"s2", L"s3", L"s3", L"" };
        CHECK(Agentmaster::CountDistinctUnreadSessions(m) == 3, "union: a session in two windows (a moving tab) counts once; duplicates + empty ids ignored");
    }

    // --- publish / fan-out on a LOCAL engine ---
    {
        Engine e;
        std::vector<uint32_t> seenA;
        std::vector<uint32_t> seenB;
        const uint64_t tokenA = Agentmaster::RegisterUnreadCountHandlerIn(e, L"{win-a}", [&](uint32_t n) { seenA.push_back(n); });
        const uint64_t tokenB = Agentmaster::RegisterUnreadCountHandlerIn(e, L"{win-b}", [&](uint32_t n) { seenB.push_back(n); });
        CHECK(tokenA != 0 && tokenB != 0 && tokenA != tokenB, "sink: distinct non-zero tokens");
        CHECK(Agentmaster::RegisterUnreadCountHandlerIn(e, L"{x}", {}) == 0, "sink: an empty handler is refused (token 0)");
        CHECK(Agentmaster::CurrentUnreadCountIn(e) == 0, "census: starts at 0");

        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{win-a}", { L"s1", L"s2" });
        CHECK(Agentmaster::CurrentUnreadCountIn(e) == 2, "census: window A's two unread sessions");
        CHECK(seenA == std::vector<uint32_t>{ 2 } && seenB == std::vector<uint32_t>{ 2 }, "fan-out: EVERY window's sink hears the change (the source included)");

        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{win-b}", { L"s3" });
        CHECK(Agentmaster::CurrentUnreadCountIn(e) == 3 && seenA.back() == 3 && seenB.back() == 3, "census: a second window's session is added (+1 per newly unread session)");

        const size_t before = seenA.size();
        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{win-b}", { L"s3" });
        CHECK(seenA.size() == before, "fan-out: re-publishing the same set is silent");
        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{win-b}", { L"s4" });
        CHECK(seenA.size() == before && Agentmaster::CurrentUnreadCountIn(e) == 3, "fan-out: a different set with the SAME count (one read, one new) repaints nothing");

        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{win-b}", { L"s4", L"s1", L"s1", L"" });
        CHECK(Agentmaster::CurrentUnreadCountIn(e) == 3, "census: a session published by two windows counts once; duplicates + empties normalized away");

        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{win-a}", {});
        CHECK(Agentmaster::CurrentUnreadCountIn(e) == 2 && seenB.back() == 2, "census: an empty publish (a closed / fully-read window) withdraws that window");
        {
            std::lock_guard<std::mutex> lk(e.unreadMutex);
            CHECK(e.unreadByWindow.count(L"{win-a}") == 0, "census: an empty publish ERASES the window's row");
        }

        Agentmaster::UnregisterUnreadCountHandlerIn(e, tokenA);
        const size_t aBefore = seenA.size();
        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{win-b}", {});
        CHECK(seenA.size() == aBefore && seenB.back() == 0, "sink: an unregistered sink is no longer called; the count returns to 0");
        Agentmaster::UnregisterUnreadCountHandlerIn(e, tokenA); // stale
        Agentmaster::UnregisterUnreadCountHandlerIn(e, 0); // zero
        CHECK(Agentmaster::CurrentUnreadCountIn(e) == 0, "sink: stale / zero token unregisters are no-ops");
        Agentmaster::UnregisterUnreadCountHandlerIn(e, tokenB);
    }

    // --- one throwing sink cannot starve the others (a dying window's dispatcher) ---
    {
        Engine e;
        std::vector<uint32_t> seen;
        Agentmaster::RegisterUnreadCountHandlerIn(e, L"{dying}", [](uint32_t) { throw std::runtime_error("dispatcher shut down"); });
        Agentmaster::RegisterUnreadCountHandlerIn(e, L"{alive}", [&](uint32_t n) { seen.push_back(n); });
        Agentmaster::PublishWindowUnreadSessionsIn(e, L"{src}", { L"s1" }); // noexcept: the throw is logged + contained
        CHECK(seen == std::vector<uint32_t>{ 1 }, "fan-out: a throwing sink is contained; the LATER sink still hears the new count");
        CHECK(Agentmaster::CurrentUnreadCountIn(e) == 1, "fan-out: the census still recorded the change");
    }
}

namespace
{
    // A top-down BGRA image of `size` with every pixel set to (b, g, r, a): the preview-frame tests' canvas.
    std::vector<uint8_t> ThumbFilled(Agentmaster::TaskbarThumbnail::PixelSize size, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
    {
        std::vector<uint8_t> out(static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4);
        for (size_t i = 0; i < out.size(); i += 4)
        {
            out[i] = b;
            out[i + 1] = g;
            out[i + 2] = r;
            out[i + 3] = a;
        }
        return out;
    }

    // Pixel (x, y) of a top-down BGRA image `width` pixels wide: a pointer to its 4 bytes (B, G, R, A).
    const uint8_t* ThumbPx(const std::vector<uint8_t>& bgra, int width, int x, int y)
    {
        return bgra.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    }

    // True when pixel (x, y) holds exactly (b, g, r, a).
    bool ThumbPxIs(const std::vector<uint8_t>& bgra, int width, int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
    {
        const uint8_t* const p = ThumbPx(bgra, width, x, y);
        return p[0] == b && p[1] == g && p[2] == r && p[3] == a;
    }
}

void TestTaskbarThumbnail()
{
    std::wprintf(L"TaskbarThumbnail (the frame on the taskbar / Alt+Tab preview of a window with unread tabs):\n");
    namespace TT = Agentmaster::TaskbarThumbnail;
    using TT::PixelSize;

    // --- FitInside: the pushed preview never exceeds DWM's box (a larger one is rejected with E_INVALIDARG) ---
    {
        const PixelSize probe = TT::FitInside(PixelSize{ 704, 381 }, PixelSize{ 200, 108 });
        CHECK(probe.width == 199 && probe.height == 108, "fit: the probe's 704x381 window in DWM's 200x108 box => 199x108");
        const PixelSize terminal = TT::FitInside(PixelSize{ 1113, 627 }, PixelSize{ 200, 108 });
        CHECK(terminal.width == 191 && terminal.height == 108, "fit: a 1113x627 Windows Terminal client area => 191x108 (rounded down, never 192)");
        const PixelSize exact = TT::FitInside(PixelSize{ 200, 108 }, PixelSize{ 200, 108 });
        CHECK(exact.width == 200 && exact.height == 108, "fit: a source the size of the box is kept");
        const PixelSize grown = TT::FitInside(PixelSize{ 100, 54 }, PixelSize{ 200, 108 });
        CHECK(grown.width == 200 && grown.height == 108, "fit: a smaller source of the same aspect is scaled UP to the box");
        const PixelSize wide = TT::FitInside(PixelSize{ 400, 100 }, PixelSize{ 200, 108 });
        CHECK(wide.width == 200 && wide.height == 50, "fit: a wide source binds on the width");
        const PixelSize tall = TT::FitInside(PixelSize{ 100, 400 }, PixelSize{ 200, 108 });
        CHECK(tall.width == 27 && tall.height == 108, "fit: a tall source binds on the height");
        const PixelSize sliver = TT::FitInside(PixelSize{ 10000, 1 }, PixelSize{ 200, 108 });
        const PixelSize needle = TT::FitInside(PixelSize{ 1, 10000 }, PixelSize{ 200, 108 });
        CHECK(sliver.width == 200 && sliver.height == 1 && needle.width == 1 && needle.height == 108, "fit: an extreme aspect still keeps at least 1 pixel on its short side");
        CHECK(TT::FitInside(PixelSize{ 0, 10 }, PixelSize{ 200, 108 }).Empty(), "fit: an empty source => empty (nothing to push)");
        CHECK(TT::FitInside(PixelSize{ 704, 381 }, PixelSize{ 200, 0 }).Empty(), "fit: an empty box => empty");
        CHECK(TT::FitInside(PixelSize{ -5, 10 }, PixelSize{ 200, 108 }).Empty(), "fit: a negative dimension counts as empty");

        // A seeded fuzz: every fit is inside its box, touches it on one side, and keeps the aspect within one pixel
        // (the cross-multiplied skew stays within the source's longer side).
        uint64_t state = 0x5EED7A5Bull;
        const auto next = [&state](int lo, int hi) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return lo + static_cast<int>((state >> 33) % static_cast<uint64_t>(hi - lo + 1));
        };
        int bad = 0;
        for (int i = 0; i < 4000; ++i)
        {
            const PixelSize src{ next(1, 20000), next(1, 20000) };
            const PixelSize box{ next(1, 2000), next(1, 2000) };
            const PixelSize f = TT::FitInside(src, box);
            const bool inBox = f.width >= 1 && f.height >= 1 && f.width <= box.width && f.height <= box.height;
            const bool touches = f.width == box.width || f.height == box.height;
            const int64_t skew = static_cast<int64_t>(f.width) * src.height - static_cast<int64_t>(f.height) * src.width;
            const int64_t limit = (std::max)(src.width, src.height);
            if (!inBox || !touches || skew > limit || skew < -limit)
            {
                ++bad;
            }
        }
        CHECK(bad == 0, "fit: 4000 random sizes land inside their box, touch it on one side, and keep the aspect within one pixel");
    }

    // --- EstimatedBox: the box pushed into before DWM has asked once ---
    {
        const PixelSize at96 = TT::EstimatedBox(96);
        CHECK(at96.width == 192 && at96.height == 115, "estimate: 100% scaling => 96% of 200x120 = 192x115");
        const PixelSize unknown = TT::EstimatedBox(0);
        CHECK(unknown == at96, "estimate: an unknown DPI (0) is treated as 96");
        const PixelSize at144 = TT::EstimatedBox(144);
        CHECK(at144.width == 288 && at144.height == 172, "estimate: 150% scaling => 288x172");
        const PixelSize at192 = TT::EstimatedBox(192);
        CHECK(at192.width == 384 && at192.height == 230, "estimate: 200% scaling => 384x230");
        const PixelSize clamped = TT::EstimatedBox(100000);
        CHECK(clamped.width == 1920 && clamped.height == 1152, "estimate: a DPI over 960 is clamped to 960 (1000%)");
        CHECK(!TT::EstimatedBox(1).Empty(), "estimate: never empty, even for a 1-DPI report");
        const PixelSize pushed = TT::FitInside(PixelSize{ 704, 381 }, at96);
        CHECK(pushed.width <= 200 && pushed.height <= 108, "estimate: the probe window's estimated push (192x103) fits inside the box DWM really asked for (200x108)");
    }

    // --- FrameThickness ---
    {
        CHECK(TT::FrameThickness(PixelSize{ 0, 0 }) == 0 && TT::FrameThickness(PixelSize{ 200, 0 }) == 0, "thickness: an empty preview has no frame");
        CHECK(TT::FrameThickness(PixelSize{ 200, 108 }) == 4, "thickness: the 100% preview (200x108) => 4 px");
        CHECK(TT::FrameThickness(PixelSize{ 300, 162 }) == 6, "thickness: the 150% preview => 6 px");
        CHECK(TT::FrameThickness(PixelSize{ 108, 200 }) == 4, "thickness: the SHORTER side decides, in either orientation");
        CHECK(TT::FrameThickness(PixelSize{ 2000, 1000 }) == 41, "thickness: about 1/24 of the shorter side on a large bitmap");
        CHECK(TT::FrameThickness(PixelSize{ 40, 40 }) == 3, "thickness: at least 3 px, so it reads as a frame");
        CHECK(TT::FrameThickness(PixelSize{ 8, 8 }) == 2, "thickness: at most a quarter of the shorter side on a tiny preview");
        CHECK(TT::FrameThickness(PixelSize{ 1, 1 }) == 1 && TT::FrameThickness(PixelSize{ 2, 100 }) == 1, "thickness: a 1-2 px side still gets a 1 px frame");
        int bad = 0;
        for (int s = 1; s <= 400; ++s)
        {
            const int t = TT::FrameThickness(PixelSize{ s, s * 2 });
            if (t < 1 || (s >= 4 && t * 4 > s) || (s >= 12 && t < 3))
            {
                ++bad;
            }
        }
        CHECK(bad == 0, "thickness: for every shorter side 1..400 the frame is >= 1, <= a quarter (from 4 px) and >= 3 (from 12 px)");
    }

    // --- FrameColor: the user's flash-ring color, never fainter than the floor ---
    {
        CHECK(TT::FrameColor(0xCCFF0000u) == 0xCCFF0000u, "color: the default flashRingColor (#CCFF0000, 80% red) is used as is");
        CHECK(TT::FrameColor(0xFF00FF00u) == 0xFF00FF00u, "color: an opaque pick is used as is");
        CHECK(TT::FrameColor(0x80123456u) == 0x80123456u, "color: an alpha exactly at the floor is kept");
        CHECK(TT::FrameColor(0x7F123456u) == 0x80123456u, "color: an alpha just under the floor is raised to it, the RGB kept");
        CHECK(TT::FrameColor(0x00ABCDEFu) == 0x80ABCDEFu, "color: a fully transparent pick still draws a visible frame");
    }

    // --- ForceOpaque ---
    {
        std::vector<uint8_t> px{ 1, 2, 3, 0, 5, 6, 7, 0, 9 };
        TT::ForceOpaque(px);
        CHECK(px[3] == 0xFF && px[7] == 0xFF, "opaque: every alpha byte becomes 255 (GDI leaves 0, which DWM shows as transparent)");
        CHECK(px[0] == 1 && px[1] == 2 && px[2] == 3 && px[4] == 5 && px[5] == 6 && px[6] == 7, "opaque: the color bytes are untouched");
        CHECK(px[8] == 9, "opaque: a trailing partial pixel is left alone");
    }

    // --- DownscaleBgra: the box filter from the client capture to the preview ---
    {
        CHECK(TT::DownscaleBgra(std::vector<uint8_t>(16), PixelSize{ 2, 2 }, PixelSize{ 0, 1 }).empty(), "downscale: an empty target => empty");
        CHECK(TT::DownscaleBgra(std::vector<uint8_t>(16), PixelSize{ 0, 2 }, PixelSize{ 1, 1 }).empty(), "downscale: an empty source => empty");
        CHECK(TT::DownscaleBgra(std::vector<uint8_t>(15), PixelSize{ 2, 2 }, PixelSize{ 1, 1 }).empty(), "downscale: a buffer shorter than the source size says => empty, never a read past it");

        // 4x2 -> 2x1: each output pixel is the ROUNDED mean of its 2x2 footprint; the source alpha (0) is replaced.
        const std::vector<uint8_t> src{
            10, 20, 30, 0, 11, 21, 31, 0, 100, 0, 255, 0, 101, 1, 255, 0,
            12, 22, 32, 0, 13, 23, 33, 0, 102, 2, 255, 0, 103, 3, 254, 0,
        };
        const std::vector<uint8_t> half = TT::DownscaleBgra(src, PixelSize{ 4, 2 }, PixelSize{ 2, 1 });
        CHECK(half.size() == 8, "downscale: 4x2 -> 2x1 gives 2 pixels");
        CHECK(half.size() == 8 && half[0] == 12 && half[1] == 22 && half[2] == 32 && half[3] == 0xFF, "downscale: the left footprint averages to (12, 22, 32), rounded half up (46/4 = 11.5 -> 12), opaque");
        CHECK(half.size() == 8 && half[4] == 102 && half[5] == 2 && half[6] == 255 && half[7] == 0xFF, "downscale: the right footprint averages to (102, 2, 255)");

        const std::vector<uint8_t> same = TT::DownscaleBgra(src, PixelSize{ 4, 2 }, PixelSize{ 4, 2 });
        bool identity = same.size() == src.size();
        for (size_t i = 0; identity && i < src.size(); ++i)
        {
            const int expected = (i % 4) == 3 ? 0xFF : src[i];
            identity = same[i] == expected;
        }
        CHECK(identity, "downscale: the same size copies every pixel (alpha forced to 255)");

        const std::vector<uint8_t> one{ 7, 8, 9, 0 };
        const std::vector<uint8_t> grownImage = TT::DownscaleBgra(one, PixelSize{ 1, 1 }, PixelSize{ 3, 2 });
        bool allSame = grownImage.size() == 24;
        for (size_t i = 0; allSame && i < grownImage.size(); i += 4)
        {
            allSame = grownImage[i] == 7 && grownImage[i + 1] == 8 && grownImage[i + 2] == 9 && grownImage[i + 3] == 0xFF;
        }
        CHECK(allSame, "downscale: growing (a window smaller than the preview) repeats the nearest pixel");

        // Tiling: one lit source pixel lands in EXACTLY one output pixel, for every position -- no source pixel is
        // skipped (a gap) or read twice (an overlap) when the ratio is not a whole number.
        struct Ratio
        {
            PixelSize from;
            PixelSize to;
        };
        const Ratio ratios[] = { { { 5, 3 }, { 2, 2 } }, { { 5, 3 }, { 3, 2 } }, { { 7, 5 }, { 3, 2 } }, { { 9, 7 }, { 4, 3 } } };
        int tilingBad = 0;
        for (const Ratio& ratio : ratios)
        {
            const size_t pixels = static_cast<size_t>(ratio.from.width) * static_cast<size_t>(ratio.from.height);
            for (size_t lit = 0; lit < pixels; ++lit)
            {
                std::vector<uint8_t> image(pixels * 4, 0);
                image[lit * 4] = 255;
                const std::vector<uint8_t> out = TT::DownscaleBgra(image, ratio.from, ratio.to);
                int hits = 0;
                for (size_t i = 0; i < out.size(); i += 4)
                {
                    hits += out[i] != 0 ? 1 : 0;
                }
                tilingBad += hits == 1 ? 0 : 1;
            }
        }
        CHECK(tilingBad == 0, "downscale: every source pixel feeds exactly one output pixel (5x3->2x2, 5x3->3x2, 7x5->3x2, 9x7->4x3)");
    }

    // --- DrawFrame: the band along all four edges, blended with the color's alpha ---
    {
        const PixelSize size{ 20, 10 };
        std::vector<uint8_t> img = ThumbFilled(size, 100, 100, 100, 0xFF);
        TT::DrawFrame(img, size, 3, 0xFFFF0000u);
        int painted = 0;
        int bandOk = 0;
        int interiorOk = 0;
        for (int y = 0; y < size.height; ++y)
        {
            for (int x = 0; x < size.width; ++x)
            {
                const bool inBand = x < 3 || x >= 17 || y < 3 || y >= 7;
                const bool red = ThumbPxIs(img, size.width, x, y, 0, 0, 255, 0xFF);
                const bool gray = ThumbPxIs(img, size.width, x, y, 100, 100, 100, 0xFF);
                painted += red ? 1 : 0;
                bandOk += (inBand && red) ? 1 : 0;
                interiorOk += (!inBand && gray) ? 1 : 0;
            }
        }
        CHECK(painted == 144 && bandOk == 144, "frame: an opaque color paints exactly the 3 px band of a 20x10 image (144 pixels) pure red");
        CHECK(interiorOk == 56, "frame: the 14x4 interior is untouched");

        // 50% red over black and over white: straight-alpha "over", rounded.
        const PixelSize sixBySix{ 6, 6 };
        std::vector<uint8_t> black = ThumbFilled(sixBySix, 0, 0, 0, 0);
        TT::DrawFrame(black, sixBySix, 1, 0x80FF0000u);
        CHECK(ThumbPxIs(black, sixBySix.width, 0, 0, 0, 0, 128, 0xFF), "frame: 50% red over black => (0, 0, 128), and the band pixel becomes opaque");
        CHECK(ThumbPxIs(black, sixBySix.width, 3, 3, 0, 0, 0, 0), "frame: the interior keeps even its alpha (0) -- only band pixels are written");
        std::vector<uint8_t> white = ThumbFilled(sixBySix, 255, 255, 255, 0xFF);
        TT::DrawFrame(white, sixBySix, 1, 0x80FF0000u);
        CHECK(ThumbPxIs(white, sixBySix.width, 5, 5, 127, 127, 255, 0xFF), "frame: 50% red over white => (127, 127, 255)");

        const PixelSize tiny{ 7, 5 };
        std::vector<uint8_t> all = ThumbFilled(tiny, 9, 9, 9, 0xFF);
        TT::DrawFrame(all, tiny, 100, 0xFFFF0000u);
        bool allRed = true;
        for (int y = 0; y < tiny.height; ++y)
        {
            for (int x = 0; x < tiny.width; ++x)
            {
                allRed = allRed && ThumbPxIs(all, tiny.width, x, y, 0, 0, 255, 0xFF);
            }
        }
        CHECK(allRed, "frame: a thickness past half the shorter side is clamped there and covers the whole image (no out-of-range write)");

        const PixelSize odd{ 5, 5 };
        std::vector<uint8_t> ring = ThumbFilled(odd, 50, 50, 50, 0xFF);
        TT::DrawFrame(ring, odd, 2, 0xFF0000FFu);
        int ringPainted = 0;
        for (int y = 0; y < odd.height; ++y)
        {
            for (int x = 0; x < odd.width; ++x)
            {
                ringPainted += ThumbPxIs(ring, odd.width, x, y, 255, 0, 0, 0xFF) ? 1 : 0;
            }
        }
        CHECK(ringPainted == 24 && ThumbPxIs(ring, odd.width, 2, 2, 50, 50, 50, 0xFF), "frame: a 2 px band on 5x5 leaves only the center pixel");

        const std::vector<uint8_t> before = ThumbFilled(size, 1, 2, 3, 4);
        std::vector<uint8_t> untouched = before;
        TT::DrawFrame(untouched, size, 0, 0xFFFF0000u);
        TT::DrawFrame(untouched, size, -2, 0xFFFF0000u);
        TT::DrawFrame(untouched, PixelSize{ 0, 10 }, 3, 0xFFFF0000u);
        TT::DrawFrame(untouched, PixelSize{ 21, 10 }, 3, 0xFFFF0000u); // claims more pixels than the buffer holds
        CHECK(untouched == before, "frame: a zero / negative thickness, an empty size or a too-short buffer changes nothing");
    }

    // --- LooksBlank: what PrintWindow returns when the content was not rendered for it ---
    {
        const PixelSize size{ 4, 4 };
        CHECK(TT::LooksBlank(std::vector<uint8_t>(64), PixelSize{ 0, 4 }), "blank: an empty size counts as blank");
        CHECK(TT::LooksBlank(std::vector<uint8_t>(63), size), "blank: a buffer too short for the size counts as blank");
        CHECK(TT::LooksBlank(std::vector<uint8_t>(64), size), "blank: an all-zero capture is blank");
        CHECK(TT::LooksBlank(ThumbFilled(size, 2, 2, 2, 0xFF), size), "blank: near-black (every channel <= 2) is blank, whatever the alpha");
        std::vector<uint8_t> lastLit = ThumbFilled(size, 0, 0, 0, 0xFF);
        lastLit[lastLit.size() - 3] = 3; // the G byte of the LAST pixel
        CHECK(!TT::LooksBlank(lastLit, size), "blank: one channel of the last pixel at 3 is content (the scan reaches the end)");
        CHECK(!TT::LooksBlank(ThumbFilled(size, 0x0C, 0x0C, 0x0C, 0xFF), size), "blank: a terminal's #0C0C0C background is content, never blank");
    }

    // --- SolidCard: the placeholder's base ---
    {
        const std::vector<uint8_t> card = TT::SolidCard(PixelSize{ 3, 2 }, 0x112233u);
        bool cardOk = card.size() == 24;
        for (size_t i = 0; cardOk && i < card.size(); i += 4)
        {
            cardOk = card[i] == 0x33 && card[i + 1] == 0x22 && card[i + 2] == 0x11 && card[i + 3] == 0xFF;
        }
        CHECK(cardOk, "card: 0xRRGGBB is stored as B, G, R, opaque");
        CHECK(TT::SolidCard(PixelSize{ 0, 2 }, 0x112233u).empty(), "card: an empty size => empty");
        CHECK(!TT::LooksBlank(TT::SolidCard(PixelSize{ 8, 8 }, TT::kCardFillRgb), PixelSize{ 8, 8 }), "card: the placeholder fill (#202020) is not mistaken for a blank capture");
    }

    // --- JobBook: at most one capture worker in flight; requests meanwhile fold into its next pass ---
    {
        TT::JobBook book;
        CHECK(!book.Running() && !book.Pending().Any(), "jobs: a new book is idle and empty");
        CHECK(book.RequestPush(), "jobs: the first request claims a worker (the caller must start one)");
        CHECK(book.Running(), "jobs: the book is running once claimed");
        CHECK(!book.RequestAnswer(PixelSize{ 200, 108 }), "jobs: a request while a worker runs starts no second worker");
        CHECK(!book.RequestAnswer(PixelSize{ 300, 162 }), "jobs: ...nor does a third");
        const TT::WorkOrder first = book.Take();
        CHECK(first.push && first.answer && !first.peek, "jobs: one pass takes everything pending (the refresh AND the answer)");
        CHECK(first.answerBox.width == 300 && first.answerBox.height == 162, "jobs: the NEWEST request's box wins (DWM's latest maximum)");
        CHECK(!book.Pending().Any() && book.Running(), "jobs: taking empties the book; the worker still owns it");
        CHECK(!book.RequestPeek(), "jobs: a request during the pass is left for the running worker");
        const TT::WorkOrder second = book.Take();
        CHECK(second.peek && !second.push && !second.answer, "jobs: the worker's next pass takes the late request");
        const TT::WorkOrder last = book.Take();
        CHECK(!last.Any() && !book.Running(), "jobs: an empty take stops the worker and marks the book idle");
        CHECK(book.RequestPeek(), "jobs: the next request claims a NEW worker");
        book.Abandon();
        CHECK(!book.Running() && book.Pending().peek, "jobs: abandoning a worker that could not start keeps the pending work");
        CHECK(book.RequestPush(), "jobs: ...so the next request tries the start again");
        CHECK(book.Pending().push && book.Pending().peek, "jobs: ...with the earlier work still pending");
        book.Clear();
        CHECK(!book.Pending().Any() && book.Running(), "jobs: Clear (the frame turned off) drops the work; the running worker...");
        CHECK(!book.Take().Any() && !book.Running(), "jobs: ...finds nothing at its next take and stops");
    }

    // --- DrawIconCentered: the app icon on the placeholder card (real GDI) ---
    {
        const HICON appIcon = ::LoadIconW(nullptr, IDI_APPLICATION); // a shared system icon: never destroyed
        CHECK(appIcon != nullptr, "icon: the system application icon loads");
        const PixelSize size{ 64, 48 };
        const std::vector<uint8_t> blankCard = TT::SolidCard(size, TT::kCardFillRgb);
        std::vector<uint8_t> card = blankCard;
        CHECK(TT::DrawIconCentered(card, size, appIcon, 32), "icon: drawn on a 64x48 card");
        int changedInside = 0;
        int changedOutside = 0;
        bool opaque = true;
        for (int y = 0; y < size.height; ++y)
        {
            for (int x = 0; x < size.width; ++x)
            {
                const bool inside = x >= 16 && x < 48 && y >= 8 && y < 40;
                const bool changed = std::memcmp(ThumbPx(card, size.width, x, y), ThumbPx(blankCard, size.width, x, y), 3) != 0;
                if (inside)
                {
                    changedInside += changed ? 1 : 0;
                }
                else
                {
                    changedOutside += changed ? 1 : 0;
                }
                opaque = opaque && ThumbPx(card, size.width, x, y)[3] == 0xFF;
            }
        }
        CHECK(changedInside > 0, "icon: pixels change inside the centered 32x32 square");
        CHECK(changedOutside == 0, "icon: nothing outside the centered square changes");
        CHECK(opaque, "icon: the card stays fully opaque (GDI's zeroed alpha is restored)");

        const PixelSize strip{ 10, 6 };
        const std::vector<uint8_t> blankStrip = TT::SolidCard(strip, TT::kCardFillRgb);
        std::vector<uint8_t> stripCard = blankStrip;
        CHECK(TT::DrawIconCentered(stripCard, strip, appIcon, 32), "icon: an edge larger than the image is clamped to its shorter side");
        bool marginsKept = true;
        for (int y = 0; y < strip.height; ++y)
        {
            for (const int x : { 0, 1, 8, 9 })
            {
                marginsKept = marginsKept && std::memcmp(ThumbPx(stripCard, strip.width, x, y), ThumbPx(blankStrip, strip.width, x, y), 4) == 0;
            }
        }
        CHECK(marginsKept, "icon: the clamped 6x6 icon stays centered (columns 0-1 and 8-9 untouched)");

        std::vector<uint8_t> keep = blankCard;
        CHECK(!TT::DrawIconCentered(keep, size, nullptr, 32), "icon: a null icon draws nothing");
        CHECK(!TT::DrawIconCentered(keep, size, appIcon, 0), "icon: a zero edge draws nothing");
        CHECK(!TT::DrawIconCentered(keep, PixelSize{ 65, 48 }, appIcon, 32), "icon: a buffer shorter than the size says draws nothing");
        CHECK(keep == blankCard, "icon: a refused draw leaves the card as it was");
    }

    // --- CreateTopDownDib: the bitmap handed to DwmSetIconicThumbnail ---
    {
        const std::vector<uint8_t> px{ 1, 2, 3, 0xFF, 4, 5, 6, 0xFF, 7, 8, 9, 0xFF, 10, 11, 12, 0xFF, 13, 14, 15, 0xFF, 16, 17, 18, 0xFF };
        const HBITMAP dib = TT::CreateTopDownDib(px, PixelSize{ 3, 2 });
        CHECK(dib != nullptr, "dib: a 3x2 image becomes a DIB section");
        if (dib)
        {
            DIBSECTION ds{};
            const int got = ::GetObjectW(dib, sizeof(ds), &ds);
            const bool described = got == static_cast<int>(sizeof(ds));
            CHECK(described && ds.dsBm.bmWidth == 3 && ds.dsBm.bmHeight == 2 && ds.dsBm.bmBitsPixel == 32, "dib: 3x2, 32 bits per pixel");
            CHECK(described && ds.dsBm.bmBits && std::memcmp(ds.dsBm.bmBits, px.data(), px.size()) == 0, "dib: the pixel memory holds the bytes as given");
            const HDC dc = ::CreateCompatibleDC(nullptr);
            const HGDIOBJ old = dc ? ::SelectObject(dc, dib) : nullptr;
            CHECK(old && ::GetPixel(dc, 0, 0) == RGB(3, 2, 1) && ::GetPixel(dc, 2, 1) == RGB(18, 17, 16), "dib: TOP-DOWN -- the first bytes are the top-left pixel, the last bytes the bottom-right");
            if (old)
            {
                ::SelectObject(dc, old);
            }
            if (dc)
            {
                ::DeleteDC(dc);
            }
            ::DeleteObject(dib);
        }
        CHECK(TT::CreateTopDownDib(px, PixelSize{ 0, 2 }) == nullptr, "dib: an empty size => null");
        CHECK(TT::CreateTopDownDib(px, PixelSize{ 3, 3 }) == nullptr, "dib: a buffer shorter than the size says => null");
        CHECK(TT::CreateTopDownDib(std::vector<uint8_t>(16385 * 4), PixelSize{ 16385, 1 }) == nullptr, "dib: an edge over 16384 => null");
    }

    // --- CaptureClient: the refusals (a real capture needs a shown window -- the probe measured that part) ---
    {
        std::vector<uint8_t> bgra(12, 7);
        PixelSize size{ 5, 5 };
        CHECK(!TT::CaptureClient(nullptr, bgra, size) && bgra.empty() && size.Empty(), "capture: a null window fails and clears both outputs");
        const HWND hidden = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        CHECK(hidden != nullptr, "capture: a hidden 0x0 test window is created (never shown)");
        if (hidden)
        {
            CHECK(!TT::CaptureClient(hidden, bgra, size) && size.Empty(), "capture: an empty client area (what a minimized window reports) fails");
            ::DestroyWindow(hidden);
            CHECK(!TT::CaptureClient(hidden, bgra, size) && bgra.empty(), "capture: a destroyed window fails");
        }
    }

    // --- one whole pass on the probe's case: capture -> fit -> downscale -> frame -> DIB ---
    {
        const PixelSize client{ 704, 381 };
        const std::vector<uint8_t> capture = ThumbFilled(client, 0x0C, 0x0C, 0x0C, 0xFF);
        CHECK(!TT::LooksBlank(capture, client), "pass: a terminal-dark capture is content");
        const PixelSize thumb = TT::FitInside(client, PixelSize{ 200, 108 });
        std::vector<uint8_t> preview = TT::DownscaleBgra(capture, client, thumb);
        const int thickness = TT::FrameThickness(thumb);
        TT::DrawFrame(preview, thumb, thickness, TT::FrameColor(0xCCFF0000u));
        CHECK(thickness == 4 && preview.size() == static_cast<size_t>(199) * 108 * 4, "pass: a 199x108 preview with a 4 px frame");
        CHECK(ThumbPxIs(preview, thumb.width, 0, 0, 2, 2, 206, 0xFF), "pass: the corner is 80% red over the dark capture (2, 2, 206)");
        CHECK(ThumbPxIs(preview, thumb.width, 99, 54, 0x0C, 0x0C, 0x0C, 0xFF), "pass: the middle still shows the window");
        const HBITMAP dib = TT::CreateTopDownDib(preview, thumb);
        CHECK(dib != nullptr, "pass: the framed preview becomes the DIB DWM takes");
        if (dib)
        {
            ::DeleteObject(dib);
        }
    }

    CHECK(TT::kReuseCaptureMs < TT::kRefreshPeriodMs && TT::kSettleDelayMs < TT::kRefreshPeriodMs, "timing: the capture reuse window and the resize debounce are both shorter than the refresh period");
}
