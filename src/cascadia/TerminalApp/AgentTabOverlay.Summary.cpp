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
// ★ AgentTabOverlay.Summary.cpp  - the pencil-toggled summary panel: build/render/load off-thread, the times bar, resize grips, wrap/truncate/whole-conversation, JUMP, session separators, copy-summary
// ======================================================================================
//
// Agentmaster per-tab overlay: the pencil-toggled SUMMARY PANEL -- build/render/load (off-thread), the times bar, resize grips, wrap/truncate/whole-conversation toggles, the conversation list (every prompt of the session's whole conversation chain, numbered as one, with clickable session separators), JUMP eligibility + highlight, and the copy-summary action. Partial TU of AgentTabOverlay.cpp.
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

        // Agentmaster (the summary's conversation chain): the WHOLE-CONVERSATION toggle, LEFTMOST in the
        // strip (left of truncate). Collapsed until a load finds earlier history to show — an earlier session
        // this one continues (a fork's parent, a plan-restart parent, a /clear predecessor, a /handover
        // origin) or a part a /compact summarized away; with nothing earlier the toggle would change
        // nothing. ON (the default) lists that history above the current part, numbered 1..N as ONE
        // conversation with a session-id separator between sessions; OFF lists only the part after the last
        // compaction. A GLOBAL setting (AppSettings::summaryPanelShowHistory) flipped via the page
        // (_onToggleSummaryHistory) so it broadcasts to every linked overlay. Same glyph size + scale as the
        // wrap/truncate toggles.
        _summaryHistoryIcon = FontIcon{};
        _summaryHistoryIcon.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // carries U+23EE (the previous-track double-bar glyph)
        _summaryHistoryIcon.Glyph(L"\x23EE"); // ⏮ — "from the beginning" (the earlier sessions + compacted parts)
        _summaryHistoryIcon.FontSize(11);
        _summaryHistoryIcon.FontWeight(FontWeights::SemiBold());
        _summaryHistoryIcon.IsHitTestVisible(false); // CLICK-THROUGH: glyph clicks land on _summaryHistoryBtn (see the action-row mkIconBtn note)
        {
            ScaleTransform historyScale{};
            historyScale.ScaleX(1.18);
            historyScale.ScaleY(1.18);
            _summaryHistoryIcon.RenderTransform(historyScale);
            _summaryHistoryIcon.RenderTransformOrigin(Point{ 0.5f, 0.5f });
        }
        _summaryHistoryBtn = Button{};
        _summaryHistoryBtn.Background(Fill(0x00, 0, 0, 0)); // transparent — still hit-testable + hover highlight
        _summaryHistoryBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        _summaryHistoryBtn.Padding(ThicknessHelper::FromLengths(3, 0, 1, 0));
        _summaryHistoryBtn.MinWidth(0);
        _summaryHistoryBtn.MinHeight(0);
        _summaryHistoryBtn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        _summaryHistoryBtn.VerticalAlignment(VerticalAlignment::Top);
        _summaryHistoryBtn.HorizontalAlignment(HorizontalAlignment::Right);
        _summaryHistoryBtn.Content(_summaryHistoryIcon);
        _summaryHistoryBtn.Visibility(Visibility::Collapsed); // shown only when the loaded summary HAS earlier history
        AgentSetTip(_summaryHistoryBtn, winrt::hstring{
            L"Whole conversation: list the earlier sessions this one continues (a fork, a plan-mode restart, "
            L"/clear, /handover) and the parts a /compact summarized, numbered as one conversation with a "
            L"session-id line between sessions. Off: only the part after the last compaction." });
        _summaryHistoryBtn.Click([weak = get_weak()](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummaryHistory();
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
        toggles.Children().Append(_summaryHistoryBtn); // leftmost: whole conversation (shown only when there IS earlier history)
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
        _UpdateSummaryHistoryButtonVisual(); // seed the whole-conversation toggle's color from _summaryShowHistory (default: lighter/on)

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
        // Agentmaster (SIDE PANEL, fill-host mode): the grips are NEVER mounted when the HOST owns
        // sizing (SetSummaryFillHost) — a drag would fight the host's own splitters and its release
        // would persist bogus per-tab GLOBAL fractions. The (cheap) grip objects above are simply
        // dropped unrooted; keeping the construction unconditional keeps this diff — and the classic
        // per-tab path — byte-identical in behavior.
        if (!_summaryFillHost)
        {
            layout.Children().Append(leftGrip);
            layout.Children().Append(bottomGrip);
            layout.Children().Append(cornerGrip); // last == on top, so the corner wins over the edge grips
        }

        _summaryRoot = Border{};
        // FULLY OPAQUE, matching the badge (_root) — its see-through-ness is owned SOLELY by
        // _summaryRoot.Opacity (the same GLOBAL "Overlay opacity" setting). A translucent fill here
        // (was 0xE6) would multiply with that Opacity, so "100%" could never be truly opaque.
        // This fill + border + rounding is the FLOATING look of the per-tab panel over a terminal; the
        // fill-host block below removes all of it for the side panel, where the panel is not floating.
        _summaryRoot.Background(Fill(0xFF, 0x20, 0x20, 0x20));
        _summaryRoot.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _summaryRoot.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _summaryRoot.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        // Padding now lives on contentBorder (so the grips reach the panel edges).
        _summaryRoot.Child(layout);
        _summaryRoot.Visibility(Visibility::Collapsed); // shown only while the GLOBAL showSummaryPanel is ON
        _summaryRoot.Opacity(_restOpacity); // dim at rest — same value as the badge (_root.Opacity)
        // Agentmaster (SIDE PANEL, fill-host mode): stretch across the host slot's width; TOP-aligned
        // vertically (NOT Stretch) so the root keeps hugging its content — the fill branch of
        // _ApplySummarySize derives the panel's own chrome height as rootActual - scrollActual, which
        // only holds while the root is content-sized. The height is governed by the scroll viewport's
        // MaxHeight (pushed host height minus that chrome). Re-applies are driven by the root's OWN
        // SizeChanged — the moment the title/times rows appear the chrome grows and the viewport cap
        // must shrink — but DEFERRED to a clean dispatcher tick (coalesced by _summaryFillReapplyQueued):
        // mutating MaxHeight synchronously inside a layout pass risks XAML's E_LAYOUTCYCLE fail-fast
        // (the tag-badge lesson), while the deferred apply is change-gated so it converges.
        if (_summaryFillHost)
        {
            // NO FRAME. The look set above (a 1px 25%-white border, 4px rounded corners, its own #202020
            // fill) marks the per-tab panel as a card FLOATING over a terminal. Here the panel is encased
            // in the side panel's lower area, so that frame drew a card inside the pane. Remove all three:
            //  - no border and square corners — the area's own edges are the panel's edges;
            //  - a TRANSPARENT fill, so the host's background shows through and the summary reads as the
            //    area's content. Transparent, NOT a null brush: a Border without a background is not
            //    hit-testable in its empty parts, and the right-click menu wired on _summaryRoot ("Copy
            //    Summary", "Show Transcript", the view toggles) must still open on the gaps between rows.
            // Square corners also remove the rounded-corner clip, for which XAML gives a Border with a
            // child its own composition node — one node fewer in the per-frame walk (UI_THREAD.md §10h).
            _summaryRoot.Background(Fill(0x00, 0, 0, 0));
            _summaryRoot.BorderThickness(ThicknessHelper::FromUniformLength(0));
            _summaryRoot.CornerRadius(CornerRadiusHelper::FromUniformRadius(0));
            _summaryRoot.HorizontalAlignment(HorizontalAlignment::Stretch);
            _summaryRoot.VerticalAlignment(VerticalAlignment::Top);
            _summaryRoot.Margin(ThicknessHelper::FromUniformLength(0));
            _summaryRoot.SizeChanged([weak](const IInspectable&, const SizeChangedEventArgs&) {
                const auto self = weak.get();
                if (!self || self->_summaryFillReapplyQueued)
                {
                    return; // gone, or a re-apply tick is already queued — this change folds into it
                }
                self->_summaryFillReapplyQueued = true;
                if (self->_dispatcher)
                {
                    self->_dispatcher.TryEnqueue([weak]() {
                        if (const auto s2 = weak.get())
                        {
                            s2->_summaryFillReapplyQueued = false;
                            s2->_ApplySummarySize(); // change-gated inside — an unchanged geometry is a no-op
                        }
                    });
                }
                else
                {
                    self->_summaryFillReapplyQueued = false; // no dispatcher (teardown) — drop, the next host push re-applies
                }
            });
        }
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
            // text as it was typed (_summaryPromptRows[k].text, straight off the transcript — an earlier
            // session's prompt included, since the conversation list spans the whole chain), never the
            // panel's RENDERING of it, which escapes newlines to a literal "\n" while wrap is off and caps
            // the message while truncate is on. Sits directly ABOVE "Copy Summary": both are copies, this
            // the narrower/more specific one, so it reads first. Shown ONLY when the right-click landed on a
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

            // Agentmaster (transcript overlay): "Show Transcript" — the WHOLE conversation behind this
            // summary, rendered as Markdown in the window-level reading view (AgentTranscriptOverlay.h).
            // The page owns that view, so this only fires the page-wired opener (inert when unwired).
            MenuFlyoutItem showTranscriptItem{};
            showTranscriptItem.Text(kShowTranscriptMenuLabel);
            AgentSetTip(showTranscriptItem, winrt::hstring{ kShowTranscriptMenuTip });
            showTranscriptItem.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                // Contained: a XAML event handler — an escape would be a process fail-fast for a menu click.
                try
                {
                    if (const auto self = weak.get(); self && self->_onShowTranscript)
                    {
                        self->_onShowTranscript();
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay summary Show Transcript");
                }
            });
            _summaryContextMenu.Items().Append(showTranscriptItem);

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
                    // the live row list, so a stale index from an earlier render can never copy anything. The
                    // row index IS the displayed number minus one: the k-th numbered line reads " k+1. ",
                    // continuous across the sessions of the chain.
                    pendingPrompt->clear();
                    const int promptIdx = self->_ctxPromptIndex;
                    if (promptIdx >= 0 && promptIdx < static_cast<int>(self->_summaryPromptRows.size()))
                    {
                        *pendingPrompt = self->_summaryPromptRows[promptIdx].text;
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
        // Agentmaster (SIDE PANEL): FILL-THE-HOST branch — the host slot (pushed via OnSummaryPaneSize)
        // sizes the panel; the global width/height fractions and their caps are deliberately ignored
        // (a 50%-of-pane cap makes sense floating over a terminal, not filling a dedicated slot).
        // Width: MaxWidth = the host width (the root stretches to it; MaxWidth still bounds a wide
        // unwrappable line). Height: the body viewport's MaxHeight = host height minus the panel's own
        // CHROME (title + times bar + padding; this mode draws no border), measured as rootActual - scrollActual — valid
        // because the fill root stays TOP-aligned/content-hugging (see _BuildSummaryPanel). Before the
        // first real layout both actuals are 0, so a conservative fallback stands in; the root's
        // deferred SizeChanged re-apply then corrects it. Every write is CHANGE-GATED (>0.5px delta)
        // so the SizeChanged -> re-apply loop converges instead of cycling (`forced` never happens
        // here — the grips that pass it are not built in fill mode).
        if (_summaryFillHost)
        {
            if (_summaryPaneW > 1.0 && std::abs(_summaryRoot.MaxWidth() - _summaryPaneW) > 0.5)
            {
                _summaryRoot.MaxWidth(_summaryPaneW);
            }
            _summaryRoot.Width(NAN); // always content/host-driven — never the fraction pin
            _summaryRoot.Height(NAN);
            if (_summaryScroll && _summaryPaneH > 1.0)
            {
                _summaryScroll.Height(NAN);
                const double rootH = _summaryRoot.ActualHeight();
                const double scrollH = _summaryScroll.ActualHeight();
                // Chrome = everything of the panel that is NOT the scrolling body. The subtraction is
                // only meaningful once both are laid out AND sane; else assume ~64px (title 18 + times
                // 17 + paddings 12 + margins, with room to spare; this mode draws no border) — corrected
                // by the next SizeChanged re-apply.
                double chrome = (rootH > 0.0 && scrollH > 0.0 && rootH > scrollH && rootH - scrollH < _summaryPaneH) ? (rootH - scrollH) : 64.0;
                const double maxH = std::max(40.0, _summaryPaneH - chrome);
                if (std::abs(_summaryScroll.MaxHeight() - maxH) > 0.5)
                {
                    _summaryScroll.MaxHeight(maxH);
                }
            }
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

    void AgentTabOverlay::SetSummaryFillHost()
    {
        // Agentmaster (SIDE PANEL): one-way, pre-Initialize switch (see the header doc — the grips and
        // the root alignment are decided when _BuildSummaryPanel constructs the tree, so flipping this
        // after Initialize would only change the sizing math, leaving anchored alignment + live grips).
        // No un-set exists on purpose: an overlay's layout mode is fixed for its lifetime.
        _summaryFillHost = true;
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
    // a session-separator line (it opens with the kSessionSepMark control character, not a letter). The
    // section a list header opens renders its "* " items single-line + clipped instead of wrapped (see
    // _SetSummaryContent).
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

    // Agentmaster (the summary's conversation chain): do two row lists describe the same render — every
    // field of every row, in order? With the rendered text, this decides whether a reload may skip the
    // panel rebuild: the rows carry what the text cannot show (a prompt's ▸ jump index, a separator's
    // title + continuation for its hover text), so equal text alone is not enough.
    //   a, b — the two row lists (prompt rows or separator rows).
    // Returns true when they match field by field. Never throws.
    static bool SameSummaryRows(const std::vector<SummaryConversationRow>& a, const std::vector<SummaryConversationRow>& b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t k = 0; k < a.size(); ++k)
        {
            if (a[k].headIndex != b[k].headIndex || a[k].text != b[k].text || a[k].sessionId != b[k].sessionId ||
                a[k].sessionTitle != b[k].sessionTitle || a[k].continuedBy != b[k].continuedBy)
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
        // Agentmaster (the summary's conversation chain): forget what the body showed until the build below
        // COMPLETES — a build that throws half-way must not let the next identical load skip its rebuild
        // (the publish compares against this) and strand a half-built body.
        _summaryRenderedText.clear();
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
        // Agentmaster (the summary's conversation chain): will ANY prompt row of this render carry a ▸ jump
        // button? Only then do the rows reserve the button column (see makePromptRow) — a render with no
        // jump target at all (a Codex session, no page jump handler) keeps the plain look.
        bool anyJump = false;
        if (_onJumpToPrompt)
        {
            for (const auto& row : _summaryPromptRows)
            {
                if (row.headIndex >= 0 && row.headIndex < static_cast<int>(_summaryUserMsgs.size()))
                {
                    anyJump = true;
                    break;
                }
            }
        }
        // Agentmaster (SUMMARY_JUMP.md + the conversation chain): a numbered prompt renders as a 2-column
        // Grid — a JUMP button (col 0, auto) + the wrapping message text (col 1, *) — so the text still
        // wraps within the panel while the button stays put. The button asks the page to center the
        // session's terminal view on where this prompt is rendered (a chime confirms a hit). Only a prompt
        // the CURRENT session's terminal can show gets one: its row's headIndex, an index into
        // _summaryUserMsgs (the list the jump resolver reads). An EARLIER session's prompt (or a part
        // before this session's last /compact) gets an INVISIBLE stand-in of the button's exact size
        // instead, so every prompt's text starts at the same x and the whole list reads as ONE
        // conversation. When no row of the render can jump (anyJump false) the column is left out.
        //   lineStr  — the rendered " N. <text>" line (its first line, when wrap keeps real newlines).
        //   rowIndex — the row's index in _summaryPromptRows (== N - 1) — what this row's "Copy Prompt" copies.
        // Returns the row element. Throws only what XAML element creation throws (the caller is on the UI thread).
        const auto makePromptRow = [this, anyJump](const std::wstring& lineStr, int rowIndex) -> winrt::Windows::UI::Xaml::UIElement {
            const SummaryConversationRow& row = _summaryPromptRows[static_cast<size_t>(rowIndex)];
            // The jump target: the row's headIndex while the page can jump and the index is in the live list.
            const int idx = (_onJumpToPrompt && row.headIndex >= 0 && row.headIndex < static_cast<int>(_summaryUserMsgs.size())) ? row.headIndex : -1;
            Grid g{};
            // A transparent fill makes the WHOLE row (its gutter included, not only the glyphs) the
            // right-click target of this row's "Copy Prompt"; a jumped-to row swaps it for the accent
            // band (_ApplySummaryHighlight).
            g.Background(Fill(0, 0, 0, 0));
            if (anyJump)
            {
                ColumnDefinition c0{};
                c0.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
                g.ColumnDefinitions().Append(c0);
            }
            ColumnDefinition c1{};
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            g.ColumnDefinitions().Append(c1);

            if (idx >= 0)
            {
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
            }
            else if (anyJump)
            {
                // The INVISIBLE stand-in: the jump button's own glyph + right padding at opacity 0, so this
                // column measures exactly what a real button's does (whatever the font's ▸ advance is) and
                // the text lines up with its neighbors'. Opacity 0 (not Collapsed) keeps it measured AND
                // hit-testable: a right-click on the gutter still bubbles to the row's "Copy Prompt".
                FontIcon ghost{};
                ghost.FontFamily(FontFamily{ L"Segoe UI Symbol" });
                ghost.Glyph(L"\x25B8");
                ghost.FontSize(10);
                ghost.VerticalAlignment(VerticalAlignment::Top);
                Border gutter{};
                gutter.Padding(ThicknessHelper::FromLengths(0, 0, 4, 0)); // the jump button's padding: the same 4-px gap to the text
                gutter.Opacity(0.0);
                gutter.Child(ghost);
                Grid::SetColumn(gutter, 0);
                g.Children().Append(gutter);
            }

            TextBlock tb{};
            tb.FontFamily(FontFamily{ L"Cascadia Mono" });
            tb.FontSize(11);
            tb.TextWrapping(TextWrapping::Wrap);
            tb.IsTextSelectionEnabled(true);
            tb.Foreground(Fill(0xFF, 0xDC, 0xDC, 0xDC));
            tb.Text(winrt::hstring{ lineStr });
            Grid::SetColumn(tb, anyJump ? 1 : 0);
            g.Children().Append(tb);
            // Right-click anywhere on THIS row (its text, its gutter, or the ▸ button, which bubbles to
            // the row) => the shared menu WITH "Copy Prompt" bound to this row's prompt. Wire the text
            // block explicitly: a selectable TextBlock shadows its parent's menu, so the row Grid alone
            // would never see the request.
            _WireSummaryContextMenu(tb, rowIndex);
            _WireSummaryContextMenu(g, rowIndex);
            if (idx >= 0)
            {
                _summaryMsgRows.emplace_back(idx, g); // register the row so HighlightSummaryMessage can band it
            }
            return g;
        };
        size_t i = 0;
        // Agentmaster (the summary's conversation chain): the rendered lines are matched back to the rows
        // they were rendered from, IN ORDER — RenderSummaryBox writes one " N. " line per prompt row and one
        // kSessionSepMark line per separator row, both in row order:
        //  * promptCursor — the next prompt row (0-based). The numbers count 1..N continuously across the
        //    sessions of the chain, so a line is that row's START only when its number is exactly
        //    promptCursor + 1: in wrap-ON mode a multi-line prompt's continuation line that merely LOOKS
        //    like "2. foo" can never steal a row (and mis-map "Copy Prompt" / the ▸ jump).
        //  * sepCursor — the next separator row. A kSessionSepMark line is that separator only when it
        //    carries the row's id, so a prompt's own text can never pose as one.
        int promptCursor = 0;
        size_t sepCursor = 0;
        // Agentmaster: the section's first line is a bare "<Label>:" LIST header (IsSummaryListHeader —
        // Files Created / Edited / Read, Skills Loaded, …): its "* " item lines render as a NoWrap +
        // clipped run instead of wrapping. Decided by the section's FIRST line after each rule, so a "* "
        // bullet inside a wrapped message or the Recap can never be mistaken for a list item.
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
                // A section's FIRST line names its kind: a bare "<Label>:" header opens a "* item" list
                // section; anything else is a normal section.
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
                sectionIsList = false; // a new section opens below this rule; the next line names its kind
                sectionKindPending = true;
            }
            else if (!lineStr.empty() && lineStr[0] == kSessionSepMark && sepCursor < _summarySeparatorRows.size() &&
                     lineStr.compare(1, std::wstring::npos, _summarySeparatorRows[sepCursor].sessionId) == 0)
            {
                flushSeg(); // close the run above: the separator follows the earlier session's last prompt
                _summaryStack.Children().Append(_MakeSessionSeparator(_summarySeparatorRows[sepCursor]));
                ++sepCursor;
            }
            else if (promptCursor < static_cast<int>(_summaryPromptRows.size()) && ParseSummaryMsgIndex(lineStr) == promptCursor)
            {
                flushSeg(); // close the run above this numbered prompt; render it as its own row
                _summaryStack.Children().Append(makePromptRow(lineStr, promptCursor));
                ++promptCursor; // the next prompt-start must be the following number
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
        _summaryRenderedText = text; // the body is complete: an identical later reload may skip the rebuild
    }

    // Agentmaster (the summary's conversation chain): the clickable SESSION SEPARATOR (see the declaration).
    // Drawn as a rule — a short dash, the id's first 8 characters, a dash to the right edge — in the panel's
    // own rule + dim-text colors, so it reads as a mark INSIDE one conversation rather than a new section.
    // It is a Button with the jump buttons' recipe (a transparent but hit-testable fill whose stock
    // hover/press highlight is the "clickable" cue; never a tab stop — keyboard focus stays on the ConPTY).
    winrt::Windows::UI::Xaml::UIElement AgentTabOverlay::_MakeSessionSeparator(const SummaryConversationRow& row)
    {
        const std::wstring id = row.sessionId;
        const std::wstring shortId = id.substr(0, std::min<size_t>(8, id.size()));

        // The hover text: whose conversation it is up to here, how the session below continued it, and
        // what a click does. A via the chain did not name ("" or an unexpected token) reads neutrally.
        std::wstring how = L"The session below continues it.";
        if (row.continuedBy == L"fork")
        {
            how = L"The session below is a fork of it.";
        }
        else if (row.continuedBy == L"plan")
        {
            how = L"The session below restarted it from plan mode.";
        }
        else if (row.continuedBy == L"clear")
        {
            how = L"The session below continued it after /clear.";
        }
        else if (row.continuedBy == L"handover")
        {
            how = L"The session below took it over with /handover.";
        }
        std::wstring hover = L"Session " + id;
        if (!row.sessionTitle.empty())
        {
            hover += L"\n\x201C" + row.sessionTitle + L"\x201D";
        }
        hover += L"\nThe conversation up to here is this session. " + how;
        hover += L"\nClick to copy its session id.";
        const winrt::hstring hoverTip{ hover };

        // [10-px dash][id][dash to the edge] — the dashes are 1-px rules centered on the id's line.
        Grid g{};
        {
            ColumnDefinition lead{};
            lead.Width(GridLengthHelper::FromPixels(10));
            ColumnDefinition label{};
            label.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition rest{};
            rest.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            g.ColumnDefinitions().Append(lead);
            g.ColumnDefinitions().Append(label);
            g.ColumnDefinitions().Append(rest);
        }
        for (const int column : { 0, 2 })
        {
            Border dash{};
            dash.Height(1);
            dash.VerticalAlignment(VerticalAlignment::Center);
            dash.Background(Fill(0x40, 0xFF, 0xFF, 0xFF)); // the section rules' color
            Grid::SetColumn(dash, column);
            g.Children().Append(dash);
        }
        TextBlock idText{};
        idText.FontFamily(FontFamily{ L"Cascadia Mono" });
        idText.FontSize(10);
        idText.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // dim: a marker, not content
        idText.Margin(ThicknessHelper::FromLengths(4, 0, 4, 0));
        idText.VerticalAlignment(VerticalAlignment::Center);
        idText.Text(winrt::hstring{ shortId });
        Grid::SetColumn(idText, 1);
        g.Children().Append(idText);

        Button btn{};
        btn.Background(Fill(0, 0, 0, 0)); // a TRANSPARENT brush (not null): the whole rule is hit-testable
        btn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        btn.Padding(ThicknessHelper::FromUniformLength(0));
        btn.MinWidth(0);
        btn.MinHeight(0);
        btn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        btn.HorizontalAlignment(HorizontalAlignment::Stretch); // border to border, like the section rules
        btn.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        btn.VerticalContentAlignment(VerticalAlignment::Center);
        btn.Margin(ThicknessHelper::FromLengths(0, 3, 0, 3));
        btn.Content(g);
        AgentSetTip(btn, hoverTip);
        btn.Click([weak = get_weak(), id](const IInspectable& sender, const RoutedEventArgs&) {
            // CONTAINED: a XAML event handler — an escape is a stowed-exception fail-fast of the whole app.
            try
            {
                // The bool-returning writer (not CopyTextToClipboard, which only chimes) so the tooltip can
                // tell the truth: a copy that never landed (the clipboard held by another app past the
                // writer's retries) says so instead of claiming success.
                const bool copied = RobustCopyTextToClipboard(id);
                if (copied)
                {
                    PlayActionSound(); // the same "copy is done" chime every other copy plays
                }
                if (const auto el = sender.try_as<UIElement>())
                {
                    AgentShowTipNow(el, copied ? winrt::hstring{ L"Copied session id of up to here" } :
                                                 winrt::hstring{ L"Could not copy the session id: another app holds the clipboard. Click again." });
                }
                if (const auto self = weak.get())
                {
                    ::Agentmaster::LogNav(L"copy summary-session-id " + ::Agentmaster::ShortId(self->_sessionId) + L" -> " + id +
                                          (copied ? L"" : L" (clipboard busy, not copied)"));
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay summary session separator copy");
            }
        });
        // After a click the tip text is the confirmation; leaving gives the NEXT hover its description back
        // (AgentSetTip's own PointerExited, attached first above, has already closed the open tip).
        btn.PointerExited([hoverTip](const IInspectable& sender, const PointerRoutedEventArgs&) {
            if (const auto el = sender.try_as<UIElement>())
            {
                AgentSetTip(el, hoverTip);
            }
        });
        _WireSummaryContextMenu(btn, -1); // right-click => the shared menu (no "Copy Prompt": not a prompt)
        return btn;
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

    void AgentTabOverlay::SetShowTranscriptHandler(std::function<void()> handler)
    {
        _onShowTranscript = std::move(handler);
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
        _summaryRenderedText.clear(); // "rebuild this panel now": never skip the rebuild, even for identical content
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

    void AgentTabOverlay::_ToggleSummaryHistory()
    {
        // The ⏮ icon flips the GLOBAL setting (AppSettings::summaryPanelShowHistory), not per-session
        // state: hand off to the page, which does the freshest-disk read-modify-write of settings.json AND
        // applies it live to every linked overlay in the window (SetSummaryShowHistory).
        if (_onToggleSummaryHistory)
        {
            _onToggleSummaryHistory();
        }
    }

    // Recolor the ⏮ icon to reflect the toggle: a dim gray when OFF (only the part after the last
    // compaction), a clearly lighter shade when ON (the whole conversation) — matching the wrap/truncate
    // toggles.
    void AgentTabOverlay::_UpdateSummaryHistoryButtonVisual()
    {
        if (!_summaryHistoryIcon)
        {
            return;
        }
        _summaryHistoryIcon.Foreground(_summaryShowHistory ? Fill(0xFF, 0xE6, 0xE6, 0xE6)  // ON: lighter (active)
                                                           : Fill(0xFF, 0x8C, 0x8C, 0x8C)); // OFF: dim (inactive)
    }

    // Recolor the badge's PENCIL (the summary-panel toggle in the row-1 action strip) to reflect the
    // GLOBAL showSummaryPanel — dim when OFF, lighter when ON, the same state language as the panel's
    // wrap/truncate/whole-conversation toggles. The pencil was the ONE toggle in the overlay with NO state
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

    void AgentTabOverlay::SetSummaryShowHistory(bool on)
    {
        // The whole-conversation mode is a GLOBAL setting (AppSettings::summaryPanelShowHistory), mirrored
        // into the overlay here by the page — on attach (seed) and on every toggle (broadcast to every
        // linked overlay in the window). The flag decides which rows are rendered (BuildSummaryConversation),
        // so a real change must force a re-analyze+render: reset the mtime gate and re-pull from the
        // freshest snapshot (the earlier sessions come from CollectConversationUpstream's process-wide
        // cache, so turning it back on re-reads nothing). Always refresh the icon color (the seed may match
        // the default but still needs painting).
        const bool changed = (_summaryShowHistory != on);
        _summaryShowHistory = on;
        _UpdateSummaryHistoryButtonVisual();
        if (!changed)
        {
            return; // seed with the same value — nothing rendered differently, no reload
        }
        _summaryMtime = 0; // invalidate the mtime gate so _LoadSummaryAsync re-renders with the new flag
        if (_summaryLoading)
        {
            _summaryHistoryDirty = true; // a load is in flight with the OLD flag; re-render on completion (else the toggle wouldn't take until the transcript next grew)
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

    void AgentTabOverlay::SetSummaryHistoryToggleHandler(std::function<void()> handler)
    {
        _onToggleSummaryHistory = std::move(handler);
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
        // The earlier sessions need no memo here: CollectConversationUpstream caches them process-wide per
        // transcript (+ its recorded link), so a quiet reload or a toggle re-reads nothing.
        _LoadSummaryAsync(_summaryPath, codex, convId, std::move(forkParent), std::move(cwd), std::move(liveGlyph), std::move(liveLabel), _summaryMtime, _summaryWrapNewlines, _summaryTruncate, _summaryShowHistory, _registry);
    }

    winrt::fire_and_forget AgentTabOverlay::_LoadSummaryAsync(std::wstring transcriptPath, bool codex, std::wstring sessionId, std::wstring forkParentId, std::wstring cwd, std::wstring liveGlyph, std::wstring liveLabel, int64_t prevMtime, bool wrapNewlines, bool truncate, bool showHistory, std::shared_ptr<::Agentmaster::SessionRegistry> registry)
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
        std::vector<std::wstring> userMsgs; // Agentmaster (SUMMARY_JUMP.md): the CURRENT session's prompts — the ▸ jump list a row's headIndex indexes
        // Agentmaster (the summary's conversation chain): the rows `text` was rendered from — prompts and
        // session separators, in display order — published with it, so the panel can match each rendered
        // line back to its row (Copy Prompt's text, the ▸ jump index, a separator's id + hover text).
        std::vector<SummaryConversationRow> rows;
        int64_t mtime = prevMtime;
        int64_t createdMs = 0, lastUserMs = 0, lastActivityMs = 0; // times-line instants (computed on reload)
        bool timesComputed = false;
        bool hasHistory = false; // Agentmaster (the summary's conversation chain): there IS earlier history to list (an earlier session of the chain, or a part before a /compact) => reveal the ⏮ toggle
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
                    rows = ::Agentmaster::CodexSummaryConversation(info.userPrompts); // the rows RenderCodexSummary numbered (Copy Prompt per line; no jump, no chain)
                }
                else
                {
                    auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
                    // Agentmaster (the summary's conversation chain): everything EARLIER in the conversation —
                    // the sessions this one continues (a fork's parent, a plan-restart parent, a /clear
                    // predecessor, a /handover origin), walked back link by link; never a session that
                    // continued THIS one (that belongs to its own summary). Collected even while the ⏮ toggle
                    // is OFF: whether the toggle is shown at all depends on it, and the walk is cached
                    // process-wide per transcript (+ its recorded link), so a reload costs a stat + a lookup
                    // and an ordinary session pays one head read, once. The /clear and /handover links are
                    // not in any transcript — Agentmaster recorded them on the session records, which the
                    // lookup reads.
                    const auto upstream = ::Agentmaster::CollectConversationUpstream(std::filesystem::path{ path }, MakeRecordedPredecessorLookup(registry));
                    hasHistory = !upstream.segments.empty() || !a.previousSegments.empty();
                    rows = ::Agentmaster::BuildSummaryConversation(a, upstream, showHistory, [&registry](const std::wstring& sid) {
                        return SummarySessionTitleOf(registry, sid);
                    });
                    userMsgs = a.userMsgs; // the current part's prompts, in order — the list each row's headIndex indexes
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
                    text = RenderSummaryBox(a, sessionId, cwd, path, L"", liveGlyph, liveLabel, planFile, /*full*/ false, wrapNewlines, truncate, rows);
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
                rows.clear();
                timesComputed = false;
                hasHistory = false;
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[summary] contained _LoadSummaryAsync analyze throw (no crash)\n");
            }
        }

        // Hop back to the UI thread to publish (the StackPanel build + member writes are UI-thread only).
        if (auto disp = _dispatcher)
        {
            disp.TryEnqueue([weak = get_weak(), text, userMsgs, rows, path, cachePath, mtime, createdMs, lastUserMs, lastActivityMs, timesComputed, hasHistory]() {
                if (auto self = weak.get())
                {
                    // Agentmaster (contained): a XAML throw in this dispatcher callback would fail-fast
                    // the app (0xC000027B stowed exception) — contain it and keep _summaryLoading sane.
                    try
                    {
                    bool sameRows = false; // this load's rows equal the ones the current body was built from
                    if (timesComputed)
                    {
                        // Agentmaster (the summary's conversation chain): split the rows by kind, each kept in
                        // display order — _SetSummaryContent matches the k-th numbered line to promptRows[k]
                        // and the j-th session-separator line to sepRows[j]. Both, like the jump list, are set
                        // BEFORE _SetSummaryContent so every rebuilt row resolves the right prompt.
                        std::vector<SummaryConversationRow> promptRows;
                        std::vector<SummaryConversationRow> sepRows;
                        for (const auto& row : rows)
                        {
                            (row.sessionId.empty() ? promptRows : sepRows).push_back(row);
                        }
                        sameRows = SameSummaryRows(promptRows, self->_summaryPromptRows) && SameSummaryRows(sepRows, self->_summarySeparatorRows);
                        self->_summaryUserMsgs = userMsgs;
                        self->_summaryPromptRows = std::move(promptRows);
                        self->_summarySeparatorRows = std::move(sepRows);
                        // Reveal the ⏮ toggle ONLY when there IS earlier history to list or hide (an earlier
                        // session of the chain, or a part before a /compact) — with none it would change
                        // nothing. Only touched on a real re-analyze, so a quiet reload keeps the last state.
                        if (self->_summaryHistoryBtn)
                        {
                            self->_summaryHistoryBtn.Visibility(hasHistory ? winrt::Windows::UI::Xaml::Visibility::Visible
                                                                           : winrt::Windows::UI::Xaml::Visibility::Collapsed);
                        }
                    }
                    if (!text.empty())
                    {
                        // Agentmaster (the summary's conversation chain): skip the rebuild when the body
                        // already shows exactly this — the same text AND the same rows. A reload runs on
                        // every transcript write, most of them assistant-only (the prompts, files and tasks
                        // read the same); with the whole conversation listed the body holds hundreds of rows,
                        // and rebuilding them unchanged costs UI-thread time and resets the panel's scroll.
                        if (!sameRows || text != self->_summaryRenderedText)
                        {
                            self->_SetSummaryContent(text); // text runs -> TextBlocks, sentinels -> full-width rules
                        }
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
                    const bool flagDirty = self->_summaryWrapDirty || self->_summaryTruncateDirty || self->_summaryHistoryDirty;
                    if (flagDirty || self->_summaryReloadPending)
                    {
                        // Re-run the load now. Two reasons can land here:
                        //  • flagDirty: a wrap / truncate / whole-conversation toggle flipped mid-load, so this
                        //    render used the OLD flag(s) — force a full re-render (invalidate the mtime gate).
                        //  • _summaryReloadPending: a content refresh was requested while this load was in flight
                        //    (the transcript may have grown) — re-check WITHOUT forcing, so the mtime gate makes a
                        //    no-growth re-check a cheap no-op while real growth is picked up. This closes the
                        //    "dropped final refresh" gap (a turn's closing message arriving after the last notify).
                        self->_summaryWrapDirty = false;
                        self->_summaryTruncateDirty = false;
                        self->_summaryHistoryDirty = false;
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
                // Rule #18: never lose a swallowed exception. The latch-clear could not be posted either, so
                // the panel stays "loading" until its overlay is rebuilt (the next selection / reattach).
                ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay _LoadSummaryAsync latch-clear post");
            }
        }
    }
}
