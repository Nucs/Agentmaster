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
        void SetArchiveHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> archive (shut down, keep restorable)
        void SetRestoreHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> re-launch (resume) an archived session
        void SetRenameHandler(std::function<void(winrt::hstring, winrt::hstring)> handler); // (sessionId, newTitle) -> rename in the registry + retitle the WT tab (the one title)
        // Agentmaster: the set of session ids hosted in THIS window (the page's _claudeTabs).
        // Used by the Explorer Tree's LOCAL scope to show only this window's sessions; GLOBAL
        // ignores it and shows every window's sessions (the whole process-wide registry).
        void SetLocalScopeProvider(std::function<std::unordered_set<std::wstring>()> provider);
        void SetPauseHandler(std::function<void(bool)> handler); // global Autopilot Pause-all
        void SetConfirmHandler(std::function<void(winrt::hstring, bool)> handler); // SemiAuto confirm/skip
        void SetSettings(const ::Agentmaster::AppSettings& settings); // seed the cog dialog's current values
        void SetSettingsHandler(std::function<void(::Agentmaster::AppSettings)> handler); // persist on Save
        // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): the "Reopen Windows (N)" recover
        // button's action — reopen saved windows that are NOT currently open (the runtime analog of the
        // WindowEmperor's startup reopen loop). The content computes N itself
        // (::Agentmaster::RecoverableWindows) and shows the button only when N>0.
        void SetReopenWindowsHandler(std::function<void()> handler);

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

        winrt::hstring Title() { return L"Agent Manager"; }
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
        void _RebuildPlan(const std::vector<::Agentmaster::SessionInfo>& sessions);

        // Agentmaster: Explorer Tree scope toggle (LOCAL = this window's tabs / GLOBAL = all
        // windows). _ToggleTreeScope flips the mode + rebuilds; _UpdateTreeScopeButton refreshes
        // the toggle button's label to the current mode.
        void _ToggleTreeScope();
        void _UpdateTreeScopeButton();

        void _SelectSession(const std::wstring& id);
        void _SetScope(const std::wstring& dir);
        std::optional<::Agentmaster::SessionInfo> _Selected(const std::vector<::Agentmaster::SessionInfo>& sessions) const;

        // Agentmaster (M10): fire _lensChangedHandler with the current lens (GetManagerState) so the
        // window debounce-saves its record; _ApplyLayoutToTracks pushes _layout's fractions into the
        // live row/column definitions (used when seeding a restored per-window layout).
        void _NotifyLensChanged();
        void _ApplyLayoutToTracks();

        // Action-bar handlers (operate on _selectedId / _selectedPromptId).
        void _OnLaunch();
        void _OnAddPrompt();
        void _OnSendNow(); // the "!" icon — confirms, then _DoSendNow
        void _DoSendNow(); // actual inject, after the Send-now confirm
        void _OnMovePrompt(int delta);
        void _OnDeletePrompt();
        void _OnAutopilotChanged(int index);
        // Agentmaster: the FLIGHT-PLAN-header Autopilot toggle. _CycleAutopilot advances the
        // selected session's mode (Off -> Semi-auto -> Full -> Off); _UpdateAutopilotButton paints
        // the button's colored state dot + label (dim/disabled when no live session is selected).
        void _CycleAutopilot();
        void _UpdateAutopilotButton(::Agentmaster::AutopilotMode mode, bool enabled);
        void _OnSaveTemplate();
        void _OnApplyTemplate(bool toWholeDirectory);
        void _RefreshTemplateCombo();

        // Launch path-picker drop-down (a Popup anchored under the cwd box). Opens on user
        // focus of the box; shows the recent dirs (excluding the current one) over the
        // subfolders of the current path; clicking a row drives the box and re-lists.
        void _OpenPathPicker();
        void _ClosePathPicker();
        void _RebuildPathPicker();
        void _PickPath(const std::wstring& dir);
        void _NormalizeCwdBox(); // platform-sensitive NormPath of the cwd box (on commit / blur / pick / launch)
        void _PushRecentDir(const std::wstring& dir);
        std::vector<std::wstring> _CollectRecentDirs(const std::wstring& current) const;
        winrt::Windows::UI::Xaml::Controls::Button _MakePathRow(const std::wstring& fullPath, const winrt::hstring& glyph, const winrt::hstring& displayText);

        // Build one session card for the Triage Board.
        winrt::Windows::UI::Xaml::Controls::Button _MakeCard(const ::Agentmaster::SessionInfo& s);

        // Draggable pane splitters (resize + on-hover cursor + persisted sizes).
        // `vertical` == a vertical bar dividing the bottom COLUMNS (↔, resizes Tree/Plan);
        // `!vertical` == a horizontal bar dividing the root ROWS (↕, resizes Board/Bottom).
        winrt::Windows::UI::Xaml::Controls::Border _MakeSplitter(bool vertical);
        void _OnSplitterPressed(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e, bool vertical);
        void _OnSplitterMoved(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e, bool vertical);
        void _OnSplitterReleased(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        // Explorer-tree session actions: right-click context menu (Rename / Delete with a
        // confirm warning) + double-click to activate.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakeSessionMenu(const std::wstring& id);
        // Flight-Plan message right-click menu: per-prompt Move up / Move down / Delete (only on
        // UPCOMING rows — a sent row can't be reordered) + Archive session (always). Queue ops act
        // on `promptId` (the right-clicked row), not the current selection.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakePromptMenu(const std::wstring& promptId, bool upcoming);
        void _OnRenameSession(const std::wstring& id); // begin an in-place rename of the row
        void _CommitRename(); // apply the in-place editor's text to the session title
        void _CancelRename(); // discard the in-place editor (Esc)
        void _RequestArchive(const std::wstring& id); // route to the page's archive seam (which presents the consequence + closes the tab)

        // Settings cog: an in-content modal overlay (NOT a ContentDialog — a text box inside a
        // ContentDialog receives no keypresses in XAML Islands; see the _renameBox note). Built
        // into the main visual tree so its TextBoxes work; shown/hidden by toggling Visibility.
        void _BuildSettingsOverlay();
        void _ShowSettings(); // populate controls from _appSettings, then reveal the overlay
        void _HideSettings();
        void _SaveSettings(); // read controls -> _appSettings -> _settingsSink, then hide

        // Archived-sessions overlay (mirrors the settings overlay): a modal list of sessions that
        // were closed/archived (live==false). Each row Restores (re-launch + resume); the header
        // states the consequence. Opened from the "Archived" toolbar button next to the cog.
        void _BuildArchiveOverlay();
        void _ShowArchive();
        void _HideArchive();
        void _RebuildArchiveList(); // (re)populate _archiveListHost from the registry's !live sessions
        void _OnRestoreSession(const std::wstring& id); // confirm -> _restoreHandler(id)
        void _OnRestoreAll(); // confirm -> restore every archived session
        void _UpdateArchivedButton(const std::vector<::Agentmaster::SessionInfo>& sessions); // label "Archived (N)" + enable
        // Agentmaster (M10 Increment 3): the "Reopen Windows (N)" recover button. _UpdateReopenButton
        // sets its label to the recoverable-window count and shows it only when N>0; _OnReopenWindows
        // confirms, then fires _reopenWindowsHandler (the page reopens every not-currently-open record).
        void _UpdateReopenButton();
        void _OnReopenWindows();
        // A buttons-only confirm (XAML-Islands-safe) for consequential actions; runs onYes on accept.
        void _Confirm(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, std::function<void()> onYes);

        std::shared_ptr<::Agentmaster::SessionRegistry> _registry;
        // Agentmaster (M9): our observer's token on the shared (process-wide) registry, so this
        // window's lens detaches cleanly on teardown instead of dangling a strong
        // DispatcherQueue ref there. (An ::Agentmaster::ObserverToken == uint64_t; kept as
        // uint64_t to avoid including SessionRegistry.h in this header.)
        uint64_t _observerToken{ 0 };
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr };

        std::function<void(winrt::hstring, winrt::hstring)> _spawnHandler;
        std::function<void(winrt::hstring)> _activateHandler;
        std::function<void(winrt::hstring)> _archiveHandler;
        std::function<void(winrt::hstring)> _restoreHandler;
        std::function<void(winrt::hstring, winrt::hstring)> _renameHandler; // Agentmaster: Explorer-tree rename -> page (registry title + tab title in lockstep)
        std::function<std::unordered_set<std::wstring>()> _localScopeProvider; // Agentmaster: this window's hosted session ids (for the Explorer Tree LOCAL scope)
        std::function<void(bool)> _pauseHandler;
        std::function<void(winrt::hstring, bool)> _confirmHandler;
        std::function<void(::Agentmaster::AppSettings)> _settingsSink;
        std::function<void()> _reopenWindowsHandler; // Agentmaster (M10): the "Reopen Windows" recover-button action
        std::function<void(::Agentmaster::ManagerState)> _lensChangedHandler; // Agentmaster (M10): push lens changes to the hosting window
        ::Agentmaster::AppSettings _appSettings{}; // current settings (seeded by SetSettings; edited via the cog)
        bool _globalPaused{ false };

        std::wstring _selectedId;
        std::wstring _scopeDir; // board filter: empty == all directories
        std::wstring _selectedPromptId;
        bool _treeGlobalScope{ false }; // Agentmaster: Explorer Tree scope. false == LOCAL (this window's tabs only); true == GLOBAL (all windows)
        std::unordered_set<std::wstring> _collapsedDirs;
        bool _suppressAutopilotEvent{ false };

        winrt::Windows::UI::Xaml::Controls::Grid _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _boardHost{ nullptr }; // horizontal columns
        winrt::Windows::UI::Xaml::Controls::TextBlock _boardScope{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _treeScopeBtn{ nullptr }; // Agentmaster: the LOCAL/GLOBAL toggle after the "EXPLORER TREE" title
        winrt::Windows::UI::Xaml::Controls::StackPanel _treeHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _planHeaderHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _planListHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _cwdBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _pathPopup{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _pathPanelBorder{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _pathListHost{ nullptr };
        std::vector<std::wstring> _recentDirs; // MRU of launched working dirs (persisted)
        bool _pathPickerUserDismissed{ false }; // Esc/Enter/blur dismiss the picker; (re)focusing/tapping the box clears it
        winrt::Windows::UI::Xaml::Controls::TextBox _addPromptBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _autopilotBtn{ nullptr }; // Agentmaster: Autopilot mode toggle in the FLIGHT PLAN header (replaces the old combo)
        winrt::Windows::UI::Xaml::Controls::StackPanel _templatesRow{ nullptr }; // Agentmaster: the Templates row — collapsed by default, toggled by the paper icon
        winrt::Windows::UI::Xaml::Controls::Button _pauseBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _settingsBtn{ nullptr }; // the cog (next to Pause)
        winrt::Windows::UI::Xaml::Controls::Button _archivedBtn{ nullptr }; // "Archived (N)" (next to the cog) -> opens the archive overlay
        winrt::Windows::UI::Xaml::Controls::Button _reopenBtn{ nullptr }; // Agentmaster (M10): "Reopen Windows (N)" -> reopen saved-but-not-open windows (shown only when N>0)
        // ---- Archived-sessions overlay (the "Archived" button) ----
        winrt::Windows::UI::Xaml::Controls::Grid _archiveOverlay{ nullptr }; // dimmed modal layer listing archived sessions
        winrt::Windows::UI::Xaml::Controls::StackPanel _archiveListHost{ nullptr }; // rows of archived sessions (Restore each)
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
        winrt::Windows::UI::Xaml::Controls::TextBox _setLaunchDir{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setRecentDirsLimit{ nullptr }; // how many recent Launch dirs the path-picker keeps
        winrt::Windows::UI::Xaml::Controls::TextBox _setEnv{ nullptr }; // ;-delimited NAME=VALUE applied to every session
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
    };
}
