// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the per-tab "link badge" overlay (see doc/agentmaster/TAB_OVERLAY.md). A small,
// compact HUD pinned to the top-right of a Claude session's terminal, summarizing the tab <->
// Agentmaster link at a glance. Row 1 is a strip of DISCRETE, individually-tooltipped parts, in order:
// hook-driven status (color-matched to the Triage Board; LEFTMOST), the action buttons (folder / copy
// menu / pencil — ALWAYS shown for a LINKED session), Autorunner mode (Paused/Auto — a CLICKABLE button
// that flips the mode), queued (Pending) count, and link state (surfaced ONLY when NOT linked —
// "observe" / "unlinked"; a linked session shows nothing for link state).
// Row 2: "<root workdir folder>/<branch>" (dim; e.g. myworkdir/feature/issue123) so the session's place +
// branch read at a glance; the label is hidden when there's no dir/branch (and on observe badges). The
// row-1 action buttons (a LINKED session only) are: a folder
// button (Open Path — the working dir via explorer.exe) + a MAIL button (queue this session's UNSENT
// input-box draft into its prompt queue — Claude only, in every build since 2026-10-09; the Manager envelope's twin)
// + a copy button whose menu copies the Session Id
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
// Duration/Tasks/Messages/Skills Loaded/Files Created/Edited/Read) — it omits everything panel 1 (the badge) already shows
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

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AgentMaster/SummaryConversation.h" // SummaryConversationRow — the rows of the summary's current render (a light, WinRT-free engine header)

namespace Agentmaster
{
    class SessionRegistry;
    struct SessionInfo; // _UpdateSummary takes a const& (full type in the .cpp via SessionRegistry.h)
}

namespace winrt::TerminalApp::implementation
{
    // Agentmaster (the summary's conversation chain): ONE row of the summary panel's conversation list —
    // a numbered PROMPT, or a SESSION SEPARATOR (sessionId non-empty) drawn after the last prompt of an
    // earlier session the current one continues. Built off-thread by BuildSummaryConversation, rendered to
    // text by RenderSummaryBox (a prompt row is one " N. " line, numbered as ONE conversation; a separator
    // is a lone kSessionSepMark + id line), then matched back to its line, in order, by _SetSummaryContent.
    // The type is ENGINE-owned (AgentMaster/SummaryConversation.h — light, no WinRT) so the m5 harness can
    // test the builder; the overlay keeps the rows of its current render as members under this name.
    using SummaryConversationRow = ::Agentmaster::SummaryConversationRow;

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

        // Agentmaster (current-model adornment): the GLOBAL AppSettings::modelFamilies csv — the
        // Anthropic family words row 1's model shortener (ShortModelName) recognizes when the model
        // is a BARE --model alias (a full "claude-…" id shortens regardless). Mirrored in by the
        // page — on attach (seed) and on cog Save / cross-window broadcast (the SetTabColorMode
        // idiom); kept as the raw csv here (parsed per refresh — engine-include-free header). A
        // change re-renders a linked badge. Call on the UI thread.
        void SetModelFamilies(const std::wstring& familiesCsv);

        // Agentmaster (the GLOBAL PAUSE on the badge — DELIVERY.md §17.3, TAB_OVERLAY.md §3b): tell this
        // badge whether the Prompt Queue Runner is paused for EVERY session (the Agent Manager toolbar's
        // "Pause Prompt Queue Runner" button — Scheduler::GlobalPaused). The pause is a fact of the
        // process-wide scheduler, NOT of the session, so no registry notify ever carries it and the
        // badge cannot read it by itself: the page mirrors it in — on attach (the seed in
        // _AttachClaudeOverlay) and from the window's global-pause listener on every change
        // (TerminalPage::_ApplyGlobalPauseShown). While it is on:
        //   - row 1's mode button reads "<current> (Off globally)" — the session's own mode
        //     ("Auto", "Paused", "Paused -> Auto"), which is kept and still switchable, followed by
        //     the reason nothing goes out by itself;
        //   - row 3 shows no "settling" cue (no automatic send is being waited for), and its tooltip
        //     names the global pause instead of promising a send on the next turn-complete — unless
        //     a question hold is the reason on screen, which stays (it outlives a resume).
        // ⚠ A DISPLAY mirror only: this neither pauses nor resumes anything (the scheduler owns the
        // state), and a badge that is never told keeps showing the plain mode — so every code path
        // that creates a linked badge must seed it.
        // Change-gated (a repeated value costs nothing); a real change re-renders a LINKED badge. An
        // observe badge has no mode button, so it only stores the value.
        //   paused — true while the global pause is in force.
        //   returns — nothing.
        //   throws  — what a badge re-render can throw: winrt::hresult_error from a failing XAML call,
        //             std::bad_alloc. The page's caller contains it per badge (_ApplyGlobalPauseShown).
        // Call on the UI thread.
        void SetGlobalPauseShown(bool paused);

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

        // Agentmaster (the summary's conversation chain): SHOW THE WHOLE CONVERSATION — a GLOBAL setting
        // (AppSettings::summaryPanelShowHistory, default ON), mirrored in by the page (seed on attach,
        // broadcast on toggle). ON lists every earlier session this one continues (a fork's parent, a
        // plan-restart parent, a /clear predecessor or /handover origin) and every part a /compact
        // summarized away, numbered 1..N as ONE conversation with a session-id separator between
        // sessions; OFF lists only the part after the last compaction. Re-renders the panel (the flag
        // decides which rows are rendered). Call on the UI thread.
        void SetSummaryShowHistory(bool on);
        // The ⏮ toggle (leftmost in the times-bar strip) flips the GLOBAL setting; the overlay can't reach
        // AppSettings, so it calls this page-wired handler for the freshest-disk RMW + the broadcast.
        void SetSummaryHistoryToggleHandler(std::function<void()> handler);

        // Agentmaster (SIDE PANEL): switch the summary panel from its native ANCHORED-top-right,
        // content-hugging, grip-resizable layout to a FILL-THE-HOST layout for embedding in a fixed
        // slot the HOST sizes (the window side panel's bottom area). In fill mode the panel stretches
        // to the host width (MaxWidth = the pushed pane width, ignoring the global width fraction and
        // its 50% cap), the body ScrollViewer's MaxHeight tracks the pushed pane height minus the
        // panel's own chrome (title + times bar + padding), and the three resize GRIPS are never built
        // — the host owns sizing (its own splitters), so a grip would fight it and its drag-persist
        // would corrupt the GLOBAL per-tab fractions. It also draws NO frame — no border, square
        // corners, a transparent fill (still hit-testable, for the right-click menu) — because it
        // sits inside the host's area instead of floating over a terminal: the host's own background
        // shows through. ⚠ MUST be called BEFORE Initialize: the grips, the root's alignment and its
        // frame are decided when _BuildSummaryPanel constructs the tree, and there
        // is deliberately no dynamic un-build (the mode is fixed for the overlay's lifetime). A
        // fill-hosted overlay still honors every content flag (wrap/truncate/previous, SetSummary*)
        // and still needs OnSummaryPaneSize pushes — the HOST's slot size, not a terminal pane's.
        void SetSummaryFillHost();

        // Agentmaster (TAB_OVERLAY.md summary panel resize): the panel SIZE is a GLOBAL setting
        // (AppSettings::summaryPanelWidthFraction/HeightFraction), stored as FRACTIONS of the pane so it
        // scales with the window. The page seeds this overlay (on attach) and broadcasts (when ANY tab
        // resizes the panel); the overlay applies fraction*paneSize. 0 == "auto" (the original look).
        // A fill-hosted overlay (SetSummaryFillHost) ignores the fractions — the host slot sizes it.
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

        // Agentmaster (PENDING_INPUT.md §8): the copy menu's "Copy Current Prompt" needs the UNSENT
        // draft as the input box holds it RIGHT NOW, and the overlay can't reach the TermControl — so
        // the page wires this reader (TerminalPage::_ReadLiveDraftForSession, itself fully wrapped:
        // any failure reads as ""). Unwired / empty simply falls back to the observer's recorded
        // SessionInfo::pendingInput inside CopySessionField. Set by _AttachClaudeOverlay.
        void SetLiveDraftHandler(std::function<std::wstring()> handler);

        // Agentmaster (PENDING_INPUT.md §8d): the MAIL button's default (plain) click REMOVES the draft
        // from the input box after queueing it (a move); Shift+Click keeps it. The overlay can't inject,
        // so the page wires this clearer (TerminalPage::_ClearLiveDraftForSession — the verified draft-swap
        // clear standing alone). Unwired => the box is simply left as-is (== Shift+Click). Set by
        // _AttachClaudeOverlay.
        void SetClearDraftHandler(std::function<void()> handler);

        // Agentmaster (transcript overlay — AgentTranscriptOverlay.h): the copy menu's "Transcript ▸ Show"
        // and the summary panel's right-click "Show Transcript" open THIS session's transcript in the
        // window-level reading view. The overlay belongs to the PAGE (it covers the whole window, not this
        // pane), so the page wires the opener (TerminalPage::_ShowTranscriptOverlayForSession). Unwired =>
        // both items are simply inert. Set by _AttachClaudeOverlay and the side panel's summary host.
        void SetShowTranscriptHandler(std::function<void()> handler);

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

        // Agentmaster (SUMMARY_JUMP.md §4, perf): tell this overlay whether its tab is the window's
        // SELECTED one. The jump-eligibility resolve is expensive and only ever changes what a VISIBLE
        // panel renders, so the periodic refreshes run for the focused tab alone; switching TO a tab forces
        // one immediate resolve so its panel is accurate the moment you look at it. Page-driven from the
        // tab-selection funnel. Call on the UI thread.
        void SetTabFocused(bool focused);

        // Agentmaster (TAB_OVERLAY.md summary panel): a cheap, mtime-gated content re-read from the
        // freshest registry snapshot — the SAME reload the panel's own 5 s backstop timer performs.
        // Exposed so the page can kick it when this tab is FOCUSED (switching TO a background tab then
        // shows current content instead of up to ~5 s stale — the panel is a "here-and-now lens"). Unlike
        // the times-bar ↻ button (_RefreshSummary) this does NOT force: the mtime gate inside
        // _LoadSummaryAsync makes an unchanged transcript a cheap stat with NO re-render (no flicker /
        // scroll reset). No-op when the panel is off / no session / no registry. Call on the UI thread.
        void RefreshSummaryContent();

    private:
        // Rebuild the LINKED badge from the registry snapshot of this session: row 1 (model, status,
        // actions, the Prompt Queue Runner button, queued count, link state), row 2 (folder / branch) and
        // row 3 (the next queued prompt with its hold cue). A no-op for an observe badge and before
        // Initialize; an id the registry no longer knows collapses the badge. UI thread only.
        //   updateSummary — true (the default: every registry-driven refresh) also runs _UpdateSummary,
        //                   which shows / hides the summary panel and, while the panel is on, asks for an
        //                   off-thread transcript growth check (a path lookup and a stat for each call).
        //                   Pass false ONLY when the cause of the repaint cannot change anything the
        //                   panel shows — today the global pause (SetGlobalPauseShown), which reaches
        //                   every badge of a window at once and would start one check per badge.
        //   returns — nothing.
        //   throws  — winrt::hresult_error from a failing XAML call; std::bad_alloc.
        void _Refresh(bool updateSummary = true);
        void _Detach(); // drop the registry observer
        void _WireHover(); // attach the pointer-over brighten handlers (idempotent; weak-captured)
        void _BuildActionsRow(); // lazily build the action buttons (folder + mail/queue + copy menu + pencil); placed in row 1 right after the status block by _Refresh, for a LINKED session
        void _SetExpanded(bool on); // dim<->bright the whole badge (driven by hover OR the copy-menu pinned state)
        void _CycleAutorunner(); // row-1 Autorunner button: flip this session's mode Paused <-> Auto (AutorunnerMode Off <-> Full); the user's explicit choice, so it cancels a pending interrupt resume, and a switch to Auto also resets the per-run send counter + releases the question-guard hold (mutates the shared registry; Rule #1). No-op with no live session. Throws std::bad_alloc only
        void _OpenFolder(); // row 3 folder button: open the session's working dir in Explorer (off-thread)
        void _QueueCurrentPrompt(bool clearBox); // row-1 MAIL button (between folder and copy): queue this session's UNSENT input-box draft into its Auto-Testing queue — the badge twin of the Manager compose row's envelope. Dev-or-debug + Claude only. clearBox (a PLAIN click) also REMOVES the draft from the input box after queueing (a move — PENDING_INPUT.md §8d); Shift+Click passes false to KEEP it (the historical copy, Rule #13)
        void _RefreshQueueButtonEnabled(const ::Agentmaster::SessionInfo& s); // _Refresh-driven: enable the MAIL button only while a draft actually exists (mirrors the "3 dots" / pendingInput), so a prominent toolbar button never silently no-ops on an empty box (the "dead button" trap)
        // Agentmaster (the STATE SETTLE hold — DELIVERY.md §16): keep row 3's "settling" cue honest when
        // the window simply RUNS OUT. The cue is drawn by _Refresh, which runs on registry notifies — but
        // the end of a settle window changes nothing in the registry, so without a wake-up the cue would
        // stay up after the hold released (until the send itself notified, or forever if another hold
        // took over). Arms a one-shot UI-thread timer that re-runs _Refresh just past the window's end;
        // remainingMs <= 0 stops a pending one. Restarting it on every _Refresh is intended: the newest
        // computation of the remaining time always wins. UI thread only; never throws (a timer failure
        // only costs the cue's self-clear and is logged).
        //   remainingMs — the settle time left for the next prompt (StateSettleRemainingMs), or 0.
        void _ArmSettleRefresh(int64_t remainingMs);
        void _CopyField(int which); // row 3 copy menu: 0=Session Id 1=Copy Path 2=Copy Branch 3=Claude CLI 4=Codex CLI 5=Transcript 6=Summary (full textual box)
        void _BuildSummaryPanel(); // build the summary panel element (the 2nd slot), collapsed
        void _SetSummaryContent(const std::wstring& text); // fill the panel StackPanel: text runs -> TextBlocks (wrapping; the "* item" file/skill lists NoWrap + clipped), separator sentinels -> full-width Border rules, each numbered conversation line -> a row matched IN ORDER to _summaryPromptRows (▸ jump button when its headIndex >= 0), each session-separator line -> the clickable id rule matched in order to _summarySeparatorRows
        // Agentmaster (the summary's conversation chain): build the clickable SESSION SEPARATOR drawn after
        // an earlier session's last prompt — a thin rule carrying the first 8 characters of `row.sessionId`.
        // A click copies the FULL id (with the copy chime) and pops the tooltip "Copied session id of up to
        // here"; hovering shows which session the conversation up to that point belongs to. UI thread only;
        // throws only what XAML element creation throws (the caller renders on the UI thread).
        //   row — the separator row (sessionId non-empty; title + continuedBy feed the hover text).
        // Returns the element to append to the panel body.
        winrt::Windows::UI::Xaml::UIElement _MakeSessionSeparator(const SummaryConversationRow& row);
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
        void _WireSummaryContextMenu(const winrt::Windows::UI::Xaml::FrameworkElement& el, int promptIndex); // attach the shared summary context menu to one panel element, remembering WHICH numbered conversation row (an index into _summaryPromptRows; -1 == none) a right-click there is about — gates + feeds the menu's "Copy Prompt"
        void _ToggleSummaryHistory(); // ⏮ button: invoke the page handler (flips the GLOBAL summaryPanelShowHistory)
        void _UpdateSummaryHistoryButtonVisual(); // recolor the ⏮ icon: dim (off) / lighter (on), per _summaryShowHistory
        void _UpdateSummaryPencilVisual(); // recolor the badge PENCIL: dim (panel off) / lighter (on), per _summaryEnabled — the pencil always visibly answers its click, even on a tab whose panel has nothing to render
        void _EnsureSummaryPlaceholder(); // when the panel is ENABLED but has NOTHING to render (a never-prompted session has no transcript; an unreconciled Codex has no rollout), show ONE dim placeholder line instead of collapsing — an invisible toggle reads as a dead button
        void _RefreshSummary(); // refresh button: force a re-analyze+render NOW (reset the mtime gate + re-pull) — a local action, no global setting
        void _UpdateSummary(const ::Agentmaster::SessionInfo& s); // _Refresh-driven: show/hide (per _summaryEnabled) + (re)load when grown
        winrt::fire_and_forget _LoadSummaryAsync(std::wstring transcriptPath, bool codex, std::wstring sessionId, std::wstring forkParentId, std::wstring cwd, std::wstring liveGlyph, std::wstring liveLabel, int64_t mtime, bool wrapNewlines, bool truncate, bool showHistory, std::shared_ptr<::Agentmaster::SessionRegistry> registry); // analyze + render off-thread (wrapNewlines: preserve message newlines vs literal \n; truncate: limit each message; showHistory: list the WHOLE conversation — the earlier sessions (TranscriptTurns.h CollectConversationUpstream, cached engine-side per transcript) and the pre-/compact parts — numbered as one), set text on the UI thread. registry: the recorded-predecessor + title lookups (the /clear and /handover links live on the session records). forkParentId: a never-messaged fork has no own transcript yet — fall back to the parent's (the fork inherits it verbatim until its first message)

        std::wstring _sessionId;
        bool _pending{ false }; // registry-less "observe" badge (a shell / unresolved claude — no linked session)
        bool _hovering{ false }; // pointer is currently over the badge
        bool _pinned{ false }; // the copy menu is open — keep expanded even after the pointer leaves
        bool _hoverWired{ false }; // _WireHover ran once (Initialize / first ShowActivity)
        double _restOpacity{ 0.50 }; // GLOBAL AppSettings::tabOverlayRestOpacity mirror (dim, at rest) — page-driven (seed + Save/broadcast)
        double _hoverOpacity{ 1.0 }; // GLOBAL AppSettings::tabOverlayHoverOpacity mirror (bright, on hover / copy-menu-open) — page-driven
        int _tabColorMode{ 0 }; // GLOBAL AppSettings::tabColorMode mirror as int (0 == WorkingDirectory) — page-driven; keys the EFFECTIVE work dir the subline/Open Path/Copy Path show (SetTabColorMode)
        std::wstring _modelFamiliesCsv; // GLOBAL AppSettings::modelFamilies mirror (raw csv; parsed per refresh) — page-driven; the family words row 1's model shortener recognizes for a bare --model alias (SetModelFamilies)
        bool _globalPauseShown{ false }; // Scheduler::GlobalPaused() mirror (the Manager toolbar's "Pause Prompt Queue Runner") — page-driven: seeded on attach, then set by the window's global-pause listener. true => row 1's mode button reads "<current> (Off globally)" and row 3 drops its "settling" cue. Display only; false until the page says otherwise (SetGlobalPauseShown)
        std::wstring _lastActivitySig; // last kind rendered by ShowActivity (skip redundant re-renders)
        std::shared_ptr<::Agentmaster::SessionRegistry> _registry;
        uint64_t _observerToken{ 0 }; // ::Agentmaster::ObserverToken (uint64_t; avoid the header here)
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr };

        winrt::Windows::UI::Xaml::Controls::Border _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _stack{ nullptr }; // vertical: row 1 / row 2 / row 3
        winrt::Windows::UI::Xaml::Controls::StackPanel _row1{ nullptr }; // row 1: a horizontal strip of DISCRETE, individually-tooltipped parts (status · [actions] · Autorunner[button] · queue · link)
        winrt::Windows::UI::Xaml::Controls::StackPanel _row2{ nullptr }; // row 2: the dir/branch label only ("<root workdir folder>/<branch>")
        winrt::Windows::UI::Xaml::Controls::TextBlock _subline{ nullptr }; // row 2 label: "<root workdir folder>/<branch>"
        winrt::Windows::UI::Xaml::Controls::StackPanel _actions{ nullptr }; // row 1: folder + mail + copy + pencil buttons — ALWAYS shown, just after the status block
        winrt::Windows::UI::Xaml::Controls::Button _queueBtn{ nullptr }; // row 1: the MAIL button (queue the unsent draft); null on a release / Codex badge (never built). _Refresh enables it only while a draft exists (_RefreshQueueButtonEnabled)
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
        winrt::Windows::UI::Xaml::Controls::FontIcon _summaryHistoryIcon{ nullptr }; // pinned top (right, LEFTMOST): the ⏮ whole-conversation toggle glyph — recolored by _UpdateSummaryHistoryButtonVisual
        winrt::Windows::UI::Xaml::Controls::Button _summaryHistoryBtn{ nullptr }; // the ⏮ toggle button — Collapsed unless the loaded summary HAS earlier history (an earlier session in the chain, or a part before a /compact)
        winrt::Windows::UI::Xaml::Controls::FontIcon _summaryPencilIcon{ nullptr }; // the badge action-row PENCIL glyph (summary-panel toggle) — recolored by _UpdateSummaryPencilVisual so the toggle state reads at the click point
        winrt::Windows::UI::Xaml::Controls::StackPanel _summaryStack{ nullptr };
        // Right-click "Copy Summary" context menu — built once (_BuildSummaryPanel) and shared as the
        // ContextFlyout of the panel root AND every selectable text block it renders (title / times line /
        // body runs), so a right-click anywhere on the panel offers the full-box copy (== _CopyField(6),
        // the same action as the badge copy menu's "Summary" item). Right-clicking a NUMBERED message row
        // additionally offers "Copy Prompt" (that one prompt, verbatim) — see _WireSummaryContextMenu.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _summaryContextMenu{ nullptr };
        // The numbered conversation row the LAST context-menu request landed on — an index into
        // _summaryPromptRows (-1 == the right-click wasn't on a numbered row) — written by
        // _WireSummaryContextMenu's per-element handler BEFORE it shows the menu, read by the menu's
        // Opening to gate + fill "Copy Prompt" (any row: an earlier session's prompt copies too).
        int _ctxPromptIndex{ -1 };
        winrt::Windows::UI::Xaml::DispatcherTimer _summaryTimer{ nullptr }; // drives the live times line; self-stops when the overlay is gone
        // One-shot re-draw at the end of a STATE SETTLE window (_ArmSettleRefresh, DELIVERY.md §16). Built
        // lazily on the first settling cue; like _summaryTimer its Tick captures only a weak ref and stops
        // itself once the overlay is gone, so the destructor never has to Stop() it off the UI thread.
        winrt::Windows::UI::Xaml::DispatcherTimer _settleTimer{ nullptr };
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
        bool _summaryHistoryDirty{ false }; // a ⏮ (whole-conversation) toggle landed while a load was in flight — re-render when it completes
        bool _summaryReloadPending{ false }; // a content refresh was requested (the transcript may have grown) WHILE a load was in flight — re-check on completion so the growth isn't dropped (the early-return otherwise relies on a "next _Refresh" that may never come once the session goes idle)
        bool _summaryEnabled{ false }; // mirror of the GLOBAL AppSettings::showSummaryPanel (page-driven)
        bool _summaryWrapNewlines{ false }; // mirror of the GLOBAL AppSettings::summaryPanelWrapNewlines (page-driven): preserve message newlines vs literal \n
        bool _summaryTruncate{ true }; // mirror of the GLOBAL AppSettings::summaryPanelTruncate (page-driven, default ON): ON=cap each message (6 lines if wrapped, else 500 chars); OFF=show every message in full
        bool _summaryShowHistory{ true }; // mirror of the GLOBAL AppSettings::summaryPanelShowHistory (page-driven, default ON): ON=list the WHOLE conversation (earlier sessions + pre-/compact parts) as one numbered list; OFF=only the part after the last compaction
        // (The earlier sessions are NOT memoized here: TranscriptTurns.h CollectConversationUpstream keeps
        // its own process-wide cache, keyed by the transcript and its recorded link — so the side panel,
        // which builds a NEW overlay at every selection, and every per-tab panel share one walk per session.)
        std::function<void()> _onToggleSummary; // pencil -> page (flip the global setting + broadcast)
        std::function<void()> _onToggleSummaryWrap; // wrap-line icon -> page (flip the global newline setting + broadcast)
        std::function<void()> _onToggleSummaryTruncate; // truncate icon -> page (flip the global truncate setting + broadcast)
        std::function<void()> _onToggleSummaryHistory; // ⏮ icon -> page (flip the global whole-conversation setting + broadcast)

        // Agentmaster (SIDE PANEL): FILL-THE-HOST layout mode (SetSummaryFillHost — must be set before
        // Initialize builds the panel). true => stretch to the pushed pane (host slot) size, no grips,
        // fractions ignored; false (default) => the classic anchored/content-hugging per-tab panel.
        bool _summaryFillHost{ false };
        // Coalesces the fill mode's root-SizeChanged re-apply onto ONE clean dispatcher tick (the
        // _PositionTagBadges discipline): mutating MaxHeight synchronously inside a layout-driven
        // handler risks XAML's E_LAYOUTCYCLE fail-fast, so SizeChanged only queues; the tick applies.
        bool _summaryFillReapplyQueued{ false };

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

        // Agentmaster (SUMMARY_JUMP.md): the CURRENT session's analyzer prompt list (SessionSummary::userMsgs),
        // in order — the list the JUMP resolver and alt+up/down navigate, so a row's headIndex resolves to
        // the right prompt. Refreshed by _LoadSummaryAsync alongside the rendered text. Empty for Codex / no
        // transcript (no jump buttons then).
        std::vector<std::wstring> _summaryUserMsgs;
        // Agentmaster (the summary's conversation chain): the rows the CURRENT render was built from, split
        // by kind but each in display order — the k-th numbered line ( " k+1. " ) is _summaryPromptRows[k],
        // the j-th session-separator line is _summarySeparatorRows[j]. Set by _LoadSummaryAsync's publish
        // BEFORE _SetSummaryContent, which matches the rendered lines back to them in order.
        std::vector<SummaryConversationRow> _summaryPromptRows;
        std::vector<SummaryConversationRow> _summarySeparatorRows;
        // Agentmaster (the summary's conversation chain): the text the panel body was last built from. A
        // reload whose text AND rows are unchanged (an assistant-only write: the prompts, files and tasks
        // read the same) skips the rebuild — with the whole conversation listed the body can hold many
        // hundreds of rows, and rebuilding them unchanged would also reset the panel's scroll position.
        std::wstring _summaryRenderedText;
        std::function<int(const std::vector<std::wstring>&, int)> _onJumpToPrompt; // jump button -> page (resolve control + center the view); returns the row or -1
        std::function<std::vector<int>(const std::vector<std::wstring>&)> _onResolveEligibility; // -> page: a row per prompt (-1 == not on screen), for icon dimming
        std::function<void(bool)> _onAdjacentPrompt; // row-2 ↑/↓ buttons -> page (_ScrollAdjacentPrompt: scroll to prev/next off-screen prompt + highlight + boundary sound)
        std::function<std::wstring()> _onReadLiveDraft; // copy menu "Copy Current Prompt" -> page (read this session's input box out of the live buffer; "" when unreadable — the remembered draft then wins)
        std::function<void()> _onClearDraft; // MAIL button (plain click) -> page (_ClearLiveDraftForSession: remove the draft from the input box after queueing — a move; Shift+Click skips this)
        std::function<void()> _onShowTranscript; // copy menu "Transcript ▸ Show" + summary panel "Show Transcript" -> page (_ShowTranscriptOverlayForSession: the window-level transcript reading view)
        // The jump buttons of the currently-rendered panel, paired with their prompt's index in _summaryUserMsgs
        // (a row's headIndex), so _RefreshJumpEligibility can dim the ones whose prompt no longer resolves.
        // Rebuilt each _SetSummaryContent.
        std::vector<std::pair<int, winrt::Windows::UI::Xaml::Controls::Button>> _jumpButtons;
        // Resolve all prompts -> set each jump button's opacity (match vs dim); SUMMARY_JUMP.md.
        // EXPENSIVE (O(prompts x 1.2M-char buffer window), ~0.3s on a 200+-prompt conversation), so the
        // periodic callers are gated: `force == false` runs ONLY on the FOCUSED tab and at most once per
        // kJumpEligibilityMinIntervalMs. `force == true` is for user actions + the focus transition, where
        // the answer must be current immediately (and is usually free — ControlCore's epoch cache returns
        // the previous rows outright when neither the buffer nor the prompt list moved).
        void _RefreshJumpEligibility(bool force = false);
        // The floor between two PERIODIC eligibility resolves (2m30s). Before this gate existed the panel's
        // own 5 s tick x every linked tab (the "visible-only" comment was never enforced) x a 244-prompt
        // conversation saturated the UI thread permanently — the 2026-07-19 release freeze.
        static constexpr int64_t kJumpEligibilityMinIntervalMs = 150'000;
        bool _tabFocused{ false }; // is this overlay's tab the window's selected one? (page-driven, SetTabFocused)
        std::chrono::steady_clock::time_point _lastJumpEligibilityRun{}; // last resolve; default == epoch, so the first call always runs
        // The numbered-message ROW containers (the 2-col Grid) that HAVE a prompt index in _summaryUserMsgs
        // (headIndex >= 0), paired with it, so HighlightSummaryMessage (fed by the ▸ button and alt+up/down,
        // both in _summaryUserMsgs indices) can paint a band behind the jumped-to row. A row of an earlier
        // session that this session's transcript does not hold is never a jump target, so it is not listed.
        // Rebuilt each _SetSummaryContent.
        std::vector<std::pair<int, winrt::Windows::UI::Xaml::Controls::Grid>> _summaryMsgRows;
        int _highlightedMsgIndex{ -1 }; // the last jumped-to message (0-based), re-applied across re-renders; -1 == none
        void _ApplySummaryHighlight(); // paint the band on _highlightedMsgIndex's row, clear the rest (SUMMARY_JUMP.md)
    };
}
