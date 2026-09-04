// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster per-tab link badge / overlay (4 partial files)
// The top-right terminal HUD on every classified tab + its pencil-toggled summary panel
// (TAB_OVERLAY.md). ONE class (AgentTabOverlay) split from the former 3221-line .cpp; all four
// share AgentTabOverlay.Internal.h.
//
// Partial files in this group (★ marks THIS file):
//   AgentTabOverlay.cpp          - CORE: ctor/dtor, Initialize/_Detach, ShowActivity (the observe badge), _Refresh (the linked badge), hover/expand, opacities, Autorunner
//   AgentTabOverlay.Internal.h   - shared file-local helpers: StateColor/Glyph/Label, the summary-box renderers, time formatting, launch-CLI + clipboard (anonymous namespace, a per-TU copy)
//   AgentTabOverlay.Actions.cpp  - the hover action row: the folder (Open Path) button, the copy menu, and the shared CopySessionField action
// ★ AgentTabOverlay.Summary.cpp  - the pencil-toggled summary panel: build/render/load off-thread, the times bar, resize grips, wrap/truncate/previous, JUMP, copy-summary
// ======================================================================================
//
// Agentmaster per-tab overlay: the pencil-toggled SUMMARY PANEL -- build/render/load (off-thread), the times bar, resize grips, wrap/truncate/previous toggles, JUMP eligibility + highlight, and the copy-summary action. Partial TU of AgentTabOverlay.cpp.
#include "pch.h"
#include "AgentTabOverlay.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (the context-menu ShowAt / Copy Prompt handlers)
#include "AgentCopyActions.h" // the shared CopySessionField (reused by the Triage Board's Copy submenu)
#include "AgentStatusColors.h" // the ONE shared state->color palette (board / overlay / tab dot)
#include "AgentTipHelpers.h" // AgentSetTip — the Dark-pinned, fast-open, stuck-proof hover tooltip recipe (vs raw ToolTipService)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeTranscriptPath / BuildClaude|CodexCommandline (row 3 CLI + transcript)
#include "AgentMaster/ProcessInspect.h" // ReadProcessCommandLine / ReadConversationText / Codex rollout resolve (row 3)
#include "AgentMaster/Persistence.h" // LoadAppSettings (skipPermissions, for the would-use CLI builder)
#include "AgentMaster/Engine.h" // SharedEngine (claudeExePath / codexExePath, for the real launch CLI)

#include <winrt/Windows.UI.h> // Color / ColorHelper / Colors
#include <winrt/Windows.UI.Core.h> // CoreWindow / CoreCursor (summary-panel resize-grip cursors)
#include <winrt/Windows.UI.Text.h> // FontWeights
#include <winrt/Windows.UI.Xaml.Documents.h> // Run / Inlines
#include <winrt/Windows.UI.Xaml.Input.h> // PointerRoutedEventArgs
#include <winrt/Windows.UI.Xaml.Media.h> // SolidColorBrush / FontFamily
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h> // FlyoutBase (Button.Flyout)
#include <winrt/Windows.ApplicationModel.DataTransfer.h> // Clipboard / DataPackage (row 3 copy)

#include <shellapi.h> // ShellExecuteExW (row 3 folder button)
#include <mmsystem.h> // PlaySoundW (row 3 copy/open confirmation chime)
#pragma comment(lib, "winmm.lib")

#include <algorithm> // std::clamp / std::min (summary-panel size fractions)
#include <cmath> // NAN (summary-panel "auto" size sentinel on drag release)
#include <chrono> // DispatcherTimer interval (summary times-line ticker)
#include <string>
#include <unordered_set> // summary file-list de-dup (Edited/Created take over Read)
#include <vector>

using namespace winrt::Windows::Foundation;
// Narrow using-DECLARATIONS for the color helpers: a `using namespace winrt::Windows::UI;` would
// also pull the nested `Text` namespace into scope and clash with the Text() helpers (see the
// AgentManagerContent gotcha / CLAUDE.md).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Core::CoreCursorType; // summary-panel resize-grip cursors
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Documents; // Run / Inlines
using namespace winrt::Windows::UI::Xaml::Input; // PointerRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue
using namespace Agentmaster;
#include "AgentTabOverlay.Internal.h" // the shared file-local helpers (StateColor/renderers/CLI/...)

namespace winrt::TerminalApp::implementation
{
    void AgentTabOverlay::_BuildSummaryPanel()
    {
        if (_summaryRoot)
        {
            return; // built once (Initialize), and only for a LINKED session (never an observe badge)
        }
        // The times line (age / last user msg / last activity) is a SEPARATE, pinned-at-top TextBlock —
        // NOT part of the mtime-gated content below — so a DispatcherTimer can re-render its "ago"
        // deltas live (every few seconds) without re-reading the transcript. Collapsed until it has text.
        _summaryTimesText = TextBlock{};
        _summaryTimesText.FontFamily(FontFamily{ L"Cascadia Mono" });
        _summaryTimesText.FontSize(11);
        _summaryTimesText.TextWrapping(TextWrapping::Wrap);
        _summaryTimesText.IsTextSelectionEnabled(true);
        _summaryTimesText.Foreground(Fill(0xFF, 0xB0, 0xB0, 0xB0)); // dimmer than the body
        _summaryTimesText.Margin(ThicknessHelper::FromLengths(0, 0, 0, 3)); // a small gap above the content
        _summaryTimesText.Visibility(Visibility::Collapsed);
        AgentSetTip(_summaryTimesText, winrt::hstring{
            L"age = time since the conversation started \x00B7 last user msg = since your last prompt"
            L" \x00B7 last activity = since the transcript last changed (updates live)." });

        // The body is a vertical StackPanel (not one TextBlock) so a section separator can be a
        // full-width Border rule that fills the panel border-to-border + re-fills on resize — a fixed
        // run of ─ chars can't do that in a wrapping block. _SetSummaryContent fills it: monospace,
        // wrapped, selectable TextBlocks for text runs — except the "* item" lists (Files Created /
        // Edited / Read, Skills Loaded), which are NoWrap + clipped at the panel edge — interleaved
        // with the Border rules.
        _summaryStack = StackPanel{};
        _summaryStack.Orientation(Orientation::Vertical);

        _summaryScroll = ScrollViewer{};
        _summaryScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _summaryScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        // No fixed MaxHeight here — the panel's height is RESIZABLE: _ApplySummarySize sets this
        // viewport's MaxHeight from the (global, persisted) height fraction, defaulting to
        // min(480, 0.75*pane). A long session scrolls past it.
        _summaryScroll.Content(_summaryStack);

        // Header row pinned at the panel top: the live "ago" times line on the LEFT, and a small
        // wrap-line TOGGLE on the RIGHT (same glyph size as the times font). The toggle flips whether the
        // panel preserves a message's real newlines (multi-line) or collapses them to a literal "\n" — a
        // GLOBAL, persisted setting (AppSettings::summaryPanelWrapNewlines), so the overlay can't write it
        // directly: the page handles the flip + broadcast (_onToggleSummaryWrap). A 2-column Grid: times =
        // star (wraps in the remaining width), toggle = auto (hugs the right edge, top-aligned).
        _summaryWrapIcon = FontIcon{};
        _summaryWrapIcon.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // a TEXT font carrying U+21B5 (the icon font would tofu a non-PUA glyph)
        _summaryWrapIcon.Glyph(L"\x21B5"); // ↵ — the newline / line-wrap symbol
        _summaryWrapIcon.FontSize(11); // "same size as the font" (the times line is 11)
        _summaryWrapIcon.FontWeight(FontWeights::SemiBold()); // a touch bolder so the small glyph reads better
        _summaryWrapIcon.IsHitTestVisible(false); // CLICK-THROUGH: a click on the glyph must land on wrapBtn, not be eaten by the icon (see the action-row mkIconBtn note)
        // Enlarge the glyph ~1-2px WITHOUT growing the times-bar row: a centered RenderTransform scale,
        // NOT a bigger FontSize. RenderTransform is applied AFTER layout, so the icon's measured box (and
        // thus the row's line height) is unchanged — the ↵ just renders a hair larger about its center.
        {
            ScaleTransform wrapScale{};
            wrapScale.ScaleX(1.18); // ~11px -> ~13px visual (within the requested 1-2px), height unaffected
            wrapScale.ScaleY(1.18);
            _summaryWrapIcon.RenderTransform(wrapScale);
            _summaryWrapIcon.RenderTransformOrigin(Point{ 0.5f, 0.5f }); // scale about the glyph's center
        }
        // The icon color (dim when off / lighter when on) is set by _UpdateSummaryWrapButtonVisual.

        Button wrapBtn{};
        wrapBtn.Background(Fill(0x00, 0, 0, 0)); // transparent (alpha 0) — still hit-testable; the template still gives a hover highlight
        wrapBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        wrapBtn.Padding(ThicknessHelper::FromLengths(3, 0, 1, 0));
        wrapBtn.MinWidth(0);
        wrapBtn.MinHeight(0);
        wrapBtn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        wrapBtn.VerticalAlignment(VerticalAlignment::Top);
        wrapBtn.HorizontalAlignment(HorizontalAlignment::Right);
        wrapBtn.Content(_summaryWrapIcon);
        AgentSetTip(wrapBtn, winrt::hstring{
            L"Wrap message newlines: keep a multi-line prompt as multiple lines instead of a literal \\n (all tabs)" });
        wrapBtn.Click([weak = get_weak()](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummaryWrap();
            }
        });

        // truncate TOGGLE, to the LEFT of the wrap toggle (same glyph size + scale). ON (default) caps
        // each message — 6 lines when wrapped (7th+ -> "..."), else 500 chars; OFF shows every message
        // in full. Also a GLOBAL, persisted setting (AppSettings::summaryPanelTruncate) flipped via the
        // page (_onToggleSummaryTruncate) so it broadcasts to every linked overlay.
        _summaryTruncateIcon = FontIcon{};
        _summaryTruncateIcon.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // carries U+2026 (the horizontal ellipsis)
        _summaryTruncateIcon.Glyph(L"\x2026"); // … — the truncate / elision symbol
        _summaryTruncateIcon.FontSize(11); // match the times font + the wrap toggle
        _summaryTruncateIcon.FontWeight(FontWeights::SemiBold());
        _summaryTruncateIcon.IsHitTestVisible(false); // CLICK-THROUGH: glyph clicks land on truncBtn (see the action-row mkIconBtn note)
        {
            ScaleTransform truncScale{};
            truncScale.ScaleX(1.18); // same ~1-2px visual bump as the wrap toggle, row height unaffected
            truncScale.ScaleY(1.18);
            _summaryTruncateIcon.RenderTransform(truncScale);
            _summaryTruncateIcon.RenderTransformOrigin(Point{ 0.5f, 0.5f });
        }
        // The icon color (dim when off / lighter when on) is set by _UpdateSummaryTruncateButtonVisual.

        Button truncBtn{};
        truncBtn.Background(Fill(0x00, 0, 0, 0)); // transparent — still hit-testable + hover highlight
        truncBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        truncBtn.Padding(ThicknessHelper::FromLengths(3, 0, 1, 0));
        truncBtn.MinWidth(0);
        truncBtn.MinHeight(0);
        truncBtn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        truncBtn.VerticalAlignment(VerticalAlignment::Top);
        truncBtn.HorizontalAlignment(HorizontalAlignment::Right);
        truncBtn.Content(_summaryTruncateIcon);
        AgentSetTip(truncBtn, winrt::hstring{
            L"Truncate long messages: cap each to 6 lines (when wrapping) or 500 characters; off shows everything" });
        truncBtn.Click([weak = get_weak()](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummaryTruncate();
            }
        });

        // Agentmaster (conversation lineage): the PREVIOUS-SESSION toggle, LEFTMOST in the strip (left of
        // truncate). Collapsed by default — shown only when the loaded summary has a pre-/compact segment.
        // ON renders the previous session(s) above the current Messages. A GLOBAL setting
        // (AppSettings::summaryPanelShowPrevious) flipped via the page (_onToggleSummaryPrevious) so it
        // broadcasts to every linked overlay. Same glyph size + scale as the wrap/truncate toggles.
        _summaryPrevIcon = FontIcon{};
        _summaryPrevIcon.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // carries U+23EE (the previous-track double-bar glyph)
        _summaryPrevIcon.Glyph(L"\x23EE"); // ⏮ — "previous session(s)" (before /compact)
        _summaryPrevIcon.FontSize(11);
        _summaryPrevIcon.FontWeight(FontWeights::SemiBold());
        _summaryPrevIcon.IsHitTestVisible(false); // CLICK-THROUGH: glyph clicks land on _summaryPrevBtn (see the action-row mkIconBtn note)
        {
            ScaleTransform prevScale{};
            prevScale.ScaleX(1.18);
            prevScale.ScaleY(1.18);
            _summaryPrevIcon.RenderTransform(prevScale);
            _summaryPrevIcon.RenderTransformOrigin(Point{ 0.5f, 0.5f });
        }
        _summaryPrevBtn = Button{};
        _summaryPrevBtn.Background(Fill(0x00, 0, 0, 0)); // transparent — still hit-testable + hover highlight
        _summaryPrevBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        _summaryPrevBtn.Padding(ThicknessHelper::FromLengths(3, 0, 1, 0));
        _summaryPrevBtn.MinWidth(0);
        _summaryPrevBtn.MinHeight(0);
        _summaryPrevBtn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        _summaryPrevBtn.VerticalAlignment(VerticalAlignment::Top);
        _summaryPrevBtn.HorizontalAlignment(HorizontalAlignment::Right);
        _summaryPrevBtn.Content(_summaryPrevIcon);
        _summaryPrevBtn.Visibility(Visibility::Collapsed); // shown only when the loaded summary has a previous segment
        AgentSetTip(_summaryPrevBtn, winrt::hstring{
            L"Show previous session(s): the pre-/compact conversation, numbered separately above the current messages" });
        _summaryPrevBtn.Click([weak = get_weak()](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummaryPrevious();
            }
        });

        // Agentmaster: a REFRESH button (rightmost in the strip) — force a re-read of the transcript +
        // rebuild of this panel NOW, bypassing the mtime gate (a transcript can change without its mtime
        // advancing, or you simply want a fresh pull). Same glyph size + scale + transparent-button styling
        // as the toggles, but steady-colored (not a toggle state).
        FontIcon refreshIcon{};
        refreshIcon.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // carries U+21BB (the clockwise reload arrow)
        refreshIcon.Glyph(L"\x21BB"); // ↻ — refresh / reload
        refreshIcon.FontSize(11);
        refreshIcon.FontWeight(FontWeights::SemiBold());
        refreshIcon.Foreground(Fill(0xFF, 0xB0, 0xB0, 0xB0)); // steady mid-gray (matches the dim toggle shade)
        refreshIcon.IsHitTestVisible(false); // CLICK-THROUGH: glyph clicks land on refreshBtn (see the action-row mkIconBtn note)
        {
            ScaleTransform refreshScale{};
            refreshScale.ScaleX(1.18);
            refreshScale.ScaleY(1.18);
            refreshIcon.RenderTransform(refreshScale);
            refreshIcon.RenderTransformOrigin(Point{ 0.5f, 0.5f });
        }
        Button refreshBtn{};
        refreshBtn.Background(Fill(0x00, 0, 0, 0)); // transparent — still hit-testable + hover highlight
        refreshBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        refreshBtn.Padding(ThicknessHelper::FromLengths(3, 0, 1, 0));
        refreshBtn.MinWidth(0);
        refreshBtn.MinHeight(0);
        refreshBtn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        refreshBtn.VerticalAlignment(VerticalAlignment::Top);
        refreshBtn.HorizontalAlignment(HorizontalAlignment::Right);
        refreshBtn.Content(refreshIcon);
        AgentSetTip(refreshBtn, winrt::hstring{
            L"Refresh the summary \x2014 re-read the transcript and rebuild this panel now" });
        refreshBtn.Click([weak = get_weak()](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_RefreshSummary();
            }
        });

        Grid timesRow{};
        {
            ColumnDefinition cStar{};
            cStar.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); // times line takes the remaining width
            ColumnDefinition cAuto{};
            cAuto.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto)); // toggles hug the right edge
            timesRow.ColumnDefinitions().Append(cStar);
            timesRow.ColumnDefinitions().Append(cAuto);
        }
        // Both toggles live in col1, in a horizontal strip: truncate on the LEFT, wrap on the RIGHT.
        StackPanel toggles{};
        toggles.Orientation(Orientation::Horizontal);
        toggles.VerticalAlignment(VerticalAlignment::Top);
        toggles.HorizontalAlignment(HorizontalAlignment::Right);
        toggles.Children().Append(_summaryPrevBtn); // leftmost: previous-session (shown only when compacted)
        toggles.Children().Append(truncBtn);
        toggles.Children().Append(wrapBtn);
        toggles.Children().Append(refreshBtn); // rightmost: re-read the transcript + rebuild now
        Grid::SetColumn(_summaryTimesText, 0);
        Grid::SetColumn(toggles, 1);
        timesRow.Children().Append(_summaryTimesText);
        timesRow.Children().Append(toggles);

        // A pinned TITLE row at the very TOP of the panel — above the times line AND the numbered messages
        // — showing the session's title (SessionInfo.title; the ONE value shared by the tab header + the
        // Explorer name, Rule #11). Set live by _UpdateSummary, so a rename updates it in place. Brighter +
        // a touch bolder than the body so it reads as the heading; a long title wraps within the panel's
        // width cap (never balloons it). Collapsed until it has text.
        _summaryTitleText = TextBlock{};
        _summaryTitleText.FontFamily(FontFamily{ L"Cascadia Mono" });
        _summaryTitleText.FontSize(12);
        _summaryTitleText.FontWeight(FontWeights::SemiBold());
        _summaryTitleText.TextWrapping(TextWrapping::Wrap);
        _summaryTitleText.IsTextSelectionEnabled(true);
        _summaryTitleText.Foreground(Fill(0xFF, 0xF0, 0xF0, 0xF0)); // brighter than the body — it's the heading
        _summaryTitleText.Margin(ThicknessHelper::FromLengths(0, 0, 0, 2)); // a small gap above the times line
        _summaryTitleText.Visibility(Visibility::Collapsed);
        AgentSetTip(_summaryTitleText, winrt::hstring{
            L"This session's title \x2014 the same value as the tab name." });

        StackPanel outer{}; // title + header (times + truncate/wrap toggles) pinned over the scrolling content
        outer.Orientation(Orientation::Vertical);
        outer.Children().Append(_summaryTitleText);
        outer.Children().Append(timesRow);
        outer.Children().Append(_summaryScroll);

        _UpdateSummaryWrapButtonVisual(); // seed the wrap toggle's color from _summaryWrapNewlines (default: dim/off)
        _UpdateSummaryTruncateButtonVisual(); // seed the truncate toggle's color from _summaryTruncate (default: lighter/on)
        _UpdateSummaryPrevButtonVisual(); // seed the previous-session toggle's color from _summaryShowPrevious (default: dim/off)

        // The padded content sits in its own inner border so the resize grips (siblings below) can hug
        // the TRUE panel edges (outside the content's 8/6px inset) while the text keeps its padding.
        Border contentBorder{};
        contentBorder.Padding(ThicknessHelper::FromLengths(8, 6, 8, 6));
        contentBorder.Child(outer);

        // Resize grips (TAB_OVERLAY.md): the panel is anchored top-right, so the LEFT edge grows width,
        // the BOTTOM edge grows height, and the BOTTOM-LEFT corner does both. Each is a thin,
        // ~invisible-but-hit-testable strip that brightens + shows a resize cursor on hover. They live
        // INSIDE _summaryRoot (a Grid child), so they collapse with the panel — no extra visibility wiring.
        auto weak = get_weak();
        const auto idleGrip = Fill(0x01, 0xFF, 0xFF, 0xFF); // ~invisible, yet hit-testable
        const auto hotGrip = Fill(0x55, 0xC0, 0xC0, 0xC0); // subtle highlight on hover / drag
        const auto wireGrip = [weak, idleGrip, hotGrip](const Border& grip, bool left, bool bottom, CoreCursorType cursor) {
            grip.Background(idleGrip);
            grip.PointerEntered([grip, hotGrip, cursor](const IInspectable&, const PointerRoutedEventArgs&) {
                ApplyCursor(cursor);
                grip.Background(hotGrip);
            });
            grip.PointerExited([weak, grip, idleGrip](const IInspectable&, const PointerRoutedEventArgs&) {
                if (const auto self = weak.get(); self && self->_summaryDragging)
                {
                    return; // mid-drag the pointer may leave the thin grip — keep it hot
                }
                ApplyCursor(CoreCursorType::Arrow);
                grip.Background(idleGrip);
            });
            grip.PointerPressed([weak, left, bottom, cursor](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                const auto self = weak.get();
                if (!self)
                {
                    return;
                }
                self->_summaryDragging = true;
                self->_summaryDragLeft = left;
                self->_summaryDragBottom = bottom;
                self->_summaryDragShift = IsShiftDown(); // SHIFT at gesture start => local-only resize (decided at release)
                const auto p = e.GetCurrentPoint(nullptr).Position(); // island-relative; only the delta matters
                self->_summaryDragStartX = p.X;
                self->_summaryDragStartY = p.Y;
                // Start from the ACTUAL rendered size, NOT the fraction cap. At rest the panel hugs THIS
                // tab's content (Width/Height == auto), which on a short transcript is much smaller than the
                // shared cap — and the grip sits at that content edge. Seeding the start from the cap would
                // desync the grip from the pointer (the panel jumps to the cap on the first move). Seeding
                // from ActualWidth/Height makes the grip track the pointer 1:1, so the panel grows smoothly
                // PAST this tab's content up to the max band (the forced exact size in _ApplySummarySize).
                // Fall back to the cap only before first layout (ActualWidth/Height == 0). BOTH dims read
                // from _summaryRoot (the OUTER box) — that is the element _ApplySummarySize now forces, so
                // the grip tracks the box edge 1:1 (height used to read the inner scroll viewport, which is
                // ~one header shorter than the box and would desync the bottom grip).
                const double aw = self->_summaryRoot ? self->_summaryRoot.ActualWidth() : 0.0;
                const double ah = self->_summaryRoot ? self->_summaryRoot.ActualHeight() : 0.0;
                self->_summaryDragStartW = aw > 0.0 ? aw : self->_CurrentSummaryWidthPx();
                self->_summaryDragStartH = ah > 0.0 ? ah : self->_CurrentSummaryHeightPx();
                if (const auto el = sender.try_as<UIElement>())
                {
                    el.CapturePointer(e.Pointer());
                }
                ApplyCursor(cursor);
                e.Handled(true);
            });
            grip.PointerMoved([weak](const IInspectable&, const PointerRoutedEventArgs& e) {
                const auto self = weak.get();
                if (!self || !self->_summaryDragging)
                {
                    return;
                }
                const auto p = e.GetCurrentPoint(nullptr).Position();
                self->_OnSummaryDragMove(p.X, p.Y);
                e.Handled(true);
            });
            const auto endHandler = [weak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                if (const auto self = weak.get())
                {
                    self->_OnSummaryDragEnd(sender);
                    e.Handled(true);
                }
            };
            grip.PointerReleased(endHandler);
            grip.PointerCaptureLost(endHandler);
        };

        // The grips are thin (6px edges / 14px corner). The left + corner tips pin PLACEMENT to Top
        // (AgentSetTipPlacement) so the tip is offset ABOVE the bar rather than popping over it (the
        // shared tip's default is near-pointer). The BOTTOM grip gets NO tooltip at all: it sits at the
        // panel's bottom edge and you resize by dragging DOWN, so any tip — even Top-placed — still lands
        // over the thin bar and blocks the grab (Top wasn't enough there). The resize cursor already
        // signals the action; the left/corner tips carry the shared-vs-Shift explanation.
        Border leftGrip{};
        leftGrip.Width(6);
        leftGrip.HorizontalAlignment(HorizontalAlignment::Left);
        leftGrip.VerticalAlignment(VerticalAlignment::Stretch);
        wireGrip(leftGrip, true, false, CoreCursorType::SizeWestEast);
        AgentSetTip(leftGrip, winrt::hstring{
            L"Drag to resize the panel width (shared across tabs).\n"
            L"Hold Shift to size this tab only." });
        AgentSetTipPlacement(leftGrip, Primitives::PlacementMode::Top);

        Border bottomGrip{};
        bottomGrip.Height(6);
        bottomGrip.HorizontalAlignment(HorizontalAlignment::Stretch);
        bottomGrip.VerticalAlignment(VerticalAlignment::Bottom);
        wireGrip(bottomGrip, false, true, CoreCursorType::SizeNorthSouth);
        // (no AgentSetTip on bottomGrip — see the comment above: the tip covered the drag-down bar.)

        Border cornerGrip{};
        cornerGrip.Width(14);
        cornerGrip.Height(14);
        cornerGrip.HorizontalAlignment(HorizontalAlignment::Left);
        cornerGrip.VerticalAlignment(VerticalAlignment::Bottom);
        wireGrip(cornerGrip, true, true, CoreCursorType::SizeNortheastSouthwest); // bottom-left corner == NE/SW diagonal
        AgentSetTip(cornerGrip, winrt::hstring{
            L"Drag to resize the panel width and height at once (shared across tabs).\n"
            L"Hold Shift to size this tab only." });
        AgentSetTipPlacement(cornerGrip, Primitives::PlacementMode::Top);

        Grid layout{};
        layout.Children().Append(contentBorder);
        layout.Children().Append(leftGrip);
        layout.Children().Append(bottomGrip);
        layout.Children().Append(cornerGrip); // last == on top, so the corner wins over the edge grips

        _summaryRoot = Border{};
        // FULLY OPAQUE, matching the badge (_root) — its see-through-ness is owned SOLELY by
        // _summaryRoot.Opacity (the same GLOBAL "Overlay opacity" setting). A translucent fill here
        // (was 0xE6) would multiply with that Opacity, so "100%" could never be truly opaque.
        _summaryRoot.Background(Fill(0xFF, 0x20, 0x20, 0x20));
        _summaryRoot.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _summaryRoot.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _summaryRoot.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        // Padding now lives on contentBorder (so the grips reach the panel edges).
        _summaryRoot.Child(layout);
        _summaryRoot.Visibility(Visibility::Collapsed); // shown only while the GLOBAL showSummaryPanel is ON
        _summaryRoot.Opacity(_restOpacity); // dim at rest — same value as the badge (_root.Opacity)
        // Same hover MECHANISM as the badge (_WireHover/_SetExpanded): brighten to full on pointer-over,
        // back to dim on exit. The panel is text-heavy (selectable title / times / body runs) + has resize
        // grips, all hit-testable children whose bubbled PointerExited would otherwise dim the panel WHILE
        // the pointer is still on it (the root never re-enters — it can stick dim); PointerWithin swallows
        // those child-bubbled exits, so it stays bright while hovering anywhere on the panel (incl. resizing).
        _summaryRoot.PointerEntered([weak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (const auto self = weak.get())
            {
                self->_summaryRoot.Opacity(self->_hoverOpacity);
            }
        });
        _summaryRoot.PointerExited([weak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            if (PointerWithin(sender, e))
            {
                return; // a child's bubbled exit while the pointer is still on the panel — not a real leave
            }
            if (const auto self = weak.get())
            {
                self->_summaryRoot.Opacity(self->_restOpacity);
            }
        });

        // Right-click anywhere on the summary panel => a "Copy Summary" context menu, the SAME action as
        // the badge copy menu's "Summary" item (_CopyField(6) -> the FULL session-end.js box, rendered
        // with this overlay's mirrored wrap/truncate flags so it matches the displayed panel). Built once
        // as a shared MenuFlyout and wired (_WireSummaryContextMenu) onto the panel root (covers the title /
        // times bar / padding / separators / scroll gaps) AND every selectable text block the panel
        // renders (the title, the times line, and each body run in _SetSummaryContent) — the panel is
        // text-heavy, and a selectable TextBlock shadows the parent's context menu, so without this a
        // right-click landing on text would offer nothing. Text selection + Ctrl+C still work (only the
        // right-click menu is overridden, not SelectionFlyout).
        //
        // ONE menu serves every element, so the items that are about WHERE the click landed gate + fill
        // themselves in Opening from state stamped by the wiring: the live text selection ("Copy Selected
        // Text") and the right-clicked numbered message ("Copy Prompt", _ctxPromptIndex).
        _summaryContextMenu = MenuFlyout{};
        {
            // Copy Selected Text — shown ONLY when text is selected in the panel. This context menu
            // OVERRIDES the built-in selection "Copy" on the panel's selectable text blocks, so without
            // this item a right-click on a selection would offer no copy at all (only Ctrl+C worked). The
            // live selection isn't known when the menu is built, so the item + its separator are gated in
            // Opening (below), which also CAPTURES the text so the click copies exactly what was shown.
            auto pendingSel = std::make_shared<std::wstring>();
            MenuFlyoutItem copySelItem{};
            copySelItem.Text(L"Copy Selected Text");
            copySelItem.Visibility(Visibility::Collapsed);
            AgentSetTip(copySelItem, winrt::hstring{ L"Copy the text you selected in this panel." });
            copySelItem.Click([pendingSel](const IInspectable&, const RoutedEventArgs&) {
                if (!pendingSel->empty())
                {
                    CopyTextToClipboard(*pendingSel);
                }
            });
            MenuFlyoutSeparator copySelSep{};
            copySelSep.Visibility(Visibility::Collapsed);
            _summaryContextMenu.Items().Append(copySelItem);
            _summaryContextMenu.Items().Append(copySelSep);

            // Copy Prompt — right-click a NUMBERED message => copy THAT prompt, whole and EXACT: the raw
            // text as it was typed (_summaryUserMsgs, straight off the transcript), never the panel's
            // RENDERING of it, which escapes newlines to a literal "\n" while wrap is off and caps the
            // message while truncate is on. Sits directly ABOVE "Copy Summary": both are copies, this the
            // narrower/more specific one, so it reads first. Shown ONLY when the right-click landed on a
            // numbered message row (_ctxPromptIndex, set by _WireSummaryContextMenu before the menu opens);
            // like the selection above, the text is CAPTURED in Opening so the click copies exactly the
            // prompt the menu was opened for even if the panel reloads underneath it.
            auto pendingPrompt = std::make_shared<std::wstring>();
            auto pendingPromptNo = std::make_shared<int>(0); // its 1-based number, captured with the text (log only)
            MenuFlyoutItem copyPromptItem{};
            copyPromptItem.Text(L"Copy Prompt");
            copyPromptItem.Visibility(Visibility::Collapsed);
            AgentSetTip(copyPromptItem, winrt::hstring{ L"Copy this prompt verbatim \x2014 the FULL exact text you typed." });
            copyPromptItem.Click([weak, pendingPrompt, pendingPromptNo](const IInspectable&, const RoutedEventArgs&) {
                if (pendingPrompt->empty())
                {
                    return;
                }
                // CONTAINED for the same reason as the ShowAt above: this runs as a XAML event handler, so
                // an escape (a bad_alloc building the log line off a huge prompt is the realistic one) is a
                // process fail-fast, not a failed copy. The clipboard write comes FIRST so a throw in the
                // audit line can never cost the user the copy they asked for.
                try
                {
                    CopyTextToClipboard(*pendingPrompt);
                    if (const auto self = weak.get())
                    {
                        // Nav audit: the same "what did the user copy" trail CopySessionField writes for the
                        // copy menus (this item is panel-local, so it doesn't route through that chokepoint).
                        ::Agentmaster::LogNav(L"copy summary-prompt " + ::Agentmaster::ShortId(self->_sessionId) +
                                              L" #" + std::to_wstring(*pendingPromptNo) +
                                              L" chars=" + std::to_wstring(pendingPrompt->size()));
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay summary Copy Prompt");
                }
            });
            _summaryContextMenu.Items().Append(copyPromptItem);

            MenuFlyoutItem copyItem{};
            copyItem.Text(L"Copy Summary");
            AgentSetTip(copyItem, winrt::hstring{
                L"Copy the FULL session summary \x2014 the complete box (id, resume CLI, dir, folder, branch, duration, tasks, messages, files), including everything the displayed panel trims" });
            copyItem.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_CopyField(6); // == the copy menu's "Summary" (the full textual box, CopySummaryAsync)
                }
            });
            _summaryContextMenu.Items().Append(copyItem);

            // View toggles — the SAME flips as the times-bar … / ↵ buttons, offered here for quick
            // right-click access. The Enable/Disable label reflects the live mirror flags (set in Opening,
            // seeded at build); _ToggleSummaryTruncate/Wrap invoke the page handler (global flip + broadcast).
            _summaryContextMenu.Items().Append(MenuFlyoutSeparator{});
            MenuFlyoutItem truncItem{};
            truncItem.Text(_summaryTruncate ? L"Disable Truncate Long Messages" : L"Enable Truncate Long Messages");
            AgentSetTip(truncItem, winrt::hstring{ L"Truncate long messages: cap each (6 lines when wrapping, else 500 chars), or show every message in full." });
            truncItem.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_ToggleSummaryTruncate();
                }
            });
            _summaryContextMenu.Items().Append(truncItem);
            MenuFlyoutItem wrapItem{};
            wrapItem.Text(_summaryWrapNewlines ? L"Disable Wrap Messages" : L"Enable Wrap Messages");
            AgentSetTip(wrapItem, winrt::hstring{ L"Wrap messages: keep each message's real line breaks (multi-line) instead of collapsing them to a literal \\n." });
            wrapItem.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_ToggleSummaryWrap();
                }
            });
            _summaryContextMenu.Items().Append(wrapItem);

            // Refresh — the SAME action as the times-bar ↻ button, offered here too (its neighbor toggles
            // are in this menu, so it belongs alongside them). Re-reads the transcript + rebuilds the panel.
            MenuFlyoutItem refreshItem{};
            refreshItem.Text(L"Refresh Summary");
            AgentSetTip(refreshItem, winrt::hstring{ L"Re-read the transcript and rebuild this panel now." });
            refreshItem.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_RefreshSummary();
                }
            });
            _summaryContextMenu.Items().Append(refreshItem);

            // Refresh the dynamic bits each time the menu opens: gate + CAPTURE the live selection, and set
            // the toggle Enable/Disable labels from the live mirror flags. (FlyoutBase::Opening fires before
            // the menu lays out; it's distinct from Opened below, which handles the dim/bright opacity.)
            _summaryContextMenu.Opening([weak, copySelItem, copySelSep, copyPromptItem, truncItem, wrapItem, pendingSel, pendingPrompt, pendingPromptNo](const IInspectable&, const IInspectable&) {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                // CONTAINED (this is a XAML event handler — an escape is a 0xC000027B fail-fast, and this
                // body copies a whole prompt + walks the panel collecting the selection, so bad_alloc is
                // the realistic escape on a huge conversation). Each capture is CLEARED before it is
                // refilled and every item is filled BEFORE it is shown, so a contained throw can only
                // leave an item under-offered (hidden, or shown-but-inert => a dead click) — never
                // offering one prompt while holding another.
                try
                {
                    // "Copy Prompt": resolve the right-clicked row's prompt (_ctxPromptIndex, stamped by the
                    // element handler that showed this menu) and CAPTURE its exact text. Re-validated against
                    // the live prompt list, so a stale index from an earlier render can never copy anything.
                    pendingPrompt->clear();
                    const int promptIdx = self->_ctxPromptIndex;
                    if (promptIdx >= 0 && promptIdx < static_cast<int>(self->_summaryUserMsgs.size()))
                    {
                        *pendingPrompt = self->_summaryUserMsgs[promptIdx];
                        *pendingPromptNo = promptIdx + 1;
                    }
                    const bool hasPrompt = !pendingPrompt->empty();
                    copyPromptItem.Visibility(hasPrompt ? Visibility::Visible : Visibility::Collapsed);
                    if (hasPrompt)
                    {
                        // Name the message the click landed on, so the item is unambiguous on a long list.
                        AgentSetTip(copyPromptItem, winrt::hstring{ L"Copy prompt " + std::to_wstring(promptIdx + 1) +
                                                                    L" verbatim \x2014 the FULL exact text you typed (not this panel's shortened/escaped rendering)." });
                    }
                    pendingSel->clear();
                    *pendingSel = self->_SummarySelectedText();
                    const bool hasSel = !pendingSel->empty();
                    copySelItem.Visibility(hasSel ? Visibility::Visible : Visibility::Collapsed);
                    copySelSep.Visibility(hasSel ? Visibility::Visible : Visibility::Collapsed);
                    truncItem.Text(self->_summaryTruncate ? winrt::hstring{ L"Disable Truncate Long Messages" } : winrt::hstring{ L"Enable Truncate Long Messages" });
                    wrapItem.Text(self->_summaryWrapNewlines ? winrt::hstring{ L"Disable Wrap Messages" } : winrt::hstring{ L"Enable Wrap Messages" });
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay summary context menu Opening");
                }
            });
            // Keep the panel bright while the menu is up: the right-tap moves the pointer onto the popup,
            // which fires the panel's PointerExited and would otherwise dim it (mirrors the badge copy
            // menu's pinned-while-open behaviour). Restore the dim rest state on close.
            _summaryContextMenu.Opened([weak](const IInspectable&, const IInspectable&) {
                if (const auto self = weak.get(); self && self->_summaryRoot)
                {
                    self->_summaryRoot.Opacity(self->_hoverOpacity);
                }
            });
            _summaryContextMenu.Closed([weak](const IInspectable&, const IInspectable&) {
                if (const auto self = weak.get(); self && self->_summaryRoot)
                {
                    self->_summaryRoot.Opacity(self->_restOpacity);
                }
            });
        }
        _WireSummaryContextMenu(_summaryRoot, -1);
        _WireSummaryContextMenu(_summaryTitleText, -1); // the title is selectable too
        _WireSummaryContextMenu(_summaryTimesText, -1); // the times line is selectable too

        _ApplySummarySize(); // seed MaxWidth/scroll-MaxHeight from the (default/seeded) fractions

        // The live "ago" ticker for the times line. Tick fires on the UI thread; it self-stops once the
        // overlay is gone (weak), so a closed tab never leaks a ticking timer (and we never Stop() it off
        // the UI thread from the dtor). Started/stopped by SetSummaryEnabled.
        _summaryTimer = DispatcherTimer{};
        _summaryTimer.Interval(std::chrono::seconds(5));
        // (reuses the `weak` captured above for the grip handlers)
        _summaryTimer.Tick([weak](const IInspectable& sender, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_UpdateTimesLine();
                self->_RefreshJumpEligibility(); // the gated interval (5 s, visible-only): re-dim stale icons
                // Backstop: re-read the transcript CONTENT too, so the message list self-refreshes on the
                // same cadence as the times line. The content otherwise reloads ONLY on a registry notify
                // (_Refresh), but a turn's closing message (and any growth on a no-hook / quiet session) lands
                // AFTER the final notify — then the session idles and no further _Refresh comes, leaving the
                // list stale until a manual refresh. _UpdateSummary is mtime-gated inside _LoadSummaryAsync,
                // so an unchanged transcript is a cheap stat with NO re-render (no flicker / scroll reset).
                if (self->_summaryEnabled && self->_registry && !self->_sessionId.empty())
                {
                    if (const auto info = self->_registry->Get(self->_sessionId))
                    {
                        self->_UpdateSummary(*info);
                    }
                }
            }
            else if (const auto t = sender.try_as<DispatcherTimer>())
            {
                t.Stop(); // overlay destroyed — stop ticking (UI thread, safe)
            }
        });
    }

    // Attach the SHARED summary context menu to one element of the panel, remembering WHICH numbered
    // prompt a right-click there is about (`promptIndex`, 0-based; -1 for the chrome — root / title /
    // times line / a plain body run). That stamp is what lets the one shared menu offer a row-specific
    // "Copy Prompt" without a per-row MenuFlyout.
    //
    // The menu is shown EXPLICITLY here (and the event marked handled) instead of leaving it to the
    // ContextFlyout default, because ORDER is the whole correctness argument: routed events bubble
    // innermost-first, so stamping + showing at the element under the pointer is the one sequence that
    // GUARANTEES _ctxPromptIndex is already right when Opening reads it — a message row's stamp can
    // never be overwritten by an ancestor's, and the chrome's -1 can never arrive after the menu opened
    // over a stale row index (which would copy a DIFFERENT prompt than the one clicked). ContextFlyout
    // stays set as a belt for any child that has no handler of its own; a handled event never opens it,
    // so the two can't double-show. Position-from-pointer mirrors the default's placement; a
    // keyboard-invoked request (Shift+F10 / the menu key) has no position and anchors to the element.
    void AgentTabOverlay::_WireSummaryContextMenu(const FrameworkElement& el, int promptIndex)
    {
        if (!el || !_summaryContextMenu)
        {
            return;
        }
        el.ContextFlyout(_summaryContextMenu);
        const auto weak = get_weak();
        el.ContextRequested([weak, promptIndex](const UIElement& sender, const ContextRequestedEventArgs& e) {
            const auto self = weak.get();
            if (!self || !self->_summaryContextMenu)
            {
                return;
            }
            self->_ctxPromptIndex = promptIndex;
            const auto target = sender.try_as<FrameworkElement>();
            if (!target)
            {
                return; // no anchor to show at — let the default path have it (index is stamped either way)
            }
            // CONTAINED: ShowAt is the throwing kind of XAML call (the tag-badge popup's E_UNEXPECTED /
            // E_LAYOUTCYCLE class — see the CLAUDE.md gotcha), and an exception escaping a routed-event
            // handler comes back as a STOWED-exception fail-fast (0xC000027B), i.e. the whole process for
            // a context menu. Losing the menu on that gesture is the right degradation. Handled is set
            // only once the menu is actually up, so a throw falls back to the ContextFlyout default path
            // instead of eating the gesture entirely.
            try
            {
                Point pos{};
                if (e.TryGetPosition(target, pos))
                {
                    winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions opts{};
                    opts.Position(pos);
                    self->_summaryContextMenu.ShowAt(target, opts);
                }
                else
                {
                    self->_summaryContextMenu.ShowAt(target); // keyboard-invoked (Shift+F10 / the menu key): no pointer position
                }
                e.Handled(true);
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay summary context menu ShowAt");
            }
        });
    }

    // The panel's current effective MAX WIDTH in px: an explicit width fraction (clamped to band) times
    // the pane width, or the 20% default when unset. 0 until the host has pushed a pane size.
    double AgentTabOverlay::_CurrentSummaryWidthPx() const
    {
        const double wf = (_summaryWidthFraction > 0.0) ? std::clamp(_summaryWidthFraction, kSummaryMinWFrac, kSummaryMaxWFrac) : kSummaryDefWFrac;
        return (_summaryPaneW > 0.0) ? wf * _summaryPaneW : 0.0;
    }

    // The scroll viewport's current effective MAX HEIGHT in px: an explicit height fraction (clamped)
    // times the pane height, or the original auto cap min(480, 0.75*pane). Always sane (defaults to 480).
    double AgentTabOverlay::_CurrentSummaryHeightPx() const
    {
        if (_summaryHeightFraction > 0.0 && _summaryPaneH > 0.0)
        {
            return std::clamp(_summaryHeightFraction, kSummaryMinHFrac, kSummaryMaxHFrac) * _summaryPaneH;
        }
        return (_summaryPaneH > 0.0) ? std::min(kSummaryDefMaxH, kSummaryMaxHFrac * _summaryPaneH) : kSummaryDefMaxH;
    }

    // Re-apply the panel size from the (global) fractions + the cached pane size. WIDTH is a MaxWidth
    // cap on the panel border — the wrapping monospace body fills to it, and since the panel hugs the
    // top-right the growth is leftward. HEIGHT is the scroll viewport's MaxHeight — the body scrolls
    // past it. Called on a fraction change (SetSummarySize), a pane resize (OnSummaryPaneSize), and live
    // during a grip drag, so the panel stays a constant % of the pane as the window resizes.
    //
    // `forced` (mid grip-drag): ALSO pin an EXPLICIT Width/Height so the panel visibly grows PAST its own
    // content — the size is a GLOBAL/shared setting and other tabs may have more content to fill it, so a
    // drag must be able to reach the max bands even when THIS tab's transcript is short (TAB_OVERLAY.md
    // summary-panel resize). At rest (!forced) we clear that explicit size (NaN == auto), so the panel
    // falls back to the MaxWidth/MaxHeight caps and snaps to hug its own content again — the
    // "release the drag => reset to fit content" the user asked for.
    void AgentTabOverlay::_ApplySummarySize(bool forced)
    {
        if (!_summaryRoot)
        {
            return;
        }
        const double wpx = _CurrentSummaryWidthPx();
        if (wpx > 0.0)
        {
            _summaryRoot.MaxWidth(wpx);
        }
        const double hpx = _CurrentSummaryHeightPx();
        if (_summaryScroll)
        {
            _summaryScroll.MaxHeight(hpx); // the at-rest height cap (content-driven below this)
        }
        // NaN is the special value XAML uses for "Auto" sizing (cf. TabManagement::_UpdateTabView).
        //
        // Grow PAST this tab's content during a drag by pinning an explicit size on the OUTER panel border
        // ONLY — a single DEFINITE box the inner content (title / times / scroll) lays out *within*.
        // CRASH FIX: the previous approach forced the inner ScrollViewer's Height instead. That viewport
        // has an Auto vertical scrollbar + wrapping content, so a forced height made its scrollbar-reflow
        // feed back into the panel's measure and never converge — XAML raised a non-continuable "Layout
        // cycle detected" fail-fast (0xc000027b in Windows.UI.Xaml.dll) on resize. A forced outer box can't
        // cycle: size flows ONE way (box -> content), and nothing observes _summaryRoot's size. When the
        // box is forced taller/wider than this tab's content the surplus is just empty space (the drag
        // preview); on release (!forced) we clear it (NaN == auto) so the panel snaps back to hug content
        // (still capped by MaxWidth and the scroll's MaxHeight).
        if (forced)
        {
            if (wpx > 0.0)
            {
                _summaryRoot.Width(wpx); // force the exact width: grow leftward past content, up to the shared max
            }
            if (hpx > 0.0)
            {
                _summaryRoot.Height(hpx); // force the exact height on the OUTER box (never the scroll viewport)
            }
        }
        else
        {
            _summaryRoot.Width(NAN); // auto => fit content (still capped by MaxWidth)
            _summaryRoot.Height(NAN); // auto => fit content (the scroll's MaxHeight still bounds it)
            if (_summaryScroll)
            {
                _summaryScroll.Height(NAN); // clear any stale forced viewport height from the old (cycle-prone) approach
            }
        }
    }

    void AgentTabOverlay::OnSummaryPaneSize(double paneWidth, double paneHeight)
    {
        _summaryPaneW = paneWidth;
        _summaryPaneH = paneHeight;
        _ApplySummarySize();
    }

    void AgentTabOverlay::SetSummarySize(double widthFraction, double heightFraction)
    {
        // Page-driven mirror of the GLOBAL AppSettings size fractions — on attach (seed) and on a resize
        // anywhere (broadcast). Pure apply; never calls the resize handler (so a broadcast can't loop).
        // A SHIFT-resize detached THIS tab from the shared size (_summarySizeLocalOverride): keep its
        // own size and IGNORE the broadcast until the user's next no-Shift drag re-attaches it.
        if (_summarySizeLocalOverride)
        {
            return;
        }
        _summaryWidthFraction = widthFraction;
        _summaryHeightFraction = heightFraction;
        _ApplySummarySize();
    }

    void AgentTabOverlay::SetSummaryResizeHandler(std::function<void(double, double)> handler)
    {
        _onResizeSummary = std::move(handler);
    }

    // Live grip drag: translate the pointer delta (island-relative; only the delta matters) into the
    // dragged size fraction(s), clamped to band, and re-apply. The panel is anchored top-right, so the
    // LEFT edge widens as the pointer moves left and the BOTTOM edge grows as it moves down.
    void AgentTabOverlay::_OnSummaryDragMove(double pointerX, double pointerY)
    {
        if (!_summaryDragging)
        {
            return;
        }
        if (_summaryDragLeft && _summaryPaneW > 0.0)
        {
            const double newW = _summaryDragStartW - (pointerX - _summaryDragStartX);
            _summaryWidthFraction = std::clamp(newW / _summaryPaneW, kSummaryMinWFrac, kSummaryMaxWFrac);
        }
        if (_summaryDragBottom && _summaryPaneH > 0.0)
        {
            const double newH = _summaryDragStartH + (pointerY - _summaryDragStartY);
            _summaryHeightFraction = std::clamp(newH / _summaryPaneH, kSummaryMinHFrac, kSummaryMaxHFrac);
        }
        _ApplySummarySize(true); // FORCED while dragging: track the pointer past this tab's content, up to the max
    }

    // Grip release: drop pointer capture, restore the cursor, then commit the new size. The fractions are
    // already band-clamped by _OnSummaryDragMove and applied to THIS panel. How it commits depends on
    // whether SHIFT was held at the gesture's start:
    //  - SHIFT held  => LOCAL-only: mark this tab detached (_summarySizeLocalOverride) and do NOT persist
    //                   or broadcast — the size stays on this tab and survives tab switches (in-memory),
    //                   but is NOT saved to settings.json nor pushed to sibling tabs.
    //  - no SHIFT    => SHARED: re-attach this tab and call the resize handler, which does the freshest-
    //                   disk RMW + the live broadcast to every linked overlay in the window (so switching
    //                   tabs shows the same size, and it persists across restart / seeds new windows).
    void AgentTabOverlay::_OnSummaryDragEnd(const IInspectable& sender)
    {
        if (!_summaryDragging)
        {
            return; // a capture-lost echo of our own release, or a stray event
        }
        _summaryDragging = false; // clear BEFORE releasing capture so the re-entrant CaptureLost no-ops
        if (const auto el = sender.try_as<UIElement>())
        {
            el.ReleasePointerCaptures();
        }
        ApplyCursor(CoreCursorType::Arrow);
        // Drop the forced exact Width/Height pinned during the drag and snap back to fit-content (the
        // MaxWidth/MaxHeight caps). The dragged FRACTION is preserved either way — the cap now carries
        // it — so this tab hugs its own (possibly short) content while the shared max we just set still
        // lets fuller tabs fill out. This is the user's "release the drag => reset to fit content".
        _ApplySummarySize(false);
        if (_summaryDragShift)
        {
            _summarySizeLocalOverride = true; // this tab now keeps its own size + ignores shared broadcasts
            return; // ephemeral: no persist, no cross-tab share
        }
        _summarySizeLocalOverride = false; // a no-Shift drag re-attaches this tab to the shared size
        if (_onResizeSummary)
        {
            _onResizeSummary(_summaryWidthFraction, _summaryHeightFraction);
        }
    }

    // Re-render the times line ("age 2d4h12m, last user msg 34m, last activity 1m13s") from the cached
    // instants + NOW, so the "ago" deltas grow live between content reloads (the DispatcherTimer drives
    // this). Last-activity prefers the transcript's CURRENT mtime (a cheap stat) so it keeps ticking
    // even when no content reload happened. Hidden when nothing is known yet.
    void AgentTabOverlay::_UpdateTimesLine()
    {
        if (!_summaryTimesText)
        {
            return;
        }
        int64_t lastAct = _summaryLastActivityMs;
        if (!_summaryPath.empty())
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(_summaryPath.c_str(), GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER li{};
                li.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                const int64_t mtimeMs = static_cast<int64_t>((li.QuadPart - kFtEpoch1970) / 10000ULL);
                if (mtimeMs > lastAct)
                {
                    lastAct = mtimeMs;
                }
            }
        }
        // Agentmaster (subagent activity): fold in the registry's SUBAGENT-AWARE last-activity so the
        // times line keeps advancing while a Task/Agent subagent runs — the panel's own sources above
        // (_summaryLastActivityMs + the parent file's mtime) come from the PARENT <id>.jsonl, which stays
        // quiescent then, so both read stale. max() only ever makes "last activity" fresher, never older;
        // 0 (unknown) is a no-op. Set by _UpdateSummary from SessionInfo::convLastActivityUnixMs each pass.
        if (_summaryConvLastActivityMs > lastAct)
        {
            lastAct = _summaryConvLastActivityMs;
        }
        const std::wstring line = FormatTimesLine(_summaryCreatedMs, _summaryLastUserMs, lastAct);
        _summaryTimesText.Text(winrt::hstring{ line });
        _summaryTimesText.Visibility(line.empty() ? Visibility::Collapsed : Visibility::Visible);
        _ApplySummaryVisibility(); // the times line may have just appeared/cleared — re-evaluate the pane
    }

    // Show the summary pane ONLY when it's enabled AND has something to render — content rows OR a
    // non-empty times line. Otherwise it's just an empty box (a never-prompted / no-transcript session,
    // or a not-yet-loaded one), so collapse it. All show/hide of _summaryRoot funnels through here.
    void AgentTabOverlay::_ApplySummaryVisibility()
    {
        if (!_summaryRoot)
        {
            return;
        }
        const bool hasContent = _summaryStack && _summaryStack.Children().Size() > 0;
        const bool hasTimes = _summaryTimesText && !_summaryTimesText.Text().empty();
        _summaryRoot.Visibility((_summaryEnabled && (hasContent || hasTimes)) ? Visibility::Visible : Visibility::Collapsed);
    }

    // Render the rendered-text box into the StackPanel: contiguous text lines become one monospace,
    // wrapped, selectable TextBlock; each separator sentinel line (kSepMark) becomes a full-width
    // Border rule (HorizontalAlignment::Stretch => border-to-border, re-fills as the pane resizes).
    // Agentmaster (SUMMARY_JUMP.md): a rendered Messages line is " <n>. <msg>" (RenderSummaryBox emits
    // exactly one leading space + the 1-based number + ". "). Parse that number to the 0-based prompt
    // index, or -1 if the line isn't a numbered message (files use "* ", tasks use "Tasks:", etc., so the
    // " <digits>. " shape is unique to messages). Tolerates >=1 leading spaces; bails without ". ".
    static int ParseSummaryMsgIndex(const std::wstring& line)
    {
        size_t i = 0;
        while (i < line.size() && line[i] == L' ')
        {
            ++i;
        }
        const size_t ds = i;
        while (i < line.size() && line[i] >= L'0' && line[i] <= L'9')
        {
            ++i;
        }
        if (i == ds || i + 1 >= line.size() || line[i] != L'.' || line[i + 1] != L' ')
        {
            return -1;
        }
        int n = 0;
        for (size_t k = ds; k < i; ++k)
        {
            n = n * 10 + (line[k] - L'0');
            if (n > 1'000'000)
            {
                return -1; // absurd; not a real index
            }
        }
        return n >= 1 ? n - 1 : -1; // 1-based render -> 0-based prompt index
    }

    // Agentmaster: is this line a bare "<Label>:" LIST HEADER — the shape RenderSummaryBox gives every
    // "* item" list section ("Skills Loaded:", "Files Created:", "Files Edited:", "Files Read:", and any
    // list section added later)? ASCII letters + spaces, then a trailing ':' and NOTHING after it. What
    // it deliberately excludes: a numbered message that happens to end with ':' (" 1. do this:" — a
    // leading space + digits, so a wrap-ON prompt whose continuation lines are markdown "* " bullets
    // keeps wrapping), "Recap: <text>" / "Tasks:  3 done …" / "Parent: <id>" (text after the colon), and
    // a "Previous session N · …" header. The section a list header opens renders its "* " items
    // single-line + clipped instead of wrapped (see _SetSummaryContent).
    static bool IsSummaryListHeader(const std::wstring& line)
    {
        if (line.size() < 2 || line.back() != L':')
        {
            return false;
        }
        const wchar_t first = line.front();
        if (!((first >= L'A' && first <= L'Z') || (first >= L'a' && first <= L'z')))
        {
            return false;
        }
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            const wchar_t c = line[k];
            if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L' '))
            {
                return false;
            }
        }
        return true;
    }

    void AgentTabOverlay::_SetSummaryContent(const std::wstring& text)
    {
        if (!_summaryStack)
        {
            return;
        }
        _summaryStack.Children().Clear();
        _jumpButtons.clear(); // rebuilt below; stale Button refs from the prior render are dropped
        _summaryMsgRows.clear(); // rebuilt below; the highlight (_highlightedMsgIndex) is re-applied after
        std::wstring seg; // accumulated contiguous text lines
        // Agentmaster: a run has ONE line mode. false = the classic WRAPPING run (headers, Tasks, Recap,
        // message continuations). true = a LIST run — the "* item" lines of a Files Created / Edited /
        // Read / Skills Loaded section — rendered NoWrap + CLIPPED at the panel edge: a long file name in
        // a narrow panel used to wrap into "*" over "myfile.txt" (the bullet alone on its line), so a
        // list read as twice its length and stopped scanning as one; now each item stays on its own
        // single line and simply runs off the right edge (widen the panel via its grip to see more; the
        // clipped text is still whole for selection / copy / the copyable Summary). The panel's
        // ScrollViewer has horizontal scrolling disabled, so a NoWrap block is measured at the viewport
        // width and TextTrimming::Clip cuts each line at a character boundary with no ellipsis (the
        // "overflow hidden" look). Everything that is NOT a list item keeps wrapping exactly as before.
        bool segNoWrap = false;
        const auto flushSeg = [&]() {
            if (seg.empty())
            {
                return;
            }
            TextBlock tb{};
            tb.FontFamily(FontFamily{ L"Cascadia Mono" });
            tb.FontSize(11);
            if (segNoWrap)
            {
                tb.TextWrapping(TextWrapping::NoWrap);
                tb.TextTrimming(TextTrimming::Clip);
            }
            else
            {
                tb.TextWrapping(TextWrapping::Wrap);
            }
            tb.IsTextSelectionEnabled(true);
            tb.Foreground(Fill(0xFF, 0xDC, 0xDC, 0xDC));
            tb.Text(winrt::hstring{ seg });
            _WireSummaryContextMenu(tb, -1); // right-click a body line => the shared "Copy Summary" menu (a selectable TextBlock shadows the parent's); not a numbered message, so no "Copy Prompt"
            _summaryStack.Children().Append(tb);
            seg.clear();
            segNoWrap = false;
        };
        // Append a line to the run being accumulated, closing the run first when the line's mode differs
        // from the run's (a list header — wrapping — is followed by its "* " items — NoWrap — so the two
        // never share one block; a run holds ONE mode).
        const auto appendLine = [&](const std::wstring& lineStr, bool noWrap) {
            if (!seg.empty() && segNoWrap != noWrap)
            {
                flushSeg();
            }
            if (!seg.empty())
            {
                seg += L"\n";
            }
            seg += lineStr;
            segNoWrap = noWrap;
        };
        // Agentmaster (SUMMARY_JUMP.md): a numbered message renders as a 2-column Grid — a JUMP button
        // (col 0, auto) + the wrapping message text (col 1, *) — so the text still wraps within the panel
        // while the button stays put. The button asks the page to center the session's terminal view on
        // where this prompt is rendered (a chime confirms a hit).
        const auto makeJumpRow = [this](const std::wstring& lineStr, int idx) -> winrt::Windows::UI::Xaml::UIElement {
            Grid g{};
            ColumnDefinition c0{};
            c0.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c1{};
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            g.ColumnDefinitions().Append(c0);
            g.ColumnDefinitions().Append(c1);

            Button jb{};
            jb.Background(Fill(0, 0, 0, 0)); // a TRANSPARENT brush (not null) — the whole content box is hit-testable
            jb.BorderThickness(ThicknessHelper::FromUniformLength(0));
            // Agentmaster: the bare ~10px ▸ glyph was a tiny click target. Enlarge the (invisible) hit
            // area to the FULL HEIGHT OF ITS OWN ROW so the whole left gutter of the prompt is clickable —
            // but it MUST be bounded BY the row. VerticalAlignment::Stretch fills the grid cell (whose
            // height is driven by the message text) and CANNOT spill above/below it; an earlier attempt
            // used a fixed "square" + negative top/bottom margins, which overflowed the row, so on short
            // one-line prompts (tiny rows) adjacent buttons OVERLAPPED. Keep the glyph pinned to the row
            // TOP (aligned with the first text line) via VerticalContentAlignment::Top, and keep the
            // original horizontal layout (padding 0,0,4,0 => icon at column-0 left, 4px gap to the text,
            // column-0 width 0+10+4 = 14) so NEITHER the icon position NOR the message-text start moves.
            jb.Padding(ThicknessHelper::FromLengths(0, 0, 4, 0));
            jb.Margin(ThicknessHelper::FromLengths(0, 0, 0, 0));
            jb.VerticalAlignment(VerticalAlignment::Stretch); // fill THIS row's height, never overflow into neighbors
            jb.VerticalContentAlignment(VerticalAlignment::Top); // glyph stays at the row top (its original Y)
            jb.Opacity(0.7);
            FontIcon ji{};
            ji.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // a text font carrying U+25B8 (the icon font would tofu it)
            ji.Glyph(L"\x25B8"); // ▸ — "go to / jump"
            ji.FontSize(10);
            ji.IsHitTestVisible(false); // CLICK-THROUGH: a click on the tiny ▸ glyph must land on the jump button jb, not be eaten by the icon (see the action-row mkIconBtn note)
            jb.Content(ji);
            AgentSetTip(jb, winrt::hstring{ L"Jump to where this prompt is on screen" });
            const auto weak = get_weak();
            jb.Click([weak, idx](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
                if (const auto self = weak.get())
                {
                    if (self->_onJumpToPrompt && idx >= 0 && idx < static_cast<int>(self->_summaryUserMsgs.size()))
                    {
                        const int row = self->_onJumpToPrompt(self->_summaryUserMsgs, idx);
                        if (row >= 0)
                        {
                            ::PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
                            self->HighlightSummaryMessage(idx); // mark the row we jumped to
                        }
                        self->_RefreshJumpEligibility(true); // a click makes the others eligible to re-check (user action -> bypass the periodic floor; usually free via the epoch cache)
                    }
                }
            });
            _jumpButtons.emplace_back(idx, jb); // register for eligibility dimming
            Grid::SetColumn(jb, 0);
            g.Children().Append(jb);

            TextBlock tb{};
            tb.FontFamily(FontFamily{ L"Cascadia Mono" });
            tb.FontSize(11);
            tb.TextWrapping(TextWrapping::Wrap);
            tb.IsTextSelectionEnabled(true);
            tb.Foreground(Fill(0xFF, 0xDC, 0xDC, 0xDC));
            tb.Text(winrt::hstring{ lineStr });
            Grid::SetColumn(tb, 1);
            g.Children().Append(tb);
            // Right-click anywhere on THIS row (its text, its gutter, or the ▸ button, which bubbles to
            // the row) => the shared menu WITH "Copy Prompt" bound to this row's prompt. Wire the text
            // block explicitly: a selectable TextBlock shadows its parent's menu, so the row Grid alone
            // would never see the request.
            _WireSummaryContextMenu(tb, idx);
            _WireSummaryContextMenu(g, idx);
            _summaryMsgRows.emplace_back(idx, g); // register the row so HighlightSummaryMessage can band it
            return g;
        };
        size_t i = 0;
        // RenderSummaryBox numbers the messages strictly sequentially (1..N, one per prompt). Only treat a
        // line as a real message-START when its number is the NEXT expected one — so in wrap-ON mode a
        // multi-line prompt's continuation line that merely LOOKS like "2. foo" isn't mis-detected as a
        // numbered message (and mis-mapped to the wrong prompt). expectedMsg is the next 0-based index.
        int expectedMsg = 0;
        // Agentmaster: with the previous-session toggle ON, the renderer puts each pre-compaction /
        // cross-file segment ABOVE the current conversation, under its own "Previous session N" header,
        // and numbers ITS prompts 1.. independently. Those numbers do NOT index _summaryUserMsgs (the
        // CURRENT segment's prompts only), so such a line must never become a jump / Copy-Prompt row — it
        // would resolve to a different prompt entirely (and, by consuming expectedMsg, would also strip
        // the real messages below of their buttons). The section's KIND is decided by the FIRST line after
        // each separator — every section starts with one — so a wrapped message's continuation lines can
        // never flip it, and a prompt whose text happens to start with "Previous session " is never a
        // section's first line (the current messages' first line is always " 1. ...").
        bool sectionIsPrevious = false;
        // Agentmaster: the section's first line is a bare "<Label>:" LIST header (IsSummaryListHeader —
        // Files Created / Edited / Read, Skills Loaded, …): its "* " item lines render as a NoWrap +
        // clipped run instead of wrapping. Decided by the section's FIRST line like sectionIsPrevious, so
        // a "* " bullet inside a wrapped message or the Recap can never be mistaken for a list item.
        bool sectionIsList = false;
        bool sectionKindPending = true; // the box's first section starts at the top, with no leading separator
        while (i <= text.size())
        {
            const size_t nl = text.find(L'\n', i);
            const size_t end = (nl == std::wstring::npos) ? text.size() : nl;
            const std::wstring lineStr = text.substr(i, end - i);
            const bool isSep = (lineStr.size() == 1 && lineStr[0] == kSepMark);
            if (!isSep && sectionKindPending)
            {
                // A section's FIRST line names its kind: a "Previous session N · <label>" header opens a
                // previous segment (its numbered prompts index ITS list); a bare "<Label>:" header opens a
                // "* item" list section; anything else is a normal section.
                sectionIsPrevious = (lineStr.rfind(L"Previous session ", 0) == 0);
                sectionIsList = IsSummaryListHeader(lineStr);
                sectionKindPending = false;
            }
            if (isSep)
            {
                flushSeg(); // close the run above the rule
                Border rule{};
                rule.Height(1);
                rule.HorizontalAlignment(HorizontalAlignment::Stretch); // border to border
                rule.Background(Fill(0x40, 0xFF, 0xFF, 0xFF));
                rule.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
                _summaryStack.Children().Append(rule);
                sectionIsPrevious = false; // a new section opens below this rule; the next line names its kind
                sectionIsList = false;
                sectionKindPending = true;
            }
            else if (const int mi = ParseSummaryMsgIndex(lineStr); _onJumpToPrompt && !sectionIsPrevious && mi == expectedMsg && mi < static_cast<int>(_summaryUserMsgs.size()))
            {
                flushSeg(); // close the run above this numbered message; render it with a jump button
                _summaryStack.Children().Append(makeJumpRow(lineStr, mi));
                ++expectedMsg; // the next message-start must be the following number
            }
            else
            {
                // A "* " item of a list section joins (or opens) a NoWrap run; every other line — the list
                // header itself included — joins the classic wrapping run.
                appendLine(lineStr, sectionIsList && lineStr.rfind(L"* ", 0) == 0);
            }
            if (nl == std::wstring::npos)
            {
                break;
            }
            i = nl + 1;
        }
        flushSeg();
        // Dim the jump buttons whose prompt isn't currently on screen. NOT forced: this runs on every panel
        // re-render, and the panel re-renders on every transcript growth — forcing here would reinstate the
        // unbounded resolve churn the focus/interval gate exists to stop. Freshly built buttons therefore
        // keep their default opacity until the next gated pass (or the focus transition, which forces one);
        // a button that resolves to nothing is still clickable and re-checks on click.
        _RefreshJumpEligibility();
        _ApplySummaryHighlight(); // re-apply the jumped-to band onto the freshly-rebuilt rows
    }

    void AgentTabOverlay::SetSummaryToggleHandler(std::function<void()> handler)
    {
        _onToggleSummary = std::move(handler);
    }

    void AgentTabOverlay::SetJumpHandler(std::function<int(const std::vector<std::wstring>&, int)> handler)
    {
        _onJumpToPrompt = std::move(handler);
    }

    void AgentTabOverlay::SetEligibilityHandler(std::function<std::vector<int>(const std::vector<std::wstring>&)> handler)
    {
        _onResolveEligibility = std::move(handler);
    }

    void AgentTabOverlay::SetAdjacentPromptHandler(std::function<void(bool)> handler)
    {
        _onAdjacentPrompt = std::move(handler);
    }

    void AgentTabOverlay::SetLiveDraftHandler(std::function<std::wstring()> handler)
    {
        _onReadLiveDraft = std::move(handler);
    }

    void AgentTabOverlay::SetClearDraftHandler(std::function<void()> handler)
    {
        _onClearDraft = std::move(handler);
    }

    // Agentmaster (SUMMARY_JUMP.md): resolve every numbered prompt against the live buffer in one pass and
    // DIM the jump buttons whose prompt currently won't resolve (scrolled off / not rendered), so the dead
    // icons are visually distinct from the working ones. Called on (re)build, on the 5 s times-line tick
    // (visible-only), and after a click. Bounded + opt-in (only when the summary panel is shown), so the
    // cost is a single linearize+resolve at most every few seconds per visible panel.
    void AgentTabOverlay::_RefreshJumpEligibility(bool force)
    {
        if (!_onResolveEligibility || _jumpButtons.empty() || _summaryUserMsgs.empty())
        {
            return;
        }
        // Agentmaster (SUMMARY_JUMP.md §4, perf) — the two gates that keep this off the UI thread's back.
        // The resolve behind _onResolveEligibility is O(prompts x the 1.2M-char buffer window): every
        // prompt that has scrolled out of the buffer (most of them, on a long conversation) costs the FULL
        // probe set, so a 244-prompt session measured ~0.3s per pass. It used to be driven by the panel's
        // own 5 s timer on EVERY linked overlay in the window (the "visible-only" comment was aspirational
        // -- SetSummaryEnabled starts the timer per overlay, gated only on the GLOBAL showSummaryPanel), so
        // ~19 live Claude tabs asked for ~19 of those every 5 s. The UI thread never caught up: it wedged
        // at ~98% of a core inside ResolvePromptAnchors and the window stopped pumping input entirely
        // (the 2026-07-19 release freeze). Both gates are skipped for `force` -- a user action or the
        // focus transition, where the answer must be current right now.
        if (!force)
        {
            // (a) Only the SELECTED tab. The result paints nothing but this panel's icon opacities, and a
            //     background panel's pixels aren't on screen -- switching to it forces a resolve anyway.
            if (!_tabFocused)
            {
                return;
            }
            // (b) A floor between periodic passes. Both periodic callers (the panel's 5 s tick and the
            //     page's 30 s focused refresh) land here, so this is what sets the real cadence.
            const auto now = std::chrono::steady_clock::now();
            if (_lastJumpEligibilityRun.time_since_epoch().count() != 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastJumpEligibilityRun).count() < kJumpEligibilityMinIntervalMs)
            {
                return;
            }
            _lastJumpEligibilityRun = now;
        }
        else
        {
            _lastJumpEligibilityRun = std::chrono::steady_clock::now();
        }
        const auto rows = _onResolveEligibility(_summaryUserMsgs);
        for (const auto& [idx, btn] : _jumpButtons)
        {
            if (!btn)
            {
                continue;
            }
            const bool ok = idx >= 0 && idx < static_cast<int>(rows.size()) && rows[idx] >= 0;
            btn.Opacity(ok ? 0.75 : 0.2); // match: normal; no-match: clearly dim (still clickable -> re-checks)
        }
    }

    // Agentmaster (SUMMARY_JUMP.md §7): public entry for the page's 30 s focused refresh — just re-resolve
    // eligibility (the underlying position resolve is fresh on every call). Kept thin so the page never
    // reaches into privates.
    void AgentTabOverlay::RefreshJumpData()
    {
        _RefreshJumpEligibility(); // periodic -> subject to the focus + interval gates
    }

    // Agentmaster (SUMMARY_JUMP.md §4, perf): the page's tab-selection funnel mirrors the window's selected
    // tab onto every linked overlay. Only the focused overlay runs the periodic (expensive) eligibility
    // resolve; the moment a tab BECOMES focused we force one so its panel is accurate immediately rather
    // than up to kJumpEligibilityMinIntervalMs stale. Losing focus is silent -- the panel is off screen.
    void AgentTabOverlay::SetTabFocused(bool focused)
    {
        if (_tabFocused == focused)
        {
            return;
        }
        _tabFocused = focused;
        if (focused && _summaryEnabled)
        {
            _RefreshJumpEligibility(true);
        }
    }

    // Agentmaster (TAB_OVERLAY.md summary panel): a cheap, mtime-gated content re-read from the freshest
    // registry snapshot — the SAME reload the 5 s backstop timer's Tick performs (the panel content
    // otherwise reloads only on that timer or a registry notify). Exposed so the page can kick it when
    // this tab is FOCUSED, so switching TO a background tab shows current content instead of up to ~5 s
    // stale. Unlike the ↻ button (_RefreshSummary) this does NOT force — the mtime gate inside
    // _LoadSummaryAsync makes an unchanged transcript a cheap stat with no re-render (no flicker / scroll
    // reset), and a load already in flight is coalesced via _summaryReloadPending. No-op when the panel is
    // off / no session / no registry.
    void AgentTabOverlay::RefreshSummaryContent()
    {
        if (_summaryEnabled && _registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
            }
        }
    }

    // Agentmaster (SUMMARY_JUMP.md): remember the message we jumped to and paint the band on its row.
    // Called from the ▸ button (idx known directly) and from the page after alt+up / alt+down nav (the
    // landed 0-based index). The mark moves to the new target on the next jump.
    void AgentTabOverlay::HighlightSummaryMessage(int index)
    {
        _highlightedMsgIndex = index;
        _ApplySummaryHighlight();
        // Bring the freshly-targeted row into view within the (scrolling) panel — only on an explicit jump,
        // not on the rebuild re-apply, so a transcript-growth refresh doesn't keep yanking the panel scroll.
        for (const auto& [idx, row] : _summaryMsgRows)
        {
            if (row && idx == index)
            {
                row.StartBringIntoView();
                break;
            }
        }
    }

    // Paint a translucent accent band behind _highlightedMsgIndex's row; clear every other row. Safe any
    // time — a no-op when the panel isn't built or the index isn't currently rendered — and re-applied at
    // the end of each _SetSummaryContent so the mark survives a transcript-growth re-render.
    void AgentTabOverlay::_ApplySummaryHighlight()
    {
        for (const auto& [idx, row] : _summaryMsgRows)
        {
            if (!row)
            {
                continue;
            }
            // accent band (alpha ~0x66) on the target; a fully-transparent fill clears the rest
            row.Background(idx == _highlightedMsgIndex ? Fill(0x66, 0x3B, 0x82, 0xF6) : Fill(0, 0, 0, 0));
        }
    }

    void AgentTabOverlay::SetSummaryEnabled(bool on)
    {
        // The summary panel's visibility is a GLOBAL setting (AppSettings::showSummaryPanel), mirrored
        // into the overlay here by the page — on attach (seed) and on every pencil toggle (broadcast to
        // every linked overlay in the window). _Refresh reads _summaryEnabled too, so the panel stays in
        // step on subsequent registry events.
        _summaryEnabled = on;
        _UpdateSummaryPencilVisual(); // the badge pencil glyph answers the toggle INSTANTLY (dim off / lighter on) — even on a tab whose panel has nothing to render
        if (!_summaryRoot)
        {
            return; // no panel on the observe badge
        }
        if (!on)
        {
            if (_summaryTimer)
            {
                _summaryTimer.Stop(); // no need to tick the times line while hidden
            }
            _summaryRoot.Visibility(Visibility::Collapsed);
            return;
        }
        if (_summaryTimer)
        {
            _summaryTimer.Start(); // drive the live "ago" times line
        }
        _UpdateTimesLine(); // show the times immediately from whatever is cached
        // Enabled: show + (re)load from the freshest session snapshot.
        if (_registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
                return;
            }
        }
        _ApplySummaryVisibility(); // enabled but no session yet — stay hidden until there's something to show
    }

    void AgentTabOverlay::_ToggleSummary()
    {
        // The pencil flips the GLOBAL setting, not per-session state: hand off to the page, which does
        // the freshest-disk read-modify-write of settings.json AND applies it live to every linked
        // overlay in the window (SetSummaryEnabled). No-op if no handler is wired (defensive).
        if (_onToggleSummary)
        {
            _onToggleSummary();
        }
    }

    void AgentTabOverlay::SetSummaryWrapToggleHandler(std::function<void()> handler)
    {
        _onToggleSummaryWrap = std::move(handler);
    }

    // The text the user selected in the summary panel (title / times / body runs) — feeds + gates the
    // context menu's "Copy Selected Text". Empty when nothing is selected. UI thread only.
    std::wstring AgentTabOverlay::_SummarySelectedText()
    {
        std::wstring out;
        SummaryCollectSelectedText(_summaryRoot, out);
        return out;
    }

    void AgentTabOverlay::_ToggleSummaryWrap()
    {
        // The wrap-line icon flips the GLOBAL setting (AppSettings::summaryPanelWrapNewlines), not
        // per-session state: hand off to the page, which does the freshest-disk read-modify-write of
        // settings.json AND applies it live to every linked overlay in the window (SetSummaryWrapNewlines).
        if (_onToggleSummaryWrap)
        {
            _onToggleSummaryWrap();
        }
    }

    // Recolor the wrap-line icon to reflect the toggle: a dim gray when OFF (the literal-\n look),
    // a clearly lighter shade when ON (newlines preserved) — the "slight color lighter change on true".
    void AgentTabOverlay::_UpdateSummaryWrapButtonVisual()
    {
        if (!_summaryWrapIcon)
        {
            return;
        }
        _summaryWrapIcon.Foreground(_summaryWrapNewlines ? Fill(0xFF, 0xE6, 0xE6, 0xE6)  // ON: lighter (active)
                                                         : Fill(0xFF, 0x8C, 0x8C, 0x8C)); // OFF: dim (inactive)
    }

    void AgentTabOverlay::SetSummaryWrapNewlines(bool on)
    {
        // The newline-wrap mode is a GLOBAL setting (AppSettings::summaryPanelWrapNewlines), mirrored into
        // the overlay here by the page — on attach (seed) and on every wrap-toggle (broadcast to every
        // linked overlay in the window). The flag is BAKED into the rendered text (SummaryEscapeMsg), so a
        // real change must force a re-analyze+render: reset the mtime gate and re-pull from the freshest
        // snapshot. Always refresh the icon color (the seed may match the default but still needs painting).
        const bool changed = (_summaryWrapNewlines != on);
        _summaryWrapNewlines = on;
        _UpdateSummaryWrapButtonVisual();
        if (!changed)
        {
            return; // seed with the same value — nothing baked differently, no reload
        }
        _summaryMtime = 0; // invalidate the mtime gate so _LoadSummaryAsync re-renders with the new flag
        if (_summaryLoading)
        {
            // A load is in flight with the OLD flag; _UpdateSummary would no-op on the guard. Mark dirty so
            // the load's completion re-renders with the now-current flag (else the toggle wouldn't take
            // until the transcript next grew).
            _summaryWrapDirty = true;
            return;
        }
        if (_summaryEnabled && _registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
            }
        }
    }

    void AgentTabOverlay::SetSummaryTruncateToggleHandler(std::function<void()> handler)
    {
        _onToggleSummaryTruncate = std::move(handler);
    }

    // Agentmaster: the times-bar REFRESH button — force a re-analyze+render of the summary NOW, regardless
    // of the transcript's mtime (which gates the automatic reload). Mirrors the toggle handlers' reload path:
    // reset the mtime gate and re-pull from the freshest registry snapshot. A purely-local action (no global
    // setting / no broadcast) — it just re-reads THIS panel's own transcript.
    void AgentTabOverlay::_RefreshSummary()
    {
        _summaryMtime = 0; // invalidate the mtime gate so _LoadSummaryAsync re-reads the file
        if (_summaryLoading)
        {
            // A load is already in flight; mark dirty so its completion re-renders (the dirty-flag path
            // resets the gate again), instead of the in-flight load's result sticking.
            _summaryWrapDirty = true;
            return;
        }
        if (_summaryEnabled && _registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
            }
        }
    }

    void AgentTabOverlay::_ToggleSummaryPrevious()
    {
        // The previous-session icon flips the GLOBAL setting (AppSettings::summaryPanelShowPrevious), not
        // per-session state: hand off to the page, which does the freshest-disk read-modify-write of
        // settings.json AND applies it live to every linked overlay in the window (SetSummaryShowPrevious).
        if (_onToggleSummaryPrevious)
        {
            _onToggleSummaryPrevious();
        }
    }

    // Recolor the previous-session icon to reflect the toggle: a dim gray when OFF (previous sessions
    // hidden), a clearly lighter shade when ON (shown) — matching the wrap/truncate toggles.
    void AgentTabOverlay::_UpdateSummaryPrevButtonVisual()
    {
        if (!_summaryPrevIcon)
        {
            return;
        }
        _summaryPrevIcon.Foreground(_summaryShowPrevious ? Fill(0xFF, 0xE6, 0xE6, 0xE6)  // ON: lighter (active)
                                                         : Fill(0xFF, 0x8C, 0x8C, 0x8C)); // OFF: dim (inactive)
    }

    // Recolor the badge's PENCIL (the summary-panel toggle in the row-1 action strip) to reflect the
    // GLOBAL showSummaryPanel — dim when OFF, lighter when ON, the same state language as the panel's
    // wrap/truncate/previous toggles. The pencil was the ONE toggle in the overlay with NO state
    // visual: on a tab whose panel has nothing to render (below), a click changed nothing visible
    // anywhere — the "clicking the pencil does nothing / button not hit" report, where the diagnostic
    // trace showed every CLICK firing and the setting flipping, invisibly. Driven by SetSummaryEnabled
    // (the page's seed + every pencil-toggle broadcast), so all tabs' pencils stay in step.
    void AgentTabOverlay::_UpdateSummaryPencilVisual()
    {
        if (!_summaryPencilIcon)
        {
            return;
        }
        _summaryPencilIcon.Foreground(_summaryEnabled ? Fill(0xFF, 0xE6, 0xE6, 0xE6)  // ON: lighter (panel shown)
                                                      : Fill(0xFF, 0x8C, 0x8C, 0x8C)); // OFF: dim (panel hidden)
    }

    // Agentmaster (pencil feedback): when the panel is ENABLED for a linked session but there is
    // NOTHING to render — a never-prompted session has no transcript yet (Claude creates <id>.jsonl on
    // the first message; the reported repro was a tab opened ~30 s earlier), a Codex not yet reconciled
    // has no rollout uuid — the panel used to stay collapsed by design ("just an empty box"), which
    // made a perfectly-working pencil toggle read as a DEAD button. Render ONE dim placeholder line
    // instead, so enabling the panel ALWAYS has a visible consequence and says WHY it's empty. Self-
    // gated: enabled + an EMPTY body + no times line (a times-only panel is already visible); the
    // first real load's _SetSummaryContent Clear()s it away, and _ApplySummaryVisibility's
    // "collapse when empty" contract is untouched (the placeholder IS content).
    void AgentTabOverlay::_EnsureSummaryPlaceholder()
    {
        if (!_summaryEnabled || !_summaryStack || _summaryStack.Children().Size() > 0)
        {
            return;
        }
        if (_summaryTimesText && !_summaryTimesText.Text().empty())
        {
            return; // a times line already makes the panel visible — no placeholder under it
        }
        TextBlock tb{};
        tb.FontFamily(FontFamily{ L"Cascadia Mono" });
        tb.FontSize(11);
        tb.TextWrapping(TextWrapping::Wrap);
        tb.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // dim — an empty-state note, not content
        tb.Text(L"No conversation yet \x2014 the summary fills in after this session's first prompt.");
        _summaryStack.Children().Append(tb);
        _ApplySummaryVisibility(); // the body is non-empty now — the panel shows
    }

    void AgentTabOverlay::SetSummaryShowPrevious(bool on)
    {
        // The show-previous mode is a GLOBAL setting (AppSettings::summaryPanelShowPrevious), mirrored into
        // the overlay here by the page — on attach (seed) and on every toggle (broadcast to every linked
        // overlay in the window). The flag is BAKED into the rendered text (RenderSummaryBox), so a real
        // change must force a re-analyze+render: reset the mtime gate and re-pull from the freshest
        // snapshot. Always refresh the icon color (the seed may match the default but still needs painting).
        const bool changed = (_summaryShowPrevious != on);
        _summaryShowPrevious = on;
        _UpdateSummaryPrevButtonVisual();
        if (!changed)
        {
            return; // seed with the same value — nothing baked differently, no reload
        }
        _summaryMtime = 0; // invalidate the mtime gate so _LoadSummaryAsync re-renders with the new flag
        if (_summaryLoading)
        {
            _summaryPrevDirty = true; // a load is in flight with the OLD flag; re-render on completion (else the toggle wouldn't take until the transcript next grew)
            return;
        }
        if (_summaryEnabled && _registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
            }
        }
    }

    void AgentTabOverlay::SetSummaryPreviousToggleHandler(std::function<void()> handler)
    {
        _onToggleSummaryPrevious = std::move(handler);
    }

    void AgentTabOverlay::_ToggleSummaryTruncate()
    {
        // The truncate icon flips the GLOBAL setting (AppSettings::summaryPanelTruncate), not per-session
        // state: hand off to the page, which does the freshest-disk read-modify-write of settings.json AND
        // applies it live to every linked overlay in the window (SetSummaryTruncate).
        if (_onToggleSummaryTruncate)
        {
            _onToggleSummaryTruncate();
        }
    }

    // Recolor the truncate icon to reflect the toggle: a dim gray when OFF (everything shown), a clearly
    // lighter shade when ON (messages capped) — same dim/active palette as the wrap toggle.
    void AgentTabOverlay::_UpdateSummaryTruncateButtonVisual()
    {
        if (!_summaryTruncateIcon)
        {
            return;
        }
        _summaryTruncateIcon.Foreground(_summaryTruncate ? Fill(0xFF, 0xE6, 0xE6, 0xE6)  // ON: lighter (active)
                                                         : Fill(0xFF, 0x8C, 0x8C, 0x8C)); // OFF: dim (inactive)
    }

    void AgentTabOverlay::SetSummaryTruncate(bool on)
    {
        // The truncate mode is a GLOBAL setting (AppSettings::summaryPanelTruncate), mirrored into the
        // overlay here by the page — on attach (seed) and on every truncate-toggle (broadcast to every
        // linked overlay in the window). The flag is BAKED into the rendered text (SummaryEscapeMsg), so a
        // real change must force a re-analyze+render: reset the mtime gate and re-pull from the freshest
        // snapshot. Always refresh the icon color (the seed may match the default but still needs painting).
        const bool changed = (_summaryTruncate != on);
        _summaryTruncate = on;
        _UpdateSummaryTruncateButtonVisual();
        if (!changed)
        {
            return; // seed with the same value — nothing baked differently, no reload
        }
        _summaryMtime = 0; // invalidate the mtime gate so _LoadSummaryAsync re-renders with the new flag
        if (_summaryLoading)
        {
            // A load is in flight with the OLD flag; _UpdateSummary would no-op on the guard. Mark dirty so
            // the load's completion re-renders with the now-current flag (else the toggle wouldn't take
            // until the transcript next grew).
            _summaryTruncateDirty = true;
            return;
        }
        if (_summaryEnabled && _registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
            }
        }
    }

    void AgentTabOverlay::_UpdateSummary(const SessionInfo& s)
    {
        if (!_summaryRoot)
        {
            return; // no panel on the observe badge (built only by Initialize, for a linked session)
        }
        if (!_summaryEnabled)
        {
            _summaryRoot.Visibility(Visibility::Collapsed);
            return;
        }
        // Agentmaster (subagent activity): capture the registry's SUBAGENT-FOLDED last-activity every
        // refresh — synchronous, from the live snapshot, and BEFORE the _summaryLoading guard below so a
        // slow in-flight load can't freeze it. The times line (_UpdateTimesLine) max()es it into "last
        // activity" so the panel keeps advancing while a Task/Agent subagent runs and the PARENT
        // <id>.jsonl (the panel's own transcript source) stays quiescent — the "looks idle while working"
        // fix, keying on the same OBSERVER subagent fold that feeds SessionInfo::convLastActivityUnixMs.
        _summaryConvLastActivityMs = s.convLastActivityUnixMs;
        // Pinned TITLE row (top of the panel): the session's title == the tab name (Rule #11). Set it every
        // refresh — synchronously on the UI thread, independent of the off-thread transcript load below — so
        // a rename updates it live. Guarded so an unchanged title doesn't relayout the wrapping block each
        // pass. (The panel as a whole still only SHOWS when there's times/content to render —
        // _ApplySummaryVisibility governs that — so this never opens a title-only box.)
        if (_summaryTitleText)
        {
            const winrt::hstring title{ s.title };
            if (_summaryTitleText.Text() != title)
            {
                _summaryTitleText.Text(title);
                _summaryTitleText.Visibility(title.empty() ? Visibility::Collapsed : Visibility::Visible);
            }
        }
        _ApplySummaryVisibility(); // show only if there's already something to render (else stay hidden until the load lands)
        if (_summaryLoading)
        {
            // One analyze+render is already in flight. Don't drop this request: the transcript may have
            // grown since that load started its read, and relying on "the next _Refresh" loses the growth
            // when this WAS the last refresh (a turn's closing message lands just after the final hook's
            // _Refresh, then the session idles and no further notify comes). Mark it pending so the load's
            // completion re-checks for growth (mtime-gated, so a no-growth re-check is a cheap no-op).
            _summaryReloadPending = true;
            return;
        }
        const bool codex = (s.kind == AgentKind::Codex);
        const std::wstring convId = codex ? (s.codexSessionId.empty() ? s.id : s.codexSessionId) : s.id;
        if (convId.empty())
        {
            _EnsureSummaryPlaceholder(); // a codex not yet reconciled (no rollout uuid) — nothing to analyze, but the enabled panel must still visibly answer the pencil
            return;
        }
        // Agentmaster (never-messaged fork): a Claude fork has NO transcript of its own until its first
        // message, so its summary panel would be empty (the "fork's summary toggle does nothing" report).
        // Pass its fork-parent id so _LoadSummaryAsync can fall back to the parent's transcript — the fork
        // inherits it verbatim until it diverges. Codex isn't covered (its source-rollout uuid isn't
        // persisted; the analyze id is the rollout uuid, not s.id), so pass empty there.
        std::wstring forkParent = codex ? std::wstring{} : s.forkParentId;
        std::wstring cwd = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
        std::wstring liveGlyph{ StateGlyph(s.state) };
        std::wstring liveLabel{ StateLabel(s.state) };
        _summaryLoading = true;
        // Cross-file lineage memo: reuse the already-resolved parents when they were computed for THIS
        // conv id (parentage is immutable); a rebind to a new id recomputes. Avoids a per-write dir scan.
        const bool lineageCached = (_summaryLineageId == convId);
        _LoadSummaryAsync(_summaryPath, codex, convId, std::move(forkParent), std::move(cwd), std::move(liveGlyph), std::move(liveLabel), _summaryMtime, _summaryWrapNewlines, _summaryTruncate, _summaryShowPrevious, lineageCached, _summaryLineage);
    }

    winrt::fire_and_forget AgentTabOverlay::_LoadSummaryAsync(std::wstring transcriptPath, bool codex, std::wstring sessionId, std::wstring forkParentId, std::wstring cwd, std::wstring liveGlyph, std::wstring liveLabel, int64_t prevMtime, bool wrapNewlines, bool truncate, bool showPrevious, bool lineageCached, std::vector<::Agentmaster::ConversationSegment> cachedLineage)
    {
        auto strong = get_strong(); // keep the overlay alive across the co_await (it owns _summaryStack)
        // Agentmaster (contained): an exception escaping this fire_and_forget == winrt::terminate() ==
        // the whole app dies silently — and THIS lane (unlike its tooltip twin, hardened by be1d63b7f)
        // had no containment at all: the v0.6.x resume crash-loop (a std::bad_alloc analyzing a huge
        // image-paste transcript ~2-4s after the resumed claude's SessionStart) died exactly here. The
        // engine helpers self-contain now; the try below nets the remaining glue (path resolution,
        // lambda-capture copies, the TryEnqueue call) so the worst case is an empty panel + a log line,
        // never a dead app. The INNER try keeps the normal completion running (it clears
        // _summaryLoading, so the panel can't wedge); the OUTER catch covers a completion that never
        // got posted and best-effort clears the flag from the UI thread.
        try
        {
        co_await winrt::resume_background();

        // Resolve the transcript path once (cached in _summaryPath across reloads). Claude: a shallow
        // glob by conversation id; Codex: a recursive date-sharded glob by rollout uuid (worth caching).
        std::wstring path = transcriptPath;
        bool cachePath = true; // write the resolved path back into _summaryPath for the next reload
        if (path.empty())
        {
            if (codex)
            {
                path = ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), sessionId);
            }
            else
            {
                path = ::Agentmaster::ResolveClaudeTranscriptPath(sessionId);
                // Agentmaster (never-messaged fork): a fork has NO transcript of its own until its first
                // message — Claude creates <forkId>.jsonl only then, copying the parent's lines verbatim.
                // Until then its conversation IS the parent's, so summarize the PARENT's transcript instead
                // of showing an empty panel (the "fork's summary toggle does nothing" report). Do NOT cache
                // this path: re-resolve the fork's OWN id each reload so the panel switches to its own
                // transcript the instant it appears (the mtime gate still skips a quiet re-resolve cheaply).
                if (path.empty() && !forkParentId.empty())
                {
                    const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(forkParentId);
                    if (!parentPath.empty())
                    {
                        path = parentPath;
                        cachePath = false;
                    }
                }
            }
        }

        std::wstring text;
        std::vector<std::wstring> userMsgs; // Agentmaster (SUMMARY_JUMP.md): the raw prompts, aligned with the rendered " N. " lines
        int64_t mtime = prevMtime;
        int64_t createdMs = 0, lastUserMs = 0, lastActivityMs = 0; // times-line instants (computed on reload)
        bool timesComputed = false;
        bool hasPrevious = false; // Agentmaster (conversation lineage): the analyzed session has a previous (pre-/compact OR cross-file) segment => reveal the toggle button
        // Agentmaster (cross-file lineage): the /clear + plan-restart parents. Reuse the memo when it was
        // computed for this session (lineageCached), else walk + memoize below (lineageComputed flags the
        // writeback). Immutable per session, so a quiet reload never re-walks.
        std::vector<::Agentmaster::ConversationSegment> lineage = std::move(cachedLineage);
        bool lineageComputed = false;
        if (!path.empty())
        {
            try
            {
            // Cheap stat: only do the heavy read+analyze when the transcript grew (mtime advanced) or
            // we've never loaded it (prevMtime == 0). A quiet tab thus costs one GetFileAttributesEx.
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER li{};
                li.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                mtime = static_cast<int64_t>(li.QuadPart);
            }
            if (mtime != prevMtime || prevMtime == 0)
            {
                // The DISPLAYED panel is the TRIMMED view (full=false): it omits everything the link
                // badge already shows. id + resumeCmd are passed empty here — full=false never renders
                // them (the full box, with both, is the copy-menu "Summary" via CopySummaryAsync).
                if (codex)
                {
                    const auto info = ::Agentmaster::ReadCodexRolloutInfo(path, 0 /* whole file */, 200 /* prompts */);
                    createdMs = info.createdUnixMs;
                    lastActivityMs = info.lastActivityUnixMs;
                    lastUserMs = 0; // a rollout carries no per-message timestamp for the last human prompt
                    text = RenderCodexSummary(info, sessionId, cwd, path, L"", liveGlyph, liveLabel, /*full*/ false, wrapNewlines, truncate);
                }
                else
                {
                    auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
                    // Cross-file lineage: PREPEND the /clear + plan-restart parents (oldest first), so the
                    // panel's "Previous session N" list spans files. Computed once per session, then memoized.
                    if (!lineageCached)
                    {
                        lineage = ::Agentmaster::CollectConversationLineage(sessionId, cwd, 16);
                        lineageComputed = true; // cache it (even when empty) so a quiet reload won't re-walk
                    }
                    if (!lineage.empty())
                    {
                        a.previousSegments.insert(a.previousSegments.begin(), lineage.begin(), lineage.end());
                    }
                    userMsgs = a.userMsgs; // the prompts, in order — aligns with the rendered " N. " jump rows
                    hasPrevious = !a.previousSegments.empty(); // a /compact'ed OR cross-file-continued session => the previous-session toggle is meaningful
                    createdMs = IsoToUnixMs(a.firstTs);
                    lastUserMs = IsoToUnixMs(a.lastUserTs);
                    lastActivityMs = IsoToUnixMs(a.lastTs);
                    // plan-start: the plan file lives in the PARENT transcript (session-end.js
                    // getPlanFileFromParent) — resolve + scan it when this session points at one.
                    std::wstring planFile = a.planFilePath;
                    if (planFile.empty() && a.hasPlanContent && !a.parentSessionId.empty())
                    {
                        const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(a.parentSessionId);
                        if (!parentPath.empty())
                        {
                            planFile = ::Agentmaster::FindPlanFileInTranscript(parentPath);
                        }
                    }
                    text = RenderSummaryBox(a, sessionId, cwd, path, L"", liveGlyph, liveLabel, planFile, /*full*/ false, wrapNewlines, truncate, showPrevious);
                }
                timesComputed = true;
            }
            }
            catch (...)
            {
                // Degrade to an empty panel (the placeholder line renders); `mtime` keeps the fresh
                // stat taken above the throw, so the completion stores it and a broken transcript
                // isn't re-analyzed in a hot loop every 5s tick — only real growth retries.
                text.clear();
                userMsgs.clear();
                lineage.clear();
                timesComputed = false;
                lineageComputed = false;
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[summary] contained _LoadSummaryAsync analyze throw (no crash)\n");
            }
        }

        // Hop back to the UI thread to publish (the StackPanel build + member writes are UI-thread only).
        if (auto disp = _dispatcher)
        {
            disp.TryEnqueue([weak = get_weak(), text, userMsgs, path, cachePath, mtime, createdMs, lastUserMs, lastActivityMs, timesComputed, hasPrevious, sessionId, lineage, lineageComputed]() {
                if (auto self = weak.get())
                {
                    // Agentmaster (contained): a XAML throw in this dispatcher callback would fail-fast
                    // the app (0xC000027B stowed exception) — contain it and keep _summaryLoading sane.
                    try
                    {
                    if (timesComputed)
                    {
                        self->_summaryUserMsgs = userMsgs; // set BEFORE _SetSummaryContent so jump rows resolve the right prompt
                        // Agentmaster (cross-file lineage): memoize the resolved parents so a quiet mtime-gated
                        // reload OR a show-previous toggle reuses them (no re-walk). Keyed by conv id — a rebind
                        // recomputes. Stored even when empty (a "no parents" verdict is also worth caching).
                        if (lineageComputed)
                        {
                            self->_summaryLineageId = sessionId;
                            self->_summaryLineage = lineage;
                        }
                        // Agentmaster (conversation lineage): reveal the previous-session toggle ONLY when this
                        // (freshly analyzed) session actually has a previous (pre-/compact OR cross-file) segment;
                        // hide it otherwise. Only touched on a real re-analyze, so a quiet reload keeps the last state.
                        if (self->_summaryPrevBtn)
                        {
                            self->_summaryPrevBtn.Visibility(hasPrevious ? winrt::Windows::UI::Xaml::Visibility::Visible
                                                                         : winrt::Windows::UI::Xaml::Visibility::Collapsed);
                        }
                    }
                    if (!text.empty())
                    {
                        self->_SetSummaryContent(text); // text runs -> TextBlocks, sentinels -> full-width rules
                    }
                    if (timesComputed)
                    {
                        self->_summaryCreatedMs = createdMs; // refresh the times-line instants (the ticker re-renders the deltas)
                        self->_summaryLastUserMs = lastUserMs;
                        self->_summaryLastActivityMs = lastActivityMs;
                    }
                    if (cachePath)
                    {
                        self->_summaryPath = path; // cache the resolved path for the next reload
                    }
                    // else: a never-messaged fork rendered from its PARENT's transcript — leave _summaryPath
                    // empty so the next reload re-resolves the fork's OWN id and switches the instant it exists.
                    self->_summaryMtime = mtime;
                    self->_summaryLoading = false;
                    self->_UpdateTimesLine(); // reflect the (possibly refreshed) instants right away
                    self->_EnsureSummaryPlaceholder(); // a no-transcript session yields NO text/times — show the empty-state line instead of an invisible (dead-looking) toggle
                    const bool flagDirty = self->_summaryWrapDirty || self->_summaryTruncateDirty || self->_summaryPrevDirty;
                    if (flagDirty || self->_summaryReloadPending)
                    {
                        // Re-run the load now. Two reasons can land here:
                        //  • flagDirty: a wrap / truncate / show-previous toggle flipped mid-load, so this render
                        //    used the OLD flag(s) — force a full re-render (invalidate the mtime gate).
                        //  • _summaryReloadPending: a content refresh was requested while this load was in flight
                        //    (the transcript may have grown) — re-check WITHOUT forcing, so the mtime gate makes a
                        //    no-growth re-check a cheap no-op while real growth is picked up. This closes the
                        //    "dropped final refresh" gap (a turn's closing message arriving after the last notify).
                        self->_summaryWrapDirty = false;
                        self->_summaryTruncateDirty = false;
                        self->_summaryPrevDirty = false;
                        self->_summaryReloadPending = false;
                        if (flagDirty)
                        {
                            self->_summaryMtime = 0; // force re-render with the new flags
                        }
                        if (self->_summaryEnabled && self->_registry && !self->_sessionId.empty())
                        {
                            if (const auto reinfo = self->_registry->Get(self->_sessionId))
                            {
                                self->_UpdateSummary(*reinfo);
                            }
                        }
                    }
                    }
                    catch (...)
                    {
                        self->_summaryLoading = false; // never wedge the panel on a failed publish
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[summary] contained _LoadSummaryAsync publish throw (no crash)\n");
                    }
                }
            });
        }
        }
        catch (...)
        {
            // The completion never got posted (a capture-copy bad_alloc, a dispatcher throw) — log and
            // best-effort clear the loading latch from the UI thread so the panel can load again.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[summary] contained _LoadSummaryAsync throw (no crash)\n");
            try
            {
                if (auto disp = _dispatcher)
                {
                    disp.TryEnqueue([weak = get_weak()]() {
                        if (auto self = weak.get())
                        {
                            self->_summaryLoading = false;
                        }
                    });
                }
            }
            catch (...)
            {
            }
        }
    }
}
