// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the LAUNCH BAR (AgentLaunchBar.h has the design: the two hosts, the rules, the ownership). The
// launch, validation and drop-down code below is AgentManagerContent.Launch.cpp's, moved verbatim; the changes are
// only those a second host forces:
//   - the Manager members it read (its handlers, _registry, _appSettings, _recentDirs, _root, _ShowClaudeMissing,
//     _ReflowLaunchBar, _ModelSpecifyOpener) are reached through AgentLaunchBarHost;
//   - every XAML handler captures a weak_ptr to the bar instead of a raw Manager `this` (the bar is not a WinRT
//     object, and an element can outlive it in its host's tree);
//   - three host hooks the side panel needs (launched, textEdited, the Claude-only chip).
// The build of the elements comes from AgentManagerContent::_BuildLayout (the toolbar block and the drop-down block),
// moved verbatim as well.
#include "pch.h"
#include "AgentLaunchBar.h"

#include "AgentTipHelpers.h" // AgentSetTip / AgentSetTitledTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCatchLog.h" // AgentLogCaughtException — every guarded step (Rule #18: never a bare catch)
#include "AgentStatusColors.h" // AgentAnimationsEnabled — required BY AgentManagerContent.Internal.h's BuildPendingDots (every Internal.h TU includes it first)
#include "AgentModelMenu.h" // AgentFillModelPickItems — the hover menu IS the "Open New Session Here" picker
#include "AgentMaster/ClaudeSpawn.h" // ClaudeConversationExists / ResolveClaudeTranscriptPath — a typed session id's transcript
#include "AgentMaster/Engine.h" // EnsureClaudeAvailable — the native-exe-only launch gate
#include "AgentMaster/LaunchPresetsOs.h" // PRESETS (HARNESSES.md): LaunchMenuEntries — the Launch button's hover menu
#include "AgentMaster/Persistence.h" // SaveRecentDirs — the recent-folders list
#include "AgentMaster/ProcessInspect.h" // ReadGitBranchForDir / ListGitWorktrees (the drop-down) + ExternalClaudeRow (Internal.h's MakeSortKey overload)
#include "AgentMaster/ProfileBootstrap.h" // (the AgentManagerContent.Internal.h include convention — keeps the include set board-identical)
#include "AgentMaster/SessionRegistry.h" // SessionRegistry::Get / Snapshot — a session id's dir + title, the live folders in RECENT
#include "AgentMaster/TranscriptStore.h" // ReadTranscriptQuickFacts — resolve a launch-box session id's cwd

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem> // create_directories — the "Create & Launch" affordance for a not-yet-existing dir
#include <optional>
#include <system_error> // std::error_code — non-throwing create_directories
#include <utility> // std::exchange — the drop-down re-fit tick consumes its "re-anchor round" flag
#include <shobjidl.h> // IFileOpenDialog — required BY AgentManagerContent.Internal.h's PickFolder / PickClaudeExe helpers (per-TU anonymous-namespace copy)

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
using namespace winrt::Windows::UI::Xaml::Input; // KeyRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue, VirtualKey
using namespace winrt::Microsoft::Terminal::Settings::Model; // (Board.cpp parity — Internal.h is written against this using set)
using namespace Agentmaster;
// The shared tooltip recipe (AgentTipHelpers.h) — using-DECLARATIONS so Internal.h's GLOBAL-scope
// anonymous-namespace helpers (e.g. TimingText) can call it unqualified, exactly as in the Manager TUs.
using winrt::TerminalApp::implementation::AgentSetTip;
using winrt::TerminalApp::implementation::AgentSetTitledTip;
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (Text/Fill/NormPath/PathEq/IsDir/PickFolder/...)

namespace winrt::TerminalApp::implementation
{
    // The handlers below name `IInspectable` unqualified, as they did in the Agent Manager's TUs. There it resolved
    // through the alias every XAML-generated header of this project declares in this namespace (the *.xaml.g.h files,
    // which those TUs include through their headers); this TU includes none, and at global scope the name is ambiguous
    // (the ABI ::IInspectable vs the using-directive's winrt::Windows::Foundation::IInspectable). The same alias here
    // keeps the moved code verbatim; redeclaring an alias of the same type is legal if one ever arrives.
    using IInspectable = ::winrt::Windows::Foundation::IInspectable;

    namespace
    {
        // The drop-down's anchor offsets are rewritten only when the anchor moved by more than this (DIPs). Any offset
        // write invalidates the popup root's arrange (WinUI CPopup::SetValue does so for every HorizontalOffset /
        // VerticalOffset write, equal or not), so an unchanged write would cost a layout pass, and a re-anchor round
        // would count its own write as a move.
        constexpr double kPathPickerAnchorEpsilonDips = 0.25;

        // How many "re-anchor after the next layout pass" rounds may follow one open or one host request
        // (_pathPickerReanchorRounds). The first round normally settles the anchor; a second one covers a layout pass
        // that belonged to another window on this UI thread; the cap keeps a list from chasing a box forever.
        constexpr uint8_t kPathPickerReanchorMaxRounds = 3;

        // COM identity of two projected objects (their IUnknown), null-safe. GettingFocus names the new focus as a
        // DependencyObject and the box is a TextBox: two projections of one object are equal only by identity.
        //   a, b    — the objects to compare (either may be null).
        //   returns — true when both are non-null and are the same object.
        bool LaunchBarSameObject(const IInspectable& a, const IInspectable& b) noexcept
        {
            if (!a || !b)
            {
                return false;
            }
            const auto ua = a.try_as<winrt::Windows::Foundation::IUnknown>();
            const auto ub = b.try_as<winrt::Windows::Foundation::IUnknown>();
            return ua && ub && winrt::get_abi(ua) == winrt::get_abi(ub);
        }
    }

    std::shared_ptr<AgentLaunchBar> AgentLaunchBar::Create(AgentLaunchBarHost host)
    {
        // make_shared can't reach the private constructor; the bar must be IN a shared_ptr before _Build wires a single
        // handler, because every handler captures weak_from_this().
        std::shared_ptr<AgentLaunchBar> bar{ new AgentLaunchBar(std::move(host)) };
        bar->_Build();
        return bar;
    }

    AgentLaunchBar::AgentLaunchBar(AgentLaunchBarHost host) :
        _host{ std::move(host) }
    {
        // The window's shared recent-folders list, or a private one when the host has none (the drop-down still works;
        // it just can't see the other bar's pushes).
        _recentDirs = _host.recentDirs ? _host.recentDirs : std::make_shared<AgentLaunchRecentDirs>();
        _dispatcher = _host.dispatcher ? _host.dispatcher : DispatcherQueue::GetForCurrentThread();
    }

    AgentLaunchBar::~AgentLaunchBar()
    {
        Shutdown(); // noexcept + guarded inside: a page teardown on a pool thread can't throw out of here
    }

    winrt::Windows::UI::Xaml::FrameworkElement AgentLaunchBar::AgentElement() const noexcept
    {
        if (_launchAgentBtn)
        {
            return _launchAgentBtn;
        }
        return _agentChip;
    }

    void AgentLaunchBar::_Build()
    {
        const auto weak = weak_from_this();

        // Agentmaster (Codex-launch): the agent toggle — Claude (default) <-> Codex. Click cycles it
        // (the scope/sort/autorunner toggle idiom). It retargets the SAME cwd box + Launch button, so a
        // managed Codex launches exactly the way a Claude does ("do what we do for Claude"). A Codex
        // launch is directory-only — Codex has no typed-id resume/fork here (Codex resume is reached via
        // the Archive page / window-restore / EXTERNAL Adopt), so _ValidateLaunchBox suppresses those.
        if (_host.codexToggle)
        {
            _launchAgentBtn = Button{};
            _launchAgentBtn.FontSize(11);
            _launchAgentBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTitledTip(_launchAgentBtn, L"Agent to launch", L"Click to toggle between Claude and Codex. The box and the Launch button beside it retarget to match \x2014 a Codex launch is directory-only (no session id to resume or fork).");
            _launchAgentBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                const auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                self->_launchCodex = !self->_launchCodex;
                self->_UpdateLaunchAgentButton();
                self->_ValidateLaunchBox(); // repaint the Launch button text + resume/fork affordances for the new agent
            });
        }
        else
        {
            // Agentmaster (side panel "+" launcher, SIDE_PANEL.md §2e): a host that launches CLAUDE ONLY shows the
            // agent as a fixed chip in the toggle's place — the same "● Claude" content and padding, so the row reads
            // like the Manager's, but no button chrome and no click (a toggle that toggles nothing would read broken).
            // Codex launches from this surface are a planned feature; the tooltip says where Codex launches today. The
            // near-invisible fill (alpha 0x01) is what makes the chip hit-testable, so the tooltip can open on it.
            _agentChip = Border{};
            _agentChip.Padding(Thickness{ 8, 1, 8, 1 });
            _agentChip.VerticalAlignment(VerticalAlignment::Center);
            _agentChip.Background(Fill(0x01, 0xFF, 0xFF, 0xFF));
            AgentSetTitledTip(_agentChip, L"Agent to launch", L"This launcher starts Claude Code sessions. Codex launches from the side panel are planned \x2014 the Agent Manager's launch bar can start a Codex session now.");
        }
        _UpdateLaunchAgentButton();

        _cwdBox = TextBox{};
        // Agentmaster: 504 (the old fixed width) is the COMFORTABLE width; the box grows with its typed
        // content — a NoWrap TextBox in the horizontal launch bar measures to its text — between the
        // live MinWidth/MaxWidth its host sets (the Manager's _ReflowLaunchBar per stage: 504 floor when
        // there's room; a 240/160 floor once the pane narrows enough to shrink / wrap; the side panel's
        // card pins it to the room its window has), so a long path never pushes the Launch button
        // off-screen. Left alignment keeps it content-sized rather than stretched-to-fill.
        _cwdBox.MinWidth(504);
        _cwdBox.MaxWidth(504); // seed; the host (re)computes Min/Max on first layout
        _cwdBox.HorizontalAlignment(HorizontalAlignment::Left);
        _cwdBox.PlaceholderText(L"working directory (the M axis)");
        // Agentmaster: the box accepts EITHER a working dir (new session) OR a session id (Resume / Fork).
        // The underline + the Launch button's label track what you type — the tip names all three states.
        AgentSetTitledTip(_cwdBox, L"Where to launch", L"A working directory for a new session, or a Claude session id to resume or fork. Start typing to pick from recent and matching folders.\n\nThe underline reads back what you typed: green = a session id found on disk (Launch becomes Resume, with a Fork twin) \x00B7 amber = a folder that doesn't exist yet (Launch creates it) \x00B7 red = an id with no conversation on disk, or a path that can't be created (Launch is disabled).");
        {
            wchar_t up[MAX_PATH];
            const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                _cwdBox.Text(winrt::hstring{ up, n });
            }
        }
        // Path-picker drop-down. Open it only on *user* focus (Pointer/Keyboard) so the
        // dropdown doesn't pop every time the tab is programmatically activated.
        // HOW focus arrived is recorded here, in GettingFocus: it is raised synchronously, inside the Focus call or the
        // input that moves focus, with the input mode as it was (WinUI RaiseAndProcessGettingAndLosingFocusEvents gets
        // the NON-coerced state). GotFocus comes later and can no longer tell (see _cwdBoxFocusArrival).
        _cwdBox.GettingFocus([weak](const UIElement&, const GettingFocusEventArgs& e) {
            const auto self = weak.lock();
            if (!self || !self->_cwdBox)
            {
                return;
            }
            try
            {
                // Only for the box itself: GettingFocus bubbles, and a part inside the box taking focus is not the box's
                // focus (the TextBox template's parts are not tab stops today; the check keeps it exact anyway).
                if (LaunchBarSameObject(e.NewFocusedElement(), self->_cwdBox))
                {
                    self->_cwdBoxFocusArrival = e.FocusState();
                }
            }
            catch (...)
            {
                // GotFocus then falls back to the coerced FocusState() (the old read). Recorded (Rule #18).
                ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar: box GettingFocus");
            }
        });
        _cwdBox.GotFocus([weak](const IInspectable&, const RoutedEventArgs&) {
            const auto self = weak.lock();
            if (!self || !self->_cwdBox)
            {
                return;
            }
            // (Re)focusing the box clears any prior Esc/Enter/blur dismissal, so the next
            // keystroke brings the list back; when the focus itself came from the user
            // (pointer/keyboard) open it right away.
            self->_pathPickerUserDismissed = false;
            // The REAL input mode (GettingFocus above), consumed here: every focus change raises its own GettingFocus.
            // Not FocusState(): XAML coerces a programmatic focus to the last input device's state before raising
            // GotFocus, so the side panel card's Focus(Programmatic) right after the "+" click read as Pointer and opened
            // the list on every "+". Unfocused = no GettingFocus seen (a re-focus of the already focused box, whose
            // coerced state changed, raises GotFocus alone): then the old read.
            auto fs = self->_cwdBoxFocusArrival;
            self->_cwdBoxFocusArrival = FocusState::Unfocused;
            if (fs == FocusState::Unfocused)
            {
                fs = self->_cwdBox.FocusState();
            }
            if (fs == FocusState::Pointer || fs == FocusState::Keyboard)
            {
                self->_OpenPathPicker();
            }
        });
        // Clicking back into an ALREADY-focused box fires no GotFocus, so a tap also re-shows
        // a previously dismissed picker (open only if needed; a tap is not a drag, so it
        // won't fight text selection).
        _cwdBox.Tapped([weak](const IInspectable&, const TappedRoutedEventArgs&) {
            const auto self = weak.lock();
            if (!self || !self->_cwdBox)
            {
                return;
            }
            self->_pathPickerUserDismissed = false;
            if (!self->_pathPopup || !self->_pathPopup.IsOpen())
            {
                self->_OpenPathPicker();
            }
        });
        _cwdBox.TextChanged([weak](const IInspectable&, const TextChangedEventArgs&) {
            const auto self = weak.lock();
            if (!self || !self->_cwdBox)
            {
                return;
            }
            // Is this the TextChanged of a PROGRAMMATIC write (SetBoxText, a Browse… pick)? TextChanged arrives later than
            // its write, so the box may hold focus by now (the card focuses it right after copying the Manager's text) —
            // and a write is not typing: it must not OPEN the list (see _programmaticTextPending). The box still holding
            // exactly the written text says it is that write; any other text means the user edited since.
            bool programmaticWrite = false;
            if (self->_programmaticTextPending)
            {
                programmaticWrite = *self->_programmaticTextPending == std::wstring{ self->_cwdBox.Text() };
                self->_programmaticTextPending.reset(); // one TextChanged per write; a lost one is cleared here by the next edit
            }
            // Agentmaster: paint the validation underline + enable/disable launch on every edit
            // (a working dir OR a session id). Runs before the picker logic's focus early-out so
            // it always reflects the current text.
            self->_ValidateLaunchBox();
            // Agentmaster (side panel "+" launcher): the host mirrors the text (the card's box shows the same text
            // as the Agent Manager's). Before the picker logic below, so an early-out never skips the mirror.
            self->_NotifyTextEdited();
            // Typing should ALWAYS surface the list. The prior version only refreshed an
            // already-open popup, so whenever the box held focus while the popup was closed
            // (focus arrived programmatically, a stray LostFocus closed it, etc.) typing
            // showed nothing — exactly the reported "list doesn't show" symptom. Now we
            // reopen when closed and refresh when open. The lone exception is an explicit
            // Esc/Enter dismissal, which sticks until the box is refocused or tapped.
            if (self->_cwdBox.FocusState() == FocusState::Unfocused || self->_pathPickerUserDismissed)
            {
                return;
            }
            if (self->_pathPopup && self->_pathPopup.IsOpen())
            {
                self->_RebuildPathPicker(); // an open list follows any text, a write's too
            }
            else if (!programmaticWrite)
            {
                self->_OpenPathPicker(); // typing opens it; a write leaves a closed list closed
            }
        });
        // Close only when focus truly left (not when a row button briefly takes it):
        // defer the check a tick, and bail if the box has refocused itself (after a pick).
        {
            auto disp = _dispatcher;
            _cwdBox.LostFocus([weak, disp](const IInspectable&, const RoutedEventArgs&) {
                if (!disp)
                {
                    return;
                }
                disp.TryEnqueue([weak]() {
                    auto self = weak.lock();
                    if (!self || !self->_cwdBox)
                    {
                        return;
                    }
                    if (self->_cwdBox.FocusState() != FocusState::Unfocused)
                    {
                        return; // regained focus (e.g. after clicking a row) — leave it alone
                    }
                    // Focus truly left the box: close the picker (if open) and normalize
                    // what's there. Mark it dismissed so the normalize's TextChanged won't
                    // reopen the (now-closed) picker; refocusing/tapping the box clears it.
                    self->_pathPickerUserDismissed = true;
                    self->_ClosePathPicker();
                    self->_NormalizeCwdBox();
                });
            });
        }
        _cwdBox.KeyDown([weak](const IInspectable&, const KeyRoutedEventArgs& e) {
            const auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (e.Key() == VirtualKey::Escape)
            {
                if (self->_pathPopup && self->_pathPopup.IsOpen())
                {
                    self->_pathPickerUserDismissed = true; // stays dismissed until refocus/tap
                    self->_ClosePathPicker();
                    e.Handled(true);
                }
            }
            else if (e.Key() == VirtualKey::Enter)
            {
                self->_pathPickerUserDismissed = true; // committing dismisses; keep normalize from reopening it
                self->_ClosePathPicker();
                self->_NormalizeCwdBox(); // commit: normalize what the user typed
            }
        });
        // Agentmaster: the box + a validation underline beneath it, in a vertical column so the
        // underline tracks the box width. The path-picker Popup anchors via _cwdBox.TransformToVisual
        // (robust to this wrapping), so the dropdown placement is unaffected.
        _cwdCol = StackPanel{};
        _cwdCol.Orientation(Orientation::Vertical);
        _cwdCol.Spacing(2);
        _cwdCol.VerticalAlignment(VerticalAlignment::Center);
        _cwdCol.Children().Append(_cwdBox);
        _cwdUnderline = Border{};
        _cwdUnderline.Height(2);
        // Stretch (no fixed width) so the underline always spans the box's CURRENT width: the box now
        // grows with content and cwdCol's width tracks it, so a stretched underline stays matched.
        _cwdUnderline.HorizontalAlignment(HorizontalAlignment::Stretch);
        _cwdUnderline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
        _cwdUnderline.Background(Fill(0x00, 0x00, 0x00, 0x00)); // transparent = neutral; kept present so painting it never reflows the bar
        _cwdCol.Children().Append(_cwdUnderline);

        _launchBtn = Button{};
        _launchBtn.Content(winrt::box_value(L"Launch Claude"));
        AgentSetTip(_launchBtn, L"Start the selected agent in the working directory above \x2014 or resume the conversation when a session id is entered. Hover to pick a model or a preset for the new session.");
        _launchBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weak.lock())
            {
                self->ClickLaunch();
            }
        });
        // PRESETS (HARNESSES.md §8.2): hovering the button opens the "Open New Session Here" picker
        // (Default · Specify… · models · presets).
        //   - A HOVER opens it AT ONCE (2026-10-10, the user's request: the 450 ms wait read as lag) — a mouse with no
        //     button held, or a pen hovering over the screen. The menu takes no focus and closes again once the
        //     pointer is 80 DIP away from it (TransientWithDismissOnPointerMoveAway, in _ShowLaunchMenu; the distance
        //     is WinUI's FlyoutBase::OnRootVisualPointerMoved), so a pointer that only crosses the button flashes the
        //     menu and is never trapped by it. A click on the button still lands while the menu is up: a transient
        //     flyout's pass-through element is the root visual, which lets EVERY press through its light-dismiss layer
        //     (WinUI PointerInputProcessor), and ClickLaunch closes the menu as well.
        //   - A pointer that arrives IN CONTACT keeps the old 450 ms hold (_launchHoverTimer): a touch, a pen touching
        //     down, a mouse dragging in with a button held. A touch has no hover — its PointerEntered comes with the
        //     press of a tap — so opening at once would flash the menu over every tap's launch; with the hold, the
        //     tap's Click (ClickLaunch stops the timer) wins, and a finger that rests 450 ms still gets the menu.
        // The Tick holds only a weak ref, so a torn-down bar is never touched — and a tick that finds the bar gone
        // stops its own timer (`sender`), so an orphaned timer can't tick forever (a running DispatcherTimer is kept
        // alive by the dispatcher, not by the bar).
        _launchHoverTimer = DispatcherTimer{};
        _launchHoverTimer.Interval(std::chrono::milliseconds{ 450 });
        _launchHoverTimer.Tick([weak](const IInspectable& sender, const IInspectable&) {
            if (auto self = weak.lock())
            {
                if (self->_launchHoverTimer)
                {
                    self->_launchHoverTimer.Stop(); // one-shot
                }
                self->_ShowLaunchMenu();
            }
            else if (const auto t = sender.try_as<DispatcherTimer>())
            {
                t.Stop();
            }
        });
        _launchBtn.PointerEntered([weak](const IInspectable&, const PointerRoutedEventArgs& e) {
            const auto self = weak.lock();
            if (!self || self->_launchMenu)
            {
                return; // the bar is gone, or its menu is still up (the pointer came back from it): nothing to stack
            }
            // Hover or contact? A failed read takes the contact path — the old hold, which can only delay the menu,
            // never flash it over a tap.
            bool inContact = true;
            try
            {
                inContact = e.GetCurrentPoint(nullptr).IsInContact();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar: Launch hover - read the pointer's contact state");
            }
            if (!inContact)
            {
                self->_ShowLaunchMenu(); // a hover: at once (a failed open is logged inside; the next hover tries again)
            }
            else if (self->_launchHoverTimer)
            {
                self->_launchHoverTimer.Start(); // Start() restarts the interval, so a re-entry waits the full delay
            }
        });
        _launchBtn.PointerExited([weak](const IInspectable&, const PointerRoutedEventArgs&) {
            const auto self = weak.lock();
            if (self && self->_launchHoverTimer)
            {
                self->_launchHoverTimer.Stop();
            }
        });

        // Fork — hidden unless the box holds a FOUND session id; forks that conversation into a
        // NEW one (the original transcript is untouched), mirroring the Sessions page's "Fork here".
        _forkBtn = Button{};
        _forkBtn.Content(winrt::box_value(L"Fork"));
        _forkBtn.Visibility(Visibility::Collapsed);
        AgentSetTip(_forkBtn, L"Fork the entered session into a NEW, independent conversation \x2014 the original transcript is left untouched.");
        _forkBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weak.lock())
            {
                self->ClickFork();
            }
        });

        // ---- Launch path-picker drop-down (a Popup anchored under the cwd box) ----
        // Parented into the host's panel (top-left aligned) so its offset is host-relative; it
        // renders in the overlay above the host's content. A fixed dark theme keeps the list readable
        // over whatever the app theme is.
        {
            _pathListHost = StackPanel{};
            _pathListHost.Spacing(0);

            auto sv = ScrollViewer{};
            sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            sv.MaxHeight(380); // seed only: _LayoutPathPicker re-caps it at every open + window resize (up to 90% of the window height)
            sv.AllowFocusOnInteraction(false); // scrollbar drags mustn't steal focus from the box either
            sv.IsTabStop(false); // ...and the scroll viewer must never become the focused element itself
            sv.Content(_pathListHost);
            _pathScroll = sv; // kept so _LayoutPathPicker can re-cap the list's height against the window

            _pathPanelBorder = Border{};
            _pathPanelBorder.Background(Fill(0xFF, 0x20, 0x20, 0x20));
            _pathPanelBorder.BorderBrush(Fill(0x90, 0x80, 0x80, 0x80));
            _pathPanelBorder.BorderThickness(Thickness{ 1, 1, 1, 1 });
            _pathPanelBorder.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
            _pathPanelBorder.Padding(Thickness{ 4, 4, 4, 6 });
            _pathPanelBorder.Width(380);
            _pathPanelBorder.RequestedTheme(ElementTheme::Dark);
            // The rows are Buttons: give the panel the Agent Manager's Button state brushes itself. On the Manager page
            // the rows found them on its root (the Popup's parent), so these are the same values; a host that parents
            // the drop-down elsewhere (the side panel's card: the page Root(), which carries no seeds) would otherwise
            // get null hover / press brushes under the forced-Dark theme — rows that flash empty on hover and take a
            // click only on their text.
            SeedButtonStateBrushes(_pathPanelBorder.Resources());
            _pathPanelBorder.Child(sv);

            _pathPopup = winrt::Windows::UI::Xaml::Controls::Primitives::Popup{};
            _pathPopup.HorizontalAlignment(HorizontalAlignment::Left);
            _pathPopup.VerticalAlignment(VerticalAlignment::Top);
            _pathPopup.Child(_pathPanelBorder);
            if (_host.popupHost)
            {
                Grid::SetRow(_pathPopup, 0);
                if (_host.popupRowSpan > 1)
                {
                    Grid::SetRowSpan(_pathPopup, _host.popupRowSpan); // the page Root(): the whole grid (tab row + infobars + content)
                }
                _host.popupHost.Children().Append(_pathPopup);
            }
        }

        _ValidateLaunchBox(); // initial state for the seeded cwd (USERPROFILE -> neutral, enabled)
    }

    // ---- Public operations -----------------------------------------------------------------------------------------

    void AgentLaunchBar::ValidateLaunchBox()
    {
        _ValidateLaunchBox();
    }

    void AgentLaunchBar::UpdateLaunchAgentButton()
    {
        _UpdateLaunchAgentButton();
    }

    void AgentLaunchBar::ClickLaunch()
    {
        if (_launchHoverTimer)
        {
            _launchHoverTimer.Stop(); // a click is a decision: the hover menu must not pop after it
        }
        // ...nor stay up over the launch: a mouse hover opens it at once, so it is normally open when the button is
        // clicked. The click's own press closes a transient menu as well; this covers the keyboard path (the side
        // panel's key-up Enter on a focused Launch) and a touch whose press arrived together with the menu's open.
        _CloseLaunchMenu();
        _OnLaunch();
    }

    void AgentLaunchBar::ClickFork()
    {
        _OnForkFromBox();
    }

    void AgentLaunchBar::ClosePathPicker()
    {
        _ClosePathPicker();
    }

    bool AgentLaunchBar::PathPickerOpen() const noexcept
    {
        try
        {
            return _pathPopup && _pathPopup.IsOpen();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar::PathPickerOpen"); // a torn-down popup reads as closed
            return false;
        }
    }

    void AgentLaunchBar::QueuePathPickerRelayout()
    {
        _QueuePathPickerRelayout();
    }

    void AgentLaunchBar::ReanchorPathPickerAfterLayout() noexcept
    {
        // A fresh budget for this request: the box moved, so the rounds of an earlier open or move say nothing about it.
        _pathPickerReanchorRounds = 0;
        _ArmPathPickerReanchor(); // noexcept; a no-op while the drop-down is closed
    }

    void AgentLaunchBar::_WriteBoxTextProgrammatically(const std::wstring& text)
    {
        // Recorded BEFORE the write: TextChanged is asynchronous, but if it were ever raised inside the write, the
        // handler must already see the mark.
        _programmaticTextPending = text;
        _cwdBox.Text(winrt::hstring{ text });
    }

    void AgentLaunchBar::DismissForHide() noexcept
    {
        try
        {
            // The blur path's three steps (the box's LostFocus tick), run NOW: the host is about to take the box off
            // the screen and reads its text right after, so the normalize must not wait for the deferred tick. The
            // latch keeps the normalize's TextChanged from reopening the drop-down; the next focus or tap clears it.
            _pathPickerUserDismissed = true;
            _ClosePathPicker();
            _NormalizeCwdBox();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar::DismissForHide");
        }
    }

    std::wstring AgentLaunchBar::BoxText() const
    {
        return _cwdBox ? std::wstring{ _cwdBox.Text() } : std::wstring{};
    }

    void AgentLaunchBar::SetBoxText(const std::wstring& text)
    {
        if (!_cwdBox)
        {
            return;
        }
        if (std::wstring{ _cwdBox.Text() } != text)
        {
            // A host write, not typing: its TextChanged (asynchronous — it can come after the host focuses the box)
            // must not open the drop-down. The side panel's card copies the Agent Manager's text here, then opens and
            // focuses its box.
            _WriteBoxTextProgrammatically(text);
            _cwdBox.Select(static_cast<int32_t>(text.size()), 0); // caret to end
        }
        // Re-validate explicitly: the box may be off the live tree (a hidden Manager page, a closed card), where
        // TextChanged does not fire reliably — the underline and buttons would keep the previous text's state.
        _ValidateLaunchBox();
    }

    void AgentLaunchBar::SeedButtonStateBrushes(const ResourceDictionary& resources)
    {
        if (!resources)
        {
            return;
        }
        // The Agent Manager root's values (AgentManagerContent's constructor): a near-invisible rest fill (alpha 0x01 —
        // invisible, yet hit-testable), a subtle hover lift, a touch more on press, light text on the dark UI.
        const auto seed = [&resources](const wchar_t* key, const SolidColorBrush& brush) {
            const auto boxedKey = winrt::box_value(key);
            if (!resources.HasKey(boxedKey))
            {
                resources.Insert(boxedKey, brush); // a key the host already seeded keeps the host's brush
            }
        };
        seed(L"ButtonBackground", Fill(0x01, 0xFF, 0xFF, 0xFF));
        seed(L"ButtonBackgroundPointerOver", Fill(0x22, 0xFF, 0xFF, 0xFF));
        seed(L"ButtonBackgroundPressed", Fill(0x33, 0xFF, 0xFF, 0xFF));
        seed(L"ButtonForeground", Fill(0xFF, 0xE6, 0xE6, 0xE6));
        seed(L"ButtonForegroundPointerOver", Fill(0xFF, 0xFF, 0xFF, 0xFF));
        seed(L"ButtonForegroundPressed", Fill(0xFF, 0xFF, 0xFF, 0xFF));
    }

    void AgentLaunchBar::Shutdown() noexcept
    {
        if (_shutDown)
        {
            return;
        }
        _shutDown = true;
        if (_launchHoverTimer)
        {
            try
            {
                _launchHoverTimer.Stop(); // PRESETS: the Launch button's hover-intent delay (its Tick holds only a weak ref)
            }
            CATCH_LOG(); // a teardown off the UI thread throws RPC_E_WRONG_THREAD here — never let it escape a destructor
        }
        // An open Launch hover menu must not outlive the bar either (a pick would launch through a host that is going
        // away). Noexcept and guarded inside, for the same off-thread teardown exposure.
        _CloseLaunchMenu();
        // Nor a drop-down re-anchor hook: it is a LayoutUpdated registration, called after every layout pass of the UI
        // thread while it lives. Noexcept (the revoker ignores the remove's result, also off the UI thread).
        _DisarmPathPickerReanchor();
        if (_pathPopup)
        {
            try
            {
                _pathPopup.IsOpen(false); // an open drop-down must not outlive the bar that fills it
            }
            CATCH_LOG(); // the same off-thread teardown exposure as the timer above
        }
    }

    // ---- Host access -----------------------------------------------------------------------------------------------

    std::shared_ptr<::Agentmaster::SessionRegistry> AgentLaunchBar::_Registry() const noexcept
    {
        try
        {
            return _host.registry ? _host.registry() : nullptr;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar registry getter"); // answers null: the transcript fallback still resolves an id
            return nullptr;
        }
    }

    uint32_t AgentLaunchBar::_RecentDirsLimitSetting() const noexcept
    {
        try
        {
            return _host.recentDirsLimit ? _host.recentDirsLimit() : 0u;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar recentDirsLimit getter"); // 0 => the shipped 10
            return 0u;
        }
    }

    std::wstring AgentLaunchBar::_LaunchModelsSetting() const noexcept
    {
        try
        {
            return _host.launchModels ? _host.launchModels() : std::wstring{};
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar launchModels getter"); // "" => Default + Specify… + presets only
            return {};
        }
    }

    void AgentLaunchBar::_ClaudeMissing() noexcept
    {
        try
        {
            if (_host.claudeMissing)
            {
                _host.claudeMissing();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar claudeMissing"); // the launch was refused either way
        }
    }

    void AgentLaunchBar::_NotifyLayoutChanged() noexcept
    {
        try
        {
            if (_host.layoutChanged)
            {
                _host.layoutChanged();
            }
        }
        catch (...)
        {
            // A failed reflow keeps the row's previous layout; it runs again at the next size / content change.
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar layoutChanged");
        }
    }

    void AgentLaunchBar::_NotifyLaunched() noexcept
    {
        try
        {
            if (_host.launched)
            {
                _host.launched();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar launched"); // the launch itself already ran
        }
    }

    void AgentLaunchBar::_NotifyTextEdited() noexcept
    {
        try
        {
            if (_host.textEdited && _cwdBox)
            {
                _host.textEdited(std::wstring{ _cwdBox.Text() });
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar textEdited"); // the next edit mirrors again
        }
    }

    // ---- The agent element + the launch actions (moved from AgentManagerContent.Launch.cpp) -------------------------

    // Agentmaster (Codex-launch): repaint the launch-bar agent toggle from _launchCodex. Codex wears the
    // SAME teal (0xFF4EC9B0) as every other codex surface (the EXTERNAL pill, the managed Board/tree pill),
    // so the agent reads one color everywhere; Claude is a neutral blue accent (the default, unchanged).
    // A Claude-only host's chip gets the same content (it is never Codex).
    void AgentLaunchBar::_UpdateLaunchAgentButton()
    {
        if (!_launchAgentBtn && !_agentChip)
        {
            return;
        }
        const bool codex = _launchCodex;
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(6);
        auto g = Text(L"\x25CF", 11, true, 1.0); // ●
        g.Foreground(SolidColorBrush{ codex ? Color{ 0xFF, 0x4E, 0xC9, 0xB0 } : Color{ 0xFF, 0x4F, 0x9C, 0xFF } });
        row.Children().Append(g);
        auto label = Text(codex ? L"Codex" : L"Claude", 11, false, 0.95);
        if (_launchAgentBtn)
        {
            row.Children().Append(label); // the toggle's text takes the Button's foreground (the seeded ButtonForeground)
            _launchAgentBtn.Content(row);
        }
        else
        {
            // The chip is a Border, which passes no foreground down: give the label the toggle's own text color (the
            // Agent Manager's ButtonForeground, #E6E6E6), so "● Claude" reads the same on both surfaces.
            label.Foreground(Fill(0xFF, 0xE6, 0xE6, 0xE6));
            row.Children().Append(label);
            _agentChip.Child(row);
        }
        _NotifyLayoutChanged(); // the toggle width changed (Claude<->Codex) -> the host re-fits the row
    }

    void AgentLaunchBar::_ShowLaunchMenu()
    {
        // Offered only while the button starts a NEW Claude session: Codex has no presets yet, a session id
        // in the box means "Resume" (that session keeps its own snapshot), and a disabled button (a red box)
        // must not be bypassed through its menu. Never after Shutdown: the hover opens this straight from
        // PointerEntered now, and an element can still be under the pointer while its host goes away.
        if (_shutDown || !_launchBtn || !_cwdBox || _launchMenu || _launchCodex || !_launchBtn.IsEnabled())
        {
            return;
        }
        if (LooksLikeSessionId(std::wstring{ _cwdBox.Text() }))
        {
            return;
        }
        try
        {
            MenuFlyout menu;
            const auto weak = weak_from_this();
            // PRESETS: the "Add presets…" item takes the presets' place while the user has none — the same shared
            // question every other model menu's host asks (ShouldOfferAddPresets), asked again at each open because a
            // Settings Save between two hovers can change the answer.
            AgentAddPresetsOpener addPresets;
            if (_host.addPresets && ::Agentmaster::ShouldOfferAddPresets())
            {
                addPresets = _host.addPresets;
            }
            AgentFillModelPickItems(
                menu.Items(),
                ::Agentmaster::LaunchMenuEntries(_LaunchModelsSetting()),
                [weak](winrt::hstring target) {
                    if (auto self = weak.lock())
                    {
                        self->_OnLaunch(std::wstring{ target });
                    }
                },
                _host.specifyModel,
                addPresets);
            // Forget the menu when it closes, so the next hover can open a fresh one (the list may have
            // changed: a Settings Save adds or renames presets).
            menu.Closed([weak](const IInspectable&, const IInspectable&) {
                if (auto self = weak.lock())
                {
                    self->_launchMenu = nullptr;
                }
            });
            _launchMenu = menu;
            Primitives::FlyoutShowOptions opts;
            opts.Placement(Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
            // A hover menu: it does not take focus and closes when the pointer leaves both the button and
            // the menu, so it never traps a user who only passed by.
            opts.ShowMode(Primitives::FlyoutShowMode::TransientWithDismissOnPointerMoveAway);
            menu.ShowAt(_launchBtn, opts);
        }
        catch (...)
        {
            _launchMenu = nullptr; // a failed open must not block every later hover
            ::Agentmaster::AgentLogCaughtException(L"_ShowLaunchMenu");
        }
    }

    void AgentLaunchBar::_CloseLaunchMenu() noexcept
    {
        try
        {
            if (_launchMenu)
            {
                // Hide is asynchronous: the menu's Closed handler forgets it (_launchMenu = nullptr) a moment later. Until
                // then a hover finds _launchMenu set and stacks no second menu. A menu that is already closing is a no-op.
                _launchMenu.Hide();
            }
        }
        catch (...)
        {
            // A torn-down menu, or Shutdown on a pool thread (RPC_E_WRONG_THREAD): the menu goes away with its window
            // then. Recorded (Rule #18), never thrown: Shutdown runs from a destructor.
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar: close the Launch hover menu");
        }
    }

    void AgentLaunchBar::_OnLaunch(const std::wstring& target)
    {
        if (!_cwdBox)
        {
            return;
        }
        _NormalizeCwdBox(); // launch with — and remember — a normalized path (a session id is unaffected)
        std::wstring text{ _cwdBox.Text() };
        { // trim surrounding whitespace so the launch target matches exactly what _ValidateLaunchBox judged
            const auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
            while (!text.empty() && isws(text.front())) { text.erase(text.begin()); }
            while (!text.empty() && isws(text.back())) { text.pop_back(); }
        }
        // Agentmaster (Codex-launch): a Codex launch is DIRECTORY-ONLY — codex has no typed-id resume/fork
        // here (Codex resume is reached via the Archive page / window-restore / EXTERNAL Adopt), and while
        // Codex is selected _ValidateLaunchBox keeps the box in directory semantics. Spawn a managed codex in
        // that dir, mirroring the EXTERNAL menu's "Open New Codex Session Here" (pid 0 = no source external,
        // adopt=false = a fresh independent session). _LaunchCodexSession registers the card immediately.
        // (A Claude-only host never sets _launchCodex, so this branch is the toggle hosts' alone.)
        if (_launchCodex)
        {
            if (_host.spawnCodex)
            {
                if (!_EnsureLaunchDirExists(text)) // "Create & Launch": make the folder first if it doesn't exist yet
                {
                    return; // creation failed (already warned) — don't launch into a missing dir
                }
                _PushRecentDir(text); // remember it as "recently selected" (shared MRU with Claude launches)
                _ClosePathPicker();
                _host.spawnCodex(text); // fresh launch: adopt=false, fork=false
                _NotifyLaunched();
            }
            return;
        }
        // Native-exe-only policy: every CLAUDE interaction (new session or resume) needs a native
        // claude.exe. With none detected, show the install/Browse notice instead of launching. (Codex
        // launches above are a separate runtime and are not gated on claude.exe.)
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _ClaudeMissing();
            return;
        }
        // A session id in the box = resume that conversation. The button reads "Resume session" and
        // is only ENABLED when the id was FOUND (_ValidateLaunchBox), so this path is reachable only
        // for a real on-disk transcript; resolve its dir/title and hand off to the host.
        if (const auto sid = LooksLikeSessionId(text))
        {
            std::wstring dir, title;
            if (_host.resumeSession && _ResolveSessionDirTitle(*sid, dir, title))
            {
                _ClosePathPicker();
                _host.resumeSession(*sid, dir, title);
                _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after resume
                _ValidateLaunchBox(); // re-validate explicitly: the resume handler just opened+selected a new tab, so the host is detached and TextChanged won't reliably fire to reset the underline/buttons
                _NotifyLaunched();
            }
            return;
        }
        // A working directory = a new, independent session there.
        if (_host.spawnClaude)
        {
            if (!_EnsureLaunchDirExists(text)) // "Create & Launch Claude": make the folder first if it doesn't exist yet
            {
                return; // creation failed (already warned) — don't launch into a missing dir
            }
            _PushRecentDir(text); // remember it as "recently selected"
            _ClosePathPicker();
            // `target`: "" = Default (a plain click — the settings model); from the hover menu, a model id or
            // "@<presetId>" (PRESETS), which the page snapshots into the new session's launchArgs.
            _host.spawnClaude(text, target);
            _NotifyLaunched();
        }
    }

    // Agentmaster: ensure the launch target directory exists, creating it (and any missing parents)
    // when the user typed a not-yet-existing absolute path — the "Create & Launch" affordance. The
    // launch button only reads "Create & Launch …" (and only enables for a missing path) when
    // _ValidateLaunchBox judged the path creatable (LooksLikeCreatableDir), so this is the matching
    // commit step. Returns true if the dir exists (already, or after a successful create) and the
    // launch may proceed; false if creation failed (a buttons-only error is shown and the caller
    // aborts). An already-existing dir is a no-op pass-through; a non-creatable path can't reach here
    // (the button is disabled for it) but is rejected defensively.
    bool AgentLaunchBar::_EnsureLaunchDirExists(const std::wstring& dir)
    {
        const std::wstring norm = NormPath(dir);
        if (IsDir(norm))
        {
            return true; // already there — nothing to create
        }
        if (!LooksLikeCreatableDir(norm))
        {
            return false; // not an absolute path we offered to create (button is disabled for this)
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ norm }, ec);
        if (ec || !IsDir(norm))
        {
            // Surface the failure (permission denied, a missing drive, a path too long, …) instead of
            // silently doing nothing after the user clicked "Create & Launch". Buttons-only ContentDialog
            // (a text box inside one gets no keypresses in XAML Islands, but an info dialog needs none).
            ContentDialog dialog;
            dialog.Title(winrt::box_value(L"Couldn't create folder"));
            dialog.Content(winrt::box_value(winrt::hstring{ L"Could not create the working directory:\n\n" } + winrt::hstring{ norm }));
            dialog.CloseButtonText(L"OK");
            // The host's dialog anchor: the Agent Manager's forced-Dark root, or the side panel's forced-Dark card —
            // so the dialog lands on the right island and renders dark either way.
            if (const auto anchor = _host.dialogAnchor)
            {
                try
                {
                    dialog.XamlRoot(anchor.XamlRoot());
                    dialog.RequestedTheme(anchor.ActualTheme());
                }
                catch (...)
                {
                    // The dialog still shows, on the default root / theme; the failure is recorded (Rule #18).
                    ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar create-folder dialog anchor");
                }
            }
            try
            {
                dialog.ShowAsync();
            }
            CATCH_LOG(); // Agentmaster: a silently-swallowed ShowAsync = this "couldn't create folder" error dialog never appears, so the user sees the Launch just do nothing with no trace — log it
            return false;
        }
        return true;
    }

    // Agentmaster: the Fork button (shown only for a FOUND session id) -> fork that conversation
    // into a NEW one in the same dir (the page's _ForkSessionFromDisk: claude --resume --fork-session).
    void AgentLaunchBar::_OnForkFromBox()
    {
        if (!_cwdBox)
        {
            return;
        }
        // Native-exe-only policy: a fork is a claude launch -> requires a native claude.exe.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _ClaudeMissing();
            return;
        }
        const auto sid = LooksLikeSessionId(std::wstring{ _cwdBox.Text() });
        if (!sid) // the button is hidden for non-session-id input, but guard anyway
        {
            return;
        }
        std::wstring dir, title;
        if (_host.forkSession && _ResolveSessionDirTitle(*sid, dir, title))
        {
            _ClosePathPicker();
            _host.forkSession(*sid, dir, title);
            _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after fork
            _ValidateLaunchBox(); // re-validate explicitly: the fork handler just opened+selected a new tab, so the host is detached and TextChanged won't reliably fire to reset the underline/buttons
            _NotifyLaunched();
        }
    }

    // Agentmaster: paint the Launch box's validation underline + drive the launch/fork buttons.
    // EMPTY -> neutral, "Launch Claude" enabled (defaults). A UUID -> session id: FOUND = green +
    // "Resume session" enabled + Fork shown; NOT found = red + disabled. Otherwise a directory:
    // EXISTS = neutral + "Launch Claude" enabled; a not-yet-existing ABSOLUTE path = amber +
    // "Create & Launch Claude" enabled (the folder is created on launch); a malformed / relative path
    // = red + disabled. (Per the design: green is reserved for a found session id; a valid folder
    // stays neutral; amber flags a folder that will be created.)
    void AgentLaunchBar::_ValidateLaunchBox()
    {
        if (!_cwdBox || !_launchBtn)
        {
            return;
        }
        // Agentmaster (responsive launch bar): the launch button's text + the Fork button's visibility
        // change below (Launch Claude / Create & Launch Claude / Resume session / +Fork / the Codex twins),
        // which changes the launch-buttons group width — let the host re-fit its row on EVERY exit path.
        auto reflowOnExit = wil::scope_exit([this]() noexcept { _NotifyLayoutChanged(); });
        std::wstring trimmed{ _cwdBox.Text() };
        const auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (!trimmed.empty() && isws(trimmed.front()))
        {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && isws(trimmed.back()))
        {
            trimmed.pop_back();
        }

        const auto paint = [this](int state) { // 0 neutral (hidden), 1 green, 2 red, 3 amber (will create)
            if (!_cwdUnderline)
            {
                return;
            }
            // transparent (neutral) / green (found id) / red (missing dir or unknown id) / amber (a
            // not-yet-existing dir we'll CREATE on launch); kept always present (Height 2) so toggling
            // color never reflows the toolbar.
            const SolidColorBrush b = state == 1 ? Fill(0xFF, 0x4C, 0xAF, 0x50) :
                                      state == 2 ? Fill(0xFF, 0xE5, 0x39, 0x35) :
                                      state == 3 ? Fill(0xFF, 0xDA, 0xA5, 0x20) :
                                                   Fill(0x00, 0x00, 0x00, 0x00);
            _cwdUnderline.Background(b);
        };
        const auto showFork = [this](bool v) {
            if (_forkBtn)
            {
                _forkBtn.Visibility(v ? Visibility::Visible : Visibility::Collapsed);
            }
        };

        // Agentmaster (Codex-launch): while Codex is the selected agent the box is DIRECTORY-ONLY — codex
        // has no --session-id, so there is no typed-id resume (the green found-id state) and no Fork. Empty
        // or an existing dir => enabled "Launch Codex" (neutral underline); a not-yet-existing absolute path
        // => amber + "Create & Launch Codex"; a malformed / relative path => red + disabled.
        if (_launchCodex)
        {
            showFork(false);
            if (trimmed.empty())
            {
                paint(0);
                _launchBtn.IsEnabled(true);
                _launchBtn.Content(winrt::box_value(L"Launch Codex"));
            }
            else
            {
                // An existing dir launches as-is; a not-yet-existing absolute path flips to amber +
                // "Create & Launch Codex" (made on launch); a malformed/relative path stays red+disabled.
                const std::wstring norm = NormPath(trimmed);
                const bool exists = IsDir(norm);
                const bool creatable = !exists && LooksLikeCreatableDir(norm);
                paint(exists ? 0 : (creatable ? 3 : 2));
                _launchBtn.IsEnabled(exists || creatable);
                _launchBtn.Content(winrt::box_value(creatable ? L"Create & Launch Codex" : L"Launch Codex"));
            }
            return;
        }

        if (trimmed.empty())
        {
            paint(0);
            _launchBtn.IsEnabled(true);
            _launchBtn.Content(winrt::box_value(L"Launch Claude"));
            showFork(false);
            return;
        }
        if (const auto sid = LooksLikeSessionId(trimmed))
        {
            const bool found = ::Agentmaster::ClaudeConversationExists(*sid);
            paint(found ? 1 : 2);
            _launchBtn.IsEnabled(found);
            _launchBtn.Content(winrt::box_value(L"Resume session"));
            showFork(found);
            return;
        }
        // a working directory: green is reserved for session ids, so a valid dir stays neutral. A
        // not-yet-existing absolute path flips to amber + "Create & Launch Claude" (_OnLaunch makes the
        // folder first); a malformed / relative path that we won't create stays red + disabled.
        const std::wstring norm = NormPath(trimmed);
        const bool exists = IsDir(norm);
        const bool creatable = !exists && LooksLikeCreatableDir(norm);
        paint(exists ? 0 : (creatable ? 3 : 2));
        _launchBtn.IsEnabled(exists || creatable);
        _launchBtn.Content(winrt::box_value(creatable ? L"Create & Launch Claude" : L"Launch Claude"));
        showFork(false);
    }

    // Agentmaster: resolve a session id to its (working dir, title) for resume / fork. The registry
    // knows a managed (live or archived) session directly; otherwise read the cwd straight off the
    // on-disk transcript (the project FOLDER name is a lossy encoding, so read the line's real cwd).
    // Title is left empty in the transcript case — the resume/fork seam derives a smart name from the
    // dir. Returns false if no dir could be found (caller no-ops).
    bool AgentLaunchBar::_ResolveSessionDirTitle(const std::wstring& id, std::wstring& dir, std::wstring& title)
    {
        dir.clear();
        title.clear();
        if (const auto registry = _Registry())
        {
            if (const auto info = registry->Get(id); info && !info->workingDir.empty())
            {
                dir = info->workingDir;
                title = info->title;
                return true;
            }
        }
        const std::wstring path = ::Agentmaster::ResolveClaudeTranscriptPath(id);
        if (path.empty())
        {
            return false;
        }
        const auto facts = ::Agentmaster::ReadTranscriptQuickFacts(path, 0);
        dir = facts.cwd;
        return !dir.empty();
    }

    std::vector<std::wstring> AgentLaunchBar::_CollectRecentDirs(const std::wstring& current, const std::wstring& query) const
    {
        // The RECENT section length is a global setting (AppSettings::recentDirsLimit, default
        // 10); 0/garbage falls back to 10.
        const uint32_t limitSetting = _RecentDirsLimitSetting();
        const size_t limit = limitSetting > 0 ? limitSetting : 10;

        // Gather the full candidate pool first — recents (MRU) over live-session dirs — deduped,
        // with the box's own path excluded. In MRU mode we'd cap while gathering; in query mode
        // we rank the WHOLE pool before truncating, so the best matches surface even when they're
        // not the most recent. The pool is small (recents are capped at the same limit on save,
        // plus the live fleet), so gathering it whole is cheap.
        std::vector<std::wstring> pool;
        auto add = [&](const std::wstring& d) {
            if (d.empty())
            {
                return;
            }
            if (!current.empty() && PathEq(d, current))
            {
                return; // exclude the path that's currently in the box
            }
            for (const auto& e : pool)
            {
                if (PathEq(e, d))
                {
                    return; // dedup
                }
            }
            pool.push_back(d);
        };

        for (const auto& d : _recentDirs->dirs)
        {
            add(d);
        }
        // Supplement from live sessions (most-recently-active first) so the list is useful even
        // before anything has been launched this run. In query mode we need the WHOLE pool to
        // rank; in MRU mode we can stop (and skip the snapshot entirely) once it's full.
        const bool ranking = !query.empty();
        if (ranking || pool.size() < limit)
        {
            if (const auto registry = _Registry())
            {
                auto snap = registry->Snapshot();
                std::sort(snap.begin(), snap.end(), [](const SessionInfo& a, const SessionInfo& b) {
                    return a.lastActivityUnixMs > b.lastActivityUnixMs;
                });
                for (const auto& s : snap)
                {
                    add(s.workingDir);
                    if (!ranking && pool.size() >= limit)
                    {
                        break;
                    }
                }
            }
        }

        if (query.empty())
        {
            // Plain MRU order (recents first, then live-session dirs), capped to the limit — the
            // historical behavior when the box is empty or holds a rooted path.
            if (pool.size() > limit)
            {
                pool.resize(limit);
            }
            return pool;
        }

        // Query mode: a bare token was typed with no root path. Rank the pool by case-insensitive
        // fuzzy closeness to the token (Levenshtein approximate-substring distance over the whole
        // path — so a mid-path segment like "...\Agentmaster" matches "agent"), keep only the
        // genuine matches, and order them closest-first. The threshold allows roughly half the
        // token to differ, which keeps near-misses ("agen", "agnet") while dropping unrelated
        // recents; the gather order (MRU) is the stable tiebreaker so equally-close recents keep
        // their recency order. Very short tokens (<=2 chars) require an EXACT substring — one
        // allowed edit on a 2-char token would otherwise match almost anything — and the filter
        // tightens naturally as more characters are typed.
        const std::wstring q = ToLowerInvariant(query);
        const size_t threshold = q.size() <= 2 ? 0 : q.size() / 2;
        struct Scored
        {
            std::wstring dir;
            size_t dist;
            size_t order;
        };
        std::vector<Scored> scored;
        for (size_t i = 0; i < pool.size(); ++i)
        {
            const size_t dist = FuzzySubstringDistance(q, ToLowerInvariant(pool[i]));
            if (dist <= threshold)
            {
                scored.push_back({ pool[i], dist, i });
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
            if (a.dist != b.dist)
            {
                return a.dist < b.dist; // closest match topmost
            }
            return a.order < b.order; // ...then most-recent first
        });
        std::vector<std::wstring> out;
        for (const auto& s : scored)
        {
            if (out.size() >= limit)
            {
                break;
            }
            out.push_back(s.dir);
        }
        return out;
    }

    void AgentLaunchBar::_PushRecentDir(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return;
        }
        auto& recent = _recentDirs->dirs; // the window's ONE list: the other launch bar sees this push at once
        recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const std::wstring& e) { return PathEq(e, dir); }), recent.end());
        recent.insert(recent.begin(), dir);
        const uint32_t capSetting = _RecentDirsLimitSetting();
        const size_t cap = capSetting > 0 ? capSetting : 10;
        if (recent.size() > cap)
        {
            recent.resize(cap);
        }
        ::Agentmaster::SaveRecentDirs(recent);
    }

    // ---- The path-picker drop-down (moved from AgentManagerContent.Launch.cpp) --------------------------------------

    // Agentmaster: the "Browse…" row — the FIRST option in the path-picker dropdown. Visually it
    // mirrors _MakePathRow (a transparent, focus-neutral row) but its glyph is a folder icon and its
    // click opens the native folder dialog instead of drilling a typed path. Kept a TextBlock (not a
    // FontIcon) so its vertical rhythm matches the other rows' TextBlock glyphs; only the FontFamily
    // is swapped to Segoe Fluent Icons so the folder glyph renders.
    Button AgentLaunchBar::_MakeBrowseRow()
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        {
            auto glyph = Text(L"\xE8B7", 13, false, 0.9); // Segoe Fluent Icons: Folder
            glyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            row.Children().Append(glyph);
        }
        row.Children().Append(Text(L"Browse\x2026", 13, true, 1.0)); // bold: this is the primary "pick a folder" action

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        // Like the path rows: must NOT take focus from the cwd box — a row grabbing focus would let
        // the box's LostFocus race ahead and tear down the popup (and this button) before the click
        // registers. Keeping focus on the box also keeps the popup open across picks.
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        AgentSetTip(btn, L"Browse for a working directory \x2014 the pick fills the box and is added to recents.");
        btn.Click([weak = weak_from_this()](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weak.lock())
            {
                self->_BrowseForLaunchDir();
            }
        });
        return btn;
    }

    // Agentmaster: open the native folder dialog for the Launch path box ("Browse…" row). A pick is
    // dropped into the box AND pushed onto the recent-dirs MRU immediately (so Browse picks are part
    // of recents even before the session is launched), then the box is re-validated + refocused. The
    // COM modal is deferred off the click tick (it needs the message pump — the same XAML-Islands rule
    // the claude.exe Browse and the profile picker both follow).
    void AgentLaunchBar::_BrowseForLaunchDir()
    {
        if (!_dispatcher || !_cwdBox)
        {
            return;
        }
        // Seed the dialog at the box's current path when it points at a real folder (or, for a partial
        // leaf being typed, that leaf's existing parent), captured before the async hop.
        std::wstring seed;
        {
            const std::wstring norm = NormPath(std::wstring{ _cwdBox.Text() });
            if (IsDir(norm))
            {
                seed = norm;
            }
            else if (const auto parent = ParentDir(norm); parent && IsDir(*parent))
            {
                seed = *parent;
            }
        }
        _dispatcher.TryEnqueue([weak = weak_from_this(), seed]() {
            const auto self = weak.lock();
            if (!self)
            {
                return; // the host went away before the deferred tick ran
            }
            const auto picked = PickFolder(::GetActiveWindow(), seed);
            if (!picked || picked->empty() || !self->_cwdBox)
            {
                return;
            }
            const std::wstring dir = NormPath(*picked);
            self->_PushRecentDir(dir); // a Browse pick joins the recents right away (shared MRU with launches)
            // Set the launch target (fires TextChanged -> _ValidateLaunchBox) as a PROGRAMMATIC write: its TextChanged
            // re-lists an open list but does not reopen a closed one — "leave a closed one closed", below.
            self->_WriteBoxTextProgrammatically(dir);
            self->_cwdBox.Select(static_cast<int32_t>(dir.size()), 0); // caret to end
            self->_ValidateLaunchBox(); // explicit: a dismissed picker can early-out of the TextChanged path
            // Keep the box focused (don't strand focus on the dialog). A programmatic focus: GotFocus reads it as such
            // (GettingFocus's input mode), so it does not reopen a closed list either.
            self->_cwdBox.Focus(FocusState::Programmatic);
            // Refresh the open picker so the just-added recent shows; leave a closed one closed (the
            // user made a definitive choice via the modal).
            if (self->_pathPopup && self->_pathPopup.IsOpen())
            {
                self->_RebuildPathPicker();
            }
        });
    }

    Button AgentLaunchBar::_MakePathRow(const std::wstring& fullPath, const winrt::hstring& glyph, const winrt::hstring& displayText, const std::wstring& branch)
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(Text(glyph, 13, false, 0.7));
        // Recents show their full path (displayText empty); folders show just the leaf —
        // the section header already states which directory they live in.
        row.Children().Append(Text(displayText.empty() ? winrt::hstring{ fullPath } : displayText, 13, false, 1.0));
        // Agentmaster: a non-empty `branch` (the RECENT rows pass it, via ReadGitBranchForDir) appends
        // "— <branch>" at the end — the GIT WORKTREES row's "<path> — <branch>" idiom — so a recent dir
        // that's a git repo shows the branch it's on. Empty (folders / non-repos) renders nothing.
        if (!branch.empty())
        {
            row.Children().Append(Text(L"\x2014", 13, false, 0.35));
            row.Children().Append(Text(winrt::hstring{ branch }, 13, false, 0.7));
        }

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        // The list is click-only and must NOT take focus from the cwd box. If a row grabbed
        // focus, the box's LostFocus could race ahead of this Click and tear down the popup
        // (and this very button) before the pick registers — so clicking a row would appear
        // to do nothing. Keeping focus on the box also keeps the popup open across picks.
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        const auto captured = fullPath;
        AgentSetTip(btn, winrt::hstring{ L"Use this folder \x2014 " } + winrt::hstring{ fullPath });
        btn.Click([weak = weak_from_this(), captured](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weak.lock())
            {
                self->_PickPath(captured);
            }
        });
        return btn;
    }

    // Agentmaster: a "GIT WORKTREES" row — "<name> — <path> — <branch>" (branch also in the tooltip).
    // Mirrors _MakePathRow (a transparent, focus-neutral row that must NOT steal focus from the cwd
    // box — or the box's LostFocus would race ahead and tear down the popup before the click lands)
    // and, like it, a click drills the launch box into the worktree path via _PickPath, so a session
    // launches THERE. The leaf name is the worktree's identity; the path follows dim so the launch
    // destination is unambiguous (worktrees of one repo share a folder, differ by branch).
    Button AgentLaunchBar::_MakeWorktreeRow(const std::wstring& fullPath, const std::wstring& name, const std::wstring& branch, bool isCurrent)
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        {
            auto glyph = Text(L"\xF1D3", 13, false, isCurrent ? 0.9 : 0.7); // Segoe Fluent Icons: BranchFork2
            glyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            row.Children().Append(glyph);
        }
        row.Children().Append(Text(winrt::hstring{ name }, 13, true, 1.0));
        row.Children().Append(Text(L"\x2014", 13, false, 0.35)); // em-dash separator: "<name> — <path>"
        row.Children().Append(Text(winrt::hstring{ fullPath }, 13, false, 0.55));
        if (!branch.empty())
        {
            row.Children().Append(Text(L"\x2014", 13, false, 0.35)); // second separator: "<path> — <branch>"
            row.Children().Append(Text(winrt::hstring{ branch }, 13, false, 0.7)); // the worktree's checked-out branch, at the end (a touch brighter than the path)
        }
        if (isCurrent)
        {
            row.Children().Append(Text(L"(current)", 11, false, 0.45)); // the worktree the box already points into
        }

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        const auto captured = fullPath;
        const winrt::hstring tip = branch.empty() ?
            (winrt::hstring{ L"Use this worktree \x2014 " } + winrt::hstring{ fullPath }) :
            (winrt::hstring{ L"Worktree on branch " } + winrt::hstring{ branch } + winrt::hstring{ L" \x2014 " } + winrt::hstring{ fullPath });
        AgentSetTip(btn, tip);
        btn.Click([weak = weak_from_this(), captured](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weak.lock())
            {
                self->_PickPath(captured);
            }
        });
        return btn;
    }

    void AgentLaunchBar::_RebuildPathPicker()
    {
        if (!_pathListHost)
        {
            return;
        }
        _pathListHost.Children().Clear();

        // Agentmaster: "Browse…" is always the FIRST option — a native folder dialog whose pick fills
        // the box and joins the recents. Appended before any section so it sits at the top in every
        // mode (empty box, fuzzy query, or a rooted path). The empty-state hint below keys off this
        // baseline so it still shows when Browse is the ONLY row.
        _pathListHost.Children().Append(_MakeBrowseRow());
        const uint32_t browseRowCount = _pathListHost.Children().Size();

        auto sectionLabel = [](const winrt::hstring& s) {
            auto lbl = Text(s, 11, true, 0.5);
            lbl.Margin(Thickness{ 6, 8, 6, 2 });
            return lbl;
        };

        const std::wstring current = _cwdBox ? std::wstring{ _cwdBox.Text() } : std::wstring{};

        // "No root path" = a bare token typed with no path anchor: no separator AND not drive-
        // qualified ("agent", not "K:\..." / "K:" / "/home" / "\\server"). In that case the box
        // text can't anchor a subfolder listing, so instead of the (useless) "(path not found)"
        // we treat the token as a fuzzy QUERY over the recent directories — matched case-
        // insensitively and ranked by Levenshtein closeness (closest first). A session id is a
        // bare token too but is never a directory query, so it stays in plain MRU mode.
        auto looksRooted = [](const std::wstring& s) {
            if (s.find_first_of(L"\\/") != std::wstring::npos)
            {
                return true; // has a path separator
            }
            if (s.size() >= 2 && s[1] == L':' &&
                ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')))
            {
                return true; // drive-qualified ("K:" / "K:foo")
            }
            return false;
        };
        const bool queryMode = !current.empty() && !looksRooted(current) && !LooksLikeSessionId(current).has_value();

        // RECENT (up to AppSettings::recentDirsLimit, current excluded). In query mode the section
        // is filtered + ranked by closeness to the typed token; otherwise it's plain MRU order.
        const auto recents = _CollectRecentDirs(current, queryMode ? current : std::wstring{});
        if (!recents.empty())
        {
            _pathListHost.Children().Append(sectionLabel(queryMode ?
                (winrt::hstring{ L"RECENT MATCHES FOR \"" } + winrt::hstring{ current } + winrt::hstring{ L"\"" }) :
                winrt::hstring{ L"RECENT" }));
            for (const auto& d : recents)
            {
                _pathListHost.Children().Append(_MakePathRow(d, L"\x21BB", winrt::hstring{}, ReadGitBranchForDir(d))); // Agentmaster: show the recent dir's git branch at the end, like the worktree rows
            }
        }

        // GIT WORKTREES of the repo containing the typed path (if any). ListGitWorktrees mirrors
        // ReadGitBranchForDir's pure-filesystem .git walk (a few small reads + one dir enum), so it is
        // cheap enough for this per-keystroke rebuild. Shown only when the path resolves into a repo
        // that has LINKED worktrees (size >= 2: the main worktree + at least one linked) — a plain repo
        // with no extra worktrees adds no section. Clicking a row drills the launch box into that
        // worktree exactly like picking a folder, so a session launches there.
        if (!current.empty() && !queryMode)
        {
            const auto worktrees = ListGitWorktrees(NormPath(current));
            if (worktrees.size() >= 2)
            {
                _pathListHost.Children().Append(sectionLabel(winrt::hstring{ L"GIT WORKTREES" }));
                for (const auto& w : worktrees)
                {
                    _pathListHost.Children().Append(_MakeWorktreeRow(w.path, w.name, w.branch, w.isCurrent));
                }
            }
        }

        // SUBFOLDERS of the current path (+ a parent up-nav).
        //
        // Agentmaster: typing a partial leaf FILTERS. The box text splits at its LAST separator
        // into (listDir, leaf):
        //   - a path ending in a separator ("K:\source\") has an empty leaf => list ALL of
        //     listDir, unfiltered;
        //   - a partial leaf ("K:\source\Agent") lists listDir's folders whose name STARTS WITH
        //     the leaf ("Agent*"), case-insensitively, with the EXACT match hoisted to the top.
        // When the WHOLE text is itself an existing directory ("K:\source", no trailing
        // separator), a SECOND section below ALSO lists that directory's own subfolders — so
        // "K:\source" shows the K:\source* siblings (above) AND everything inside K:\source.
        // (Clicking any folder row appends the separator, drilling into it.)
        //
        // Skipped in query mode: a bare token (no root path) has no directory to enumerate, so
        // this would only ever produce "(path not found)" — the RECENT MATCHES section above is
        // the whole answer there.
        if (!current.empty() && !queryMode)
        {
            std::wstring listDir = current; // the directory whose subfolders we enumerate
            std::wstring filter; // leaf prefix to match; empty => list everything
            const wchar_t lastCh = current.back();
            const bool endsSep = (lastCh == L'\\' || lastCh == L'/');
            if (!endsSep)
            {
                if (const auto pos = current.find_last_of(L"\\/"); pos != std::wstring::npos)
                {
                    filter = current.substr(pos + 1); // the leaf being typed
                    listDir = current.substr(0, pos + 1); // its parent (keep the trailing sep)
                }
                // else: a bare relative token with no separator — there's no parent in the box
                // to anchor a listing to, so fall through and list `current` itself (no filter).
            }

            // Append `dir`'s subfolders, optionally keeping only names that start with `filt`
            // (case-insensitive) and hoisting an exact match to the front (a directory's leaf is
            // unique within its parent, so at most one); the rest stay newest-modified-first.
            // Returns how many folder rows were appended.
            auto appendFolderRows = [&](const std::wstring& dir, const std::wstring& filt) -> size_t {
                auto subs = EnumSubdirs(dir);
                if (!filt.empty())
                {
                    std::wstring exact;
                    std::vector<std::wstring> matched;
                    for (auto& leaf : subs)
                    {
                        if (!LeafStartsWith(leaf, filt))
                        {
                            continue;
                        }
                        if (exact.empty() && LeafEquals(leaf, filt))
                        {
                            exact = std::move(leaf);
                        }
                        else
                        {
                            matched.push_back(std::move(leaf));
                        }
                    }
                    subs.clear();
                    if (!exact.empty())
                    {
                        subs.push_back(std::move(exact));
                    }
                    for (auto& m : matched)
                    {
                        subs.push_back(std::move(m));
                    }
                }
                for (const auto& leaf : subs)
                {
                    _pathListHost.Children().Append(_MakePathRow(JoinDir(dir, leaf), L"\x25B8", winrt::hstring{ leaf }));
                }
                return subs.size();
            };

            // Section A — matches in listDir (the typed leaf's siblings, or — with no leaf — the
            // whole directory). Carries the parent up-nav (one level above listDir).
            {
                const winrt::hstring hdr = filter.empty() ?
                    (winrt::hstring{ L"SUBFOLDERS OF " } + winrt::hstring{ listDir }) :
                    (winrt::hstring{ L"MATCHES FOR \"" } + winrt::hstring{ filter } + winrt::hstring{ L"\" IN " } + winrt::hstring{ listDir });
                _pathListHost.Children().Append(sectionLabel(hdr));
            }
            if (const auto parent = ParentDir(listDir))
            {
                _pathListHost.Children().Append(_MakePathRow(*parent, L"\x2191", winrt::hstring{ L".. (parent)" }));
            }
            if (IsDir(listDir))
            {
                if (appendFolderRows(listDir, filter) == 0)
                {
                    _pathListHost.Children().Append(Text(filter.empty() ? L"(no subfolders)" : L"(no matches)", 12, false, 0.5));
                }
            }
            else
            {
                _pathListHost.Children().Append(Text(L"(path not found)", 12, false, 0.5));
            }

            // Section B — when the WHOLE text names an existing directory (and a leaf was matched
            // above), ALSO list everything INSIDE it, below the sibling matches.
            if (!endsSep && !filter.empty() && IsDir(current))
            {
                _pathListHost.Children().Append(sectionLabel(winrt::hstring{ L"SUBFOLDERS OF " } + winrt::hstring{ current }));
                if (appendFolderRows(NormPath(current), std::wstring{}) == 0)
                {
                    _pathListHost.Children().Append(Text(L"(no subfolders)", 12, false, 0.5));
                }
            }
        }

        if (_pathListHost.Children().Size() == browseRowCount)
        {
            // Only the Browse row is present (no recents / subfolders). In query mode that means the
            // token matched no recent directory; otherwise it's the initial empty-box hint.
            _pathListHost.Children().Append(Text(queryMode ?
                L"No recent directory matches — type a full path, or Browse\x2026 above." :
                L"Type a path, pick a recent directory, or Browse\x2026 above.",
                12, false, 0.6));
        }
    }

    void AgentLaunchBar::_OpenPathPicker()
    {
        if (!_pathPopup || !_cwdBox)
        {
            return;
        }
        _pathPickerUserDismissed = false; // opening clears the dismiss latch
        _LayoutPathPicker(); // anchor under the box + size against the window (the width rule + the 90%-of-height cap)
        _RebuildPathPicker();
        _pathPopup.IsOpen(true);
        // ...and once more after the next layout pass, where the box really is then: the anchor above read the box's
        // LAST layout, and a box that just moved is not there yet — a card whose Popup offsets were just written (its
        // child moves only in the next pass), or one never laid out at all (the window's first "+"). A list anchored to
        // that stale place covered the box and Launch (2026-10-12). The re-anchor writes only on a real move.
        _pathPickerReanchorRounds = 0;
        _ArmPathPickerReanchor();
    }

    // Agentmaster: the drop-down's geometry (contract on the declaration). Three independent steps —
    // anchor, width, height cap — each guarded on its own, so a failed read keeps that step's previous
    // value without skipping the others. Runs at every open and on a (deferred) window resize while open.
    bool AgentLaunchBar::_LayoutPathPicker(double* dx, double* dy)
    {
        if (dx)
        {
            *dx = 0.0;
        }
        if (dy)
        {
            *dy = 0.0;
        }
        bool moved = false;
        if (!_pathPopup || !_cwdBox)
        {
            return moved;
        }
        // The host panel the drop-down is parented into: its coordinate space is the offsets' (top-left aligned,
        // grid row 0), and its width is the "window" the width rule measures against.
        const auto layoutRoot = _host.popupHost;
        // The gap between the box's bottom edge and the drop-down's top edge. Shared by the anchor (1)
        // and the height cap (3): the cap must measure the room below from where the panel really starts.
        constexpr double kAnchorGapDips = 2.0;

        // 1. Anchor the popup just under the cwd box (left-aligned with it). Capture its left edge in
        // host coordinates so the width below can be measured against the window's right edge.
        double leftInRoot = 0.0;
        try
        {
            const auto xform = _cwdBox.TransformToVisual(layoutRoot);
            const auto pt = xform.TransformPoint(Point{ 0.0f, static_cast<float>(_cwdBox.ActualHeight()) });
            leftInRoot = static_cast<double>(pt.X);
            const double top = static_cast<double>(pt.Y) + kAnchorGapDips;
            // Written only on a real move: an offset write invalidates the popup root's arrange even when the value is
            // the same, and the re-anchor rounds read "moved" from these writes.
            const double moveX = leftInRoot - _pathPopup.HorizontalOffset();
            const double moveY = top - _pathPopup.VerticalOffset();
            if (std::abs(moveX) > kPathPickerAnchorEpsilonDips)
            {
                _pathPopup.HorizontalOffset(leftInRoot);
                moved = true;
                if (dx)
                {
                    *dx = moveX;
                }
            }
            if (std::abs(moveY) > kPathPickerAnchorEpsilonDips)
            {
                _pathPopup.VerticalOffset(top);
                moved = true;
                if (dy)
                {
                    *dy = moveY;
                }
            }
        }
        catch (...)
        {
            // Rule #18: the popup keeps its previous offsets (a stale anchor still opens a usable list),
            // but the failure is recorded instead of vanishing.
            ::Agentmaster::AgentLogCaughtException(L"_LayoutPathPicker anchor");
        }

        // 2. Width.
        if (_pathPanelBorder)
        {
            try
            {
                // Width is set EXPLICITLY here, not left to content/MaxWidth: the rows are short folder
                // leaf names, so a MaxWidth cap alone never grows the panel (it sizes to content and sits
                // at the floor). Instead the panel fills from its left anchor out to 10% short of the
                // window's right edge, with a readable floor when the window is narrow. The cwd box width
                // is the lower bound so it's never narrower than the box it drops from.
                const double minWidth = std::max<double>(560.0, _cwdBox.ActualWidth());
                const double rootWidth = layoutRoot ? layoutRoot.ActualWidth() : 0.0;
                const double rightLimit = rootWidth * 0.9; // leave a 10% gap on the window's right edge
                const double width = std::max<double>(minWidth, rightLimit - leftInRoot);
                _pathPanelBorder.Width(width);
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_LayoutPathPicker width"); // keeps the previous width
            }
        }

        // 3. Height cap: the list grows with its rows up to 90% of the WINDOW's height, never past the
        // window's bottom edge. The cap lives on the scroller (_pathScroll.MaxHeight) — a short list stays
        // short; a long one scrolls inside the cap.
        if (_pathScroll && _pathPanelBorder && layoutRoot)
        {
            try
            {
                constexpr double kMaxWindowFraction = 0.9; // the drop-down takes up to 90% of the window's height
                constexpr double kBottomGapDips = 8.0; // keeps the panel's rounded bottom edge visibly inside the window
                constexpr double kMinPanelDips = 120.0; // ~Browse… + a section header + two rows

                // "The window" is the XAML island: it fills the client area at (0,0), so XamlRoot().Size()
                // is the window's client size in DIPs (tab row included — the host's own height may not be),
                // and TransformToVisual(nullptr) yields positions in those same coordinates. A missing island
                // (not laid out yet) keeps the current cap rather than guessing.
                const auto xamlRoot = layoutRoot.XamlRoot();
                const double windowHeight = xamlRoot ? static_cast<double>(xamlRoot.Size().Height) : 0.0;
                if (windowHeight > 0.0)
                {
                    const auto boxBottom = _cwdBox.TransformToVisual(nullptr).TransformPoint(Point{ 0.0f, static_cast<float>(_cwdBox.ActualHeight()) });
                    const double panelTop = static_cast<double>(boxBottom.Y) + kAnchorGapDips;
                    // Both limits, the tighter wins: 90% of the window, and the room left below the box.
                    // The room limit matters because the popup is CLIPPED at the window edge, and a clipped
                    // list strands its last rows (and its scrollbar's lower end) where no scroll can reach.
                    const double roomBelow = windowHeight - panelTop - kBottomGapDips;
                    const double panelMax = std::max<double>(kMinPanelDips, std::min<double>(windowHeight * kMaxWindowFraction, roomBelow));
                    // The Border's padding + edge sit between the panel's outside and the scroller, so the
                    // scroller's own cap is the panel cap minus that chrome (read live, never hard-coded).
                    const auto pad = _pathPanelBorder.Padding();
                    const auto edge = _pathPanelBorder.BorderThickness();
                    const double chrome = pad.Top + pad.Bottom + edge.Top + edge.Bottom;
                    _pathScroll.MaxHeight(std::max<double>(0.0, panelMax - chrome));
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_LayoutPathPicker height"); // keeps the previous cap
            }
        }
        return moved;
    }

    // Agentmaster: a window resize while the drop-down is open re-fits it on the NEXT tick (contract on
    // the declaration). Called from the host's SizeChanged — a layout pass, which is exactly why the
    // re-fit itself must not run here.
    void AgentLaunchBar::_QueuePathPickerRelayout()
    {
        try
        {
            if (_pathPickerRelayoutQueued || !_dispatcher || !_pathPopup || !_pathPopup.IsOpen())
            {
                return; // closed (the next open re-fits) or a re-fit is already queued (coalesced)
            }
            _pathPickerRelayoutQueued = true;
            const bool queued = _dispatcher.TryEnqueue([weak = weak_from_this()]() {
                auto self = weak.lock();
                if (!self)
                {
                    return; // the bar was torn down before the tick ran
                }
                try
                {
                    self->_pathPickerRelayoutQueued = false; // BEFORE the re-fit: a resize after its reads queues another
                    // A re-anchor round (the layout-pass hook queued this) or a window-resize re-fit? Only the log
                    // below tells them apart; both re-fit the same way.
                    const bool reanchorRound = std::exchange(self->_pathPickerReanchorDue, false);
                    if (self->_pathPopup && self->_pathPopup.IsOpen())
                    {
                        double dx = 0.0;
                        double dy = 0.0;
                        if (self->_LayoutPathPicker(&dx, &dy))
                        {
                            // The list MOVED: the box was not where the previous anchor read it. A re-anchor round that
                            // moves it is the stale-anchor case of the 2026-10-12 report, caught and corrected — worth a
                            // line (rare: an open on a box that was not laid out at its place yet, or a host move).
                            if (reanchorRound)
                            {
                                ::Agentmaster::AppendStateLog(L"hooks.log",
                                                              L"[launchbar] the folder list moved under its box after a layout pass (dx=" +
                                                                  std::to_wstring(std::lround(dx)) + L" dy=" + std::to_wstring(std::lround(dy)) +
                                                                  L" DIP)\n");
                            }
                            // Check once more after the next layout pass: the pass that just ran may have been another
                            // window's on this UI thread. Bounded, so a list can never chase a box forever.
                            if (self->_pathPickerReanchorRounds < kPathPickerReanchorMaxRounds)
                            {
                                ++self->_pathPickerReanchorRounds;
                                self->_ArmPathPickerReanchor(); // noexcept
                            }
                        }
                    }
                }
                catch (...)
                {
                    // Nothing may escape a dispatcher callback (it would take the window down); the list
                    // keeps its previous geometry and the next resize/open re-fits.
                    ::Agentmaster::AgentLogCaughtException(L"_QueuePathPickerRelayout tick");
                }
            });
            if (!queued)
            {
                _pathPickerRelayoutQueued = false; // the dispatcher is shutting down with the window: re-arm, never latch
            }
        }
        catch (...)
        {
            _pathPickerRelayoutQueued = false; // re-arm: a latch stuck true would freeze the open list's size
            ::Agentmaster::AgentLogCaughtException(L"_QueuePathPickerRelayout");
        }
    }

    void AgentLaunchBar::_ClosePathPicker()
    {
        _DisarmPathPickerReanchor(); // a closed list re-anchors nothing; its next open arms afresh
        _pathPickerReanchorDue = false; // a round queued just before the close has nothing to re-fit (its tick finds the list closed)
        if (_pathPopup)
        {
            _pathPopup.IsOpen(false);
        }
    }

    void AgentLaunchBar::_ArmPathPickerReanchor() noexcept
    {
        try
        {
            if (_shutDown || _pathPickerReanchorArmed || !_cwdBox || !_pathPopup || !_pathPopup.IsOpen())
            {
                return; // the bar is shut down, a hook already waits (one pass serves every request), or nothing is open
            }
            // LayoutUpdated, not SizeChanged: the box does not change SIZE when only its card moves. It is raised after
            // every layout pass of the UI thread, the pass that places a moved popup's child included — so the hook
            // lives for exactly one call.
            _pathPickerReanchorRevoker = _cwdBox.LayoutUpdated(winrt::auto_revoke, [weak = weak_from_this()](const IInspectable&, const IInspectable&) {
                const auto self = weak.lock();
                if (!self)
                {
                    return; // the bar is gone; its revoker went with it
                }
                // One-shot: drop the hook FIRST, then re-fit on the next tick — never here: moving a parented Popup from a
                // layout-driven handler re-enters the pass (the E_LAYOUTCYCLE fail-fast). Both calls contain their own
                // failures, so nothing escapes into XAML.
                self->_DisarmPathPickerReanchor();
                self->_pathPickerReanchorDue = true;
                self->_QueuePathPickerRelayout();
            });
            _pathPickerReanchorArmed = true;
        }
        catch (...)
        {
            // No hook: the list keeps the anchor it has (the old behavior). Recorded (Rule #18).
            _pathPickerReanchorArmed = false;
            ::Agentmaster::AgentLogCaughtException(L"AgentLaunchBar: arm the folder list re-anchor");
        }
    }

    void AgentLaunchBar::_DisarmPathPickerReanchor() noexcept
    {
        _pathPickerReanchorArmed = false;
        _pathPickerReanchorRevoker.revoke(); // noexcept: a C++/WinRT revoker ignores the remove's result (a no-op when unarmed)
    }

    // Normalize the Launch path box in place. Platform-sensitive via NormPath (Windows: flip
    // '/'->'\' and trim insignificant trailing separators; POSIX: trim trailing '/'). Called
    // on commit (Enter), blur, list-pick, and launch — deliberately NOT per-keystroke, which
    // would eat a trailing separator the user types to descend into a folder.
    void AgentLaunchBar::_NormalizeCwdBox()
    {
        if (!_cwdBox)
        {
            return;
        }
        const std::wstring cur{ _cwdBox.Text() };
        const std::wstring norm = NormPath(cur);
        if (norm != cur)
        {
            _cwdBox.Text(winrt::hstring{ norm });
            _cwdBox.Select(static_cast<int32_t>(norm.size()), 0); // caret to end
        }
    }

    void AgentLaunchBar::_PickPath(const std::wstring& dir)
    {
        if (!_cwdBox)
        {
            return;
        }
        std::wstring norm = NormPath(dir); // selecting from the list normalizes too
        // Clicking a directory row DRILLS INTO it: append a separator so the rebuilt picker
        // lists that folder's children unfiltered (the trailing-separator rule above), turning
        // a click into a drill-down like the old picker. The slash is cosmetic — _NormalizeCwdBox
        // strips it again on commit / launch, and a drive root ("K:\") already carries one.
        if (IsDir(norm) && !norm.empty() && norm.back() != L'\\' && norm.back() != L'/')
        {
            norm += L'\\';
        }
        _cwdBox.Text(winrt::hstring{ norm }); // fires TextChanged -> _RebuildPathPicker (popup open)
        _cwdBox.Select(static_cast<int32_t>(norm.size()), 0); // caret to end
        _cwdBox.Focus(FocusState::Programmatic); // keep the box focused so the popup stays open
    }
}
