// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster Manager tab content -- C1 'Linked Lenses' (7 partial files)
// The pinned leftmost tab's UI (DESIGN section 9): a Triage Board + Explorer Tree + Auto Testing over
// ONE shared SessionRegistry, built imperatively. ONE class (AgentManagerContent) split from the
// former 10864-line .cpp into by-area TUs that share AgentManagerContent.Internal.h.
//
// Partial files in this group (★ marks THIS file):
//   AgentManagerContent.cpp             - CORE: ctor/dtor, the Set* wiring, IPaneContent, the per-window lens, _BuildLayout, _Refresh
//   AgentManagerContent.Internal.h      - the ~48 shared file-local helpers: StateColor/Pill/StateDot/Text/Fill + path/sort utils (anonymous namespace, a per-TU copy)
//   AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
//   AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
// ★ AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
//   AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
//   AgentManagerContent.Launch.cpp      - the Launch bar: cwd validation, the Claude/Codex toggle, launch/create/fork, the path-picker drop-down
// ======================================================================================
//
// Agentmaster Manager tab: the toolbar buttons (keep-awake / reopen-windows / activate-all) and the Settings cog overlay (its tab strip, save/show/hide, the env-vars editor, the UPDATES section, and the "Claude not detected" overlay). Partial TU of AgentManagerContent.cpp.
#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCopyActions.h" // CopySessionField — the shared copy-menu action (same path as the per-tab overlay's copy button)
#include "AgentStatusColors.h" // ParseArgbHexColor / FormatArgbHexColor — the cog's "status flashing color" picker <-> AppSettings::flashRingColor
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/ProfileBootstrap.h" // the cog's Profile row (active dir + Change… picker)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/Engine.h" // RecoverableWindows (the "Reopen Windows (N)" recover button)
#include "AgentMaster/ProcessInspect.h" // ReadTranscriptInfo (read-only Auto Testing of an external) + BringClaudeWindowToFront (EXTERNAL menu)
#include "AgentMaster/TranscriptStore.h" // ReadTranscriptQuickFacts — resolve a launch-box session id's cwd
#include "AgentMaster/Updater.h" // the in-app updater: the cog's "Check for updates" + the "vX available!" label

// Agentmaster: the build-stamped git commit + branch (the Settings page header). Generated into
// $(GeneratedFilesDir) by TerminalAppLib.vcxproj's AgentmasterGenerateBuildInfo target, which is
// on the include path. The __has_include guard + fallback defines keep this file compilable if
// the generator hasn't run yet (e.g. opened in an IDE before any build); a real build always
// regenerates the header first (BeforeTargets ClCompile).
#if __has_include("AgentmasterBuildInfo.g.h")
#include "AgentmasterBuildInfo.g.h"
#endif
#ifndef AGENTMASTER_COMMIT_HASH
#define AGENTMASTER_COMMIT_HASH L"unknown"
#endif
#ifndef AGENTMASTER_COMMIT_BRANCH
#define AGENTMASTER_COMMIT_BRANCH L"unknown"
#endif

#include <algorithm>
#include <chrono>
#include <cmath> // std::pow — relative-luminance black/white contrast pick for the card title band
#include <filesystem> // create_directories — the "Create & Launch" affordance for a not-yet-existing dir
#include <system_error> // std::error_code — non-throwing create_directories
#include <thread> // background transcript read for an external's read-only plan
#include <shobjidl.h> // IFileOpenDialog — Browse for claude.exe (native-exe-only policy)

using namespace winrt::Windows::Foundation;
// Using-DECLARATIONS (not a directive) for the color helpers: a `using namespace
// winrt::Windows::UI;` would also pull the nested `Text` namespace into scope and collide
// with our Text() TextBlock helper below.
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Colors;
using winrt::Windows::UI::Core::CoreCursor;
using winrt::Windows::UI::Core::CoreCursorType;
using winrt::Windows::UI::Core::CoreWindow;
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input; // KeyRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue, VirtualKey
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace Agentmaster;
// The shared tooltip recipe (AgentTipHelpers.h) — a using-DECLARATION so the file-scope
// helpers below (e.g. TimingText) can call it unqualified too.
using winrt::TerminalApp::implementation::AgentSetTip;
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (StateColor/Pill/Text/...)

namespace winrt::TerminalApp::implementation
{
    void AgentManagerContent::_CycleKeepAwake()
    {
        switch (_keepAwakeMode)
        {
        case KeepAwakeMode::Off: _keepAwakeMode = KeepAwakeMode::Always; break;
        case KeepAwakeMode::Always: _keepAwakeMode = KeepAwakeMode::WhileRunning; break;
        case KeepAwakeMode::WhileRunning: _keepAwakeMode = KeepAwakeMode::Off; break;
        }
        // Re-evaluate the hold for the new mode immediately (queries the registry for WhileRunning) + repaint.
        _RefreshKeepAwakeHold(nullptr);
    }

    // Compute the DESIRED execution-state hold for the current mode and apply it on a transition only (so a
    // per-_Refresh call while nothing changed is a no-op OS-wise). WhileRunning holds iff a live session is
    // actively Running — counted across the whole fleet (the hold is machine-global, and the Manager shows the
    // whole fleet anyway), so the PC stays awake mid-turn but is allowed to sleep once every agent is at rest.
    // `sessions`, when non-null, is the snapshot _Refresh already fetched (avoids a second Snapshot() copy).
    void AgentManagerContent::_RefreshKeepAwakeHold(const std::vector<::Agentmaster::SessionInfo>* sessions)
    {
        bool desired = false;
        switch (_keepAwakeMode)
        {
        case KeepAwakeMode::Off:
            desired = false;
            break;
        case KeepAwakeMode::Always:
            desired = true;
            break;
        case KeepAwakeMode::WhileRunning:
        {
            const auto anyRunning = [](const std::vector<::Agentmaster::SessionInfo>& v) {
                for (const auto& s : v)
                {
                    if (s.live && s.state == ::Agentmaster::SessionState::Running)
                    {
                        return true;
                    }
                }
                return false;
            };
            if (sessions)
            {
                desired = anyRunning(*sessions);
            }
            else if (_registry)
            {
                desired = anyRunning(_registry->Snapshot());
            }
            break;
        }
        }

        if (desired != _keepAwakeHeld)
        {
            _keepAwakeHeld = desired;
            constexpr DWORD esContinuous = 0x80000000; // ES_CONTINUOUS
            constexpr DWORD esSystem = 0x00000001; // ES_SYSTEM_REQUIRED
            constexpr DWORD esDisplay = 0x00000002; // ES_DISPLAY_REQUIRED
            // Hold: continuous + system + display. Release: continuous alone clears the prior requirements.
            ::SetThreadExecutionState(_keepAwakeHeld ? (esContinuous | esSystem | esDisplay) : esContinuous);
        }
        _UpdateKeepAwakeButton();
    }

    void AgentManagerContent::_UpdateKeepAwakeButton()
    {
        if (!_keepAwakeBtn)
        {
            return;
        }
        // Repaint only on an actual (mode, held) transition — _RefreshKeepAwakeHold calls this on every
        // _Refresh, and rebuilding the content + resource overrides each tick would churn (and flicker).
        if (_keepAwakeRendered && _keepAwakeRenderedMode == _keepAwakeMode && _keepAwakeRenderedHeld == _keepAwakeHeld)
        {
            return;
        }
        _keepAwakeRendered = true;
        _keepAwakeRenderedMode = _keepAwakeMode;
        _keepAwakeRenderedHeld = _keepAwakeHeld;

        const wchar_t* glyph = L"\xE708"; // default (Off): QuietHours-ish moon
        const wchar_t* label = L"Keep Awake";
        switch (_keepAwakeMode)
        {
        case KeepAwakeMode::Off:
            glyph = L"\xE708";
            label = L"Keep Awake";
            break;
        case KeepAwakeMode::Always:
            glyph = L"\xEC46"; // PowerButton
            label = L"Awake On";
            break;
        case KeepAwakeMode::WhileRunning:
            glyph = L"\xE768"; // Play -> "while running"
            label = L"Running Awake";
            break;
        }

        auto content = StackPanel{};
        content.Orientation(Orientation::Horizontal);
        content.Spacing(5);
        FontIcon icon;
        icon.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
        icon.Glyph(glyph);
        icon.FontSize(12); // compact, matching the thinner actions-row buttons
        content.Children().Append(icon);
        content.Children().Append(Text(label, 11, false, 1.0));
        _keepAwakeBtn.Content(content);

        // Color: Off -> theme default; actively holding -> green; WhileRunning but idle (armed, not holding)
        // -> amber, so the user can tell at a glance whether the machine is being kept awake right now. The
        // colored states use PaintHoldButton so the fill (and white text) hold through hover/press instead of
        // reverting to the subtle theme hover brush — base/hover(lighter)/pressed(darker) per state.
        if (_keepAwakeMode == KeepAwakeMode::Off)
        {
            ClearHoldButton(_keepAwakeBtn);
        }
        else if (_keepAwakeHeld)
        {
            PaintHoldButton(_keepAwakeBtn, 0xFF2E7D32, 0xFF3C9A42, 0xFF21601F); // green = holding now
        }
        else
        {
            PaintHoldButton(_keepAwakeBtn, 0xFF8A6D1B, 0xFFA8851F, 0xFF6B5414); // amber = armed (WhileRunning), nothing running
        }
    }

    std::pair<int, int> AgentManagerContent::_DormantCounts() const
    {
        int thisWindow = 0, fleet = 0;
        if (!_registry)
        {
            return { 0, 0 };
        }
        std::unordered_set<std::wstring> localIds;
        const bool haveLocal = static_cast<bool>(_localScopeProvider);
        if (haveLocal)
        {
            localIds = _localScopeProvider();
        }
        for (const auto& s : _registry->Snapshot())
        {
            if (!IsSessionDormant(s))
            {
                continue;
            }
            ++fleet;
            if (!haveLocal || localIds.find(s.id) != localIds.end())
            {
                ++thisWindow;
            }
        }
        return { thisWindow, fleet };
    }

    // Agentmaster (responsive launch bar): stage-collapse the toolbar's top row to the live pane width.
    // Measures each piece's natural width (DesiredSize — DPI/theme-proof) and picks the LEAST-collapsed
    // stage that fits: (A) full label + comfortable box -> (B) drop "launch a"/"session in" -> (C) shrink
    // the cwd box toward its floor (buttons stay inline) -> (D) wrap the launch buttons onto their own line
    // (the box reclaims the freed width). Idempotent + cheap; safe to call on every resize and whenever a
    // button's content/visibility changes. No layout loop — nothing here resizes _root (the pane owns it).
    void AgentManagerContent::_ReflowLaunchBar()
    {
        if (!_launchBar || !_launchBtns || !_cwdBox || !_toolbarCol)
        {
            return;
        }
        const double W = _lastRootWidth > 0.0 ? _lastRootWidth : (_root ? _root.ActualWidth() : 0.0);
        if (W <= 1.0)
        {
            return; // not laid out yet — SizeChanged drives the first real reflow
        }

        // Natural width of an element (0 when collapsed). Measure with a large finite bound (NoWrap text /
        // non-wrapping buttons treat it like infinity), so DesiredSize is the content width regardless of
        // how the element is currently arranged (e.g. _launchBtns while it sits on the wrapped 2nd line).
        const auto desired = [](const FrameworkElement& el) -> double {
            if (!el || el.Visibility() == Visibility::Collapsed)
            {
                return 0.0;
            }
            el.Measure(winrt::Windows::Foundation::Size{ 100000.0f, 100000.0f });
            return el.DesiredSize().Width;
        };

        // The two collapsible words read 0 once hidden, so cache their last visible width — we need it to
        // decide when there's room to UN-collapse them again.
        double laW = desired(_launchAText);
        if (laW > 0.0) { _measLaunchA = laW; } else { laW = _measLaunchA; }
        double siW = desired(_sessionInText);
        if (siW > 0.0) { _measSessionIn = siW; } else { siW = _measSessionIn; }

        const double amW = desired(_agentmasterText);
        const double dashW = desired(_dashText);
        const double togW = desired(_launchAgentBtn);
        const double btnsW = desired(_launchBtns); // Launch + visible Fork/Reopen/Activate + their inner spacing

        constexpr double kGap = 8.0; // _launchBar.Spacing
        constexpr double kMargin = 24.0; // _toolbarCol Margin L(12)+R(12)
        constexpr double kSlack = 12.0; // right-edge breathing room (+ a touch of hysteresis)
        constexpr double kWrapRightReserve = 17.0; // wrapped fill: the box ends this far from the window edge (12px _toolbarCol margin + a 5px gap)
        constexpr double kBoxPref = 504.0; // comfortable box width when there's room
        constexpr double kBoxFloor = 240.0; // smallest inline box (still shows the placeholder / a path tail)
        constexpr double kBoxHardMin = 160.0; // smallest box once the buttons have wrapped away

        const double avail = W - kMargin;

        // Row total = sum(child widths) + (childCount-1)*Spacing. Fixed (non-box) parts per config:
        //   Full inline    : [AM][dash][launchA][toggle][sessionIn] <box> [btns]  -> 6 gaps
        //   Collapsed inline: [AM][dash][toggle] <box> [btns]                     -> 4 gaps
        //   Wrapped (row 1): [AM][dash][toggle] <box FILLS ->|                     -> 3 gaps (btns on row 2)
        const double fullFixed = amW + dashW + laW + togW + siW + btnsW + 6.0 * kGap + kSlack;
        const double collapsedFixed = amW + dashW + togW + btnsW + 4.0 * kGap + kSlack;

        bool labelCollapsed = true;
        bool wrap = false;
        double boxMin = kBoxFloor; // non-wrap: content-grow bounds (the box sizes to its text between these)
        double boxMax = kBoxFloor;
        double fillW = 0.0; // wrap only: the explicit width that fills row 1 to the padded end

        if (avail - fullFixed >= kBoxPref)
        {
            // (A) Full label, comfortable box, buttons inline.
            labelCollapsed = false;
            boxMin = kBoxPref;
            boxMax = avail - fullFixed;
        }
        else if (avail - collapsedFixed >= kBoxPref)
        {
            // (B) Collapse the words; box still comfortable.
            boxMin = kBoxPref;
            boxMax = avail - collapsedFixed;
        }
        else if (avail - collapsedFixed >= kBoxFloor)
        {
            // (C) Shrink the box (floor..pref) but keep the buttons inline & visible.
            boxMin = kBoxFloor;
            boxMax = avail - collapsedFixed;
        }
        else
        {
            // (D) Wrap the buttons onto their own line. Nothing follows the box on row 1 now, so it FILLS
            // the row to the padded end (an explicit width — a horizontal StackPanel won't stretch a child
            // along its axis, so the apply below pins Min==Max==fillW) rather than sit content-sized with a gap.
            // Base the fill on the box's ACTUAL left edge (post-arrange transform, exact) instead of the
            // summed measurements — a small prefix-measurement undershoot was letting the filled box exceed
            // the window. When the words are still visible THIS pass (a direct A->D jump) subtract the width
            // they're about to lose, so the fill is correct immediately (no one-frame over/undershoot).
            wrap = true;
            double boxLeft = kMargin / 2.0 + amW + dashW + togW + 3.0 * kGap; // fallback if the transform fails
            try
            {
                boxLeft = _cwdBox.TransformToVisual(_root).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f }).X;
                if (_launchAText && _launchAText.Visibility() == Visibility::Visible)
                {
                    boxLeft -= laW + siW + 2.0 * kGap; // the two words collapse this pass -> the box shifts left by that much
                }
            }
            catch (...)
            {
            }
            fillW = W - boxLeft - kWrapRightReserve; // box right edge lands at (W - 17): 5px inside the 12px right margin
            if (fillW < kBoxHardMin)
            {
                fillW = kBoxHardMin; // pathologically narrow pane: floor it (may clip) rather than go invisible
            }
        }

        if (boxMax < boxMin)
        {
            boxMax = boxMin;
        }

        // Apply: label visibility (B+ hides the two words), the box width, then the wrap state (move
        // _launchBtns to / from its own line). Inline (A/B/C) the box is content-sized between Min/Max;
        // wrapped (D) it's PINNED (Min==Max==fillW) so it stretches to fill row 1 to the padded end.
        const auto vis = labelCollapsed ? Visibility::Collapsed : Visibility::Visible;
        if (_launchAText && _launchAText.Visibility() != vis) { _launchAText.Visibility(vis); }
        if (_sessionInText && _sessionInText.Visibility() != vis) { _sessionInText.Visibility(vis); }
        if (wrap)
        {
            _cwdBox.MinWidth(fillW);
            _cwdBox.MaxWidth(fillW);
        }
        else
        {
            _cwdBox.MinWidth(boxMin);
            _cwdBox.MaxWidth(boxMax);
        }

        if (wrap != _launchBtnsWrapped)
        {
            if (wrap)
            {
                uint32_t idx = 0;
                if (_launchBar.Children().IndexOf(_launchBtns, idx))
                {
                    _launchBar.Children().RemoveAt(idx);
                }
                _launchBtns.HorizontalAlignment(HorizontalAlignment::Left);
                _launchBtns.VerticalAlignment(VerticalAlignment::Center);
                // -2 top margin tightens _toolbarCol's 6px row Spacing to a 4px gap below the title row
                // (without changing the shared Spacing, which also sets the gap above the actions row).
                _launchBtns.Margin(Thickness{ 0, -2, 0, 0 });
                _toolbarCol.Children().InsertAt(1, _launchBtns); // directly below the title row, above the actions row
            }
            else
            {
                uint32_t idx = 0;
                if (_toolbarCol.Children().IndexOf(_launchBtns, idx))
                {
                    _toolbarCol.Children().RemoveAt(idx);
                }
                _launchBtns.Margin(Thickness{ 0, 0, 0, 0 });
                _launchBtns.HorizontalAlignment(HorizontalAlignment::Left);
                _launchBtns.VerticalAlignment(VerticalAlignment::Center);
                _launchBar.Children().Append(_launchBtns); // back inline at the end of the title row
            }
            _launchBtnsWrapped = wrap;
        }
    }

    void AgentManagerContent::_UpdateActivateAllButton()
    {
        if (!_activateAllBtn)
        {
            return;
        }
        // Headline = THIS window's dormant count (the default, no-prompt action scope). The button hides
        // at 0; cross-window escalation is offered by the click prompt only when other windows also have
        // dormant tabs. Reads SessionInfo::started (maintained by the page from ConnectionState()).
        const auto [thisWindow, fleet] = _DormantCounts();
        (void)fleet;
        _activateAllBtn.Content(winrt::box_value(winrt::hstring{ L"Activate All Tabs (" } + winrt::to_hstring(thisWindow) + L")"));
        _activateAllBtn.Visibility(thisWindow > 0 ? Visibility::Visible : Visibility::Collapsed);
        _ReflowLaunchBar(); // the button just appeared/vanished/relabeled -> re-fit the row
    }

    void AgentManagerContent::_OnActivateAllTabs()
    {
        const auto [thisWindow, fleet] = _DormantCounts();
        if (thisWindow <= 0 && fleet <= 0)
        {
            return; // nothing dormant anywhere
        }
        const int other = fleet - thisWindow;
        if (other <= 0)
        {
            // Only this window has dormant tabs (or it's the only window) -> just wake them, no prompt.
            if (_activateAllHandler)
            {
                _activateAllHandler(false);
            }
            return;
        }
        // Other windows ALSO have dormant tabs -> let the user choose the scope (the user's design:
        // "prompt for choice; if 1 window then automatically just that window").
        _ConfirmChoice(
            L"Activate dormant tabs",
            winrt::hstring{ std::wstring{ L"Start the Claude sessions that haven't initialized yet (restored tabs you haven't opened).\n\nThis window: " } + std::to_wstring(thisWindow) + L"   \x2022   All windows: " + std::to_wstring(fleet) + L"." },
            winrt::hstring{ std::wstring{ L"This window (" } + std::to_wstring(thisWindow) + L")" },
            winrt::hstring{ std::wstring{ L"All windows (" } + std::to_wstring(fleet) + L")" },
            [weak = get_weak()]() { if (auto self = weak.get()) { if (self->_activateAllHandler) { self->_activateAllHandler(false); } } },
            [weak = get_weak()]() { if (auto self = weak.get()) { if (self->_activateAllHandler) { self->_activateAllHandler(true); } } });
    }

    void AgentManagerContent::_UpdateReopenButton()
    {
        if (!_reopenBtn)
        {
            return;
        }
        // Recoverable == saved window records NOT currently open in this process (Engine-tracked).
        // Reads windows/*.json off disk, so keep this on the _Refresh cadence (registry changes), not
        // a hot path. Show the button only when there is something to recover.
        const auto n = static_cast<int>(::Agentmaster::RecoverableWindows().size());
        _reopenBtn.Content(winrt::box_value(winrt::hstring{ L"Reopen Windows (" } + winrt::to_hstring(n) + L")"));
        _reopenBtn.Visibility(n > 0 ? Visibility::Visible : Visibility::Collapsed);
        _ReflowLaunchBar(); // the button just appeared/vanished/relabeled -> re-fit the row
    }

    void AgentManagerContent::_OnReopenWindows()
    {
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] button clicked\n");
        if (!_reopenWindowsHandler)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] no handler bound\n");
            return;
        }
        int n = 0;
        try
        {
            n = static_cast<int>(::Agentmaster::RecoverableWindows().size());
        }
        CATCH_LOG();
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] recoverable N=" + std::to_wstring(n) + L"\n");
        if (n <= 0)
        {
            return;
        }
        const auto reopen = _reopenWindowsHandler;
        _Confirm(L"Reopen saved windows?",
                 winrt::hstring{ L"This reopens " } + winrt::to_hstring(n) + L" previously-saved window(s) at their saved position and layout. Their sessions stay archived until you restore them.",
                 L"Reopen",
                 [reopen]() {
                     ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] confirm accepted -> handler\n");
                     if (reopen)
                     {
                         reopen();
                     }
                 });
    }

    void AgentManagerContent::_BuildSettingsOverlay()
    {
        // A dimmed, full-bleed modal layer over the whole Manager. Deliberately NOT a
        // ContentDialog: a text box inside a ContentDialog gets no keypresses in XAML Islands
        // (see the _renameBox note), and this surface has free-text fields (model, dir, count).
        // Living in the main visual tree, its TextBoxes behave normally.
        _settingsOverlay = Grid{};
        _settingsOverlay.Visibility(Visibility::Collapsed);
        _settingsOverlay.Background(SolidColorBrush{ ColorHelper::FromArgb(0xA0, 0x00, 0x00, 0x00) });
        Grid::SetRow(_settingsOverlay, 0);
        Grid::SetRowSpan(_settingsOverlay, 99); // cover every row of _root regardless of count
        Grid::SetColumnSpan(_settingsOverlay, 99);
        // Click on the backdrop = Cancel; the card swallows taps so inside-clicks don't close.
        _settingsOverlay.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
            _HideSettings();
        });

        auto card = Border{};
        card.Background(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x25, 0x25, 0x25) });
        card.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0x80, 0x80, 0x80) });
        card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        card.Padding(Thickness{ 20, 16, 20, 16 });
        card.Width(560); // Agentmaster: wider so the six top-tab buttons fit on one row
        card.HorizontalAlignment(HorizontalAlignment::Center);
        card.VerticalAlignment(VerticalAlignment::Center);
        card.RequestedTheme(ElementTheme::Dark);
        card.Tapped([](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs& e) {
            e.Handled(true);
        });

        // Agentmaster: the cog is organized into TOP TABS — a horizontal button-tab strip swapping one
        // scrollable panel per settings group (the _SwitchEnvTab idiom; deliberately NOT a Pivot, which
        // themes unreliably under XAML Islands). Save/Cancel is a fixed footer OUTSIDE the tabs (always
        // reachable). Each section below appends into one of the six group panels; the `panel` variable is
        // RESEATED at every group boundary (a StackPanel is a ref-counted handle, so `panel = claudePanel`
        // just re-points it) so the per-control creation code stays byte-for-byte identical to before.
        auto outer = StackPanel{};
        outer.Spacing(8);
        outer.Children().Append(Text(L"Agentmaster Settings", 18, true, 1.0));

        // The six group panels (built empty; filled by the sections below, in this label order).
        auto sessionsPanel = StackPanel{};
        sessionsPanel.Spacing(10);
        auto autorunnerPanel = StackPanel{};
        autorunnerPanel.Spacing(10);
        auto behaviorPanel = StackPanel{};
        behaviorPanel.Spacing(10);
        auto tabsPanel = StackPanel{};
        tabsPanel.Spacing(10);
        auto claudePanel = StackPanel{};
        claudePanel.Spacing(10);
        auto aboutPanel = StackPanel{};
        aboutPanel.Spacing(10);

        // The tab strip + the single content scroller. _settingsTabButtons / _settingsTabPanels are
        // parallel-indexed for _SwitchSettingsTab, which swaps panel i into _settingsScroll.
        _settingsTabButtons.clear();
        _settingsTabPanels.clear();
        auto tabStrip = StackPanel{};
        tabStrip.Orientation(Orientation::Horizontal);
        tabStrip.Spacing(0);
        tabStrip.Margin(Thickness{ 0, 2, 0, 0 });
        // ONE scroller whose Content is SWAPPED to the selected tab's panel (the ContentPresenter-swap
        // idiom the user asked for — not six overlapping ScrollViewers). _SwitchSettingsTab sets
        // _settingsScroll.Content(panel), which cleanly detaches the previous panel (single parent) and
        // re-projects the new one — so a panel is never double-parented and never renders as an empty
        // ContentPresenter. Content is only ever set in _SwitchSettingsTab, AFTER the panels are populated.
        _settingsScroll = ScrollViewer{};
        _settingsScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _settingsScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        _settingsScroll.MaxHeight(470);
        const auto addSettingsTab = [&](const wchar_t* label, const wchar_t* tip, const StackPanel& body) {
            const int index = static_cast<int>(_settingsTabButtons.size());
            auto btn = Button{};
            btn.Content(winrt::box_value(winrt::hstring{ label }));
            btn.FontSize(12);
            btn.Padding(Thickness{ 12, 3, 12, 3 });
            btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            AgentSetTip(btn, winrt::hstring{ tip });
            btn.Click([this, index](const IInspectable&, const RoutedEventArgs&) { _SwitchSettingsTab(index); });
            tabStrip.Children().Append(btn);
            _settingsTabButtons.push_back(btn);
            _settingsTabPanels.push_back(body); // _SwitchSettingsTab swaps the selected panel into _settingsScroll
        };

        // The sections below run top-to-bottom; `panel` starts on About (the version + updates block leads
        // it) and is reseated at each group boundary. The tab strip + scroller are assembled below, after
        // the panels are filled (so the first _SwitchSettingsTab swaps in a populated panel).
        auto panel = aboutPanel;

        // Agentmaster: build identity line at the very top — the release VERSION (read live from the
        // package manifest; the release pipeline stamps Package-Rel.appxmanifest, a dev loose layout
        // shows its own manifest version), the git COMMIT this build was made from (build-stamped via
        // AgentmasterBuildInfo.g.h), and whether this is the RELEASE or DEV install (package family:
        // Agentmaster vs AgentmasterDev) plus the compile CONFIGURATION (Debug/Release). Full detail
        // (branch, package family name) rides a hover tooltip.
        {
            std::wstring version{ L"?" };
            try
            {
                version = std::wstring{ CascadiaSettings::ApplicationVersion() };
            }
            CATCH_LOG();

            const std::wstring pfn = ::Agentmaster::Profiles::PackageFamilyName();
            const std::wstring channel = pfn.empty()                                 ? std::wstring{ L"Unpackaged" } :
                                         ::Agentmaster::Profiles::IsDevPackage()     ? std::wstring{ L"Dev" } :
                                                                                       std::wstring{ L"Release" };
#if defined(_DEBUG)
            const std::wstring config{ L"Debug" };
#else
            const std::wstring config{ L"Release" };
#endif
            const std::wstring commit{ AGENTMASTER_COMMIT_HASH };
            const std::wstring branch{ AGENTMASTER_COMMIT_BRANCH };

            // e.g. "v0.0.1.0  ·  3c4e2a942  ·  Dev · Debug"  (· = U+00B7, always trailed by a space
            // so the \x00B7 hex escape can't swallow a following hex digit).
            const std::wstring line = L"v" + version + L"  \x00B7  " + commit + L"  \x00B7  " + channel + L" \x00B7 " + config;
            auto sub = Text(winrt::hstring{ line }, 12, false, 0.6);

            std::wstring tip = L"Version " + version + L"\nCommit " + commit + L" (" + branch + L")\nChannel " + channel;
            if (!pfn.empty())
            {
                tip += L" (" + pfn + L")";
            }
            tip += L"\nConfiguration " + config;
            AgentSetTip(sub, winrt::hstring{ tip });
            panel.Children().Append(sub);
        }

        // UPDATES (Agentmaster updater; Updater.h) — check GitHub Releases for a newer version
        // (the same prompt the startup check shows) + the pre-release opt-in. The status label
        // beside the button shows "vX.Y.Z available!" in dark green when a newer release exists
        // (filled by a silent check kicked when the cog opens — see _ShowSettings/_CheckForUpdates).
        panel.Children().Append(Text(L"UPDATES", 11, true, 0.6));
        {
            auto row = StackPanel{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(8);
            row.VerticalAlignment(VerticalAlignment::Center);
            _setCheckUpdates = Button{};
            _setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
            AgentSetTip(_setCheckUpdates, L"Check GitHub for a newer Agentmaster release, then choose to update now, postpone (3 / 7 / 30 days), or skip this version.");
            _setCheckUpdates.Click([this](const IInspectable&, const RoutedEventArgs&) { _CheckForUpdates(true); });
            row.Children().Append(_setCheckUpdates);

            // "Current version changelog" — opens THIS build's GitHub release page (the changelog fixated
            // on the installed version). Always shown; the URL is fixed for the process lifetime. Opened
            // off-thread (a browser launch can stall) like the per-tab overlay's Open-Path.
            const std::wstring curChangelogUrl =
                ::Agentmaster::Updater::ReleasePageForTag(::Agentmaster::Updater::VersionToString(::Agentmaster::Updater::CurrentPackageVersion()));
            _setCurrentChangelog = HyperlinkButton{};
            _setCurrentChangelog.Content(winrt::box_value(L"Current version changelog"));
            _setCurrentChangelog.Padding(Thickness{ 4, 2, 4, 2 });
            _setCurrentChangelog.FontSize(12);
            AgentSetTip(_setCurrentChangelog, L"Open the GitHub release notes for the version you're running");
            _setCurrentChangelog.Click([curChangelogUrl](const IInspectable&, const RoutedEventArgs&) {
                if (!curChangelogUrl.empty())
                {
                    std::thread([curChangelogUrl]() { ::ShellExecuteW(nullptr, L"open", curChangelogUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }).detach();
                }
            });
            row.Children().Append(_setCurrentChangelog);

            // "Update's changelog" — opens the AVAILABLE update's release page; revealed only after a
            // check finds one (_lastUpdateChangelogUrl + Visibility set in _CheckForUpdates' completion).
            _setUpdateChangelog = HyperlinkButton{};
            _setUpdateChangelog.Content(winrt::box_value(L"Update's changelog"));
            _setUpdateChangelog.Padding(Thickness{ 4, 2, 4, 2 });
            _setUpdateChangelog.FontSize(12);
            _setUpdateChangelog.Visibility(Visibility::Collapsed);
            AgentSetTip(_setUpdateChangelog, L"Open the GitHub release notes for the available update");
            _setUpdateChangelog.Click([this](const IInspectable&, const RoutedEventArgs&) {
                const std::wstring u = _lastUpdateChangelogUrl;
                if (!u.empty())
                {
                    std::thread([u]() { ::ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }).detach();
                }
            });
            row.Children().Append(_setUpdateChangelog);
            panel.Children().Append(row);
        }
        // Status label ("vX.Y.Z available!" dark green / "up to date" / "Checking…") on its OWN row
        // beneath the action row — kept off the action row so the button + the two changelog links fit
        // the 460-wide card without clipping (the cog's ScrollViewer scrolls vertically only).
        _setUpdateStatus = TextBlock{};
        _setUpdateStatus.Opacity(0.9);
        _setUpdateStatus.FontSize(12);
        _setUpdateStatus.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setUpdateStatus);

        _setAllowPrerelease = ToggleSwitch{};
        _setAllowPrerelease.Header(winrt::box_value(L"Allow updating to pre-release versions"));
        AgentSetTip(_setAllowPrerelease, L"When on, update checks also consider GitHub pre-releases (beta builds), not just stable releases. Off by default.");
        panel.Children().Append(_setAllowPrerelease);

        // (The "Uninstall Agentmaster…" button is built at the END of the About tab, after the Profile row.)

        // === SESSIONS tab ===
        panel = sessionsPanel;
        // CLAUDE SESSIONS
        panel.Children().Append(Text(L"CLAUDE SESSIONS", 11, true, 0.6));
        _setSkipPermissions = ToggleSwitch{};
        _setSkipPermissions.Header(winrt::box_value(L"Skip permission prompts (bypass)"));
        AgentSetTip(_setSkipPermissions, L"Launch new sessions with --dangerously-skip-permissions \x2014 auto-accepts tool prompts and the per-folder trust dialog so an unattended session never wedges. Off pins normal prompts instead.");
        panel.Children().Append(_setSkipPermissions);
        _setModel = TextBox{};
        _setModel.Header(winrt::box_value(L"Model"));
        _setModel.PlaceholderText(L"As Is \x2014 blank keeps Claude's default (e.g. opus / sonnet)");
        AgentSetTip(_setModel, L"Model new sessions launch with (like /model) \x2014 e.g. opus or sonnet. Blank keeps Claude's own default.");
        panel.Children().Append(_setModel);
        _setIncludeCoAuthored = ToggleSwitch{};
        _setIncludeCoAuthored.Header(winrt::box_value(L"Include co-authored-by in commits"));
        AgentSetTip(_setIncludeCoAuthored, L"When off, commits Claude makes omit the \x201C" L"Co-authored-by\x201D trailer. Applies to new sessions.");
        panel.Children().Append(_setIncludeCoAuthored);
        // ENV_VARS.md: the two-tab "Environment variables" area (Global / Per-directory) replaces the old
        // single ;-delimited box. Built in its own method to keep this builder readable + the diff local.
        _BuildEnvVarsArea(panel);

        // === CLAUDE tab ===
        panel = claudePanel;
        // CLAUDE HISTORY (ENV_VARS.md §8): cleanupPeriodDays lives in the user's GLOBAL ~/.claude/settings.json
        // (NOT an Agentmaster setting, NOT an env var). Read on open / written on save through the
        // ClaudeUserSettings repository (a managed layer that preserves every other key in that file). 36500
        // (~100y) ships by default so Claude never purges global history; blank removes our key (Claude's
        // 30-day default). A separate field from the env area on purpose — it's a Claude settings key, not env.
        panel.Children().Append(Text(L"CLAUDE HISTORY", 11, true, 0.6));
        _setCleanupDays = TextBox{};
        _setCleanupDays.Header(winrt::box_value(L"Keep Claude history (days)"));
        _setCleanupDays.PlaceholderText(L"e.g. 36500 (~never) \x2014 blank = Claude default (30 days)");
        AgentSetTip(_setCleanupDays, L"Sets cleanupPeriodDays in your global ~/.claude/settings.json \x2014 how many days Claude keeps session transcripts before deleting them at startup (your GLOBAL Claude history, used by Resume + the Sessions browser). 36500 \x2248 never. Blank removes the key (Claude's 30-day default). Avoid 0: in Claude it DISABLES history entirely.");
        panel.Children().Append(_setCleanupDays);

        // CLAUDE BINARY (native-exe-only policy): the auto-detected native claude.exe + an optional
        // explicit override. The whole app gates launch/fork/resume on resolving one (ResolveClaudeExe);
        // the override must be a real *.exe (a .cmd/.bat or the Node CLI is rejected).
        panel.Children().Append(Text(L"CLAUDE BINARY (native build required)", 11, true, 0.6));
        _setClaudeDetected = TextBlock{};
        _setClaudeDetected.TextWrapping(TextWrapping::Wrap);
        _setClaudeDetected.Opacity(0.85);
        _setClaudeDetected.FontSize(12);
        AgentSetTip(_setClaudeDetected, L"The native claude.exe Agentmaster resolved (PATH \xB7 %USERPROFILE%\\.local\\bin \xB7 behind an npm claude.cmd). The fleet view drives this binary, so a pure-Node Claude is unsupported \x2014 launch / resume / fork stay disabled until one is found.");
        panel.Children().Append(_setClaudeDetected);
        _setClaudeExePath = TextBox{};
        _setClaudeExePath.Header(winrt::box_value(L"Override claude.exe path"));
        _setClaudeExePath.PlaceholderText(L"blank \x2014 auto-detect; or a full path to claude.exe");
        AgentSetTip(_setClaudeExePath, L"Force a specific claude.exe instead of auto-detecting \x2014 must be a real *.exe (a .cmd / .bat or the Node CLI is rejected). Blank auto-detects.");
        panel.Children().Append(_setClaudeExePath);
        {
            auto browse = Button{};
            browse.Content(winrt::box_value(L"Browse for claude.exe\x2026"));
            AgentSetTip(browse, L"Pick claude.exe with a file dialog \x2014 sets the override above and re-resolves the binary immediately, no restart.");
            browse.Click([this](const IInspectable&, const RoutedEventArgs&) { _BrowseForClaudeExe(true); });
            panel.Children().Append(browse);
        }

        // === TESTS AUTORUNNER tab ===
        panel = autorunnerPanel;
        // TESTS AUTORUNNER
        panel.Children().Append(Text(L"TESTS AUTORUNNER (defaults for new sessions)", 11, true, 0.6));
        _setDefaultMode = ComboBox{};
        _setDefaultMode.Header(winrt::box_value(L"New-session mode"));
        _setDefaultMode.Items().Append(winrt::box_value(L"Off"));
        _setDefaultMode.Items().Append(winrt::box_value(L"SemiAuto"));
        _setDefaultMode.Items().Append(winrt::box_value(L"Full"));
        AgentSetTip(_setDefaultMode, L"Tests Autorunner mode each new session starts in \x2014 Off (manual) \xB7 SemiAuto (you confirm each send) \xB7 Full (auto-send the queue on turn-complete). Per-session, changeable from the Auto Testing pane.");
        panel.Children().Append(_setDefaultMode);
        _setMaxAutoSends = TextBox{};
        _setMaxAutoSends.Header(winrt::box_value(L"Max auto-sends per run"));
        _setMaxAutoSends.PlaceholderText(L"100");
        AgentSetTip(_setMaxAutoSends, L"Backstop cap on how many prompts Tests Autorunner may auto-send in one run before stopping. Blank or 0 resets to 100.");
        panel.Children().Append(_setMaxAutoSends);
        _setStopOnError = ToggleSwitch{};
        _setStopOnError.Header(winrt::box_value(L"Stop on error"));
        AgentSetTip(_setStopOnError, L"When on, Tests Autorunner halts a session's queue as soon as it enters the Error state instead of sending the next prompt.");
        panel.Children().Append(_setStopOnError);
        _setPauseOnHuman = ToggleSwitch{};
        _setPauseOnHuman.Header(winrt::box_value(L"Pause on human input"));
        AgentSetTip(_setPauseOnHuman, L"When on, typing into a session's terminal yourself pauses its Tests Autorunner so a manual interruption isn't overwritten by the next queued send.");
        panel.Children().Append(_setPauseOnHuman);

        // === BEHAVIOR tab ===
        panel = behaviorPanel;
        // BEHAVIOR
        panel.Children().Append(Text(L"BEHAVIOR", 11, true, 0.6));
        _setConfirmKill = ToggleSwitch{};
        _setConfirmKill.Header(winrt::box_value(L"Confirm before closing a session"));
        AgentSetTip(_setConfirmKill, L"When on, closing a session (tab X, the tree's Del, or the Close menu) first asks to confirm. Off closes without the prompt. Closing always keeps the session in Sessions, resumable \x2014 nothing on disk is deleted either way.");
        panel.Children().Append(_setConfirmKill);
        // How the tab/session rename box commits via the keyboard. Clicking away (focus loss) ALWAYS
        // commits; this only governs the Enter / Shift+Enter shortcut. The box is multi-line, so the
        // key that ISN'T the commit key inserts a newline. GLOBAL across windows (TabRenameCommitMode).
        _setRenameCommit = ComboBox{};
        _setRenameCommit.Header(winrt::box_value(L"Tab rename: commit with"));
        _setRenameCommit.Items().Append(winrt::box_value(L"Click away only"));
        _setRenameCommit.Items().Append(winrt::box_value(L"Click away + Shift+Enter"));
        _setRenameCommit.Items().Append(winrt::box_value(L"Click away + Enter"));
        AgentSetTip(_setRenameCommit, L"Which key commits a tab rename: Shift+Enter (default) or Enter. The other inserts a line break (titles can be multi-line); clicking away always commits.");
        panel.Children().Append(_setRenameCommit);
        // Agentmaster (Waiting-for-you "unread" model): the Waiting-for-you -> Idle timeout. A "Never"
        // toggle (stay Waiting until read), else a slider 1m .. 3d (default 1h). This is the "unread
        // inbox" lifetime; it was split from Claude's ~5-min server cache, which is now the separate
        // "Server-side cache lifetime" below (driving only the card's ⚡ hint).
        _setWaitingNever = ToggleSwitch{};
        _setWaitingNever.Header(winrt::box_value(L"Never decay Waiting-for-you (keep until read)"));
        AgentSetTip(_setWaitingNever, L"When on, a Waiting-for-you session never auto-demotes to Idle by time \x2014 it stays until you read (visit) its tab. When off, it decays after the timeout below (and only once you've read it).");
        _setWaitingNever.Toggled([this](const IInspectable&, const RoutedEventArgs&) {
            if (_setWaitingNever)
            {
                const bool never = _setWaitingNever.IsOn();
                if (_setWaitingDecaySlider)
                {
                    _setWaitingDecaySlider.IsEnabled(!never);
                }
                if (_setWaitingDecayText) // the box mirrors the slider's enabled state
                {
                    _setWaitingDecayText.IsEnabled(!never);
                }
            }
        });
        panel.Children().Append(_setWaitingNever);

        // Agentmaster: the timeout as a compact "12h5m" free-text BOX to the LEFT of a slider. The box is
        // the source of truth (parsed on Save) and may exceed the slider's 7d max — the slider then sits
        // maxed. The two stay in lockstep via _waitingDecaySyncing (suppresses the TextChanged/ValueChanged
        // echo). A normally-transparent ring around the box turns red when the text can't be parsed; the
        // box's tooltip lists the accepted syntax with examples.
        {
            auto waitWrap = StackPanel{};
            auto waitHeader = Text(L"Waiting-for-you \x2192 Idle after", 13, false, 0.9);
            waitHeader.Margin(Thickness{ 0, 0, 0, 6 });
            waitWrap.Children().Append(waitHeader);

            auto waitRow = Grid{};
            {
                ColumnDefinition cBox;
                cBox.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto)); // the box hugs its content
                ColumnDefinition cSlider;
                cSlider.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); // the slider fills the rest
                waitRow.ColumnDefinitions().Append(cBox);
                waitRow.ColumnDefinitions().Append(cSlider);
            }

            // The red-on-error ring: an outer Border (transparent at rest) around the textbox, so the
            // indicator survives the TextBox's own PointerOver/Focused visual states (which would override a
            // BorderBrush set directly on the box while the user is typing in it).
            _setWaitingDecayBorder = Border{};
            _setWaitingDecayBorder.BorderThickness(Thickness{ 1, 1, 1, 1 });
            _setWaitingDecayBorder.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            _setWaitingDecayBorder.BorderBrush(SolidColorBrush{ Colors::Transparent() });
            _setWaitingDecayBorder.VerticalAlignment(VerticalAlignment::Center);
            _setWaitingDecayBorder.Margin(Thickness{ 0, 0, 12, 0 });

            _setWaitingDecayText = TextBox{};
            _setWaitingDecayText.Width(96);
            AgentSetTip(_setWaitingDecayText, L"How long a READ Waiting-for-you session waits before it may demote to Idle. Type a duration \x2014 combine days/hours/minutes:  d = days, h = hours, m = minutes  (a bare number = minutes).\n\nExamples:   3d   \x00B7   12h5m   \x00B7   2d4h30m   \x00B7   90m   \x00B7   45m   \x00B7   120 (= 2h)\n\nThe slider tops out at 7d, but you can type more here (it then sits maxed). The border turns red if the text can't be read. Use the \x201CNever\x201D toggle above to never decay.");
            _setWaitingDecayText.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) {
                if (_waitingDecaySyncing || !_setWaitingDecayText)
                {
                    return;
                }
                uint32_t mins = 0;
                if (!ParseDurationToMinutes(std::wstring{ _setWaitingDecayText.Text() }, mins))
                {
                    if (_setWaitingDecayBorder) // unparsable -> red ring; leave the slider where it was
                    {
                        _setWaitingDecayBorder.BorderBrush(Fill(0xFF, 0xE5, 0x39, 0x35));
                    }
                    return;
                }
                if (_setWaitingDecayBorder) // valid -> clear the red ring
                {
                    _setWaitingDecayBorder.BorderBrush(SolidColorBrush{ Colors::Transparent() });
                }
                if (_setWaitingDecaySlider)
                {
                    // Mirror into the slider, clamped to its 1..7d range; a bigger value sits maxed.
                    double sv = static_cast<double>(std::min<uint32_t>(mins, 10080));
                    if (sv < 1)
                    {
                        sv = 1;
                    }
                    _waitingDecaySyncing = true;
                    _setWaitingDecaySlider.Value(sv);
                    _waitingDecaySyncing = false;
                }
            });
            _setWaitingDecayBorder.Child(_setWaitingDecayText);
            Grid::SetColumn(_setWaitingDecayBorder, 0);
            waitRow.Children().Append(_setWaitingDecayBorder);

            _setWaitingDecaySlider = Slider{};
            _setWaitingDecaySlider.Minimum(1); // 1 minute
            _setWaitingDecaySlider.Maximum(10080); // 7 days
            _setWaitingDecaySlider.StepFrequency(1);
            _setWaitingDecaySlider.VerticalAlignment(VerticalAlignment::Center);
            AgentSetTip(_setWaitingDecaySlider, L"How long a Waiting-for-you session waits before it may demote to Idle \x2014 1 minute \x2026 7 days. It only demotes once you've READ it (an unread session keeps waiting past the timeout). The box on the left mirrors this and can go beyond 7 days. Use the toggle above for \x201Cnever\x201D.");
            _setWaitingDecaySlider.ValueChanged([this](const IInspectable&, const Primitives::RangeBaseValueChangedEventArgs&) {
                if (_waitingDecaySyncing || !_setWaitingDecaySlider || !_setWaitingDecayText)
                {
                    return;
                }
                // Dragging the slider is always a valid value -> mirror it into the box + clear any red ring.
                _waitingDecaySyncing = true;
                _setWaitingDecayText.Text(winrt::hstring{ FormatMinutesCompact(static_cast<uint32_t>(_setWaitingDecaySlider.Value())) });
                if (_setWaitingDecayBorder)
                {
                    _setWaitingDecayBorder.BorderBrush(SolidColorBrush{ Colors::Transparent() });
                }
                _waitingDecaySyncing = false;
            });
            Grid::SetColumn(_setWaitingDecaySlider, 1);
            waitRow.Children().Append(_setWaitingDecaySlider);

            waitWrap.Children().Append(waitRow);
            panel.Children().Append(waitWrap);
        }

        _setServerCache = TextBox{};
        _setServerCache.Header(winrt::box_value(L"Server-side cache lifetime (minutes)"));
        _setServerCache.PlaceholderText(L"5");
        AgentSetTip(_setServerCache, L"How long after a turn Claude's server-side prompt cache stays warm \x2014 drives the card's \x26A1 \x201Cstill cached\x201D hint (a follow-up within the window is cheaper & faster). Default 5.");
        panel.Children().Append(_setServerCache);

        // The Launch box's directory settings belong with SESSIONS (they shape launching), so append them
        // there even though they sit inside the BEHAVIOR section in source.
        panel = sessionsPanel;
        _setLaunchDir = TextBox{};
        _setLaunchDir.Header(winrt::box_value(L"Default Launch directory"));
        _setLaunchDir.PlaceholderText(L"blank \x2014 defaults to %USERPROFILE%");
        AgentSetTip(_setLaunchDir, L"Directory the Launch box is pre-filled with. Blank defaults to %USERPROFILE%.");
        panel.Children().Append(_setLaunchDir);
        _setRecentDirsLimit = TextBox{};
        _setRecentDirsLimit.Header(winrt::box_value(L"Recent Launch directories to remember"));
        _setRecentDirsLimit.PlaceholderText(L"10");
        AgentSetTip(_setRecentDirsLimit, L"How many recently-used directories the Launch box's path-picker keeps in its history. Blank or 0 resets to 10.");
        panel.Children().Append(_setRecentDirsLimit);

        // (back to the BEHAVIOR tab)
        panel = behaviorPanel;
        // Sessions browser: un-hide every session removed via the Sessions page's right-click
        // "Hide from list". The list lives in AppSettings.hiddenSessionIds, owned by the page
        // (TerminalPage), so this fires the handler there rather than reading a count the cog
        // doesn't track; the button gives inline confirmation. Re-enabled/relabeled per _ShowSettings.
        _setResetHidden = Button{};
        _setResetHidden.Content(winrt::box_value(L"Reset hidden sessions"));
        AgentSetTip(_setResetHidden, L"Un-hide every session you removed from the Sessions browser with \x201CHide from list\x201D");
        _setResetHidden.Click([this](const IInspectable& sender, const RoutedEventArgs&) {
            ::Agentmaster::LogNav(L"reset-hidden-sessions (un-hide every Sessions-browser row)"); // Nav audit: the cog "Reset hidden sessions"
            if (_resetHiddenSessionsHandler)
            {
                _resetHiddenSessionsHandler();
            }
            // The list lives in TerminalPage; confirm locally (the cog doesn't track the count).
            if (const auto b = sender.try_as<Button>())
            {
                b.Content(winrt::box_value(L"Hidden sessions cleared"));
                b.IsEnabled(false);
            }
        });
        panel.Children().Append(_setResetHidden);

        // === TABS & OVERLAY tab ===
        panel = tabsPanel;
        // TABS — close affordances on the terminal tab strip (GLOBAL across windows, applied live
        // on Save via TerminalPage::_updateAllTabCloseButtons + the cross-window broadcast).
        panel.Children().Append(Text(L"TABS", 11, true, 0.6));
        _setShowTabCloseButton = ToggleSwitch{};
        _setShowTabCloseButton.Header(winrt::box_value(L"Show close (\x00D7) button on tabs"));
        AgentSetTip(_setShowTabCloseButton, L"When off, the close (\x00D7) button is hidden on every tab (you can still close with the tab's right-click menu, the middle-mouse button below, or Ctrl+Shift+W). The pinned Manager tab is always X-less. Default on.");
        panel.Children().Append(_setShowTabCloseButton);
        _setCloseTabOnMiddleClick = ToggleSwitch{};
        _setCloseTabOnMiddleClick.Header(winrt::box_value(L"Close tab with middle-mouse click"));
        AgentSetTip(_setCloseTabOnMiddleClick, L"When off, middle-clicking a tab no longer closes it \x2014 handy if you keep closing tabs by accident. Default on.");
        panel.Children().Append(_setCloseTabOnMiddleClick);
        _setAlwaysShowHomeButton = ToggleSwitch{};
        _setAlwaysShowHomeButton.Header(winrt::box_value(L"Always display Home button"));
        AgentSetTip(_setAlwaysShowHomeButton, L"Keep the tab-strip \x201CHome\x201D button (jump to the pinned Agent Manager tab) visible whenever you're on another tab. When off, it appears only once the Manager tab has scrolled out of view. Default on.");
        panel.Children().Append(_setAlwaysShowHomeButton);
        // FAVORITES.md §5a: which glyph marks a FAVORITE (starred) session on its live tab — Crown
        // (default, a gold crown at the status dot's NW) or Star (the status dot foregrounded on a white,
        // golden-tipped star). GLOBAL across windows; applied live on Save + cross-window broadcast.
        _setFavoriteIcon = ComboBox{};
        _setFavoriteIcon.Header(winrt::box_value(L"Favorite marker"));
        _setFavoriteIcon.Items().Append(winrt::box_value(L"Crown")); // index 0 == FavoriteIcon::Crown (default)
        _setFavoriteIcon.Items().Append(winrt::box_value(L"Star")); // index 1 == FavoriteIcon::Star
        AgentSetTip(_setFavoriteIcon, L"The marker shown on a favorited (\x2605) session's live tab, over its status dot \x2014 Crown (default, a small gold crown at the dot's corner) or Star (the status dot becomes the centre of a white, golden-tipped star).");
        panel.Children().Append(_setFavoriteIcon);
        // TABS (bookmark tags): the GLOBAL cap on distinct tags (the tab context menu's "Tag" panel
        // refuses to create a NEW name past it; existing tags are never dropped by lowering it).
        // Default 20, hard ceiling 40 — ClampMaxTags is shared with the Persistence load, so a
        // hand-edited settings.json self-heals identically.
        _setMaxTags = TextBox{};
        _setMaxTags.Header(winrt::box_value(L"Max bookmark tags (global)"));
        _setMaxTags.PlaceholderText(L"20");
        AgentSetTip(_setMaxTags, L"How many distinct bookmark tags may exist across all sessions (the tab right-click \x2192 Tag panel). Blank or 0 resets to 20; capped at 40. Tags already applied are never removed by lowering this.");
        panel.Children().Append(_setMaxTags);

        // TABS (bookmark tags): the OPACITY of the tag chips in the rich tab TOOLTIP (each tag's name
        // over its colored underscore), 10..100%. GLOBAL (AppSettings::tooltipTagsOpacity); default
        // 90% (0.9). Only the tooltip's tag row honors it — the tab-strip badge ribbons + the
        // Sessions Tags column stay fully solid (they're the primary affordance). The header shows
        // the live percent; the value is read back on Save (÷100 -> 0..1, ClampTooltipTagsOpacity).
        _setTooltipTagsOpacity = Slider{};
        _setTooltipTagsOpacity.Minimum(10);
        _setTooltipTagsOpacity.Maximum(100);
        _setTooltipTagsOpacity.StepFrequency(1);
        _setTooltipTagsOpacity.Header(winrt::box_value(L"Tag opacity in tooltip \x2014 90%"));
        AgentSetTip(_setTooltipTagsOpacity, L"How solid the bookmark-tag chips look in a tab's hover tooltip \x2014 10% (faint) to 100% (fully solid). Default 90%. Affects the tooltip only; the tab's bookmark ribbons and the Sessions Tags column stay solid.");
        _setTooltipTagsOpacity.ValueChanged([this](const IInspectable&, const Primitives::RangeBaseValueChangedEventArgs&) {
            if (_setTooltipTagsOpacity)
            {
                const int pct = static_cast<int>(_setTooltipTagsOpacity.Value() + 0.5);
                _setTooltipTagsOpacity.Header(winrt::box_value(winrt::hstring{ L"Tag opacity in tooltip \x2014 " + std::to_wstring(pct) + L"%" }));
            }
        });
        panel.Children().Append(_setTooltipTagsOpacity);

        // Tab color modes: HOW managed tabs get their color — shared per working dir (the classic
        // Rule-#12 behavior, default), individual per tab, or shared per the INFERRED working dir
        // (detected from the files the session reads/edits/creates). GLOBAL (AppSettings::tabColorMode);
        // applied live on Save + cross-window broadcast (TerminalPage::_ReapplyManagedTabColors).
        _setTabColorMode = ComboBox{};
        _setTabColorMode.Header(winrt::box_value(L"Tab coloring"));
        _setTabColorMode.Items().Append(winrt::box_value(L"Shared per working directory")); // index 0 == TabColorMode::WorkingDirectory (default)
        _setTabColorMode.Items().Append(winrt::box_value(L"Individual per tab")); // index 1 == TabColorMode::Individual
        _setTabColorMode.Items().Append(winrt::box_value(L"Inferred working directory")); // index 2 == TabColorMode::InferredWorkingDirectory
        AgentSetTip(_setTabColorMode, L"How session tabs are colored.\n\x2022 Shared per working directory (default): every tab launched in a folder wears that folder's permanent color; picking a color recolors the whole folder.\n\x2022 Individual per tab: each session gets its own color (kept across close/reopen); picking a color changes only that tab.\n\x2022 Inferred working directory: like shared-per-directory, but keyed by the directory the session ACTUALLY works in \x2014 the deepest folder most of the files it reads/edits/creates share \x2014 re-detected as the session works, so a session that settles into one subtree takes that subtree's color.");
        panel.Children().Append(_setTabColorMode);

        // TABS: the "status flashing color" — color (and OPACITY) of the unread FLASH RING that pulses
        // around a managed session's tab status dot when it leaves Running for a needs-you state on an
        // unvisited tab (and the manual "Mark Unread" ring). To keep the cog SHORT, this is a COMPACT
        // swatch button (a preview of the current color) that opens the muxc::ColorPicker in a FLYOUT —
        // the full picker is huge inline, so it only appears on demand (the Windows Terminal tab-color
        // idiom). The picker's ALPHA slider IS the OPACITY control (one control sets hue + opacity).
        // GLOBAL (AppSettings::flashRingColor, "#AARRGGBB"); applied live on Save + cross-window
        // broadcast (TerminalPage::_RefreshFlashRingBrush). Default red at 80% opacity (#CCFF0000).
        {
            auto row = StackPanel{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(10);
            row.VerticalAlignment(VerticalAlignment::Center);

            auto label = Text(L"Status flashing color", 13, false, 0.9);
            label.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(label);

            // The swatch preview — shows the current color (its opacity blends over the dark card); a
            // thin light border keeps a fully-transparent pick visible as an outlined box.
            _flashRingSwatch = Border{};
            _flashRingSwatch.Width(36);
            _flashRingSwatch.Height(18);
            _flashRingSwatch.CornerRadius(CornerRadius{ 3, 3, 3, 3 });
            _flashRingSwatch.BorderThickness(Thickness{ 1, 1, 1, 1 });
            _flashRingSwatch.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0xFF, 0xFF, 0xFF) });
            _flashRingSwatch.Background(SolidColorBrush{ ColorHelper::FromArgb(0xCC, 0xFF, 0x00, 0x00) });

            // The picker lives INSIDE the flyout (built once, opened on demand). Reading its Color() at
            // Save works whether or not the flyout was ever opened.
            _setFlashRingPicker = winrt::Microsoft::UI::Xaml::Controls::ColorPicker{};
            _setFlashRingPicker.IsAlphaEnabled(true); // the OPACITY slider + alpha in the chosen Color (the "add opacity" ask)
            _setFlashRingPicker.IsMoreButtonVisible(true); // tuck the RGB / HSV / Hex / Alpha text inputs behind a "More" expander
            _setFlashRingPicker.IsHexInputVisible(true);
            _setFlashRingPicker.IsAlphaTextInputVisible(true);
            _setFlashRingPicker.IsColorChannelTextInputVisible(true);
            _setFlashRingPicker.Color(ColorHelper::FromArgb(0xCC, 0xFF, 0x00, 0x00)); // seed pass (_ShowSettings) sets the real saved color
            // Live-preview the swatch as the user drags the picker.
            _setFlashRingPicker.ColorChanged([this](auto&&, const winrt::Microsoft::UI::Xaml::Controls::ColorChangedEventArgs& e) {
                if (_flashRingSwatch)
                {
                    _flashRingSwatch.Background(SolidColorBrush{ e.NewColor() });
                }
            });

            auto pickerPanel = StackPanel{};
            pickerPanel.Spacing(8);
            pickerPanel.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark; the flyout renders in the popup root
            pickerPanel.Children().Append(_setFlashRingPicker);
            {
                // A clear way back to the default (the picker has no built-in "default") — sets the
                // picker to red at 80% opacity; Save then writes "#CCFF0000".
                auto reset = HyperlinkButton{};
                reset.Content(winrt::box_value(L"Reset to default (red)"));
                reset.Padding(Thickness{ 4, 2, 4, 2 });
                reset.FontSize(12);
                reset.Click([this](const IInspectable&, const RoutedEventArgs&) {
                    if (_setFlashRingPicker)
                    {
                        _setFlashRingPicker.Color(ColorHelper::FromArgb(0xCC, 0xFF, 0x00, 0x00));
                    }
                });
                pickerPanel.Children().Append(reset);
            }

            auto flyout = Flyout{};
            flyout.Content(pickerPanel);

            auto swatchBtn = Button{};
            swatchBtn.Padding(Thickness{ 4, 3, 4, 3 });
            swatchBtn.Content(_flashRingSwatch);
            swatchBtn.Flyout(flyout);
            AgentSetTip(swatchBtn, L"Pick the tab status-dot \x201Cunread\x201D flash-ring color. The picker's alpha slider sets its opacity. Default: red at 80% opacity.");
            row.Children().Append(swatchBtn);

            panel.Children().Append(row);
        }

        // TABS: the unsent-draft "3 dots" color (PENDING_INPUT.md) — a LIGHT/DARK contrast PAIR. The dots
        // (shown on a tab / board card when the user has typed but not sent a message) are painted the
        // LIGHT color on a DARK tab background and the DARK color on a LIGHT one, auto-picked from the
        // session's per-directory color, so they're never invisible. Same compact swatch-button-opens-a-
        // ColorPicker-flyout idiom as the flash-ring row above (alpha slider enabled). GLOBAL
        // (AppSettings::pendingDotsLightColor / pendingDotsDarkColor); applied live (the tab dots re-read on
        // the next scan tick, board cards on the next rebuild). A local lambda builds both identical rows.
        {
            auto makeDotsColorRow = [&panel](const wchar_t* labelText,
                                             const wchar_t* tipText,
                                             const wchar_t* resetText,
                                             Color defColor,
                                             winrt::Microsoft::UI::Xaml::Controls::ColorPicker& pickerOut,
                                             Border& swatchOut) {
                auto row = StackPanel{};
                row.Orientation(Orientation::Horizontal);
                row.Spacing(10);
                row.VerticalAlignment(VerticalAlignment::Center);

                auto label = Text(labelText, 13, false, 0.9);
                label.VerticalAlignment(VerticalAlignment::Center);
                row.Children().Append(label);

                // The swatch preview (a thin light border keeps a transparent pick visible as an outline).
                Border swatch{};
                swatch.Width(36);
                swatch.Height(18);
                swatch.CornerRadius(CornerRadius{ 3, 3, 3, 3 });
                swatch.BorderThickness(Thickness{ 1, 1, 1, 1 });
                swatch.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0xFF, 0xFF, 0xFF) });
                swatch.Background(SolidColorBrush{ defColor });
                swatchOut = swatch;

                // The picker lives INSIDE the flyout (built once, opened on demand); reading its Color() at
                // Save works whether or not the flyout was ever opened.
                winrt::Microsoft::UI::Xaml::Controls::ColorPicker picker{};
                picker.IsAlphaEnabled(true);
                picker.IsMoreButtonVisible(true);
                picker.IsHexInputVisible(true);
                picker.IsAlphaTextInputVisible(true);
                picker.IsColorChannelTextInputVisible(true);
                picker.Color(defColor); // the seed pass (_ShowSettings) sets the real saved color
                picker.ColorChanged([swatch](auto&&, const winrt::Microsoft::UI::Xaml::Controls::ColorChangedEventArgs& e) {
                    swatch.Background(SolidColorBrush{ e.NewColor() }); // live-preview the swatch as the user drags
                });
                pickerOut = picker;

                auto pickerPanel = StackPanel{};
                pickerPanel.Spacing(8);
                pickerPanel.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark; the flyout renders in the popup root
                pickerPanel.Children().Append(picker);
                {
                    auto reset = HyperlinkButton{};
                    reset.Content(winrt::box_value(winrt::hstring{ resetText }));
                    reset.Padding(Thickness{ 4, 2, 4, 2 });
                    reset.FontSize(12);
                    reset.Click([picker, defColor](const IInspectable&, const RoutedEventArgs&) {
                        picker.Color(defColor);
                    });
                    pickerPanel.Children().Append(reset);
                }

                auto flyout = Flyout{};
                flyout.Content(pickerPanel);

                auto swatchBtn = Button{};
                swatchBtn.Padding(Thickness{ 4, 3, 4, 3 });
                swatchBtn.Content(swatch);
                swatchBtn.Flyout(flyout);
                AgentSetTip(swatchBtn, tipText);
                row.Children().Append(swatchBtn);

                panel.Children().Append(row);
            };

            makeDotsColorRow(L"Pending dots (on dark tabs)",
                             L"Pick the color of the unsent-draft \x201C" L"3 dots\x201D shown on a DARK tab / card background. The picker's alpha slider sets its opacity. Default: gold.",
                             L"Reset to default (gold)",
                             ColorHelper::FromArgb(0xFF, 0xE0, 0xA9, 0x2B),
                             _setPendingLightPicker, _pendingLightSwatch);
            makeDotsColorRow(L"Pending dots (on light tabs)",
                             L"Pick the color of the unsent-draft \x201C" L"3 dots\x201D shown on a LIGHT tab / card background. The picker's alpha slider sets its opacity. Default: deep amber.",
                             L"Reset to default (amber)",
                             ColorHelper::FromArgb(0xFF, 0x5A, 0x3E, 0x00),
                             _setPendingDarkPicker, _pendingDarkSwatch);
        }

        // TABS: "Overlay opacity" — the per-tab overlay's REST (dim, idle) and HOVER (bright, on
        // pointer-over) opacities on ONE track with TWO dots. The rail is a transparent->solid gradient
        // (left = transparent, right = solid — the requested visual); the LEFT dot is rest, the RIGHT is
        // hover, and they CAN'T CROSS (rest <= hover, enforced in the drag). Dragged with the splitter
        // idiom (CapturePointer + root-relative delta — Islands-safe). GLOBAL (AppSettings::
        // tabOverlayRestOpacity / tabOverlayHoverOpacity); applied live on Save + cross-window broadcast.
        {
            auto headerRow = StackPanel{};
            headerRow.Orientation(Orientation::Horizontal);
            headerRow.Spacing(10);
            auto oLabel = Text(L"Overlay opacity", 13, false, 0.9);
            oLabel.VerticalAlignment(VerticalAlignment::Center);
            headerRow.Children().Append(oLabel);
            _overlayOpacityLabel = Text(L"Rest 50%   \x00B7   Hover 100%", 12, false, 0.6);
            _overlayOpacityLabel.VerticalAlignment(VerticalAlignment::Center);
            headerRow.Children().Append(_overlayOpacityLabel);
            panel.Children().Append(headerRow);

            _overlayOpacityTrack = Canvas{};
            _overlayOpacityTrack.Width(kOverlayTrackW);
            _overlayOpacityTrack.Height(kOverlayTrackH);
            _overlayOpacityTrack.Margin(Thickness{ 0, 4, 0, 4 });
            _overlayOpacityTrack.HorizontalAlignment(HorizontalAlignment::Left);
            AgentSetTip(_overlayOpacityTrack, L"Drag the two dots to set how visible the per-tab overlay badge is \x2014 the LEFT dot is its REST opacity (idle; the transparent end), the RIGHT dot its HOVER opacity (on pointer-over; the solid end). The dots can't cross.");

            // The gradient rail (transparent left -> solid white right). Non-hit-test so only the dots
            // capture the pointer; a faint outline keeps the transparent end visible on the dark card.
            auto rail = Border{};
            rail.Width(kOverlayTrackW);
            rail.Height(8);
            rail.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            rail.IsHitTestVisible(false);
            rail.BorderThickness(Thickness{ 1, 1, 1, 1 });
            rail.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x30, 0xFF, 0xFF, 0xFF) });
            {
                auto grad = winrt::Windows::UI::Xaml::Media::LinearGradientBrush{};
                grad.StartPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.5f });
                grad.EndPoint(winrt::Windows::Foundation::Point{ 1.0f, 0.5f });
                auto s0 = winrt::Windows::UI::Xaml::Media::GradientStop{};
                s0.Color(ColorHelper::FromArgb(0x00, 0xFF, 0xFF, 0xFF)); // transparent (left = low opacity)
                s0.Offset(0.0);
                auto s1 = winrt::Windows::UI::Xaml::Media::GradientStop{};
                s1.Color(ColorHelper::FromArgb(0xFF, 0xFF, 0xFF, 0xFF)); // solid (right = full opacity)
                s1.Offset(1.0);
                grad.GradientStops().Append(s0);
                grad.GradientStops().Append(s1);
                rail.Background(grad);
            }
            Canvas::SetLeft(rail, 0.0);
            Canvas::SetTop(rail, (kOverlayTrackH - 8.0) / 2.0);
            _overlayOpacityTrack.Children().Append(rail);

            // Build one dot. isRest picks which value it drives + the cross-constraint direction.
            const auto makeDot = [this](bool isRest) {
                winrt::Windows::UI::Xaml::Shapes::Ellipse dot{};
                dot.Width(kOverlayThumb);
                dot.Height(kOverlayThumb);
                dot.Fill(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0xFF, 0xFF, 0xFF) });
                dot.Stroke(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x20, 0x20, 0x20) }); // dark ring -> visible on any part of the gradient
                dot.StrokeThickness(1.5);
                Canvas::SetTop(dot, (kOverlayTrackH - kOverlayThumb) / 2.0);
                // Drag (the splitter idiom): pin the root-relative X + this dot's value at press, derive
                // the new value from the delta on move, clamp to [0,1] AND to the other dot (no crossing).
                dot.PointerPressed([this, isRest](const IInspectable& s, const PointerRoutedEventArgs& e) {
                    if (!_root)
                    {
                        return;
                    }
                    if (isRest)
                    {
                        _overlayDragRest = true;
                    }
                    else
                    {
                        _overlayDragHover = true;
                    }
                    _overlayDragStartX = e.GetCurrentPoint(_root).Position().X;
                    _overlayDragStartVal = isRest ? _overlayRestVal : _overlayHoverVal;
                    if (const auto el = s.try_as<UIElement>())
                    {
                        el.CapturePointer(e.Pointer());
                    }
                    e.Handled(true);
                });
                dot.PointerMoved([this, isRest](const IInspectable&, const PointerRoutedEventArgs& e) {
                    if ((isRest && !_overlayDragRest) || (!isRest && !_overlayDragHover) || !_root)
                    {
                        return;
                    }
                    const double cur = e.GetCurrentPoint(_root).Position().X;
                    double v = _overlayDragStartVal + (cur - _overlayDragStartX) / (kOverlayTrackW - kOverlayThumb);
                    v = std::clamp(v, 0.0, 1.0);
                    if (isRest)
                    {
                        _overlayRestVal = std::min(v, _overlayHoverVal); // rest can't pass hover
                    }
                    else
                    {
                        _overlayHoverVal = std::max(v, _overlayRestVal); // hover can't drop below rest
                    }
                    _LayoutOverlayOpacitySlider();
                    e.Handled(true);
                });
                const auto endDrag = [this, isRest](const IInspectable& s, const PointerRoutedEventArgs& e) {
                    if (isRest)
                    {
                        _overlayDragRest = false;
                    }
                    else
                    {
                        _overlayDragHover = false;
                    }
                    if (const auto el = s.try_as<UIElement>())
                    {
                        el.ReleasePointerCaptures();
                    }
                    e.Handled(true);
                };
                dot.PointerReleased(endDrag);
                dot.PointerCaptureLost(endDrag);
                return dot;
            };
            _overlayRestThumb = makeDot(true);
            _overlayHoverThumb = makeDot(false);
            AgentSetTip(_overlayRestThumb, L"REST opacity \x2014 how visible the overlay badge is when idle (drag left for more transparent). Can't go past the hover dot.");
            AgentSetTip(_overlayHoverThumb, L"HOVER opacity \x2014 how visible the overlay becomes on pointer-over (drag right for more solid). Can't drop below the rest dot.");
            // Hover dot LAST so it sits on top + stays grabbable when the two dots coincide.
            _overlayOpacityTrack.Children().Append(_overlayRestThumb);
            _overlayOpacityTrack.Children().Append(_overlayHoverThumb);
            panel.Children().Append(_overlayOpacityTrack);
            _LayoutOverlayOpacitySlider(); // seed positions from the defaults; _ShowSettings re-seeds from AppSettings
        }

        // === ABOUT tab (continued) === — the version + UPDATES block led the About panel at the top of
        // this builder; the PROFILE row + the Uninstall button finish it here.
        panel = aboutPanel;
        // PROFILE — the per-install state folder (NOT an AppSettings field: it is the pointer
        // TO settings.json, resolved by ProfileBootstrap BEFORE any state loads, so it lives in
        // the choice file / env, never inside the profile it selects). Read-only display +
        // "Change…", which re-runs the same picker the first launch shows and applies on the
        // NEXT start (the running engine cannot re-home its state mid-run).
        panel.Children().Append(Text(L"PROFILE", 11, true, 0.6));
        _setProfileDir = TextBlock{};
        _setProfileDir.TextWrapping(TextWrapping::Wrap);
        _setProfileDir.Opacity(0.85);
        _setProfileDir.FontSize(12);
        AgentSetTip(_setProfileDir, L"This install's active profile folder \x2014 where all sessions, settings, hooks, and window layouts are stored. A staged change shows as current \x2192 new (after restart).");
        panel.Children().Append(_setProfileDir);
        auto changeProfile = Button{};
        changeProfile.Content(winrt::box_value(L"Change profile folder\x2026"));
        AgentSetTip(changeProfile, L"Point this install at a different profile folder \x2014 applies on the next start (the running app can't re-home its state mid-run).");
        changeProfile.Click([this](const IInspectable&, const RoutedEventArgs&) {
            // Defer off the click tick (the XAML-Islands pointer-handler rule), then run the
            // pure-Win32 picker — a Win32 modal gets its keyboard input directly in islands.
            if (_dispatcher)
            {
                _dispatcher.TryEnqueue([this]() {
                    const std::wstring active = ::Agentmaster::Profiles::ResolveProfileDir();
                    const auto pick = ::Agentmaster::Profiles::ShowProfilePicker(::GetActiveWindow(), false, active);
                    if (!pick.chosen)
                    {
                        return;
                    }
                    ::Agentmaster::Profiles::SaveChoice(pick.dir);
                    // Nav audit: the user re-pointed this install at a different profile folder (the cog's
                    // "Change profile folder…"). It re-homes ALL persisted state and applies on the NEXT
                    // start (never mid-run), so the trail records the staged from -> to.
                    ::Agentmaster::LogNav(L"profile-change " + active + L" -> " + pick.dir + (pick.migrate ? std::wstring{ L" (migrate data)" } : std::wstring{}) + L" (applies on restart)");
                    if (pick.migrate)
                    {
                        ::Agentmaster::Profiles::MigrateProfileData(active, pick.dir);
                    }
                    ::Agentmaster::Profiles::SeedTerminalSettings(pick.dir);
                    if (_setProfileDir)
                    {
                        _setProfileDir.Text(winrt::hstring{ active + L"  \x2192  " + pick.dir + L" (after restart)" });
                    }
                    ::MessageBoxW(::GetActiveWindow(),
                                  (L"Profile saved:\n\n    " + pick.dir + L"\n\nIt applies the next time Agentmaster starts.").c_str(),
                                  L"Agentmaster",
                                  MB_OK | MB_ICONINFORMATION);
                });
            }
        });
        panel.Children().Append(changeProfile);

        // "Uninstall Agentmaster…" — removes THIS install (the current package family) via the same
        // embedded am-update.ps1 (-Uninstall). Shown only for packaged installs (gated in _ShowSettings);
        // per-user, no admin, and the profile data (~/.agentmaster) is kept. Confirms, then quits so the
        // package isn't in use while it's removed. LAST item in About — a destructive action at the foot.
        _setUninstallBtn = Button{};
        _setUninstallBtn.Content(winrt::box_value(L"Uninstall Agentmaster\x2026"));
        _setUninstallBtn.Margin(Thickness{ 0, 10, 0, 0 });
        AgentSetTip(_setUninstallBtn, L"Remove this Agentmaster install. Your data (sessions, settings, e.g. %USERPROFILE%\\.agentmaster) is kept. Agentmaster closes to finish.");
        _setUninstallBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            _Confirm(L"Uninstall Agentmaster?",
                     L"This removes the installed Agentmaster package. Your data (sessions, settings, e.g. %USERPROFILE%\\.agentmaster) is kept. Agentmaster will close to finish uninstalling.",
                     L"Uninstall",
                     [this]() {
                         const std::wstring stateDir = ::Agentmaster::Profiles::ResolveProfileDir();
                         if (::Agentmaster::Updater::LaunchUninstaller(stateDir) && _quitForUpdateHandler)
                         {
                             _quitForUpdateHandler();
                         }
                     });
        });
        panel.Children().Append(_setUninstallBtn);

        // Every panel is now POPULATED — register the tabs (button + panel pairs) and slot the strip +
        // the single content scroller into `outer` between the title and the footer
        // (title -> tabs -> divider -> content -> Save/Cancel). _SwitchSettingsTab(0) below swaps the
        // Sessions panel into the scroller.
        addSettingsTab(L"Sessions", L"How new Claude sessions launch \x2014 permissions, model, environment variables, and the Launch box's directory history.", sessionsPanel);
        // Auto Testing is a DEV-ONLY feature: the Tests Autorunner defaults tab is added only under the
        // AgentmasterDev package (the autorunner never runs in a release build — see Engine.cpp). The
        // panel is still built above so the cog's load/save code paths stay uniform; it's just not shown.
        if (::Agentmaster::Profiles::IsDevPackage())
        {
            addSettingsTab(L"Tests Autorunner", L"Tests Autorunner defaults stamped onto every new session \x2014 the starting mode and its backstops.", autorunnerPanel);
        }
        addSettingsTab(L"Behavior", L"Interaction + session-state behavior \x2014 close confirms, the rename commit key, and the Waiting-for-you \x201Cunread\x201D timeout.", behaviorPanel);
        addSettingsTab(L"Tabs & Overlay", L"The terminal tab strip + the per-tab overlay badge \x2014 close affordances, the favorite marker, the status-flash color, and overlay opacity.", tabsPanel);
        addSettingsTab(L"Claude", L"The Claude install Agentmaster drives \x2014 which native claude.exe, and how long Claude keeps session history.", claudePanel);
        addSettingsTab(L"About", L"Version + build, updates, the active profile folder, and uninstall.", aboutPanel);

        outer.Children().Append(tabStrip);
        // A thin divider under the strip so the active tab reads as connected to its content below.
        {
            auto sep = Border{};
            sep.Height(1);
            sep.Background(Fill(0x30, 0xFF, 0xFF, 0xFF));
            sep.Margin(Thickness{ 0, 0, 0, 2 });
            outer.Children().Append(sep);
        }
        outer.Children().Append(_settingsScroll);

        // Cancel / Save — a FIXED footer OUTSIDE the tabs (always reachable regardless of the active tab;
        // each tab scrolls on its own, so the footer never scrolls away).
        auto buttons = StackPanel{};
        buttons.Orientation(Orientation::Horizontal);
        buttons.HorizontalAlignment(HorizontalAlignment::Right);
        buttons.Spacing(8);
        buttons.Margin(Thickness{ 0, 8, 0, 0 });
        auto cancel = Button{};
        cancel.Content(winrt::box_value(L"Cancel"));
        AgentSetTip(cancel, L"Close without saving \x2014 discard any changes made here.");
        cancel.Click([this](const IInspectable&, const RoutedEventArgs&) { _HideSettings(); });
        auto save = Button{};
        save.Content(winrt::box_value(L"Save"));
        AgentSetTip(save, L"Save these settings and apply them \x2014 they persist to disk and govern future sessions (and live-apply where possible, e.g. the Claude binary and tab options).");
        save.Click([this](const IInspectable&, const RoutedEventArgs&) { _SaveSettings(); });
        buttons.Children().Append(cancel);
        buttons.Children().Append(save);
        outer.Children().Append(buttons);

        card.Child(outer);

        _settingsOverlay.Children().Append(card);
        _root.Children().Append(_settingsOverlay);

        _SwitchSettingsTab(0); // seed the strip styling + show the first tab
    }

    // Agentmaster: swap the selected tab's (already-populated) panel into the single scroller + restyle the
    // strip (the _SwitchEnvTab idiom — active = blue fill + white text, inactive = transparent + gray).
    // Setting Content detaches the previous panel cleanly (a UIElement has one parent), so nothing is ever
    // double-parented and no panel renders as an empty ContentPresenter. Index out of range is a no-op.
    void AgentManagerContent::_SwitchSettingsTab(int index)
    {
        if (index < 0 || index >= static_cast<int>(_settingsTabPanels.size()) || !_settingsScroll)
        {
            return;
        }
        _settingsActiveTab = index;
        if (_settingsTabPanels[index])
        {
            _settingsScroll.Content(_settingsTabPanels[index]); // the ContentPresenter swap
        }
        for (size_t i = 0; i < _settingsTabButtons.size(); ++i)
        {
            const auto& b = _settingsTabButtons[i];
            if (!b)
            {
                continue;
            }
            const bool active = static_cast<int>(i) == index;
            b.Background(active ? Fill(0xFF, 0x0E, 0x63, 0x9C) : Fill(0x00, 0x00, 0x00, 0x00));
            b.Foreground(active ? Fill(0xFF, 0xFF, 0xFF, 0xFF) : Fill(0xFF, 0xB0, 0xB0, 0xB0));
        }
    }

    void AgentManagerContent::_ShowSettings()
    {
        if (!_settingsOverlay)
        {
            return;
        }
        // Populate every control from the current settings before revealing.
        if (_setSkipPermissions)
        {
            _setSkipPermissions.IsOn(_appSettings.skipPermissions);
        }
        if (_setModel)
        {
            _setModel.Text(winrt::hstring{ _appSettings.model });
        }
        if (_setIncludeCoAuthored)
        {
            _setIncludeCoAuthored.IsOn(_appSettings.includeCoAuthoredBy);
        }
        // ENV_VARS.md: seed both env tabs (global text + the per-directory draft from dir-env.json).
        _LoadEnvVarsArea();
        if (_setClaudeExePath)
        {
            _setClaudeExePath.Text(winrt::hstring{ _appSettings.claudeExePath });
        }
        if (_setCleanupDays)
        {
            // ENV_VARS.md §8: reflect the LIVE value from the user's global ~/.claude/settings.json (blank if unset).
            const auto days = ::Agentmaster::GetClaudeCleanupPeriodDays();
            _setCleanupDays.Text(winrt::hstring{ days ? std::to_wstring(*days) : std::wstring{} });
        }
        if (_setClaudeDetected)
        {
            const auto& exe = ::Agentmaster::SharedEngine().claudeExePath;
            _setClaudeDetected.Text(winrt::hstring{ exe.empty() ? std::wstring{ L"Detected: none \x2014 Claude launch/fork/resume is disabled until a native claude.exe is found" } : (L"Detected: " + exe) });
        }
        if (_setDefaultMode)
        {
            _setDefaultMode.SelectedIndex(_appSettings.defaultAutorunnerMode == AutorunnerMode::Full ? 2 :
                                          _appSettings.defaultAutorunnerMode == AutorunnerMode::SemiAuto ? 1 :
                                                                                                         0);
        }
        if (_setMaxAutoSends)
        {
            _setMaxAutoSends.Text(winrt::hstring{ std::to_wstring(_appSettings.maxAutoSends) });
        }
        if (_setStopOnError)
        {
            _setStopOnError.IsOn(_appSettings.stopOnError);
        }
        if (_setPauseOnHuman)
        {
            _setPauseOnHuman.IsOn(_appSettings.pauseOnHumanInput);
        }
        if (_setConfirmKill)
        {
            _setConfirmKill.IsOn(_appSettings.confirmBeforeKill);
        }
        if (_setRenameCommit)
        {
            // Items are ordered to match TabRenameCommitMode (0 click-away / 1 +Shift+Enter / 2 +Enter).
            _setRenameCommit.SelectedIndex(_appSettings.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrEnter      ? 2 :
                                           _appSettings.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter ? 1 :
                                                                                                                            0);
        }
        if (_setWaitingDecaySlider && _setWaitingNever && _setWaitingDecayText)
        {
            const bool never = (_appSettings.waitingForYouTimeoutMinutes == 0);
            _setWaitingNever.IsOn(never);
            uint32_t m = _appSettings.waitingForYouTimeoutMinutes;
            if (m < 1)
            {
                m = 4320; // a sane value when "never" is on (toggling off then lands on 3d)
            }
            // The textbox carries the TRUE value (may exceed the slider's 7d max); the slider is clamped.
            uint32_t sliderM = m > 10080 ? 10080 : m;
            if (sliderM < 1)
            {
                sliderM = 1;
            }
            _waitingDecaySyncing = true; // seed both without echoing through ValueChanged/TextChanged
            _setWaitingDecaySlider.Value(static_cast<double>(sliderM));
            _setWaitingDecayText.Text(winrt::hstring{ FormatMinutesCompact(m) });
            if (_setWaitingDecayBorder)
            {
                _setWaitingDecayBorder.BorderBrush(SolidColorBrush{ Colors::Transparent() });
            }
            _waitingDecaySyncing = false;
            _setWaitingDecaySlider.IsEnabled(!never);
            _setWaitingDecayText.IsEnabled(!never);
        }
        if (_setServerCache)
        {
            _setServerCache.Text(winrt::hstring{ std::to_wstring(_appSettings.serverCacheMinutes) });
        }
        if (_setLaunchDir)
        {
            _setLaunchDir.Text(winrt::hstring{ _appSettings.defaultLaunchDir });
        }
        if (_setRecentDirsLimit)
        {
            _setRecentDirsLimit.Text(winrt::hstring{ std::to_wstring(_appSettings.recentDirsLimit) });
        }
        if (_setShowTabCloseButton)
        {
            _setShowTabCloseButton.IsOn(_appSettings.showTabCloseButton);
        }
        if (_setCloseTabOnMiddleClick)
        {
            _setCloseTabOnMiddleClick.IsOn(_appSettings.closeTabOnMiddleClick);
        }
        if (_setAlwaysShowHomeButton)
        {
            _setAlwaysShowHomeButton.IsOn(_appSettings.alwaysShowHomeButton);
        }
        if (_setFavoriteIcon)
        {
            // Items: 0 == Crown (default), 1 == Star.
            _setFavoriteIcon.SelectedIndex(_appSettings.favoriteIcon == FavoriteIcon::Star ? 1 : 0);
        }
        if (_setMaxTags)
        {
            _setMaxTags.Text(winrt::hstring{ std::to_wstring(_appSettings.maxTags) });
        }
        if (_setTooltipTagsOpacity)
        {
            // 0..1 -> the slider's 10..100% band; setting Value fires ValueChanged, which refreshes
            // the "\x2026 N%" header.
            const int pct = static_cast<int>(::Agentmaster::ClampTooltipTagsOpacity(_appSettings.tooltipTagsOpacity) * 100.0 + 0.5);
            _setTooltipTagsOpacity.Value(static_cast<double>(pct));
        }
        if (_setTabColorMode)
        {
            // Items: 0 == WorkingDirectory (default), 1 == Individual, 2 == InferredWorkingDirectory.
            _setTabColorMode.SelectedIndex(_appSettings.tabColorMode == TabColorMode::Individual ? 1 :
                                               _appSettings.tabColorMode == TabColorMode::InferredWorkingDirectory ? 2 :
                                                                                                                     0);
        }
        if (_setFlashRingPicker)
        {
            // The status flashing color (with opacity in the alpha byte). Malformed/empty -> default (80% red).
            const auto c = ParseArgbHexColor(_appSettings.flashRingColor, ColorHelper::FromArgb(0xCC, 0xFF, 0x00, 0x00));
            _setFlashRingPicker.Color(c); // also raises ColorChanged -> updates the swatch preview
            if (_flashRingSwatch)
            {
                _flashRingSwatch.Background(SolidColorBrush{ c }); // set directly too (don't rely on a programmatic ColorChanged firing)
            }
        }
        if (_setPendingLightPicker)
        {
            // Pending "3 dots" color on a DARK background (PENDING_INPUT.md). Malformed/empty -> gold default.
            const auto c = ParseArgbHexColor(_appSettings.pendingDotsLightColor, ColorHelper::FromArgb(0xFF, 0xE0, 0xA9, 0x2B));
            _setPendingLightPicker.Color(c);
            if (_pendingLightSwatch)
            {
                _pendingLightSwatch.Background(SolidColorBrush{ c });
            }
        }
        if (_setPendingDarkPicker)
        {
            // Pending "3 dots" color on a LIGHT background (PENDING_INPUT.md). Malformed/empty -> amber default.
            const auto c = ParseArgbHexColor(_appSettings.pendingDotsDarkColor, ColorHelper::FromArgb(0xFF, 0x5A, 0x3E, 0x00));
            _setPendingDarkPicker.Color(c);
            if (_pendingDarkSwatch)
            {
                _pendingDarkSwatch.Background(SolidColorBrush{ c });
            }
        }
        if (_overlayOpacityTrack)
        {
            // Seed the overlay-opacity dots from the saved rest/hover (clamped + ordered, so a hand-edited
            // settings.json can't place a dot off-track or crossed).
            double rest = _appSettings.tabOverlayRestOpacity;
            double hover = _appSettings.tabOverlayHoverOpacity;
            if (!(rest > 0.0 && rest <= 1.0))
            {
                rest = 0.50;
            }
            if (!(hover > 0.0 && hover <= 1.0))
            {
                hover = 1.0;
            }
            if (rest > hover)
            {
                rest = hover;
            }
            _overlayRestVal = rest;
            _overlayHoverVal = hover;
            _LayoutOverlayOpacitySlider();
        }
        if (_setResetHidden)
        {
            // The "cleared" state is per-click feedback; restore the actionable label each open.
            _setResetHidden.Content(winrt::box_value(L"Reset hidden sessions"));
            _setResetHidden.IsEnabled(true);
        }
        if (_setProfileDir)
        {
            // The ACTIVE profile (this run) — a pending Change… is re-shown as pending until restart.
            const std::wstring active = ::Agentmaster::Profiles::ResolveProfileDir();
            const std::wstring saved = ::Agentmaster::Profiles::ReadSavedChoice();
            if (!saved.empty() && !::Agentmaster::Profiles::detail::SamePath(saved, active))
            {
                _setProfileDir.Text(winrt::hstring{ active + L"  \x2192  " + saved + L" (after restart)" });
            }
            else
            {
                _setProfileDir.Text(winrt::hstring{ active });
            }
        }
        if (_setAllowPrerelease)
        {
            _setAllowPrerelease.IsOn(_appSettings.allowUpdatePrerelease);
        }
        if (_setUpdateChangelog)
        {
            // Hide "Update's changelog" until THIS open's check confirms an update is available
            // (the silent check below, or the explicit button, reveals it).
            _setUpdateChangelog.Visibility(Visibility::Collapsed);
        }
        // Updater is RELEASE-channel only (Updater::IsUpdaterChannel). On a dev/unpackaged build the
        // GitHub release is NOT a self-update (different package + always-"behind" the 0.0.1.0
        // placeholder), so don't check or offer it: disable the button, hide the changelog links, and
        // explain. Only the release install checks (silently on open) + shows "vX.Y.Z available!".
        const bool updaterChannel = ::Agentmaster::Updater::IsUpdaterChannel();
        if (_setCheckUpdates)
        {
            _setCheckUpdates.IsEnabled(updaterChannel);
        }
        if (_setUninstallBtn)
        {
            // Uninstall removes the CURRENT package family (release OR dev), so it's offered for any
            // packaged install — independent of the release-only updater channel. An unpackaged build
            // has nothing registered to remove, so hide it there.
            _setUninstallBtn.Visibility(::Agentmaster::Updater::IsPackaged() ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_setCurrentChangelog)
        {
            // The current build's release page only exists for a published version; hide it on dev.
            _setCurrentChangelog.Visibility(updaterChannel ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_setUpdateStatus)
        {
            if (updaterChannel)
            {
                _setUpdateStatus.Text(L""); // cleared until the silent check (below) finds an update
            }
            else
            {
                _setUpdateStatus.Text(L"Dev build \x2014 the updater manages the Release install (rebuild to update this one).");
                _setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x99, 0x99, 0x99) });
            }
        }
        _SwitchSettingsTab(0); // always reopen on the first tab (Sessions)
        _settingsOverlay.Visibility(Visibility::Visible);
        // Updater: a silent check on open — if a newer release exists, the label next to "Check for
        // updates" reads "vX.Y.Z available!" in dark green. Quiet on no-update / no-network (the
        // explicit button gives that feedback). Runs off the UI thread (Updater.h uses WinHTTP).
        // RELEASE channel only — a dev build neither auto-checks nor is offered the release as an update.
        if (updaterChannel)
        {
            _CheckForUpdates(false);
        }
    }

    void AgentManagerContent::_HideSettings()
    {
        if (_settingsOverlay)
        {
            _settingsOverlay.Visibility(Visibility::Collapsed);
        }
    }

    void AgentManagerContent::_SaveSettings()
    {
        if (_setSkipPermissions)
        {
            _appSettings.skipPermissions = _setSkipPermissions.IsOn();
        }
        if (_setModel)
        {
            std::wstring m{ _setModel.Text() };
            const auto a = m.find_first_not_of(L" \t");
            const auto b = m.find_last_not_of(L" \t");
            _appSettings.model = (a == std::wstring::npos) ? std::wstring{} : m.substr(a, b - a + 1);
        }
        if (_setIncludeCoAuthored)
        {
            _appSettings.includeCoAuthoredBy = _setIncludeCoAuthored.IsOn();
        }
        // ENV_VARS.md: global editor -> _appSettings.env; flush the per-directory draft -> dir-env.json.
        _SaveEnvVarsArea();
        if (_setClaudeExePath)
        {
            std::wstring p{ _setClaudeExePath.Text() };
            const auto a = p.find_first_not_of(L" \t");
            const auto b = p.find_last_not_of(L" \t");
            _appSettings.claudeExePath = (a == std::wstring::npos) ? std::wstring{} : p.substr(a, b - a + 1);
        }
        if (_setCleanupDays)
        {
            // ENV_VARS.md §8: write cleanupPeriodDays back to the user's global ~/.claude/settings.json.
            // Blank => remove our key (revert to Claude's 30-day default); a pure non-negative integer => set
            // it; any other text => leave the file untouched. (0 is accepted but is a Claude footgun — the
            // tooltip warns; we don't second-guess a deliberate entry.)
            std::wstring t{ _setCleanupDays.Text() };
            const auto a = t.find_first_not_of(L" \t\r\n");
            const auto b = t.find_last_not_of(L" \t\r\n");
            const std::wstring s = (a == std::wstring::npos) ? std::wstring{} : t.substr(a, b - a + 1);
            if (s.empty())
            {
                ::Agentmaster::SetClaudeCleanupPeriodDays(std::nullopt);
            }
            else
            {
                bool pure = true;
                for (const wchar_t c : s)
                {
                    if (c < L'0' || c > L'9')
                    {
                        pure = false;
                        break;
                    }
                }
                if (pure)
                {
                    int64_t v = 0;
                    for (const wchar_t c : s)
                    {
                        v = v * 10 + static_cast<int64_t>(c - L'0');
                        if (v > 100000000)
                        {
                            v = 100000000; // clamp; cleanupPeriodDays far past ~100y is meaningless
                        }
                    }
                    ::Agentmaster::SetClaudeCleanupPeriodDays(v);
                }
            }
        }
        if (_setDefaultMode)
        {
            const int idx = _setDefaultMode.SelectedIndex();
            _appSettings.defaultAutorunnerMode = idx == 2 ? AutorunnerMode::Full : idx == 1 ? AutorunnerMode::SemiAuto :
                                                                                            AutorunnerMode::Off;
        }
        if (_setMaxAutoSends)
        {
            const std::wstring t{ _setMaxAutoSends.Text() };
            uint32_t v = 0;
            bool any = false;
            for (const wchar_t c : t)
            {
                if (c >= L'0' && c <= L'9')
                {
                    v = v * 10 + static_cast<uint32_t>(c - L'0');
                    any = true;
                }
            }
            _appSettings.maxAutoSends = (any && v > 0) ? v : 100; // empty/zero/garbage -> default backstop
        }
        if (_setStopOnError)
        {
            _appSettings.stopOnError = _setStopOnError.IsOn();
        }
        if (_setPauseOnHuman)
        {
            _appSettings.pauseOnHumanInput = _setPauseOnHuman.IsOn();
        }
        if (_setConfirmKill)
        {
            _appSettings.confirmBeforeKill = _setConfirmKill.IsOn();
        }
        if (_setRenameCommit)
        {
            const int idx = _setRenameCommit.SelectedIndex();
            _appSettings.tabRenameCommitMode = idx == 2 ? TabRenameCommitMode::ClickAwayOrEnter :
                                               idx == 0 ? TabRenameCommitMode::ClickAwayOnly :
                                                          TabRenameCommitMode::ClickAwayOrShiftEnter;
        }
        if (_setWaitingDecayText && _setWaitingNever)
        {
            // "Never" => 0 (never time-decay; stay Waiting until read). Else the TEXTBOX's parsed minutes —
            // the source of truth, which may exceed the slider's 7d max. An unparsable/blank box keeps the
            // prior value rather than corrupting it. Clamp to a generous 365d ceiling, well past the slider.
            if (_setWaitingNever.IsOn())
            {
                _appSettings.waitingForYouTimeoutMinutes = 0;
            }
            else
            {
                uint32_t v = 0;
                if (ParseDurationToMinutes(std::wstring{ _setWaitingDecayText.Text() }, v))
                {
                    if (v < 1)
                    {
                        v = 1;
                    }
                    constexpr uint32_t kMaxWaitingMinutes = 525600; // 365 days
                    if (v > kMaxWaitingMinutes)
                    {
                        v = kMaxWaitingMinutes;
                    }
                    _appSettings.waitingForYouTimeoutMinutes = v;
                }
                // else: unparsable/blank box -> leave _appSettings.waitingForYouTimeoutMinutes untouched.
            }
        }
        if (_setServerCache)
        {
            const std::wstring t{ _setServerCache.Text() };
            uint32_t v = 0;
            bool any = false;
            for (const wchar_t c : t)
            {
                if (c >= L'0' && c <= L'9')
                {
                    v = v * 10 + static_cast<uint32_t>(c - L'0');
                    any = true;
                }
            }
            _appSettings.serverCacheMinutes = (any && v > 0) ? v : 5; // blank/0 -> 5 (the cosmetic default)
        }
        if (_setLaunchDir)
        {
            _appSettings.defaultLaunchDir = std::wstring{ _setLaunchDir.Text() };
        }
        if (_setRecentDirsLimit)
        {
            const std::wstring t{ _setRecentDirsLimit.Text() };
            uint32_t v = 0;
            bool any = false;
            for (const wchar_t c : t)
            {
                if (c >= L'0' && c <= L'9')
                {
                    v = v * 10 + static_cast<uint32_t>(c - L'0');
                    any = true;
                }
            }
            _appSettings.recentDirsLimit = (any && v > 0) ? v : 10; // empty/zero/garbage -> default
        }
        if (_setShowTabCloseButton)
        {
            _appSettings.showTabCloseButton = _setShowTabCloseButton.IsOn();
        }
        if (_setCloseTabOnMiddleClick)
        {
            _appSettings.closeTabOnMiddleClick = _setCloseTabOnMiddleClick.IsOn();
        }
        if (_setAlwaysShowHomeButton)
        {
            _appSettings.alwaysShowHomeButton = _setAlwaysShowHomeButton.IsOn();
        }
        if (_setFavoriteIcon)
        {
            // Items: 0 == Crown (default), 1 == Star.
            _appSettings.favoriteIcon = _setFavoriteIcon.SelectedIndex() == 1 ? FavoriteIcon::Star : FavoriteIcon::Crown;
        }
        if (_setMaxTags)
        {
            const std::wstring t{ _setMaxTags.Text() };
            uint32_t v = 0;
            for (const wchar_t c : t)
            {
                if (c >= L'0' && c <= L'9' && v < 1000)
                {
                    v = v * 10 + static_cast<uint32_t>(c - L'0');
                }
            }
            _appSettings.maxTags = ClampMaxTags(v); // blank/0 -> 20 (default), >40 -> 40
        }
        if (_setTooltipTagsOpacity)
        {
            // The slider is 10..100(%); store 0..1, clamped (shared with the Persistence load).
            _appSettings.tooltipTagsOpacity = ClampTooltipTagsOpacity(_setTooltipTagsOpacity.Value() / 100.0);
        }
        if (_setTabColorMode)
        {
            // Items: 0 == WorkingDirectory (default), 1 == Individual, 2 == InferredWorkingDirectory.
            _appSettings.tabColorMode = _setTabColorMode.SelectedIndex() == 1 ? TabColorMode::Individual :
                                        _setTabColorMode.SelectedIndex() == 2 ? TabColorMode::InferredWorkingDirectory :
                                                                                TabColorMode::WorkingDirectory;
        }
        if (_setFlashRingPicker)
        {
            // The picker always yields a valid Color; store it as "#AARRGGBB" (opacity in the alpha byte).
            _appSettings.flashRingColor = FormatArgbHexColor(_setFlashRingPicker.Color());
        }
        if (_setPendingLightPicker)
        {
            _appSettings.pendingDotsLightColor = FormatArgbHexColor(_setPendingLightPicker.Color()); // pending dots on a DARK bg
        }
        if (_setPendingDarkPicker)
        {
            _appSettings.pendingDotsDarkColor = FormatArgbHexColor(_setPendingDarkPicker.Color()); // pending dots on a LIGHT bg
        }
        if (_overlayOpacityTrack)
        {
            // The dual-thumb slider already keeps rest <= hover (drag clamps); store both verbatim.
            _appSettings.tabOverlayRestOpacity = _overlayRestVal;
            _appSettings.tabOverlayHoverOpacity = _overlayHoverVal;
        }
        if (_setAllowPrerelease)
        {
            _appSettings.allowUpdatePrerelease = _setAllowPrerelease.IsOn(); // UPDATES: the form OWNS this field
        }
        // Preserve fields owned by out-of-cog UI actions, freshest from disk (the page's settings handler
        // does the same for hiddenSessionIds/showSummaryPanel): the summary panel SIZE (width/height
        // fractions, TAB_OVERLAY.md) is written by the panel's resize grips, not this form, so a form Save
        // must not regress a resize done since the modal was seeded (incl. from another window). The
        // updater's skip/postpone state (Updater.h) is likewise written outside this form (a JSON RMW),
        // so preserve it too — only allowUpdatePrerelease (above) is the form's to write.
        {
            const auto disk = ::Agentmaster::LoadAppSettings();
            _appSettings.summaryPanelWidthFraction = disk.summaryPanelWidthFraction;
            _appSettings.summaryPanelHeightFraction = disk.summaryPanelHeightFraction;
            _appSettings.summaryPanelWrapNewlines = disk.summaryPanelWrapNewlines; // wrap-line toggle (panel times bar), out-of-cog UI action
            _appSettings.updateSkippedVersion = disk.updateSkippedVersion; // updater "Skip this version" (out-of-cog JSON RMW)
            _appSettings.updatePostponedUntilUnixMs = disk.updatePostponedUntilUnixMs; // updater "Postpone N days" (out-of-cog JSON RMW)
            _appSettings.envDefaultsVersion = disk.envDefaultsVersion; // shipped-default seed marker (engine-init, out-of-cog) — a Save must never reset it (would re-add a deleted default)
            _appSettings.claudeCleanupDaysSeeded = disk.claudeCleanupDaysSeeded; // shipped-default seed marker (engine-init, out-of-cog)
        }
        if (_settingsSink)
        {
            // Nav audit: the user saved the global Settings (cog) — the one consequential cog action; it
            // governs every future session. Record the most behavior-impacting fields (the rest persist to
            // settings.json, the durable record).
            ::Agentmaster::LogNav(std::wstring{ L"settings-save skipPerms=" } + (_appSettings.skipPermissions ? L"1" : L"0") +
                                  L" model=" + (_appSettings.model.empty() ? std::wstring{ L"(default)" } : _appSettings.model) +
                                  L" autorunner=" + (_appSettings.defaultAutorunnerMode == AutorunnerMode::Full ? L"Full" : _appSettings.defaultAutorunnerMode == AutorunnerMode::SemiAuto ? L"Semi" : L"Off") +
                                  L" claudeExe=" + (_appSettings.claudeExePath.empty() ? std::wstring{ L"(auto)" } : _appSettings.claudeExePath));
            _settingsSink(_appSettings); // page persists + applies to future spawns
        }
        // Native-exe-only policy: re-resolve the claude.exe now, so a changed/cleared override (or a
        // freshly-installed binary) takes effect this run — no restart needed (RefreshClaudeExe updates
        // the shared engine's cached path; ClaudeAvailable() flips accordingly).
        ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
        _HideSettings();
    }

    // === ENV_VARS.md: the "Environment variables" area (Global / Per-directory two-tab editor) ===========
    // The plumbing already merges global + per-dir at spawn (ResolveSessionEnv); this is the editor.

    void AgentManagerContent::_BuildEnvVarsArea(const StackPanel& panel)
    {
        // Section header (matches the "CLAUDE SESSIONS" style above it).
        panel.Children().Append(Text(L"ENVIRONMENT VARIABLES", 11, true, 0.6));

        // Tab toggle: [ Global ][ Per-directory ] — two Buttons swapping the two panels (the LOCAL/GLOBAL
        // scope-toggle idiom; not a Pivot, which themes unreliably under XAML Islands).
        auto tabs = StackPanel{};
        tabs.Orientation(Orientation::Horizontal);
        tabs.Spacing(0);
        tabs.Margin(Thickness{ 0, 2, 0, 4 });
        _setEnvTabGlobal = Button{};
        _setEnvTabGlobal.Content(box_value(L"Global"));
        _setEnvTabGlobal.FontSize(12);
        _setEnvTabGlobal.Padding(Thickness{ 12, 2, 12, 2 });
        _setEnvTabGlobal.BorderThickness(Thickness{ 0, 0, 0, 0 });
        AgentSetTip(_setEnvTabGlobal, L"Variables applied to EVERY session (Claude and Codex), in every directory.");
        _setEnvTabGlobal.Click([this](const IInspectable&, const RoutedEventArgs&) { _SwitchEnvTab(false); });
        _setEnvTabDir = Button{};
        _setEnvTabDir.Content(box_value(L"Per-directory"));
        _setEnvTabDir.FontSize(12);
        _setEnvTabDir.Padding(Thickness{ 12, 2, 12, 2 });
        _setEnvTabDir.BorderThickness(Thickness{ 0, 0, 0, 0 });
        AgentSetTip(_setEnvTabDir, L"Variables added only for sessions launched in a chosen working directory \x2014 they OVERRIDE a Global variable of the same name.");
        _setEnvTabDir.Click([this](const IInspectable&, const RoutedEventArgs&) { _SwitchEnvTab(true); });
        tabs.Children().Append(_setEnvTabGlobal);
        tabs.Children().Append(_setEnvTabDir);
        panel.Children().Append(tabs);

        // A multi-line NAME=VALUE editor, monospaced; its OWN border is suppressed so the wrapping Border
        // (recolored by the live lexer) is the status indicator.
        const auto makeEditor = [](TextBox& box) {
            box = TextBox{};
            box.AcceptsReturn(true);
            box.TextWrapping(TextWrapping::Wrap);
            box.FontFamily(FontFamily{ L"Consolas" });
            box.FontSize(12);
            box.MinHeight(84);
            box.MaxHeight(168);
            box.BorderThickness(Thickness{ 0, 0, 0, 0 });
            ScrollViewer::SetVerticalScrollBarVisibility(box, ScrollBarVisibility::Auto);
        };
        const auto wrapInBorder = [](const TextBox& box, Border& border) {
            border = Border{};
            border.BorderThickness(Thickness{ 1, 1, 1, 1 });
            border.CornerRadius(CornerRadius{ 2, 2, 2, 2 });
            border.BorderBrush(Fill(0x60, 0x80, 0x80, 0x80)); // subtle neutral until the lexer paints it
            border.Child(box);
        };

        // --- Global panel ---
        _envGlobalPanel = StackPanel{};
        _envGlobalPanel.Spacing(2);
        {
            auto hint = Text(L"One NAME=VALUE per line (e.g. HTTPS_PROXY=http://h:8080). Applied to every session.", 11, false, 0.55);
            hint.TextWrapping(TextWrapping::Wrap);
            _envGlobalPanel.Children().Append(hint);
        }
        makeEditor(_setEnv);
        _setEnv.PlaceholderText(L"NAME=VALUE\nNAME=VALUE");
        AgentSetTip(_setEnv, L"Environment variables applied to every launched session (Claude and Codex). One NAME=VALUE per line; '#' starts a comment.");
        _setEnv.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _RefreshEnvLex(false); });
        wrapInBorder(_setEnv, _setEnvBorder);
        _envGlobalPanel.Children().Append(_setEnvBorder);
        _setEnvStatus = Text(L"", 11, false, 0.7);
        _envGlobalPanel.Children().Append(_setEnvStatus);
        panel.Children().Append(_envGlobalPanel);

        // --- Per-directory panel (hidden until its tab is picked) ---
        _envDirPanel = StackPanel{};
        _envDirPanel.Spacing(2);
        _envDirPanel.Visibility(Visibility::Collapsed);
        {
            auto hint = Text(L"Pick a working directory, then add variables for sessions launched there \x2014 they override the Global ones of the same name.  \x25CF = already has variables.", 11, false, 0.55);
            hint.TextWrapping(TextWrapping::Wrap);
            _envDirPanel.Children().Append(hint);
        }
        _setEnvDirFilter = TextBox{};
        _setEnvDirFilter.PlaceholderText(L"type to filter directories\x2026");
        _setEnvDirFilter.FontSize(12);
        _setEnvDirFilter.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _RebuildEnvDirList(); });
        _envDirPanel.Children().Append(_setEnvDirFilter);
        _setEnvDirList = ListBox{};
        _setEnvDirList.MaxHeight(120);
        _setEnvDirList.SelectionChanged([this](const IInspectable&, const SelectionChangedEventArgs&) {
            if (!_setEnvDirList)
            {
                return;
            }
            const auto sel = _setEnvDirList.SelectedItem().try_as<ListBoxItem>();
            if (!sel)
            {
                return; // a Clear() during rebuild fires SelectionChanged with no item — keep editing the current dir
            }
            const std::wstring composite{ winrt::unbox_value_or<winrt::hstring>(sel.Tag(), L"") };
            const auto sepPos = composite.find(L'\x1f');
            if (sepPos == std::wstring::npos)
            {
                return;
            }
            _SelectEnvDir(composite.substr(0, sepPos), composite.substr(sepPos + 1));
        });
        _envDirPanel.Children().Append(_setEnvDirList);
        makeEditor(_setEnvDir);
        _setEnvDir.IsEnabled(false);
        _setEnvDir.PlaceholderText(L"select a directory above");
        _setEnvDir.Header(box_value(L"Variables for the selected directory"));
        _setEnvDir.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _RefreshEnvLex(true); });
        wrapInBorder(_setEnvDir, _setEnvDirBorder);
        _envDirPanel.Children().Append(_setEnvDirBorder);
        _setEnvDirStatus = Text(L"", 11, false, 0.7);
        _envDirPanel.Children().Append(_setEnvDirStatus);
        panel.Children().Append(_envDirPanel);

        _SwitchEnvTab(false); // start on Global (also styles the tab buttons)
    }

    void AgentManagerContent::_SwitchEnvTab(bool perDir)
    {
        _envTabIsDir = perDir;
        if (_envGlobalPanel)
        {
            _envGlobalPanel.Visibility(perDir ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_envDirPanel)
        {
            _envDirPanel.Visibility(perDir ? Visibility::Visible : Visibility::Collapsed);
        }
        const auto style = [](const Button& b, bool active) {
            if (!b)
            {
                return;
            }
            b.Background(active ? Fill(0xFF, 0x0E, 0x63, 0x9C) : Fill(0x00, 0x00, 0x00, 0x00));
            b.Foreground(active ? Fill(0xFF, 0xFF, 0xFF, 0xFF) : Fill(0xFF, 0xB0, 0xB0, 0xB0));
        };
        style(_setEnvTabGlobal, !perDir);
        style(_setEnvTabDir, perDir);
        if (perDir)
        {
            _RebuildEnvDirList();
        }
    }

    void AgentManagerContent::_RebuildEnvDirList()
    {
        if (!_setEnvDirList)
        {
            return;
        }
        const auto lc = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        const auto isBlank = [](const std::wstring& v) {
            for (const wchar_t c : v)
            {
                if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
                {
                    return false;
                }
            }
            return true;
        };

        // Candidates: every live session's working dir UNION every dir already holding env in the draft.
        std::vector<std::pair<std::wstring, std::wstring>> cands; // (NormDirKey, displayPath)
        std::unordered_set<std::wstring> seen;
        if (_registry)
        {
            for (const auto& s : _registry->Snapshot())
            {
                if (s.workingDir.empty())
                {
                    continue;
                }
                const std::wstring key = ::Agentmaster::NormDirKey(s.workingDir);
                if (seen.insert(key).second)
                {
                    cands.emplace_back(key, s.workingDir);
                }
            }
        }
        std::unordered_set<std::wstring> withEnv;
        for (const auto& [k, env] : _dirEnvDraft)
        {
            if (!isBlank(env))
            {
                withEnv.insert(k);
            }
            if (seen.insert(k).second)
            {
                cands.emplace_back(k, k); // no live session in this dir — show the key itself
            }
        }

        const std::wstring filt = lc(std::wstring{ _setEnvDirFilter ? _setEnvDirFilter.Text() : winrt::hstring{} });
        std::vector<std::pair<std::wstring, std::wstring>> shown;
        for (const auto& c : cands)
        {
            if (filt.empty() || lc(c.second).find(filt) != std::wstring::npos)
            {
                shown.push_back(c);
            }
        }
        std::sort(shown.begin(), shown.end(), [&](const auto& a, const auto& b) {
            const bool ea = withEnv.count(a.first) != 0;
            const bool eb = withEnv.count(b.first) != 0;
            if (ea != eb)
            {
                return ea; // dirs that already have env sort first
            }
            return lc(a.second) < lc(b.second);
        });

        _setEnvDirList.Items().Clear();
        for (const auto& [key, disp] : shown)
        {
            ListBoxItem item{};
            const bool has = withEnv.count(key) != 0;
            item.Content(box_value(winrt::hstring{ (has ? std::wstring{ L"\x25CF  " } : std::wstring{ L"     " }) + disp }));
            item.Tag(box_value(winrt::hstring{ key + L"\x1f" + disp })); // (NormDirKey, displayPath) for SelectionChanged
            item.FontSize(12);
            _setEnvDirList.Items().Append(item);
        }
    }

    void AgentManagerContent::_SelectEnvDir(const std::wstring& normKey, const std::wstring& displayPath)
    {
        if (!_setEnvDir)
        {
            return;
        }
        // The previously-edited dir's text is already mirrored into _dirEnvDraft by _RefreshEnvLex(true)
        // (fired on every keystroke), so switching keys loses nothing.
        _envEditingDirKey = normKey;
        std::wstring text;
        for (const auto& [k, env] : _dirEnvDraft)
        {
            if (k == normKey)
            {
                text = env;
                break;
            }
        }
        _setEnvDir.IsEnabled(true);
        _setEnvDir.Header(box_value(winrt::hstring{ L"Variables for: " + displayPath }));
        _setEnvDir.Text(winrt::hstring{ text }); // fires _RefreshEnvLex(true) -> lex + (idempotent) draft upsert under normKey
    }

    void AgentManagerContent::_RefreshEnvLex(bool perDir)
    {
        const auto box = perDir ? _setEnvDir : _setEnv;
        const auto border = perDir ? _setEnvDirBorder : _setEnvBorder;
        const auto status = perDir ? _setEnvDirStatus : _setEnvStatus;
        if (!box)
        {
            return;
        }
        const std::wstring text{ box.Text() };

        // Keep the per-dir draft current as the user types (the global side writes _appSettings only on Save).
        if (perDir && !_envEditingDirKey.empty())
        {
            bool found = false;
            for (auto& [k, env] : _dirEnvDraft)
            {
                if (k == _envEditingDirKey)
                {
                    env = text;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                _dirEnvDraft.emplace_back(_envEditingDirKey, text);
            }
        }

        const auto res = ::Agentmaster::LexEnvText(text);

        // Same palette as _ValidateLaunchBox: green ok / amber warn / red error; a subtle gray at rest.
        const SolidColorBrush green = Fill(0xFF, 0x4C, 0xAF, 0x50);
        const SolidColorBrush red = Fill(0xFF, 0xE5, 0x39, 0x35);
        const SolidColorBrush amber = Fill(0xFF, 0xDA, 0xA5, 0x20);
        const SolidColorBrush neutral = Fill(0x60, 0x80, 0x80, 0x80);
        const SolidColorBrush dim = Fill(0xFF, 0x99, 0x99, 0x99);

        const bool empty = (res.ok == 0 && res.warn == 0 && res.error == 0);
        if (border)
        {
            const SolidColorBrush bc = empty ? neutral :
                                       res.worst == ::Agentmaster::EnvLineKind::Error ? red :
                                       res.worst == ::Agentmaster::EnvLineKind::Warn  ? amber :
                                                                                        green;
            border.BorderBrush(bc);
        }
        if (status)
        {
            std::wstring msg;
            SolidColorBrush fg = dim;
            if (empty)
            {
                msg = (perDir && _envEditingDirKey.empty()) ? L"Select a directory to edit its variables." : L"No variables.";
            }
            else if (res.error > 0)
            {
                msg = L"\x2715 " + res.firstIssue + L" (line " + std::to_wstring(res.firstIssueLine) + L")";
                fg = red;
            }
            else if (res.warn > 0)
            {
                msg = L"\x26A0 " + std::to_wstring(res.ok) + (res.ok == 1 ? L" variable \x00B7 " : L" variables \x00B7 ") + res.firstIssue + L" (line " + std::to_wstring(res.firstIssueLine) + L")";
                fg = amber;
            }
            else
            {
                msg = L"\x2713 " + std::to_wstring(res.ok) + (res.ok == 1 ? L" variable" : L" variables");
                fg = green;
            }
            status.Text(winrt::hstring{ msg });
            status.Foreground(fg);
        }
    }

    void AgentManagerContent::_LoadEnvVarsArea()
    {
        // Global: show the stored env one-per-line. A legacy ';'-delimited value reads as multi-line ('; '
        // is only ever a separator — values can't contain it — so this is lossless), then saves back
        // newline-delimited (ParseEnvAssignments accepts both).
        if (_setEnv)
        {
            std::wstring disp = _appSettings.env;
            for (auto& c : disp)
            {
                if (c == L';')
                {
                    c = L'\n';
                }
            }
            _setEnv.Text(winrt::hstring{ disp });
        }
        // Per-directory: load the on-disk map into the editable draft; reset the selector + editor (the
        // draft is the working copy until Save, so Cancel discards any per-dir edits made in the modal).
        _dirEnvDraft = ::Agentmaster::LoadDirEnv();
        _envEditingDirKey.clear();
        if (_setEnvDirFilter)
        {
            _setEnvDirFilter.Text(L"");
        }
        if (_setEnvDir)
        {
            _setEnvDir.Text(L"");
            _setEnvDir.IsEnabled(false);
            _setEnvDir.Header(box_value(L"Variables for the selected directory"));
        }
        _RebuildEnvDirList();
        _RefreshEnvLex(false);
        _RefreshEnvLex(true);
        _SwitchEnvTab(false); // always open on the Global tab
    }

    void AgentManagerContent::_SaveEnvVarsArea()
    {
        if (_setEnv)
        {
            _appSettings.env = std::wstring{ _setEnv.Text() }; // newline-delimited; ParseEnvAssignments handles it
        }
        // Flush the per-directory draft (the active editor is already mirrored in by _RefreshEnvLex). Drop
        // blank entries so a cleared editor removes a dir (matches SetDirEnv / DeserializeDirEnv semantics).
        std::vector<std::pair<std::wstring, std::wstring>> clean;
        clean.reserve(_dirEnvDraft.size());
        for (const auto& [k, env] : _dirEnvDraft)
        {
            bool blank = true;
            for (const wchar_t c : env)
            {
                if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
                {
                    blank = false;
                    break;
                }
            }
            if (!k.empty() && !blank)
            {
                clean.emplace_back(k, env);
            }
        }
        ::Agentmaster::SaveDirEnv(clean);
    }

    void AgentManagerContent::SetQuitForUpdateHandler(std::function<void()> handler)
    {
        _quitForUpdateHandler = std::move(handler);
    }

    void AgentManagerContent::_CheckForUpdates(bool interactive)
    {
        // A double-click of the button must not stack two prompts; a silent on-open check is allowed
        // to overlap (both are idempotent GitHub reads — the later result just wins the label).
        if (interactive && _interactiveUpdateInFlight)
        {
            return;
        }
        // The worker marshals its result back via the dispatcher; with no dispatcher it could never
        // re-enable the button / clear the in-flight flag, so bail before we touch either.
        if (!_dispatcher)
        {
            return;
        }
        // Release-channel only (Updater::IsUpdaterChannel): a dev/unpackaged build must NOT present a
        // GitHub release as a self-update — it's a different package and always-"behind" the 0.0.1.0
        // placeholder. _ShowSettings already disables the button + shows the explanatory note there;
        // this is the backstop so a stray call never runs the misleading check.
        if (!::Agentmaster::Updater::IsUpdaterChannel())
        {
            return;
        }
        if (interactive)
        {
            _interactiveUpdateInFlight = true;
            if (_setCheckUpdates)
            {
                _setCheckUpdates.IsEnabled(false);
                _setCheckUpdates.Content(winrt::box_value(L"Checking\x2026"));
            }
            if (_setUpdateStatus)
            {
                _setUpdateStatus.Text(L"Checking GitHub\x2026");
                _setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0xB0, 0xB0, 0xB0) });
            }
        }

        // Prefer the LIVE pre-release toggle (so toggling it, then clicking Check, uses the new value
        // before a Save) over the saved setting.
        bool prerelease = _appSettings.allowUpdatePrerelease;
        if (_setAllowPrerelease)
        {
            prerelease = _setAllowPrerelease.IsOn();
        }
        // Nav audit: the user clicked "Check for updates" (interactive only — the silent on-cog-open check
        // is automatic, never logged). The result lands in the cog label, not the trail.
        if (interactive)
        {
            ::Agentmaster::LogNav(std::wstring{ L"check-for-updates (allowPrerelease=" } + (prerelease ? L"1" : L"0") + L")");
        }

        const std::wstring stateDir = ::Agentmaster::Profiles::ResolveProfileDir();
        const auto cur = ::Agentmaster::Updater::CurrentPackageVersion();

        auto weak = get_weak();
        auto disp = _dispatcher;
        std::thread([weak, disp, interactive, prerelease, stateDir, cur]() {
            // The network round-trip (Updater.h uses WinHTTP) — bounded; a longer budget for the
            // explicit button than the silent on-open check.
            const ::Agentmaster::Updater::UpdateInfo info =
                ::Agentmaster::Updater::CheckForUpdate(cur, prerelease, interactive ? 8000 : 5000);
            if (!disp)
            {
                return;
            }
            disp.TryEnqueue([weak, interactive, stateDir, info]() {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                if (interactive)
                {
                    self->_interactiveUpdateInFlight = false;
                    if (self->_setCheckUpdates)
                    {
                        self->_setCheckUpdates.IsEnabled(true);
                        self->_setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
                    }
                }
                if (self->_setUpdateStatus)
                {
                    if (info.available)
                    {
                        // "v0.4.3 available!" — dark green, legible on the dark settings card.
                        self->_setUpdateStatus.Text(winrt::hstring{ ::Agentmaster::Updater::DisplayVersion(info) + L" available!" });
                        self->_setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x2E, 0xA0, 0x43) });
                    }
                    else if (interactive)
                    {
                        self->_setUpdateStatus.Text(info.checked ? winrt::hstring{ L"You're on the latest version" } : winrt::hstring{ L"Couldn't reach GitHub" });
                        self->_setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x99, 0x99, 0x99) });
                    }
                    else
                    {
                        self->_setUpdateStatus.Text(L""); // silent check: stay quiet unless there IS an update
                    }
                }
                // "Update's changelog" link: reveal it (and remember its release page) when an update is
                // available — from EITHER the silent on-open check or the explicit button — else hide it.
                if (info.available)
                {
                    self->_lastUpdateChangelogUrl = ::Agentmaster::Updater::ChangelogUrl(info);
                    if (self->_setUpdateChangelog)
                    {
                        self->_setUpdateChangelog.Visibility(Visibility::Visible);
                    }
                }
                else
                {
                    self->_lastUpdateChangelogUrl.clear();
                    if (self->_setUpdateChangelog)
                    {
                        self->_setUpdateChangelog.Visibility(Visibility::Collapsed);
                    }
                }
                // Interactive only: prompt + apply on a found update (the silent on-open check just labels).
                if (interactive && info.available)
                {
                    const HWND owner = ::GetActiveWindow();
                    const auto d = ::Agentmaster::Updater::ShowUpdatePrompt(owner, info);
                    if (::Agentmaster::Updater::ApplyDecision(stateDir, info, d, owner))
                    {
                        // The installer was launched detached; close the app gracefully so the package
                        // isn't in use while it upgrades + relaunches (the page's RequestQuit).
                        if (self->_quitForUpdateHandler)
                        {
                            self->_quitForUpdateHandler();
                        }
                    }
                }
            });
        }).detach();
    }

    // ---- "Claude not detected" overlay (native-exe-only policy gate) --------

    void AgentManagerContent::_BuildClaudeMissingOverlay()
    {
        // Mirrors the settings overlay: a dimmed modal in the main tree (NOT a ContentDialog, so its
        // buttons + text behave in XAML Islands). Shown when a launch/fork is attempted with no native
        // claude.exe resolved (ClaudeAvailable() false).
        _claudeMissingOverlay = Grid{};
        _claudeMissingOverlay.Visibility(Visibility::Collapsed);
        _claudeMissingOverlay.Background(SolidColorBrush{ ColorHelper::FromArgb(0xA0, 0x00, 0x00, 0x00) });
        Grid::SetRow(_claudeMissingOverlay, 0);
        Grid::SetRowSpan(_claudeMissingOverlay, 99);
        Grid::SetColumnSpan(_claudeMissingOverlay, 99);
        _claudeMissingOverlay.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
            _HideClaudeMissing();
        });

        auto card = Border{};
        card.Background(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x25, 0x25, 0x25) });
        card.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0x80, 0x80, 0x80) });
        card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        card.Padding(Thickness{ 20, 16, 20, 16 });
        card.Width(500);
        card.HorizontalAlignment(HorizontalAlignment::Center);
        card.VerticalAlignment(VerticalAlignment::Center);
        card.RequestedTheme(ElementTheme::Dark);
        card.Tapped([](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs& e) {
            e.Handled(true);
        });

        auto panel = StackPanel{};
        panel.Spacing(10);
        panel.Children().Append(Text(L"\x26A0 Claude Code (native) not found", 18, true, 1.0));
        {
            auto body = TextBlock{};
            body.TextWrapping(TextWrapping::Wrap);
            body.Opacity(0.9);
            body.FontSize(13);
            body.Text(L"Agentmaster drives the native claude.exe. None was found on PATH, in "
                      L"%USERPROFILE%\\.local\\bin, or behind an npm claude.cmd. Launching, resuming, and "
                      L"forking are disabled until one is available \x2014 a pure-Node `claude` is not "
                      L"supported (the fleet view, adoption, and process insight all need the native binary).");
            panel.Children().Append(body);
        }
        {
            auto how = TextBlock{};
            how.TextWrapping(TextWrapping::Wrap);
            how.Opacity(0.9);
            how.FontSize(13);
            how.Text(L"Install it: open a terminal and run   claude install   (migrates an existing "
                     L"Claude Code to the native build), then click Re-check. Or get Claude Code below, "
                     L"or Browse\x2026 to a claude.exe you already have.");
            panel.Children().Append(how);
        }
        _claudeMissingStatus = TextBlock{};
        _claudeMissingStatus.TextWrapping(TextWrapping::Wrap);
        _claudeMissingStatus.Opacity(0.7);
        _claudeMissingStatus.FontSize(12);
        panel.Children().Append(_claudeMissingStatus);

        auto buttons = StackPanel{};
        buttons.Orientation(Orientation::Horizontal);
        buttons.HorizontalAlignment(HorizontalAlignment::Right);
        buttons.Spacing(8);
        buttons.Margin(Thickness{ 0, 8, 0, 0 });
        auto getClaude = Button{};
        getClaude.Content(winrt::box_value(L"Get Claude Code"));
        AgentSetTip(getClaude, L"Open the Claude Code setup docs in your browser.");
        getClaude.Click([](const IInspectable&, const RoutedEventArgs&) {
            try
            {
                winrt::Windows::System::Launcher::LaunchUriAsync(Uri{ L"https://code.claude.com/docs/en/setup" });
            }
            CATCH_LOG();
        });
        auto browse = Button{};
        browse.Content(winrt::box_value(L"Browse for claude.exe\x2026"));
        AgentSetTip(browse, L"Pick an existing claude.exe with a file dialog \x2014 sets it as the override and re-resolves.");
        browse.Click([this](const IInspectable&, const RoutedEventArgs&) { _BrowseForClaudeExe(false); });
        auto recheck = Button{};
        recheck.Content(winrt::box_value(L"Re-check"));
        AgentSetTip(recheck, L"Look for claude.exe again \x2014 click after running claude install or installing the native build.");
        recheck.Click([this](const IInspectable&, const RoutedEventArgs&) {
            const auto exe = ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
            if (!exe.empty())
            {
                _HideClaudeMissing();
                _ValidateLaunchBox();
            }
            else if (_claudeMissingStatus)
            {
                _claudeMissingStatus.Text(L"Still not found. Run `claude install`, or Browse to a claude.exe.");
            }
        });
        auto close = Button{};
        close.Content(winrt::box_value(L"Close"));
        AgentSetTip(close, L"Dismiss this notice \x2014 Claude launch / resume / fork stay disabled until a native claude.exe is found.");
        close.Click([this](const IInspectable&, const RoutedEventArgs&) { _HideClaudeMissing(); });
        buttons.Children().Append(getClaude);
        buttons.Children().Append(browse);
        buttons.Children().Append(recheck);
        buttons.Children().Append(close);
        panel.Children().Append(buttons);

        card.Child(panel);
        _claudeMissingOverlay.Children().Append(card);
        _root.Children().Append(_claudeMissingOverlay);
    }

    void AgentManagerContent::_ShowClaudeMissing()
    {
        if (!_claudeMissingOverlay)
        {
            return;
        }
        if (_claudeMissingStatus)
        {
            const auto& exe = ::Agentmaster::SharedEngine().claudeExePath;
            _claudeMissingStatus.Text(winrt::hstring{ exe.empty() ? std::wstring{ L"Status: no native claude.exe detected." } : (L"Status: using " + exe) });
        }
        _claudeMissingOverlay.Visibility(Visibility::Visible);
    }

    void AgentManagerContent::_HideClaudeMissing()
    {
        if (_claudeMissingOverlay)
        {
            _claudeMissingOverlay.Visibility(Visibility::Collapsed);
        }
    }

    void AgentManagerContent::_BrowseForClaudeExe(bool fromSettings)
    {
        if (!_dispatcher)
        {
            return;
        }
        // Defer off the click tick, then run the COM modal (it needs the message pump — the XAML-Islands
        // rule the profile picker follows). A picked .exe becomes the persisted override + re-resolves.
        _dispatcher.TryEnqueue([this, fromSettings]() {
            const auto picked = PickClaudeExe(::GetActiveWindow());
            if (!picked || picked->empty())
            {
                return;
            }
            _appSettings.claudeExePath = *picked;
            if (_settingsSink)
            {
                _settingsSink(_appSettings); // persist the override
            }
            const auto exe = ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
            if (fromSettings && _setClaudeExePath)
            {
                _setClaudeExePath.Text(winrt::hstring{ _appSettings.claudeExePath });
            }
            if (fromSettings && _setClaudeDetected)
            {
                _setClaudeDetected.Text(winrt::hstring{ exe.empty() ? std::wstring{ L"Detected: none" } : (L"Detected: " + exe) });
            }
            if (!exe.empty())
            {
                _HideClaudeMissing();
                _ValidateLaunchBox();
            }
            else if (_claudeMissingStatus)
            {
                _claudeMissingStatus.Text(L"That file isn't a usable claude.exe \x2014 pick the native claude.exe.");
            }
        });
    }

}
