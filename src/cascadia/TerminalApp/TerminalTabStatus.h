// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TerminalTabStatus.g.h"

namespace winrt::TerminalApp::implementation
{
    struct TerminalTabStatus : TerminalTabStatusT<TerminalTabStatus>
    {
        TerminalTabStatus() = default;

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(bool, IsConnectionClosed, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsPaneZoomed, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsProgressRingActive, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsProgressRingIndeterminate, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, BellIndicator, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsReadOnlyActive, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(uint32_t, ProgressValue, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsInputBroadcastActive, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Color, TabColorIndicator, PropertyChanged.raise);
        // Agentmaster: the tab-strip status dot (see the idl note) — driven by TerminalPage's
        // registry observer / bind paths via Tab.TabStatus(); rendered by TabHeaderControl.xaml.
        WINRT_OBSERVABLE_PROPERTY(bool, AgentStatusVisible, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, AgentStatusBrush, PropertyChanged.raise, nullptr);
        // Agentmaster (status-dot RED FLASH RING): a separate red ring (an ellipse behind the dot,
        // peeking out around its constant black outline) that the TerminalPage shared timer blinks
        // on/off when a hosted session goes from Running to a resting state (Idle / WaitingForInput /
        // NeedsApproval) on an unvisited tab. The dot keeps its black outline + status fill; only this
        // ring flashes.
        WINRT_OBSERVABLE_PROPERTY(bool, AgentFlashRingVisible, PropertyChanged.raise);
        // Agentmaster (Linked Lenses): the "selected/active" pill behind the header (see the idl
        // note) — driven by TerminalPage::_SetTabSelectionPill via Tab.TabStatus().
        WINRT_OBSERVABLE_PROPERTY(bool, AgentSelectionVisible, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, AgentSelectionBrush, PropertyChanged.raise, nullptr);
        // Agentmaster (FAVORITES.md): the FAVORITE crown over the status dot (see the idl note) —
        // driven by TerminalPage::_SetTabAgentFavorite via Tab.TabStatus(); rendered by
        // TabHeaderControl.xaml as a small gold crown at the dot's north-west.
        WINRT_OBSERVABLE_PROPERTY(bool, AgentFavoriteVisible, PropertyChanged.raise);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalTabStatus);
}
