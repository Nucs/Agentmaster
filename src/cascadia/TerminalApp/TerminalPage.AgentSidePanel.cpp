// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster SIDE PANEL — a WINDOW-CHROME fleet panel beside the terminal content.
// Part of the TerminalPage same-class partial group (the TabManagement.cpp pattern).
//
// WHAT: a non-terminal, non-tab pane docked LEFT or RIGHT of the tab content (ContentArea's
// 3-column grid in TerminalPage.xaml), split into two vertically-stacked areas with independent
// scrolling:
//   TOP    — the LIVE session CARDS (the Triage-Board integration re-hosted vertically): a search
//            box (the Sessions browser's ParseSessionQuery grammar over title / dir / branch / id)
//            + an order toggle (the SHARED AppSettings::boardSort, cycled through
//            AgentManagerContent::CycleBoardSort so the board and this list can never sort by
//            different specs) over a scrollable card list. A card single-click SELECTS (Linked
//            Lenses: it also drives the Manager lens), double-click ACTIVATES (jump to the tab,
//            cross-window), right-click shows THE session context menu — the exact lazily-filled
//            MenuFlyout the board card / tree row shows (AgentManagerContent::BuildSessionMenuFor).
//   BOTTOM — the SELECTED session's summary: a REAL AgentTabOverlay in fill-host mode
//            (SetSummaryFillHost), i.e. the per-tab pencil panel's whole business logic — analyze/
//            render off-thread, times bar, wrap/truncate/previous toggles, JUMP buttons, context
//            menus — reused verbatim, just sized by this host instead of anchoring over a terminal.
//
// PERSISTENCE (the user's contract): both separators — the VERTICAL one (panel width) and the
// HORIZONTAL one (cards/summary split) — persist PER WINDOW (WindowRecord::sidePanel*, riding the
// window-record autosave), and EVERY adjustment additionally RMWs the GLOBAL AppSettings defaults
// ("the last one to resize sets the default for new windows"). Collapse follows the same rule; the
// dock side (left/right) is GLOBAL-only.
//
// PERF DISCIPLINE (the Manager-lens rebuild-tax lesson): rebuilds are COALESCED off the page's
// already-batched registry drain, min-gap throttled, and deferred entirely while collapsed; the
// ~30s timing refresh rewrites the timing TextBlocks IN PLACE instead of rebuilding; cards carry
// NO eager flyouts (the context menu is built lazily at right-click); and every per-card resolve
// (color, dir) goes through the already-cached engine resolvers.
// ======================================================================================
//
// This file implements TerminalPage methods (same class, separate TU — the TabManagement.cpp
// pattern). It reuses AgentManagerContent.Internal.h (the board's ~48 anonymous-namespace helpers:
// StateColor/StateLabel/Text/Fill/TimingText/BuildPendingDots/MakeSortKey/SortKeyLess/
// PointerStillWithin/HexToColor/PreferDarkTextOn/ApplyCursor/...) so the cards speak the exact
// visual + sort language of the Triage Board. NOTE: that header requires the SAME file-scope
// using-directives the AgentManagerContent TUs replicate (narrow Color/ColorHelper/Colors
// using-DECLARATIONS — a `using namespace winrt::Windows::UI;` would pull the nested Text
// namespace over the Text() helper), which is why the using block below mirrors
// AgentManagerContent.Board.cpp rather than the other TerminalPage TUs.

#include "pch.h"
#include "TerminalPage.h"

#include "AgentManagerContent.h" // CycleBoardSort / BuildSessionMenuFor / SelectSession (the Linked-Lenses delegation targets)
#include "AgentTabOverlay.h" // the fill-hosted summary panel (SetSummaryFillHost)
#include "AgentCatchLog.h" // AgentLogCaughtException — swallowed-exception forensics (Rule #18)
#include "AgentTipHelpers.h" // AgentSetTip / AgentSetTitledTip — the islands-safe hover tooltips
#include "AgentStatusColors.h" // ParseArgbHexColor — the pending-dots color pair
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / LogNav / ShortId
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings + EffectiveWorkingDir / ResolveSessionColorHex
#include "AgentMaster/ProcessInspect.h" // ExternalClaudeRow (the Internal.h MakeSortKey overload's parameter type)
#include "AgentMaster/ProfileBootstrap.h" // (Internal.h convention — harmless here, keeps the include set board-identical)
#include "AgentMaster/SessionRegistry.h" // SnapshotLive / Get
#include "AgentMaster/SessionSearch.h" // ParseSessionQuery / MatchesQueryText / MatchesPathQuery — the Sessions grammar, reused

#include <algorithm>
#include <chrono> // kCardTipDelay (the Internal.h card-tip hold-back) + the tip helpers' delay overrides
#include <cmath>
#include <cwctype> // towlower — the fold for the in-process matchers
#include <shobjidl.h> // IFileOpenDialog — required BY AgentManagerContent.Internal.h's PickClaudeExe helper (per-TU anonymous-namespace copy)

using namespace winrt::Windows::Foundation;
// Using-DECLARATIONS (not a directive) for the color helpers: a `using namespace
// winrt::Windows::UI;` would also pull the nested `Text` namespace into scope and collide
// with the Internal.h Text() TextBlock helper (the AgentManagerContent gotcha).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Colors;
using winrt::Windows::UI::Core::CoreCursor;
using winrt::Windows::UI::Core::CoreCursorType;
using winrt::Windows::UI::Core::CoreWindow;
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input; // PointerRoutedEventArgs / TappedRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue
using namespace winrt::Microsoft::Terminal::Settings::Model; // (Board.cpp parity — Internal.h is written against this using set)
using namespace ::Agentmaster;
// The shared tooltip recipe (AgentTipHelpers.h) — using-DECLARATIONS so Internal.h's GLOBAL-scope
// anonymous-namespace helpers (e.g. TimingText) can call it unqualified, exactly as in Board.cpp.
using winrt::TerminalApp::implementation::AgentSetTip;
using winrt::TerminalApp::implementation::AgentSetTitledTip;
#include "AgentManagerContent.Internal.h" // the shared board helpers (anonymous namespace, per-TU copy)

namespace
{
    // --- Side panel geometry constants (one place, so the drag clamps, the load-time persistence
    // bands (Persistence.cpp), and the layout math can never disagree) --------------------------
    constexpr double kSidePanelSplitterPx = 6.0; // both separators' thickness (matches the overlay grips)
    constexpr double kSidePanelStripPx = 20.0; // the collapsed expander strip's column width
    constexpr double kSidePanelMinPx = 180.0; // the panel never renders thinner than this (fractions aside) — the header row stays usable
    constexpr double kSidePanelMinWidthFrac = 0.10; // drag clamp — mirrors the Persistence load band
    constexpr double kSidePanelMaxWidthFrac = 0.50; // never more than half the content area
    constexpr double kSidePanelMinSplitFrac = 0.15; // cards area never squeezed below 15% of the panel
    constexpr double kSidePanelMaxSplitFrac = 0.85; // summary area never squeezed below 15% either
    constexpr uint64_t kSidePanelRebuildMinGapMs = 500; // registry-drain rebuild throttle (a storm folds into the periodic catch-up)
    constexpr uint64_t kSidePanelTimingRefreshMs = 30'000; // the in-place "ago" text refresh cadence

    // The two drag-to-resize separators paint a SOLID gray a touch lighter than the #2E2E2E panel
    // background, so each reads as a visible raised divider at rest instead of a see-through gap: the
    // previous ~9%-alpha white overlay was effectively transparent (and vanished entirely against a
    // dark terminal on the VERTICAL bar's terminal-facing edge, since an alpha blend takes the color
    // of whatever it sits on). Opaque + a single gray level (r==g==b) makes the bar look identical no
    // matter what backs it; the HOT shade (pointer-over / mid-drag) is brighter to signal the grab.
    // One place, so both splitters' initial paint AND their hover swaps can never disagree.
    constexpr uint8_t kSidePanelSplitterIdleGray = 0x3E; // #3E3E3E — resting divider, subtly lighter than the panel
    constexpr uint8_t kSidePanelSplitterHotGray = 0x60; // #606060 — the pointer-over / drag highlight

    // Case-fold for the in-process matchers (MatchesQueryText expects PRE-FOLDED haystacks; the
    // parser pre-folds the needles into SearchTerm::textLower). Per-character towlower — the same
    // basis the search module uses, so a term that matches on the Sessions page matches here.
    std::wstring SidePanelFoldLower(std::wstring s)
    {
        for (auto& ch : s)
        {
            ch = static_cast<wchar_t>(::towlower(ch));
        }
        return s;
    }

    // The last path component of a directory (display leaf for the card's dir line). Trailing
    // separators are insignificant; both separator styles accepted (a cwd can carry either).
    std::wstring SidePanelLeafOf(const std::wstring& dir)
    {
        std::wstring d = dir;
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
        {
            d.pop_back();
        }
        const auto pos = d.find_last_of(L"\\/");
        return pos == std::wstring::npos ? d : d.substr(pos + 1);
    }

    // AND-match one live session against the parsed query terms — the Sessions browser's NAME-field
    // semantics scoped to what a live card can answer cheaply (title / effective dir / branch / id;
    // no transcript IO — this runs per session per keystroke). A bare whole-GUID term matches by
    // IDENTITY (the session's own id or its fork parent — SESSIONS.md: a pasted id finds the
    // session and its forks); every other term must hit at least one field (dir matched
    // slash-insensitively via MatchesPathQuery). Terms empty => everything matches (no filter).
    bool SidePanelSessionMatches(const ::Agentmaster::SessionInfo& s,
                                 const std::vector<::Agentmaster::SearchTerm>& terms,
                                 ::Agentmaster::TabColorMode mode)
    {
        if (terms.empty())
        {
            return true;
        }
        // Fold each field ONCE per session (not per term) — the AND loop below re-reads them.
        const std::wstring title = SidePanelFoldLower(s.title);
        const std::wstring dir = SidePanelFoldLower(::Agentmaster::EffectiveWorkingDir(mode, s));
        const std::wstring branch = SidePanelFoldLower(s.branch);
        const std::wstring id = SidePanelFoldLower(s.id);
        const std::wstring fork = SidePanelFoldLower(s.forkParentId);
        for (const auto& t : terms)
        {
            if (t.isGuid)
            {
                // Identity semantics: the whole GUID must EQUAL the session's id or its fork
                // parent (never a substring — a guid term is an identity claim, not text).
                if (id != t.textLower && fork != t.textLower)
                {
                    return false;
                }
                continue;
            }
            const bool hit = ::Agentmaster::MatchesQueryText(title, t.textLower, false) ||
                             ::Agentmaster::MatchesPathQuery(dir, t.textLower, false) ||
                             ::Agentmaster::MatchesQueryText(branch, t.textLower, false) ||
                             ::Agentmaster::MatchesQueryText(id, t.textLower, false);
            if (!hit)
            {
                return false; // AND: every term must land somewhere
            }
        }
        return true;
    }

    // Paint one card's 1px border for its selection/hover state: selected == the full state color,
    // hover == the state color at half alpha, rest == transparent. CONSTANT thickness on purpose —
    // the board's 0/1/2px scheme reflows the column on hover; a fixed-width side list must not
    // jiggle under the pointer.
    void SidePanelStyleCardBorder(const Border& card, const Color& stateColor, bool selected, bool hover)
    {
        if (!card)
        {
            return;
        }
        if (selected)
        {
            card.BorderBrush(SolidColorBrush{ stateColor });
        }
        else if (hover)
        {
            Color c = stateColor;
            c.A = 0x80;
            card.BorderBrush(SolidColorBrush{ c });
        }
        else
        {
            card.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x00, 0, 0, 0) });
        }
    }
}

namespace winrt::TerminalApp::implementation
{
    // Build the whole side panel into ContentArea. Called ONCE from _OnFirstLayout, AFTER
    // _InitAgentmasterEngine (the registry + the claimed WindowRecord exist) and after
    // _OpenAgentManagerTab (the AgentManagerContent the sort/menu/selection delegate to exists).
    // Seeds the per-window layout from the claimed record (per FIELD — a pre-feature record's unset
    // fields fall back to the AppSettings defaults, honoring "last one to resize seeds new
    // windows"), then docks, renders the first card list, and re-selects the lens's restored
    // session so the summary shows content immediately on reopen. A failure to build (no
    // ContentArea / no registry) leaves _sidePanelBuilt false and every other entry point a no-op.
    void TerminalPage::_InitAgentSidePanel()
    {
        if (_sidePanelBuilt)
        {
            return;
        }
        const auto area = ContentArea();
        if (!area || !_sessionRegistry)
        {
            return; // nothing to host in / no fleet to show — the panel simply doesn't exist this run
        }

        // --- Seed the per-window layout: record field if set, else the GLOBAL default -----------
        _sidePanelWidthFraction = (_windowRecord.sidePanelWidthFraction > 0.0) ? _windowRecord.sidePanelWidthFraction : _appSettings.sidePanelWidthFraction;
        _sidePanelSplitFraction = (_windowRecord.sidePanelSplitFraction > 0.0) ? _windowRecord.sidePanelSplitFraction : _appSettings.sidePanelSplitFraction;
        _sidePanelCollapsed = (_windowRecord.sidePanelCollapsed >= 0) ? (_windowRecord.sidePanelCollapsed != 0) : _appSettings.sidePanelCollapsed;
        // The card-list SCOPE is per-window ONLY: the record's flag (absent == false), never an
        // AppSettings default — the user's contract is that every window starts on ITS OWN tabs
        // (LOCAL), and a fleet-wide (GLOBAL) view is an explicit choice that stays with the window
        // that made it, so it can never leak into another window's list.
        _sidePanelGlobal = _windowRecord.sidePanelGlobal;
        // Belt the seeds into the drag bands: the loaders band-check too, but the AppSettings copy
        // could have been mutated in-process — a degenerate fraction must never reach layout.
        _sidePanelWidthFraction = std::clamp(_sidePanelWidthFraction, kSidePanelMinWidthFrac, kSidePanelMaxWidthFrac);
        _sidePanelSplitFraction = std::clamp(_sidePanelSplitFraction, kSidePanelMinSplitFrac, kSidePanelMaxSplitFrac);

        const auto weakThis = get_weak();

        // --- The VERTICAL separator (terminal <-> panel), its own grid column --------------------
        // The drag idiom is the summary-panel grip's: capture on press, island-relative deltas,
        // clamp per move, commit (persist) on release. PointerMoved applies layout SYNCHRONOUSLY —
        // legal, because pointer input is not a layout pass (the E_LAYOUTCYCLE trap is only
        // layout-driven triggers; those go through _SidePanelQueueLayout).
        _sidePanelSplitter = Border{};
        _sidePanelSplitter.Background(Fill(0xFF, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray)); // subtle but visible + fully hit-testable
        {
            const auto idle = Fill(0xFF, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray);
            const auto hot = Fill(0xFF, kSidePanelSplitterHotGray, kSidePanelSplitterHotGray, kSidePanelSplitterHotGray);
            _sidePanelSplitter.PointerEntered([weakThis, hot](const IInspectable& sender, const PointerRoutedEventArgs&) {
                ApplyCursor(CoreCursorType::SizeWestEast);
                if (const auto b = sender.try_as<Border>())
                {
                    b.Background(hot);
                }
            });
            _sidePanelSplitter.PointerExited([weakThis, idle](const IInspectable& sender, const PointerRoutedEventArgs&) {
                const auto self = weakThis.get();
                if (self && self->_sidePanelDragWidth)
                {
                    return; // mid-drag the pointer can leave the thin bar — keep it hot + the resize cursor
                }
                ApplyCursor(CoreCursorType::Arrow);
                if (const auto b = sender.try_as<Border>())
                {
                    b.Background(idle);
                }
            });
            _sidePanelSplitter.PointerPressed([weakThis](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                self->_sidePanelDragWidth = true;
                self->_sidePanelDragStart = e.GetCurrentPoint(nullptr).Position().X; // island-relative; only the delta matters
                // Start from the ACTUAL rendered panel width (the hosted grid fills its pixel
                // column), so the bar tracks the pointer 1:1 instead of jumping to the fraction.
                const double aw = self->_sidePanelHost ? self->_sidePanelHost.ActualWidth() : 0.0;
                self->_sidePanelDragStartVal = aw > 0.0 ? aw : kSidePanelMinPx;
                if (const auto el = sender.try_as<UIElement>())
                {
                    el.CapturePointer(e.Pointer());
                }
                ApplyCursor(CoreCursorType::SizeWestEast);
                e.Handled(true);
            });
            _sidePanelSplitter.PointerMoved([weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || !self->_sidePanelDragWidth)
                {
                    return;
                }
                const double dx = e.GetCurrentPoint(nullptr).Position().X - self->_sidePanelDragStart;
                // Docked RIGHT the panel grows as the pointer moves LEFT; docked LEFT it grows rightward.
                const double newPx = self->_appSettings.sidePanelOnLeft ? (self->_sidePanelDragStartVal + dx) : (self->_sidePanelDragStartVal - dx);
                const double areaW = self->ContentArea() ? self->ContentArea().ActualWidth() : 0.0;
                if (areaW > 50.0)
                {
                    self->_sidePanelWidthFraction = std::clamp(newPx / areaW, kSidePanelMinWidthFrac, kSidePanelMaxWidthFrac);
                    self->_SidePanelApplyLayout();
                }
                e.Handled(true);
            });
            const auto endWidthDrag = [weakThis, idle](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || !self->_sidePanelDragWidth)
                {
                    return; // a capture-lost echo of our own release, or a stray event
                }
                self->_sidePanelDragWidth = false;
                if (const auto el = sender.try_as<UIElement>())
                {
                    el.ReleasePointerCapture(e.Pointer());
                }
                ApplyCursor(CoreCursorType::Arrow);
                if (const auto b = sender.try_as<Border>())
                {
                    b.Background(idle);
                }
                // COMMIT: per-window (record autosave reads the live member) + the global default
                // ("last one to resize sets the default for new windows").
                self->_ScheduleWindowRecordSave();
                self->_SidePanelPersistDefaults(/*width*/ true, /*split*/ false, /*collapsed*/ false);
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] width -> " + std::to_wstring(self->_sidePanelWidthFraction) + L" (window " + ::Agentmaster::ShortId(self->_windowId) + L"; new-window default updated)\n");
                e.Handled(true);
            };
            _sidePanelSplitter.PointerReleased(endWidthDrag);
            _sidePanelSplitter.PointerCaptureLost(endWidthDrag);
            AgentSetTip(_sidePanelSplitter, winrt::hstring{ L"Drag to resize the side panel.\nThis window remembers its own width; the last resize anywhere becomes the default for new windows." });
        }

        // --- The panel host -----------------------------------------------------------------------
        // Forced DARK + an explicit dark fill (the Agentmaster surface convention — an app-theme
        // brush would bleed light-mode pixels through widget gaps).
        _sidePanelHost = Grid{};
        _sidePanelHost.RequestedTheme(ElementTheme::Dark);
        _sidePanelHost.Background(Fill(0xFF, 0x2E, 0x2E, 0x2E));

        // EXPANDED content: header (search + sort + dock + collapse) / count line / body.
        _sidePanelExpandedRoot = Grid{};
        {
            RowDefinition r0{};
            r0.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            RowDefinition r1{};
            r1.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            RowDefinition r2{};
            r2.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            _sidePanelExpandedRoot.RowDefinitions().Append(r0);
            _sidePanelExpandedRoot.RowDefinitions().Append(r1);
            _sidePanelExpandedRoot.RowDefinitions().Append(r2);
        }

        // Header row: [ search * ][ scope ][ sort ][ ⇄ ][ chevron ].
        Grid header{};
        {
            ColumnDefinition c0{};
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition c1{};
            c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c2{};
            c2.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c3{};
            c3.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c4{};
            c4.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            header.ColumnDefinitions().Append(c0);
            header.ColumnDefinitions().Append(c1);
            header.ColumnDefinitions().Append(c2);
            header.ColumnDefinitions().Append(c3);
            header.ColumnDefinitions().Append(c4);
            header.Padding(ThicknessHelper::FromLengths(4, 4, 4, 2));
            header.ColumnSpacing(3);
        }
        _sidePanelSearchBox = TextBox{};
        _sidePanelSearchBox.FontSize(12);
        _sidePanelSearchBox.PlaceholderText(L"filter sessions");
        _sidePanelSearchBox.VerticalAlignment(VerticalAlignment::Center);
        AgentSetTitledTip(_sidePanelSearchBox, L"Filter the cards", winrt::hstring{ L"Whitespace-split terms AND-match a session's title, working directory, branch, or id.\n\"quoted phrase\" matches exactly; a pasted whole session-id GUID matches that session and its forks by identity." });
        _sidePanelSearchBox.TextChanged([weakThis](const IInspectable& sender, const TextChangedEventArgs&) {
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            if (const auto box = sender.try_as<TextBox>())
            {
                self->_sidePanelSearchText = std::wstring{ box.Text() };
            }
            // User-driven — rebuild NOW (forced past the min-gap throttle) so typing filters live.
            self->_SidePanelScheduleRebuild(/*force*/ true);
        });
        Grid::SetColumn(_sidePanelSearchBox, 0);
        header.Children().Append(_sidePanelSearchBox);

        // The card-list SCOPE toggle — LOCAL (this window's tabs; the default, always) / GLOBAL
        // (every window's). The board's own LOCAL/GLOBAL words + accent look, but a SEPARATE state:
        // it re-scopes only this list, never the Manager's tree/board (which answer a different
        // question — what the fleet lens shows, not what sits beside your terminal), and it is
        // remembered per window with NO global default (see _SidePanelToggleScope).
        _sidePanelScopeBtn = Button{};
        _sidePanelScopeBtn.FontSize(11);
        _sidePanelScopeBtn.Padding(ThicknessHelper::FromLengths(8, 1, 8, 1));
        _sidePanelScopeBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelScopeBtn.IsTabStop(false);
        EmphasizeScopeButton(_sidePanelScopeBtn); // the board's accent-blue scope look — this is the panel's primary toggle, louder than sort/dock/collapse
        AgentSetTitledTip(_sidePanelScopeBtn, L"Card scope", winrt::hstring{ L"Which sessions the cards list: LOCAL = only the sessions open in THIS window (the default), GLOBAL = every window's.\nRemembered for this window only \x2014 a new window always starts LOCAL \x2014 and independent of the Triage Board's own scope." });
        _sidePanelScopeBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleScope();
            }
        });
        Grid::SetColumn(_sidePanelScopeBtn, 1);
        header.Children().Append(_sidePanelScopeBtn);

        _sidePanelSortBtn = Button{};
        _sidePanelSortBtn.FontSize(11);
        _sidePanelSortBtn.Padding(ThicknessHelper::FromLengths(8, 1, 8, 1));
        _sidePanelSortBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelSortBtn.IsTabStop(false);
        AgentSetTitledTip(_sidePanelSortBtn, L"Card order", winrt::hstring{ L"Order the session cards. Click to cycle: MOST ACTIVE (last activity of any kind, running sessions on top) \xB7 MOST RECENT (by the last turn boundary only: the prompt you last sent or the reply that last finished \x2014 tool calls and subagent work never move it) \xB7 NEWEST \xB7 OLDEST \xB7 A\x2013Z.\nThe SAME saved order as the Triage Board's card sort \x2014 changing it here re-sorts the board too (and every window)." });
        _sidePanelSortBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            // ONE sort spec: delegate to the Manager content's own cycle (mutate boardSort ->
            // persist via the settings sink, which also updates this page's _appSettings copy and
            // broadcasts — the exact path the board header's sort toggle runs). On return the new
            // order is already in _appSettings, so relabel + resort immediately.
            if (const auto ipc = self->_agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    mgr->CycleBoardSort();
                }
            }
            self->_SidePanelUpdateSortButton();
            self->_SidePanelScheduleRebuild(/*force*/ true);
        });
        Grid::SetColumn(_sidePanelSortBtn, 2);
        header.Children().Append(_sidePanelSortBtn);

        Button dockBtn{};
        dockBtn.FontSize(11);
        dockBtn.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
        dockBtn.VerticalAlignment(VerticalAlignment::Center);
        dockBtn.IsTabStop(false);
        dockBtn.Content(winrt::box_value(L"\x21C4")); // ⇄ — flip which side the panel docks on
        AgentSetTitledTip(dockBtn, L"Dock side", winrt::hstring{ L"Move the panel to the other side of the terminal (left \x21C4 right).\nSaved globally \x2014 other windows adopt it on their next launch or settings change." });
        dockBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelFlipDock();
            }
        });
        Grid::SetColumn(dockBtn, 3);
        header.Children().Append(dockBtn);

        _sidePanelCollapseBtn = Button{};
        _sidePanelCollapseBtn.FontSize(11);
        _sidePanelCollapseBtn.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
        _sidePanelCollapseBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelCollapseBtn.IsTabStop(false);
        // Glyph set by _SidePanelApplyLayout (it points TOWARD the docked edge, so it flips with the dock).
        AgentSetTip(_sidePanelCollapseBtn, winrt::hstring{ L"Collapse the side panel to a thin strip (click the strip to bring it back)." });
        _sidePanelCollapseBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleCollapsed();
            }
        });
        Grid::SetColumn(_sidePanelCollapseBtn, 4);
        header.Children().Append(_sidePanelCollapseBtn);
        Grid::SetRow(header, 0);
        _sidePanelExpandedRoot.Children().Append(header);

        // Count line: "N of M sessions" — the filter's effect at a glance.
        _sidePanelCountText = Text(L"", 10, false, 0.5);
        _sidePanelCountText.Margin(ThicknessHelper::FromLengths(8, 0, 8, 2));
        Grid::SetRow(_sidePanelCountText, 1);
        _sidePanelExpandedRoot.Children().Append(_sidePanelCountText);

        // Body: cards area (star*split) / horizontal splitter / summary area (star*(1-split)).
        _sidePanelBody = Grid{};
        {
            RowDefinition r0{};
            r0.Height(GridLengthHelper::FromValueAndType(_sidePanelSplitFraction, GridUnitType::Star));
            RowDefinition r1{};
            r1.Height(GridLengthHelper::FromPixels(kSidePanelSplitterPx));
            RowDefinition r2{};
            r2.Height(GridLengthHelper::FromValueAndType(1.0 - _sidePanelSplitFraction, GridUnitType::Star));
            _sidePanelBody.RowDefinitions().Append(r0);
            _sidePanelBody.RowDefinitions().Append(r1);
            _sidePanelBody.RowDefinitions().Append(r2);
        }

        // TOP: the scrollable card list (its OWN scroll, independent of the summary's).
        _sidePanelCards = StackPanel{};
        _sidePanelCards.Orientation(Orientation::Vertical);
        ScrollViewer cardsScroll{};
        cardsScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        cardsScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        cardsScroll.Content(_sidePanelCards);
        Border cardsArea{};
        cardsArea.Background(Fill(0xFF, 0x2A, 0x2A, 0x2A));
        cardsArea.Child(cardsScroll);
        Grid::SetRow(cardsArea, 0);
        _sidePanelBody.Children().Append(cardsArea);

        // The HORIZONTAL separator (cards <-> summary) — same drag discipline as the vertical one.
        _sidePanelHSplitter = Border{};
        _sidePanelHSplitter.Background(Fill(0xFF, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray));
        {
            const auto idle = Fill(0xFF, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray, kSidePanelSplitterIdleGray);
            const auto hot = Fill(0xFF, kSidePanelSplitterHotGray, kSidePanelSplitterHotGray, kSidePanelSplitterHotGray);
            _sidePanelHSplitter.PointerEntered([weakThis, hot](const IInspectable& sender, const PointerRoutedEventArgs&) {
                ApplyCursor(CoreCursorType::SizeNorthSouth);
                if (const auto b = sender.try_as<Border>())
                {
                    b.Background(hot);
                }
            });
            _sidePanelHSplitter.PointerExited([weakThis, idle](const IInspectable& sender, const PointerRoutedEventArgs&) {
                const auto self = weakThis.get();
                if (self && self->_sidePanelDragSplit)
                {
                    return;
                }
                ApplyCursor(CoreCursorType::Arrow);
                if (const auto b = sender.try_as<Border>())
                {
                    b.Background(idle);
                }
            });
            _sidePanelHSplitter.PointerPressed([weakThis](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                self->_sidePanelDragSplit = true;
                self->_sidePanelDragStart = e.GetCurrentPoint(nullptr).Position().Y;
                // Start from the cards row's ACTUAL height so the bar tracks the pointer 1:1.
                double cardsH = 0.0;
                if (self->_sidePanelBody && self->_sidePanelBody.RowDefinitions().Size() >= 1)
                {
                    cardsH = self->_sidePanelBody.RowDefinitions().GetAt(0).ActualHeight();
                }
                self->_sidePanelDragStartVal = cardsH;
                if (const auto el = sender.try_as<UIElement>())
                {
                    el.CapturePointer(e.Pointer());
                }
                ApplyCursor(CoreCursorType::SizeNorthSouth);
                e.Handled(true);
            });
            _sidePanelHSplitter.PointerMoved([weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || !self->_sidePanelDragSplit)
                {
                    return;
                }
                const double dy = e.GetCurrentPoint(nullptr).Position().Y - self->_sidePanelDragStart;
                const double bodyH = self->_sidePanelBody ? self->_sidePanelBody.ActualHeight() : 0.0;
                const double total = bodyH - kSidePanelSplitterPx; // what the two star rows share
                if (total > 50.0)
                {
                    self->_sidePanelSplitFraction = std::clamp((self->_sidePanelDragStartVal + dy) / total, kSidePanelMinSplitFrac, kSidePanelMaxSplitFrac);
                    self->_SidePanelApplyLayout();
                }
                e.Handled(true);
            });
            const auto endSplitDrag = [weakThis, idle](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || !self->_sidePanelDragSplit)
                {
                    return;
                }
                self->_sidePanelDragSplit = false;
                if (const auto el = sender.try_as<UIElement>())
                {
                    el.ReleasePointerCapture(e.Pointer());
                }
                ApplyCursor(CoreCursorType::Arrow);
                if (const auto b = sender.try_as<Border>())
                {
                    b.Background(idle);
                }
                // Same commit contract as the width drag: this window's record + the global default.
                self->_ScheduleWindowRecordSave();
                self->_SidePanelPersistDefaults(/*width*/ false, /*split*/ true, /*collapsed*/ false);
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] split -> " + std::to_wstring(self->_sidePanelSplitFraction) + L" (window " + ::Agentmaster::ShortId(self->_windowId) + L"; new-window default updated)\n");
                e.Handled(true);
            };
            _sidePanelHSplitter.PointerReleased(endSplitDrag);
            _sidePanelHSplitter.PointerCaptureLost(endSplitDrag);
            AgentSetTip(_sidePanelHSplitter, winrt::hstring{ L"Drag to rebalance the cards list and the summary.\nThis window remembers its own split; the last resize anywhere becomes the default for new windows." });
        }
        Grid::SetRow(_sidePanelHSplitter, 1);
        _sidePanelBody.Children().Append(_sidePanelHSplitter);

        // BOTTOM: the selected session's summary host (a fill-mode AgentTabOverlay panel, or the hint).
        _sidePanelSummaryHost = Grid{};
        _sidePanelSummaryHost.Background(Fill(0xFF, 0x25, 0x25, 0x25));
        _sidePanelSummaryHost.Padding(ThicknessHelper::FromLengths(4, 4, 4, 4));
        _sidePanelSummaryHint = TextBlock{};
        _sidePanelSummaryHint.Text(L"Select a session to see its summary here.");
        _sidePanelSummaryHint.FontSize(11);
        _sidePanelSummaryHint.Opacity(0.45);
        _sidePanelSummaryHint.TextWrapping(TextWrapping::Wrap);
        _sidePanelSummaryHint.HorizontalAlignment(HorizontalAlignment::Center);
        _sidePanelSummaryHint.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelSummaryHost.Children().Append(_sidePanelSummaryHint);
        // Push the live slot size into the fill-hosted overlay (the OnSummaryPaneSize contract —
        // the same push TerminalPaneContent does for the per-tab panel). Synchronous is safe: the
        // overlay's fill branch is change-gated, so an unchanged size is a no-op.
        _sidePanelSummaryHost.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs& e) {
            const auto self = weakThis.get();
            if (self && self->_sidePanelSummaryOverlay)
            {
                self->_sidePanelSummaryOverlay->OnSummaryPaneSize(e.NewSize().Width, e.NewSize().Height);
            }
        });
        Grid::SetRow(_sidePanelSummaryHost, 2);
        _sidePanelBody.Children().Append(_sidePanelSummaryHost);

        Grid::SetRow(_sidePanelBody, 2);
        _sidePanelExpandedRoot.Children().Append(_sidePanelBody);
        _sidePanelHost.Children().Append(_sidePanelExpandedRoot);

        // COLLAPSED strip: a thin column with one expand chevron — the way back in.
        _sidePanelStrip = StackPanel{};
        _sidePanelStrip.Orientation(Orientation::Vertical);
        _sidePanelStrip.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelStrip.HorizontalAlignment(HorizontalAlignment::Center);
        _sidePanelStripBtn = Button{};
        _sidePanelStripBtn.FontSize(11);
        _sidePanelStripBtn.Padding(ThicknessHelper::FromLengths(2, 10, 2, 10));
        _sidePanelStripBtn.IsTabStop(false);
        AgentSetTip(_sidePanelStripBtn, winrt::hstring{ L"Expand the side panel (session cards + the selected session's summary)." });
        _sidePanelStripBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleCollapsed();
            }
        });
        _sidePanelStrip.Children().Append(_sidePanelStripBtn);
        _sidePanelStrip.Visibility(Visibility::Collapsed);
        _sidePanelHost.Children().Append(_sidePanelStrip);

        area.Children().Append(_sidePanelSplitter);
        area.Children().Append(_sidePanelHost);
        // A window resize re-derives the pixel column width from the fraction — DEFERRED (queue):
        // SizeChanged is a layout-driven trigger, and a synchronous ColumnDefinition write inside
        // the pass risks the E_LAYOUTCYCLE fail-fast (the tag-badge lesson).
        area.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelQueueLayout();
            }
        });

        // --- The PEEK-vs-FOLLOW rule: the bottom summary FOLLOWS the terminal tab you're working in ---
        // By default the summary tracks the FOCUSED terminal tab (so it reflects the session you are
        // actually in); a side-panel CARD click PINS it to another session (a "peek") until you
        // re-engage the terminal. Two complementary re-engage signals on TabContent (the terminal
        // CONTENT column) route through _SidePanelFollowFocusedTab. Crucially the panel's own controls
        // (cards / search / summary) live in a SIBLING ContentArea column, NOT under TabContent, so
        // interacting with THEM never fires these — only touching the terminal does, which is exactly
        // what ends a peek:
        //   * PointerPressed via AddHandler(handledEventsToo) — the "click back into the terminal"
        //     gesture. handledEventsToo is REQUIRED because TermControl marks its own presses Handled
        //     (the Root() outside-press + tab-drag hooks use the same idiom), and it is the ONLY
        //     reliable trigger for the common case: clicking a non-focusable card Border does NOT move
        //     keyboard focus off the terminal, so a later re-click raises no GotFocus.
        //   * GotFocus (plain bubbling) — covers a keyboard/programmatic return of focus INTO the
        //     terminal (e.g. from the side panel's search box, or the window regaining activation).
        // Registered ONCE (TabContent outlives the page) and captured weak, so no revoke is needed —
        // the Root()/_tabView AddHandler precedent; a dead page makes the handler a no-op.
        if (const auto content = TabContent())
        {
            content.AddHandler(
                UIElement::PointerPressedEvent(),
                winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs&) {
                    if (const auto self = weakThis.get())
                    {
                        self->_SidePanelFollowFocusedTab();
                    }
                } }),
                true /* handledEventsToo — the terminal control marks its presses Handled */);
            content.GotFocus([weakThis](const IInspectable&, const RoutedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelFollowFocusedTab();
                }
            });
        }

        _sidePanelBuilt = true;
        _SidePanelApplyDock(); // assigns the three columns per the global dock side + applies widths/glyphs
        _SidePanelUpdateSortButton();
        _SidePanelUpdateScopeButton(); // LOCAL / GLOBAL per the seeded per-window scope
        _SidePanelRebuildCards();
        // Re-select the lens's restored session so a reopened window's summary shows immediately
        // (the same restored selection SetManagerState seeded into the Manager lens).
        if (!_windowRecord.manager.selectedId.empty())
        {
            _SidePanelSelectSession(_windowRecord.manager.selectedId, /*driveManagerLens*/ false);
        }
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[sidepanel] built (window " + ::Agentmaster::ShortId(_windowId) +
                                          L") width=" + std::to_wstring(_sidePanelWidthFraction) +
                                          L" split=" + std::to_wstring(_sidePanelSplitFraction) +
                                          (_sidePanelCollapsed ? L" collapsed" : L" expanded") +
                                          (_sidePanelGlobal ? L" scope=global" : L" scope=local") +
                                          (_appSettings.sidePanelOnLeft ? L" dock=left\n" : L" dock=right\n"));
    }

    // Assign TabContent / the splitter / the panel host to ContentArea's three columns per the
    // GLOBAL AppSettings::sidePanelOnLeft (dock-right: [content *][splitter][panel]; dock-left is
    // the mirror), then re-apply widths + the direction-dependent chevrons via _SidePanelApplyLayout.
    // Idempotent — re-running with an unchanged dock just re-applies the same assignments.
    void TerminalPage::_SidePanelApplyDock()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        const bool left = _appSettings.sidePanelOnLeft;
        if (const auto content = TabContent())
        {
            Grid::SetColumn(content, left ? 2 : 0);
        }
        Grid::SetColumn(_sidePanelSplitter, 1);
        Grid::SetColumn(_sidePanelHost, left ? 0 : 2);
        _SidePanelApplyLayout();
    }

    // Derive the three ContentArea column widths + the cards/summary star ratios + the collapse
    // visibility + the direction-dependent chevron glyphs from the live state. Every write is
    // CHANGE-GATED so re-running from the deferred SizeChanged tick converges instead of cycling.
    // Pre-layout (ContentArea width still 0) the pixel width falls back to the minimum — corrected
    // by the first real SizeChanged.
    void TerminalPage::_SidePanelApplyLayout()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        const auto area = ContentArea();
        if (!area)
        {
            return;
        }
        const auto cols = area.ColumnDefinitions();
        if (cols.Size() < 3)
        {
            return; // XAML contract violated — nothing sane to do
        }
        const bool left = _appSettings.sidePanelOnLeft;
        const auto mainCol = cols.GetAt(left ? 2u : 0u);
        const auto splitCol = cols.GetAt(1u);
        const auto panelCol = cols.GetAt(left ? 0u : 2u);

        // Small change-gated setter: only touch a ColumnDefinition when the target actually
        // differs (a same-value Width write still invalidates layout — the cycle fuel).
        const auto setColPx = [](const ColumnDefinition& col, double px) {
            const auto cur = col.Width();
            if (cur.GridUnitType != GridUnitType::Pixel || std::abs(cur.Value - px) > 0.5)
            {
                col.Width(GridLengthHelper::FromPixels(px));
            }
        };
        const auto setColStar = [](const ColumnDefinition& col) {
            const auto cur = col.Width();
            if (cur.GridUnitType != GridUnitType::Star || std::abs(cur.Value - 1.0) > 0.001)
            {
                col.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            }
        };

        setColStar(mainCol);
        if (_sidePanelCollapsed)
        {
            setColPx(splitCol, 0.0); // no separator while collapsed — the strip isn't resizable
            setColPx(panelCol, kSidePanelStripPx);
        }
        else
        {
            setColPx(splitCol, kSidePanelSplitterPx);
            const double areaW = area.ActualWidth();
            double px = kSidePanelMinPx;
            if (areaW > 50.0)
            {
                // The fraction is already band-clamped at every writer; the pixel floor keeps the
                // header usable on a tiny window, capped so the terminal always keeps >= half.
                px = std::clamp(_sidePanelWidthFraction * areaW, kSidePanelMinPx, std::max(kSidePanelMinPx, areaW * kSidePanelMaxWidthFrac));
            }
            setColPx(panelCol, px);
        }

        // Body split (star ratios — the archiveSplitFraction idiom: scales with the panel height).
        if (_sidePanelBody && _sidePanelBody.RowDefinitions().Size() >= 3)
        {
            const auto rows = _sidePanelBody.RowDefinitions();
            const auto setRowStar = [](const RowDefinition& row, double v) {
                const auto cur = row.Height();
                if (cur.GridUnitType != GridUnitType::Star || std::abs(cur.Value - v) > 0.001)
                {
                    row.Height(GridLengthHelper::FromValueAndType(v, GridUnitType::Star));
                }
            };
            setRowStar(rows.GetAt(0), _sidePanelSplitFraction);
            setRowStar(rows.GetAt(2), 1.0 - _sidePanelSplitFraction);
        }

        // Expanded content vs the collapsed strip.
        if (_sidePanelExpandedRoot)
        {
            _sidePanelExpandedRoot.Visibility(_sidePanelCollapsed ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_sidePanelStrip)
        {
            _sidePanelStrip.Visibility(_sidePanelCollapsed ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_sidePanelSplitter)
        {
            _sidePanelSplitter.Visibility(_sidePanelCollapsed ? Visibility::Collapsed : Visibility::Visible);
        }

        // Chevrons point TOWARD the docked edge to collapse, AWAY to expand — so they flip with
        // the dock. (Plain text glyphs — no icon-font guessing; any UI font carries » «.)
        const wchar_t* const towardEdge = left ? L"\x00AB" : L"\x00BB"; // « / »
        const wchar_t* const awayFromEdge = left ? L"\x00BB" : L"\x00AB";
        if (_sidePanelCollapseBtn)
        {
            _sidePanelCollapseBtn.Content(winrt::box_value(towardEdge));
        }
        if (_sidePanelStripBtn)
        {
            _sidePanelStripBtn.Content(winrt::box_value(awayFromEdge));
        }
    }

    // Coalesce a layout re-apply onto ONE clean dispatcher tick — the only legal path from a
    // layout-driven trigger (ContentArea.SizeChanged): writing column widths synchronously inside
    // the layout pass risks XAML's E_LAYOUTCYCLE fail-fast. Later triggers fold into the queued
    // tick; a dead dispatcher (teardown) drops the request harmlessly (the next trigger re-queues).
    void TerminalPage::_SidePanelQueueLayout()
    {
        if (!_sidePanelBuilt || _sidePanelLayoutQueued)
        {
            return;
        }
        _sidePanelLayoutQueued = true;
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                if (const auto self = weakThis.get())
                {
                    self->_sidePanelLayoutQueued = false;
                    self->_SidePanelApplyLayout();
                }
            });
        }
        catch (...)
        {
            // Dispatcher gone (window tearing down) — never leave the coalescer latched shut.
            _sidePanelLayoutQueued = false;
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelQueueLayout");
        }
    }

    // Reflect the current GLOBAL board sort on the order button — the board header's own label
    // mapping, verbatim, so the two toggles always read the same word for the same order.
    void TerminalPage::_SidePanelUpdateSortButton()
    {
        if (!_sidePanelSortBtn)
        {
            return;
        }
        const wchar_t* label = (_appSettings.boardSort == ExplorerSort::Newest)     ? L"NEWEST" :
                               (_appSettings.boardSort == ExplorerSort::Oldest)     ? L"OLDEST" :
                               (_appSettings.boardSort == ExplorerSort::Alpha)      ? L"A\x2013Z" :
                               (_appSettings.boardSort == ExplorerSort::MostRecent) ? L"MOST RECENT" : // the turn-boundary order (ExplorerSort::MostRecent)
                                                                                      L"MOST ACTIVE"; // MostActive (default) + any stray ByPid
        _sidePanelSortBtn.Content(winrt::box_value(label));
    }

    // Reflect THIS window's card-list scope on the scope toggle — the board's own LOCAL/GLOBAL
    // words (_UpdateBoardScopeButton), so the two surfaces read the same label for the same idea.
    void TerminalPage::_SidePanelUpdateScopeButton()
    {
        if (!_sidePanelScopeBtn)
        {
            return;
        }
        _sidePanelScopeBtn.Content(winrt::box_value(_sidePanelGlobal ? L"GLOBAL" : L"LOCAL"));
    }

    // Flip the card list between LOCAL (only the sessions hosted in this window) and GLOBAL (every
    // window's) — for THIS window only. Persists through the window-record autosave ALONE: there is
    // deliberately no _SidePanelPersistDefaults call and no AppSettings field, because the scope has
    // no global default by design — every new window starts LOCAL, so one window's fleet-wide view can
    // never seed another window's list (the "my list shows another window's tabs" bug this scope
    // exists to prevent). The Manager's own tree/board scope is NOT touched: it answers a different
    // question (what the fleet lens shows), and a click here must not re-scope the Manager under
    // the user. Logged as a [sidepanel] mechanism line like the width/split commits — not [nav]: a
    // scope flip is a pure view filter (the board's LOCAL/GLOBAL toggle is unlogged for the same reason).
    void TerminalPage::_SidePanelToggleScope()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        _sidePanelGlobal = !_sidePanelGlobal;
        _SidePanelUpdateScopeButton();
        _SidePanelScheduleRebuild(/*force*/ true); // user action — past the throttle; the rebuild re-snapshots the hosted set
        _ScheduleWindowRecordSave(); // the per-window half is the ONLY half (see above)
        ::Agentmaster::AppendStateLog(L"hooks.log", std::wstring{ L"[sidepanel] scope -> " } + (_sidePanelGlobal ? L"global" : L"local") + L" (window " + ::Agentmaster::ShortId(_windowId) + L"; per-window only)\n");
    }

    // Collapse to the thin expander strip / expand back. Persists BOTH halves of the contract:
    // this window's record (autosave reads the live member) and the global new-window default
    // (granular RMW — only the collapsed field). An expand replays any rebuild deferred while the
    // pixels were hidden.
    void TerminalPage::_SidePanelToggleCollapsed()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        _sidePanelCollapsed = !_sidePanelCollapsed;
        ::Agentmaster::LogNav(_sidePanelCollapsed ? L"sidepanel collapse" : L"sidepanel expand");
        _SidePanelApplyLayout();
        if (!_sidePanelCollapsed && _sidePanelDirty)
        {
            _SidePanelRebuildCards(); // the deferred catch-up — the list is visible again
        }
        _ScheduleWindowRecordSave();
        _SidePanelPersistDefaults(/*width*/ false, /*split*/ false, /*collapsed*/ true);
    }

    // Flip the GLOBAL dock side (left <-> right) and re-dock THIS window live. Deliberately a
    // freshest-disk RMW like the summary-panel toggles (no cross-window broadcast of its own —
    // other windows adopt on their next launch or on the next settings broadcast that reaches
    // them, where _SidePanelApplySettings re-docks them).
    void TerminalPage::_SidePanelFlipDock()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        const bool next = !_appSettings.sidePanelOnLeft;
        auto s = ::Agentmaster::LoadAppSettings();
        s.sidePanelOnLeft = next;
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.sidePanelOnLeft = next;
        ::Agentmaster::LogNav(next ? L"sidepanel dock -> left" : L"sidepanel dock -> right");
        _SidePanelApplyDock();
    }

    // The "last one to adjust sets the default" half: RMW ONLY the flagged AppSettings sidePanel*
    // fields from this window's live values (granular on purpose — a width drag must never export
    // this window's collapse state as the global default), then keep the in-memory copy in step so
    // a later cog Save's preserve block can't regress it.
    // `width`/`split`/`collapsed`: which live values to export as the new-window defaults.
    void TerminalPage::_SidePanelPersistDefaults(bool width, bool split, bool collapsed)
    {
        auto s = ::Agentmaster::LoadAppSettings();
        if (width)
        {
            s.sidePanelWidthFraction = _sidePanelWidthFraction;
        }
        if (split)
        {
            s.sidePanelSplitFraction = _sidePanelSplitFraction;
        }
        if (collapsed)
        {
            s.sidePanelCollapsed = _sidePanelCollapsed;
        }
        ::Agentmaster::SaveAppSettings(s);
        // Adopt the merged trio (not just the flagged one): the freshest disk may carry another
        // window's newer defaults, and _appSettings must mirror what is actually stored.
        _appSettings.sidePanelWidthFraction = s.sidePanelWidthFraction;
        _appSettings.sidePanelSplitFraction = s.sidePanelSplitFraction;
        _appSettings.sidePanelCollapsed = s.sidePanelCollapsed;
    }

    // Coalesced rebuild entry. force==true (a user action — typing, sort, settings) rebuilds NOW;
    // otherwise a rebuild inside the min gap, or while the panel is collapsed (no pixels), only
    // marks the list dirty — the periodic tick / the expand path catches up. This is the
    // Manager-lens rebuild-tax discipline applied to the side panel.
    void TerminalPage::_SidePanelScheduleRebuild(bool force)
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        if (_sidePanelCollapsed && !force)
        {
            _sidePanelDirty = true;
            return;
        }
        const uint64_t now = ::GetTickCount64();
        if (!force && now - _sidePanelLastRebuildMs < kSidePanelRebuildMinGapMs)
        {
            _sidePanelDirty = true; // folded into the next periodic tick
            return;
        }
        _SidePanelRebuildCards();
    }

    // The page's batched registry drain calls this (UI thread) once per drained change set —
    // sessions changed somewhere in the fleet, so the card list may be stale.
    void TerminalPage::_SidePanelNoteRegistryChange()
    {
        _SidePanelScheduleRebuild(/*force*/ false);
    }

    // Liveness-probe rider (SCANNER thread): terminate-netted fire_and_forget over the awaitable
    // Impl, exactly like _ScanCacheWarmTabs — an exception escaping a fire_and_forget is
    // std::terminate, so the body lives in an IAsyncAction whose throw lands in this catch.
    winrt::fire_and_forget TerminalPage::_SidePanelPeriodicTick()
    {
        auto strongThis{ get_strong() };
        try
        {
            co_await _SidePanelPeriodicTickImpl();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelPeriodicTick");
        }
    }

    // The periodic body (UI thread after the hop): (1) replay a rebuild deferred by the throttle,
    // (2) kick the summary overlay's cheap mtime-gated content re-check (so a quiet session's last
    // message still lands without a registry notify), (3) every ~30s rewrite the cards' timing
    // texts IN PLACE — never a full rebuild just to move an "ago" string (the Manager lens lesson).
    winrt::Windows::Foundation::IAsyncAction TerminalPage::_SidePanelPeriodicTickImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sidePanelBuilt || _sidePanelCollapsed)
        {
            co_return; // collapsed: stay fully idle — the expand path runs the catch-up
        }
        // LOCAL-scope belt: the hosted set can change with NO registry notify — a tab docked into
        // or torn out of this window moves a session that stays live + unchanged (the [move-out]
        // path drops only the per-window binding), so the registry-drain trigger never fires for
        // it and the list would keep (or miss) that card until an unrelated session changed. A
        // set-compare per tick (tens of ids, no registry access) catches it; under GLOBAL every
        // membership change IS a registry change, so there is nothing to diff.
        if (!_sidePanelGlobal && !_sidePanelDirty && _HostedSessionIds() != _sidePanelRenderedLocalIds)
        {
            _sidePanelDirty = true;
        }
        if (_sidePanelDirty)
        {
            _SidePanelRebuildCards(); // rebuild resets the timing texts too — skip the in-place pass
            co_return;
        }
        if (_sidePanelSummaryOverlay)
        {
            _sidePanelSummaryOverlay->RefreshSummaryContent(); // mtime-gated: a quiet transcript costs one stat
        }
        const uint64_t now = ::GetTickCount64();
        if (now < _sidePanelNextTimingMs)
        {
            co_return;
        }
        _sidePanelNextTimingMs = now + kSidePanelTimingRefreshMs;
        if (!_sessionRegistry)
        {
            co_return;
        }
        for (const auto& [id, tb] : _sidePanelTimingTexts)
        {
            if (!tb)
            {
                continue;
            }
            if (const auto s = _sessionRegistry->Get(id))
            {
                const std::wstring t = FormatSessionTiming(s->convCreatedUnixMs, s->convLastActivityUnixMs ? s->convLastActivityUnixMs : s->lastActivityUnixMs);
                if (!t.empty())
                {
                    tb.Text(winrt::hstring{ t });
                }
            }
        }
    }

    // The managed sessions hosted in THIS window: every _claudeTabs entry whose weak Tab still
    // resolves (a torn-down tab's expired ref is skipped, so the set is live, not historical). The
    // ONE "is this session here?" predicate — the Explorer Tree's LOCAL scope provider
    // (_WireAgentManagerContent) and the side panel's LOCAL card list both read it, so a card in the
    // panel always has a tab in this window and the two surfaces can never disagree about what
    // "here" means. Cheap: a walk over the per-window map (tens of entries), no registry access.
    // Returns the set of hosted session ids (Claude AND Codex — _claudeTabs is agent-agnostic).
    std::unordered_set<std::wstring> TerminalPage::_HostedSessionIds() const
    {
        std::unordered_set<std::wstring> ids;
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            if (weakTab.get())
            {
                ids.insert(id);
            }
        }
        return ids;
    }

    // Rebuild the card list from the live fleet: SnapshotLive (never the full archived Snapshot —
    // the Manager-lens tax rule) -> the SCOPE (LOCAL, the default: only sessions hosted in THIS
    // window per _HostedSessionIds; GLOBAL: every window's) -> the search filter (the Sessions
    // grammar) -> the SHARED boardSort order (MakeSortKey/SortKeyLess — the board's own comparator)
    // -> compact cards. Also refreshes the count line (which names the scope) and re-registers the
    // selection highlight + timing-text maps + the hosted set the periodic LOCAL belt diffs against.
    void TerminalPage::_SidePanelRebuildCards()
    {
        if (!_sidePanelBuilt || !_sidePanelCards || !_sessionRegistry)
        {
            return;
        }
        _sidePanelLastRebuildMs = ::GetTickCount64();
        _sidePanelDirty = false;

        const auto snap = _sessionRegistry->SnapshotLive();
        const auto terms = ::Agentmaster::ParseSessionQuery(_sidePanelSearchText);
        // LOCAL scope (the default): only the sessions whose tab lives in THIS window — the hosted
        // set, snapshotted ONCE per rebuild and remembered so the periodic tick can detect a hosting
        // change the registry never announces (a tab docked in / torn out — see _SidePanelPeriodicTickImpl).
        const bool local = !_sidePanelGlobal;
        std::unordered_set<std::wstring> hosted;
        if (local)
        {
            hosted = _HostedSessionIds();
        }
        _sidePanelRenderedLocalIds = hosted; // empty under GLOBAL — the belt only diffs while LOCAL
        std::vector<const ::Agentmaster::SessionInfo*> rows;
        rows.reserve(snap.size());
        size_t liveTotal = 0; // the IN-SCOPE total (what "N of M" counts against — never another window's sessions under LOCAL)
        for (const auto& s : snap)
        {
            if (!s.live)
            {
                continue; // belt — SnapshotLive should already be live-only
            }
            if (local && hosted.find(s.id) == hosted.end())
            {
                continue; // hosted by another window (or a live session with no tab here) — not "here"
            }
            ++liveTotal;
            if (SidePanelSessionMatches(s, terms, _appSettings.tabColorMode))
            {
                rows.push_back(&s);
            }
        }
        std::stable_sort(rows.begin(), rows.end(), [this](const ::Agentmaster::SessionInfo* a, const ::Agentmaster::SessionInfo* b) {
            return SortKeyLess(_appSettings.boardSort, MakeSortKey(*a), MakeSortKey(*b));
        });

        _sidePanelCards.Children().Clear();
        _sidePanelCardsById.clear();
        _sidePanelTimingTexts.clear();
        for (const auto* s : rows)
        {
            _sidePanelCards.Children().Append(_SidePanelMakeCard(*s));
        }
        if (_sidePanelCountText)
        {
            const std::wstring shown = std::to_wstring(rows.size());
            const std::wstring total = std::to_wstring(liveTotal);
            // The count names the scope, so a short list under LOCAL reads as "this window" rather
            // than "the fleet shrank" — the cue that the toggle exists when a session seems missing.
            const std::wstring where = local ? L" in this window" : L" across all windows";
            _sidePanelCountText.Text(winrt::hstring{ terms.empty() ? (total + L" session" + (liveTotal == 1 ? L"" : L"s") + where) : (shown + L" of " + total + L" sessions" + where) });
        }
    }

    // Build ONE compact session card — the Triage-Board card's visual language re-hosted for a
    // narrow vertical list: the title on a band painted the session's RESOLVED color (the same
    // ResolveSessionColorHex every color surface shares, contrast-picked text), then a state row
    // (dot + label in the shared AgentStatusColors palette, the pending-draft dots, the timing
    // adornment) and a dim dir/branch line. Interactions: click = select (Linked Lenses),
    // double-click = activate (jump to the tab, cross-window), right-click = the SHARED lazy
    // session menu (AgentManagerContent::BuildSessionMenuFor — zero drift from the board's).
    // `s`: the live session snapshot the card renders (values copied — the card never holds it).
    winrt::Windows::UI::Xaml::Controls::Border TerminalPage::_SidePanelMakeCard(const ::Agentmaster::SessionInfo& s)
    {
        const std::wstring id = s.id;
        const std::wstring dir = ::Agentmaster::EffectiveWorkingDir(_appSettings.tabColorMode, s);
        const Color stateColor = StateColor(s.state);
        const bool selected = (id == _sidePanelSelectedId);

        Border card{};
        card.Background(Fill(0xFF, 0x33, 0x33, 0x33));
        card.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        card.Margin(ThicknessHelper::FromLengths(4, 2, 4, 2));
        card.BorderThickness(ThicknessHelper::FromUniformLength(1)); // constant width — hover/selection recolor, never reflow
        SidePanelStyleCardBorder(card, stateColor, selected, /*hover*/ false);

        StackPanel v{};
        v.Orientation(Orientation::Vertical);

        // Title band: the session's resolved color (dir-keyed / individual / inferred per the
        // GLOBAL tab-color mode — empty under Remove-colors) with WCAG-contrast-picked ink, the
        // same visual as the board card's band, so same-dir cards cluster by color here too.
        Border band{};
        const std::wstring colorHex = ::Agentmaster::ResolveSessionColorHex(_appSettings.tabColorMode, s);
        const auto bandColor = HexToColor(colorHex);
        band.Background(SolidColorBrush{ bandColor.value_or(ColorHelper::FromArgb(0xFF, 0x3A, 0x3A, 0x3A)) });
        band.CornerRadius(CornerRadiusHelper::FromRadii(3, 3, 0, 0));
        band.Padding(ThicknessHelper::FromLengths(6, 2, 6, 2));
        TextBlock title{};
        title.Text(winrt::hstring{ s.title.empty() ? id : s.title });
        title.FontSize(12);
        title.FontWeight(FontWeights::SemiBold());
        title.TextWrapping(TextWrapping::NoWrap);
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        const bool darkText = bandColor && PreferDarkTextOn(*bandColor);
        title.Foreground(darkText ? Fill(0xFF, 0x10, 0x10, 0x10) : Fill(0xFF, 0xF0, 0xF0, 0xF0));
        band.Child(title);
        v.Children().Append(band);

        // State row: dot + label (the shared palette), a teal `codex` pill for a managed Codex,
        // the pending-draft "3 dots" (the light half of the contrast pair — card bodies are dark),
        // and the timing adornment at the end.
        StackPanel row1{};
        row1.Orientation(Orientation::Horizontal);
        row1.Spacing(5);
        row1.Margin(ThicknessHelper::FromLengths(6, 3, 6, 0));
        winrt::Windows::UI::Xaml::Shapes::Ellipse dot{};
        dot.Width(8);
        dot.Height(8);
        dot.VerticalAlignment(VerticalAlignment::Center);
        dot.Fill(SolidColorBrush{ stateColor });
        row1.Children().Append(dot);
        {
            auto stateText = Text(StateLabel(s.state), 10, false, 0.9);
            stateText.Foreground(SolidColorBrush{ stateColor });
            row1.Children().Append(stateText);
        }
        if (s.kind == ::Agentmaster::AgentKind::Codex)
        {
            auto pill = Text(L"codex", 9, true, 0.9);
            pill.Foreground(Fill(0xFF, 0x2E, 0xB8, 0xA8)); // the External/Codex teal pill color family
            row1.Children().Append(pill);
        }
        if (!s.pendingInput.empty())
        {
            // Unsent-draft indicator — the same BuildPendingDots cluster the board card pulses,
            // in the user's configured LIGHT color (card bodies are always dark here).
            const auto dotsColor = ParseArgbHexColor(_appSettings.pendingDotsLightColor, ColorHelper::FromArgb(0xFF, 0xE0, 0xA9, 0x2B));
            row1.Children().Append(BuildPendingDots(4.0, dotsColor));
        }
        {
            const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
            if (auto timing = TimingText(s.convCreatedUnixMs, last))
            {
                row1.Children().Append(timing);
                _sidePanelTimingTexts.emplace_back(id, timing); // the 30s tick rewrites it in place
            }
        }
        v.Children().Append(row1);

        // Dir/branch line: "<leaf>[ / <branch>]", dim — the card's place at a glance (the full
        // path rides the tooltip).
        {
            std::wstring line = SidePanelLeafOf(dir);
            if (!s.branch.empty())
            {
                line += L" / " + s.branch;
            }
            auto dirText = Text(winrt::hstring{ line }, 10, false, 0.55);
            dirText.Margin(ThicknessHelper::FromLengths(6, 0, 6, 4));
            v.Children().Append(dirText);
        }

        card.Child(v);
        AgentSetTitledTip(card, winrt::hstring{ s.title }, winrt::hstring{ dir + (s.branch.empty() ? L"" : (L"\nbranch: " + s.branch)) + L"\n\nClick to select \x00B7 double-click to jump to its tab \x00B7 right-click for actions." }, kCardTipDelay);

        const auto weakThis = get_weak();
        // Hover ring — guarded by PointerStillWithin so a child-bubbled exit (crossing between the
        // card's hit-testable labels) can't strobe the ring off while the pointer is still inside.
        // ⚠ The handlers reach the card ONLY through `sender` — capturing the Border itself in a
        // handler attached TO it is a self-reference cycle (element -> handler -> element) that
        // leaks one card per rebuild forever (the board's cards capture weakly for the same reason).
        card.PointerEntered([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs&) {
            const auto self = weakThis.get();
            const auto b = sender.try_as<Border>();
            if (!self || !b)
            {
                return;
            }
            Color c = Colors::Gray();
            if (self->_sessionRegistry)
            {
                if (const auto cur = self->_sessionRegistry->Get(id))
                {
                    c = StateColor(cur->state);
                }
            }
            SidePanelStyleCardBorder(b, c, id == self->_sidePanelSelectedId, /*hover*/ true);
        });
        card.PointerExited([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            const auto self = weakThis.get();
            const auto b = sender.try_as<Border>();
            if (!self || !b || PointerStillWithin(sender, e))
            {
                return; // bubbled from a child — the pointer never actually left the card
            }
            Color c = Colors::Gray();
            if (self->_sessionRegistry)
            {
                if (const auto cur = self->_sessionRegistry->Get(id))
                {
                    c = StateColor(cur->state);
                }
            }
            SidePanelStyleCardBorder(b, c, id == self->_sidePanelSelectedId, /*hover*/ false);
        });
        card.Tapped([weakThis, id](const IInspectable&, const TappedRoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelSelectSession(id, /*driveManagerLens*/ true);
            }
        });
        card.DoubleTapped([weakThis, id](const IInspectable&, const DoubleTappedRoutedEventArgs& e) {
            e.Handled(true);
            if (const auto self = weakThis.get())
            {
                self->_ActivateClaudeSession(winrt::hstring{ id }); // jump to the tab — cross-window via the activate sinks
            }
        });
        card.RightTapped([weakThis, id](const IInspectable& sender, const RightTappedRoutedEventArgs& e) {
            e.Handled(true);
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            // Contained: menu build + ShowAt can throw under islands teardown races — a lost menu
            // must never cost the window (Rule #18: log, keep running).
            try
            {
                self->_SidePanelSelectSession(id, /*driveManagerLens*/ true); // right-click implies "about THIS one" — align the lenses first
                if (const auto ipc = self->_agentManagerContent.get())
                {
                    if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                    {
                        std::wstring cwd;
                        if (self->_sessionRegistry)
                        {
                            if (const auto cur = self->_sessionRegistry->Get(id))
                            {
                                cwd = ::Agentmaster::EffectiveWorkingDir(self->_appSettings.tabColorMode, *cur);
                            }
                        }
                        auto menu = mgr->BuildSessionMenuFor(id, cwd);
                        if (const auto fe = sender.try_as<UIElement>())
                        {
                            menu.ShowAt(fe, e.GetPosition(fe));
                        }
                    }
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"sidepanel card context menu");
            }
        });

        _sidePanelCardsById[id] = card;
        return card;
    }

    // Select a card: restyle the old/new borders in place (no rebuild), bind the summary below,
    // and — for selections that ORIGINATE here (a card click) — drive the Manager lens so the
    // board/tree/Auto-Testing follow (Linked Lenses). Calls that arrive FROM the lens or the tab
    // funnel pass driveManagerLens=false, and the same-id early-out breaks the echo loop either
    // way. An empty id deselects (hint shown below).
    // `id`: the managed session to select ("" == clear). `driveManagerLens`: also push the
    // selection into AgentManagerContent::SelectSession.
    void TerminalPage::_SidePanelSelectSession(const std::wstring& id, bool driveManagerLens)
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        if (_sidePanelSelectedId == id)
        {
            return; // already there — and the lens echo of our own drive lands here too
        }
        const std::wstring prev = _sidePanelSelectedId;
        _sidePanelSelectedId = id;
        // Restyle in place via the card map — the reason the map exists (a selection change must
        // not pay a list rebuild).
        const auto restyle = [this](const std::wstring& sid, bool sel) {
            const auto it = _sidePanelCardsById.find(sid);
            if (it == _sidePanelCardsById.end() || !it->second)
            {
                return; // filtered out / not rendered — nothing to paint
            }
            Color c = Colors::Gray();
            if (_sessionRegistry)
            {
                if (const auto cur = _sessionRegistry->Get(sid))
                {
                    c = StateColor(cur->state);
                }
            }
            SidePanelStyleCardBorder(it->second, c, sel, /*hover*/ false);
        };
        if (!prev.empty())
        {
            restyle(prev, false);
        }
        if (!id.empty())
        {
            restyle(id, true);
        }
        _SidePanelBindSummary(id);
        if (driveManagerLens && !id.empty())
        {
            if (const auto ipc = _agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    mgr->SelectSession(winrt::hstring{ id });
                }
            }
        }
    }

    // The PEEK-vs-FOLLOW rule (the summary follows what you're working IN): re-select the FOCUSED
    // terminal tab's session in the SIDE PANEL — card highlight + the bottom summary — which ENDS
    // any card "peek" (a side-panel card click that pinned the summary to a different session). Wired
    // to TabContent's PointerPressed (the click-back-into-the-terminal gesture) + bubbling GotFocus
    // in _InitAgentSidePanel, so re-engaging the terminal snaps the summary back to the tab you're in
    // while clicking a card overrides it until then. Deliberately scoped to the side panel
    // (driveManagerLens=false): the Manager board/tree/Auto-Testing keeps its own deliberate
    // selection — a terminal click must not yank the fleet lens around (the request is about the
    // summary area only). The same-id early-out in _SidePanelSelectSession makes the steady state
    // (already on the focused tab — the vast majority of these high-frequency events) a no-op.
    void TerminalPage::_SidePanelFollowFocusedTab()
    {
        if (!_sidePanelBuilt)
        {
            return; // panel never built this run — nothing to follow
        }
        // Never during startup/restore: _RestoreWindowTabs programmatically focuses tabs, and this
        // must not clobber the per-window side-panel selection _InitAgentSidePanel seeded from the
        // record's manager.selectedId (the same gate _SyncManagerSelectionToTab uses for the lens).
        if (_startupState != StartupState::Initialized)
        {
            return;
        }
        const auto tab = _GetFocusedTab();
        // The Manager tab (its content isn't a session) and "nothing focused" both leave the current
        // selection alone — returning to the Manager tab should keep showing the last peek, not clear it.
        if (!tab || tab == _managerTab)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // a pwsh / cmd / external shell tab — no managed summary to show; keep the current peek
        }
        _SidePanelSelectSession(id, /*driveManagerLens*/ false);
    }

    // Point the bottom area at `id`: create (or replace) the fill-hosted AgentTabOverlay whose
    // summary panel renders that session — the per-tab pencil panel's ENTIRE pipeline reused
    // (off-thread analyze, RenderSummaryBox/RenderCodexSummary, times ticker, wrap/truncate/
    // previous toggles, JUMP buttons) with this host doing the sizing. A same-id rebind only kicks
    // the cheap mtime-gated content re-check; an empty id releases the overlay (its dtor detaches
    // the registry observer) and shows the hint. The overlay is DELIBERATELY not in
    // _claudeOverlays (that map is keyed by session id and may already hold the session's own
    // per-tab overlay) — settings pushes reach it via _SidePanelApplySettings + the extended
    // _ToggleSummary* loops.
    // `id`: the session to summarize ("" == none).
    void TerminalPage::_SidePanelBindSummary(const std::wstring& id)
    {
        if (!_sidePanelBuilt || !_sidePanelSummaryHost)
        {
            return;
        }
        if (id.empty())
        {
            _sidePanelSummaryHost.Children().Clear();
            _sidePanelSummaryHost.Children().Append(_sidePanelSummaryHint);
            _sidePanelSummaryOverlay = nullptr; // releases the old overlay -> its observer detaches (Rule #10)
            _sidePanelSummaryId.clear();
            return;
        }
        if (id == _sidePanelSummaryId && _sidePanelSummaryOverlay)
        {
            _sidePanelSummaryOverlay->RefreshSummaryContent(); // cheap: mtime-gated stat unless the transcript grew
            return;
        }
        if (!_sessionRegistry)
        {
            return;
        }
        _sidePanelSummaryHost.Children().Clear();
        _sidePanelSummaryOverlay = nullptr; // drop the previous session's overlay FIRST (observer detach)

        auto overlay = winrt::make_self<implementation::AgentTabOverlay>();
        // Fill-host BEFORE Initialize — the grips + root alignment are decided at build time.
        overlay->SetSummaryFillHost();
        // Seed the GLOBAL content flags before the first render (plain members — safe pre-build).
        overlay->SetSummaryWrapNewlines(_appSettings.summaryPanelWrapNewlines);
        overlay->SetSummaryTruncate(_appSettings.summaryPanelTruncate);
        overlay->SetSummaryShowPrevious(_appSettings.summaryPanelShowPrevious);
        overlay->Initialize(id, _sessionRegistry);
        // The side panel's summary is a primary surface, not a HUD floating over a terminal — it
        // is never dimmed (rest == hover == 1.0). Applied after Initialize so it re-paints the
        // built panel; this instance is outside _claudeOverlays, so the global opacity broadcast
        // can never re-dim it.
        overlay->SetOverlayOpacities(1.0, 1.0);
        overlay->SetTabColorMode(static_cast<int>(_appSettings.tabColorMode));
        overlay->SetModelFamilies(_appSettings.modelFamilies);
        {
            // The page-side handlers — the SAME wiring _AttachClaudeOverlay gives a per-tab panel,
            // resolved by session id at call time so a pane restart can't strand a stale control.
            auto weakThis = get_weak();
            const std::wstring sid = id;
            overlay->SetSummaryWrapToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryWrap();
                }
            });
            overlay->SetSummaryTruncateToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryTruncate();
                }
            });
            overlay->SetSummaryPreviousToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryPrevious();
                }
            });
            overlay->SetJumpHandler([weakThis, sid](const std::vector<std::wstring>& msgs, int index) -> int {
                auto self = weakThis.get();
                return self ? self->_JumpToPromptInSession(sid, msgs, index) : -1;
            });
            overlay->SetEligibilityHandler([weakThis, sid](const std::vector<std::wstring>& msgs) -> std::vector<int> {
                auto self = weakThis.get();
                return self ? self->_JumpEligibilityInSession(sid, msgs) : std::vector<int>{};
            });
            overlay->SetAdjacentPromptHandler([weakThis, sid](bool up) {
                if (auto self = weakThis.get())
                {
                    self->_ScrollAdjacentPrompt(sid, up);
                }
            });
            overlay->SetLiveDraftHandler([weakThis, sid]() -> std::wstring {
                auto self = weakThis.get();
                return self ? self->_ReadLiveDraftForSession(sid) : std::wstring{};
            });
            overlay->SetClearDraftHandler([weakThis, sid]() {
                if (auto self = weakThis.get())
                {
                    self->_ClearLiveDraftForSession(sid);
                }
            });
            // NOT wired on purpose: SetSummaryToggleHandler (the pencil lives on the badge, which
            // is never mounted here — the bottom area's visibility is the split, not the global
            // pencil) and SetSummaryResizeHandler (fill mode builds no grips).
        }
        // ALWAYS enabled — independent of the global showSummaryPanel pencil: this slot exists to
        // show the summary; hiding it would leave a dead gray area.
        overlay->SetSummaryEnabled(true);
        _sidePanelSummaryHost.Children().Append(overlay->SummaryRoot());
        _sidePanelSummaryOverlay = overlay;
        _sidePanelSummaryId = id;
        // Push the current slot size (SizeChanged only fires on future changes).
        overlay->OnSummaryPaneSize(_sidePanelSummaryHost.ActualWidth(), _sidePanelSummaryHost.ActualHeight());
    }

    // Adopt GLOBAL settings changes for the side panel — called from the cog-save sink and the
    // cross-window settings broadcast (both run after _appSettings was replaced): re-label the
    // shared sort, re-dock if sidePanelOnLeft changed elsewhere, push the summary content flags +
    // color mode into the fill-hosted overlay (it is outside the _claudeOverlays broadcast loops),
    // and rebuild the cards (sort/color rules may have changed). Per-window layout (fractions /
    // collapse) is deliberately NOT touched — a global-default change only affects NEW windows.
    void TerminalPage::_SidePanelApplySettings()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        _SidePanelUpdateSortButton();
        _SidePanelApplyDock(); // no-op when the dock side didn't change (idempotent assignments)
        if (_sidePanelSummaryOverlay)
        {
            _sidePanelSummaryOverlay->SetSummaryWrapNewlines(_appSettings.summaryPanelWrapNewlines);
            _sidePanelSummaryOverlay->SetSummaryTruncate(_appSettings.summaryPanelTruncate);
            _sidePanelSummaryOverlay->SetSummaryShowPrevious(_appSettings.summaryPanelShowPrevious);
            _sidePanelSummaryOverlay->SetTabColorMode(static_cast<int>(_appSettings.tabColorMode));
            _sidePanelSummaryOverlay->SetModelFamilies(_appSettings.modelFamilies);
        }
        _SidePanelScheduleRebuild(/*force*/ true);
    }
}
