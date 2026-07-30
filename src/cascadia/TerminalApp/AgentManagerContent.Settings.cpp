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
#include "AgentModelPrompt.h" // AgentBuildSpecifyModelCard — the shared "Specify a model..." prompt this content hosts

// Agentmaster (launch-model picker -> "Specify..."): the successor-model combos' sentinel id for
// their "Specify..." row. A control character prefix keeps it disjoint from every real model id
// (settings-typed ids are printable), so it can never collide with something a user configured.
static constexpr std::wstring_view kSpecifyModelSentinel = L"" "specify-model";
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/RegexUtil.h" // COMMANDS.md §6b — live validation of the Commands tab's regex boxes
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
using winrt::TerminalApp::implementation::AgentSetTitledTip;
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (StateColor/Pill/Text/...)

namespace
{
    // Agentmaster (Settings cog): a styled SECTION SEPARATOR — an optional uppercase title on the left
    // followed by a thin horizontal hairline that fills the rest of the row. It replaces the bare
    // Text(L"HEADER", 11, true, 0.6) section labels (same 11pt SemiBold 60%-opacity type, now carrying the
    // dividing rule the flat stacks lacked) AND subdivides the crowded tabs (e.g. Tabs & Overlay) into
    // labeled groups, so related settings read as one block instead of one long stack. Built as a 2-column
    // Grid (Auto title | Star rule) so the rule always stretches to the panel's right edge regardless of
    // title length or window width. A blank / nullptr title yields a plain full-width rule — an UNLABELED
    // divider within a group. `leading` (the FIRST separator in a panel) drops the extra top margin so the
    // opening group isn't pushed down from the card's top; every later separator gets it, so a group break
    // reads ~20px (the panel's own 10px item spacing + this 10px) against the 10px between in-group rows.
    winrt::Windows::UI::Xaml::UIElement SettingsSeparator(const wchar_t* title, bool leading = false)
    {
        auto grid = Grid{};
        grid.Margin(Thickness{ 0, leading ? 0.0 : 10.0, 0, 0 });

        ColumnDefinition cLabel;
        cLabel.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto)); // the title hugs its text
        ColumnDefinition cRule;
        cRule.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); // the rule fills the rest
        grid.ColumnDefinitions().Append(cLabel);
        grid.ColumnDefinitions().Append(cRule);

        if (title && *title)
        {
            auto label = Text(winrt::hstring{ title }, 11, true, 0.6); // the prior section-header type look
            label.Margin(Thickness{ 0, 0, 8, 0 }); // gap between the title and the rule
            Grid::SetColumn(label, 0);
            grid.Children().Append(label);
        }

        auto rule = Border{};
        rule.Height(1);
        rule.VerticalAlignment(VerticalAlignment::Center); // centered on the title's row
        rule.Background(Fill(0x24, 0xFF, 0xFF, 0xFF)); // a subtle white hairline (the tab-strip divider tone)
        Grid::SetColumn(rule, 1);
        grid.Children().Append(rule);

        return grid;
    }

    // Agentmaster (COMMANDS.md §6b): read the Commands tab's "wait this many minutes for the
    // successor to start" box. ONE rule, because TWO readers consume it — the cog's Save and the
    // live status line — and a drift between them would show the user a sentence the Save doesn't
    // honor. Digit-scrape (the ClampMaxTags idiom) with a wider accumulator guard so the 30-day
    // ceiling fits, then clamped. A BLANK box means "the default"; a typed 0 is a REAL value
    // ("don't wait for the successor at all"), so the two must never collapse onto one meaning —
    // hence the anyDigit flag rather than leaning on the accumulator's 0.
    uint32_t ReadDeleteDeadlineMinutesBox(const winrt::Windows::UI::Xaml::Controls::TextBox& box)
    {
        if (!box)
        {
            return 1440;
        }
        const std::wstring t{ box.Text() };
        bool anyDigit = false;
        uint32_t v = 0;
        for (const wchar_t c : t)
        {
            if (c >= L'0' && c <= L'9')
            {
                anyDigit = true;
                if (v < 10000000)
                {
                    v = v * 10 + static_cast<uint32_t>(c - L'0');
                }
            }
        }
        return anyDigit ? ::Agentmaster::ClampCommandHandoverDeleteDeadlineMinutes(v) : 1440;
    }
}

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
        if (_activateAllBusy)
        {
            // The page's drip is running (500ms apart, 4 per 10s) — the operation takes many seconds, so
            // make it unmistakably "in progress": relabel + DISABLE the button (it stays put, showing the
            // remaining count ticking down as each tab wakes — a live progress readout), and keep it
            // visible even at 0 (SetActivateAllBusy(false) reverts to the normal hide-when-idle rule).
            _activateAllBtn.Content(winrt::box_value(winrt::hstring{ L"Activating " } + winrt::to_hstring(thisWindow) + L" tab" + (thisWindow == 1 ? L"" : L"s") + L"\x2026"));
            _activateAllBtn.IsEnabled(false);
            _activateAllBtn.Visibility(Visibility::Visible);
            _ReflowLaunchBar();
            return;
        }
        _activateAllBtn.IsEnabled(true);
        _activateAllBtn.Content(winrt::box_value(winrt::hstring{ L"Activate All Tabs (" } + winrt::to_hstring(thisWindow) + L")"));
        _activateAllBtn.Visibility(thisWindow > 0 ? Visibility::Visible : Visibility::Collapsed);
        _ReflowLaunchBar(); // the button just appeared/vanished/relabeled -> re-fit the row
    }

    // Agentmaster (eager-init / "Activate All Tabs" PACING): the page toggles this around its drip so the
    // Manager shows the operation is running. Disable the launch/cwd box (the user's "hands off, busy"
    // cue — the path picker is closed first so it can't linger over a disabled box), and refresh the
    // button into / out of its "Activating N tabs…" disabled state. Idempotent; UI thread only.
    void AgentManagerContent::SetActivateAllBusy(bool busy)
    {
        if (_activateAllBusy == busy)
        {
            return;
        }
        _activateAllBusy = busy;
        if (_cwdBox)
        {
            if (busy)
            {
                _ClosePathPicker(); // don't leave the dropdown hanging over a greyed-out box
            }
            _cwdBox.IsEnabled(!busy);
        }
        _UpdateActivateAllButton();
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
        card.Width(730); // Agentmaster: wider so the eight top-tab buttons (incl. Commands + Notifications) fit on one row
        card.HorizontalAlignment(HorizontalAlignment::Center);
        card.VerticalAlignment(VerticalAlignment::Center);
        card.RequestedTheme(ElementTheme::Dark);
        card.Tapped([](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs& e) {
            e.Handled(true);
        });

        // Agentmaster: the cog is organized into TOP TABS — a horizontal button-tab strip swapping one
        // scrollable panel per settings group (the _SwitchEnvTab idiom; deliberately NOT a Pivot, which
        // themes unreliably under XAML Islands). Save/Cancel is a fixed footer OUTSIDE the tabs (always
        // reachable). Each section below appends into one of the eight group panels; the `panel` variable is
        // RESEATED at every group boundary (a StackPanel is a ref-counted handle, so `panel = claudePanel`
        // just re-points it) so the per-control creation code stays byte-for-byte identical to before.
        auto outer = StackPanel{};
        outer.Spacing(8);
        outer.Children().Append(Text(L"Agentmaster Settings", 18, true, 1.0));

        // The eight group panels (built empty; filled by the sections below, in this label order).
        auto sessionsPanel = StackPanel{};
        sessionsPanel.Spacing(10);
        auto autorunnerPanel = StackPanel{};
        autorunnerPanel.Spacing(10);
        auto behaviorPanel = StackPanel{};
        behaviorPanel.Spacing(10);
        auto commandsPanel = StackPanel{}; // COMMANDS.md §6a — the /handover family customization
        commandsPanel.Spacing(10);
        auto notificationsPanel = StackPanel{};
        notificationsPanel.Spacing(10);
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
        _settingsTabResets.clear();
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
        // `reset` (optional): this tab's "Reset" handler — see _settingsTabResets. Pass nullptr
        // (the default, what every tab but Commands does today) and the footer button simply
        // isn't offered while that tab is showing.
        const auto addSettingsTab = [&](const wchar_t* label, const wchar_t* tip, const StackPanel& body, std::function<void()> reset = nullptr) {
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
            _settingsTabResets.push_back(std::move(reset)); // parallel-indexed; empty => no Reset button on this tab
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
            tip += L"\n\nExactly which build this is \x2014 quote it in a bug report. Release and Dev install side by side, so the channel says which of the two you are looking at.";
            AgentSetTitledTip(sub, L"This build", winrt::hstring{ tip });
            panel.Children().Append(sub);
        }

        // UPDATES (Agentmaster updater; Updater.h) — check GitHub Releases for a newer version
        // (the same prompt the startup check shows) + the pre-release opt-in. The status label
        // beside the button shows "vX.Y.Z available!" in dark green when a newer release exists
        // (filled by a silent check kicked when the cog opens — see _ShowSettings/_CheckForUpdates).
        panel.Children().Append(SettingsSeparator(L"UPDATES"));
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
        AgentSetTip(_setAllowPrerelease, L"When on, update checks also consider GitHub pre-releases (beta builds), not just stable releases. Off by default. Applies immediately \x2014 no Save needed.");
        // INSTANT-APPLY (no Save gate): flipping the switch persists immediately via a freshest-disk
        // RMW of just this field (the showSummaryPanel idiom) — a flip followed by a backdrop-tap
        // close used to be silently discarded, reading as "the checkbox doesn't persist". The form no
        // longer owns the field: the Save path preserves it from disk, the switch re-seeds from disk
        // at every cog open (behind _seedingAllowPrerelease so the programmatic IsOn can't re-fire
        // this), and the startup/hourly checks read settings.json fresh each pass (Updater::ReadPrefs)
        // — so no broadcast is needed for correctness anywhere.
        _setAllowPrerelease.Toggled([this](const IInspectable&, const RoutedEventArgs&) {
            if (_seedingAllowPrerelease)
            {
                return; // a cog-open seed, not a user flip
            }
            // Guarded: this runs straight off a XAML event — an escape would fail-fast the app over
            // a settings write. A failed flip is logged and harmless (the switch re-seeds from disk
            // at the next cog open, so UI and truth re-converge).
            try
            {
                const bool on = _setAllowPrerelease.IsOn();
                _appSettings.allowUpdatePrerelease = on;
                auto disk = ::Agentmaster::LoadAppSettings();
                disk.allowUpdatePrerelease = on;
                ::Agentmaster::SaveAppSettings(disk);
                ::Agentmaster::LogNav(std::wstring{ L"update-prerelease -> " } + (on ? L"on" : L"off"));
                // Re-run the silent check so the "vX.Y.Z available!" label + changelog link reflect the
                // new channel right away (release channel only — _CheckForUpdates gates itself).
                _CheckForUpdates(false);
            }
            catch (...)
            {
                try
                {
                    ::Agentmaster::LogNav(L"update-prerelease toggle FAILED (exception swallowed)");
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
            }
        });
        panel.Children().Append(_setAllowPrerelease);

        // NIGHTLY opt-in — a tier BELOW pre-release (Updater.h IsNightlyTag/ReleaseAllowedOnChannel):
        // a nightly is an unstable DEVELOPMENT build whose tag contains "nightly" (e.g.
        // v0.6.10-prerelease-nightly, published as a GitHub prerelease). It is ALWAYS skipped — by
        // the startup/hourly checks AND "Check for updates", even with pre-releases allowed — unless
        // this switch is on. Turning it ON is gated behind an explicit warning confirm: the Toggled
        // handler REVERTS the switch first (behind the seeding latch — it must never show ON while
        // nothing is accepted/persisted), asks, and re-applies programmatically only on "I
        // understand" — so EVERY attempt to enable re-asks, and a Cancel/backdrop dismiss leaves the
        // switch honestly OFF with no state written. OFF needs no warning. Same INSTANT-APPLY
        // freshest-disk RMW as the pre-release switch above (_ApplyAllowNightly).
        _setAllowNightly = ToggleSwitch{};
        _setAllowNightly.Header(winrt::box_value(L"Allow updating to nightly builds (unstable)"));
        AgentSetTip(_setAllowNightly, L"When on, update checks also consider NIGHTLY builds \x2014 unstable development versions (release tags containing \x201Cnightly\x201D) that may have memory leaks, CPU issues, and crashes. Nightlies are otherwise ALWAYS skipped, even with pre-releases allowed. Turning this on asks for confirmation first. Applies immediately \x2014 no Save needed.");
        _setAllowNightly.Toggled([this](const IInspectable&, const RoutedEventArgs&) {
            if (_seedingAllowNightly)
            {
                return; // a cog-open seed / our own revert-then-ask + confirm-apply writes, not a user flip
            }
            // Guarded like the pre-release switch: this runs straight off a XAML event — an escape
            // would fail-fast the app. A failed flip is logged and harmless (the switch re-seeds
            // from disk at the next cog open, so UI and truth re-converge).
            try
            {
                if (_setAllowNightly.IsOn())
                {
                    // Revert FIRST, then warn — the requested opt-in gate: nightly may cost the
                    // user their work, so it lands only on an explicit, informed accept.
                    _seedingAllowNightly = true;
                    _setAllowNightly.IsOn(false);
                    _seedingAllowNightly = false;
                    _Confirm(L"Enable nightly updates?",
                             L"Nightly builds are unstable development versions. They may have memory leaks, CPU issues, and crashes.\n\nUse them if you wish to contribute and help testing \x2014 but be willing to have your work suddenly interrupted.",
                             L"I understand \x2014 enable nightly",
                             [this]() {
                                 _seedingAllowNightly = true;
                                 _setAllowNightly.IsOn(true);
                                 _seedingAllowNightly = false;
                                 _ApplyAllowNightly(true);
                             });
                    return;
                }
                _ApplyAllowNightly(false); // turning OFF needs no warning
            }
            catch (...)
            {
                try
                {
                    ::Agentmaster::LogNav(L"update-nightly toggle FAILED (exception swallowed)");
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
            }
        });
        panel.Children().Append(_setAllowNightly);

        // (The "Uninstall Agentmaster…" button is built at the END of the About tab, after the Profile row.)

        // === SESSIONS tab ===
        panel = sessionsPanel;
        panel.Children().Append(SettingsSeparator(L"CLAUDE SESSIONS", true)); // leading section
        _setSkipPermissions = ToggleSwitch{};
        _setSkipPermissions.Header(winrt::box_value(L"Skip permission prompts (bypass)"));
        AgentSetTip(_setSkipPermissions, L"Launch new sessions with --dangerously-skip-permissions \x2014 auto-accepts tool prompts so an unattended session never wedges on one. Off pins normal prompts instead. (It does NOT skip Claude's workspace-trust dialog \x2014 that is the setting below.)");
        panel.Children().Append(_setSkipPermissions);
        // Agentmaster: pre-trust the working directory so Claude's startup "Is this a project you
        // trust?" modal can never park an unattended tab (SessionModels.h / ClaudeSpawn.h).
        _setTrustWorkspace = ToggleSwitch{};
        _setTrustWorkspace.Header(winrt::box_value(L"Trust the working directory automatically"));
        AgentSetTip(_setTrustWorkspace, L"Before launching, mark the session's folder (its git repo, if any) as trusted in ~/.claude.json \x2014 the fix Claude Code itself suggests. Without it every session in a new folder stops on Claude's \"Is this a project you trust?\" prompt, which an unattended tab cannot answer: no prompt is ever submitted, the Tests Autorunner can't drive it, and a /handover briefing is eaten by the menu. Worst in your home folder, which Claude refuses to remember \x2014 it asks again every single launch. Off = never touch ~/.claude.json and answer the prompt yourself.");
        panel.Children().Append(_setTrustWorkspace);
        _setModel = TextBox{};
        _setModel.Header(winrt::box_value(L"Model"));
        _setModel.PlaceholderText(L"As Is \x2014 blank keeps Claude's default (e.g. opus / sonnet)");
        AgentSetTip(_setModel, L"Model new sessions launch with (like /model) \x2014 e.g. opus or sonnet. Blank keeps Claude's own default.");
        panel.Children().Append(_setModel);
        // Launch-model picker: the models every "Open New Session Here" / "Fork session" submenu
        // offers (the Manager board/tree + External row menus, the Sessions page, the WT tab menu).
        // Multi-line like the env editor — one "Display name | model-id" per line; ParseLaunchModels
        // is lenient (';' separates too, '#' comments, a bare model id is its own label) — and LIVE-
        // LEXED exactly like the env editors: the box's own border is suppressed so the wrapping
        // Border (recolored green/amber/red by LexLaunchModelsText) is the status indicator, with a
        // counts + first-issue status line beneath (_RefreshLaunchModelsLex).
        _setLaunchModels = TextBox{};
        _setLaunchModels.Header(winrt::box_value(L"Launch models (the \x201COpen New Session Here\x201D / \x201C" L"Fork session\x201D picker)"));
        _setLaunchModels.AcceptsReturn(true);
        _setLaunchModels.TextWrapping(TextWrapping::NoWrap);
        _setLaunchModels.FontFamily(FontFamily{ L"Consolas" });
        _setLaunchModels.FontSize(12);
        _setLaunchModels.MinHeight(64);
        _setLaunchModels.MaxHeight(140);
        _setLaunchModels.BorderThickness(Thickness{ 0, 0, 0, 0 });
        ScrollViewer::SetVerticalScrollBarVisibility(_setLaunchModels, ScrollBarVisibility::Auto);
        _setLaunchModels.PlaceholderText(L"Display name | model-id \x2014 one per line (e.g. Opus | opus)");
        AgentSetTip(_setLaunchModels, L"The models every \x201COpen New Session Here\x201D and \x201C" L"Fork session\x201D submenu offers \x2014 on the Manager's board/tree and External menus, the Sessions page, and a tab's right-click menu. One \x201C" L"Display name | model-id\x201D per line: the left side is the menu label, the right is launched as --model <id> (that session only \x2014 \x201C" L"Default\x201D keeps the Model box above). An ALIAS (opus / sonnet / fable) always launches the LATEST model of that family; a full id (claude-opus-4-8) pins that exact version. '#' comments a line; clear the box to offer just Default.");
        _setLaunchModels.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _RefreshLaunchModelsLex(); });
        _setLaunchModelsBorder = Border{};
        _setLaunchModelsBorder.BorderThickness(Thickness{ 1, 1, 1, 1 });
        _setLaunchModelsBorder.CornerRadius(CornerRadius{ 2, 2, 2, 2 });
        _setLaunchModelsBorder.BorderBrush(Fill(0x60, 0x80, 0x80, 0x80)); // subtle neutral until the lexer paints it
        _setLaunchModelsBorder.Child(_setLaunchModels);
        panel.Children().Append(_setLaunchModelsBorder);
        _setLaunchModelsStatus = Text(L"", 11, false, 0.7);
        _setLaunchModelsStatus.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setLaunchModelsStatus);
        // Agentmaster (current-model adornment): the Anthropic model-FAMILY words the model shortener
        // recognizes (ShortModelName — the "fable-5" / "opus-4.6" text on the board card, the per-tab
        // overlay, and the tab tooltip). Only a BARE --model alias consults it (a full "claude-…" id —
        // the transcript's usual shape — shortens regardless), so this needs editing exactly once per
        // NEW family Anthropic ships. Plain comma-separated words; REPLACES the built-in list.
        _setModelFamilies = TextBox{};
        _setModelFamilies.Header(winrt::box_value(L"Model families (short model display)"));
        _setModelFamilies.PlaceholderText(winrt::hstring{ std::wstring{ L"blank = built-ins: " } + std::wstring{ ::Agentmaster::kDefaultModelFamilies } });
        AgentSetTip(_setModelFamilies, L"The Anthropic model-family words the short model display recognizes (comma-separated, case-insensitive) \x2014 shown as e.g. \x201C" L"fable-5\x201D on the board card, the tab tooltip, and the per-tab overlay. Add a word here when a new Claude family ships. Only a bare --model alias needs this; a full \x201C" L"claude-\x2026\x201D id always shortens. Blank falls back to the built-in list.");
        panel.Children().Append(_setModelFamilies);
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
        panel.Children().Append(SettingsSeparator(L"CLAUDE HISTORY", true)); // leading section
        _setCleanupDays = TextBox{};
        _setCleanupDays.Header(winrt::box_value(L"Keep Claude history (days)"));
        _setCleanupDays.PlaceholderText(L"e.g. 36500 (~never) \x2014 blank = Claude default (30 days)");
        AgentSetTip(_setCleanupDays, L"Sets cleanupPeriodDays in your global ~/.claude/settings.json \x2014 how many days Claude keeps session transcripts before deleting them at startup (your GLOBAL Claude history, used by Resume + the Sessions browser). 36500 \x2248 never. Blank removes the key (Claude's 30-day default). Avoid 0: in Claude it DISABLES history entirely.");
        panel.Children().Append(_setCleanupDays);

        // CLAUDE BINARY (native-exe-only policy): the auto-detected native claude.exe + an optional
        // explicit override. The whole app gates launch/fork/resume on resolving one (ResolveClaudeExe);
        // the override must be a real *.exe (a .cmd/.bat or the Node CLI is rejected).
        panel.Children().Append(SettingsSeparator(L"CLAUDE BINARY (native build required)"));
        _setClaudeDetected = TextBlock{};
        _setClaudeDetected.TextWrapping(TextWrapping::Wrap);
        _setClaudeDetected.Opacity(0.85);
        _setClaudeDetected.FontSize(12);
        AgentSetTitledTip(_setClaudeDetected, L"Detected claude.exe", L"The native claude.exe Agentmaster found, looking on your PATH, then in %USERPROFILE%\\.local\\bin, then behind an npm claude.cmd. Everything here drives that binary directly \x2014 a Claude that is only the Node CLI can't be tracked, so launch / resume / fork stay disabled until a real .exe turns up. Set one explicitly below if the wrong one was picked.");
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
        // One-click install/migrate — shown only when NO native claude.exe is detected (label + visibility
        // set in _RefreshSettingsForm from ClaudeInstallKind()). Opens a visible PowerShell window running
        // `claude install` (npm-legacy) or the official claude.ai bootstrap (none); the app re-resolves on
        // the next launch attempt / cog reopen, so no restart is needed.
        _setClaudeInstallBtn = Button{};
        _setClaudeInstallBtn.Content(winrt::box_value(L"Install Claude Code"));
        _setClaudeInstallBtn.Margin(Thickness{ 0, 6, 0, 0 });
        _setClaudeInstallBtn.Visibility(Visibility::Collapsed);
        AgentSetTip(_setClaudeInstallBtn, L"Install or migrate to the native claude.exe in a PowerShell window \x2014 the app re-checks automatically on your next launch.");
        _setClaudeInstallBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            const auto kind = ::Agentmaster::ClaudeInstallKind();
            if (kind != ::Agentmaster::ClaudeInstallState::Native)
            {
                ::Agentmaster::LaunchClaudeInstall(kind);
            }
        });
        panel.Children().Append(_setClaudeInstallBtn);

        // === TESTS AUTORUNNER tab ===
        panel = autorunnerPanel;
        panel.Children().Append(SettingsSeparator(L"TESTS AUTORUNNER (defaults for new sessions)", true)); // leading section
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
        _setPauseOnHuman.Header(winrt::box_value(L"Pause on human input (not wired up yet)"));
        // HONESTY (reviewed 2026-07-17): this toggle currently does NOTHING. Scheduler.h's DecideAdvance
        // gates on `lastHumanInputUnixMs != 0`, fed by SessionRegistry::NoteHumanInput — which has ZERO
        // call sites in the tree, so LastHumanInputUnixMs is always 0 and the gate can never fire. It is
        // the "feed pauseOnHumanInput from a TermControl input tap" follow-up (CLAUDE.md; PENDING_INPUT.md
        // §4/§6) — the setting round-trips and the scheduler is ready, only the keystroke tap is missing.
        // Say so rather than describe a behavior that doesn't happen; drop this caveat when it lands.
        AgentSetTip(_setPauseOnHuman, L"Meant to hold a session's queue while you are typing into its terminal yourself, so an auto-send can't land mid-sentence.\n\nNot wired up yet: nothing reports your keystrokes to the Tests Autorunner, so this toggle has no effect today. Your choice is saved and takes effect once it is connected.\n\nA prompt you actually SEND does stop the queue \x2014 the session goes Running, and nothing is ever auto-sent mid-turn.");
        panel.Children().Append(_setPauseOnHuman);
        // Agentmaster (PENDING_INPUT.md §9 — the DRAFT SWAP). Unlike the toggle above, this one is
        // live: it governs every prompt-submit path at once (auto-send, SemiAuto confirm, Send-now,
        // the /handover paste pump), which all share SessionRegistry::SubmitPrompt.
        _setPreserveDraft = ToggleSwitch{};
        _setPreserveDraft.Header(winrt::box_value(L"Preserve my unsent draft when a prompt is sent"));
        AgentSetTip(_setPreserveDraft, L"When a prompt is sent to a session whose input box already holds text you typed but have NOT sent, take your draft out of the way first and put it straight back afterwards \x2014 instead of pasting the prompt on top of it, which submitted your words and the prompt together as one message you never wrote.\n\nWhile it works (about a second) that tab's keyboard is locked so your typing cannot land mid-swap; you still see everything happening. Your draft is put back with the terminal's own undo buffer, so it comes back exactly as you typed it \x2014 unsent, one Enter away.\n\nIf the box cannot be cleared, NOTHING is sent: the prompt stays queued and your draft is left alone. Off restores the old behavior (the prompt is pasted into your draft).");
        panel.Children().Append(_setPreserveDraft);
        _setDraftSwapCtrlS = ToggleSwitch{};
        _setDraftSwapCtrlS.Header(winrt::box_value(L"Use Ctrl+S to stash my draft aside while it sends"));
        AgentSetTip(_setDraftSwapCtrlS, L"How the setting above moves your draft out of the way: with Claude's own stash (Ctrl+S), which lifts the WHOLE input box aside in one keystroke and puts it back exactly as it was \x2014 wherever your cursor happened to be.\n\nRecommended, and on by default. Turn it off only if Ctrl+S misbehaves in your build: the swap then falls back to the terminal's line-editor undo, which works a line at a time and can restore poorly when the cursor sits mid-text.\n\nNote Claude keeps ONE stash slot, so a swap replaces anything you had stashed yourself earlier. Your live draft is never at risk either way \x2014 if the box cannot be cleared, nothing is sent.");
        panel.Children().Append(_setDraftSwapCtrlS);
        // Agentmaster (PENDING_INPUT.md §10 — the restore RE-FILL). Claude never restores its own
        // input box, so a remembered unsent draft is typed back by us when the session reopens.
        _setRestoreDraft = ToggleSwitch{};
        _setRestoreDraft.Header(winrt::box_value(L"Re-type a remembered unsent draft when a session reopens"));
        AgentSetTip(_setRestoreDraft, L"A message you typed into a session's input box but never sent (its \"3 dots\" are pulsing) is remembered across an app restart or a close. When that session is resumed, this types the remembered draft back into the fresh input box \x2014 pasted WITHOUT pressing Enter, so it sits there exactly as you left it, one Enter away \x2014 and verifies it actually landed by reading the box back. Claude itself never restores its input box, so without this the memory only shows on the dots until the empty box clears it a few seconds after the tab starts.\n\nNever fights you: a session you (or the Tests Autorunner) already sent a prompt to is left alone, a box you started typing into keeps YOUR text, and a draft whose [Pasted text #N] placeholder cannot be matched to its cached content is not re-typed at all (a literal label would silently lose the pasted content when sent).");
        panel.Children().Append(_setRestoreDraft);

        // === BEHAVIOR tab ===
        panel = behaviorPanel;
        panel.Children().Append(SettingsSeparator(L"GENERAL", true)); // leading section (the tab itself is "Behavior")
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
        panel.Children().Append(SettingsSeparator(L"SESSION STATE"));
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
            // No Header on the box/slider (the row shares one label above them), so both name themselves.
            AgentSetTitledTip(_setWaitingDecayText, L"Waiting-for-you \x2192 Idle after", L"How long a session you have READ sits in Waiting-for-you before it may drop to Idle. Type a duration, combining days / hours / minutes:  d = days, h = hours, m = minutes  (a bare number means minutes).\n\nExamples:   3d   \x00B7   12h5m   \x00B7   2d4h30m   \x00B7   90m   \x00B7   45m   \x00B7   120 (= 2h)\n\nThis box is the real value \x2014 the slider beside it only reaches 7d, so anything longer typed here leaves it sitting at its maximum. The border turns red while the text can't be read. Nothing decays until you have actually visited the tab, and the \x201CNever\x201D toggle above stops it decaying at all.");
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
            AgentSetTitledTip(_setWaitingDecaySlider, L"Waiting-for-you \x2192 Idle after", L"How long a session sits in Waiting-for-you before it may drop to Idle \x2014 1 minute to 7 days. It only ever drops once you have READ it: an unread session keeps waiting however long the timeout says. The box on the left mirrors this and accepts more than 7 days; the \x201CNever\x201D toggle above stops it decaying at all.");
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
        panel.Children().Append(SettingsSeparator(L"LAUNCH DIRECTORY"));
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
        panel.Children().Append(SettingsSeparator(L"SESSIONS BROWSER"));
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

        // === NOTIFICATIONS tab (System notifications) ===
        // A Windows TOAST when a managed session's status leaves Running — the "your agent finished /
        // needs you" cue for tabs (and windows) you're not looking at:
        //     <session title>
        //     Has completed after <2h30m> and is <status>
        //     "<the truncated prompt whose turn just finished>"   (omitted when there is none)
        // The DEFAULT rule is "Running -> anything else" (master ON + every target state checked); the
        // checkboxes mute individual target states. Fired by the window hosting the session's tab on the
        // registry-observer push (TerminalPage::_EvaluateAgentNotification), so exactly one toast per
        // transition; clicking the toast jumps to the session's tab while the app is running.
        panel = notificationsPanel;
        panel.Children().Append(SettingsSeparator(L"SYSTEM NOTIFICATIONS", true)); // leading section
        _setNotifyEnabled = ToggleSwitch{};
        _setNotifyEnabled.Header(winrt::box_value(L"Show Windows notifications"));
        AgentSetTip(_setNotifyEnabled, L"Raise a Windows notification when a session's status changes from Running to another state \x2014 \x201C<title>: Has completed after 2h30m and is waiting for you\x201D, plus a third line quoting the prompt that just finished (so you can tell WHICH request came back). The checkboxes below pick which states notify. Default on.");
        // The master gates the rest: greying the dependents while OFF makes "nothing will fire" legible
        // at a glance (the _setWaitingNever slider-enable idiom).
        _setNotifyEnabled.Toggled([this](const IInspectable&, const RoutedEventArgs&) {
            if (!_setNotifyEnabled)
            {
                return;
            }
            const bool on = _setNotifyEnabled.IsOn();
            for (const auto& dep : { _setNotifyWaiting, _setNotifyNeedsApproval, _setNotifyIdle, _setNotifyDone, _setNotifyError })
            {
                if (dep)
                {
                    dep.IsEnabled(on);
                }
            }
            if (_setNotifySuppressFocused)
            {
                _setNotifySuppressFocused.IsEnabled(on);
            }
            if (_setNotifySound)
            {
                _setNotifySound.IsEnabled(on);
            }
        });
        panel.Children().Append(_setNotifyEnabled);
        {
            auto cap = Text(L"Notify when a running session becomes:", 13, false, 0.9);
            cap.Margin(Thickness{ 0, 2, 0, 0 });
            panel.Children().Append(cap);

            // The five target-state checkboxes, in the Triage-Board column order. All checked == the
            // default "Running to anything else" rule; unchecking one mutes just that transition.
            const auto mkStateBox = [&](CheckBox& slot, const wchar_t* label, const wchar_t* tip) {
                slot = CheckBox{};
                slot.Content(winrt::box_value(winrt::hstring{ label }));
                slot.MinWidth(0);
                slot.Margin(Thickness{ 8, 0, 0, 0 }); // indented under the caption
                AgentSetTip(slot, winrt::hstring{ tip });
                panel.Children().Append(slot);
            };
            mkStateBox(_setNotifyWaiting, L"Waiting for you", L"Notify on Running \x2192 Waiting-for-you \x2014 the turn completed and the session waits for your next prompt (or asked a question).");
            mkStateBox(_setNotifyNeedsApproval, L"Needs approval", L"Notify on Running \x2192 Needs-approval \x2014 the session is blocked on a permission prompt or a question you must answer.");
            mkStateBox(_setNotifyIdle, L"Idle", L"Notify on Running \x2192 Idle \x2014 the session settled back to idle.");
            mkStateBox(_setNotifyDone, L"Done", L"Notify on Running \x2192 Done \x2014 the session ended and claude exited.");
            mkStateBox(_setNotifyError, L"Error", L"Notify on Running \x2192 Error \x2014 the turn died on an API failure (rate limit, overloaded, prompt too long, \x2026).");
        }
        panel.Children().Append(SettingsSeparator(L"DELIVERY"));
        _setNotifySuppressFocused = ToggleSwitch{};
        _setNotifySuppressFocused.Header(winrt::box_value(L"Skip when the tab is focused"));
        AgentSetTip(_setNotifySuppressFocused, L"Don't notify when the session's tab is the one you're looking at (the focused tab of the active window) \x2014 you already saw it finish. Off notifies regardless. Default on.");
        panel.Children().Append(_setNotifySuppressFocused);
        _setNotifySound = ToggleSwitch{};
        _setNotifySound.Header(winrt::box_value(L"Play the notification sound"));
        AgentSetTip(_setNotifySound, L"Play the Windows notification sound with the toast. Off shows the toast silently. Default on.");
        panel.Children().Append(_setNotifySound);

        // === COMMANDS tab (COMMANDS.md §6a) ===
        // The /handover slash-command family: rename + enable/disable each command. Everything
        // here applies at the NEXT START only — the definition files under <claude-config>\commands
        // and the CommandWatch bindings are set up once at engine init (deliberately never re-bound
        // mid-run), so the status lines under the boxes read the LIVE state (the engine's
        // materialized-name markers, read fresh from disk at cog open) against the CONFIGURED one
        // and show the "after restart" staging explicitly (the PROFILE row's idiom).
        panel = commandsPanel;
        panel.Children().Append(SettingsSeparator(L"SLASH COMMANDS (the /handover family)", true)); // leading section
        {
            auto intro = Text(L"Agentmaster ships three slash commands into your global Claude commands folder (~\\.claude\\commands, or CLAUDE_CONFIG_DIR). Typed into a managed Claude session, they write a HANDOVER-*.md briefing \x2014 by default into the session's temp scratchpad, since its CONTENT is what gets injected \x2014 that Agentmaster turns into fresh successor session(s). Rename or disable them here \x2014 names, enables, and the file-match pattern apply AFTER RESTART; the successor shaping below (model \x00B7 title rewrite \x00B7 file location \x00B7 delete-after) applies to the NEXT handover right after Save. A rename/disable deletes the old definition file only when it is byte-identical to a version Agentmaster shipped; a file you edited yourself is never touched (and a disabled command's edited file keeps working as YOUR command, just unwatched).", 11, false, 0.6);
            intro.TextWrapping(TextWrapping::Wrap);
            panel.Children().Append(intro);
        }
        panel.Children().Append(SettingsSeparator(L"HAND OVER \x2192 NEW TAB(S)"));
        _setCmdHandoverEnabled = ToggleSwitch{};
        _setCmdHandoverEnabled.Header(winrt::box_value(L"Enable (successors open in new tabs)"));
        AgentSetTip(_setCmdHandoverEnabled, L"The classic handover: the command's HANDOVER-*.md file(s) each open a fresh successor session in a NEW TAB beside this one, the document injected as its first user message. Off = the command is not offered and Agentmaster ignores it. Applies after restart.");
        _setCmdHandoverEnabled.Toggled([this](const IInspectable&, const RoutedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdHandoverEnabled);
        _setCmdHandoverName = TextBox{};
        _setCmdHandoverName.Header(winrt::box_value(L"Command name"));
        _setCmdHandoverName.PlaceholderText(L"handover \x2014 typed as /handover");
        AgentSetTip(_setCmdHandoverName, L"The word you type after '/' to run the new-tab handover (also the definition's file name, <name>.md). Lowercase letters, digits, '-' and '_' only \x2014 anything else is dropped. Blank falls back to \x201Chandover\x201D. Applies after restart; the old name's pristine definition file is deleted, a hand-edited one is left alone.");
        _setCmdHandoverName.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdHandoverName);
        _setCmdHandoverStatus = Text(L"", 11, false, 0.7);
        _setCmdHandoverStatus.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setCmdHandoverStatus);
        // §6b: the command's SUCCESSOR MODEL — "Default" (the settings model, index 0) + the
        // launch-model picker's list. Items are (re)seeded at every _ShowSettings (the launchModels
        // box may have changed since the overlay was built); the parallel _cmdModelIds* vector maps
        // SelectedIndex -> the stored model id. Applies to the NEXT handover right after Save.
        _setCmdModelHandover = ComboBox{};
        _setCmdModelHandover.Header(winrt::box_value(L"Successor model"));
        AgentSetTip(_setCmdModelHandover, L"The model this command's successor session(s) launch with. Default keeps the Sessions tab's Model box (the shipped behavior); the other entries come from the Launch models list and add --model <id> to just the successor's launch. You can also pick per MESSAGE \x2014 start the command with a model word, \x201C/handover [fable] do a b c\x201D or \x201C/handover fable 5: \x2026\x201D (partial, case-blind, matches either the display name or the model id) \x2014 which overrides this for that one handover. Applies to the next handover \x2014 no restart needed.");
        _WireSuccessorModelCombo(_setCmdModelHandover, &_cmdModelIdsHandover);
        panel.Children().Append(_setCmdModelHandover);
        panel.Children().Append(SettingsSeparator(L"HAND OVER IN PLACE (REPLACE THIS TAB)"));
        _setCmdHandoverHereEnabled = ToggleSwitch{};
        _setCmdHandoverHereEnabled.Header(winrt::box_value(L"Enable (first successor replaces the tab)"));
        AgentSetTip(_setCmdHandoverHereEnabled, L"The in-place twin: the FIRST HANDOVER-*.md's successor RESTARTS this very tab into a fresh session (the origin conversation is archived, resumable from Sessions); additional files open beside it. Off = the command is not offered and Agentmaster ignores it. Applies after restart.");
        _setCmdHandoverHereEnabled.Toggled([this](const IInspectable&, const RoutedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdHandoverHereEnabled);
        _setCmdHandoverHereName = TextBox{};
        _setCmdHandoverHereName.Header(winrt::box_value(L"Command name"));
        _setCmdHandoverHereName.PlaceholderText(L"handover-here \x2014 typed as /handover-here");
        AgentSetTip(_setCmdHandoverHereName, L"The word you type after '/' to run the in-place handover (also the definition's file name, <name>.md). Lowercase letters, digits, '-' and '_' only. Blank falls back to \x201Chandover-here\x201D; a name equal to the other command's falls back too (the two must differ). Applies after restart.");
        _setCmdHandoverHereName.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdHandoverHereName);
        _setCmdHandoverHereStatus = Text(L"", 11, false, 0.7);
        _setCmdHandoverHereStatus.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setCmdHandoverHereStatus);
        _setCmdModelHandoverHere = ComboBox{};
        _setCmdModelHandoverHere.Header(winrt::box_value(L"Successor model"));
        AgentSetTip(_setCmdModelHandoverHere, L"The model the in-place successor (and any additional-file tabs) launches with. Default keeps the Sessions tab's Model box; the other entries come from the Launch models list and add --model <id> to just the successor's launch. A model word at the START of the typed command (\x201C/handover-here [fable] \x2026\x201D \x2014 partial, case-blind, display name or id) overrides this for that one handover. Applies to the next handover \x2014 no restart needed.");
        _WireSuccessorModelCombo(_setCmdModelHandoverHere, &_cmdModelIdsHandoverHere);
        panel.Children().Append(_setCmdModelHandoverHere);
        // §5b — the STANDBY member: successors open like /handover, but the briefing is TYPED
        // into each session's input box WITHOUT being submitted (one Enter away).
        panel.Children().Append(SettingsSeparator(L"HAND OVER \x2192 STANDBY (fill, don't send)"));
        _setCmdStandbyEnabled = ToggleSwitch{};
        _setCmdStandbyEnabled.Header(winrt::box_value(L"Enable (briefing pre-typed, never sent)"));
        AgentSetTip(_setCmdStandbyEnabled, L"The standby handover: each HANDOVER-*.md still opens a fresh successor session in a NEW TAB, but its document is TYPED into that session's input box WITHOUT being submitted \x2014 you review the pre-filled message and press Enter yourself. Nothing runs until you do. Off = the command is not offered and Agentmaster ignores it. Applies after restart.");
        _setCmdStandbyEnabled.Toggled([this](const IInspectable&, const RoutedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdStandbyEnabled);
        _setCmdStandbyName = TextBox{};
        _setCmdStandbyName.Header(winrt::box_value(L"Command name"));
        _setCmdStandbyName.PlaceholderText(L"handover-standby \x2014 typed as /handover-standby");
        AgentSetTip(_setCmdStandbyName, L"The word you type after '/' to run the standby handover (also the definition's file name, <name>.md). Lowercase letters, digits, '-' and '_' only. Blank falls back to \x201Chandover-standby\x201D; a name equal to another command's falls back too (the three must differ). Applies after restart.");
        _setCmdStandbyName.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdStandbyName);
        _setCmdStandbyStatus = Text(L"", 11, false, 0.7);
        _setCmdStandbyStatus.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setCmdStandbyStatus);
        _setCmdModelStandby = ComboBox{};
        _setCmdModelStandby.Header(winrt::box_value(L"Successor model"));
        AgentSetTip(_setCmdModelStandby, L"The model this command's standby successor session(s) launch with. Default keeps the Sessions tab's Model box; the other entries come from the Launch models list and add --model <id> to just the successor's launch. A model word at the START of the typed command (\x201C/handover-standby [fable] \x2026\x201D \x2014 partial, case-blind, display name or id) overrides this for that one handover. Applies to the next handover \x2014 no restart needed.");
        _WireSuccessorModelCombo(_setCmdModelStandby, &_cmdModelIdsStandby);
        panel.Children().Append(_setCmdModelStandby);

        // §6b SUCCESSOR SHAPING — family-wide (all three commands), built on the ONE reusable regex
        // component (RegexUtil.h): the successor-TITLE find/replace pair, the HANDOVER file-match
        // pattern, and the delete-after-hand-off toggle. The status line below the boxes calls out
        // an invalid pattern live (an invalid regex never breaks a handover — the consumers fall
        // back to the shipped behavior; the line is the honesty channel).
        panel.Children().Append(SettingsSeparator(L"SUCCESSOR SHAPING (all commands)"));
        {
            // The title rewrite: [ find (regex) | replace ] side by side (a Grid, so the pair reads
            // as one rule: successorTitle = regex_replace(originTitle, find, replace)).
            auto titleRow = Grid{};
            {
                ColumnDefinition cFind;
                cFind.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                ColumnDefinition cGap;
                cGap.Width(GridLengthHelper::FromValueAndType(8, GridUnitType::Pixel));
                ColumnDefinition cRepl;
                cRepl.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                titleRow.ColumnDefinitions().Append(cFind);
                titleRow.ColumnDefinitions().Append(cGap);
                titleRow.ColumnDefinitions().Append(cRepl);
            }
            _setCmdTitleFind = TextBox{};
            _setCmdTitleFind.Header(winrt::box_value(L"Successor title: find (regex)"));
            _setCmdTitleFind.PlaceholderText(L"cleared \x2014 falls back to built-in \x201C(handover)\x201D naming");
            AgentSetTip(_setCmdTitleFind, L"Rewrite successor titles with a regex applied to the ORIGIN tab's title: every match of this pattern is replaced by the text on the right ($1 backrefs work; anchor with ^/$ to match the whole title).\n\nThe box ships with Agentmaster's ACTUAL default rule, so you can see and edit it rather than guess: it optionally eats an existing \x201C (handover)\x201D / \x201C (handover 2)\x201D ending and re-adds \x201C (handover)\x201D \x2014 which is why handing over repeatedly gives (handover 2), (handover 3) instead of stacking \x201C(handover) (handover)\x201D.\n\nClearing the box, an invalid pattern, one that doesn't match a title, or a result that would be blank all fall back to the built-in naming. Titles are still made unique afterwards. Applies to the next handover.");
            _setCmdTitleFind.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
            Grid::SetColumn(_setCmdTitleFind, 0);
            titleRow.Children().Append(_setCmdTitleFind);
            _setCmdTitleReplace = TextBox{};
            _setCmdTitleReplace.Header(winrt::box_value(L"replace with"));
            _setCmdTitleReplace.PlaceholderText(L"e.g. $1 (next)");
            AgentSetTip(_setCmdTitleReplace, L"The replacement for every match of the find pattern \x2014 $1, $2 \x2026 insert the pattern's capture groups; an empty replacement deletes the matched part. A rewrite that would leave the title blank falls back to the default naming (a title never goes empty).");
            _setCmdTitleReplace.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
            Grid::SetColumn(_setCmdTitleReplace, 2);
            titleRow.Children().Append(_setCmdTitleReplace);
            panel.Children().Append(titleRow);
        }
        // §6c WHERE the briefings are written — a free-typed FOLDER with a preset drop-down beside
        // it. FOLDER-ONLY by design: the file NAME stays the HANDOVER-<topic>.md contract the rest
        // of the pipeline keys on (the match regex below, the one-successor-per-file fan-out,
        // delete-after) — only the directory moves. The value is rendered into BOTH definition
        // files on Save, so it applies to the next handover with no restart.
        {
            auto locRow = Grid{};
            {
                ColumnDefinition cBox;
                cBox.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                ColumnDefinition cGap;
                cGap.Width(GridLengthHelper::FromValueAndType(6, GridUnitType::Pixel));
                ColumnDefinition cBtn;
                cBtn.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
                locRow.ColumnDefinitions().Append(cBox);
                locRow.ColumnDefinitions().Append(cGap);
                locRow.ColumnDefinitions().Append(cBtn);
            }
            _setCmdWritePath = TextBox{};
            _setCmdWritePath.Header(winrt::box_value(L"Handover file location (folder)"));
            _setCmdWritePath.PlaceholderText(L"scratchpad \x2014 the session's temp folder");
            AgentSetTip(_setCmdWritePath, L"The FOLDER a handover briefing is written to. The file is always named HANDOVER-<topic>.md \x2014 only the folder changes, so everything else (the file match below, one successor per file, delete-after) keeps working.\n\n\x2022 scratchpad (the default, also what a blank box means): the session's own temp scratchpad. A briefing is a transient hand-off whose CONTENT is injected into the successor anyway, so nothing lands in your repo.\n\x2022 ./ : the session's working directory (how it always used to work).\n\x2022 ./docs, ./handovers, \x2026 : a folder relative to the working directory \x2014 created if missing.\n\x2022 An absolute path (D:\\briefings) works too.\n\nSaved changes are written straight into the command definition files, so the next /handover uses the new folder \x2014 no restart. A definition file you edited by hand is never rewritten (see the line below).");
            _setCmdWritePath.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
            Grid::SetColumn(_setCmdWritePath, 0);
            locRow.Children().Append(_setCmdWritePath);
            _setCmdWritePathPresets = Button{};
            _setCmdWritePathPresets.Content(winrt::box_value(L"Presets \x25BE"));
            _setCmdWritePathPresets.VerticalAlignment(VerticalAlignment::Bottom);
            AgentSetTip(_setCmdWritePathPresets, L"Common locations. Picking \x201CScratchpad\x201D also turns OFF \x201C" L"Delete the handover file after a successful hand-off\x201D \x2014 a briefing in a temp folder is already out of your repo, so there is nothing to clean up, and keeping it means you can still go back and read it.");
            {
                auto flyout = MenuFlyout{};
                // { label, value, is-the-scratchpad } — the scratchpad entry also UNticks delete-after.
                const struct
                {
                    const wchar_t* label;
                    const wchar_t* value;
                    bool scratchpad;
                } presets[] = {
                    { L"Scratchpad (temp \x2014 recommended)", L"scratchpad", true },
                    { L"Working directory (./)", L"./", false },
                    { L"./docs", L"./docs", false },
                    { L"./docs/handovers", L"./docs/handovers", false },
                    { L"./handovers", L"./handovers", false },
                };
                for (const auto& p : presets)
                {
                    auto item = MenuFlyoutItem{};
                    item.Text(p.label);
                    const std::wstring value{ p.value };
                    const bool scratchpad = p.scratchpad;
                    item.Click([this, value, scratchpad](const IInspectable&, const RoutedEventArgs&) {
                        if (_setCmdWritePath)
                        {
                            _setCmdWritePath.Text(winrt::hstring{ value });
                        }
                        if (scratchpad && _setCmdDeleteAfter)
                        {
                            // The auto-pairing, INVERTED: a briefing in the temp scratchpad is already
                            // out of the user's repo, so there is no litter to clean up — and deleting
                            // it is the one irreversible thing we could do to a document they may want
                            // to re-read. Scratchpad therefore turns delete-after OFF.
                            _setCmdDeleteAfter.IsOn(false);
                        }
                        _UpdateCommandsTabStatus();
                    });
                    flyout.Items().Append(item);
                }
                _setCmdWritePathPresets.Flyout(flyout);
            }
            Grid::SetColumn(_setCmdWritePathPresets, 2);
            locRow.Children().Append(_setCmdWritePathPresets);
            panel.Children().Append(locRow);
        }
        _setCmdFileMatch = TextBox{};
        _setCmdFileMatch.Header(winrt::box_value(L"Handover file match (regex \x2014 applies after restart)"));
        _setCmdFileMatch.PlaceholderText(L"cleared \x2014 falls back to the built-in \x201Cname contains handover\x201D rule");
        AgentSetTip(_setCmdFileMatch, L"Which markdown files a handover COLLECTS, matched against the written file's NAME (case-insensitive regex search; anchor with ^/$ for a whole-name match, e.g. ^BRIEF-.*\\.md$).\n\nThe box ships with the ACTUAL default rule \x2014 HANDOVER\\- , i.e. the name contains \x201CHANDOVER-\x201D (the HANDOVER-<topic>.md contract the shipped definitions instruct) \x2014 so it is visible and editable. Clearing it or an invalid pattern falls back to the LOOSER built-in rule: the name merely contains \x201Chandover\x201D.\n\nCHANGING it away from the default also makes the match STRICT: the tolerance that would otherwise adopt the turn's first markdown when nothing matched is switched off, so an unrelated notes.md can't become the briefing. Applies after restart. The shipped command definitions still tell Claude to write HANDOVER-<topic>.md \x2014 pair a custom pattern with an edited definition that names files to match it.");
        _setCmdFileMatch.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdFileMatch);
        _setCmdDeleteAfter = ToggleSwitch{};
        _setCmdDeleteAfter.Header(winrt::box_value(L"Delete the handover file after a successful hand-off"));
        AgentSetTip(_setCmdDeleteAfter, L"OFF by default \x2014 a briefing is never deleted unless you ask for it here, because it is a document you may still want to read and deleting it cannot be undone.\n\nTurned ON: once a HANDOVER-*.md's successor session is created AND has actually started, and its briefing is secured (delivered on the launch commandline, or parked durably on the successor's queue for the paste injection), the file itself is deleted \x2014 no more HANDOVER-*.md litter in your repos. A file whose successor got only a POINTER to it is never deleted (the successor must read it), a failed spawn leaves its file, and a successor that dies before starting leaves its file. Applies to the next handover.");
        _setCmdDeleteAfter.Toggled([this](const IInspectable&, const RoutedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdDeleteAfter);
        _setCmdDeleteDeadline = TextBox{};
        _setCmdDeleteDeadline.Header(winrt::box_value(L"\x2026 waiting up to this many minutes for the successor to start"));
        _setCmdDeleteDeadline.PlaceholderText(L"1440 \x2014 24 hours");
        AgentSetTip(_setCmdDeleteDeadline, L"How long the delete WAITS for the successor session to actually start before giving up and KEEPING the file. It does not delay anything: a successor that comes straight up has its briefing deleted immediately.\n\nIt matters because a successor opened in a background tab starts LAZILY \x2014 its claude.exe only launches when you first visit the tab, which can be hours later. The default 1440 minutes (24 hours) means the briefing is cleaned up whenever you get around to opening it; a successor you never open keeps its file forever.\n\nA typed 0 = don't wait at all (delete only if the successor is already up); a BLANK box restores the 1440 default. Capped at 30 days.\n\nThe wait spans a single run: if Agentmaster is closed while a successor is still un-started, its briefing is KEPT (never swept on the next launch).");
        _setCmdDeleteDeadline.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _UpdateCommandsTabStatus(); });
        panel.Children().Append(_setCmdDeleteDeadline);
        _setCmdShapingStatus = Text(L"", 11, false, 0.7);
        _setCmdShapingStatus.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setCmdShapingStatus);

        // §6c THE DEFINITION FILES themselves — the honesty line + the escape hatch. Everything on
        // this tab is delivered by writing <claude-config>\commands\<name>.md, and Agentmaster
        // NEVER overwrites a definition it doesn't recognize (a hand edit sticks forever). That is
        // right, but it means an edited file silently ignores the settings above — so we SAY what
        // each file is, and offer one confirmed overwrite back to the shipped text.
        panel.Children().Append(SettingsSeparator(L"COMMAND DEFINITION FILES"));
        _setCmdDefState = Text(L"", 11, false, 0.7);
        _setCmdDefState.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setCmdDefState);
        _setCmdReinstallBtn = Button{};
        _setCmdReinstallBtn.Content(winrt::box_value(L"Reinstall definition files\x2026"));
        _setCmdReinstallBtn.Margin(Thickness{ 0, 6, 0, 0 });
        AgentSetTip(_setCmdReinstallBtn, L"Overwrite handover.md, handover-here.md and handover-standby.md in your Claude commands folder with Agentmaster's shipped text, rendered with the names and file location configured here \x2014 whatever is in them now. This is the only action that overwrites a definition you edited yourself; normally an edited file is left alone forever (which is why an edited one stops following these settings). Your edits are NOT recoverable afterwards.");
        _setCmdReinstallBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            _Confirm(L"Reinstall the handover command definitions?",
                     L"handover.md, handover-here.md and handover-standby.md in your Claude commands folder will be REPLACED with Agentmaster's shipped instructions, using the command names and handover file location configured here. Any edits you made to those files are lost.",
                     L"Reinstall",
                     [this]() {
                         try
                         {
                             // Freshest disk (the engine owns the materialized-name markers this
                             // acts on), with the location as currently TYPED — so a Reinstall
                             // right after changing the box writes the folder the user can see.
                             auto s = ::Agentmaster::LoadAppSettings();
                             if (_setCmdWritePath)
                             {
                                 s.commandHandoverWritePath = ::Agentmaster::NormalizeCommandWritePath(std::wstring{ _setCmdWritePath.Text() });
                             }
                             const auto [ho, hh, hs] = ::Agentmaster::ReinstallHandoverCommandFiles(s);
                             ::Agentmaster::LogNav(L"commands reinstall-definitions handover=" + std::wstring{ ho.empty() ? L"(none)" : L"ok" } +
                                                   L" here=" + std::wstring{ hh.empty() ? L"(none)" : L"ok" } +
                                                   L" standby=" + std::wstring{ hs.empty() ? L"(none)" : L"ok" });
                             _RefreshCommandDefinitionState();
                             _UpdateCommandsTabStatus();
                         }
                         catch (...)
                         {
                             ::Agentmaster::AgentLogCaughtException(L"_ReinstallHandoverDefinitions");
                         }
                     });
        });
        panel.Children().Append(_setCmdReinstallBtn);

        // === TABS & OVERLAY tab ===
        panel = tabsPanel;
        // TABS — close affordances on the terminal tab strip (GLOBAL across windows, applied live
        // on Save via TerminalPage::_updateAllTabCloseButtons + the cross-window broadcast).
        panel.Children().Append(SettingsSeparator(L"TAB STRIP", true)); // leading section (the tab itself is "Tabs & Overlay")
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
        // TABS: show the profile ICON on tabs (the leftmost glyph, before the status dot + title).
        // GLOBAL (AppSettings::showTabIcon); applied live on Save via TerminalPage::_UpdateAllTabIcons +
        // the cross-window broadcast. DEFAULT OFF — this app hides tab icons by default so the strip
        // reads on the status dot + title; off forces IconStyle::Hidden so the icon takes no space.
        _setShowTabIcon = ToggleSwitch{};
        _setShowTabIcon.Header(winrt::box_value(L"Show icons on tabs"));
        AgentSetTip(_setShowTabIcon, L"Show the profile icon at the left of each tab (before the status dot and title). When off (the default), the icon is hidden entirely and takes no space \x2014 the tab reads on its status dot + title. Default off.");
        panel.Children().Append(_setShowTabIcon);
        // Tab title naming (Agentmaster): HOW an untitled session's default tab title derives from
        // its working directory — the technique dropdown + a case dropdown + a whitespace->'_'
        // toggle, with a LIVE example preview beneath (made-up paths run through the REAL
        // DeriveSessionTitle with the CURRENT control state, re-rendered on every change). GLOBAL
        // (AppSettings::tabTitleNaming / tabTitleCase / tabTitleSpacesToUnderscores); applied at the
        // NEXT launch/adopt of an untitled session — existing and renamed titles never re-derive
        // (Rule #11).
        panel.Children().Append(SettingsSeparator(L"TAB TITLES"));
        _setTitleNaming = ComboBox{};
        _setTitleNaming.Header(winrt::box_value(L"Tab title naming"));
        _setTitleNaming.Items().Append(winrt::box_value(L"Last word in folder name")); // index 0 == TabTitleNaming::LastWord (default)
        _setTitleNaming.Items().Append(winrt::box_value(L"Folder name as is")); // index 1 == TabTitleNaming::FolderName
        _setTitleNaming.Items().Append(winrt::box_value(L"Two folder names")); // index 2 == TabTitleNaming::TwoFolders
        _setTitleNaming.Items().Append(winrt::box_value(L"Folder name capital letters")); // index 3 == TabTitleNaming::Capitals
        _setTitleNaming.Items().Append(winrt::box_value(L"Branch name")); // index 4 == TabTitleNaming::Branch
        _setTitleNaming.Items().Append(winrt::box_value(L"Branch name / folder name")); // index 5 == TabTitleNaming::BranchFolder
        _setTitleNaming.Items().Append(winrt::box_value(L"Branch name / two folder names")); // index 6 == TabTitleNaming::BranchTwoFolders
        AgentSetTip(_setTitleNaming, L"How a new session's tab is named from its working directory (generic bin/obj/Debug/\x2026 segments are skipped first).\n\x2022 Last word in folder name (default): \x201CPotato.Tomato.SlangGang\x201D \x2192 \x201CSlangGang\x201D; a name without separators stays whole (\x201CPotatoTomato\x201D).\n\x2022 Folder name as is: the folder name unchanged.\n\x2022 Two folder names: parent/folder \x2014 \x201C" L"C:\\repos\\Potato.Tomato.SlangGang\x201D \x2192 \x201Crepos/Potato.Tomato.SlangGang\x201D.\n\x2022 Folder name capital letters: the capitals only \x2014 \x201CPotaTo.Tomato.Slang\x201D \x2192 \x201CPTTS\x201D (an all-lowercase name falls back to its word initials).\n\x2022 Branch name: the working directory's current git branch (\x201C" L"feature/issue123\x201D; a detached HEAD reads as the short commit id) \x2014 a non-git folder falls back to the folder name.\n\x2022 Branch name / folder name: \x201C" L"feature/issue123/Agentmaster\x201D.\n\x2022 Branch name / two folder names: \x201C" L"feature/issue123/source/Agentmaster\x201D.\nAny '\\' in the title is normalized to '/'. Applies when a session is launched or adopted; existing and renamed titles are kept.");
        _setTitleNaming.SelectionChanged([this](const IInspectable&, const SelectionChangedEventArgs&) { _UpdateTitleNamingPreview(); });
        panel.Children().Append(_setTitleNaming);
        _setTitleCase = ComboBox{};
        _setTitleCase.Header(winrt::box_value(L"Title case"));
        _setTitleCase.Items().Append(winrt::box_value(L"Default")); // index 0 == TabTitleCase::Default
        _setTitleCase.Items().Append(winrt::box_value(L"Lowercase")); // index 1 == TabTitleCase::Lower
        _setTitleCase.Items().Append(winrt::box_value(L"Uppercase")); // index 2 == TabTitleCase::Upper
        AgentSetTip(_setTitleCase, L"The case applied to the derived tab title \x2014 Default keeps it as derived; Lowercase / Uppercase transform the whole title.");
        _setTitleCase.SelectionChanged([this](const IInspectable&, const SelectionChangedEventArgs&) { _UpdateTitleNamingPreview(); });
        panel.Children().Append(_setTitleCase);
        _setTitleUnderscores = ToggleSwitch{};
        _setTitleUnderscores.Header(winrt::box_value(L"Spaces to underscores"));
        AgentSetTip(_setTitleUnderscores, L"Convert any whitespace in the derived tab title to '_' (\x201CPotato Tomato Slang\x201D \x2192 \x201CPotato_Tomato_Slang\x201D). Default off.");
        _setTitleUnderscores.Toggled([this](const IInspectable&, const RoutedEventArgs&) { _UpdateTitleNamingPreview(); });
        panel.Children().Append(_setTitleUnderscores);
        _titleNamingPreview = TextBlock{};
        _titleNamingPreview.FontFamily(FontFamily{ L"Consolas" });
        _titleNamingPreview.FontSize(11);
        _titleNamingPreview.Opacity(0.65);
        _titleNamingPreview.TextWrapping(TextWrapping::NoWrap);
        _titleNamingPreview.Margin(Thickness{ 0, -2, 0, 4 });
        AgentSetTitledTip(_titleNamingPreview, L"Preview", L"What these made-up working directories would name their tabs under the settings above \x2014 re-derived through the real naming code as you change them, so what you see here is what a new session gets.");
        panel.Children().Append(_titleNamingPreview);
        _UpdateTitleNamingPreview(); // initial render (nothing selected yet -> the defaults)
        // FAVORITES.md §5a: which glyph marks a FAVORITE (starred) session on its live tab — Crown
        // (default, a gold crown at the status dot's NW) or Star (the status dot foregrounded on a white,
        // golden-tipped star). GLOBAL across windows; applied live on Save + cross-window broadcast.
        panel.Children().Append(SettingsSeparator(L"FAVORITES & TAGS"));
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
        // Rule-#12 behavior, default), individual per tab, shared per the INFERRED working dir
        // (detected from the files the session reads/edits/creates), or "Remove colors" (STRIP-WIDE:
        // no tab is colored — shell + Manager tabs' colors are SUSPENDED, not voided — and "Change
        // tab color" is disabled on every tab; saved colors are kept, just not loaded). GLOBAL
        // (AppSettings::tabColorMode);
        // applied live on Save + cross-window broadcast (TerminalPage::_ReapplyManagedTabColors).
        panel.Children().Append(SettingsSeparator(L"TAB COLORS"));
        _setTabColorMode = ComboBox{};
        _setTabColorMode.Header(winrt::box_value(L"Tab coloring"));
        _setTabColorMode.Items().Append(winrt::box_value(L"Shared per working directory")); // index 0 == TabColorMode::WorkingDirectory (default)
        _setTabColorMode.Items().Append(winrt::box_value(L"Individual per tab")); // index 1 == TabColorMode::Individual
        _setTabColorMode.Items().Append(winrt::box_value(L"Inferred working directory")); // index 2 == TabColorMode::InferredWorkingDirectory
        _setTabColorMode.Items().Append(winrt::box_value(L"Remove colors")); // index 3 == TabColorMode::NoColor
        AgentSetTip(_setTabColorMode, L"How session tabs are colored.\n\x2022 Shared per working directory (default): every tab launched in a folder wears that folder's permanent color; picking a color recolors the whole folder.\n\x2022 Individual per tab: each session gets its own color (kept across close/reopen); picking a color changes only that tab.\n\x2022 Inferred working directory: like shared-per-directory, but keyed by where the session ACTUALLY works rather than where it was started. Every file it touches votes; the votes group into work areas \x2014 a git repository, or a top-level folder outside one \x2014 and the area with the most votes wins (a repo answers as the repo root, never deeper). Re-checked as the session works, so one that settles into another tree takes that tree's color. Scratch files under your temp folder never vote.\n\x2022 Remove colors: NO tab is colored \x2014 session tabs, shell tabs (e.g. a PowerShell tab that kept a color), and the Manager tab alike \x2014 and \x201C" L"Change tab color\x201D is disabled on every tab. Nothing is deleted: folder/session colors and a shell tab's own color are kept (just not shown) \x2014 switch back to any other mode and they return exactly as they were.\n\nOne exception, in every mode: a session launched straight in your home folder infers anyway \x2014 a bare \x201C" L"claude\x201D with no cd almost never works there, so its color follows the files it touches instead.");
        panel.Children().Append(_setTabColorMode);
        // Tab color modes — "Use .git folder to infer" (AppSettings::inferGitRoot, default ON): the
        // inferred working dir SNAPS to the enclosing git repository root (the folder holding .git —
        // a worktree's .git FILE counts, nearest wins), so an in-repo session infers the repo — which
        // normally equals its launch cwd, keeping the inferred mode's colors in step with
        // shared-per-directory. Off: the plain deepest-majority folder of the touched files. Only
        // meaningful in the Inferred mode, so it's enabled only while that mode is selected.
        _setInferGitRoot = ToggleSwitch{};
        _setInferGitRoot.Header(winrt::box_value(L"Use .git folder to infer"));
        AgentSetTip(_setInferGitRoot, L"When inferring the working directory, let a git repository be the answer: files are grouped by the repo enclosing them (the nearest folder holding .git \x2014 a worktree counts as its own), and if that repo is where most of a session's work lands, its ROOT is the inferred directory \x2014 never a folder inside it, since the repo is one working area. That is what keeps an in-repo session the same color it has under \x201Cshared per working directory\x201D, so the two modes only ever disagree when a session genuinely works outside its own repo.\n\nWhen off, files group by their top-level folder instead, and the pick walks down only as long as one child holds more than half of its parent's files \x2014 so a 60/40 split between two subfolders stays on the folder above them.\n\nDefault on; only applies while \x201CTab coloring\x201D is \x201CInferred working directory\x201D.");
        _setInferGitRoot.IsEnabled(false); // enabled by the combo handler / the seed when Inferred is selected
        _setTabColorMode.SelectionChanged([this](const IInspectable&, const SelectionChangedEventArgs&) {
            if (_setInferGitRoot && _setTabColorMode)
            {
                _setInferGitRoot.IsEnabled(_setTabColorMode.SelectedIndex() == 2); // 2 == InferredWorkingDirectory
            }
        });
        panel.Children().Append(_setInferGitRoot);

        // TABS: the "status flashing color" — color (and OPACITY) of the unread FLASH RING that pulses
        // around a managed session's tab status dot when it leaves Running for a needs-you state on an
        // unvisited tab (and the manual "Mark Unread" ring). To keep the cog SHORT, this is a COMPACT
        // swatch button (a preview of the current color) that opens the muxc::ColorPicker in a FLYOUT —
        // the full picker is huge inline, so it only appears on demand (the Windows Terminal tab-color
        // idiom). The picker's ALPHA slider IS the OPACITY control (one control sets hue + opacity).
        // GLOBAL (AppSettings::flashRingColor, "#AARRGGBB"); applied live on Save + cross-window
        // broadcast (TerminalPage::_RefreshFlashRingBrush). Default red at 80% opacity (#CCFF0000).
        panel.Children().Append(SettingsSeparator(L"STATUS & OVERLAY"));
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
            // The swatch button's content is the color preview itself — nothing to derive a heading from.
            AgentSetTitledTip(swatchBtn, L"Status flashing color", L"The color of the ring that pulses around a tab's status dot when a session you weren't looking at stops running and wants you \x2014 it clears the moment you visit the tab. The picker's alpha slider is its opacity, so one control sets both. Default: red at 80%.");
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
                                             const wchar_t* tipTitle,
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
                AgentSetTitledTip(swatchBtn, tipTitle, tipText); // the content is the color preview — nothing derives a heading
                row.Children().Append(swatchBtn);

                panel.Children().Append(row);
            };

            makeDotsColorRow(L"Pending dots (on dark tabs)",
                             L"Pending dots (on dark tabs)",
                             L"The three dots that pulse on a tab and its card when you have typed a message into a session but not sent it. This is the color used against a DARK tab background \x2014 the light half of the pair, picked automatically so the dots are never invisible. The picker's alpha slider is its opacity. Default: gold.",
                             L"Reset to default (gold)",
                             ColorHelper::FromArgb(0xFF, 0xE0, 0xA9, 0x2B),
                             _setPendingLightPicker, _pendingLightSwatch);
            makeDotsColorRow(L"Pending dots (on light tabs)",
                             L"Pending dots (on light tabs)",
                             L"The same unsent-draft dots, in the color used against a LIGHT tab background \x2014 the dark half of the pair. Which of the two a tab gets is decided by how light its own color is, so neither can vanish into it. The picker's alpha slider is its opacity. Default: deep amber.",
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
            AgentSetTitledTip(_overlayOpacityTrack, L"Overlay opacity", L"How visible the badge in a terminal tab's top-right corner is. Drag the two dots: the LEFT one sets how it looks at rest (the transparent end of the rail), the RIGHT one how bright it gets when you point at it (the solid end). They can't cross, so hover is never dimmer than rest.");

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
            AgentSetTitledTip(_overlayRestThumb, L"Rest opacity", L"How visible the overlay badge is while you are not pointing at it \x2014 drag left to fade it further into the terminal. It stops at the hover dot; it can never be brighter than hover.");
            AgentSetTitledTip(_overlayHoverThumb, L"Hover opacity", L"How bright the overlay badge becomes when you point at it \x2014 drag right for more solid. It stops at the rest dot; it can never be dimmer than rest.");
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
        panel.Children().Append(SettingsSeparator(L"PROFILE"));
        _setProfileDir = TextBlock{};
        _setProfileDir.TextWrapping(TextWrapping::Wrap);
        _setProfileDir.Opacity(0.85);
        _setProfileDir.FontSize(12);
        AgentSetTitledTip(_setProfileDir, L"Active profile folder", L"Where this install keeps everything it remembers \x2014 your sessions, these settings, the hooks, the window layouts, the logs. Release and Dev each get their own by default, which is how they coexist without touching each other's state. A change you have staged reads as current \x2192 new (after restart).");
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

        // DEVELOPER — the debug escape hatch (ProfileBootstrap.h IsDebugPackage). "Enable Debug Mode" is the
        // durable, in-UI twin of the --debug / AGENTMASTER_DEBUG launch flag: it unlocks the DEV-only Auto
        // Testing / Tests Autorunner subsystem (queue prompts + auto-send on turn-complete, the pane toggle,
        // the board queue badge, the cog's Tests Autorunner tab, the per-tab autorunner control) in THIS
        // Release install. The gates + the scheduler are wired ONCE at startup (Engine.cpp /
        // Profiles::ApplyPersistedDebugMode), so a change applies on the NEXT start — matching the flag's own
        // "relaunch to persist" behaviour. On a Dev build these tools are always on: _ShowSettings shows the
        // toggle ON + disabled with a note, and OnSave skips writing it (so the forced display can't persist).
        panel.Children().Append(SettingsSeparator(L"DEVELOPER"));
        _setDebugMode = ToggleSwitch{};
        _setDebugMode.Header(winrt::box_value(L"Enable Debug Mode"));
        AgentSetTip(_setDebugMode, L"Unlock the developer Auto Testing / Tests Autorunner tools \x2014 queue prompts, auto-send them on turn-complete, the per-tab autorunner control, and the cog's Tests Autorunner tab \x2014 in this build. The durable equivalent of launching with --debug (or setting the AGENTMASTER_DEBUG environment variable). Applies after you restart Agentmaster.");
        panel.Children().Append(_setDebugMode);
        _setDebugModeNote = Text(L"Applies after restart.", 11, false, 0.6);
        _setDebugModeNote.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setDebugModeNote);

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
                         // Guarded: quits ONLY when the uninstaller actually launched (LaunchUninstaller
                         // is no-throw and false on any failure) — never close the app with nothing
                         // uninstalling, and never let a resolution hiccup crash the confirm callback.
                         try
                         {
                             const std::wstring stateDir = ::Agentmaster::Profiles::ResolveProfileDir();
                             if (::Agentmaster::Updater::LaunchUninstaller(stateDir) && _quitForUpdateHandler)
                             {
                                 _quitForUpdateHandler();
                             }
                         }
                         catch (...)
                         {
                             try
                             {
                                 ::Agentmaster::LogNav(L"uninstall launch FAILED (exception \x2014 app stays open)");
                             }
                             catch (...)
                             {
                                 // logger-failed: nothing left to report through
                             }
                         }
                     });
        });
        panel.Children().Append(_setUninstallBtn);

        // Every panel is now POPULATED — register the tabs (button + panel pairs) and slot the strip +
        // the single content scroller into `outer` between the title and the footer
        // (title -> tabs -> divider -> content -> Save/Cancel). _SwitchSettingsTab(0) below swaps the
        // Sessions panel into the scroller.
        addSettingsTab(L"Sessions", L"How new Claude sessions launch \x2014 permissions, model, environment variables, and the Launch box's directory history.", sessionsPanel);
        // Auto Testing is a DEV-OR-DEBUG feature: the Tests Autorunner defaults tab is added under the
        // AgentmasterDev package OR a `--debug` / AGENTMASTER_DEBUG release (IsDevOrDebugPackage; the
        // autorunner never runs otherwise — see Engine.cpp). The panel is still built above so the cog's
        // load/save code paths stay uniform; it's just not shown.
        if (::Agentmaster::Profiles::IsDevOrDebugPackage())
        {
            addSettingsTab(L"Tests Autorunner", L"Tests Autorunner defaults stamped onto every new session \x2014 the starting mode and its backstops.", autorunnerPanel);
        }
        addSettingsTab(L"Behavior", L"Interaction + session-state behavior \x2014 close confirms, the rename commit key, and the Waiting-for-you \x201Cunread\x201D timeout.", behaviorPanel);
        // COMMANDS is the first tab to offer a per-tab RESET: put every control back to the
        // SHIPPED DEFAULTS, read from a default-constructed AppSettings so there is ONE source of
        // truth (no hand-copied literals to drift). It touches the FORM only — Save commits it,
        // Cancel discards it — so no confirm dialog is needed. The engine-owned materialized-name
        // markers are deliberately untouched (they are disk reality, not a preference).
        addSettingsTab(L"Commands", L"The /handover slash-command family \x2014 rename or disable each command, pick the successor model, and shape the successor (title rewrite, where the briefing file is written, file match, delete-after). \x201C" L"Reset\x201D restores this tab's shipped defaults.", commandsPanel, [this]() {
            const ::Agentmaster::AppSettings d{}; // the shipped defaults, verbatim
            if (_setCmdHandoverEnabled)
            {
                _setCmdHandoverEnabled.IsOn(d.commandHandoverEnabled);
            }
            if (_setCmdHandoverName)
            {
                _setCmdHandoverName.Text(winrt::hstring{ d.commandHandoverName });
            }
            if (_setCmdHandoverHereEnabled)
            {
                _setCmdHandoverHereEnabled.IsOn(d.commandHandoverHereEnabled);
            }
            if (_setCmdHandoverHereName)
            {
                _setCmdHandoverHereName.Text(winrt::hstring{ d.commandHandoverHereName });
            }
            if (_setCmdStandbyEnabled)
            {
                _setCmdStandbyEnabled.IsOn(d.commandHandoverStandbyEnabled);
            }
            if (_setCmdStandbyName)
            {
                _setCmdStandbyName.Text(winrt::hstring{ d.commandHandoverStandbyName });
            }
            // Successor models -> "Default" (index 0 of every combo, by construction).
            if (_setCmdModelHandover && !_cmdModelIdsHandover.empty())
            {
                _setCmdModelHandover.SelectedIndex(0);
            }
            if (_setCmdModelHandoverHere && !_cmdModelIdsHandoverHere.empty())
            {
                _setCmdModelHandoverHere.SelectedIndex(0);
            }
            if (_setCmdModelStandby && !_cmdModelIdsStandby.empty())
            {
                _setCmdModelStandby.SelectedIndex(0);
            }
            if (_setCmdTitleFind)
            {
                _setCmdTitleFind.Text(winrt::hstring{ d.commandHandoverTitleFindRegex });
            }
            if (_setCmdTitleReplace)
            {
                _setCmdTitleReplace.Text(winrt::hstring{ d.commandHandoverTitleReplace });
            }
            if (_setCmdFileMatch)
            {
                _setCmdFileMatch.Text(winrt::hstring{ d.commandHandoverFileMatchRegex });
            }
            if (_setCmdDeleteAfter)
            {
                _setCmdDeleteAfter.IsOn(d.commandHandoverDeleteFileAfterLaunch);
            }
            if (_setCmdDeleteDeadline)
            {
                _setCmdDeleteDeadline.Text(winrt::hstring{ std::to_wstring(d.commandHandoverDeleteDeadlineMinutes) });
            }
            if (_setCmdWritePath)
            {
                _setCmdWritePath.Text(winrt::hstring{ d.commandHandoverWritePath }); // §6c — back to the scratchpad
            }
            _UpdateCommandsTabStatus(); // explicit: never depend on a programmatic set raising the change events
            ::Agentmaster::LogNav(L"settings-reset tab=Commands (form only \x2014 Save commits, Cancel discards)");
        });
        addSettingsTab(L"Notifications", L"Windows notifications when a session's status changes from Running to another state \x2014 which states notify, the focused-tab skip, and the sound.", notificationsPanel);
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
        // Per-tab RESET — LEFT of Cancel, and shown ONLY while the active tab supplies a handler
        // (_settingsTabResets; today just Commands). It restores that tab's controls to the
        // shipped defaults WITHOUT touching disk: Save commits the reset, Cancel discards it —
        // which is also why it needs no confirm (and a ContentDialog over this in-content modal
        // is the XAML-Islands keypress trap anyway). _SwitchSettingsTab re-evaluates visibility.
        _settingsResetBtn = Button{};
        _settingsResetBtn.Content(winrt::box_value(L"Reset"));
        _settingsResetBtn.Visibility(Visibility::Collapsed); // _SwitchSettingsTab(0) decides on open
        AgentSetTip(_settingsResetBtn, L"Put THIS tab's settings back to Agentmaster's defaults. Nothing is written yet \x2014 press Save to keep the reset, or Cancel to discard it. Other tabs are unaffected.");
        _settingsResetBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            // Guarded: this runs straight off a XAML event, and a reset handler touches many
            // controls — an escape here would fail-fast the app over a cosmetic action.
            try
            {
                if (_settingsActiveTab >= 0 && _settingsActiveTab < static_cast<int>(_settingsTabResets.size()))
                {
                    if (const auto& reset = _settingsTabResets[static_cast<size_t>(_settingsActiveTab)])
                    {
                        reset();
                    }
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"settings tab reset");
            }
        });
        auto cancel = Button{};
        cancel.Content(winrt::box_value(L"Cancel"));
        AgentSetTip(cancel, L"Close without saving \x2014 discard any changes made here.");
        cancel.Click([this](const IInspectable&, const RoutedEventArgs&) { _HideSettings(); });
        auto save = Button{};
        save.Content(winrt::box_value(L"Save"));
        AgentSetTip(save, L"Save these settings and apply them \x2014 they persist to disk and govern future sessions (and live-apply where possible, e.g. the Claude binary and tab options).");
        save.Click([this](const IInspectable&, const RoutedEventArgs&) { _SaveSettings(); });
        buttons.Children().Append(_settingsResetBtn); // leftmost — Reset · Cancel · Save
        buttons.Children().Append(cancel);
        buttons.Children().Append(save);
        outer.Children().Append(buttons);

        card.Child(outer);

        _settingsOverlay.Children().Append(card);

        // Agentmaster (LocalTooltip, AgentLocalTooltip.h): the cog's tooltips are LOCAL — hovering
        // any control renders its description in a fixed panel OUTSIDE the card (top-left side,
        // sharing the card's top edge, growing only to the left) instead of a floating ToolTip
        // popping over the very control you're about to click. AttachScope(card) routes every
        // AgentSetTip text inside the card there (the 60+ per-control tips above are untouched)
        // and suppresses their floating twin; the anchor keeps the panel's right edge glued to
        // the card's left edge across window resizes and per-tab card-height changes. A window
        // too narrow to host the panel falls back to the classic floating tips automatically.
        _settingsLocalTip.Initialize();
        _settingsLocalTip.AttachScope(card);
        _settingsLocalTip.AnchorTopLeftOutside(_settingsOverlay, card, 12.0);

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
        // The footer's per-tab "Reset" follows the ACTIVE tab: shown only where a handler was
        // registered (today Commands; every other tab passes nullptr and simply doesn't offer it).
        if (_settingsResetBtn)
        {
            const bool hasReset = index < static_cast<int>(_settingsTabResets.size()) &&
                                  static_cast<bool>(_settingsTabResets[static_cast<size_t>(index)]);
            _settingsResetBtn.Visibility(hasReset ? Visibility::Visible : Visibility::Collapsed);
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
        if (_setTrustWorkspace)
        {
            _setTrustWorkspace.IsOn(_appSettings.trustWorkspaceOnLaunch);
        }
        if (_setModel)
        {
            _setModel.Text(winrt::hstring{ _appSettings.model });
        }
        if (_setLaunchModels)
        {
            _setLaunchModels.Text(winrt::hstring{ _appSettings.launchModels });
            _RefreshLaunchModelsLex(); // explicit: a re-seed of IDENTICAL text fires no TextChanged, and the border/status must reflect THIS open's value
        }
        if (_setModelFamilies)
        {
            _setModelFamilies.Text(winrt::hstring{ _appSettings.modelFamilies });
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
        if (_setClaudeInstallBtn)
        {
            // Offer the one-click fix only when a native claude.exe is missing; label it for the situation.
            switch (::Agentmaster::ClaudeInstallKind())
            {
            case ::Agentmaster::ClaudeInstallState::LegacyNpm:
                _setClaudeInstallBtn.Content(winrt::box_value(L"Run claude install (migrate to native)"));
                _setClaudeInstallBtn.Visibility(Visibility::Visible);
                break;
            case ::Agentmaster::ClaudeInstallState::None:
                _setClaudeInstallBtn.Content(winrt::box_value(L"Install native (PowerShell)"));
                _setClaudeInstallBtn.Visibility(Visibility::Visible);
                break;
            default:
                _setClaudeInstallBtn.Visibility(Visibility::Collapsed);
                break;
            }
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
        if (_setPreserveDraft)
        {
            _setPreserveDraft.IsOn(_appSettings.preserveDraftOnSend);
        }
        if (_setDraftSwapCtrlS)
        {
            _setDraftSwapCtrlS.IsOn(_appSettings.draftSwapUseCtrlS);
        }
        if (_setRestoreDraft)
        {
            _setRestoreDraft.IsOn(_appSettings.restoreDraftOnResume);
        }
        if (_setConfirmKill)
        {
            _setConfirmKill.IsOn(_appSettings.confirmBeforeKill);
        }
        // NOTIFICATIONS tab: seed the master + the five target-state checkboxes + the two behavior
        // toggles, then sync the dependents' enabled state explicitly (don't rely on the programmatic
        // IsOn firing Toggled — the _setInferGitRoot seeding rule).
        if (_setNotifyEnabled)
        {
            _setNotifyEnabled.IsOn(_appSettings.notificationsEnabled);
            const bool on = _appSettings.notificationsEnabled;
            for (const auto& dep : { _setNotifyWaiting, _setNotifyNeedsApproval, _setNotifyIdle, _setNotifyDone, _setNotifyError })
            {
                if (dep)
                {
                    dep.IsEnabled(on);
                }
            }
            if (_setNotifySuppressFocused)
            {
                _setNotifySuppressFocused.IsEnabled(on);
            }
            if (_setNotifySound)
            {
                _setNotifySound.IsEnabled(on);
            }
        }
        if (_setNotifyWaiting)
        {
            _setNotifyWaiting.IsChecked(_appSettings.notifyOnWaiting);
        }
        if (_setNotifyNeedsApproval)
        {
            _setNotifyNeedsApproval.IsChecked(_appSettings.notifyOnNeedsApproval);
        }
        if (_setNotifyIdle)
        {
            _setNotifyIdle.IsChecked(_appSettings.notifyOnIdle);
        }
        if (_setNotifyDone)
        {
            _setNotifyDone.IsChecked(_appSettings.notifyOnDone);
        }
        if (_setNotifyError)
        {
            _setNotifyError.IsChecked(_appSettings.notifyOnError);
        }
        if (_setNotifySuppressFocused)
        {
            _setNotifySuppressFocused.IsOn(_appSettings.notifySuppressFocused);
        }
        if (_setNotifySound)
        {
            _setNotifySound.IsOn(_appSettings.notifySound);
        }
        // COMMANDS tab (COMMANDS.md §6a): seed the /handover-family controls, and capture the LIVE
        // names from DISK — the engine-owned materialized-name markers, RMW'd at engine init, are
        // what is actually bound/on-disk THIS RUN ("" == disabled) — so the status lines can stage
        // a pending change as "… after restart" (the PROFILE row's current -> new idiom).
        if (_setCmdHandoverEnabled)
        {
            _setCmdHandoverEnabled.IsOn(_appSettings.commandHandoverEnabled);
        }
        if (_setCmdHandoverName)
        {
            _setCmdHandoverName.Text(winrt::hstring{ _appSettings.commandHandoverName });
        }
        if (_setCmdHandoverHereEnabled)
        {
            _setCmdHandoverHereEnabled.IsOn(_appSettings.commandHandoverHereEnabled);
        }
        if (_setCmdHandoverHereName)
        {
            _setCmdHandoverHereName.Text(winrt::hstring{ _appSettings.commandHandoverHereName });
        }
        if (_setCmdStandbyEnabled)
        {
            _setCmdStandbyEnabled.IsOn(_appSettings.commandHandoverStandbyEnabled);
        }
        if (_setCmdStandbyName)
        {
            _setCmdStandbyName.Text(winrt::hstring{ _appSettings.commandHandoverStandbyName });
        }
        {
            const auto disk = ::Agentmaster::LoadAppSettings();
            _cmdLiveHandoverName = disk.commandHandoverMaterializedName;
            _cmdLiveHandoverHereName = disk.commandHandoverHereMaterializedName;
            _cmdLiveStandbyName = disk.commandHandoverStandbyMaterializedName;
        }
        // §6b successor shaping: (re)seed the two model combos from the CURRENT launchModels list
        // — rebuilt at every open, so an edit to the Launch models box shows here next open. The
        // parallel id vectors map SelectedIndex -> stored id ([0] == "" Default); a stored id no
        // longer in the list is appended as "(custom) <id>" so it round-trips instead of silently
        // resetting to Default.
        if (_setCmdModelHandover && _setCmdModelHandoverHere && _setCmdModelStandby)
        {
            const auto models = ::Agentmaster::ParseLaunchModels(_appSettings.launchModels);
            const auto seedModelCombo = [&models](const winrt::Windows::UI::Xaml::Controls::ComboBox& combo, std::vector<std::wstring>& ids, const std::wstring& stored) {
                combo.Items().Clear();
                ids.clear();
                combo.Items().Append(winrt::box_value(L"Default"));
                ids.push_back(L"");
                // "Specify..." — the same escape hatch the launch submenus grew: pick any model id,
                // including one no list mentions. Selecting it opens the shared prompt and lands the
                // typed id as a "(custom) <id>" row (_WireSuccessorModelCombo); the sentinel itself
                // is never a storable value (comboPick maps it to "" == Default).
                combo.Items().Append(winrt::box_value(L"Specify\x2026"));
                ids.push_back(std::wstring{ kSpecifyModelSentinel });
                int sel = 0;
                for (const auto& [display, id] : models)
                {
                    combo.Items().Append(winrt::box_value(winrt::hstring{ display }));
                    ids.push_back(id);
                    if (sel == 0 && !stored.empty() && id == stored)
                    {
                        sel = static_cast<int>(ids.size()) - 1;
                    }
                }
                if (!stored.empty() && sel == 0)
                {
                    combo.Items().Append(winrt::box_value(winrt::hstring{ L"(custom) " + stored }));
                    ids.push_back(stored);
                    sel = static_cast<int>(ids.size()) - 1;
                }
                combo.SelectedIndex(sel);
            };
            seedModelCombo(_setCmdModelHandover, _cmdModelIdsHandover, _appSettings.commandHandoverSuccessorModel);
            seedModelCombo(_setCmdModelHandoverHere, _cmdModelIdsHandoverHere, _appSettings.commandHandoverHereSuccessorModel);
            seedModelCombo(_setCmdModelStandby, _cmdModelIdsStandby, _appSettings.commandHandoverStandbySuccessorModel);
        }
        if (_setCmdTitleFind)
        {
            _setCmdTitleFind.Text(winrt::hstring{ _appSettings.commandHandoverTitleFindRegex });
        }
        if (_setCmdTitleReplace)
        {
            _setCmdTitleReplace.Text(winrt::hstring{ _appSettings.commandHandoverTitleReplace });
        }
        if (_setCmdFileMatch)
        {
            _setCmdFileMatch.Text(winrt::hstring{ _appSettings.commandHandoverFileMatchRegex });
        }
        if (_setCmdDeleteAfter)
        {
            _setCmdDeleteAfter.IsOn(_appSettings.commandHandoverDeleteFileAfterLaunch);
        }
        if (_setCmdDeleteDeadline)
        {
            _setCmdDeleteDeadline.Text(winrt::hstring{ std::to_wstring(_appSettings.commandHandoverDeleteDeadlineMinutes) });
        }
        if (_setCmdWritePath)
        {
            // §6c: shown VERBATIM (blank == the scratchpad default, which the placeholder says).
            _setCmdWritePath.Text(winrt::hstring{ _appSettings.commandHandoverWritePath });
        }
        // §6c: sample what the definition files on disk currently ARE — once per cog open (the
        // status pass below runs per keystroke and this reads + hashes three files).
        _RefreshCommandDefinitionState();
        // The seeds above re-fire TextChanged/Toggled, but be explicit so the status lines never
        // depend on a programmatic set actually raising them (the _UpdateTitleNamingPreview rule).
        _UpdateCommandsTabStatus();
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
        if (_setShowTabIcon)
        {
            _setShowTabIcon.IsOn(_appSettings.showTabIcon);
        }
        if (_setTitleNaming)
        {
            // Items: 0 == LastWord (default), 1 == FolderName, 2 == TwoFolders, 3 == Capitals,
            // 4 == Branch, 5 == BranchFolder, 6 == BranchTwoFolders.
            _setTitleNaming.SelectedIndex(_appSettings.tabTitleNaming == TabTitleNaming::FolderName ? 1 :
                                              _appSettings.tabTitleNaming == TabTitleNaming::TwoFolders       ? 2 :
                                              _appSettings.tabTitleNaming == TabTitleNaming::Capitals         ? 3 :
                                              _appSettings.tabTitleNaming == TabTitleNaming::Branch           ? 4 :
                                              _appSettings.tabTitleNaming == TabTitleNaming::BranchFolder     ? 5 :
                                              _appSettings.tabTitleNaming == TabTitleNaming::BranchTwoFolders ? 6 :
                                                                                                                0);
        }
        if (_setTitleCase)
        {
            // Items: 0 == Default, 1 == Lowercase, 2 == Uppercase.
            _setTitleCase.SelectedIndex(_appSettings.tabTitleCase == TabTitleCase::Lower ? 1 :
                                            _appSettings.tabTitleCase == TabTitleCase::Upper ? 2 :
                                                                                               0);
        }
        if (_setTitleUnderscores)
        {
            _setTitleUnderscores.IsOn(_appSettings.tabTitleSpacesToUnderscores);
        }
        // The seeded values above re-fire the controls' change handlers, but be explicit so the
        // preview never depends on a programmatic set actually raising them.
        _UpdateTitleNamingPreview();
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
            // Items: 0 == WorkingDirectory (default), 1 == Individual, 2 == InferredWorkingDirectory, 3 == NoColor.
            _setTabColorMode.SelectedIndex(_appSettings.tabColorMode == TabColorMode::Individual ? 1 :
                                               _appSettings.tabColorMode == TabColorMode::InferredWorkingDirectory ? 2 :
                                               _appSettings.tabColorMode == TabColorMode::NoColor                  ? 3 :
                                                                                                                     0);
        }
        if (_setInferGitRoot)
        {
            _setInferGitRoot.IsOn(_appSettings.inferGitRoot);
            // Meaningful only in the Inferred mode (the combo's SelectionChanged keeps this in step
            // with later user picks; set explicitly here so the seed never depends on the handler
            // having fired for the programmatic SelectedIndex above).
            _setInferGitRoot.IsEnabled(_appSettings.tabColorMode == TabColorMode::InferredWorkingDirectory);
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
            // Seed from DISK, not the in-memory copy: the toggle INSTANT-APPLIES via a freshest-disk
            // RMW (possibly flipped from another window since this window's copy was seeded), so disk
            // is the one truth. The latch keeps the programmatic IsOn from re-firing the Toggled RMW —
            // and MUST clear on every path: a latch stuck true would silently ignore every future
            // flip (the toggle would look alive but persist nothing — the exact bug class this
            // instant-apply replaced), so the IsOn is guarded and the clear is unconditional.
            try
            {
                const bool diskOn = ::Agentmaster::LoadAppSettings().allowUpdatePrerelease;
                _appSettings.allowUpdatePrerelease = diskOn;
                _seedingAllowPrerelease = true;
                _setAllowPrerelease.IsOn(diskOn);
            }
            catch (...)
            {
                try
                {
                    ::Agentmaster::LogNav(L"update-prerelease seed FAILED (exception \x2014 switch shows a stale state until reopen)");
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
            }
            _seedingAllowPrerelease = false;
        }
        if (_setAllowNightly)
        {
            // Same disk-truth seed as the pre-release switch above (the nightly switch instant-applies
            // via its own freshest-disk RMW, possibly flipped from another window). The latch keeps the
            // programmatic IsOn from re-firing the Toggled handler — which for THIS switch would raise
            // the warning confirm out of nowhere on every cog open; the clear is unconditional so a
            // stuck latch can never silently eat every future flip.
            try
            {
                const bool diskOn = ::Agentmaster::LoadAppSettings().allowUpdateNightly;
                _appSettings.allowUpdateNightly = diskOn;
                _seedingAllowNightly = true;
                _setAllowNightly.IsOn(diskOn);
            }
            catch (...)
            {
                try
                {
                    ::Agentmaster::LogNav(L"update-nightly seed FAILED (exception \x2014 switch shows a stale state until reopen)");
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
            }
            _seedingAllowNightly = false;
        }
        if (_setDebugMode)
        {
            // DEVELOPER (debug escape hatch): reflect the persisted setting, EXCEPT on a Dev build where the
            // developer tools are always on (IsDevPackage) and can't be turned off — show ON + disabled with a
            // note. Otherwise it is user-controllable, and the caption reflects whether debug is ALREADY active
            // this session (persisted-on + applied at startup, or --debug / AGENTMASTER_DEBUG) so a live vs
            // pending change reads unambiguously.
            if (::Agentmaster::Profiles::IsDevPackage())
            {
                _setDebugMode.IsOn(true);
                _setDebugMode.IsEnabled(false);
                if (_setDebugModeNote)
                {
                    _setDebugModeNote.Text(L"Dev build \x2014 developer tools are always on.");
                }
            }
            else
            {
                _setDebugMode.IsEnabled(true);
                _setDebugMode.IsOn(_appSettings.debugMode);
                if (_setDebugModeNote)
                {
                    _setDebugModeNote.Text(::Agentmaster::Profiles::IsDebugPackage()
                                               ? winrt::hstring{ L"Active now. A change applies after restart." }
                                               : winrt::hstring{ L"Applies after restart." });
                }
            }
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
        // Wedge-proofing: the in-flight flag + "Checking…" label are normally restored by the check's
        // completion — but if a worker/completion ever died mid-flight, they'd stay latched and every
        // future interactive check would silently no-op FOREVER. A fresh cog open is a fresh state.
        _interactiveUpdateInFlight = false;
        if (_setCheckUpdates)
        {
            _setCheckUpdates.IsEnabled(updaterChannel);
            _setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
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
        _settingsLocalTip.Hide(); // fresh open: the description panel stays hidden until the first hover
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
        // Drop the sticky description (and its hover throttle) with the card — the next open
        // starts hidden-until-hover and re-hovering the same control re-renders.
        _settingsLocalTip.Hide();
    }

    void AgentManagerContent::_SaveSettings()
    {
        if (_setSkipPermissions)
        {
            _appSettings.skipPermissions = _setSkipPermissions.IsOn();
        }
        if (_setTrustWorkspace)
        {
            _appSettings.trustWorkspaceOnLaunch = _setTrustWorkspace.IsOn();
        }
        if (_setModel)
        {
            std::wstring m{ _setModel.Text() };
            const auto a = m.find_first_not_of(L" \t");
            const auto b = m.find_last_not_of(L" \t");
            _appSettings.model = (a == std::wstring::npos) ? std::wstring{} : m.substr(a, b - a + 1);
        }
        if (_setLaunchModels)
        {
            // Stored verbatim (the env-editor idiom — ParseLaunchModels handles the TextBox's
            // \r-normalized newlines). A cleared box deliberately stores "" — the submenus then
            // offer just "Default" (Persistence keeps a present-but-empty key, never re-seeds).
            _appSettings.launchModels = std::wstring{ _setLaunchModels.Text() };
        }
        if (_setModelFamilies)
        {
            // Stored verbatim (ParseModelFamilies trims/lowercases at every consumer); a cleared
            // box stores "" — the display sites then fall back to the built-in family list.
            _appSettings.modelFamilies = std::wstring{ _setModelFamilies.Text() };
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
        if (_setPreserveDraft)
        {
            _appSettings.preserveDraftOnSend = _setPreserveDraft.IsOn();
        }
        if (_setDraftSwapCtrlS)
        {
            _appSettings.draftSwapUseCtrlS = _setDraftSwapCtrlS.IsOn();
        }
        if (_setRestoreDraft)
        {
            _appSettings.restoreDraftOnResume = _setRestoreDraft.IsOn();
        }
        if (_setConfirmKill)
        {
            _appSettings.confirmBeforeKill = _setConfirmKill.IsOn();
        }
        // NOTIFICATIONS tab. The checkboxes are read even while disabled (master off) — like
        // _setInferGitRoot, they still hold the user's stored preference, and dropping them here
        // would silently reset a mute to the default. IsChecked is an IReference<bool>; a null
        // (indeterminate — not reachable from the UI, two-state boxes) reads as unchecked.
        if (_setNotifyEnabled)
        {
            _appSettings.notificationsEnabled = _setNotifyEnabled.IsOn();
        }
        if (_setNotifyWaiting)
        {
            _appSettings.notifyOnWaiting = _setNotifyWaiting.IsChecked() && _setNotifyWaiting.IsChecked().Value();
        }
        if (_setNotifyNeedsApproval)
        {
            _appSettings.notifyOnNeedsApproval = _setNotifyNeedsApproval.IsChecked() && _setNotifyNeedsApproval.IsChecked().Value();
        }
        if (_setNotifyIdle)
        {
            _appSettings.notifyOnIdle = _setNotifyIdle.IsChecked() && _setNotifyIdle.IsChecked().Value();
        }
        if (_setNotifyDone)
        {
            _appSettings.notifyOnDone = _setNotifyDone.IsChecked() && _setNotifyDone.IsChecked().Value();
        }
        if (_setNotifyError)
        {
            _appSettings.notifyOnError = _setNotifyError.IsChecked() && _setNotifyError.IsChecked().Value();
        }
        if (_setNotifySuppressFocused)
        {
            _appSettings.notifySuppressFocused = _setNotifySuppressFocused.IsOn();
        }
        if (_setNotifySound)
        {
            _appSettings.notifySound = _setNotifySound.IsOn();
        }
        // COMMANDS tab (COMMANDS.md §6a): store the /handover-family names NORMALIZED +
        // COLLISION-HEALED (the same ResolveCommandNameTriple the Persistence load applies, so the
        // stored names are always distinct slugs — exactly what the status lines previewed).
        // Applies at the NEXT START (engine init binds + reconciles the definition files then);
        // the engine-owned materialized-name markers are preserved from disk below.
        if (_setCmdHandoverEnabled)
        {
            _appSettings.commandHandoverEnabled = _setCmdHandoverEnabled.IsOn();
        }
        if (_setCmdHandoverHereEnabled)
        {
            _appSettings.commandHandoverHereEnabled = _setCmdHandoverHereEnabled.IsOn();
        }
        if (_setCmdStandbyEnabled)
        {
            _appSettings.commandHandoverStandbyEnabled = _setCmdStandbyEnabled.IsOn();
        }
        if (_setCmdHandoverName && _setCmdHandoverHereName && _setCmdStandbyName)
        {
            std::wstring ho{ _setCmdHandoverName.Text() };
            std::wstring hh{ _setCmdHandoverHereName.Text() };
            std::wstring hs{ _setCmdStandbyName.Text() };
            ::Agentmaster::ResolveCommandNameTriple(ho, hh, hs);
            _appSettings.commandHandoverName = ho;
            _appSettings.commandHandoverHereName = hh;
            _appSettings.commandHandoverStandbyName = hs;
        }
        // §6b successor shaping. The model combos map SelectedIndex -> the parallel id vector
        // ([0]/none == "" Default); the regexes store VERBATIM (freeform patterns — RegexUtil
        // guards every use, the status line already warned on invalid), the toggle as-is.
        {
            const auto comboPick = [](const winrt::Windows::UI::Xaml::Controls::ComboBox& combo, const std::vector<std::wstring>& ids) -> std::wstring {
                const int idx = combo ? combo.SelectedIndex() : -1;
                if (idx <= 0 || idx >= static_cast<int>(ids.size()))
                {
                    return {};
                }
                const auto& picked = ids[static_cast<size_t>(idx)];
                // The "Specify..." row is a VERB, not a value — if it is somehow still selected (a
                // prompt dismissed at just the wrong moment), that means "no pick", i.e. Default.
                return picked == kSpecifyModelSentinel ? std::wstring{} : picked;
            };
            if (_setCmdModelHandover)
            {
                _appSettings.commandHandoverSuccessorModel = comboPick(_setCmdModelHandover, _cmdModelIdsHandover);
            }
            if (_setCmdModelHandoverHere)
            {
                _appSettings.commandHandoverHereSuccessorModel = comboPick(_setCmdModelHandoverHere, _cmdModelIdsHandoverHere);
            }
            if (_setCmdModelStandby)
            {
                _appSettings.commandHandoverStandbySuccessorModel = comboPick(_setCmdModelStandby, _cmdModelIdsStandby);
            }
        }
        if (_setCmdTitleFind)
        {
            _appSettings.commandHandoverTitleFindRegex = std::wstring{ _setCmdTitleFind.Text() };
        }
        if (_setCmdTitleReplace)
        {
            _appSettings.commandHandoverTitleReplace = std::wstring{ _setCmdTitleReplace.Text() };
        }
        if (_setCmdFileMatch)
        {
            _appSettings.commandHandoverFileMatchRegex = std::wstring{ _setCmdFileMatch.Text() };
        }
        if (_setCmdDeleteAfter)
        {
            _appSettings.commandHandoverDeleteFileAfterLaunch = _setCmdDeleteAfter.IsOn();
        }
        if (_setCmdDeleteDeadline)
        {
            // The SAME parse the status line shows (blank -> the 24 h default, a typed 0 -> "don't wait").
            _appSettings.commandHandoverDeleteDeadlineMinutes = ReadDeleteDeadlineMinutesBox(_setCmdDeleteDeadline);
        }
        if (_setCmdWritePath)
        {
            // §6c: normalized on the way in (the value is rendered into a one-line markdown
            // instruction), then pushed into the LIVE definition files further below — after the
            // settings sink has persisted them, so a crash between the two can't leave the files
            // describing a location settings.json doesn't hold.
            _appSettings.commandHandoverWritePath = ::Agentmaster::NormalizeCommandWritePath(std::wstring{ _setCmdWritePath.Text() });
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
        if (_setShowTabIcon)
        {
            _appSettings.showTabIcon = _setShowTabIcon.IsOn();
        }
        if (_setTitleNaming)
        {
            // Items: 0 == LastWord (default), 1 == FolderName, 2 == TwoFolders, 3 == Capitals,
            // 4 == Branch, 5 == BranchFolder, 6 == BranchTwoFolders.
            _appSettings.tabTitleNaming = _setTitleNaming.SelectedIndex() == 1 ? TabTitleNaming::FolderName :
                                          _setTitleNaming.SelectedIndex() == 2 ? TabTitleNaming::TwoFolders :
                                          _setTitleNaming.SelectedIndex() == 3 ? TabTitleNaming::Capitals :
                                          _setTitleNaming.SelectedIndex() == 4 ? TabTitleNaming::Branch :
                                          _setTitleNaming.SelectedIndex() == 5 ? TabTitleNaming::BranchFolder :
                                          _setTitleNaming.SelectedIndex() == 6 ? TabTitleNaming::BranchTwoFolders :
                                                                                 TabTitleNaming::LastWord;
        }
        if (_setTitleCase)
        {
            // Items: 0 == Default, 1 == Lowercase, 2 == Uppercase.
            _appSettings.tabTitleCase = _setTitleCase.SelectedIndex() == 1 ? TabTitleCase::Lower :
                                        _setTitleCase.SelectedIndex() == 2 ? TabTitleCase::Upper :
                                                                             TabTitleCase::Default;
        }
        if (_setTitleUnderscores)
        {
            _appSettings.tabTitleSpacesToUnderscores = _setTitleUnderscores.IsOn();
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
            // Items: 0 == WorkingDirectory (default), 1 == Individual, 2 == InferredWorkingDirectory, 3 == NoColor.
            _appSettings.tabColorMode = _setTabColorMode.SelectedIndex() == 1 ? TabColorMode::Individual :
                                        _setTabColorMode.SelectedIndex() == 2 ? TabColorMode::InferredWorkingDirectory :
                                        _setTabColorMode.SelectedIndex() == 3 ? TabColorMode::NoColor :
                                                                                TabColorMode::WorkingDirectory;
        }
        if (_setInferGitRoot)
        {
            // Read back even while disabled (mode != Inferred): the toggle still holds the user's
            // stored preference, and dropping it here would silently reset it to the default.
            _appSettings.inferGitRoot = _setInferGitRoot.IsOn();
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
        if (_setDebugMode && !::Agentmaster::Profiles::IsDevPackage())
        {
            // DEVELOPER: the form owns debugMode — but only when it is user-controllable. On a Dev build the
            // toggle is a disabled always-on indicator (see _ShowSettings), so never write its forced value
            // back (that would spuriously persist debugMode=true). The durable --debug twin; applies next start.
            _appSettings.debugMode = _setDebugMode.IsOn();
        }
        // Preserve fields owned by out-of-cog UI actions, freshest from disk (the page's settings handler
        // does the same for hiddenSessionIds/showSummaryPanel): the summary panel SIZE (width/height
        // fractions, TAB_OVERLAY.md) is written by the panel's resize grips, not this form, so a form Save
        // must not regress a resize done since the modal was seeded (incl. from another window). The
        // updater's state (Updater.h) is likewise written outside this form — skip/postpone by the
        // prompt's JSON RMW, allowUpdatePrerelease by the switch's own INSTANT-APPLY RMW — so preserve
        // all three (a stale form copy must never overwrite a flip made since the modal was seeded).
        {
            const auto disk = ::Agentmaster::LoadAppSettings();
            _appSettings.summaryPanelWidthFraction = disk.summaryPanelWidthFraction;
            _appSettings.summaryPanelHeightFraction = disk.summaryPanelHeightFraction;
            _appSettings.summaryPanelWrapNewlines = disk.summaryPanelWrapNewlines; // wrap-line toggle (panel times bar), out-of-cog UI action
            _appSettings.tabColorPickerCustomOpen = disk.tabColorPickerCustomOpen; // tab color picker Custom-panel expansion, written by the flyout's own RMW
            _appSettings.tabColorPickerAdvancedOpen = disk.tabColorPickerAdvancedOpen; // ditto for the picker's More/Less "advanced" inputs
            _appSettings.allowUpdatePrerelease = disk.allowUpdatePrerelease; // updater pre-release opt-in (instant-applied by the switch itself)
            _appSettings.allowUpdateNightly = disk.allowUpdateNightly; // updater NIGHTLY opt-in (instant-applied by its warning-gated switch)
            _appSettings.updateSkippedVersion = disk.updateSkippedVersion; // updater "Skip this version" (out-of-cog JSON RMW)
            _appSettings.updatePostponedUntilUnixMs = disk.updatePostponedUntilUnixMs; // updater "Postpone N days" (out-of-cog JSON RMW)
            _appSettings.envDefaultsVersion = disk.envDefaultsVersion; // shipped-default seed marker (engine-init, out-of-cog) — a Save must never reset it (would re-add a deleted default)
            _appSettings.claudeCleanupDaysSeeded = disk.claudeCleanupDaysSeeded; // shipped-default seed marker (engine-init, out-of-cog)
            _appSettings.commandHandoverMaterializedName = disk.commandHandoverMaterializedName; // engine-owned reality: which name's definition file the last init wrote (COMMANDS.md §6a) — a Save must never rewrite history
            _appSettings.commandHandoverHereMaterializedName = disk.commandHandoverHereMaterializedName; // ditto for the in-place twin
            _appSettings.commandHandoverStandbyMaterializedName = disk.commandHandoverStandbyMaterializedName; // ditto for the §5b standby member
        }
        if (_settingsSink)
        {
            // Nav audit: the user saved the global Settings (cog) — the one consequential cog action; it
            // governs every future session. Record the most behavior-impacting fields (the rest persist to
            // settings.json, the durable record).
            ::Agentmaster::LogNav(std::wstring{ L"settings-save skipPerms=" } + (_appSettings.skipPermissions ? L"1" : L"0") +
                                  L" trustWorkspace=" + (_appSettings.trustWorkspaceOnLaunch ? L"1" : L"0") +
                                  L" model=" + (_appSettings.model.empty() ? std::wstring{ L"(default)" } : _appSettings.model) +
                                  L" autorunner=" + (_appSettings.defaultAutorunnerMode == AutorunnerMode::Full ? L"Full" : _appSettings.defaultAutorunnerMode == AutorunnerMode::SemiAuto ? L"Semi" : L"Off") +
                                  L" claudeExe=" + (_appSettings.claudeExePath.empty() ? std::wstring{ L"(auto)" } : _appSettings.claudeExePath) +
                                  L" handover=" + (_appSettings.commandHandoverEnabled ? (L"/" + _appSettings.commandHandoverName) : std::wstring{ L"off" }) +
                                  L" here=" + (_appSettings.commandHandoverHereEnabled ? (L"/" + _appSettings.commandHandoverHereName) : std::wstring{ L"off" }) /* both apply at next start (COMMANDS.md §6a) */);
            _settingsSink(_appSettings); // page persists + applies to future spawns
        }
        // Native-exe-only policy: re-resolve the claude.exe now, so a changed/cleared override (or a
        // freshly-installed binary) takes effect this run — no restart needed (RefreshClaudeExe updates
        // the shared engine's cached path; ClaudeAvailable() flips accordingly).
        ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
        // COMMANDS.md §6c: push the handover WRITE LOCATION into the live definition files, so the
        // next /handover writes where the box says — no restart. Runs AFTER the sink persisted
        // settings.json (the files must never describe a location the settings don't hold) and only
        // rewrites definitions we RECOGNIZE: a hand-edited one stays frozen (the tab's state line
        // says so, and its Reinstall button is the explicit way out). Names/enables are NOT applied
        // here — a rename stays restart-applied so the file and its CommandWatch binding can never
        // disagree mid-run (§6a). Best-effort: a failed write is logged by the writer itself.
        ::Agentmaster::RefreshHandoverCommandWritePath(_appSettings);
        _HideSettings();
    }

    // === ENV_VARS.md: the "Environment variables" area (Global / Per-directory two-tab editor) ===========
    // The plumbing already merges global + per-dir at spawn (ResolveSessionEnv); this is the editor.

    void AgentManagerContent::_BuildEnvVarsArea(const StackPanel& panel)
    {
        // Section header (a styled separator, matching the other sections in the tab).
        panel.Children().Append(SettingsSeparator(L"ENVIRONMENT VARIABLES"));

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
        // The editor has no Header (the tab strip above it is the label), so it names itself.
        AgentSetTitledTip(_setEnv, L"Global environment variables", L"Variables handed to every session Agentmaster launches, Claude and Codex alike, whatever directory it starts in. One NAME=VALUE per line; '#' starts a comment. The border turns green / amber / red as you type, and the line beneath it says what it made of the text.");
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

    // Agentmaster (tab title naming): re-render the cog's example preview from the CURRENT
    // (unsaved) naming controls — every change of technique / case / underscores re-derives a set
    // of made-up example paths through the REAL 2-arg DeriveSessionTitle, so the preview is exactly
    // what a launch under these picks would name. The examples are chosen so every technique reads
    // visibly different (dotted, one-word camel, spaced, mixed-caps).
    // COMMANDS tab (COMMANDS.md §6a): re-render both /handover-family status lines from the
    // CURRENT (unsaved) control state — the effective (normalized + collision-healed) name each
    // box resolves to, staged against the LIVE state captured at cog open (_cmdLive*, the engine's
    // materialized-name markers; "" == disabled this run). Runs on every keystroke/toggle, so the
    // user sees "what Save will make of this" before saving — including a junk name falling back
    // and a colliding here-name healing to its default.
    void AgentManagerContent::_UpdateCommandsTabStatus()
    {
        if (!_setCmdHandoverStatus && !_setCmdHandoverHereStatus && !_setCmdStandbyStatus)
        {
            return;
        }
        std::wstring ho = _setCmdHandoverName ? std::wstring{ _setCmdHandoverName.Text() } : std::wstring{ ::Agentmaster::kDefaultHandoverCommandName };
        std::wstring hh = _setCmdHandoverHereName ? std::wstring{ _setCmdHandoverHereName.Text() } : std::wstring{ ::Agentmaster::kDefaultHandoverHereCommandName };
        std::wstring hs = _setCmdStandbyName ? std::wstring{ _setCmdStandbyName.Text() } : std::wstring{ ::Agentmaster::kDefaultHandoverStandbyCommandName };
        // What each box normalizes to ALONE (to tell "fell back because invalid/blank" apart from
        // "fell back because it collides with another command").
        const std::wstring hoAlone = ::Agentmaster::NormalizeCommandName(ho);
        const std::wstring hhAlone = ::Agentmaster::NormalizeCommandName(hh);
        const std::wstring hsAlone = ::Agentmaster::NormalizeCommandName(hs);
        ::Agentmaster::ResolveCommandNameTriple(ho, hh, hs); // the names Save will store
        const bool hoOn = _setCmdHandoverEnabled && _setCmdHandoverEnabled.IsOn();
        const bool hhOn = _setCmdHandoverHereEnabled && _setCmdHandoverHereEnabled.IsOn();
        const bool hsOn = _setCmdStandbyEnabled && _setCmdStandbyEnabled.IsOn();
        const auto lineFor = [](bool enabled, const std::wstring& live, const std::wstring& want, const std::wstring& alone) {
            std::wstring s;
            if (!enabled)
            {
                s = live.empty() ? std::wstring{ L"Disabled." } :
                                   (L"Active as /" + live + L" \x2014 DISABLED after restart.");
            }
            else if (want == live)
            {
                s = L"Active as /" + want + L".";
            }
            else if (live.empty())
            {
                s = L"Disabled this run \x2014 enabled as /" + want + L" after restart.";
            }
            else
            {
                s = L"Active as /" + live + L" \x2192 becomes /" + want + L" after restart.";
            }
            if (enabled && alone != want)
            {
                s += alone.empty() ? L" (blank/invalid name \x2014 falls back to the default)" :
                                     L" (name collides with another command \x2014 falls back)";
            }
            return s;
        };
        if (_setCmdHandoverStatus)
        {
            _setCmdHandoverStatus.Text(winrt::hstring{ lineFor(hoOn, _cmdLiveHandoverName, ho, hoAlone) });
        }
        if (_setCmdHandoverHereStatus)
        {
            _setCmdHandoverHereStatus.Text(winrt::hstring{ lineFor(hhOn, _cmdLiveHandoverHereName, hh, hhAlone) });
        }
        if (_setCmdStandbyStatus)
        {
            _setCmdStandbyStatus.Text(winrt::hstring{ lineFor(hsOn, _cmdLiveStandbyName, hs, hsAlone) });
        }
        // §6b shaping summary — the honesty line under the regex boxes: what the CURRENT (unsaved)
        // patterns will actually do, an INVALID one called out explicitly (the consumers fall back
        // to the shipped behavior rather than break, so the only way the user learns is here).
        if (_setCmdShapingStatus)
        {
            const std::wstring find = _setCmdTitleFind ? std::wstring{ _setCmdTitleFind.Text() } : std::wstring{};
            const std::wstring replace = _setCmdTitleReplace ? std::wstring{ _setCmdTitleReplace.Text() } : std::wstring{};
            const std::wstring fileRe = _setCmdFileMatch ? std::wstring{ _setCmdFileMatch.Text() } : std::wstring{};
            std::wstring t = L"Titles: ";
            if (find.empty())
            {
                t += L"cleared \x2014 built-in \x201C(handover)\x201D naming.";
            }
            else if (!::Agentmaster::RegexIsValid(find))
            {
                t += L"find regex INVALID \x2014 built-in naming applies.";
            }
            else if (find == ::Agentmaster::kDefaultCommandTitleFindRegex && replace == ::Agentmaster::kDefaultCommandTitleReplace)
            {
                t += L"default rule \x2014 \x201C<origin> (handover)\x201D, bumping to (handover 2) on a repeat.";
            }
            else
            {
                t += L"CUSTOM rewrite (a title the pattern doesn't match keeps the built-in naming).";
            }
            t += L"  Files: ";
            if (fileRe.empty())
            {
                t += L"cleared \x2014 built-in \x201Cname contains handover\x201D rule.";
            }
            else if (!::Agentmaster::RegexIsValid(fileRe))
            {
                t += L"match regex INVALID \x2014 the built-in rule applies (re-checked at next start).";
            }
            else if (fileRe == ::Agentmaster::kDefaultCommandFileMatchRegex)
            {
                t += L"default rule \x2014 name contains \x201CHANDOVER-\x201D.";
            }
            else
            {
                t += L"CUSTOM match, strict (no first-markdown fallback) \x2014 applies after restart.";
            }
            // §6c: WHERE the briefings land, spelled out from the SAME renderer the definition file
            // gets — so the line is literally the instruction Claude will read, not a paraphrase.
            const std::wstring loc = _setCmdWritePath ? std::wstring{ _setCmdWritePath.Text() } : std::wstring{};
            t += L"  Location: ";
            t += ::Agentmaster::CommandWritePathIsScratchpad(loc) ?
                     std::wstring{ L"the session scratchpad (temp) \x2014 the default." } :
                     (L"written to " + ::Agentmaster::CommandWritePathPhrase(loc) + L".");
            // §6b delete-after, said out loud: deleting a briefing is the one irreversible thing on
            // this tab, so the effective rule (and the wait budget behind it) is spelled out rather
            // than left to a toggle label + a number box the user has to compose in their head.
            t += L"  Briefings: ";
            if (!_setCmdDeleteAfter || !_setCmdDeleteAfter.IsOn())
            {
                t += L"KEPT \x2014 never deleted (the default).";
            }
            else
            {
                const uint32_t mins = ReadDeleteDeadlineMinutesBox(_setCmdDeleteDeadline); // the SAME parse the Save applies
                if (mins == 0)
                {
                    t += L"deleted only if the successor is already up (0 min wait) \x2014 otherwise kept.";
                }
                else
                {
                    // Minutes are what the box takes; hours/days are what 1440 actually MEANS.
                    std::wstring human = std::to_wstring(mins) + L" min";
                    if (mins % (24 * 60) == 0)
                    {
                        const uint32_t d = mins / (24 * 60);
                        human += L" = " + std::to_wstring(d) + (d == 1 ? L" day" : L" days");
                    }
                    else if (mins % 60 == 0)
                    {
                        const uint32_t h = mins / 60;
                        human += L" = " + std::to_wstring(h) + (h == 1 ? L" hour" : L" hours");
                    }
                    t += L"deleted once the successor starts, waiting up to " + human + L" for it \x2014 otherwise kept.";
                }
            }
            _setCmdShapingStatus.Text(winrt::hstring{ t });
        }
        // §6c: what the definition FILES on disk are (sampled at cog open / after a Reinstall).
        // A user-owned file is frozen — it keeps whatever instructions it holds, so the settings on
        // this tab that are delivered THROUGH the definition (the location above, and the command's
        // own name) stop applying to it. That must be said out loud, not discovered.
        if (_setCmdDefState)
        {
            const auto label = [](::Agentmaster::ShippedCommandFileState st) -> const wchar_t* {
                switch (st)
                {
                case ::Agentmaster::ShippedCommandFileState::UpToDate:
                    return L"up to date";
                case ::Agentmaster::ShippedCommandFileState::OursStale:
                    return L"managed \x2014 updates on Save";
                case ::Agentmaster::ShippedCommandFileState::UserOwned:
                    return L"EDITED BY YOU \x2014 left alone";
                default:
                    return L"not installed yet";
                }
            };
            std::wstring t = L"handover.md: ";
            t += label(_cmdDefStateHandover);
            t += L"   \x00B7   handover-here.md: ";
            t += label(_cmdDefStateHere);
            t += L"   \x00B7   handover-standby.md: ";
            t += label(_cmdDefStateStandby);
            const bool anyUserOwned = _cmdDefStateHandover == ::Agentmaster::ShippedCommandFileState::UserOwned ||
                                      _cmdDefStateHere == ::Agentmaster::ShippedCommandFileState::UserOwned ||
                                      _cmdDefStateStandby == ::Agentmaster::ShippedCommandFileState::UserOwned;
            t += anyUserOwned ?
                     L"\nAgentmaster never overwrites a definition you edited \x2014 so that file keeps its own instructions and ignores the settings above. Reinstall to take it back." :
                     L"\nAgentmaster keeps these in step with the settings above; a file you edit yourself is never overwritten.";
            _setCmdDefState.Text(winrt::hstring{ t });
        }
    }

    // COMMANDS.md §6c: re-sample the /handover-family definition files from disk. Called at cog
    // OPEN and after a Reinstall — never from the per-keystroke status pass (three file reads +
    // three SHA-256s). Uses the freshest disk settings for the engine-owned materialized NAMES (which
    // file is live) with the location as currently TYPED, so the state answers "what would Save do
    // to the file I'm looking at".
    void AgentManagerContent::_RefreshCommandDefinitionState()
    try
    {
        auto s = ::Agentmaster::LoadAppSettings();
        if (_setCmdWritePath)
        {
            s.commandHandoverWritePath = ::Agentmaster::NormalizeCommandWritePath(std::wstring{ _setCmdWritePath.Text() });
        }
        const auto [ho, hh, hs] = ::Agentmaster::InspectHandoverCommandFiles(s);
        _cmdDefStateHandover = ho;
        _cmdDefStateHere = hh;
        _cmdDefStateStandby = hs;
    }
    catch (...)
    {
        // Never let a disk hiccup take the cog down; the states keep their previous value (the
        // status line then simply shows what the last successful sample said).
        ::Agentmaster::AgentLogCaughtException(L"_RefreshCommandDefinitionState");
    }

    void AgentManagerContent::_UpdateTitleNamingPreview()
    {
        if (!_titleNamingPreview)
        {
            return;
        }
        ::Agentmaster::TitleNamingOptions opts;
        if (_setTitleNaming)
        {
            // Items: 0 == LastWord (default; also the -1 no-selection build-time state), 1 ==
            // FolderName, 2 == TwoFolders, 3 == Capitals, 4 == Branch, 5 == BranchFolder,
            // 6 == BranchTwoFolders — the same map the seed + Save use.
            opts.naming = _setTitleNaming.SelectedIndex() == 1 ? TabTitleNaming::FolderName :
                          _setTitleNaming.SelectedIndex() == 2 ? TabTitleNaming::TwoFolders :
                          _setTitleNaming.SelectedIndex() == 3 ? TabTitleNaming::Capitals :
                          _setTitleNaming.SelectedIndex() == 4 ? TabTitleNaming::Branch :
                          _setTitleNaming.SelectedIndex() == 5 ? TabTitleNaming::BranchFolder :
                          _setTitleNaming.SelectedIndex() == 6 ? TabTitleNaming::BranchTwoFolders :
                                                                 TabTitleNaming::LastWord;
        }
        if (_setTitleCase)
        {
            opts.caseMode = _setTitleCase.SelectedIndex() == 1 ? TabTitleCase::Lower :
                            _setTitleCase.SelectedIndex() == 2 ? TabTitleCase::Upper :
                                                                 TabTitleCase::Default;
        }
        if (_setTitleUnderscores)
        {
            opts.spacesToUnderscores = _setTitleUnderscores.IsOn();
        }
        // The Branch* techniques consume a branch the CALLER resolves (a real launch reads the
        // dir's .git); these example paths are made-up, so preview with a made-up branch and say
        // so in the block's first line (only when the picked technique actually uses it).
        opts.branch = L"feature/ui";
        static const wchar_t* kExamples[] = {
            L"C:\\repos\\Potato.Tomato.SlangGang",
            L"C:\\work\\PotatoTomato",
            L"D:\\projects\\Potato Tomato Slang",
            L"K:\\source\\PotaTo.Tomato.Slang",
        };
        size_t widest = 0;
        for (const auto* p : kExamples)
        {
            widest = std::max(widest, std::wcslen(p));
        }
        std::wstring text;
        if (::Agentmaster::TitleNamingUsesBranch(opts.naming))
        {
            text = L"(example git branch: feature/ui)";
        }
        for (const auto* p : kExamples)
        {
            std::wstring line{ p };
            line.append(widest - line.size(), L' '); // monospace column so the arrows align
            line += L"  \x2192  ";
            line += ::Agentmaster::DeriveSessionTitle(p, opts);
            if (!text.empty())
            {
                text += L'\n';
            }
            text += line;
        }
        _titleNamingPreview.Text(winrt::hstring{ text });
    }

    // Agentmaster (launch-model picker): the "Launch models" editor's live validation — the
    // _RefreshEnvLex twin over LexLaunchModelsText. Runs on every keystroke (TextChanged) + once on
    // _ShowSettings (a programmatic re-seed of IDENTICAL text fires no TextChanged, so the explicit
    // call keeps the border/status from going stale). Same palette + status shape as the env
    // editors: green = every entry lists, amber = lists with warnings (duplicate name / spaced id /
    // past the cap), red = a skipped entry (missing name or id side), neutral+hint when empty —
    // `ok` is the true offered-model count, so "N models" is exactly every picker submenu's size.
    void AgentManagerContent::_RefreshLaunchModelsLex()
    {
        if (!_setLaunchModels)
        {
            return;
        }
        const auto res = ::Agentmaster::LexLaunchModelsText(std::wstring{ _setLaunchModels.Text() });

        // Same palette as _RefreshEnvLex / _ValidateLaunchBox: green ok / amber warn / red error.
        const SolidColorBrush green = Fill(0xFF, 0x4C, 0xAF, 0x50);
        const SolidColorBrush red = Fill(0xFF, 0xE5, 0x39, 0x35);
        const SolidColorBrush amber = Fill(0xFF, 0xDA, 0xA5, 0x20);
        const SolidColorBrush neutral = Fill(0x60, 0x80, 0x80, 0x80);
        const SolidColorBrush dim = Fill(0xFF, 0x99, 0x99, 0x99);

        const bool empty = (res.ok == 0 && res.warn == 0 && res.error == 0);
        if (_setLaunchModelsBorder)
        {
            const SolidColorBrush bc = empty ? neutral :
                                       res.worst == ::Agentmaster::EnvLineKind::Error ? red :
                                       res.worst == ::Agentmaster::EnvLineKind::Warn  ? amber :
                                                                                        green;
            _setLaunchModelsBorder.BorderBrush(bc);
        }
        if (_setLaunchModelsStatus)
        {
            std::wstring msg;
            SolidColorBrush fg = dim;
            if (empty)
            {
                msg = L"No models \x2014 the pickers offer just Default.";
            }
            else if (res.error > 0)
            {
                msg = L"\x2715 " + res.firstIssue + L" (line " + std::to_wstring(res.firstIssueLine) + L")";
                fg = red;
            }
            else if (res.warn > 0)
            {
                msg = L"\x26A0 " + std::to_wstring(res.ok) + (res.ok == 1 ? L" model \x00B7 " : L" models \x00B7 ") + res.firstIssue + L" (line " + std::to_wstring(res.firstIssueLine) + L")";
                fg = amber;
            }
            else
            {
                msg = L"\x2713 " + std::to_wstring(res.ok) + (res.ok == 1 ? L" model in the pickers" : L" models in the pickers");
                fg = green;
            }
            _setLaunchModelsStatus.Text(winrt::hstring{ msg });
            _setLaunchModelsStatus.Foreground(fg);
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

    // The nightly switch's INSTANT-APPLY commit — the pre-release switch's recipe verbatim: a
    // freshest-disk RMW of just this field (a form Save preserves it from disk), then re-run the
    // silent check so the "vX.Y.Z available!" label + changelog link reflect the new channel right
    // away. Reached only through accepted paths: the warning confirm's "I understand" (ON) and a
    // plain OFF flip — never straight from an ON click. Callers guard (the Toggled try / _Confirm's
    // CATCH_LOG), so this stays plain.
    void AgentManagerContent::_ApplyAllowNightly(bool on)
    {
        _appSettings.allowUpdateNightly = on;
        auto disk = ::Agentmaster::LoadAppSettings();
        disk.allowUpdateNightly = on;
        ::Agentmaster::SaveAppSettings(disk);
        ::Agentmaster::LogNav(std::wstring{ L"update-nightly -> " } + (on ? L"on (warning accepted)" : L"off"));
        _CheckForUpdates(false);
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

        // Prefer the LIVE toggles (so flipping one, then clicking Check, uses the new value before a
        // Save) over the saved settings. The nightly switch can only BE on through the accepted
        // warning path, so reading it live never smuggles an unaccepted opt-in into the check.
        bool prerelease = _appSettings.allowUpdatePrerelease;
        if (_setAllowPrerelease)
        {
            prerelease = _setAllowPrerelease.IsOn();
        }
        bool nightly = _appSettings.allowUpdateNightly;
        if (_setAllowNightly)
        {
            nightly = _setAllowNightly.IsOn();
        }
        // Nav audit: the user clicked "Check for updates" (interactive only — the silent on-cog-open check
        // is automatic, never logged). The result lands in the cog label, not the trail.
        if (interactive)
        {
            ::Agentmaster::LogNav(std::wstring{ L"check-for-updates (allowPrerelease=" } + (prerelease ? L"1" : L"0") + L" allowNightly=" + (nightly ? L"1" : L"0") + L")");
        }

        const std::wstring stateDir = ::Agentmaster::Profiles::ResolveProfileDir();
        const auto cur = ::Agentmaster::Updater::CurrentPackageVersion();

        auto weak = get_weak();
        auto disp = _dispatcher;
        // The worker body is FULLY guarded: an exception escaping a DETACHED thread is
        // std::terminate — instant process death over a background version check. The catch also
        // marshals a UI reset so the button/flag never stay latched ("Checking…" forever).
        const auto worker = [weak, disp, interactive, prerelease, nightly, stateDir, cur]() {
            try
            {
                // The network round-trip (Updater.h uses WinHTTP) — bounded; a longer budget for the
                // explicit button than the silent on-open check.
                const ::Agentmaster::Updater::UpdateInfo info =
                    ::Agentmaster::Updater::CheckForUpdate(cur, prerelease, nightly, interactive ? 8000 : 5000);
                // The cog leg of the [update] observability trail (the startup/hourly legs log inside
                // RunUpdateCheckAndPrompt; this path calls CheckForUpdate directly, so log here).
                {
                    const std::wstring tag = interactive ? L"check (cog)" : L"check (cog-silent)";
                    std::wstring line;
                    if (!info.checked)
                    {
                        line = tag + L" failed: " + (info.error.empty() ? std::wstring{ L"unknown" } : info.error);
                    }
                    else if (info.available)
                    {
                        line = tag + L" done: " + ::Agentmaster::Updater::DisplayVersion(info) + L" available (" + (info.isNightly ? L"NIGHTLY" : info.isPrerelease ? L"pre-release" : L"stable") + (info.installable ? L", installable)" : L", NO installable assets)");
                    }
                    else
                    {
                        line = tag + L" done: up to date (latest=" + (info.latestTag.empty() ? std::wstring{ L"none" } : info.latestTag) + L")";
                    }
                    ::Agentmaster::Updater::LogUpdate(stateDir, line + L" cur=" + info.currentVersionStr + L" prerelease=" + (prerelease ? L"on" : L"off") + L" nightly=" + (nightly ? L"on" : L"off"));
                }
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
                    try
                    {
                        self->_ApplyUpdateCheckResult(interactive, stateDir, info);
                    }
                    catch (...)
                    {
                        // Recovery: never leave the button dead / the flag latched over a UI hiccup —
                        // the next click must still work.
                        try
                        {
                            self->_interactiveUpdateInFlight = false;
                            if (self->_setCheckUpdates)
                            {
                                self->_setCheckUpdates.IsEnabled(true);
                                self->_setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
                            }
                            ::Agentmaster::Updater::LogUpdate(stateDir, L"check (cog) UI completion CRASHED (recovered \x2014 button restored)");
                        }
                        catch (...)
                        {
                            // recovery-of-recovery: the crash above IS logged (or the logger itself
                            // failed) — _ShowSettings' fresh-open reset is the final backstop
                        }
                    }
                });
            }
            catch (...)
            {
                try
                {
                    ::Agentmaster::Updater::LogUpdate(stateDir, L"check (cog) worker CRASHED (exception swallowed)");
                    if (disp)
                    {
                        disp.TryEnqueue([weak, interactive]() {
                            try
                            {
                                if (auto self = weak.get(); self && interactive)
                                {
                                    self->_interactiveUpdateInFlight = false;
                                    if (self->_setCheckUpdates)
                                    {
                                        self->_setCheckUpdates.IsEnabled(true);
                                        self->_setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
                                    }
                                    if (self->_setUpdateStatus)
                                    {
                                        self->_setUpdateStatus.Text(L"Check failed");
                                    }
                                }
                            }
                            catch (...)
                            {
                                // recovery-of-recovery: the worker crash was already logged above —
                                // _ShowSettings' fresh-open reset is the final backstop
                            }
                        });
                    }
                }
                catch (...)
                {
                    // logger-failed (the crash log or the reset enqueue threw): a detached thread
                    // must swallow terminally — nothing left to report through
                }
            }
        };
        try
        {
            std::thread(worker).detach();
        }
        catch (...)
        {
            // std::thread construction can throw (resource limits) — restore the interactive UI so
            // the button isn't left disabled at "Checking…" with the flag latched.
            _interactiveUpdateInFlight = false;
            if (_setCheckUpdates)
            {
                _setCheckUpdates.IsEnabled(true);
                _setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
            }
            if (_setUpdateStatus)
            {
                _setUpdateStatus.Text(L"Check failed");
            }
            try
            {
                ::Agentmaster::Updater::LogUpdate(stateDir, L"check (cog) worker spawn FAILED (thread \x2014 UI restored)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
        }
    }

    // The check's UI completion — label/changelog updates + (interactive only) the prompt + apply.
    // Split out of the worker lambda so the recovery catch above stays small; runs on the UI thread.
    void AgentManagerContent::_ApplyUpdateCheckResult(bool interactive, const std::wstring& stateDir, const ::Agentmaster::Updater::UpdateInfo& info)
    {
        if (interactive)
        {
            _interactiveUpdateInFlight = false;
            if (_setCheckUpdates)
            {
                _setCheckUpdates.IsEnabled(true);
                _setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
            }
        }
        if (_setUpdateStatus)
        {
            if (info.available)
            {
                // "v0.4.3 available!" — dark green, legible on the dark settings card.
                _setUpdateStatus.Text(winrt::hstring{ ::Agentmaster::Updater::DisplayVersion(info) + L" available!" });
                _setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x2E, 0xA0, 0x43) });
            }
            else if (interactive)
            {
                _setUpdateStatus.Text(info.checked ? winrt::hstring{ L"You're on the latest version" } : winrt::hstring{ L"Couldn't reach GitHub" });
                _setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x99, 0x99, 0x99) });
            }
            else
            {
                _setUpdateStatus.Text(L""); // silent check: stay quiet unless there IS an update
            }
        }
        // "Update's changelog" link: reveal it (and remember its release page) when an update is
        // available — from EITHER the silent on-open check or the explicit button — else hide it.
        if (info.available)
        {
            _lastUpdateChangelogUrl = ::Agentmaster::Updater::ChangelogUrl(info);
            if (_setUpdateChangelog)
            {
                _setUpdateChangelog.Visibility(Visibility::Visible);
            }
        }
        else
        {
            _lastUpdateChangelogUrl.clear();
            if (_setUpdateChangelog)
            {
                _setUpdateChangelog.Visibility(Visibility::Collapsed);
            }
        }
        // Interactive only: prompt + apply on a found update (the silent on-open check just labels).
        // ShowUpdatePrompt/ApplyDecision are themselves no-throw (Updater.h) — a broken prompt reads
        // as remind-next-restart, a failed decision-apply never quits the app with no installer running.
        if (interactive && info.available)
        {
            const HWND owner = ::GetActiveWindow();
            const auto d = ::Agentmaster::Updater::ShowUpdatePrompt(owner, info);
            if (::Agentmaster::Updater::ApplyDecision(stateDir, info, d, owner))
            {
                // The installer was launched detached; close the app gracefully so the package
                // isn't in use while it upgrades + relaunches (the page's RequestQuit).
                if (_quitForUpdateHandler)
                {
                    _quitForUpdateHandler();
                }
            }
        }
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
        // One-click install/migrate — the primary fix. Its label + tooltip are set in _ShowClaudeMissing
        // from ClaudeInstallKind() (LegacyNpm => "Run claude install"; None => "Install native (PowerShell)");
        // hidden when a native claude is somehow already present. Opens a VISIBLE PowerShell window running
        // the official command so the user can watch it, then click Re-check (the gate also auto-recovers).
        _claudeMissingInstallBtn = Button{};
        _claudeMissingInstallBtn.Content(winrt::box_value(L"Install Claude Code"));
        _claudeMissingInstallBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            const auto kind = ::Agentmaster::ClaudeInstallKind();
            if (kind == ::Agentmaster::ClaudeInstallState::Native)
            {
                _HideClaudeMissing(); // installed via another path while the modal was open — clear the gate
                _ValidateLaunchBox();
                return;
            }
            if (::Agentmaster::LaunchClaudeInstall(kind))
            {
                if (_claudeMissingStatus)
                {
                    _claudeMissingStatus.Text(L"Installing in a PowerShell window\x2026 when it finishes, click Re-check.");
                }
            }
            else if (_claudeMissingStatus)
            {
                _claudeMissingStatus.Text(L"Couldn't open PowerShell \x2014 use Get Claude Code, or install manually.");
            }
        });
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
        buttons.Children().Append(_claudeMissingInstallBtn); // leftmost = the primary one-click fix
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
        // Point the one-click install button at the RIGHT fix for the user's situation (recomputed each
        // show — a claude may have appeared/vanished since the modal was built).
        if (_claudeMissingInstallBtn)
        {
            switch (::Agentmaster::ClaudeInstallKind())
            {
            case ::Agentmaster::ClaudeInstallState::LegacyNpm:
                _claudeMissingInstallBtn.Content(winrt::box_value(L"Run claude install"));
                AgentSetTip(_claudeMissingInstallBtn, L"An npm/Node `claude` is installed but not the native build. Opens a PowerShell window and runs  claude install  to migrate it to native \x2014 then click Re-check.");
                _claudeMissingInstallBtn.Visibility(Visibility::Visible);
                break;
            case ::Agentmaster::ClaudeInstallState::None:
                _claudeMissingInstallBtn.Content(winrt::box_value(L"Install native (PowerShell)"));
                AgentSetTip(_claudeMissingInstallBtn, L"Opens a PowerShell window and runs the official installer  irm https://claude.ai/install.ps1 | iex  (from claude.ai) \x2014 then click Re-check.");
                _claudeMissingInstallBtn.Visibility(Visibility::Visible);
                break;
            default: // Native — already resolved; nothing to install
                _claudeMissingInstallBtn.Visibility(Visibility::Collapsed);
                break;
            }
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

    // Agentmaster (launch-model picker -> "Specify..."): show the type-any-model-id prompt over the
    // Manager. The dimmed LAYER is built once and reused (the claude-missing/settings idiom); the
    // CARD inside is rebuilt per open because it carries this invocation's callback and re-reads the
    // durable model list — a prompt opened after another window specified a model must list it.
    void AgentManagerContent::_PromptForModel(std::function<void(winrt::hstring)> onPicked, const std::wstring& seed)
    {
        if (!_root || !_dispatcher)
        {
            return;
        }
        // Defer off the invoking flyout's click: a MenuFlyout closes asynchronously and its close
        // restores focus, which would fight the prompt's text box for it (the tag editor's rule).
        _dispatcher.TryEnqueue([this, onPicked, seed]() {
            try
            {
                if (!_specifyModelOverlay)
                {
                    _specifyModelOverlay = Grid{};
                    _specifyModelOverlay.Background(SolidColorBrush{ ColorHelper::FromArgb(0xA0, 0x00, 0x00, 0x00) });
                    Grid::SetRow(_specifyModelOverlay, 0);
                    Grid::SetRowSpan(_specifyModelOverlay, 99);
                    Grid::SetColumnSpan(_specifyModelOverlay, 99);
                    // Backdrop press == cancel: hide, and deliberately DON'T call onPicked (nothing
                    // launches unless the user committed a model).
                    _specifyModelOverlay.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
                        _HideSpecifyModel();
                    });
                    _root.Children().Append(_specifyModelOverlay);
                }
                auto built = AgentBuildSpecifyModelCard(seed, ::Agentmaster::ParseLaunchModels(_appSettings.launchModels), [this, onPicked](winrt::hstring id) {
                    _HideSpecifyModel();
                    if (!id.empty())
                    {
                        onPicked(id);
                    }
                });
                _specifyModelOverlay.Children().Clear();
                _specifyModelOverlay.Children().Append(built.card);
                _specifyModelOverlay.Visibility(Visibility::Visible);
                // Focus the box one tick AFTER the show — a same-tick Focus on a not-yet-laid-out
                // control no-ops (the tag editor learned this on its first open).
                auto box = built.box;
                _dispatcher.TryEnqueue([box]() {
                    if (box)
                    {
                        box.Focus(FocusState::Programmatic);
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::LogSwallowedException(L"AgentManagerContent::_PromptForModel");
            }
        });
    }

    void AgentManagerContent::_HideSpecifyModel()
    {
        if (_specifyModelOverlay)
        {
            _specifyModelOverlay.Visibility(Visibility::Collapsed);
            _specifyModelOverlay.Children().Clear(); // release the card (and its captured callback)
        }
    }

    // Wire a successor-model combo's "Specify..." row (seeded at index 1 by seedModelCombo). Chosen
    // once at BUILD time — seedModelCombo only refills Items(), so the handler survives every reseed.
    void AgentManagerContent::_WireSuccessorModelCombo(const winrt::Windows::UI::Xaml::Controls::ComboBox& combo, std::vector<std::wstring>* ids)
    {
        if (!combo || !ids)
        {
            return;
        }
        // The last REAL pick, so a dismissed prompt can put the selection back exactly where it was
        // (a combo whose "Specify..." row stayed selected would read as a model nobody chose).
        auto prev = std::make_shared<int>(0);
        combo.SelectionChanged([this, combo, ids, prev](const IInspectable&, const winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs&) {
            const int idx = combo.SelectedIndex();
            const bool isSentinel = idx > 0 && idx < static_cast<int>(ids->size()) && (*ids)[static_cast<size_t>(idx)] == kSpecifyModelSentinel;
            if (!isSentinel)
            {
                *prev = idx < 0 ? 0 : idx;
                return;
            }
            // Restore the previous pick FIRST (this re-enters this handler, harmlessly — `back` is
            // never the sentinel), so cancelling the prompt simply leaves the combo as it was.
            const int back = *prev;
            combo.SelectedIndex(back);
            _PromptForModel([combo, ids](winrt::hstring picked) {
                const std::wstring id{ picked };
                if (id.empty())
                {
                    return;
                }
                // Already offered (a configured model typed by hand, or a second visit to the same
                // custom id)? Select that row rather than growing a duplicate.
                for (size_t k = 0; k < ids->size(); ++k)
                {
                    const auto& have = (*ids)[k];
                    if (have.size() == id.size() && std::equal(have.begin(), have.end(), id.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); }))
                    {
                        combo.SelectedIndex(static_cast<int>(k));
                        return;
                    }
                }
                combo.Items().Append(winrt::box_value(winrt::hstring{ L"(custom) " + id }));
                ids->push_back(id);
                combo.SelectedIndex(static_cast<int>(ids->size()) - 1);
            });
        });
    }

    std::function<void(std::function<void(winrt::hstring)>)> AgentManagerContent::_ModelSpecifyOpener()
    {
        auto weak = get_weak();
        return [weak](std::function<void(winrt::hstring)> pick) {
            if (auto self = weak.get())
            {
                self->_PromptForModel(pick);
            }
        };
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
