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
//            + an order toggle (the SHARED AppSettings::boardSort, cycled through
//            AgentManagerContent::CycleBoardSort so the board and this list can never sort by
//            different specs) over a scrollable card list. A card single-click SELECTS (Linked
//            Lenses: it also drives the Manager lens), double-click ACTIVATES (jump to the tab,
//            cross-window), right-click shows THE session context menu — the exact lazily-filled
//            MenuFlyout the board card / tree row shows (AgentManagerContent::BuildSessionMenuFor).
//            The header's ⌖ icon toggle (GLOBAL AppSettings::sidePanelScrollToSelectedTab, default ON)
//            makes SELECTING A TAB scroll this list to that tab's card — one scroll per selection,
//            deferred to a clean tick, never moving a card that is already fully visible.
//   BOTTOM — the SELECTED session's summary: a REAL AgentTabOverlay in fill-host mode
//            (SetSummaryFillHost), i.e. the per-tab pencil panel's whole business logic — analyze/
//            render off-thread, times bar, wrap/truncate/history toggles, JUMP buttons, context
//            menus — reused verbatim, just sized by this host instead of anchoring over a terminal.
//   "Sum" OFF (SIDE_PANEL.md §2d) — the header's first toggle hides the BOTTOM area (and its
//            splitter): the cards fill the body, and each card shows the RICH SESSION TOOLTIP a tab
//            header shows — the same AgentSessionToolTipHost lifecycle and the same card push
//            (TerminalPage::_PushSessionToolTipCard) — placed just past the panel's edge, beside the
//            hovered card (right of a left-docked panel, left of a right-docked one).
//
// PERSISTENCE (the user's contract): both separators — the VERTICAL one (panel width) and the
// HORIZONTAL one (cards/summary split) — persist PER WINDOW (WindowRecord::sidePanel*, riding the
// window-record autosave), and EVERY adjustment additionally RMWs the GLOBAL AppSettings defaults
// ("the last one to resize sets the default for new windows"). Collapse follows the same rule; the
// dock side (LEFT by default; the header ⇄ flips it) is GLOBAL-only.
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
#include "AgentSessionToolTip.h" // the cards' rich session tooltip host ("Sum" off — the tab tooltip's lifecycle, shared)
#include "AgentTabOverlay.h" // the fill-hosted summary panel (SetSummaryFillHost)
#include "AgentCatchLog.h" // AgentLogCaughtException — swallowed-exception forensics (Rule #18)
#include "AgentTipHelpers.h" // AgentSetTip / AgentSetTitledTip — the islands-safe hover tooltips
#include "AgentStatusColors.h" // ParseArgbHexColor — the pending-dots color pair
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / LogNav / ShortId
#include "AgentMaster/Engine.h" // BroadcastSettingsChanged — the ⌖ scroll-to-tab toggle reaches every OTHER open window live
#include "AgentMaster/KeyedListDiff.h" // PlanKeyedListOps / ApplyKeyedListOp — the card list's fewest removes / inserts / moves (SIDE_PANEL.md §2c)
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings + EffectiveWorkingDir / ResolveSessionColorHex + LoadDirColorIndex (one color snapshot per list update)
#include "AgentMaster/ProcessInspect.h" // ExternalClaudeRow (the Internal.h MakeSortKey overload's parameter type)
#include "AgentMaster/ProfileBootstrap.h" // (Internal.h convention — harmless here, keeps the include set board-identical)
#include "AgentMaster/SessionRegistry.h" // SnapshotLiveForCards / Get
#include "AgentMaster/SessionSearch.h" // ParseSessionQuery / MatchesQueryText / MatchesPathQuery — the Sessions grammar, reused
#include "AgentMaster/SessionStore.h" // GetSessionTags / LoadAllTagColors / SessionTagsEpoch — the cards' bookmark-tag strip (cached per card until a tag changes)

#include <algorithm>
#include <chrono> // kCardTipDelay (the Internal.h card-tip hold-back) + the tip helpers' delay overrides
#include <cmath>
#include <cwctype> // towlower — the fold for the in-process matchers
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
    constexpr double kSidePanelSplitterPx = 6.0; // both separators' thickness (matches the overlay grips)
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
    // The header's ICON buttons (⌖ scroll-to-tab, ⇄ dock flip, «/» collapse) and the collapsed strip's chevron:
    // one size above the 11px text buttons (Sum / LOCAL / sort), so the single-glyph controls read as clearly as
    // the worded ones beside them. One place, so the three header icons and the strip can never drift apart.
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

    // Everything ONE card shows, computed from its live session (the lean SnapshotLiveForCards copy) and
    // this update's shared lookups — a plain record that _SidePanelApplyCardModel compares, value by value,
    // with the one the card shows now.
    //   s                 — the session (its queue and transcript texts are empty in the lean copy; nothing
    //                       here reads them).
    //   mode              — the GLOBAL tab-color mode (the band color and the effective working dir).
    //   tags              — the session's bookmark tags in add order (the card's tag cache).
    //   tagColors         — this tags epoch's LoadAllTagColors() (empty when no rendered card is tagged).
    //   dirColors         — this update's LoadDirColorIndex() snapshot.
    //   pendingArgb       — the pending dots' color, 0xAARRGGBB (the configured LIGHT color: card bodies are dark).
    //   animationsEnabled — Windows' "Animation effects" (AgentAnimationsEnabled), read once per update.
    //   returns           — the model.
    //   throws            — std::bad_alloc on allocation failure only.
    winrt::TerminalApp::implementation::AgentSidePanelCardModel SidePanelCardModelFor(const ::Agentmaster::SessionInfo& s,
                                                                                      ::Agentmaster::TabColorMode mode,
                                                                                      const std::vector<std::wstring>& tags,
                                                                                      const std::map<std::wstring, std::wstring>& tagColors,
                                                                                      const ::Agentmaster::DirColorIndex& dirColors,
                                                                                      uint32_t pendingArgb,
                                                                                      bool animationsEnabled)
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
            // Windows' animation switch is folded in HERE (not only inside BuildPendingDots) so that turning
            // it on or off changes the model, and the dots of every card are rebuilt in the new mode.
            m.pendingAnimated = (s.started || s.external) && animationsEnabled;
            m.pendingArgb = pendingArgb;
        }
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
        m.tipTitle = s.title;
        m.tipText = dir + (s.branch.empty() ? L"" : (L"\nbranch: " + s.branch)) + tagsLine + L"\n\nClick to select \x00B7 double-click to jump to its tab \x00B7 right-click for actions.";
        return m;
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

        // Header: TWO lines.
        //   line 1 (row 0): [ spacer * ][ Sum ][ scope ][ sort ][ ⌖ ][ ⇄ ][ chevron ] — the buttons keep the
        //     right-aligned places they had when the filter box shared this line (column 0, the star, is
        //     now an empty spacer), "Sum" first (left of LOCAL/GLOBAL — it reshapes the whole panel), the
        //     three icons together at the end, the collapse chevron outermost.
        //   line 2 (row 1): the filter box across the FULL width. On a line of its own it no longer
        //     competes with the buttons for room — sharing one line, a narrow panel squeezed the box down
        //     to its minimum while the buttons kept their natural widths.
        Grid header{};
        {
            RowDefinition hr0{};
            hr0.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            RowDefinition hr1{};
            hr1.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            header.RowDefinitions().Append(hr0);
            header.RowDefinitions().Append(hr1);
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
            ColumnDefinition c5{};
            c5.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c6{}; // the "Sum" toggle's column (column 1) pushed the others one to the right
            c6.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            header.ColumnDefinitions().Append(c0);
            header.ColumnDefinitions().Append(c1);
            header.ColumnDefinitions().Append(c2);
            header.ColumnDefinitions().Append(c3);
            header.ColumnDefinitions().Append(c4);
            header.ColumnDefinitions().Append(c5);
            header.ColumnDefinitions().Append(c6);
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
        // Line 2, spanning EVERY column — counted, not hard-coded, so a button added to line 1 later (a new
        // column) can't silently leave the box one column short of the right edge.
        Grid::SetRow(_sidePanelSearchBox, 1);
        Grid::SetColumn(_sidePanelSearchBox, 0);
        Grid::SetColumnSpan(_sidePanelSearchBox, static_cast<int32_t>(header.ColumnDefinitions().Size()));
        header.Children().Append(_sidePanelSearchBox);

        // The "Sum" toggle — the summary area below the cards ON (checked, the default) / OFF. OFF hides the
        // bottom area and its splitter so the cards fill the body, and each card then shows the RICH session
        // tooltip beside the panel on hover — the very card a tab header shows (SIDE_PANEL.md §2d). A REAL
        // ToggleButton (the ⌖ idiom), so its accent fill IS the state; IsChecked is seeded by
        // _SidePanelUpdateSumButton once the panel is built (a programmatic IsChecked raises no Click, so
        // seeding can never persist anything). A worded button like LOCAL/sort, so it keeps their 11px size.
        _sidePanelSumBtn = winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton{};
        _sidePanelSumBtn.FontSize(11);
        _sidePanelSumBtn.Padding(ThicknessHelper::FromLengths(8, 1, 8, 1));
        _sidePanelSumBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelSumBtn.IsTabStop(false); // like its siblings: a click must never pull keyboard focus out of the terminal
        _sidePanelSumBtn.Content(winrt::box_value(L"Sum"));
        AgentSetTitledTip(_sidePanelSumBtn, L"Summary area", winrt::hstring{ L"On: the selected session's summary shows below the cards.\nOff: the cards fill the panel, and hovering a card shows that session's full card beside the panel \x2014 the same card a tab shows when you hover it (scroll it with the mouse wheel).\nThis window remembers its own choice; the last change becomes the default for new windows." });
        _sidePanelSumBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleSummary();
            }
        });
        Grid::SetColumn(_sidePanelSumBtn, 1);
        header.Children().Append(_sidePanelSumBtn);

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
        Grid::SetColumn(_sidePanelScopeBtn, 2);
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
        Grid::SetColumn(_sidePanelSortBtn, 3);
        header.Children().Append(_sidePanelSortBtn);

        // The ⌖ SCROLL-TO-TAB icon toggle: while ON (the GLOBAL AppSettings::sidePanelScrollToSelectedTab,
        // default ON) selecting a tab scrolls this list to that tab's card. A REAL ToggleButton — the
        // Sessions page's scope-toggle idiom (SessToggle) — so the accent fill of its checked state IS the
        // on/off readout; its plain-Button siblings don't need one (their labels carry their state, and ⇄ /
        // the chevron are actions, not modes). Sized exactly like the ⇄ dock button beside it (same
        // FontSize/Padding; ⌖ even comes from the same Segoe UI Symbol fallback as ⇄), so the three icons
        // read as one group. IsChecked is seeded by _SidePanelUpdateScrollToTabButton once the panel is
        // built — a programmatic IsChecked raises no Click, so seeding can never persist or broadcast.
        _sidePanelScrollToTabBtn = winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton{};
        _sidePanelScrollToTabBtn.FontSize(kSidePanelIconFontSize); // an ICON button: one size above the 11px worded buttons
        _sidePanelScrollToTabBtn.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
        _sidePanelScrollToTabBtn.VerticalAlignment(VerticalAlignment::Center);
        _sidePanelScrollToTabBtn.IsTabStop(false); // like its siblings: a click must never pull keyboard focus out of the terminal
        _sidePanelScrollToTabBtn.Content(winrt::box_value(L"\x2316")); // ⌖ POSITION INDICATOR — "find my tab's card"
        AgentSetTitledTip(_sidePanelScrollToTabBtn, L"Scroll to the selected tab", winrt::hstring{ L"On: selecting a tab scrolls these cards to that tab's card \x2014 a card already on screen stays put. Clicking back into the terminal after peeking at another card scrolls back to your tab's card too.\nOff: the list keeps whatever scroll position you left it at.\nSaved globally \x2014 every open window follows it at once." });
        _sidePanelScrollToTabBtn.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_SidePanelToggleScrollToTab();
            }
        });
        Grid::SetColumn(_sidePanelScrollToTabBtn, 4);
        header.Children().Append(_sidePanelScrollToTabBtn);

        Button dockBtn{};
        dockBtn.FontSize(kSidePanelIconFontSize); // an ICON button: one size above the 11px worded buttons
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
        Grid::SetColumn(dockBtn, 5);
        header.Children().Append(dockBtn);

        _sidePanelCollapseBtn = Button{};
        _sidePanelCollapseBtn.FontSize(kSidePanelIconFontSize); // an ICON button: one size above the 11px worded buttons
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
        Grid::SetColumn(_sidePanelCollapseBtn, 6);
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
        _sidePanelStripBtn.FontSize(kSidePanelIconFontSize); // the collapsed strip's chevron — the same icon size as the header's collapse chevron it pairs with
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
        _SidePanelApplyDock(); // assigns the three columns per the global dock side + applies widths/glyphs (and the summary rows per "Sum")
        _SidePanelUpdateSumButton(); // "Sum" checked == the summary area shown (no Click => nothing persisted)
        _SidePanelUpdateSortButton();
        _SidePanelUpdateScopeButton(); // LOCAL / GLOBAL per the seeded per-window scope
        _SidePanelUpdateScrollToTabButton(); // ⌖ checked per the GLOBAL setting (no Click => nothing persisted)
        _SidePanelRebuildCards(); // with "Sum" off, every new card is armed with its rich tooltip as it is built
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
                                          (_sidePanelSummaryHidden ? L" summary=hidden(card-tooltips)" : L" summary=shown") +
                                          (_sidePanelGlobal ? L" scope=global" : L" scope=local") +
                                          (_appSettings.sidePanelScrollToSelectedTab ? L" scroll-to-tab=on" : L" scroll-to-tab=off") +
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
    // live UI-thread control can (the sibling label updaters' exposure).
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
        _SidePanelApplyDock();
    }

    // The ⌖ header toggle's click: flip the GLOBAL "scroll to the selected tab" preference
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
        // The ToggleButton flipped its own IsChecked before Click fired; re-pin it to the stored truth anyway,
        // so a button that ever drifted from the setting (a missed broadcast) can't stay inverted.
        _SidePanelUpdateScrollToTabButton();
        ::Agentmaster::LogNav(next ? L"sidepanel scroll-to-tab -> on" : L"sidepanel scroll-to-tab -> off");
        try
        {
            // Every OTHER window (the source is excluded): each hops to its own UI thread and re-pins its
            // toggle through _ApplyBroadcastSettings -> _SidePanelApplySettings.
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
            _sidePanelRevealPendingId.clear();
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

    // Pin the ⌖ toggle's checked state to the GLOBAL setting — the setting is the truth, the
    // ToggleButton's own click-flip is not. Called at build, after a click, and whenever a cog Save or
    // another window's broadcast replaced _appSettings (_SidePanelApplySettings). A programmatic IsChecked
    // raises Checked/Unchecked but never Click, so re-pinning can't loop back into the toggle handler or
    // persist anything. No parameters; returns nothing; a no-op before the panel is built. Throws only
    // what a XAML property set on a live UI-thread control can (the sibling label updaters' exposure).
    void TerminalPage::_SidePanelUpdateScrollToTabButton()
    {
        if (!_sidePanelScrollToTabBtn)
        {
            return;
        }
        _sidePanelScrollToTabBtn.IsChecked(_appSettings.sidePanelScrollToSelectedTab);
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
        if (!_sidePanelBuilt || _sidePanelCollapsed || _sidePanelRevealPendingId.empty() || _sidePanelRevealQueued)
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

    // The reveal tick's body: pay the owed scroll when its card is RENDERED. UpdateLayout first — a card
    // created by the rebuild that queued this tick has no size until measured, and StartBringIntoView on
    // it would silently do nothing — then StartBringIntoView: the Manager board's own recipe
    // (BringSelectedIntoView). Default BringIntoViewOptions mean the MINIMUM scroll that shows the card, so
    // a card already fully on screen never moves and a far one stops at the nearest edge. The request walks
    // up to the cards ScrollViewer only (the panel has no outer scroller) and never touches keyboard focus,
    // so the terminal keeps its typing focus through every reveal. The debt is consumed BEFORE the XAML
    // calls, so a throwing reveal can't turn into one that retries on every later rebuild; a card that is
    // NOT rendered yet (filtered out, not built) keeps it owed for the next rebuild.
    // No parameters; returns nothing. Never throws: XAML failures (a teardown race, a layout error) are
    // logged (Rule #18) and swallowed — a lost scroll must never cost the window.
    void TerminalPage::_SidePanelRevealPendingNow()
    {
        if (!_sidePanelBuilt || _sidePanelCollapsed || _sidePanelRevealPendingId.empty())
        {
            return; // collapsed: the expand path re-queues; nothing owed: a stale tick
        }
        if (!_appSettings.sidePanelScrollToSelectedTab)
        {
            _sidePanelRevealPendingId.clear(); // switched off (here or by a broadcast) after the request was made
            return;
        }
        const auto it = _sidePanelCardViews.find(_sidePanelRevealPendingId);
        if (it == _sidePanelCardViews.end() || !it->second.card)
        {
            return; // no card for it (yet) — _SidePanelRebuildCards re-queues; a newer tab selection replaces the debt
        }
        const auto card = it->second.card; // a strong ref: layout below can't release it under us
        _sidePanelRevealPendingId.clear();
        try
        {
            if (_sidePanelCards)
            {
                _sidePanelCards.UpdateLayout();
            }
            card.StartBringIntoView();
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
    // `width`/`split`/`collapsed`/`summaryHidden`: which live values to export as the new-window defaults
    // (`summaryHidden` defaults to false so the three older call sites read unchanged).
    void TerminalPage::_SidePanelPersistDefaults(bool width, bool split, bool collapsed, bool summaryHidden)
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
        ::Agentmaster::SaveAppSettings(s);
        // Adopt the merged set (not just the flagged one): the freshest disk may carry another
        // window's newer defaults, and _appSettings must mirror what is actually stored.
        _appSettings.sidePanelWidthFraction = s.sidePanelWidthFraction;
        _appSettings.sidePanelSplitFraction = s.sidePanelSplitFraction;
        _appSettings.sidePanelCollapsed = s.sidePanelCollapsed;
        _appSettings.sidePanelSummaryHidden = s.sidePanelSummaryHidden;
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

    // A bookmark tag was added to, removed from, or recolored on a session FROM THIS WINDOW: the Tags
    // panel's two mutation paths (_ToggleSessionTag, _CommitTagEditorAdd) call this right after the
    // board's _RefreshManagerBoardTags. Tags live in the SessionStore, never the registry, so no
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
    // full registry copy per card.
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
        // Tags belt: a tag added, removed or recolored in ANOTHER window (or on the Sessions page) fires no
        // registry notify and no _SidePanelNoteTagsChanged here — but it moves the process-wide tag epoch the
        // cards' tag cache was read at. One atomic load per tick.
        if (!_sidePanelDirty && ::Agentmaster::SessionTagsEpoch() != _sidePanelTagsEpoch)
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
    // key per session). Then one AgentSidePanelCardModel per row, and the list is EDITED, never rebuilt:
    //   - a session that keeps its card gets only the values that changed written (_SidePanelApplyCardModel);
    //   - a session new to the list gets a card (_SidePanelCreateCardView + a fresh apply);
    //   - the order changes with the fewest removes / inserts / moves (PlanKeyedListOps). A XAML Move is a
    //     plain reorder of the panel's items (no Unloaded/Loaded), so a card that moves keeps its elements,
    //     its pulsing dots and its hover state.
    // In the steady state (one session changed its state) that is one card's dot, label and border. The old
    // Clear() + rebuild re-created every card — ~466 in the release's biggest window — up to twice a second.
    // The reads are lean too: each card's tags come from the per-card cache, re-read only when
    // SessionTagsEpoch moved (a tag write anywhere in this process) or for a card new to the list, and every
    // card's color resolves against ONE LoadDirColorIndex snapshot. Also refreshes the count line (which names
    // the scope; written only when its text changed), every card's timing text (so the ~30s timing pass is
    // due only after 30 quiet seconds), and the hosted set the periodic LOCAL belt diffs against; and pays an
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

            // The rows, each with its sort key built ONCE: a comparator that built both keys per comparison
            // (a title copy each) cost O(n log n) string copies per update.
            struct CardRow
            {
                SortKey key;
                const ::Agentmaster::SessionInfo* session;
            };
            std::vector<CardRow> rows;
            rows.reserve(snap.size());
            size_t liveTotal = 0; // the IN-SCOPE total (what "N of M" counts against — never another window's sessions under LOCAL)
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
                if (SidePanelSessionMatches(s, terms, mode))
                {
                    rows.push_back(CardRow{ MakeSortKey(s), &s });
                }
            }
            std::stable_sort(rows.begin(), rows.end(), [this](const CardRow& a, const CardRow& b) {
                return SortKeyLess(_appSettings.boardSort, a.key, b.key);
            });

            // Bookmark tags, from the per-card cache. The epoch is loaded BEFORE any read (the SessionStore.h
            // contract): a write landing during the reads below leaves the older epoch stored here, so the next
            // update re-reads — a cache refreshed once too often, never kept stale. A moved epoch means some tag
            // or tag color changed in this process since the last read, so everything cached is dropped. A card
            // new to the list reads its tags once (GetSessionTags: SessionStore's (size,mtime)-validated record
            // cache — one file stat); a card that stays reads nothing. Rows the filter hid are never read.
            const uint64_t tagsEpoch = ::Agentmaster::SessionTagsEpoch();
            if (tagsEpoch != _sidePanelTagsEpoch)
            {
                _sidePanelTagsById.clear();
                _sidePanelTagColors.clear();
                _sidePanelTagColorsLoaded = false;
                _sidePanelTagsEpoch = tagsEpoch;
            }
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
            if (_sidePanelTagsById.size() > rows.size())
            {
                // Keep only the rendered cards' entries, so the cache can't grow with every session that ever
                // passed through the list (a card that comes back re-reads its tags once). Node handles move
                // each kept entry over without copying its strings.
                std::unordered_map<std::wstring, std::vector<std::wstring>> kept;
                kept.reserve(rows.size());
                for (const auto& row : rows)
                {
                    auto node = _sidePanelTagsById.extract(row.session->id);
                    if (!node.empty())
                    {
                        kept.insert(std::move(node));
                    }
                }
                _sidePanelTagsById.swap(kept);
            }
            if (anyTagged && !_sidePanelTagColorsLoaded)
            {
                // The colors file is read once per epoch, and only when a rendered card is tagged at all — an
                // untagged fleet never pays for it.
                _sidePanelTagColors = ::Agentmaster::LoadAllTagColors();
                _sidePanelTagColorsLoaded = true;
            }

            // One model per card. The shared inputs are read ONCE per update: the dir-colors snapshot every
            // card's band color resolves against, the dots' color, and Windows' animation switch.
            const auto dirColors = ::Agentmaster::LoadDirColorIndex();
            const uint32_t pendingArgb = SidePanelPackArgb(ParseArgbHexColor(_appSettings.pendingDotsLightColor, Color{ 0xFF, 0xE0, 0xA9, 0x2B }));
            const bool animationsEnabled = AgentAnimationsEnabled();
            std::vector<std::wstring> order; // the new card order, as session ids — the planner's target
            std::vector<AgentSidePanelCardModel> models; // parallel to `order`
            // Parallel to `order` too: each card's lean session row (pointers into `snap`, alive for this whole
            // update) — the "Sum" off mode arm-builds a NEW card's rich tooltip from it instead of a per-card
            // registry read.
            std::vector<const ::Agentmaster::SessionInfo*> sessions;
            order.reserve(rows.size());
            models.reserve(rows.size());
            sessions.reserve(rows.size());
            // Belt: the planner needs unique ids, and a session listed twice must get ONE card. The registry
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
                models.push_back(SidePanelCardModelFor(*row.session, mode, _sidePanelTagsById[row.session->id], _sidePanelTagColors, dirColors, pendingArgb, animationsEnabled));
                sessions.push_back(row.session);
            }

            // The panel and its mirror must agree before any in-place edit: every index the plan carries is
            // computed on the mirror. They always do — the list is only ever edited here and by
            // _SidePanelResetCards — so a mismatch means an earlier edit failed halfway; start from empty.
            auto children = _sidePanelCards.Children();
            if (children.Size() != _sidePanelCardOrder.size() || _sidePanelCardViews.size() != _sidePanelCardOrder.size())
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[sidepanel] card list out of step with its mirror (panel " + std::to_wstring(children.Size()) +
                                                  L", mirror " + std::to_wstring(_sidePanelCardOrder.size()) + L", cards " +
                                                  std::to_wstring(_sidePanelCardViews.size()) + L") - rebuilt from empty (window " +
                                                  ::Agentmaster::ShortId(_windowId) + L")\n");
                _SidePanelResetCards();
            }

            // The cards that stay: write only what changed. The new ones: build them now (detached until the
            // plan's Insert puts them in the list).
            for (size_t i = 0; i < order.size(); ++i)
            {
                const auto it = _sidePanelCardViews.find(order[i]);
                if (it != _sidePanelCardViews.end())
                {
                    _SidePanelApplyCardModel(it->second, std::move(models[i]), order[i], /*fresh*/ false);
                }
                else
                {
                    const auto created = _sidePanelCardViews.emplace(order[i], _SidePanelCreateCardView(order[i])).first;
                    _SidePanelApplyCardModel(created->second, std::move(models[i]), order[i], /*fresh*/ true);
                    if (_sidePanelSummaryHidden)
                    {
                        // "Sum" off: a NEW card gets its rich tooltip now (header-only arm-build; the host attaches
                        // it on the card's Loaded, before any pointer can reach it). Contained inside — a tooltip
                        // failure never resets the list. The fresh apply above already built the tag strip in this
                        // mode, so no strip rebuild.
                        _SidePanelApplyCardTipMode(created->second, order[i], /*rebuildTags*/ false, sessions[i]);
                    }
                }
            }

            // The order: the fewest removes / inserts / moves (KeyedListDiff.h), each made on the panel and on
            // the mirror in step. A removed card's view is erased right after its RemoveAt — its handlers hold
            // the page only weakly and reach the card through `sender`, so nothing keeps it alive after that.
            bool mirrorBroken = false;
            for (const auto& op : ::Agentmaster::PlanKeyedListOps(_sidePanelCardOrder, order))
            {
                switch (op.kind)
                {
                case ::Agentmaster::KeyedListOpKind::Remove:
                    children.RemoveAt(op.from);
                    if (const auto leaving = _sidePanelCardViews.find(op.key); leaving != _sidePanelCardViews.end())
                    {
                        // Detach its rich tooltip BEFORE the view goes: the host's own Unloaded detach can run
                        // after the host is gone (the event is deferred), and a ToolTip left attached to a dropped
                        // element is the islands leak class (AgentTipHelpers.h). Never throws.
                        _SidePanelDropCardToolTip(leaving->second, op.key);
                    }
                    _sidePanelCardViews.erase(op.key);
                    break;
                case ::Agentmaster::KeyedListOpKind::Insert:
                    children.InsertAt(op.to, _sidePanelCardViews.at(op.key).card);
                    break;
                case ::Agentmaster::KeyedListOpKind::Move:
                    children.Move(op.from, op.to); // a reorder of the panel's item array — no Unloaded/Loaded
                    break;
                }
                if (!::Agentmaster::ApplyKeyedListOp(_sidePanelCardOrder, op))
                {
                    mirrorBroken = true; // an index out of range: the mirror no longer describes the panel
                    break;
                }
            }
            if (mirrorBroken || _sidePanelCardOrder != order || _sidePanelCardViews.size() != order.size() || children.Size() != order.size())
            {
                // A planned edit applied in order can't end anywhere else — this would be a planner or mirror
                // bug, or a panel that kept its old children because the reset above could not Clear() it (the
                // mirror then planned from empty while the panel was not). Editing on from a wrong mirror would
                // only compound it, so start over from empty instead.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[sidepanel] card list edit ended out of step with its plan - rebuilding from empty (window " + ::Agentmaster::ShortId(_windowId) + L")\n");
                _SidePanelResetCards();
                _sidePanelDirty = true;
                return;
            }

            if (_sidePanelCountText)
            {
                const std::wstring shown = std::to_wstring(order.size());
                const std::wstring total = std::to_wstring(liveTotal);
                // The count names the scope, so a short list under LOCAL reads as "this window" rather
                // than "the fleet shrank" — the cue that the toggle exists when a session seems missing.
                const std::wstring where = local ? L" in this window" : L" across all windows";
                std::wstring countText = terms.empty() ? (total + L" session" + (liveTotal == 1 ? L"" : L"s") + where) : (shown + L" of " + total + L" sessions" + where);
                if (countText != _sidePanelCountValue)
                {
                    _sidePanelCountText.Text(winrt::hstring{ countText });
                    _sidePanelCountValue = std::move(countText);
                }
            }
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

    // Drop every card: the panel's children, the id -> card map, the order mirror and the remembered count
    // text. The recovery path of _SidePanelRebuildCards — when the panel and its mirror disagree, or an update
    // failed halfway — after which the next update rebuilds the list from empty (the planner then fills it
    // front to back). The bookmark-tag caches are DATA, not UI, and stay.
    // No parameters; returns nothing. Never throws: a panel that refuses Clear() is logged (Rule #18) and left
    // non-empty, which the next update sees as out of step with the (now empty) mirror — and calls this again.
    void TerminalPage::_SidePanelResetCards()
    {
        // Every card's rich tooltip is detached first (the "Sum" off mode attaches one per card): a ToolTip left
        // attached to a dropped element is the islands leak class. _SidePanelDropCardToolTip never throws.
        for (auto& [id, view] : _sidePanelCardViews)
        {
            _SidePanelDropCardToolTip(view, id);
        }
        // The maps first: clearing them cannot fail, so the page's view of the list is consistently "empty"
        // even when the panel below refuses its Clear.
        _sidePanelCardViews.clear();
        _sidePanelCardOrder.clear();
        _sidePanelCountValue.clear(); // the next update writes the count line again
        if (!_sidePanelCards)
        {
            return;
        }
        try
        {
            _sidePanelCards.Children().Clear();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SidePanelResetCards");
        }
    }

    // Build ONE card's skeleton — the Triage-Board card's visual language re-hosted for a narrow vertical
    // list: a title on a band, then a state line (a row with the dot + label in the shared AgentStatusColors
    // palette, followed by whatever optional parts the model adds — the teal `codex` pill, the pending-draft
    // dots — and, in the line's star column, the "created 4d1h, active 2m4s ago" timing text), then a dim
    // dir/branch line. Every card has these elements; their VALUES — texts, brushes,
    // the band's tag strip, the optional parts — come from _SidePanelApplyCardModel, so a card is built once
    // per session and from then on only edited.
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
        AgentSidePanelCardView v;
        v.card = Border{};
        v.card.Background(Fill(0xFF, 0x33, 0x33, 0x33));
        v.card.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        v.card.Margin(ThicknessHelper::FromLengths(4, 2, 4, 2));
        v.card.BorderThickness(ThicknessHelper::FromUniformLength(1)); // constant width — hover/selection recolor, never reflow

        StackPanel body{};
        body.Orientation(Orientation::Vertical);

        // Title band: its color and the title's ink come from the model (the session's resolved color, the
        // same visual as the board card's band, so same-dir cards cluster by color here too).
        v.band = Border{};
        v.band.CornerRadius(CornerRadiusHelper::FromRadii(3, 3, 0, 0));
        v.band.Padding(ThicknessHelper::FromLengths(6, 2, 6, 2));
        v.title = TextBlock{};
        v.title.FontSize(12);
        v.title.FontWeight(FontWeights::SemiBold());
        v.title.TextWrapping(TextWrapping::NoWrap);
        v.title.TextTrimming(TextTrimming::CharacterEllipsis);
        v.band.Child(v.title); // the untagged layout; _SidePanelApplyCardTags wraps the title in [ title * ][ strip ] for a tagged session
        body.Children().Append(v.band);

        // State line: [ state row Auto ][ timing * ]. The row holds the dot and the label, and the model inserts the
        // codex pill and the draft dots after them; the timing text goes in the star column (added by the model too),
        // so on a narrow panel it is the part that gives way, ending in "…" (Text's CharacterEllipsis) — in the row's
        // StackPanel it was offered unlimited width and got cut off mid-letter at the card's edge instead.
        v.stateLine = Grid{};
        v.stateLine.Margin(ThicknessHelper::FromLengths(6, 3, 6, 0));
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
        v.stateDot = winrt::Windows::UI::Xaml::Shapes::Ellipse{};
        v.stateDot.Width(8);
        v.stateDot.Height(8);
        v.stateDot.VerticalAlignment(VerticalAlignment::Center);
        v.stateRow.Children().Append(v.stateDot);
        v.stateText = Text(L"", 10, false, 0.9);
        v.stateRow.Children().Append(v.stateText);
        Grid::SetColumn(v.stateRow, 0);
        v.stateLine.Children().Append(v.stateRow);
        body.Children().Append(v.stateLine);

        // Dir/branch line: "<leaf>[ / <branch>]", dim — the card's place at a glance (the full path rides the tooltip).
        v.dirLine = Text(L"", 10, false, 0.55);
        v.dirLine.Margin(ThicknessHelper::FromLengths(6, 0, 6, 4));
        body.Children().Append(v.dirLine);

        v.card.Child(body);

        const auto weakThis = get_weak();
        // Hover ring — guarded by PointerStillWithin so a child-bubbled exit (crossing between the card's
        // hit-testable labels) can't strobe the ring off while the pointer is still inside. The ring takes the
        // color of the state the card SHOWS (its view's model — what its dot shows, with no registry copy per
        // pointer event), and the view remembers the hover so a state change repaints the ring in the new color.
        // ⚠ The handlers reach the card ONLY through `sender` — capturing the Border itself in a handler
        // attached TO it is a self-reference cycle (element -> handler -> element) that would leak every card
        // ever dropped from the list (the board's cards capture weakly for the same reason). A sender that is
        // not the card registered for `id` (a card already dropped, or replaced by a reset) is ignored.
        v.card.PointerEntered([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs&) {
            const auto self = weakThis.get();
            const auto b = sender.try_as<Border>();
            if (!self || !b)
            {
                return;
            }
            const auto it = self->_sidePanelCardViews.find(id);
            if (it == self->_sidePanelCardViews.end() || it->second.card != b)
            {
                return; // not the live card of this session any more — nothing of ours to paint
            }
            it->second.hovered = true;
            SidePanelStyleCardBorder(b, StateColor(it->second.model.state), id == self->_sidePanelSelectedId, /*hover*/ true);
        });
        v.card.PointerExited([weakThis, id](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            const auto self = weakThis.get();
            const auto b = sender.try_as<Border>();
            if (!self || !b || PointerStillWithin(sender, e))
            {
                return; // bubbled from a child — the pointer never actually left the card
            }
            const auto it = self->_sidePanelCardViews.find(id);
            if (it == self->_sidePanelCardViews.end() || it->second.card != b)
            {
                return; // not the live card of this session any more
            }
            it->second.hovered = false;
            SidePanelStyleCardBorder(b, StateColor(it->second.model.state), id == self->_sidePanelSelectedId, /*hover*/ false);
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
                        auto menu = mgr->BuildSessionMenuFor(id, cwd, sender.try_as<FrameworkElement>());
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
        return v;
    }

    // Write `model` into a card IN PLACE: every value that differs from the one the card shows now (its
    // view.model) is written — a text, a brush, the border — and an optional part (the codex pill, the pending
    // dots, the timing text, the band's tag strip) is added or dropped only when its presence changes. A value
    // that did not change is never touched, so an update that changed nothing for this card costs it nothing.
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
        if (fresh || model.state != shown.state)
        {
            const Color stateColor = StateColor(model.state);
            view.stateDot.Fill(SolidColorBrush{ stateColor });
            view.stateText.Text(StateLabel(model.state));
            view.stateText.Foreground(SolidColorBrush{ stateColor });
            SidePanelStyleCardBorder(view.card, stateColor, id == _sidePanelSelectedId, view.hovered);
        }
        // The state row's optional parts, always in this order after the dot and the label:
        // [codex pill][pending dots]. An insert index is derived from the parts in front of it. The timing text is the
        // state LINE's star column (after the row), not a row part.
        if (model.codex && !view.codexPill)
        {
            view.codexPill = Text(L"codex", 9, true, 0.9);
            view.codexPill.Foreground(Fill(0xFF, 0x2E, 0xB8, 0xA8)); // the External/Codex teal pill color family
            view.stateRow.Children().InsertAt(2, view.codexPill); // right after the dot and the label
        }
        else if (!model.codex && view.codexPill)
        {
            SidePanelRemoveChild(view.stateRow, view.codexPill);
            view.codexPill = nullptr;
        }
        if (fresh || model.pending != shown.pending || model.pendingAnimated != shown.pendingAnimated || model.pendingArgb != shown.pendingArgb)
        {
            // Replaced whole — it changes rarely (a draft appears or goes, a dormant tab starts, the dots' color
            // or Windows' animation switch changes), and a cluster is three ellipses plus its pulse, so a new
            // one is simpler than retuning the old. The old cluster's Unloaded stops its pulse.
            if (view.pendingDots)
            {
                SidePanelRemoveChild(view.stateRow, view.pendingDots);
                view.pendingDots = nullptr;
            }
            if (model.pending)
            {
                // The unsent-draft indicator — the same BuildPendingDots cluster the board card shows, in the
                // user's configured LIGHT color (card bodies are always dark here).
                view.pendingDots = BuildPendingDots(4.0, SidePanelUnpackArgb(model.pendingArgb), model.pendingAnimated);
                const uint32_t dotsIndex = view.codexPill ? 3u : 2u; // after the dot, the label and the pill (when there is one)
                view.stateRow.Children().InsertAt(dotsIndex, view.pendingDots);
            }
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
            view.timing.Margin(ThicknessHelper::FromLengths(5, 0, 0, 0)); // the gap the row's own Spacing(5) gives its parts
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
    }

    // Give a card's band the layout its tags need. Untagged: the band is exactly the title, byte-identical to
    // a card from before tags existed. Tagged: [ title * ][ tag strip Auto ] — the strip takes the width its
    // (capped) ribbons need and the title takes the rest, trimming with its ellipsis, so the ribbons stay in
    // the band's top-right corner and a long title can never push them off the card. A changed tag list, tag
    // color or title ink REPLACES the strip (a few ribbons — simpler than editing them); the title element is
    // only re-parented between the band and the grid, never rebuilt.
    // `view`: the card. `model`: the values it must show (tags, their colors, the ink). `id`: the card's session —
    // handed to the strip so, with "Sum" off, its ribbons can suspend THIS card's rich tooltip while hovered.
    // Throws winrt::hresult_error if XAML refuses an edit — _SidePanelRebuildCards resets the list on a throw
    // (the "Sum" mode switch calls it inside _SidePanelApplyCardTipMode's catch).
    void TerminalPage::_SidePanelApplyCardTags(AgentSidePanelCardView& view, const AgentSidePanelCardModel& model, const std::wstring& id)
    {
        if (model.tags.empty())
        {
            if (view.bandGrid)
            {
                // Back to the plain band. The title leaves the grid FIRST: a XAML element has one parent, and
                // handing it to the band while the grid still holds it would throw.
                SidePanelRemoveChild(view.bandGrid, view.title);
                view.band.Child(view.title);
                view.bandGrid = nullptr;
                view.tagStrip = nullptr;
            }
            return;
        }
        std::vector<Color> colors;
        colors.reserve(model.tagArgb.size());
        for (const uint32_t argb : model.tagArgb)
        {
            colors.push_back(SidePanelUnpackArgb(argb));
        }
        // Hang the strip from the band's TOP EDGE: lift it by exactly the band's top padding, so the ribbons
        // start where the card's colored bar starts — bookmarks clipped to the top of the card rather than
        // badges floating mid-bar. Read back from the band (not a literal) so a later padding change can't
        // silently detach them from the edge.
        auto strip = _SidePanelMakeTagStrip(model.tags, colors, model.darkInk, view.band.Padding().Top, view.card, id);
        Grid::SetColumn(strip, 1);
        if (!view.bandGrid)
        {
            Grid bandGrid{};
            ColumnDefinition titleCol{};
            titleCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition tagCol{};
            tagCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            bandGrid.ColumnDefinitions().Append(titleCol);
            bandGrid.ColumnDefinitions().Append(tagCol);
            view.band.Child(nullptr); // release the title from the band before the grid takes it (one parent)
            Grid::SetColumn(view.title, 0);
            bandGrid.Children().Append(view.title);
            bandGrid.Children().Append(strip);
            view.band.Child(bandGrid);
            view.bandGrid = bandGrid;
        }
        else
        {
            if (view.tagStrip)
            {
                SidePanelRemoveChild(view.bandGrid, view.tagStrip);
            }
            view.bandGrid.Children().Append(strip);
        }
        view.tagStrip = strip;
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
    // `card`: the card Border whose tooltip must yield over the strip. The handlers hold it WEAKLY: the
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
                                                                                    const winrt::Windows::UI::Xaml::Controls::Border& card,
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
            // BEFORE the same-id early-out: re-clicking the already-selected card is just as explicit.
            _sidePanelRevealPendingId.clear();
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
            if (it == _sidePanelCardViews.end() || !it->second.card)
            {
                return; // filtered out / not rendered — nothing to paint
            }
            SidePanelStyleCardBorder(it->second.card, StateColor(it->second.model.state), sel, it->second.hovered);
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
    // cross-window settings broadcast (both run after _appSettings was replaced): re-label the
    // shared sort, re-dock if sidePanelOnLeft changed elsewhere, re-pin the ⌖ scroll-to-tab toggle
    // (dropping an owed reveal when it turned off), push the summary content flags +
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
        // ⌖ scroll-to-tab is GLOBAL: re-pin this window's toggle to the adopted value (another window's
        // click broadcast it here), and if it just turned OFF, drop a reveal still owed so nothing scrolls
        // after the user asked it not to.
        _SidePanelUpdateScrollToTabButton();
        if (!_appSettings.sidePanelScrollToSelectedTab)
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
        _SidePanelScheduleRebuild(/*force*/ true);
    }
}
