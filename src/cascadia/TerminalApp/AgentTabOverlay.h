// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster: the per-tab "link badge" overlay (see doc/agentmaster/TAB_OVERLAY.md). A small,
// read-only HUD pinned to the top-right of a Claude session's terminal, summarizing the tab <->
// Agentmaster link at a glance. Row 1: hook-driven status (color-matched to the Triage Board),
// Autopilot mode (Off/Semi/Full), queued (Pending) count, and link state (⛓ linked vs observe-only).
// Row 2 (dim/secondary): "<root workdir folder>/<branch>" (e.g. myworkdir/feature/issue123) so the
// session's place + branch read at a glance; hidden when there's no dir/branch (and on observe badges).
// Row 3 (hover-only actions): a folder button (Open Path — the working dir via explorer.exe) + a copy
// button whose menu copies the Session Id / Copy Path (working dir) / Copy Branch Name / Claude Launch
// CLI / Codex Launch CLI (each the REAL full command — the live process commandline with hooks, or the
// builder Agentmaster would use) / Transcript (the whole conversation, user + assistant TEXT only).
// A completed copy (and Open Path) plays a short confirmation chime. Collapsed at rest; revealed while
// the pointer is over the badge OR the copy menu is open (so the menu doesn't vanish under the
// pointer). Built only for a LINKED session (Initialize), never an observe badge.
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
        void _WireHover(); // attach the pointer-over expand handlers (idempotent; weak-captured)
        void _BuildActionsRow(); // lazily build row 3 (folder + copy menu) for a LINKED session
        void _SetExpanded(bool on); // dim<->bright + show/hide row 3 (driven by hover OR pinned)
        void _OpenFolder(); // row 3 folder button: open the session's working dir in Explorer (off-thread)
        void _CopyField(int which); // row 3 copy menu: 0=Session Id 1=Claude CLI 2=Codex CLI 3=Transcript path

        std::wstring _sessionId;
        bool _pending{ false }; // registry-less "observe" badge (a shell / unresolved claude — no linked session)
        bool _hovering{ false }; // pointer is currently over the badge
        bool _pinned{ false }; // the copy menu is open — keep expanded even after the pointer leaves
        bool _hoverWired{ false }; // _WireHover ran once (Initialize / first ShowActivity)
        std::wstring _lastActivitySig; // last kind rendered by ShowActivity (skip redundant re-renders)
        std::shared_ptr<::Agentmaster::SessionRegistry> _registry;
        uint64_t _observerToken{ 0 }; // ::Agentmaster::ObserverToken (uint64_t; avoid the header here)
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr };

        winrt::Windows::UI::Xaml::Controls::Border _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _stack{ nullptr }; // vertical: row 1 / row 2 / row 3
        winrt::Windows::UI::Xaml::Controls::TextBlock _line{ nullptr }; // row 1: status · model · mode · queue · link
        winrt::Windows::UI::Xaml::Controls::TextBlock _subline{ nullptr }; // row 2: "<root workdir folder>/<branch>"
        winrt::Windows::UI::Xaml::Controls::StackPanel _row3{ nullptr }; // row 3: folder + copy buttons (hover-only)
    };
}
