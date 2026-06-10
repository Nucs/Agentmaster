// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
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
}
