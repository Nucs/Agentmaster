// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: the TASKBAR UNREAD BADGE (doc/agentmaster/NOTIFICATIONS.md section 8).
// Two halves: TaskbarBadge.h, the renderer that turns a count into the red-disc overlay icon (the pure label /
// description / size / composition helpers, then the real GDI glyph pass and CreateBadgeIcon, verified by
// DRAWING the icon the way the shell does), and the engine's process-wide unread census (Engine.h), driven on a
// LOCAL Engine through the `...In` seams — never SharedEngine(), whose first access would start the bridge.
// Shared CHECK/fixtures/decls live in m5_tests.h.
#include "m5_tests.h"

#include "../Engine.h"
#include "../TaskbarBadge.h"

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
