// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once

#include "winrt/Microsoft.UI.Xaml.Controls.h"

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
        // Agentmaster (PENDING_INPUT.md): the UNSENT-DRAFT "3 dots" pulse below the status dot. Built
        // imperatively in the constructor (so it targets the 3 named dot ellipses by ref — no resource
        // lookup) and started ONLY while TabStatus.AgentPendingVisible is true, so an idle fleet never
        // holds the compositor at 60fps. TabStatus is assigned by the Tab AFTER construction, so
        // _HookTabStatusForPending (re)subscribes to its PropertyChanged whenever TabStatus changes.
        // (The same one subscription also feeds the bookmark-tag badges below.)
        void _HookTabStatusForPending();
        void _UpdatePendingAnimation();
        winrt::Windows::UI::Xaml::Media::Animation::Storyboard _pendingDotsStoryboard{ nullptr };
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
        void _PositionTagBadges(); // the coalescing scheduler — safe from ANY trigger
        void _PositionTagBadgesNow(); // the deferred work: resolve geometry, place, show/hide (clean tick only)
        bool _badgeReposQueued{ false }; // a reposition tick is pending — triggers coalesce onto it
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
