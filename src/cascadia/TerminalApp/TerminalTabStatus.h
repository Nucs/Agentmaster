// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

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
        // Agentmaster (eager-init / "Activate Tab"): the DORMANT half-hollow dot. A window-restored /
        // re-homed managed tab whose claude hasn't STARTED yet (ConnectionState == NotConnected) shows a
        // HALF circle — one half filled with AgentStatusBrush (the current state colour), one half hollow
        // (just the outline) — to read as "not initialized; Activate to wake it". MUTUALLY EXCLUSIVE with
        // AgentStatusVisible (the full dot): _SetTabAgentDot drives exactly one true. Shares AgentStatusBrush.
        WINRT_OBSERVABLE_PROPERTY(bool, AgentStatusHalfVisible, PropertyChanged.raise);
        // Agentmaster (status-dot RED FLASH RING): a separate ring (an ellipse behind the dot,
        // peeking out around its constant black outline) that the TerminalPage shared timer blinks
        // on/off when a hosted session goes from Running to a resting state (Idle / WaitingForInput /
        // NeedsApproval) on an unvisited tab. The dot keeps its black outline + status fill; only this
        // ring flashes.
        WINRT_OBSERVABLE_PROPERTY(bool, AgentFlashRingVisible, PropertyChanged.raise);
        // Agentmaster (status-dot flash-ring COLOR): the brush the ring is painted with — the
        // user-configurable "status flashing color" (Settings cog -> AppSettings::flashRingColor),
        // GLOBAL across windows, its alpha channel == the ring's opacity. TerminalPage points this at a
        // per-window shared SolidColorBrush via _SetTabFlashRing; null until the tab first flashes (the
        // ring is collapsed until then). Replaces the previously hardcoded Fill="Red".
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, AgentFlashRingBrush, PropertyChanged.raise, nullptr);
        // Agentmaster (Linked Lenses): the "selected/active" pill behind the header (see the idl
        // note) — driven by TerminalPage::_SetTabSelectionPill via Tab.TabStatus().
        WINRT_OBSERVABLE_PROPERTY(bool, AgentSelectionVisible, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, AgentSelectionBrush, PropertyChanged.raise, nullptr);
        // Agentmaster (FAVORITES.md §5a): the FAVORITE marker over the status dot (see the idl note),
        // driven by TerminalPage::_SetTabAgentFavorite via Tab.TabStatus() from the GLOBAL
        // AppSettings::favoriteIcon. The two are mutually exclusive — at most one is true at a time:
        //  * AgentFavoriteVisible    => the CROWN (a small gold crown at the dot's north-west) — the default.
        //  * AgentFavoriteStarVisible => the STAR (a white, golden-tipped star drawn BEHIND the dot, so the
        //                                status dot reads as its foreground).
        WINRT_OBSERVABLE_PROPERTY(bool, AgentFavoriteVisible, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, AgentFavoriteStarVisible, PropertyChanged.raise);
        // Agentmaster (PENDING_INPUT.md): the UNSENT-DRAFT "3 dots" indicator below the status dot (see
        // the idl note) — driven by TerminalPage::_SetTabPending from the debounced SessionInfo::pendingInput.
        WINRT_OBSERVABLE_PROPERTY(bool, AgentPendingVisible, PropertyChanged.raise);
        // Agentmaster (PENDING_INPUT.md): the brush the "3 dots" are painted with — the user-configurable
        // pending-dots color, contrast-picked from the tab's per-dir color (AppSettings::pendingDots*),
        // pointed here by TerminalPage::_SetTabPending just before AgentPendingVisible flips true; null
        // until the tab first shows a draft (the dots are collapsed until then). Replaces Fill="#E0A92B".
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, AgentPendingBrush, PropertyChanged.raise, nullptr);
        // Agentmaster (bookmark tags): the '\n'-joined tag-name spec behind the tab-header BOOKMARK
        // badges (see the idl note) — written by TerminalPage::_SetTabAgentTags, consumed by
        // TabHeaderControl's code-behind (_UpdateTagBadges) via the same PropertyChanged pipeline as
        // the pending-dots animation. "" (the default) == no tags, no badges.
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, AgentTagsSpec, PropertyChanged.raise);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalTabStatus);
}
