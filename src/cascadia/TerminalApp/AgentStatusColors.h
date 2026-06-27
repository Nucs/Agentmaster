// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ONE session-state -> status color palette, shared by every surface that
// renders a state dot: the per-tab link-badge overlay (AgentTabOverlay) and the tab strip's
// header status dot (TabHeaderControl's "[icon] ● <title>" element, driven through
// TerminalTabStatus). AgentTabOverlay.cpp used to carry a hand-synced copy of
// AgentManagerContent's StateColor ("kept in sync by hand — a small duplication until the
// palette is factored into a shared header" — this is that header); the third consumer (the
// tab dot) was the cue to factor it. AgentManagerContent.cpp still holds its own identical
// copy for now (a concurrently-edited file; converge it on a quiet day) — if you change a
// color HERE, change it THERE.
//
// WinRT types (winrt::Windows::UI::Color), so this lives in TerminalApp, NOT the plain-C++
// AgentMaster/ engine directory.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "AgentMaster/SessionModels.h" // ::Agentmaster::SessionState

namespace winrt::TerminalApp::implementation
{
    // Color-matched across the Triage Board cards/columns, the overlay badge, and the
    // tab-strip dot, so the same session reads the same color on every surface.
    inline winrt::Windows::UI::Color AgentStatusColorFor(::Agentmaster::SessionState s)
    {
        using winrt::Windows::UI::Colors;
        switch (s)
        {
        case ::Agentmaster::SessionState::Running:
            return Colors::DodgerBlue();
        case ::Agentmaster::SessionState::WaitingForInput:
            return Colors::Goldenrod();
        case ::Agentmaster::SessionState::NeedsApproval:
            return Colors::OrangeRed();
        case ::Agentmaster::SessionState::Error:
            return Colors::Crimson();
        case ::Agentmaster::SessionState::Done:
            return Colors::MediumSeaGreen();
        case ::Agentmaster::SessionState::Idle:
        default:
            return Colors::Gray();
        }
    }

    // Agentmaster (status-dot flash-ring color): parse an "#AARRGGBB" hex string — the leading byte is
    // the ALPHA (opacity) — into a Color, returning `fallback` on anything malformed. A legacy
    // "#RRGGBB" (7 chars, no alpha) reads as fully opaque (A=0xFF). This is the ONE parser shared by
    // the Settings cog (seed the color picker from AppSettings::flashRingColor) and TerminalPage
    // (build the per-window flash-ring brush), so the on-disk string and the rendered ring never drift.
    inline winrt::Windows::UI::Color ParseArgbHexColor(std::wstring_view hex, winrt::Windows::UI::Color fallback)
    {
        if ((hex.size() != 9 && hex.size() != 7) || hex[0] != L'#')
        {
            return fallback;
        }
        const auto nib = [](wchar_t c) -> int {
            if (c >= L'0' && c <= L'9')
            {
                return c - L'0';
            }
            if (c >= L'a' && c <= L'f')
            {
                return 10 + (c - L'a');
            }
            if (c >= L'A' && c <= L'F')
            {
                return 10 + (c - L'A');
            }
            return -1;
        };
        const size_t n = hex.size() - 1; // count of hex digits (6 or 8)
        int v[8];
        for (size_t i = 0; i < n; ++i)
        {
            v[i] = nib(hex[1 + i]);
            if (v[i] < 0)
            {
                return fallback;
            }
        }
        const bool hasAlpha = (hex.size() == 9);
        size_t idx = 0;
        const uint8_t a = hasAlpha ? static_cast<uint8_t>(v[idx] * 16 + v[idx + 1]) : static_cast<uint8_t>(0xFF);
        if (hasAlpha)
        {
            idx += 2;
        }
        const uint8_t r = static_cast<uint8_t>(v[idx] * 16 + v[idx + 1]);
        const uint8_t g = static_cast<uint8_t>(v[idx + 2] * 16 + v[idx + 3]);
        const uint8_t b = static_cast<uint8_t>(v[idx + 4] * 16 + v[idx + 5]);
        return winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b);
    }

    // Format a Color as the uppercase "#AARRGGBB" the flash-ring setting stores (opacity in the alpha
    // byte). The inverse of ParseArgbHexColor; the color picker's chosen Color goes through here on Save.
    inline std::wstring FormatArgbHexColor(winrt::Windows::UI::Color c)
    {
        constexpr wchar_t d[] = L"0123456789ABCDEF";
        const uint8_t bytes[4] = { c.A, c.R, c.G, c.B };
        std::wstring out;
        out.reserve(9);
        out.push_back(L'#');
        for (const uint8_t by : bytes)
        {
            out.push_back(d[(by >> 4) & 0xF]);
            out.push_back(d[by & 0xF]);
        }
        return out;
    }
}
