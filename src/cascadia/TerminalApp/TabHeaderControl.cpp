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

        // Agentmaster (SPARK CROWN — the cache-warm hint): build the glow breath + the three rising
        // embers, wired (like the pending pulse above) to run ONLY while TabStatus.AgentCacheWarmVisible.
        // Seven animations total: one Opacity breath on the glow Grid (which drives BOTH its discs at once
        // — nested opacity multiplies), plus a rise + a fade per ember. Each ember uses a KEYFRAME opacity
        // (0 -> 1 -> 0) rather than an AutoReverse: a spark must restart at the BOTTOM of its arc, not sail
        // back down it. The stagger is a positive BeginTime (0 / 380 / 760ms) — XAML has no negative delay,
        // so the first cycle ramps in one-by-one and every later cycle is evenly phased, exactly as the
        // pending dots do with their 160ms offsets.
        {
            namespace MA = winrt::Windows::UI::Xaml::Media::Animation;
            using winrt::Windows::Foundation::TimeSpan;
            using winrt::Windows::UI::Xaml::Duration;

            MA::RepeatBehavior forever;
            forever.Type = MA::RepeatBehaviorType::Forever; // a value struct — members are fields, not setters

            _cacheWarmStoryboard = MA::Storyboard{};

            // The halo's slow breath. Range is deliberately gentle: the glow must read as heat behind the
            // dot, never as a second status indicator competing with it.
            {
                MA::DoubleAnimation g{};
                g.From(0.28);
                g.To(0.62);
                g.Duration(Duration{ TimeSpan{ std::chrono::milliseconds(1400) } });
                g.AutoReverse(true);
                g.RepeatBehavior(forever);
                MA::Storyboard::SetTarget(g, HeaderCacheWarmGlow());
                MA::Storyboard::SetTargetProperty(g, L"Opacity");
                _cacheWarmStoryboard.Children().Append(g);
            }

            // The embers. Y runs +1.4 -> -1.6 (a 3px rise): the sparks sit at Canvas.Top 1.6/2.8, so the
            // TOP of the highest one lands at 0 — flush with the 18px cell, never clipped by the header.
            constexpr int64_t kCycleMs = 1150;
            const winrt::Windows::UI::Xaml::UIElement sparks[3]{ HeaderCacheSpark0(), HeaderCacheSpark1(), HeaderCacheSpark2() };
            constexpr int64_t kStaggerMs[3]{ 0, 380, 760 };
            for (int i = 0; i < 3; ++i)
            {
                const TimeSpan begin{ std::chrono::milliseconds(kStaggerMs[i]) };

                MA::DoubleAnimation rise{};
                rise.From(1.4);
                rise.To(-1.6);
                rise.Duration(Duration{ TimeSpan{ std::chrono::milliseconds(kCycleMs) } });
                rise.BeginTime(begin);
                rise.RepeatBehavior(forever);
                MA::Storyboard::SetTarget(rise, sparks[i]);
                MA::Storyboard::SetTargetProperty(rise, L"(UIElement.RenderTransform).(TranslateTransform.Y)");
                _cacheWarmStoryboard.Children().Append(rise);

                // Fade in fast (an ember appears already glowing), then out over the rest of the arc.
                MA::DoubleAnimationUsingKeyFrames fade{};
                const auto key = [](int64_t ms, double v) {
                    MA::LinearDoubleKeyFrame k{};
                    k.KeyTime(MA::KeyTime{ TimeSpan{ std::chrono::milliseconds(ms) } });
                    k.Value(v);
                    return k;
                };
                fade.KeyFrames().Append(key(0, 0.0));
                fade.KeyFrames().Append(key(kCycleMs / 4, 1.0));
                fade.KeyFrames().Append(key(kCycleMs, 0.0));
                fade.BeginTime(begin);
                fade.RepeatBehavior(forever);
                MA::Storyboard::SetTarget(fade, sparks[i]);
                MA::Storyboard::SetTargetProperty(fade, L"Opacity");
                _cacheWarmStoryboard.Children().Append(fade);
            }
        }
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
        // SizeChanged sees — the popup-hosted badges would float at the stale spot. LayoutUpdated
        // fires per tree-wide layout pass; the no-badges early-out + _PositionTagBadges' absolute-
        // position change-gate keep it O(one transform compare) when nothing tag-related moved.
        HeaderRootGrid().LayoutUpdated([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                if (!self->_renderedTagsSpec.empty())
                {
                    self->_PositionTagBadges();
                }
            }
        });
        // A popup-root child isn't torn down with the tab's subtree — close it explicitly when this
        // header unloads (tab closed / torn out; a re-parented header's next layout pass reopens it).
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
            }
        });
        // ... and (re)assert the badges the moment the header ENTERS a live tree: the popup open is
        // gated on IsLoaded + XamlRoot (_PositionTagBadges — opening earlier THROWS under islands,
        // the 2026-07-02 fail-fast), and a restored tab's badges are asserted via AgentTagsSpec
        // BEFORE its header is in the tree, so this Loaded is what finally shows them.
        Loaded([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                if (!self->_renderedTagsSpec.empty())
                {
                    self->_PositionTagBadges();
                }
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
            _UpdateCacheWarmAnimation();
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
                    if (n.empty() || n == L"AgentPendingVisible")
                    {
                        self->_UpdatePendingAnimation();
                    }
                    // Agentmaster (SPARK CROWN): the same one subscription starts/stops the cache-warm
                    // glow + embers — no second revoker to juggle (the bookmark badges ride it too).
                    if (n.empty() || n == L"AgentCacheWarmVisible")
                    {
                        self->_UpdateCacheWarmAnimation();
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
        _UpdateCacheWarmAnimation();
        _UpdateTagBadges();
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
            ::Agentmaster::AgentLogCaughtException(L"tab-header pending-dots storyboard");
        }
    }

    // Agentmaster (SPARK CROWN): run the cache-warm glow + embers iff this tab's session is still inside
    // Claude's server-side prompt-cache window. Stopping matters more here than for most indicators: a warm
    // cache lasts MINUTES and a busy fleet can have many warm tabs at once, so leaving these running would
    // hold the compositor awake long after the hint stopped being true. Stop() also returns each ember to
    // its base Opacity="0", so a stopped storyboard leaves nothing frozen on screen.
    void TabHeaderControl::_UpdateCacheWarmAnimation()
    {
        if (!_cacheWarmStoryboard)
        {
            return;
        }
        const auto status = TabStatus();
        const bool on = status && status.AgentCacheWarmVisible();
        try
        {
            if (on)
            {
                _cacheWarmStoryboard.Begin();
            }
            else
            {
                _cacheWarmStoryboard.Stop();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"tab-header cache-warm storyboard");
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
    void TabHeaderControl::_PositionTagBadges()
    {
        if (_badgeReposQueued)
        {
            return; // a check is already scheduled — triggers coalesce
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
        }
        CATCH_LOG();
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
        // narrow the box can't fully fit — visibility of a 120px box beats a 0px one.
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
