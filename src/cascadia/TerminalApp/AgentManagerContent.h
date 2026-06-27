// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster: content for the pinned, leftmost "Agent Manager" tab — the C1 "Linked
// Lenses" UI (DESIGN §9). Three regions over ONE shared model (the SessionRegistry):
//   * Triage Board (top)        — sessions as cards in hook-driven state columns
//   * Explorer Tree (bottom-left) — the M working directories -> their N sessions
//   * Flight Plan (bottom-right) — the selected session's prompt queue + Autopilot
// with bidirectional selection sync. Built imperatively (no IDL/XAML markup, like
// ScratchpadContent). The views are snapshot-driven: on any registry change we rebuild
// from SessionRegistry::Snapshot() on the UI thread.

#pragma once
#include "winrt/TerminalApp.h"
#include "BasicPaneEvents.h"
#include "AgentMaster/SessionModels.h"

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h> // Popup (the path-picker drop-down)

#include <vector>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Agentmaster
{
    class SessionRegistry;
}

namespace winrt::TerminalApp::implementation
{
    class AgentManagerContent : public winrt::implements<AgentManagerContent, IPaneContent>, public BasicPaneEvents
    {
    public:
        AgentManagerContent();
        ~AgentManagerContent(); // Agentmaster (M9): detach our observer from the shared registry

        // Wiring from the page (called right after construction).
        void SetRegistry(std::shared_ptr<::Agentmaster::SessionRegistry> registry);
        void SetSpawnHandler(std::function<void(winrt::hstring, winrt::hstring)> handler); // (workingDir, title)
        void SetActivateHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> jump to tab
        // Agentmaster (eager-init / "Activate Tab"): (sessionId) -> start a DORMANT session's claude IN
        // PLACE (no focus change) — distinct from SetActivateHandler ("Jump to Tab", which switches to it).
        void SetActivateDormantHandler(std::function<void(winrt::hstring)> handler);
        // Agentmaster (eager-init / "Activate All Tabs"): (allWindows) -> wake every dormant managed tab.
        // allWindows=false => this window only; true => this window + fan out to every other window.
        void SetActivateAllHandler(std::function<void(bool)> handler);
        // Agentmaster (Linked Lenses): report a pointer enter/leave on a managed session's board card
        // / tree row (id, entering). The PAGE owns the effective-hover bookkeeping (so it can't desync
        // from a missed PointerExited on a keyboard tab-switch) and pills that session's terminal tab
        // while the Manager tab is active — a live preview that follows the mouse, falling back to the
        // selected session.
        void SetHoverSessionHandler(std::function<void(winrt::hstring, bool)> handler);
        void SetArchiveHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> Close (shut down, keep the record so it stays resumable in Sessions; FAVORITES.md)
        void SetRestoreHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> re-launch (resume) a closed session
        // Agentmaster: the Launch box accepts EITHER a working dir OR a session id. A FOUND session id
        // turns the launch button into "Resume session" (resume the conversation) and reveals a "Fork"
        // button (fork it into a new conversation). Both resolve (dir, title) in the content, so the
        // page just forwards to _ResumeSessionFromDisk / _ForkSessionFromDisk.
        void SetResumeSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler); // (sessionId, dir, title)
        void SetForkSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler); // (sessionId, dir, title)
        // Agentmaster (Triage Board / Explorer-tree session menu — mirror the WT tab's "Restart session"
        // / "Fork session"). Restart: (sessionId) -> rebuild the live ConPTY connection in place (the
        // page does it local-first, then fans out to the hosting window). Fork: (sessionId) -> the
        // kind-aware fork the WT tab menu uses (Claude --fork-session / Codex `codex fork`), opening the
        // fork tab in the acting window. Both are kind-agnostic at this seam — the page branches.
        void SetRestartSessionHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> restart a managed session's connection in place
        void SetForkManagedSessionHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> fork a managed session (kind-aware), like the WT tab's "Fork session"
        void SetRenameHandler(std::function<void(winrt::hstring, winrt::hstring)> handler); // (sessionId, newTitle) -> rename in the registry + retitle the WT tab (the one title)
        // Agentmaster: adopt an EXTERNAL (observe-only) claude from the Explorer Tree's EXTERNAL scope.
        // (pid, workingDir) -> the page resolves the conversation id from the transcript and resumes it
        // into a NEW managed, controllable tab (`claude --resume <id>`), or launches fresh if it has no
        // transcript. The original external process is left running (we never inject into / kill it).
        // (pid, cwd, fork): fork==true branches the conversation into a NEW transcript (safe while the
        // original is live); fork==false resumes the same conversation (true take-over). The user picks
        // in the Adopt dialog; an external with no transcript launches fresh either way.
        void SetAdoptExternalHandler(std::function<void(uint32_t, winrt::hstring, bool)> handler);
        // Agentmaster (Codex-launch): the EXTERNAL-tree menu for a Codex row. (pid, cwd, adopt, fork):
        // adopt=true brings that codex's rollout under management (fork=true => `codex fork` into a NEW
        // rollout [safe while the original is live]; fork=false => `codex resume` the same); adopt=false
        // launches a fresh managed Codex in cwd (fork ignored). Routes to _AdoptExternalCodex / _SpawnCodexSession.
        void SetCodexLaunchHandler(std::function<void(uint32_t, winrt::hstring, bool, bool)> handler);
        // Agentmaster: the set of session ids hosted in THIS window (the page's _claudeTabs).
        // Used by the Explorer Tree's LOCAL scope to show only this window's sessions; GLOBAL
        // ignores it and shows every window's sessions (the whole process-wide registry).
        void SetLocalScopeProvider(std::function<std::unordered_set<std::wstring>()> provider);
        // Agentmaster (focus-steal fix): returns true iff this Manager's hosting window is the OS
        // FOREGROUND window. _Refresh re-focuses the keyboard-focused board card/row after a rebuild,
        // and in XAML Islands Control.Focus() escalates to Win32 activation of the island's host window
        // — so a refresh that fires on a BACKGROUND Manager window (the Triage Board sitting behind a
        // Claude tab you're working in, in another window) would yank the OS foreground to the Manager
        // on every ~2s observer/registry tick. The page wires this to GetForegroundWindow()==hwnd; when
        // backgrounded the user isn't keyboard-navigating this board, so the focus-restore is skipped.
        void SetWindowForegroundProvider(std::function<bool()> provider);
        void SetPauseHandler(std::function<void(bool)> handler); // global Autopilot Pause-all
        void SetConfirmHandler(std::function<void(winrt::hstring, bool)> handler); // SemiAuto confirm/skip
        void SetSettings(const ::Agentmaster::AppSettings& settings); // seed the cog dialog's current values
        void SetSettingsHandler(std::function<void(::Agentmaster::AppSettings)> handler); // persist on Save
        // Agentmaster (cross-window settings broadcast): adopt GLOBAL settings that were changed in
        // ANOTHER window (the cog Save, or the Explorer-Tree / Triage-Board sort toggle) and re-apply the
        // bits THIS window renders live — refresh _appSettings (so future spawns use the latest globals)
        // + repaint the tree/board sort toggles + re-sort the views. Unlike SetSettings it does NOT
        // re-seed the Launch cwd box (that would stomp in-progress typing) and it forces an immediate
        // _Refresh. The engine's settings sink marshals onto this UI thread before calling it; the
        // SOURCE window is excluded by BroadcastSettingsChanged (it already applied the change itself).
        void ApplyExternalSettings(const ::Agentmaster::AppSettings& settings);
        // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): the "Reopen Windows (N)" recover
        // button's action — reopen saved windows that are NOT currently open (the runtime analog of the
        // WindowEmperor's startup reopen loop). The content computes N itself
        // (::Agentmaster::RecoverableWindows) and shows the button only when N>0.
        void SetReopenWindowsHandler(std::function<void()> handler);
        // Agentmaster (Sessions page; SESSIONS.md / FAVORITES.md): the Manager's "Sessions" button opens
        // the full-window browser over EVERY on-disk Claude Code session — the sole history view.
        void SetOpenSessionsHandler(std::function<void()> handler);
        // Agentmaster (Sessions page; SESSIONS.md): the Settings cog's "Reset hidden sessions"
        // button — clear the user's "Hide from list" set (AppSettings.hiddenSessionIds). The page
        // (TerminalPage) owns the list + the Sessions browser, so the cog just fires the action there.
        void SetResetHiddenSessionsHandler(std::function<void()> handler);
        // Agentmaster (updater; Updater.h): "Update now" closes the app gracefully
        // (TerminalPage::RequestQuit) so the embedded installer can Add-AppxPackage the new build and
        // relaunch. Fired only AFTER the installer has been launched detached.
        void SetQuitForUpdateHandler(std::function<void()> handler);
        // Agentmaster: the Explorer Tree's "refresh" button (after the sort toggle) — reload the data
        // for the CURRENT scope. The content redraws immediately; this fires so the page can force the
        // Fleet Observer to re-survey now (re-enrich the registry + recompute the External census)
        // instead of waiting for the next tick. Optional — unwired, the button is a plain redraw.
        void SetRefreshHandler(std::function<void()> handler);
        // Agentmaster: force a UI redraw (board/tree/plan) from the current data — the page calls this
        // after an out-of-band reload (the observer survey lands asynchronously) so fresh data shows.
        void RefreshNow();
        // Agentmaster (Linked Lenses — the per-tab -> Manager sync): select a managed session in the
        // lens from OUTSIDE. The page calls this when the user switches to that session's terminal tab,
        // so returning to the Manager tab shows the session you were just in selected (board card + tree
        // row highlighted + its Flight Plan). Equivalent to a single-click on the session's board card;
        // a no-op when the id is empty or already selected. Marshal to the UI thread is the caller's job.
        void SelectSession(winrt::hstring id);
        // Agentmaster (Linked Lenses — selection VISIBILITY sync): scroll the currently-selected board
        // card / Explorer-tree row into view. The page calls this when the user switches TO the Manager
        // tab: while it was hidden the selection followed the user's tab switches (SelectSession), so the
        // highlighted card can be scrolled off-screen on return — this synchronizes its visibility with
        // its highlight. A no-op when nothing managed is selected or the card/row isn't currently shown.
        void BringSelectedIntoView();
        // Agentmaster (Linked Lenses): the currently-selected managed session id (the board card /
        // tree row selection), or empty. The page reads it live to decide which tab wears the
        // "selected/active" pill when nothing is hovered.
        winrt::hstring SelectedSessionId() const noexcept { return winrt::hstring{ _selectedId }; }

        // Agentmaster (Fleet Observer O6; OBSERVER.md §11c): the External (WindowsTerminal) claude
        // census — observe-only sessions the observer detected in a real Windows Terminal (NOT our
        // tabs, NO registry session, NO Flight Plan). The page's _ObserverProbe pushes the observer's
        // External() table here each tick; the content shows them as a collapsible "External (N)"
        // board group. Diffs against the current list, so an unchanged push is a no-op (no rebuild).
        void SetExternalClaudes(std::vector<::Agentmaster::ExternalClaudeRow> rows);

        // Agentmaster (M10; PERSISTENCE.md §13): the per-window Manager LENS (selection / scope /
        // selected prompt / collapsed dirs / splitter sizes). GetManagerState reads it;
        // SetManagerState seeds it on restore (re-applies the splitter sizes, then refreshes);
        // SetLensChangedHandler installs a callback the content fires — carrying the current lens —
        // whenever the lens mutates, so the hosting window debounce-saves its window record.
        ::Agentmaster::ManagerState GetManagerState() const;
        void SetManagerState(const ::Agentmaster::ManagerState& state);
        void SetLensChangedHandler(std::function<void(::Agentmaster::ManagerState)> handler);

        // IPaneContent
        winrt::Windows::UI::Xaml::FrameworkElement GetRoot();
        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);
        winrt::Windows::Foundation::Size MinimumSize();
        void Focus(winrt::Windows::UI::Xaml::FocusState reason = winrt::Windows::UI::Xaml::FocusState::Programmatic);
        void Close();
        winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(BuildStartupKind kind) const;

        winrt::hstring Title(); // "Agent Manager", or "Agent Manager Dev" on the AgentmasterDev package (defined in the .cpp — needs ProfileBootstrap)
        uint64_t TaskbarState() { return 0; }
        uint64_t TaskbarProgress() { return 0; }
        bool ReadOnly() { return false; }
        winrt::hstring Icon() const;
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept { return nullptr; }
        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush();

        // See BasicPaneEvents for most generic event definitions

    private:
        void _BuildLayout();
        void _Refresh();
        void _RebuildBoard(const std::vector<::Agentmaster::SessionInfo>& sessions);
        void _RebuildTree(const std::vector<::Agentmaster::SessionInfo>& sessions);
        // Agentmaster: the Explorer Tree's EXTERNAL scope — render the Fleet Observer's observe-only
        // external claudes (_externalClaudes) grouped by working dir, each row carrying an Adopt / Open
        // New Session Here / Bring Window To Front right-click menu. Reads the member table, not the
        // registry snapshot.
        void _RebuildExternalTree();
        void _RebuildPlan(const std::vector<::Agentmaster::SessionInfo>& sessions);
        // Agentmaster: select an EXTERNAL (observe-only) row in the tree's EXTERNAL scope -> the
        // Flight Plan shows that conversation's prompts READ-ONLY (we host no ConPTY, so we can't
        // drive it). _LoadExternalPlan reads the transcript prompts on a background thread (one-shot
        // per id) and posts them back via the dispatcher; _RebuildExternalPlan renders them.
        // `kind` (Claude vs Codex) + `rolloutPath` (Codex's date-sharded .jsonl, carried on the row)
        // select the right read-only-plan reader: Claude -> ReadTranscriptInfo(cwd,id); Codex ->
        // ReadCodexRolloutInfo(rolloutPath). (Phase C1.)
        void _SelectExternal(const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& title, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath);
        void _LoadExternalPlan(const std::wstring& sessionId, const std::wstring& cwd, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath);
        void _RebuildExternalPlan();
        // Agentmaster: pin the Flight Plan's scroll to the BOTTOM the first time a given subject is
        // viewed (the latest SENT message + the UPCOMING queue sit at the bottom of the list, so a
        // freshly-opened plan defaults to "where the conversation left off"). `subjectKey` identifies
        // what the list currently shows — a managed session id, or "x:<id>" for an external's
        // read-only conversation; empty means nothing scrollable. We only scroll when the subject
        // CHANGES from the last one we pinned (_planAutoScrolledFor): a mere _Refresh on the same
        // subject (a background state change, an OSC title float) must NOT yank the user's scroll
        // position. Deferred to a clean tick + UpdateLayout so ScrollableHeight is valid (the list was
        // just (re)populated this frame).
        void _PinPlanToBottomOnSubjectChange(const std::wstring& subjectKey);

        // Agentmaster: Explorer Tree scope toggle, cycling LOCAL -> GLOBAL -> EXTERNAL (this
        // window's tabs / all windows / observe-only externals). _ToggleTreeScope advances the mode
        // + rebuilds (and clears selection when entering EXTERNAL so the Flight Plan reads
        // nothing-selected); _UpdateTreeScopeButton refreshes the toggle button's label.
        void _ToggleTreeScope();
        void _UpdateTreeScopeButton();
        // Agentmaster: Explorer Tree sort toggle, after the scope toggle, cycling NEWEST -> OLDEST ->
        // MOST ACTIVE -> A-Z. The sort is a GLOBAL setting (AppSettings::treeSort): _CycleTreeSort
        // advances it, persists it through the settings sink (so every window + a relaunch pick it
        // up), and rebuilds; _UpdateTreeSortButton refreshes the toggle button's label.
        void _CycleTreeSort();
        void _UpdateTreeSortButton();
        // Agentmaster: Triage Board sort toggle, after the board's LOCAL/GLOBAL scope toggle, cycling
        // MOST ACTIVE -> NEWEST -> OLDEST -> A-Z (the tree's set minus BY PID; default MOST ACTIVE). A
        // SEPARATE GLOBAL setting from treeSort (AppSettings::boardSort): _CycleBoardSort advances +
        // persists it through the settings sink (shared by every window, adopted on relaunch) and
        // re-sorts the board; _UpdateBoardSortButton refreshes the toggle's label.
        void _CycleBoardSort();
        void _UpdateBoardSortButton();

        void _SelectSession(const std::wstring& id);
        void _ClearSelection(); // Agentmaster: the board header's "Clear" button — deselect the managed OR external selection
        // Agentmaster (Linked Lenses): forward a pointer enter/leave on a managed card/row to the
        // page (id, entering). Stateless here — the page does the effective-hover matching (which
        // absorbs the enter-B-before-leave-A ordering when sliding between rows) so the content holds
        // no hover state to desync.
        void _ReportHover(const std::wstring& id, bool entering);
        void _SetScope(const std::wstring& dir);
        std::optional<::Agentmaster::SessionInfo> _Selected(const std::vector<::Agentmaster::SessionInfo>& sessions) const;

        // Agentmaster (M10): fire _lensChangedHandler with the current lens (GetManagerState) so the
        // window debounce-saves its record; _ApplyLayoutToTracks pushes _layout's fractions into the
        // live row/column definitions (used when seeding a restored per-window layout).
        void _NotifyLensChanged();
        void _ApplyLayoutToTracks();

        // Action-bar handlers (operate on _selectedId / _selectedPromptId).
        void _OnLaunch();
        // Agentmaster: "Create & Launch" — ensure the typed working dir exists, creating it (and any
        // missing parents) when it's a not-yet-existing absolute path. Returns false (and warns) if the
        // create failed, so the caller aborts the launch. An existing dir is a no-op pass-through.
        bool _EnsureLaunchDirExists(const std::wstring& dir);
        void _OnAddPrompt();
        void _OnSendNow(); // the "!" icon — confirms, then _DoSendNow
        void _DoSendNow(); // actual inject, after the Send-now confirm
        // Agentmaster: after queueing / sending a prompt, return keyboard focus to the compose box
        // so the user can keep typing the next one (clicking the icon button stole focus). A no-op
        // if the box isn't built / present.
        void _FocusPromptBox();
        // Agentmaster (prompt history — shell/REPL idiom): on the live DRAFT, pressing Up once the caret
        // reaches the FIRST VISUAL ROW of the compose box recalls the selected session's previously SENT
        // prompts (newest first); while BROWSING history Up/Down walk older/newer FREELY (no caret gate,
        // Esc cancels back to the draft) and Down off the newest entry restores the in-progress draft (the
        // draft itself only ever moves the caret on Down — nothing is newer). _BuildPromptHistory snapshots
        // the sent prompts (Flight + Typed), newest first, consecutive-duplicate-collapsed; _ApplyPromptHistoryText
        // writes a recalled body (guarded so its TextChanged doesn't reset navigation) + parks the caret at
        // the end; _ResetPromptHistory leaves navigation (called when the user edits, the box is cleared, or
        // the selection changes). _PromptCaretOnFirstRow gates the enter-history trigger (visual row, so it
        // respects word-wrap).
        std::vector<std::wstring> _BuildPromptHistory() const;
        void _ApplyPromptHistoryText(const std::wstring& text);
        void _ResetPromptHistory();
        bool _PromptCaretOnFirstRow() const;
        void _OnMovePrompt(int delta);
        void _OnDeletePrompt();
        void _OnAutopilotChanged(int index);
        // Agentmaster: the FLIGHT-PLAN-header Autopilot toggle. _CycleAutopilot advances the
        // selected session's mode (Off -> Semi-auto -> Full -> Off); _UpdateAutopilotButton paints
        // the button's colored state dot + label (dim/disabled when no live session is selected).
        void _CycleAutopilot();
        void _UpdateAutopilotButton(::Agentmaster::AutopilotMode mode, bool enabled);
        // Agentmaster: the Flight-Plan pane's two-state [Summary | Flight Plan] tab toggle (its top
        // line). _SelectPlanPaneTab(summary) sets the GLOBAL choice (AppSettings::flightPlanShowsSummary),
        // persists + broadcasts it through the settings sink (the treeSort idiom), and re-paints;
        // _UpdatePlanPaneTab reflects the current choice — accents the selected segment and shows that
        // tab's content (Summary host vs the Flight Plan body), no-op while the controls are null.
        void _SelectPlanPaneTab(bool summary);
        void _UpdatePlanPaneTab();
        // Agentmaster (Summary tab): render the selected managed Claude session's summary — the SAME
        // RenderSessionSummaryBox the Sessions page / per-tab overlay use — into the Summary tab, with
        // the user MESSAGES newest-first and the whole box on one inner scrollbar. _RefreshSummaryTab
        // (cheap; called on every plan refresh + tab switch) resolves the subject and renders from the
        // single-entry cache or kicks the off-thread analyze; _LoadSummaryForSession does the background
        // analyze+render; _RenderSummaryBox splits the box text into mono lines + full-width rules
        // (mirrors the Sessions page's SessAppendSummaryBox).
        void _RefreshSummaryTab();
        void _LoadSummaryForSession(const std::wstring& id, const std::wstring& dir, int64_t mtime);
        void _RenderSummaryBox(const std::wstring& text);
        void _OnSaveTemplate();
        void _OnApplyTemplate(bool toWholeDirectory);
        void _RefreshTemplateCombo();

        // Launch path-picker drop-down (a Popup anchored under the cwd box). Opens on user
        // focus of the box; shows the recent dirs (excluding the current one) over the
        // subfolders of the current path; clicking a row drives the box and re-lists. When the
        // box holds a bare token with NO root path (e.g. "agent"), the recent section instead
        // becomes case-insensitive fuzzy MATCHES ranked by Levenshtein closeness (closest first).
        void _OpenPathPicker();
        void _ClosePathPicker();
        void _RebuildPathPicker();
        void _PickPath(const std::wstring& dir);
        void _NormalizeCwdBox(); // platform-sensitive NormPath of the cwd box (on commit / blur / pick / launch)
        // Agentmaster: validate the Launch box's content as a working dir OR a session id, painting the
        // underline (green = a FOUND session id; red = an unknown id or a missing dir) and enabling the
        // launch button (disabled on red) + the Fork button (shown only for a found session id).
        void _ValidateLaunchBox();
        // Agentmaster (Codex-launch): the launch bar's Claude<->Codex agent toggle. _UpdateLaunchAgentButton
        // repaints it from _launchCodex; clicking flips _launchCodex and re-validates (the Launch button text
        // + the session-id resume/fork affordances are Claude-only — Codex launches a directory only).
        void _UpdateLaunchAgentButton();
        void _OnForkFromBox(); // the Fork button (visible for a found session id) -> _forkSessionHandler
        bool _ResolveSessionDirTitle(const std::wstring& id, std::wstring& dir, std::wstring& title); // registry first, transcript cwd fallback
        void _PushRecentDir(const std::wstring& dir);
        // Recent/known working dirs for the path-picker. `current` is the box text (excluded from
        // the result). A non-empty `query` (a bare token typed with no root path) switches the
        // result from plain MRU order to case-insensitive fuzzy matches ranked by Levenshtein
        // closeness (closest first); an empty query keeps the historical MRU order.
        std::vector<std::wstring> _CollectRecentDirs(const std::wstring& current, const std::wstring& query) const;
        winrt::Windows::UI::Xaml::Controls::Button _MakePathRow(const std::wstring& fullPath, const winrt::hstring& glyph, const winrt::hstring& displayText, const std::wstring& branch = {}); // `branch` (RECENT rows) appends "— <branch>"
        // Agentmaster: a "GIT WORKTREES" row — "<name> — <path> — <branch>". Mirrors _MakePathRow
        // (transparent + focus-neutral); a click drills the launch box into the worktree.
        winrt::Windows::UI::Xaml::Controls::Button _MakeWorktreeRow(const std::wstring& fullPath, const std::wstring& name, const std::wstring& branch, bool isCurrent);
        // Agentmaster: the "Browse…" row pinned to the TOP of the path-picker (the first option) — a
        // native folder dialog whose pick both fills the box AND joins the recents (_BrowseForLaunchDir).
        winrt::Windows::UI::Xaml::Controls::Button _MakeBrowseRow();
        void _BrowseForLaunchDir();

        // Build one session card for the Triage Board.
        winrt::Windows::UI::Xaml::Controls::Button _MakeCard(const ::Agentmaster::SessionInfo& s);
        // Agentmaster (Waiting-for-you countdown bar): drain every tracked card's 1px bottom bar in
        // place (ScaleX = fraction of the waiting window still remaining) and start/stop the 1s timer
        // that drives it depending on whether any bar is tracked. _RebuildBoard re-seeds the tracks.
        void _UpdateCardProgress();
        void _SyncProgressTimer();
        // Agentmaster: assemble one Triage Board column. With `fill` (default) the column fills the
        // board height with a pinned `header` over a vertically-scrolling `cards` list, so a tall
        // column (e.g. a large External census) scrolls within the board instead of clipping past
        // the bottom edge (the board's own ScrollViewer has vertical scroll disabled). With
        // `fill=false` the box hugs its content (a collapsed column: header only, nothing to scroll).
        // `columnKey` (when `fill`) registers the new card ScrollViewer in _boardColumnScrollers and,
        // with `restoreOffset > 0`, re-applies that vertical offset once the column lays out — so a
        // rebuild preserves the user's scroll instead of snapping to the top (see _RebuildBoard).
        winrt::Windows::UI::Xaml::Controls::Border _MakeBoardColumn(
            const winrt::Windows::UI::Xaml::UIElement& header,
            const winrt::Windows::UI::Xaml::UIElement& cards,
            bool fill = true,
            const std::wstring& columnKey = {},
            double restoreOffset = 0.0);
        // Agentmaster (O6): build the "External (N)" board column (real-WindowsTerminal claudes,
        // observe-only); empty if there are none. The header toggles _externalCollapsed; each card is
        // non-interactive with an (currently disabled) Adopt seam for future external-session restore.
        // `restoreOffset` preserves the column's scroll across a rebuild (see _RebuildBoard).
        winrt::Windows::UI::Xaml::Controls::Border _MakeExternalColumn(double restoreOffset = 0.0);
        winrt::Windows::UI::Xaml::Controls::Button _MakeExternalCard(const ::Agentmaster::ExternalClaudeRow& ex);

        // Draggable pane splitters (resize + on-hover cursor + persisted sizes).
        // `vertical` == a vertical bar dividing the bottom COLUMNS (↔, resizes Tree/Plan);
        // `!vertical` == a horizontal bar dividing the root ROWS (↕, resizes Board/Bottom).
        winrt::Windows::UI::Xaml::Controls::Border _MakeSplitter(bool vertical);
        void _OnSplitterPressed(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e, bool vertical);
        void _OnSplitterMoved(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e, bool vertical);
        void _OnSplitterReleased(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        // Explorer-tree session actions: right-click context menu (Rename / Archive, then Open New
        // Session Here in the row's cwd as the last item) + double-click to activate.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakeSessionMenu(const std::wstring& id, const std::wstring& cwd);
        // Agentmaster: the EXTERNAL-tree row right-click menu. Adopt -> resume the external's conversation
        // into a managed, controllable tab (via _adoptExternalHandler); Open New Session Here -> spawn a
        // managed session in the external's cwd (an independent conversation); Bring Window To Front (last)
        // -> surface the external's HOSTING window (_BringExternalToFront). Observe-only externals carry no
        // registry session, so this menu acts on the row's facts, not a session id. For a CODEX row
        // (kind=Codex, observe-only in Phase C1) Adopt + Open New Session Here are omitted (those drive
        // CLAUDE) and an "Open rollout file" item is offered instead; Bring Window To Front (agent-
        // agnostic window activation) stays.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakeExternalTreeMenu(const ::Agentmaster::ExternalClaudeRow& ex);
        // Agentmaster: the EXTERNAL menu's Bring Window To Front — resolve the row's host facts
        // (hostPid / sessionId / title) from the latest _externalClaudes snapshot, then surface its
        // hosting window on a BACKGROUND thread (ProcessInspect::BringClaudeWindowToFront:
        // restore-if-minimized + foreground + best-effort WT tab select — the worker reads the
        // transcript for the custom title + prompt corpus the tab pick matches against).
        // Observe-only safe: window activation, never input.
        void _BringExternalToFront(uint32_t pid, const std::wstring& cwd);
        // Flight-Plan message right-click menu: per-prompt Move up / Move down / Delete (only on
        // UPCOMING rows — a sent row can't be reordered) + Archive session (always). Queue ops act
        // on `promptId` (the right-clicked row), not the current selection.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakePromptMenu(const std::wstring& promptId, bool upcoming);
        void _OnRenameSession(const std::wstring& id); // begin an in-place rename of the row
        void _CommitRename(); // apply the in-place editor's text to the session title
        void _CancelRename(); // discard the in-place editor (Esc)
        void _RequestArchive(const std::wstring& id); // route to the page's Close seam (presents the confirm + closes the tab; keeps the record so it stays resumable in Sessions — FAVORITES.md)

        // Settings cog: an in-content modal overlay (NOT a ContentDialog — a text box inside a
        // ContentDialog receives no keypresses in XAML Islands; see the _renameBox note). Built
        // into the main visual tree so its TextBoxes work; shown/hidden by toggling Visibility.
        void _BuildSettingsOverlay();
        void _ShowSettings(); // populate controls from _appSettings, then reveal the overlay
        void _HideSettings();
        void _SaveSettings(); // read controls -> _appSettings -> _settingsSink, then hide

        // Agentmaster (ENV_VARS.md): the cog's "Environment variables" area — a Global / Per-directory
        // two-tab editor (multi-line NAME=VALUE, one per line) feeding ResolveSessionEnv at spawn. Built
        // once into the settings panel; _Load seeds it on cog open; _Save writes AppSettings.env (global)
        // + flushes the per-directory draft to dir-env.json. The per-dir draft is held in memory until
        // Save so Cancel discards (the cog's form semantics), mirroring _appSettings for the global side.
        void _BuildEnvVarsArea(const winrt::Windows::UI::Xaml::Controls::StackPanel& panel);
        void _LoadEnvVarsArea(); // seed global text from _appSettings.env + load dir-env.json into the draft
        void _SaveEnvVarsArea(); // global -> _appSettings.env; flush _dirEnvDraft -> SaveDirEnv
        void _SwitchEnvTab(bool perDir); // toggle Global <-> Per-directory panels + restyle the tab buttons
        void _RebuildEnvDirList(); // (re)populate the per-dir selector from session dirs ∪ dir-env keys, filtered
        void _SelectEnvDir(const std::wstring& normKey, const std::wstring& displayPath); // load a dir's env into the per-dir editor
        // Lex an env editor's text -> recolor its wrapping border + write its status line (the SAME
        // green/amber/red palette _ValidateLaunchBox uses). `perDir` picks which editor/border/status.
        void _RefreshEnvLex(bool perDir);

        // Agentmaster (updater; Updater.h): the Settings cog's UPDATES section. Runs a GitHub
        // release check OFF the UI thread (WinHTTP), then marshals back. interactive==true (the
        // "Check for updates" button): show the prompt (Update now / Postpone 3·7·30 days / Skip
        // this version / Not now) on a found update, else report "up to date" / "couldn't reach
        // GitHub". interactive==false (the silent check kicked when the cog opens): only set the
        // "vX.Y.Z available!" label (dark green) when a newer release exists; stay quiet otherwise.
        // "Update now" launches the embedded am-update installer (detached) and asks the app to quit
        // via _quitForUpdateHandler so the package isn't in use while it upgrades + relaunches.
        void _CheckForUpdates(bool interactive);

        // Agentmaster (native-exe-only policy): the "Claude not detected" modal — shown when a
        // launch/fork is attempted with no native claude.exe (::Agentmaster::ClaudeAvailable() false).
        // Carries why (native required, the Node CLI is unsupported), the install path (`claude install`
        // + a docs link), a Browse… (locate claude.exe), and Re-check. Built into the tree like the
        // settings overlay (toggled by Visibility), so its content behaves in XAML Islands.
        void _BuildClaudeMissingOverlay();
        void _ShowClaudeMissing();
        void _HideClaudeMissing();
        // Browse for claude.exe (IFileOpenDialog, .exe filter) -> persist it as the claudeExePath
        // override (through _settingsSink) + ::Agentmaster::RefreshClaudeExe; hides the missing-overlay
        // and re-validates the launch box if claude is now available. Runs the Win32 modal OFF the click
        // tick (the XAML-Islands deferral rule). `fromSettings` true => also refresh the Settings fields.
        void _BrowseForClaudeExe(bool fromSettings);

        // Agentmaster (FAVORITES.md): the Archived overlay/page + their methods were removed — the
        // Sessions browser is the sole history view (closed sessions stay there, resumable, starred).
        // Agentmaster: keep-awake control — a TRI-MODE button (Off / Always / While-Running).
        // _CycleKeepAwake advances the mode (Off -> Always -> WhileRunning -> Off); _RefreshKeepAwakeHold
        // computes the DESIRED execution-state hold for the current mode (Always => always hold; WhileRunning
        // => hold iff a live session is actively Running) and, on a transition only, calls SetThreadExecutionState
        // (ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED to hold, ES_CONTINUOUS alone to release). It is
        // also called from _Refresh so WhileRunning tracks the fleet live; pass the already-fetched snapshot to
        // avoid a second Snapshot() copy. _UpdateKeepAwakeButton repaints the button to reflect mode + hold.
        void _CycleKeepAwake();
        void _RefreshKeepAwakeHold(const std::vector<::Agentmaster::SessionInfo>* sessions = nullptr);
        void _UpdateKeepAwakeButton();
        // Agentmaster (M10 Increment 3): the "Reopen Windows (N)" recover button. _UpdateReopenButton
        // sets its label to the recoverable-window count and shows it only when N>0; _OnReopenWindows
        // confirms, then fires _reopenWindowsHandler (the page reopens every not-currently-open record).
        void _UpdateReopenButton();
        void _OnReopenWindows();
        // Agentmaster (eager-init / "Activate All Tabs (N)"): set the button's label to THIS window's
        // dormant managed-tab count and show it only when N>0 (the hide-when-idle idiom, like the reopen
        // button); refreshed on the _Refresh cadence. _OnActivateAllTabs decides the scope: if other
        // windows ALSO have dormant tabs it prompts (This window / All windows), else it just wakes this
        // window's. Both counts come from _DormantCounts (snapshot + the local-scope provider).
        void _UpdateActivateAllButton();
        void _OnActivateAllTabs();
        // {thisWindowDormant, fleetDormant} — live MANAGED sessions whose claude hasn't started yet
        // (IsSessionDormant), split by whether they are hosted in THIS window (per _localScopeProvider).
        std::pair<int, int> _DormantCounts() const;
        // A buttons-only confirm (XAML-Islands-safe) for consequential actions; runs onYes on accept.
        void _Confirm(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, std::function<void()> onYes);
        // Agentmaster: a buttons-only THREE-way choice (XAML-Islands-safe): primary / secondary / Cancel.
        // Used by Adopt to offer Fork-a-copy (safe) vs Resume-anyway (take-over). Primary is the default
        // (the safe choice); Cancel does nothing. onPrimary / onSecondary run on the respective click.
        void _ConfirmChoice(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, const winrt::hstring& secondary, std::function<void()> onPrimary, std::function<void()> onSecondary);

        std::shared_ptr<::Agentmaster::SessionRegistry> _registry;
        // Agentmaster (M9): our observer's token on the shared (process-wide) registry, so this
        // window's lens detaches cleanly on teardown instead of dangling a strong
        // DispatcherQueue ref there. (An ::Agentmaster::ObserverToken == uint64_t; kept as
        // uint64_t to avoid including SessionRegistry.h in this header.)
        uint64_t _observerToken{ 0 };
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr };
        // Agentmaster (Waiting-for-you "unread" model): a 30s timer that re-runs _Refresh() so the
        // TIME-derived card adornments (the ⚡ "still cached" hint + the "ago" timing) stay current in
        // quiet periods with no registry events. Stopped in the destructor.
        winrt::Windows::UI::Xaml::DispatcherTimer _cardRefreshTimer{ nullptr };

        // Agentmaster (Waiting-for-you countdown bar): the live 1px bottom bars to drain. Each holds the
        // bar's ScaleTransform (ScaleX = fraction of the waiting window still remaining, origin LEFT) +
        // the countdown anchor + the full duration. Re-seeded by _RebuildBoard (cleared at its top,
        // pushed by _MakeCard); _progressTimer ticks ~1s and sets each ScaleX in place (no rebuild —
        // a cheap render-transform write). The timer runs only while >=1 bar is tracked.
        struct CardProgress
        {
            // The bar element (its RenderTransform is a ScaleTransform whose ScaleX = fraction). Stored
            // as the Border rather than the ScaleTransform so this header needs no winrt Media include
            // (which collides ::IInspectable with winrt's in this TU); _UpdateCardProgress resolves the
            // transform in the .cpp where Media is in scope.
            winrt::Windows::UI::Xaml::Controls::Border bar{ nullptr };
            int64_t lastActivityUnixMs{ 0 }; // the turn's last activity (the countdown start)
            int64_t timeoutMs{ 0 }; // waitingForYouTimeoutMinutes * 60000 (the full bar duration)
        };
        std::vector<CardProgress> _cardProgress;
        winrt::Windows::UI::Xaml::DispatcherTimer _progressTimer{ nullptr };

        std::function<void(winrt::hstring, winrt::hstring)> _spawnHandler;
        std::function<void(winrt::hstring)> _activateHandler;
        std::function<void(winrt::hstring)> _activateDormantHandler; // Agentmaster (eager-init): "Activate Tab" -> start a dormant session's claude in place (no focus change)
        std::function<void(bool)> _activateAllHandler; // Agentmaster (eager-init): "Activate All Tabs" -> wake every dormant tab (allWindows=false this window only / true + fan out)
        std::function<void(winrt::hstring, bool)> _hoverSessionHandler; // Agentmaster (Linked Lenses): push a managed card/row pointer enter/leave (id, entering) so the page pills its tab
        std::function<void(winrt::hstring)> _archiveHandler;
        std::function<void(winrt::hstring)> _restoreHandler;
        std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> _resumeSessionHandler; // Agentmaster: launch box holds a FOUND session id -> resume it (id, dir, title)
        std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> _forkSessionHandler; // Agentmaster: launch box Fork -> fork the session id (id, dir, title)
        std::function<void(winrt::hstring)> _restartSessionHandler; // Agentmaster: Triage Board / Explorer-tree "Restart session" -> rebuild a managed session's connection in place (page does it cross-window)
        std::function<void(winrt::hstring)> _forkManagedSessionHandler; // Agentmaster: Triage Board / Explorer-tree "Fork session" -> kind-aware fork of a managed session (the WT tab menu's fork), opening the fork in the acting window
        std::function<void(winrt::hstring, winrt::hstring)> _renameHandler; // Agentmaster: Explorer-tree rename -> page (registry title + tab title in lockstep)
        std::function<void(uint32_t, winrt::hstring, bool)> _adoptExternalHandler; // Agentmaster: EXTERNAL-tree Adopt (pid, cwd, fork) -> page forks/resumes the external's conversation into a managed tab
        std::function<void(uint32_t, winrt::hstring, bool, bool)> _codexLaunchHandler; // Agentmaster (Codex-launch): EXTERNAL-codex (pid, cwd, adopt, fork): Adopt (adopt=true; fork picks fork/resume) / Open-New-Codex (adopt=false)
        std::function<std::unordered_set<std::wstring>()> _localScopeProvider; // Agentmaster: this window's hosted session ids (for the Explorer Tree LOCAL scope)
        std::function<bool()> _windowForegroundProvider; // Agentmaster (focus-steal fix): is this Manager's window the OS foreground window? Gates _Refresh's focus-restore so a background rebuild can't steal foreground.
        std::function<void(bool)> _pauseHandler;
        std::function<void(winrt::hstring, bool)> _confirmHandler;
        std::function<void(::Agentmaster::AppSettings)> _settingsSink;
        std::function<void()> _reopenWindowsHandler; // Agentmaster (M10): the "Reopen Windows" recover-button action
        std::function<void()> _openSessionsHandler; // Agentmaster (Sessions page): open the full-window global Sessions browser (TerminalPage-hosted)
        std::function<void()> _resetHiddenSessionsHandler; // Agentmaster (Sessions page): the Settings cog's "Reset hidden sessions" action — clear AppSettings.hiddenSessionIds (TerminalPage-side)
        std::function<void()> _quitForUpdateHandler; // Agentmaster (updater): "Update now" -> quit the app gracefully (page's RequestQuit) so the installer can replace it
        std::function<void()> _refreshHandler; // Agentmaster: Explorer Tree refresh -> page re-surveys the Fleet Observer (reload the current scope's data)
        std::function<void(::Agentmaster::ManagerState)> _lensChangedHandler; // Agentmaster (M10): push lens changes to the hosting window
        ::Agentmaster::AppSettings _appSettings{}; // current settings (seeded by SetSettings; edited via the cog)
        bool _globalPaused{ false };

        std::wstring _selectedId;
        std::wstring _scopeDir; // board filter: empty == all directories
        std::wstring _selectedPromptId;
        // Agentmaster: the EXTERNAL row selected in the tree's EXTERNAL scope (its resolved
        // conversation id + cwd + title), and the prompts read from its transcript for the read-only
        // Flight Plan. _externalPlanLoadedFor == the id whose prompts are loaded (empty while loading
        // or none selected). Mutually exclusive with _selectedId (a managed selection clears these).
        std::wstring _selectedExternalSessionId;
        std::wstring _selectedExternalCwd;
        std::wstring _selectedExternalTitle;
        ::Agentmaster::AgentKind _selectedExternalKind{ ::Agentmaster::AgentKind::Claude }; // Phase C1: which reader the read-only plan uses (Claude transcript vs Codex rollout)
        std::wstring _selectedExternalRolloutPath; // Codex: the rollout .jsonl path for the read-only plan (Claude leaves empty)
        std::wstring _externalPlanLoadedFor;
        std::vector<std::wstring> _externalPlanPrompts;
        // Agentmaster: the ONE session scope behind BOTH toggles — the Explorer Tree's 3-way cycle
        // (after the "EXPLORER TREE" title) and the Triage Board's 2-way LOCAL/GLOBAL (after the
        // "TRIAGE BOARD" title; the board has no External mode — it reads External as Global, and
        // a click there flips the shared scope to LOCAL).
        //   Local    == this window's sessions only (the page's _claudeTabs)
        //   Global   == every window's sessions (the whole process-wide registry)
        //   External == the Fleet Observer's observe-only external claudes (_externalClaudes), grouped
        //               by cwd; right-click a row for Open New Session Here / Adopt.
        // PERSISTED per window in the lens (ManagerState.treeScope) — no longer in-memory-only —
        // so a reopened window keeps its scope; both buttons reflect the one state.
        enum class TreeScope { Local, Global, External };
        TreeScope _treeScope{ TreeScope::Local };
        // Agentmaster: the ONE scope mutator behind both toggles — handles the External enter/leave
        // selection cleanup, reflects BOTH buttons, pushes the lens (persisted), and refreshes
        // (skippable when the caller refreshes itself, e.g. _OnRenameSession). _UpdateBoardScopeButton
        // paints the board toggle's LOCAL/GLOBAL label (External shows as GLOBAL).
        void _SetTreeScope(TreeScope scope, bool refresh = true);
        void _UpdateBoardScopeButton();
        std::unordered_set<std::wstring> _collapsedDirs;
        bool _suppressAutopilotEvent{ false };
        // Agentmaster (O6): the observer's External (WindowsTerminal) claudes, pushed by the page's
        // _ObserverProbe; rendered as a collapsible "External (N)" board group. _externalCollapsed
        // hides the cards (the header keeps the count).
        std::vector<::Agentmaster::ExternalClaudeRow> _externalClaudes;
        bool _externalCollapsed{ false };

        winrt::Windows::UI::Xaml::Controls::Grid _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _focusSink{ nullptr }; // Agentmaster: invisible, caret-less keyboard-focus sink so the pane-root key handlers (alt+left/right, ctrl+tab, shift+home) fire on tab switch — see Focus()
        winrt::Windows::UI::Xaml::Controls::StackPanel _boardHost{ nullptr }; // horizontal columns
        winrt::Windows::UI::Xaml::Controls::TextBlock _boardScope{ nullptr }; // Agentmaster: "[scope: <dir>]" — shown ONLY while a directory is scoped (the old "[all directories]" placeholder is gone; it was display-only)
        winrt::Windows::UI::Xaml::Controls::Button _showAllBtn{ nullptr }; // Agentmaster: the board's "Show all" — collapsed while already showing all (empty scope), shown once a dir is scoped
        winrt::Windows::UI::Xaml::Controls::Button _boardScopeBtn{ nullptr }; // Agentmaster: the board's LOCAL/GLOBAL toggle after the "TRIAGE BOARD" title — same state as _treeScopeBtn (External reads GLOBAL)
        winrt::Windows::UI::Xaml::Controls::Button _boardSortBtn{ nullptr }; // Agentmaster: the board's MOST ACTIVE/NEWEST/OLDEST/A-Z sort toggle after the scope toggle (global, persisted; AppSettings::boardSort, separate from _treeSortBtn)
        winrt::Windows::UI::Xaml::Controls::Button _boardRefreshBtn{ nullptr }; // Agentmaster: the board's ↻ refresh button after the sort toggle (re-scan + redraw the whole tab; twin of _treeRefreshBtn)
        winrt::Windows::UI::Xaml::Controls::Button _clearSelBtn{ nullptr }; // Agentmaster: the board's "Clear" button next to LOCAL/GLOBAL — deselect the current card/row; hidden while nothing is selected (synced by _RebuildBoard, like _showAllBtn)
        winrt::Windows::UI::Xaml::Controls::Button _treeScopeBtn{ nullptr }; // Agentmaster: the LOCAL/GLOBAL/EXTERNAL toggle after the "EXPLORER TREE" title
        winrt::Windows::UI::Xaml::Controls::Button _treeSortBtn{ nullptr }; // Agentmaster: the NEWEST/OLDEST/MOST ACTIVE/A-Z sort toggle after the scope toggle (global, persisted)
        winrt::Windows::UI::Xaml::Controls::Button _treeRefreshBtn{ nullptr }; // Agentmaster: the ↻ refresh button after the sort toggle (reload the current scope's data)
        winrt::Windows::UI::Xaml::Controls::StackPanel _treeHost{ nullptr };
        // Agentmaster: id -> the live board card / tree row Button, repopulated on every _Refresh
        // (cleared + refilled by _RebuildBoard / _RebuildTree). Used ONLY to RESTORE keyboard focus
        // onto the same card/row after a rebuild: _Refresh recreates every element on any registry
        // notification — including a title-only change — which would otherwise drop focus off the
        // clicked card (the selection highlight survives via _selectedId, the focused element does
        // not). The focused element is identified by its "b:<id>" / "t:<id>" Tag.
        std::unordered_map<std::wstring, winrt::Windows::UI::Xaml::Controls::Button> _boardCardsById;
        std::unordered_map<std::wstring, winrt::Windows::UI::Xaml::Controls::Button> _treeRowsById;
        // Agentmaster: column title (e.g. "Running", "External") -> that column's live card
        // ScrollViewer, repopulated on every _RebuildBoard. Used ONLY to PRESERVE each column's
        // vertical scroll offset across a rebuild: _RebuildBoard recreates the per-column ScrollViewers
        // from scratch (fresh => offset 0), so without capturing+restoring the offset, any _Refresh (a
        // select, a state/title change, an observer enrichment) would snap a scrolled column to the TOP
        // and lose the card the user just clicked. Captured before the clear, restored on Loaded.
        std::unordered_map<std::wstring, winrt::Windows::UI::Xaml::Controls::ScrollViewer> _boardColumnScrollers;
        // Agentmaster (scroll-jump fix): the DURABLE remembered scroll offset per column title — the
        // authoritative restore source, persisted ACROSS rebuilds (unlike the per-rebuild local capture
        // it replaced). Why a member: a refresh that lands before a PRIOR rebuild's restore-on-Loaded has
        // fired would read that rebuild's fresh, not-yet-restored ScrollViewer sitting at 0 and PERMANENTLY
        // lose the saved scroll (the "a click jumps the scroll to top" race when a background scanner/observer
        // refresh coincides with the user's click). The capture in _RebuildBoard updates this map only from a
        // LOADED ScrollViewer (FrameworkElement::IsLoaded) — a fresh SV's meaningless 0 never clobbers the
        // remembered offset, while a genuinely-scrolled-to-top loaded column records its real 0.
        std::unordered_map<std::wstring, double> _boardColumnOffsets;
        winrt::Windows::UI::Xaml::Controls::StackPanel _planHeaderHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _planListHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _planScroll{ nullptr }; // Agentmaster: hosts _planListHost — pinned to the bottom on first view of a subject (see _PinPlanToBottomOnSubjectChange)
        std::wstring _planAutoScrolledFor; // Agentmaster: the subject key (session id / "x:<extId>") we last auto-scrolled the Flight Plan to bottom for; only a CHANGE re-pins (a same-subject _Refresh keeps the user's scroll)
        winrt::Windows::UI::Xaml::Controls::TextBox _cwdBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _launchBtn{ nullptr }; // Agentmaster: "Launch Claude" (dir) / "Resume session" (a found session id); disabled on a red box
        winrt::Windows::UI::Xaml::Controls::Button _launchAgentBtn{ nullptr }; // Agentmaster (Codex-launch): the Claude<->Codex agent toggle before the box
        bool _launchCodex{ false }; // Agentmaster (Codex-launch): false = launch a Claude (default, unchanged); true = launch a managed Codex in the typed dir
        winrt::Windows::UI::Xaml::Controls::Button _forkBtn{ nullptr }; // Agentmaster: "Fork" — visible only when the box holds a FOUND session id
        winrt::Windows::UI::Xaml::Controls::Border _cwdUnderline{ nullptr }; // Agentmaster: validation underline (green=found session id, red=missing dir / unknown id, hidden=neutral)
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _pathPopup{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _pathPanelBorder{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _pathListHost{ nullptr };
        std::vector<std::wstring> _recentDirs; // MRU of launched working dirs (persisted)
        bool _pathPickerUserDismissed{ false }; // Esc/Enter/blur dismiss the picker; (re)focusing/tapping the box clears it
        winrt::Windows::UI::Xaml::Controls::TextBox _addPromptBox{ nullptr };
        // Agentmaster (prompt history): navigation state for the compose box's Up/Down recall.
        // _promptHistoryIndex == -1 means "not navigating" (the live draft); 0 == the newest sent
        // prompt, growing older. _promptHistory is the newest-first snapshot taken when navigation
        // begins; _promptHistoryDraft is the text that was being composed when it began (restored on
        // Down past the newest). _promptHistoryNavigating guards our own .Text() writes so the box's
        // TextChanged handler doesn't mistake a recall for a user edit and reset navigation.
        int _promptHistoryIndex{ -1 };
        std::vector<std::wstring> _promptHistory;
        std::wstring _promptHistoryDraft;
        bool _promptHistoryNavigating{ false };
        winrt::Windows::UI::Xaml::Controls::Button _autopilotBtn{ nullptr }; // Agentmaster: Autopilot mode toggle, now in a thin strip atop the Flight Plan TAB body (was the old FLIGHT PLAN header)
        // Agentmaster: the Flight-Plan pane's two-state [Summary | Flight Plan] segmented tab toggle
        // (its top line) + the two swappable tab bodies. _summaryHost holds the Summary tab; _flightPlanBody
        // holds the Autopilot strip + the prompt queue / compose box. Visibility is driven by
        // AppSettings::flightPlanShowsSummary via _UpdatePlanPaneTab.
        winrt::Windows::UI::Xaml::Controls::Button _summaryTabBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _flightPlanTabBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Grid _summaryHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Grid _flightPlanBody{ nullptr };
        // Agentmaster (Summary tab): the scrollable host + box panel for the Summary tab. _summaryScroll
        // is the ONE inner scrollbar (the narrow pane scrolls a long box); _summaryBoxHost holds the
        // rendered RenderSessionSummaryBox lines (mono TextBlocks + full-width rules), mirroring the
        // Sessions page's SessAppendSummaryBox. Loaded off-thread + cached single-entry by (id, mtime);
        // user messages are rendered newest-first. _summaryShown* tracks what is CURRENTLY on screen
        // (incl. "\x01loading" / "\x01none" sentinels) so a frequent _Refresh doesn't rebuild the box
        // (and lose scroll) when nothing changed; _summaryLoadingId dedupes the in-flight analyze (one
        // per id — a busy session's mtime churn can't stack loads).
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _summaryScroll{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _summaryBoxHost{ nullptr };
        std::wstring _summaryCacheId;
        int64_t _summaryCacheMtime{ -1 };
        std::wstring _summaryCacheText;
        std::wstring _summaryShownId;
        int64_t _summaryShownMtime{ -1 };
        std::wstring _summaryLoadingId;
        winrt::Windows::UI::Xaml::Controls::StackPanel _templatesRow{ nullptr }; // Agentmaster: the Templates row — collapsed by default, toggled by the paper icon
        winrt::Windows::UI::Xaml::Controls::Button _pauseBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _settingsBtn{ nullptr }; // the cog (next to Pause)
        winrt::Windows::UI::Xaml::Controls::Button _sessionsBtn{ nullptr }; // Agentmaster (Sessions page): "Sessions" -> the global on-disk sessions browser (the sole history view; FAVORITES.md)
        winrt::Windows::UI::Xaml::Controls::Button _reopenBtn{ nullptr }; // Agentmaster (M10): "Reopen Windows (N)" -> reopen saved-but-not-open windows (shown only when N>0)
        winrt::Windows::UI::Xaml::Controls::Button _activateAllBtn{ nullptr }; // Agentmaster (eager-init): "Activate All Tabs (N)" -> wake this window's dormant tabs (shown only when N>0)
        winrt::Windows::UI::Xaml::Controls::Button _keepAwakeBtn{ nullptr }; // Agentmaster: tri-mode "Keep Awake" button -> SetThreadExecutionState keeps the PC + display from sleeping
        // Agentmaster: the user-selected keep-awake mode. Off = sleep normally; Always = always hold the
        // execution-state flag; WhileRunning = hold ONLY while a live session is actively Running (so the
        // machine can sleep once every agent is idle/waiting/done — and never mid-turn).
        enum class KeepAwakeMode { Off, Always, WhileRunning };
        KeepAwakeMode _keepAwakeMode{ KeepAwakeMode::Off };
        bool _keepAwakeHeld{ false }; // whether SetThreadExecutionState is CURRENTLY holding the flag (drives transition-only OS calls + the button color)
        // Last-rendered button appearance — _UpdateKeepAwakeButton repaints ONLY on a (mode, held) transition,
        // so the frequent _Refresh -> _RefreshKeepAwakeHold calls don't rebuild the content + resources (or
        // flicker the button) every tick. _keepAwakeRendered forces the very first paint.
        KeepAwakeMode _keepAwakeRenderedMode{ KeepAwakeMode::Off };
        bool _keepAwakeRenderedHeld{ false };
        bool _keepAwakeRendered{ false };
        // ---- "Claude not detected" overlay (native-exe-only policy gate) ----
        winrt::Windows::UI::Xaml::Controls::Grid _claudeMissingOverlay{ nullptr }; // dimmed modal layer; shown when launch/fork is blocked by no native claude.exe
        winrt::Windows::UI::Xaml::Controls::TextBlock _claudeMissingStatus{ nullptr }; // the live detection status line (updated by Browse / Re-check)
        // ---- Settings overlay (the cog dialog) ----
        winrt::Windows::UI::Xaml::Controls::Grid _settingsOverlay{ nullptr }; // dimmed modal layer over _root
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setSkipPermissions{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setModel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setIncludeCoAuthored{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _setDefaultMode{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setMaxAutoSends{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setStopOnError{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setPauseOnHuman{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setConfirmKill{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _setRenameCommit{ nullptr }; // how the tab rename box commits via the keyboard (None / +Shift+Enter / +Enter); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setWaitingNever{ nullptr }; // Waiting-for-you "unread" model: ON => never time-decay (stay Waiting until read); disables the slider
        winrt::Windows::UI::Xaml::Controls::Slider _setWaitingDecaySlider{ nullptr }; // Waiting-for-you -> Idle timeout, 1..4320 minutes (1m..3d); the "Never" toggle above owns 0
        winrt::Windows::UI::Xaml::Controls::TextBox _setServerCache{ nullptr }; // Claude's server-side prompt-cache lifetime in minutes (drives the card's ⚡ "still cached" hint); default 5
        winrt::Windows::UI::Xaml::Controls::TextBox _setLaunchDir{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setRecentDirsLimit{ nullptr }; // how many recent Launch dirs the path-picker keeps
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setShowTabCloseButton{ nullptr }; // TABS: show the close (x) button on tabs (OFF => force every tab to "Never"); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setCloseTabOnMiddleClick{ nullptr }; // TABS: close a tab on middle-mouse click (OFF => disable both the manual hook + WinUI's native middle-close); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setAlwaysShowHomeButton{ nullptr }; // TABS: always show the strip "Home" button (OFF => only when the Manager tab is scrolled off); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ComboBox _setFavoriteIcon{ nullptr }; // TABS: the FAVORITE marker glyph on a live session's tab — Crown (default) / Star (FAVORITES.md §5a); GLOBAL
        winrt::Windows::UI::Xaml::Controls::TextBlock _setProfileDir{ nullptr }; // the ACTIVE per-install profile dir (read-only; Change… applies on restart)
        winrt::Windows::UI::Xaml::Controls::Button _setResetHidden{ nullptr }; // BEHAVIOR: "Reset hidden sessions" — clears the Sessions browser's "Hide from list" set (fires _resetHiddenSessionsHandler; relabeled per open)
        winrt::Windows::UI::Xaml::Controls::TextBox _setEnv{ nullptr }; // ENV area: GLOBAL multi-line NAME=VALUE editor (one per line)
        // ENV area (ENV_VARS.md): the two-tab "Environment variables" editor. Global vs Per-directory tab
        // buttons swap two panels; each editor is wrapped in a Border whose color = the live lexer status,
        // with a status TextBlock beneath it.
        winrt::Windows::UI::Xaml::Controls::Button _setEnvTabGlobal{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _setEnvTabDir{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _envGlobalPanel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _envDirPanel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _setEnvBorder{ nullptr }; // wraps _setEnv; recolored by the lexer
        winrt::Windows::UI::Xaml::Controls::TextBlock _setEnvStatus{ nullptr }; // global editor bottom status line
        winrt::Windows::UI::Xaml::Controls::TextBox _setEnvDirFilter{ nullptr }; // type-to-filter the dir list
        winrt::Windows::UI::Xaml::Controls::ListBox _setEnvDirList{ nullptr }; // selectable known/working dirs (● = has env)
        winrt::Windows::UI::Xaml::Controls::TextBox _setEnvDir{ nullptr }; // PER-DIR multi-line NAME=VALUE editor (selected dir)
        winrt::Windows::UI::Xaml::Controls::Border _setEnvDirBorder{ nullptr }; // wraps _setEnvDir; recolored by the lexer
        winrt::Windows::UI::Xaml::Controls::TextBlock _setEnvDirStatus{ nullptr }; // per-dir editor bottom status line
        // The in-memory per-directory env draft (NormDirKey -> env-text), seeded from dir-env.json on cog
        // open, edited across dir switches, and flushed on Save (Cancel discards). Mirrors dir-colors'
        // representation (a flat vector of pairs) so no new include is needed.
        std::vector<std::pair<std::wstring, std::wstring>> _dirEnvDraft;
        std::wstring _envEditingDirKey; // NormDirKey of the dir currently loaded into _setEnvDir ("" = none)
        bool _envTabIsDir{ false }; // which tab is showing (false = Global)
        winrt::Windows::UI::Xaml::Controls::TextBlock _setClaudeDetected{ nullptr }; // Agentmaster: the AUTO-DETECTED native claude.exe (read-only; "Not detected" when none)
        winrt::Windows::UI::Xaml::Controls::TextBox _setClaudeExePath{ nullptr }; // Agentmaster: explicit claude.exe override (blank = auto-detect; must be an .exe)
        winrt::Windows::UI::Xaml::Controls::TextBox _setCleanupDays{ nullptr }; // ENV_VARS.md §8: cleanupPeriodDays in the user's GLOBAL ~/.claude/settings.json (history retention; read/written via the ClaudeUserSettings repo, NOT AppSettings)
        // ---- UPDATES (Agentmaster updater; Updater.h) ----
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setAllowPrerelease{ nullptr }; // include GitHub pre-releases in the update check (default OFF)
        winrt::Windows::UI::Xaml::Controls::Button _setCheckUpdates{ nullptr }; // "Check for updates" -> the same prompt the startup check shows
        winrt::Windows::UI::Xaml::Controls::TextBlock _setUpdateStatus{ nullptr }; // status label ("vX.Y.Z available!" dark green / "up to date" / "Checking…")
        bool _interactiveUpdateInFlight{ false }; // guard so a double-click of "Check for updates" can't fire two prompts
        winrt::Windows::UI::Xaml::Controls::HyperlinkButton _setCurrentChangelog{ nullptr }; // opens THIS build's release page (github .../releases/tag/v<current>)
        winrt::Windows::UI::Xaml::Controls::HyperlinkButton _setUpdateChangelog{ nullptr }; // opens the AVAILABLE update's release page; shown only after a check found one
        std::wstring _lastUpdateChangelogUrl; // the available update's release page (drives _setUpdateChangelog's click)
        winrt::Windows::UI::Xaml::Controls::Button _setUninstallBtn{ nullptr }; // Agentmaster (updater): "Uninstall Agentmaster…" -> remove THIS install (per-user; profile data kept), then quit. Shown only for packaged installs.
        winrt::Windows::UI::Xaml::Controls::TextBox _templateNameBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _templateCombo{ nullptr };
        std::vector<::Agentmaster::PlanTemplate> _templates;

        // ---- Resizable splitters (persisted geometry) ----
        // The definitions each splitter resizes + the panes they bound (read for live size).
        ::Agentmaster::ManagerLayout _layout;
        winrt::Windows::UI::Xaml::Controls::RowDefinition _boardRow{ nullptr };
        winrt::Windows::UI::Xaml::Controls::RowDefinition _bottomRow{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ColumnDefinition _treeCol{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ColumnDefinition _planCol{ nullptr };
        // Drag state. One splitter drags at a time; sizes are pinned at PointerPressed so the
        // boundary tracks the cursor 1:1 (no feedback loop from the live re-layout).
        enum class DragKind
        {
            None,
            Rows, // dragging the horizontal bar (Board vs Bottom)
            Cols // dragging the vertical bar (Tree vs Flight Plan)
        };
        DragKind _dragKind{ DragKind::None };
        double _dragOrigin{ 0 }; // root-relative pointer coord on the drag axis at press
        double _dragSizeA{ 0 }; // first track's px size at drag start
        double _dragSizeB{ 0 }; // second track's px size at drag start

        // ---- Explorer-tree interactions ----
        // Button swallows DoubleTapped, so we synthesize a double-click by timing successive
        // clicks on the same row (GetDoubleClickTime threshold) -> Activate; single -> select.
        std::wstring _lastTreeClickId;
        unsigned long long _lastTreeClickTick{ 0 };
        // Triage Board cards use the same click-timing trick (a Button swallows DoubleTapped):
        // double-click a card -> Activate (jump to the live terminal tab); single -> select.
        std::wstring _lastCardClickId;
        unsigned long long _lastCardClickTick{ 0 };
        // In-place rename: a TextBox swapped into the row being renamed. A ContentDialog can't
        // host a text box in XAML Islands (it receives no keypresses), so we edit inline like
        // the tab renamer; while the editor is live the tree skips rebuilds to keep focus+text.
        std::wstring _renamingId;
        winrt::Windows::UI::Xaml::Controls::TextBox _renameBox{ nullptr };
        // Agentmaster: like TabHeaderControl, the inline rename box is multi-line; a commit combo
        // (Enter / Shift+Enter per AppSettings::tabRenameCommitMode) is flagged in PreviewKeyDown
        // (which suppresses the newline) and committed on the matching KeyUp — Rule #11 keeps this
        // tree-rename path behaving like the WT tab-rename box.
        bool _renameCommitOnKeyUp{ false };
    };
}
