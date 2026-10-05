// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#include "pch.h"
#include "TabHeaderControl.h"

#include "TabHeaderControl.g.cpp"

#include "AgentCatchLog.h" // Agentmaster: AgentLogCaughtException — full-detail swallowed-exception forensics (type/hr/msg + throw stacks)
#include "AgentTipHelpers.h" // Agentmaster: islands-safe hover tooltip for the tab-strip status dot
#include "AgentStatusColors.h" // Agentmaster (bookmark tags): ParseArgbHexColor (the spec-carried picked color) + TagColorFor (the name-hash fallback)

#include <atomic>
#include <chrono>
#include <cmath>
#include <string_view>

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

    // Agentmaster (tab sizing): the process-global tab width mode override (SetTabWidthModeOverride) —
    // -1 == follow Windows Terminal's own tabWidthMode (the default), else a MUX TabViewWidthMode value.
    // atomic for the same reason as the rename mode: written from a window's settings apply, read when
    // a Tab sizes its rename box.
    constexpr int32_t kTabWidthModeFollowTerminal = -1;
    std::atomic<int32_t> g_tabWidthModeOverride{ kTabWidthModeFollowTerminal };

    // Agentmaster (tab sizing): the rename box's own font size in TabHeaderControl.xaml (FontSize="12").
    // ApplyTitleFont puts it back when the title font returns to the default — the box carries an
    // EXPLICIT size (unlike the title, which inherits), so clearing it would hand it the inherited
    // TabViewItem size instead of restoring the XAML's 12.
    constexpr double kRenamerDefaultFontSize = 12.0;
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

    void SetTabWidthModeOverride(int32_t mode) noexcept
    {
        // Only the three MUX TabViewWidthMode values are meaningful; anything else (incl. a future
        // enum value this build doesn't know) means "follow Terminal" — the conservative reading,
        // since Terminal's own setting is always a valid mode.
        if (mode < 0 || mode > static_cast<int32_t>(winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode::Compact))
        {
            mode = kTabWidthModeFollowTerminal;
        }
        g_tabWidthModeOverride.store(mode, std::memory_order_relaxed);
    }

    int32_t GetTabWidthModeOverride() noexcept
    {
        return g_tabWidthModeOverride.load(std::memory_order_relaxed);
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

        // Agentmaster (PENDING_INPUT.md): set up the unsent-draft "3 dots" pulse and wire it to run ONLY
        // while this tab has a pending draft. It drives the 3 named dot ellipses directly (no name/resource
        // lookup); each dot pulses its Opacity 1.0 -> 0.3 -> 1.0 forever, phase-shifted by 160ms, for the
        // classic "typing"/waiting wave. _HookTabStatusForPending (re)subscribes to TabStatus's
        // PropertyChanged — TabStatus is assigned by the Tab AFTER construction — and starts/stops the pulse
        // on AgentPendingVisible, so idle tabs animate nothing.
        // UI_THREAD.md §10a #1: composition Opacity loops, not a storyboard — a running storyboard makes XAML
        // walk the window's whole composition tree on every display frame, a composition loop does not. The
        // dots' panel is the anchor that supplies the compositor, so the dots themselves never get a hand-out
        // visual. Nothing composition-side is created here: the loops are built on the first start.
        _pendingDotsPulse = AgentDotsPulse{ HeaderPendingDots(), { HeaderPendingDot0(), HeaderPendingDot1(), HeaderPendingDot2() } };

        // Agentmaster (SPARK CROWN — the cache-warm hint): set up the glow breath + the three rising
        // embers, wired (like the pending pulse above) to run ONLY while TabStatus.AgentCacheWarmVisible.
        // UI_THREAD.md §10a #1: both on the Composition API, so a warm tab costs the UI thread nothing per
        // frame (a running storyboard made XAML walk the window's whole composition tree every frame).
        //   - The glow: one Opacity loop on the glow Grid (which drives BOTH its discs at once — nested
        //     opacity multiplies). Range deliberately gentle: the glow must read as heat behind the dot, never
        //     as a second status indicator competing with it. 0.45 -> 0.62 -> 0.28 -> 0.45 over 2.8 s is the
        //     storyboard's 0.28 <-> 0.62 breath (1.4 s each way), begun and ended on the still 0.45 (the
        //     Grid's XAML Opacity), so a stopped crown lands on its still look.
        //   - The embers: composition visuals of our own on the sparks canvas (AgentEmberSparks — the places,
        //     sizes, 3 DIP rise, 1150 ms cycle and 0 / 380 / 760 ms stagger of the XAML ellipses they
        //     replace). Not XAML elements: XAML follows a moving element on every frame for hit-testing.
        // The canvas is the anchor for both (the glow Grid itself never gets a hand-out visual). Nothing
        // composition-side is created here: the loop and the ember visuals are built on the first start.
        _cacheWarmGlow = AgentOpacityLoop{ HeaderCacheWarmSparks(),
                                           HeaderCacheWarmGlow(),
                                           { { 0.0f, 0.45f }, { 0.25f, 0.62f }, { 0.75f, 0.28f }, { 1.0f, 0.45f } },
                                           std::chrono::milliseconds{ 2800 },
                                           std::chrono::milliseconds{ 0 } };
        _cacheWarmEmbers = AgentEmberSparks{ HeaderCacheWarmSparks() };

        // Agentmaster (tab status-dot RED FLASH RING, UI_THREAD.md §10i): set up the ring's blink, wired (like
        // the two above) to run ONLY while TabStatus.AgentFlashRingVisible — which the page now sets once per
        // flash instead of flipping it every 600 ms. A composition expression on the ring's Opacity that follows
        // the UI thread's ONE shared clock (AgentLockstepBlink): every flashing tab, in every window, blinks in
        // lockstep, a tab that starts flashing mid-cycle joins the current phase, and nothing flips a XAML
        // property per blink (no layout pass, no XAML frame). The dot's wrap grid is the anchor, so the ring
        // never gets a hand-out visual. Nothing composition-side is created here: the clock is built on the
        // thread's first blink.
        _flashRingBlink = AgentLockstepBlink{ HeaderAgentStatusDotWrap(), HeaderAgentStatusFlashRing() };
        PropertyChanged([weakThis = get_weak()](auto&&, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
            if (auto self = weakThis.get())
            {
                const auto name = args.PropertyName();
                if (name == L"TabStatus")
                {
                    self->_HookTabStatusForPending();
                }
                else if (name == L"RenamerMaxWidth")
                {
                    // Agentmaster: the settings-driven width ceiling changed (tab-width mode flip) — re-fit.
                    self->_ApplyRenamerMaxWidth();
                }
            }
        });
        _HookTabStatusForPending();

        // Agentmaster: drive the on-screen fit from the box's resize — it lays out (Collapsed -> Visible)
        // and grows as you type here. _ApplyRenamerMaxWidth is internally gated on the WINDOW width, so
        // the fit runs once when the box first arranges and the later growth-driven SizeChanges are
        // no-ops — that gate is what prevents the MaxWidth-write -> reflow -> SizeChanged layout cycle
        // (the crash the first attempt hit). A genuine window resize changes the width and re-fits.
        HeaderRenamerTextBox().SizeChanged([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                self->_ApplyRenamerMaxWidth();
            }
        });

        // Agentmaster (bookmark tags): keep the overlay badge row pinned to the title's first
        // character + the hosting tab's bottom edge as the header lays out/resizes (leading
        // indicator icons appearing/disappearing shift the title's x; the first layout establishes
        // the real geometry; _PositionTagBadges also hooks the TabViewItem's own SizeChanged for
        // strip-height changes this grid never sees).
        HeaderRootGrid().SizeChanged([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                self->_PositionTagBadges();
            }
        });
        // ... and on EVERY layout pass: a tab REORDER (or the strip re-laying out around a
        // closed/opened sibling) MOVES this header without resizing it or its TabViewItem, which no
        // SizeChanged sees — the popup-hosted badges would float at the stale spot. That
        // LayoutUpdated hook is NOT subscribed here any more: _UpdateBadgeLayoutHook arms it only
        // while this header shows badges AND is in the live tree (UI_THREAD.md §10a #2). XAML calls
        // every LayoutUpdated handler of the thread after every layout pass — headers outside any
        // tree included — so an always-on subscription on all ~550 headers made each layout pass, in
        // any window, run ~550 handlers that almost always had no badges to place.
        // A popup-root child isn't torn down with the tab's subtree — close it explicitly when this
        // header unloads (tab closed / torn out; a re-parented header's Loaded places + reopens it).
        Unloaded([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                try
                {
                    if (const auto popup = self->HeaderTagBookmarksPopup(); popup && popup.IsOpen())
                    {
                        popup.IsOpen(false);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"tab-header tag popup close (unload)");
                }
                // UI_THREAD.md §10a #2: out of the live tree there is nothing to place, so the LayoutUpdated
                // hook (and its recorded placement) goes until the next Loaded. The method reads IsLoaded():
                // if it still reports the header in the tree, this Unloaded is STALE (raised after the next
                // Loaded — a tab move re-inserts its TabViewItem within one tick), the hook stays, and the
                // badges the close above just hid are placed again now, not at some later layout pass.
                if (self->_UpdateBadgeLayoutHook())
                {
                    self->_PositionTagBadges();
                }
                // RENDERING.md §4c: a header out of the live tree (virtualized away by the strip, its tab
                // closed or torn out) stops its pulse / embers — each update reads IsLoaded() and is
                // exception-safe on its own.
                self->_UpdatePendingAnimation();
                self->_UpdateCacheWarmAnimation();
                // UI_THREAD.md §10i: and lets go of the shared blink clock — the last ring to stop stops the clock,
                // so the compositor animates nothing while no flashing header is on any strip.
                self->_UpdateFlashRingAnimation();
            }
        });
        // ... and (re)assert the badges the moment the header ENTERS a live tree: the popup open is
        // gated on IsLoaded + XamlRoot (_PositionTagBadges — opening earlier THROWS under islands,
        // the 2026-07-02 fail-fast), and a restored tab's badges are asserted via AgentTagsSpec
        // BEFORE its header is in the tree, so this Loaded is what finally shows them.
        Loaded([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                // UI_THREAD.md §10a #2: back in the live tree — a tagged header needs its LayoutUpdated
                // hook again to follow reorders (a no-op for an untagged header, and for a stale Loaded
                // whose header has already left the tree again: the method reads IsLoaded()).
                self->_UpdateBadgeLayoutHook();
                if (!self->_renderedTagsSpec.empty())
                {
                    self->_PositionTagBadges();
                }
                // RENDERING.md §4c: back in the live tree (scrolled into the strip's realized range, a
                // re-parented header) — resume the pulse / embers if their flags still want them.
                self->_UpdatePendingAnimation();
                self->_UpdateCacheWarmAnimation();
                // UI_THREAD.md §10i: a still-flashing ring rejoins the shared blink clock, in phase with every other.
                self->_UpdateFlashRingAnimation();
            }
        });

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

    // Agentmaster (consistent multi-line tab-row height): reserve `lines` worth of vertical space so the
    // tab strip measures a stable height regardless of which tabs the ListView has realized (see the
    // HeaderTitleLineReserver comment in the .xaml). The reserver holds (lines-1) blank rows joined by
    // newlines — each carrying a zero-width space so the empty row is guaranteed full line height — in the
    // title's OWN inherited font, so the reserved height equals an N-line title exactly at any DPI. <=1
    // collapses the shim (the slim single-line strip, unchanged). Idempotent: guarded on the last count so
    // TerminalPage can push to every header on any title change and only a real change touches the tree.
    void TabHeaderControl::ReserveTitleLines(int32_t lines)
    {
        const int32_t n = lines < 1 ? 1 : lines;
        if (n == _reservedTitleLines)
        {
            return;
        }
        _reservedTitleLines = n;
        const auto reserver = HeaderTitleLineReserver();
        if (n <= 1)
        {
            reserver.Text(winrt::hstring{});
            reserver.Visibility(Windows::UI::Xaml::Visibility::Collapsed);
            return;
        }
        std::wstring text;
        text.reserve(static_cast<size_t>(n) * 2);
        for (int32_t i = 0; i < n; ++i)
        {
            if (i != 0)
            {
                text.push_back(L'\n');
            }
            text.push_back(static_cast<wchar_t>(0x200B)); // U+200B zero-width space: forces full line height, adds no width
        }
        reserver.Text(winrt::hstring{ text });
        reserver.Visibility(Windows::UI::Xaml::Visibility::Visible);
    }

    // Agentmaster (tab sizing): the cog's title font onto this header (see the declaration for the
    // contract). Applied to the TWO TextBlocks that draw title text — the title itself and the invisible
    // multi-line height reserver, which must keep the title's exact font or a reserved N-line strip would
    // measure the wrong height — rather than to the whole UserControl, so the indicator glyphs (explicit
    // 12 px FontIcons) and the shape-drawn dot/star/crown are untouched by construction.
    void TabHeaderControl::ApplyTitleFont(double fontSize, uint16_t fontWeight) noexcept
    {
        try
        {
            // Drop a LOCAL value only when one exists — the default path runs for every tab of a big window
            // restore, so it must not even touch a property it never set.
            const auto clearIfLocal = [](const winrt::Windows::UI::Xaml::Controls::TextBlock& tb, const winrt::Windows::UI::Xaml::DependencyProperty& dp) {
                if (tb.ReadLocalValue(dp) != winrt::Windows::UI::Xaml::DependencyProperty::UnsetValue())
                {
                    tb.ClearValue(dp);
                }
            };
            const auto applyToText = [fontSize, fontWeight, &clearIfLocal](const winrt::Windows::UI::Xaml::Controls::TextBlock& tb) {
                if (!tb)
                {
                    return;
                }
                // SIZE: set only on a real change (each write re-measures the tab); the default CLEARS the
                // local value so the TabViewItem's inherited TabViewItemHeaderFontSize governs again.
                if (fontSize > 0.0)
                {
                    if (tb.FontSize() != fontSize)
                    {
                        tb.FontSize(fontSize);
                    }
                }
                else
                {
                    clearIfLocal(tb, winrt::Windows::UI::Xaml::Controls::TextBlock::FontSizeProperty());
                }
                // WEIGHT: the same, comparing the struct's OpenType number.
                if (fontWeight > 0)
                {
                    if (tb.FontWeight().Weight != fontWeight)
                    {
                        tb.FontWeight(winrt::Windows::UI::Text::FontWeight{ fontWeight });
                    }
                }
                else
                {
                    clearIfLocal(tb, winrt::Windows::UI::Xaml::Controls::TextBlock::FontWeightProperty());
                }
            };
            applyToText(HeaderTextBlock());
            applyToText(HeaderTitleLineReserver());

            // The rename box carries an EXPLICIT size in the XAML (it doesn't inherit), so it is SET in both
            // directions — to the title size, or back to its XAML 12 — never cleared. Its weight stays the
            // XAML default: a bold editor would only make the caret harder to place.
            const double boxSize = fontSize > 0.0 ? fontSize : kRenamerDefaultFontSize;
            if (const auto box = HeaderRenamerTextBox(); box && box.FontSize() != boxSize)
            {
                box.FontSize(boxSize);
            }
        }
        catch (...)
        {
            // A failed font write leaves the tab with its previous font — cosmetic, never fatal — but it
            // must not escape: this runs inside TerminalPage's per-tab settings loop (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"TabHeaderControl::ApplyTitleFont");
        }
    }

    // Agentmaster (PENDING_INPUT.md): (re)subscribe to the current TabStatus's PropertyChanged so the
    // unsent-draft pulse starts/stops with AgentPendingVisible (and the crown, the flash ring's blink and the
    // tag badges follow their own flags). TabStatus is set by the Tab AFTER this
    // control is constructed (and could, in principle, be re-assigned), so this runs both from the
    // control's own "TabStatus" change notification and once at construction. The auto-revoke revoker
    // detaches from a superseded TabStatus; same-status re-hooks are a cheap no-op.
    void TabHeaderControl::_HookTabStatusForPending()
    {
        const auto status = TabStatus();
        if (status == _pendingHookedStatus)
        {
            _UpdatePendingAnimation();
            _UpdateCacheWarmAnimation();
            _UpdateFlashRingAnimation();
            _UpdateTagBadges();
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
                    // AgentStatusHalfVisible == "this tab's terminal has not started" (the dormant
                    // half-hollow dot): a remembered draft on a dormant tab shows still dots, and the
                    // pulse starts the moment the tab's claude starts (RENDERING.md §4c).
                    if (n.empty() || n == L"AgentPendingVisible" || n == L"AgentStatusHalfVisible")
                    {
                        self->_UpdatePendingAnimation();
                    }
                    // Agentmaster (SPARK CROWN): the same one subscription starts/stops the cache-warm
                    // glow + embers — no second revoker to juggle (the bookmark badges ride it too).
                    // UI_THREAD.md §10a #1: the embers are composition visuals, which cannot bind the brush in
                    // XAML, so a brush change is copied onto them here. The page sets the brush BEFORE it turns
                    // the crown on, so this branch runs first and the embers start in the right color.
                    if (n.empty() || n == L"AgentCacheWarmBrush")
                    {
                        self->_SyncCacheWarmEmberColor();
                    }
                    if (n.empty() || n == L"AgentCacheWarmVisible")
                    {
                        self->_UpdateCacheWarmAnimation();
                    }
                    // Agentmaster (RED FLASH RING, UI_THREAD.md §10i): the same one subscription starts / stops the
                    // ring's composition blink when the page shows or hides the ring. The ring's color needs no
                    // copying: it stays a XAML Fill (x:Bind AgentFlashRingBrush), only its Opacity is composed.
                    if (n.empty() || n == L"AgentFlashRingVisible")
                    {
                        self->_UpdateFlashRingAnimation();
                    }
                    // Agentmaster (bookmark tags): the same one subscription also rebuilds the
                    // bookmark badges when the tag spec changes (no second revoker to juggle).
                    if (n.empty() || n == L"AgentTagsSpec")
                    {
                        self->_UpdateTagBadges();
                    }
                }
            });
        }
        _UpdatePendingAnimation();
        // A new TabStatus carries its own crown brush: copy its color first (a running crown keeps running
        // through a re-hook, so the turn-on belt in _UpdateCacheWarmAnimation would not see it).
        _SyncCacheWarmEmberColor();
        _UpdateCacheWarmAnimation();
        // A new TabStatus carries its own flash flag: a ring that flashes there starts blinking, one that does not
        // stops (UI_THREAD.md §10i).
        _UpdateFlashRingAnimation();
        _UpdateTagBadges();
    }

    // Agentmaster (PENDING_INPUT.md + RENDERING.md §4c): run the 3-dot pulse iff the dots are shown AND
    // the motion is worth its cost. A running animation keeps the compositor (DWM) working on every display
    // frame, so the pulse also needs: this header in the live tree (IsLoaded — the strip virtualizes far
    // tabs away, and a hidden header must not animate), a STARTED tab (a dormant tab's REMEMBERED draft shows
    // still dots; AgentStatusHalfVisible is the published "this tab's terminal has not started" fact, so
    // the pulse begins the moment the tab's claude starts), and Windows' animation effects on. Still dots
    // keep their full base opacity, so the draft stays visible in every case. Called on every relevant
    // TabStatus change, on hook, and on Loaded / Unloaded.
    // UI_THREAD.md §10a #1: the pulse is composition Opacity loops (AgentDotsPulse), so a running pulse
    // costs the UI thread nothing per frame; a stopped pulse lands every dot back on its still opacity 1.0.
    void TabHeaderControl::_UpdatePendingAnimation()
    {
        // Everything, the WinRT reads included, inside the try: this runs from XAML event handlers
        // (Loaded / Unloaded / PropertyChanged), where an escaping exception is a fail-fast.
        try
        {
            const auto status = TabStatus();
            const bool on = status && status.AgentPendingVisible() && !status.AgentStatusHalfVisible() &&
                            IsLoaded() && AgentAnimationsEnabled();
            // A start on a running pulse RESTARTS it (the wave visibly jumps back), and the callers
            // re-assert often: act only on a real change.
            if (on == _pendingDotsRunning)
            {
                return;
            }
            if (on)
            {
                _pendingDotsPulse.Start();
            }
            else
            {
                _pendingDotsPulse.Stop();
            }
            _pendingDotsRunning = on;
        }
        catch (...)
        {
            // The flag keeps its old value, so the next call retries the transition.
            ::Agentmaster::AgentLogCaughtException(L"tab-header pending-dots pulse");
        }
    }

    // Agentmaster (SPARK CROWN): run the cache-warm glow + embers iff this tab's session is still inside
    // Claude's server-side prompt-cache window. Stopping matters more here than for most indicators: a warm
    // cache lasts MINUTES and a busy fleet can have many warm tabs at once, so leaving these running would
    // keep the compositor busy long after the hint stopped being true. A stop returns the glow to its still
    // Opacity 0.45 and hides every ember, so a stopped crown leaves nothing frozen on screen.
    // RENDERING.md §4c: it also runs only while this header is in the live tree (IsLoaded) and Windows'
    // animation effects are on. A still crown is the steady glow alone (its base Opacity 0.45), embers
    // hidden — the hint stays readable without a frame of motion.
    // UI_THREAD.md §10a #1: the glow is a composition Opacity loop and the embers are composition visuals
    // (see the constructor), so a warm tab costs the UI thread nothing per frame.
    void TabHeaderControl::_UpdateCacheWarmAnimation()
    {
        // Everything inside the try, as in _UpdatePendingAnimation (XAML event handlers call this).
        try
        {
            const auto status = TabStatus();
            const bool on = status && status.AgentCacheWarmVisible() && IsLoaded() && AgentAnimationsEnabled();
            // A start on a running crown restarts it: act only on a real change (see the pending pulse).
            if (on == _cacheWarmRunning)
            {
                return;
            }
            if (on)
            {
                // Belt for the color: the brush branch of the TabStatus subscription normally ran first, but a
                // re-hook to a new TabStatus or a Loaded re-start passes through here alone.
                _SyncCacheWarmEmberColor();
                _cacheWarmGlow.Start();
                _cacheWarmEmbers.Start();
            }
            else
            {
                _cacheWarmGlow.Stop();
                _cacheWarmEmbers.Stop();
            }
            _cacheWarmRunning = on;
        }
        catch (...)
        {
            // The flag keeps its old value, so the next call retries the transition (a half-started crown is
            // restarted whole; a half-stopped one is stopped again — both are safe to repeat).
            ::Agentmaster::AgentLogCaughtException(L"tab-header cache-warm crown");
        }
    }

    // UI_THREAD.md §10a #1: copy the crown brush's color onto the composition embers (see the declaration).
    void TabHeaderControl::_SyncCacheWarmEmberColor() noexcept
    {
        try
        {
            const auto status = TabStatus();
            if (!status)
            {
                return;
            }
            // The page always sets a SolidColorBrush (TerminalPage::_SetTabCacheWarm); anything else (null
            // before the tab's first warm moment) leaves the embers' color as it is.
            if (const auto brush = status.AgentCacheWarmBrush().try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>())
            {
                _cacheWarmEmbers.SetColor(brush.Color());
            }
        }
        catch (...)
        {
            // The embers keep their previous color; the next brush change retries (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"tab-header cache-warm ember color");
        }
    }

    // Agentmaster (tab status-dot RED FLASH RING, UI_THREAD.md §10i): run the ring's blink iff the page shows the
    // ring (TabStatus.AgentFlashRingVisible) and this header is in the live tree. The page sets that flag ONCE
    // when a flash starts and clears it when the flash ends (a visit, a window activation, a triage move, an
    // archive) — the blink itself is entirely here, on the Composition API, following the UI thread's one shared
    // clock (AgentLockstepBlink), so every flashing ring of every window blinks together and a ring that starts
    // mid-cycle joins the current phase.
    // Gates, and why each is (not) there:
    //   - IsLoaded(): a header out of the live tree (the strip virtualized it away, its tab closed or torn out)
    //     releases the clock, so the last flashing header to leave stops it; back in the tree, it rejoins in
    //     phase. The live answer, never the event that called (a stale Unloaded can follow the next Loaded).
    //   - NO AgentAnimationsEnabled() gate, unlike the pulses: the ring is an ALERT. The timer blink it replaces
    //     ran whatever Windows' animation effects said, and a steady ring would read as "nothing new".
    //   - NO "started tab" gate: a manual Mark Unread flashes a dormant (never-started) tab too.
    // A stop ends the blink with the ring at full opacity (AgentLockstepBlink::Stop), and the page's flag then
    // collapses it. Called on every relevant TabStatus change, on hook, and on Loaded / Unloaded.
    void TabHeaderControl::_UpdateFlashRingAnimation()
    {
        // Everything, the WinRT reads included, inside the try: this runs from XAML event handlers (Loaded /
        // Unloaded / PropertyChanged), where an escaping exception is a fail-fast.
        try
        {
            const auto status = TabStatus();
            const bool on = status && status.AgentFlashRingVisible() && IsLoaded();
            // Act only on a real change: the callers re-assert often, and each start is a XAML commit.
            if (on == _flashRingBlinking)
            {
                return;
            }
            if (on)
            {
                _flashRingBlink.Start();
                // Recorded only after a successful start: a failed one leaves the flag false, so the next call
                // retries it.
                _flashRingBlinking = true;
            }
            else
            {
                // Recorded BEFORE the stop: Stop() releases the clock even when it throws, so the blink has
                // stopped either way, and a flag left true would make the next flash skip its start.
                _flashRingBlinking = false;
                _flashRingBlink.Stop();
            }
        }
        catch (...)
        {
            // A failed start leaves the ring shown steadily (its static Opacity stays 1.0) and the flag false, so
            // the alert is still visible — it just does not blink until the next call retries. A failed stop
            // (a torn-down ring) has released the clock already.
            ::Agentmaster::AgentLogCaughtException(L"tab-header flash-ring blink");
        }
    }

    // Agentmaster (bookmark tags): rebuild the HeaderTagBookmarks overlay row from
    // TabStatus.AgentTagsSpec — one small bookmark-ribbon Polygon per '\n'-separated
    // "name\t#AARRGGBB" line. The COLOR rides the spec: the page resolves it once at the producer
    // (_SetTabAgentTags — the user-picked tag-colors.json color, else the stable name-hash) so this
    // renderer does no store I/O; a color-less legacy line falls back to TagColorFor here. Painted
    // over a thin black stroke (legible on any per-dir tab color, like the status dot). Hovering a
    // badge raises TagBadgeHoverBegin(tag, badge) / TagBadgeHoverEnd — the PAGE shows the rich tag
    // panel there (every session carrying the tag + status, click == jump), which a ToolTip cannot
    // do (tooltips are non-interactive; AgentSetTip's are hit-test-invisible on purpose).
    // Change-gated on the rendered spec so the frequent re-asserts (bind/launch/refresh paths)
    // rebuild nothing; capped at 20 badges (the layout-neutral overlay costs no strip width — the
    // tab's own clip is the real bound; every tag stays stored + listed in the Tags panel).
    void TabHeaderControl::_UpdateTagBadges()
    {
        const auto status = TabStatus();
        const winrt::hstring spec = status ? status.AgentTagsSpec() : winrt::hstring{};
        if (spec == _renderedTagsSpec)
        {
            return; // unchanged — the common re-assert costs nothing
        }
        _renderedTagsSpec = spec;
        // UI_THREAD.md §10a #2: the first tag arms the LayoutUpdated hook (when the header is in the
        // live tree), and removing the last tag disarms it — before any early return below, so the
        // hook always follows the spec.
        _UpdateBadgeLayoutHook();
        const auto panel = HeaderTagBookmarks();
        if (!panel)
        {
            return;
        }
        panel.Children().Clear();
        constexpr size_t kMaxBadges = 20;
        std::wstring_view rest{ spec };
        size_t shown = 0;
        while (!rest.empty() && shown < kMaxBadges)
        {
            const size_t nl = rest.find(L'\n');
            const std::wstring_view line = rest.substr(0, nl);
            rest = (nl == std::wstring_view::npos) ? std::wstring_view{} : rest.substr(nl + 1);
            // Split the line's resolved color off the name ("name\t#AARRGGBB"; NormalizeTagName
            // strips tabs from names, so the first '\t' is always the separator).
            std::wstring_view name = line;
            std::wstring_view hex{};
            if (const size_t sep = line.find(L'\t'); sep != std::wstring_view::npos)
            {
                name = line.substr(0, sep);
                hex = line.substr(sep + 1);
            }
            if (name.empty())
            {
                continue;
            }
            // A classic bookmark ribbon: a 6.5x9.3 rectangle (the base 5x7 scaled ~33% up, per
            // request) with a notch cut up into the bottom edge — positioned straddling the tab's
            // bottom line (~30% above / ~70% hanging below it; _PositionTagBadges), notch down.
            winrt::Windows::UI::Xaml::Shapes::Polygon ribbon;
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 6.5f, 0.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 6.5f, 9.3f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 3.25f, 6.5f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 9.3f });
            ribbon.Fill(winrt::Windows::UI::Xaml::Media::SolidColorBrush{ ParseArgbHexColor(hex, TagColorFor(name)) });
            ribbon.Stroke(winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::Black() });
            ribbon.StrokeThickness(0.75);
            // Hit-testable ON PURPOSE: the hover panel needs enter/leave.
            const winrt::hstring tagName{ name };
            ribbon.PointerEntered([weakThis = get_weak(), tagName](const winrt::Windows::Foundation::IInspectable& s, auto&&) {
                if (auto self = weakThis.get())
                {
                    if (const auto el = s.try_as<winrt::Windows::UI::Xaml::UIElement>())
                    {
                        self->TagBadgeHoverBegin.raise(tagName, el);
                    }
                }
            });
            ribbon.PointerExited([weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->TagBadgeHoverEnd.raise();
                }
            });
            // Popup-hosted badges live in the popup root, so a press no longer bubbles to the
            // TabViewItem — select the tab explicitly to keep click-on-badge == click-on-tab.
            ribbon.Tapped([weakThis = get_weak()](auto&&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs& e) {
                if (auto self = weakThis.get())
                {
                    if (const auto tvi = self->_badgeTabViewItem.get())
                    {
                        e.Handled(true);
                        tvi.IsSelected(true);
                    }
                }
            });
            panel.Children().Append(ribbon);
            ++shown;
        }
        _PositionTagBadges();
    }

    // Agentmaster (bookmark tags): place the popup-hosted badge row at (title's first character x,
    // straddling the TAB's bottom line — ~30% of each ribbon above it, ~70% OVERHANGING below) — the
    // "bookmark sticking out of the book" placement. The overhang is only renderable because the row
    // lives in a parented Popup: its child draws in the island's POPUP ROOT, outside the tab strip
    // ScrollViewer's clip (whose viewport bottom IS the tab's bottom edge — in-tree pixels below it
    // never render; the pre-popup flush-bottom placement was that constraint's ceiling). The tab
    // bottom is resolved by walking up to the TabViewItem ancestor (fresh each pass — the weak ref
    // only gates re-hooking its SizeChanged, which covers the STRIP growing for ANOTHER tab's
    // wrapped title without this header resizing). Popup mechanics this must own:
    //   - offsets are PARENT-relative (the header grid), so the same grid-space math applies; an
    //     offset change while open repositions live;
    //   - a PARENT MOVE (tab reorder / strip scroll) does NOT reliably re-place an open popup, so
    //     when the grid's island-absolute position changed since the last apply, close+reopen;
    //   - an unclipped popup would float over the caption buttons when its tab scrolls out of the
    //     strip viewport — hide it when the row leaves the strip ScrollViewer's x-range (the
    //     scroller's ViewChanged is hooked here, fresh-resolved like the TabViewItem).
    // The title x comes from a live transform (leading indicator icons shift it); pre-layout — or
    // before the ancestor resolves — sensible fallbacks hold until the next trigger re-pins
    // (fallback x: just past the 18px status-dot slot).
    //
    // CYCLE SAFETY — this is a SCHEDULER, not the work. Every trigger is layout-driven
    // (SizeChanged / LayoutUpdated / ViewChanged / a PropertyChanged inside a layout pass), and
    // mutating the popup (offsets / IsOpen) SYNCHRONOUSLY from inside a layout pass re-invalidates
    // layout, which re-enters the same pass: mid-pass geometry reads (a settling tab strip) shift
    // between iterations, so the "unchanged" gates never latch and XAML's cycle detector fail-fasts
    // with E_LAYOUTCYCLE 0x88000FA8 — the SECOND 0xC000027B crash (dumps 2026-07-02 13:32 + 13:50:
    // stowed backtraces show HorizontalOffset writes from the LayoutUpdated hook, ~250 iterations).
    // The same trap _ApplyRenamerMaxWidth's comment documents. So triggers only COALESCE-SCHEDULE
    // (_badgeReposQueued) and the mutation runs in _PositionTagBadgesNow on a CLEAN dispatcher tick,
    // after layout settles: reads are stable there, a mutation starts a fresh pass whose
    // LayoutUpdated merely schedules one more check that no-ops on the unchanged gates — settled,
    // no synchronous feedback, no cycle.
    //
    // LAYOUT-PASS GATE (UI_THREAD.md §10a #2): the per-pass check used to BE the placement, and the
    // placement creates up to 4 TransformToVisual transforms; releasing each one is a linear search
    // over every event-registered object in XAML's event manager (thousands at fleet scale). With 7
    // tagged tabs that was ~84 ms/s of the release UI thread while sessions were busy. So a trigger
    // from the LayoutUpdated hook (fromLayoutPass) first compares _BadgePlacementSignature — plain
    // property reads, no transforms — with the signature of the last COMPLETED placement, and
    // schedules nothing when they match. Reading inside the LayoutUpdated handler is safe: the pass
    // has finished, and only a WRITE re-enters layout. If a later pass moves something, it raises
    // LayoutUpdated again and that pass is compared. Explicit triggers (SizeChanged, ViewChanged,
    // Loaded, a tag change) skip the gate and always schedule, exactly as before.
    // fromLayoutPass: true only from the LayoutUpdated hook; false for every other trigger.
    void TabHeaderControl::_PositionTagBadges(const bool fromLayoutPass)
    {
        if (_badgeReposQueued)
        {
            return; // a check is already scheduled — triggers coalesce
        }
        // An empty recorded signature means no completed placement yet, and an empty current one means
        // a read failed: both read as "changed", so the full placement runs, which is the old behavior.
        if (fromLayoutPass && !_badgePlacedSignature.empty() && _BadgePlacementSignature() == _badgePlacedSignature)
        {
            return; // nothing the placement reads has moved since it last ran
        }
        _badgeReposQueued = true;
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                if (auto self = weakThis.get())
                {
                    self->_badgeReposQueued = false;
                    self->_PositionTagBadgesNow();
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header badge repos schedule");
            _badgeReposQueued = false; // no dispatcher (teardown) — drop the request
        }
    }

    void TabHeaderControl::_PositionTagBadgesNow()
    {
        // UI_THREAD.md §10a #2: forget the last completed placement until this one completes. Every
        // early return below (no row, no badges, a thrown read) then leaves it empty, so the next
        // layout pass schedules a full placement again instead of trusting a stale signature.
        _badgePlacedSignature.clear();
        const auto popup = HeaderTagBookmarksPopup();
        const auto row = HeaderTagBookmarks();
        if (!popup || !row)
        {
            return;
        }
        // No badges -> nothing to show (the popup stays closed; _UpdateTagBadges re-runs us on change).
        if (_renderedTagsSpec.empty() || row.Children().Size() == 0)
        {
            try
            {
                if (popup.IsOpen())
                {
                    popup.IsOpen(false);
                }
            }
            CATCH_LOG();
            return;
        }
        double x = 20.0; // fallback: just past the status-dot slot
        try
        {
            if (const auto title = HeaderTextBlock(); title && title.Visibility() == winrt::Windows::UI::Xaml::Visibility::Visible && title.ActualWidth() > 0)
            {
                const auto pt = title.TransformToVisual(HeaderRootGrid()).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                x = pt.X;
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header badge position (title x)");
        }
        constexpr double kBadgeHeight = 9.3; // the ribbon Polygon's height (_UpdateTagBadges)
        double top = 0.0;
        bool pinned = false;
        try
        {
            if (const auto grid = HeaderRootGrid())
            {
                // Find the hosting TabViewItem — the visible tab rectangle whose bottom line we straddle.
                winrt::Microsoft::UI::Xaml::Controls::TabViewItem tvi{ nullptr };
                auto d = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(grid);
                while (d && !tvi)
                {
                    tvi = d.try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
                    if (!tvi)
                    {
                        d = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(d);
                    }
                }
                if (tvi && tvi.ActualHeight() > kBadgeHeight)
                {
                    const auto bottom = tvi.TransformToVisual(grid).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, static_cast<float>(tvi.ActualHeight()) });
                    // The 30/70 straddle: the ribbon's top sits 30% of its height above the tab's
                    // bottom line, the remaining ~70% hangs below it (unclipped — popup root).
                    top = std::floor(bottom.Y - std::ceil(kBadgeHeight * 0.30));
                    pinned = top > 0.0;
                    if (pinned && _badgeTabViewItem.get() != tvi)
                    {
                        _badgeTabViewItem = tvi;
                        _badgeTviSizeRevoker = tvi.SizeChanged(winrt::auto_revoke, [weakThis = get_weak()](auto&&, auto&&) {
                            if (auto self = weakThis.get())
                            {
                                self->_PositionTagBadges();
                            }
                        });
                    }
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header badge position (anchor)");
        }
        if (!pinned)
        {
            // Pre-layout / ancestor not resolvable yet: straddle the header block's bottom instead,
            // until a SizeChanged/LayoutUpdated trigger re-pins to the real tab bottom.
            double h = HeaderRootGrid() ? HeaderRootGrid().ActualHeight() : 0.0;
            if (h <= 0)
            {
                h = 22.0; // the dot wrap's height (the row's usual tallest child)
            }
            top = std::floor(h - std::ceil(kBadgeHeight * 0.30));
        }

        // Island-absolute position + strip-viewport visibility.
        bool visible = true;
        double absX = _badgeLastAbsX;
        double absY = _badgeLastAbsY;
        try
        {
            const auto grid = HeaderRootGrid();
            const auto origin = grid.TransformToVisual(nullptr).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
            absX = origin.X;
            absY = origin.Y;
            // Resolve the tab strip's ScrollViewer (fresh — the weak ref only gates re-hooking) and
            // hide the row when it leaves the viewport's x-range: the popup escapes the clip that
            // would otherwise hide a scrolled-out tab's badges.
            winrt::Windows::UI::Xaml::Controls::ScrollViewer sv{ nullptr };
            auto d = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(grid);
            while (d && !sv)
            {
                sv = d.try_as<winrt::Windows::UI::Xaml::Controls::ScrollViewer>();
                if (!sv)
                {
                    d = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(d);
                }
            }
            if (sv)
            {
                if (_badgeStripScroller.get() != sv)
                {
                    _badgeStripScroller = sv;
                    _badgeSvViewChangedRevoker = sv.ViewChanged(winrt::auto_revoke, [weakThis = get_weak()](auto&&, auto&&) {
                        if (auto self = weakThis.get())
                        {
                            if (!self->_renderedTagsSpec.empty())
                            {
                                self->_PositionTagBadges();
                            }
                        }
                    });
                }
                const auto svOrigin = sv.TransformToVisual(nullptr).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                const double svLeft = svOrigin.X;
                const double svRight = svOrigin.X + sv.ActualWidth();
                const double rowLeft = absX + x;
                const double rowWidth = std::max(12.0, row.ActualWidth()); // pre-arrange fallback
                if (rowLeft + rowWidth < svLeft + 2.0 || rowLeft > svRight - 2.0)
                {
                    visible = false; // the tab is scrolled (mostly) out of the strip
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header badge visibility");
        }

        const bool parentMoved = std::abs(absX - _badgeLastAbsX) > 0.5 || std::abs(absY - _badgeLastAbsY) > 0.5;
        _badgeLastAbsX = absX;
        _badgeLastAbsY = absY;
        // ISLANDS SAFETY — the popup open/close tail is fully guarded. THIS EXACT SECTION
        // fail-fasted the app (0xC000027B, dumps 2026-07-02 12:41 + 12:50, 4 stowed records all
        // pointing here): Popup.IsOpen mutations on a header that is not (yet / anymore) rooted in
        // a live island tree throw — E_UNEXPECTED opening with no XamlRoot (a RESTORED tab's badges
        // are asserted via _SetTabAgentTags -> AgentTagsSpec BEFORE its header enters the tree, so
        // every launch with a tagged session crashed), E_INVALIDARG around a teardown-pass
        // SizeChanged — and a winrt exception escaping a layout/property handler becomes the stowed
        // fail-fast (uncatchable at the crash site; it must be caught HERE). Two layers:
        //   (a) never OPEN unless the grid IsLoaded + has a XamlRoot — the ctor's Loaded hook
        //       re-runs us the moment the header actually enters a tree, so gated-off badges
        //       appear then (and a torn-down header never reopens its popup);
        //   (b) CATCH_LOG the whole tail so any residual refusal degrades to "badges hidden this
        //       pass" instead of process death.
        try
        {
            if (!visible)
            {
                if (popup.IsOpen())
                {
                    popup.IsOpen(false);
                }
                // UI_THREAD.md §10a #2: this placement completed (the row is hidden); record what it saw
                // so the layout passes that follow skip it until something moves.
                _badgePlacedSignature = _BadgePlacementSignature();
                return;
            }
            if (popup.IsOpen() && parentMoved)
            {
                popup.IsOpen(false); // force the reopen below to re-evaluate the parent's new position
            }
            popup.HorizontalOffset(x);
            popup.VerticalOffset(top);
            if (!popup.IsOpen())
            {
                const auto grid = HeaderRootGrid();
                if (grid && grid.IsLoaded() && grid.XamlRoot())
                {
                    popup.IsOpen(true);
                }
            }
            // UI_THREAD.md §10a #2: record the signature AFTER the open above, so the popup's resulting
            // state is part of it: a popup that something else closes later reads as a change.
            _badgePlacedSignature = _BadgePlacementSignature();
        }
        CATCH_LOG();
    }

    // Agentmaster (bookmark tags, UI_THREAD.md §10a #2): arm or disarm the header grid's LayoutUpdated
    // hook, the trigger that re-places the badges after a tab REORDER moved this header without
    // resizing it. XAML raises LayoutUpdated on EVERY subscriber of the UI thread after every layout
    // pass: all windows, and headers outside any visual tree too. So the hook is held only while it can
    // matter: this header shows badges AND is in the live tree. A strip of 550 tabs with 7 tagged ones
    // then runs 7 handlers per layout pass instead of ~550.
    // "In the live tree" is IsLoaded(), the live answer, never the event that brought us here: XAML can
    // raise a STALE Unloaded after the next Loaded when an element is re-parented within one tick (a tab
    // move removes and re-inserts its TabViewItem), the case TermControl::_UpdateRenderingSuspension
    // documents. Trusting that Unloaded would drop the hook of a header that is still on screen.
    // Returns: true when the hook is armed after the call (the header is tagged and in the live tree).
    // The Unloaded handler uses it to tell a stale Unloaded and re-place the badges it just hid.
    // Disarming also clears _badgePlacedSignature: the popup is closed while the header is out of the
    // tree, and a re-armed hook must start from a full placement, never from a stale "nothing moved".
    // Never throws (it runs from XAML event handlers, where an escaping exception is a fail-fast): a
    // failed read or subscribe is logged and leaves the hook as it was; the return value reports that
    // state. With the hook off, explicit triggers still place the badges; only a reorder can then leave
    // them at a stale spot until the next trigger.
    bool TabHeaderControl::_UpdateBadgeLayoutHook()
    {
        try
        {
            const bool want = IsLoaded() && !_renderedTagsSpec.empty();
            // The revoker holds the subscription, so its state IS "armed". Acting only on a change keeps
            // the repeated Loaded / tag re-asserts free and can never stack a second handler.
            if (want == static_cast<bool>(_badgeLayoutUpdatedRevoker))
            {
                return want;
            }
            if (!want)
            {
                _badgeLayoutUpdatedRevoker.revoke();
                _badgePlacedSignature.clear();
                return false;
            }
            _badgeLayoutUpdatedRevoker = HeaderRootGrid().LayoutUpdated(winrt::auto_revoke, [weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    // Belt: the hook is disarmed the moment the last tag goes, but a raise already in
                    // progress can still reach a header whose spec has just emptied.
                    if (!self->_renderedTagsSpec.empty())
                    {
                        self->_PositionTagBadges(true);
                    }
                }
            });
            return true;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header badge layout hook");
            // A failed subscribe throws before the assignment, so the revoker still tells the truth.
            return static_cast<bool>(_badgeLayoutUpdatedRevoker);
        }
    }

    // Agentmaster (bookmark tags, UI_THREAD.md §10a #2): the fingerprint behind the layout-pass gate in
    // _PositionTagBadges. It holds every input of _PositionTagBadgesNow's placement, read as plain
    // properties (the header comment lists them). No TransformToVisual: the transform objects it
    // creates made the per-pass placement expensive, because each release walks XAML's event-manager
    // list. Read-only, so it is safe inside a LayoutUpdated handler.
    // Returns: the fingerprint; empty when a needed element is missing or a read threw (empty reads as
    //          "changed", so the caller falls back to a full placement).
    // Never throws: a failed read is logged (AgentLogCaughtException throttles repeats per context).
    std::vector<double> TabHeaderControl::_BadgePlacementSignature() noexcept
    {
        try
        {
            const auto popup = HeaderTagBookmarksPopup();
            const auto row = HeaderTagBookmarks();
            const auto grid = HeaderRootGrid();
            if (!popup || !row || !grid)
            {
                return {};
            }
            std::vector<double> signature;
            signature.reserve(128);
            // The x source: the title's first character while the title shows, else the fixed
            // fallback. _PositionTagBadgesNow reads the title's position under this same condition.
            const auto title = HeaderTextBlock();
            const bool titleGivesX = title && title.Visibility() == winrt::Windows::UI::Xaml::Visibility::Visible && title.ActualWidth() > 0;
            signature.push_back(titleGivesX ? 1.0 : 0.0);
            // The show/hide inputs: the popup's state, the open gate, and the row width that the strip-
            // viewport test uses. Then the header height: the top when no TabViewItem is found.
            signature.push_back(popup.IsOpen() ? 1.0 : 0.0);
            signature.push_back((grid.IsLoaded() && grid.XamlRoot()) ? 1.0 : 0.0);
            signature.push_back(row.ActualWidth());
            signature.push_back(grid.ActualHeight());
            // The position inputs: the layout offset of every element from the x source up to the root.
            // A reorder moves the TabViewItem inside its panel, an indicator icon moves the title inside
            // the header, and a change around the strip moves the ScrollViewer: each changes an offset
            // here. The first TabViewItem (its height sets the badge top) and the first ScrollViewer
            // (its scroll offsets, which ActualOffset leaves out, and its width, the visible range) are
            // the elements _PositionTagBadgesNow finds by the same upward walk from the header grid.
            constexpr size_t kMaxDepth = 128; // far deeper than a tab header's ancestry (~40 elements)
            winrt::Windows::UI::Xaml::DependencyObject node{ nullptr };
            if (titleGivesX)
            {
                node = title;
            }
            else
            {
                node = grid;
            }
            bool sawTabViewItem = false;
            bool sawScrollViewer = false;
            for (size_t depth = 0; node && depth < kMaxDepth; ++depth)
            {
                if (const auto element = node.try_as<winrt::Windows::UI::Xaml::UIElement>())
                {
                    const auto offset = element.ActualOffset();
                    signature.push_back(offset.x);
                    signature.push_back(offset.y);
                }
                if (!sawTabViewItem)
                {
                    if (const auto tvi = node.try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>())
                    {
                        sawTabViewItem = true;
                        signature.push_back(tvi.ActualHeight());
                    }
                }
                if (!sawScrollViewer)
                {
                    if (const auto scroller = node.try_as<winrt::Windows::UI::Xaml::Controls::ScrollViewer>())
                    {
                        sawScrollViewer = true;
                        signature.push_back(scroller.HorizontalOffset());
                        signature.push_back(scroller.VerticalOffset());
                        signature.push_back(scroller.ActualWidth());
                    }
                }
                node = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(node);
            }
            return signature;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header badge placement signature");
            return {};
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

        // Agentmaster: keep the rename box fully on-screen for the life of this rename — re-arm the
        // one-shot fit (see _ApplyRenamerMaxWidth). The box just went Collapsed -> Visible, so it isn't
        // arranged yet; this synchronous call sets a safe initial cap, and the box's SizeChanged (which
        // fires once it lays out with content) does the accurate fit. Window resizes during the rename
        // are picked up by the same SizeChanged path via the width gate.
        _renamerFitDone = false;
        _lastRootWidth = -1.0;
        _ApplyRenamerMaxWidth();

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
            // Agentmaster: the box is hidden again — drop the fit latch so the next rename re-fits fresh.
            _renamerFitDone = false;
            HeaderRenamerTextBox().Visibility(Windows::UI::Xaml::Visibility::Collapsed);
            HeaderTextBlock().Visibility(Windows::UI::Xaml::Visibility::Visible);
            RenameEnded.raise(*this, nullptr);
        }
    }

    // Agentmaster: cap the rename box's MaxWidth so its right border always stays inside the window.
    // The box is anchored at the tab's left and grows rightward (NoWrap auto-size); RenamerMaxWidth is
    // the settings ceiling (360 in fixed-width tab modes, +inf in SizeToContent). We tighten that to the
    // space between the box's actual on-screen left and the window's right edge, so the box grows as
    // large as it can ("maximize") while the WHOLE border — and, for RTL/Hebrew text whose start sits at
    // the right edge, the beginning of the text — stays visible.
    // (Tab title fit: the box also lives in the title row's STAR column now — TabHeaderControl.xaml's
    // HeaderTitleRow — so the room its TAB gives it bounds it on top of this cap; past that the text
    // scrolls inside the box. This window cap still matters for a tab at the strip's right end, where the
    // tab itself can reach past the window's visible edge.)
    //
    // Recomputes AT MOST ONCE per (rename, window width). See the header comment: writing MaxWidth
    // reflows layout and re-fires the SizeChanged that calls this, and recomputing leftX every pass made
    // the cap oscillate sub-pixel -> XAML layout-cycle abort (the stowed exception that crashed the first
    // attempt). The width gate below makes a box-growth-driven SizeChanged a no-op once fitted, so the
    // write can't feed back; only a real window-size change re-arms it.
    void TabHeaderControl::_ApplyRenamerMaxWidth()
    {
        const auto box = HeaderRenamerTextBox();
        if (box.Visibility() != Windows::UI::Xaml::Visibility::Visible)
        {
            return;
        }

        const auto xr = XamlRoot();
        if (!xr)
        {
            return;
        }
        const double rootWidth = static_cast<double>(xr.Size().Width);
        if (rootWidth <= 0.0)
        {
            return;
        }

        // Already fitted for this window width? Then this call is a box-growth SizeChanged echo — do
        // NOTHING, or we risk the layout-cycle feedback. A genuine window resize changes rootWidth and
        // falls through to re-fit.
        if (_renamerFitDone && std::abs(rootWidth - _lastRootWidth) < 0.5)
        {
            return;
        }

        double maxW = RenamerMaxWidth(); // the settings ceiling
        constexpr double rightMargin = 8.0; // keep the border clear of the very edge

        bool gotLeft = false;
        double leftX = 0.0;
        try
        {
            // The box's FlowDirection is LTR (only its TEXT auto-detects RTL), so (0,0) is the top-LEFT
            // corner — its left edge in window coordinates. leftX is independent of the box's width
            // (left-anchored), so one post-arrange measurement holds for the whole rename.
            leftX = box.TransformToVisual(xr.Content()).TransformPoint({ 0.0f, 0.0f }).X;
            gotLeft = true;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header renamer measure");
        }

        // With a real on-screen left, cap to (left -> window right); otherwise (box not arranged yet)
        // fall back to never exceeding the window width, and let the post-arrange SizeChanged refine it.
        const double fit = gotLeft ? (rootWidth - leftX - rightMargin) : (rootWidth - rightMargin);
        if (fit < maxW)
        {
            maxW = fit;
        }

        // Never collapse below the usable floor (an emptied box must stay grabbable) even on a window so
        // narrow the box can't fully fit — visibility of a MinWidth (40px) box beats a 0px one.
        const double floorW = box.MinWidth();
        if (maxW < floorW)
        {
            maxW = floorW;
        }

        // Latch as fitted only once we had a REAL arranged position — so a synchronous pre-layout call
        // (leftX stale/zero) doesn't cache a wrong width and starve the real fit on the next SizeChanged.
        // Latch BEFORE the write: should the write ever re-enter SizeChanged synchronously, the gate is
        // already armed and that re-entry returns immediately — belt-and-suspenders against a layout cycle.
        if (gotLeft && box.ActualWidth() > 0.0)
        {
            _renamerFitDone = true;
            _lastRootWidth = rootWidth;
        }

        if (std::abs(box.MaxWidth() - maxW) > 0.5)
        {
            box.MaxWidth(maxW);
        }
    }
}
