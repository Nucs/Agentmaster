// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster SIDE PANEL — a WINDOW-CHROME fleet panel beside the terminal content.
// Part of the TerminalPage same-class partial group (the TabManagement.cpp pattern).
//
// WHAT: a non-terminal, non-tab pane docked LEFT (the default) or RIGHT of the tab content
// (ContentArea's 3-column grid in TerminalPage.xaml), split into two vertically-stacked areas with
// independent scrolling:
//   TOP    — the LIVE session CARDS (the Triage-Board integration re-hosted vertically): a search
//            box (the Sessions browser's ParseSessionQuery grammar over title / dir / branch / id)
//            over a scrollable card list, ordered by the SHARED AppSettings::boardSort (the header's
//            ⋯ ▸ Card order sets it through AgentManagerContent::SetBoardSort, so the board and this
//            list can never sort by different specs). A card single-click SELECTS (Linked
//            Lenses: it also drives the Manager lens), double-click ACTIVATES (jump to the tab,
//            cross-window), right-click shows THE session context menu — the exact lazily-filled
//            MenuFlyout the board card / tree row shows (AgentManagerContent::BuildSessionMenuFor).
//            The ⋯ menu's ⌖ item (GLOBAL AppSettings::sidePanelScrollToSelectedTab, default ON)
//            makes SELECTING A TAB scroll this list to that tab's card — one scroll per selection,
//            deferred to a clean tick, never moving a card that is already fully visible.
//   "⋯"   (SIDE_PANEL.md §2i) — the header's last button: a menu with the panel's settings and the
//            collapse (Dock on the left / right · Scroll to the selected tab · Card scope ▸ · Card
//            order ▸ · Collapse the side panel), rebuilt from the live state at every open.
//   BOTTOM — the SELECTED session's summary: a REAL AgentTabOverlay in fill-host mode
//            (SetSummaryFillHost), i.e. the per-tab pencil panel's whole business logic — analyze/
//            render off-thread, times bar, wrap/truncate/history toggles, JUMP buttons, context
//            menus — reused verbatim, just sized by this host instead of anchoring over a terminal.
//   "Sum" OFF (SIDE_PANEL.md §2d) — the header's first toggle hides the BOTTOM area (and its
//            splitter): the cards fill the body, and each card shows the RICH SESSION TOOLTIP a tab
//            header shows — the same AgentSessionToolTipHost lifecycle and the same card push
//            (TerminalPage::_PushSessionToolTipCard) — placed just past the panel's edge, beside the
//            hovered card (right of a left-docked panel, left of a right-docked one).
//   "+"   (SIDE_PANEL.md §2e) — the header's first button opens a LAUNCHER CARD at the click point,
//            extending toward the window's center: the Agent Manager's own launch row (the shared
//            AgentLaunchBar — the same box, drop-down, validation, hover menu, launch / resume / fork /
//            "Create & Launch"), Claude only, its box mirroring the Manager's box. Closed by a press
//            outside it, Esc, "+" again, a launch, a tab switch, a collapse or a dock flip.
//
// PERSISTENCE (the user's contract): both separators — the VERTICAL one (panel width) and the
// HORIZONTAL one (cards/summary split) — persist PER WINDOW (WindowRecord::sidePanel*, riding the
// window-record autosave), and EVERY adjustment additionally RMWs the GLOBAL AppSettings defaults
// ("the last one to resize sets the default for new windows"). Collapse follows the same rule; the
// dock side (LEFT by default; the ⋯ menu's Dock item flips it) is GLOBAL-only.
//
// PERF DISCIPLINE (the Manager-lens rebuild-tax lesson): list updates are COALESCED off the page's
// already-batched registry drain, min-gap throttled, and deferred entirely while collapsed; and
// since SIDE_PANEL.md §2c an update is IN PLACE — every session keeps ONE card for as long as it is
// listed, an update writes only the values that changed (a card model compared field by field), new
// cards are built only for sessions new to the list, and the order changes with the fewest
// removes / inserts / moves (AgentMaster/KeyedListDiff.h; a XAML Move is a plain reorder, no
// Unloaded/Loaded). The per-update reads are lean: SnapshotLiveForCards (no queues, no transcript
// texts), ONE dir-colors snapshot for every card's color (LoadDirColorIndex), and the bookmark tags
// cached per card until SessionTagsEpoch moves. The ~30s timing refresh runs the same update, so
// only the timing texts that changed are written; cards carry NO eager flyouts (the context menu is
// built lazily at right-click).
// Since SIDE_PANEL.md §2f the list is VIRTUALIZED: only the rows near the viewport have cards
// (AgentMaster/VirtualListWindow.h decides which; two spacers stand in for the rest, so the scroll
// extent never moves), and a card's rounded shapes are CHILDLESS layers behind its content — a
// rounded element with children is a composition node of its own, which every frame walks
// (UI_THREAD.md §10h / §14: two per card were 81% of production's nodes on 2026-10-20).
// ======================================================================================
//
// This file implements TerminalPage methods (same class, separate TU — the TabManagement.cpp
// pattern). It reuses AgentManagerContent.Internal.h (the board's ~48 anonymous-namespace helpers:
// StateColor/StateLabel/Text/Fill/TimingText/MakeSortKey/SortKeyLess/
// PointerStillWithin/HexToColor/PreferDarkTextOn/ApplyCursor/...) so the cards speak the exact
// visual + sort language of the Triage Board — except the card's STATUS ICON, which speaks the TAB
// STRIP's (AgentStatusGlyph, SIDE_PANEL.md §2g: the board's BuildPendingDots cluster is no longer
// used here; the draft dots ride the icon, under its dot). NOTE: that header requires the SAME file-scope
// using-directives the AgentManagerContent TUs replicate (narrow Color/ColorHelper/Colors
// using-DECLARATIONS — a `using namespace winrt::Windows::UI;` would pull the nested Text
// namespace over the Text() helper), which is why the using block below mirrors
// AgentManagerContent.Board.cpp rather than the other TerminalPage TUs.

#include "pch.h"
#include "TerminalPage.h"

#include "AgentManagerContent.h" // CycleBoardSort / BuildSessionMenuFor / SelectSession (the Linked-Lenses delegation targets) + LaunchBoxText / SetLaunchBoxText / LaunchRecentDirs (the "+" launcher mirrors its box)
#include "AgentLaunchBar.h" // the "+" launcher card's launch row — the Agent Manager's own launch bar, Claude only (SIDE_PANEL.md §2e)
#include "AgentModelMenu.h" // AgentFillModelPickItems — the AI search's closed-result menu: Fork here ▸ / Open New Session Here ▸ (SIDE_PANEL.md §2h)
#include "AgentSessionToolTip.h" // the cards' rich session tooltip host ("Sum" off — the tab tooltip's lifecycle, shared)
#include "AgentStatusGlyph.h" // every card's STATUS ICON — the tab's dot with its 3 dots, flash ring and spark crown (SIDE_PANEL.md §2g)
#include "AgentTabOverlay.h" // the fill-hosted summary panel (SetSummaryFillHost)
#include "AgentCatchLog.h" // AgentLogCaughtException — swallowed-exception forensics (Rule #18)
#include "AgentTipHelpers.h" // AgentSetTip / AgentSetTitledTip — the islands-safe hover tooltips
#include "AgentAutomation.h" // the panel's AutomationIds, its glyph buttons' names, the cards' ItemStatus (E2E_TESTING.md §6.3)
#include "AgentStatusColors.h" // PendingDotsColorFor / CacheWarmSparkColorFor — the status icon's contrast picks (the tab strip's rules, SIDE_PANEL.md §2g); AgentAnimationsEnabled
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / LogNav / ShortId
#include "AgentMaster/Engine.h" // BroadcastSettingsChanged — the ⌖ scroll-to-tab toggle reaches every OTHER open window live; CurrentUnreadSessionIds — which cards' status icons ring (SIDE_PANEL.md §2g)
#include "AgentMaster/KeyedListDiff.h" // PlanKeyedListOps / ApplyKeyedListOp — the card list's fewest removes / inserts / moves (SIDE_PANEL.md §2c)
#include "AgentMaster/LaunchPresetsOs.h" // PRESETS (HARNESSES.md): LaunchMenuEntries — the AI search's closed-result menu lists the models, then the visible presets (SIDE_PANEL.md §2h)
#include "AgentMaster/VirtualListWindow.h" // PlanRealizedRows / RowsAroundRow / SpacersFor / MedianRowHeight — which rows of the card list have cards (SIDE_PANEL.md §2f)
#include "AgentMaster/PointerCardPlacement.h" // PlacePointerCard — the "+" launcher card opens at the click, toward the window's center (SIDE_PANEL.md §2e)
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings + EffectiveWorkingDir / ResolveSessionColorHex + LoadDirColorIndex (one color snapshot per list update)
#include "AgentMaster/ProcessInspect.h" // ExternalClaudeRow (the Internal.h MakeSortKey overload's parameter type)
#include "AgentMaster/ProfileBootstrap.h" // (Internal.h convention — harmless here, keeps the include set board-identical)
#include "AgentMaster/SessionRegistry.h" // SnapshotLiveForCards / Get
#include "AgentMaster/SessionSearch.h" // ParseSessionQuery / MatchesQueryText / MatchesPathQuery — the Sessions grammar, reused
#include "AgentMaster/SessionSearchAgent.h" // SharedSessionSearchAgent / SessionSearchEvent / LoadSessionIndexEntryById — the side panel's AI search, a view of the process's one search conversation (SIDE_PANEL.md §2h)
#include "AgentMaster/SidePanelAiList.h" // SidePanelAiListStepFor / SidePanelAiListDiffers — the AI view's list changes only when a request ends (SIDE_PANEL.md §2h)
#include "AgentMaster/SidePanelAiCopy.h" // SidePanelAiRefillText / SidePanelAiCopyExchanges / BuildSidePanelAiCopyText — ⊘ and the copy button under it (SIDE_PANEL.md §2h)
#include "AgentMaster/SidePanelPins.h" // CompareSidePanelPins / NextSidePanelPinStamp / IsSidePanelPinStamp — Pin to Top: pinned cards first, the newest pin on top (SIDE_PANEL.md §2j)
#include "AgentMaster/SidePanelFilters.h" // the funnel's FILTERS: the facets, the matcher, the chips, the record's edits (SIDE_PANEL.md §2l)
#include "AgentMaster/SessionFolderFilter.h" // CollectSessionFolders / SortFoldersByUse — the folder card's "folders of the sessions" section (SIDE_PANEL.md §2l)
#include "AgentMaster/SessionStore.h" // GetSessionTags / LoadAllTagColors / SessionTagsEpoch — the cards' bookmark-tag strip (cached per card until a tag changes)

#include <algorithm>
#include <bit> // std::popcount — how many filters a facet mask holds (the build's log line, the [nav] lines; SIDE_PANEL.md §2l)
#include <chrono> // kCardTipDelay (the Internal.h card-tip hold-back) + the tip helpers' delay overrides
#include <cmath>
#include <cwctype> // towlower — the fold for the in-process matchers
#include <limits> // the chips row's wrap panel measures its children with an unbounded height (SIDE_PANEL.md §2l)
#include <mmsystem.h> // PlaySoundW — the chime every Agentmaster copy plays (the AI strip's copy button, SIDE_PANEL.md §2h)
#include <map> // the cached tag-color map (folded tag name -> "#AARRGGBB") the cards' ribbons resolve against
#include <shobjidl.h> // IFileOpenDialog — required BY AgentManagerContent.Internal.h's PickClaudeExe helper (per-TU anonymous-namespace copy)
#include <string_view> // the list update's duplicate-id belt (a set of views into the snapshot's ids)
#include <unordered_set>
#include <vector>

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
    // The thickness of both drag-to-resize separators: the VERTICAL one (its own ContentArea column, between the panel
    // and the terminal) and the HORIZONTAL one (its own body row, between the cards and the summary). 3px since
    // 2026-10-05 — at 6px (the size of the per-tab overlay's resize grips, which stay 6px) the two bars read as heavy
    // borders. Tradeoff: each bar is its own grab area, so the drag target shrank with it. No wider invisible hit area
    // was added: it would have to overlap the terminal's edge or the cards and take their pointer input (a text
    // selection that starts at the terminal's first column, a card click).
    constexpr double kSidePanelSplitterPx = 3.0;
    constexpr double kSidePanelStripPx = 20.0; // the collapsed expander strip's column width
    constexpr double kSidePanelMinPx = 180.0; // the panel never renders thinner than this (fractions aside) — the header row stays usable
    constexpr double kSidePanelMinWidthFrac = 0.10; // drag clamp — mirrors the Persistence load band
    constexpr double kSidePanelMaxWidthFrac = 0.50; // never more than half the content area
    constexpr double kSidePanelMinSplitFrac = 0.15; // cards area never squeezed below 15% of the panel
    constexpr double kSidePanelMaxSplitFrac = 0.85; // summary area never squeezed below 15% either
    // The header filter box's height FLOOR (DIPs), replacing the TextBox template's stock 32px
    // (TextControlThemeMinHeight) for that one box only: its natural slim height at FontSize 11 — one line
    // plus the tight 2/3 vertical padding plus the 1px borders — so it sits under the ~19px header buttons
    // without towering over them. A floor, deliberately NOT a fixed Height: the box's parts still measure
    // their own needs, so a larger text scale or DPI rounding grows the box instead of clipping it (a fixed
    // Height below a template part's floor would cut off the bottom border).
    constexpr double kSidePanelSearchBoxMinHeightPx = 22.0;
    constexpr uint64_t kSidePanelRebuildMinGapMs = 500; // registry-drain rebuild throttle (a storm folds into the periodic catch-up)
    constexpr uint64_t kSidePanelTimingRefreshMs = 30'000; // the in-place "ago" text refresh cadence

    // --- One card, and the row it takes in the list (SIDE_PANEL.md §2c / §2f) ---------------------------------------
    // A card's margins in the list: 4 DIPs at the sides, 2 above and 2 below (two cards stand 4 apart). The vertical
    // pair is part of the row height the virtualized list computes with (the stride = a card's height + both).
    constexpr double kSidePanelCardMarginX = 4.0;
    constexpr double kSidePanelCardMarginY = 2.0;
    // The card's constant 1px border (selection and hover recolor it, never resize it, so the list never jiggles
    // under the pointer). It is drawn by the card's CHILDLESS background, so the content sits this far inside it —
    // where the old Border placed its child.
    constexpr double kSidePanelCardBorderPx = 1.0;
    // The card's corner radius, and the band's top corners: they follow the card's INNER curve (the outer radius
    // minus the border), as they did when the card's Border clipped the band to that curve.
    constexpr double kSidePanelCardCornerRadius = 4.0;
    constexpr double kSidePanelBandCornerRadius = kSidePanelCardCornerRadius - kSidePanelCardBorderPx;
    // The title band's padding: the MARGIN of whatever the band area shows over the band (the title alone, or the
    // [title][tag strip] grid while tagged) — the band itself is childless, so it has no padding of its own.
    constexpr double kSidePanelBandPadX = 6.0;
    constexpr double kSidePanelBandPadY = 2.0;
    // The card's body color (#333333): its CHILDLESS background's fill, and so the background the state line — and on it
    // the status icon's 3 dots and spark crown (SIDE_PANEL.md §2g) — sit on. One place, so the indicators' contrast picks
    // (PendingDotsColorFor / CacheWarmSparkColorFor, the tab strip's own rules) always judge the color the card really has:
    // a tab picks against its rendered tab color, a card against this.
    constexpr Color kSidePanelCardBodyColor{ 0xFF, 0x33, 0x33, 0x33 };
    // The state line's gap above it (DIPs). 1, not the 3 it had before the status icon (§2g): the icon's 18 DIP cell
    // already holds 4 DIPs of air above its 10 DIP dot (the room the glow and the rising embers use), so the dot keeps
    // about the distance from the title band it had (5 DIPs, was ~5.6), and the card grows by ~6.7 DIPs instead of ~8.7.
    constexpr double kSidePanelStateLineTopPx = 1.0;
    // How far the status icon's root reaches past its slot on the LEFT and the RIGHT (a negative margin, DIPs): the
    // 18 DIP cell is 8 DIPs wider than the 10 DIP dot, so the dot's left edge stays aligned with the title's first letter
    // (both 6 DIPs in) and the label keeps its 5 DIP gap from the dot. The overhang stays inside the card (the state
    // line is 6 DIPs in), and nothing clips it: a negative margin only shrinks the slot, and XAML adds no layout clip
    // while the element's arranged size equals its unclipped desired size.
    constexpr double kSidePanelStatusIconOverhangPx = 4.0;
    // The status icon's pending band under its glyph cell (DIPs, AgentStatusGlyph's 4 DIP row): the label, the codex pill
    // and the timing text take it as a bottom margin, so their centers line up with the DOT's center (the middle of the
    // 18 DIP cell), not with the middle of the 22 DIP icon.
    constexpr double kSidePanelStatusIconBandPx = 4.0;
    // The row height the list computes with before any card was measured: a card plus its vertical margins at 100%
    // text size (the 12px title line + its 2px pads, the 22px status icon line + its 1px gap, the 10px dir line + its 4px
    // gap, the 1px border twice, the 2px margins twice: ~66.3 DIPs; ~59.6 before the status icon of §2g). Only the first
    // plan of a window uses it; the pass queued after the first layout measures the real cards (_SidePanelSyncRealizedCards).
    constexpr double kSidePanelCardDefaultStride = 66.0;
    // _SidePanelSyncRealizedCards' anchorRow when the plan follows the viewport (no row to realize around).
    constexpr size_t kSidePanelNoAnchorRow = SIZE_MAX;
    // The header's ICON buttons (+ launcher, ⋯ menu) and the collapsed strip's chevron: one size above the 11px
    // worded button (Sum), so the single-glyph controls read as clearly as the word beside them. One place, so the
    // header's icons and the strip can never drift apart.
    constexpr double kSidePanelIconFontSize = 12.0;
    // The room the card tooltip ("Sum" off) leaves between itself and the window edge, beyond the framework's own
    // gap: ToolTip placement inflates the dock rect by DEFAULT_MOUSE_OFFSET (20px, WinUI ToolTip_Partial.h) for a
    // hover-opened tooltip, so the card starts 20px past the panel's edge; another 8px keeps it off the far edge.
    constexpr double kSidePanelTipGapPx = 20.0;
    constexpr double kSidePanelTipEdgeMarginPx = 8.0;

    // The SURFACE KEY of a side-panel card's rich tooltip (the "Sum" off mode): the key its content signature and
    // wheel-scroll pieces live under in TerminalPage::_tabTooltipSig / _tabTooltipScroll. A tab's card uses the
    // bare session id there, and both surfaces of ONE session can be armed at once (its tab and its card host
    // separate card elements), so the card's key must never collide with an id: the U+001F prefix is a control
    // character no session id (a GUID, or our minted Codex handle) can contain.
    //   id      — the card's session id.
    //   returns — the card's surface key.
    //   throws  — std::bad_alloc on allocation failure only.
    std::wstring SidePanelToolTipKey(const std::wstring& id)
    {
        return std::wstring{ L"\x1F" L"sidepanel:" } + id;
    }

    // The two drag-to-resize separators paint a SOLID gray a touch lighter than the #2E2E2E panel
    // background, so each reads as a visible raised divider at rest instead of a see-through gap: the
    // previous ~9%-alpha white overlay was effectively transparent (and vanished entirely against a
    // dark terminal on the VERTICAL bar's terminal-facing edge, since an alpha blend takes the color
    // of whatever it sits on). Opaque + a single gray level (r==g==b) makes the bar look identical no
    // matter what backs it; the HOT shade (pointer-over / mid-drag) is brighter to signal the grab.
    // One place, so both splitters' initial paint AND their hover swaps can never disagree.
    constexpr uint8_t kSidePanelSplitterIdleGray = 0x3E; // #3E3E3E — resting divider, subtly lighter than the panel
    constexpr uint8_t kSidePanelSplitterHotGray = 0x60; // #606060 — the pointer-over / drag highlight

    // How long the pointer must REST on a drag separator before its tooltip opens: 7 s (2026-10-06), where
    // the panel's buttons wait 3 s (kSidePanelButtonTipDelay, below) and most other AgentSetTip tips open
    // after ~133 ms. Each separator is a thin bar the user aims at to drag it, and a tip that opened while
    // they aimed got in the way of the drag (the user's report); after 7 s the tip shows only for someone
    // who stops on the bar on purpose. Any move restarts the wait (AgentTipHelpers' rest rule). Applies to
    // these two bars only.
    // Tradeoff: a user who wants the explanation waits 7 s for it. A drag never shows it at all: the press
    // silences the tip until the drag ends (AgentSuppressTip in each PointerPressed, the text given back by
    // endWidthDrag / endSplitDrag), so a tip that was already open also closes when the drag starts.
    constexpr std::chrono::milliseconds kSidePanelSplitterTipDelay{ 7000 };

    // How long the pointer must REST on one of the side panel's BUTTONS before its tooltip opens: 3 s (2026-10-10,
    // the user's request), where most other AgentSetTip tips open after ~133 ms. The buttons: the header's "+" · Sum ·
    // ⋯, every item of the ⋯ menu (SIDE_PANEL.md §2i: the dock, ⌖, the scope and order submenus and their orders, the
    // collapse — the buttons they replaced waited 3 s, and a menu is crossed on the way to an item just the same), and
    // the collapsed strip's expand chevron. Every one of their tips is a paragraph, and at ~133 ms it opened over the
    // panel each time the pointer reached a button on its way to a click; after 3 s it shows only for someone who stops
    // on the button to read it. Any move restarts the wait (AgentTipHelpers' rest rule), and the delay is recorded on
    // the element, so the tip keeps it for the element's life.
    // ⚠ A NEW button in the panel, or a new ⋯ menu item, must pass this constant to its AgentSetTip /
    // AgentSetTitledTip, or its tip opens after ~133 ms.
    // The FILTER BOX waits the same 3 s (2026-11-06, the user's request: "the tooltip on the filter sessions text box must
    // appear after 3s, it appears almost instantly"): it spans the panel's whole width under the buttons, so the pointer
    // crosses it on its way to almost anything, and its paragraph opened over the cards each time. Both of its texts (the
    // filter's and the AI search's request line's, _SidePanelSetAiMode) pass the constant. It was left out before, as "a
    // text box, not a button".
    // Not covered on purpose: the cards (kCardTipDelay, 4 s), the two drag separators (kSidePanelSplitterTipDelay, 7 s), the
    // summary area's own buttons (the per-tab summary panel's, AgentTabOverlay — shared with every tab) and the "+" launcher
    // card (the Agent Manager's own launch bar, AgentLaunchBar — SIDE_PANEL.md §2e keeps the two surfaces identical).
    // Tradeoff: a first-time user waits 3 s for a button's explanation, or the filter box's grammar.
    constexpr std::chrono::milliseconds kSidePanelButtonTipDelay{ 3000 };

    // The two separators' tooltip texts, named once: each is set when its bar is built and set again when a
    // drag on the bar ends (the press emptied it).
    constexpr const wchar_t* kSidePanelWidthSplitterTip =
        L"Drag to resize the side panel.\nThis window remembers its own width; the last resize anywhere becomes the default for new windows.";
    constexpr const wchar_t* kSidePanelSplitSplitterTip =
        L"Drag to rebalance the cards list and the summary.\nThis window remembers its own split; the last resize anywhere becomes the default for new windows.";

    // --- The header's ⋯ MENU (SIDE_PANEL.md §2i) ------------------------------------------------------------------------
    // Its symbols, all from Segoe UI Symbol: the font the panel's glyph buttons draw from by fallback (the ⋯ itself, the
    // strip's chevron, and the ⌖ / ⇄ / « buttons the menu replaced), so each item keeps the symbol its button wore and the
    // two new ones look like them. ⧉ (two joined squares) stands for "windows" on the card scope, ⇅ for "sort" on the order.
    constexpr const wchar_t* kSidePanelMoreGlyph = L"\x22EF"; // ⋯ MIDLINE HORIZONTAL ELLIPSIS — the button's own face
    constexpr const wchar_t* kSidePanelDockGlyph = L"\x21C4"; // ⇄ — the dock flip
    constexpr const wchar_t* kSidePanelScrollToTabGlyph = L"\x2316"; // ⌖ POSITION INDICATOR — "find my tab's card"
    constexpr const wchar_t* kSidePanelScopeGlyph = L"\x29C9"; // ⧉ TWO JOINED SQUARES — this window / every window
    constexpr const wchar_t* kSidePanelOrderGlyph = L"\x21C5"; // ⇅ — the card order
    constexpr const wchar_t* kSidePanelCollapseLeftGlyph = L"\x00AB"; // « — collapse toward a LEFT dock
    constexpr const wchar_t* kSidePanelCollapseRightGlyph = L"\x00BB"; // » — collapse toward a RIGHT dock

    // One entry of the ⋯ ▸ Card order submenu.
    struct SidePanelOrderChoice
    {
        ::Agentmaster::ExplorerSort order; // the AppSettings::boardSort value a pick sets
        const wchar_t* label; // the item's text: the Triage Board sort button's word for the order (SidePanelOrderLabel)
        const wchar_t* tip; // what the order sorts by — SortKeyLess (AgentManagerContent.Internal.h) in words
    };
    // The five orders, in the board cycle's own sequence (_CycleBoardSort), so the submenu reads in the order the old
    // button's clicks visited them. BY PID is not offered: the board never cycles to it either (grouping by pid means
    // nothing once the board splits the cards across its state columns).
    constexpr SidePanelOrderChoice kSidePanelOrderChoices[] = {
        { ::Agentmaster::ExplorerSort::MostActive, L"MOST ACTIVE", L"Running sessions first, then the most recent activity of any kind \x2014 a tool call or a subagent's work moves a card up too." },
        { ::Agentmaster::ExplorerSort::MostRecent, L"MOST RECENT", L"By the last turn boundary only: the prompt you last sent or the reply that last finished. Tool calls and subagent work never move a card, and a running session is not lifted." },
        { ::Agentmaster::ExplorerSort::Newest, L"NEWEST", L"Newest conversation first, by when it was created. A session with no transcript yet counts as just created." },
        { ::Agentmaster::ExplorerSort::Oldest, L"OLDEST", L"Oldest conversation first, by when it was created." },
        { ::Agentmaster::ExplorerSort::Alpha, L"A\x2013Z", L"By title, A to Z; case does not matter." },
    };

    // The word for `order` on the ⋯ ▸ Card order header — the Triage Board sort button's mapping verbatim (this panel's own
    // order button carried the same until the ⋯ menu), so the board and the panel name one order with one word, and the
    // submenu checks the entry whose word the header shows.
    //   order   - AppSettings::boardSort.
    //   returns - the word. BY PID (a stray stored value no cycle produces) reads MOST ACTIVE, as on the board.
    //   throws  - nothing.
    const wchar_t* SidePanelOrderLabel(::Agentmaster::ExplorerSort order) noexcept
    {
        using ::Agentmaster::ExplorerSort;
        return (order == ExplorerSort::Newest)     ? L"NEWEST" :
               (order == ExplorerSort::Oldest)     ? L"OLDEST" :
               (order == ExplorerSort::Alpha)      ? L"A\x2013Z" :
               (order == ExplorerSort::MostRecent) ? L"MOST RECENT" : // the turn-boundary order
                                                     L"MOST ACTIVE"; // MostActive (the default) + a stray ByPid
    }

    // A ⋯ menu item's icon: `glyph` drawn in Segoe UI Symbol, left of the item's text. The family is named on purpose: a
    // FontIcon draws from the icon font by default (Segoe MDL2 Assets / Fluent Icons), which has none of these symbols.
    //   glyph   - one of the kSidePanel*Glyph symbols above.
    //   returns - a new FontIcon for MenuFlyoutItem::Icon (the item's 16 DIP icon box scales it to fit).
    //   throws  - hresult_error / bad_alloc from creating the element; the menu's Opening handler contains it.
    FontIcon SidePanelMenuGlyph(const wchar_t* glyph)
    {
        FontIcon icon{};
        icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ L"Segoe UI Symbol" });
        icon.Glyph(glyph);
        return icon;
    }

    // --- The AI SEARCH (SIDE_PANEL.md §2h) ------------------------------------------------------------------------------
    // The "AI" toggle sits INSIDE the slim filter box, at its right end (the Sessions page's idiom, scaled to the ~22 DIP
    // box): a fixed size, so the room the box must leave for it is known before anything is laid out. The reserve = the
    // gap between the text and the toggle + the toggle + its inset from the box's right edge. DIPs.
    constexpr double kSidePanelAiToggleWidth = 24.0;
    constexpr double kSidePanelAiToggleHeight = 16.0; // the box is ~22 DIPs high: 3 above and below
    constexpr double kSidePanelAiToggleInset = 3.0;
    constexpr double kSidePanelAiToggleGap = 2.0;
    constexpr double kSidePanelAiToggleReserve = kSidePanelAiToggleGap + kSidePanelAiToggleWidth + kSidePanelAiToggleInset;
    // The strip's plan / answer text scrolls inside this height, so a long descriptive answer (the prompt asks for up to
    // ~1,200 characters) never pushes the result cards out of a narrow panel.
    constexpr double kSidePanelAiStatusMaxHeight = 150.0;
    // The strip's loading icon (a WinUI ProgressRing in the state dot's place): 12 DIPs, with a 3 DIP gap to the text — the
    // dot's own 8 + 7 footprint, so the text never moves when one replaces the other — and 2 DIPs from the top, so the two
    // share their center (the dot's is 4 + 8 / 2 = 8 DIPs down).
    constexpr double kSidePanelAiSpinnerSize = 12.0;
    // The result cards' opacity while a request runs: they are the LAST answer's list until the request ends (the user's rule
    // of 2026-11-05, SidePanelAiList.h), still readable and clickable, but never mistaken for this request's result.
    constexpr double kSidePanelAiStaleListOpacity = 0.55;
    // The refresh cadence of the result cards while nothing owes one (the "ago" texts age): the session cards' own 30 s.
    constexpr uint64_t kSidePanelAiRefreshMs = 30'000;
    // The two meanings of the filter box, as its placeholder says them: filtering, or a request in the user's own words that
    // Enter sends.
    constexpr const wchar_t* kSidePanelFilterPlaceholder = L"filter sessions";
    constexpr const wchar_t* kSidePanelAiPlaceholder = L"describe a session, then press Enter";
    constexpr const wchar_t* kSidePanelFilterTipTitle = L"Filter the cards";
    constexpr const wchar_t* kSidePanelFilterTip =
        L"Whitespace-split terms AND-match a session's title, working directory, branch, or id.\n\"quoted phrase\" matches exactly; a pasted whole session-id GUID matches that session and its forks by identity.";
    constexpr const wchar_t* kSidePanelAiBoxTipTitle = L"AI search";
    constexpr const wchar_t* kSidePanelAiBoxTip =
        L"Write what you remember (\x201Cthe session where I fixed the tab drag crash\x201D) and press Enter: a Claude model searches every session and lists the ones that fit, each with the line of evidence it found. A follow-up refines the list (\x201Conly the open ones\x201D). Esc stops a request that runs.\n\nThe AI toggle at the right brings the session cards back, and the text in the box then filters them; the conversation is kept.";
    // A session with no live tab (closed in Agentmaster, or never opened in it): its card's dot and state label. The Idle
    // gray darkened, so "closed" never reads as a live session at rest.
    constexpr Color kSidePanelAiClosedColor{ 0xFF, 0x6A, 0x6A, 0x6A };
    // The why line's quote bar: the Sessions page's evidence-bar blue family, so the model's evidence reads the same on both.
    constexpr Color kSidePanelAiWhyBarColor{ 0xFF, 0x6B, 0x8F, 0xD6 };
    // The strip's "the agent is running" motion (AgentWorkingIndicator, the user's pick of 2026-11-05): its sweep host lies
    // along the strip's bottom edge, just inside the back Border's 1 DIP border line, and stops this far short of each end —
    // the strip's 6 DIP corner radius — because the back Border is childless (no composition node, so nothing clips the light
    // to the curve): the light runs on the straight part of the edge only.
    constexpr double kSidePanelAiSweepInsetX = 6.0;
    constexpr double kSidePanelAiSweepInsetBottom = 1.0;
    // The light's color: the strip's working blue (the busy dot's and the busy status ink's hue).
    constexpr Color kSidePanelAiWorkingColor{ 0xFF, 0x4E, 0xA1, 0xFF };
    // The copy button under ⊘ (the user's request of 2026-11-10, "a copy icon below the circle icon"): Segoe Fluent Icons'
    // Copy, two overlapping pages (U+E8C8; the same code point in Segoe MDL2 Assets, which kSidePanelFluentFont names after
    // it), drawn at the header's icon size (kSidePanelIconFontSize).
    constexpr const wchar_t* kSidePanelAiCopyGlyph = L"\xE8C8";

    // --- The FILTERS (SIDE_PANEL.md §2l) ------------------------------------------------------------------------------------
    // The funnel button's face: Segoe Fluent Icons' Filter (U+E71C; the same code point in Segoe MDL2 Assets, named after it
    // for a Windows 10 without the Fluent font). An icon button, the "+" size (kSidePanelIconFontSize).
    constexpr const wchar_t* kSidePanelFilterGlyph = L"\xE71C";
    // The "+" launcher's face (SIDE_PANEL.md §2e): Segoe Fluent Icons' Add (U+E710; the same code point in Segoe MDL2 Assets),
    // the funnel's own icon font — the user's pick of 2026-11-10 ("Both from the icon font") when the funnel and the "+" were
    // not one size: the two glyphs are drawn on one icon grid, so they read as a matched pair at one size.
    constexpr const wchar_t* kSidePanelAddGlyph = L"\xE710";
    constexpr const wchar_t* kSidePanelFluentFont = L"Segoe Fluent Icons, Segoe MDL2 Assets";
    // The icon font's glyphs the panel's rows and the chips wear (each also in Segoe MDL2 Assets).
    constexpr const wchar_t* kSidePanelFolderGlyph = L"\xE8B7"; // Folder — By Folder…, a folder chip
    constexpr const wchar_t* kSidePanelTagGlyph = L"\xE8EC"; // Tag — By Tag's section header
    constexpr const wchar_t* kSidePanelClockGlyph = L"\xE823"; // Recent (a clock) — Active within
    constexpr const wchar_t* kSidePanelSearchGlyph = L"\xE721"; // Search — the typed text's chip
    constexpr const wchar_t* kSidePanelClearGlyph = L"\xE894"; // Clear — "Clear all filters"
    // The panel's width (DIPs): its longest row ("Waiting for you" / a long tag name with a count) fits at the panel's font;
    // a longer tag name ends in "…". Fixed, so a pick that changes a count never resizes the card under the pointer.
    constexpr double kSidePanelFilterPanelWidth = 248.0;
    // A section's rows are indented by this much under its header (DIPs), so they read as the section's, like a submenu's.
    constexpr double kSidePanelFilterIndentPx = 14.0;
    // The panel's tooltips wait the header buttons' 3 s (kSidePanelButtonTipDelay): the rows are crossed on the way to a pick.
    // The chips row: a chip's height-defining font (10.5 DIPs, a hair under the card's 11 so the row stays slim), the gap
    // between two chips and between two lines of chips (DIPs), and the chip's rounded corners.
    constexpr double kSidePanelChipFontSize = 10.5;
    constexpr double kSidePanelChipGapX = 4.0;
    constexpr double kSidePanelChipGapY = 3.0;
    constexpr double kSidePanelChipCornerRadius = 9.0;
    // The "Clear" link's color: the AI strip's working blue family, the panel's one link-like text.
    constexpr Color kSidePanelLinkColor{ 0xFF, 0x7F, 0xB5, 0xFF };
    // By Folder's hover SUBMENU (SIDE_PANEL.md §2l "The By Folder submenu"): its width (DIPs). A folder line holds the leaf,
    // "(exactly)", the dim whole path and a count; a longer path ends in "…" (the line's tooltip has it whole). Fixed, so a
    // click that changes a count never resizes the card under the pointer.
    constexpr double kSidePanelFolderMenuWidth = 400.0;
    // The submenu card's chrome above its first row (DIPs): its 1 DIP border and 4 DIP top padding. The placement lifts the
    // card by this much, so Pick… sits level with By Folder… (both cards share the look).
    constexpr double kSidePanelFolderMenuChromeTop = 5.0;
    // The hover delay when Windows' own menu delay cannot be read (ms): Windows' default MenuShowDelay.
    constexpr DWORD kSidePanelFolderMenuDefaultDelayMs = 400;
    // The longest hover delay honored (ms): a larger MenuShowDelay (a hand-edited registry) reads as this.
    constexpr DWORD kSidePanelFolderMenuMaxDelayMs = 5000;
    // The ink of a folder line's path: dim beside the leaf (the mockup's #8C8C8C).
    constexpr Color kSidePanelFolderMenuPathColor{ 0xFF, 0x8C, 0x8C, 0x8C };

    // The hover delay of By Folder's submenu: Windows' own menu show delay (SPI_GETMENUSHOWDELAY — what every desktop menu
    // waits before a resting pointer opens or leaves a submenu; Control Panel's MenuShowDelay), so the panel's submenu feels
    // like the user's other menus. Read at each arm (one cheap call), so a changed setting applies at once.
    //   returns - the delay, at most kSidePanelFolderMenuMaxDelayMs.
    //   throws  - nothing.
    std::chrono::milliseconds SidePanelFolderMenuDelay() noexcept
    {
        DWORD ms = kSidePanelFolderMenuDefaultDelayMs;
        if (!::SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &ms, 0))
        {
            ms = kSidePanelFolderMenuDefaultDelayMs; // unreadable: Windows' default
        }
        return std::chrono::milliseconds{ std::min(ms, kSidePanelFolderMenuMaxDelayMs) };
    }

    // Write a folder line's words into its label: the leaf in SemiBold — "(exactly)" after it without subfolders, the chip's
    // words (SidePanelFolderLineName) — then the whole path in a dim, smaller ink. ONE TextBlock of two runs, so the
    // TextBlock's own trimming ends a long path in "…" (a StackPanel of two blocks would never trim).
    //   label             - the line's TextBlock.
    //   path              - the folder.
    //   includeSubfolders - the line's choice.
    //   throws            - hresult_error / bad_alloc from the XAML calls (the callers contain it).
    void SidePanelSetFolderMenuLabel(const TextBlock& label, const std::wstring& path, bool includeSubfolders)
    {
        label.Inlines().Clear();
        winrt::Windows::UI::Xaml::Documents::Run leaf{};
        leaf.Text(winrt::hstring{ SidePanelFolderLineName(path, includeSubfolders) });
        leaf.FontWeight(FontWeights::SemiBold());
        winrt::Windows::UI::Xaml::Documents::Run where{};
        where.Text(winrt::hstring{ L"   " + path });
        where.FontSize(11.5);
        where.Foreground(SolidColorBrush{ kSidePanelFolderMenuPathColor });
        label.Inlines().Append(leaf);
        label.Inlines().Append(where);
    }

    // The words a screen reader and the e2e driver hear for a folder line: the line's own words, then the whole path (two
    // folders of one leaf in different places must not sound alike).
    //   path              - the folder.
    //   includeSubfolders - the line's choice.
    //   returns           - the accessible name.
    //   throws            - std::bad_alloc.
    std::wstring SidePanelFolderMenuLineAccessibleName(const std::wstring& path, bool includeSubfolders)
    {
        return SidePanelFolderLineName(path, includeSubfolders) + L", " + path;
    }

    // The funnel panel's "By Folder…" row, the submenu's anchor and the keyboard's way back from it.
    //   rows    - the panel's rows (_sidePanelFilterRows).
    //   returns - its Button; null while the panel has no such row (never built, or being rebuilt).
    //   throws  - nothing (a projected object's copy is an AddRef).
    Button SidePanelByFolderButton(const std::vector<winrt::TerminalApp::implementation::AgentSidePanelFilterRowView>& rows) noexcept
    {
        for (const auto& row : rows)
        {
            if (row.kind == winrt::TerminalApp::implementation::AgentSidePanelFilterRowView::Kind::ByFolder)
            {
                return row.button;
            }
        }
        return nullptr;
    }

    // The pure line (SidePanelFilters.h) a submenu row stands for: the row view keeps the line's facts in plain members,
    // because TerminalPage.h does not include SidePanelFilters.h.
    //   row     - the submenu row (a Folder row; a Pick row gives an empty line).
    //   returns - the line.
    //   throws  - std::bad_alloc.
    SidePanelFolderMenuLine SidePanelLineOf(const winrt::TerminalApp::implementation::AgentSidePanelFolderMenuRowView& row)
    {
        return SidePanelFolderMenuLine{ row.path, row.key, row.includeSubfolders, row.checked };
    }

    // A Segoe Fluent Icons glyph as a TextBlock (a TextBlock, not a FontIcon: its line box matches the text beside it, the
    // launch bar's Browse… row idiom).
    //   glyph   - the code point (one of the kSidePanel*Glyph above).
    //   size    - its font size (DIPs).
    //   opacity - its opacity.
    //   returns - the element.
    //   throws  - hresult_error / bad_alloc from creating it (the callers' builds contain it).
    TextBlock SidePanelFluentGlyph(const wchar_t* glyph, double size, double opacity)
    {
        auto t = Text(glyph, size, false, opacity);
        t.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ kSidePanelFluentFont });
        return t;
    }

    // The face of the header's two ICON buttons, the "+" launcher and the funnel — the user's report of 2026-11-10, "the
    // filter icon is not same size as the + button", and their pick "Both from the icon font": the Segoe Fluent Icons glyph
    // at the header's icon size, centered over an INVISIBLE "+" in the button's own text font.
    //   - One width: both glyphs are one-em squares of the icon font (12 x 12 DIPs at 12 px), whichever glyph shows.
    //   - One height, the text buttons' (⋯, Sum): the icon font's line box is only one em tall, the text font's is 1.33 em
    //     (Segoe UI: ~16 DIPs at 12 px), and a button grows with its content — so the invisible "+" lends both faces the text
    //     line's height. Measured from the font files (font_boxes.py, SIDE_PANEL.md §2l): before, the "+" button was
    //     22.2 x 20 DIPs (the text "+") and the funnel 25 x 15 (an 11 px icon); with this face both are 26 x 20.
    //   - Scaled with the text: the sizer is text too, so a Windows text size change grows both faces alike — never a fixed
    //     number of DIPs that a larger text size would overflow.
    //   glyph   - the icon's code point (kSidePanelAddGlyph, kSidePanelFilterGlyph).
    //   returns - the face (a Grid): the button's Content. Its foreground follows the button's state (nothing local is set).
    //   throws  - hresult_error / bad_alloc from building the elements (the panel's build contains it).
    Grid SidePanelIconButtonFace(const wchar_t* glyph)
    {
        Grid face{};
        // The sizer inherits the button's FontSize (kSidePanelIconFontSize) and its theme text font: exactly what the old
        // "+" text was drawn in. Opacity 0 keeps its layout box (Collapsed would drop it); it never takes a press; and it is
        // out of UI Automation's content and control views, so a screen reader never reads a "+" nobody can see.
        TextBlock sizer{};
        sizer.Text(L"+");
        sizer.Opacity(0.0);
        sizer.IsHitTestVisible(false);
        sizer.HorizontalAlignment(HorizontalAlignment::Center);
        sizer.VerticalAlignment(VerticalAlignment::Center);
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetAccessibilityView(sizer, winrt::Windows::UI::Xaml::Automation::Peers::AccessibilityView::Raw);
        face.Children().Append(sizer);
        auto icon = SidePanelFluentGlyph(glyph, kSidePanelIconFontSize, 1.0);
        icon.HorizontalAlignment(HorizontalAlignment::Center);
        icon.VerticalAlignment(VerticalAlignment::Center);
        // A private-use code point reads as nothing useful aloud: the button's own name speaks for it.
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetAccessibilityView(icon, winrt::Windows::UI::Xaml::Automation::Peers::AccessibilityView::Raw);
        face.Children().Append(icon);
        return face;
    }

    // The CHIPS ROW's layout: a code-only Panel that lays its children out left to right and WRAPS onto a new line when the
    // next one does not fit — UWP XAML has no WrapPanel, and a horizontal StackPanel would cut the last chips off on a narrow
    // panel. Every child keeps its desired size (a chip is as wide as its words, a long folder chip ends in "…" by its own
    // MaxWidth); a child wider than the whole line gets a line of its own at the line's width.
    // Code-only (no IDL, never named in markup — the AgentTranscriptChunkPanel idiom): winrt::make returns it as a Panel.
    struct AgentSidePanelWrapPanel : winrt::Windows::UI::Xaml::Controls::PanelT<AgentSidePanelWrapPanel>
    {
        // Measure every child against the line's width and stack the lines.
        //   available - the room the row gives (its width bounds a line; the height is unbounded in practice).
        //   returns   - the widest line and the lines' total height.
        //   throws    - nothing of its own; a child's Measure that throws fails XAML's layout pass (never seen: plain chips).
        winrt::Windows::Foundation::Size MeasureOverride(const winrt::Windows::Foundation::Size& available)
        {
            const float lineWidth = available.Width;
            const winrt::Windows::Foundation::Size childAvailable{ lineWidth, std::numeric_limits<float>::infinity() };
            float x = 0.0f;
            float y = 0.0f;
            float lineHeight = 0.0f;
            float widest = 0.0f;
            for (const auto& child : Children())
            {
                child.Measure(childAvailable);
                const auto desired = child.DesiredSize();
                const float w = std::min(desired.Width, lineWidth);
                if (x > 0.0f && x + w > lineWidth)
                {
                    // The next line. Never before the line's first child: a child wider than the line still gets placed.
                    y += lineHeight + static_cast<float>(kSidePanelChipGapY);
                    x = 0.0f;
                    lineHeight = 0.0f;
                }
                x += w;
                widest = std::max(widest, x);
                x += static_cast<float>(kSidePanelChipGapX);
                lineHeight = std::max(lineHeight, desired.Height);
            }
            return winrt::Windows::Foundation::Size{ widest, y + lineHeight };
        }

        // Arrange every child where MeasureOverride's walk put it, against the final width.
        //   finalSize - the room the row arranged the panel in.
        //   returns   - finalSize (the panel takes what it was given).
        //   throws    - nothing of its own (see MeasureOverride).
        winrt::Windows::Foundation::Size ArrangeOverride(const winrt::Windows::Foundation::Size& finalSize)
        {
            const float lineWidth = finalSize.Width;
            // Two passes over each line: one to learn the line's height (children are vertically centered in it), one to
            // place. A line is the run of children MeasureOverride's rule puts together.
            std::vector<winrt::Windows::UI::Xaml::UIElement> line;
            float x = 0.0f;
            float y = 0.0f;
            float lineHeight = 0.0f;
            const auto place = [&]() {
                float cx = 0.0f;
                for (const auto& child : line)
                {
                    const auto desired = child.DesiredSize();
                    const float w = std::min(desired.Width, lineWidth);
                    const float top = y + (lineHeight - desired.Height) / 2.0f;
                    child.Arrange(winrt::Windows::Foundation::Rect{ cx, top, w, desired.Height });
                    cx += w + static_cast<float>(kSidePanelChipGapX);
                }
            };
            for (const auto& child : Children())
            {
                const auto desired = child.DesiredSize();
                const float w = std::min(desired.Width, lineWidth);
                if (x > 0.0f && x + w > lineWidth)
                {
                    place();
                    y += lineHeight + static_cast<float>(kSidePanelChipGapY);
                    line.clear();
                    x = 0.0f;
                    lineHeight = 0.0f;
                }
                line.push_back(child);
                x += w + static_cast<float>(kSidePanelChipGapX);
                lineHeight = std::max(lineHeight, desired.Height);
            }
            place();
            return finalSize;
        }
    };

    // The words for the scope at the end of a count ("in this window" / "across all windows") — the old count line's, kept
    // as the filter box's ItemStatus and in the chips row's count tooltip.
    //   global  - the panel lists every window's sessions.
    //   returns - the words, with their leading space.
    //   throws  - nothing.
    const wchar_t* SidePanelScopeWords(bool global) noexcept
    {
        return global ? L" across all windows" : L" in this window";
    }

    // Is a facet bit set in a mask? (The panel's expanded sections are kept as SidePanelFacetBit bits.)
    //   mask    - the bits.
    //   facet   - the facet asked about.
    //   returns - true when its bit is set.
    //   throws  - nothing.
    bool SidePanelHasFacet(uint32_t mask, ::Agentmaster::SidePanelFacet facet) noexcept
    {
        return (mask & ::Agentmaster::SidePanelFacetBit(facet)) != 0;
    }

    // The two texts one row of the funnel's panel shows from the record and the census: its CHECK column (a pick row: "✓" or
    // nothing; an Active within row: "●" for the window in effect, "○" for the others; a section header: "⌄" expanded, "›"
    // folded) and its COUNT column (how many sessions would be listed with that pick, every other facet as it is; a section
    // header: what is picked in it). By Folder… and Clear show neither. One function for the build and the in-place refresh,
    // so the two can never write different texts for one state.
    //   row      - the row (what it stands for).
    //   census   - the counts (_SidePanelFilterCensus).
    //   filters  - the window's record.
    //   expanded - the panel's expanded sections (SidePanelFacetBit bits).
    //   returns  - { check, count }.
    //   throws   - std::bad_alloc.
    std::pair<std::wstring, std::wstring> SidePanelFilterRowTexts(const winrt::TerminalApp::implementation::AgentSidePanelFilterRowView& row,
                                                                  const winrt::TerminalApp::implementation::AgentSidePanelFilterCensus& census,
                                                                  const ::Agentmaster::SidePanelFilters& filters,
                                                                  uint32_t expanded)
    {
        using Kind = winrt::TerminalApp::implementation::AgentSidePanelFilterRowView::Kind;
        using ::Agentmaster::SidePanelFacet;
        const auto mark = [](bool on) { return std::wstring{ on ? L"\x2713" : L"" }; }; // ✓
        switch (row.kind)
        {
        case Kind::ByFolder:
            // The folders already picked, as the count ("2"): the folder card adds one more each time.
            return { std::wstring{}, filters.folders.empty() ? std::wstring{} : std::to_wstring(filters.folders.size()) };
        case Kind::Section:
        {
            const std::wstring chevron = SidePanelHasFacet(expanded, row.facet) ? L"\x2304" : L"\x203A"; // ⌄ / ›
            std::wstring picked;
            if (row.facet == SidePanelFacet::State)
            {
                const auto n = std::popcount(filters.states & ::Agentmaster::kSidePanelAllStateBits);
                picked = n ? std::to_wstring(n) + L" on" : L"";
            }
            else if (row.facet == SidePanelFacet::Tag)
            {
                const size_t n = filters.tags.size() + (filters.untagged ? 1u : 0u);
                picked = n ? std::to_wstring(n) + L" on" : L"";
            }
            else if (row.facet == SidePanelFacet::Active && filters.activeWithinMinutes != 0)
            {
                picked = ::Agentmaster::SidePanelActiveChipText(filters.activeWithinMinutes).substr(std::wstring_view{ L"active in " }.size());
            }
            return { chevron, picked };
        }
        case Kind::State:
        {
            for (size_t i = 0; i < std::size(::Agentmaster::kSidePanelStateChoices); ++i)
            {
                if (::Agentmaster::kSidePanelStateChoices[i].bit == row.stateBit)
                {
                    return { mark((filters.states & row.stateBit) != 0), std::to_wstring(census.states[i]) };
                }
            }
            return { mark((filters.states & row.stateBit) != 0), std::wstring{} };
        }
        case Kind::Tag:
        {
            const std::wstring folded = ::Agentmaster::FoldTagName(row.tag);
            const bool on = std::any_of(filters.tags.begin(), filters.tags.end(), [&](const std::wstring& t) { return ::Agentmaster::FoldTagName(t) == folded; });
            size_t count = 0;
            for (const auto& [name, n] : census.tags)
            {
                if (::Agentmaster::FoldTagName(name) == folded)
                {
                    count = n;
                    break;
                }
            }
            return { mark(on), std::to_wstring(count) };
        }
        case Kind::Untagged:
            return { mark(filters.untagged), std::to_wstring(census.untagged) };
        case Kind::Flag:
            switch (row.facet)
            {
            case SidePanelFacet::Draft:
                return { mark(filters.draft), std::to_wstring(census.draft) };
            case SidePanelFacet::Unread:
                return { mark(filters.unread), std::to_wstring(census.unread) };
            case SidePanelFacet::Pinned:
                return { mark(filters.pinned), std::to_wstring(census.pinned) };
            case SidePanelFacet::Favorite:
                return { mark(filters.favorite), std::to_wstring(census.favorite) };
            default:
                return {};
            }
        case Kind::Active:
        {
            const bool on = filters.activeWithinMinutes == row.minutes;
            size_t count = census.active[0];
            for (size_t i = 0; i < std::size(::Agentmaster::kSidePanelActiveChoices); ++i)
            {
                if (::Agentmaster::kSidePanelActiveChoices[i].minutes == row.minutes)
                {
                    count = census.active[i + 1];
                }
            }
            return { std::wstring{ on ? L"\x25CF" : L"\x25CB" }, std::to_wstring(count) }; // ● / ○ — one window at a time
        }
        case Kind::Clear:
        default:
            return {};
        }
    }

    // Are two rows of the funnel's panel the SAME pick (the kind and what it stands for)? A refill (a section expanded, a tag
    // that appeared) rebuilds every row; the focus goes back to the row equal to the one that had it.
    //   a, b    - the rows.
    //   returns - true when they stand for the same thing.
    //   throws  - nothing.
    bool SidePanelSameFilterRow(const winrt::TerminalApp::implementation::AgentSidePanelFilterRowView& a,
                                const winrt::TerminalApp::implementation::AgentSidePanelFilterRowView& b) noexcept
    {
        return a.kind == b.kind && a.facet == b.facet && a.stateBit == b.stateBit && a.minutes == b.minutes && a.tag == b.tag;
    }

    // The first element named `name` in the visual tree under `root`, depth first — a template part of the filter box.
    //   root    - where to look; its own name is not tested.
    //   name    - the x:Name to find.
    //   returns - the element, or null (the template is not applied yet, or it has no such part).
    //   throws  - what the visual tree calls throw (hresult_error); the one caller contains it.
    // The tree under a TextBox is a dozen elements, so the walk is cheap.
    FrameworkElement SidePanelFindDescendant(const DependencyObject& root, std::wstring_view name)
    {
        const int count = VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < count; ++i)
        {
            const auto child = VisualTreeHelper::GetChild(root, i);
            if (const auto element = child.try_as<FrameworkElement>(); element && std::wstring_view{ element.Name() } == name)
            {
                return element;
            }
            if (auto found = SidePanelFindDescendant(child, name))
            {
                return found;
            }
        }
        return nullptr;
    }

    // `text` without the white space at its two ends (a request is sent trimmed; a box of spaces sends nothing).
    //   text    - the box's text.
    //   returns - the trimmed copy.
    //   throws  - std::bad_alloc only.
    std::wstring SidePanelAiTrim(std::wstring_view text)
    {
        const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        size_t a = 0;
        size_t b = text.size();
        while (a < b && isSpace(text[a]))
        {
            ++a;
        }
        while (b > a && isSpace(text[b - 1]))
        {
            --b;
        }
        return std::wstring{ text.substr(a, b - a) };
    }

    // The color a RESULT card's state shows — its dot, its label, its hover and selection ring: the live state's color for a
    // session with a tab, the "closed" gray for one without (SIDE_PANEL.md §2h).
    //   view    - the result card (its `open` flag and the state of its model).
    //   returns - the color. Never throws.
    Color SidePanelAiRingColor(const winrt::TerminalApp::implementation::AgentSidePanelAiResultView& view) noexcept
    {
        return view.open ? StateColor(view.base.model.state) : kSidePanelAiClosedColor;
    }

    // A RESULT card's ITEM STATUS (E2E_TESTING.md §6.3), written on its title like a session card's: the state word — or
    // "closed" for a session with no tab — then its tags, then "selected". The session card's own composer
    // (SidePanelApplyCardItemStatus) would read a closed session as "idle", which is not what the card shows.
    //   view     - the result card; a card with no title yet is skipped.
    //   selected - the card's session is the side panel's selected session.
    // Throws what a XAML property write throws (the callers contain it).
    void SidePanelAiApplyItemStatus(const winrt::TerminalApp::implementation::AgentSidePanelAiResultView& view, bool selected)
    {
        namespace impl = winrt::TerminalApp::implementation;
        if (!view.base.title)
        {
            return;
        }
        std::wstring tags;
        for (const auto& tag : view.base.model.tags)
        {
            tags += tags.empty() ? L"tags: " : L", ";
            tags += tag;
        }
        impl::AgentItemStatus status;
        status.Add(view.open ? impl::AgentStateWord(view.base.model.state) : std::wstring_view{ L"closed" })
            .Add(impl::kAgentStatusCodex, view.base.model.codex)
            .Add(impl::kAgentStatusPinnedToTop, view.base.model.pinned) // SIDE_PANEL.md §2j: the band's pushpin, in words
            .Add(tags)
            .Add(impl::kAgentStatusSelected, selected);
        impl::AgentSetItemStatus(view.base.title, status.Text());
    }

    // The sessions the AI view lists NOW, as the copy button names them (SIDE_PANEL.md §2h "⊘ and the copy button",
    // AgentMaster/SidePanelAiCopy.h): the list on screen in the model's order, each session's facts by the rules its result
    // card follows (TerminalPage::_SidePanelAiResultModelFor) — the registry's record when it has one (the managed title, the
    // effective working dir), else the agent's index entry (the transcript's title and folder) — its state "open · <the card's
    // state word>" for a session with a live tab and "closed" (the card's gray word) for any other, and the model's why.
    //   ids      - the view's list (_sidePanelAiShownIds); an empty or repeated id is skipped, as the result list skips it.
    //   reasons  - the view's whys by id (_sidePanelAiReasons; "" is never stored there).
    //   entries  - the view's index entries by id (_sidePanelAiEntries).
    //   registry - the shared registry, or null: every session then reads from its index entry alone.
    //   mode     - AppSettings::tabColorMode, which decides the effective working dir (the inferred folder while it infers).
    //   returns  - one row per listed session, in the list's order.
    //   throws   - std::bad_alloc.
    std::vector<::Agentmaster::SidePanelAiCopyListRow> SidePanelAiCopyRowsFor(const std::vector<std::wstring>& ids,
                                                                              const std::unordered_map<std::wstring, std::wstring>& reasons,
                                                                              const std::unordered_map<std::wstring, ::Agentmaster::SessionIndexEntry>& entries,
                                                                              const ::Agentmaster::SessionRegistry* registry,
                                                                              ::Agentmaster::TabColorMode mode)
    {
        std::vector<::Agentmaster::SidePanelAiCopyListRow> rows;
        rows.reserve(ids.size());
        std::unordered_set<std::wstring> seen; // the result list's own belt (_SidePanelAiRebuildResults): one row per session
        for (const auto& id : ids)
        {
            if (id.empty() || !seen.insert(id).second)
            {
                continue;
            }
            ::Agentmaster::SidePanelAiCopyListRow row;
            row.id = id;
            // Without the prompt queue: the costliest part of a record copy, which the copy never reads (UI_THREAD.md §10a #4).
            const auto lean = registry ? registry->GetWithoutQueue(id) : std::nullopt;
            const auto entryIt = entries.find(id);
            const ::Agentmaster::SessionIndexEntry* const entry = entryIt != entries.end() ? &entryIt->second : nullptr;
            if (lean)
            {
                row.title = lean->title;
                row.state = lean->live ? (std::wstring{ L"open \x00B7 " } + std::wstring{ StateLabel(lean->state) }) : std::wstring{ L"closed" };
                row.folder = ::Agentmaster::EffectiveWorkingDir(mode, *lean);
                row.branch = !lean->branch.empty() ? lean->branch : (entry ? entry->stats.gitBranch : std::wstring{});
            }
            else
            {
                row.state = L"closed"; // no record: a session Agentmaster never opened — the card's gray word all the same
                if (entry)
                {
                    row.title = ::Agentmaster::SessionSearchTitleOf(*entry);
                    row.folder = ::Agentmaster::SessionSearchDirOf(*entry);
                    row.branch = entry->stats.gitBranch;
                }
            }
            if (const auto why = reasons.find(id); why != reasons.end())
            {
                row.why = why->second;
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

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
    //   cardBack   — the card's CHILDLESS rounded background (AgentSidePanelCardView::cardBack), which draws the
    //                border; a null element is a no-op (a card torn down mid-event).
    //   stateColor — the color of the state the card shows.
    //   selected   — the card is the selected one.
    //   hover      — the pointer is on the card.
    // Throws winrt::hresult_error only when XAML refuses the property write (a torn-down tree) — the list update's
    // catch resets the list; the pointer handlers run it on a live card.
    void SidePanelStyleCardBorder(const Border& cardBack, const Color& stateColor, bool selected, bool hover)
    {
        if (!cardBack)
        {
            return;
        }
        if (selected)
        {
            cardBack.BorderBrush(SolidColorBrush{ stateColor });
        }
        else if (hover)
        {
            Color c = stateColor;
            c.A = 0x80;
            cardBack.BorderBrush(SolidColorBrush{ c });
        }
        else
        {
            cardBack.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x00, 0, 0, 0) });
        }
    }

    // Agentmaster (accessibility, E2E_TESTING.md §6.3, 2026-10-23): a card's ITEM STATUS, the words for what its status
    // icon, its border and its tag strip show — written on its TITLE, because the card itself (a Grid over childless
    // Borders) has no automation peer. Called by every path that changes one of them: the model apply (state, draft,
    // unread ring, warm cache, tags) and the selection restyle (the border). AgentAutomation.h's words in its order, so
    // the card reads like the same session's tab and board card: "running; unsent draft; tags: release; selected".
    // Before, the side panel showed all of it as pixels only, and the e2e suite read the selection from its effects.
    //   view     - the card; a card with no title yet is skipped.
    //   selected - the card is the panel's selected (followed or peeked) session.
    // Throws what a XAML property write throws (the callers already contain that).
    void SidePanelApplyCardItemStatus(const winrt::TerminalApp::implementation::AgentSidePanelCardView& view, bool selected)
    {
        namespace impl = winrt::TerminalApp::implementation;
        if (!view.title)
        {
            return;
        }
        const auto& model = view.model;
        std::wstring tags;
        for (const auto& tag : model.tags)
        {
            tags += tags.empty() ? L"tags: " : L", ";
            tags += tag;
        }
        impl::AgentItemStatus status;
        status.Add(impl::AgentStateWord(model.state))
            .Add(impl::kAgentStatusCodex, model.codex)
            .Add(impl::kAgentStatusUnread, model.flashRing)
            .Add(impl::kAgentStatusDraft, model.pending)
            .Add(impl::kAgentStatusCacheWarm, model.cacheWarm)
            .Add(impl::kAgentStatusPinnedToTop, model.pinned) // SIDE_PANEL.md §2j: the band's pushpin, in words
            .Add(tags)
            .Add(impl::kAgentStatusSelected, selected);
        impl::AgentSetItemStatus(view.title, status.Text());
    }

    // --- The cards' bookmark-TAG strip (the title band's top-right corner) ---------------------
    // How many SLOTS the strip may occupy. Up to this many tags each get a ribbon; with more, the
    // LAST slot turns into a "+N" count of the rest — so the strip's footprint is capped however
    // heavily a session is tagged, and the title (which owns the band and trims with an ellipsis)
    // always keeps the rest. Deliberately tighter than the Triage-Board card's 8 ribbons: that card
    // is a fixed 220px, while this list can be dragged down to 10% of the window. It is the
    // Sessions browser's fixed-footprint idiom (its Tags column: 4 slots, "+N" in the last one).
    constexpr size_t kSidePanelCardTagSlots = 5;

    // Build ONE bookmark ribbon: the 6.5x9.3 swallowtail Polygon every tag surface draws (the tab
    // badges, the Triage-Board card, the Sessions Tags column, the board's filter chips), so a tag
    // reads as the same mark here as everywhere else. The thin black outline is what keeps a ribbon
    // legible on ANY title-band color — the tab badges' answer to the same problem (a tag whose color
    // is close to the band's would otherwise melt into it).
    // `fill`: the tag's RESOLVED display color — the caller's ResolveTagDisplayColor (a user-picked
    //         tag-colors.json color, else the stable name-hash palette color), never re-derived here,
    //         so a tag can never wear a different color on this card than on its tab.
    // Returns the unparented shape, hit-testable over its fill (the tag hover panel keys on its
    // pointer enter/leave) and top-aligned (the strip hangs it from the band's top edge).
    // Throws winrt::hresult_error only if XAML object creation fails (called off the UI thread, or on
    // a torn-down XAML core) — the card rebuild that calls it always runs on the UI thread.
    winrt::Windows::UI::Xaml::Shapes::Polygon SidePanelMakeTagRibbon(const Color& fill)
    {
        // Pinned by a block-scope using-DECLARATION (it outranks the file's using-directives): this
        // helper sits at file scope, where several of those meet, unlike the board's copy, which lives
        // in a member function.
        using winrt::Windows::Foundation::Point;
        winrt::Windows::UI::Xaml::Shapes::Polygon ribbon;
        ribbon.Points().Append(Point{ 0.0f, 0.0f });
        ribbon.Points().Append(Point{ 6.5f, 0.0f });
        ribbon.Points().Append(Point{ 6.5f, 9.3f });
        ribbon.Points().Append(Point{ 3.25f, 6.5f }); // the notch that makes it a bookmark, not a block
        ribbon.Points().Append(Point{ 0.0f, 9.3f });
        ribbon.Fill(SolidColorBrush{ fill });
        ribbon.Stroke(SolidColorBrush{ Colors::Black() });
        ribbon.StrokeThickness(0.75);
        ribbon.VerticalAlignment(VerticalAlignment::Top);
        return ribbon;
    }

    // --- The cards' PIN TO TOP pushpin (the title band's right end — SIDE_PANEL.md §2j) -------------------------
    // The user's pick of 2026-11-06 from three looks (an outline pushpin, a filled one, the 📌 emoji): the icon font's OUTLINE
    // pushpin, Pin (U+E718 in Segoe Fluent Icons, and the same code point in Segoe MDL2 Assets — the fallback named after it,
    // for a Windows 10 without the Fluent font), drawn in the title's own ink so it reads on every folder color; a colored
    // emoji melts into a band of its own color.
    constexpr const wchar_t* kSidePanelPinGlyph = L"\xE718";
    // The menu's Unpin item: the icon font's Unpin (U+E77A, the pushpin struck through), the twin of Pin's glyph.
    constexpr const wchar_t* kSidePanelUnpinGlyph = L"\xE77A";
    // The family list both glyphs are drawn from: Segoe Fluent Icons first (Windows 11), Segoe MDL2 Assets when it is missing.
    // Named explicitly: XAML falls back to the text font for a family it cannot find, and that has no glyph at these points.
    constexpr const wchar_t* kSidePanelPinFont = L"Segoe Fluent Icons, Segoe MDL2 Assets";
    // 10 DIPs: about the tag ribbons' 9.3 DIP height beside it, and well inside the band's 12 px title line, so a pin never
    // makes the band (and with it every card's stride, SIDE_PANEL.md §2f) taller.
    constexpr double kSidePanelPinIconSize = 10.0;
    // The air between the pushpin and what is left of it: the last ribbon or "+N", else the title's ellipsis.
    constexpr double kSidePanelPinIconGapPx = 5.0;

    // --- The in-place card list (SIDE_PANEL.md §2c) ----------------------------------------------

    // Pack a Color as 0xAARRGGBB — the card model's comparable form of a color.
    //   c       — the color.
    //   returns — the packed value. Never throws.
    uint32_t SidePanelPackArgb(const Color& c) noexcept
    {
        return (static_cast<uint32_t>(c.A) << 24) | (static_cast<uint32_t>(c.R) << 16) | (static_cast<uint32_t>(c.G) << 8) | static_cast<uint32_t>(c.B);
    }

    // The inverse of SidePanelPackArgb. A plain struct initialization (Color is a POD of A, R, G, B), not
    // ColorHelper::FromArgb — that one is a WinRT factory call, and the dots and ribbons unpack per update.
    //   argb    — 0xAARRGGBB.
    //   returns — the color. Never throws.
    Color SidePanelUnpackArgb(uint32_t argb) noexcept
    {
        return Color{ static_cast<uint8_t>(argb >> 24), static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8), static_cast<uint8_t>(argb) };
    }

    // Remove `child` from `panel` wherever it sits — the optional parts of a card (codex pill, dots,
    // timing, tag strip) are found by identity, so a part's removal never depends on its siblings' count.
    //   panel — the parent panel.
    //   child — the element to remove; a no-op when it is not one of the panel's children.
    // Throws winrt::hresult_error if XAML refuses the edit (a torn-down tree) — the list update's catch
    // resets the list.
    void SidePanelRemoveChild(const winrt::Windows::UI::Xaml::Controls::Panel& panel, const UIElement& child)
    {
        uint32_t index = 0;
        if (panel && child && panel.Children().IndexOf(child, index))
        {
            panel.Children().RemoveAt(index);
        }
    }

    // The status icon's inputs that are the same for every card of one list update (SIDE_PANEL.md §2g), read ONCE per
    // update — the census lock, the settings and the clock are never touched per card.
    struct SidePanelIndicatorInputs
    {
        std::vector<std::wstring> unreadIds; // the process-wide unread census (CurrentUnreadSessionIds): sorted + deduped, binary-searched per card
        uint32_t cacheMinutes{ 5 }; // the server cache's lifetime (AppSettings::serverCacheMinutes, 0 -> 5 — the tab strip's and the board's normalization)
        int64_t nowMs{ 0 }; // the registry's clock (system-clock ms) the warmth is judged at
        uint32_t cacheWarmArgb{ 0 }; // the crown's fire color over the card body, 0xAARRGGBB (CacheWarmSparkColorFor)
    };

    // The cache-warm crown's color on a card: the tab strip's fire pair (CacheWarmSparkColorFor) picked against the card
    // body — the bright amber, since the body is dark. The tab picks against its own rendered tab color; the card's dot
    // sits on the body, never on the band.
    //   returns — the color, packed 0xAARRGGBB. Never throws.
    uint32_t SidePanelCacheWarmArgb() noexcept
    {
        return SidePanelPackArgb(winrt::TerminalApp::implementation::CacheWarmSparkColorFor(kSidePanelCardBodyColor));
    }

    // Read the per-update indicator inputs (see SidePanelIndicatorInputs).
    //   settings — this window's settings copy (serverCacheMinutes).
    //   returns  — the inputs.
    //   throws   — std::bad_alloc only (the census copy); CurrentUnreadSessionIds itself never throws.
    SidePanelIndicatorInputs SidePanelReadIndicatorInputs(const ::Agentmaster::AppSettings& settings)
    {
        SidePanelIndicatorInputs in;
        in.unreadIds = ::Agentmaster::CurrentUnreadSessionIds();
        in.cacheMinutes = settings.serverCacheMinutes ? settings.serverCacheMinutes : 5;
        in.nowMs = NowMs();
        in.cacheWarmArgb = SidePanelCacheWarmArgb();
        return in;
    }

    // Everything ONE card shows, computed from its live session (the lean SnapshotLiveForCards copy) and
    // this update's shared lookups — a plain record that _SidePanelApplyCardModel compares, value by value,
    // with the one the card shows now.
    //   s                 — the session (its queue and transcript texts are empty in the lean copy; nothing
    //                       here reads them).
    //   mode              — the GLOBAL tab-color mode (the band color and the effective working dir).
    //   tags              — the session's bookmark tags in add order (the card's tag cache).
    //   tagColors         — this tags epoch's LoadAllTagColors() (empty when no rendered card is tagged).
    //   dirColors         — this update's LoadDirColorIndex() snapshot.
    //   pendingArgb       — the pending dots' color, 0xAARRGGBB (PendingDotsColorFor over the card body: the
    //                       configured LIGHT color, since card bodies are dark).
    //   animationsEnabled — Windows' "Animation effects" (AgentAnimationsEnabled), read once per update.
    //   indicators        — the status icon's per-update inputs (the unread census, the cache lifetime, the clock,
    //                       the crown's color — SIDE_PANEL.md §2g).
    //   returns           — the model.
    //   throws            — std::bad_alloc on allocation failure only.
    winrt::TerminalApp::implementation::AgentSidePanelCardModel SidePanelCardModelFor(const ::Agentmaster::SessionInfo& s,
                                                                                      ::Agentmaster::TabColorMode mode,
                                                                                      const std::vector<std::wstring>& tags,
                                                                                      const std::map<std::wstring, std::wstring>& tagColors,
                                                                                      const ::Agentmaster::DirColorIndex& dirColors,
                                                                                      uint32_t pendingArgb,
                                                                                      bool animationsEnabled,
                                                                                      const SidePanelIndicatorInputs& indicators)
    {
        winrt::TerminalApp::implementation::AgentSidePanelCardModel m;
        m.title = s.title.empty() ? s.id : s.title;
        // The band: the session's resolved color (dir-keyed / individual / inferred per the GLOBAL mode;
        // empty under Remove-colors) with WCAG-contrast-picked ink — resolved from the update's ONE
        // dir-colors snapshot, never one file stat + map copy per card.
        m.bandHex = ::Agentmaster::ResolveSessionColorHex(mode, s, dirColors);
        if (const auto band = HexToColor(m.bandHex))
        {
            m.darkInk = PreferDarkTextOn(*band);
        }
        m.state = s.state;
        m.codex = (s.kind == ::Agentmaster::AgentKind::Codex);
        m.pending = !s.pendingInput.empty();
        if (m.pending)
        {
            // RENDERING.md §4c: still dots for a session whose tab has not started (a dormant tab's
            // remembered draft), as on its tab; an external session (no control of ours) keeps the pulse.
            // Windows' animation switch is folded in HERE (not only inside the status icon's own motion
            // decision) so that turning it on or off changes the model, and every card re-decides its dots.
            m.pendingAnimated = (s.started || s.external) && animationsEnabled;
            m.pendingArgb = pendingArgb;
        }
        // The status icon's other two indicators (SIDE_PANEL.md §2g), from the SAME facts the tab strip reads:
        //   - the FLASH RING: the session is in the process-wide unread census — exactly the sessions whose tab's ring
        //     flashes in its hosting window (the automatic "left Running while you weren't looking" flash, or a manual
        //     Mark Unread), whichever window hosts it, so a GLOBAL list rings for other windows' tabs too;
        //   - the SPARK CROWN: ServerCacheStillWarm, the one predicate behind the tab's crown and the board card's ⚡
        //     (live + Claude + at rest + a real API turn younger than the cache's lifetime) — so the three surfaces can
        //     never disagree about a session's warmth at the same instant.
        m.flashRing = std::binary_search(indicators.unreadIds.begin(), indicators.unreadIds.end(), s.id);
        m.cacheWarm = ::Agentmaster::ServerCacheStillWarm(s, indicators.cacheMinutes, indicators.nowMs);
        if (m.cacheWarm)
        {
            m.cacheWarmArgb = indicators.cacheWarmArgb;
        }
        m.animationsEnabled = animationsEnabled;
        const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
        m.timing = FormatSessionTiming(s.convCreatedUnixMs, last);
        // The dir line: "<leaf>[ / <branch>]", dim — the card's place at a glance (the full path rides the tooltip).
        const std::wstring dir = ::Agentmaster::EffectiveWorkingDir(mode, s);
        m.dirLine = SidePanelLeafOf(dir);
        if (!s.branch.empty())
        {
            m.dirLine += L" / " + s.branch;
        }
        m.tags = tags;
        m.tagArgb.reserve(tags.size());
        for (const auto& tag : tags)
        {
            // The ONE resolution every tag surface shares (a user-picked color, else the name hash), so a
            // tag wears the same color on this card as on its tab.
            m.tagArgb.push_back(SidePanelPackArgb(winrt::TerminalApp::implementation::ResolveTagDisplayColor(tag, tagColors)));
        }
        // The tooltip names EVERY tag, in the order they were added — the ribbons are colors only, and a
        // "+N" corner hides the overflow, so this is where the full set reads as words.
        std::wstring tagsLine;
        if (!tags.empty())
        {
            tagsLine = L"\ntags: ";
            for (size_t i = 0; i < tags.size(); ++i)
            {
                tagsLine += (i ? L" \x00B7 " : L"") + tags[i];
            }
        }
        // PIN TO TOP (SIDE_PANEL.md §2j): the band's pushpin. The pushpin itself takes no pointer (a press selects the card), so the
        // card's tooltip is where it reads as words, with the way back: the same right-click menu that pinned it.
        m.pinned = ::Agentmaster::IsSidePanelPinStamp(s.sidePanelPinnedAtUnixMs);
        const wchar_t* const pinLine = m.pinned ? L"\npinned to the top of this list (right-click \x25B8 Unpin)" : L"";
        m.tipTitle = s.title;
        m.tipText = dir + (s.branch.empty() ? L"" : (L"\nbranch: " + s.branch)) + tagsLine + pinLine + L"\n\nClick to select \x00B7 double-click to jump to its tab \x00B7 right-click for actions.";
        return m;
    }

    // --- The header's "+" LAUNCHER card (SIDE_PANEL.md §2e) ------------------------------------------------------
    // The gap between the card row's parts: the Agent Manager's launch bar spacing, so the row reads the same.
    constexpr double kSidePanelLaunchRowSpacingPx = 8.0;
    // The card box's width band: 504 DIPs is the Agent Manager's comfortable width (its widest stage), 160 its
    // narrowest. The box is pinned inside the band to what the window leaves beside the chip, Launch and Fork.
    constexpr double kSidePanelLaunchBoxMinPx = 160.0;
    constexpr double kSidePanelLaunchBoxMaxPx = 504.0;
    // How far the card's popup offsets must move before they are written again (DIPs): a re-placement that would land
    // within a quarter DIP of the current place writes nothing (no layout invalidation for a no-op).
    constexpr double kSidePanelLaunchPlaceEpsilonPx = 0.25;
    // A measure bound large enough to read an element's natural (content) size — the _ReflowLaunchBar idiom: NoWrap
    // text and buttons treat it like infinity.
    constexpr float kSidePanelLaunchMeasureBound = 100000.0f;

    // The COM identity of `o` (its IUnknown pointer), or null for a null object: two references to one element compare
    // equal by it whatever interface they hold. Read once per element, so a walk that compares an ancestor chain with
    // many elements costs one QueryInterface per element instead of two per comparison.
    //   o       - the object (any projected type).
    //   returns - the identity pointer, valid only for comparison (no reference is kept).
    //   throws  - winrt::hresult_error when the object refuses IUnknown (never for a XAML element).
    void* SidePanelLaunchIdentity(const winrt::Windows::Foundation::IInspectable& o)
    {
        return o ? winrt::get_abi(o.as<winrt::Windows::Foundation::IUnknown>()) : nullptr;
    }

    // True when `element` is `ancestor` or lies under it in the visual tree. A TextElement (a Run, a Hyperlink) is not a
    // visual, so the walk starts from the control that hosts its text (AgentTranscriptOverlay::_IsUnder's rule).
    // Bounded at 1024 steps, so a broken chain can never hang the UI thread.
    //   element  - where the walk starts (the focused element).
    //   ancestor - the element to find.
    //   returns  - true when found.
    //   throws   - what VisualTreeHelper / the TextElement read throws (callers contain it).
    bool SidePanelLaunchIsUnder(winrt::Windows::UI::Xaml::DependencyObject element, const winrt::Windows::UI::Xaml::DependencyObject& ancestor)
    {
        if (!element || !ancestor)
        {
            return false;
        }
        if (const auto textElement = element.try_as<winrt::Windows::UI::Xaml::Documents::TextElement>())
        {
            element = textElement.ElementStart().VisualParent();
        }
        const void* const target = SidePanelLaunchIdentity(ancestor);
        for (int guard = 0; element && guard < 1024; ++guard)
        {
            if (SidePanelLaunchIdentity(element) == target)
            {
                return true;
            }
            element = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(element);
        }
        return false;
    }

    // --- The FILTERS' two popup cards (SIDE_PANEL.md §2l): the "+" card's walk and placement, shared ------------------------

    // Does a press under `source` lie inside one of the `inside` elements, or inside another open popup of `xamlRoot` than the
    // `own` ones (a dialog's content, a model prompt)? The "+" card's outside-press walk (_SidePanelLaunchPressIsInside), for
    // the funnel's panel and the folder card. A TextElement source starts from its host.
    //   source   - the press's OriginalSource.
    //   inside   - the identities that count as inside (SidePanelLaunchIdentity of each element; nulls are ignored).
    //   xamlRoot - the island's root, for its open popups (null: none are counted).
    //   own      - the identities of the filter surfaces' OWN popups: their content is inside only through `inside`, so a
    //              press on the panel is never "inside the folder card" merely because both are open popups.
    //   returns  - true = keep the surface open.
    //   throws   - what the visual tree walk throws (the caller contains it and closes the surface).
    bool SidePanelPressInside(DependencyObject source, std::vector<void*> inside, const winrt::Windows::UI::Xaml::XamlRoot& xamlRoot, const std::vector<void*>& own)
    {
        if (!source)
        {
            return false;
        }
        if (xamlRoot)
        {
            for (const auto& popup : VisualTreeHelper::GetOpenPopupsForXamlRoot(xamlRoot))
            {
                const void* const id = SidePanelLaunchIdentity(popup);
                if (std::find(own.begin(), own.end(), id) == own.end())
                {
                    if (const auto child = popup.Child())
                    {
                        inside.push_back(SidePanelLaunchIdentity(child));
                    }
                }
            }
        }
        if (const auto textElement = source.try_as<winrt::Windows::UI::Xaml::Documents::TextElement>())
        {
            source = textElement.ElementStart().VisualParent();
        }
        for (int guard = 0; source && guard < 1024; ++guard)
        {
            const void* const id = SidePanelLaunchIdentity(source);
            if (id && std::find(inside.begin(), inside.end(), id) != inside.end())
            {
                return true;
            }
            source = VisualTreeHelper::GetParent(source);
        }
        return false;
    }

    // Place a Root()-parented popup card with its corner at `anchor`, extending toward the window's center (PlacePointerCard,
    // the "+" card's placement), measured afresh (a Popup measures its child with no limit). Offsets are written only on a
    // real move, so a re-placement that changes nothing invalidates nothing.
    //   popup  - the card's Popup.
    //   card   - the card (the Popup's child).
    //   anchor - the corner's point, in Root() coordinates.
    //   root   - Root(), for the window's size (0 = unmeasured: no clamp).
    //   returns - true when an offset was written.
    //   throws  - what the measure and the writes throw (the callers contain it).
    bool SidePanelPlaceCardAt(const winrt::Windows::UI::Xaml::Controls::Primitives::Popup& popup,
                              const FrameworkElement& card,
                              const winrt::Windows::Foundation::Point& anchor,
                              const FrameworkElement& root)
    {
        card.Measure(winrt::Windows::Foundation::Size{ kSidePanelLaunchMeasureBound, kSidePanelLaunchMeasureBound });
        const auto size = card.DesiredSize();
        const auto placed = ::Agentmaster::PlacePointerCard(static_cast<double>(anchor.X),
                                                            static_cast<double>(anchor.Y),
                                                            static_cast<double>(size.Width),
                                                            static_cast<double>(size.Height),
                                                            root ? root.ActualWidth() : 0.0,
                                                            root ? root.ActualHeight() : 0.0);
        bool moved = false;
        if (std::abs(popup.HorizontalOffset() - placed.left) > kSidePanelLaunchPlaceEpsilonPx)
        {
            popup.HorizontalOffset(placed.left);
            moved = true;
        }
        if (std::abs(popup.VerticalOffset() - placed.top) > kSidePanelLaunchPlaceEpsilonPx)
        {
            popup.VerticalOffset(placed.top);
            moved = true;
        }
        return moved;
    }

    // --- FOLLOW vs a focus RETURN (SIDE_PANEL.md §3) --------------------------------------------------------------
    // Does this arrival of focus in the terminal column END a peek, that is, is it the user coming back to the terminal?
    // YES in two cases: focus moved within the terminal column itself (another pane of the tab), and focus came from the
    // side panel's own controls (its filter box) — the keyboard way back from the panel.
    // NO in every other case, because none of them is the user touching the terminal:
    //   - focus arriving from NOWHERE (no element held it), or from the closing popup itself. That is how a closing
    //     menu, flyout or other light-dismiss popup hands focus back, as read in WinUI's source (microsoft-ui-xaml:
    //     Popup.cpp, Control.cpp, FocusMgr.cpp): CPopup::Close first takes the popup's content out of the tree; the
    //     focused element in it (a menu item, or the menu itself) leaves, and its CControl::LeaveImpl asks
    //     CFocusManager::SetFocusOnNextFocusableElement for a new home. While the menu is still listed open every
    //     candidate lies in that closing popup — its next tab stop, or the popup itself, which GetFirstFocusableElement
    //     returns while a light-dismiss popup is open — so focus ends on the popup or nowhere (cleared). Only then does
    //     Close give focus back to the element that held it before the popup opened, and only in exactly those two
    //     states ("nothing now has focus", or the popup still holds it). So the terminal's GettingFocus names no
    //     previous element, or one inside the closed popup — never one under the terminal column or the panel.
    //     A card's context menu takes focus when it opens (MenuFlyout::ShowAt, the standard show mode), so every way
    //     of closing it — an item, Esc, a click outside it — used to end the peek of the card it was opened on and,
    //     with the ⌖ toggle on, scroll the list back to the focused tab's card. The user's reports of 2026-10-24:
    //     "Move to Idle/Done" on the 5th card selected the top card and jumped the list to the top, and closing the
    //     menu without a choice jumped it too. The first version of this rule (a0e892b90) read "from nowhere" as the
    //     window regaining activation and followed, so it fixed neither. A window coming back to the foreground keeps
    //     the peek now as well: it is not a touch either.
    //   - an element elsewhere: an overlay over the page that gives focus back when it closes (the transcript reading
    //     view of the same card menu's "Show Transcript", then Esc; the command palette), the title bar's tab strip
    //     (a tab's rename box).
    //   - an element already gone: the previous terminal of a tab switch, whose own funnel
    //     (_SyncManagerSelectionToTab) moved the selection already.
    // A click into the terminal still ends a peek: its PointerPressed follows unconditionally. A press that only closes
    // a light-dismiss menu never reaches it: the popup root takes that press (CPopupRoot::OnPointerPressed).
    //   oldFocus       - GettingFocusEventArgs::OldFocusedElement, the element losing focus (null: none held it).
    //   terminalColumn - the terminal column the handler is on (TabContent).
    //   sidePanelHost  - the side panel's root (_sidePanelHost; null before the panel is built).
    //   returns        - true when the arrival ends a peek (the follow runs), false when the peek is kept.
    //   throws         - what the XAML reads throw on a torn-down tree (the caller contains it).
    bool SidePanelFocusArrivalEndsPeek(const winrt::Windows::UI::Xaml::DependencyObject& oldFocus,
                                       const winrt::Windows::UI::Xaml::UIElement& terminalColumn,
                                       const winrt::Windows::UI::Xaml::UIElement& sidePanelHost)
    {
        if (!oldFocus)
        {
            return false; // from nowhere: a closing popup handing focus back (or the window's activation) — keep
        }
        // The same bounded walk the "+" card's focus checks use (a Hyperlink starts from the control hosting its text).
        if (terminalColumn && SidePanelLaunchIsUnder(oldFocus, terminalColumn))
        {
            return true;
        }
        return sidePanelHost && SidePanelLaunchIsUnder(oldFocus, sidePanelHost);
    }

    // Where an arrival of focus in the terminal column came from, in words, for the one hooks.log line a kept peek
    // writes ("[sidepanel] focus came back to the terminal from <this>, …"). On a real build that line is the evidence
    // of which case of SidePanelFocusArrivalEndsPeek a hand-back was: "nowhere", the closed menu's own presenter, an
    // overlay's element. Asked only AFTER the follow-or-keep decision, so a failed read can never change it.
    //   oldFocus - GettingFocusEventArgs::OldFocusedElement, the element losing focus (null: none held it).
    //   returns  - "nowhere (…)" for null; else the element's runtime class, followed by its x:Name in quotes when it
    //              has one (for example Windows.UI.Xaml.Controls.MenuFlyoutPresenter); "" when the read failed (the
    //              caller then names it "an element it could not name").
    //   throws   - nothing: a failed read is recorded in hooks.log (Rule #18) and only loses the name.
    std::wstring SidePanelFocusOriginText(const winrt::Windows::UI::Xaml::DependencyObject& oldFocus) noexcept
    {
        // The whole body, the returns' string constructions included, sits in the try: this runs inside a focus event
        // of XAML, where an escaped exception (even a bad_alloc) would end the process through the noexcept.
        try
        {
            if (!oldFocus)
            {
                return L"nowhere (no element held focus: a closing menu or popup gave it back, or the window came back to the foreground)";
            }
            std::wstring text{ winrt::get_class_name(oldFocus) };
            if (const auto element = oldFocus.try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
            {
                if (const auto name = element.Name(); !name.empty())
                {
                    text += L" '" + std::wstring{ name } + L"'";
                }
            }
            return text;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"sidepanel focus origin text");
            return {}; // a default-constructed string cannot throw
        }
    }
}

namespace winrt::TerminalApp::implementation
{
    // Build the whole side panel into ContentArea. Called ONCE from _OnFirstLayout, AFTER
    // _InitAgentmasterEngine (the registry + the claimed WindowRecord exist) and after
    // _CreateAgentManagerPage (the AgentManagerContent the sort/menu/selection delegate to exists).
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
        // The "Sum" toggle (summary area hidden == the cards' rich tooltip mode) follows the collapse contract:
        // this window's own choice when its record has one, else the last change anywhere (the global default).
        _sidePanelSummaryHidden = (_windowRecord.sidePanelSummaryHidden >= 0) ? (_windowRecord.sidePanelSummaryHidden != 0) : _appSettings.sidePanelSummaryHidden;
        // The "AI" toggle (SIDE_PANEL.md §2h) follows the same contract: this window's own choice, else the last change
        // anywhere. Only READ here: the AI view replaces the card list, which must exist first, so the mode is applied at the
        // end of the build (_SidePanelSetAiMode, silently — a window's open persists nothing and starts no process).
        const bool startInAiMode = (_windowRecord.sidePanelAiMode >= 0) ? (_windowRecord.sidePanelAiMode != 0) : _appSettings.sidePanelAiMode;
        // The card-list SCOPE is per-window ONLY: the record's flag (absent == false), never an
        // AppSettings default — the user's contract is that every window starts on ITS OWN tabs
        // (LOCAL), and a fleet-wide (GLOBAL) view is an explicit choice that stays with the window
        // that made it, so it can never leak into another window's list.
        _sidePanelGlobal = _windowRecord.sidePanelGlobal;
        // The card list's FILTERS (SIDE_PANEL.md §2l): the same per-window-only contract as the scope — this window's record,
        // never a global default, so a new window starts unfiltered. Normalized again (the loader already did): the record
        // copy could have been edited in memory, and a filter the chips row cannot show must never hide a card.
        _sidePanelFilters = ::Agentmaster::NormalizeSidePanelFilters(_windowRecord.sidePanelFilters);
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
                    // Keep the tooltip out of the drag (kSidePanelSplitterTipDelay): a tip already open closes,
                    // a pending open is cancelled, and the emptied text keeps every move of the drag from arming
                    // it again. Without this, a tip open at the press could stay up while the bar follows the
                    // pointer (the tip host ignores moves while a tip is open, and only leaving the bar closes
                    // it), and a pause in the drag could open one. endWidthDrag gives the text back.
                    AgentSuppressTip(el);
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
                    // The drag is over: the bar gets back the tooltip its press emptied, with the same 7 s wait.
                    // This lambda serves both PointerReleased and PointerCaptureLost, and only its first call per
                    // drag gets past the _sidePanelDragWidth gate, so the tip is silent exactly while a drag runs.
                    AgentSetTip(el, winrt::hstring{ kSidePanelWidthSplitterTip }, kSidePanelSplitterTipDelay);
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
            AgentSetTip(_sidePanelSplitter, winrt::hstring{ kSidePanelWidthSplitterTip }, kSidePanelSplitterTipDelay); // opens after 7 s of rest only
        }

        // --- The panel host -----------------------------------------------------------------------
        // Forced DARK + an explicit dark fill (the Agentmaster surface convention — an app-theme
        // brush would bleed light-mode pixels through widget gaps).
        _sidePanelHost = Grid{};
        _sidePanelHost.RequestedTheme(ElementTheme::Dark);
        _sidePanelHost.Background(Fill(0xFF, 0x2E, 0x2E, 0x2E));

        // EXPANDED content: header (the buttons line + the filter box) / count line / body.
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

        // Header: TWO lines.
        //   line 1 (row 0): the four buttons at the panel's OUTER end — the window's edge, away from the terminal — in a
        //     line that MIRRORS with the dock (SIDE_PANEL.md §2i, the user's layout of 2026-11-04), so ⋯ is always the
        //     outermost button: docked right [ spacer * ][ + ][ funnel ][ Sum ][ ⋯ ] (right-aligned), docked left
        //     [ ⋯ ][ Sum ][ funnel ][ + ][ spacer * ] (left-aligned). "+" is the innermost, on the terminal's side of the
        //     FUNNEL (SIDE_PANEL.md §2l: the user's "a button for FILTER", "like the +"), which sits beside Sum — the user's
        //     "Swap between filter and + button" of 2026-11-10 (the funnel was the innermost before); docked right "+" is the
        //     leftmost again (§2e's "leftmost"). "+" and the funnel wear one face (SidePanelIconButtonFace: two Fluent icons,
        //     one box); "Sum" the middle (it reshapes the whole panel), ⋯ the outermost, where the collapse chevron was (the
        //     user's layout of 2026-11-03):
        //     its menu holds ⌖, the card scope, the card order, the dock flip and the collapse, which were five more buttons
        //     on this line and made it wider than a narrow panel. The build below seeds the docked-right shape;
        //     _SidePanelApplyHeaderOrder (run by every _SidePanelApplyDock, the first one at the end of this build, before the
        //     panel is ever drawn) moves the star and the four buttons to the columns of the dock in effect.
        //   line 2 (row 1): the filter box across the FULL width. On a line of its own it no longer
        //     competes with the buttons for room — sharing one line, a narrow panel squeezed the box down
        //     to its minimum while the buttons kept their natural widths.
        Grid header{};
        // Kept as a member too (the same element): every dock apply re-lays line 1 for its side (_SidePanelApplyHeaderOrder).
        _sidePanelHeader = header;
        {
            RowDefinition hr0{};
            hr0.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            RowDefinition hr1{};
            hr1.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            header.RowDefinitions().Append(hr0);
            header.RowDefinitions().Append(hr1);
            // FIVE columns: one star spacer and four Auto button columns. Seeded docked right (the spacer first); docked
            // left the star is the LAST column and the buttons fill columns 0-3 in mirror order (_SidePanelApplyHeaderOrder).
            ColumnDefinition c0{}; // the spacer docked right; "⋯" docked left
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition c1{}; // "+" (the launcher, SIDE_PANEL.md §2e) docked right; "Sum" docked left
            c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c2{}; // the funnel (the filters, SIDE_PANEL.md §2l) in both docks — the line's middle
            c2.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c3{}; // "Sum" (the summary area, SIDE_PANEL.md §2d) docked right; "+" docked left
            c3.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c4{}; // "⋯" (the menu, SIDE_PANEL.md §2i) docked right; the spacer docked left
            c4.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            header.ColumnDefinitions().Append(c0);
            header.ColumnDefinitions().Append(c1);
            header.ColumnDefinitions().Append(c2);
            header.ColumnDefinitions().Append(c3);
            header.ColumnDefinitions().Append(c4);
            header.Padding(ThicknessHelper::FromLengths(4, 4, 4, 2));
            header.ColumnSpacing(3);
            header.RowSpacing(3); // a little air between the buttons line and the filter line
        }
        // The filter box — SLIM, so it sits cleanly under the ~19px header buttons instead of towering over
        // them at the stock 32px: FontSize 11 (the buttons' size; was 12) and a tight padding (the stock
        // TextControlThemePadding is 10,5,6,6). Left 8 keeps the text off the rounded border; the extra
        // bottom pixel keeps the stock padding's bottom-heavy shape. Natural height: one 11px line (~15px)
        // + 5 of padding + a 2px border ≈ 22px — the kSidePanelSearchBoxMinHeightPx floor below.
        _sidePanelSearchBox = TextBox{};
        _sidePanelSearchBox.FontSize(11);
        _sidePanelSearchBox.Padding(ThicknessHelper::FromLengths(8, 2, 6, 3));
        // The TextBox TEMPLATE floors its border part at {ThemeResource TextControlThemeMinHeight} (32px, on
        // BorderElement — checked in both the SDK's generic.xaml and the WinUI 2.8 resources WT loads), and
        // a MinHeight on the TextBox itself cannot lower a template part's floor. So override that one key in
        // THIS box's own Resources — lightweight styling, the PaintHoldButton idiom — which the template's
        // ThemeResource lookup finds first when the template is applied (the box isn't in the tree yet, so
        // it is applied after this). Scoped to this box: every other TextBox keeps the stock 32px. The local
        // MinHeight(0) is a belt in case a style ever sets a floor on the control itself.
        _sidePanelSearchBox.Resources().Insert(winrt::box_value(L"TextControlThemeMinHeight"), winrt::box_value(kSidePanelSearchBoxMinHeightPx));
        _sidePanelSearchBox.MinHeight(0.0);
        _sidePanelSearchBox.PlaceholderText(kSidePanelFilterPlaceholder); // the AI mode swaps it for its own (_SidePanelSetAiMode)
        _sidePanelSearchBox.VerticalAlignment(VerticalAlignment::Center);
        // Opens after 3 s of rest (kSidePanelButtonTipDelay, the user's 2026-11-06 request): the box spans the panel, so the
        // pointer crosses it on its way to the cards, and at ~133 ms its paragraph popped up over them each time.
        AgentSetTitledTip(_sidePanelSearchBox, winrt::hstring{ kSidePanelFilterTipTitle }, winrt::hstring{ kSidePanelFilterTip }, kSidePanelButtonTipDelay);
        _sidePanelSearchBox.TextChanged([weakThis](const IInspectable& sender, const TextChangedEventArgs&) {
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            // The "AI" toggle lies over the box's right end: each text change is a moment the box may show its own clear
            // button, which must be moved left of the toggle (a no-op once done — SIDE_PANEL.md §2h).
            self->_SidePanelAiFitToggleIntoBox();
            // In AI mode the box is the AI search's REQUEST line: what is typed there is a sentence for the model, sent by
            // Enter — it must never filter the (hidden) cards, which would come back filtered by half a request.
            if (self->_sidePanelAiMode)
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
        // The AI search's keys, in AI mode only (SIDE_PANEL.md §2h): Enter sends the request, Escape stops a request that
        // runs. KeyDown on the box (the keys mean this only while the caret is in it). Neither moves focus to a terminal, so
        // acting on the key-down is safe here (the KEY-UP rule is for a panel that hands focus to a terminal as it closes).
        _sidePanelSearchBox.KeyDown([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
            const auto self = weakThis.get();
            if (!self || !self->_sidePanelAiMode)
            {
                return; // the filter box: its keys keep their meaning
            }
            if (e.Key() == VirtualKey::Enter)
            {
                e.Handled(true); // a TextBox without AcceptsReturn would ignore it anyway; nothing else may see it as a submit
                try
                {
                    // On a clean tick: the request clears the box (a programmatic write) and repaints the strip.
                    self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                        if (const auto page = weakThis.get())
                        {
                            page->_SidePanelAiAsk();
                        }
                    });
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel AI search: queue a request"); // the request is lost; the text stays in the box
                }
            }
            else if (e.Key() == VirtualKey::Escape)
            {
                try
                {
                    auto& agent = ::Agentmaster::SharedSessionSearchAgent();
                    if (agent.Snapshot().busy)
                    {
                        // Only then is the key ours: an Escape with nothing running keeps whatever meaning it has.
                        e.Handled(true);
                        ::Agentmaster::LogNav(L"sidepanel ai-stop");
                        agent.Interrupt();
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel AI search: stop a request");
                }
            }
        });
        // The box's focus in AI mode drives the search's PROCESS (SIDE_PANEL.md §2h, the user's "only after a request"
        // rule): a click into the box starts it (so it is usually up by the time Enter is pressed), and leaving the box with
        // nothing running arms its park. GotFocus also gives the "AI" toggle its room (the box's clear button appears with
        // focus). Both are plain bubbling events: the box has no focusable child that would raise them twice in a way that
        // matters (a Prewarm with the process up does nothing; a second park arming only moves its time).
        _sidePanelSearchBox.GotFocus([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelAiFitToggleIntoBox();
                if (self->_sidePanelAiMode)
                {
                    self->_SidePanelAiPrewarm();
                }
            }
        });
        _sidePanelSearchBox.LostFocus([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get(); self && self->_sidePanelAiMode)
            {
                self->_SidePanelAiArmPark(L"the request box lost focus");
            }
        });
        _sidePanelSearchBox.Loaded([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelAiFitToggleIntoBox(); // the style's padding is known and the template exists from here on
            }
        });
        // Line 2, spanning EVERY column — counted, not hard-coded, so a button added to line 1 later (a new
        // column) can't silently leave the box one column short of the right edge.
        Grid::SetRow(_sidePanelSearchBox, 1);
        Grid::SetColumn(_sidePanelSearchBox, 0);
        Grid::SetColumnSpan(_sidePanelSearchBox, static_cast<int32_t>(header.ColumnDefinitions().Size()));
        header.Children().Append(_sidePanelSearchBox);

        // The "AI" toggle (SIDE_PANEL.md §2h) — INSIDE the filter box, at its right end, like the Sessions page's AI toggle in
        // its search box (the user's "AI button on the right inside the search box"). It says how the box reads its text, so
        // it belongs to the box, not to the button line. It is NOT a part of the TextBox: a sibling in the box's grid cell,
        // appended after it, so it is drawn over the box's right end and takes the pointer there, while the box keeps its whole
        // chrome around both and leaves the toggle room (_SidePanelAiFitToggleIntoBox). Flat while off (a transparent fill and
        // no border — LOCAL values, which only the rest state uses, so the stock hover / pressed fills and the accent fill of
        // the checked state still show: the one cue that the box is a request line). IsTabStop false like the panel's buttons:
        // a click must never pull keyboard focus out of the terminal by itself (an AI-on puts the caret in the box on purpose).
        _sidePanelAiBtn = winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton{};
        _sidePanelAiBtn.Content(winrt::box_value(L"AI"));
        _sidePanelAiBtn.Width(kSidePanelAiToggleWidth);
        _sidePanelAiBtn.Height(kSidePanelAiToggleHeight);
        _sidePanelAiBtn.MinWidth(0.0);
        _sidePanelAiBtn.MinHeight(0.0);
        _sidePanelAiBtn.Padding(ThicknessHelper::FromUniformLength(0));
        _sidePanelAiBtn.FontSize(10);
        _sidePanelAiBtn.FontWeight(FontWeights::SemiBold());
        _sidePanelAiBtn.Background(SolidColorBrush{ Colors::Transparent() });
        _sidePanelAiBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        _sidePanelAiBtn.CornerRadius(CornerRadiusHelper::FromUniformRadius(3));
        _sidePanelAiBtn.HorizontalAlignment(HorizontalAlignment::Right);
        _sidePanelAiBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelAiBtn.Margin(ThicknessHelper::FromLengths(0, 0, kSidePanelAiToggleInset, 0));
        _sidePanelAiBtn.IsTabStop(false);
        AgentSetTitledTip(_sidePanelAiBtn, L"AI search", winrt::hstring{ L"On: the box takes a request in your own words \x2014 \x201Cthe session where I fixed the tab drag crash\x201D, Enter \x2014 and a Claude model (Sonnet) lists the sessions that fit here, in place of the cards, each with the evidence it found. Refine it with a follow-up; \x2298 starts over.\nOff: the box filters the cards again. The conversation is kept, and it is the SAME one as the Sessions page's AI search.\nIts claude.exe starts only when you click into the box or ask, and ends after the idle time set in Settings \x25B8 Sessions \x25B8 AI SEARCH (2 hours by default).\nThis window remembers its own choice; the last change becomes the default for new windows." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        // Its Click reads no state from the button: the member is the truth (the ToggleButton flips itself before Click, and
        // _SidePanelSetAiMode re-pins it). The mode change itself — it swaps the card area, may show the "Claude not
        // detected" notice and moves focus — runs on a clean tick (the pointer-handler rule: no tree change inside the click).
        _sidePanelAiBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            try
            {
                self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    if (const auto page = weakThis.get())
                    {
                        page->_SidePanelSetAiMode(!page->_sidePanelAiMode, /*userAction*/ true);
                    }
                });
            }
            catch (...)
            {
                // The click is lost: re-pin the toggle so it never shows a mode that did not happen (Rule #18).
                ::Agentmaster::AgentLogCaughtException(L"side panel AI toggle: queue the mode change");
                self->_SidePanelUpdateAiButton();
            }
        });
        Grid::SetRow(_sidePanelAiBtn, 1);
        Grid::SetColumn(_sidePanelAiBtn, 0);
        Grid::SetColumnSpan(_sidePanelAiBtn, static_cast<int32_t>(header.ColumnDefinitions().Size())); // the box's own cell, so HorizontalAlignment Right lands on the box's right end
        header.Children().Append(_sidePanelAiBtn); // after the box: drawn over it

        // The "+" LAUNCHER (SIDE_PANEL.md §2e) — the INNERMOST button of the line, on the terminal's side of the funnel (the
        // user's "Swap between filter and + button" of 2026-11-10; docked right it is the leftmost, §2e's "leftmost"; docked
        // left the line mirrors and it is the rightmost, _SidePanelApplyHeaderOrder): a click opens a card AT the click
        // point holding the Agent Manager's own launch row ([● Claude] [ box ] [Launch Claude] [Fork] — the shared
        // AgentLaunchBar, Claude only), so a session starts from the side panel without leaving the tab. Built on the first
        // click (_SidePanelEnsureLaunchCard): a window that never opens it pays nothing. An ICON button wearing the funnel's
        // face (SidePanelIconButtonFace: Segoe Fluent Icons' Add, the user's pick of 2026-11-10 "Both from the icon font"),
        // so the two are one size. IsTabStop false like its siblings: a click must never pull keyboard focus out of the
        // terminal by itself (the card focuses its own box when it opens, and hands focus back when it closes).
        _sidePanelLaunchBtn = Button{};
        _sidePanelLaunchBtn.FontSize(kSidePanelIconFontSize); // the face's invisible sizer inherits it (the text line's height)
        _sidePanelLaunchBtn.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
        _sidePanelLaunchBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelLaunchBtn.IsTabStop(false);
        _sidePanelLaunchBtn.Content(SidePanelIconButtonFace(kSidePanelAddGlyph));
        // A screen reader would otherwise have no words for the icon (a private-use glyph): name the button for what it does.
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_sidePanelLaunchBtn, winrt::hstring{ L"Launch a session" });
        AgentSetTitledTip(_sidePanelLaunchBtn, L"Launch a session", winrt::hstring{ L"Open the launcher at the click: the Agent Manager's own launch box \x2014 a working directory (recent and matching folders as you type, Browse\x2026) or a session id to resume or fork \x2014 and Launch Claude (hover it for a model or a preset).\nIt shows the same text as the Agent Manager's box. Click outside it, press Esc, or click + again to close it." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        // The PRESS point, in Root() coordinates: the card's corner sits where the user clicked. Read on the press, not
        // in Click: a Click carries no position, and the Button raises it from its own release handling, before an
        // instance PointerReleased handler would run. handledEventsToo, because the Button marks its presses handled.
        _sidePanelLaunchBtn.AddHandler(
            UIElement::PointerPressedEvent(),
            winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    self->_sidePanelLaunchAnchor = e.GetCurrentPoint(self->Root()).Position();
                    self->_sidePanelLaunchAnchorFromPress = true;
                }
                catch (...)
                {
                    // The open then anchors under the button instead (the no-press fallback); recorded (Rule #18).
                    self->_sidePanelLaunchAnchorFromPress = false;
                    ::Agentmaster::AgentLogCaughtException(L"side panel launcher: + press point");
                }
            } }),
            true /* handledEventsToo — the Button handles its own presses */);
        // A press that ends without a click (released off the button) must not leave its point for a later Click that
        // has no press of its own (UI Automation, Narrator). A real click normally consumes the point first (ButtonBase
        // raises Click from its release handling, then releases the capture). Should an order ever be the other way
        // round, the Click finds no point and anchors under the button instead — a few DIPs from the click, never a
        // wrong place.
        _sidePanelLaunchBtn.AddHandler(
            UIElement::PointerCaptureLostEvent(),
            winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_sidePanelLaunchAnchorFromPress = false;
                }
            } }),
            true /* handledEventsToo */);
        _sidePanelLaunchBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleLaunchCard();
            }
        });
        Grid::SetColumn(_sidePanelLaunchBtn, 1); // the docked-right seed; docked left it is column 3 (_SidePanelApplyHeaderOrder)
        header.Children().Append(_sidePanelLaunchBtn);

        // The "Sum" toggle — the summary area below the cards ON (checked, the default) / OFF. OFF hides the
        // bottom area and its splitter so the cards fill the body, and each card then shows the RICH session
        // tooltip beside the panel on hover — the very card a tab header shows (SIDE_PANEL.md §2d). A REAL
        // ToggleButton (the Sessions page's scope-toggle idiom), so its accent fill IS the state; IsChecked is seeded by
        // _SidePanelUpdateSumButton once the panel is built (a programmatic IsChecked raises no Click, so
        // seeding can never persist anything). The line's one WORDED button, so it keeps the 11px size the worded
        // buttons had (the "+" and "⋯" icons beside it are 12px).
        _sidePanelSumBtn = winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton{};
        _sidePanelSumBtn.FontSize(11);
        _sidePanelSumBtn.Padding(ThicknessHelper::FromLengths(8, 1, 8, 1));
        _sidePanelSumBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelSumBtn.IsTabStop(false); // like its siblings: a click must never pull keyboard focus out of the terminal
        _sidePanelSumBtn.Content(winrt::box_value(L"Sum"));
        AgentSetTitledTip(_sidePanelSumBtn, L"Summary area", winrt::hstring{ L"On: the selected session's summary shows below the cards.\nOff: the cards fill the panel, and hovering a card shows that session's full card beside the panel \x2014 the same card a tab shows when you hover it (scroll it with the mouse wheel).\nThis window remembers its own choice; the last change becomes the default for new windows." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        _sidePanelSumBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleSummary();
            }
        });
        Grid::SetColumn(_sidePanelSumBtn, 3); // the docked-right seed; docked left it is column 1 (_SidePanelApplyHeaderOrder)
        header.Children().Append(_sidePanelSumBtn);

        // The "⋯" MENU button (SIDE_PANEL.md §2i) — the OUTERMOST of the three buttons, at the window's edge, where the
        // collapse chevron was (the user's layout of 2026-11-03): the rightmost docked right, the leftmost docked left (the
        // line mirrors with the dock, the user's layout of 2026-11-04 — _SidePanelApplyHeaderOrder). Its menu holds the
        // five controls that were buttons on this line: Scroll to the selected tab (⌖) · Card scope ▸ · Card order ▸ · a
        // separator · Dock on the left / right (⇄) · Collapse the side panel (« / »), so the line stays four buttons wide
        // ("+ ⏷ Sum ⋯" or "⋯ Sum ⏷ +") and fits a narrow panel. An ICON button (the "+" font size and padding; its "⋯" is
        // text, so it stands as tall as the icon buttons' faces); IsTabStop false like its siblings:
        // a click must never pull keyboard focus out of the terminal by itself (the open menu holds the keyboard while it
        // shows and hands it back as it closes).
        // The menu is an EMPTY MenuFlyout that fills itself at every open (_SidePanelFillMoreMenu; the Agent Manager's
        // lazy session menus' idiom): its checks and labels are read from the live settings at that moment, so a change
        // made in another window (the order, ⌖, the dock) never leaves it stale, and a window that never opens it builds
        // no items. Its handler reaches the menu through `sender` and the page through a weak reference — never by
        // capture, which would make the menu own itself (the islands leak class). It opens BELOW the button, aligned on
        // the button's OUTER edge — the right edges docked right, the left edges docked left: the placement follows the
        // dock (_SidePanelApplyHeaderOrder; the docked-right one is seeded here) — so it spreads over the panel's own width
        // instead of out past the window's edge. Button::Flyout opens it on the button's Click — a pointer, or UI
        // Automation's Invoke — and owns it for the button's life.
        _sidePanelMoreBtn = Button{};
        _sidePanelMoreBtn.FontSize(kSidePanelIconFontSize); // an ICON button: one size above the 11px "Sum"
        _sidePanelMoreBtn.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
        _sidePanelMoreBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelMoreBtn.IsTabStop(false);
        _sidePanelMoreBtn.Content(winrt::box_value(kSidePanelMoreGlyph)); // ⋯
        AgentSetTitledTip(_sidePanelMoreBtn, L"More options", winrt::hstring{ L"Scroll the cards to the selected tab's card, choose which sessions the cards list (Card scope) and their order (Card order), dock the panel on the other side, or collapse it to a thin strip." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        {
            MenuFlyout moreMenu{};
            // The docked-right placement; _SidePanelApplyHeaderOrder sets the one for the dock in effect before any open.
            moreMenu.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
            moreMenu.Opening([weakThis](const IInspectable& sender, const IInspectable&) {
                const auto self = weakThis.get();
                const auto menu = sender.try_as<MenuFlyout>();
                if (!self || !menu)
                {
                    return;
                }
                try
                {
                    // Rebuilt from scratch at every open, so no item outlives the open it was made for.
                    menu.Items().Clear();
                    self->_SidePanelFillMoreMenu(menu);
                }
                catch (...)
                {
                    // The menu opens with whatever was built before the throw (possibly nothing): a short menu
                    // rather than a dead button. Recorded (Rule #18).
                    ::Agentmaster::AgentLogCaughtException(L"side panel more menu: fill at open");
                }
            });
            _sidePanelMoreBtn.Flyout(moreMenu);
        }
        Grid::SetColumn(_sidePanelMoreBtn, 4); // the docked-right seed; docked left it is column 0 (_SidePanelApplyHeaderOrder)
        header.Children().Append(_sidePanelMoreBtn);

        // The FUNNEL (SIDE_PANEL.md §2l, 2026-11-08 — the user's "a button for FILTER and in it have menu appear on click"):
        // the button between "+" and Sum (the user's "Swap between filter and + button" of 2026-11-10 put "+" on its terminal
        // side). Its click opens the FILTER PANEL at the click — a card that looks like a menu (By Folder… · By State ▸ · By
        // Tag ▸ · Active within ▸ · the four flags · Clear all filters) and STAYS OPEN while you pick (the user's
        // "Multiselect enabled, menu shouldnt close on clicking"): a Root() popup with the "+" card's outside-press close,
        // never a MenuFlyout (which closes at every click). An ICON button with the "+" button's face and padding
        // (SidePanelIconButtonFace — the user's "the filter icon is not same size as the + button": it was 25 x 15 DIPs
        // beside the "+" button's 22.2 x 20; now both are 26 x 20), IsTabStop false like its siblings (the panel focuses its
        // own first row). A ToggleButton for its accent fill alone — "a filter narrows the list" at a glance — whose
        // IsChecked the member pins (_SidePanelUpdateFilterButton), never the click's self-flip. Appended LAST: the header's
        // children keep the order the e2e driver walks (+ · Sum · ⋯), and the line's order on screen is the columns' business.
        _sidePanelFilterBtn = winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton{};
        _sidePanelFilterBtn.FontSize(kSidePanelIconFontSize); // the face's invisible sizer inherits it (the text line's height)
        _sidePanelFilterBtn.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
        _sidePanelFilterBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelFilterBtn.IsTabStop(false);
        _sidePanelFilterBtn.Content(SidePanelIconButtonFace(kSidePanelFilterGlyph)); // the funnel at 12 px, the "+" Add's size
        AgentSetTitledTip(_sidePanelFilterBtn, L"Filter the cards", winrt::hstring{ L"Narrow the cards: By Folder (the + launcher's folder picker), By State, By Tag, how recently a session was active, and four checks \x2014 an unsent draft, unread, pinned to the top, \x2605 favorite. The panel stays open while you pick; different filters narrow together, picks inside one filter add up.\nWhile a filter is on this button is lit, and the row under the filter box says what the cards are filtered by \x2014 \x2715 on a chip drops that filter, Clear drops them all.\nThis window keeps its own filters." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        // The PRESS point anchors the panel (the "+" button's two handlers: the point on the press, handledEventsToo because
        // a ToggleButton marks its presses handled; a press that ends without a click drops it).
        _sidePanelFilterBtn.AddHandler(
            UIElement::PointerPressedEvent(),
            winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    self->_sidePanelFilterAnchor = e.GetCurrentPoint(self->Root()).Position();
                    self->_sidePanelFilterAnchorFromPress = true;
                }
                catch (...)
                {
                    // The open then anchors under the funnel instead (the no-press fallback). Recorded (Rule #18).
                    self->_sidePanelFilterAnchorFromPress = false;
                    ::Agentmaster::AgentLogCaughtException(L"side panel filter: funnel press point");
                }
            } }),
            true /* handledEventsToo — the ToggleButton handles its own presses */);
        _sidePanelFilterBtn.AddHandler(
            UIElement::PointerCaptureLostEvent(),
            winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_sidePanelFilterAnchorFromPress = false;
                }
            } }),
            true /* handledEventsToo */);
        _sidePanelFilterBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelUpdateFilterButton(); // undo the ToggleButton's own flip first: the member is the truth
                self->_SidePanelToggleFilterPanel();
            }
        });
        Grid::SetColumn(_sidePanelFilterBtn, 2); // column 2 in both docks: the five-column line's middle (_SidePanelApplyHeaderOrder)
        header.Children().Append(_sidePanelFilterBtn);
        Grid::SetRow(header, 0);
        _sidePanelExpandedRoot.Children().Append(header);

        // Row 1 holds two elements, one shown at a time (_SidePanelShowRowForMode):
        //   - the COUNT LINE, which since SIDE_PANEL.md §2l speaks for the AI view alone ("3 sessions found", "AI search —
        //     searching…"): the cards' "N of M sessions in this window" is gone from the screen (the user's 2026-11-08 "The row
        //     below the filter with N sessions across all window, shall be replaced by a row only shown when filter is on"),
        //     and its words live on as the filter box's UI Automation ItemStatus (_SidePanelUpdateFilterRow);
        //   - the CHIPS ROW, in card mode, shown only while a filter narrows the list: "N of M", a chip per filter, Clear.
        _sidePanelCountText = Text(L"", 10, false, 0.5);
        _sidePanelCountText.Margin(ThicknessHelper::FromLengths(8, 0, 8, 2));
        _sidePanelCountText.Visibility(Visibility::Collapsed); // card mode at the build; an AI-mode window shows it at the end of the build
        Grid::SetRow(_sidePanelCountText, 1);
        _sidePanelExpandedRoot.Children().Append(_sidePanelCountText);
        _sidePanelFilterRow = Grid{};
        _sidePanelFilterRow.Margin(ThicknessHelper::FromLengths(6, 0, 6, 3));
        _sidePanelFilterRow.Visibility(Visibility::Collapsed); // nothing filters yet: the first list update shows it when one does
        _sidePanelFilterChips = winrt::make<AgentSidePanelWrapPanel>();
        _sidePanelFilterRow.Children().Append(_sidePanelFilterChips);
        // The chips' ✕ and Clear are Buttons: the Agent Manager's six Button state brushes, or under the forced-Dark theme
        // their hover and press fills resolve to null (a click lands only on the glyph).
        AgentLaunchBar::SeedButtonStateBrushes(_sidePanelFilterRow.Resources());
        Grid::SetRow(_sidePanelFilterRow, 1);
        _sidePanelExpandedRoot.Children().Append(_sidePanelFilterRow);

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

        // TOP: the scrollable card list (its OWN scroll, independent of the summary's). VIRTUALIZED (SIDE_PANEL.md
        // §2f): only the rows near the viewport have cards; two spacers stand in for the rows above and below them,
        // so the list's height — and the scroll extent — is the same whichever rows are realized.
        _sidePanelCards = StackPanel{};
        _sidePanelCards.Orientation(Orientation::Vertical);
        // The spacers: plain Borders whose Height is all they are. TRANSPARENT, not null: a null background is not
        // hit-testable, and a wheel over a spacer (the instant before its rows are realized) would then land on the
        // cards area behind the ScrollViewer and never scroll it.
        _sidePanelCardsTopSpacer = Border{};
        _sidePanelCardsTopSpacer.Height(0.0);
        _sidePanelCardsTopSpacer.Background(SolidColorBrush{ Colors::Transparent() });
        _sidePanelCardsBottomSpacer = Border{};
        _sidePanelCardsBottomSpacer.Height(0.0);
        _sidePanelCardsBottomSpacer.Background(SolidColorBrush{ Colors::Transparent() });
        _sidePanelCards.Children().Append(_sidePanelCardsTopSpacer);
        _sidePanelCards.Children().Append(_sidePanelCardsBottomSpacer);
        _sidePanelCardsScroll = ScrollViewer{};
        _sidePanelCardsScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _sidePanelCardsScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        _sidePanelCardsScroll.Content(_sidePanelCards);
        // A scroll moves the viewport over the rows: re-plan the realized range (a pass only when the plan changes —
        // usually every half viewport of scrolling). Raised on the UI thread after the offset changed, never inside a
        // layout pass, so the pass may edit the list synchronously (no blank frame while a wheel scroll runs).
        // Weak page capture and no element capture: the ScrollViewer owns this handler (no element -> handler ->
        // element cycle — the 2026-10-14 leak class).
        _sidePanelCardsScroll.ViewChanged([weakThis](const IInspectable&, const ScrollViewerViewChangedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelOnCardsViewChanged();
            }
        });
        // A taller (or shorter) viewport needs other rows — the "Sum" toggle, a splitter drag, a window resize. A
        // layout-driven trigger, so the re-check is QUEUED, never run inside the pass (the E_LAYOUTCYCLE rule).
        _sidePanelCardsScroll.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelQueueRealize();
            }
        });
        Border cardsArea{};
        cardsArea.Background(Fill(0xFF, 0x2A, 0x2A, 0x2A));
        // The cards area holds ONE grid: the card list's ScrollViewer, and — from the first AI-on (_SidePanelAiEnsureView) —
        // the AI search's view over it in the same cell. One of the two is visible (SIDE_PANEL.md §2h). A plain Grid with no
        // corner radius, so it adds no composition node.
        _sidePanelCardsHost = Grid{};
        _sidePanelCardsHost.Children().Append(_sidePanelCardsScroll);
        cardsArea.Child(_sidePanelCardsHost);
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
                    // The tooltip stays out of the drag, as on the vertical bar: closed if open, a pending open
                    // cancelled, silent until endSplitDrag gives the text back.
                    AgentSuppressTip(el);
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
                    // The drag is over: give back the tooltip the press emptied (once per drag, as in endWidthDrag).
                    AgentSetTip(el, winrt::hstring{ kSidePanelSplitSplitterTip }, kSidePanelSplitterTipDelay);
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
            AgentSetTip(_sidePanelHSplitter, winrt::hstring{ kSidePanelSplitSplitterTip }, kSidePanelSplitterTipDelay); // opens after 7 s of rest only
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
        _sidePanelStripBtn.FontSize(kSidePanelIconFontSize); // the collapsed strip's chevron — the same icon size as the header's icon buttons (+, ⋯)
        _sidePanelStripBtn.Padding(ThicknessHelper::FromLengths(2, 10, 2, 10));
        _sidePanelStripBtn.IsTabStop(false);
        AgentSetTip(_sidePanelStripBtn, winrt::hstring{ L"Expand the side panel (session cards + the selected session's summary)." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        _sidePanelStripBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleCollapsed();
            }
        });
        _sidePanelStrip.Children().Append(_sidePanelStripBtn);
        _sidePanelStrip.Visibility(Visibility::Collapsed);
        _sidePanelHost.Children().Append(_sidePanelStrip);

        // Agentmaster (accessibility, E2E_TESTING.md §6.3, 2026-10-23): the panel's stable AutomationIds, in ONE place.
        // NAMES only for the glyph-only buttons — ⋯ and the strip's chevron read as noise to a screen reader (the e2e
        // driver finds them by these ids); "Sum" keeps the name its content gives it, and the filter box keeps its
        // placeholder as its name. "+" was named before ("Launch a session"). The ⋯ menu's items keep the ids the header
        // buttons they replaced had (am.side.dock / scrollToTab / scope / sort / collapse — ids are added, never renamed)
        // and are set where the menu builds them, at every open (_SidePanelFillMoreMenu); their texts name them.
        AgentSetAutomationId(_sidePanelLaunchBtn, L"am.side.launch");
        AgentSetAutomationId(_sidePanelSearchBox, L"am.side.filter");
        AgentSetAutomationId(_sidePanelAiBtn, L"am.side.ai"); // the filter box's "AI" toggle (SIDE_PANEL.md §2h); its content "AI" names it
        AgentSetAutomationId(_sidePanelSumBtn, L"am.side.sum");
        AgentSetAutomationId(_sidePanelMoreBtn, L"am.side.more"); // the ⋯ menu button (SIDE_PANEL.md §2i)
        AgentSetAccessibleName(_sidePanelMoreBtn, L"More options");
        AgentSetAutomationId(_sidePanelStripBtn, L"am.side.expand");
        AgentSetAccessibleName(_sidePanelStripBtn, L"Expand the side panel");
        // The filters (SIDE_PANEL.md §2l): the funnel — named, its face is a glyph (am.side.filter is the filter BOX's id, taken
        // since 2026-10-23: ids are added, never renamed) — and the chips row (each chip's id is set when the row builds it).
        AgentSetAutomationId(_sidePanelFilterBtn, L"am.side.funnel");
        AgentSetAccessibleName(_sidePanelFilterBtn, L"Filter the cards");
        // (The chips row itself is a Grid, which has no automation peer: a reader finds it by its count, am.side.filterRow.count.)

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
        //   * GotFocus (plain bubbling) — covers a keyboard return of focus INTO the terminal: from
        //     the side panel's search box, or from another pane of the tab. ONLY those (2026-10-24,
        //     SidePanelFocusArrivalEndsPeek): focus that a closing menu, flyout, dialog, popup or page
        //     overlay hands back is XAML (or the overlay) putting focus back where it was, not the user
        //     touching the terminal — and a light-dismiss popup hands it back from NOWHERE (the closing
        //     popup first moves focus off its leaving content, onto the popup itself or nowhere, and
        //     only then gives it back), so an arrival from nowhere keeps the peek too, the window
        //     coming back to the foreground included. A card's context menu takes focus when it
        //     opens, so before this every way of closing it (an item, Esc, a click outside it) ended
        //     the peek of the very card it was opened on, and with ⌖ on scrolled the list back to the
        //     focused tab's card. A press that only closes a light-dismiss menu is not a terminal
        //     click either, even one on the terminal: the popup root takes it and marks it handled
        //     (CPopupRoot::OnPointerPressed), so the PointerPressed above never sees it; the next
        //     click into the terminal ends the peek.
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
            // HOW focus last arrived in the column, for the GotFocus below. GettingFocus is raised synchronously, while
            // the element losing focus can still be asked where it lives; GotFocus comes later and can no longer tell
            // (AgentLaunchBar's _cwdBoxFocusArrival idiom). Written by EVERY GettingFocus that reaches the column and
            // consumed by the GotFocus that follows it. Shared by the two handlers alone, so the page gains no member;
            // neither handler holds an element (the column is read through `sender`, the panel through the weak page,
            // and where focus came from is kept as text, never as the element).
            struct FocusArrival
            {
                bool keepsPeek = false; // this arrival keeps the peek: SidePanelFocusArrivalEndsPeek answered false
                std::wstring origin; // where focus came from (SidePanelFocusOriginText); set for a kept peek only
            };
            auto arrival = std::make_shared<FocusArrival>();
            content.AddHandler(
                UIElement::GettingFocusEvent(),
                winrt::box_value(winrt::Windows::Foundation::TypedEventHandler<UIElement, GettingFocusEventArgs>{
                    [weakThis, arrival](const UIElement& sender, const GettingFocusEventArgs& e) {
                        // Cleared first: an arrival that follows (or whose read fails) must not carry the text of an
                        // earlier one into the log line. clear() never throws.
                        arrival->origin.clear();
                        try
                        {
                            UIElement panel{ nullptr }; // stays null with the page gone: then only the column counts
                            if (const auto self = weakThis.get())
                            {
                                panel = self->_sidePanelHost; // the panel's root Grid (built before this handler)
                            }
                            const auto oldFocus = e.OldFocusedElement();
                            arrival->keepsPeek = !SidePanelFocusArrivalEndsPeek(oldFocus, sender, panel);
                            if (arrival->keepsPeek)
                            {
                                // Named only for a kept peek (the one case that may log it), and after the decision:
                                // the helper never throws, so a name that cannot be read never turns a keep into a follow.
                                arrival->origin = SidePanelFocusOriginText(oldFocus);
                            }
                        }
                        catch (...)
                        {
                            // Where focus came from is unknown: follow, the rule before this one (a peek ends and
                            // nothing is lost). Recorded (Rule #18).
                            arrival->keepsPeek = false;
                            ::Agentmaster::AgentLogCaughtException(L"sidepanel terminal column GettingFocus");
                        }
                    } }),
                true /* handledEventsToo — every arrival is read, so a stale value can never apply to the next one */);
            content.GotFocus([weakThis, arrival](const IInspectable&, const RoutedEventArgs&) {
                // Consumed whatever happens next, so this arrival's answer never leaks into a later one. Both exchanges
                // are noexcept (a bool, and a string MOVED out with an empty one moved in).
                const bool keepPeek = std::exchange(arrival->keepsPeek, false);
                const std::wstring origin = std::exchange(arrival->origin, std::wstring{});
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                if (!keepPeek)
                {
                    self->_SidePanelFollowFocusedTab();
                    return;
                }
                // Focus was handed back (a menu, a dialog, an overlay closing, the window coming back): the selection
                // and the list stay where the user left them. One hooks.log line when that kept a peek the follow would
                // have ended, naming where focus came from, so a selection that "did not follow" says why.
                try
                {
                    if (self->_sidePanelBuilt && self->_startupState == StartupState::Initialized)
                    {
                        if (const auto tab = self->_GetFocusedTab())
                        {
                            const auto tabId = self->_ClaudeSessionForTab(tab);
                            if (!tabId.empty() && tabId != self->_sidePanelSelectedId)
                            {
                                ::Agentmaster::AppendStateLog(L"hooks.log",
                                                              L"[sidepanel] focus came back to the terminal from " +
                                                                  (origin.empty() ? std::wstring{ L"an element it could not name" } : origin) +
                                                                  L", not by a click into it: the selection stays on " +
                                                                  ::Agentmaster::ShortId(self->_sidePanelSelectedId) + L" (the focused tab shows " +
                                                                  ::Agentmaster::ShortId(tabId) + L"; a click into the terminal ends the peek)\n");
                            }
                        }
                    }
                }
                catch (...)
                {
                    // Only the diagnostic line is lost; the selection already stayed. Recorded (Rule #18).
                    ::Agentmaster::AgentLogCaughtException(L"sidepanel focus handed back to the terminal");
                }
            });
        }

        _sidePanelBuilt = true;
        _SidePanelApplyDock(); // assigns the three columns per the global dock side + applies widths/glyphs (and the summary rows per "Sum")
        _SidePanelUpdateSumButton(); // "Sum" checked == the summary area shown (no Click => nothing persisted)
        _SidePanelUpdateFilterButton(); // the funnel lit == this window's record filters the list (SIDE_PANEL.md §2l; no Click => nothing persisted)
        // Nothing to seed for the ⋯ menu: it reads the dock, ⌖, the scope and the order when it opens (SIDE_PANEL.md §2i);
        // its placement, the one thing about it that follows the dock before an open, was set by _SidePanelApplyDock above.
        _SidePanelRebuildCards(); // with "Sum" off, every new card is armed with its rich tooltip as it is built
        // Re-select the lens's restored session so a reopened window's summary shows immediately
        // (the same restored selection SetManagerState seeded into the Manager lens).
        if (!_windowRecord.manager.selectedId.empty())
        {
            _SidePanelSelectSession(_windowRecord.manager.selectedId, /*driveManagerLens*/ false);
        }
        // The "AI" toggle's seeded mode (SIDE_PANEL.md §2h), applied now that the card list exists for the AI view to cover.
        // userAction=false: nothing is persisted (the seed IS the persisted state), no focus moves, no process starts — the
        // view shows the conversation's last list from the agent's stored view, and the search's claude.exe waits for a click
        // into the box or a request.
        _SidePanelUpdateAiButton();
        if (startInAiMode)
        {
            _SidePanelSetAiMode(true, /*userAction*/ false);
        }
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[sidepanel] built (window " + ::Agentmaster::ShortId(_windowId) +
                                          L") width=" + std::to_wstring(_sidePanelWidthFraction) +
                                          L" split=" + std::to_wstring(_sidePanelSplitFraction) +
                                          (_sidePanelCollapsed ? L" collapsed" : L" expanded") +
                                          (_sidePanelSummaryHidden ? L" summary=hidden(card-tooltips)" : L" summary=shown") +
                                          (_sidePanelAiMode ? L" ai=on" : L" ai=off") +
                                          (_sidePanelGlobal ? L" scope=global" : L" scope=local") +
                                          L" filters=" + std::to_wstring(std::popcount(::Agentmaster::SidePanelActiveFacets(_sidePanelFilters, false))) +
                                          (_appSettings.sidePanelScrollToSelectedTab ? L" scroll-to-tab=on" : L" scroll-to-tab=off") +
                                          (_appSettings.sidePanelOnLeft ? L" dock=left\n" : L" dock=right\n"));
    }

    // Assign TabContent / the splitter / the panel host to ContentArea's three columns per the
    // GLOBAL AppSettings::sidePanelOnLeft (dock-right: [content *][splitter][panel]; dock-left is
    // the mirror), mirror the header's button line to match (_SidePanelApplyHeaderOrder: ⋯ at the
    // window's edge), then re-apply widths + the strip's direction-dependent chevron via _SidePanelApplyLayout.
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
        _SidePanelApplyHeaderOrder(); // never throws: a failure leaves the line as it was, logged
        _SidePanelApplyLayout();
    }

    // Lay the header's button line out for the dock in effect (SIDE_PANEL.md §2i, the user's layout of 2026-11-04): the line
    // is the MIRROR image of itself in the other dock, so ⋯ is always the outermost button — at the window's edge — and "+"
    // the innermost, on the terminal's side, with the funnel (SIDE_PANEL.md §2l) between it and Sum — the user's "Swap
    // between filter and + button" of 2026-11-10 (the funnel was the innermost from 2026-11-08):
    //   docked right: [ spacer * ][ + ][ funnel ][ Sum ][ ⋯ ]   the menu opens below ⋯ with the RIGHT edges aligned;
    //   docked left:  [ ⋯ ][ Sum ][ funnel ][ + ][ spacer * ]   the menu opens below ⋯ with the LEFT edges aligned.
    // Either way the menu spreads over the panel's own width, never out past the window's edge (a flyout pushed off the
    // window is slid or flipped by the framework, and lands over the terminal). Only the columns move: which column is the
    // star spacer, which column each button sits in, and the menu's placement. The four buttons keep their place in the
    // header's Children (appended + · Sum · ⋯ · funnel), so every handler stays as the build made them and the e2e driver's
    // walk ("+" comes before the cards) holds in either dock — but the tree order need not follow the screen, so a reader of
    // the line's order goes by position (the driver's header_buttons). The filter box and the AI toggle span all five
    // columns, so neither moves. (The funnel's panel needs no placement here: it opens at the click, toward the window's
    // center, like the "+" card.)
    // Every write is change-gated (a same-value Width or Grid.Column write still invalidates layout), so a re-apply with an
    // unchanged dock — a settings broadcast that moved nothing, the build's first apply over its docked-right seed when the
    // panel docks right — writes nothing. Called by every _SidePanelApplyDock: the panel's build (before its first frame,
    // so a left-docked panel is never drawn with the seed), a dock flip here and a settings broadcast that moved the dock.
    //   returns - nothing; a no-op before the panel is built or when the header lacks its five columns.
    //   throws  - nothing: a failed XAML write (a window tearing down) is logged (Rule #18) and leaves the line in whatever
    //             shape the writes before it made — a cosmetic loss; the panel itself has already moved.
    void TerminalPage::_SidePanelApplyHeaderOrder() noexcept
    {
        if (!_sidePanelBuilt || !_sidePanelHeader)
        {
            return;
        }
        try
        {
            const bool left = _appSettings.sidePanelOnLeft;
            const auto cols = _sidePanelHeader.ColumnDefinitions();
            if (cols.Size() < 5)
            {
                return; // the build's five columns are the contract: with fewer there is no line to mirror
            }
            // The star spacer is the line's TERMINAL end: column 0 docked right, column 4 docked left. The other four are
            // Auto, each as wide as its button.
            const uint32_t spacerColumn = left ? 4u : 0u;
            for (uint32_t i = 0; i < 5u; ++i)
            {
                const auto col = cols.GetAt(i);
                const auto cur = col.Width();
                if (i == spacerColumn)
                {
                    if (cur.GridUnitType != GridUnitType::Star || std::abs(cur.Value - 1.0) > 0.001)
                    {
                        col.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                    }
                }
                else if (cur.GridUnitType != GridUnitType::Auto)
                {
                    col.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
                }
            }
            // The buttons, outermost first: ⋯ (4 / 0), Sum (3 / 1), the funnel (2 / 2), + (1 / 3) — docked right / docked left.
            const auto place = [](const FrameworkElement& button, int32_t column) {
                if (button && Grid::GetColumn(button) != column)
                {
                    Grid::SetColumn(button, column);
                }
            };
            place(_sidePanelMoreBtn, left ? 0 : 4);
            place(_sidePanelSumBtn, left ? 1 : 3);
            place(_sidePanelFilterBtn, 2);
            place(_sidePanelLaunchBtn, left ? 3 : 1);
            // The menu's placement is read when it opens, so setting it now (the menu is closed: a dock change comes from a
            // menu command that runs after the menu closed, or from another window's broadcast) takes effect at the next open.
            if (_sidePanelMoreBtn)
            {
                if (const auto menu = _sidePanelMoreBtn.Flyout())
                {
                    using winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode;
                    const auto placement = left ? FlyoutPlacementMode::BottomEdgeAlignedLeft : FlyoutPlacementMode::BottomEdgeAlignedRight;
                    if (menu.Placement() != placement)
                    {
                        menu.Placement(placement);
                    }
                }
            }
        }
        catch (...)
        {
            // The line keeps the shape the writes before the throw left it in (possibly the old dock's): cosmetic, and the
            // next dock apply re-lays it. Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelApplyHeaderOrder");
        }
    }

    // Derive the three ContentArea column widths + the cards/summary star ratios + the collapse
    // visibility + the strip chevron's direction-dependent glyph from the live state. Every write is
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
            // The pixel twin of setRowStar (the splitter row is a fixed height, and a hidden area is 0px) —
            // change-gated for the same reason: a same-value Height write still invalidates layout.
            const auto setRowPx = [](const RowDefinition& row, double px) {
                const auto cur = row.Height();
                if (cur.GridUnitType != GridUnitType::Pixel || std::abs(cur.Value - px) > 0.5)
                {
                    row.Height(GridLengthHelper::FromPixels(px));
                }
            };
            if (_sidePanelSummaryHidden)
            {
                // "Sum" off: the cards take the whole body; the splitter row and the summary row shrink to
                // nothing. The split fraction is untouched, so turning the summary back on restores it.
                setRowStar(rows.GetAt(0), 1.0);
                setRowPx(rows.GetAt(1), 0.0);
                setRowPx(rows.GetAt(2), 0.0);
            }
            else
            {
                setRowStar(rows.GetAt(0), _sidePanelSplitFraction);
                setRowPx(rows.GetAt(1), kSidePanelSplitterPx);
                setRowStar(rows.GetAt(2), 1.0 - _sidePanelSplitFraction);
            }
        }
        // The horizontal splitter and the summary host are collapsed with their rows, so a 0px row can never
        // leave a hit-testable sliver (a drag grip nobody can see) or an empty host taking pointer input.
        if (_sidePanelHSplitter)
        {
            _sidePanelHSplitter.Visibility(_sidePanelSummaryHidden ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_sidePanelSummaryHost)
        {
            _sidePanelSummaryHost.Visibility(_sidePanelSummaryHidden ? Visibility::Collapsed : Visibility::Visible);
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

        // The strip's chevron points AWAY from the docked edge (it expands), so it flips with the dock. (A plain text
        // glyph — no icon-font guessing; any UI font carries » «.) Its twin pointing TOWARD the edge, the collapse, is
        // the ⋯ menu's last item now, and that menu picks its glyph from the dock when it opens.
        const wchar_t* const awayFromEdge = left ? kSidePanelCollapseRightGlyph : kSidePanelCollapseLeftGlyph; // » / «
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

    // The ⋯ ▸ Card order pick (SIDE_PANEL.md §2i): put the GLOBAL board sort on `order` through the Manager content's own
    // setter — ONE sort spec, so the board and this list can never sort by different orders: AgentManagerContent::
    // SetBoardSort sets boardSort, persists it through the settings sink (which also updates this page's _appSettings copy
    // and broadcasts to the other windows) and re-sorts the board. On return the new order is in _appSettings, so this
    // list re-sorts at once (a forced rebuild: the user's action skips the throttle). No [nav] line: an order is a pure
    // view choice (the board's own sort button is unlogged for the same reason).
    //   order   - the picked order.
    //   returns - nothing; a no-op before the panel is built. A window without its Manager page (none built yet — the page
    //             is built at the window's first layout, before this panel) sets nothing and logs one [sidepanel] line,
    //             so a pick that changed nothing says why.
    //   throws  - what the setter's sink, the board refresh and the rebuild throw; the menu command contains and logs it.
    void TerminalPage::_SidePanelSetSort(::Agentmaster::ExplorerSort order)
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        implementation::AgentManagerContent* mgr = nullptr;
        if (const auto ipc = _agentManagerContent.get())
        {
            mgr = winrt::get_self<implementation::AgentManagerContent>(ipc);
        }
        if (!mgr)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] card order " + ::Agentmaster::ToString(order) + L" not set: this window has no Agent Manager page yet (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
            return;
        }
        mgr->SetBoardSort(order); // a pick of the order already set returns at once: nothing saved, nothing broadcast
        _SidePanelScheduleRebuild(/*force*/ true);
    }

    // The ⋯ ▸ Card scope pick (SIDE_PANEL.md §2i): put the card list on LOCAL (only the sessions hosted in this window) or
    // GLOBAL (every window's) — for THIS window only. Persists through the window-record autosave ALONE: there is
    // deliberately no _SidePanelPersistDefaults call and no AppSettings field, because the scope has
    // no global default by design — every new window starts LOCAL, so one window's fleet-wide view can
    // never seed another window's list (the "my list shows another window's tabs" bug this scope
    // exists to prevent). The Manager's own tree/board scope is NOT touched: it answers a different
    // question (what the fleet lens shows), and a pick here must not re-scope the Manager under
    // the user. Logged as a [sidepanel] mechanism line like the width/split commits — not [nav]: a
    // scope change is a pure view filter (the board's LOCAL/GLOBAL toggle is unlogged for the same reason).
    //   global  - true = GLOBAL, false = LOCAL.
    //   returns - nothing; a no-op before the panel is built and when the list is already on that scope (a pick of the
    //             checked entry: nothing to rebuild or save).
    //   throws  - bad_alloc from the log line only; the menu command contains it.
    void TerminalPage::_SidePanelSetScope(bool global)
    {
        if (!_sidePanelBuilt || _sidePanelGlobal == global)
        {
            return;
        }
        _sidePanelGlobal = global;
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
        if (_sidePanelCollapsed)
        {
            // The "+" launcher card belongs to the header that is going away (SIDE_PANEL.md §2e). The press on ⋯ (the
            // menu that holds the collapse) already closed it as an outside press; this is the belt for a collapse with no
            // press (UI Automation, the keyboard in the menu).
            _SidePanelCloseLaunchCard(L"panel collapsed");
            // The funnel's panel and the folder card hang from the same header (SIDE_PANEL.md §2l): the same belt.
            _SidePanelCloseFilterSurfaces(L"panel collapsed");
        }
        _SidePanelApplyLayout();
        // The cards' status icons (SIDE_PANEL.md §2g): a collapse only COLLAPSES the panel's content, so the cards stay
        // loaded — and without this their pulses, crowns and ring blinks would keep the compositor animating for nobody.
        // An expand lets them move again (each icon re-decides from its own flags). Never throws (SetHostShown is noexcept).
        for (const auto& [cardId, view] : _sidePanelCardViews)
        {
            if (view.glyph)
            {
                // Hidden too while the AI view covers the list (SIDE_PANEL.md §2h): an expand in AI mode shows the AI view,
                // not the session cards, so their icons keep still.
                view.glyph->SetHostShown(!_SidePanelCardsHidden());
            }
        }
        // The AI search's result cards move with the panel itself: shown whenever the expanded panel shows the AI view.
        for (const auto& [resultId, result] : _sidePanelAiResultViews)
        {
            if (result.base.glyph)
            {
                result.base.glyph->SetHostShown(!_sidePanelCollapsed);
            }
        }
        // ...and so does the AI strip's "agent is running" motion: a collapse stops it (nothing animates inside a collapsed
        // panel), an expand starts it again when a request still runs. The agent is asked only when this window ever attached
        // to it (no listener: nothing of its ever ran here, and the first call would CREATE the agent). Contained: the
        // collapse chevron's click calls this with no catch of its own.
        if (_sidePanelAiRoot)
        {
            bool busy = false;
            try
            {
                busy = _sidePanelAiListenerToken != 0 && ::Agentmaster::SharedSessionSearchAgent().Snapshot().busy;
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_SidePanelToggleCollapsed: the AI strip's motion"); // stays off until the next repaint
            }
            _sidePanelAiWorking.SetRunning(_sidePanelAiMode && !_sidePanelCollapsed && busy); // never throws
        }
        if (!_sidePanelCollapsed && _sidePanelDirty)
        {
            _SidePanelRebuildCards(); // the deferred catch-up — the list is visible again (in AI mode it stays owed: the AI view covers the list until it closes)
        }
        if (!_sidePanelCollapsed && _sidePanelAiMode)
        {
            // A collapsed AI view refreshed nothing (the periodic tick idles while collapsed): its cards' states and "ago"
            // texts catch up now that they are on screen again.
            _SidePanelAiRefreshResults();
        }
        if (_sidePanelAiMode && _sidePanelAiRoot)
        {
            // The strip's loading icon follows the panel like the status icons above: a collapse stops it (no ring animates
            // inside a collapsed panel), an expand repaints the strip from the conversation as it is NOW — a request may have
            // started or ended meanwhile, and the strip's repaints while collapsed kept the ring still. Contained: the strip's
            // own click (the expand chevron) calls this with no catch of its own.
            try
            {
                _SidePanelAiUpdateStrip(::Agentmaster::SharedSessionSearchAgent().Snapshot());
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_SidePanelToggleCollapsed: the AI strip");
            }
        }
        if (!_sidePanelCollapsed)
        {
            // ⌖ scroll-to-tab: a tab selected while the panel was collapsed left its reveal OWED (a hidden
            // list has nothing to scroll) — pay it now that the list is back on screen. A no-op when nothing
            // is owed or the rebuild above already queued it (the reveal tick is coalesced).
            _SidePanelQueueReveal();
        }
        _ScheduleWindowRecordSave();
        _SidePanelPersistDefaults(/*width*/ false, /*split*/ false, /*collapsed*/ true);
    }

    // The "Sum" header toggle's click: show / hide the bottom summary area (and its splitter). HIDDEN gives the
    // cards the whole body and moves "what is this session" into the RICH session tooltip each card shows beside
    // the panel on hover — the card a tab header shows, through the same host + push (SIDE_PANEL.md §2d); SHOWN
    // restores the classic layout: the selected session's summary below, each card with its small floating tip.
    // Persists both halves of the collapse contract: this window's record (the autosave reads the live member)
    // and the new-window default (a granular RMW of that one field). Logged as a [nav] line like collapse.
    // No parameters; returns nothing. Never throws past its callees' own containment: every card's switch is
    // contained + logged inside _SidePanelApplyCardTipMode, a failed snapshot is logged (the arm-builds then read
    // each session alone), and persistence failures are logged at the WriteAllUtf8 chokepoint.
    void TerminalPage::_SidePanelToggleSummary()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        _sidePanelSummaryHidden = !_sidePanelSummaryHidden;
        ::Agentmaster::LogNav(_sidePanelSummaryHidden ? L"sidepanel summary -> hidden (cards show the rich session tooltip)" : L"sidepanel summary -> shown");
        _SidePanelUpdateSumButton(); // the ToggleButton flipped itself before Click; re-pin it to the member anyway
        _SidePanelApplyLayout(); // the body rows + the splitter / summary host visibility
        // The summary overlay only does work while it can be seen: hidden releases it (its registry observer
        // detaches, no more off-thread analyses), shown rebinds the CURRENT selection (a peek included) —
        // _SidePanelBindSummary decides both from _sidePanelSummaryHidden.
        _SidePanelBindSummary(_sidePanelSelectedId);
        // Every card switches its tooltip mode. Going to the rich mode arm-builds a header-only card per card, fed
        // from ONE lean snapshot (no queue, no transcript texts) looked up by id — not a whole-record copy per card
        // (the UI_THREAD.md §10g tax: a record carries up to 200 queued prompts).
        std::vector<::Agentmaster::SessionInfo> snap;
        if (_sidePanelSummaryHidden && _sessionRegistry)
        {
            try
            {
                snap = _sessionRegistry->SnapshotLiveForCards();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_SidePanelToggleSummary snapshot"); // each arm-build reads its session alone then
            }
        }
        std::unordered_map<std::wstring_view, const ::Agentmaster::SessionInfo*> byId;
        byId.reserve(snap.size());
        for (const auto& s : snap)
        {
            byId.emplace(s.id, &s);
        }
        for (auto& [id, view] : _sidePanelCardViews)
        {
            const auto row = byId.find(id);
            _SidePanelApplyCardTipMode(view, id, /*rebuildTags*/ true, row != byId.end() ? row->second : nullptr);
        }
        _ScheduleWindowRecordSave();
        _SidePanelPersistDefaults(/*width*/ false, /*split*/ false, /*collapsed*/ false, /*summaryHidden*/ true);
    }

    // Pin the "Sum" toggle's checked state to the live member — checked == the summary area is SHOWN. Called at
    // build and after every click (the ToggleButton flips its own IsChecked before Click fires, so the member,
    // not the button, is the truth). A programmatic IsChecked raises no Click, so re-pinning can't loop or persist.
    // No parameters; returns nothing; a no-op before the panel is built. Throws only what a property write on a
    // live UI-thread control can.
    void TerminalPage::_SidePanelUpdateSumButton()
    {
        if (!_sidePanelSumBtn)
        {
            return;
        }
        _sidePanelSumBtn.IsChecked(!_sidePanelSummaryHidden);
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
        // The "+" launcher card was opened from the header that is moving to the other side (SIDE_PANEL.md §2e). The
        // press on ⋯ (the menu that holds the dock item) already closed it as an outside press; this is the belt for a
        // flip with no press (UI Automation, the keyboard in the menu).
        _SidePanelCloseLaunchCard(L"dock flip");
        _SidePanelCloseFilterSurfaces(L"dock flip"); // the funnel moves with the header too (SIDE_PANEL.md §2l)
        _SidePanelApplyDock();
    }

    // The ⋯ menu's ⌖ item (SIDE_PANEL.md §2i): flip the GLOBAL "scroll to the selected tab" preference
    // (AppSettings::sidePanelScrollToSelectedTab). Persisted as a freshest-disk RMW of that ONE field — the
    // dock flip's recipe, because another window may have saved other settings since this one loaded, and
    // writing this window's whole copy back would regress them. Then, UNLIKE the dock flip, broadcast LIVE:
    // a behavior switch that kept doing the old thing in the other open windows until their next launch
    // would read as "the toggle is broken". The broadcast carries the struct just READ FROM DISK, never
    // this window's _appSettings — that copy can be stale on other out-of-cog fields (a summary-panel toggle
    // flipped in another window, say) and a receiver adopts a broadcast wholesale (_ApplyBroadcastSettings).
    // Turning it ON answers "where is my tab's card?" at once — but only when the focused tab's card IS the
    // highlighted one: a live PEEK of another card is left alone until you touch the terminal or switch tabs
    // (scrolling away from the card you are reading would be the toggle misbehaving). Turning it OFF drops
    // any reveal still owed, so nothing scrolls after you asked it not to.
    // No parameters; returns nothing. Never throws: persistence failures are logged at the WriteAllUtf8
    // chokepoint, and the broadcast is contained here (a window mid-teardown can refuse its dispatcher hop).
    void TerminalPage::_SidePanelToggleScrollToTab()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        const bool next = !_appSettings.sidePanelScrollToSelectedTab;
        auto s = ::Agentmaster::LoadAppSettings();
        s.sidePanelScrollToSelectedTab = next;
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.sidePanelScrollToSelectedTab = next;
        // Nothing to re-pin: the ⋯ menu's ⌖ check is read from this setting at the menu's next open.
        ::Agentmaster::LogNav(next ? L"sidepanel scroll-to-tab -> on" : L"sidepanel scroll-to-tab -> off");
        try
        {
            // Every OTHER window (the source is excluded): each hops to its own UI thread and adopts it through
            // _ApplyBroadcastSettings -> _SidePanelApplySettings (an owed reveal dropped when it turned off; its ⋯ menu
            // shows the new check at its next open).
            ::Agentmaster::BroadcastSettingsChanged(s, _windowId);
        }
        catch (...)
        {
            // The setting is already saved + applied here; another window simply adopts it at its next
            // broadcast or launch. Contained so a click can never escape into the XAML event dispatch.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelToggleScrollToTab broadcast");
        }
        if (!next)
        {
            if (!_sidePanelRevealForced)
            {
                _sidePanelRevealPendingId.clear(); // a PIN's reveal stays (SIDE_PANEL.md §2j): the setting governs tab selections only
            }
            return;
        }
        if (const auto tab = _GetFocusedTab()) // null on the Agent Manager page — no tab, nothing to reveal
        {
            const auto id = _ClaudeSessionForTab(tab);
            if (!id.empty() && id == _sidePanelSelectedId)
            {
                _SidePanelRevealTabCard(id);
            }
        }
    }

    // Fill the header's ⋯ menu (SIDE_PANEL.md §2i) from the live state. The Opening handler empties the menu and calls this
    // at EVERY open, so no check or label can be stale: another window may have moved the order, ⌖ or the dock since the
    // last open. The items, in the user's order (2026-11-03; the dock moved below the separator, before the collapse, on
    // 2026-11-04), each with its symbol LEFT of its text (the item's icon slot: a Segoe UI Symbol glyph,
    // SidePanelMenuGlyph) and the tooltip of the button it replaced (3 s of rest, kSidePanelButtonTipDelay):
    //   ⌖ Scroll to the selected tab — a check item, checked while the GLOBAL setting is on;
    //   ⧉ Card scope: LOCAL ▸        — LOCAL / GLOBAL for this window's list, the current one checked;
    //   ⇅ Card order: MOST ACTIVE ▸  — the five board orders, the current one checked, each with what it sorts by;
    //   ─────
    //   ⇄ Dock on the right / left   — the dock flip, worded as where the panel goes;
    //   « Collapse the side panel    — the glyph points toward the docked edge, like the chevron it replaced.
    // Above the separator: what the cards show (all three only change the list). Below it: where the panel itself goes
    // (both move or shrink the whole panel, and the header this menu hangs from).
    // The submenu headers carry the current scope and order in their text, because the line no longer shows them. Every
    // item keeps the AutomationId of the button it replaced (am.side.dock / scrollToTab / scope / sort / collapse — ids
    // are added, never renamed: the e2e driver knows them), and the submenus' entries add am.side.scope.<local|global> and
    // am.side.sort.<the order's settings.json token>.
    // Every command runs one dispatcher tick later (_SidePanelRunMenuCommand) and first re-checks the state its item was
    // built for, so a menu left open while another window changed the setting acts on what it showed and never inverts it.
    //   menu    - the ⋯ button's MenuFlyout, empty.
    //   returns - nothing.
    //   throws  - what creating and wiring the XAML elements throws (hresult_error, bad_alloc); the Opening handler
    //             contains and logs it (the menu then opens with the items built so far).
    void TerminalPage::_SidePanelFillMoreMenu(const MenuFlyout& menu)
    {
        auto weakThis = get_weak();
        // The state the items are built for, captured by each command so it can tell a still-valid click from a stale one.
        const bool dockedLeft = _appSettings.sidePanelOnLeft;
        const bool scrollToTabOn = _appSettings.sidePanelScrollToSelectedTab;

        // ⌖ — the scroll-to-tab preference, a check item.
        ToggleMenuFlyoutItem scrollToTab{};
        scrollToTab.Text(L"Scroll to the selected tab");
        scrollToTab.IsChecked(scrollToTabOn);
        scrollToTab.Icon(SidePanelMenuGlyph(kSidePanelScrollToTabGlyph));
        AgentSetTitledTip(scrollToTab, L"Scroll to the selected tab", winrt::hstring{ L"On: selecting a tab scrolls these cards to that tab's card \x2014 a card already on screen stays put. Clicking back into the terminal after peeking at another card scrolls back to your tab's card too.\nOff: the list keeps whatever scroll position you left it at.\nSaved globally \x2014 every open window follows it at once." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        AgentSetAutomationId(scrollToTab, L"am.side.scrollToTab");
        scrollToTab.Click([weakThis, scrollToTabOn](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelRunMenuCommand(L"scroll to the selected tab", [scrollToTabOn](TerminalPage& page) {
                    // The item flipped its own check before Click; the setting is the truth. Flip only from the state the
                    // check showed, so another window's change made while the menu was open is not undone.
                    if (page._appSettings.sidePanelScrollToSelectedTab == scrollToTabOn)
                    {
                        page._SidePanelToggleScrollToTab();
                    }
                });
            }
        });
        menu.Items().Append(scrollToTab);

        // ⧉ — the card scope: LOCAL / GLOBAL for THIS window's list.
        MenuFlyoutSubItem scope{};
        scope.Text(_sidePanelGlobal ? L"Card scope: GLOBAL" : L"Card scope: LOCAL");
        scope.Icon(SidePanelMenuGlyph(kSidePanelScopeGlyph));
        AgentSetTitledTip(scope, L"Card scope", winrt::hstring{ L"Which sessions the cards list: LOCAL = only the sessions open in THIS window (the default), GLOBAL = every window's.\nRemembered for this window only \x2014 a new window always starts LOCAL \x2014 and independent of the Triage Board's own scope." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        AgentSetAutomationId(scope, L"am.side.scope");
        for (const bool global : { false, true })
        {
            // A check item per scope, the current one checked. A click on the checked one changes nothing (its own
            // check clears as the menu closes; the next open reads the scope again).
            ToggleMenuFlyoutItem choice{};
            choice.Text(global ? L"GLOBAL \x2014 every window's sessions" : L"LOCAL \x2014 this window's sessions");
            choice.IsChecked(global == _sidePanelGlobal);
            AgentSetAutomationId(choice, global ? L"am.side.scope.global" : L"am.side.scope.local");
            choice.Click([weakThis, global](const IInspectable&, const RoutedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelRunMenuCommand(L"card scope", [global](TerminalPage& page) { page._SidePanelSetScope(global); });
                }
            });
            scope.Items().Append(choice);
        }
        menu.Items().Append(scope);

        // ⇅ — the card order: the SHARED board sort, picked by name.
        const std::wstring_view currentOrder = SidePanelOrderLabel(_appSettings.boardSort);
        MenuFlyoutSubItem order{};
        order.Text(winrt::hstring{ std::wstring{ L"Card order: " }.append(currentOrder) });
        order.Icon(SidePanelMenuGlyph(kSidePanelOrderGlyph));
        AgentSetTitledTip(order, L"Card order", winrt::hstring{ L"Order the session cards. The SAME saved order as the Triage Board's card sort \x2014 a pick here re-sorts the board too (and every window)." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        AgentSetAutomationId(order, L"am.side.sort");
        for (const auto& entry : kSidePanelOrderChoices)
        {
            // Checked by the WORD the header shows, so a stray BY PID (shown as MOST ACTIVE, as on the board) checks
            // MOST ACTIVE; picking it then really sets MOST ACTIVE (the setter compares the orders, not the words).
            ToggleMenuFlyoutItem choice{};
            choice.Text(entry.label);
            choice.IsChecked(currentOrder == entry.label);
            AgentSetTip(choice, winrt::hstring{ entry.tip }, kSidePanelButtonTipDelay); // opens after 3 s of rest
            AgentSetAutomationId(choice, L"am.side.sort." + ::Agentmaster::ToString(entry.order));
            choice.Click([weakThis, picked = entry.order](const IInspectable&, const RoutedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelRunMenuCommand(L"card order", [picked](TerminalPage& page) { page._SidePanelSetSort(picked); });
                }
            });
            order.Items().Append(choice);
        }
        menu.Items().Append(order);

        menu.Items().Append(MenuFlyoutSeparator{});

        // ⇄ — the dock flip, named by where the panel goes (the symbol alone says only "swap"). Below the separator, the
        // last but one: the user's place for it (2026-11-04), beside the collapse — the two items that move the panel itself.
        MenuFlyoutItem dock{};
        dock.Text(dockedLeft ? L"Dock on the right" : L"Dock on the left");
        dock.Icon(SidePanelMenuGlyph(kSidePanelDockGlyph));
        AgentSetTitledTip(dock, L"Dock side", winrt::hstring{ L"Move the panel to the other side of the terminal (left \x21C4 right).\nSaved globally \x2014 other windows adopt it on their next launch or settings change." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        AgentSetAutomationId(dock, L"am.side.dock");
        dock.Click([weakThis, dockedLeft](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelRunMenuCommand(L"dock", [dockedLeft](TerminalPage& page) {
                    // Flip only away from the side the item named: when a settings broadcast already moved the panel there,
                    // a flip would bring it back.
                    if (page._appSettings.sidePanelOnLeft == dockedLeft)
                    {
                        page._SidePanelFlipDock();
                    }
                });
            }
        });
        menu.Items().Append(dock);

        // « / » — the collapse, pointing toward the docked edge like the chevron it replaced; the strip's chevron (pointing
        // away) brings the panel back.
        MenuFlyoutItem collapse{};
        collapse.Text(L"Collapse the side panel");
        collapse.Icon(SidePanelMenuGlyph(dockedLeft ? kSidePanelCollapseLeftGlyph : kSidePanelCollapseRightGlyph));
        AgentSetTip(collapse, winrt::hstring{ L"Collapse the side panel to a thin strip (click the strip to bring it back)." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        AgentSetAutomationId(collapse, L"am.side.collapse");
        collapse.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelRunMenuCommand(L"collapse", [](TerminalPage& page) {
                    // _SidePanelToggleCollapsed toggles: a menu built for the expanded panel must never expand it.
                    if (!page._sidePanelCollapsed)
                    {
                        page._SidePanelToggleCollapsed();
                    }
                });
            }
        });
        menu.Items().Append(collapse);
    }

    // Run one ⋯ menu command one dispatcher tick after its item's Click (SIDE_PANEL.md §2i). Inside the Click the menu is
    // still closing and about to hand focus back, and two of the commands re-lay the very header the menu is anchored to
    // (the collapse hides it, the dock flip moves it to the other column): no tree change inside a pointer handler, and the
    // Agent Manager's menus defer every item one tick for the same reason. Normal priority: the command lands right after
    // the menu's own close work, so the click still feels immediate.
    //   what    - the command's name for the log line a failure writes ("dock", "card scope", ...).
    //   command - the work, handed this page when the tick runs; written inside a TerminalPage member, it may call private
    //             members. Not run when the page is gone by then (the window closed).
    //   returns - nothing.
    //   throws  - nothing: a refused enqueue (the window tearing down) and a throwing command are both contained and
    //             logged (Rule #18) — the click is lost and nothing else changes.
    void TerminalPage::_SidePanelRunMenuCommand(std::wstring_view what, std::function<void(TerminalPage&)> command)
    {
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak(), whatText = std::wstring(what), run = std::move(command)]() {
                const auto page = weakThis.get();
                if (!page)
                {
                    return; // the window closed before the tick: nothing left to change
                }
                try
                {
                    run(*page);
                }
                catch (...)
                {
                    // The command stopped part-way; what it changed before the throw stays (each command saves its own
                    // change first). Recorded with the command's name (Rule #18).
                    ::Agentmaster::AgentLogCaughtException((L"side panel more menu: " + whatText).c_str());
                }
            });
        }
        catch (...)
        {
            // The dispatcher refused the hop (the window is tearing down): the click is lost. Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"side panel more menu: queue a command");
        }
    }

    // The selected TAB now shows session `id` (a tab switch, or a terminal touch that ended a peek): when
    // the ⌖ toggle is ON, owe the card list ONE scroll to that card and queue it. Recording a debt instead
    // of scrolling on the spot is what makes it reliable: the card may not exist yet (a just-launched
    // session's card lands on the NEXT rebuild, a collapsed panel shows no list, the filter may hide it),
    // and a card built this very tick has no size until it is laid out — the first reveal tick that finds
    // the card RENDERED pays it (_SidePanelRevealPendingNow).
    // `id`: the selected tab's managed session. EMPTY means "a tab with no card" (pwsh / cmd / external) —
    //   which, like the toggle being OFF, CANCELS any reveal still owed to the tab you just left: it must
    //   never fire later and scroll the list to a tab you are no longer on.
    // Returns nothing. Throws only on allocation failure (the id copy); the queue guards its own dispatch.
    void TerminalPage::_SidePanelRevealTabCard(const std::wstring& id)
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        // A tab selection's debt is never a pin's forced one (SIDE_PANEL.md §2j): whichever way this goes, it replaces or drops a
        // pin's unpaid scroll — the user moved to a tab, and the list follows that (or nothing) now.
        _sidePanelRevealForced = false;
        if (id.empty() || !_appSettings.sidePanelScrollToSelectedTab)
        {
            _sidePanelRevealPendingId.clear();
            return;
        }
        _sidePanelRevealPendingId = id; // a newer selection REPLACES an older unpaid debt — only the last tab counts
        _SidePanelQueueReveal();
    }

    // Coalesce the owed reveal onto ONE Low-priority dispatcher tick. LOW on purpose: the tick must run
    // AFTER whatever may have just created or moved the card (a rebuild, the registry drain, the tab
    // switch's own content swap), so the reveal measures the list the user will actually see — the same
    // ordering the Manager board's BringSelectedIntoView relies on. Coalesced so rapid Ctrl+Tab cycling
    // folds into ONE scroll to the LAST tab rather than a queue of scrolls replaying the whole journey (the
    // tick reads the owed id fresh when it runs). A no-op while collapsed (a hidden list has nothing to
    // scroll — the debt stays owed for the expand path), when nothing is owed, or when a tick is queued.
    // No parameters; returns nothing. Never throws: a dispatcher that is gone (window teardown) is logged
    // and the coalescer re-armed, so the latch can never stick shut.
    void TerminalPage::_SidePanelQueueReveal()
    {
        // _SidePanelCardsHidden: a collapsed panel, and a card list the AI view covers (SIDE_PANEL.md §2h), have nothing to
        // scroll — the debt stays owed for the expand / the AI view's close, which re-queue it.
        if (!_sidePanelBuilt || _SidePanelCardsHidden() || _sidePanelRevealPendingId.empty() || _sidePanelRevealQueued)
        {
            return;
        }
        _sidePanelRevealQueued = true;
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis = get_weak()]() {
                if (const auto self = weakThis.get())
                {
                    self->_sidePanelRevealQueued = false; // re-arm BEFORE the body, so no outcome of it can leave the latch shut
                    self->_SidePanelRevealPendingNow();
                }
            });
        }
        catch (...)
        {
            _sidePanelRevealQueued = false;
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelQueueReveal");
        }
    }

    // The reveal tick's body: pay the owed scroll when its session is a LISTED row. A row outside the realized range
    // has no card (SIDE_PANEL.md §2f), so the rows around it are realized first (RowsAroundRow: wherever the minimum
    // scroll leaves the viewport, its rows exist, and the scroll that follows needs no second pass). UpdateLayout
    // then — a card created by the rebuild or the realize pass that preceded this tick has no size until measured,
    // and StartBringIntoView on it would silently do nothing — then StartBringIntoView: the Manager board's own
    // recipe (BringSelectedIntoView). Default BringIntoViewOptions mean the MINIMUM scroll that shows the card, so a
    // card already fully on screen never moves and a far one stops at the nearest edge. A card that had to be
    // realized is reached WITHOUT the scroll animation: an animated scroll across hundreds of rows would realize and
    // drop cards at every intermediate offset on its way. The request walks up to the cards ScrollViewer only (the
    // panel has no outer scroller) and never touches keyboard focus, so the terminal keeps its typing focus through
    // every reveal. The debt is consumed BEFORE the XAML calls, so a throwing reveal can't turn into one that
    // retries on every later rebuild; a session that is NOT listed yet (filtered out, its first update not run)
    // keeps it owed for the next rebuild.
    // No parameters; returns nothing. Never throws: XAML failures (a teardown race, a layout error) are
    // logged (Rule #18) and swallowed — a lost scroll must never cost the window.
    void TerminalPage::_SidePanelRevealPendingNow()
    {
        if (!_sidePanelBuilt || _SidePanelCardsHidden() || _sidePanelRevealPendingId.empty())
        {
            return; // collapsed / covered by the AI view: the expand path or the view's close re-queues; nothing owed: a stale tick
        }
        if (!_appSettings.sidePanelScrollToSelectedTab && !_sidePanelRevealForced)
        {
            _sidePanelRevealPendingId.clear(); // switched off (here or by a broadcast) after the request was made
            return;
        }
        // (A PIN's reveal — _sidePanelRevealForced, SIDE_PANEL.md §2j — is paid with ⌖ off too: it is the answer to the user's
        // own "Pin to Top", which moved the card to the top of the list, not a selection the setting governs.)
        const auto row = std::find(_sidePanelRowIds.begin(), _sidePanelRowIds.end(), _sidePanelRevealPendingId);
        if (row == _sidePanelRowIds.end())
        {
            return; // not listed (yet) — _SidePanelRebuildCards re-queues; a newer tab selection replaces the debt
        }
        const size_t rowIndex = static_cast<size_t>(row - _sidePanelRowIds.begin());
        const bool realized = rowIndex >= _sidePanelRealizedFirst && rowIndex < _sidePanelRealizedLast;
        if (!realized && !_SidePanelRunRealizePass(L"reveal", rowIndex))
        {
            return; // the pass failed and reset the list (logged); the debt stays owed for the rebuild that follows
        }
        const auto it = _sidePanelCardViews.find(_sidePanelRevealPendingId);
        if (it == _sidePanelCardViews.end() || !it->second.card)
        {
            return; // belt: a realized row always has its card — keep the debt for the next rebuild
        }
        const auto card = it->second.card; // a strong ref: layout below can't release it under us
        _sidePanelRevealPendingId.clear();
        _sidePanelRevealForced = false; // the debt is paid, a pin's included
        try
        {
            if (_sidePanelCards)
            {
                _sidePanelCards.UpdateLayout();
            }
            if (realized)
            {
                card.StartBringIntoView();
            }
            else
            {
                BringIntoViewOptions options{};
                options.AnimationDesired(false); // a jump: an animated scroll would realize every row on its way
                card.StartBringIntoView(options);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelRevealPendingNow");
        }
    }

    // The "last one to adjust sets the default" half: RMW ONLY the flagged AppSettings sidePanel*
    // fields from this window's live values (granular on purpose — a width drag must never export
    // this window's collapse or summary state as the global default), then keep the in-memory copy in
    // step so a later cog Save's preserve block can't regress it.
    // `width`/`split`/`collapsed`/`summaryHidden`/`aiMode`: which live values to export as the new-window defaults
    // (`summaryHidden` and `aiMode` default to false so the older call sites read unchanged; `aiMode` is the filter box's
    // "AI" toggle, SIDE_PANEL.md §2h).
    void TerminalPage::_SidePanelPersistDefaults(bool width, bool split, bool collapsed, bool summaryHidden, bool aiMode)
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
        if (summaryHidden)
        {
            s.sidePanelSummaryHidden = _sidePanelSummaryHidden;
        }
        if (aiMode)
        {
            s.sidePanelAiMode = _sidePanelAiMode;
        }
        ::Agentmaster::SaveAppSettings(s);
        // Adopt the merged set (not just the flagged one): the freshest disk may carry another
        // window's newer defaults, and _appSettings must mirror what is actually stored.
        _appSettings.sidePanelWidthFraction = s.sidePanelWidthFraction;
        _appSettings.sidePanelSplitFraction = s.sidePanelSplitFraction;
        _appSettings.sidePanelCollapsed = s.sidePanelCollapsed;
        _appSettings.sidePanelSummaryHidden = s.sidePanelSummaryHidden;
        _appSettings.sidePanelAiMode = s.sidePanelAiMode;
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
        if (_sidePanelAiMode)
        {
            // The AI view covers the card list (SIDE_PANEL.md §2h): the list catches up when the view closes (_SidePanelSetAiMode
            // rebuilds it), even for a user action — a forced rebuild would also write the count line the AI view owns now.
            // The same change may concern a session the model listed (it opened, closed or moved on): its result card owes a
            // refresh, which the periodic tick pays (never more than once per tick, however busy the fleet's drain is).
            _sidePanelDirty = true;
            _sidePanelAiRefreshOwed = true;
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

    // A bookmark tag was added to, removed from, or recolored on a session FROM THIS WINDOW: the Tags
    // panel's two mutation paths (_ToggleSessionTag, _CommitTagEditorAdd) call this right after the
    // board's _RefreshManagerBoardTags. (Since 2026-10-10 they reach both through
    // _QueueTagSurfacesRefresh -> _RunTagSurfacesRefresh, ~40 ms after the last edit, so the Tags panel's
    // own list is rebuilt first; the order here is unchanged.) Tags live in the SessionStore, never the registry, so no
    // registry drain will ever report the change — without this call the cards' top-right ribbons would
    // keep showing the old tags until some unrelated session happened to change. Same-window instant,
    // like the tab badges and the board; another window's side panel catches up on its own next rebuild
    // (the cross-window lag every tag surface accepts). Deleting a 0-carrier tag (_RemoveGlobalTag) needs
    // no call: no card can be showing a tag that nothing carries.
    // While collapsed it only marks the list dirty (the expand path rebuilds). No-op before
    // _InitAgentSidePanel. UI thread only.
    // Never throws: a failed rebuild is logged (Rule #18) and left dirty for the periodic tick to retry.
    void TerminalPage::_SidePanelNoteTagsChanged()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        if (_sidePanelCollapsed)
        {
            _sidePanelDirty = true; // nothing on screen to repaint — the expand path runs the catch-up rebuild
            return;
        }
        if (_sidePanelAiMode)
        {
            // The AI view is on screen (SIDE_PANEL.md §2h): the hidden card list catches up when the view closes, and the
            // result cards — which may show the session just tagged — re-read their tags NOW (a user's edit, so not on the
            // next tick): the epoch moved, so their cached tags and colors are dropped first. Never throws (contained inside).
            _sidePanelDirty = true;
            _sidePanelAiTagsEpoch = UINT64_MAX;
            _SidePanelAiRefreshResults();
            return;
        }
        // A deliberate user edit, so NOW — past the registry-drain min-gap throttle (a throttled rebuild
        // would only be replayed on the next periodic tick, seconds after the Tags panel click).
        // Contained: this runs inside the Tags panel's click, which also repaints the tab badges, the
        // Sessions page and the board — a failed card rebuild must cost only the side panel's freshness
        // (it stays dirty, so the next tick retries), never that click or the window (Rule #18).
        try
        {
            _SidePanelRebuildCards();
        }
        catch (...)
        {
            _sidePanelDirty = true;
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelNoteTagsChanged");
        }
    }

    // The page's batched registry drain calls this (UI thread) once per drained change set —
    // sessions changed somewhere in the fleet, so the card list may be stale.
    void TerminalPage::_SidePanelNoteRegistryChange()
    {
        _SidePanelScheduleRebuild(/*force*/ false);
    }

    // The unread census moved its count (this window's census-sink hop — see the declaration): re-derive every row's
    // flash ring in place. Not the throttled list update: a flash is an ALERT, and the tab's ring shows at once, so the
    // card's must not wait out the 500 ms min gap and the next periodic tick (~2.5 s) behind it. The patch reads the
    // census once and writes only the cards whose ring changed, so it is cheap enough to run on every count change.
    // A collapsed panel is patched too (the rows' models must stay current for the expand); its icons just do not move.
    void TerminalPage::_SidePanelNoteUnreadChanged()
    {
        _SidePanelRefreshIndicators(/*rereadCacheWarm*/ false);
        // With the Unread flag on (SIDE_PANEL.md §2l) the census decides which cards are LISTED, not only which ring: a session
        // that turned unread must appear, one just visited must leave. The in-place patch above never adds or drops a row, so
        // the list update runs too — the throttled one (an unread flip is no user action), which the next tick catches up.
        if (_sidePanelFilters.unread)
        {
            _SidePanelScheduleRebuild(/*force*/ false);
        }
    }

    void TerminalPage::_SidePanelRefreshIndicators(bool rereadCacheWarm)
    {
        if (!_sidePanelBuilt || _sidePanelRowIds.empty())
        {
            return; // nothing listed yet: the first list update computes every indicator itself
        }
        if (_sidePanelRealizing)
        {
            // Never under a realize pass (it walks the rows and the card map this patch writes): fold into the next
            // update instead. Nothing a pass does reaches here synchronously today; this is the belt.
            _sidePanelDirty = true;
            return;
        }
        try
        {
            const size_t count = _sidePanelRowIds.size();
            if (_sidePanelRowModels.size() != count)
            {
                return; // belt: the rows are written as a parallel pair (one move each), so this never happens
            }
            // Every row whose model changed, so only their cards are written, once each.
            std::vector<size_t> changed;

            // 1. The FLASH RING of every row: membership in the census union — the sessions whose tab's ring is flashing,
            //    in whichever window hosts it (each page publishes its own set; the census unions them). Sorted, so one
            //    binary search per row; a few hundred rows cost microseconds.
            const auto unread = ::Agentmaster::CurrentUnreadSessionIds();
            for (size_t row = 0; row < count; ++row)
            {
                auto& model = _sidePanelRowModels[row];
                const bool ring = std::binary_search(unread.begin(), unread.end(), _sidePanelRowIds[row]);
                if (model.flashRing != ring)
                {
                    model.flashRing = ring;
                    changed.push_back(row);
                }
            }

            // 2. The SPARK CROWN of the REALIZED rows — the cards on screen — from a fresh lean read of each: the tab
            //    strip's own poll (_ScanCacheWarmTabsImpl: GetWithoutQueue + ServerCacheStillWarm, the same 0 -> 5
            //    minutes normalization), so a card's crown lights and goes out on the same tick as its tab's. A row
            //    without a card keeps the value of the last list update; it is re-read here once a scroll realizes it.
            if (rereadCacheWarm && _sessionRegistry)
            {
                const uint32_t cacheMin = _appSettings.serverCacheMinutes ? _appSettings.serverCacheMinutes : 5;
                const int64_t now = NowMs();
                const uint32_t warmArgb = SidePanelCacheWarmArgb();
                const size_t last = std::min(_sidePanelRealizedLast, count);
                for (size_t row = _sidePanelRealizedFirst; row < last; ++row)
                {
                    // Without the prompt queue (UI_THREAD.md §10a #4): ServerCacheStillWarm reads the state, the turn
                    // stamps and the kind, never the queue — the costliest part of a Get.
                    const auto info = _sessionRegistry->GetWithoutQueue(_sidePanelRowIds[row]);
                    // A session that left the registry between the last update and now reads cold; the next list update
                    // drops its row anyway.
                    const bool warm = info && ::Agentmaster::ServerCacheStillWarm(*info, cacheMin, now);
                    auto& model = _sidePanelRowModels[row];
                    const uint32_t argb = warm ? warmArgb : 0u;
                    if (model.cacheWarm != warm || model.cacheWarmArgb != argb)
                    {
                        model.cacheWarm = warm;
                        model.cacheWarmArgb = argb;
                        changed.push_back(row);
                    }
                }
                // A row whose ring AND crown both changed was pushed by both passes: sort + unique, so each card is
                // written once.
                std::sort(changed.begin(), changed.end());
                changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
            }

            // 3. Write the changed rows that have cards. A realized card shows exactly its row's model (the list update and
            //    every realize pass write it), so handing it the patched row model writes only the patched indicators.
            for (const size_t row : changed)
            {
                if (row < _sidePanelRealizedFirst || row >= _sidePanelRealizedLast)
                {
                    continue; // no card: a scroll that realizes the row builds its card from the patched model
                }
                const auto& id = _sidePanelRowIds[row];
                if (const auto it = _sidePanelCardViews.find(id); it != _sidePanelCardViews.end())
                {
                    _SidePanelApplyCardModel(it->second, _sidePanelRowModels[row], id, /*fresh*/ false); // a copy: the row keeps its model
                }
            }
        }
        catch (...)
        {
            // A refused XAML write or an allocation failure: the same recovery as every other card-list edit — the list
            // starts over from empty on the next tick, so a half-patched card never outlives this call.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelRefreshIndicators");
            _SidePanelResetCards();
            _sidePanelDirty = true;
        }
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

    // The periodic body (UI thread after the hop): (1) run a list update deferred by the throttle, or owed
    // because the hosted set or the tags changed without a registry notify, (2) kick the summary overlay's
    // cheap mtime-gated content re-check (so a quiet session's last message still lands without a registry
    // notify), (3) after ~30s without any list update, refresh the cards' timing texts through the same
    // in-place update — which writes only the texts whose value moved (SIDE_PANEL.md §2c), never a rebuild
    // just to move an "ago" string (the Manager lens lesson), and reads the lean card snapshot instead of a
    // full registry copy per card, (4) on every other tick, re-derive the status icons' spark crown (the realized
    // cards, a fresh lean read each — warmth ends on a clock) and flash ring (every row, from the unread census) in
    // place, without a list update (SIDE_PANEL.md §2g, _SidePanelRefreshIndicators).
    winrt::Windows::Foundation::IAsyncAction TerminalPage::_SidePanelPeriodicTickImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sidePanelBuilt || _sidePanelCollapsed)
        {
            co_return; // collapsed: stay fully idle — the expand path runs the catch-up
        }
        if (_sidePanelAiMode)
        {
            // The AI view covers the card list (SIDE_PANEL.md §2h): the list's belts and its timing pass wait for the view to
            // close (which rebuilds it). What IS on screen keeps fresh: the summary area (it stays below the AI view), and the
            // result cards — when a registry change owed them a refresh, or ~30 s passed (the "ago" texts age).
            if (_sidePanelSummaryOverlay)
            {
                _sidePanelSummaryOverlay->RefreshSummaryContent(); // mtime-gated: a quiet transcript costs one stat
            }
            if (_sidePanelAiRefreshOwed || ::GetTickCount64() >= _sidePanelAiNextRefreshMs)
            {
                _SidePanelAiRefreshResults(); // re-arms the 30 s deadline and clears the debt itself; never throws
            }
            co_return;
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
        // Tags belt: a tag added, removed or recolored in ANOTHER window (or on the Sessions page) fires no
        // registry notify and no _SidePanelNoteTagsChanged here — but it moves the process-wide tag epoch the
        // cards' tag cache was read at. One atomic load per tick.
        if (!_sidePanelDirty && ::Agentmaster::SessionTagsEpoch() != _sidePanelTagsEpoch)
        {
            _sidePanelDirty = true;
        }
        // Favorites belt (SIDE_PANEL.md §2l): with the Favorite flag on, a ★ set or cleared anywhere in this process (the
        // Sessions page, a tab's menu, another window) moves the facts epoch the ★ cache was read at — and decides which
        // cards are listed. One atomic load per tick, only while the flag is on.
        if (!_sidePanelDirty && _sidePanelFilters.favorite && ::Agentmaster::SessionFactsEpoch() != _sidePanelFavoritesEpoch)
        {
            _sidePanelDirty = true;
        }
        if (_sidePanelDirty)
        {
            _SidePanelRebuildCards(); // the update refreshes the timing texts too — no separate timing pass
            co_return;
        }
        if (_sidePanelSummaryOverlay)
        {
            _sidePanelSummaryOverlay->RefreshSummaryContent(); // mtime-gated: a quiet transcript costs one stat
        }
        if (::GetTickCount64() < _sidePanelNextTimingMs)
        {
            // No list update this tick, so the status icons' two indicators that change with no registry notify are
            // re-derived in place (SIDE_PANEL.md §2g): the spark crown of the realized cards — warmth ENDS on a clock,
            // and this tick is the tab strip's own ~2.5 s cache-warm poll's cadence (_ScanCacheWarmTabs runs in the
            // same liveness probe, just before) — and every row's flash ring against the unread census (the belt for a
            // census change that kept its count, which no sink hears). Writes only the cards whose value moved.
            _SidePanelRefreshIndicators(/*rereadCacheWarm*/ true);
            co_return;
        }
        // ~30 quiet seconds: the timing texts age, so run the in-place update — it rewrites only the cards
        // whose text moved (and re-arms the next deadline itself).
        _SidePanelRebuildCards();
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

    // Bring the card list up to date with the live fleet — IN PLACE (SIDE_PANEL.md §2c). The rows:
    // SnapshotLiveForCards (the live sessions without their queues and transcript texts — never the full
    // archived Snapshot, the Manager-lens tax rule) -> the SCOPE (LOCAL, the default: only sessions hosted
    // in THIS window per _HostedSessionIds; GLOBAL: every window's) -> the search filter (the Sessions
    // grammar) -> the SHARED boardSort order (MakeSortKey/SortKeyLess — the board's own comparator, with one
    // key per session). Then one AgentSidePanelCardModel per row, stored with the row (_sidePanelRowIds /
    // _sidePanelRowModels), and the list is EDITED, never rebuilt (_SidePanelSyncRealizedCards). Only the rows
    // near the viewport have cards (SIDE_PANEL.md §2f, VirtualListWindow.h); the others are two spacers:
    //   - a realized session that keeps its card gets only the values that changed written (_SidePanelApplyCardModel);
    //   - a row newly realized gets a card (_SidePanelCreateCardView + a fresh apply);
    //   - the order changes with the fewest removes / inserts / moves (PlanKeyedListOps). A XAML Move is a
    //     plain reorder of the panel's items (no Unloaded/Loaded), so a card that moves keeps its elements,
    //     its pulsing dots and its hover state.
    // In the steady state (one session changed its state) that is one card's dot, label and border — or nothing
    // at all when that session's row is not realized. The old Clear() + rebuild re-created every card — ~466 in
    // the release's biggest window — up to twice a second; until §2f every listed session had a card (~706 per
    // GLOBAL window on 2026-10-20, UI_THREAD.md §13).
    // The reads are lean too: each row's tags come from the per-row cache, re-read only when
    // SessionTagsEpoch moved (a tag write anywhere in this process) or for a session new to the list, and every
    // row's color resolves against ONE LoadDirColorIndex snapshot. Also refreshes the count line (which names
    // the scope; written only when its text changed), every realized card's timing text (so the ~30s timing pass
    // is due only after 30 quiet seconds), and the hosted set the periodic LOCAL belt diffs against; and pays an
    // owed ⌖ reveal.
    // Tags live in the SessionStore, not the registry, so a tag edit fires no registry trigger: this window's
    // own edits call _SidePanelNoteTagsChanged, and an edit made in another window moves the epoch, which the
    // periodic tick notices (_SidePanelPeriodicTickImpl).
    // No-op before _InitAgentSidePanel (or without a registry). UI thread only.
    // Never throws: a failure midway (a XAML edit refused — a torn-down tree — or an allocation) is logged
    // (Rule #18), the list is reset to empty (_SidePanelResetCards) and marked dirty, and the next tick
    // rebuilds it from scratch. The in-place edits rely on the order mirror matching the panel exactly, and
    // after a partial edit it might not.
    void TerminalPage::_SidePanelRebuildCards()
    {
        if (!_sidePanelBuilt || !_sidePanelCards || !_sessionRegistry)
        {
            return;
        }
        if (_sidePanelAiMode)
        {
            // The AI view covers the card list (SIDE_PANEL.md §2h): every caller's update is owed instead — the expand path,
            // a tag edit, the build's first update when the window opens in AI mode — and _SidePanelSetAiMode(false) pays it
            // the moment the list is on screen again. Running it now would write the count line the AI view owns.
            _sidePanelDirty = true;
            return;
        }
        if (_sidePanelRealizing)
        {
            // Belt: an update never runs inside a realize pass (both are UI-thread work, and nothing a pass does raises
            // a registry drain or a user action synchronously). If one ever did, replacing the rows would pull them
            // out from under the pass's loop — so it is folded into the next periodic tick instead.
            _sidePanelDirty = true;
            return;
        }
        _sidePanelLastRebuildMs = ::GetTickCount64();
        _sidePanelDirty = false;
        // This update writes every card's current timing text, so the periodic timing pass is owed only
        // after 30 seconds without one.
        _sidePanelNextTimingMs = _sidePanelLastRebuildMs + kSidePanelTimingRefreshMs;
        try
        {
            const auto mode = _appSettings.tabColorMode;
            const auto snap = _sessionRegistry->SnapshotLiveForCards();
            const auto terms = ::Agentmaster::ParseSessionQuery(_sidePanelSearchText);
            // LOCAL scope (the default): only the sessions whose tab lives in THIS window — the hosted
            // set, snapshotted ONCE per update and remembered so the periodic tick can detect a hosting
            // change the registry never announces (a tab docked in / torn out — see _SidePanelPeriodicTickImpl).
            const bool local = !_sidePanelGlobal;
            std::unordered_set<std::wstring> hosted;
            if (local)
            {
                hosted = _HostedSessionIds();
            }
            _sidePanelRenderedLocalIds = hosted; // empty under GLOBAL — the belt only diffs while LOCAL

            // Bookmark tags and ★, from their per-session caches. The epochs are loaded BEFORE any read (the SessionStore.h
            // contract): a write landing during the reads below leaves the older epoch stored, so the next update re-reads — a
            // cache refreshed once too often, never kept stale. A moved epoch means some tag, tag color or favorite changed in
            // this process since the last read, so that cache is dropped (_SidePanelSyncFactCaches). A card new to the list
            // reads its tags once (GetSessionTags: SessionStore's (size,mtime)-validated record cache — one file stat); a card
            // that stays reads nothing. Rows the filters hid are never read — unless a filter needs them (By Tag, Favorite).
            _SidePanelSyncFactCaches();
            // The status icon's per-update inputs (SIDE_PANEL.md §2g) — read here, before the rows, because the Unread filter
            // (SIDE_PANEL.md §2l) judges membership by the same census the rings show.
            const SidePanelIndicatorInputs indicators = SidePanelReadIndicatorInputs(_appSettings);
            // The FILTERS (SIDE_PANEL.md §2l): compiled once per update (folder keys, folded tag names, this update's clock). An
            // unfiltered window builds no matcher and judges nothing beyond the typed text — the list as before the feature.
            std::optional<::Agentmaster::SidePanelFilterMatcher> matcher;
            if (::Agentmaster::SidePanelFiltersActive(_sidePanelFilters))
            {
                matcher.emplace(_sidePanelFilters, indicators.nowMs);
            }

            // The rows, each with its sort key built ONCE: a comparator that built both keys per comparison
            // (a title copy each) cost O(n log n) string copies per update.
            struct CardRow
            {
                SortKey key;
                const ::Agentmaster::SessionInfo* session;
            };
            // ⚠ The comparator below reads each row's PIN through `session` (SessionInfo::sidePanelPinnedAtUnixMs, SIDE_PANEL.md
            // §2j): `snap` owns the sessions and outlives the sort, so the pointer is valid for every comparison.
            std::vector<CardRow> rows;
            rows.reserve(snap.size());
            size_t liveTotal = 0; // the IN-SCOPE total (what "N of M" counts against — never another window's sessions under LOCAL)
            // The sessions the TAG filter read tags for: their cache entries are kept below like the listed rows', so a tag
            // filter over a big scope does not re-read every unlisted session's tags at every update.
            std::unordered_set<std::wstring_view> tagJudged;
            for (const auto& s : snap)
            {
                if (!s.live)
                {
                    continue; // belt — SnapshotLiveForCards is already live-only
                }
                if (local && hosted.find(s.id) == hosted.end())
                {
                    continue; // hosted by another window (or a live session with no tab here) — not "here"
                }
                ++liveTotal;
                if (!SidePanelSessionMatches(s, terms, mode))
                {
                    continue; // the typed text hides it (and the filters need not look)
                }
                if (matcher)
                {
                    // Only what the active facets read: the tags (a cached session-store read) for By Tag, the ★ for Favorite;
                    // the rest are fields of the lean snapshot and the census read above.
                    const bool needTags = matcher->NeedsTags();
                    const auto facts = _SidePanelFilterFactsOf(s, needTags, matcher->NeedsFavorite(), indicators.unreadIds);
                    if (needTags)
                    {
                        tagJudged.insert(s.id);
                    }
                    if (!matcher->Matches(facts, /*textOk*/ true))
                    {
                        continue; // a filter hides it
                    }
                }
                rows.push_back(CardRow{ MakeSortKey(s), &s });
            }
            // PINNED FIRST (SIDE_PANEL.md §2j, the user's "pinning puts on top and repinning will bring a tab to the top again"):
            // every card pinned to the top comes before the rest, the newest pin first — a pin again restamps a card, so it goes
            // back to the top — and the board's SHARED order ranks everything the pins leave equal (the unpinned rows, and pins
            // of one stamp). The pins first, then the board's order: that pairing is a lexicographic order on (pin class, board
            // key), so the comparator stays the strict weak ordering std::stable_sort needs (CompareSidePanelPins). Only the
            // cards are ordered so: the Triage Board and the tree keep their own order.
            std::stable_sort(rows.begin(), rows.end(), [this](const CardRow& a, const CardRow& b) {
                const int pin = ::Agentmaster::CompareSidePanelPins(a.session->sidePanelPinnedAtUnixMs, b.session->sidePanelPinnedAtUnixMs);
                return pin < 0 || (pin == 0 && SortKeyLess(_appSettings.boardSort, a.key, b.key));
            });

            // Bookmark tags, from the per-card cache (its epoch was checked above, _SidePanelSyncFactCaches, before any read).
            bool anyTagged = false;
            for (const auto& row : rows)
            {
                auto it = _sidePanelTagsById.find(row.session->id);
                if (it == _sidePanelTagsById.end())
                {
                    it = _sidePanelTagsById.emplace(row.session->id, ::Agentmaster::GetSessionTags(row.session->id)).first;
                }
                anyTagged = anyTagged || !it->second.empty();
            }
            // While the funnel's panel is open its counts read every session's tags (the census, at every pick): nothing is
            // pruned then, so a pick never re-reads the whole scope's tags; the first update after it closes prunes.
            if (!_SidePanelFilterPanelOpen() && _sidePanelTagsById.size() > rows.size() + tagJudged.size())
            {
                // Keep only the listed rows' entries — and, while By Tag filters (SIDE_PANEL.md §2l), the entries of every
                // session it judged, listed or not — so the cache can't grow with every session that ever passed through the
                // list (a session that comes back re-reads its tags once). Node handles move each kept entry over without
                // copying its strings. ⚠ The judged views point into `snap`'s ids, which outlive this block; the facts' tag
                // pointers into the old map are not used past the loop above.
                std::unordered_map<std::wstring, std::vector<std::wstring>> kept;
                kept.reserve(rows.size() + tagJudged.size());
                const auto keep = [&](const std::wstring& id) {
                    auto node = _sidePanelTagsById.extract(id);
                    if (!node.empty())
                    {
                        kept.insert(std::move(node));
                    }
                };
                for (const auto& row : rows)
                {
                    keep(row.session->id);
                }
                for (const auto& judged : tagJudged)
                {
                    keep(std::wstring{ judged });
                }
                _sidePanelTagsById.swap(kept);
            }
            if (anyTagged && !_sidePanelTagColorsLoaded)
            {
                // The colors file is read once per epoch, and only when a listed row is tagged at all — an
                // untagged fleet never pays for it.
                _sidePanelTagColors = ::Agentmaster::LoadAllTagColors();
                _sidePanelTagColorsLoaded = true;
            }

            // One model per ROW — realized or not: a scroll realizes a row from its stored model, so every row's must
            // be current. The shared inputs are read ONCE per update: the dir-colors snapshot every row's band color
            // resolves against, the dots' color, Windows' animation switch, and the status icon's inputs (the unread
            // census, the cache lifetime, the clock — SIDE_PANEL.md §2g).
            const auto dirColors = ::Agentmaster::LoadDirColorIndex();
            // The tab strip's own rule (PendingDotsColorFor) judged against the card BODY, where the icon's dots sit: a
            // dark body, so the configured LIGHT color (the same value the cards used before the rule was applied here).
            const uint32_t pendingArgb = SidePanelPackArgb(PendingDotsColorFor(kSidePanelCardBodyColor, _appSettings.pendingDotsLightColor, _appSettings.pendingDotsDarkColor));
            const bool animationsEnabled = AgentAnimationsEnabled();
            // (The status icon's per-update inputs, `indicators`, were read before the rows: the Unread filter needs the census.)
            std::vector<std::wstring> order; // the new row order, as session ids (the realized range is a window over it)
            std::vector<AgentSidePanelCardModel> models; // parallel to `order`
            // Parallel to `order` too: each row's lean session (pointers into `snap`, alive for this whole update)
            // — the "Sum" off mode arm-builds a card newly realized by THIS update from it instead of a per-card
            // registry read (a card realized later by a scroll reads GetWithoutQueue instead).
            std::vector<const ::Agentmaster::SessionInfo*> sessions;
            order.reserve(rows.size());
            models.reserve(rows.size());
            sessions.reserve(rows.size());
            // Belt: the planner needs unique ids, and a session listed twice must get ONE row. The registry
            // never lists an id twice, so this only guards the guarantee (views into `snap`'s ids, no copies).
            std::unordered_set<std::wstring_view> seen;
            seen.reserve(rows.size());
            for (const auto& row : rows)
            {
                if (!seen.insert(row.session->id).second)
                {
                    continue;
                }
                order.push_back(row.session->id);
                models.push_back(SidePanelCardModelFor(*row.session, mode, _sidePanelTagsById[row.session->id], _sidePanelTagColors, dirColors, pendingArgb, animationsEnabled, indicators));
                sessions.push_back(row.session);
            }

            // Keep the rows: the realized range is a window over them, and a scroll realizes other rows from these
            // stored models without a new snapshot or a registry read (SIDE_PANEL.md §2f).
            _sidePanelRowIds = std::move(order);
            _sidePanelRowModels = std::move(models);
            // The cards: only the rows near the viewport have one (VirtualListWindow.h), each edited in place — the
            // realized cards that stay get only their changed values, the newly realized rows get new cards, and the
            // order changes with the fewest removes / inserts / moves.
            if (!_SidePanelSyncRealizedCards(/*modelsChanged*/ true, &sessions, kSidePanelNoAnchorRow))
            {
                // A planned edit applied in order can't end anywhere else — this would be a planner or mirror bug, or a
                // panel that kept its old children because a reset could not Clear() it (the mirror then planned from
                // empty while the panel was not). Editing on from a wrong mirror would only compound it, so start over
                // from empty instead.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] card list edit ended out of step with its plan - rebuilding from empty (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
                _SidePanelResetCards();
                _sidePanelDirty = true;
                return;
            }

            // The row under the filter box (SIDE_PANEL.md §2l): the count line's words are gone from the screen (the user's
            // 2026-11-08 "replaced by a row only shown when filter is on") — the chips row says what narrows the list while
            // something does; the count words live on as the filter box's ItemStatus.
            _SidePanelUpdateFilterRow(_sidePanelRowIds.size(), liveTotal, !terms.empty());
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelRebuildCards");
            _SidePanelResetCards();
            _sidePanelDirty = true; // the next periodic tick rebuilds the list from empty
            return;
        }
        // ⌖ scroll-to-tab: a reveal owed to a card that did NOT exist yet (a just-launched session's first
        // card, a card the filter hid until now, a tab docked in from another window) can be paid now that
        // the list is up to date. Deliberately ONLY an owed reveal — an update never re-scrolls on its own
        // (updates follow every fleet change, and would otherwise fight the user's own scrolling).
        _SidePanelQueueReveal();
    }

    // Bring the cards between the two spacers in step with the rows (SIDE_PANEL.md §2f). The rows are
    // _sidePanelRowIds / _sidePanelRowModels (the last list update's); only the rows of ONE range have cards:
    //   1. Measure: the row stride (a card + its vertical margins) is the MEDIAN of the realized cards' measured
    //      heights — one taller card (a title whose emoji came from a fallback font with a taller line box) cannot
    //      move it — and is kept unless it moved by more than layout-rounding noise. Before any card was measured,
    //      kSidePanelCardDefaultStride.
    //   2. Plan: around `anchorRow` (a ⌖ reveal of a row without a card: RowsAroundRow), else from the viewport
    //      (PlanRealizedRows: the current range while it covers the viewport's NEAR rows, otherwise the FAR rows —
    //      half a viewport of scrolling in hand before the next pass).
    //   3. Models: a realized card that stays gets the differences of its row's model when `modelsChanged` (a scroll
    //      pass leaves it alone — it already shows its row's model); a newly realized row gets a new card with its
    //      model written in full and, with "Sum" off, its rich tooltip armed.
    //   4. Order: the fewest removes / inserts / moves turn the cards between the spacers into the range's rows
    //      (PlanKeyedListOps on the mirror; child index = mirror index + 1, the top spacer being child 0, so neither
    //      spacer ever moves). A removed card's rich tooltip is shut down before its view goes (the islands leak
    //      class), and the view then goes with it — its elements, its DirectWrite state, its pulsing dots.
    //   5. Spacers: first * stride above, (count - last) * stride below — the list stays count * stride tall
    //      whichever rows are realized, so a scroll in flight (DirectManipulation inertia, a thumb drag) never sees its
    //      extent move.
    //   6. Verify, and queue one follow-up pass when cards were built: their heights are known only after the next
    //      layout pass, and the follow-up measures them (a first measurement corrects the default stride).
    // `modelsChanged`, `sessions`, `anchorRow`: see the declaration in TerminalPage.h. Returns false when the panel
    // ended out of step with its mirror (the caller resets the list). Throws winrt::hresult_error when XAML refuses an
    // edit, std::out_of_range / std::bad_alloc on a planner bug / allocation failure — every caller contains it.
    bool TerminalPage::_SidePanelSyncRealizedCards(bool modelsChanged, const std::vector<const ::Agentmaster::SessionInfo*>* sessions, size_t anchorRow)
    {
        if (!_sidePanelBuilt || !_sidePanelCards || !_sidePanelCardsTopSpacer || !_sidePanelCardsBottomSpacer)
        {
            return true;
        }
        // A spacer resize can move the scroll offset, and the ViewChanged that follows would land here again while
        // this pass runs: the pass in progress already plans from the final state, so the nested one is dropped.
        if (_sidePanelRealizing)
        {
            return true;
        }
        _sidePanelRealizing = true;
        const auto realizingDone = wil::scope_exit([this]() noexcept { _sidePanelRealizing = false; });

        auto children = _sidePanelCards.Children();
        // The panel and its mirror must agree before any edit: [top spacer][one card per mirror id][bottom spacer].
        // Every index the plan carries is computed on the mirror. They always do — the list is only edited here and
        // by _SidePanelResetCards — so a mismatch means an earlier edit failed halfway: start from empty.
        if (children.Size() != _sidePanelCardOrder.size() + 2 || _sidePanelCardViews.size() != _sidePanelCardOrder.size())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[sidepanel] card list out of step with its mirror (panel " + std::to_wstring(children.Size()) +
                                              L", mirror " + std::to_wstring(_sidePanelCardOrder.size()) + L", cards " +
                                              std::to_wstring(_sidePanelCardViews.size()) + L") - rebuilt from empty (window " +
                                              ::Agentmaster::ShortId(_windowId) + L")\n");
            _SidePanelResetCards();
            children = _sidePanelCards.Children();
            if (children.Size() != 2)
            {
                return false; // the reset could not empty the panel — nothing below may plan from it
            }
        }

        // 1. The stride. Each realized card that has been laid out (ActualHeight > 0) is one sample; a card built by
        //    the previous pass is measured here once the layout pass in between has run.
        std::vector<double> measured;
        measured.reserve(_sidePanelCardViews.size());
        for (const auto& [id, view] : _sidePanelCardViews)
        {
            if (view.card)
            {
                const double h = view.card.ActualHeight();
                if (h > 0.0)
                {
                    measured.push_back(h + 2.0 * kSidePanelCardMarginY);
                }
            }
        }
        const bool anyMeasured = !measured.empty();
        const double previousStride = _sidePanelCardStride > 0.0 ? _sidePanelCardStride : kSidePanelCardDefaultStride;
        const double measuredStride = ::Agentmaster::MedianRowHeight(std::move(measured), previousStride);
        const double stride = ::Agentmaster::StrideChanged(measuredStride, previousStride) ? measuredStride : previousStride;
        if (anyMeasured)
        {
            _sidePanelCardStride = stride; // 0 stays 0 until a card was really measured (the default is a guess)
        }

        // 2. The plan.
        const size_t count = _sidePanelRowIds.size();
        if (_sidePanelRowModels.size() != count)
        {
            return false; // belt: the rows are written as a parallel pair (one move each), so this never happens
        }
        const ::Agentmaster::RowRange current{ _sidePanelRealizedFirst, _sidePanelRealizedLast };
        double viewTop = 0.0;
        double viewHeight = 0.0; // 0 = no layout yet: the planner then assumes kVirtualRowsFallbackViewportDip
        if (_sidePanelCardsScroll)
        {
            viewTop = _sidePanelCardsScroll.VerticalOffset();
            viewHeight = _sidePanelCardsScroll.ViewportHeight();
        }
        const ::Agentmaster::RowRange target = (anchorRow < count) ? ::Agentmaster::RowsAroundRow(count, stride, anchorRow, viewHeight) :
                                                                     ::Agentmaster::PlanRealizedRows(count, stride, viewTop, viewHeight, current);
        const std::vector<std::wstring> targetIds(_sidePanelRowIds.begin() + static_cast<std::ptrdiff_t>(target.first),
                                                  _sidePanelRowIds.begin() + static_cast<std::ptrdiff_t>(target.last));

        // 3. The models. The new cards are built detached (the plan's Insert puts them in the list).
        bool built = false;
        for (size_t row = target.first; row < target.last; ++row)
        {
            const std::wstring& id = _sidePanelRowIds[row];
            if (const auto it = _sidePanelCardViews.find(id); it != _sidePanelCardViews.end())
            {
                if (modelsChanged)
                {
                    _SidePanelApplyCardModel(it->second, _sidePanelRowModels[row], id, /*fresh*/ false); // a copy: the row keeps its model
                }
                continue;
            }
            const auto created = _sidePanelCardViews.emplace(id, _SidePanelCreateCardView(id)).first;
            _SidePanelApplyCardModel(created->second, _sidePanelRowModels[row], id, /*fresh*/ true);
            if (_sidePanelSummaryHidden)
            {
                // "Sum" off: a NEW card gets its rich tooltip now (a header-only arm-build; the host attaches it on the
                // card's Loaded, before any pointer can reach it). Contained inside — a tooltip failure never resets
                // the list. The fresh apply above already built the tag strip in this mode, so no strip rebuild. The
                // list update hands its lean session row; a scroll has none (null: the arm-build reads GetWithoutQueue).
                const ::Agentmaster::SessionInfo* lean = (sessions && row < sessions->size()) ? (*sessions)[row] : nullptr;
                _SidePanelApplyCardTipMode(created->second, id, /*rebuildTags*/ false, lean);
            }
            built = true;
        }

        // 4. The order: the fewest removes / inserts / moves (KeyedListDiff.h), each made on the panel and on the
        //    mirror in step. A removed card's view is erased right after its RemoveAt — its handlers hold the page
        //    only weakly and reach the card through `sender`, so nothing keeps it alive after that.
        for (const auto& op : ::Agentmaster::PlanKeyedListOps(_sidePanelCardOrder, targetIds))
        {
            switch (op.kind)
            {
            case ::Agentmaster::KeyedListOpKind::Remove:
                children.RemoveAt(op.from + 1);
                if (const auto leaving = _sidePanelCardViews.find(op.key); leaving != _sidePanelCardViews.end())
                {
                    // Detach its rich tooltip BEFORE the view goes: the host's own Unloaded detach can run after the
                    // host is gone (the event is deferred), and a ToolTip left attached to a dropped element is the
                    // islands leak class (AgentTipHelpers.h). Never throws.
                    _SidePanelDropCardToolTip(leaving->second, op.key);
                    // Stop its status icon's motions for the same reason (SIDE_PANEL.md §2g): the icon's own Unloaded
                    // stop is deferred too, and by then the view — and with it the glyph — is gone, so a flashing card's
                    // ring would keep its hold on the UI thread's shared blink clock until the glyph's destructor gave
                    // it back. Never throws.
                    if (leaving->second.glyph)
                    {
                        leaving->second.glyph->Shutdown();
                    }
                }
                _sidePanelCardViews.erase(op.key);
                break;
            case ::Agentmaster::KeyedListOpKind::Insert:
                children.InsertAt(op.to + 1, _sidePanelCardViews.at(op.key).card);
                break;
            case ::Agentmaster::KeyedListOpKind::Move:
                children.Move(op.from + 1, op.to + 1); // a reorder of the panel's item array — no Unloaded/Loaded
                break;
            }
            if (!::Agentmaster::ApplyKeyedListOp(_sidePanelCardOrder, op))
            {
                return false; // an index out of range: the mirror no longer describes the panel
            }
        }

        // 5. The spacers, written only when their height moved (a same-value write still invalidates layout). A
        //    Height never set reads NaN, which the comparison treats as "moved".
        const auto spacers = ::Agentmaster::SpacersFor(count, stride, target);
        const auto setHeight = [](const Border& spacer, double height) {
            const double shown = spacer.Height();
            if (!(std::abs(shown - height) <= 0.01))
            {
                spacer.Height(height);
            }
        };
        setHeight(_sidePanelCardsTopSpacer, spacers.top);
        setHeight(_sidePanelCardsBottomSpacer, spacers.bottom);
        _sidePanelRealizedFirst = target.first;
        _sidePanelRealizedLast = target.last;

        // 6. Verify: a planned edit applied in order can't end anywhere else, so a difference here is a planner or
        //    mirror bug, or a panel that kept old children a reset could not Clear().
        if (_sidePanelCardOrder != targetIds || _sidePanelCardViews.size() != targetIds.size() || children.Size() != targetIds.size() + 2)
        {
            return false;
        }
        if (built)
        {
            _SidePanelQueueRealize(); // measure the new cards after the next layout pass (a first measurement fixes the stride)
        }
        return true;
    }

    // The card list's ScrollViewer moved. The plan is a few multiplications (VirtualListWindow.h), so it runs on every
    // ViewChanged — DirectManipulation raises one per intermediate offset while a scroll runs — and a pass (XAML
    // edits) only when the realized rows no longer cover the viewport's NEAR rows: about every half viewport of
    // scrolling. Handled synchronously (not queued): ViewChanged is raised after the offset changed, never inside a
    // layout pass, and a queued pass would leave a frame of blank spacer on screen during a fast wheel scroll.
    // No parameters; returns nothing. Never throws: _SidePanelRunRealizePass contains every failure.
    void TerminalPage::_SidePanelOnCardsViewChanged()
    {
        if (!_sidePanelBuilt || _sidePanelCollapsed || _sidePanelRealizing || !_sidePanelCardsScroll)
        {
            return; // collapsed: no viewport to follow (the expand's SizeChanged re-checks); realizing: see the pass
        }
        try
        {
            const double stride = _sidePanelCardStride > 0.0 ? _sidePanelCardStride : kSidePanelCardDefaultStride;
            const ::Agentmaster::RowRange current{ _sidePanelRealizedFirst, _sidePanelRealizedLast };
            const auto target = ::Agentmaster::PlanRealizedRows(_sidePanelRowIds.size(), stride, _sidePanelCardsScroll.VerticalOffset(), _sidePanelCardsScroll.ViewportHeight(), current);
            if (target == current)
            {
                return; // the realized rows still cover the viewport (the common case: no XAML is touched)
            }
        }
        catch (...)
        {
            // Reading the offset of a ScrollViewer being torn down: nothing to realize for.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelOnCardsViewChanged");
            return;
        }
        _SidePanelRunRealizePass(L"scroll", kSidePanelNoAnchorRow);
    }

    // Coalesce a realized-range re-check onto ONE Low-priority tick (see the declaration): LOW so it runs after the
    // layout pass that sized the ScrollViewer or measured the cards a pass just built — the reveal tick's ordering.
    // No parameters; returns nothing. Never throws: a dispatcher that is gone (window teardown) is logged and the
    // coalescer re-armed, so the latch can never stick shut.
    void TerminalPage::_SidePanelQueueRealize()
    {
        if (!_sidePanelBuilt || _sidePanelCollapsed || _sidePanelRealizeQueued)
        {
            return;
        }
        _sidePanelRealizeQueued = true;
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis = get_weak()]() {
                if (const auto self = weakThis.get())
                {
                    self->_sidePanelRealizeQueued = false; // re-arm BEFORE the pass, so no outcome of it can leave the latch shut
                    if (self->_sidePanelBuilt && !self->_sidePanelCollapsed)
                    {
                        self->_SidePanelRunRealizePass(L"measure", kSidePanelNoAnchorRow); // never throws
                    }
                }
            });
        }
        catch (...)
        {
            _sidePanelRealizeQueued = false;
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelQueueRealize");
        }
    }

    // One realize pass with every failure contained (see the declaration): out of step or a throw -> logged, the list
    // reset and marked dirty, so the next periodic tick (~2.5 s) rebuilds it from empty.
    bool TerminalPage::_SidePanelRunRealizePass(std::wstring_view context, size_t anchorRow)
    {
        try
        {
            if (_SidePanelSyncRealizedCards(/*modelsChanged*/ false, nullptr, anchorRow))
            {
                return true;
            }
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] card list " + std::wstring{ context } + L" pass ended out of step with its plan - rebuilding from empty (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelRunRealizePass");
        }
        _SidePanelResetCards();
        _sidePanelDirty = true;
        return false;
    }

    // Drop every card: the panel's children (the two spacers come back at 0 height), the id -> card map, the order
    // mirror, the realized range and the remembered count text. The recovery path of the list update and of every
    // realize pass — when the panel and its mirror disagree, or an edit failed halfway — after which the next update
    // rebuilds the list from empty (the planner then fills it front to back). The rows, the bookmark-tag caches and
    // the measured stride are DATA, not UI, and stay.
    // No parameters; returns nothing. Never throws: a panel that refuses Clear() is logged (Rule #18) and left
    // non-empty, which the next pass sees as out of step with the (now empty) mirror — and calls this again.
    void TerminalPage::_SidePanelResetCards()
    {
        // Every card's rich tooltip is detached first (the "Sum" off mode attaches one per card): a ToolTip left
        // attached to a dropped element is the islands leak class. _SidePanelDropCardToolTip never throws. And every
        // card's status icon stops its motions (SIDE_PANEL.md §2g) — a flashing card's ring gives back its hold on the
        // shared blink clock now, not whenever its glyph is destroyed. Shutdown never throws.
        for (auto& [id, view] : _sidePanelCardViews)
        {
            _SidePanelDropCardToolTip(view, id);
            if (view.glyph)
            {
                view.glyph->Shutdown();
            }
        }
        // The maps first: clearing them cannot fail, so the page's view of the list is consistently "empty"
        // even when the panel below refuses its Clear.
        _sidePanelCardViews.clear();
        _sidePanelCardOrder.clear();
        _sidePanelRealizedFirst = 0;
        _sidePanelRealizedLast = 0;
        _sidePanelCountValue.clear(); // the next update writes the count line again
        _sidePanelFilterRowSig.clear(); // ...and redraws the chips row (SIDE_PANEL.md §2l), whatever it showed before the reset
        if (!_sidePanelCards)
        {
            return;
        }
        try
        {
            auto children = _sidePanelCards.Children();
            children.Clear();
            // An empty list is [top spacer][bottom spacer]: the shape every pass expects (child 0 and the last child).
            if (_sidePanelCardsTopSpacer && _sidePanelCardsBottomSpacer)
            {
                _sidePanelCardsTopSpacer.Height(0.0);
                _sidePanelCardsBottomSpacer.Height(0.0);
                children.Append(_sidePanelCardsTopSpacer);
                children.Append(_sidePanelCardsBottomSpacer);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelResetCards");
        }
    }

    // Build ONE card's skeleton — the Triage-Board card's visual language re-hosted for a narrow vertical
    // list: a title on a band, then a state line (a row with the STATUS ICON + label in the shared AgentStatusColors
    // palette — the icon is the tab strip's own, with its 3 dots, flash ring and spark crown, SIDE_PANEL.md §2g —
    // followed by the optional part the model adds — the teal `codex` pill — and, in the line's star column, the
    // "created 4d1h, active 2m4s ago" timing text), then a dim
    // dir/branch line. Every card has these elements; their VALUES — texts, brushes,
    // the band's tag strip, the optional parts, which of the icon's indicators show — come from
    // _SidePanelApplyCardModel, so a card is built once per session and from then on only edited.
    // The card's status icon must be shut down before the card leaves the list (_SidePanelSyncRealizedCards' Remove,
    // _SidePanelResetCards): it stops its motions and gives back the flash ring's hold on the shared blink clock.
    // Interactions: click = select (Linked Lenses), double-click = activate (jump to the tab, cross-window),
    // right-click = the SHARED lazy session menu (AgentManagerContent::BuildSessionMenuFor — zero drift from
    // the board's), anchored on this card so its Tags item opens the tag panel right under it.
    // `id`: the session the card belongs to, for its whole life (a card is never re-keyed: a session that
    // leaves the list loses its card, and a later one gets a new card).
    // Returns the view, built and wired but showing nothing yet: the caller applies the first model with
    // fresh == true, then inserts view.card into the list.
    // Throws winrt::hresult_error if XAML construction fails (off the UI thread / a torn-down tree) —
    // _SidePanelRebuildCards calls it on the UI thread and resets the list on a throw.
    AgentSidePanelCardView TerminalPage::_SidePanelCreateCardView(const std::wstring& id)
    {
        // The elements every side panel card has (since SIDE_PANEL.md §2h one builder for the session cards and the AI
        // search's result cards, so the two can never drift apart); this card's own part is its handlers, below.
        AgentSidePanelCardView v = _SidePanelBuildCardSkeleton(id, L"am.side.card.");
        // A collapsed panel holds still from the start, and so does a card list the AI view covers (SIDE_PANEL.md §2h): the
        // host gate (_SidePanelToggleCollapsed and _SidePanelSetAiMode flip it for the realized cards).
        v.glyph->SetHostShown(!_SidePanelCardsHidden());
        _SidePanelWireCardHandlers(v, id);
        return v;
    }

    // The skeleton of a side panel card (see the declaration in TerminalPage.h): the elements, no handler. Its body is the
    // session card's original build, moved here unchanged except for the title's AutomationId prefix (a parameter: the result
    // cards use their own) and the glyph's host gate (each caller decides it).
    AgentSidePanelCardView TerminalPage::_SidePanelBuildCardSkeleton(const std::wstring& id, std::wstring_view automationIdPrefix)
    {
        AgentSidePanelCardView v;
        // The card is a plain Grid with TWO layers in its one cell: the rounded background, then the content. The
        // rounded shapes stay CHILDLESS (UI_THREAD.md §10h / §14): XAML gives an element with a non-zero CornerRadius
        // AND children its own composition node (CFrameworkElement::UpdateRequiresCompNodeForRoundedCorners, so it can
        // clip the children to the curve), and every frame of the window walks every node — the old card (a rounded
        // Border holding the body) and its band (a rounded Border holding the title) were 2 nodes per card, 81% of
        // production's 3,634 on 2026-10-20. A childless rounded Border draws the same shape with no node. Nothing in
        // the card needs the clip: the band rounds its own top corners, and no content reaches a corner.
        v.card = Grid{};
        v.card.Margin(ThicknessHelper::FromLengths(kSidePanelCardMarginX, kSidePanelCardMarginY, kSidePanelCardMarginX, kSidePanelCardMarginY));
        v.cardBack = Border{};
        // The body color is ALSO what the status icon's contrast picks judge (kSidePanelCardBodyColor), and the fill is
        // what makes the whole card hit-testable (the Grid has no background — and the status icon is not hit-testable,
        // so a press on the dot lands here and selects the card like any other press on it).
        v.cardBack.Background(SolidColorBrush{ kSidePanelCardBodyColor });
        v.cardBack.CornerRadius(CornerRadiusHelper::FromUniformRadius(kSidePanelCardCornerRadius));
        v.cardBack.BorderThickness(ThicknessHelper::FromUniformLength(kSidePanelCardBorderPx)); // constant width — hover/selection recolor, never reflow
        v.card.Children().Append(v.cardBack); // first: drawn behind the content

        StackPanel body{};
        body.Orientation(Orientation::Vertical);
        // Inside the background's border, where the old Border laid out its child — so nothing moves by a pixel.
        body.Margin(ThicknessHelper::FromUniformLength(kSidePanelCardBorderPx));

        // Title band: its color and the title's ink come from the model (the session's resolved color, the
        // same visual as the board card's band, so same-dir cards cluster by color here too). The same two layers
        // as the card: the band area is a plain Grid, the band a CHILDLESS rounded Border behind the title.
        v.bandArea = Grid{};
        v.band = Border{};
        v.band.CornerRadius(CornerRadiusHelper::FromRadii(kSidePanelBandCornerRadius, kSidePanelBandCornerRadius, 0, 0));
        v.bandArea.Children().Append(v.band); // first: drawn behind the title
        v.title = TextBlock{};
        v.title.FontSize(12);
        v.title.FontWeight(FontWeights::SemiBold());
        v.title.TextWrapping(TextWrapping::NoWrap);
        v.title.TextTrimming(TextTrimming::CharacterEllipsis);
        // The band's padding is the margin of what the band shows (the band itself has no child to pad).
        v.title.Margin(ThicknessHelper::FromLengths(kSidePanelBandPadX, kSidePanelBandPadY, kSidePanelBandPadX, kSidePanelBandPadY));
        v.bandArea.Children().Append(v.title); // the untagged layout; _SidePanelApplyCardTags puts the title in [ title * ][ strip ] for a tagged session
        // Agentmaster (accessibility, E2E_TESTING.md §6.3): a card is a Grid of Borders — no automation peer — so its
        // TITLE carries the card's id (am.side.card.<key>) and its item status (_SidePanelApplyCardAutomation). A test
        // finds the card by the id and clicks the title's rectangle; a screen reader reads the title, then the state.
        AgentSetAutomationId(v.title, AgentAutomationIdFor(automationIdPrefix, id));
        body.Children().Append(v.bandArea);

        // State line: [ state row Auto ][ timing * ]. The row holds the status icon and the label, and the model inserts
        // the codex pill after them; the timing text goes in the star column (added by the model too),
        // so on a narrow panel it is the part that gives way, ending in "…" (Text's CharacterEllipsis) — in the row's
        // StackPanel it was offered unlimited width and got cut off mid-letter at the card's edge instead.
        v.stateLine = Grid{};
        v.stateLine.Margin(ThicknessHelper::FromLengths(6, kSidePanelStateLineTopPx, 6, 0));
        {
            ColumnDefinition rowCol{};
            rowCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition timingCol{};
            timingCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            v.stateLine.ColumnDefinitions().Append(rowCol);
            v.stateLine.ColumnDefinitions().Append(timingCol);
        }
        v.stateRow = StackPanel{};
        v.stateRow.Orientation(Orientation::Horizontal);
        v.stateRow.Spacing(5);
        // The STATUS ICON (SIDE_PANEL.md §2g): the tab strip's own icon — the 10 DIP dot with its black stroke, and around
        // it the tab's three indicators (the unsent-draft 3 dots under it, the unread flash ring behind it, the cache-warm
        // glow + rising embers) with the tab's composition motions. It replaces the plain 8 DIP dot and the separate draft
        // dots cluster this row held before. Top-aligned: the row is the icon's 22 DIP tall, and the label / pill / timing
        // center on the DOT through their kSidePanelStatusIconBandPx bottom margin. The horizontal overhang keeps the dot
        // where the old one sat (see kSidePanelStatusIconOverhangPx). Whether its host shows (a collapsed panel, or a list the
        // AI view covers, holds still from the start) is the CALLER's call: a session card and a result card hide with
        // different things (_SidePanelCreateCardView, _SidePanelAiCreateResultView).
        v.glyph = AgentStatusGlyph::Create();
        {
            const auto icon = v.glyph->Root();
            icon.VerticalAlignment(VerticalAlignment::Top);
            icon.Margin(ThicknessHelper::FromLengths(-kSidePanelStatusIconOverhangPx, 0, -kSidePanelStatusIconOverhangPx, 0));
            v.stateRow.Children().Append(icon);
        }
        v.stateText = Text(L"", 10, false, 0.9);
        v.stateText.Margin(ThicknessHelper::FromLengths(0, 0, 0, kSidePanelStatusIconBandPx)); // centered on the dot, not the whole icon
        v.stateRow.Children().Append(v.stateText);
        Grid::SetColumn(v.stateRow, 0);
        v.stateLine.Children().Append(v.stateRow);
        body.Children().Append(v.stateLine);

        // Dir/branch line: "<leaf>[ / <branch>]", dim — the card's place at a glance (the full path rides the tooltip).
        v.dirLine = Text(L"", 10, false, 0.55);
        v.dirLine.Margin(ThicknessHelper::FromLengths(6, 0, 6, 4));
        body.Children().Append(v.dirLine);

        v.card.Children().Append(body); // second: over the background
        return v;
    }

    // Wire a SESSION card's gestures on its skeleton — the hover ring, click = select (Linked Lenses), double-click = activate,
    // right-click = the shared lazy session menu — every one resolving the card through _sidePanelCardViews at event time.
    // (The AI search's result cards wire their own, against their own map: _SidePanelAiCreateResultView.) The handlers'
    // bodies are the session card's original ones, moved here unchanged.
    //   v  - the card (its root Grid owns the handlers).
    //   id - the card's session, captured by value.
    // Throws winrt::hresult_error if XAML refuses a handler registration (a torn-down tree) — the list update's catch resets
    // the list.
    void TerminalPage::_SidePanelWireCardHandlers(AgentSidePanelCardView& v, const std::wstring& id)
    {
        const auto weakThis = get_weak();
        // Hover ring — guarded by PointerStillWithin so a child-bubbled exit (crossing between the card's
        // hit-testable labels) can't strobe the ring off while the pointer is still inside. The ring takes the
        // color of the state the card SHOWS (its view's model — what its dot shows, with no registry copy per
        // pointer event), and the view remembers the hover so a state change repaints the ring in the new color.
        // ⚠ The handlers reach the card ONLY through `sender` — capturing the card itself in a handler
        // attached TO it is a self-reference cycle (element -> handler -> element) that would leak every card
        // ever dropped from the list (the board's cards capture weakly for the same reason). A sender that is
        // not the card registered for `id` (a card already dropped — its row scrolled out of the realized range —
        // or replaced by a reset) is ignored. The ring is drawn by the card's background layer (cardBack), reached
        // through the view, never captured.
        v.card.PointerEntered([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs&) {
            const auto self = weakThis.get();
            const auto g = sender.try_as<Grid>();
            if (!self || !g)
            {
                return;
            }
            const auto it = self->_sidePanelCardViews.find(id);
            if (it == self->_sidePanelCardViews.end() || it->second.card != g)
            {
                return; // not the live card of this session any more — nothing of ours to paint
            }
            it->second.hovered = true;
            SidePanelStyleCardBorder(it->second.cardBack, StateColor(it->second.model.state), id == self->_sidePanelSelectedId, /*hover*/ true);
        });
        v.card.PointerExited([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            const auto self = weakThis.get();
            const auto g = sender.try_as<Grid>();
            if (!self || !g || PointerStillWithin(sender, e))
            {
                return; // bubbled from a child — the pointer never actually left the card
            }
            const auto it = self->_sidePanelCardViews.find(id);
            if (it == self->_sidePanelCardViews.end() || it->second.card != g)
            {
                return; // not the live card of this session any more
            }
            it->second.hovered = false;
            SidePanelStyleCardBorder(it->second.cardBack, StateColor(it->second.model.state), id == self->_sidePanelSelectedId, /*hover*/ false);
        });
        v.card.Tapped([weakThis, id](const IInspectable&, const TappedRoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelSelectSession(id, /*driveManagerLens*/ true);
                // Agentmaster (tab-strip reveal): while the Manager page is up, a card click scrolls the
                // session's tab into view like a board card click does — including a RE-click of the
                // already-selected card, which _SidePanelSelectSession's same-id early-out and the lens's
                // change-only reveal both swallow. Off the Manager page it does nothing (the click is a PEEK —
                // SIDE_PANEL.md §3); the gate lives in _RevealClickedSessionTab.
                self->_RevealClickedSessionTab(id);
            }
        });
        v.card.DoubleTapped([weakThis, id](const IInspectable&, const DoubleTappedRoutedEventArgs& e) {
            e.Handled(true);
            if (const auto self = weakThis.get())
            {
                self->_ActivateClaudeSession(winrt::hstring{ id }); // jump to the tab — cross-window via the activate sinks
            }
        });
        v.card.RightTapped([weakThis, id](const IInspectable& sender, const RightTappedRoutedEventArgs& e) {
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
                        // Anchored on THIS card (the sender — the handler never captures the card
                        // itself): the menu's "Tags" item then opens the tag panel right under the card
                        // instead of at the window's top-left default. The menu holds it weakly.
                        // The card's part of the shared menu (AgentManagerContent::SessionMenuHost). The page is held weakly
                        // in every function: the menu keeps them for its life.
                        implementation::AgentManagerContent::SessionMenuHost host;
                        // Under Jump to Tab, the card's own items (SIDE_PANEL.md §2j): Pin to Top, and Unpin for a pinned
                        // card — filled at every open from the session as it is then.
                        host.afterJumpToTab = [weakThis, id](MenuFlyout& m) {
                            if (const auto page = weakThis.get())
                            {
                                page->_SidePanelAppendPinItems(m, id, /*fromAiView*/ false);
                            }
                        };
                        // SIDE_PANEL.md §2k: "Open New Session Here" and "Fork session" open the new tab right next to THIS
                        // card's tab, in the window that hosts it — not at the end of this window's strip.
                        host.openNewSessionHere = [weakThis, id](winrt::hstring model) {
                            if (const auto page = weakThis.get())
                            {
                                page->_SidePanelLaunchNextToTab(id, /*fork*/ false, std::wstring{ model });
                            }
                        };
                        host.forkSession = [weakThis, id](winrt::hstring model) {
                            if (const auto page = weakThis.get())
                            {
                                page->_SidePanelLaunchNextToTab(id, /*fork*/ true, std::wstring{ model });
                            }
                        };
                        auto menu = mgr->BuildSessionMenuFor(id, cwd, sender.try_as<FrameworkElement>(), std::move(host));
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
    }

    // The card menu's PIN TO TOP items (SIDE_PANEL.md §2j; see the declaration in TerminalPage.h). The user's words: "pinning
    // can only happen through right click. put pin below Jump to tab" — so these two items are the only way in and out, and the
    // band's pushpin is no button. Labels: "Pin to Top" on every live card — on a pinned one it is the user's "repinning will
    // bring a tab to the top again" — and "Unpin" under it on a pinned card only.
    void TerminalPage::_SidePanelAppendPinItems(MenuFlyout& menu, const std::wstring& id, bool fromAiView)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        // The session as it is NOW (the menu is being filled at its open): a lean read — the queue, which no item needs, is the
        // costliest part of a record copy.
        const auto lean = _sessionRegistry->GetWithoutQueue(id);
        if (!lean || !lean->live)
        {
            return; // unknown, or closed since its card was drawn: no list card for a pin to order
        }
        const bool pinned = ::Agentmaster::IsSidePanelPinStamp(lean->sidePanelPinnedAtUnixMs);
        const bool revealAfter = !fromAiView; // behind the AI view the cards list is hidden: no scroll owed until it closes
        // An item's icon: the band's own pushpin glyphs, from the same font list as the band (a FontIcon per item — a menu
        // element cannot be shared between two items).
        const auto pinIcon = [](const wchar_t* glyph) {
            FontIcon icon{};
            icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ kSidePanelPinFont });
            icon.Glyph(glyph);
            return icon;
        };
        // Every click runs one tick later through the ⋯ menu's command helper (_SidePanelRunMenuCommand): inside the Click the
        // menu is still closing and hands focus back as it closes — the session menu defers every item for the same reason.
        // The handlers capture the page weakly (inside the helper) and the item never: no element -> handler -> element cycle.
        const auto weakThis = get_weak();

        MenuFlyoutItem pinItem;
        pinItem.Text(L"Pin to Top");
        pinItem.Icon(pinIcon(kSidePanelPinGlyph));
        AgentSetTip(pinItem, winrt::hstring{ pinned ? L"Bring this pinned card back to the top of the side panel's list, above every other pinned card." :
                                                      L"Keep this card at the top of the side panel's list: pinned cards come first, the last one pinned on top, then the rest in the chosen order. A pushpin marks it; Pin to Top again brings it back to the top, Unpin lets it go.\nThe pin is this session's in every window, and it is kept after a restart. Nothing else changes: the session runs and idles as before." });
        AgentSetAutomationId(pinItem, L"am.side.pin");
        pinItem.Click([weakThis, id, revealAfter](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelRunMenuCommand(L"card menu: Pin to Top", [id, revealAfter](TerminalPage& page) { page._SidePanelSetPinned(id, /*pin*/ true, revealAfter); });
            }
        });
        menu.Items().Append(pinItem);

        if (pinned)
        {
            MenuFlyoutItem unpinItem;
            unpinItem.Text(L"Unpin");
            unpinItem.Icon(pinIcon(kSidePanelUnpinGlyph));
            AgentSetTip(unpinItem, L"Let this card go back to its place in the chosen order, and take its pushpin away.");
            AgentSetAutomationId(unpinItem, L"am.side.unpin");
            unpinItem.Click([weakThis, id](const IInspectable&, const RoutedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelRunMenuCommand(L"card menu: Unpin", [id](TerminalPage& page) { page._SidePanelSetPinned(id, /*pin*/ false, /*revealAfter*/ false); });
                }
            });
            menu.Items().Append(unpinItem);
        }
    }

    // Pin, pin again or unpin a session's card (SIDE_PANEL.md §2j; see the declaration in TerminalPage.h).
    void TerminalPage::_SidePanelSetPinned(const std::wstring& id, bool pin, bool revealAfter) noexcept
    {
        try
        {
            if (!_sidePanelBuilt || !_sessionRegistry || id.empty())
            {
                return;
            }
            // The stamp: the clock, but past the newest pin of EVERY live session — not only this list's: a GLOBAL list in any
            // window orders its pins by these same stamps, and the card must be the top one there too (NextSidePanelPinStamp).
            // One lean snapshot per click (no queues): a user action, never a per-update cost.
            int64_t stamp = 0;
            if (pin)
            {
                int64_t newest = 0;
                for (const auto& s : _sessionRegistry->SnapshotLiveForCards())
                {
                    newest = (std::max)(newest, s.sidePanelPinnedAtUnixMs);
                }
                stamp = ::Agentmaster::NextSidePanelPinStamp(NowMs(), newest);
            }
            // Through Update, so every observer hears it: each window's panel re-sorts from its registry drain, and the
            // persistence observer saves sessions.json. A session closed since the menu opened is left as it is (its card is
            // gone; a pin would only resurface when it is resumed).
            bool live = false;
            const bool known = _sessionRegistry->Update(id, [pin, stamp, &live](::Agentmaster::SessionInfo& s) {
                live = s.live;
                if (live)
                {
                    s.sidePanelPinnedAtUnixMs = pin ? stamp : 0;
                }
            });
            if (!known || !live)
            {
                return;
            }
            ::Agentmaster::LogNav(std::wstring{ pin ? L"sidepanel pin-to-top " : L"sidepanel unpin " } + ::Agentmaster::ShortId(id));
            // A pin owes the list a scroll to the card's new place, the top: paid by the reveal tick once the card is listed
            // there, whatever the ⌖ setting says (_sidePanelRevealForced) — the user asked for the card to move and should
            // see where it went. Recorded BEFORE the update below, whose end queues the owed reveal (_SidePanelQueueReveal).
            // An unpin owes none: the card drops back into the order, and the user is looking at the pins.
            if (pin && revealAfter)
            {
                _sidePanelRevealPendingId = id;
                _sidePanelRevealForced = true;
            }
            // This window's list at once — the drain would come back through _SidePanelScheduleRebuild's 500 ms throttle, and
            // the card should move while the user watches — the way a tag edit does (_SidePanelNoteTagsChanged): a collapsed
            // list only owes its update (the expand pays it), and behind the AI view the hidden list owes its update while the
            // result cards, which can show this session, take the pushpin now.
            if (_sidePanelCollapsed)
            {
                _sidePanelDirty = true;
            }
            else if (_sidePanelAiMode)
            {
                _sidePanelDirty = true;
                _SidePanelAiRefreshResults();
            }
            else
            {
                _SidePanelRebuildCards();
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the list catches up on the periodic tick (the registry write, when it landed, stands).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelSetPinned");
            _sidePanelDirty = true;
        }
    }

    // "Open New Session Here" / "Fork session" next to the card's tab, in that tab's window (SIDE_PANEL.md §2k; see the
    // declaration in TerminalPage.h).
    void TerminalPage::_SidePanelLaunchNextToTab(const std::wstring& sessionId, const bool fork, const std::wstring& model) noexcept
    {
        try
        {
            if (sessionId.empty())
            {
                return;
            }
            const std::wstring what = std::wstring{ fork ? L"fork" : L"new-session" } + L" next to " + ::Agentmaster::ShortId(sessionId) + (model.empty() ? std::wstring{} : (L" model=" + model));
            ::Agentmaster::LogNav(L"sidepanel " + what);
            if (_SidePanelLaunchNextToTabHere(sessionId, fork, model, /*fromOtherWindow*/ false))
            {
                return; // this window hosts the card's tab: the new tab opened right after it, here
            }
            // Not hosted here. Does ANY window host a tab of it? A live MANAGED session always has one: a launch makes it, a Close
            // archives the record (live = false), and a closing window archives its sessions — so it is another window's. A live
            // ADOPTED session (external) has a tab only once a window bound it to that window's own ConPTY, which gave it a stdin
            // injector; one that no window bound (a claude running outside Agentmaster) has none, and no window would answer.
            const auto info = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(sessionId) : std::nullopt;
            const bool tabSomewhere = info && info->live && (!info->external || _sessionRegistry->HasInjector(sessionId));
            if (tabSomewhere)
            {
                // Hand it to the window that hosts the tab: every window gets it on its own UI thread and only the host acts
                // (_RemoteSessionAction -> _SidePanelLaunchNextToTabHere). Fire-and-forget, like the session menu's Deactivate ▸ /
                // Close ▸: a tab that left every window meanwhile (closed, or in the middle of a tear-out) opens nothing.
                ::Agentmaster::LogNav(L"sidepanel " + what + L" -> handed to the window that hosts its tab");
                ::Agentmaster::RequestSessionActionInWindows(sessionId, fork ? kSidePanelForkNextAction : kSidePanelNewSessionNextAction, model, {});
                return;
            }
            // No tab anywhere to sit next to: the menu's old behavior — THIS window, at the end of the strip.
            ::Agentmaster::LogNav(L"sidepanel " + what + L" -> no window hosts a tab of it: opened in this window, at the end of the strip");
            _SidePanelOpenNewOrFork(sessionId, fork, model, static_cast<uint32_t>(-1));
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing opened (or what opened before the throw stays); the user can pick the item again.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelLaunchNextToTab");
        }
    }

    bool TerminalPage::_SidePanelLaunchNextToTabHere(const std::wstring& sessionId, const bool fork, const std::wstring& model, const bool fromOtherWindow)
    {
        const auto it = _claudeTabs.find(sessionId);
        const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr;
        if (!tab)
        {
            return false; // not hosted here (another window's tab, or no tab anywhere): the caller hands it on, a sink just misses
        }
        const std::wstring what = std::wstring{ fork ? L"fork" : L"new-session" } + L" next to " + ::Agentmaster::ShortId(sessionId);
        // The slot is read NOW, from the strip as it is (it may have changed since the menu opened): right after the tab.
        uint32_t index = 0;
        if (!_tabs.IndexOf(tab, index))
        {
            // Mapped, but not in this window's strip this very moment (being torn out or moved): there is no "next to it" here,
            // and the window it is moving to does not know it yet.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] " + what + L": its tab is not in this window's strip right now (nothing opened)\n");
            return true;
        }
        if (fromOtherWindow)
        {
            ::Agentmaster::LogNav(L"sidepanel-remote " + what + L" (asked from another window's side panel card)");
            // The new tab — and a fork's interrupted-turn question, when one is asked — shows in THIS window, which can sit behind
            // the one the user clicked in: bring it to the front first, with the restore / foreground / SwitchToThisWindow recipe
            // of the session menu's cross-window Close (_RunSessionMenuScopeHere). The process owns the foreground (the click
            // happened in its other window), so SetForegroundWindow may hand it over.
            if (_hostingHwnd)
            {
                const HWND hwnd = *_hostingHwnd;
                if (::IsIconic(hwnd))
                {
                    ::ShowWindow(hwnd, SW_RESTORE);
                }
                ::SetForegroundWindow(hwnd);
                if (::GetForegroundWindow() != hwnd)
                {
                    ::SwitchToThisWindow(hwnd, TRUE); // belt-and-braces when the shell denies the hand-off
                }
            }
        }
        _SidePanelOpenNewOrFork(sessionId, fork, model, index + 1);
        return true;
    }

    void TerminalPage::_SidePanelOpenNewOrFork(const std::wstring& sessionId, const bool fork, const std::wstring& model, const uint32_t insertPosition)
    {
        if (fork)
        {
            // The content's own fork (its _forkManagedSessionHandler: kind-aware, Claude --fork-session / `codex fork`), with the
            // slot: the interrupted-turn question first when the source is in the middle of a turn (HOOKS.md "Interrupted
            // turns"), then the fork through _LaunchFromManagerPage (fromManagerPage — decided when the fork RUNS, after the
            // question: behind the page while it is on screen, in front over a tab). The slot is the one read now, as the WT tab
            // menu's own fork does.
            ForkLaunchRequest request;
            request.sourceId = sessionId;
            request.managed = true;
            request.model = model;
            request.insertPosition = insertPosition;
            request.origin = L"side-panel";
            request.fromManagerPage = true;
            _ForkAskingAboutRunningTurn(std::move(request));
            return;
        }
        // "Open New Session Here": the card session's EFFECTIVE working directory — the folder the menu's "Here" names (the
        // content's _WorkDirOf) — and its kind, read now. An empty directory goes through as it did (the launch seam falls back to
        // its default folder), so a record without one behaves as before.
        std::wstring dir;
        bool codex = false;
        if (_sessionRegistry)
        {
            if (const auto s = _sessionRegistry->GetWithoutQueue(sessionId))
            {
                dir = ::Agentmaster::EffectiveWorkingDir(_appSettings.tabColorMode, *s);
                codex = (s->kind == ::Agentmaster::AgentKind::Codex);
            }
        }
        if (codex)
        {
            // A Codex card spawns codex, which takes no --model (the Codex item sends ""), as the content's Codex launch does.
            _LaunchFromManagerPage(L"side panel new codex session", [&]() {
                _SpawnCodexSession(winrt::hstring{ dir }, winrt::hstring{}, insertPosition);
                return std::wstring{}; // the seam returns no id: the launch records it (_NoteLaunchedSessionTab)
            });
            return;
        }
        _LaunchFromManagerPage(L"side panel new session", [&]() {
            return _SpawnClaudeSession(winrt::hstring{ dir }, winrt::hstring{}, insertPosition, winrt::hstring{ model });
        });
    }

    // Write `model` into a card IN PLACE: every value that differs from the one the card shows now (its
    // view.model) is written — a text, a brush, the border — and an optional part (the codex pill, the timing
    // text, the band's tag strip) is added or dropped only when its presence changes. The status icon's three
    // indicators (the 3 dots, the flash ring, the spark crown — SIDE_PANEL.md §2g) are shown or hidden through
    // its setters, which also start or stop their motions. A value that did not change is never touched, so an
    // update that changed nothing for this card costs it nothing. Also the in-place indicator patch's writer
    // (_SidePanelRefreshIndicators), which hands a row model that differs from the card's only in the patched fields.
    // `view`: the card (its elements + the model they show).
    // `model`: what it must show; moved into view.model at the end.
    // `id`: the card's session — the selection check of its border.
    // `fresh`: true for a card _SidePanelCreateCardView just built — write EVERY value (its model is still the
    //          empty default); false — write only the differences.
    // Throws winrt::hresult_error if XAML refuses an edit (a torn-down tree) — _SidePanelRebuildCards resets
    // the list on a throw, so a half-edited card never outlives its update.
    void TerminalPage::_SidePanelApplyCardModel(AgentSidePanelCardView& view, AgentSidePanelCardModel model, const std::wstring& id, bool fresh)
    {
        const AgentSidePanelCardModel& shown = view.model; // the card as it is now (ignored when fresh)
        if (fresh || model.title != shown.title)
        {
            view.title.Text(winrt::hstring{ model.title });
        }
        if (fresh || model.bandHex != shown.bandHex)
        {
            const auto bandColor = HexToColor(model.bandHex);
            view.band.Background(SolidColorBrush{ bandColor.value_or(Color{ 0xFF, 0x3A, 0x3A, 0x3A }) });
        }
        if (fresh || model.darkInk != shown.darkInk)
        {
            view.title.Foreground(model.darkInk ? Fill(0xFF, 0x10, 0x10, 0x10) : Fill(0xFF, 0xF0, 0xF0, 0xF0));
        }
        // The tag strip shows the tags, their colors and — in its "+N" count — the title's ink.
        if (fresh || model.tags != shown.tags || model.tagArgb != shown.tagArgb || (!model.tags.empty() && model.darkInk != shown.darkInk))
        {
            _SidePanelApplyCardTags(view, model, id);
        }
        // The PIN TO TOP pushpin (SIDE_PANEL.md §2j), at the band's right end in the title's ink. After the tags on purpose: an
        // untagging that leaves a pinned card keeps its band grid (the pushpin still needs it), and an unpinning that leaves no
        // strip gives the band back its plain layout — each half releases the grid only when the other has nothing there.
        if (fresh || model.pinned != shown.pinned || (model.pinned && model.darkInk != shown.darkInk))
        {
            _SidePanelApplyCardPin(view, model);
        }
        if (fresh || model.state != shown.state)
        {
            const Color stateColor = StateColor(model.state);
            view.glyph->SetStateColor(stateColor);
            view.stateText.Text(StateLabel(model.state));
            view.stateText.Foreground(SolidColorBrush{ stateColor });
            SidePanelStyleCardBorder(view.cardBack, stateColor, id == _sidePanelSelectedId, view.hovered);
        }
        // The state row's optional part, after the status icon and the label: [codex pill]. The timing text is the state
        // LINE's star column (after the row), not a row part.
        if (model.codex && !view.codexPill)
        {
            view.codexPill = Text(L"codex", 9, true, 0.9);
            view.codexPill.Foreground(Fill(0xFF, 0x2E, 0xB8, 0xA8)); // the External/Codex teal pill color family
            view.codexPill.Margin(ThicknessHelper::FromLengths(0, 0, 0, kSidePanelStatusIconBandPx)); // centered on the dot, like the label
            view.stateRow.Children().InsertAt(2, view.codexPill); // right after the status icon and the label
        }
        else if (!model.codex && view.codexPill)
        {
            SidePanelRemoveChild(view.stateRow, view.codexPill);
            view.codexPill = nullptr;
        }
        // The STATUS ICON's three indicators (SIDE_PANEL.md §2g) — never added or dropped, only shown or hidden: the glyph
        // holds every element, and each setter starts / stops its own motion (the tab header's rule, StatusIconMotion.h).
        // The unsent-draft 3 dots, under the dot: shown with a draft, pulsing once the session's tab has started.
        if (fresh || model.pending != shown.pending || model.pendingAnimated != shown.pendingAnimated || model.pendingArgb != shown.pendingArgb)
        {
            view.glyph->SetPending(model.pending, SidePanelUnpackArgb(model.pendingArgb), model.pendingAnimated);
        }
        // The unread FLASH RING, behind the dot: painted with THIS window's one flash-ring brush, the very instance its
        // tabs' rings use — a settings change re-colors it in place (_RefreshFlashRingBrush), so the cards follow with no
        // update of their own. Built here if no tab of the window has flashed yet (the tab path's _SetTabFlashRing order:
        // brush first, then the ring shows). Its blink follows the UI thread's shared clock: in lockstep with the tabs'.
        if (fresh || model.flashRing != shown.flashRing)
        {
            if (model.flashRing)
            {
                _EnsureFlashRingBrush();
            }
            view.glyph->SetFlashRing(model.flashRing, _flashRingBrush);
        }
        // The cache-warm SPARK CROWN, around the dot: the glow behind it and the embers rising off it, in the tab's fire.
        if (fresh || model.cacheWarm != shown.cacheWarm || model.cacheWarmArgb != shown.cacheWarmArgb)
        {
            view.glyph->SetCacheWarm(model.cacheWarm, SidePanelUnpackArgb(model.cacheWarmArgb));
        }
        // Windows' animation switch moved since this card's last model: re-decide every motion (the crown has no model
        // field of its own that would carry the change; the dots' pendingAnimated above already re-decided theirs).
        if (!fresh && model.animationsEnabled != shown.animationsEnabled)
        {
            view.glyph->RefreshMotion();
        }
        if (model.timing.empty())
        {
            if (view.timing)
            {
                SidePanelRemoveChild(view.stateLine, view.timing);
                view.timing = nullptr;
            }
        }
        else if (!view.timing)
        {
            // Built like the board's TimingText (Internal.h) — the same dim 10px text and its explanatory
            // tip — from the model's already-formatted string ("created 4d1h, active 2m4s ago"). With "Sum" off
            // the card shows ONE tooltip, the rich session card (a tab header's model), so the explanatory
            // floating tip is not attached here; _SidePanelApplyCardTipMode adds it when the summary comes back.
            view.timing = Text(winrt::hstring{ model.timing }, 10, false, 0.45);
            // Left: the gap the row's own Spacing(5) gives its parts. Bottom: the status icon's pending band, so the text
            // centers on the dot like the label beside it (the line is the icon's 22 DIPs tall — SIDE_PANEL.md §2g).
            view.timing.Margin(ThicknessHelper::FromLengths(5, 0, 0, kSidePanelStatusIconBandPx));
            if (!_sidePanelSummaryHidden)
            {
                AgentSetTitledTip(view.timing, winrt::hstring{ kTimingTooltipTitle }, winrt::hstring{ kTimingTooltip });
            }
            Grid::SetColumn(view.timing, 1); // the state line's star column: it trims with "…" when the panel is narrow
            view.stateLine.Children().Append(view.timing);
        }
        else if (fresh || model.timing != shown.timing)
        {
            view.timing.Text(winrt::hstring{ model.timing }); // the ~30s "ago" refresh lands here, for the cards whose text moved
        }
        if (fresh || model.dirLine != shown.dirLine)
        {
            view.dirLine.Text(winrt::hstring{ model.dirLine });
        }
        if ((fresh || model.tipTitle != shown.tipTitle || model.tipText != shown.tipText) && !_sidePanelSummaryHidden)
        {
            // On an element that already has a tip, AgentSetTitledTip only updates the recorded text (no
            // second handler set), and a tip open right now shows the new text at once. (The tip yields while
            // the pointer is on the tag strip itself; see _SidePanelMakeTagStrip.) Skipped with "Sum" off: the
            // card's floating tip must stay silent then — its open would REPLACE the rich session tooltip
            // attached to the card (both go through ToolTipService) and its close would detach it. The new
            // text is not lost: it lands in view.model below, which _SidePanelApplyCardTipMode re-applies when
            // the summary comes back.
            AgentSetTitledTip(view.card, winrt::hstring{ model.tipTitle }, winrt::hstring{ model.tipText }, kCardTipDelay);
        }
        view.model = std::move(model);
        // Agentmaster (accessibility, E2E_TESTING.md §6.3): the card's item status follows what it now shows (change-gated
        // inside, so an apply that changed only the timing writes nothing here).
        SidePanelApplyCardItemStatus(view, id == _sidePanelSelectedId);
    }

    // Give a card's band the layout its tags need. The band area holds the CHILDLESS band (its first child, drawn
    // behind) and ONE content element over it, which carries the band padding as its margin. Untagged and unpinned: the
    // content is the title alone. Tagged: [ title * ][ tag strip Auto ][ pushpin Auto ] — the strip takes the width its
    // (capped) ribbons need and the title takes the rest, trimming with its ellipsis, so the ribbons stay in the band's
    // top-right corner and a long title can never push them off the card; the last column is the PIN TO TOP pushpin's
    // (SIDE_PANEL.md §2j, _SidePanelApplyCardPin), empty and 0 wide on an unpinned card. A changed tag list, tag color or
    // title ink REPLACES the strip (a few ribbons — simpler than editing them); the title element is only re-parented
    // between the band area and the grid, never rebuilt (_SidePanelEnsureBandGrid / _SidePanelReleaseBandGridIfBare), and
    // the band never gets a child (a rounded element with children gets its own composition node — see
    // _SidePanelCreateCardView).
    // `view`: the card. `model`: the values it must show (tags, their colors, the ink). `id`: the card's session —
    // handed to the strip so, with "Sum" off, its ribbons can suspend THIS card's rich tooltip while hovered.
    // Throws winrt::hresult_error if XAML refuses an edit — _SidePanelRebuildCards resets the list on a throw
    // (the "Sum" mode switch calls it inside _SidePanelApplyCardTipMode's catch).
    void TerminalPage::_SidePanelApplyCardTags(AgentSidePanelCardView& view, const AgentSidePanelCardModel& model, const std::wstring& id)
    {
        if (model.tags.empty())
        {
            if (view.tagStrip)
            {
                // The strip leaves the grid; the grid itself goes only when no pushpin is left in it either — a pinned card
                // keeps [ title * ][ ][ pushpin ] (SIDE_PANEL.md §2j). Then the band is the title alone again.
                if (view.bandGrid)
                {
                    SidePanelRemoveChild(view.bandGrid, view.tagStrip);
                }
                view.tagStrip = nullptr;
            }
            _SidePanelReleaseBandGridIfBare(view);
            return;
        }
        std::vector<Color> colors;
        colors.reserve(model.tagArgb.size());
        for (const uint32_t argb : model.tagArgb)
        {
            colors.push_back(SidePanelUnpackArgb(argb));
        }
        // Hang the strip from the band's TOP EDGE: lift it by exactly the band's top padding (the grid's top margin),
        // so the ribbons start where the card's colored bar starts — bookmarks clipped to the top of the card rather
        // than badges floating mid-bar. The same constant the margin is made of, so a later padding change can't
        // silently detach them from the edge.
        auto strip = _SidePanelMakeTagStrip(model.tags, colors, model.darkInk, kSidePanelBandPadY, view.card, id);
        Grid::SetColumn(strip, 1); // the middle column: left of the pushpin's, so the pin stays at the band's very end
        // The title alone becomes [ title * ][ strip ][ pushpin ] (a no-op when the grid exists — a card pinned before it was
        // tagged already has it). Built BEFORE the strip goes in, so the strip only ever lands in a grid that is in the band.
        _SidePanelEnsureBandGrid(view);
        if (view.tagStrip)
        {
            SidePanelRemoveChild(view.bandGrid, view.tagStrip); // the strip is replaced, never edited
        }
        view.bandGrid.Children().Append(strip);
        view.tagStrip = strip;
    }

    void TerminalPage::_SidePanelEnsureBandGrid(AgentSidePanelCardView& view)
    {
        if (view.bandGrid)
        {
            return;
        }
        Grid bandGrid{};
        ColumnDefinition titleCol{};
        titleCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        ColumnDefinition tagCol{};
        tagCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
        // SIDE_PANEL.md §2j: the PIN TO TOP pushpin's column, the band's right end. Auto, so it is 0 wide on an unpinned card —
        // a tagged card that is not pinned lays out exactly as before the pin existed.
        ColumnDefinition pinCol{};
        pinCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
        bandGrid.ColumnDefinitions().Append(titleCol);
        bandGrid.ColumnDefinitions().Append(tagCol);
        bandGrid.ColumnDefinitions().Append(pinCol);
        bandGrid.Margin(ThicknessHelper::FromLengths(kSidePanelBandPadX, kSidePanelBandPadY, kSidePanelBandPadX, kSidePanelBandPadY)); // the band padding moves from the title to the grid that now holds it
        SidePanelRemoveChild(view.bandArea, view.title); // release the title from the band area before the grid takes it (one parent)
        view.title.Margin(ThicknessHelper::FromUniformLength(0));
        Grid::SetColumn(view.title, 0);
        bandGrid.Children().Append(view.title);
        view.bandArea.Children().Append(bandGrid); // after the band: drawn over it
        view.bandGrid = bandGrid;
    }

    void TerminalPage::_SidePanelReleaseBandGridIfBare(AgentSidePanelCardView& view)
    {
        if (!view.bandGrid || view.tagStrip || view.pinIcon)
        {
            return; // the band is already the title alone, or the grid still holds a tag strip or the pushpin
        }
        // Back to the plain band: the title alone over the band, the padding back on it. The title leaves the grid FIRST: a
        // XAML element has one parent, and handing it to the band area while the grid still holds it would throw.
        SidePanelRemoveChild(view.bandGrid, view.title);
        SidePanelRemoveChild(view.bandArea, view.bandGrid);
        Grid::SetColumn(view.title, 0);
        view.title.Margin(ThicknessHelper::FromLengths(kSidePanelBandPadX, kSidePanelBandPadY, kSidePanelBandPadX, kSidePanelBandPadY));
        view.bandArea.Children().Append(view.title); // after the band: drawn over it
        view.bandGrid = nullptr;
    }

    void TerminalPage::_SidePanelApplyCardPin(AgentSidePanelCardView& view, const AgentSidePanelCardModel& model)
    {
        if (!model.pinned)
        {
            if (view.pinIcon)
            {
                if (view.bandGrid)
                {
                    SidePanelRemoveChild(view.bandGrid, view.pinIcon);
                }
                view.pinIcon = nullptr;
            }
            _SidePanelReleaseBandGridIfBare(view); // the title alone again, unless a tag strip is left
            return;
        }
        if (!view.pinIcon)
        {
            FontIcon pin{};
            pin.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ kSidePanelPinFont });
            pin.Glyph(kSidePanelPinGlyph);
            pin.FontSize(kSidePanelPinIconSize);
            pin.VerticalAlignment(VerticalAlignment::Center); // on the title's line, not hung from the top edge like the ribbons
            pin.Margin(ThicknessHelper::FromLengths(kSidePanelPinIconGapPx, 0, 0, 0));
            // Not hit-testable: a press on the pushpin falls through to the band, so it selects the card like any other press
            // on it, and the card's own tooltip — which names the pin and its way back — covers it. That also keeps the user's
            // rule that a card is pinned (and unpinned) only through its right-click menu: the pushpin is no button.
            pin.IsHitTestVisible(false);
            Grid::SetColumn(pin, 2); // the band's right end, right of the tag strip
            _SidePanelEnsureBandGrid(view);
            view.bandGrid.Children().Append(pin);
            view.pinIcon = pin;
        }
        // The title's own ink (WCAG-picked against the band, like the "+N" count), so the pushpin reads on any folder color.
        view.pinIcon.Foreground(model.darkInk ? Fill(0xFF, 0x10, 0x10, 0x10) : Fill(0xFF, 0xF0, 0xF0, 0xF0));
    }

    // Build a card's bookmark-TAG strip: the ribbons in the title band's TOP-RIGHT corner. It is the
    // side-panel twin of the Triage-Board card's ribbons, which hang BELOW that card's band instead —
    // here the tags live ON the bar, where the eye already reads the card's name and folder color. One
    // ribbon per tag, in the order the tags were added; with more tags than kSidePanelCardTagSlots, the
    // last slot becomes a "+N" whose own tip names the hidden ones. Each ribbon opens the SAME rich
    // hover panel the tab badges, the board card and the Sessions Tags column open
    // (_OnTagBadgeHoverBegin/End: every session carrying the tag, live ones first; a click on a live
    // row jumps to its tab, cross-window).
    //
    // ⚠ The strip is a TOOLTIP-FREE ZONE for the card. The card carries one AgentSetTip tooltip on its
    // whole Border, and pointer events bubble, so a ribbon would otherwise ALSO arm that tip — which
    // floats at the pointer (this panel has no LocalTooltip area) and would pop on top of the hover
    // panel once the pointer rests on a ribbon. So: a ribbon's entry cancels any pending card tip,
    // closes an open one, and stops the event there; the strip swallows the moves bubbling up from its
    // children, which would re-arm it; and the "+N" wires its own tip BEFORE marking its entry handled,
    // so ITS tip still opens while the card's cannot. Leaving the strip for the rest of the card re-arms
    // the card tip as usual (the tip helper's nested-tip re-arm on the next PointerMoved).
    //
    // With "Sum" OFF (SIDE_PANEL.md §2d) the card's tooltip is the RICH session card instead — a ToolTip attached
    // to the card for its whole life, opened by ToolTipService, which nothing may close by writing IsOpen. A
    // ribbon / "+N" entry therefore DETACHES it (_SidePanelSuspendCardToolTip: the framework closes an open one
    // and cancels a pending open itself), so the session card never floats beside the panel while the tag hover
    // panel is up; the next hover over the card's body re-attaches it (the card push re-hosts whenever nothing
    // is attached). The "+N" has NO floating tip in that mode — the rich card lists every tag as a chip.
    //
    // `tags`: the session's tags in the order they were added — non-empty (an untagged card builds no strip).
    // `colors`: each tag's RESOLVED display color, parallel to `tags` — the card model's tagArgb, which
    //           SidePanelCardModelFor resolved through ResolveTagDisplayColor (user-picked > name hash), exactly
    //           like the tab badges. A missing entry (never, for a model) falls back to the name hash.
    // `darkInk`: true when the band is light enough that the title uses dark ink; the "+N" count follows
    //            the title's ink so it stays readable on any folder color.
    // `hangPx`: the band's top padding — the strip is lifted by exactly this much, so the ribbons hang
    //           from the band's top edge.
    // `card`: the card (its root Grid) whose tooltip must yield over the strip. The handlers hold it WEAKLY: the
    //         strip lives inside the card, so a strong capture would be the element -> handler -> element
    //         cycle that leaks one card per rebuild (the board's weak-capture rule).
    // `id`: the card's session — whose rich tooltip a ribbon / "+N" entry suspends in the "Sum" off mode
    //       (captured by value; resolved through the card map at hover time, so a dropped card is a no-op).
    // Returns the unparented strip (a horizontal StackPanel), right- and top-aligned for the band's
    // Auto column.
    // Throws winrt::hresult_error if XAML construction fails (off the UI thread / a torn-down tree); the
    // pointer handlers themselves never throw out (weak refs that no longer resolve just no-op; the suspend
    // never throws).
    winrt::Windows::UI::Xaml::Controls::StackPanel TerminalPage::_SidePanelMakeTagStrip(const std::vector<std::wstring>& tags,
                                                                                    const std::vector<winrt::Windows::UI::Color>& colors,
                                                                                    bool darkInk,
                                                                                    double hangPx,
                                                                                    const winrt::Windows::UI::Xaml::UIElement& card,
                                                                                    const std::wstring& id)
    {
        StackPanel strip{};
        strip.Orientation(Orientation::Horizontal);
        strip.Spacing(2); // the gap the tab badges and the board card leave between ribbons
        strip.HorizontalAlignment(HorizontalAlignment::Right);
        strip.VerticalAlignment(VerticalAlignment::Top);
        // Left: 6px of air between the ellipsis-trimmed title and the first ribbon. Top: negative, to hang
        // the ribbons from the band's top edge (`hangPx`). The negative margin also takes that much off the
        // strip's desired height, so the strip can never make the band taller than its title line.
        strip.Margin(ThicknessHelper::FromLengths(6, -hangPx, 0, 0));

        const bool overflow = tags.size() > kSidePanelCardTagSlots;
        const size_t ribbonCount = overflow ? (kSidePanelCardTagSlots - 1) : tags.size();
        const auto weakThis = get_weak();
        const auto cardWeak = winrt::make_weak(card);
        for (size_t i = 0; i < ribbonCount; ++i)
        {
            auto ribbon = SidePanelMakeTagRibbon(i < colors.size() ? colors[i] : TagColorFor(tags[i]));
            const winrt::hstring tagName{ tags[i] };
            ribbon.PointerEntered([weakThis, cardWeak, tagName, id](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                // Keep the card's own tip off the hover panel: drop a pending open and close an open one
                // (AgentCloseTipsIn does both, for the card and anything under it)...
                if (const auto c = cardWeak.get())
                {
                    AgentCloseTipsIn(c);
                }
                // ...and end the event here — bubbling on, it would reach the card's tip handler and arm it again.
                e.Handled(true);
                const auto self = weakThis.get();
                const auto el = sender.try_as<UIElement>();
                if (self && el)
                {
                    // "Sum" off: the rich session card must not float beside the panel under the tag hover panel —
                    // detach it (a no-op in the classic mode). Before the hover begins, so the two never overlap.
                    self->_SidePanelSuspendCardToolTip(id);
                    self->_OnTagBadgeHoverBegin(tagName, el); // ~160ms open delay; re-targets if another ribbon is mid-delay
                }
            });
            ribbon.PointerExited([weakThis](const IInspectable&, const PointerRoutedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_OnTagBadgeHoverEnd(); // cancels a pending open; an open panel gets the ~300ms grace close
                }
            });
            strip.Children().Append(ribbon);
        }

        if (overflow)
        {
            const size_t hidden = tags.size() - ribbonCount;
            auto more = Text(winrt::hstring{ L"+" + std::to_wstring(hidden) }, 9, true, 0.85);
            more.Foreground(darkInk ? Fill(0xFF, 0x10, 0x10, 0x10) : Fill(0xFF, 0xF0, 0xF0, 0xF0)); // the title's ink
            std::wstring hiddenList;
            for (size_t i = ribbonCount; i < tags.size(); ++i)
            {
                hiddenList += (i > ribbonCount ? L" \x00B7 " : L"") + tags[i];
            }
            // ORDER MATTERS: the tip helper's own PointerEntered/Moved handlers must be hooked BEFORE the
            // entry handler below, so they arm the "+N" tip before that handler marks the event handled.
            // Classic mode only: with "Sum" off the card's ONE tooltip is the rich session card, which lists every
            // tag as a chip — a second floating tip here would only stack on it. (_SidePanelApplyCardTipMode
            // rebuilds the strip on every mode switch, so the "+N" always matches the mode.)
            if (!_sidePanelSummaryHidden)
            {
                AgentSetTitledTip(more,
                                  winrt::hstring{ std::to_wstring(hidden) + L" more tag" + (hidden == 1 ? L"" : L"s") },
                                  winrt::hstring{ hiddenList + L"\n\nRight-click the card \x2192 Tags to see or change them all." });
            }
            more.PointerEntered([weakThis, cardWeak, id](const IInspectable&, const PointerRoutedEventArgs& e) {
                // Close only the CARD's tip if it is up. Not AgentCloseTipsIn: that also cancels the
                // pending open this element's own tip handler has just armed.
                if (const auto c = cardWeak.get())
                {
                    AgentCloseTipOn(c);
                }
                e.Handled(true); // don't let the card's tip handler re-arm the card over this one
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelSuspendCardToolTip(id); // "Sum" off: the "+N" is part of the tag zone too (a no-op in the classic mode)
                }
            });
            strip.Children().Append(more);
        }

        // Swallow the moves bubbling up from the ribbons and the "+N": on reaching the card, each one
        // would re-arm the card's tip (the helper re-arms an un-armed outer element on any move). The
        // children's own handlers have already run by the time a move bubbles here.
        strip.PointerMoved([](const IInspectable&, const PointerRoutedEventArgs& e) {
            e.Handled(true);
        });
        return strip;
    }

    // --- "Sum" off: the cards' RICH SESSION TOOLTIP (SIDE_PANEL.md §2d) --------------------------------------------
    // With the summary area hidden, hovering a card shows the card a TAB header shows when hovered: the same
    // AgentSessionToolTipHost lifecycle (framework-managed ToolTip, closed-only Content swaps, the Loaded re-attach,
    // the lazy hover build, the wheel scroll read on the owner) and the same content push
    // (_PushSessionToolTipCard — strings, signature, budgets, summary cache + waiters), placed beside the panel.

    // Switch ONE card to the tooltip mode the summary state asks for.
    //   RICH ("Sum" off): silence the card's own floating tips — the card's, the timing text's and (by rebuilding the
    //     strip) the "+N"'s — because the shared tip host's open would REPLACE the rich ToolTip on the card and its
    //     close would detach it; then arm the rich tooltip host.
    //   CLASSIC ("Sum" on): clear the rich host (it detaches — the framework closes an open card itself) and forget its
    //     surface-keyed state, then give the floating tips back from the card's CURRENT model.
    // `view`: the card. `id`: its session. `rebuildTags`: re-make the tag strip (a mode switch: the "+N" tip follows
    // the mode); false for a card whose fresh apply just built it in this mode. `lean`: the card's lean session row
    // for the arm-build (null -> the arm-build reads GetWithoutQueue).
    // Never throws: any failure is logged (Rule #18) and costs only this card's tooltip — never the list.
    void TerminalPage::_SidePanelApplyCardTipMode(AgentSidePanelCardView& view, const std::wstring& id, bool rebuildTags, const ::Agentmaster::SessionInfo* lean)
    {
        if (!view.card)
        {
            return;
        }
        try
        {
            if (_sidePanelSummaryHidden)
            {
                AgentSuppressTip(view.card);
                if (view.timing)
                {
                    AgentSuppressTip(view.timing);
                }
                if (rebuildTags && !view.model.tags.empty())
                {
                    _SidePanelApplyCardTags(view, view.model, id); // the "+N" comes back without its floating tip
                }
                _SidePanelArmCardToolTip(view, id, lean);
            }
            else
            {
                if (view.tipHost)
                {
                    view.tipHost->Clear(); // detach; the hooks stay wired but their callbacks no-op while "Sum" is on
                }
                const std::wstring key = SidePanelToolTipKey(id);
                _tabTooltipSig.erase(key);
                _tabTooltipScroll.erase(key);
                // Re-applied from view.model (current: _SidePanelApplyCardModel stores every model even while it
                // skips the tip write in the rich mode). The first call on a card born in the rich mode wires the
                // floating tip's handlers; a card that had one just gets its text back (AgentSuppressTip kept the
                // "already wired" marker).
                AgentSetTitledTip(view.card, winrt::hstring{ view.model.tipTitle }, winrt::hstring{ view.model.tipText }, kCardTipDelay);
                if (view.timing)
                {
                    AgentSetTitledTip(view.timing, winrt::hstring{ kTimingTooltipTitle }, winrt::hstring{ kTimingTooltip });
                }
                if (rebuildTags && !view.model.tags.empty())
                {
                    _SidePanelApplyCardTags(view, view.model, id); // the "+N" gets its floating tip back
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelApplyCardTipMode");
        }
    }

    // Create the card's rich tooltip host on first use, wire its hooks once, and arm-build it. The host's owner is
    // the card Border (held weakly — the card view holds it); its placement provider docks the card beside the panel
    // (_SidePanelCardToolTipPlacement); "card" is its [tooltip-wheel] label and "Side-panel card" its [exc] context.
    // The arm-build hosts a HEADER-ONLY card (kickSummary=false — the transcript analyze belongs to a real hover,
    // else arming a 400-card list would burst 400 whole-transcript parses) so ToolTipService has a tooltip attached
    // before the first hover: a card built before its list insert is not loaded yet, so the host's Loaded handler
    // does the attach, still before any pointer can reach it. Re-armed after a classic -> rich switch (the Clear left
    // the host inactive).
    // `view`: the card. `id`: its session (captured by value in the hooks; resolved through the card map at call
    // time, so a card that left the list is a no-op). `lean`: the card's lean session row, or null.
    // Throws what the host / the push throws (std::bad_alloc, a XAML failure) — _SidePanelApplyCardTipMode contains it.
    void TerminalPage::_SidePanelArmCardToolTip(AgentSidePanelCardView& view, const std::wstring& id, const ::Agentmaster::SessionInfo* lean)
    {
        if (!view.card)
        {
            return;
        }
        if (!view.tipHost)
        {
            view.tipHost = AgentSessionToolTipHost::Create(view.card, L"card", L"Side-panel card");
            view.tipHost->SetPlacementProvider([weakThis = get_weak()](const FrameworkElement& owner) -> std::optional<AgentSessionToolTipPlacement> {
                const auto self = weakThis.get();
                return self ? self->_SidePanelCardToolTipPlacement(owner) : std::nullopt;
            });
        }
        view.tipHost->EnsureHoverHook(
            [weakThis = get_weak(), id]() {
                if (const auto self = weakThis.get())
                {
                    self->_OnSidePanelCardToolTipHover(id);
                }
            },
            [weakThis = get_weak(), id](int delta) -> bool {
                const auto self = weakThis.get();
                return self ? self->_ScrollSidePanelCardToolTip(id, delta) : false;
            });
        if (!view.tipHost->Active())
        {
            _UpdateSidePanelCardToolTip(id, /*swapWhileOpen*/ false, /*kickSummary*/ false, lean);
        }
    }

    // The SIDE-PANEL CARD surface of the rich session tooltip — the twin of _UpdateTabAgentToolTip: resolve the
    // session, clear on gone / archived, else hand everything to the SHARED _PushSessionToolTipCard with this card's
    // surface key, its tags (the card model already holds them color-resolved through ResolveTagDisplayColor — the
    // same colors the tab's AgentTagsSpec carries), and the room beside the panel as the width budget.
    // `id`: the card's session. `swapWhileOpen`: the one-shot open-swap grant (the summary arrival). `kickSummary`:
    // false = header only, no transcript analyze (the arm-build and the arrival's re-host). `lean`: a lean session row
    // to use instead of a registry read (the list update / the mode switch have one); null -> GetWithoutQueue (the
    // card reads no queue field, so the most expensive part of a record copy is skipped).
    // A no-op unless the panel is built, "Sum" is off, and the card has a host. Throws what the push throws —
    // callers contain it (the host's hover catch, _SidePanelApplyCardTipMode, the arrival's per-waiter catch).
    void TerminalPage::_UpdateSidePanelCardToolTip(const std::wstring& id, bool swapWhileOpen, bool kickSummary, const ::Agentmaster::SessionInfo* lean)
    {
        if (!_sidePanelBuilt || !_sidePanelSummaryHidden || !_sessionRegistry)
        {
            return;
        }
        const auto it = _sidePanelCardViews.find(id);
        if (it == _sidePanelCardViews.end() || !it->second.tipHost)
        {
            return; // the card left the list (or never got a host) — nothing to host on
        }
        const auto host = it->second.tipHost; // a local strong ref: nothing below may outlive the host it writes to
        const auto& model = it->second.model;
        const std::wstring key = SidePanelToolTipKey(id);
        std::optional<::Agentmaster::SessionInfo> fetched;
        const ::Agentmaster::SessionInfo* s = lean;
        if (!s)
        {
            fetched = _sessionRegistry->GetWithoutQueue(id);
            s = fetched ? &*fetched : nullptr;
        }
        if (!s || !s->live)
        {
            // Gone / archived between the list update and this hover: drop the card (the next update removes the
            // card itself) and its surface state — the tab path's !live branch, minus a default tooltip to revert to.
            host->Clear();
            _tabTooltipSig.erase(key);
            _tabTooltipScroll.erase(key);
            return;
        }
        // The tags as the push wants them: (name, color) chips in add order, and a fingerprint for the signature (a
        // tag edit or a recolor moves it, so the card re-hosts on the next hover).
        std::wstring tagsSpec;
        std::vector<std::pair<std::wstring, Color>> tagChips;
        tagChips.reserve(model.tags.size());
        for (size_t i = 0; i < model.tags.size(); ++i)
        {
            const bool resolved = i < model.tagArgb.size();
            tagChips.emplace_back(model.tags[i], resolved ? SidePanelUnpackArgb(model.tagArgb[i]) : TagColorFor(model.tags[i]));
            tagsSpec += model.tags[i];
            tagsSpec += L'\t';
            tagsSpec += resolved ? std::to_wstring(model.tagArgb[i]) : std::wstring{};
            tagsSpec += L'\n';
        }
        // The summary arrival re-hosts THIS card (looked up again by id then — the card may have left the list).
        auto rehost = [weakThis = get_weak(), id]() {
            if (const auto self = weakThis.get())
            {
                // ⚠ kickSummary=false — an arrival must NEVER schedule another analyze (the 2026-08-29 loop rule).
                self->_UpdateSidePanelCardToolTip(id, /*swapWhileOpen*/ true, /*kickSummary*/ false, nullptr);
            }
        };
        _PushSessionToolTipCard(*host, key, *s, tagsSpec, tagChips, _SidePanelCardToolTipRoomWidth(), swapWhileOpen, kickSummary, std::move(rehost));
    }

    // The card host's PointerEntered callback — the tab hover recipe (_ArmTabAgentToolTipHover's lambda) for a card:
    // a hover-time build (fresh state + fresh "ago"; kicks the mtime-gated summary load), and — only for a hover that
    // starts CLOSED — the scroll reset to the top of the body. PointerEntered BUBBLES, so it re-fires as the pointer
    // crosses the card's own children (band, title, state row) while the tip is already up; resetting then would snap
    // a reader back to the top every time the mouse moved. (The host re-anchored the CLOSED card before calling here.)
    // `id`: the card's session. Throws what the build throws — the host's hover catch contains and logs it.
    void TerminalPage::_OnSidePanelCardToolTipHover(const std::wstring& id)
    {
        if (!_sidePanelBuilt || !_sidePanelSummaryHidden)
        {
            return; // the classic mode: the hook stays wired on the card, but the floating tip owns the hover
        }
        const auto it = _sidePanelCardViews.find(id);
        if (it == _sidePanelCardViews.end() || !it->second.tipHost)
        {
            return;
        }
        const bool tipOpen = it->second.tipHost->Open();
        _UpdateSidePanelCardToolTip(id); // hover-time build: fresh state + fresh "ago", kicks the mtime-gated summary load
        if (!tipOpen)
        {
            _ResetTabAgentToolTipScroll(SidePanelToolTipKey(id)); // WHEEL SCROLL: a fresh hover starts at the top of the body
        }
    }

    // The card host's wheel callback: scroll the OPEN card's body one notch (the shared _ScrollSessionToolTipBody,
    // logged "[tooltip-wheel] <sid8> src=card …"). Returning false leaves the notch to the cards list's own
    // ScrollViewer (the host marks the event Handled only on true). Deliberately does NOT set _tooltipWheelClaimed:
    // that flag de-dupes the tab ROW's belt against a tab's own handler, and a stale true from here would make the
    // belt skip the next notch over the tab strip.
    // `id`: the card's session. `wheelDelta`: the notch's wheel units. Returns true when the card took the notch.
    // Throws std::bad_alloc while formatting the log line only — the host's wheel catch contains it.
    bool TerminalPage::_ScrollSidePanelCardToolTip(const std::wstring& id, int wheelDelta)
    {
        if (!_sidePanelBuilt || !_sidePanelSummaryHidden)
        {
            return false;
        }
        return _ScrollSessionToolTipBody(SidePanelToolTipKey(id), id, wheelDelta, L"card");
    }

    // The card host's placement provider: dock the rich card OUTSIDE the panel, beside the hovered card. The rect is
    // in the CARD's coordinate space and spans the whole panel plus its splitter horizontally and the card vertically,
    // so ToolTip placement (WinUI ToolTip::PerformNonMousePlacementWithPopup: the rect is mapped to window coordinates
    // at open time, inflated by the 20px mouse offset, then the popup is positioned + slid to fit the window) opens the
    // card just past the panel's edge — to the RIGHT of a left-docked panel ([panel][splitter][terminal]), to the LEFT
    // of a right-docked one ([terminal][splitter][panel]) — vertically centered on the hovered card
    // (ToolTipPositioning::PositionRelativeOnSide centers a Left/Right popup on the rect), slid back inside the window
    // when it would cross an edge. A popup wider than the room on its side is flipped by the framework to another
    // side; the width budget (_SidePanelCardToolTipRoomWidth) keeps the card inside that room. Recomputed on every closed
    // placement (each hover's re-anchor), so a width drag or a dock flip is picked up by the next hover.
    // `card`: the hovered card (the host's owner). Returns nullopt while the card or the panel has no size (not laid
    // out yet — the host keeps its current placement). Never throws: a failed transform is logged and reads nullopt.
    std::optional<AgentSessionToolTipPlacement> TerminalPage::_SidePanelCardToolTipPlacement(const FrameworkElement& card)
    {
        try
        {
            if (!_sidePanelBuilt || !_sidePanelHost || !card)
            {
                return std::nullopt;
            }
            const double cardH = card.ActualHeight();
            const double panelW = _sidePanelHost.ActualWidth();
            if (cardH <= 0.0 || panelW <= 0.0)
            {
                return std::nullopt;
            }
            // The card's left edge inside the panel (its margin + the list's own offsets) — so the rect can start at
            // the panel's left edge, which in the card's own coordinates is at -x.
            const auto origin = card.TransformToVisual(_sidePanelHost).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            AgentSessionToolTipPlacement p;
            if (_appSettings.sidePanelOnLeft)
            {
                p.mode = winrt::Windows::UI::Xaml::Controls::Primitives::PlacementMode::Right;
                p.rect = winrt::Windows::Foundation::Rect{ -origin.X, 0.0f, static_cast<float>(panelW + kSidePanelSplitterPx), static_cast<float>(cardH) };
            }
            else
            {
                p.mode = winrt::Windows::UI::Xaml::Controls::Primitives::PlacementMode::Left;
                p.rect = winrt::Windows::Foundation::Rect{ -origin.X - static_cast<float>(kSidePanelSplitterPx), 0.0f, static_cast<float>(kSidePanelSplitterPx + panelW), static_cast<float>(cardH) };
            }
            return p;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelCardToolTipPlacement");
            return std::nullopt;
        }
    }

    // The width the terminal side leaves for the rich card beside the panel: the content area minus the panel, its
    // splitter, the framework's 20px tooltip gap and an 8px margin to the window edge. The push clamps it to
    // [320, 660] (kTtCardMinWidth / kTtCardMaxWidth in TerminalPage.AgentObserver.cpp): never wider than a tab's card,
    // never squeezed unreadable — a window too narrow even for 320 lets the framework slide / clip the popup. (The
    // ToolTip itself is capped at 320 px by WinUI 2.8's ToolTip style — see the kTtCardMaxWidth note — which today
    // bounds this card exactly like the tab's; the budget is what keeps the card inside the room if that cap is lifted.)
    // Returns 0 while unmeasured (the push then uses the flat 660). Never throws: a XAML failure is logged, 0.
    double TerminalPage::_SidePanelCardToolTipRoomWidth()
    {
        try
        {
            const auto area = ContentArea();
            if (!area || !_sidePanelHost)
            {
                return 0.0;
            }
            const double areaW = area.ActualWidth();
            const double panelW = _sidePanelHost.ActualWidth();
            if (areaW <= 0.0 || panelW <= 0.0)
            {
                return 0.0;
            }
            return std::max(0.0, areaW - panelW - kSidePanelSplitterPx - kSidePanelTipGapPx - kSidePanelTipEdgeMarginPx);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelCardToolTipRoomWidth");
            return 0.0;
        }
    }

    // A tag ribbon / "+N" of this card is hovered (the strip's entry handlers): with "Sum" off, DETACH the card's rich
    // tooltip — the framework closes an open one and cancels a pending open itself (UnregisterToolTip), so nothing writes
    // IsOpen — so the session card never floats beside the panel while the tag hover panel is up. The hover's next entry
    // into the card's body re-attaches it: _PushSessionToolTipCard re-hosts whenever nothing is attached.
    // `id`: the card's session. A no-op in the classic mode or for a card without a host. Never throws (Detach is noexcept).
    void TerminalPage::_SidePanelSuspendCardToolTip(const std::wstring& id)
    {
        if (!_sidePanelBuilt || !_sidePanelSummaryHidden)
        {
            return;
        }
        const auto it = _sidePanelCardViews.find(id);
        if (it == _sidePanelCardViews.end() || !it->second.tipHost)
        {
            return;
        }
        it->second.tipHost->Detach();
    }

    // Before a card leaves the list (an update's Remove, or a reset): shut its rich tooltip host down — detach the
    // ToolTip from the card and drop both callbacks — and forget its surface-keyed signature + scroll state. The host's
    // own Unloaded detach is not enough: the event is deferred and can run after the view (and so the host) is gone,
    // and a ToolTip left attached to a dropped element is the islands leak class AgentTipHelpers.h describes. A summary
    // load still in flight for the session keeps its re-host lambda; it resolves the card by id at arrival and finds
    // none (or the session's NEW card, which is the right one to update).
    // `view`: the leaving card. `id`: its session. Never throws (Shutdown is noexcept; a failed key build is logged).
    void TerminalPage::_SidePanelDropCardToolTip(AgentSidePanelCardView& view, const std::wstring& id)
    {
        if (view.tipHost)
        {
            view.tipHost->Shutdown();
            view.tipHost = nullptr;
        }
        try
        {
            const std::wstring key = SidePanelToolTipKey(id);
            _tabTooltipSig.erase(key);
            _tabTooltipScroll.erase(key);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelDropCardToolTip");
        }
    }

    // Select a card: restyle the old/new borders in place (no rebuild), bind the summary below,
    // and — for selections that ORIGINATE here (a card click) — drive the Manager lens so the
    // board/tree/Auto-Testing follow (Linked Lenses). Calls that arrive FROM the lens or the tab
    // funnel pass driveManagerLens=false, and the same-id early-out breaks the echo loop either
    // way. An empty id deselects (hint shown below).
    // `id`: the managed session to select ("" == clear). `driveManagerLens`: true ONLY for a selection
    // that originates from a card click — it also pushes the selection into
    // AgentManagerContent::SelectSession, and cancels any ⌖ scroll-to-tab reveal still owed (the user's
    // own pick outranks a pending "scroll to the tab's card", which would otherwise yank the list away
    // from the card just clicked once the owed card rendered).
    void TerminalPage::_SidePanelSelectSession(const std::wstring& id, bool driveManagerLens)
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        if (driveManagerLens)
        {
            // BEFORE the same-id early-out: re-clicking the already-selected card is just as explicit. It outranks a PIN's
            // owed scroll too (SIDE_PANEL.md §2j): the user has picked where to look.
            _sidePanelRevealPendingId.clear();
            _sidePanelRevealForced = false;
        }
        if (_sidePanelSelectedId == id)
        {
            return; // already there — and the lens echo of our own drive lands here too
        }
        const std::wstring prev = _sidePanelSelectedId;
        _sidePanelSelectedId = id;
        // Restyle in place via the card map — a selection change must not pay a list update. The border
        // takes the color of the state the card SHOWS (its view's model, the same color as its dot) — no
        // registry copy per selection — and keeps the card's hover ring if the pointer is on it.
        const auto restyle = [this](const std::wstring& sid, bool sel) {
            const auto it = _sidePanelCardViews.find(sid);
            if (it == _sidePanelCardViews.end() || !it->second.cardBack)
            {
                return; // filtered out / its row not realized — nothing to paint (a card realized later is styled by its fresh apply)
            }
            SidePanelStyleCardBorder(it->second.cardBack, StateColor(it->second.model.state), sel, it->second.hovered);
            SidePanelApplyCardItemStatus(it->second, sel); // Agentmaster (accessibility): "selected" follows the border
        };
        if (!prev.empty())
        {
            restyle(prev, false);
        }
        if (!id.empty())
        {
            restyle(id, true);
        }
        // The AI search's result cards (SIDE_PANEL.md §2h) follow the same selection: a result card of the session now
        // selected wears the ring, the one of the session left loses it (a no-op while no result card exists).
        _SidePanelAiRestyleSelection(prev, id);
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
    // With the ⌖ scroll-to-tab toggle ON, a follow that MOVES the selection (a peek ending) also scrolls
    // the list back to your tab's card; a follow that changes nothing never scrolls.
    // Never throws beyond what its callees do (_SidePanelSelectSession's restyle/bind are UI-thread XAML
    // calls, exactly as before this toggle existed; the reveal itself is deferred + self-guarded).
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
        // "Nothing focused" — the Agent Manager page is on screen (no tab is focused then) — leaves the
        // current selection alone: returning to the Manager page should keep showing the last peek, not
        // clear it.
        if (!tab)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // a pwsh / cmd / external shell tab — no managed summary to show; keep the current peek
        }
        // Read BEFORE selecting: did this terminal touch actually MOVE the selection back to your tab (a
        // peek ended, or a Manager-side change had moved it off)? Only then does the ⌖ toggle owe the list a
        // scroll — the steady state (already on your tab's card) must never re-scroll, or every click into
        // the terminal would snap back a list you had scrolled by hand.
        const bool movesSelection = (_sidePanelSelectedId != id);
        _SidePanelSelectSession(id, /*driveManagerLens*/ false);
        if (movesSelection)
        {
            _SidePanelRevealTabCard(id); // no-op unless the ⌖ toggle is ON
        }
    }

    // Point the bottom area at `id`: create (or replace) the fill-hosted AgentTabOverlay whose
    // summary panel renders that session — the per-tab pencil panel's ENTIRE pipeline reused
    // (off-thread analyze, RenderSummaryBox/RenderCodexSummary, times ticker, wrap/truncate/
    // previous toggles, JUMP buttons) with this host doing the sizing. A same-id rebind only kicks
    // the cheap mtime-gated content re-check; an empty id releases the overlay (its dtor detaches
    // the registry observer) and shows the hint. The overlay is DELIBERATELY not in
    // _claudeOverlays (that map is keyed by session id and may already hold the session's own
    // per-tab overlay) — settings pushes reach it via _SidePanelApplySettings + the extended
    // _ToggleSummary* loops. With the summary area HIDDEN ("Sum" off) every bind RELEASES instead: an overlay nobody can
    // see would still run its registry observer and its off-thread analyses for each selection change; the toggle
    // rebinds the current selection when the area comes back.
    // `id`: the session to summarize ("" == none).
    void TerminalPage::_SidePanelBindSummary(const std::wstring& id)
    {
        if (!_sidePanelBuilt || !_sidePanelSummaryHost)
        {
            return;
        }
        if (id.empty() || _sidePanelSummaryHidden)
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
        overlay->SetSummaryShowHistory(_appSettings.summaryPanelShowHistory);
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
            overlay->SetSummaryHistoryToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryHistory();
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
            // Agentmaster (transcript overlay): the summary panel's right-click "Show Transcript" opens
            // the selected session's transcript in the window-level reading view.
            overlay->SetShowTranscriptHandler([weakThis, sid]() {
                if (auto self = weakThis.get())
                {
                    self->_ShowTranscriptOverlayForSession(sid, L"side panel summary");
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
    // cross-window settings broadcast (both run after _appSettings was replaced): re-dock if
    // sidePanelOnLeft changed elsewhere, drop an owed ⌖ reveal when scroll-to-tab turned off, push the
    // summary content flags + color mode into the fill-hosted overlay (it is outside the _claudeOverlays
    // broadcast loops), and rebuild the cards (the shared order / color rules may have changed). The
    // header's ⋯ menu needs nothing here: it reads the order, ⌖ and the dock from _appSettings at its next
    // open (SIDE_PANEL.md §2i). Per-window layout (fractions / collapse) is deliberately NOT touched — a
    // global-default change only affects NEW windows.
    void TerminalPage::_SidePanelApplySettings()
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        // The "+" launcher card (SIDE_PANEL.md §2e) stays open across a settings broadcast — the user may be typing in
        // it while another window saves — unless the broadcast MOVED the dock: its "+" button then sits on the other
        // side of the window, away from the card it opened.
        if (_SidePanelLaunchCardOpen() && _sidePanelLaunchCardDockLeft != _appSettings.sidePanelOnLeft)
        {
            _SidePanelCloseLaunchCard(L"dock moved by a settings change");
        }
        // The funnel's panel and the folder card follow the same rule (SIDE_PANEL.md §2l): kept across a broadcast, closed when
        // it moved the dock (the funnel they opened from went to the other side).
        if ((_SidePanelFilterPanelOpen() || _SidePanelFolderCardOpen()) && _sidePanelFilterDockLeft != _appSettings.sidePanelOnLeft)
        {
            _SidePanelCloseFilterSurfaces(L"dock moved by a settings change");
        }
        _SidePanelApplyDock(); // no-op when the dock side didn't change (idempotent assignments)
        // ⌖ scroll-to-tab is GLOBAL (another window's ⋯ menu may have broadcast it here): if it just turned OFF,
        // drop a reveal still owed so nothing scrolls after the user asked it not to — except a PIN's (SIDE_PANEL.md §2j),
        // which answers this window's own "Pin to Top" and is no tab selection the setting governs.
        if (!_appSettings.sidePanelScrollToSelectedTab && !_sidePanelRevealForced)
        {
            _sidePanelRevealPendingId.clear();
        }
        if (_sidePanelSummaryOverlay)
        {
            _sidePanelSummaryOverlay->SetSummaryWrapNewlines(_appSettings.summaryPanelWrapNewlines);
            _sidePanelSummaryOverlay->SetSummaryTruncate(_appSettings.summaryPanelTruncate);
            _sidePanelSummaryOverlay->SetSummaryShowHistory(_appSettings.summaryPanelShowHistory);
            _sidePanelSummaryOverlay->SetTabColorMode(static_cast<int>(_appSettings.tabColorMode));
            _sidePanelSummaryOverlay->SetModelFamilies(_appSettings.modelFamilies);
        }
        _SidePanelScheduleRebuild(/*force*/ true); // in AI mode it is owed until the AI view closes (SIDE_PANEL.md §2h)
        if (_sidePanelAiMode)
        {
            // The tab coloring rules may have changed: the result cards' bands resolve against the new mode now, not on the
            // next tick (never throws).
            _SidePanelAiRefreshResults();
        }
    }

    // ======================================================================================
    // The header's "+" LAUNCHER (SIDE_PANEL.md §2e)
    // ======================================================================================
    // WHAT: a click on "+" opens a CARD at the click point — its corner at the cursor, extending toward the window's
    // center (AgentMaster/PointerCardPlacement.h), the user's tooltip-like rule — holding the Agent Manager's own
    // launch row: [● Claude] [ box ] [Launch Claude] [Fork]. The row IS the Manager's code (the shared AgentLaunchBar,
    // moved out of AgentManagerContent.Launch.cpp verbatim): the same box, validation underline, path-picker drop-down
    // (Browse…, RECENT, GIT WORKTREES, MATCHES / SUBFOLDERS), Launch hover menu (models, Specify…, presets) and the
    // same launch / resume / fork / "Create & Launch" — reaching the very page seams the Manager's handlers call. Two
    // differences, both the user's: no "launch a" / "session in" words, and Claude only (a fixed "● Claude" chip;
    // Codex launches from here are planned).
    // THE BOX IS THE MANAGER'S: the card opens on the Manager box's text, mirrors every edit into it live, and writes
    // its last text back when it closes — so the two boxes always read the same ("keep the last, same one we have in
    // agent manager tab"). Both bars push into ONE recent-folders list (AgentManagerContent::LaunchRecentDirs).
    // THE FOLDER LIST OPENS WITH THE CARD (the user's rule, 2026-10-19): right under the box, once a layout pass has put
    // the card at its place (_SidePanelFinishLaunchCardOpen) — never at the card's previous place, over the box and
    // Launch (the 2026-10-12 report). Esc closes the list first, a second Esc the card.
    // LIFETIME: built on the first "+" click and kept for the window's life (a window that never opens it pays
    // nothing); closed by a press outside it, Esc, "+" again, a launch, a tab switch, a collapse and a dock flip.
    // Interactive like a tooltip that can be entered: nothing closes on pointer-leave.

    bool TerminalPage::_SidePanelLaunchCardOpen() const noexcept
    {
        try
        {
            return _sidePanelLaunchPopup && _sidePanelLaunchPopup.IsOpen();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: IsOpen read"); // a torn-down popup reads as closed
            return false;
        }
    }

    void TerminalPage::_SidePanelToggleLaunchCard()
    {
        // Consume the press FIRST, whatever happens next: one press belongs to one Click, so a later Click without a
        // press of its own (UI Automation) anchors under the button, never at an old point.
        const bool fromPress = _sidePanelLaunchAnchorFromPress;
        _sidePanelLaunchAnchorFromPress = false;
        if (_SidePanelLaunchCardOpen())
        {
            // The "+" press itself is exempt from the outside-press close (_SidePanelLaunchPressIsInside), so a click on
            // "+" with the card open lands here: a toggle, like a second click on any button that opened a pane.
            _SidePanelCloseLaunchCard(L"+ again");
            return;
        }
        _SidePanelOpenLaunchCard(fromPress);
    }

    bool TerminalPage::_SidePanelEnsureLaunchCard()
    {
        if (_sidePanelLaunchPopup && _sidePanelLaunchCard && _sidePanelLaunchBar)
        {
            return true;
        }
        try
        {
            const auto root = Root();
            if (!root)
            {
                return false; // no page root to parent into (never after the first layout)
            }
            const auto weakThis = get_weak();

            // The card: the tag editor's dark look (every Agentmaster surface is dark, whatever the app theme).
            Border card{};
            card.RequestedTheme(ElementTheme::Dark);
            card.Background(Fill(0xFF, 0x26, 0x26, 0x26));
            card.BorderBrush(Fill(0xFF, 0x3A, 0x3A, 0x3A));
            card.BorderThickness(ThicknessHelper::FromUniformLength(1));
            card.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
            card.Padding(ThicknessHelper::FromLengths(10, 8, 10, 8));
            // Tab cycles INSIDE the card (box -> Launch -> Fork -> box) instead of walking out into the page behind it.
            card.TabFocusNavigation(KeyboardNavigationMode::Cycle);
            // The Agent Manager's six Button state brushes, on the card: under the forced-Dark theme the default
            // ButtonBackground* brushes resolve to null, and Launch / Fork would be hit-testable only on their text.
            AgentLaunchBar::SeedButtonStateBrushes(card.Resources());

            // The launch bar: the Agent Manager's launch code, hosted Claude-only.
            AgentLaunchBarHost host;
            host.codexToggle = false; // Claude only (the user's call): the "● Claude" chip, no Codex mode, no spawnCodex
            // ONE recent-folders list per window — the Manager's — so a folder launched from either bar tops both
            // drop-downs. Without a Manager (the page builds it at first layout, before the side panel, so this is a
            // belt), a private list read from disk keeps the drop-down's RECENT section useful.
            if (const auto ipc = _agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    host.recentDirs = mgr->LaunchRecentDirs();
                }
            }
            if (!host.recentDirs)
            {
                host.recentDirs = std::make_shared<AgentLaunchRecentDirs>();
                host.recentDirs->dirs = ::Agentmaster::LoadRecentDirs();
            }
            // The drop-down lives in Root() beside the card's own Popup, across Root's three rows (tab row, infobars,
            // content — the tag editor's recipe), so its offsets are Root-relative like the card's.
            host.popupHost = root;
            host.popupRowSpan = 3;
            host.dialogAnchor = card; // a "Couldn't create folder" dialog takes the forced-Dark card's island and theme
            host.dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            // Read at call time, like the Manager's host: a cog Save or a settings broadcast replaces _appSettings.
            host.registry = [weakThis]() -> std::shared_ptr<::Agentmaster::SessionRegistry> {
                if (const auto self = weakThis.get())
                {
                    return self->_sessionRegistry;
                }
                return nullptr; // the page is gone: a session id resolves through its transcript only
            };
            host.recentDirsLimit = [weakThis]() -> uint32_t {
                if (const auto self = weakThis.get())
                {
                    return self->_appSettings.recentDirsLimit;
                }
                return 0u; // the bar maps 0 to the shipped 10
            };
            host.launchModels = [weakThis]() -> std::wstring {
                if (const auto self = weakThis.get())
                {
                    return self->_appSettings.launchModels;
                }
                return {};
            };
            // The three launches close the card FIRST (its final text mirrored into the Manager's box, the drop-down
            // shut), then run the page's own seams — the ones the Agent Manager's handlers call
            // (_WireAgentManagerContent), so a launch from the card IS a launch from the Manager: the same transcript
            // gates, the same [nav] lines, the same new tab. Closing first keeps the card from sitting over the new
            // tab while the launch selects and focuses it.
            // MGR-K1 (2026-10-23): over the Agent Manager page, a launch from the card is a PAGE launch
            // (_LaunchFromManagerPage): the page stays, the new session starts behind it and is selected there, as when it is
            // launched from the page's own row. Over a tab, the new tab opens in front as before.
            host.spawnClaude = [weakThis](const std::wstring& dir, const std::wstring& target) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelCloseLaunchCard(L"launch");
                    // An empty title lets the page derive one from the dir; `target` is "" (Default), a model id or
                    // "@<presetId>" from the Launch hover menu.
                    self->_LaunchFromManagerPage(L"plus-card", [&]() {
                        return self->_SpawnClaudeSession(winrt::hstring{ dir }, winrt::hstring{}, static_cast<uint32_t>(-1), winrt::hstring{ target });
                    });
                }
            };
            host.resumeSession = [weakThis](const std::wstring& id, const std::wstring& dir, const std::wstring& title) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelCloseLaunchCard(L"resume");
                    // An OPEN session is no launch: its id jumps to its tab, as before (the Agent Manager row's rule).
                    if (const auto existing = self->_sessionRegistry ? self->_sessionRegistry->GetWithoutQueue(id) : std::nullopt; existing && existing->live)
                    {
                        self->_ResumeSessionFromDisk(id, dir, title);
                        return;
                    }
                    self->_LaunchFromManagerPage(L"plus-card resume", [&]() {
                        return self->_ResumeSessionFromDisk(id, dir, title);
                    });
                }
            };
            host.forkSession = [weakThis](const std::wstring& id, const std::wstring& dir, const std::wstring& title) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelCloseLaunchCard(L"fork");
                    // HOOKS.md "Interrupted turns": asked first when the session is in the middle of a turn.
                    ForkLaunchRequest request;
                    request.sourceId = id;
                    request.dir = dir;
                    request.title = title;
                    request.origin = L"side-panel-launcher";
                    request.fromManagerPage = true; // MGR-K1: over the page, the fork stays behind it (decided when it runs)
                    self->_ForkAskingAboutRunningTurn(std::move(request));
                }
            };
            host.claudeMissing = [weakThis]() {
                if (const auto self = weakThis.get())
                {
                    self->_PromptClaudeMissing(); // the page-level "Claude Code (native) not found" notice
                }
            };
            // The page's "Specify a model" opener (a Root() popup — _SidePanelLaunchPressIsInside keeps the card open
            // while it is up, so its pick can still finish this card's launch).
            host.specifyModel = _ModelSpecifyOpener();
            // PRESETS: the hover menu's "Add presets…" item opens the page's help dialog. The bar decides when the item
            // shows (only while the user has no preset at all), so this is just the dialog.
            host.addPresets = [weakThis]() {
                if (const auto self = weakThis.get())
                {
                    self->_PromptAddPresets();
                }
            };
            host.layoutChanged = [weakThis]() {
                if (const auto self = weakThis.get())
                {
                    // Launch's label or Fork's visibility changed the row's width: re-fit the box and re-place the card,
                    // on a later tick (this can run inside a TextChanged; the card's SizeChanged follows anyway).
                    self->_SidePanelQueueLaunchCardPlacement();
                }
            };
            host.launched = [weakThis]() {
                if (const auto self = weakThis.get())
                {
                    // The bar's follow-up after the hand-off (a resumed / forked id cleared from the box) ran with the
                    // card already closed — off the live tree, where TextChanged is unreliable — so mirror once more:
                    // the Manager's box then ends empty too, exactly as a resume from the Manager leaves it.
                    self->_SidePanelSyncLaunchTextToManager();
                    self->_SidePanelCloseLaunchCard(L"launch"); // the belt: the launch callbacks above closed it already
                }
            };
            host.textEdited = [weakThis](const std::wstring& text) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                // Live: the Manager's box reads the same text while you type here. Its own TextChanged re-validates it
                // and stays quiet otherwise (no textEdited on the Manager's host, so the mirror can never echo back).
                if (const auto ipc = self->_agentManagerContent.get())
                {
                    if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                    {
                        mgr->SetLaunchBoxText(text);
                    }
                }
            };
            auto bar = AgentLaunchBar::Create(std::move(host));

            // [● Claude] [ box / underline ] [Launch Claude] [Fork] — the Manager's row minus its words.
            StackPanel row{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(kSidePanelLaunchRowSpacingPx);
            row.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(bar->AgentElement());
            row.Children().Append(bar->CwdColumn());
            row.Children().Append(bar->LaunchButton());
            row.Children().Append(bar->ForkButton());
            card.Child(row);

            // Enter / Escape, acted on at the KEY-UP — the AgentModelPrompt.h rule. Closing the card (or launching,
            // which closes it) moves focus to a terminal, and Windows still delivers the rest of the keystroke — the
            // character message, a CR for Enter, an ESC for Escape — to whatever holds focus next: the CR would submit
            // that claude's unsent draft, the ESC would interrupt its running turn. By the key-up the character has gone
            // to the card's own focused element, which ignores it. PreviewKeyDown TUNNELS, so it runs before the box and
            // the buttons, and marking the key handled there keeps them from acting on it:
            //   - Escape with the drop-down CLOSED closes the card. With the drop-down open the box takes its own
            //     Escape (it closes the list, as on the Agent Manager page) and the card stays.
            //   - Enter on the focused Launch / Fork button runs what its click runs. Enter in the BOX stays the box's own
            //     commit (close the drop-down, normalize the path — the Agent Manager page's Enter), never a launch.
            //   - Never for a Ctrl / Alt chord (Alt+Enter is Terminal's full-screen toggle).
            card.PreviewKeyDown([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    // Any key-down disarms first: the next key-up must belong to the key pressed last.
                    self->_sidePanelLaunchArmedKey = VirtualKey::None;
                    self->_sidePanelLaunchArmedAction = SidePanelLaunchKeyAction::None;
                    const auto key = e.Key();
                    if (key != VirtualKey::Enter && key != VirtualKey::Escape)
                    {
                        return;
                    }
                    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
                    {
                        return; // an app chord: not ours
                    }
                    const auto bar = self->_sidePanelLaunchBar;
                    if (!bar)
                    {
                        return;
                    }
                    auto action = SidePanelLaunchKeyAction::None;
                    if (key == VirtualKey::Escape)
                    {
                        if (bar->PathPickerOpen())
                        {
                            return; // the box's own Escape closes the drop-down first (the Agent Manager page's key)
                        }
                        action = SidePanelLaunchKeyAction::Close;
                    }
                    else
                    {
                        const void* const source = SidePanelLaunchIdentity(e.OriginalSource());
                        if (source && source == SidePanelLaunchIdentity(bar->LaunchButton()))
                        {
                            action = SidePanelLaunchKeyAction::Launch;
                        }
                        else if (source && source == SidePanelLaunchIdentity(bar->ForkButton()))
                        {
                            action = SidePanelLaunchKeyAction::Fork;
                        }
                        else
                        {
                            return; // Enter in the box: its own commit, as on the Agent Manager page
                        }
                    }
                    self->_sidePanelLaunchArmedKey = key;
                    self->_sidePanelLaunchArmedAction = action;
                    e.Handled(true); // the Button must not click on this key-down, the box must not see this Escape
                }
                catch (...)
                {
                    // Nothing may escape a XAML event handler (it can fail the whole app); the key then just does what
                    // the focused control does with it. Recorded (Rule #18).
                    ::Agentmaster::AgentLogCaughtException(L"side panel launcher: key-down");
                }
            });
            card.PreviewKeyUp([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                const auto key = e.Key();
                if (self->_sidePanelLaunchArmedKey == VirtualKey::None || key != self->_sidePanelLaunchArmedKey)
                {
                    return; // its key-down was not armed here (another key, or a key-down that happened elsewhere)
                }
                const auto action = self->_sidePanelLaunchArmedAction;
                self->_sidePanelLaunchArmedKey = VirtualKey::None;
                self->_sidePanelLaunchArmedAction = SidePanelLaunchKeyAction::None;
                e.Handled(true); // set BEFORE acting: a close or a launch removes the card synchronously
                try
                {
                    const auto bar = self->_sidePanelLaunchBar;
                    switch (action)
                    {
                    case SidePanelLaunchKeyAction::Close:
                        self->_SidePanelCloseLaunchCard(L"esc");
                        break;
                    case SidePanelLaunchKeyAction::Launch:
                        if (bar)
                        {
                            bar->ClickLaunch(); // exactly a click: hover timer stopped, open hover menu closed, Default launch
                        }
                        break;
                    case SidePanelLaunchKeyAction::Fork:
                        if (bar)
                        {
                            bar->ClickFork();
                        }
                        break;
                    default:
                        break;
                    }
                }
                catch (...)
                {
                    // The launch / close failed past its own containment; the card stays as the failure left it, and
                    // the throw is recorded (Rule #18) instead of escaping into XAML.
                    ::Agentmaster::AgentLogCaughtException(L"side panel launcher: key-up");
                }
            });
            // The card's size changed (the box re-fitted, Fork shown / hidden, Launch relabeled): re-place it so its
            // corner stays at the click. SizeChanged is a layout pass, and moving a parented Popup inside one is the
            // E_LAYOUTCYCLE fail-fast — so only queue here.
            card.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelQueueLaunchCardPlacement();
                }
            });

            // The Popup: Root()-parented across its three rows, top/left aligned, so HorizontalOffset / VerticalOffset
            // are Root-relative and the card can sit anywhere in the window (the tag editor's recipe). Not
            // light-dismiss: the outside-press hook below decides what closes it.
            winrt::Windows::UI::Xaml::Controls::Primitives::Popup popup{};
            popup.Child(card);
            popup.HorizontalAlignment(HorizontalAlignment::Left);
            popup.VerticalAlignment(VerticalAlignment::Top);
            Grid::SetRow(popup, 0);
            Grid::SetRowSpan(popup, 3);
            root.Children().Append(popup);

            if (!_sidePanelLaunchHooked)
            {
                _sidePanelLaunchHooked = true;
                // Outside-press close: ONE handledEventsToo hook on Root() (registered once, inert while the card is
                // closed). handledEventsToo, because the terminal, the tabs and the buttons mark their presses handled.
                // A press inside the card, its drop-down, on "+" or in another open popup is kept (see
                // _SidePanelLaunchPressIsInside); anything else closes the card. Popup open / close is not a tree
                // mutation, so closing inside a pointer handler is safe (the tag editor's note).
                root.AddHandler(
                    UIElement::PointerPressedEvent(),
                    winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                        const auto self = weakThis.get();
                        if (!self || !self->_SidePanelLaunchCardOpen())
                        {
                            return;
                        }
                        try
                        {
                            if (!self->_SidePanelLaunchPressIsInside(e.OriginalSource().try_as<DependencyObject>()))
                            {
                                self->_SidePanelCloseLaunchCard(L"outside press");
                            }
                        }
                        catch (...)
                        {
                            // The press goes on to its own target either way; only the close was lost (logged).
                            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: outside press");
                        }
                    } }),
                    true /* handledEventsToo */);
                // A window resize: re-fit the box and re-place the card (queued — SizeChanged is a layout pass), and
                // re-fit an open drop-down (the bar's own coalesced re-fit, the Agent Manager root's SizeChanged twin).
                root.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
                    const auto self = weakThis.get();
                    if (!self || !self->_SidePanelLaunchCardOpen())
                    {
                        return; // a closed card costs nothing: every open fits and places afresh
                    }
                    self->_SidePanelQueueLaunchCardPlacement();
                    if (self->_sidePanelLaunchBar)
                    {
                        self->_sidePanelLaunchBar->QueuePathPickerRelayout(); // never throws
                    }
                });
            }

            _sidePanelLaunchPopup = popup;
            _sidePanelLaunchCard = card;
            _sidePanelLaunchBar = std::move(bar);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] launcher card built (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
            return true;
        }
        catch (...)
        {
            // Nothing was kept (the members are set only at the end), so the next "+" click builds from scratch.
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: build");
            return false;
        }
    }

    void TerminalPage::_SidePanelOpenLaunchCard(bool fromPress)
    {
        if (!_sidePanelBuilt || _SidePanelLaunchCardOpen())
        {
            return;
        }
        if (!_SidePanelEnsureLaunchCard())
        {
            return; // the build failed (logged); the next "+" click tries again
        }
        try
        {
            // The anchor: the "+" press point; without a press (a Click from UI Automation), the button's bottom
            // center — the card then opens just below the "+", toward the window's center like any click.
            if (!fromPress && _sidePanelLaunchBtn)
            {
                const auto bottomCenter = winrt::Windows::Foundation::Point{ static_cast<float>(_sidePanelLaunchBtn.ActualWidth() / 2.0), static_cast<float>(_sidePanelLaunchBtn.ActualHeight()) };
                _sidePanelLaunchAnchor = _sidePanelLaunchBtn.TransformToVisual(Root()).TransformPoint(bottomCenter);
            }
            _sidePanelLaunchCardDockLeft = _appSettings.sidePanelOnLeft; // a broadcast that moves the dock closes the card
            _sidePanelLaunchArmedKey = VirtualKey::None;
            _sidePanelLaunchArmedAction = SidePanelLaunchKeyAction::None;
            // The box opens on the Agent Manager box's text — the user's "keep the last, same one we have in agent
            // manager tab". SetBoxText re-validates explicitly either way: the folder may be gone since, an id may have a
            // transcript now, and the closed card was off the live tree where TextChanged is unreliable.
            bool copied = false;
            if (const auto ipc = _agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    _sidePanelLaunchBar->SetBoxText(mgr->LaunchBoxText());
                    copied = true;
                }
            }
            if (!copied)
            {
                _sidePanelLaunchBar->ValidateLaunchBox(); // no Manager: keep the card's own text, re-judged now
            }
            _SidePanelFitLaunchBox();
            _sidePanelLaunchPopup.IsOpen(true);
            // Place it NOW, in this tick, at its real size (_SidePanelPlaceLaunchCard measures the card): a card placed
            // a tick later would show for a frame at the previous spot first.
            _SidePanelPlaceLaunchCard();
            ::Agentmaster::LogNav(fromPress ? L"sidepanel launcher open (at the click)" : L"sidepanel launcher open (under the + button)");
            // The rest one tick later: on the FIRST open the card was only just realized, and a same-tick Focus on a
            // TextBox that has not been laid out can do nothing (the tag editor's lesson). That tick lays the card out at
            // its place, focuses its box and opens the box's folder list right under it — the list shows whenever the
            // card does (the user's rule, 2026-10-19; _SidePanelFinishLaunchCardOpen has the steps and their order). The
            // text copied above is a programmatic write: its late TextChanged re-lists an open list and opens nothing.
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelFinishLaunchCardOpen(); // never throws; a no-op when the card closed before the tick ran
                }
            });
        }
        catch (...)
        {
            // A half-opened card must not stay on screen with no placement or focus: close it again (best effort).
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: open");
            try
            {
                if (_sidePanelLaunchPopup)
                {
                    _sidePanelLaunchPopup.IsOpen(false);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel launcher: open rollback");
            }
        }
    }

    void TerminalPage::_SidePanelFinishLaunchCardOpen()
    {
        if (!_SidePanelLaunchCardOpen() || !_sidePanelLaunchBar || !_sidePanelLaunchCard)
        {
            return; // closed again before the tick ran (Esc, "+" again, an outside press): nothing to finish
        }
        // 1. Lay the card out NOW, at the offsets _SidePanelPlaceLaunchCard wrote in the open's tick. A popup's child is
        //    placed at its offsets only in a layout pass (WinUI CPopupRoot::ArrangeOverride; the offset write only
        //    invalidates that arrange), so until one runs, the box still reads the card's previous place — (0,0) on a
        //    window's first "+" — and a list anchored there covered the box and Launch (the report of 2026-10-12).
        //    UpdateLayout runs that pass synchronously for the card's tree (its root visual, the popup root included —
        //    WinUI CUIElement::UpdateLayout → the tree's CLayoutManager): the work XAML would do before its next frame
        //    anyway, and a no-op when that frame's pass already ran (nothing is dirty then). Safe here: a fresh dispatcher tick, outside any layout pass (inside one, XAML skips the call —
        //    the layout manager's reentrancy guard), and the SizeChanged / LayoutUpdated handlers it raises only queue
        //    their work (the card's own re-placement, the bar's re-fit), never move a Popup inside the pass.
        try
        {
            _sidePanelLaunchCard.UpdateLayout();
        }
        catch (...)
        {
            // The list then anchors to the box's last layout, and the bar's re-anchor round moves it under the box one
            // frame later (the belt it arms at every open). Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: lay the card out before its folder list opens");
        }
        if (!_SidePanelLaunchCardOpen())
        {
            return; // a handler run by that pass closed the card (none does today): open nothing over a closed card
        }
        // 2. Focus the box PROGRAMMATICALLY: the caret waits in it, and Enter / Escape reach the card's key handlers. The
        //    bar reads this focus's real input mode from the box's GettingFocus — GotFocus would report the last input
        //    device's state instead (Pointer after the "+" click) — so the focus itself opens nothing; step 3 opens the
        //    list, once.
        try
        {
            if (const auto box = _sidePanelLaunchBar->CwdBox())
            {
                if (!box.Focus(FocusState::Programmatic))
                {
                    // Keys then still go where focus was (the terminal): an Esc meant for the list would reach that claude.
                    // Never seen; logged so a report of it can be traced (a click in the box focuses it).
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] launcher card: its box did not take focus (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: focus the box"); // a click in the box still focuses it
        }
        // 3. Open the box's folder list right under it — the user's rule (2026-10-19): "the path textbox's dropdown menu is
        //    supposed to be open by default when + the menu is shown". The bar anchors it to the box as step 1 laid it out
        //    and re-anchors once more after the next pass (a belt: the card re-places itself when its size settles, and a
        //    real move re-anchors the list through _SidePanelPlaceLaunchCard). Before, the list stayed closed until a click
        //    in the box or a keystroke (the user's answer of 2026-10-06). Opened even when step 2 failed: the rows work
        //    with the mouse, and a row click focuses the box.
        _sidePanelLaunchBar->OpenPathPicker(); // noexcept: a failure is logged inside, and a click in the box opens the list
    }

    void TerminalPage::_SidePanelCloseLaunchCard(std::wstring_view reason)
    {
        if (!_SidePanelLaunchCardOpen())
        {
            return;
        }
        _sidePanelLaunchArmedKey = VirtualKey::None; // a key held across the close must not act on its release elsewhere
        _sidePanelLaunchArmedAction = SidePanelLaunchKeyAction::None;
        // Did the card hold keyboard focus? A press on a focusable element (the terminal, a text box) moved focus there
        // before this hook ran — that focus stays. A press on a surface that takes no focus (a side-panel card, the
        // header), Esc, "+" again, or a launch leaves focus in the card, which is about to leave the tree: then focus
        // goes back to the current tab (the terminal the user was in before "+").
        bool hadFocus = false;
        try
        {
            if (const auto xr = Root().XamlRoot())
            {
                if (const auto focused = FocusManager::GetFocusedElement(xr).try_as<DependencyObject>())
                {
                    hadFocus = SidePanelLaunchIsUnder(focused, _sidePanelLaunchCard);
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: focus read at close"); // no restore: focus stays where XAML puts it
        }
        if (_sidePanelLaunchBar)
        {
            // The box's blur work, done now: the drop-down closed, the dismissal latched, the text normalized — so the
            // text mirrored below is the normalized one (the bar's own LostFocus tick would run after this).
            _sidePanelLaunchBar->DismissForHide(); // noexcept
        }
        _SidePanelSyncLaunchTextToManager(); // the Manager's box ends on the card's last text
        try
        {
            _sidePanelLaunchPopup.IsOpen(false);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: close");
        }
        ::Agentmaster::LogNav(L"sidepanel launcher close (" + std::wstring{ reason } + L")");
        if (hadFocus)
        {
            try
            {
                // Low priority, after the close settles: a launch has selected and focused its new tab by then, so the
                // "current tab" is the right one either way.
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis = get_weak()]() {
                    const auto self = weakThis.get();
                    if (!self || self->_SidePanelLaunchCardOpen())
                    {
                        return; // reopened meanwhile: its box takes focus again
                    }
                    try
                    {
                        self->_FocusCurrentTab(true);
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"side panel launcher: focus back to the tab"); // a click in the terminal focuses it
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel launcher: queue the focus restore"); // the window is closing
            }
        }
    }

    void TerminalPage::_SidePanelQueueLaunchCardPlacement()
    {
        if (_sidePanelLaunchPlaceQueued)
        {
            return; // a tick is already queued: this change folds into it
        }
        _sidePanelLaunchPlaceQueued = true;
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                self->_sidePanelLaunchPlaceQueued = false; // BEFORE the work: a size change during it queues another tick
                if (!self->_SidePanelLaunchCardOpen())
                {
                    return; // closed meanwhile: the next open fits and places afresh
                }
                self->_SidePanelFitLaunchBox(); // never throws
                self->_SidePanelPlaceLaunchCard(); // never throws
            });
        }
        catch (...)
        {
            // The dispatcher is going away with the window: re-arm, never latch shut (a stuck latch would freeze the
            // card's place for the rest of the window's life).
            _sidePanelLaunchPlaceQueued = false;
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: queue placement");
        }
    }

    void TerminalPage::_SidePanelPlaceLaunchCard()
    {
        if (!_sidePanelLaunchPopup || !_sidePanelLaunchCard)
        {
            return;
        }
        try
        {
            const auto root = Root();
            // The card's size from a fresh measure — what the Popup arranges it at (a Popup measures its child with no
            // limit) — rather than ActualWidth, which still holds the previous arrange right after the text, the box
            // width or Fork changed. Measuring outside a layout pass is legal; the next pass re-measures if needed.
            _sidePanelLaunchCard.Measure(winrt::Windows::Foundation::Size{ kSidePanelLaunchMeasureBound, kSidePanelLaunchMeasureBound });
            const auto size = _sidePanelLaunchCard.DesiredSize();
            const auto placed = ::Agentmaster::PlacePointerCard(static_cast<double>(_sidePanelLaunchAnchor.X),
                                                                static_cast<double>(_sidePanelLaunchAnchor.Y),
                                                                static_cast<double>(size.Width),
                                                                static_cast<double>(size.Height),
                                                                root ? root.ActualWidth() : 0.0,
                                                                root ? root.ActualHeight() : 0.0);
            // Written only on a real move, so a re-placement that changes nothing invalidates nothing.
            bool moved = false;
            if (std::abs(_sidePanelLaunchPopup.HorizontalOffset() - placed.left) > kSidePanelLaunchPlaceEpsilonPx)
            {
                _sidePanelLaunchPopup.HorizontalOffset(placed.left);
                moved = true;
            }
            if (std::abs(_sidePanelLaunchPopup.VerticalOffset() - placed.top) > kSidePanelLaunchPlaceEpsilonPx)
            {
                _sidePanelLaunchPopup.VerticalOffset(placed.top);
                moved = true;
            }
            // The card moved with its folder list open (a window resize clamped it; Fork appeared on a card that grows
            // leftward): the list must follow the box. Not now — the card's child moves to the new offsets only in the
            // next layout pass, so a read now would anchor the list to the card's old place — but after that pass
            // (noexcept; a no-op while the list is closed).
            if (moved && _sidePanelLaunchBar)
            {
                _sidePanelLaunchBar->ReanchorPathPickerAfterLayout();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: place"); // the card keeps its previous offsets
        }
    }

    void TerminalPage::_SidePanelFitLaunchBox()
    {
        if (!_sidePanelLaunchBar || !_sidePanelLaunchCard)
        {
            return;
        }
        try
        {
            const auto box = _sidePanelLaunchBar->CwdBox();
            if (!box)
            {
                return;
            }
            const auto root = Root();
            const double rootW = root ? root.ActualWidth() : 0.0;
            double width = kSidePanelLaunchBoxMaxPx; // an unmeasured window: the comfortable width, re-fitted on its first SizeChanged
            if (rootW > 0.0)
            {
                // The natural width of each other part of the row (0 when collapsed — Fork usually is), the
                // _ReflowLaunchBar measure.
                const auto natural = [](const FrameworkElement& el) -> double {
                    if (!el || el.Visibility() == Visibility::Collapsed)
                    {
                        return 0.0;
                    }
                    el.Measure(winrt::Windows::Foundation::Size{ kSidePanelLaunchMeasureBound, kSidePanelLaunchMeasureBound });
                    return static_cast<double>(el.DesiredSize().Width);
                };
                const FrameworkElement others[] = { _sidePanelLaunchBar->AgentElement(), _sidePanelLaunchBar->LaunchButton(), _sidePanelLaunchBar->ForkButton() };
                double othersW = 0.0;
                double gaps = 0.0; // one row gap per visible part beside the box
                for (const auto& el : others)
                {
                    const double w = natural(el);
                    if (w > 0.0)
                    {
                        othersW += w;
                        gaps += kSidePanelLaunchRowSpacingPx;
                    }
                }
                const auto pad = _sidePanelLaunchCard.Padding();
                const auto edge = _sidePanelLaunchCard.BorderThickness();
                const double chrome = pad.Left + pad.Right + edge.Left + edge.Right;
                // The room the window leaves the box: its width less the placement's two edge margins, the card's own
                // chrome, the row's gaps and the other parts.
                const double room = rootW - 2.0 * ::Agentmaster::kPointerCardMarginDips - chrome - gaps - othersW;
                width = std::clamp(room, kSidePanelLaunchBoxMinPx, kSidePanelLaunchBoxMaxPx);
            }
            if (std::abs(width - _sidePanelLaunchBoxWidth) < 0.5)
            {
                return; // unchanged: no layout invalidation
            }
            _sidePanelLaunchBoxWidth = width;
            // A FIXED width (Min = Max): the card does not grow or shrink while you type — its corner stays at the click.
            box.MinWidth(width);
            box.MaxWidth(width);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: fit the box"); // the box keeps its previous width
        }
    }

    void TerminalPage::_SidePanelSyncLaunchTextToManager()
    {
        if (!_sidePanelLaunchBar)
        {
            return;
        }
        try
        {
            if (const auto ipc = _agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    mgr->SetLaunchBoxText(_sidePanelLaunchBar->BoxText()); // writes only on a change; re-validates the Manager's box
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: mirror the text"); // the next edit or close mirrors again
        }
    }

    bool TerminalPage::_SidePanelLaunchPressIsInside(const DependencyObject& source)
    {
        if (!source)
        {
            return false;
        }
        try
        {
            // The identities a press may land under and still count as INSIDE: the card, the bar's drop-down panel (a
            // separate Root() popup, below the box), and the "+" button (its Click toggles the card closed — closing on
            // its press would let that Click reopen it at once).
            std::vector<void*> inside;
            inside.reserve(8);
            inside.push_back(SidePanelLaunchIdentity(_sidePanelLaunchCard));
            inside.push_back(SidePanelLaunchIdentity(_sidePanelLaunchBtn));
            if (_sidePanelLaunchBar)
            {
                inside.push_back(SidePanelLaunchIdentity(_sidePanelLaunchBar->PathPanel()));
            }
            // ...plus the content of every OTHER open popup: the "Specify a model" prompt that the Launch hover menu
            // opens is a Root() popup too (presses in a Root()-parented popup bubble to Root() like any element's), and a
            // click in it must not close the card whose launch it is about to finish. A flyout or a dialog renders in
            // the popup layer outside Root() and never reaches this hook at all.
            if (const auto xr = Root().XamlRoot())
            {
                const void* const own = SidePanelLaunchIdentity(_sidePanelLaunchPopup);
                for (const auto& popup : VisualTreeHelper::GetOpenPopupsForXamlRoot(xr))
                {
                    if (SidePanelLaunchIdentity(popup) != own)
                    {
                        if (const auto child = popup.Child())
                        {
                            inside.push_back(SidePanelLaunchIdentity(child));
                        }
                    }
                }
            }
            // Walk up from the press source. A TextElement (a Run in a TextBlock) is not a visual: start from its host.
            DependencyObject current = source;
            if (const auto textElement = current.try_as<winrt::Windows::UI::Xaml::Documents::TextElement>())
            {
                current = textElement.ElementStart().VisualParent();
            }
            for (int guard = 0; current && guard < 1024; ++guard)
            {
                const void* const id = SidePanelLaunchIdentity(current);
                if (id && std::find(inside.begin(), inside.end(), id) != inside.end())
                {
                    return true;
                }
                current = VisualTreeHelper::GetParent(current);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel launcher: press walk"); // an unreadable press counts as outside: the card closes
        }
        return false;
    }

    // ======================================================================================
    // The side panel's FILTERS (SIDE_PANEL.md §2l)
    // ======================================================================================
    // WHAT (the user's request of 2026-11-08): "introduce ability to filter, filter by folder and even have like the +, but a
    // button for FILTER and in it have menu appear on click and then By Folder -> <the picker of +>. The row below the filter
    // with N sessions across all window, shall be replaced by a row only shown when filter is on and in that row we show what
    // we filter based on. Add other filterables." Their picks the same day (design-ask with mockups): By Folder opens the "+"
    // card itself in a folder-picking mode; a folder keeps its subfolders unless the user unchecks Include subfolders; the
    // other filters are By State, By Tag, the four flags and Active within, and the menu must NOT close on a click; the row is
    // the count, a chip per filter and Clear.
    // THE PIECES:
    //   - the FUNNEL (_sidePanelFilterBtn): the header line's button between "+" and Sum; lit while a menu filter is on;
    //   - the PANEL (_sidePanelFilterPopup): a card that looks like a menu and stays open while you pick — a Root() popup with
    //     the "+" card's outside-press close and the KEY-UP Escape, because a MenuFlyout closes at every click and a
    //     light-dismiss Flyout eats the press that closes it and closes on Escape's key-down (its ESC would reach claude);
    //   - the FOLDER CARD (_sidePanelFolderPopup): the "+" card's own launch row, AgentLaunchBar in folder-pick mode;
    //   - the CHIPS ROW (_sidePanelFilterRow): in the count line's place, shown only while something narrows the list.
    // THE ONE RECORD (_sidePanelFilters, AgentMaster/SidePanelFilters.h's rules): every surface reads it, _SidePanelSetFilters
    // alone writes it; it is persisted per window (WindowRecord::sidePanelFilters) and never becomes a global default.
    // COST: the panel and the card are built on first use; the panel's rows are rebuilt at every open (about twenty); the
    // filters add one compiled matcher per list update and read tags / ★ only while a filter needs them (cached per epoch).

    void TerminalPage::_SidePanelSetFilters(::Agentmaster::SidePanelFilters next, std::wstring_view what) noexcept
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        try
        {
            // Normalized first, so "no change" is judged on the record as it would be stored (a re-pick of a folder in another
            // spelling, a tag in another case) and nothing un-normalized ever reaches the rebuild or the window record.
            next = ::Agentmaster::NormalizeSidePanelFilters(next);
            if (next == _sidePanelFilters)
            {
                _SidePanelUpdateFilterButton(); // a pick that changed nothing: keep the funnel honest anyway
                return;
            }
            _sidePanelFilters = std::move(next);
            // The nav line names the facet and the action and how many filters are on now — never a folder path or a tag
            // name (the nav trail keeps the user's words out, like the AI search box's length-only lines).
            const int facets = std::popcount(::Agentmaster::SidePanelActiveFacets(_sidePanelFilters, false));
            ::Agentmaster::LogNav(L"sidepanel filter " + std::wstring{ what } + L" -> " + std::to_wstring(facets) + (facets == 1 ? L" filter on" : L" filters on"));
            _ScheduleWindowRecordSave(); // per window only (no AppSettings default exists for the filters)
            _SidePanelUpdateFilterButton();
            // A user's action: past the min-gap throttle. The update re-filters the cards and redraws the chips row; in AI mode
            // it is owed until the cards come back (_SidePanelScheduleRebuild's rule).
            _SidePanelScheduleRebuild(/*force*/ true);
            _SidePanelRefreshFilterPanel(); // an open panel's checks and counts follow (noexcept)
        }
        catch (...)
        {
            // The record keeps what was assigned before the throw (normally all or nothing: the move is the last step that
            // can allocate); the next update or pick catches up. Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelSetFilters");
        }
    }

    void TerminalPage::_SidePanelUpdateFilterButton() noexcept
    {
        try
        {
            if (!_sidePanelFilterBtn)
            {
                return;
            }
            const bool on = ::Agentmaster::SidePanelFiltersActive(_sidePanelFilters);
            // A programmatic IsChecked raises no Click, so re-pinning can never loop back into the toggle or persist anything.
            _sidePanelFilterBtn.IsChecked(on);
            // In words for UI Automation (the accent is pixels only): how many filters narrow the list.
            const int facets = std::popcount(::Agentmaster::SidePanelActiveFacets(_sidePanelFilters, false));
            AgentSetItemStatus(_sidePanelFilterBtn, on ? (std::to_wstring(facets) + (facets == 1 ? L" filter on" : L" filters on")) : std::wstring{});
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelUpdateFilterButton"); // the accent may lag one change; the next one re-pins it
        }
    }

    void TerminalPage::_SidePanelSyncFactCaches() noexcept
    {
        // Each epoch is loaded BEFORE any read of its cache (the SessionStore.h contract): a write landing during the reads
        // that follow leaves the older epoch stored, so the next sync drops the cache again — refreshed once too often,
        // never kept stale. Every call here is noexcept (atomic loads, clear()).
        const uint64_t tagsEpoch = ::Agentmaster::SessionTagsEpoch();
        if (tagsEpoch != _sidePanelTagsEpoch)
        {
            _sidePanelTagsById.clear();
            _sidePanelTagColors.clear();
            _sidePanelTagColorsLoaded = false;
            _sidePanelTagsEpoch = tagsEpoch;
        }
        const uint64_t factsEpoch = ::Agentmaster::SessionFactsEpoch();
        if (factsEpoch != _sidePanelFavoritesEpoch)
        {
            _sidePanelFavoriteById.clear();
            _sidePanelFavoritesEpoch = factsEpoch;
        }
    }

    const std::vector<std::wstring>& TerminalPage::_SidePanelTagsOf(const std::wstring& id)
    {
        auto it = _sidePanelTagsById.find(id);
        if (it == _sidePanelTagsById.end())
        {
            // One session-store read (a stat when its record is cached and unchanged). The entry stays until the epoch moves
            // or a list update prunes it (the listed rows, the sessions By Tag judged, everything while the panel is open).
            it = _sidePanelTagsById.emplace(id, ::Agentmaster::GetSessionTags(id)).first;
        }
        return it->second;
    }

    bool TerminalPage::_SidePanelFavoriteOf(const std::wstring& id) noexcept
    {
        try
        {
            if (const auto it = _sidePanelFavoriteById.find(id); it != _sidePanelFavoriteById.end())
            {
                return it->second;
            }
            const bool favorite = ::Agentmaster::IsSessionFavorite(id); // one session-store read
            // Bounded: the map only grows between two moves of the facts epoch, by the sessions the filters judged; a fleet
            // past this many sessions re-reads from empty instead of growing without end.
            if (_sidePanelFavoriteById.size() >= 4096)
            {
                _sidePanelFavoriteById.clear();
            }
            _sidePanelFavoriteById.emplace(id, favorite);
            return favorite;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelFavoriteOf"); // reads as no ★: the Favorite flag hides it this update
            return false;
        }
    }

    ::Agentmaster::SidePanelFilterFacts TerminalPage::_SidePanelFilterFactsOf(const ::Agentmaster::SessionInfo& s, bool needTags, bool needFavorite, const std::vector<std::wstring>& unreadSorted)
    {
        ::Agentmaster::SidePanelFilterFacts f;
        // The folder keys only while By Folder has a pick: a NormDirKey per session per update is cheap, but not free.
        if (!_sidePanelFilters.folders.empty())
        {
            f.launchKey = s.workingDir.empty() ? std::wstring{} : ::Agentmaster::NormDirKey(s.workingDir);
            f.inferredKey = s.inferredWorkingDir.empty() ? std::wstring{} : ::Agentmaster::NormDirKey(s.inferredWorkingDir);
        }
        f.state = s.state;
        f.tags = needTags ? &_SidePanelTagsOf(s.id) : nullptr; // valid until the cache drops the entry (after the caller's loop)
        f.draft = !s.pendingInput.empty(); // the lean snapshot keeps the draft (the cards' 3 dots)
        f.unread = std::binary_search(unreadSorted.begin(), unreadSorted.end(), s.id); // the census the rings show
        f.pinned = ::Agentmaster::IsSidePanelPinStamp(s.sidePanelPinnedAtUnixMs);
        f.favorite = needFavorite && _SidePanelFavoriteOf(s.id);
        // The card's "active … ago" source (SidePanelCardModelFor): the transcript's last activity, else the hook anchor.
        f.lastActivityMs = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
        return f;
    }

    AgentSidePanelFilterCensus TerminalPage::_SidePanelFilterCensus()
    {
        using ::Agentmaster::SidePanelFacet;
        AgentSidePanelFilterCensus census;
        if (!_sessionRegistry)
        {
            return census;
        }
        _SidePanelSyncFactCaches();
        const auto mode = _appSettings.tabColorMode;
        const auto snap = _sessionRegistry->SnapshotLiveForCards();
        const auto terms = ::Agentmaster::ParseSessionQuery(_sidePanelSearchText);
        const bool local = !_sidePanelGlobal;
        std::unordered_set<std::wstring> hosted;
        if (local)
        {
            hosted = _HostedSessionIds();
        }
        const auto unread = ::Agentmaster::CurrentUnreadSessionIds();
        const int64_t now = NowMs();
        const ::Agentmaster::SidePanelFilterMatcher matcher{ _sidePanelFilters, now };
        // folded tag -> (display spelling, sessions): the first spelling seen is shown (every carrier folds to one tag).
        std::map<std::wstring, std::pair<std::wstring, size_t>> tagCounts;
        for (const auto& s : snap)
        {
            if (!s.live || (local && hosted.find(s.id) == hosted.end()))
            {
                continue;
            }
            ++census.inScope;
            if (!SidePanelSessionMatches(s, terms, mode))
            {
                continue; // the typed text hides it: no facet counts it
            }
            // Every fact, for every session: each facet's counts need the facts the OTHER facets judge.
            const auto facts = _SidePanelFilterFactsOf(s, /*needTags*/ true, /*needFavorite*/ true, unread);
            const uint32_t passed = matcher.Passed(facts, /*textOk*/ true);
            if (passed == ::Agentmaster::kSidePanelAllFacets)
            {
                ++census.listed;
            }
            if (::Agentmaster::SidePanelPassesAllBut(passed, SidePanelFacet::State))
            {
                for (size_t i = 0; i < std::size(::Agentmaster::kSidePanelStateChoices); ++i)
                {
                    if (::Agentmaster::kSidePanelStateChoices[i].state == facts.state)
                    {
                        ++census.states[i];
                    }
                }
            }
            if (::Agentmaster::SidePanelPassesAllBut(passed, SidePanelFacet::Tag))
            {
                if (!facts.tags || facts.tags->empty())
                {
                    ++census.untagged;
                }
                else
                {
                    for (const auto& tag : *facts.tags)
                    {
                        auto& entry = tagCounts[::Agentmaster::FoldTagName(tag)];
                        if (entry.first.empty())
                        {
                            entry.first = tag;
                        }
                        ++entry.second;
                    }
                }
            }
            census.draft += (facts.draft && ::Agentmaster::SidePanelPassesAllBut(passed, SidePanelFacet::Draft)) ? 1u : 0u;
            census.unread += (facts.unread && ::Agentmaster::SidePanelPassesAllBut(passed, SidePanelFacet::Unread)) ? 1u : 0u;
            census.pinned += (facts.pinned && ::Agentmaster::SidePanelPassesAllBut(passed, SidePanelFacet::Pinned)) ? 1u : 0u;
            census.favorite += (facts.favorite && ::Agentmaster::SidePanelPassesAllBut(passed, SidePanelFacet::Favorite)) ? 1u : 0u;
            if (::Agentmaster::SidePanelPassesAllBut(passed, SidePanelFacet::Active))
            {
                ++census.active[0]; // "Any time"
                for (size_t i = 0; i < std::size(::Agentmaster::kSidePanelActiveChoices); ++i)
                {
                    if (::Agentmaster::SidePanelActiveWithin(facts.lastActivityMs, now, ::Agentmaster::kSidePanelActiveChoices[i].minutes))
                    {
                        ++census.active[i + 1];
                    }
                }
            }
        }
        // A picked tag no counted session carries any more still gets its row (count 0), so it can be unchecked.
        for (const auto& tag : _sidePanelFilters.tags)
        {
            auto& entry = tagCounts[::Agentmaster::FoldTagName(tag)];
            if (entry.first.empty())
            {
                entry.first = tag;
            }
        }
        census.tags.reserve(tagCounts.size());
        for (auto& [folded, entry] : tagCounts)
        {
            census.tags.emplace_back(std::move(entry.first), entry.second);
        }
        // Most carried first; the folded name breaks ties (the map's order, kept by the stable sort), so the rows never shuffle.
        std::stable_sort(census.tags.begin(), census.tags.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return census;
    }

    bool TerminalPage::_SidePanelFilterPanelOpen() const noexcept
    {
        try
        {
            return _sidePanelFilterPopup && _sidePanelFilterPopup.IsOpen();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: IsOpen read"); // a torn-down popup reads as closed
            return false;
        }
    }

    bool TerminalPage::_SidePanelFolderCardOpen() const noexcept
    {
        try
        {
            return _sidePanelFolderPopup && _sidePanelFolderPopup.IsOpen();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: IsOpen read"); // a torn-down popup reads as closed
            return false;
        }
    }

    void TerminalPage::_SidePanelToggleFilterPanel()
    {
        // Consume the press FIRST (the "+" button's rule): one press belongs to one Click, so a later Click without a press of
        // its own (UI Automation) anchors under the funnel, never at an old point.
        const bool fromPress = _sidePanelFilterAnchorFromPress;
        _sidePanelFilterAnchorFromPress = false;
        if (_SidePanelFilterPanelOpen())
        {
            // The funnel is INSIDE for the outside-press close (its Click toggles), so a click on it with the panel open lands
            // here: the panel closes, like a second click on any button that opened a pane.
            _SidePanelCloseFilterPanel(L"funnel again");
            return;
        }
        _SidePanelOpenFilterPanel(fromPress);
    }

    bool TerminalPage::_SidePanelEnsureFilterPanel() noexcept
    {
        if (_sidePanelFilterPopup && _sidePanelFilterCard && _sidePanelFilterRowsHost)
        {
            return true;
        }
        try
        {
            const auto root = Root();
            if (!root)
            {
                return false; // no page root to parent into (never after the first layout)
            }
            const auto weakThis = get_weak();

            // The card: a menu's dark look (every Agentmaster surface is dark, whatever the app theme), 7 DIP corners. Tab
            // cycles inside it, and the arrow keys move between its rows (XY navigation on the rows host below).
            Border card{};
            card.RequestedTheme(ElementTheme::Dark);
            card.Background(Fill(0xFF, 0x2B, 0x2B, 0x2B));
            card.BorderBrush(Fill(0xFF, 0x47, 0x47, 0x47));
            card.BorderThickness(ThicknessHelper::FromUniformLength(1));
            card.CornerRadius(CornerRadiusHelper::FromUniformRadius(7));
            card.Padding(ThicknessHelper::FromLengths(0, 4, 0, 4));
            card.TabFocusNavigation(KeyboardNavigationMode::Cycle);
            // The rows are Buttons: the Agent Manager's six Button state brushes, or under the forced-Dark theme their hover and
            // press fills resolve to null (a row would take a click only on its text and never light up under the pointer).
            AgentLaunchBar::SeedButtonStateBrushes(card.Resources());
            StackPanel rows{};
            rows.Width(kSidePanelFilterPanelWidth);
            rows.XYFocusKeyboardNavigation(XYFocusKeyboardNavigationMode::Enabled); // Up / Down walk the rows
            ScrollViewer scroll{};
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroll.IsTabStop(false); // the scroller itself never holds the keyboard: the rows do
            scroll.Content(rows);
            card.Child(scroll);

            // Keys, acted on at the KEY-UP (the "+" card's rule): closing the panel moves focus to a terminal, and Windows still
            // delivers the rest of the keystroke (the ESC of an Escape, the CR of an Enter) to whatever holds focus next — a
            // claude would interrupt its turn or submit its draft. PreviewKeyDown TUNNELS, so it runs before the focused row:
            //   - Escape: closes the panel (on its key-up);
            //   - Enter / Space on "By Folder…": opens the folder card (on its key-up — focus moves to the card's box);
            //   - Enter / Space on any other row: the row's own Button handles it (a pick toggles in place, focus stays on the
            //     row, so its character goes nowhere);
            //   - never a Ctrl / Alt chord (Alt+Enter is Terminal's full-screen toggle).
            card.PreviewKeyDown([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    self->_sidePanelFilterArmedKey = VirtualKey::None; // any key-down disarms: the next key-up must be its own
                    self->_sidePanelFilterArmedByFolder = false;
                    const auto key = e.Key();
                    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
                    {
                        return; // an app chord: not ours
                    }
                    if (key == VirtualKey::Escape)
                    {
                        self->_sidePanelFilterArmedKey = key;
                        e.Handled(true);
                        return;
                    }
                    if (key == VirtualKey::Enter || key == VirtualKey::Space)
                    {
                        const void* const source = SidePanelLaunchIdentity(e.OriginalSource());
                        for (const auto& row : self->_sidePanelFilterRows)
                        {
                            if (row.kind == AgentSidePanelFilterRowView::Kind::ByFolder && source && source == SidePanelLaunchIdentity(row.button))
                            {
                                self->_sidePanelFilterArmedKey = key;
                                self->_sidePanelFilterArmedByFolder = true;
                                e.Handled(true); // the Button must not click on this key-down
                                return;
                            }
                        }
                    }
                    // Right on By Folder… opens its submenu with Pick… focused (SIDE_PANEL.md §2l "The By Folder submenu"), the
                    // desktop menu's key. On the key-down: focus moves into the submenu, a popup of this panel, and Right
                    // writes no character anywhere.
                    if (key == VirtualKey::Right)
                    {
                        const void* const source = SidePanelLaunchIdentity(e.OriginalSource());
                        if (source && source == SidePanelLaunchIdentity(SidePanelByFolderButton(self->_sidePanelFilterRows)))
                        {
                            e.Handled(true); // no XY move: the submenu takes the keyboard
                            self->_SidePanelOpenFolderMenu(/*focusFirst*/ true);
                            return;
                        }
                    }
                }
                catch (...)
                {
                    // The key then does what the focused row does with it. Recorded (Rule #18).
                    ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: key-down");
                }
            });
            card.PreviewKeyUp([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || self->_sidePanelFilterArmedKey == VirtualKey::None || e.Key() != self->_sidePanelFilterArmedKey)
                {
                    return; // its key-down was not armed here
                }
                const bool byFolder = self->_sidePanelFilterArmedByFolder;
                self->_sidePanelFilterArmedKey = VirtualKey::None;
                self->_sidePanelFilterArmedByFolder = false;
                e.Handled(true); // set BEFORE acting: the close takes the card out of the tree synchronously
                try
                {
                    if (byFolder)
                    {
                        for (size_t i = 0; i < self->_sidePanelFilterRows.size(); ++i)
                        {
                            if (self->_sidePanelFilterRows[i].kind == AgentSidePanelFilterRowView::Kind::ByFolder)
                            {
                                self->_SidePanelOnFilterRowClick(i);
                                break;
                            }
                        }
                    }
                    else if (self->_SidePanelFolderMenuOpen())
                    {
                        // A submenu open beside the panel (by hover — the keyboard is still here): Escape closes the innermost
                        // level first, the desktop menu's rule; the next Escape closes the panel.
                        self->_SidePanelCloseFolderMenu(L"esc in the panel", /*focusByFolder*/ false);
                    }
                    else
                    {
                        self->_SidePanelCloseFilterPanel(L"esc");
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: key-up");
                }
            });
            // A section expanded or folded changes the card's height: re-place it (queued — SizeChanged is a layout pass, and
            // moving a parented Popup inside one is the E_LAYOUTCYCLE fail-fast).
            card.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelQueueFilterSurfacesPlacement();
                }
            });

            // The Popup: Root()-parented across its three rows, top/left aligned, so its offsets are Root-relative (the tag
            // editor's and the "+" card's recipe). Not light-dismiss: the outside-press hook below decides what closes it.
            winrt::Windows::UI::Xaml::Controls::Primitives::Popup popup{};
            popup.Child(card);
            popup.HorizontalAlignment(HorizontalAlignment::Left);
            popup.VerticalAlignment(VerticalAlignment::Top);
            Grid::SetRow(popup, 0);
            Grid::SetRowSpan(popup, 3);
            root.Children().Append(popup);
            // No id on the card: a Border has no automation peer, so the panel is found by its rows (am.side.filter.byFolder…).

            if (!_sidePanelFilterHooked)
            {
                _sidePanelFilterHooked = true;
                // The outside-press close of BOTH filter surfaces: ONE handledEventsToo hook on Root() (inert while both are
                // closed), the "+" card's recipe. A press inside the surface (or on the funnel, whose Click toggles the panel;
                // or in the folder card's own drop-down; or in another open popup — a dialog) keeps it; anything else closes
                // it. Popup open / close is no tree mutation, so closing inside a pointer handler is safe.
                root.AddHandler(
                    UIElement::PointerPressedEvent(),
                    winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                        const auto self = weakThis.get();
                        if (!self)
                        {
                            return;
                        }
                        const bool panelOpen = self->_SidePanelFilterPanelOpen();
                        const bool cardOpen = self->_SidePanelFolderCardOpen();
                        if (!panelOpen && !cardOpen)
                        {
                            return;
                        }
                        try
                        {
                            const auto source = e.OriginalSource().try_as<DependencyObject>();
                            const auto xr = self->Root().XamlRoot();
                            // By Folder's submenu (SIDE_PANEL.md §2l "The By Folder submenu") is one of the surfaces' OWN popups, and
                            // a press inside it is inside the panel: a click on a folder line must keep both open.
                            const std::vector<void*> own{ SidePanelLaunchIdentity(self->_sidePanelFilterPopup),
                                                          SidePanelLaunchIdentity(self->_sidePanelFolderPopup),
                                                          SidePanelLaunchIdentity(self->_sidePanelFolderMenuPopup) };
                            if (panelOpen && !SidePanelPressInside(source,
                                                                   { SidePanelLaunchIdentity(self->_sidePanelFilterCard),
                                                                     SidePanelLaunchIdentity(self->_sidePanelFilterBtn),
                                                                     SidePanelLaunchIdentity(self->_sidePanelFolderMenuCard) },
                                                                   xr,
                                                                   own))
                            {
                                self->_SidePanelCloseFilterPanel(L"outside press"); // the submenu closes with it
                            }
                            if (cardOpen)
                            {
                                std::vector<void*> inside{ SidePanelLaunchIdentity(self->_sidePanelFolderCard) };
                                if (self->_sidePanelFolderBar)
                                {
                                    inside.push_back(SidePanelLaunchIdentity(self->_sidePanelFolderBar->PathPanel())); // its folder list (a separate Root() popup)
                                }
                                if (!SidePanelPressInside(source, std::move(inside), xr, own))
                                {
                                    self->_SidePanelCloseFolderCard(L"outside press");
                                }
                            }
                        }
                        catch (...)
                        {
                            // The press goes on to its own target either way; only the close was lost (logged).
                            ::Agentmaster::AgentLogCaughtException(L"side panel filter surfaces: outside press");
                        }
                    } }),
                    true /* handledEventsToo */);
                // A window resize: re-place both (queued — SizeChanged is a layout pass) and re-fit the folder card's open list.
                root.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
                    const auto self = weakThis.get();
                    if (!self || (!self->_SidePanelFilterPanelOpen() && !self->_SidePanelFolderCardOpen()))
                    {
                        return; // closed surfaces cost nothing: every open places afresh
                    }
                    self->_SidePanelQueueFilterSurfacesPlacement();
                    if (self->_sidePanelFolderBar)
                    {
                        self->_sidePanelFolderBar->QueuePathPickerRelayout(); // never throws
                    }
                });
            }

            _sidePanelFilterPopup = popup;
            _sidePanelFilterCard = card;
            _sidePanelFilterRowsHost = rows;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] filter panel built (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
            return true;
        }
        catch (...)
        {
            // Nothing was kept (the members are set only at the end), so the next funnel click builds from scratch.
            ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: build");
            return false;
        }
    }

    void TerminalPage::_SidePanelOpenFilterPanel(bool fromPress)
    {
        if (!_sidePanelBuilt || _SidePanelFilterPanelOpen())
        {
            return;
        }
        if (!_SidePanelEnsureFilterPanel())
        {
            return; // the build failed (logged); the next click tries again
        }
        try
        {
            _SidePanelCloseFolderCard(L"filter panel opened"); // belt: the press on the funnel closed it already as an outside press
            // The anchor: the funnel's press point; without a press (a Click from UI Automation), the funnel's bottom center.
            if (!fromPress && _sidePanelFilterBtn)
            {
                const auto bottomCenter = winrt::Windows::Foundation::Point{ static_cast<float>(_sidePanelFilterBtn.ActualWidth() / 2.0), static_cast<float>(_sidePanelFilterBtn.ActualHeight()) };
                _sidePanelFilterAnchor = _sidePanelFilterBtn.TransformToVisual(Root()).TransformPoint(bottomCenter);
            }
            _sidePanelFilterDockLeft = _appSettings.sidePanelOnLeft; // a broadcast that moves the dock closes the panel
            _sidePanelFilterArmedKey = VirtualKey::None;
            _sidePanelFilterArmedByFolder = false;
            _SidePanelFillFilterPanel();
            _sidePanelFilterPopup.IsOpen(true);
            // Placed NOW, in this tick, at its real size (a panel placed a tick later would show for a frame at the previous spot).
            _SidePanelPlaceFilterSurfaces();
            ::Agentmaster::LogNav(fromPress ? L"sidepanel filter panel open (at the click)" : L"sidepanel filter panel open (under the funnel)");
            // The first row takes the keyboard one tick later (the card was only just realized; a same-tick Focus on an element
            // not laid out yet can do nothing — the tag editor's lesson): Escape then reaches the panel, not a terminal.
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                const auto self = weakThis.get();
                if (!self || !self->_SidePanelFilterPanelOpen() || self->_sidePanelFilterRows.empty())
                {
                    return; // closed before the tick ran
                }
                try
                {
                    if (const auto first = self->_sidePanelFilterRows.front().button)
                    {
                        first.Focus(FocusState::Programmatic);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: focus the first row"); // a click on a row works anyway
                }
            });
        }
        catch (...)
        {
            // A half-opened panel must not stay on screen with no rows or placement: close it again (best effort).
            ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: open");
            try
            {
                if (_sidePanelFilterPopup)
                {
                    _sidePanelFilterPopup.IsOpen(false);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: open rollback");
            }
        }
    }

    void TerminalPage::_SidePanelCloseFilterPanel(std::wstring_view reason)
    {
        if (!_SidePanelFilterPanelOpen())
        {
            // Belt: By Folder's submenu never outlives its panel (a no-op when it is closed; it also drops a pending hover).
            _SidePanelCloseFolderMenu(reason, /*focusByFolder*/ false);
            return;
        }
        _sidePanelFilterArmedKey = VirtualKey::None; // a key held across the close must not act on its release elsewhere
        _sidePanelFilterArmedByFolder = false;
        // Did the panel — or By Folder's submenu beside it — hold the keyboard? (A press on a focusable element outside both —
        // the terminal, a text box — moved focus there before this ran, and that focus stays.) Read BEFORE the submenu closes:
        // its close moves the focus it held.
        bool hadFocus = false;
        try
        {
            if (const auto xr = Root().XamlRoot())
            {
                if (const auto focused = FocusManager::GetFocusedElement(xr).try_as<DependencyObject>())
                {
                    hadFocus = SidePanelLaunchIsUnder(focused, _sidePanelFilterCard) || (_sidePanelFolderMenuCard && SidePanelLaunchIsUnder(focused, _sidePanelFolderMenuCard));
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: focus read at close"); // no restore: focus stays where XAML puts it
        }
        // The submenu first: it stands beside the panel and must never show without it. No focus move of its own — the
        // panel gives the keyboard back below (hadFocus counts the submenu).
        _SidePanelCloseFolderMenu(reason, /*focusByFolder*/ false);
        try
        {
            _sidePanelFilterPopup.IsOpen(false);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: close");
        }
        ::Agentmaster::LogNav(L"sidepanel filter panel close (" + std::wstring{ reason } + L")");
        if (hadFocus)
        {
            try
            {
                // Low priority, after the close settles. The folder card (By Folder…) focuses its own box on a Normal tick, so
                // by then it is open and this gives nothing back; otherwise the terminal the user was in gets the keyboard.
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis = get_weak()]() {
                    const auto self = weakThis.get();
                    if (!self || self->_SidePanelFilterPanelOpen() || self->_SidePanelFolderCardOpen())
                    {
                        return; // reopened, or the folder card took over: it holds the keyboard
                    }
                    try
                    {
                        self->_FocusCurrentTab(true);
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: focus back to the tab"); // a click in the terminal focuses it
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: queue the focus restore"); // the window is closing
            }
        }
    }

    void TerminalPage::_SidePanelFillFilterPanel()
    {
        using Kind = AgentSidePanelFilterRowView::Kind;
        using ::Agentmaster::SidePanelFacet;
        if (!_sidePanelFilterRowsHost)
        {
            return;
        }
        const auto census = _SidePanelFilterCensus();
        // The tags' colors for the tag rows' ribbons: the cards' cache (one read per tags epoch), loaded here when no listed
        // card needed it yet.
        if (!_sidePanelTagColorsLoaded && !census.tags.empty())
        {
            _sidePanelTagColors = ::Agentmaster::LoadAllTagColors();
            _sidePanelTagColorsLoaded = true;
        }
        _sidePanelFilterRowsHost.Children().Clear();
        _sidePanelFilterRows.clear();
        const auto weakThis = get_weak();

        // ONE row: a transparent full-width Button — [check][icon][text *][count] — the panel's line. The handler captures the
        // weak page and the row's INDEX, never the row's elements (an element must not own itself through its handler).
        //   view    - what the row stands for (its check and count are made here).
        //   icon    - the icon column's element (null = none): a FrameworkElement, so it can take its column and alignment.
        //   text    - the row's words.
        //   indent  - a section's row: indented under its header.
        //   tip     - the row's tooltip ("" = none).
        //   autoId  - its AutomationId.
        const auto addRow = [&](AgentSidePanelFilterRowView view, const FrameworkElement& icon, const std::wstring& text, bool indent, const std::wstring& tip, const std::wstring& autoId) {
            Grid line{};
            line.Margin(ThicknessHelper::FromLengths(indent ? kSidePanelFilterIndentPx : 0.0, 0, 0, 0));
            ColumnDefinition cCheck{};
            cCheck.Width(GridLengthHelper::FromPixels(16));
            ColumnDefinition cIcon{};
            cIcon.Width(GridLengthHelper::FromPixels(20));
            ColumnDefinition cText{};
            cText.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition cCount{};
            cCount.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            // The submenu chevron's column (By Folder… alone fills it: SIDE_PANEL.md §2l "The By Folder submenu"); an empty Auto
            // column takes no width on the other rows.
            ColumnDefinition cChevron{};
            cChevron.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            line.ColumnDefinitions().Append(cCheck);
            line.ColumnDefinitions().Append(cIcon);
            line.ColumnDefinitions().Append(cText);
            line.ColumnDefinitions().Append(cCount);
            line.ColumnDefinitions().Append(cChevron);
            if (view.kind != Kind::ByFolder && view.kind != Kind::Clear)
            {
                view.check = Text(L"", 12, false, 0.9);
                view.check.VerticalAlignment(VerticalAlignment::Center);
                Grid::SetColumn(view.check, 0);
                line.Children().Append(view.check);
            }
            if (icon)
            {
                icon.VerticalAlignment(VerticalAlignment::Center);
                icon.HorizontalAlignment(HorizontalAlignment::Left);
                Grid::SetColumn(icon, 1);
                line.Children().Append(icon);
            }
            auto label = Text(winrt::hstring{ text }, 12.5, false, 0.95);
            label.TextTrimming(TextTrimming::CharacterEllipsis);
            label.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(label, 2);
            line.Children().Append(label);
            if (view.kind != Kind::Clear)
            {
                view.count = Text(L"", 11.5, false, 0.55);
                view.count.VerticalAlignment(VerticalAlignment::Center);
                view.count.Margin(ThicknessHelper::FromLengths(8, 0, 0, 0));
                Grid::SetColumn(view.count, 3);
                line.Children().Append(view.count);
            }
            if (view.kind == Kind::ByFolder)
            {
                // "›" at the row's end: the desktop menu's sign of a submenu — the folders picked before, on hover or Right.
                auto chevron = Text(L"\x203A", 13, false, 0.7);
                chevron.Margin(ThicknessHelper::FromLengths(6, 0, 0, 0));
                Grid::SetColumn(chevron, 4);
                line.Children().Append(chevron);
            }
            Button btn{};
            btn.Content(line);
            btn.HorizontalAlignment(HorizontalAlignment::Stretch);
            btn.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            btn.Background(SolidColorBrush{ Colors::Transparent() }); // hit-testable everywhere; hover / press take the seeded brushes
            btn.BorderThickness(ThicknessHelper::FromUniformLength(0));
            btn.CornerRadius(CornerRadiusHelper::FromUniformRadius(0)); // flat rows (a rounded Button is a composition node each)
            btn.Padding(ThicknessHelper::FromLengths(10, 4, 10, 4));
            if (!tip.empty())
            {
                AgentSetTip(btn, winrt::hstring{ tip }, kSidePanelButtonTipDelay); // opens after 3 s of rest: the rows are crossed on the way to a pick
            }
            AgentSetAutomationId(btn, autoId);
            AgentSetAccessibleName(btn, text);
            const size_t index = _sidePanelFilterRows.size();
            btn.Click([weakThis, index](const IInspectable&, const RoutedEventArgs&) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    // On a clean tick: a section's expand rebuilds the rows, and the clicked Button must not be taken out of
                    // the tree inside its own Click.
                    self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, index]() {
                        if (const auto page = weakThis.get())
                        {
                            page->_SidePanelOnFilterRowClick(index);
                        }
                    });
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel filter panel: queue a row click"); // the click is lost
                }
            });
            // By Folder's hover SUBMENU (SIDE_PANEL.md §2l "The By Folder submenu"): every row tells the hover intent where the
            // pointer rests — on By Folder… the submenu opens after the system's menu delay, on any other row an open one
            // closes after it. handledEventsToo: a Button may mark its pointer events handled for its own visual states. The
            // handlers capture the weak page and whether the row is By Folder…, never the row's elements. Touch is left out:
            // a finger has no hover (its contact "enters" the row, and a slow tap would flash the submenu before the click).
            const bool isByFolder = view.kind == Kind::ByFolder;
            btn.AddHandler(
                UIElement::PointerEnteredEvent(),
                winrt::box_value(PointerEventHandler{ [weakThis, isByFolder](const IInspectable&, const PointerRoutedEventArgs& e) {
                    const auto self = weakThis.get();
                    if (!self || e.Pointer().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Touch)
                    {
                        return;
                    }
                    self->_SidePanelFolderMenuHover(isByFolder ? SidePanelFolderMenuHover::EnterByFolder : SidePanelFolderMenuHover::EnterOtherRow);
                } }),
                true /* handledEventsToo */);
            if (isByFolder)
            {
                // Leaving By Folder… before the delay drops a pending open (an open submenu stays: the pointer may be on its
                // way into it).
                btn.AddHandler(
                    UIElement::PointerExitedEvent(),
                    winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                        const auto self = weakThis.get();
                        if (!self || e.Pointer().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Touch)
                        {
                            return;
                        }
                        self->_SidePanelFolderMenuHover(SidePanelFolderMenuHover::LeaveByFolder);
                    } }),
                    true /* handledEventsToo */);
            }
            view.button = btn;
            _sidePanelFilterRowsHost.Children().Append(btn);
            _sidePanelFilterRows.push_back(std::move(view));
        };
        const auto separator = [&]() {
            Border rule{};
            rule.Height(1);
            rule.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
            rule.Background(Fill(0xFF, 0x45, 0x45, 0x45));
            _sidePanelFilterRowsHost.Children().Append(rule);
        };
        const auto dot = [](const Color& color) {
            auto t = Text(L"\x25CF", 11, false, 1.0); // ●
            t.Foreground(SolidColorBrush{ color });
            return t;
        };

        // 1. By Folder… — the user's "By Folder -> <the picker of +>": the folder card opens where the panel was.
        {
            AgentSidePanelFilterRowView view;
            view.kind = Kind::ByFolder;
            addRow(std::move(view), SidePanelFluentGlyph(kSidePanelFolderGlyph, 12, 0.9), L"By Folder\x2026", false,
                   L"Pick a folder in the + launcher's folder picker: the cards then list the sessions that work in it (and, unless you uncheck Include subfolders, in every folder under it). Pick again to add more folders: a session in any of them is listed. Rest the pointer here, or press \x2192, for the folders you picked before: a click there turns one on or off.",
                   L"am.side.filter.byFolder");
        }
        // 2. By State ▸ — the six states, any of them (OR).
        {
            AgentSidePanelFilterRowView view;
            view.kind = Kind::Section;
            view.facet = SidePanelFacet::State;
            addRow(std::move(view), dot(StateColor(::Agentmaster::SessionState::Running)), L"By State", false,
                   L"Show the states to pick from. Picks add up: Running and Waiting for you lists the sessions in either state.",
                   L"am.side.filter.section.state");
            if (SidePanelHasFacet(_sidePanelFilterExpanded, SidePanelFacet::State))
            {
                for (const auto& choice : ::Agentmaster::kSidePanelStateChoices)
                {
                    AgentSidePanelFilterRowView row;
                    row.kind = Kind::State;
                    row.stateBit = choice.bit;
                    addRow(std::move(row), dot(StateColor(choice.state)), choice.label, true, L"", std::wstring{ L"am.side.filter.state." } + choice.token);
                }
            }
        }
        // 3. By Tag ▸ — every tag of the counted sessions (most carried first), then Untagged.
        {
            AgentSidePanelFilterRowView view;
            view.kind = Kind::Section;
            view.facet = SidePanelFacet::Tag;
            addRow(std::move(view), SidePanelFluentGlyph(kSidePanelTagGlyph, 12, 0.9), L"By Tag", false,
                   L"Show the tags to pick from: every tag a session here carries, and Untagged. Picks add up.",
                   L"am.side.filter.section.tag");
            if (SidePanelHasFacet(_sidePanelFilterExpanded, SidePanelFacet::Tag))
            {
                for (const auto& [name, count] : census.tags)
                {
                    AgentSidePanelFilterRowView row;
                    row.kind = Kind::Tag;
                    row.tag = name;
                    addRow(std::move(row), SidePanelMakeTagRibbon(ResolveTagDisplayColor(name, _sidePanelTagColors)), name, true, L"",
                           L"am.side.filter.tag." + ::Agentmaster::FoldTagName(name));
                }
                AgentSidePanelFilterRowView untagged;
                untagged.kind = Kind::Untagged;
                addRow(std::move(untagged), SidePanelMakeTagRibbon(Color{ 0xFF, 0x55, 0x55, 0x55 }), L"Untagged", true,
                       L"Sessions that carry no tag.", L"am.side.filter.untagged");
            }
        }
        // 4. Active within ▸ — one window at a time ("Any time" = none).
        {
            AgentSidePanelFilterRowView view;
            view.kind = Kind::Section;
            view.facet = SidePanelFacet::Active;
            addRow(std::move(view), SidePanelFluentGlyph(kSidePanelClockGlyph, 12, 0.9), L"Active within", false,
                   L"Show how recently a session must have been active to be listed (the card's \x201C" L"active \x2026 ago\x201D).",
                   L"am.side.filter.section.active");
            if (SidePanelHasFacet(_sidePanelFilterExpanded, SidePanelFacet::Active))
            {
                AgentSidePanelFilterRowView any;
                any.kind = Kind::Active;
                any.minutes = 0;
                addRow(std::move(any), nullptr, L"Any time", true, L"", L"am.side.filter.active.any");
                for (const auto& choice : ::Agentmaster::kSidePanelActiveChoices)
                {
                    AgentSidePanelFilterRowView row;
                    row.kind = Kind::Active;
                    row.minutes = choice.minutes;
                    addRow(std::move(row), nullptr, choice.label, true, L"", L"am.side.filter.active." + std::to_wstring(choice.minutes));
                }
            }
        }
        separator();
        // 5. The four flags — each one narrows (two checked: both must hold).
        {
            const auto flag = [&](SidePanelFacet facet, const FrameworkElement& icon, const wchar_t* text, const wchar_t* tip, const wchar_t* token) {
                AgentSidePanelFilterRowView row;
                row.kind = Kind::Flag;
                row.facet = facet;
                addRow(std::move(row), icon, text, false, tip, std::wstring{ L"am.side.filter.flag." } + token);
            };
            auto dots = Text(L"\x2022\x2022\x2022", 11, true, 0.9); // the 3 dots of an unsent draft
            flag(SidePanelFacet::Draft, dots, L"Unsent draft", L"Sessions whose input box holds a prompt you have not sent (the 3 dots under the dot).", L"draft");
            auto ring = Text(L"\x25CB", 13, true, 1.0); // ○ in the flash ring's red
            ring.Foreground(SolidColorBrush{ Color{ 0xFF, 0xE5, 0x39, 0x35 } });
            flag(SidePanelFacet::Unread, ring, L"Unread", L"Sessions with news you have not looked at (the red ring on their tab and card).", L"unread");
            flag(SidePanelFacet::Pinned, SidePanelFluentGlyph(kSidePanelPinGlyph, 11, 0.9), L"Pinned to top", L"Cards you pinned with their right-click Pin to Top.", L"pinned");
            auto star = Text(L"\x2605", 12, false, 1.0); // ★ in gold
            star.Foreground(SolidColorBrush{ Color{ 0xFF, 0xF5, 0xC5, 0x18 } });
            flag(SidePanelFacet::Favorite, star, L"Favorite", L"Sessions you marked with the \x2605 (the Sessions page, a tab's menu).", L"favorite");
        }
        separator();
        // 6. Clear all filters (the typed text too, like the chips row's Clear).
        {
            AgentSidePanelFilterRowView view;
            view.kind = Kind::Clear;
            addRow(std::move(view), SidePanelFluentGlyph(kSidePanelClearGlyph, 11, 0.9), L"Clear all filters", false,
                   L"Drop every filter, and empty the filter box: the cards list every session again.", L"am.side.filter.clear");
        }

        // The checks, the counts and the Clear row's enabled state, written by the one function the in-place refresh uses.
        for (auto& row : _sidePanelFilterRows)
        {
            const auto [check, count] = SidePanelFilterRowTexts(row, census, _sidePanelFilters, _sidePanelFilterExpanded);
            if (row.check)
            {
                row.check.Text(winrt::hstring{ check });
            }
            if (row.count)
            {
                row.count.Text(winrt::hstring{ count });
            }
            if (row.kind == Kind::Clear && row.button)
            {
                row.button.IsEnabled(::Agentmaster::SidePanelFiltersActive(_sidePanelFilters) || !::Agentmaster::ParseSessionQuery(_sidePanelSearchText).empty());
            }
            if (row.button && row.kind != Kind::ByFolder && row.kind != Kind::Section && row.kind != Kind::Clear)
            {
                AgentSetItemStatus(row.button, check == L"\x2713" || check == L"\x25CF" ? L"checked" : L"");
            }
        }
    }

    void TerminalPage::_SidePanelRefreshFilterPanel() noexcept
    {
        using Kind = AgentSidePanelFilterRowView::Kind;
        if (!_SidePanelFilterPanelOpen())
        {
            return;
        }
        try
        {
            const auto census = _SidePanelFilterCensus();
            // A tag row set that no longer matches the census (a tag that appeared, a picked tag gone and dropped) cannot be
            // patched in place: refill, keeping the keyboard on the row it was on.
            if (SidePanelHasFacet(_sidePanelFilterExpanded, ::Agentmaster::SidePanelFacet::Tag))
            {
                std::vector<std::wstring> shownTags;
                for (const auto& row : _sidePanelFilterRows)
                {
                    if (row.kind == Kind::Tag)
                    {
                        shownTags.push_back(::Agentmaster::FoldTagName(row.tag));
                    }
                }
                std::vector<std::wstring> wantTags;
                for (const auto& [name, count] : census.tags)
                {
                    wantTags.push_back(::Agentmaster::FoldTagName(name));
                }
                std::sort(shownTags.begin(), shownTags.end());
                std::sort(wantTags.begin(), wantTags.end());
                if (shownTags != wantTags)
                {
                    std::optional<AgentSidePanelFilterRowView> focusedRow;
                    if (const auto xr = Root().XamlRoot())
                    {
                        if (const auto focused = FocusManager::GetFocusedElement(xr).try_as<DependencyObject>())
                        {
                            for (const auto& row : _sidePanelFilterRows)
                            {
                                if (row.button && SidePanelLaunchIsUnder(focused, row.button))
                                {
                                    focusedRow = row;
                                    break;
                                }
                            }
                        }
                    }
                    _SidePanelFillFilterPanel();
                    if (focusedRow)
                    {
                        for (const auto& row : _sidePanelFilterRows)
                        {
                            if (row.button && SidePanelSameFilterRow(row, *focusedRow))
                            {
                                row.button.Focus(FocusState::Programmatic);
                                break;
                            }
                        }
                    }
                    _SidePanelRefreshFolderMenu(); // By Folder's open submenu follows too (noexcept; a no-op when closed)
                    return;
                }
            }
            const bool clearable = ::Agentmaster::SidePanelFiltersActive(_sidePanelFilters) || !::Agentmaster::ParseSessionQuery(_sidePanelSearchText).empty();
            for (auto& row : _sidePanelFilterRows)
            {
                const auto [check, count] = SidePanelFilterRowTexts(row, census, _sidePanelFilters, _sidePanelFilterExpanded);
                // Written only on a change: a same-value Text write still re-measures the TextBlock.
                if (row.check && std::wstring_view{ row.check.Text() } != check)
                {
                    row.check.Text(winrt::hstring{ check });
                }
                if (row.count && std::wstring_view{ row.count.Text() } != count)
                {
                    row.count.Text(winrt::hstring{ count });
                }
                if (row.kind == Kind::Clear && row.button && row.button.IsEnabled() != clearable)
                {
                    row.button.IsEnabled(clearable);
                }
                if (row.button && row.kind != Kind::ByFolder && row.kind != Kind::Section && row.kind != Kind::Clear)
                {
                    AgentSetItemStatus(row.button, check == L"\x2713" || check == L"\x25CF" ? L"checked" : L""); // change-gated inside
                }
            }
            // By Folder's open submenu: its checks follow the record, and its counts every other filter and the typed text
            // (SIDE_PANEL.md §2l "The By Folder submenu"). Noexcept; a no-op when it is closed.
            _SidePanelRefreshFolderMenu();
        }
        catch (...)
        {
            // The panel shows the previous checks and counts until the next pick or open. Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelRefreshFilterPanel");
        }
    }

    void TerminalPage::_SidePanelOnFilterRowClick(size_t index) noexcept
    {
        using Kind = AgentSidePanelFilterRowView::Kind;
        using ::Agentmaster::SidePanelFacet;
        try
        {
            if (!_SidePanelFilterPanelOpen() || index >= _sidePanelFilterRows.size())
            {
                return; // closed before the tick ran, or the rows were rebuilt meanwhile
            }
            const AgentSidePanelFilterRowView row = _sidePanelFilterRows[index]; // a copy: a refill below replaces the vector
            auto next = _sidePanelFilters;
            switch (row.kind)
            {
            case Kind::ByFolder:
                // The user's "By Folder -> <the picker of +>": the panel gives way to the "+" card in folder-pick mode, at the
                // same anchor (the funnel's click). The card focuses its box on its own tick; the panel's close sees the card
                // open by then and gives the keyboard to nothing else.
                _SidePanelCloseFilterPanel(L"by folder");
                _SidePanelOpenFolderCard();
                return;
            case Kind::Section:
            {
                // Expand / fold the section, in place: the rows are rebuilt (a few dozen) and the keyboard stays on the header.
                _sidePanelFilterExpanded ^= ::Agentmaster::SidePanelFacetBit(row.facet);
                _SidePanelFillFilterPanel();
                for (const auto& rebuilt : _sidePanelFilterRows)
                {
                    if (rebuilt.button && SidePanelSameFilterRow(rebuilt, row))
                    {
                        rebuilt.button.Focus(FocusState::Programmatic);
                        break;
                    }
                }
                return;
            }
            case Kind::State:
                ::Agentmaster::SidePanelToggleState(next, row.stateBit);
                _SidePanelSetFilters(std::move(next), L"state");
                return;
            case Kind::Tag:
                ::Agentmaster::SidePanelToggleTag(next, row.tag);
                _SidePanelSetFilters(std::move(next), L"tag");
                return;
            case Kind::Untagged:
                next.untagged = !next.untagged;
                _SidePanelSetFilters(std::move(next), L"untagged");
                return;
            case Kind::Flag:
                switch (row.facet)
                {
                case SidePanelFacet::Draft:
                    next.draft = !next.draft;
                    break;
                case SidePanelFacet::Unread:
                    next.unread = !next.unread;
                    break;
                case SidePanelFacet::Pinned:
                    next.pinned = !next.pinned;
                    break;
                case SidePanelFacet::Favorite:
                    next.favorite = !next.favorite;
                    break;
                default:
                    return;
                }
                _SidePanelSetFilters(std::move(next), L"flag");
                return;
            case Kind::Active:
                ::Agentmaster::SidePanelSetActiveWithin(next, row.minutes);
                _SidePanelSetFilters(std::move(next), L"active-within");
                return;
            case Kind::Clear:
                _SidePanelClearFilterText(); // the box's TextChanged re-filters the cards (asynchronously)
                _SidePanelSetFilters(::Agentmaster::SidePanelFilters{}, L"clear");
                _SidePanelRefreshFilterPanel(); // the Clear row grays even when the record was already empty (text only)
                return;
            default:
                return;
            }
        }
        catch (...)
        {
            // The pick is lost; the panel shows the record as it is. Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelOnFilterRowClick");
        }
    }

    void TerminalPage::_SidePanelClearFilterText() noexcept
    {
        try
        {
            if (_sidePanelSearchBox && !_sidePanelAiMode && !_sidePanelSearchBox.Text().empty())
            {
                // In AI mode the box is the request line, whose text is the user's unsent request: never ours to empty.
                _sidePanelSearchBox.Text(L"");
                // The filter's mirror of the box, emptied NOW: the box's TextChanged (which would set it and rebuild) comes
                // later, asynchronously, and the caller's own update or panel refresh must already see "no text" (Clear grays).
                _sidePanelSearchText.clear();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelClearFilterText"); // the text stays; the box's own ✕ empties it
        }
    }

    bool TerminalPage::_SidePanelEnsureFolderCard() noexcept
    {
        if (_sidePanelFolderPopup && _sidePanelFolderCard && _sidePanelFolderBar)
        {
            return true;
        }
        try
        {
            const auto root = Root();
            if (!root)
            {
                return false;
            }
            const auto weakThis = get_weak();

            // The card: the "+" card's dark look, its Button brushes, Tab cycling inside it.
            Border card{};
            card.RequestedTheme(ElementTheme::Dark);
            card.Background(Fill(0xFF, 0x26, 0x26, 0x26));
            card.BorderBrush(Fill(0xFF, 0x3A, 0x3A, 0x3A));
            card.BorderThickness(ThicknessHelper::FromUniformLength(1));
            card.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
            card.Padding(ThicknessHelper::FromLengths(10, 8, 10, 8));
            card.TabFocusNavigation(KeyboardNavigationMode::Cycle);
            AgentLaunchBar::SeedButtonStateBrushes(card.Resources());

            // The bar: the "+" card's own launch row, in FOLDER-PICK MODE (AgentLaunchBarHost::pickFolder) — the user's "By
            // Folder -> <the picker of +>": the same box, underline and drop-down (Browse…, RECENT, GIT WORKTREES, SUBFOLDERS),
            // plus the folders the panel's sessions work in, and "Filter" in Launch's place.
            AgentLaunchBarHost host;
            host.codexToggle = false; // the fixed chip, which pick mode turns into "Folder"
            if (const auto ipc = _agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    host.recentDirs = mgr->LaunchRecentDirs(); // RECENT lists where you launched (pick mode never pushes into it)
                }
            }
            if (!host.recentDirs)
            {
                host.recentDirs = std::make_shared<AgentLaunchRecentDirs>();
                host.recentDirs->dirs = ::Agentmaster::LoadRecentDirs();
            }
            host.popupHost = root;
            host.popupRowSpan = 3;
            host.dialogAnchor = card;
            host.dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            host.registry = [weakThis]() -> std::shared_ptr<::Agentmaster::SessionRegistry> {
                if (const auto self = weakThis.get())
                {
                    return self->_sessionRegistry;
                }
                return nullptr;
            };
            host.recentDirsLimit = [weakThis]() -> uint32_t {
                if (const auto self = weakThis.get())
                {
                    return self->_appSettings.recentDirsLimit;
                }
                return 0u;
            };
            host.pickFolder = [weakThis](const std::wstring& dir) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelPickFilterFolder(dir);
                }
            };
            host.pickLabel = L"Filter";
            host.pickTip = L"List only the sessions that work in this folder \x2014 and in every folder under it while Include subfolders is checked. Another folder picked later adds to this one: a session in either is listed.";
            host.folderChoices = [weakThis]() -> std::vector<AgentLaunchFolderChoice> {
                if (const auto self = weakThis.get())
                {
                    return self->_SidePanelFolderChoices();
                }
                return {};
            };
            host.folderChoicesLabel = L"FOLDERS YOUR SESSIONS WORK IN";
            host.layoutChanged = [weakThis]() {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelQueueFilterSurfacesPlacement(); // the row's width may have changed: re-fit and re-place later
                }
            };
            auto bar = AgentLaunchBar::Create(std::move(host));

            // "Include subfolders" — the user's "some way to get Exactly that folder": checked at every open (their default),
            // unchecked = this pick is exactly that folder. A real CheckBox (its own check glyph and keyboard toggle).
            CheckBox subfolders{};
            subfolders.Content(winrt::box_value(winrt::hstring{ L"Include subfolders" }));
            subfolders.MinWidth(0);
            subfolders.IsChecked(true);
            subfolders.VerticalAlignment(VerticalAlignment::Center);
            AgentSetTitledTip(subfolders, L"Include subfolders", L"Checked: the folder and every folder under it (K:\\source covers all of its repositories). Unchecked: exactly this folder. Each folder you pick keeps its own choice; its chip says \x201C(exactly)\x201D when unchecked.", kSidePanelButtonTipDelay);
            AgentSetAutomationId(subfolders, L"am.side.folderCard.subfolders");

            // [Folder] [ box / underline ] [☑ Include subfolders] [Filter] — the "+" row's shape, Fork left out (pick mode
            // never shows it).
            StackPanel row{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(kSidePanelLaunchRowSpacingPx);
            row.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(bar->AgentElement());
            row.Children().Append(bar->CwdColumn());
            row.Children().Append(subfolders);
            row.Children().Append(bar->LaunchButton());
            card.Child(row);
            // Ids on the controls (a Border card has no automation peer, so the card is found by its box).
            AgentSetAutomationId(bar->CwdBox(), L"am.side.folderCard.box");
            AgentSetAutomationId(bar->LaunchButton(), L"am.side.folderCard.filter");

            // Keys at the KEY-UP (the "+" card's rule — the close hands the keyboard to a terminal):
            //   - Escape with the folder list CLOSED closes the card (with it open, the box's own Escape closes the list);
            //   - Enter in the box or on Filter picks the folder (what Filter's click does: the box's own Enter would only
            //     commit the text, the launch bar's rule — here the box IS the filter's input, so Enter applies it);
            //   - never a Ctrl / Alt chord.
            card.PreviewKeyDown([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    self->_sidePanelFolderArmedKey = VirtualKey::None;
                    self->_sidePanelFolderArmedPick = false;
                    const auto key = e.Key();
                    if (key != VirtualKey::Enter && key != VirtualKey::Escape)
                    {
                        return;
                    }
                    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
                    {
                        return;
                    }
                    const auto bar = self->_sidePanelFolderBar;
                    if (!bar)
                    {
                        return;
                    }
                    if (key == VirtualKey::Escape)
                    {
                        if (bar->PathPickerOpen())
                        {
                            return; // the box's own Escape closes the list first
                        }
                        self->_sidePanelFolderArmedKey = key;
                        self->_sidePanelFolderArmedPick = false;
                        e.Handled(true);
                        return;
                    }
                    const void* const source = SidePanelLaunchIdentity(e.OriginalSource());
                    if (source && (source == SidePanelLaunchIdentity(bar->CwdBox()) || source == SidePanelLaunchIdentity(bar->LaunchButton())))
                    {
                        self->_sidePanelFolderArmedKey = key;
                        self->_sidePanelFolderArmedPick = true;
                        e.Handled(true); // neither the box's commit nor the Button's key-down click runs: the key-up picks
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel folder card: key-down");
                }
            });
            card.PreviewKeyUp([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || self->_sidePanelFolderArmedKey == VirtualKey::None || e.Key() != self->_sidePanelFolderArmedKey)
                {
                    return;
                }
                const bool pick = self->_sidePanelFolderArmedPick;
                self->_sidePanelFolderArmedKey = VirtualKey::None;
                self->_sidePanelFolderArmedPick = false;
                e.Handled(true); // before acting: a pick closes the card synchronously
                try
                {
                    if (pick)
                    {
                        if (const auto bar = self->_sidePanelFolderBar)
                        {
                            bar->ClickLaunch(); // exactly Filter's click: the box's folder, or nothing for an invalid text
                        }
                    }
                    else
                    {
                        self->_SidePanelCloseFolderCard(L"esc");
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel folder card: key-up");
                }
            });
            card.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelQueueFilterSurfacesPlacement();
                }
            });

            winrt::Windows::UI::Xaml::Controls::Primitives::Popup popup{};
            popup.Child(card);
            popup.HorizontalAlignment(HorizontalAlignment::Left);
            popup.VerticalAlignment(VerticalAlignment::Top);
            Grid::SetRow(popup, 0);
            Grid::SetRowSpan(popup, 3);
            root.Children().Append(popup);

            _sidePanelFolderPopup = popup;
            _sidePanelFolderCard = card;
            _sidePanelFolderSubfolders = subfolders;
            _sidePanelFolderBar = std::move(bar);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] folder filter card built (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
            return true;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: build"); // nothing kept: the next By Folder… retries
            return false;
        }
    }

    void TerminalPage::_SidePanelOpenFolderCard()
    {
        if (!_sidePanelBuilt || _SidePanelFolderCardOpen())
        {
            return;
        }
        // The outside-press and resize hooks are the panel's (registered by its build, which By Folder… always follows); a belt
        // for an open with no panel built yet registers them here.
        if (!_sidePanelFilterHooked && !_SidePanelEnsureFilterPanel())
        {
            return;
        }
        if (!_SidePanelEnsureFolderCard())
        {
            return;
        }
        try
        {
            _sidePanelFilterDockLeft = _appSettings.sidePanelOnLeft;
            _sidePanelFolderArmedKey = VirtualKey::None;
            _sidePanelFolderArmedPick = false;
            // An EMPTY box: the list under it then shows every folder of the sessions, the recent ones and Browse… (a typed
            // word narrows them). SetBoxText re-validates explicitly (Filter disabled until a folder is in the box).
            _sidePanelFolderBar->SetBoxText(L"");
            if (_sidePanelFolderSubfolders)
            {
                _sidePanelFolderSubfolders.IsChecked(true); // the user's default at every open: the folder and its subfolders
            }
            _SidePanelFitFolderBox();
            _sidePanelFolderPopup.IsOpen(true);
            _SidePanelPlaceFilterSurfaces();
            ::Agentmaster::LogNav(L"sidepanel filter folder card open");
            // The "+" card's three steps one tick later: lay it out at its place, focus the box, open the folder list.
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelFinishFolderCardOpen();
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: open");
            try
            {
                if (_sidePanelFolderPopup)
                {
                    _sidePanelFolderPopup.IsOpen(false);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel folder card: open rollback");
            }
        }
    }

    void TerminalPage::_SidePanelFinishFolderCardOpen() noexcept
    {
        if (!_SidePanelFolderCardOpen() || !_sidePanelFolderBar || !_sidePanelFolderCard)
        {
            return; // closed again before the tick ran
        }
        // 1. Lay the card out at the offsets the open wrote (a popup's child moves only in a layout pass), so the list anchors
        //    under the box where it really is — never over the box (the "+" card's 2026-10-12 lesson).
        try
        {
            _sidePanelFolderCard.UpdateLayout();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: lay the card out before its folder list opens");
        }
        if (!_SidePanelFolderCardOpen())
        {
            return;
        }
        // 2. The caret in the box: typing narrows the list at once; Enter and Escape reach the card's key handlers.
        try
        {
            if (const auto box = _sidePanelFolderBar->CwdBox())
            {
                if (!box.Focus(FocusState::Programmatic))
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] folder filter card: its box did not take focus (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: focus the box");
        }
        // 3. The folder list open under the box — the "+" card's rule ("the path textbox's dropdown menu is supposed to be open
        //    by default"): the folders of the sessions first.
        _sidePanelFolderBar->OpenPathPicker(); // noexcept
    }

    void TerminalPage::_SidePanelCloseFolderCard(std::wstring_view reason)
    {
        if (!_SidePanelFolderCardOpen())
        {
            return;
        }
        _sidePanelFolderArmedKey = VirtualKey::None;
        _sidePanelFolderArmedPick = false;
        bool hadFocus = false;
        try
        {
            if (const auto xr = Root().XamlRoot())
            {
                if (const auto focused = FocusManager::GetFocusedElement(xr).try_as<DependencyObject>())
                {
                    hadFocus = SidePanelLaunchIsUnder(focused, _sidePanelFolderCard);
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: focus read at close");
        }
        if (_sidePanelFolderBar)
        {
            _sidePanelFolderBar->DismissForHide(); // its folder list closed, the dismissal latched (noexcept)
        }
        try
        {
            _sidePanelFolderPopup.IsOpen(false);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: close");
        }
        ::Agentmaster::LogNav(L"sidepanel filter folder card close (" + std::wstring{ reason } + L")");
        if (hadFocus)
        {
            try
            {
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis = get_weak()]() {
                    const auto self = weakThis.get();
                    if (!self || self->_SidePanelFolderCardOpen() || self->_SidePanelFilterPanelOpen())
                    {
                        return; // reopened, or the panel took the keyboard
                    }
                    try
                    {
                        self->_FocusCurrentTab(true);
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"side panel folder card: focus back to the tab");
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel folder card: queue the focus restore");
            }
        }
    }

    void TerminalPage::_SidePanelPickFilterFolder(const std::wstring& dir) noexcept
    {
        try
        {
            bool includeSubfolders = true;
            if (_sidePanelFolderSubfolders)
            {
                const auto checked = _sidePanelFolderSubfolders.IsChecked();
                includeSubfolders = !checked || checked.Value(); // an indeterminate (null) box keeps the default
            }
            auto next = _sidePanelFilters;
            const bool added = ::Agentmaster::SidePanelAddFolderFilter(next, dir, includeSubfolders);
            _SidePanelCloseFolderCard(L"picked");
            if (added)
            {
                _SidePanelSetFilters(std::move(next), includeSubfolders ? L"folder+ (with subfolders)" : L"folder+ (exactly)");
            }
            // By Folder's pick history (SIDE_PANEL.md §2l "The By Folder submenu"), the user's "repicking moves to top": EVERY
            // pick through the card joins it — a folder this window already filters by included (it moves up) — whatever the
            // pick changed in the record. A path that is not an absolute folder is skipped there. Never throws.
            _SidePanelRememberFolderPick(dir, includeSubfolders);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelPickFilterFolder"); // nothing added; the card may stay open
        }
    }

    std::vector<AgentLaunchFolderChoice> TerminalPage::_SidePanelFolderChoices() noexcept
    {
        std::vector<AgentLaunchFolderChoice> out;
        try
        {
            if (!_sessionRegistry)
            {
                return out;
            }
            _SidePanelSyncFactCaches();
            const auto mode = _appSettings.tabColorMode;
            const auto snap = _sessionRegistry->SnapshotLiveForCards();
            const auto terms = ::Agentmaster::ParseSessionQuery(_sidePanelSearchText);
            const bool local = !_sidePanelGlobal;
            std::unordered_set<std::wstring> hosted;
            if (local)
            {
                hosted = _HostedSessionIds();
            }
            const auto unread = ::Agentmaster::CurrentUnreadSessionIds();
            const ::Agentmaster::SidePanelFilterMatcher matcher{ _sidePanelFilters, NowMs() };
            const auto dirColors = ::Agentmaster::LoadDirColorIndex();
            std::vector<::Agentmaster::SessionFolderFacts> facts;
            std::unordered_map<std::wstring, uint32_t> colorByKey; // folder key -> the dot's color (the first session's band)
            for (const auto& s : snap)
            {
                if (!s.live || (local && hosted.find(s.id) == hosted.end()) || !SidePanelSessionMatches(s, terms, mode))
                {
                    continue;
                }
                // Every facet but By Folder itself: the folders offered are those where another pick still lists something.
                const auto f = _SidePanelFilterFactsOf(s, matcher.NeedsTags(), matcher.NeedsFavorite(), unread);
                if (!::Agentmaster::SidePanelPassesAllBut(matcher.Passed(f, true), ::Agentmaster::SidePanelFacet::Folder))
                {
                    continue;
                }
                const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
                facts.push_back(::Agentmaster::SessionFolderFacts{ s.workingDir, s.inferredWorkingDir, last });
                if (const auto band = HexToColor(::Agentmaster::ResolveSessionColorHex(mode, s, dirColors)))
                {
                    const uint32_t argb = SidePanelPackArgb(*band);
                    for (const auto* dir : { &s.workingDir, &s.inferredWorkingDir })
                    {
                        if (!dir->empty())
                        {
                            colorByKey.try_emplace(::Agentmaster::NormDirKey(::Agentmaster::TrimFolderPath(*dir)), argb);
                        }
                    }
                }
            }
            auto uses = ::Agentmaster::CollectSessionFolders(facts); // launch + inferred folders, each session once per folder
            ::Agentmaster::SortFoldersByUse(uses); // most used first (the folder card's list is a "where do my sessions work")
            out.reserve(uses.size());
            for (const auto& use : uses)
            {
                AgentLaunchFolderChoice choice;
                choice.dir = use.path;
                choice.sessions = static_cast<uint32_t>(use.sessions);
                if (const auto it = colorByKey.find(use.key); it != colorByKey.end())
                {
                    choice.colorArgb = it->second;
                }
                out.push_back(std::move(choice));
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelFolderChoices"); // the section is left out; RECENT and Browse… still list
            out.clear();
        }
        return out;
    }

    void TerminalPage::_SidePanelFitFolderBox() noexcept
    {
        if (!_sidePanelFolderBar || !_sidePanelFolderCard)
        {
            return;
        }
        try
        {
            const auto box = _sidePanelFolderBar->CwdBox();
            if (!box)
            {
                return;
            }
            const auto root = Root();
            const double rootW = root ? root.ActualWidth() : 0.0;
            double width = kSidePanelLaunchBoxMaxPx; // an unmeasured window: the comfortable width, re-fitted on its first SizeChanged
            if (rootW > 0.0)
            {
                // The "+" card's fit (_SidePanelFitLaunchBox): the window's width less the margins, the card's chrome, the row's
                // gaps and every other part's natural width (the chip, the check box, Filter).
                const auto natural = [](const FrameworkElement& el) -> double {
                    if (!el || el.Visibility() == Visibility::Collapsed)
                    {
                        return 0.0;
                    }
                    el.Measure(winrt::Windows::Foundation::Size{ kSidePanelLaunchMeasureBound, kSidePanelLaunchMeasureBound });
                    return static_cast<double>(el.DesiredSize().Width);
                };
                const FrameworkElement others[] = { _sidePanelFolderBar->AgentElement(), _sidePanelFolderSubfolders, _sidePanelFolderBar->LaunchButton() };
                double othersW = 0.0;
                double gaps = 0.0;
                for (const auto& el : others)
                {
                    const double w = natural(el);
                    if (w > 0.0)
                    {
                        othersW += w;
                        gaps += kSidePanelLaunchRowSpacingPx;
                    }
                }
                const auto pad = _sidePanelFolderCard.Padding();
                const auto edge = _sidePanelFolderCard.BorderThickness();
                const double chrome = pad.Left + pad.Right + edge.Left + edge.Right;
                const double room = rootW - 2.0 * ::Agentmaster::kPointerCardMarginDips - chrome - gaps - othersW;
                width = std::clamp(room, kSidePanelLaunchBoxMinPx, kSidePanelLaunchBoxMaxPx);
            }
            if (std::abs(width - _sidePanelFolderBoxWidth) < 0.5)
            {
                return; // unchanged: no layout invalidation
            }
            _sidePanelFolderBoxWidth = width;
            box.MinWidth(width); // fixed (Min = Max): the card keeps its size while you type, its corner at the anchor
            box.MaxWidth(width);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder card: fit the box"); // the box keeps its previous width
        }
    }

    void TerminalPage::_SidePanelPlaceFilterSurfaces() noexcept
    {
        try
        {
            const auto root = Root();
            if (_SidePanelFilterPanelOpen() && _sidePanelFilterCard)
            {
                // The rows scroll inside the window's height (a long tag list): the cap is written only when it changes.
                if (const auto scroll = _sidePanelFilterCard.Child().try_as<ScrollViewer>(); scroll && root && root.ActualHeight() > 0.0)
                {
                    const double cap = std::max(120.0, root.ActualHeight() - 2.0 * ::Agentmaster::kPointerCardMarginDips - 10.0);
                    if (std::abs(scroll.MaxHeight() - cap) > 0.5)
                    {
                        scroll.MaxHeight(cap);
                    }
                }
                SidePanelPlaceCardAt(_sidePanelFilterPopup, _sidePanelFilterCard, _sidePanelFilterAnchor, root);
                // By Folder's submenu stands beside the panel: it follows every placement of it (noexcept; a no-op when closed).
                _SidePanelPlaceFolderMenu();
            }
            if (_SidePanelFolderCardOpen() && _sidePanelFolderCard)
            {
                if (SidePanelPlaceCardAt(_sidePanelFolderPopup, _sidePanelFolderCard, _sidePanelFilterAnchor, root) && _sidePanelFolderBar)
                {
                    // Moved with its folder list open: the list follows the box after the next pass (noexcept; a no-op when closed).
                    _sidePanelFolderBar->ReanchorPathPickerAfterLayout();
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel filter surfaces: place"); // they keep their offsets
        }
    }

    void TerminalPage::_SidePanelQueueFilterSurfacesPlacement() noexcept
    {
        if (_sidePanelFilterPlaceQueued)
        {
            return; // a tick is already queued: this change folds into it
        }
        _sidePanelFilterPlaceQueued = true;
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                self->_sidePanelFilterPlaceQueued = false; // BEFORE the work: a size change during it queues another tick
                if (self->_SidePanelFolderCardOpen())
                {
                    self->_SidePanelFitFolderBox();
                }
                self->_SidePanelPlaceFilterSurfaces();
            });
        }
        catch (...)
        {
            // The dispatcher is going away with the window: re-arm, never latch shut.
            _sidePanelFilterPlaceQueued = false;
            ::Agentmaster::AgentLogCaughtException(L"side panel filter surfaces: queue placement");
        }
    }

    void TerminalPage::_SidePanelCloseFilterSurfaces(std::wstring_view reason)
    {
        _SidePanelCloseFilterPanel(reason); // By Folder's submenu closes with the panel
        _SidePanelCloseFolderCard(reason);
    }

    // ===== By Folder's hover SUBMENU (SIDE_PANEL.md §2l "The By Folder submenu") ========================================
    // The user's request of 2026-11-10: "By Folder works great and as expected, keep By Folder AS IS, clickable, but when
    // hovered have a submenu with / Pick / --- / <each line is an item we picked, repicking moves to top>". Their picks the same
    // day (design-ask with mockups): a folder line TOGGLES that folder in this window's record (✓ = filtering now; the submenu
    // stays open, and its lines keep their place while it is open) and the history is ONE LIST FOR THE APP (the profile's
    // filter-folders.json: every window's submenu shows it, the last pick anywhere on top; the ✓ marks follow each window's
    // own record).
    // SHAPE: a second Root()-parented Popup card in the panel's look, beside the panel's By Folder… row (PlaceSubmenuCard) — a
    // MenuFlyout would close at every pick. HOVER: the desktop menu's intent (_SidePanelFolderMenuHover): a pointer resting on
    // By Folder… for the system's menu delay opens it, one resting on another row of the panel for the delay closes it, and
    // reaching the submenu meanwhile cancels that close. KEYS: Right on By Folder… opens it with Pick… focused; inside it, Left
    // (key-down) or Escape (key-up) closes it and the keyboard goes back to By Folder…; Enter / Space on Pick… opens the folder
    // card on its key-up; Escape in the panel closes an open submenu before the panel. PRESSES: inside the submenu is inside
    // the panel (the outside-press hook). It closes with the panel (every reason the panel closes).
    // COST: built at the first open; about a dozen rows rebuilt per open (the history is ten folders at most); a click
    // rewrites the checks and counts in place, its census one pass over the scope like the panel's.

    bool TerminalPage::_SidePanelFolderMenuOpen() const noexcept
    {
        try
        {
            return _sidePanelFolderMenuPopup && _sidePanelFolderMenuPopup.IsOpen();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: IsOpen read"); // a torn-down popup reads as closed
            return false;
        }
    }

    bool TerminalPage::_SidePanelEnsureFolderMenu() noexcept
    {
        if (_sidePanelFolderMenuPopup && _sidePanelFolderMenuCard && _sidePanelFolderMenuRowsHost)
        {
            return true;
        }
        try
        {
            const auto root = Root();
            if (!root)
            {
                return false; // no page root to parent into (never after the first layout)
            }
            const auto weakThis = get_weak();

            // The card: the panel's own look (a menu's dark card, 7 DIP corners), so the two read as one menu and its submenu.
            Border card{};
            card.RequestedTheme(ElementTheme::Dark);
            card.Background(Fill(0xFF, 0x2B, 0x2B, 0x2B));
            card.BorderBrush(Fill(0xFF, 0x47, 0x47, 0x47));
            card.BorderThickness(ThicknessHelper::FromUniformLength(1));
            card.CornerRadius(CornerRadiusHelper::FromUniformRadius(7));
            card.Padding(ThicknessHelper::FromLengths(0, 4, 0, 4)); // kSidePanelFolderMenuChromeTop = this top padding + the border
            card.TabFocusNavigation(KeyboardNavigationMode::Cycle);
            // The rows are Buttons: the Agent Manager's Button state brushes, or under the forced-Dark theme a row would light up
            // only on its text (the panel's reason).
            AgentLaunchBar::SeedButtonStateBrushes(card.Resources());
            StackPanel rows{};
            rows.Width(kSidePanelFolderMenuWidth);
            rows.XYFocusKeyboardNavigation(XYFocusKeyboardNavigationMode::Enabled); // Up / Down walk the rows
            ScrollViewer scroll{};
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroll.IsTabStop(false); // the scroller itself never holds the keyboard: the rows do
            scroll.Content(rows);
            card.Child(scroll);

            // Keys. PreviewKeyDown TUNNELS, so it runs before the focused row:
            //   - Escape: arms the close for its KEY-UP (the keyboard then goes back to By Folder…; the panel's rule for keys
            //     that move it);
            //   - Left: closes at once, the keyboard back on By Folder… (Left writes no character anywhere);
            //   - Enter / Space on Pick…: arms the folder card for the KEY-UP (focus moves to the card's box: its CR / space
            //     must not land there);
            //   - Enter / Space on a folder line: the row's own Button handles it (a toggle in place, the focus stays);
            //   - never a Ctrl / Alt chord (Alt+Enter is Terminal's full-screen toggle).
            card.PreviewKeyDown([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    self->_sidePanelFolderMenuArmedKey = VirtualKey::None; // any key-down disarms: the next key-up must be its own
                    self->_sidePanelFolderMenuArmedPick = false;
                    const auto key = e.Key();
                    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
                    {
                        return; // an app chord: not ours
                    }
                    if (key == VirtualKey::Escape)
                    {
                        self->_sidePanelFolderMenuArmedKey = key;
                        e.Handled(true);
                        return;
                    }
                    if (key == VirtualKey::Left)
                    {
                        e.Handled(true); // no XY move: the submenu gives the keyboard back
                        self->_SidePanelCloseFolderMenu(L"left", /*focusByFolder*/ true);
                        return;
                    }
                    if (key == VirtualKey::Enter || key == VirtualKey::Space)
                    {
                        const void* const source = SidePanelLaunchIdentity(e.OriginalSource());
                        for (const auto& row : self->_sidePanelFolderMenuRows)
                        {
                            if (row.kind == AgentSidePanelFolderMenuRowView::Kind::Pick && source && source == SidePanelLaunchIdentity(row.button))
                            {
                                self->_sidePanelFolderMenuArmedKey = key;
                                self->_sidePanelFolderMenuArmedPick = true;
                                e.Handled(true); // the Button must not click on this key-down
                                return;
                            }
                        }
                    }
                }
                catch (...)
                {
                    // The key then does what the focused row does with it. Recorded (Rule #18).
                    ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: key-down");
                }
            });
            card.PreviewKeyUp([weakThis](const IInspectable&, const KeyRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || self->_sidePanelFolderMenuArmedKey == VirtualKey::None || e.Key() != self->_sidePanelFolderMenuArmedKey)
                {
                    return; // its key-down was not armed here
                }
                const bool pick = self->_sidePanelFolderMenuArmedPick;
                self->_sidePanelFolderMenuArmedKey = VirtualKey::None;
                self->_sidePanelFolderMenuArmedPick = false;
                e.Handled(true); // set BEFORE acting: the close takes the card out of the tree synchronously
                try
                {
                    if (pick)
                    {
                        for (size_t i = 0; i < self->_sidePanelFolderMenuRows.size(); ++i)
                        {
                            if (self->_sidePanelFolderMenuRows[i].kind == AgentSidePanelFolderMenuRowView::Kind::Pick)
                            {
                                self->_SidePanelOnFolderMenuClick(i);
                                break;
                            }
                        }
                    }
                    else
                    {
                        self->_SidePanelCloseFolderMenu(L"esc", /*focusByFolder*/ true);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: key-up");
                }
            });
            // The pointer reaching the submenu cancels a pending close: on its way from By Folder… it may cross a neighbour row
            // of the panel (the desktop menu's diagonal). handledEventsToo, like the rows' hooks; touch has no hover.
            card.AddHandler(
                UIElement::PointerEnteredEvent(),
                winrt::box_value(PointerEventHandler{ [weakThis](const IInspectable&, const PointerRoutedEventArgs& e) {
                    const auto self = weakThis.get();
                    if (!self || e.Pointer().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Touch)
                    {
                        return;
                    }
                    self->_SidePanelFolderMenuHover(SidePanelFolderMenuHover::EnterMenu);
                } }),
                true /* handledEventsToo */);
            // A different height (no folder yet → a few): re-place it, QUEUED — SizeChanged is a layout pass, and moving a
            // parented Popup inside one is the E_LAYOUTCYCLE fail-fast.
            card.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs&) {
                if (const auto self = weakThis.get())
                {
                    self->_SidePanelQueueFilterSurfacesPlacement();
                }
            });

            // The Popup: Root()-parented across its three rows, top/left aligned, so its offsets are Root-relative (the panel's
            // recipe). Not light-dismiss: the panel's outside-press hook counts it inside the panel, and it closes with it.
            winrt::Windows::UI::Xaml::Controls::Primitives::Popup popup{};
            popup.Child(card);
            popup.HorizontalAlignment(HorizontalAlignment::Left);
            popup.VerticalAlignment(VerticalAlignment::Top);
            Grid::SetRow(popup, 0);
            Grid::SetRowSpan(popup, 3);
            root.Children().Append(popup);
            // No id on the card: a Border has no automation peer, so the submenu is found by its rows (am.side.filter.folderMenu.…).

            _sidePanelFolderMenuPopup = popup;
            _sidePanelFolderMenuCard = card;
            _sidePanelFolderMenuRowsHost = rows;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] filter folder menu built (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
            return true;
        }
        catch (...)
        {
            // Nothing was kept (the members are set only at the end), so the next hover builds from scratch.
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: build");
            return false;
        }
    }

    void TerminalPage::_SidePanelOpenFolderMenu(bool focusFirst) noexcept
    {
        try
        {
            // An open settles any pending hover intent (a key-opened submenu must not be closed by a stale hover tick).
            _sidePanelFolderMenuIntent = SidePanelFolderMenuIntent::None;
            if (_sidePanelFolderMenuTimer)
            {
                _sidePanelFolderMenuTimer.Stop();
            }
            if (!_sidePanelBuilt || !_SidePanelFilterPanelOpen())
            {
                return; // a submenu never opens without its panel
            }
            if (_SidePanelFolderMenuOpen())
            {
                // Already open (the pointer opened it): the keyboard's Right only moves the focus in, at once — the rows are
                // laid out.
                if (focusFirst && !_sidePanelFolderMenuRows.empty() && _sidePanelFolderMenuRows.front().button)
                {
                    _sidePanelFolderMenuRows.front().button.Focus(FocusState::Programmatic);
                }
                return;
            }
            if (!_SidePanelEnsureFolderMenu())
            {
                return; // the build failed (logged); the next hover or Right tries again
            }
            // The history as it stands NOW — another window may have picked a folder a moment ago (one list for the app) — with
            // this window's filtering folders seeded when it lacks them: they filter now, so they must stay listed once turned
            // off here (a filter an older build saved, a folder the cap pushed out). The only write an open can make, and only
            // when something is missing. A failed read lists the record's own folders alone (SidePanelFolderMenuLines).
            std::vector<::Agentmaster::SidePanelFolderFilter> history;
            try
            {
                history = ::Agentmaster::RememberFilterFolderPicks(_sidePanelFilters.folders, /*onlyMissing*/ true);
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: read the pick history");
            }
            _sidePanelFolderMenuArmedKey = VirtualKey::None;
            _sidePanelFolderMenuArmedPick = false;
            _SidePanelFillFolderMenu(history);
            _sidePanelFolderMenuPopup.IsOpen(true);
            // Placed NOW, in this tick, at its real size (a card placed a tick later would show for a frame at its old spot).
            _SidePanelPlaceFolderMenu();
            ::Agentmaster::LogNav(focusFirst ? L"sidepanel filter folder menu open (keyboard)" : L"sidepanel filter folder menu open (hover)");
            if (focusFirst)
            {
                // Pick… takes the keyboard one tick later: the card was only just realized, and a same-tick Focus on an element
                // not laid out yet can do nothing (the panel's own open, the tag editor's lesson).
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                    const auto self = weakThis.get();
                    if (!self || !self->_SidePanelFolderMenuOpen() || self->_sidePanelFolderMenuRows.empty())
                    {
                        return; // closed before the tick ran
                    }
                    try
                    {
                        if (const auto first = self->_sidePanelFolderMenuRows.front().button)
                        {
                            first.Focus(FocusState::Programmatic);
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: focus the first row"); // a click on a row works anyway
                    }
                });
            }
        }
        catch (...)
        {
            // A half-opened submenu must not stay on screen with no rows or placement: close it again (best effort).
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: open");
            try
            {
                if (_sidePanelFolderMenuPopup)
                {
                    _sidePanelFolderMenuPopup.IsOpen(false);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: open rollback");
            }
        }
    }

    void TerminalPage::_SidePanelCloseFolderMenu(std::wstring_view reason, bool focusByFolder) noexcept
    {
        // Whatever the state, a close settles the pending intent and the armed key: a tick or a key-up after it must find
        // nothing to act on.
        _sidePanelFolderMenuIntent = SidePanelFolderMenuIntent::None;
        _sidePanelFolderMenuArmedKey = VirtualKey::None;
        _sidePanelFolderMenuArmedPick = false;
        try
        {
            if (_sidePanelFolderMenuTimer)
            {
                _sidePanelFolderMenuTimer.Stop();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: stop the hover timer at close"); // its tick re-checks the state
        }
        if (!_SidePanelFolderMenuOpen())
        {
            return;
        }
        // Did the submenu hold the keyboard? Then it goes to By Folder… BEFORE the close: closing a Popup that holds focus lets
        // the focus manager drop it on any focusable element (the Gotcha "Closing a plain Popup that holds focus"). When the
        // panel closes too (focusByFolder false), the panel gives the keyboard back itself.
        try
        {
            if (focusByFolder)
            {
                if (const auto xr = Root().XamlRoot())
                {
                    const auto focused = FocusManager::GetFocusedElement(xr).try_as<DependencyObject>();
                    if (focused && SidePanelLaunchIsUnder(focused, _sidePanelFolderMenuCard))
                    {
                        if (const auto byFolder = SidePanelByFolderButton(_sidePanelFilterRows))
                        {
                            byFolder.Focus(FocusState::Programmatic);
                        }
                    }
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: hand the keyboard back to By Folder"); // a click puts it anywhere
        }
        try
        {
            _sidePanelFolderMenuPopup.IsOpen(false);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: close");
        }
        try
        {
            ::Agentmaster::LogNav(L"sidepanel filter folder menu close (" + std::wstring{ reason } + L")");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: the close's nav line"); // the close itself happened
        }
    }

    void TerminalPage::_SidePanelFillFolderMenu(const std::vector<::Agentmaster::SidePanelFolderFilter>& history)
    {
        using Kind = AgentSidePanelFolderMenuRowView::Kind;
        if (!_sidePanelFolderMenuRowsHost)
        {
            return;
        }
        _sidePanelFolderMenuRowsHost.Children().Clear();
        _sidePanelFolderMenuRows.clear();
        const auto weakThis = get_weak();
        // The lines: this window's filtering folders the history lacks first (checked), then the history, most recent pick
        // first — each checked, in the record's spelling and choice, while the record holds it (the pure rule, tested).
        const auto lines = ::Agentmaster::SidePanelFolderMenuLines(history, _sidePanelFilters);

        // ONE row: a transparent full-width Button around `content` — the panel's row recipe. Its Click runs one tick later (a
        // toggle rewrites texts, Pick… closes the card: the clicked Button must not leave the tree inside its own Click). The
        // handler captures the weak page and the row's INDEX, never the row's elements.
        //   view    - what the row stands for (its texts are already made, inside `content`).
        //   content - the row's Grid.
        //   tip     - the row's tooltip.
        //   autoId  - its AutomationId.
        //   name    - its accessible name.
        const auto addRow = [&](AgentSidePanelFolderMenuRowView view, const Grid& content, const std::wstring& tip, const std::wstring& autoId, const std::wstring& name) {
            Button btn{};
            btn.Content(content);
            btn.HorizontalAlignment(HorizontalAlignment::Stretch);
            btn.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            btn.Background(SolidColorBrush{ Colors::Transparent() }); // hit-testable everywhere; hover / press take the seeded brushes
            btn.BorderThickness(ThicknessHelper::FromUniformLength(0));
            btn.CornerRadius(CornerRadiusHelper::FromUniformRadius(0)); // flat rows (a rounded Button is a composition node each)
            btn.Padding(ThicknessHelper::FromLengths(10, 4, 10, 4));
            AgentSetTip(btn, winrt::hstring{ tip }, kSidePanelButtonTipDelay); // 3 s of rest: the rows are crossed on the way to a pick
            AgentSetAutomationId(btn, autoId);
            AgentSetAccessibleName(btn, name);
            const size_t index = _sidePanelFolderMenuRows.size();
            btn.Click([weakThis, index](const IInspectable&, const RoutedEventArgs&) {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, index]() {
                        if (const auto page = weakThis.get())
                        {
                            page->_SidePanelOnFolderMenuClick(index);
                        }
                    });
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: queue a row click"); // the click is lost
                }
            });
            view.button = btn;
            _sidePanelFolderMenuRowsHost.Children().Append(btn);
            _sidePanelFolderMenuRows.push_back(std::move(view));
        };
        // A grid of [check 16][middle *][count Auto] — the folder lines' columns; Pick… puts its icon and words in the middle.
        const auto makeGrid = []() {
            Grid g{};
            ColumnDefinition cCheck{};
            cCheck.Width(GridLengthHelper::FromPixels(16));
            ColumnDefinition cMiddle{};
            cMiddle.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition cCount{};
            cCount.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            g.ColumnDefinitions().Append(cCheck);
            g.ColumnDefinitions().Append(cMiddle);
            g.ColumnDefinitions().Append(cCount);
            return g;
        };

        // 1. Pick… — what By Folder…'s own click does (the user's "Pick"): the "+" card in folder-pick mode.
        {
            auto g = makeGrid();
            StackPanel words{};
            words.Orientation(Orientation::Horizontal);
            auto icon = SidePanelFluentGlyph(kSidePanelFolderGlyph, 12, 0.9);
            icon.VerticalAlignment(VerticalAlignment::Center);
            icon.Width(20);
            words.Children().Append(icon);
            words.Children().Append(Text(L"Pick\x2026", 12.5, false, 0.95));
            Grid::SetColumn(words, 1);
            g.Children().Append(words);
            AgentSidePanelFolderMenuRowView view;
            view.kind = Kind::Pick;
            addRow(std::move(view), g, L"Pick a folder in the + launcher's folder picker \x2014 what a click on By Folder\x2026 does. The folder joins this list, on top.",
                   L"am.side.filter.folderMenu.pick", L"Pick\x2026");
        }
        // 2. The separator, then the folders picked (or a dim line saying where they will appear).
        {
            Border rule{};
            rule.Height(1);
            rule.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
            rule.Background(Fill(0xFF, 0x45, 0x45, 0x45));
            _sidePanelFolderMenuRowsHost.Children().Append(rule);
        }
        if (lines.empty())
        {
            auto none = Text(L"No folder picked yet \x2014 Pick\x2026 adds one here.", 12, false, 0.55);
            none.Margin(ThicknessHelper::FromLengths(26, 4, 10, 6)); // under Pick…'s words (10 padding + the 16 DIP check column)
            _sidePanelFolderMenuRowsHost.Children().Append(none);
        }
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const auto& line = lines[i];
            AgentSidePanelFolderMenuRowView view;
            view.kind = Kind::Folder;
            view.path = line.path;
            view.key = line.key;
            view.includeSubfolders = line.includeSubfolders;
            view.checked = line.checked;
            auto g = makeGrid();
            view.check = Text(L"", 12, false, 0.9);
            Grid::SetColumn(view.check, 0);
            g.Children().Append(view.check);
            TextBlock label{};
            label.FontSize(12.5);
            label.TextWrapping(TextWrapping::NoWrap);
            label.TextTrimming(TextTrimming::CharacterEllipsis);
            label.VerticalAlignment(VerticalAlignment::Center);
            SidePanelSetFolderMenuLabel(label, view.path, view.includeSubfolders);
            Grid::SetColumn(label, 1);
            g.Children().Append(label);
            view.label = label;
            view.count = Text(L"", 11.5, false, 0.55);
            view.count.Margin(ThicknessHelper::FromLengths(8, 0, 0, 0));
            Grid::SetColumn(view.count, 2);
            g.Children().Append(view.count);
            const std::wstring tip = view.path +
                                     (view.includeSubfolders ? std::wstring{ L" and every folder under it." } : std::wstring{ L" \x2014 exactly this folder (its subfolders are not included)." }) +
                                     L" A click turns it on or off for this window's list (\x2713 = on); the count is how many sessions it alone would list.";
            const std::wstring name = SidePanelFolderMenuLineAccessibleName(view.path, view.includeSubfolders);
            addRow(std::move(view), g, tip, L"am.side.filter.folderMenu.folder." + std::to_wstring(i), name);
        }
        // The checks and the counts: the in-place refresh's own writes, so the two can never draw one state differently.
        const auto counts = _SidePanelFolderMenuCounts();
        for (size_t i = 0; i < _sidePanelFolderMenuRows.size(); ++i)
        {
            auto& row = _sidePanelFolderMenuRows[i];
            if (row.kind != Kind::Folder)
            {
                continue;
            }
            if (row.check)
            {
                row.check.Text(row.checked ? L"\x2713" : L"");
            }
            if (row.count)
            {
                row.count.Text(winrt::hstring{ i < counts.size() ? std::to_wstring(counts[i]) : std::wstring{} });
            }
            if (row.button)
            {
                AgentSetItemStatus(row.button, row.checked ? L"checked" : L"");
            }
        }
    }

    void TerminalPage::_SidePanelRefreshFolderMenu() noexcept
    {
        using Kind = AgentSidePanelFolderMenuRowView::Kind;
        if (!_SidePanelFolderMenuOpen())
        {
            return;
        }
        try
        {
            // 1. Each line against the record as it is NOW (SidePanelSyncFolderMenuLine): its check, and — only when the record's
            //    spelling or choice of the folder moved — its words. A line turned off keeps its words (it turns back on as it was).
            for (auto& row : _sidePanelFolderMenuRows)
            {
                if (row.kind != Kind::Folder)
                {
                    continue;
                }
                auto line = SidePanelLineOf(row);
                const bool wordsMoved = ::Agentmaster::SidePanelSyncFolderMenuLine(line, _sidePanelFilters);
                row.path = line.path;
                row.includeSubfolders = line.includeSubfolders;
                row.checked = line.checked;
                if (wordsMoved && row.label)
                {
                    SidePanelSetFolderMenuLabel(row.label, row.path, row.includeSubfolders);
                    AgentSetAccessibleName(row.button, SidePanelFolderMenuLineAccessibleName(row.path, row.includeSubfolders));
                }
            }
            // 2. The checks and counts, each written only on a change (a same-value Text write still re-measures the TextBlock).
            const auto counts = _SidePanelFolderMenuCounts();
            for (size_t i = 0; i < _sidePanelFolderMenuRows.size(); ++i)
            {
                auto& row = _sidePanelFolderMenuRows[i];
                if (row.kind != Kind::Folder)
                {
                    continue;
                }
                const std::wstring check = row.checked ? L"\x2713" : L"";
                const std::wstring count = i < counts.size() ? std::to_wstring(counts[i]) : std::wstring{};
                if (row.check && std::wstring_view{ row.check.Text() } != check)
                {
                    row.check.Text(winrt::hstring{ check });
                }
                if (row.count && std::wstring_view{ row.count.Text() } != count)
                {
                    row.count.Text(winrt::hstring{ count });
                }
                if (row.button)
                {
                    AgentSetItemStatus(row.button, row.checked ? L"checked" : L""); // change-gated inside
                }
            }
        }
        catch (...)
        {
            // The submenu shows the previous checks and counts until the next click or open. Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelRefreshFolderMenu");
        }
    }

    void TerminalPage::_SidePanelOnFolderMenuClick(size_t index) noexcept
    {
        using Kind = AgentSidePanelFolderMenuRowView::Kind;
        try
        {
            if (!_SidePanelFolderMenuOpen() || index >= _sidePanelFolderMenuRows.size())
            {
                return; // closed before the tick ran, or the rows were rebuilt meanwhile
            }
            const AgentSidePanelFolderMenuRowView row = _sidePanelFolderMenuRows[index]; // a copy: the panel's close below clears nothing, but a refill would
            if (row.kind == Kind::Pick)
            {
                // The user's "Pick": exactly what By Folder…'s own click does — the panel (and this submenu with it) gives way to
                // the "+" card in folder-pick mode, at the funnel's click. The card focuses its own box on its tick.
                _SidePanelCloseFilterPanel(L"pick (folder menu)");
                _SidePanelOpenFolderCard();
                return;
            }
            // A folder line: judged against the record AS IT IS (the pure rule) — held → dropped, missing → added with the line's
            // choice. _SidePanelSetFilters persists the window's record, rebuilds the cards, and rewrites the panel's and this
            // submenu's checks and counts in place (the lines keep their place).
            auto next = _sidePanelFilters;
            bool turnedOn = false;
            if (!::Agentmaster::SidePanelToggleFolderLine(next, SidePanelLineOf(row), turnedOn))
            {
                return; // not an absolute folder (never, for a line made from the history): nothing to change
            }
            _SidePanelSetFilters(std::move(next),
                                 turnedOn ? (row.includeSubfolders ? L"folder+ (folder menu, with subfolders)" : L"folder+ (folder menu, exactly)") : L"folder- (folder menu)");
            if (turnedOn)
            {
                // The user's "repicking moves to top": a folder turned on here is a pick — on top of the history from the next
                // open (in this window and every other one).
                _SidePanelRememberFolderPick(row.path, row.includeSubfolders);
            }
        }
        catch (...)
        {
            // The click is lost; the submenu shows the record as it is. Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelOnFolderMenuClick");
        }
    }

    std::vector<size_t> TerminalPage::_SidePanelFolderMenuCounts()
    {
        using Kind = AgentSidePanelFolderMenuRowView::Kind;
        std::vector<size_t> counts(_sidePanelFolderMenuRows.size(), 0);
        if (!_sessionRegistry || _sidePanelFolderMenuRows.empty())
        {
            return counts;
        }
        // The lines in their pure form (their keys and choices), by row index; a Pick row stays empty and counts 0.
        std::vector<std::optional<::Agentmaster::SidePanelFolderMenuLine>> lines(_sidePanelFolderMenuRows.size());
        for (size_t i = 0; i < _sidePanelFolderMenuRows.size(); ++i)
        {
            if (_sidePanelFolderMenuRows[i].kind == Kind::Folder)
            {
                lines[i] = SidePanelLineOf(_sidePanelFolderMenuRows[i]);
            }
        }
        // The panel's census walk (_SidePanelFolderChoices'): the scope, the typed text, every facet but By Folder.
        _SidePanelSyncFactCaches();
        const auto mode = _appSettings.tabColorMode;
        const auto snap = _sessionRegistry->SnapshotLiveForCards();
        const auto terms = ::Agentmaster::ParseSessionQuery(_sidePanelSearchText);
        const bool local = !_sidePanelGlobal;
        std::unordered_set<std::wstring> hosted;
        if (local)
        {
            hosted = _HostedSessionIds();
        }
        const auto unread = ::Agentmaster::CurrentUnreadSessionIds();
        const ::Agentmaster::SidePanelFilterMatcher matcher{ _sidePanelFilters, NowMs() };
        for (const auto& s : snap)
        {
            if (!s.live || (local && hosted.find(s.id) == hosted.end()) || !SidePanelSessionMatches(s, terms, mode))
            {
                continue;
            }
            auto f = _SidePanelFilterFactsOf(s, matcher.NeedsTags(), matcher.NeedsFavorite(), unread);
            if (!::Agentmaster::SidePanelPassesAllBut(matcher.Passed(f, true), ::Agentmaster::SidePanelFacet::Folder))
            {
                continue; // another filter (or the text) already hides it: no line would list it
            }
            if (_sidePanelFilters.folders.empty())
            {
                // _SidePanelFilterFactsOf fills the folder keys only while By Folder has a pick; a line needs them always.
                f.launchKey = s.workingDir.empty() ? std::wstring{} : ::Agentmaster::NormDirKey(s.workingDir);
                f.inferredKey = s.inferredWorkingDir.empty() ? std::wstring{} : ::Agentmaster::NormDirKey(s.inferredWorkingDir);
            }
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (lines[i] && ::Agentmaster::SidePanelFolderLineCovers(*lines[i], f))
                {
                    ++counts[i];
                }
            }
        }
        return counts;
    }

    void TerminalPage::_SidePanelPlaceFolderMenu() noexcept
    {
        try
        {
            if (!_SidePanelFolderMenuOpen() || !_sidePanelFolderMenuCard || !_sidePanelFilterPopup || !_sidePanelFilterCard || !_SidePanelFilterPanelOpen())
            {
                return;
            }
            const auto root = Root();
            const double rootW = root ? root.ActualWidth() : 0.0;
            const double rootH = root ? root.ActualHeight() : 0.0;
            // The panel's edges: its popup's offsets (its own placement wrote them, this tick or before) and its measured width.
            _sidePanelFilterCard.Measure(winrt::Windows::Foundation::Size{ kSidePanelLaunchMeasureBound, kSidePanelLaunchMeasureBound });
            const double panelLeft = _sidePanelFilterPopup.HorizontalOffset();
            const double panelTop = _sidePanelFilterPopup.VerticalOffset();
            const double panelRight = panelLeft + static_cast<double>(_sidePanelFilterCard.DesiredSize().Width);
            // By Folder…'s top inside the panel's card, its scroll included; the first row's place (the card's chrome) when the
            // transform cannot be read (a row not laid out yet).
            double rowTop = kSidePanelFolderMenuChromeTop;
            if (const auto byFolder = SidePanelByFolderButton(_sidePanelFilterRows))
            {
                try
                {
                    rowTop = static_cast<double>(byFolder.TransformToVisual(_sidePanelFilterCard).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f }).Y);
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: read By Folder's place"); // the first row's place stands in
                }
            }
            // The rows scroll inside the window's height, like the panel's (a short window): the cap, written only on a change.
            if (const auto scroll = _sidePanelFolderMenuCard.Child().try_as<ScrollViewer>(); scroll && rootH > 0.0)
            {
                const double cap = std::max(120.0, rootH - 2.0 * ::Agentmaster::kPointerCardMarginDips - 10.0);
                if (std::abs(scroll.MaxHeight() - cap) > 0.5)
                {
                    scroll.MaxHeight(cap);
                }
            }
            _sidePanelFolderMenuCard.Measure(winrt::Windows::Foundation::Size{ kSidePanelLaunchMeasureBound, kSidePanelLaunchMeasureBound });
            const auto size = _sidePanelFolderMenuCard.DesiredSize();
            // Beside the panel (its right edge first, else its left), the submenu's first row level with By Folder….
            const auto placed = ::Agentmaster::PlaceSubmenuCard(panelLeft,
                                                                panelRight,
                                                                panelTop + rowTop - kSidePanelFolderMenuChromeTop,
                                                                static_cast<double>(size.Width),
                                                                static_cast<double>(size.Height),
                                                                rootW,
                                                                rootH);
            // Written only on a real move: a re-placement that changes nothing invalidates nothing.
            if (std::abs(_sidePanelFolderMenuPopup.HorizontalOffset() - placed.left) > kSidePanelLaunchPlaceEpsilonPx)
            {
                _sidePanelFolderMenuPopup.HorizontalOffset(placed.left);
            }
            if (std::abs(_sidePanelFolderMenuPopup.VerticalOffset() - placed.top) > kSidePanelLaunchPlaceEpsilonPx)
            {
                _sidePanelFolderMenuPopup.VerticalOffset(placed.top);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: place"); // it keeps its offsets
        }
    }

    void TerminalPage::_SidePanelFolderMenuHover(SidePanelFolderMenuHover what) noexcept
    {
        try
        {
            if (!_SidePanelFilterPanelOpen())
            {
                return; // a row of a closing panel: nothing to open or close
            }
            const bool open = _SidePanelFolderMenuOpen();
            // Arm the one-shot timer for `intent`: created on first use (its tick stops it through the sender, so a page that
            // died meanwhile leaves no running timer), its interval the system's menu delay read now, restarted from zero.
            const auto arm = [this](SidePanelFolderMenuIntent intent) {
                if (!_sidePanelFolderMenuTimer)
                {
                    _sidePanelFolderMenuTimer = winrt::Windows::UI::Xaml::DispatcherTimer{};
                    _sidePanelFolderMenuTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
                        try
                        {
                            if (const auto timer = sender.try_as<winrt::Windows::UI::Xaml::DispatcherTimer>())
                            {
                                timer.Stop(); // one-shot
                            }
                        }
                        catch (...)
                        {
                            ::Agentmaster::AgentLogCaughtException(L"side panel folder menu: stop the hover timer"); // the intent check below still holds
                        }
                        if (const auto page = weak.get())
                        {
                            page->_SidePanelOnFolderMenuTimer();
                        }
                    });
                }
                _sidePanelFolderMenuIntent = intent;
                _sidePanelFolderMenuTimer.Stop();
                _sidePanelFolderMenuTimer.Interval(SidePanelFolderMenuDelay());
                _sidePanelFolderMenuTimer.Start();
            };
            // Drop the pending intent (the pointer changed its mind before the delay ran out).
            const auto cancel = [this]() {
                _sidePanelFolderMenuIntent = SidePanelFolderMenuIntent::None;
                if (_sidePanelFolderMenuTimer)
                {
                    _sidePanelFolderMenuTimer.Stop();
                }
            };
            switch (what)
            {
            case SidePanelFolderMenuHover::EnterByFolder:
                if (open)
                {
                    cancel(); // back on By Folder… with the submenu open: it stays (a pending close from a neighbour row is dropped)
                }
                else
                {
                    arm(SidePanelFolderMenuIntent::Open);
                }
                break;
            case SidePanelFolderMenuHover::LeaveByFolder:
                if (!open && _sidePanelFolderMenuIntent == SidePanelFolderMenuIntent::Open)
                {
                    cancel(); // left before the delay: no submenu (an open one stays: the pointer may be on its way in)
                }
                break;
            case SidePanelFolderMenuHover::EnterOtherRow:
                if (open)
                {
                    arm(SidePanelFolderMenuIntent::Close); // closes unless the pointer reaches the submenu or By Folder… in time
                }
                else if (_sidePanelFolderMenuIntent == SidePanelFolderMenuIntent::Open)
                {
                    cancel();
                }
                break;
            case SidePanelFolderMenuHover::EnterMenu:
                if (_sidePanelFolderMenuIntent == SidePanelFolderMenuIntent::Close)
                {
                    cancel(); // the pointer made it into the submenu: keep it
                }
                break;
            default:
                break;
            }
        }
        catch (...)
        {
            // The hover is lost (the submenu opens by Right, or on the next hover). Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelFolderMenuHover");
        }
    }

    void TerminalPage::_SidePanelOnFolderMenuTimer() noexcept
    {
        const auto intent = _sidePanelFolderMenuIntent;
        _sidePanelFolderMenuIntent = SidePanelFolderMenuIntent::None; // one-shot: consumed whatever happens below
        try
        {
            // Each intent is judged again against the state NOW: the panel may have closed, a key may have opened or closed the
            // submenu while the delay ran.
            if (intent == SidePanelFolderMenuIntent::Open)
            {
                if (_SidePanelFilterPanelOpen() && !_SidePanelFolderMenuOpen())
                {
                    _SidePanelOpenFolderMenu(/*focusFirst*/ false); // noexcept
                }
            }
            else if (intent == SidePanelFolderMenuIntent::Close)
            {
                // The pointer rests on another row of the panel: the keyboard, if the submenu held it, goes back to By Folder….
                _SidePanelCloseFolderMenu(L"hover left", /*focusByFolder*/ true); // noexcept; a no-op when closed
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelOnFolderMenuTimer");
        }
    }

    void TerminalPage::_SidePanelRememberFolderPick(const std::wstring& path, bool includeSubfolders) noexcept
    {
        try
        {
            // A read-modify-write of the profile's freshest filter-folders.json (one list for the app; the process-wide lock
            // inside). The returned history is not needed here: the submenu reads the file again at its next open.
            (void)::Agentmaster::RememberFilterFolderPicks({ ::Agentmaster::SidePanelFolderFilter{ path, includeSubfolders } }, /*onlyMissing*/ false);
        }
        catch (...)
        {
            // The history misses this pick (the filter itself is set). Recorded (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelRememberFolderPick");
        }
    }

    void TerminalPage::_SidePanelShowRowForMode() noexcept
    {
        try
        {
            if (_sidePanelCountText)
            {
                _sidePanelCountText.Visibility(_sidePanelAiMode ? Visibility::Visible : Visibility::Collapsed);
            }
            if (_sidePanelAiMode)
            {
                if (_sidePanelFilterRow)
                {
                    _sidePanelFilterRow.Visibility(Visibility::Collapsed); // the AI view's count line owns row 1
                }
                if (_sidePanelSearchBox)
                {
                    // The box is the AI request line now: the cards' count words are not its status (the old count line
                    // showed the AI view's words in this mode too).
                    AgentSetItemStatus(_sidePanelSearchBox, L"");
                }
            }
            // Card mode: the chips row is shown by the list update that follows (_SidePanelUpdateFilterRow decides).
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelShowRowForMode");
        }
    }

    void TerminalPage::_SidePanelUpdateFilterRow(size_t shown, size_t total, bool textActive)
    {
        if (!_sidePanelFilterRow || !_sidePanelFilterChips)
        {
            return;
        }
        // The old count line's words, unchanged — "2 sessions in this window", "1 of 3 sessions across all windows" — as the
        // filter box's ItemStatus: a screen reader still hears how many cards the box's filtering left, and the e2e driver
        // reads the scope from them (count_line). Change-gated inside AgentSetItemStatus.
        const std::wstring where = SidePanelScopeWords(_sidePanelGlobal);
        const bool narrowed = textActive || ::Agentmaster::SidePanelFiltersActive(_sidePanelFilters);
        const std::wstring countWords = narrowed ? (std::to_wstring(shown) + L" of " + std::to_wstring(total) + L" sessions" + where) :
                                                   (std::to_wstring(total) + (total == 1 ? L" session" : L" sessions") + where);
        if (_sidePanelSearchBox)
        {
            AgentSetItemStatus(_sidePanelSearchBox, countWords);
        }
        // The row shows only while something narrows the list (the user's "a row only shown when filter is on").
        const auto chips = ::Agentmaster::SidePanelFilterChips(_sidePanelFilters, textActive ? SidePanelAiTrim(_sidePanelSearchText) : std::wstring{});
        if (chips.empty())
        {
            if (_sidePanelFilterRow.Visibility() != Visibility::Collapsed)
            {
                _sidePanelFilterRow.Visibility(Visibility::Collapsed);
            }
            if (!_sidePanelFilterRowSig.empty())
            {
                _sidePanelFilterChips.Children().Clear(); // a hidden row keeps no elements (the next filter builds afresh)
                _sidePanelFilterRowSig.clear();
            }
            return;
        }
        // The row's content as one string: the count, the scope and every chip's words. Equal to the last build = nothing to
        // write (the list updates up to twice a second, and the chips change only with a pick).
        std::wstring sig = std::to_wstring(shown) + L"/" + std::to_wstring(total) + where;
        for (const auto& chip : chips)
        {
            sig += L"\x1F" + std::to_wstring(static_cast<int>(chip.kind)) + L":" + chip.text;
        }
        if (sig != _sidePanelFilterRowSig)
        {
            _sidePanelFilterRowSig = sig;
            auto children = _sidePanelFilterChips.Children();
            children.Clear();
            const auto weakThis = get_weak();
            // "N of M": dim, its tooltip naming the scope (the count line's old cue that a short list is "this window").
            {
                auto count = Text(winrt::hstring{ std::to_wstring(shown) + L" of " + std::to_wstring(total) }, kSidePanelChipFontSize, false, 0.6);
                count.VerticalAlignment(VerticalAlignment::Center);
                count.Margin(ThicknessHelper::FromLengths(2, 0, 2, 0));
                AgentSetTip(count, winrt::hstring{ L"Cards listed of the sessions" + where + L" (Card scope in the \x22EF menu)." }, kSidePanelButtonTipDelay);
                AgentSetAutomationId(count, L"am.side.filterRow.count");
                children.Append(count);
            }
            // One chip per filter: [glyph][words][✕]. Its rounded background is a CHILDLESS Border behind the content in the same
            // Grid cell (a rounded element with children is a composition node every frame walks — UI_THREAD.md §10h).
            size_t folderOrdinal = 0;
            for (const auto& chip : chips)
            {
                Grid cell{};
                Border back{};
                back.Background(Fill(0xFF, 0x3B, 0x3B, 0x3B));
                back.BorderBrush(Fill(0xFF, 0x56, 0x56, 0x56));
                back.BorderThickness(ThicknessHelper::FromUniformLength(1));
                back.CornerRadius(CornerRadiusHelper::FromUniformRadius(kSidePanelChipCornerRadius));
                cell.Children().Append(back);
                StackPanel content{};
                content.Orientation(Orientation::Horizontal);
                content.Spacing(4);
                content.Margin(ThicknessHelper::FromLengths(7, 1, 1, 1));
                content.VerticalAlignment(VerticalAlignment::Center);
                // The glyph of the facet (the state's dot, the first tag's ribbon, the icon font for the rest).
                UIElement glyph{ nullptr };
                switch (chip.kind)
                {
                case ::Agentmaster::SidePanelChipKind::Folder:
                    glyph = SidePanelFluentGlyph(kSidePanelFolderGlyph, 10, 0.85);
                    break;
                case ::Agentmaster::SidePanelChipKind::State:
                {
                    auto stateDot = Text(L"\x25CF", 9, false, 1.0);
                    for (const auto& choice : ::Agentmaster::kSidePanelStateChoices)
                    {
                        if ((_sidePanelFilters.states & choice.bit) != 0)
                        {
                            stateDot.Foreground(SolidColorBrush{ StateColor(choice.state) }); // the first picked state's color
                            break;
                        }
                    }
                    glyph = stateDot;
                    break;
                }
                case ::Agentmaster::SidePanelChipKind::Tag:
                    glyph = _sidePanelFilters.tags.empty() ? SidePanelMakeTagRibbon(Color{ 0xFF, 0x55, 0x55, 0x55 }) :
                                                             SidePanelMakeTagRibbon(ResolveTagDisplayColor(_sidePanelFilters.tags.front(), _sidePanelTagColors));
                    break;
                case ::Agentmaster::SidePanelChipKind::Draft:
                    glyph = Text(L"\x2022\x2022\x2022", 9, true, 0.9);
                    break;
                case ::Agentmaster::SidePanelChipKind::Unread:
                {
                    auto ring = Text(L"\x25CB", 10, true, 1.0);
                    ring.Foreground(SolidColorBrush{ Color{ 0xFF, 0xE5, 0x39, 0x35 } });
                    glyph = ring;
                    break;
                }
                case ::Agentmaster::SidePanelChipKind::Pinned:
                    glyph = SidePanelFluentGlyph(kSidePanelPinGlyph, 9, 0.9);
                    break;
                case ::Agentmaster::SidePanelChipKind::Favorite:
                {
                    auto star = Text(L"\x2605", 10, false, 1.0);
                    star.Foreground(SolidColorBrush{ Color{ 0xFF, 0xF5, 0xC5, 0x18 } });
                    glyph = star;
                    break;
                }
                case ::Agentmaster::SidePanelChipKind::Active:
                    glyph = SidePanelFluentGlyph(kSidePanelClockGlyph, 9, 0.85);
                    break;
                case ::Agentmaster::SidePanelChipKind::Text:
                default:
                    glyph = SidePanelFluentGlyph(kSidePanelSearchGlyph, 9, 0.85);
                    break;
                }
                if (const auto fe = glyph.try_as<FrameworkElement>())
                {
                    fe.VerticalAlignment(VerticalAlignment::Center);
                }
                content.Children().Append(glyph);
                auto words = Text(winrt::hstring{ chip.text }, kSidePanelChipFontSize, false, 0.95);
                words.VerticalAlignment(VerticalAlignment::Center);
                words.TextTrimming(TextTrimming::CharacterEllipsis);
                words.MaxWidth(180); // a long folder or tag list ends in "…"; the chip's tooltip has it whole
                content.Children().Append(words);
                // ✕: drops what this chip stands for — the chip is captured BY VALUE (its kind, its folder's index, its words),
                // never an element; the page weakly.
                Button remove{};
                remove.Content(winrt::box_value(L"\x2715"));
                remove.FontSize(8);
                remove.Padding(ThicknessHelper::FromLengths(4, 0, 4, 0));
                remove.MinWidth(0);
                remove.MinHeight(0);
                remove.VerticalAlignment(VerticalAlignment::Center);
                remove.Background(SolidColorBrush{ Colors::Transparent() });
                remove.BorderThickness(ThicknessHelper::FromUniformLength(0));
                remove.CornerRadius(CornerRadiusHelper::FromUniformRadius(0));
                remove.IsTabStop(false); // like the header's buttons: a click must not pull the keyboard out of the terminal
                remove.Click([weakThis, chip](const IInspectable&, const RoutedEventArgs&) {
                    const auto self = weakThis.get();
                    if (!self)
                    {
                        return;
                    }
                    try
                    {
                        // On a clean tick: the row is rebuilt by the list update this triggers, and the clicked ✕ must not
                        // leave the tree inside its own Click.
                        self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, chip]() {
                            const auto page = weakThis.get();
                            if (!page)
                            {
                                return;
                            }
                            if (chip.kind == ::Agentmaster::SidePanelChipKind::Text)
                            {
                                page->_SidePanelClearFilterText();
                                ::Agentmaster::LogNav(L"sidepanel filter chip x text");
                                return;
                            }
                            // A Folder chip whose index no longer names its folder (the record changed since the row was drawn —
                            // never between a draw and a click on one thread, but cheap to check) drops nothing.
                            if (chip.kind == ::Agentmaster::SidePanelChipKind::Folder &&
                                (chip.folderIndex >= page->_sidePanelFilters.folders.size() ||
                                 ::Agentmaster::SidePanelFolderLeaf(page->_sidePanelFilters.folders[chip.folderIndex].path) + (page->_sidePanelFilters.folders[chip.folderIndex].includeSubfolders ? L"" : L" (exactly)") != chip.text))
                            {
                                return;
                            }
                            page->_SidePanelSetFilters(::Agentmaster::SidePanelFiltersWithoutChip(page->_sidePanelFilters, chip), L"chip x");
                        });
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"side panel filter chip: queue the remove"); // the click is lost
                    }
                });
                AgentSetTip(remove, winrt::hstring{ chip.kind == ::Agentmaster::SidePanelChipKind::Text ? std::wstring{ L"Empty the filter box." } : L"Drop this filter." }, kSidePanelButtonTipDelay);
                content.Children().Append(remove);
                cell.Children().Append(content);
                // The chip's whole meaning in its tooltip (on the cell: the ✕ keeps its own).
                AgentSetTip(cell, winrt::hstring{ chip.tip }, kSidePanelButtonTipDelay);
                // Ids: am.side.chip.<kind>[.<n>] — n counts the folder chips from 0 — and the ✕ beside it, ".remove".
                std::wstring id = L"am.side.chip.";
                switch (chip.kind)
                {
                case ::Agentmaster::SidePanelChipKind::Folder:
                    id += L"folder." + std::to_wstring(folderOrdinal++);
                    break;
                case ::Agentmaster::SidePanelChipKind::State:
                    id += L"state";
                    break;
                case ::Agentmaster::SidePanelChipKind::Tag:
                    id += L"tag";
                    break;
                case ::Agentmaster::SidePanelChipKind::Draft:
                    id += L"draft";
                    break;
                case ::Agentmaster::SidePanelChipKind::Unread:
                    id += L"unread";
                    break;
                case ::Agentmaster::SidePanelChipKind::Pinned:
                    id += L"pinned";
                    break;
                case ::Agentmaster::SidePanelChipKind::Favorite:
                    id += L"favorite";
                    break;
                case ::Agentmaster::SidePanelChipKind::Active:
                    id += L"active";
                    break;
                case ::Agentmaster::SidePanelChipKind::Text:
                default:
                    id += L"text";
                    break;
                }
                AgentSetAutomationId(words, id);
                AgentSetAutomationId(remove, id + L".remove");
                AgentSetAccessibleName(remove, L"Remove the filter " + chip.text);
                children.Append(cell);
            }
            // Clear: every filter and the typed text (the panel's "Clear all filters").
            {
                Button clear{};
                auto words = Text(L"Clear", kSidePanelChipFontSize, false, 1.0);
                words.Foreground(SolidColorBrush{ kSidePanelLinkColor });
                clear.Content(words);
                clear.Padding(ThicknessHelper::FromLengths(4, 0, 4, 0));
                clear.MinWidth(0);
                clear.MinHeight(0);
                clear.VerticalAlignment(VerticalAlignment::Center);
                clear.Background(SolidColorBrush{ Colors::Transparent() });
                clear.BorderThickness(ThicknessHelper::FromUniformLength(0));
                clear.CornerRadius(CornerRadiusHelper::FromUniformRadius(0));
                clear.IsTabStop(false);
                clear.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
                    const auto self = weakThis.get();
                    if (!self)
                    {
                        return;
                    }
                    try
                    {
                        self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                            if (const auto page = weakThis.get())
                            {
                                page->_SidePanelClearFilterText();
                                page->_SidePanelSetFilters(::Agentmaster::SidePanelFilters{}, L"clear");
                            }
                        });
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"side panel filter row: queue Clear"); // the click is lost
                    }
                });
                AgentSetTip(clear, winrt::hstring{ L"Drop every filter and empty the filter box." }, kSidePanelButtonTipDelay);
                AgentSetAutomationId(clear, L"am.side.filterRow.clear");
                children.Append(clear);
            }
        }
        if (_sidePanelFilterRow.Visibility() != Visibility::Visible)
        {
            _sidePanelFilterRow.Visibility(Visibility::Visible);
        }
    }

    // ======================================================================================
    // The side panel's AI SEARCH (SIDE_PANEL.md §2h)
    // ======================================================================================
    // WHAT: the filter box's "AI" toggle (inside the box, at its right end — the user's choice of 2026-10-31) turns the box
    // into a REQUEST line and replaces the card list with the AI view: a strip (the request, the model's plan while it works,
    // then its answer; "thinking <level>" and the context size; ⊘ to start over, and the copy button under it) over the model's
    // list of sessions, each a side panel card with the model's evidence (its "why") under it. The summary area stays below and
    // works with it: a result's click peeks its summary like a card's. Clicking AI again brings the cards back; the
    // conversation is kept.
    // ⊘ AND THE COPY BUTTON (the user's two requests of 2026-11-10, AgentMaster/SidePanelAiCopy.h): ⊘ puts the newest request
    // back into an EMPTY box (running or answered), so Enter asks it again in the new conversation; the copy button copies the
    // whole conversation and the listed sessions as Markdown, and refills an empty box by the same rule.
    // WHILE A REQUEST RUNS (the user's two requests of 2026-11-05): a loading icon spins in the strip's dot place
    // (_SidePanelAiShowWorking), and the list does NOT change — not for a search's candidates, not for a show the model may
    // still revise; it changes when the request ends, to the conversation's final list (AgentMaster/SidePanelAiList.h). Until
    // then the last answer's cards stay, dimmed.
    // WHOSE CONVERSATION: the process's ONE search agent (SharedSessionSearchAgent) — the Sessions page's. Every surface is a view
    // of it, so a request asked here shows in a Sessions page with its AI search on, and the reverse.
    // LIFECYCLE (the user's "only after a request" rule, with a configurable park): a window that shows the AI view starts no
    // process. The first click into the box (or a request) starts it; the end of THIS window's request, the box losing focus with
    // nothing running, leaving AI mode and the window closing each arm its park after AppSettings::sessionsAiParkMinutes (2 hours
    // by default) — unless a Sessions page shows the AI search, whose own rule (the process lives while a page shows it) then
    // holds. A park ends only the process; the next request continues from a digest of the conversation.
    // COST: the view is built on the first AI-on; the result cards are a handful (the model shows 1 to 8 as a rule), rebuilt per
    // list and patched in place by the periodic tick; while the view shows, the card list is deferred like a collapsed one.

    bool TerminalPage::_SidePanelCardsHidden() const noexcept
    {
        return _sidePanelCollapsed || _sidePanelAiMode;
    }

    void TerminalPage::_SidePanelUpdateAiButton()
    {
        if (!_sidePanelAiBtn)
        {
            return;
        }
        // A programmatic IsChecked raises no Click, so re-pinning can never loop back into the toggle or persist anything.
        _sidePanelAiBtn.IsChecked(_sidePanelAiMode);
    }

    void TerminalPage::_SidePanelAiFitToggleIntoBox()
    {
        if (!_sidePanelSearchBox || (_sidePanelAiBoxPadded && _sidePanelAiClearButtonMoved))
        {
            return;
        }
        try
        {
            if (!_sidePanelSearchBox.IsLoaded())
            {
                return; // before Loaded the style's padding is not known yet: the Loaded hook tries again
            }
            if (!_sidePanelAiBoxPadded)
            {
                // Read from the box, not hard-coded: the box's own tight padding (8,2,6,3) stays what it is, and the text and
                // the placeholder now end before the toggle.
                auto padding = _sidePanelSearchBox.Padding();
                padding.Right += kSidePanelAiToggleReserve;
                _sidePanelSearchBox.Padding(padding);
                _sidePanelAiBoxPadded = true; // set once applied: a second Loaded must not add the room again
            }
            if (!_sidePanelAiClearButtonMoved)
            {
                // The box's own clear button (the template part "DeleteButton", the X shown while the box has focus and text)
                // sits in the template's last column; shifted left by the reserve it stands left of the toggle (text | X | AI),
                // and a render transform moves its hit area with its picture. A template without the part leaves the X under
                // the toggle — cosmetic only (the toggle takes the click) — and every later call tries again.
                if (const auto clear = SidePanelFindDescendant(_sidePanelSearchBox, L"DeleteButton"))
                {
                    TranslateTransform shift;
                    shift.X(-kSidePanelAiToggleReserve);
                    clear.RenderTransform(shift);
                    _sidePanelAiClearButtonMoved = true;
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiFitToggleIntoBox"); // the next Loaded / GotFocus / TextChanged tries again
        }
    }

    void TerminalPage::_SidePanelSetAiMode(bool on, bool userAction)
    {
        if (!_sidePanelBuilt)
        {
            return;
        }
        try
        {
            if (on == _sidePanelAiMode)
            {
                _SidePanelUpdateAiButton(); // keep the button honest after a repeated or refused change
                return;
            }
            if (on && userAction && !::Agentmaster::EnsureClaudeAvailable())
            {
                // The AI search runs through the native claude.exe: without one, "on" would show a view that can never
                // answer. The page's own install notice says what to do (the Sessions page's rule). A window that OPENS in
                // AI mode is not refused: it starts no process, and a request then shows the notice.
                ::Agentmaster::LogNav(L"sidepanel ai on (blocked: no native claude.exe)");
                _SidePanelUpdateAiButton();
                _PromptClaudeMissing();
                return;
            }
            if (on)
            {
                // First the steps that can fail for a reason other than memory: the view, and the listener — whose first call
                // creates the process's agent object (no process). A mode that is on without either would show a view that
                // never changes; a throw here leaves the mode off (the catch below).
                _SidePanelAiEnsureView();
                _SidePanelAiAttach();
            }
            _sidePanelAiMode = on;
            // The box KEEPS its text through the switch, both ways — the user's rule for the AI search box (2026-10-31, the
            // Sessions page's 7d8ed1043: "the box is one box: the prompt line with AI on, the search box with AI off"). Only
            // its placeholder and tip follow the mode (a placeholder shows only while the box is empty), and only Enter sends
            // or empties it. ⚠ Never empty the box here: that loses what the user typed. The nav line carries the box's
            // LENGTH (never its words, which stay out of the nav trail), so a live check can read that the text survived.
            if (userAction)
            {
                const uint32_t boxChars = _sidePanelSearchBox ? _sidePanelSearchBox.Text().size() : 0u;
                ::Agentmaster::LogNav(std::wstring{ on ? L"sidepanel ai on box=" : L"sidepanel ai off box=" } + std::to_wstring(boxChars));
            }
            _SidePanelUpdateAiButton();
            // The FILTER follows the box's meaning, not its text. On: the card filter is dropped (TextChanged ignores the box in
            // AI mode, so a kept filter would narrow the cards by a text that is now a request). Off: the kept text filters the
            // cards at once, as if just typed — no TextChanged comes, since the text did not change at the switch.
            _sidePanelSearchText = (!on && _sidePanelSearchBox) ? std::wstring{ _sidePanelSearchBox.Text() } : std::wstring{};
            if (_sidePanelSearchBox)
            {
                _sidePanelSearchBox.PlaceholderText(on ? kSidePanelAiPlaceholder : kSidePanelFilterPlaceholder);
                // The 3 s rest delay again (kSidePanelButtonTipDelay): the element keeps the delay it was given at the build, so
                // this is a belt — the request line's tip must never open at ~133 ms if the build's call ever changes.
                AgentSetTitledTip(_sidePanelSearchBox, winrt::hstring{ on ? kSidePanelAiBoxTipTitle : kSidePanelFilterTipTitle }, winrt::hstring{ on ? kSidePanelAiBoxTip : kSidePanelFilterTip }, kSidePanelButtonTipDelay);
            }
            // The two views share the cards area's one cell: exactly one is visible.
            if (_sidePanelCardsScroll)
            {
                _sidePanelCardsScroll.Visibility(on ? Visibility::Collapsed : Visibility::Visible);
            }
            if (_sidePanelAiRoot)
            {
                _sidePanelAiRoot.Visibility(on ? Visibility::Visible : Visibility::Collapsed);
            }
            // A Collapsed element stays loaded, so the covered session cards' status icons would keep pulsing, blinking and
            // glowing for nobody (the collapse's rule, SIDE_PANEL.md §2g): their host gate follows the cover.
            for (const auto& [cardId, view] : _sidePanelCardViews)
            {
                if (view.glyph)
                {
                    view.glyph->SetHostShown(!_SidePanelCardsHidden());
                }
            }
            if (!on)
            {
                // The strip's "agent is running" motion goes with the view: a request may still run, but nobody sees the strip
                // now. An AI-on starts it again through the strip repaint below when something runs. Never throws.
                _sidePanelAiWorking.SetRunning(false);
            }
            // The count line belongs to whichever view shows: each writes it from scratch after a switch (their "last written"
            // caches would otherwise skip a text that the other view overwrote).
            _sidePanelCountValue.clear();
            _sidePanelAiCountValue.clear();
            _sidePanelFilterRowSig.clear(); // the chips row is redrawn by the card list's catch-up (SIDE_PANEL.md §2l)
            // Row 1 shows the element of the mode (SIDE_PANEL.md §2l): the AI view's count line with AI on, the chips row (when
            // a filter is on) with AI off. The funnel's panel and the folder card close with an AI-on: the view they filter is
            // covered (their filters still apply when the cards come back).
            _SidePanelShowRowForMode();
            if (on)
            {
                _SidePanelCloseFilterSurfaces(L"AI search on");
                auto& agent = ::Agentmaster::SharedSessionSearchAgent();
                const auto snapshot = agent.Snapshot();
                _sidePanelAiEpoch = snapshot.epoch;
                _SidePanelAiAdoptSnapshot(snapshot);
                _SidePanelAiRebuildResults();
                _SidePanelAiUpdateStrip(snapshot);
                _SidePanelAiUpdateCountLine(snapshot);
            }
            else
            {
                _SidePanelAiClearResultCards();
                _sidePanelAiShownIds.clear();
                _sidePanelAiReasons.clear();
                _sidePanelAiEntries.clear();
                _sidePanelAiHasShown = false;
                // The view drops the agent's events while it is off, so a count it can no longer follow is dropped too; the next
                // AI-on starts at none (the strip says only what it saw).
                _sidePanelAiCandidateCount = 0;
                // The loading icon goes with the view: a ring left active under a collapsed view would keep the compositor
                // animating for nobody (the next AI-on repaints the strip, which shows it again while something runs).
                _SidePanelAiShowWorking(false);
                ++_sidePanelAiListSeq; // a disk fetch still in flight now lands for a list that is gone, and is dropped
                // The card list catches up at once: every update deferred while the view covered it (registry changes, tag
                // edits, the sort, a settings Save) runs now, and an owed ⌖ reveal is paid on the list the user sees.
                _SidePanelRebuildCards();
                _SidePanelQueueReveal();
                // Leaving the AI view is "not using the search": the caret may stay in the box (now a filter), so no LostFocus
                // would arm the park — arm it here (a request running, or a Sessions page showing the search, keeps it).
                _SidePanelAiArmPark(L"the AI view closed");
            }
            if (userAction)
            {
                // Both halves of the "Sum" contract: this window's record (the autosave reads the live member) and the
                // new-window default (a granular freshest-disk RMW of that one field).
                _ScheduleWindowRecordSave();
                _SidePanelPersistDefaults(/*width*/ false, /*split*/ false, /*collapsed*/ false, /*summaryHidden*/ false, /*aiMode*/ true);
                if (on && _sidePanelSearchBox)
                {
                    // The user who turns it on is about to ask: the caret waits in the box — where the user left it, after any
                    // kept text (a programmatic focus selects nothing), so typing goes on with that text. Its GotFocus starts
                    // the search's process — the click asked to search: the "first click into the box" of the lifecycle rule.
                    _sidePanelSearchBox.Focus(FocusState::Programmatic);
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelSetAiMode");
            // Keep the button honest: it must not read a mode that did not happen.
            try
            {
                _SidePanelUpdateAiButton();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_SidePanelSetAiMode: restore the toggle");
            }
        }
    }

    void TerminalPage::_SidePanelAiEnsureView()
    {
        if (_sidePanelAiRoot || !_sidePanelCardsHost)
        {
            return;
        }
        const auto weakThis = get_weak();
        Grid root{};
        {
            RowDefinition stripRow{};
            stripRow.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            RowDefinition resultsRow{};
            resultsRow.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            root.RowDefinitions().Append(stripRow);
            root.RowDefinitions().Append(resultsRow);
        }

        // THE STRIP: the Sessions page's strip, laid out for a narrow panel. Its rounded look is a CHILDLESS back Border behind the
        // content in one Grid cell (the card's rule, UI_THREAD.md §10h): a rounded element with children would be a composition
        // node every frame walks.
        Grid strip{};
        strip.Margin(ThicknessHelper::FromLengths(4, 4, 4, 4));
        Border stripBack{};
        stripBack.Background(Fill(0xFF, 0x26, 0x26, 0x26));
        stripBack.BorderBrush(Fill(0xFF, 0x3A, 0x3A, 0x3A));
        stripBack.BorderThickness(ThicknessHelper::FromUniformLength(1));
        stripBack.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
        strip.Children().Append(stripBack); // first: drawn behind
        Grid inner{};
        inner.Margin(ThicknessHelper::FromLengths(8, 5, 4, 5));
        {
            ColumnDefinition dotCol{};
            dotCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition linesCol{};
            linesCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition resetCol{};
            resetCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            inner.ColumnDefinitions().Append(dotCol);
            inner.ColumnDefinitions().Append(linesCol);
            inner.ColumnDefinitions().Append(resetCol);
        }
        // The state dot: a CHILDLESS rounded Border (no node), top-aligned with the first line.
        _sidePanelAiDot = Border{};
        _sidePanelAiDot.Width(8);
        _sidePanelAiDot.Height(8);
        _sidePanelAiDot.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        _sidePanelAiDot.Margin(ThicknessHelper::FromLengths(0, 4, 7, 0));
        _sidePanelAiDot.VerticalAlignment(VerticalAlignment::Top);
        _sidePanelAiDot.Background(Fill(0xFF, 0x80, 0x80, 0x80));
        Grid::SetColumn(_sidePanelAiDot, 0);
        inner.Children().Append(_sidePanelAiDot);
        // The LOADING icon (the user's request of 2026-11-05: "a loading icon while the assistant is running"): a spinning ring in
        // the dot's cell, shown instead of the dot while a request runs or the process starts (_SidePanelAiShowWorking; exactly
        // one of the two shows). WinUI's ProgressRing — the one the tab header and the Sessions page's summary loader use — draws
        // its indeterminate spin as a Composition animation (an AnimatedVisualPlayer), not a XAML storyboard, so it keeps no XAML
        // frame loop running (the looping-animation rule, UI_THREAD.md); it is ACTIVE only while it shows, so it never animates
        // for nobody. Its size and margins give it the dot's footprint and center (kSidePanelAiSpinnerSize), so the text never
        // shifts when one replaces the other. Not a tab stop and not hit-testable (the style's own defaults): the status text
        // says what runs.
        _sidePanelAiSpinner = winrt::Microsoft::UI::Xaml::Controls::ProgressRing{};
        _sidePanelAiSpinner.Width(kSidePanelAiSpinnerSize);
        _sidePanelAiSpinner.Height(kSidePanelAiSpinnerSize);
        _sidePanelAiSpinner.MinWidth(0.0); // the style's 16 DIP floor would win over the size (the tab header's ring clears it too)
        _sidePanelAiSpinner.MinHeight(0.0);
        _sidePanelAiSpinner.Margin(ThicknessHelper::FromLengths(0, 2, 3, 0));
        _sidePanelAiSpinner.VerticalAlignment(VerticalAlignment::Top); // with the first line, like the dot
        _sidePanelAiSpinner.HorizontalAlignment(HorizontalAlignment::Left);
        _sidePanelAiSpinner.Foreground(Fill(0xFF, 0x4E, 0xA1, 0xFF)); // the strip's working blue (the dot's busy color, the status ink's hue)
        _sidePanelAiSpinner.IsActive(false);
        _sidePanelAiSpinner.Visibility(Visibility::Collapsed); // the strip's first repaint shows it when something runs
        Grid::SetColumn(_sidePanelAiSpinner, 0);
        inner.Children().Append(_sidePanelAiSpinner);

        StackPanel lines{};
        lines.Spacing(2);
        _sidePanelAiQuestionText = Text(L"", 10, false, 0.6);
        _sidePanelAiQuestionText.TextTrimming(TextTrimming::CharacterEllipsis); // one line: the whole request is in the conversation
        _sidePanelAiQuestionText.Visibility(Visibility::Collapsed); // until there is a request
        lines.Children().Append(_sidePanelAiQuestionText);
        _sidePanelAiStatusText = Text(L"", 11, false, 0.95);
        _sidePanelAiStatusText.TextWrapping(TextWrapping::Wrap);
        _sidePanelAiStatusText.MaxLines(0); // every line: the plan is several numbered lines, the answer up to ~1,200 characters
        _sidePanelAiStatusText.IsTextSelectionEnabled(true); // the answer can be selected and copied
        ScrollViewer statusScroll{};
        statusScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        statusScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        statusScroll.MaxHeight(kSidePanelAiStatusMaxHeight); // a long answer scrolls inside the strip instead of pushing the results away
        statusScroll.Content(_sidePanelAiStatusText);
        lines.Children().Append(statusScroll);
        // The footer: "thinking <level>  ctx 18K / 200K" — the Sessions strip's right end, under the text here (a narrow panel
        // has no room beside it).
        StackPanel footer{};
        footer.Orientation(Orientation::Horizontal);
        _sidePanelAiThinkingText = Text(L"thinking low", 10, false, 0.6); // a placeholder: the footer shows the agent's level once the AI view is on
        _sidePanelAiThinkingBtn = Button{};
        _sidePanelAiThinkingBtn.Content(_sidePanelAiThinkingText);
        _sidePanelAiThinkingBtn.Padding(ThicknessHelper::FromLengths(4, 0, 4, 1));
        _sidePanelAiThinkingBtn.MinWidth(0.0);
        _sidePanelAiThinkingBtn.MinHeight(0.0);
        // FLAT at rest (local values only the rest state uses: the stock hover / pressed fills still show), as small as the
        // strip's text, so the footer keeps its height.
        _sidePanelAiThinkingBtn.Background(SolidColorBrush{ Colors::Transparent() });
        _sidePanelAiThinkingBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        _sidePanelAiThinkingBtn.CornerRadius(CornerRadiusHelper::FromUniformRadius(3));
        _sidePanelAiThinkingBtn.IsTabStop(false);
        AgentSetTitledTip(_sidePanelAiThinkingBtn, L"Thinking", winrt::hstring{ L"How much the model thinks before it searches. Click for the next level: low \x2192 medium \x2192 high \x2192 xhigh \x2192 max \x2192 low.\nEvery level runs adaptive thinking at that effort; low is the fastest, and deeper answers harder requests better and takes longer. There is no off: with thinking off, every request fails.\nIt applies from your next request, in every window and on the Sessions page (one conversation), and is kept after a restart." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        _sidePanelAiThinkingBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            try
            {
                // On a clean tick (the pointer-handler rule). The Sessions page's own rotation: it persists the next level and
                // gives it to the agent, whose StateChanged event repaints every surface's strip — this one included.
                self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    if (const auto page = weakThis.get())
                    {
                        page->_CycleSessionsAiThinking();
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel AI search: queue the thinking rotation");
            }
        });
        footer.Children().Append(_sidePanelAiThinkingBtn);
        _sidePanelAiUsageText = Text(L"", 10, false, 0.5);
        _sidePanelAiUsageText.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
        _sidePanelAiUsageText.VerticalAlignment(VerticalAlignment::Center);
        AgentSetTitledTip(_sidePanelAiUsageText, L"Conversation size", winrt::hstring{ L"How much the AI search's conversation holds, in tokens, of the most it can hold before Claude Code compacts it by itself. A new subject is better served by a new conversation: \x2298." });
        footer.Children().Append(_sidePanelAiUsageText);
        lines.Children().Append(footer);
        Grid::SetColumn(lines, 1);
        inner.Children().Append(lines);

        // ⊘ — a new conversation. A FontIcon, not a TextBlock: a TextBlock keeps room for descenders and the glyph sits high.
        _sidePanelAiResetBtn = Button{};
        {
            FontIcon icon;
            // Qualified: inside a TerminalPage member the bare name is the page's own FontFamily property.
            icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ L"Segoe UI Symbol" });
            icon.Glyph(L"\x2298"); // CIRCLED DIVISION SLASH, the Sessions page's reset glyph
            icon.FontSize(13);
            _sidePanelAiResetBtn.Content(icon);
        }
        _sidePanelAiResetBtn.Padding(ThicknessHelper::FromLengths(4, 0, 4, 0));
        _sidePanelAiResetBtn.MinWidth(0.0);
        _sidePanelAiResetBtn.MinHeight(0.0);
        _sidePanelAiResetBtn.Background(SolidColorBrush{ Colors::Transparent() });
        _sidePanelAiResetBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        _sidePanelAiResetBtn.VerticalAlignment(VerticalAlignment::Top);
        _sidePanelAiResetBtn.HorizontalAlignment(HorizontalAlignment::Center); // one center line with the copy button under it
        _sidePanelAiResetBtn.IsTabStop(false);
        AgentSetTitledTip(_sidePanelAiResetBtn, L"New conversation", winrt::hstring{ L"Throw the AI search's conversation away and start a new one: the model forgets what you asked and what it showed. Use it when you start looking for something else \x2014 a follow-up refers to the list before it. What you typed in the box stays: Enter sends it to the new conversation. An empty box gets your newest request back (running or answered), so Enter asks it again, now in the new conversation.\nIt clears the conversation everywhere (every window, the Sessions page). Your sessions are not touched." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        _sidePanelAiResetBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            try
            {
                self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    const auto page = weakThis.get();
                    if (!page)
                    {
                        return;
                    }
                    try
                    {
                        // A reset throws the CONVERSATION away, never the box's text: what is typed there is often the new
                        // request itself, written before the user saw it would read as a follow-up (the user's rule of
                        // 2026-10-31, the Sessions page's ⊘ too). An EMPTY box gets the conversation's newest request back
                        // instead, running or answered (the user's request and pick of 2026-11-10, SidePanelAiCopy.h): who
                        // starts over usually asks the same thing again, in a conversation that no longer reads it as a
                        // follow-up. ⚠ Read it BEFORE the Reset, which clears it; the Reset event's path never writes the box.
                        // The nav line says how much the box held and how much came back — lengths only, never the words.
                        auto& agent = ::Agentmaster::SharedSessionSearchAgent();
                        const std::wstring boxText = page->_sidePanelSearchBox ? std::wstring{ page->_sidePanelSearchBox.Text() } : std::wstring{};
                        const std::wstring refill = page->_sidePanelAiMode ? ::Agentmaster::SidePanelAiRefillText(boxText, agent.Snapshot().question) : std::wstring{};
                        ::Agentmaster::LogNav(L"sidepanel ai-reset box=" + std::to_wstring(boxText.size()) + L" refill=" + std::to_wstring(refill.size()));
                        // The Reset event clears this view (and every other surface's). A reset restarts a process that was up;
                        // with nobody asking afterwards it must still end: the park, as after a request.
                        agent.Reset();
                        page->_SidePanelAiArmPark(L"the conversation was reset");
                        page->_SidePanelAiWriteRefill(refill); // "" writes nothing: a box that held text keeps it
                        if (page->_sidePanelSearchBox && page->_sidePanelAiMode)
                        {
                            page->_sidePanelSearchBox.Focus(FocusState::Programmatic); // the user who resets is about to ask again: Enter sends the box's text to the new conversation
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"side panel AI search: reset");
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel AI search: queue the reset");
            }
        });
        // The COPY button, under ⊘ (the user's request of 2026-11-10: "a copy icon below the circle icon that copies to
        // clipboard the prompt user sent + assistant answers and additionally, if prompt box is empty, copy the prompt sent to
        // prompt box"; their pick of four layouts: the whole conversation and the listed sessions). Styled as ⊘ — flat at rest
        // (local values: the stock hover / pressed fills still show), no tab stop (a click must never pull the keyboard out of
        // the terminal) — and its work runs on a clean tick (the pointer-handler rule). A FontIcon for the same reason as ⊘'s.
        _sidePanelAiCopyBtn = Button{};
        {
            FontIcon icon;
            icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ kSidePanelFluentFont });
            icon.Glyph(kSidePanelAiCopyGlyph);
            icon.FontSize(kSidePanelIconFontSize);
            _sidePanelAiCopyBtn.Content(icon);
        }
        _sidePanelAiCopyBtn.Padding(ThicknessHelper::FromLengths(4, 2, 4, 2));
        _sidePanelAiCopyBtn.MinWidth(0.0);
        _sidePanelAiCopyBtn.MinHeight(0.0);
        _sidePanelAiCopyBtn.Background(SolidColorBrush{ Colors::Transparent() });
        _sidePanelAiCopyBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        _sidePanelAiCopyBtn.HorizontalAlignment(HorizontalAlignment::Center);
        _sidePanelAiCopyBtn.IsTabStop(false);
        AgentSetTitledTip(_sidePanelAiCopyBtn, L"Copy the conversation", winrt::hstring{ L"Copy this AI search conversation to the clipboard as Markdown: every request you sent with Claude's reply to it, then the sessions listed now, each with its state, folder, id and why.\nWhen the box is empty, your newest request also goes back into it, so Enter asks it again." }, kSidePanelButtonTipDelay); // opens after 3 s of rest
        _sidePanelAiCopyBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            try
            {
                self->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    if (const auto page = weakThis.get())
                    {
                        page->_SidePanelAiCopyConversation(); // never throws: a failure is logged and copies nothing
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel AI search: queue the copy"); // this click copies nothing; the next one tries again
            }
        });
        // The strip's right column: ⊘ over the copy button, top-aligned with the first line as ⊘ alone was.
        StackPanel actions{};
        actions.Orientation(Orientation::Vertical);
        actions.Spacing(2);
        actions.VerticalAlignment(VerticalAlignment::Top);
        actions.Children().Append(_sidePanelAiResetBtn);
        actions.Children().Append(_sidePanelAiCopyBtn);
        Grid::SetColumn(actions, 2);
        inner.Children().Append(actions);
        strip.Children().Append(inner); // second: over the back
        // THE SWEEP HOST: a 2 DIP strip on the back's bottom edge, over the lines, that the "agent is running" light draws on
        // (AgentSweepBar: composition visuals of its own, so XAML neither lays out nor hit-tests the moving light). A layer of
        // its own in the strip's cell, so the lines' layout never changes when the light runs. Not hit-testable: a press there
        // belongs to the strip, never to an empty Border.
        Border sweepHost{};
        sweepHost.Height(kAgentSweepBarHeight);
        sweepHost.VerticalAlignment(VerticalAlignment::Bottom);
        sweepHost.HorizontalAlignment(HorizontalAlignment::Stretch);
        sweepHost.Margin(ThicknessHelper::FromLengths(kSidePanelAiSweepInsetX, 0, kSidePanelAiSweepInsetX, kSidePanelAiSweepInsetBottom));
        sweepHost.IsHitTestVisible(false);
        // The light's travel follows the host's width (AgentSweepBar::SetWidth): XAML does not size a child visual. Through the
        // weak page reference, never the host itself (a handler capturing its own element would keep it alive forever).
        sweepHost.SizeChanged([weakThis](const IInspectable&, const SizeChangedEventArgs& e) {
            if (const auto self = weakThis.get())
            {
                self->_sidePanelAiWorking.SetWidth(static_cast<float>(e.NewSize().Width)); // never throws
            }
        });
        strip.Children().Append(sweepHost); // third: over the lines' bottom edge
        // The dot breathes and the host's light sweeps while a request runs (_SidePanelAiUpdateStrip switches it). Nothing
        // composition-side exists until the first request this view shows.
        _sidePanelAiWorking = AgentWorkingIndicator{ _sidePanelAiDot, sweepHost, kSidePanelAiWorkingColor };
        Grid::SetRow(strip, 0);
        root.Children().Append(strip);

        // THE RESULTS: the model's list, in its own scroll (a long list never moves the strip).
        _sidePanelAiResults = StackPanel{};
        _sidePanelAiResults.Orientation(Orientation::Vertical);
        _sidePanelAiResultsScroll = ScrollViewer{};
        _sidePanelAiResultsScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _sidePanelAiResultsScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        _sidePanelAiResultsScroll.Content(_sidePanelAiResults);
        Grid::SetRow(_sidePanelAiResultsScroll, 1);
        root.Children().Append(_sidePanelAiResultsScroll);

        // Agentmaster (accessibility, E2E_TESTING.md §6.3): the AI view's stable ids. Names only where a glyph is the content.
        AgentSetAutomationId(_sidePanelAiStatusText, L"am.side.ai.status");
        AgentSetAutomationId(_sidePanelAiThinkingBtn, L"am.side.ai.thinking");
        AgentSetAutomationId(_sidePanelAiResetBtn, L"am.side.ai.reset");
        AgentSetAccessibleName(_sidePanelAiResetBtn, L"New conversation");
        AgentSetAutomationId(_sidePanelAiCopyBtn, L"am.side.ai.copy");
        AgentSetAccessibleName(_sidePanelAiCopyBtn, L"Copy the conversation");
        // The loading icon: a progress control has no name of its own, and it is in the tree (visible) only while the
        // assistant works — so "is am.side.ai.busy there" is the e2e driver's "is it running".
        AgentSetAutomationId(_sidePanelAiSpinner, L"am.side.ai.busy");
        AgentSetAccessibleName(_sidePanelAiSpinner, L"The AI search is working");

        root.Visibility(Visibility::Collapsed); // _SidePanelSetAiMode shows it
        _sidePanelCardsHost.Children().Append(root); // after the card list's ScrollViewer: drawn over it in the shared cell
        _sidePanelAiRoot = root;
    }

    void TerminalPage::_SidePanelAiAttach()
    {
        if (_sidePanelAiListenerToken != 0)
        {
            return;
        }
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher(); // agile: callable from the agent's threads
        _sidePanelAiListenerToken = ::Agentmaster::SharedSessionSearchAgent().AddListener([weakThis, dispatcher](const ::Agentmaster::SessionSearchEvent& e) {
            // ⚠ No strong reference to the page on the agent's thread: one that is the LAST would run ~TerminalPage there, off
            // the UI thread (the terminate-net note in ~TerminalPage). The weak reference is resolved inside the dispatched call.
            try
            {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, e]() {
                    if (const auto self = weakThis.get())
                    {
                        try
                        {
                            self->_OnSidePanelAiEvent(e);
                        }
                        catch (...)
                        {
                            ::Agentmaster::AgentLogCaughtException(L"side panel AI search: an agent event");
                        }
                    }
                });
            }
            catch (...)
            {
                // The window's dispatcher is gone (it is closing): the event is for nobody. Recorded (Rule #18).
                ::Agentmaster::AgentLogCaughtException(L"side panel AI search: post an agent event");
            }
        });
    }

    void TerminalPage::_SidePanelAiDetach() noexcept
    {
        if (_sidePanelAiListenerToken == 0)
        {
            return; // never turned on in this window: nothing registered, nothing of ours to park
        }
        try
        {
            // A token exists only when the agent does: this call cannot create it.
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            agent.RemoveListener(_sidePanelAiListenerToken);
            _sidePanelAiListenerToken = 0;
            // A window that closes while its side panel used the search must not leave the process up for the rest of the run
            // (its own request end, focus loss or mode change will never come now): arm the park — unless a Sessions page
            // shows the search, whose rule holds the process. The destructor may run on a pool thread: the agent's methods
            // are thread-safe, and _SessionsAiParkDelayMs reads a plain member.
            if (_SessionsAiPagesShowingCount() == 0)
            {
                agent.ParkProcessAfter(_SessionsAiParkDelayMs());
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiDetach");
        }
    }

    void TerminalPage::_SidePanelAiAsk()
    {
        try
        {
            if (!_sidePanelAiMode || !_sidePanelSearchBox)
            {
                return;
            }
            const std::wstring text = SidePanelAiTrim(std::wstring_view{ _sidePanelSearchBox.Text() });
            if (text.empty())
            {
                return;
            }
            // The path can be gone since the mode was turned on (the Settings cog's override), and a window that opened in AI
            // mode was never checked.
            if (!::Agentmaster::EnsureClaudeAvailable())
            {
                ::Agentmaster::LogNav(L"sidepanel ai-ask (blocked: no native claude.exe)");
                _PromptClaudeMissing();
                return;
            }
            _SidePanelAiAttach();
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            agent.SetClaudeExePath(::Agentmaster::SharedEngine().claudeExePath); // UI thread: the path's owner
            // The window the request comes from: the tabs server tells the model (asked_from), so with several windows it can
            // offer this one when it must ask where to open a session (SESSIONS.md §9.14).
            const uint64_t turn = agent.Ask(text, _windowId);
            if (turn != 0)
            {
                _sidePanelAiAskedTurn = turn; // its Answer / Failed event — in this window — arms the park
            }
            // The words stay out of the nav trail (the agent's own [ai-search] line has them).
            ::Agentmaster::LogNav(L"sidepanel ai-ask turn=" + std::to_wstring(turn) + L" chars=" + std::to_wstring(text.size()));
            _sidePanelSearchBox.Text(L""); // the strip shows the request; the box is free for the follow-up
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiAsk");
        }
    }

    void TerminalPage::_SidePanelAiPrewarm()
    {
        try
        {
            // The cached check only (no re-scan): a missing claude.exe is reported by the request itself, with the notice.
            if (!_sidePanelAiMode || !::Agentmaster::ClaudeAvailable())
            {
                return;
            }
            _SidePanelAiAttach();
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            agent.SetClaudeExePath(::Agentmaster::SharedEngine().claudeExePath);
            // The process and the session index, both in the background. A process already up makes this a no-op — and it
            // CANCELS a pending park ("somebody is back"), which the box's LostFocus or the request's end arms again.
            agent.Prewarm(/*readIndex*/ true);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiPrewarm");
        }
    }

    void TerminalPage::_SidePanelAiCopyConversation()
    {
        try
        {
            // The conversation's state and its record, read once each. The record is the truth for every request it holds; the
            // snapshot adds only whether the newest one still runs, and its words while the harness has not recorded it yet.
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            const auto snapshot = agent.Snapshot();
            const auto record = agent.Record();
            ::Agentmaster::SidePanelAiCopyContent content;
            content.conversation = ::Agentmaster::SidePanelAiCopyExchanges(record, snapshot.turns, snapshot.question, snapshot.busy);
            // The LIST ON SCREEN, not the conversation's newest show: while a request runs, the side panel still shows the answer
            // before it (SidePanelAiList.h), and the copy copies what the user sees — saying so (listIsPrevious).
            content.hasShown = _sidePanelAiHasShown;
            content.listed = SidePanelAiCopyRowsFor(_sidePanelAiShownIds, _sidePanelAiReasons, _sidePanelAiEntries, _sessionRegistry.get(), _appSettings.tabColorMode);
            content.listIsPrevious = snapshot.busy && _sidePanelAiHasShown;
            const std::wstring text = ::Agentmaster::BuildSidePanelAiCopyText(content, &::Agentmaster::SidePanelAiCopyLocalStamp);
            // The box: an EMPTY one gets the newest request back (the user's "if prompt box is empty, copy the prompt sent to
            // prompt box"), whether or not the clipboard takes the text — the click's two halves need nothing from each other.
            // No focus: a click into the box is what starts the search's process (the "only after a request" rule), and a copy
            // asks for none.
            const std::wstring boxText = _sidePanelSearchBox ? std::wstring{ _sidePanelSearchBox.Text() } : std::wstring{};
            const std::wstring refill = _sidePanelAiMode ? ::Agentmaster::SidePanelAiRefillText(boxText, snapshot.question) : std::wstring{};
            _SidePanelAiWriteRefill(refill);
            // [nav]: counts and lengths only — the words stay out of the nav trail (the agent's own [ai-search] lines hold them).
            const std::wstring counts = L" requests=" + std::to_wstring(content.conversation.exchanges.size()) + L" listed=" + std::to_wstring(content.listed.size()) + L" refill=" + std::to_wstring(refill.size());
            if (text.empty())
            {
                ::Agentmaster::LogNav(L"sidepanel ai-copy (nothing to copy)" + counts);
                return;
            }
            // The retry-looped Win32 writer every Agentmaster copy uses (AgentClipboard.h): the focused terminal holds the OLE
            // clipboard now and then, and a single attempt would lose the copy without a word.
            if (RobustCopyTextToClipboard(text))
            {
                ::PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC); // the chime every Agentmaster copy plays
                ::Agentmaster::LogNav(L"sidepanel ai-copy chars=" + std::to_wstring(text.size()) + counts);
            }
            else
            {
                ::Agentmaster::LogNav(L"sidepanel ai-copy (the clipboard could not be opened)" + counts);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing was copied (the clipboard keeps what it held) and the box keeps what it showed; a click
            // again tries again.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiCopyConversation");
        }
    }

    void TerminalPage::_SidePanelAiWriteRefill(const std::wstring& refill)
    {
        if (refill.empty() || !_sidePanelSearchBox || !_sidePanelAiMode)
        {
            return; // nothing to put back, no box, or the box filters the cards now (the words would filter them)
        }
        _sidePanelSearchBox.Text(winrt::hstring{ refill });
        // The caret after the request (a programmatic Text leaves it at the start): Enter sends it as it is, or typing goes on.
        _sidePanelSearchBox.Select(static_cast<int32_t>(refill.size()), 0);
    }

    void TerminalPage::_SidePanelAiArmPark(std::wstring_view why) noexcept
    {
        try
        {
            if (_sidePanelAiListenerToken == 0)
            {
                return; // this window never used the search: nothing it started, nothing of its to park
            }
            if (_SessionsAiPagesShowingCount() > 0)
            {
                return; // a Sessions page shows the search: its rule (the process lives while a page shows it) holds
            }
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            if (agent.Snapshot().busy)
            {
                return; // a request runs: its end arms the park (the asking window's Answer / Failed event)
            }
            // ⚠ Never right after a Prewarm of our own: ParkProcessAfter drops a start that has not happened yet (the
            // harness's "a Prewarm would start one only for the park to end it"). The callers are all moments AFTER use.
            const int64_t delayMs = _SessionsAiParkDelayMs();
            agent.ParkProcessAfter(delayMs);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[ai-search] side panel (window " + ::Agentmaster::ShortId(_windowId) + L"): the idle process parks in " + std::to_wstring(delayMs / 60000) + L" min (" + std::wstring{ why } + L")\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiArmPark");
        }
    }

    void TerminalPage::_OnSidePanelAiEvent(const ::Agentmaster::SessionSearchEvent& e)
    {
        using Kind = ::Agentmaster::SessionSearchEventKind;
        if (e.snapshot.epoch < _sidePanelAiEpoch)
        {
            return; // an event of a conversation a reset threw away
        }
        _sidePanelAiEpoch = e.snapshot.epoch;
        // THIS window's request ended: arm the park — even when the AI view was closed meanwhile (the process is still ours).
        if ((e.kind == Kind::Answer || e.kind == Kind::Failed) && e.turn != 0 && e.turn == _sidePanelAiAskedTurn)
        {
            _SidePanelAiArmPark(e.kind == Kind::Answer ? L"its request was answered" : L"its request failed");
        }
        if (!_sidePanelAiMode || !_sidePanelAiRoot)
        {
            return; // no view: turning the AI mode on adopts the conversation's whole state (_SidePanelSetAiMode)
        }
        // THE LIST — the user's rule of 2026-11-05, "the ai assistant should update the list only at the end when the result is
        // final" (AgentMaster/SidePanelAiList.h): a running request's candidates and shows never reach the cards (the model may
        // still show another list, and a search's rows were read by nobody); the event that ENDS the request (Answer — an
        // interrupted one too — or Failed) puts the conversation's list up, the pick of its last show, when it differs from
        // the list on screen, so an answer in words leaves the cards and their scroll alone; a reset clears them at once.
        // Until the end, the strip's loading icon says the request runs and the last answer's list stays, dimmed
        // (_SidePanelAiUpdateStrip).
        bool rebuild = false;
        bool newList = false; // a list that replaced the one on screen, or a reset: it starts at its top
        switch (::Agentmaster::SidePanelAiListStepFor(e.kind, e.snapshot.busy))
        {
        case ::Agentmaster::SidePanelAiListStep::AdoptNow:
            _SidePanelAiAdoptSnapshot(e.snapshot); // an empty conversation: no list
            rebuild = true;
            newList = true;
            break;
        case ::Agentmaster::SidePanelAiListStep::AdoptIfChanged:
            // The entries come from the agent's index (_SidePanelAiAdoptSnapshot → EntriesFor): the very rows the show event
            // carried, so nothing is lost by not keeping that event's copy.
            if (::Agentmaster::SidePanelAiListDiffers(_sidePanelAiHasShown, _sidePanelAiShownIds, _sidePanelAiReasons, e.snapshot))
            {
                _SidePanelAiAdoptSnapshot(e.snapshot);
                rebuild = true;
                newList = true;
            }
            break;
        case ::Agentmaster::SidePanelAiListStep::Keep:
        default:
            break; // the strip and the count line below are all the event changes
        }
        // The strip's progress words: how many sessions the running request's NEWEST search returned ("N candidates,
        // choosing…"). Counted, never drawn. A new request starts at none, and the model's pick (a show), the request's end
        // or a reset ends the count — after a show the model is writing its answer, not choosing. A Candidates event that
        // comes when nothing runs (a superseded request's late search) counts nothing.
        switch (e.kind)
        {
        case Kind::Candidates:
            if (e.snapshot.busy)
            {
                _sidePanelAiCandidateCount = e.sessionIds.size();
            }
            break;
        case Kind::Asked:
        case Kind::Shown:
        case Kind::Answer:
        case Kind::Failed:
        case Kind::Reset:
            _sidePanelAiCandidateCount = 0;
            break;
        case Kind::StateChanged:
        case Kind::Searching:
        case Kind::Compacting:
        case Kind::Narration:
        default:
            break; // the count of the newest search stands
        }
        if (rebuild)
        {
            _SidePanelAiRebuildResults();
            if (newList && _sidePanelAiResultsScroll)
            {
                _sidePanelAiResultsScroll.ChangeView(nullptr, 0.0, nullptr, true); // a new list starts at its top
            }
        }
        _SidePanelAiUpdateStrip(e.snapshot);
        _SidePanelAiUpdateCountLine(e.snapshot);
    }

    void TerminalPage::_SidePanelAiAdoptSnapshot(const ::Agentmaster::SessionSearchSnapshot& snapshot)
    {
        _sidePanelAiHasShown = snapshot.hasShown;
        _sidePanelAiShownIds = snapshot.hasShown ? snapshot.shownIds : std::vector<std::wstring>{};
        _sidePanelAiReasons.clear();
        for (size_t i = 0; i < _sidePanelAiShownIds.size() && i < snapshot.shownReasons.size(); ++i)
        {
            if (!snapshot.shownReasons[i].empty())
            {
                _sidePanelAiReasons.insert_or_assign(_sidePanelAiShownIds[i], snapshot.shownReasons[i]);
            }
        }
        // The entries the agent's in-memory index holds for the list (none when no search ran yet this run — the window's open
        // restores the last list from the agent's stored view before any index exists; the rebuild fetches those from disk).
        _sidePanelAiEntries.clear();
        if (!_sidePanelAiShownIds.empty())
        {
            for (auto& entry : ::Agentmaster::SharedSessionSearchAgent().EntriesFor(_sidePanelAiShownIds))
            {
                const std::wstring id = entry.sessionId;
                _sidePanelAiEntries.insert_or_assign(id, std::move(entry));
            }
        }
        ++_sidePanelAiListSeq;
    }

    winrt::fire_and_forget TerminalPage::_SidePanelAiFetchMissingEntries(std::vector<std::wstring> ids, uint64_t listSeq)
    {
        // ⚠ Only a WEAK page reference crosses the pool hop: a strong one dropped last on the pool thread would run the whole
        // page teardown there (the fire_and_forget rule, CLAUDE.md "Engine, threads and data"). The dispatcher is agile.
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        try
        {
            co_await winrt::resume_background();
            std::vector<::Agentmaster::SessionIndexEntry> found;
            found.reserve(ids.size());
            for (const auto& id : ids)
            {
                // One small sidecar read each (a transcript that grew is parsed from its stored offset — the reason this runs
                // off the UI thread). A gone transcript reads nullopt: its card keeps its placeholder until the next list.
                if (auto entry = ::Agentmaster::LoadSessionIndexEntryById(id))
                {
                    found.push_back(std::move(*entry));
                }
            }
            co_await wil::resume_foreground(dispatcher);
            const auto self = weakThis.get();
            if (!self || listSeq != self->_sidePanelAiListSeq || !self->_sidePanelAiMode)
            {
                co_return; // the window closed, the list was replaced, or the view closed: these entries are for nobody
            }
            for (auto& entry : found)
            {
                const std::wstring id = entry.sessionId;
                self->_sidePanelAiEntries.insert_or_assign(id, std::move(entry));
            }
            if (!found.empty())
            {
                self->_SidePanelAiRebuildResults(); // the placeholders become real cards (the fetch is not queued again: one per list)
            }
        }
        catch (...)
        {
            // Rule #18; a fire_and_forget must never let an exception escape (winrt::terminate). The cards keep their
            // placeholders until the next list.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiFetchMissingEntries");
        }
    }

    void TerminalPage::_SidePanelAiUpdateStrip(const ::Agentmaster::SessionSearchSnapshot& snapshot)
    {
        using State = ::Agentmaster::HarnessState;
        if (!_sidePanelAiRoot || !_sidePanelAiStatusText || !_sidePanelAiQuestionText || !_sidePanelAiDot || !_sidePanelAiUsageText)
        {
            return;
        }
        // Nothing to throw away, nor to copy, in a conversation that has not started.
        const bool conversationStarted = snapshot.turns > 0 || snapshot.busy || snapshot.hasShown;
        if (_sidePanelAiResetBtn)
        {
            _sidePanelAiResetBtn.IsEnabled(conversationStarted);
        }
        if (_sidePanelAiCopyBtn)
        {
            _sidePanelAiCopyBtn.IsEnabled(conversationStarted);
        }
        if (snapshot.question.empty())
        {
            _sidePanelAiQuestionText.Visibility(Visibility::Collapsed);
        }
        else
        {
            _sidePanelAiQuestionText.Text(winrt::hstring{ L"\x276F " + snapshot.question });
            _sidePanelAiQuestionText.Visibility(Visibility::Visible);
        }
        // The status line and the dot — the Sessions strip's rules: working (blue; the model's plan above what it does now),
        // an error (red), the answer (green, plain), starting, or — before any request — how to use it (dim). While working,
        // the loading icon stands in the dot's place (below).
        std::wstring status;
        Color dot = ColorHelper::FromArgb(0xFF, 0x80, 0x80, 0x80);
        Color ink = ColorHelper::FromArgb(0xFF, 0xF2, 0xF2, 0xF2);
        double inkOpacity = 0.95;
        if (snapshot.busy)
        {
            status = !snapshot.activity.empty() ? snapshot.activity : std::wstring{ L"Thinking\x2026" };
            if (_sidePanelAiCandidateCount > 0)
            {
                // The newest search's size, in words only: its rows are not drawn — the list changes when the request ends.
                status += L" \x2014 " + std::to_wstring(_sidePanelAiCandidateCount) + (_sidePanelAiCandidateCount == 1 ? L" candidate, choosing\x2026" : L" candidates, choosing\x2026");
            }
            if (!snapshot.narration.empty())
            {
                status = snapshot.narration + L"\n" + status; // the plan the model wrote before its first call ("I'll 1. ... 2. ...")
            }
            dot = ColorHelper::FromArgb(0xFF, 0x4E, 0xA1, 0xFF);
            ink = ColorHelper::FromArgb(0xFF, 0x8A, 0xB4, 0xF8);
        }
        else if (snapshot.answerIsError || snapshot.state == State::Failed)
        {
            status = !snapshot.answer.empty() ? snapshot.answer : std::wstring{ L"The search assistant could not start." };
            dot = ColorHelper::FromArgb(0xFF, 0xE5, 0x53, 0x3D);
            ink = ColorHelper::FromArgb(0xFF, 0xF2, 0x8B, 0x82);
        }
        else if (!snapshot.answer.empty())
        {
            status = snapshot.answer;
            dot = ColorHelper::FromArgb(0xFF, 0x5C, 0xB8, 0x5C);
        }
        else if (snapshot.state == State::Starting)
        {
            status = L"Starting the search assistant\x2026";
            dot = ColorHelper::FromArgb(0xFF, 0x4E, 0xA1, 0xFF);
            inkOpacity = 0.6;
        }
        else
        {
            status = L"Describe the session you are looking for, or ask what is open, and press Enter. A follow-up refines the list; \x2298 starts over.";
            if (snapshot.state == State::Ready)
            {
                dot = ColorHelper::FromArgb(0xFF, 0x5C, 0xB8, 0x5C);
            }
            inkOpacity = 0.6;
        }
        _sidePanelAiStatusText.Text(winrt::hstring{ status });
        _sidePanelAiStatusText.Foreground(SolidColorBrush{ ink });
        _sidePanelAiStatusText.Opacity(inkOpacity);
        _sidePanelAiDot.Background(SolidColorBrush{ dot });
        // The LOADING icon (the user's request of 2026-11-05): spinning in the dot's place while the assistant works — a request
        // runs, or its process starts (the strip's two blue states) — and only while this view can be seen: AI mode on and the
        // panel expanded (the status icons' host rule; a collapse and an expand repaint the strip to follow it). Every other
        // state shows the dot.
        const bool working = snapshot.busy || snapshot.state == State::Starting;
        _SidePanelAiShowWorking(working && _sidePanelAiMode && !_sidePanelCollapsed);
        // The result cards while a request runs are the LAST answer's list (SidePanelAiList.h: the list changes only when the
        // request ends): dimmed, so they never read as this request's result, and still readable and clickable. A process that
        // only starts (a prewarm) leaves them as they are: no request, nothing pending. Change-gated: a same-value Opacity write
        // still invalidates the element.
        if (_sidePanelAiResultsScroll)
        {
            const double listOpacity = snapshot.busy ? kSidePanelAiStaleListOpacity : 1.0;
            if (std::abs(_sidePanelAiResultsScroll.Opacity() - listOpacity) > 0.001)
            {
                _sidePanelAiResultsScroll.Opacity(listOpacity);
            }
        }
        // The thinking level the next request runs with — the agent's, so a click in another window shows here too. A level that
        // thinks reads brighter than the context size (it changes how long a request takes); the agent never runs Off, the one
        // level that would read dim, so every level shown reads bright.
        if (_sidePanelAiThinkingText)
        {
            const bool thinks = ::Agentmaster::HarnessThinkingLevelThinks(snapshot.thinkingLevel);
            _sidePanelAiThinkingText.Text(winrt::hstring{ std::wstring{ L"thinking " } + ::Agentmaster::HarnessThinkingLevelWord(snapshot.thinkingLevel) });
            _sidePanelAiThinkingText.Opacity(thinks ? 0.95 : 0.6);
        }
        std::wstring usage;
        if (snapshot.usage.contextTokens > 0)
        {
            usage = L"ctx " + FormatTokenCount(snapshot.usage.contextTokens);
            if (snapshot.usage.contextWindow > 0)
            {
                usage += L" / " + FormatTokenCount(snapshot.usage.contextWindow);
            }
            if (snapshot.compactions > 0)
            {
                usage += L" \x00B7 compacted " + std::to_wstring(snapshot.compactions) + L"\x00D7";
            }
        }
        _sidePanelAiUsageText.Text(winrt::hstring{ usage });
        // The "agent is running" motion (the user's pick of 2026-11-05): the dot breathes and a light sweeps along the strip's
        // bottom edge while a request runs — and only while this view can be seen (AI mode on, the panel expanded; the status
        // icons' host rule), so a strip nobody sees never keeps the compositor animating. LAST, so it reads the dot as this
        // repaint left it: AgentWorkingIndicator never animates a dot that is not shown. Acts on changes only; never throws.
        _sidePanelAiWorking.SetRunning(_sidePanelAiMode && !_sidePanelCollapsed && snapshot.busy);
    }

    void TerminalPage::_SidePanelAiShowWorking(bool working) noexcept
    {
        try
        {
            if (_sidePanelAiSpinner)
            {
                // IsActive is what starts and stops the ring's animation (its AnimatedVisualPlayer plays only while active), so a
                // ring that is collapsed is also stopped: a running Composition animation keeps the compositor working on every
                // frame, seen or not. Each write only when it changes something (a strip repaint comes with every agent event).
                if (_sidePanelAiSpinner.IsActive() != working)
                {
                    _sidePanelAiSpinner.IsActive(working);
                }
                const auto ringVisibility = working ? Visibility::Visible : Visibility::Collapsed;
                if (_sidePanelAiSpinner.Visibility() != ringVisibility)
                {
                    _sidePanelAiSpinner.Visibility(ringVisibility);
                }
            }
            if (_sidePanelAiDot)
            {
                // The dot's cell is the ring's: one of the two at a time, never both side by side.
                const auto dotVisibility = working ? Visibility::Collapsed : Visibility::Visible;
                if (_sidePanelAiDot.Visibility() != dotVisibility)
                {
                    _sidePanelAiDot.Visibility(dotVisibility);
                }
            }
        }
        catch (...)
        {
            // The strip keeps whichever of the two it showed; the next repaint (every agent event) writes them again. Recorded
            // (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiShowWorking");
        }
    }

    void TerminalPage::_SidePanelAiUpdateCountLine(const ::Agentmaster::SessionSearchSnapshot& snapshot)
    {
        if (!_sidePanelAiMode || !_sidePanelCountText)
        {
            return;
        }
        std::wstring text;
        const size_t n = _sidePanelAiShownIds.size();
        if (snapshot.busy)
        {
            // A running request outranks whatever is listed: the cards on screen are the last answer's until it ends, so their
            // count is not this request's result — the line says the search runs instead.
            text = L"AI search \x2014 searching\x2026";
        }
        else if (_sidePanelAiHasShown)
        {
            text = n == 0 ? std::wstring{ L"no session fits" } : (std::to_wstring(n) + (n == 1 ? L" session found" : L" sessions found"));
        }
        else
        {
            text = L"AI search \x2014 the same conversation as the Sessions page";
        }
        if (text != _sidePanelAiCountValue)
        {
            _sidePanelCountText.Text(winrt::hstring{ text });
            _sidePanelAiCountValue = std::move(text);
        }
    }

    void TerminalPage::_SidePanelAiClearResultCards() noexcept
    {
        // Every status icon first: its motions stop and its flash ring gives back the shared blink clock NOW, not at the
        // glyph's destruction (the session cards' Remove rule). Shutdown is noexcept.
        for (auto& [id, view] : _sidePanelAiResultViews)
        {
            if (view.base.glyph)
            {
                view.base.glyph->Shutdown();
            }
        }
        _sidePanelAiResultViews.clear();
        try
        {
            if (_sidePanelAiResults)
            {
                // A whole clear is right here, unlike the session card list (SIDE_PANEL.md §2c): this list is the model's
                // handful, replaced as one when the model shows a new one.
                _sidePanelAiResults.Children().Clear();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiClearResultCards"); // a torn-down tree: nothing left to show anyway
        }
    }

    AgentSidePanelCardModel TerminalPage::_SidePanelAiResultModelFor(const std::wstring& id,
                                                                     const std::optional<::Agentmaster::SessionInfo>& lean,
                                                                     const ::Agentmaster::DirColorIndex& dirColors,
                                                                     const std::map<std::wstring, std::wstring>& tagColors,
                                                                     const std::vector<std::wstring>& tags,
                                                                     AgentSidePanelAiResultView& out)
    {
        AgentSidePanelCardModel m;
        const auto mode = _appSettings.tabColorMode;
        out.open = false;
        out.known = false;
        out.dir.clear();
        out.title.clear();
        out.hasMessages = false;
        const auto entryIt = _sidePanelAiEntries.find(id);
        const ::Agentmaster::SessionIndexEntry* entry = entryIt != _sidePanelAiEntries.end() ? &entryIt->second : nullptr;
        std::wstring shownDir; // the folder the card names (the effective working dir, like a session card)
        std::wstring branch;
        int64_t createdMs = 0;
        int64_t lastMs = 0;
        if (lean)
        {
            // The registry knows it (open, or closed and archived): its record is the truth — the managed title (Rule #11),
            // its color, its live state. The cwd is what a resume or a fork launches in (the effective dir is display only).
            out.known = true;
            out.open = lean->live;
            out.dir = lean->workingDir;
            out.title = lean->title;
            out.hasMessages = true; // a managed session's title is its own: a fork derives " (fork)" from it
            shownDir = ::Agentmaster::EffectiveWorkingDir(mode, *lean);
            branch = lean->branch;
            createdMs = lean->convCreatedUnixMs;
            lastMs = lean->convLastActivityUnixMs ? lean->convLastActivityUnixMs : lean->lastActivityUnixMs;
            m.bandHex = ::Agentmaster::ResolveSessionColorHex(mode, *lean, dirColors);
            m.state = lean->state;
            m.codex = (lean->kind == ::Agentmaster::AgentKind::Codex);
            if (entry)
            {
                // An older archived record may lack facts its transcript index has.
                if (createdMs == 0)
                {
                    createdMs = ::Agentmaster::SessionSearchCreatedMs(*entry);
                }
                if (lastMs == 0)
                {
                    lastMs = ::Agentmaster::SessionSearchLastActivityMs(*entry);
                }
                if (branch.empty())
                {
                    branch = entry->stats.gitBranch;
                }
            }
        }
        else if (entry)
        {
            // A session Agentmaster never opened: the facts its transcript index has (the Sessions page's own field rules).
            out.dir = ::Agentmaster::SessionSearchDirOf(*entry);
            out.title = ::Agentmaster::SessionSearchTitleOf(*entry);
            out.hasMessages = entry->stats.userPrompts > 0;
            shownDir = out.dir;
            branch = entry->stats.gitBranch;
            createdMs = ::Agentmaster::SessionSearchCreatedMs(*entry);
            lastMs = ::Agentmaster::SessionSearchLastActivityMs(*entry);
            // The band: the folder's color as every surface resolves it, through a stand-in record that carries only the
            // folder (the dir-keyed modes; "Individual" has no color for a session that never had a tab — the neutral band).
            ::Agentmaster::SessionInfo standIn;
            standIn.id = id;
            standIn.workingDir = out.dir;
            m.bandHex = ::Agentmaster::ResolveSessionColorHex(mode, standIn, dirColors);
        }
        m.title = !out.title.empty() ? out.title : (L"session " + ::Agentmaster::ShortId(id));
        if (!lean && !entry)
        {
            m.title += L" \x2014 reading\x2026"; // its index entry is on its way from disk (_SidePanelAiFetchMissingEntries)
        }
        if (const auto band = HexToColor(m.bandHex))
        {
            m.darkInk = PreferDarkTextOn(*band);
        }
        m.timing = (createdMs != 0 || lastMs != 0) ? FormatSessionTiming(createdMs, lastMs) : std::wstring{};
        m.dirLine = SidePanelLeafOf(shownDir);
        if (!branch.empty())
        {
            m.dirLine += L" / " + branch;
        }
        m.tags = tags;
        m.tagArgb.reserve(tags.size());
        for (const auto& tag : tags)
        {
            m.tagArgb.push_back(SidePanelPackArgb(ResolveTagDisplayColor(tag, tagColors))); // the ONE tag color resolution (Rule #17)
        }
        // PIN TO TOP (SIDE_PANEL.md §2j): a result card is the session's card, so it wears the same pushpin — for an OPEN session
        // only. A closed session keeps its stamp (it comes back pinned when resumed), but it has no card in the list a pin
        // orders, so a pushpin on its "closed" card would promise nothing.
        m.pinned = out.open && lean && ::Agentmaster::IsSidePanelPinStamp(lean->sidePanelPinnedAtUnixMs);
        // The why: the model's evidence for this session. The list on screen is always a pick (a show) — a search's candidates
        // are never drawn (SidePanelAiList.h) — so every listed session's why, when the model gave one, is shown.
        out.whyText.clear();
        if (const auto why = _sidePanelAiReasons.find(id); why != _sidePanelAiReasons.end())
        {
            out.whyText = why->second;
        }
        // The tip: the whole folder, the branch, every tag, the whole why, and what the gestures do for THIS session.
        std::wstring tipText = shownDir.empty() ? std::wstring{ L"(the session's folder is being read)" } : shownDir;
        if (!branch.empty())
        {
            tipText += L"\nbranch: " + branch;
        }
        if (!tags.empty())
        {
            tipText += L"\ntags: ";
            for (size_t i = 0; i < tags.size(); ++i)
            {
                tipText += (i ? L" \x00B7 " : L"") + tags[i];
            }
        }
        if (m.pinned)
        {
            tipText += L"\npinned to the top of the session cards (right-click \x25B8 Unpin)"; // the session card's own line (SidePanelCardModelFor)
        }
        if (!out.whyText.empty())
        {
            tipText += L"\n\nwhy: " + out.whyText;
        }
        tipText += out.open ? L"\n\nClick to see its summary below \x00B7 double-click to jump to its tab \x00B7 right-click for actions." :
                   out.known ? L"\n\nClosed. Click to see its summary below \x00B7 double-click to resume it in a new tab here \x00B7 right-click for actions." :
                               L"\n\nNever opened in Agentmaster. Click to read its transcript \x00B7 double-click to resume it in a new tab here \x00B7 right-click for actions.";
        m.tipTitle = m.title;
        m.tipText = std::move(tipText);
        return m;
    }

    AgentSidePanelAiResultView TerminalPage::_SidePanelAiCreateResultView(const std::wstring& id)
    {
        AgentSidePanelAiResultView v;
        v.base = _SidePanelBuildCardSkeleton(id, L"am.side.ai.result.");
        // The result cards are the AI view's: they hold still only when the panel collapses (the view shows whenever the
        // expanded panel is in AI mode, which is when these cards exist at all).
        v.base.glyph->SetHostShown(!_sidePanelCollapsed);
        const auto weakThis = get_weak();
        // ⚠ Every handler reaches the card through `sender` and the view through _sidePanelAiResultViews — capturing the card in
        // a handler attached TO it would be the element -> handler -> element cycle that leaks every card a list replaces. A
        // sender that is not the live card of `id` (a card a newer list replaced) is ignored.
        v.base.card.PointerEntered([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs&) {
            const auto self = weakThis.get();
            const auto g = sender.try_as<Grid>();
            if (!self || !g)
            {
                return;
            }
            const auto it = self->_sidePanelAiResultViews.find(id);
            if (it == self->_sidePanelAiResultViews.end() || it->second.base.card != g)
            {
                return;
            }
            it->second.base.hovered = true;
            SidePanelStyleCardBorder(it->second.base.cardBack, SidePanelAiRingColor(it->second), id == self->_sidePanelSelectedId, /*hover*/ true);
        });
        v.base.card.PointerExited([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            const auto self = weakThis.get();
            const auto g = sender.try_as<Grid>();
            if (!self || !g || PointerStillWithin(sender, e))
            {
                return; // bubbled from a child: the pointer never left the card
            }
            const auto it = self->_sidePanelAiResultViews.find(id);
            if (it == self->_sidePanelAiResultViews.end() || it->second.base.card != g)
            {
                return;
            }
            it->second.base.hovered = false;
            SidePanelStyleCardBorder(it->second.base.cardBack, SidePanelAiRingColor(it->second), id == self->_sidePanelSelectedId, /*hover*/ false);
        });
        v.base.card.Tapped([weakThis, id](const IInspectable&, const TappedRoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelAiClickResult(id);
            }
        });
        v.base.card.DoubleTapped([weakThis, id](const IInspectable&, const DoubleTappedRoutedEventArgs& e) {
            e.Handled(true);
            if (const auto self = weakThis.get())
            {
                self->_SidePanelAiOpenResult(id);
            }
        });
        v.base.card.RightTapped([weakThis, id](const IInspectable& sender, const RightTappedRoutedEventArgs& e) {
            e.Handled(true);
            const auto self = weakThis.get();
            if (!self)
            {
                return;
            }
            // Contained: a menu build + ShowAt can throw under islands teardown races — a lost menu never costs the window.
            try
            {
                const auto it = self->_sidePanelAiResultViews.find(id);
                const auto anchor = sender.try_as<FrameworkElement>();
                if (it == self->_sidePanelAiResultViews.end() || !anchor)
                {
                    return;
                }
                // Read before the selection below: it repaints the cards (the map itself is not changed, but the facts are
                // taken once, at the press).
                const bool open = it->second.open;
                const bool known = it->second.known;
                if (open)
                {
                    // An open session: THE session menu every card and tree row shows, anchored on this card (its Tags item
                    // opens the tag panel under it), after the lenses align on it — the session card's own right-click.
                    self->_SidePanelSelectSession(id, /*driveManagerLens*/ true);
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
                            // A result card is the session's card, so its menu takes the session card's part too
                            // (AgentManagerContent::SessionMenuHost; the page held weakly — the menu keeps the functions for
                            // its life): Pin to Top / Unpin under Jump to Tab (SIDE_PANEL.md §2j — from here the pin owes no
                            // scroll, the cards list being behind the AI view, and the result cards show the pushpin at once),
                            // and "Open New Session Here" / "Fork session" right next to the session's tab, in its window (§2k).
                            implementation::AgentManagerContent::SessionMenuHost host;
                            host.afterJumpToTab = [weakThis, id](MenuFlyout& m) {
                                if (const auto page = weakThis.get())
                                {
                                    page->_SidePanelAppendPinItems(m, id, /*fromAiView*/ true);
                                }
                            };
                            host.openNewSessionHere = [weakThis, id](winrt::hstring model) {
                                if (const auto page = weakThis.get())
                                {
                                    page->_SidePanelLaunchNextToTab(id, /*fork*/ false, std::wstring{ model });
                                }
                            };
                            host.forkSession = [weakThis, id](winrt::hstring model) {
                                if (const auto page = weakThis.get())
                                {
                                    page->_SidePanelLaunchNextToTab(id, /*fork*/ true, std::wstring{ model });
                                }
                            };
                            auto menu = mgr->BuildSessionMenuFor(id, cwd, anchor, std::move(host));
                            menu.ShowAt(anchor, e.GetPosition(anchor));
                        }
                    }
                    return;
                }
                if (known)
                {
                    self->_SidePanelSelectSession(id, /*driveManagerLens*/ false); // the summary shows the session the menu is about
                }
                auto menu = self->_SidePanelAiMakeClosedResultMenu(id);
                menu.ShowAt(anchor, e.GetPosition(anchor));
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel AI result: context menu");
            }
        });
        return v;
    }

    void TerminalPage::_SidePanelAiApplyResultModel(AgentSidePanelAiResultView& view, AgentSidePanelCardModel model, const std::wstring& id, bool fresh)
    {
        auto& b = view.base;
        const AgentSidePanelCardModel& shown = b.model; // the card as it is now (ignored when fresh)
        if (fresh || model.title != shown.title)
        {
            b.title.Text(winrt::hstring{ model.title });
        }
        if (fresh || model.bandHex != shown.bandHex)
        {
            const auto bandColor = HexToColor(model.bandHex);
            b.band.Background(SolidColorBrush{ bandColor.value_or(Color{ 0xFF, 0x3A, 0x3A, 0x3A }) });
        }
        if (fresh || model.darkInk != shown.darkInk)
        {
            b.title.Foreground(model.darkInk ? Fill(0xFF, 0x10, 0x10, 0x10) : Fill(0xFF, 0xF0, 0xF0, 0xF0));
        }
        if (fresh || model.tags != shown.tags || model.tagArgb != shown.tagArgb || (!model.tags.empty() && model.darkInk != shown.darkInk))
        {
            _SidePanelApplyCardTags(b, model, id); // the session card's own band layout and tag strip (the shared hover panel)
        }
        // The session card's PIN TO TOP pushpin too (SIDE_PANEL.md §2j), after the tags for the same reason as there: each half
        // gives the band its plain layout back only when the other has nothing left in the grid.
        if (fresh || model.pinned != shown.pinned || (model.pinned && model.darkInk != shown.darkInk))
        {
            _SidePanelApplyCardPin(b, model);
        }
        // The state line: the live state for a session with a tab; "closed" in gray for one without — a result is often a
        // session nobody runs now, and a gray "idle" would read as an open tab at rest.
        if (fresh || model.state != shown.state || view.open != view.shownOpen)
        {
            const Color stateColor = view.open ? StateColor(model.state) : kSidePanelAiClosedColor;
            b.glyph->SetStateColor(stateColor);
            b.stateText.Text(view.open ? StateLabel(model.state) : winrt::hstring{ L"closed" });
            b.stateText.Foreground(SolidColorBrush{ stateColor });
            SidePanelStyleCardBorder(b.cardBack, stateColor, id == _sidePanelSelectedId, b.hovered);
            view.shownOpen = view.open;
        }
        if (model.codex && !b.codexPill)
        {
            b.codexPill = Text(L"codex", 9, true, 0.9);
            b.codexPill.Foreground(Fill(0xFF, 0x2E, 0xB8, 0xA8)); // the session card's teal pill
            b.codexPill.Margin(ThicknessHelper::FromLengths(0, 0, 0, kSidePanelStatusIconBandPx));
            b.stateRow.Children().InsertAt(2, b.codexPill); // right after the status icon and the label
        }
        else if (!model.codex && b.codexPill)
        {
            SidePanelRemoveChild(b.stateRow, b.codexPill);
            b.codexPill = nullptr;
        }
        if (model.timing.empty())
        {
            if (b.timing)
            {
                SidePanelRemoveChild(b.stateLine, b.timing);
                b.timing = nullptr;
            }
        }
        else if (!b.timing)
        {
            // The session card's timing text and its explanatory tip (a result card's tips are always the floating ones: it has
            // no rich tooltip host, whatever "Sum" says).
            b.timing = Text(winrt::hstring{ model.timing }, 10, false, 0.45);
            b.timing.Margin(ThicknessHelper::FromLengths(5, 0, 0, kSidePanelStatusIconBandPx));
            AgentSetTitledTip(b.timing, winrt::hstring{ kTimingTooltipTitle }, winrt::hstring{ kTimingTooltip });
            Grid::SetColumn(b.timing, 1); // the state line's star column: it trims with "…" on a narrow panel
            b.stateLine.Children().Append(b.timing);
        }
        else if (fresh || model.timing != shown.timing)
        {
            b.timing.Text(winrt::hstring{ model.timing });
        }
        if (fresh || model.dirLine != shown.dirLine)
        {
            b.dirLine.Text(winrt::hstring{ model.dirLine });
        }
        if (fresh || model.tipTitle != shown.tipTitle || model.tipText != shown.tipText)
        {
            AgentSetTitledTip(b.card, winrt::hstring{ model.tipTitle }, winrt::hstring{ model.tipText }, kCardTipDelay);
        }
        // The WHY row, under the folder line: [ a 2 DIP quote bar ][ the evidence, WHOLE ]. The evidence wraps over as many
        // lines as it needs and is never trimmed (the user's rule of 2026-11-05: "the cards in sidemenu with an explanation must
        // not be trimmed and support multiline"); the bar stretches beside every line, a quote block. The card grows with it:
        // a card has no fixed height, and the result list is a plain StackPanel (a handful of cards, never virtualized). Added on
        // the first apply only — the why belongs to the list, which a new list replaces with new cards.
        if (fresh && !view.whyText.empty() && !view.whyRow)
        {
            if (const auto body = b.card.Children().Size() >= 2 ? b.card.Children().GetAt(1).try_as<StackPanel>() : nullptr)
            {
                Grid whyRow{};
                whyRow.Margin(ThicknessHelper::FromLengths(6, 0, 6, 5));
                ColumnDefinition barCol{};
                barCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
                ColumnDefinition textCol{};
                textCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                whyRow.ColumnDefinitions().Append(barCol);
                whyRow.ColumnDefinitions().Append(textCol);
                Border bar{}; // square corners, no child: no composition node
                bar.Width(2);
                bar.Background(SolidColorBrush{ kSidePanelAiWhyBarColor });
                bar.Margin(ThicknessHelper::FromLengths(0, 1, 6, 1));
                whyRow.Children().Append(bar);
                view.why = Text(winrt::hstring{ view.whyText }, 10, false, 0.85);
                view.why.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
                // Text() makes a one-line, "…"-trimmed label: undo both for the evidence. Top-aligned, so its first line sits
                // level with the bar's top whatever the number of lines.
                view.why.TextWrapping(TextWrapping::Wrap);
                view.why.TextTrimming(TextTrimming::None);
                view.why.VerticalAlignment(VerticalAlignment::Top);
                view.why.Foreground(Fill(0xFF, 0xC9, 0xD6, 0xEE));
                Grid::SetColumn(view.why, 1);
                whyRow.Children().Append(view.why);
                body.Children().Append(whyRow); // after the folder line
                view.whyRow = whyRow;
            }
        }
        b.model = std::move(model);
        SidePanelAiApplyItemStatus(view, id == _sidePanelSelectedId); // Agentmaster (accessibility): the words for what the card shows
    }

    void TerminalPage::_SidePanelAiRebuildResults()
    {
        if (!_sidePanelAiResults)
        {
            return;
        }
        try
        {
            _SidePanelAiClearResultCards();
            _sidePanelAiRefreshOwed = false;
            _sidePanelAiNextRefreshMs = ::GetTickCount64() + kSidePanelAiRefreshMs;
            if (_sidePanelAiShownIds.empty())
            {
                if (_sidePanelAiHasShown)
                {
                    // The model said "nothing fits" (an empty show): say so where the list would be, so an empty area does not
                    // read as "still working".
                    auto none = Text(L"No session fits this request.", 11, false, 0.5);
                    none.Margin(ThicknessHelper::FromLengths(10, 8, 10, 8));
                    none.TextWrapping(TextWrapping::Wrap);
                    _sidePanelAiResults.Children().Append(none);
                }
                return;
            }
            // The tags, read once per listed session (SessionStore's (size,mtime)-validated cache: one stat each), with the
            // epoch loaded BEFORE the reads (the SessionStore contract: a write that lands meanwhile leaves the older epoch,
            // so the next refresh reads again), and the colors file once — only when a listed session is tagged.
            const uint64_t tagsEpoch = ::Agentmaster::SessionTagsEpoch();
            struct Row
            {
                std::wstring id;
                std::optional<::Agentmaster::SessionInfo> lean;
                std::vector<std::wstring> tags;
            };
            std::vector<Row> rows;
            rows.reserve(_sidePanelAiShownIds.size());
            std::unordered_set<std::wstring> seen; // belt: a list never names a session twice, and a card is keyed by its id
            bool anyTagged = false;
            for (const auto& id : _sidePanelAiShownIds)
            {
                if (id.empty() || !seen.insert(id).second)
                {
                    continue;
                }
                Row row;
                row.id = id;
                // Without the prompt queue (the costliest part of a record copy, which no card shows — UI_THREAD.md §10a #4).
                row.lean = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(id) : std::nullopt;
                row.tags = ::Agentmaster::GetSessionTags(id);
                anyTagged = anyTagged || !row.tags.empty();
                rows.push_back(std::move(row));
            }
            _sidePanelAiTagColors = anyTagged ? ::Agentmaster::LoadAllTagColors() : std::map<std::wstring, std::wstring>{};
            _sidePanelAiTagsEpoch = tagsEpoch;
            const auto dirColors = ::Agentmaster::LoadDirColorIndex(); // ONE folder-colors snapshot for every card's band
            std::vector<std::wstring> missing; // sessions neither the registry nor the agent's index knows: read from disk
            for (auto& row : rows)
            {
                AgentSidePanelAiResultView view = _SidePanelAiCreateResultView(row.id);
                auto model = _SidePanelAiResultModelFor(row.id, row.lean, dirColors, _sidePanelAiTagColors, row.tags, view);
                if (!row.lean && _sidePanelAiEntries.find(row.id) == _sidePanelAiEntries.end())
                {
                    missing.push_back(row.id);
                }
                _SidePanelAiApplyResultModel(view, std::move(model), row.id, /*fresh*/ true);
                _sidePanelAiResults.Children().Append(view.base.card);
                _sidePanelAiResultViews.insert_or_assign(row.id, std::move(view));
            }
            if (!missing.empty() && _sidePanelAiFetchedSeq != _sidePanelAiListSeq)
            {
                // One fetch per list: its landing rebuilds the cards, and a refresh or a rebuild of the same list must not queue
                // the same reads again (a transcript that is gone stays a placeholder until the next list).
                _sidePanelAiFetchedSeq = _sidePanelAiListSeq;
                _SidePanelAiFetchMissingEntries(std::move(missing), _sidePanelAiListSeq);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: an empty list rather than a half-built one; the next list event (or the AI view's next open)
            // builds it again.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiRebuildResults");
            _SidePanelAiClearResultCards();
        }
    }

    void TerminalPage::_SidePanelAiRefreshResults()
    {
        if (!_sidePanelAiMode || !_sidePanelAiResults)
        {
            return;
        }
        _sidePanelAiRefreshOwed = false;
        _sidePanelAiNextRefreshMs = ::GetTickCount64() + kSidePanelAiRefreshMs;
        if (_sidePanelAiResultViews.empty())
        {
            return;
        }
        try
        {
            // Tags: re-read only when the process-wide tag epoch moved since the last read (a tag edited anywhere in this
            // process); otherwise each card's own model already holds them — a quiet refresh reads no tag file at all.
            const uint64_t tagsEpoch = ::Agentmaster::SessionTagsEpoch();
            const bool tagsMoved = tagsEpoch != _sidePanelAiTagsEpoch;
            std::unordered_map<std::wstring, std::vector<std::wstring>> tagsById;
            tagsById.reserve(_sidePanelAiResultViews.size());
            bool anyTagged = false;
            for (const auto& [id, view] : _sidePanelAiResultViews)
            {
                auto tags = tagsMoved ? ::Agentmaster::GetSessionTags(id) : view.base.model.tags;
                anyTagged = anyTagged || !tags.empty();
                tagsById.emplace(id, std::move(tags));
            }
            if (tagsMoved)
            {
                _sidePanelAiTagColors = anyTagged ? ::Agentmaster::LoadAllTagColors() : std::map<std::wstring, std::wstring>{};
                _sidePanelAiTagsEpoch = tagsEpoch;
            }
            const auto dirColors = ::Agentmaster::LoadDirColorIndex();
            for (auto& [id, view] : _sidePanelAiResultViews)
            {
                // A fresh lean read each: the session may have opened, closed, been renamed or moved on since the last pass.
                const auto lean = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(id) : std::nullopt;
                auto model = _SidePanelAiResultModelFor(id, lean, dirColors, _sidePanelAiTagColors, tagsById[id], view);
                _SidePanelAiApplyResultModel(view, std::move(model), id, /*fresh*/ false); // only the values that moved are written
            }
        }
        catch (...)
        {
            // A refused XAML write or an allocation failure: the cards keep what they showed; the next list event rebuilds them.
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiRefreshResults");
        }
    }

    void TerminalPage::_SidePanelAiRestyleSelection(const std::wstring& previous, const std::wstring& current) noexcept
    {
        if (_sidePanelAiResultViews.empty())
        {
            return;
        }
        try
        {
            const auto paint = [this](const std::wstring& sid, bool selected) {
                const auto it = _sidePanelAiResultViews.find(sid);
                if (it == _sidePanelAiResultViews.end() || !it->second.base.cardBack)
                {
                    return;
                }
                SidePanelStyleCardBorder(it->second.base.cardBack, SidePanelAiRingColor(it->second), selected, it->second.base.hovered);
                SidePanelAiApplyItemStatus(it->second, selected); // "selected" follows the ring
            };
            if (!previous.empty())
            {
                paint(previous, false);
            }
            if (!current.empty())
            {
                paint(current, true);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiRestyleSelection"); // only a ring is lost
        }
    }

    void TerminalPage::_SidePanelAiClickResult(const std::wstring& id)
    {
        try
        {
            const auto it = _sidePanelAiResultViews.find(id);
            if (it == _sidePanelAiResultViews.end())
            {
                return;
            }
            if (it->second.known)
            {
                // Like a session card's click: the summary area peeks it until a click into the terminal ends the peek. Only
                // an OPEN session drives the Manager lens (a closed one has no card there to select) and reveals its tab on
                // the Agent Manager page.
                const bool open = it->second.open;
                _SidePanelSelectSession(id, /*driveManagerLens*/ open);
                if (open)
                {
                    _RevealClickedSessionTab(id);
                }
                return;
            }
            // Never opened in Agentmaster: the summary has no record to read, so the click opens the transcript reading view —
            // on a clean tick (an overlay over the window must not be built inside a pointer handler).
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak(), id]() {
                if (const auto self = weakThis.get())
                {
                    self->_ShowTranscriptOverlayForSession(id, L"side panel AI search"); // never throws
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiClickResult");
        }
    }

    void TerminalPage::_SidePanelAiOpenResult(const std::wstring& id)
    {
        try
        {
            // On a clean tick: a resume opens a tab, and nothing may change the tree inside the pointer handler (or the menu
            // click) that asked for it.
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak(), id]() {
                const auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    // The registry is read again NOW: the session may have opened or closed since its card was drawn.
                    const auto lean = self->_sessionRegistry ? self->_sessionRegistry->GetWithoutQueue(id) : std::nullopt;
                    if (lean && lean->live)
                    {
                        ::Agentmaster::LogNav(L"sidepanel ai-open " + ::Agentmaster::ShortId(id) + L" -> jump (its tab is open)");
                        self->_ActivateClaudeSession(winrt::hstring{ id }); // cross-window through the activate sinks
                        return;
                    }
                    const auto it = self->_sidePanelAiResultViews.find(id);
                    const std::wstring dir = lean ? lean->workingDir : (it != self->_sidePanelAiResultViews.end() ? it->second.dir : std::wstring{});
                    const std::wstring title = lean ? lean->title : (it != self->_sidePanelAiResultViews.end() ? it->second.title : std::wstring{});
                    if (dir.empty())
                    {
                        // The folder is not known yet (its index entry is still being read): a resume has nowhere to launch, so
                        // the transcript is what this double-click can show.
                        ::Agentmaster::LogNav(L"sidepanel ai-open " + ::Agentmaster::ShortId(id) + L" -> transcript (its folder is not known yet)");
                        self->_ShowTranscriptOverlayForSession(id, L"side panel AI search");
                        return;
                    }
                    ::Agentmaster::LogNav(L"sidepanel ai-open " + ::Agentmaster::ShortId(id) + L" -> resume here");
                    // The Sessions page's resume seam: transcript-gated (claude --resume, else a fresh conversation under the same
                    // id), the "Claude not detected" gate and its notice, an upserted record for a session never opened before.
                    self->_ResumeSessionFromDisk(id, dir, title);
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"side panel AI result: open");
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelAiOpenResult");
        }
    }

    MenuFlyout TerminalPage::_SidePanelAiMakeClosedResultMenu(const std::wstring& id)
    {
        MenuFlyout menu;
        const auto weakThis = get_weak();
        // Filled on Opening, from the session's facts AT OPEN TIME (the session may have been resumed elsewhere since the list
        // was drawn: then the first item jumps instead).
        menu.Opening([weakThis, id](const IInspectable& sender, const IInspectable&) {
            const auto self = weakThis.get();
            auto m = sender.try_as<MenuFlyout>();
            if (!self || !m)
            {
                return;
            }
            try
            {
                m.Items().Clear();
                const auto lean = self->_sessionRegistry ? self->_sessionRegistry->GetWithoutQueue(id) : std::nullopt;
                const auto it = self->_sidePanelAiResultViews.find(id);
                const bool listed = it != self->_sidePanelAiResultViews.end();
                const std::wstring dir = lean ? lean->workingDir : (listed ? it->second.dir : std::wstring{});
                const std::wstring title = lean ? lean->title : (listed ? it->second.title : std::wstring{});
                // A never-prompted source lets the fork seam derive a smart name (the Sessions page's rule).
                const std::wstring forkTitle = (lean || (listed && it->second.hasMessages)) ? title : std::wstring{};
                const bool openNow = lean && lean->live;

                // Resume here (or Jump to Tab, when it opened since): what the double-click does.
                MenuFlyoutItem resume;
                resume.Text(openNow ? L"Jump to Tab" : L"Resume here");
                resume.IsEnabled(openNow || !dir.empty());
                AgentSetTip(resume, openNow ? L"Switch to this session's tab \x2014 it is open now." : L"Continue this conversation in a new tab in this window (claude --resume), with its whole history.");
                resume.Click([weakThis, id](const IInspectable&, const RoutedEventArgs&) {
                    if (const auto s = weakThis.get())
                    {
                        s->_SidePanelAiOpenResult(id); // re-reads the registry and defers itself
                    }
                });
                m.Items().Append(resume);

                // Fork here ▸ and Open New Session Here ▸: the shared launch-model pick lists (Default, Specify…, the models,
                // the presets), each acting on a clean tick (the menu restores focus as it closes).
                const auto models = ::Agentmaster::LaunchMenuEntries(self->_appSettings.launchModels);
                MenuFlyoutSubItem fork;
                fork.Text(L"Fork here");
                fork.IsEnabled(!dir.empty());
                AgentSetTip(fork, winrt::hstring{ L"Branch a new conversation off this one in a new tab here \x2014 the original is left exactly as it is. Pick the model the fork starts on, or Default. " } + AgentModelEditHint());
                AgentFillModelPickItems(fork.Items(), models, [weakThis, id, dir, forkTitle](winrt::hstring model) {
                    if (const auto s = weakThis.get())
                    {
                        s->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id, dir, forkTitle, model]() {
                            if (const auto page = weakThis.get())
                            {
                                // The fork question first when the source is mid-turn (HOOKS.md "Interrupted turns"), then the fork
                                // seam for a session on disk — the Sessions page's row menu, in the foreground.
                                ForkLaunchRequest request;
                                request.sourceId = id;
                                request.dir = dir;
                                request.title = forkTitle;
                                request.model = std::wstring{ model };
                                request.background = false;
                                request.origin = L"sidepanel-ai";
                                page->_ForkAskingAboutRunningTurn(std::move(request));
                            }
                        });
                    }
                },
                                        self->_ModelSpecifyOpener(), self->_AddPresetsOpener());
                m.Items().Append(fork);
                MenuFlyoutSubItem fresh;
                fresh.Text(L"Open New Session Here");
                fresh.IsEnabled(!dir.empty());
                AgentSetTip(fresh, winrt::hstring{ L"Start a FRESH session in this session's folder. Pick the model it starts on, or Default. " } + AgentModelEditHint());
                AgentFillModelPickItems(fresh.Items(), models, [weakThis, dir](winrt::hstring model) {
                    if (const auto s = weakThis.get())
                    {
                        s->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, dir, model]() {
                            if (const auto page = weakThis.get())
                            {
                                page->_SpawnClaudeSession(winrt::hstring{ dir }, winrt::hstring{}, static_cast<uint32_t>(-1), model);
                            }
                        });
                    }
                },
                                        self->_ModelSpecifyOpener(), self->_AddPresetsOpener());
                m.Items().Append(fresh);

                m.Items().Append(MenuFlyoutSeparator{});
                MenuFlyoutItem show;
                show.Text(L"Show Transcript");
                AgentSetTip(show, L"Read the whole conversation in a view over the window, at the level you pick.");
                show.Click([weakThis, id](const IInspectable&, const RoutedEventArgs&) {
                    if (const auto s = weakThis.get())
                    {
                        s->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id]() {
                            if (const auto page = weakThis.get())
                            {
                                page->_ShowTranscriptOverlayForSession(id, L"side panel AI search"); // never throws
                            }
                        });
                    }
                });
                m.Items().Append(show);
                MenuFlyoutSubItem copy;
                copy.Text(L"Copy");
                MenuFlyoutItem copyId;
                copyId.Text(L"Session Id");
                copyId.Click([id](const IInspectable&, const RoutedEventArgs&) {
                    ::Agentmaster::LogNav(L"copy session-id " + ::Agentmaster::ShortId(id) + L" (side panel AI search)");
                    CopyTextToClipboard(id);
                });
                copy.Items().Append(copyId);
                MenuFlyoutItem copyPath;
                copyPath.Text(L"Folder Path");
                copyPath.IsEnabled(!dir.empty());
                copyPath.Click([id, dir](const IInspectable&, const RoutedEventArgs&) {
                    ::Agentmaster::LogNav(L"copy path " + ::Agentmaster::ShortId(id) + L" (side panel AI search)");
                    CopyTextToClipboard(dir);
                });
                copy.Items().Append(copyPath);
                m.Items().Append(copy);
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"side panel AI result: closed-session menu"); // an empty menu rather than a dead card
            }
        });
        return menu;
    }
}
