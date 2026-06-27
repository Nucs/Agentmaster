// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#include "pch.h"
#include "TabHeaderControl.h"

#include "TabHeaderControl.g.cpp"

#include "AgentTipHelpers.h" // Agentmaster: islands-safe hover tooltip for the tab-strip status dot

#include <atomic>
#include <chrono>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    // Agentmaster: the process-global tab-rename commit mode. Raw ints mirror
    // Agentmaster::TabRenameCommitMode (SessionModels.h) so this leaf control needn't pull in the
    // engine model; TerminalPage casts the enum to int via SetTabRenameCommitMode (a static_assert
    // there locks the values). atomic == read on the UI thread per keypress, written from any
    // window's settings-apply.
    constexpr int32_t kRenameCommitClickAwayOnly = 0; // only focus-loss commits (both keys insert a newline)
    constexpr int32_t kRenameCommitShiftEnter = 1; // default: Shift+Enter commits; plain Enter inserts a newline
    constexpr int32_t kRenameCommitEnter = 2; // Enter commits; Shift+Enter inserts a newline
    std::atomic<int32_t> g_renameCommitMode{ kRenameCommitShiftEnter };
}

namespace winrt::TerminalApp::implementation
{
    void SetTabRenameCommitMode(int32_t mode) noexcept
    {
        // Clamp an unknown value to the default (keyboard commit on Shift+Enter) rather than
        // silently disabling the keyboard commit altogether.
        if (mode != kRenameCommitClickAwayOnly && mode != kRenameCommitEnter)
        {
            mode = kRenameCommitShiftEnter;
        }
        g_renameCommitMode.store(mode, std::memory_order_relaxed);
    }

    TabHeaderControl::TabHeaderControl()
    {
        InitializeComponent();

        // Agentmaster: explain the tab-strip status dot on hover. Its COLOR encodes the session's
        // Triage state (and a dim gray dot = an observed, unmanaged tab) — not obvious without a
        // legend, so this is a core learning-curve aid. Islands-safe tooltip (AgentTipHelpers) on the
        // dot's wrap, set once here; only hoverable while a dot actually shows (the dot is collapsed
        // for tabs Agentmaster doesn't classify, so there's no stray tip on a plain tab).
        AgentSetTip(HeaderAgentStatusDotWrap(), L"Session status \x2014 the dot's color is the agent's Triage state: blue Running \xB7 goldenrod Waiting-for-you \xB7 orange-red Needs-approval \xB7 crimson Error \xB7 green Done \xB7 gray Idle. A dim gray dot marks an observed (unmanaged) tab; a flashing red ring means a background session needs you.");

        // Agentmaster (PENDING_INPUT.md): build the unsent-draft "3 dots" pulse and wire it to run ONLY
        // while this tab has a pending draft. The storyboard targets the 3 named dot ellipses directly
        // (no name/resource lookup); each dot pulses its Opacity 0.3<->1.0 forever, phase-shifted by
        // 160ms, for the classic "typing"/waiting wave. _HookTabStatusForPending (re)subscribes to
        // TabStatus's PropertyChanged — TabStatus is assigned by the Tab AFTER construction — and
        // starts/stops the storyboard on AgentPendingVisible, so idle tabs animate nothing (no perpetual
        // compositor wakeups across the fleet).
        {
            namespace MA = winrt::Windows::UI::Xaml::Media::Animation;
            _pendingDotsStoryboard = MA::Storyboard{};
            const winrt::Windows::UI::Xaml::UIElement dots[3]{ HeaderPendingDot0(), HeaderPendingDot1(), HeaderPendingDot2() };
            for (int i = 0; i < 3; ++i)
            {
                MA::DoubleAnimation a{};
                a.From(0.3);
                a.To(1.0);
                a.Duration(winrt::Windows::UI::Xaml::Duration{ std::chrono::milliseconds(500) });
                a.BeginTime(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds(160 * i) });
                a.AutoReverse(true);
                MA::RepeatBehavior forever;
                forever.Type = MA::RepeatBehaviorType::Forever; // a value struct — its members are fields, not setters
                a.RepeatBehavior(forever);
                MA::Storyboard::SetTarget(a, dots[i]);
                MA::Storyboard::SetTargetProperty(a, L"Opacity");
                _pendingDotsStoryboard.Children().Append(a);
            }
        }
        PropertyChanged([weakThis = get_weak()](auto&&, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
            if (auto self = weakThis.get())
            {
                if (args.PropertyName() == L"TabStatus")
                {
                    self->_HookTabStatusForPending();
                }
            }
        });
        _HookTabStatusForPending();

        // We'll only process the KeyUp event if we received an initial KeyDown event first.
        // Avoids issue immediately closing the tab rename when we see the enter KeyUp event that was
        // sent to the command palette to trigger the openTabRenamer action in the first place.
        HeaderRenamerTextBox().KeyDown([&](auto&&, auto&& e) {
            _receivedKeyDown = true;

            // GH#9632 - mark navigation buttons as handled.
            // This should prevent the tab view to use this key for navigation between tabs
            if (e.OriginalKey() == Windows::System::VirtualKey::Down ||
                e.OriginalKey() == Windows::System::VirtualKey::Up ||
                e.OriginalKey() == Windows::System::VirtualKey::Left ||
                e.OriginalKey() == Windows::System::VirtualKey::Right)
            {
                e.Handled(true);
            }
        });

        // Agentmaster: the rename box is AcceptsReturn (multi-line), so a plain Return normally
        // inserts a newline into the title rather than committing. The global TabRenameCommitMode
        // (a GLOBAL cross-window AppSetting) optionally promotes Enter or Shift+Enter to a COMMIT
        // that behaves EXACTLY like clicking away. PreviewKeyDown (tunneling) runs BEFORE the
        // TextBox's own key handling, which makes it the one place that can (a) see Enter reliably
        // regardless of how the box marks it and (b) PREVENT the AcceptsReturn newline for the commit
        // combo by marking the event handled. We deliberately DON'T close the box here: the original
        // note below warns that removing the box on a *down* event lets the following key-up bubble
        // to the NewTabButton — so we only flag the commit and perform it on the matching key-up.
        HeaderRenamerTextBox().PreviewKeyDown([this](auto&&, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
            if (e.OriginalKey() != Windows::System::VirtualKey::Enter)
            {
                return;
            }
            const auto mode = g_renameCommitMode.load(std::memory_order_relaxed);
            if (mode == kRenameCommitClickAwayOnly)
            {
                return; // "None" — Enter and Shift+Enter both just insert a newline (commit by clicking away)
            }
            // Shift+Enter and a plain Enter both arrive as VirtualKey::Enter, so the Shift modifier is
            // what distinguishes the commit key from the newline key. Read it islands-safe — guard the
            // CoreWindow like the rest of Agentmaster's UI; if absent, treat Shift as not-pressed.
            auto shiftDown = false;
            if (const auto w = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
            {
                shiftDown = WI_IsFlagSet(w.GetKeyState(winrt::Windows::System::VirtualKey::Shift), winrt::Windows::UI::Core::CoreVirtualKeyStates::Down);
            }
            const bool commit = (mode == kRenameCommitShiftEnter) ? shiftDown : !shiftDown;
            if (commit)
            {
                _commitOnKeyUp = true;
                e.Handled(true); // suppress the AcceptsReturn newline + stop the down from bubbling; KeyUp commits
            }
        });

        // NOTE: (Preview)KeyDown does not work here. If you use that, we'll
        // remove the TextBox from the UI tree, then the following KeyUp
        // will bubble to the NewTabButton, which we don't want to have
        // happen. (Agentmaster: this is exactly why the Enter/Shift+Enter commit, flagged in
        // PreviewKeyDown above, is *performed* here on key-up rather than on key-down.)
        HeaderRenamerTextBox().KeyUp([this](auto&&, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
            // Agentmaster: a commit combo (Enter / Shift+Enter per the global mode) was pressed; its
            // newline was already suppressed in PreviewKeyDown. Commit == "click away": collapsing the
            // box drives RenameBoxLostFocusHandler, which trims + raises TitleChangeRequested. Gated on
            // _commitOnKeyUp (set only while the box was focused), NOT _receivedKeyDown, so a commit on
            // the very first keystroke still works AND the command-palette open-Enter (whose down never
            // reached this box) is still ignored. Set Handled before closing — the close may synchronously
            // raise RenameEnded and tear this control down.
            if (_commitOnKeyUp)
            {
                _commitOnKeyUp = false;
                e.Handled(true);
                _CloseRenameBox();
                return;
            }
            if (_receivedKeyDown && e.OriginalKey() == Windows::System::VirtualKey::Escape)
            {
                // User wants to discard the changes they made,
                // set _renameCancelled to true and close the rename box
                _renameCancelled = true;
                _CloseRenameBox();
            }
        });
    }

    // Method Description:
    // - Returns true if we're in the middle of a tab rename. This is used to
    //   mitigate GH#10112.
    // Arguments:
    // - <none>
    // Return Value:
    // - true if the renamer is open.
    bool TabHeaderControl::InRename()
    {
        return Windows::UI::Xaml::Visibility::Visible == HeaderRenamerTextBox().Visibility();
    }

    // Agentmaster (PENDING_INPUT.md): (re)subscribe to the current TabStatus's PropertyChanged so the
    // unsent-draft pulse starts/stops with AgentPendingVisible. TabStatus is set by the Tab AFTER this
    // control is constructed (and could, in principle, be re-assigned), so this runs both from the
    // control's own "TabStatus" change notification and once at construction. The auto-revoke revoker
    // detaches from a superseded TabStatus; same-status re-hooks are a cheap no-op.
    void TabHeaderControl::_HookTabStatusForPending()
    {
        const auto status = TabStatus();
        if (status == _pendingHookedStatus)
        {
            _UpdatePendingAnimation();
            _UpdatePendingDotsOffset();
            return; // already hooked to this exact status (or both null)
        }
        _pendingStatusRevoker.revoke(); // detach the previous status (no-op if none)
        _pendingHookedStatus = status;
        if (status)
        {
            _pendingStatusRevoker = status.PropertyChanged(winrt::auto_revoke, [weakThis = get_weak()](auto&&, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
                if (auto self = weakThis.get())
                {
                    const auto n = args.PropertyName();
                    if (n.empty() || n == L"AgentPendingVisible")
                    {
                        self->_UpdatePendingAnimation();
                    }
                    // The STAR favorite marker collides with the pending dots (it's drawn behind the
                    // dot, points radiating down); a Crown<->Star switch (cog) flips this flag, so
                    // re-evaluate the dots' vertical offset on it too.
                    if (n.empty() || n == L"AgentFavoriteStarVisible")
                    {
                        self->_UpdatePendingDotsOffset();
                    }
                }
            });
        }
        _UpdatePendingAnimation();
        _UpdatePendingDotsOffset();
    }

    // Agentmaster (PENDING_INPUT.md): run the 3-dot pulse iff this tab currently has a pending draft.
    void TabHeaderControl::_UpdatePendingAnimation()
    {
        if (!_pendingDotsStoryboard)
        {
            return;
        }
        const auto status = TabStatus();
        const bool on = status && status.AgentPendingVisible();
        try
        {
            if (on)
            {
                _pendingDotsStoryboard.Begin();
            }
            else
            {
                _pendingDotsStoryboard.Stop();
            }
        }
        catch (...)
        {
        }
    }

    // Agentmaster (PENDING_INPUT.md): nudge the unsent-draft "3 dots" 3px LOWER when the STAR favorite
    // marker is active (favorited tab + the cog's Star icon — AgentFavoriteStarVisible). The star is
    // drawn BEHIND the status dot and its lower points radiate DOWN over the dots' band, so they'd
    // collide; the CROWN sits at the dot's NW and never does, so the offset is star-only. A pure render
    // transform (no layout reflow); re-applied whenever the favorite marker (or TabStatus) changes.
    void TabHeaderControl::_UpdatePendingDotsOffset()
    {
        const auto status = TabStatus();
        const bool star = status && status.AgentFavoriteStarVisible();
        try
        {
            winrt::Windows::UI::Xaml::Media::TranslateTransform tt;
            tt.Y(star ? 3.0 : 0.0);
            HeaderPendingDots().RenderTransform(tt);
        }
        catch (...)
        {
        }
    }

    // Method Description:
    // - Show the tab rename box for the user to rename the tab title
    // - We automatically use the previous title as the initial text of the box
    void TabHeaderControl::BeginRename()
    {
        _receivedKeyDown = false;
        _renameCancelled = false;
        _commitOnKeyUp = false;

        HeaderTextBlock().Visibility(Windows::UI::Xaml::Visibility::Collapsed);
        HeaderRenamerTextBox().Visibility(Windows::UI::Xaml::Visibility::Visible);

        HeaderRenamerTextBox().Text(Title());
        HeaderRenamerTextBox().SelectAll();
        HeaderRenamerTextBox().Focus(Windows::UI::Xaml::FocusState::Programmatic);

        TraceLoggingWrite(
            g_hTerminalAppProvider, // handle to TerminalApp tracelogging provider
            "TabRenamerOpened",
            TraceLoggingDescription("Event emitted when the tab renamer is opened"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    // Method Description:
    // - Event handler for when the rename box loses focus
    // - When the rename box loses focus, we send a request for the title change depending
    //   on whether the rename was cancelled
    void TabHeaderControl::RenameBoxLostFocusHandler(const Windows::Foundation::IInspectable& /*sender*/,
                                                     const Windows::UI::Xaml::RoutedEventArgs& /*e*/)
    {
        // If the context menu associated with the renamer text box is open we know it gained the focus.
        // In this case we ignore this event (we will regain the focus once the menu will be closed).
        const auto flyout = HeaderRenamerTextBox().ContextFlyout();
        if (flyout && flyout.IsOpen())
        {
            return;
        }

        // Log the data here, rather than in _CloseRenameBox. If we do it there,
        // it'll get fired twice, once when the key is pressed to commit/cancel,
        // and then again when the focus is lost

        TraceLoggingWrite(
            g_hTerminalAppProvider, // handle to TerminalApp tracelogging provider
            "TabRenamerClosed",
            TraceLoggingDescription("Event emitted when the tab renamer is closed"),
            TraceLoggingBoolean(_renameCancelled, "CancelledRename", "True if the user cancelled the rename, false if they committed."),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        _CloseRenameBox();
        if (!_renameCancelled)
        {
            // Agentmaster: the box is now multi-line (AcceptsReturn). Trim SURROUNDING whitespace /
            // newlines — a title typed then ended with a habitual Return shouldn't pad the tab with a
            // blank line — while preserving INTERNAL line breaks. Mirrors the Explorer-tree editor's
            // _CommitRename so both rename paths land the same value (Rule #11). An all-whitespace
            // result raises empty, which resets the tab to its auto/managed title (unchanged behavior).
            std::wstring text{ HeaderRenamerTextBox().Text() };
            const auto first = text.find_first_not_of(L" \t\r\n");
            const auto last = text.find_last_not_of(L" \t\r\n");
            text = (first == std::wstring::npos) ? std::wstring{} : text.substr(first, last - first + 1);
            TitleChangeRequested.raise(winrt::hstring{ text });
        }
    }

    // Method Description:
    // - Hides the rename box and displays the title text block
    void TabHeaderControl::_CloseRenameBox()
    {
        if (HeaderRenamerTextBox().Visibility() == Windows::UI::Xaml::Visibility::Visible)
        {
            HeaderRenamerTextBox().Visibility(Windows::UI::Xaml::Visibility::Collapsed);
            HeaderTextBlock().Visibility(Windows::UI::Xaml::Visibility::Visible);
            RenameEnded.raise(*this, nullptr);
        }
    }
}
