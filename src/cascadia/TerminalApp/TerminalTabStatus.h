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
        // Agentmaster (Linked Lenses): the "selected/active" pill behind the header (see the idl
        // note) — driven by TerminalPage::_SetTabSelectionPill via Tab.TabStatus().
        WINRT_OBSERVABLE_PROPERTY(bool, AgentSelectionVisible, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, AgentSelectionBrush, PropertyChanged.raise, nullptr);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalTabStatus);
}
