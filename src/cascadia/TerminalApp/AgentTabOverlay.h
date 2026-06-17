// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster: the per-tab "link badge" overlay (see doc/agentmaster/TAB_OVERLAY.md). A small,
// read-only HUD pinned to the top-right of a Claude session's terminal, summarizing the tab <->
// Agentmaster link at a glance. Row 1: hook-driven status (color-matched to the Triage Board),
// Autopilot mode (Off/Semi/Full), queued (Pending) count, and link state (⛓ linked vs observe-only).
// Row 2 (dim/secondary): "<root workdir folder>/<branch>" (e.g. myworkdir/feature/issue123) so the
// session's place + branch read at a glance; hidden when there's no dir/branch (and on observe badges).
//
// Built imperatively (no IDL/XAML markup), like AgentManagerContent. It is NOT an IPaneContent —
// it just produces a FrameworkElement the app installs into the pane's overlay slot
// (TerminalPaneContent::SetAgentOverlay). Ref-counted (winrt::implements) so the page can own it
// and the registry observer can get_weak() it; it detaches its observer on teardown (Rule #10).

#pragma once

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.System.h>

#include <memory>
#include <string>

namespace Agentmaster
{
    class SessionRegistry;
}

namespace winrt::TerminalApp::implementation
{
    class AgentTabOverlay : public winrt::implements<AgentTabOverlay, winrt::Windows::Foundation::IInspectable>
    {
    public:
        AgentTabOverlay();
        ~AgentTabOverlay();

        // Wire to the shared registry + this session's id, render once, and start observing
        // (id-filtered, marshaled to the UI thread). Call on the UI thread.
        void Initialize(const std::wstring& sessionId, std::shared_ptr<::Agentmaster::SessionRegistry> registry);

        // Agentmaster (OBSERVER.md §4/§11d): render a registry-LESS "observe" badge for a tab the
        // Fleet Observer classified but that is NOT a linked Claude session — a shell (kind "pwsh" /
        // "cmd"), a never-prompted claude ("claude", no transcript id yet), or codex. Shows
        // "○ <kind> · unlinked" (gray); no registry observer (there is no session to track). Idempotent
        // by `kind` (a re-render is skipped when unchanged). The real Initialize-bound overlay replaces
        // it once a claude resolves a conversation id (its first prompt).
        void ShowActivity(const std::wstring& kind);

        // The FrameworkElement to install into the pane's overlay slot.
        winrt::Windows::UI::Xaml::FrameworkElement Root() const { return _root; }

    private:
        void _Refresh(); // rebuild the line from the registry snapshot (UI thread)
        void _Detach(); // drop the registry observer

        std::wstring _sessionId;
        bool _pending{ false }; // registry-less "observe" badge (a shell / unresolved claude — no linked session)
        std::wstring _lastActivitySig; // last kind rendered by ShowActivity (skip redundant re-renders)
        std::shared_ptr<::Agentmaster::SessionRegistry> _registry;
        uint64_t _observerToken{ 0 }; // ::Agentmaster::ObserverToken (uint64_t; avoid the header here)
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr };

        winrt::Windows::UI::Xaml::Controls::Border _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _line{ nullptr }; // row 1: status · model · mode · queue · link
        winrt::Windows::UI::Xaml::Controls::TextBlock _subline{ nullptr }; // row 2: "<root workdir folder>/<branch>"
    };
}
