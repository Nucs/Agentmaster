// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the per-tab "link badge" overlay (see doc/agentmaster/TAB_OVERLAY.md). A small,
// compact HUD pinned to the top-right of a Claude session's terminal, summarizing the tab <->
// Agentmaster link at a glance. Row 1 is a strip of DISCRETE, individually-tooltipped parts, in order:
// hook-driven status (color-matched to the Triage Board; LEFTMOST), the action buttons (folder / copy
// menu / pencil — ALWAYS shown for a LINKED session), Autorunner mode (Off/Semi/Full — a CLICKABLE button
// that cycles the mode), queued (Pending) count, and link state (surfaced ONLY when NOT linked —
// "observe" / "unlinked"; a linked session shows nothing for link state).
// Row 2: "<root workdir folder>/<branch>" (dim; e.g. myworkdir/feature/issue123) so the session's place +
// branch read at a glance; the label is hidden when there's no dir/branch (and on observe badges). The
// row-1 action buttons (a LINKED session only) are: a folder
// button (Open Path — the working dir via explorer.exe) + a copy button whose menu copies the Session Id
// / Copy Path (working dir) / Copy Branch Name / Claude Launch CLI / Codex Launch CLI (each the REAL full
// command — the live process commandline with hooks, or the builder Agentmaster would use) / Summary (the
// FULL textual session box — everything, even what the displayed panel trims) / Transcript (the whole
// conversation, user + assistant TEXT only) + a PENCIL button. A completed copy (and Open Path) plays a
// short confirmation chime. The buttons sit just to the RIGHT of the status part and are ALWAYS visible
// (no longer hover-only); the whole badge is dim at rest and brightens on hover. Built only for a LINKED
// session (Initialize), never an observe badge. The PENCIL toggles the SUMMARY PANEL — a SECOND overlay (its own slot, below the badge, max
// 20% of the pane) that renders a session-end.js-style box analyzed off-thread from the transcript via
// ProcessInspect::AnalyzeSessionTranscript. A pinned TITLE row (the tab/session name, SessionInfo.title) +
// the live "ago" times line head the panel; below them the DISPLAYED box is a TRIMMED view (Parent/Plan/
// Duration/Tasks/Messages/Files Read/Files Edited) — it omits everything panel 1 (the badge) already shows
// (state, branch, dir/folder); the copy menu's "Summary" yields the COMPLETE box (id +
// resume CLI + dir + folder + branch + state header included). The toggle is a GLOBAL setting
// (AppSettings::showSummaryPanel) — shared across windows + persisted; the pencil on any tab flips every tab's panel.
//
// Built imperatively (no IDL/XAML markup), like AgentManagerContent. It is NOT an IPaneContent —
// it just produces a FrameworkElement the app installs into the pane's overlay slot
// (TerminalPaneContent::SetAgentOverlay). Ref-counted (winrt::implements) so the page can own it
// and the registry observer can get_weak() it; it detaches its observer on teardown (Rule #10).

#pragma once

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.System.h>

#include <functional>
#include <memory>
#include <string>

namespace Agentmaster
{
    class SessionRegistry;
    struct SessionInfo; // _UpdateSummary takes a const& (full type in the .cpp via SessionRegistry.h)
    struct ConversationSegment; // cross-file lineage parents (full type in the .cpp via ProcessInspect.h)
}

namespace winrt::TerminalApp::implementation
{
    class AgentTabOverlay : public winrt::implements<AgentTabOverlay, winrt::Windows::Foundation::IInspectable>
    {
    public:
        AgentTabOverlay();
        ~AgentTabOverlay();

        // Wire to the shared registry + this session's id, render once, and start observing
        // (id-filtered, marshaled to the UI thread). Call on the UI thread.
        void Initialize(const std::wstring& sessionId, std::shared_ptr<::Agentmaster::SessionRegistry> registry);

        // Agentmaster (OBSERVER.md §4/§11d): render a registry-LESS "observe" badge for a tab the
        // Fleet Observer classified but that is NOT a linked Claude session — a shell (kind "pwsh" /
        // "cmd"), a never-prompted claude ("claude", no transcript id yet), or codex. Shows
        // "○ <kind> · unlinked" (gray); no registry observer (there is no session to track). Idempotent
        // by `kind` (a re-render is skipped when unchanged). The real Initialize-bound overlay replaces
        // it once a claude resolves a conversation id (its first prompt).
        void ShowActivity(const std::wstring& kind);

        // The FrameworkElement to install into the pane's overlay slot.
        winrt::Windows::UI::Xaml::FrameworkElement Root() const { return _root; }

        // Agentmaster (TAB_OVERLAY.md summary panel): the FrameworkElement for the SECOND slot (below
        // the badge), installed via TerminalPaneContent::SetAgentSummaryOverlay. Built by Initialize;
        // shown only while the GLOBAL toggle (AppSettings::showSummaryPanel) is ON.
        winrt::Windows::UI::Xaml::FrameworkElement SummaryRoot() const { return _summaryRoot; }

        // Agentmaster (TAB_OVERLAY.md summary panel): the summary panel's visibility is a GLOBAL setting
        // (AppSettings::showSummaryPanel), not per-session. The page mirrors it in here — on attach (seed)
        // and on every pencil toggle (broadcast to every linked overlay in the window). Shows/hides +
        // (re)loads the panel. Call on the UI thread.
        void SetSummaryEnabled(bool on);
        // The pencil button flips the GLOBAL setting; the overlay can't reach AppSettings, so it calls
        // this handler (wired by the page) to do the freshest-disk read-modify-write + the live
        // broadcast. Set by _AttachClaudeOverlay.
        void SetSummaryToggleHandler(std::function<void()> handler);

        // Agentmaster (TAB_OVERLAY.md): the badge + summary panel REST (dim, at rest) and HOVER (bright,
        // on pointer-over / copy-menu-open) opacities are GLOBAL settings (AppSettings::tabOverlayRestOpacity
        // / tabOverlayHoverOpacity), mirrored in by the page — on attach (seed) and on cog Save / cross-
        // window broadcast. Re-applies live to the badge (at its current expanded state) + the summary panel.
        // Enforces rest <= hover defensively. Call on the UI thread.
        void SetOverlayOpacities(double rest, double hover);

        // Agentmaster (inferred working dir): the GLOBAL tab-color MODE (AppSettings::tabColorMode as
        // an int — ::Agentmaster::TabColorMode's numeric values; kept as int so this header stays
        // engine-include-free), mirrored in by the page — on attach (seed) and on cog Save / cross-
        // window broadcast (_ReapplyManagedTabColors). Row 2 (the "<workdir folder>/<branch>" subline),
        // Open Path, and Copy Path resolve the session's EFFECTIVE work dir through it
        // (EffectiveWorkingDir: the INFERRED dir under InferredWorkingDirectory when the scan detected
        // the session working outside its launch cwd, else the persisted workingDir) — so the badge
        // tells the same directory story as the tab color / board card / tree group. A change
        // re-renders a linked badge. Call on the UI thread.
        void SetTabColorMode(int mode);

        // Agentmaster (TAB_OVERLAY.md summary panel): whether the panel preserves a message's real
        // newlines (true) or collapses each message to one line with a literal "\n" (false, the default
        // session-end.js look). A GLOBAL setting (AppSettings::summaryPanelWrapNewlines), mirrored in here
        // by the page — on attach (seed) and on every wrap-toggle (broadcast to every linked overlay in
        // the window). Re-renders the panel (the flag is baked into the rendered text). Call on the UI thread.
        void SetSummaryWrapNewlines(bool on);
        // The wrap-line toggle (right of the times bar) flips the GLOBAL setting; the overlay can't reach
        // AppSettings, so it calls this handler (wired by the page) to do the freshest-disk read-modify-
        // write + the live broadcast. Set by _AttachClaudeOverlay.
        void SetSummaryWrapToggleHandler(std::function<void()> handler);

        // Agentmaster (TAB_OVERLAY.md summary panel): TRUNCATE long messages — a GLOBAL setting
        // (AppSettings::summaryPanelTruncate), mirrored in by the page (seed on attach, broadcast on
        // toggle). OFF shows everything; ON limits each message (6 lines wrapped / 500 chars unwrapped).
        // Re-renders the panel (the flag is baked into the rendered text). Call on the UI thread.
        void SetSummaryTruncate(bool on);
        // The truncate toggle (left of the wrap toggle) flips the GLOBAL setting; the overlay can't reach
        // AppSettings, so it calls this handler (wired by the page) for the freshest-disk RMW + broadcast.
        void SetSummaryTruncateToggleHandler(std::function<void()> handler);

        // Agentmaster (conversation lineage): SHOW PREVIOUS SESSION(S) — a GLOBAL setting
        // (AppSettings::summaryPanelShowPrevious), mirrored in by the page (seed on attach, broadcast on
        // toggle). When ON, a /compact'ed session renders its pre-compaction segment(s) ABOVE the current
        // Messages. Re-renders the panel (the flag is baked into the rendered text). Call on the UI thread.
        void SetSummaryShowPrevious(bool on);
        // The previous-session toggle (leftmost in the times-bar strip) flips the GLOBAL setting; the
        // overlay can't reach AppSettings, so it calls this page-wired handler for the RMW + broadcast.
        void SetSummaryPreviousToggleHandler(std::function<void()> handler);

        // Agentmaster (TAB_OVERLAY.md summary panel resize): the panel SIZE is a GLOBAL setting
        // (AppSettings::summaryPanelWidthFraction/HeightFraction), stored as FRACTIONS of the pane so it
        // scales with the window. The page seeds this overlay (on attach) and broadcasts (when ANY tab
        // resizes the panel); the overlay applies fraction*paneSize. 0 == "auto" (the original look).
        // Call on the UI thread.
        void SetSummarySize(double widthFraction, double heightFraction);
        // The host (TerminalPaneContent) pushes the live PANE size here — on its wrapper's SizeChanged
        // and once on wire — so the overlay can convert the size fractions to pixels and re-apply when
        // the window resizes (keeping the panel a constant % of the pane). Call on the UI thread.
        void OnSummaryPaneSize(double paneWidth, double paneHeight);
        // The resize grips flip the GLOBAL fractions; the overlay can't reach AppSettings, so on drag
        // release it calls this handler (wired by the page) to do the freshest-disk read-modify-write +
        // the live broadcast to every linked overlay in the window. Set by _AttachClaudeOverlay.
        void SetSummaryResizeHandler(std::function<void(double, double)> handler);

        // Agentmaster (SUMMARY_JUMP.md): a numbered prompt in the summary panel carries a JUMP button that
        // scrolls the session's terminal view to where that prompt is rendered. The overlay can't reach the
        // TermControl, so it calls this page-wired handler with the session's prompt list + the 0-based
        // index; the page resolves the tab's control and calls TermControl::JumpToConversationPrompt,
        // returning the buffer row jumped to (or -1 if not on screen). Set by _AttachClaudeOverlay.
        void SetJumpHandler(std::function<int(const std::vector<std::wstring>&, int)> handler);

        // Agentmaster (SUMMARY_JUMP.md): per-icon eligibility. The page resolves every prompt to a buffer
        // row (-1 == not on screen); the overlay dims the jump buttons whose prompt currently won't
        // resolve. Set by _AttachClaudeOverlay.
        void SetEligibilityHandler(std::function<std::vector<int>(const std::vector<std::wstring>&)> handler);

        // Agentmaster (SUMMARY_JUMP.md §7): the overlay's row-2 ↑/↓ buttons step to the previous / next
        // off-screen SENT prompt — same as alt+up / alt+down. The overlay can't reach the control, so it
        // calls this page-wired handler with the direction (true == up); the page runs the SAME
        // _ScrollAdjacentPrompt for this session (scroll + the summary highlight + the boundary sound at
        // the ends). Set by _AttachClaudeOverlay.
        void SetAdjacentPromptHandler(std::function<void(bool)> handler);

        // Agentmaster (SUMMARY_JUMP.md): highlight the summary row for the message we just jumped to —
        // via the ▸ button OR alt+up / alt+down nav (the page passes the landed 0-based message index). A
        // translucent band behind the row; it persists across panel re-renders and moves to the new
        // target on the next jump. Out-of-range, or the panel not built, == no-op. Call on the UI thread.
        void HighlightSummaryMessage(int index);

        // Agentmaster (SUMMARY_JUMP.md §4a / §7): re-resolve the summary panel's jump-icon eligibility
        // against the live buffer (dim the icons whose prompt isn't on screen right now). Invoked by the
        // page's 30 s focused refresh, on top of the panel's own visible-only 5 s tick. No-op when the panel
        // is closed / has no jump buttons. Call on the UI thread.
        void RefreshJumpData();

        // Agentmaster (TAB_OVERLAY.md summary panel): a cheap, mtime-gated content re-read from the
        // freshest registry snapshot — the SAME reload the panel's own 5 s backstop timer performs.
        // Exposed so the page can kick it when this tab is FOCUSED (switching TO a background tab then
        // shows current content instead of up to ~5 s stale — the panel is a "here-and-now lens"). Unlike
        // the times-bar ↻ button (_RefreshSummary) this does NOT force: the mtime gate inside
        // _LoadSummaryAsync makes an unchanged transcript a cheap stat with NO re-render (no flicker /
        // scroll reset). No-op when the panel is off / no session / no registry. Call on the UI thread.
        void RefreshSummaryContent();

    private:
        void _Refresh(); // rebuild the line from the registry snapshot (UI thread)
        void _Detach(); // drop the registry observer
        void _WireHover(); // attach the pointer-over brighten handlers (idempotent; weak-captured)
        void _BuildActionsRow(); // lazily build the action buttons (folder + copy menu + pencil); placed in row 1 right after the status block by _Refresh, for a LINKED session
        void _SetExpanded(bool on); // dim<->bright the whole badge (driven by hover OR the copy-menu pinned state)
        void _CycleAutorunner(); // row-1 Autorunner button: cycle this session's mode Off -> Semi -> Full -> Off (mutates the shared registry; Rule #1)
        void _OpenFolder(); // row 3 folder button: open the session's working dir in Explorer (off-thread)
        void _CopyField(int which); // row 3 copy menu: 0=Session Id 1=Copy Path 2=Copy Branch 3=Claude CLI 4=Codex CLI 5=Transcript 6=Summary (full textual box)
        void _BuildSummaryPanel(); // build the summary panel element (the 2nd slot), collapsed
        void _SetSummaryContent(const std::wstring& text); // fill the panel StackPanel: text runs -> TextBlocks, separator sentinels -> full-width Border rules
        void _UpdateTimesLine(); // re-render the live "age / last user msg / last activity" ago line (DispatcherTimer-driven)
        void _ApplySummaryVisibility(); // show the 2nd pane only when enabled AND non-empty (content rows or a times line); else collapse it
        void _ApplySummarySize(bool forced = false); // re-apply the panel size from the size fractions + cached pane size: at rest MaxWidth/MaxHeight only (fit content); forced==true (mid grip-drag) ALSO pins explicit Width/Height so the panel grows PAST its own content up to the shared max, then snaps back on release
        double _CurrentSummaryWidthPx() const; // the panel's current effective max width in px (fraction*pane, or the 20% default)
        double _CurrentSummaryHeightPx() const; // the scroll viewport's current effective max height in px (fraction*pane, or min(480, 0.75*pane))
        void _OnSummaryDragMove(double pointerX, double pointerY); // live grip-drag: update the dragged size fraction(s) from the pointer delta + re-apply
        void _OnSummaryDragEnd(const winrt::Windows::Foundation::IInspectable& sender); // grip-drag release: release pointer capture + persist via the resize handler
        void _ToggleSummary(); // pencil button: invoke the page handler (flips the GLOBAL showSummaryPanel)
        void _ToggleSummaryWrap(); // wrap-line button: invoke the page handler (flips the GLOBAL summaryPanelWrapNewlines)
        void _UpdateSummaryWrapButtonVisual(); // recolor the wrap-line icon: dim (off) / lighter (on), per _summaryWrapNewlines
        void _ToggleSummaryTruncate(); // truncate button: invoke the page handler (flips the GLOBAL summaryPanelTruncate)
        void _UpdateSummaryTruncateButtonVisual(); // recolor the truncate icon: dim (off) / lighter (on), per _summaryTruncate
        std::wstring _SummarySelectedText(); // the text the user selected in the summary panel (title / times / body runs) — feeds + gates the context menu's "Copy Selected Text"
        void _ToggleSummaryPrevious(); // previous-session button: invoke the page handler (flips the GLOBAL summaryPanelShowPrevious)
        void _UpdateSummaryPrevButtonVisual(); // recolor the previous-session icon: dim (off) / lighter (on), per _summaryShowPrevious
        void _RefreshSummary(); // refresh button: force a re-analyze+render NOW (reset the mtime gate + re-pull) — a local action, no global setting
        void _UpdateSummary(const ::Agentmaster::SessionInfo& s); // _Refresh-driven: show/hide (per _summaryEnabled) + (re)load when grown
        winrt::fire_and_forget _LoadSummaryAsync(std::wstring transcriptPath, bool codex, std::wstring sessionId, std::wstring forkParentId, std::wstring cwd, std::wstring liveGlyph, std::wstring liveLabel, int64_t mtime, bool wrapNewlines, bool truncate, bool showPrevious, bool lineageCached, std::vector<::Agentmaster::ConversationSegment> cachedLineage); // analyze + render off-thread (wrapNewlines: preserve message newlines vs literal \n; truncate: limit each message; showPrevious: render pre-compaction previous session(s)), set text on the UI thread. lineageCached/cachedLineage: reuse the memoized cross-file (/clear + plan-restart) parents instead of re-walking. forkParentId: a never-messaged fork has no own transcript yet — fall back to the parent's (the fork inherits it verbatim until its first message)

        std::wstring _sessionId;
        bool _pending{ false }; // registry-less "observe" badge (a shell / unresolved claude — no linked session)
        bool _hovering{ false }; // pointer is currently over the badge
        bool _pinned{ false }; // the copy menu is open — keep expanded even after the pointer leaves
        bool _hoverWired{ false }; // _WireHover ran once (Initialize / first ShowActivity)
        double _restOpacity{ 0.50 }; // GLOBAL AppSettings::tabOverlayRestOpacity mirror (dim, at rest) — page-driven (seed + Save/broadcast)
        double _hoverOpacity{ 1.0 }; // GLOBAL AppSettings::tabOverlayHoverOpacity mirror (bright, on hover / copy-menu-open) — page-driven
        int _tabColorMode{ 0 }; // GLOBAL AppSettings::tabColorMode mirror as int (0 == WorkingDirectory) — page-driven; keys the EFFECTIVE work dir the subline/Open Path/Copy Path show (SetTabColorMode)
        std::wstring _lastActivitySig; // last kind rendered by ShowActivity (skip redundant re-renders)
        std::shared_ptr<::Agentmaster::SessionRegistry> _registry;
        uint64_t _observerToken{ 0 }; // ::Agentmaster::ObserverToken (uint64_t; avoid the header here)
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr };

        winrt::Windows::UI::Xaml::Controls::Border _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _stack{ nullptr }; // vertical: row 1 / row 2 / row 3
        winrt::Windows::UI::Xaml::Controls::StackPanel _row1{ nullptr }; // row 1: a horizontal strip of DISCRETE, individually-tooltipped parts (status · [actions] · Autorunner[button] · queue · link)
        winrt::Windows::UI::Xaml::Controls::StackPanel _row2{ nullptr }; // row 2: the dir/branch label only ("<root workdir folder>/<branch>")
        winrt::Windows::UI::Xaml::Controls::TextBlock _subline{ nullptr }; // row 2 label: "<root workdir folder>/<branch>"
        winrt::Windows::UI::Xaml::Controls::StackPanel _actions{ nullptr }; // row 1: folder + copy + pencil buttons — ALWAYS shown, just after the status block
        // Row 3 (Agentmaster): a preview of the NEXT queued prompt waiting to be sent — the hourglass +
        // the first line of the first Pending prompt (the one DecideAdvance would fire next), capped at
        // 300 chars (a longer first line, or any further lines, ends with "..."). Wraps + width-capped so
        // a long prompt can't balloon the HUD; hidden when nothing is queued (and on observe badges).
        winrt::Windows::UI::Xaml::Controls::TextBlock _promptLine{ nullptr };

        // Summary panel (the 2nd slot): a scrollable box, shown while the global showSummaryPanel is ON.
        // The body is a StackPanel (not one TextBlock) so separators can be full-width Border rules.
        winrt::Windows::UI::Xaml::Controls::Border _summaryRoot{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _summaryTitleText{ nullptr }; // pinned TOP row (above the times line + the numbered messages): the tab/session title (SessionInfo.title)
        winrt::Windows::UI::Xaml::Controls::TextBlock _summaryTimesText{ nullptr }; // pinned top (left): the live "ago" times line
        winrt::Windows::UI::Xaml::Controls::FontIcon _summaryWrapIcon{ nullptr }; // pinned top (right): the wrap-line toggle glyph — recolored by _UpdateSummaryWrapButtonVisual
        winrt::Windows::UI::Xaml::Controls::FontIcon _summaryTruncateIcon{ nullptr }; // pinned top (right, LEFT of the wrap toggle): the truncate toggle glyph — recolored by _UpdateSummaryTruncateButtonVisual
        winrt::Windows::UI::Xaml::Controls::FontIcon _summaryPrevIcon{ nullptr }; // pinned top (right, LEFTMOST): the previous-session toggle glyph — recolored by _UpdateSummaryPrevButtonVisual
        winrt::Windows::UI::Xaml::Controls::Button _summaryPrevBtn{ nullptr }; // the previous-session toggle button — Collapsed unless the loaded summary has a previous segment (a /compact'ed session)
        winrt::Windows::UI::Xaml::Controls::StackPanel _summaryStack{ nullptr };
        // Right-click "Copy Summary" context menu — built once (_BuildSummaryPanel) and shared as the
        // ContextFlyout of the panel root AND every selectable text block it renders (title / times line /
        // body runs), so a right-click anywhere on the panel offers the full-box copy (== _CopyField(6),
        // the same action as the badge copy menu's "Summary" item).
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _summaryContextMenu{ nullptr };
        winrt::Windows::UI::Xaml::DispatcherTimer _summaryTimer{ nullptr }; // drives the live times line; self-stops when the overlay is gone
        std::wstring _summaryPath; // cached resolved transcript path (resolve once)
        int64_t _summaryMtime{ 0 }; // last-loaded transcript mtime — reload only when it grows
        int64_t _summaryCreatedMs{ 0 }; // times line: conversation start (unix ms) — "age"
        int64_t _summaryLastUserMs{ 0 }; // times line: last real user prompt (unix ms) — "last user msg"
        int64_t _summaryLastActivityMs{ 0 }; // times line: last transcript entry (unix ms) — "last activity"
        // Agentmaster (subagent activity): the registry's SUBAGENT-FOLDED last-activity
        // (SessionInfo::convLastActivityUnixMs) — captured synchronously by _UpdateSummary from the live
        // snapshot and max()'d into the "last activity" the times line shows. The panel's OWN sources
        // (_summaryLastActivityMs from the transcript tail + the parent file's mtime) read the PARENT
        // <id>.jsonl only, which stays quiescent while a Task/Agent subagent runs (its work streams to
        // <id>/subagents/*.jsonl) — so without this the times line froze ("looks idle while working")
        // even as the badge showed Running. 0 == unknown (a harmless no-op in the max).
        int64_t _summaryConvLastActivityMs{ 0 };
        bool _summaryLoading{ false }; // one analyze+render in flight at a time
        bool _summaryWrapDirty{ false }; // a wrap-mode toggle landed while a load was in flight — re-render when it completes
        bool _summaryTruncateDirty{ false }; // a truncate-mode toggle landed while a load was in flight — re-render when it completes
        bool _summaryPrevDirty{ false }; // a previous-session toggle landed while a load was in flight — re-render when it completes
        bool _summaryReloadPending{ false }; // a content refresh was requested (the transcript may have grown) WHILE a load was in flight — re-check on completion so the growth isn't dropped (the early-return otherwise relies on a "next _Refresh" that may never come once the session goes idle)
        bool _summaryEnabled{ false }; // mirror of the GLOBAL AppSettings::showSummaryPanel (page-driven)
        bool _summaryWrapNewlines{ false }; // mirror of the GLOBAL AppSettings::summaryPanelWrapNewlines (page-driven): preserve message newlines vs literal \n
        bool _summaryTruncate{ true }; // mirror of the GLOBAL AppSettings::summaryPanelTruncate (page-driven, default ON): ON=cap each message (6 lines if wrapped, else 500 chars); OFF=show every message in full
        bool _summaryShowPrevious{ false }; // mirror of the GLOBAL AppSettings::summaryPanelShowPrevious (page-driven, default OFF): ON=render pre-compaction previous session(s) above the current Messages
        // Agentmaster (cross-file lineage): the resolved /clear + plan-restart PARENT segments
        // (oldest-first), MEMOIZED — parentage is immutable, so compute once per bound session then
        // reuse on every quiet mtime-gated reload + show-previous toggle (no per-write dir scan).
        // _summaryLineageId is the conv id _summaryLineage was computed for ("" == not yet); a rebind
        // to a new conv id recomputes. Folded into SessionSummary.previousSegments before render.
        std::wstring _summaryLineageId;
        std::vector<::Agentmaster::ConversationSegment> _summaryLineage;
        std::function<void()> _onToggleSummary; // pencil -> page (flip the global setting + broadcast)
        std::function<void()> _onToggleSummaryWrap; // wrap-line icon -> page (flip the global newline setting + broadcast)
        std::function<void()> _onToggleSummaryTruncate; // truncate icon -> page (flip the global truncate setting + broadcast)
        std::function<void()> _onToggleSummaryPrevious; // previous-session icon -> page (flip the global show-previous setting + broadcast)

        // Summary panel RESIZE (TAB_OVERLAY.md): the panel is anchored top-right; left/bottom/corner
        // grips drag it bigger (left=width, bottom=height, corner=both). Size is kept as FRACTIONS of
        // the pane (0 == auto) so it scales with the window; the page persists them GLOBALLY.
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _summaryScroll{ nullptr }; // the body scroller — its MaxHeight is the panel's height control
        double _summaryWidthFraction{ 0.0 }; // 0 == auto (20% cap); else explicit width fraction of the pane (0.08..0.5)
        double _summaryHeightFraction{ 0.0 }; // 0 == auto (content up to min(480,0.75*pane)); else explicit height fraction (0.06..0.75)
        bool _summarySizeLocalOverride{ false }; // a SHIFT-resize detached THIS tab from the shared/global size: it keeps its own size + ignores broadcasts until the next no-Shift drag re-attaches it
        double _summaryPaneW{ 0.0 }; // last pane width pushed by the host (px)
        double _summaryPaneH{ 0.0 }; // last pane height pushed by the host (px)
        bool _summaryDragging{ false }; // a grip drag is in flight
        bool _summaryDragLeft{ false }; // the in-flight drag adjusts width (left edge / corner)
        bool _summaryDragBottom{ false }; // the in-flight drag adjusts height (bottom edge / corner)
        bool _summaryDragShift{ false }; // SHIFT held at this drag's START => a LOCAL-only resize (ephemeral; not persisted, not broadcast)
        double _summaryDragStartX{ 0.0 }; // pointer X at press (island-relative)
        double _summaryDragStartY{ 0.0 }; // pointer Y at press
        double _summaryDragStartW{ 0.0 }; // panel width px at press
        double _summaryDragStartH{ 0.0 }; // scroll viewport height px at press
        std::function<void(double, double)> _onResizeSummary; // grip release -> page (persist the fractions globally + broadcast)

        // Agentmaster (SUMMARY_JUMP.md): the raw user prompts of the loaded summary, in order, so a
        // numbered message's JUMP button resolves to the right prompt (index N-1). Refreshed by
        // _LoadSummaryAsync alongside the rendered text; aligned with the " N. " lines _SetSummaryContent
        // renders. Empty for Codex / no transcript (no jump buttons then).
        std::vector<std::wstring> _summaryUserMsgs;
        std::function<int(const std::vector<std::wstring>&, int)> _onJumpToPrompt; // jump button -> page (resolve control + center the view); returns the row or -1
        std::function<std::vector<int>(const std::vector<std::wstring>&)> _onResolveEligibility; // -> page: a row per prompt (-1 == not on screen), for icon dimming
        std::function<void(bool)> _onAdjacentPrompt; // row-2 ↑/↓ buttons -> page (_ScrollAdjacentPrompt: scroll to prev/next off-screen prompt + highlight + boundary sound)
        // The jump buttons of the currently-rendered panel, paired with their 0-based prompt index, so
        // _RefreshJumpEligibility can dim the ones whose prompt no longer resolves. Rebuilt each _SetSummaryContent.
        std::vector<std::pair<int, winrt::Windows::UI::Xaml::Controls::Button>> _jumpButtons;
        void _RefreshJumpEligibility(); // resolve all prompts -> set each jump button's opacity (match vs dim); SUMMARY_JUMP.md
        // The numbered-message ROW containers (the 2-col Grid), paired with their 0-based prompt index, so
        // HighlightSummaryMessage can paint a band behind the jumped-to row. Rebuilt each _SetSummaryContent.
        std::vector<std::pair<int, winrt::Windows::UI::Xaml::Controls::Grid>> _summaryMsgRows;
        int _highlightedMsgIndex{ -1 }; // the last jumped-to message (0-based), re-applied across re-renders; -1 == none
        void _ApplySummaryHighlight(); // paint the band on _highlightedMsgIndex's row, clear the rest (SUMMARY_JUMP.md)
    };
}
