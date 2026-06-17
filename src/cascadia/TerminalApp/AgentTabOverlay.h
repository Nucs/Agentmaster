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
// builder Agentmaster would use) / Summary (the FULL textual session box — everything, even what the
// displayed panel trims) / Transcript (the whole conversation, user + assistant TEXT only).
// A completed copy (and Open Path) plays a short confirmation chime. Collapsed at rest; revealed while
// the pointer is over the badge OR the copy menu is open (so the menu doesn't vanish under the
// pointer). Built only for a LINKED session (Initialize), never an observe badge. Row 3 also carries a
// PENCIL button that toggles the SUMMARY PANEL — a SECOND overlay (its own slot, below the badge, max
// 20% of the pane) that renders a session-end.js-style box analyzed off-thread from the transcript via
// ProcessInspect::AnalyzeSessionTranscript. The DISPLAYED panel is a TRIMMED view (Parent/Plan/Duration/
// Tasks/Messages/Files Read/Files Edited) — it omits everything panel 1 (the badge) already shows
// (state, model·effort, branch, dir/folder); the copy menu's "Summary" yields the COMPLETE box (id +
// resume CLI + dir + folder + branch + state header included). The toggle is a GLOBAL setting
// (AppSettings::showSummaryPanel) — shared across windows + persisted; the pencil on any tab flips every tab's panel.
//
// Built imperatively (no IDL/XAML markup), like AgentManagerContent. It is NOT an IPaneContent —
// it just produces a FrameworkElement the app installs into the pane's overlay slot
// (TerminalPaneContent::SetAgentOverlay). Ref-counted (winrt::implements) so the page can own it
// and the registry observer can get_weak() it; it detaches its observer on teardown (Rule #10).

#pragma once

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.System.h>

#include <functional>
#include <memory>
#include <string>

namespace Agentmaster
{
    class SessionRegistry;
    struct SessionInfo; // _UpdateSummary takes a const& (full type in the .cpp via SessionRegistry.h)
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

        // Agentmaster (TAB_OVERLAY.md summary panel): the FrameworkElement for the SECOND slot (below
        // the badge), installed via TerminalPaneContent::SetAgentSummaryOverlay. Built by Initialize;
        // shown only while the GLOBAL toggle (AppSettings::showSummaryPanel) is ON.
        winrt::Windows::UI::Xaml::FrameworkElement SummaryRoot() const { return _summaryRoot; }

        // Agentmaster (TAB_OVERLAY.md summary panel): the summary panel's visibility is a GLOBAL setting
        // (AppSettings::showSummaryPanel), not per-session. The page mirrors it in here — on attach (seed)
        // and on every pencil toggle (broadcast to every linked overlay in the window). Shows/hides +
        // (re)loads the panel. Call on the UI thread.
        void SetSummaryEnabled(bool on);
        // The pencil button flips the GLOBAL setting; the overlay can't reach AppSettings, so it calls
        // this handler (wired by the page) to do the freshest-disk read-modify-write + the live
        // broadcast. Set by _AttachClaudeOverlay.
        void SetSummaryToggleHandler(std::function<void()> handler);

    private:
        void _Refresh(); // rebuild the line from the registry snapshot (UI thread)
        void _Detach(); // drop the registry observer
        void _WireHover(); // attach the pointer-over expand handlers (idempotent; weak-captured)
        void _BuildActionsRow(); // lazily build row 3 (folder + copy menu) for a LINKED session
        void _SetExpanded(bool on); // dim<->bright + show/hide row 3 (driven by hover OR pinned)
        void _OpenFolder(); // row 3 folder button: open the session's working dir in Explorer (off-thread)
        void _CopyField(int which); // row 3 copy menu: 0=Session Id 1=Copy Path 2=Copy Branch 3=Claude CLI 4=Codex CLI 5=Transcript 6=Summary (full textual box)
        void _BuildSummaryPanel(); // build the summary panel element (the 2nd slot), collapsed
        void _SetSummaryContent(const std::wstring& text); // fill the panel StackPanel: text runs -> TextBlocks, separator sentinels -> full-width Border rules
        void _UpdateTimesLine(); // re-render the live "age / last user msg / last activity" ago line (DispatcherTimer-driven)
        void _ApplySummaryVisibility(); // show the 2nd pane only when enabled AND non-empty (content rows or a times line); else collapse it
        void _ToggleSummary(); // pencil button: invoke the page handler (flips the GLOBAL showSummaryPanel)
        void _UpdateSummary(const ::Agentmaster::SessionInfo& s); // _Refresh-driven: show/hide (per _summaryEnabled) + (re)load when grown
        winrt::fire_and_forget _LoadSummaryAsync(std::wstring transcriptPath, bool codex, std::wstring sessionId, std::wstring cwd, std::wstring liveGlyph, std::wstring liveLabel, int64_t mtime); // analyze + render off-thread, set text on the UI thread

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

        // Summary panel (the 2nd slot): a scrollable box, shown while the global showSummaryPanel is ON.
        // The body is a StackPanel (not one TextBlock) so separators can be full-width Border rules.
        winrt::Windows::UI::Xaml::Controls::Border _summaryRoot{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _summaryTimesText{ nullptr }; // pinned top: the live "ago" times line
        winrt::Windows::UI::Xaml::Controls::StackPanel _summaryStack{ nullptr };
        winrt::Windows::UI::Xaml::DispatcherTimer _summaryTimer{ nullptr }; // drives the live times line; self-stops when the overlay is gone
        std::wstring _summaryPath; // cached resolved transcript path (resolve once)
        int64_t _summaryMtime{ 0 }; // last-loaded transcript mtime — reload only when it grows
        int64_t _summaryCreatedMs{ 0 }; // times line: conversation start (unix ms) — "age"
        int64_t _summaryLastUserMs{ 0 }; // times line: last real user prompt (unix ms) — "last user msg"
        int64_t _summaryLastActivityMs{ 0 }; // times line: last transcript entry (unix ms) — "last activity"
        bool _summaryLoading{ false }; // one analyze+render in flight at a time
        bool _summaryEnabled{ false }; // mirror of the GLOBAL AppSettings::showSummaryPanel (page-driven)
        std::function<void()> _onToggleSummary; // pencil -> page (flip the global setting + broadcast)
    };
}
