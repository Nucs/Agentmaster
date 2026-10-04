// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once

#include "winrt/Microsoft.UI.Xaml.Controls.h"

#include "AgentAmbientAnimation.h" // Agentmaster (UI_THREAD.md §10a #1, §10i): the draft-dots pulse, spark crown and flash-ring blink on the Composition API

#include "TabHeaderControl.g.h"

namespace winrt::TerminalApp::implementation
{
    // Agentmaster: the process-global tab-rename commit mode (mirrors AppSettings::tabRenameCommitMode;
    // see SessionModels.h). It is GLOBAL across windows, so rather than plumb the value into every
    // Tab's header on each settings change, TerminalPage writes this one process-wide value (on
    // AppSettings load + cog Save) and every header's rename box reads it live on a keypress. The raw
    // int avoids coupling this leaf control to the engine model; values match TabRenameCommitMode
    // (0 = click-away only, 1 = +Shift+Enter, 2 = +Enter), locked by a static_assert in
    // TerminalPage.AgentEngine.cpp.
    void SetTabRenameCommitMode(int32_t mode) noexcept;

    // Agentmaster (tab sizing): the process-global TAB WIDTH MODE OVERRIDE — the cog's "Tab width"
    // (AppSettings::tabWidthMode) resolved to a MUX TabViewWidthMode value, or -1 for "follow Windows
    // Terminal's own tabWidthMode setting" (the default). TerminalPage applies the effective mode to
    // its TabView itself; this copy exists for Tab::_UpdateHeaderControlMaxWidth, which sizes the
    // rename box by the width mode but only knows Terminal's settings — without it a tab in an
    // overridden "Fit to title" strip would still cap its rename box at the Equal-mode width. GLOBAL
    // across windows (one process), so one atomic rather than per-tab plumbing (the rename-commit idiom
    // above). Written by TerminalPage::_ApplyTabSizingSettings (engine init, cog Save, broadcast).
    // Parameters: mode - -1 (follow Terminal), or 0 Equal / 1 SizeToContent / 2 Compact (the MUX
    //             TabViewWidthMode values); anything else is stored as -1.
    void SetTabWidthModeOverride(int32_t mode) noexcept;
    // Returns: the override set above — -1 when the strip follows Terminal's own setting.
    int32_t GetTabWidthModeOverride() noexcept;

    struct TabHeaderControl : TabHeaderControlT<TabHeaderControl>
    {
        TabHeaderControl();
        void BeginRename();

        void RenameBoxLostFocusHandler(const winrt::Windows::Foundation::IInspectable& sender,
                                       const winrt::Windows::UI::Xaml::RoutedEventArgs& e);

        bool InRename();

        // Agentmaster (consistent multi-line tab-row height): reserve `lines` worth of vertical space in
        // this header (an invisible blank-line shim in the title's own font), so the tab strip measures a
        // STABLE height regardless of which tabs the ListView has realized. TerminalPage pushes the max
        // title-line-count across ALL tabs here; <=1 collapses the shim (the slim single-line strip).
        void ReserveTitleLines(int32_t lines);

        // Agentmaster (tab sizing): apply the cog's TITLE FONT to this header — the title (and the
        // invisible multi-line height reserver, which must carry the title's exact font, or a reserved
        // N-line strip would no longer measure N title lines) plus the rename box, so what you type reads
        // at the size it will commit at. Set on those TextBlocks only — the indicator glyphs carry explicit sizes and
        // the dot/star/crown are shapes, so nothing but the TEXT changes. A 0 argument means "the
        // default" and CLEARS the TextBlocks' local value, so the TabViewItem's own
        // TabViewItemHeaderFontSize / weight flow through by inheritance (an untouched install is
        // byte-identical to before). Change-gated: an unchanged value touches nothing (every write
        // re-measures the tab, and TerminalPage calls this for every tab on each settings apply).
        // Parameters:
        //   fontSize   - the title size in DIPs (> 0), or 0 to inherit the TabViewItem's size.
        //   fontWeight - an OpenType weight (400 Normal / 600 SemiBold / 700 Bold), or 0 to inherit.
        // UI thread only. Never throws: a XAML failure is logged (Rule #18) and leaves the old font.
        void ApplyTitleFont(double fontSize, uint16_t fontWeight) noexcept;

        til::event<TerminalApp::TitleChangeRequestedArgs> TitleChangeRequested;
        til::typed_event<> RenameEnded;

        // Agentmaster (bookmark tags): the pointer entered/left one of the header's bookmark badges.
        // The Tab forwards these to the page, which shows the rich tag hover panel (the sessions
        // carrying that tag + their status, click == jump to the tab) anchored at the badge — a
        // clickable popup, which a ToolTip can never be. Begin carries the tag name + the badge
        // element (the page transforms it into its own Root() space for placement).
        til::event<winrt::delegate<winrt::hstring /*tag*/, winrt::Windows::UI::Xaml::UIElement /*anchor*/>> TagBadgeHoverBegin;
        til::event<winrt::delegate<>> TagBadgeHoverEnd;

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Title, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(double, RenamerMaxWidth, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::TerminalApp::TerminalTabStatus, TabStatus, PropertyChanged.raise);

    private:
        // Agentmaster (PENDING_INPUT.md): the UNSENT-DRAFT "3 dots" pulse below the status dot. Set up in
        // the constructor on the 3 named dot ellipses (by ref — no resource lookup) and started ONLY while
        // TabStatus.AgentPendingVisible is true. TabStatus is assigned by the Tab AFTER construction, so
        // _HookTabStatusForPending (re)subscribes to its PropertyChanged whenever TabStatus changes.
        // (The same one subscription also feeds the spark crown, the flash ring's blink and the bookmark-tag
        // badges below.)
        void _HookTabStatusForPending();
        // RENDERING.md §4c: also gated on this header being in the live tree (IsLoaded — re-evaluated on
        // Loaded / Unloaded), on a STARTED tab (a dormant tab's remembered draft shows still dots) and on
        // Windows' animation effects (AgentAnimationsEnabled). Never throws (logged).
        void _UpdatePendingAnimation();
        // UI_THREAD.md §10a #1: the pulse runs as three composition Opacity loops (AgentDotsPulse), NOT a
        // XAML storyboard. While ANY storyboard runs, XAML walks the window's whole composition tree on every
        // display frame (~2.7 ms a frame, 50-70% of the release UI thread with a handful of pulses running);
        // a composition loop costs the UI thread nothing per frame. The animations are built on the first
        // start, so a header that never pulses allocates nothing.
        AgentDotsPulse _pendingDotsPulse;
        // RENDERING.md §4c: whether _pendingDotsPulse is currently running — the change gate, because a
        // restart of a running pulse jumps the wave back to its start and the callers re-assert often.
        bool _pendingDotsRunning{ false };
        // Agentmaster (SPARK CROWN — the cache-warm hint): the glow breath + the three rising embers, set up
        // in the constructor beside the pending-dots pulse and run ONLY while TabStatus.AgentCacheWarmVisible
        // is true. Same start/stop discipline, and for the same reason: a cache stays warm for MINUTES, so an
        // always-on animation would keep the compositor busy across every recently-used tab in the fleet.
        // Fed by the one _HookTabStatusForPending subscription.
        // RENDERING.md §4c: also gated on IsLoaded and Windows' animation effects (a still crown = the steady
        // glow, embers hidden). Never throws (logged).
        void _UpdateCacheWarmAnimation();
        // UI_THREAD.md §10a #1: the glow breathes through a composition Opacity loop on the XAML glow grid
        // (0.45 -> 0.62 -> 0.28 -> 0.45, begun and ended on its still 0.45), and the embers are composition
        // visuals of our own on the sparks canvas (AgentEmberSparks) — a MOVING XAML element would make XAML
        // track it every frame for hit-testing, which is the per-frame UI-thread work this removes.
        AgentOpacityLoop _cacheWarmGlow;
        AgentEmberSparks _cacheWarmEmbers;
        // RENDERING.md §4c: whether the glow + embers are currently running — the same change gate.
        bool _cacheWarmRunning{ false };
        // UI_THREAD.md §10a #1: copy TabStatus.AgentCacheWarmBrush's color onto the embers. The XAML ellipses
        // they replace bound that brush in XAML; composition visuals cannot, so the TabStatus subscription
        // calls this on every brush change (TerminalPage::_SetTabCacheWarm sets the brush just before the
        // crown turns on, and again on a real color change), and _UpdateCacheWarmAnimation calls it before
        // a start. A null or non-solid brush keeps the embers' current color. Never throws (logged).
        void _SyncCacheWarmEmberColor() noexcept;
        // Agentmaster (tab status-dot RED FLASH RING, UI_THREAD.md §10i): blink the ring iff the page shows it
        // (TabStatus.AgentFlashRingVisible — set once when a flash starts, cleared when it ends) AND this header
        // is in the live tree (IsLoaded — re-evaluated on Loaded / Unloaded; a header the strip virtualized
        // away resumes in phase when it comes back). Deliberately NOT gated on Windows' animation effects or on
        // a started tab, unlike the pulses: the ring is an ALERT, the timer blink it replaces ran under every
        // setting, and a manual Mark Unread flashes a dormant tab too. Fed by the one _HookTabStatusForPending
        // subscription. Never throws (logged); a failed start leaves the ring shown steadily, never hidden.
        void _UpdateFlashRingAnimation();
        // The ring's blink: a composition expression on HeaderAgentStatusFlashRing's Opacity that follows the UI
        // thread's ONE shared blink clock (AgentLockstepBlink), so every flashing tab in every window blinks in
        // lockstep and nothing flips a XAML property per blink. Replaces the page timer's Visibility flip every
        // 600 ms, which cost a layout pass and a XAML frame each time. The dot's wrap grid is the anchor (used
        // only while the thread has no compositor cached; the ring itself must never get a hand-out visual).
        AgentLockstepBlink _flashRingBlink;
        // Whether _flashRingBlink is running — the change gate: the callers re-assert often, and each start is a
        // XAML commit.
        bool _flashRingBlinking{ false };
        winrt::TerminalApp::TerminalTabStatus _pendingHookedStatus{ nullptr };
        winrt::Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _pendingStatusRevoker{};

        // Agentmaster (bookmark tags): rebuild the HeaderTagBookmarks row — one small bookmark
        // Polygon per tag in TabStatus.AgentTagsSpec ('\n'-joined names), each filled with its
        // stable TagColorFor color and raising TagBadgeHoverBegin/End on pointer enter/leave (the
        // page's rich hover panel). Driven through the SAME TabStatus PropertyChanged subscription
        // as the pending-dots pulse; change-gated on _renderedTagsSpec so a re-assert of an
        // unchanged spec rebuilds nothing.
        void _UpdateTagBadges();
        // Position + show/hide the popup-hosted badge row: left == the title's first-character x
        // (fallback: just past the status-dot slot), top == the hosting TabViewItem's bottom edge
        // minus ~30% of the ribbon height — so ~70% of each ribbon OVERHANGS below the tab (the
        // popup root isn't clipped by the strip's ScrollViewer, which is what makes the overhang
        // renderable at all). Also: hides the popup when the tab is scrolled out of the strip
        // viewport (an unclipped popup would otherwise float over the caption buttons), and — since
        // an open popup does not reliably track its PARENT moving without a size change (tab
        // reorder, strip scroll) — close+reopens when the header's absolute position changed since
        // the last apply (offset changes alone reposition live). Triggers: every badge rebuild, the
        // root grid's SizeChanged, the TabViewItem's SizeChanged (the strip growing for ANOTHER
        // tab's wrapped title), the strip ScrollViewer's ViewChanged (scrolls), LayoutUpdated
        // (reorders), and Loaded (a restored tab's badges are asserted pre-tree; the open is gated
        // on rootedness). CYCLE SAFETY: _PositionTagBadges only SCHEDULES (coalesced,
        // _badgeReposQueued) — every trigger is layout-driven, and mutating the popup synchronously
        // inside a layout pass is the E_LAYOUTCYCLE fail-fast (the 2026-07-02 13:32/13:50 dumps);
        // _PositionTagBadgesNow does the actual reads + gated mutations on a clean dispatcher tick.
        // UI_THREAD.md §10a #2: a LayoutUpdated trigger (fromLayoutPass) schedules the placement only
        // when _BadgePlacementSignature differs from the one the last completed placement recorded —
        // layout passes run for ANY change on the UI thread (every window), and each placement creates
        // up to 4 TransformToVisual transforms whose release is a linear search in XAML's event manager
        // (~84 ms/s with 7 tagged tabs on the 550-tab release). Every other trigger always schedules.
        // Parameters: fromLayoutPass - true only from the LayoutUpdated hook (_UpdateBadgeLayoutHook);
        //             false (the default) for every explicit trigger, which skips the fingerprint check.
        // Never throws: a failed schedule is logged and dropped (the next trigger retries).
        void _PositionTagBadges(bool fromLayoutPass = false); // the coalescing scheduler — safe from ANY trigger
        void _PositionTagBadgesNow(); // the deferred work: resolve geometry, place, show/hide (clean tick only)
        bool _badgeReposQueued{ false }; // a reposition tick is pending — triggers coalesce onto it
        // UI_THREAD.md §10a #2: arm the header's LayoutUpdated hook only while the header shows badges
        // AND is in the live tree; disarm it otherwise. XAML calls EVERY LayoutUpdated handler of the
        // thread after EVERY layout pass, a header outside any tree included, so the old always-on
        // subscription made each layout pass in any window run ~550 header handlers (UI_THREAD.md §4c).
        // A loaded, tagged header still needs the hook: a tab REORDER moves the header without
        // resizing it, which no SizeChanged reports. Idempotent; disarming also drops the recorded
        // placement signature, so the next arm starts with a full placement. "In the live tree" is
        // IsLoaded(), never the event that called: XAML can raise a STALE Unloaded after the next Loaded
        // when an element is re-parented within one tick (a tab move), and trusting it would drop the
        // hook of a header that is still on screen.
        // Returns: true when the hook is armed after the call (tagged + in the live tree); the Unloaded
        // handler uses it to spot a stale Unloaded and re-place the badges it has just hidden.
        // Never throws: a failed read or subscribe is logged and leaves the hook as it was (the return
        // reports that state); explicit triggers still place the badges, only a reorder can then leave
        // them at a stale spot until the next trigger.
        bool _UpdateBadgeLayoutHook();
        winrt::Windows::UI::Xaml::FrameworkElement::LayoutUpdated_revoker _badgeLayoutUpdatedRevoker{};
        // UI_THREAD.md §10a #2: a cheap fingerprint of everything the placement reads, built from plain
        // property reads only — no TransformToVisual, so no transform whose release walks XAML's
        // event-manager list. Holds: whether the title gives the x; the popup's open state and the
        // open gate (IsLoaded + XamlRoot); the badge row's width; the header's height; then the layout
        // offset (UIElement::ActualOffset, x and y) of every element from the title (or the header grid,
        // when the title is hidden) up to the root, with the nearest TabViewItem's height and the
        // nearest ScrollViewer's scroll offsets and width (ActualOffset leaves out the scroll offset)
        // added where the walk meets them. Equal fingerprints mean the placement would produce the same
        // result, unless an ancestor's RenderTransform changed alone — nothing in the tab strip does
        // that; any explicit trigger (SizeChanged, ViewChanged, Loaded, a tag change) re-places anyway.
        // Safe to call inside a LayoutUpdated handler: it reads, it never writes.
        // Returns: the fingerprint; empty when a part could not be read (empty == "changed").
        // Never throws: a failed read is logged (throttled) and returns empty.
        std::vector<double> _BadgePlacementSignature() noexcept;
        // The _BadgePlacementSignature of the last COMPLETED placement — written at the end of
        // _PositionTagBadgesNow (after its popup open/close), cleared when it starts and when the hook is
        // disarmed. Empty means "no completed placement": the next layout-pass check schedules one.
        std::vector<double> _badgePlacedSignature;
        winrt::hstring _renderedTagsSpec;
        // The TabViewItem ancestor the badges are pinned to + its SizeChanged hook, and the tab
        // strip's ScrollViewer + its ViewChanged hook (both re-resolved fresh each position pass —
        // the weak refs only gate re-hooking, e.g. after a tab tears out into another window).
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::TabViewItem> _badgeTabViewItem;
        winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker _badgeTviSizeRevoker{};
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::ScrollViewer> _badgeStripScroller;
        winrt::Windows::UI::Xaml::Controls::ScrollViewer::ViewChanged_revoker _badgeSvViewChangedRevoker{};
        double _badgeLastAbsX{ -1e9 }; // the header grid's island-absolute position at the last apply —
        double _badgeLastAbsY{ -1e9 }; // a change means the PARENT moved => force a close+reopen

        bool _receivedKeyDown{ false };
        bool _renameCancelled{ false };
        // Agentmaster: set in PreviewKeyDown when the commit combo (Enter / Shift+Enter, per the
        // global mode) is pressed — where we also suppress the AcceptsReturn newline — and consumed
        // on the matching KeyUp to commit (the original upstream note warns that closing the box on a
        // *down* event lets the up bubble to the NewTabButton, so we defer the close to key-up).
        bool _commitOnKeyUp{ false };

        void _CloseRenameBox();

        // Agentmaster: keep the rename box's RIGHT border on-screen. The box is anchored at the tab's
        // left and grows rightward (NoWrap auto-size), so a long title — or RTL (e.g. Hebrew) text whose
        // start sits at the right edge — can push the box past the window's right edge, hiding the right
        // border (and the beginning of RTL text). _ApplyRenamerMaxWidth caps MaxWidth to the space from
        // the box's actual on-screen left to the window's right edge, so the box grows as large as it can
        // while staying fully visible.
        //
        // CYCLE SAFETY (this is what crashed the first attempt with a XAML layout-cycle / stowed
        // exception): the fit is recomputed at most ONCE per (rename, window width). Writing MaxWidth
        // reflows layout and re-fires SizeChanged; if we recomputed leftX every time, its sub-pixel
        // shift across passes made the cap oscillate and XAML aborted the layout pass. So _renamerFitDone
        // + _lastRootWidth gate the recompute: a SizeChanged driven by the box's own growth (window width
        // unchanged) is a no-op, breaking the feedback loop. A genuine window resize changes the width and
        // re-arms it. leftX is invariant to the box's own width (the tab's left depends only on the tabs
        // before it), so a single post-arrange measurement is correct for the whole rename.
        void _ApplyRenamerMaxWidth();
        bool _renamerFitDone{ false };
        double _lastRootWidth{ -1.0 };

        // Agentmaster (consistent multi-line tab-row height): the line count currently reserved by
        // HeaderTitleLineReserver, so ReserveTitleLines() no-ops on an unchanged count (TerminalPage
        // may push to every header on any title change; only a real change touches the tree).
        int32_t _reservedTitleLines{ 1 };
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TabHeaderControl);
}
