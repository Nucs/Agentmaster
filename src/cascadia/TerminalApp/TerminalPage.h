// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <ThrottledFunc.h>
#include <atomic>
#include <unordered_set>

#include "TerminalPage.g.h"
#include "Tab.h"
#include "AppKeyBindings.h"
#include "AppCommandlineArgs.h"
#include "RenameWindowRequestedArgs.g.h"
#include "RequestMoveContentArgs.g.h"
#include "LaunchPositionRequest.g.h"
#include "Toast.h"

#include "WindowsPackageManagerFactory.h"

#define DECLARE_ACTION_HANDLER(action) void _Handle##action(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

namespace TerminalAppLocalTests
{
    class TabTests;
    class SettingsTests;
}

namespace Microsoft::Terminal::Core
{
    class ControlKeyStates;
}

namespace winrt::Microsoft::Terminal::Settings
{
    struct TerminalSettingsCreateResult;
}

// Agentmaster: the native session-management engine (plain C++; see
// src/cascadia/TerminalApp/AgentMaster/). The engine classes are forward-declared so
// TerminalPage can hold them by shared_ptr; SessionModels.h (value types) is included
// because restore passes a SessionInfo by value.
#include "AgentMaster/SessionModels.h"
#include "AgentMaster/TranscriptStore.h" // Agentmaster (Sessions page): SessionIndexEntry (by-value member)
#include <optional>
namespace Agentmaster
{
    class SessionRegistry;
    class HooksBridge;
    class Scheduler;
    class SessionScanner;
    class ProcessObserver;
}

namespace winrt::TerminalApp::implementation
{
    struct TerminalSettingsCache;
    class AgentManagerContent; // Agentmaster: the Manager tab's content (IPaneContent)
    class AgentTabOverlay; // Agentmaster: the per-tab "link badge" overlay (TAB_OVERLAY.md)

    inline constexpr uint32_t DefaultRowsToScroll{ 3 };
    inline constexpr std::wstring_view TabletInputServiceKey{ L"TabletInputService" };

    enum StartupState : int
    {
        NotInitialized = 0,
        InStartup = 1,
        Initialized = 2
    };

    enum ScrollDirection : int
    {
        ScrollUp = 0,
        ScrollDown = 1
    };

    enum class ConfirmCloseDialogKind
    {
        Pane,
        Tab,
        MultiplePanes,
        MultipleTabs,
        Window,
        CloseAll
    };

    struct RenameWindowRequestedArgs : RenameWindowRequestedArgsT<RenameWindowRequestedArgs>
    {
        WINRT_PROPERTY(winrt::hstring, ProposedName);

    public:
        RenameWindowRequestedArgs(const winrt::hstring& name) :
            _ProposedName{ name } {};
    };

    struct RequestMoveContentArgs : RequestMoveContentArgsT<RequestMoveContentArgs>
    {
        WINRT_PROPERTY(winrt::hstring, Window);
        WINRT_PROPERTY(winrt::hstring, Content);
        WINRT_PROPERTY(uint32_t, TabIndex);
        WINRT_PROPERTY(Windows::Foundation::IReference<Windows::Foundation::Point>, WindowPosition);

    public:
        RequestMoveContentArgs(const winrt::hstring window, const winrt::hstring content, uint32_t tabIndex) :
            _Window{ window },
            _Content{ content },
            _TabIndex{ tabIndex } {};
    };

    struct LaunchPositionRequest : LaunchPositionRequestT<LaunchPositionRequest>
    {
        LaunchPositionRequest() = default;

        til::property<winrt::Microsoft::Terminal::Settings::Model::LaunchPosition> Position;
    };

    struct WinGetSearchParams
    {
        winrt::Microsoft::Management::Deployment::PackageMatchField Field;
        winrt::Microsoft::Management::Deployment::PackageFieldMatchOption MatchOption;
    };

    struct TerminalPage : TerminalPageT<TerminalPage>
    {
    public:
        TerminalPage(TerminalApp::WindowProperties properties, const TerminalApp::ContentManager& manager);
        ~TerminalPage(); // Agentmaster: detach this window's adoption handler from the shared engine (M9)

        // This implements shobjidl's IInitializeWithWindow, but due to a XAML Compiler bug we cannot
        // put it in our inheritance graph. https://github.com/microsoft/microsoft-ui-xaml/issues/3331
        STDMETHODIMP Initialize(HWND hwnd);

        void SetSettings(Microsoft::Terminal::Settings::Model::CascadiaSettings settings, bool needRefreshUI);

        void Create();
        Windows::UI::Xaml::Automation::Peers::AutomationPeer OnCreateAutomationPeer();

        bool ShouldImmediatelyHandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings) const;
        void HandoffToElevated(const Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        hstring Title();

        void TitlebarClicked();
        void WindowVisibilityChanged(const bool showOrHide);

        float CalcSnappedDimension(const bool widthOrHeight, const float dimension) const;

        winrt::hstring ApplicationDisplayName();
        winrt::hstring ApplicationVersion();

        CommandPalette LoadCommandPalette();
        SuggestionsControl LoadSuggestionsUI();

        safe_void_coroutine RequestQuit();
        safe_void_coroutine CloseWindow();
        void PersistState();
        std::vector<IPaneContent> Panes() const;

        void ToggleFocusMode();
        void ToggleFullscreen();
        void ToggleAlwaysOnTop();
        bool FocusMode() const;
        bool Fullscreen() const;
        bool AlwaysOnTop() const;
        bool ShowTabsFullscreen() const;
        void SetShowTabsFullscreen(bool newShowTabsFullscreen);
        void SetFullscreen(bool);
        void SetFocusMode(const bool inFocusMode);
        void Maximized(bool newMaximized);
        void RequestSetMaximized(bool newMaximized);

        void SetStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions);
        void SetStartupConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);
        void SetAgentmasterWindowId(winrt::hstring windowId); // Agentmaster (M10): the Emperor-assigned restore-record id (multi-window reopen)

        static std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> ConvertExecuteCommandlineToActions(const Microsoft::Terminal::Settings::Model::ExecuteCommandlineArgs& args);

        winrt::TerminalApp::IDialogPresenter DialogPresenter() const;
        void DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter);

        winrt::TerminalApp::TaskbarState TaskbarState() const;

        void ShowKeyboardServiceWarning() const;
        winrt::hstring KeyboardServiceDisabledText();

        void IdentifyWindow();
        void ActionSaved(winrt::hstring input, winrt::hstring name, winrt::hstring keyChord);
        void ActionSaveFailed(winrt::hstring message);
        void ShowTerminalWorkingDirectory();

        safe_void_coroutine ProcessStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions,
                                                  const winrt::hstring cwd = winrt::hstring{},
                                                  const winrt::hstring env = winrt::hstring{});
        safe_void_coroutine CreateTabFromConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

        TerminalApp::WindowProperties WindowProperties() const noexcept { return _WindowProperties; };

        bool CanDragDrop() const noexcept;
        bool IsRunningElevated() const noexcept;

        void OpenSettingsUI();
        void WindowActivated(const bool activated);
        bool FocusTab(const winrt::TerminalApp::Tab& tab);

        bool OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down);

        void AttachContent(Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::ActionAndArgs> args, uint32_t tabIndex);
        void SendContentToOther(winrt::TerminalApp::RequestReceiveContentArgs args);

        uint32_t NumberOfTabs() const;

        til::property_changed_event PropertyChanged;

        // -------------------------------- WinRT Events ---------------------------------
        til::typed_event<IInspectable, IInspectable> TitleChanged;
        til::typed_event<IInspectable, IInspectable> CloseWindowRequested;
        til::typed_event<IInspectable, winrt::Windows::UI::Xaml::UIElement> SetTitleBarContent;
        til::typed_event<IInspectable, IInspectable> FocusModeChanged;
        til::typed_event<IInspectable, IInspectable> FullscreenChanged;
        til::typed_event<IInspectable, IInspectable> ChangeMaximizeRequested;
        til::typed_event<IInspectable, IInspectable> AlwaysOnTopChanged;
        til::typed_event<IInspectable, IInspectable> RaiseVisualBell;
        til::typed_event<IInspectable, IInspectable> SetTaskbarProgress;
        til::typed_event<IInspectable, IInspectable> Initialized;
        til::typed_event<IInspectable, IInspectable> IdentifyWindowsRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::RenameWindowRequestedArgs> RenameWindowRequested;
        til::typed_event<IInspectable, IInspectable> SummonWindowRequested;
        til::typed_event<IInspectable, winrt::TerminalApp::Tab> FocusTabRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs> WindowSizeChanged;

        til::typed_event<IInspectable, IInspectable> OpenSystemMenu;
        til::typed_event<IInspectable, IInspectable> QuitRequested;
        til::typed_event<IInspectable, winrt::Microsoft::Terminal::Control::ShowWindowArgs> ShowWindowChanged;
        til::typed_event<Windows::Foundation::IInspectable, Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Terminal::Settings::Model::SettingsLoadWarnings>> ShowLoadWarningsDialog;

        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestMoveContentArgs> RequestMoveContent;
        til::typed_event<Windows::Foundation::IInspectable, winrt::TerminalApp::RequestReceiveContentArgs> RequestReceiveContent;

        til::typed_event<IInspectable, winrt::TerminalApp::LaunchPositionRequest> RequestLaunchPosition;

        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, TitlebarBrush, PropertyChanged.raise, nullptr);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, FrameBrush, PropertyChanged.raise, nullptr);

        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionName, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionKeyChord, PropertyChanged.raise, L"");
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, SavedActionCommandLine, PropertyChanged.raise, L"");

    private:
        friend struct TerminalPageT<TerminalPage>; // for Xaml to bind events
        std::optional<HWND> _hostingHwnd;

        // If you add controls here, but forget to null them either here or in
        // the ctor, you're going to have a bad time. It'll mysteriously fail to
        // activate the app.
        // ALSO: If you add any UIElements as roots here, make sure they're
        // updated in App::_ApplyTheme. The roots currently is _tabRow
        // (which is a root when the tabs are in the titlebar.)
        Microsoft::UI::Xaml::Controls::TabView _tabView{ nullptr };
        TerminalApp::TabRowControl _tabRow{ nullptr };
        Windows::UI::Xaml::Controls::Grid _tabContent{ nullptr };
        Microsoft::UI::Xaml::Controls::SplitButton _newTabButton{ nullptr };
        winrt::TerminalApp::ColorPickupFlyout _tabColorPicker{ nullptr };

        Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };

        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _tabs;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _mruTabs;
        static winrt::com_ptr<Tab> _GetTabImpl(const TerminalApp::Tab& tab);

        void _UpdateTabIndices();

        TerminalApp::Tab _settingsTab{ nullptr };
        TerminalApp::Tab _managerTab{ nullptr }; // Agentmaster: the pinned, leftmost Manager tab
        // Agentmaster (Fleet Observer O6): a WEAK handle to the Manager tab's content (held as its
        // projected IPaneContent — recovered to the impl via winrt::get_self), so _ObserverProbe can
        // push the observer's External (WindowsTerminal) census to it each tick. Weak so there is no
        // page<->content cycle; the content is owned by the (non-closable) Manager pane regardless.
        winrt::weak_ref<winrt::TerminalApp::IPaneContent> _agentManagerContent;

        // Agentmaster: the session-management engine (see AgentMaster/). SessionRegistry is
        // the single source of truth; HooksBridge feeds it authoritative state from Claude
        // Code hooks over a local named pipe. M9: these are the ONE process-wide engine
        // (::Agentmaster::SharedEngine()), shared by every window; this page just holds copies
        // of the shared_ptrs. Forward-declared here (HooksBridge's dtor joins its threads).
        std::shared_ptr<::Agentmaster::SessionRegistry> _sessionRegistry{ nullptr };
        std::shared_ptr<::Agentmaster::HooksBridge> _hooksBridge{ nullptr };
        std::shared_ptr<::Agentmaster::Scheduler> _scheduler{ nullptr }; // Agentmaster: Autopilot
        std::shared_ptr<::Agentmaster::SessionScanner> _scanner{ nullptr }; // Agentmaster: the interval reconciler (PULL)
        std::shared_ptr<::Agentmaster::ProcessObserver> _observer{ nullptr }; // Agentmaster: the Fleet Observer S-lane (PULL census/correlation; OBSERVER.md §10)
        // Agentmaster (M9): this window's adoption handler on the shared registry — fans out a
        // hand-typed `+`-tab `claude` to whichever window hosts it. Detached in ~TerminalPage.
        // (An ::Agentmaster::AdoptionToken; uint64_t to avoid pulling SessionRegistry.h here.)
        uint64_t _adoptionToken{ 0 };
        // Agentmaster: this window's liveness probe on the shared scanner — the scanner ticks it
        // on the slow cadence and it archives any of THIS window's claude tabs whose ConPTY has
        // Closed. Detached in ~TerminalPage. (An ::Agentmaster::LivenessToken; uint64_t to avoid
        // pulling SessionScanner.h into this header.)
        uint64_t _livenessToken{ 0 };
        // Agentmaster (cross-window activate): this window's activate sink on the shared engine —
        // another window's Activate (board/tree double-click, tree Enter, the Flight Plan's eye) on
        // a session hosted HERE hops to this window's UI thread, selects the session's tab, and
        // brings this window to the foreground. Detached in ~TerminalPage (Rule #10).
        uint64_t _windowActivateToken{ 0 };
        ::Agentmaster::AppSettings _appSettings{}; // Agentmaster: global settings (the cog); loaded at engine init
        // Agentmaster: sessionId -> its terminal tab, so the Manager can Activate (jump) or
        // Kill a session. Weak so closing a tab the normal way doesn't keep it alive.
        std::unordered_map<std::wstring, winrt::weak_ref<TerminalApp::Tab>> _claudeTabs;
        // Agentmaster (TAB_OVERLAY.md): sessionId -> its per-tab "link badge" overlay. STRONG ref
        // (the page builds + owns it); erased alongside _claudeTabs on archive/close/liveness so the
        // overlay's registry observer detaches. Type completed in AgentTabOverlay.h (TerminalPage.cpp).
        std::unordered_map<std::wstring, winrt::com_ptr<implementation::AgentTabOverlay>> _claudeOverlays;
        // Agentmaster (OBSERVER.md §11d): wtSession -> a registry-LESS "claude · unlinked" pending
        // overlay for a tab whose claude the observer correlated but can't resolve a conversation id
        // for yet (never prompted). Keyed by WT_SESSION (there is no sessionId). Replaced by the real
        // _claudeOverlays entry once the session resolves; pruned when the tab leaves this window's roster.
        std::unordered_map<std::wstring, winrt::com_ptr<implementation::AgentTabOverlay>> _pendingOverlays;

        // Agentmaster (M10; PERSISTENCE.md §13): per-window workspace persistence. _windowId is
        // this window's stable GUID; _windowRecord is its persisted UI state (geometry + Manager
        // lens + ORDERED tab refs). Claimed at engine init (an existing windows/<id>.json, or a
        // fresh GUID), its lens seeded into the Manager tab on wire, then autosaved on
        // structural/lens change (debounced via _saveWindowRecordThrottled) and flushed on close.
        // The session fleet stays in sessions.json — the record only REFERENCES sessions by id
        // (Option 1; Correctness Rule #10), never copies them.
        std::wstring _windowId;
        ::Agentmaster::WindowRecord _windowRecord{};
        std::shared_ptr<ThrottledFunc<>> _saveWindowRecordThrottled{ nullptr };
        // True when _windowRecord was CLAIMED from disk (a real prior layout) vs freshly minted.
        // Only a claimed record seeds the Manager lens on wire — a fresh window keeps the content's
        // ctor-loaded global splitter sizes, so opening a new window never resets them to default.
        bool _windowRecordClaimed{ false };
        // Agentmaster (quit-all window-record loss): set once CloseWindow/RequestQuit has
        // flushed the record at its deterministic close/quit seam, so ~TerminalPage's catch-all flush
        // won't re-capture a post-teardown-archive state (where _claudeTabs is already cleared and every
        // Claude tab would degrade to an anonymous Other ref) over that good record. Stays false on the
        // quit-all path for the NON-initiating windows — they are torn down straight through the
        // destructor with no per-window close, so the destructor is their only flush seam.
        bool _windowRecordTeardownFlushed{ false };
        // Agentmaster (M10 Increment 3): the Emperor-assigned record id for a multi-window restore,
        // set by TerminalWindow before _OnFirstLayout. Empty => single-window (claim the front record).
        std::wstring _assignedWindowId;

        // Agentmaster (Archive page): the full-window archive surface's state. _archivePageHost is the
        // collapsed Grid mounted on Root's content rows (1-2, below the tab strip); the rest are its live sub-elements +
        // selection / multi-select / sort / filter state. _archiveRows is the gathered data (re-gathered
        // on show + after an action, NOT per keystroke — RecoverableWindows() reads disk), then filtered
        // + sorted into the table by _RenderArchiveTable.
        struct _ArchiveRow
        {
            std::wstring id;
            std::wstring title;
            std::wstring dir;
            std::wstring branch;
            int64_t createdUnixMs{ 0 };
            int64_t lastActivityUnixMs{ 0 };
            int windowIndex{ -1 };  // RecoverableWindow::index AT GATHER TIME (fallback for "Reopen its window"); -1 = loose
            std::wstring windowId;  // Agentmaster: the record's stable GUID — reopen re-resolves the live index from this (the gather-time index goes stale if the record set shifts while the page is open)
            int windowOrdinal{ 0 }; // 1-based "W{n}" display chip; 0 = loose (no saved window)
            int sentCount{ 0 };
            int totalCount{ 0 };
            // Agentmaster: a synthetic "saved window" row (id = "window:<guid>") for a recoverable record
            // with NO archived Claude sessions (a pure-shell workspace, or sessions live elsewhere). Without
            // it such a window was invisible + un-reopenable from this page (only the toolbar's "Reopen
            // Windows (N)" covered it). No checkbox / no "Restore here"; its detail shows the tab
            // composition (counts below) + "Reopen its window".
            bool windowOnly{ false };
            int winClaudeTabs{ 0 };
            int winShellTabs{ 0 };
            // Agentmaster: the W{n} chip's hover tooltip — what that saved window IS ("W2 · 4 tabs
            // (2 claude, 2 shell) · 1466×780 @ 14,173"), built ONCE per record at gather time from its
            // WindowRecord (tab composition + geometry); the render loop only has the row. Empty for
            // loose rows (no chip).
            std::wstring windowTip;
            // Agentmaster: the row's lowercase search haystack, built ONCE at gather — title / dir
            // (+ a slash-flipped twin) / branch / session id, then every queued prompt's label + text
            // ('\n'-fenced fields). _RenderArchiveTable AND-matches the filter's whitespace tokens
            // against this, so "remember that prompt I queued" (or a UUID pasted from hooks.log) finds
            // its session — the old per-render haystack covered only title/dir/branch.
            std::wstring searchBlob;
        };
        winrt::Windows::UI::Xaml::Controls::Grid _archivePageHost{ nullptr };          // full-bleed page over Root
        winrt::Windows::UI::Xaml::Controls::Grid _archiveHeaderRow{ nullptr };         // LEFT: sortable column header
        winrt::Windows::UI::Xaml::Controls::StackPanel _archiveRowsHost{ nullptr };    // LEFT: table data rows
        winrt::Windows::UI::Xaml::Controls::StackPanel _archiveDetailHost{ nullptr };  // RIGHT: detail/preview
        winrt::Windows::UI::Xaml::Controls::TextBox _archiveSearchBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _archiveCountText{ nullptr };    // header "N sessions · M windows"
        winrt::Windows::UI::Xaml::Controls::Button _archiveRestoreSelBtn{ nullptr };   // footer bulk action
        std::vector<_ArchiveRow> _archiveRows;
        std::wstring _archiveSelectedId;                  // the row whose detail is shown
        std::unordered_set<std::wstring> _archiveChecked;    // multi-select set (by session id)
        std::unordered_set<std::wstring> _archiveVisibleIds; // Agentmaster: ids currently passing the filter (rebuilt each render); bulk-restore + its "(N)" count act on checked ∩ visible only
        std::vector<std::wstring> _archiveVisibleOrder;      // Agentmaster: the visible ids in TABLE (sorted) order — bulk restore follows it, so restored tabs open in display order, not the unordered_set's hash order
        int _archiveSortColumn{ 4 };                      // default sort column: Created (see _RenderArchiveTable)
        bool _archiveSortAscending{ false };              // default: newest first
        std::wstring _archiveFilter;                      // lowercased search text
        // Agentmaster (Archive page live refresh): the registry changes while the page is open (a tab X
        // archives a session, another window restores one, the liveness sweep archives a dead claude) —
        // an observer (fires on ANY thread) pokes a throttled UI-thread re-gather while the page is
        // visible; the search rebuild is debounced through its own throttle. _archivePageVisible mirrors
        // the host's Visibility as an atomic so the observer thread can pre-filter without touching XAML.
        // Token detached in ~TerminalPage (an ::Agentmaster::ObserverToken; uint64_t to avoid pulling
        // SessionRegistry.h into this header — same pattern as _adoptionToken).
        uint64_t _archiveRegistryObserverToken{ 0 };
        std::shared_ptr<ThrottledFunc<>> _archiveRefreshThrottled{ nullptr };
        std::shared_ptr<ThrottledFunc<>> _archiveFilterThrottled{ nullptr };
        std::atomic<bool> _archivePageVisible{ false };
        // Agentmaster (tab status dot): this window's registry observer driving the tab-strip
        // "[icon] ● <title>" dot — a state change recolors the hosting tab's dot in place (the same
        // push that redraws the Manager board). Registered at engine init; detached in ~TerminalPage
        // (an ::Agentmaster::ObserverToken; uint64_t to avoid pulling SessionRegistry.h here — the
        // _adoptionToken pattern).
        uint64_t _agentDotObserverToken{ 0 };
        // Agentmaster (branch backfill): archived ids whose transcript head this run already read for a
        // missing `branch` — SessionInfo.branch had NO live writer (only the JSON loader), so the Branch
        // column + search were permanently empty; the backfill reads each transcript at most once per run.
        std::unordered_set<std::wstring> _archiveBranchBackfilled;
        // Agentmaster: one-entry transcript cache for the detail pane, validated by (id, transcript
        // mtime) — _RenderArchiveTable re-shows the detail on every rebuild (search keystrokes, observer
        // refreshes), and an uncached ReadTranscriptInfo was a synchronous <=128 KB UI-thread file read
        // each time. Discrete fields so this header needn't pull ProcessInspect.h (TranscriptInfo).
        std::wstring _archiveDetailTiId;
        int64_t _archiveDetailTiMtime{ 0 };
        int64_t _archiveDetailTiCreated{ 0 };
        int64_t _archiveDetailTiLast{ 0 };
        std::wstring _archiveDetailTiBranch;
        std::vector<std::wstring> _archiveDetailTiPrompts;
        // Agentmaster: one-entry "last assistant reply" cache for the detail pane, keyed by (id,
        // transcript mtime) like the head cache above. ReadTranscriptInfo is a HEAD read (title /
        // branch / prompts); the LAST assistant message — "where did this conversation leave off?" —
        // lives at the TAIL, so _LoadArchiveAssistantTail tail-reads + parses it OFF-THREAD and
        // re-shows the detail on completion. An attempted (id, mtime) caches even an empty result so
        // a reply-less transcript isn't re-read on every detail re-show.
        std::wstring _archiveDetailTailId;
        int64_t _archiveDetailTailMtime{ 0 };
        std::wstring _archiveDetailTailText;
        bool _archiveDetailTailPending{ false };

        // Agentmaster (Sessions page; SESSIONS.md): the full-window browser over EVERY on-disk
        // Claude Code session (not just managed ones), opened by the Manager's "Sessions" button
        // (right after Archived). Duplicates the Archive page's structure — search bar at the top:
        // [ text ] (👤)(🤖)(📁)(📄)(F) [window] — backed by the TranscriptStore sidecar index +
        // the two-phase SessionSearch (fast in-memory + history.jsonl; slow rg-prefiltered content).
        struct _SessionsRow
        {
            std::wstring id; // the conversation/session uuid (== transcript stem)
            std::wstring path; // transcript path at gather time
            std::wstring title; // PickDisplayTitle(custom > ai > legacy summary > first prompt)
            std::wstring dir; // the REAL cwd from the lines (folder names are lossy)
            std::wstring branch;
            int64_t createdMs{ 0 }; // fork-aware (a fork's copied line stamps lie -> file birth)
            int64_t lastActivityMs{ 0 }; // last MESSAGE timestamp (mtime only as fallback)
            int64_t sizeBytes{ 0 };
            int msgs{ 0 }; // REAL user prompts
            int tools{ 0 }; // assistant tool_use blocks
            bool fork{ false };
            std::wstring forkedFromId;
        };
        winrt::Windows::UI::Xaml::Controls::Grid _sessionsPageHost{ nullptr }; // full-bleed page over Root rows 1-2
        winrt::Windows::UI::Xaml::Controls::Grid _sessionsHeaderRow{ nullptr }; // LEFT: sortable column header
        winrt::Windows::UI::Xaml::Controls::StackPanel _sessionsRowsHost{ nullptr }; // LEFT: table data rows
        winrt::Windows::UI::Xaml::Controls::StackPanel _sessionsDetailHost{ nullptr }; // RIGHT: detail/preview
        winrt::Windows::UI::Xaml::Controls::TextBox _sessionsSearchBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _sessionsCountText{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeUserBtn{ nullptr }; // 👤 search user messages
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeAgentBtn{ nullptr }; // 🤖 search agent + tools
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeDirsBtn{ nullptr }; // 📁 dirs accessed
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeFilesBtn{ nullptr }; // 📄 files accessed
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessFuzzyBtn{ nullptr }; // (F) fuzzy
        winrt::Windows::UI::Xaml::Controls::Button _sessWindowBtn{ nullptr }; // [1 month] — click cycles presets, hover opens the range popup
        winrt::Windows::UI::Xaml::Controls::Button _sessRefreshBtn{ nullptr }; // ↻ — re-enumerate the window + load-or-refresh each sidecar index (pick up new/updated sessions)
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _sessRangePopup{ nullptr }; // hover: From/To range picker (answer Q4 — text boxes)
        winrt::Windows::UI::Xaml::Controls::TextBox _sessFromBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _sessToBox{ nullptr };
        int _sessionsWindowPreset{ 4 }; // index into {1d,3d,7d,14d,1mo,3mo}; default 1 month
        int64_t _sessionsFromMs{ 0 }; // custom range (popup Apply); 0 = use the preset
        int64_t _sessionsToMs{ 0 }; // 0 = now
        std::vector<_SessionsRow> _sessionsRows; // gathered for the current window (UI thread)
        std::vector<::Agentmaster::SessionIndexEntry> _sessionsEntries; // the sidecar index entries behind the rows (fast-phase haystack)
        std::unordered_map<std::wstring, int> _sessionsHitCounts; // sid -> matched messages (history + slow phase)
        std::unordered_map<std::wstring, std::vector<std::wstring>> _sessionsHitSnippets; // sid -> display snippets
        std::unordered_set<std::wstring> _sessionsFastIds; // fast-phase (title/dir/paths) matches
        std::wstring _sessionsSelectedId;
        int _sessionsSortColumn{ 5 }; // default: Active (last activity), newest first
        bool _sessionsSortAscending{ false };
        std::wstring _sessionsQueryText; // the raw search text (folding happens in the engine)
        std::atomic<bool> _sessionsPageVisible{ false };
        std::atomic<uint64_t> _sessionsSearchGen{ 0 }; // bumps per query — a stale slow search self-cancels
        std::atomic<bool> _sessionsIndexing{ false }; // a background gather/index pass is running
        std::shared_ptr<ThrottledFunc<>> _sessionsSearchThrottled{ nullptr }; // keystroke debounce
        // One-entry detail prompt-list cache, keyed by (id, transcript mtime) — the archive
        // detail's pattern: the head read happens off-thread once, re-shows hit the cache.
        std::wstring _sessionsDetailTiId;
        int64_t _sessionsDetailTiMtime{ 0 };
        std::vector<std::wstring> _sessionsDetailPrompts;
        bool _sessionsDetailPending{ false };
        // The visible row ids in TABLE (sorted+filtered) order — the Up/Down keyboard
        // navigation list (the archive page's _archiveVisibleOrder pattern). Rebuilt each render.
        std::vector<std::wstring> _sessionsVisibleOrder;

        // Agentmaster: WINDOW-LEVEL page overlays (Archive, Sessions, any future full-window
        // page mounted over Root) — each registers itself ONCE at build (host + its atomic
        // visibility mirror + an optional extra-dismiss hook, e.g. closing an owned Popup,
        // which the collapsed host would NOT hide — popups render in the popup root). Generic
        // dismiss sites (the tab-switch seam in TabManagement.cpp) close ALL of them without
        // knowing any page by name, so a new page binds automatically by registering.
        struct _AgentPageOverlay
        {
            winrt::Windows::UI::Xaml::Controls::Grid host{ nullptr };
            std::atomic<bool>* visibleMirror{ nullptr };
            std::function<void()> onDismiss; // optional (close popups etc.); may be empty
        };
        std::vector<_AgentPageOverlay> _agentPageOverlays;

        bool _isInFocusMode{ false };
        bool _isFullscreen{ false };
        bool _isMaximized{ false };
        bool _isAlwaysOnTop{ false };
        bool _showTabsFullscreen{ false };

        std::optional<uint32_t> _loadFromPersistedLayoutIdx{};

        bool _rearranging{ false };
        std::optional<int> _rearrangeFrom{};
        std::optional<int> _rearrangeTo{};
        bool _removing{ false };

        bool _activated{ false };
        bool _visible{ true };

        std::vector<std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>> _previouslyClosedPanesAndTabs{};

        uint32_t _systemRowsToScroll{ DefaultRowsToScroll };

        // use a weak reference to prevent circular dependency with AppLogic
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _dialogPresenter;

        winrt::com_ptr<AppKeyBindings> _bindings{ winrt::make_self<implementation::AppKeyBindings>() };
        winrt::com_ptr<ShortcutActionDispatch> _actionDispatch{ winrt::make_self<implementation::ShortcutActionDispatch>() };

        winrt::Windows::UI::Xaml::Controls::Grid::LayoutUpdated_revoker _layoutUpdatedRevoker;
        StartupState _startupState{ StartupState::NotInitialized };

        std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> _startupActions;
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _startupConnection{ nullptr };

        std::shared_ptr<Toast> _windowIdToast{ nullptr };
        std::shared_ptr<Toast> _actionSavedToast{ nullptr };
        std::shared_ptr<Toast> _actionSaveFailedToast{ nullptr };
        std::shared_ptr<Toast> _windowCwdToast{ nullptr };

        winrt::Windows::UI::Xaml::Controls::TextBox::LayoutUpdated_revoker _renamerLayoutUpdatedRevoker;
        int _renamerLayoutCount{ 0 };
        bool _renamerPressedEnter{ false };

        TerminalApp::WindowProperties _WindowProperties{ nullptr };
        PaneResources _paneResources;

        TerminalApp::ContentManager _manager{ nullptr };

        std::shared_ptr<TerminalSettingsCache> _terminalSettingsCache{};

        struct StashedDragData
        {
            winrt::com_ptr<winrt::TerminalApp::implementation::Tab> draggedTab{ nullptr };
            winrt::Windows::Foundation::Point dragOffset{ 0, 0 };
        } _stashed;

        safe_void_coroutine _NewTerminalByDrop(const Windows::Foundation::IInspectable&, winrt::Windows::UI::Xaml::DragEventArgs e);

        __declspec(noinline) CommandPalette _loadCommandPaletteSlowPath();
        bool _commandPaletteIs(winrt::Windows::UI::Xaml::Visibility visibility);
        __declspec(noinline) SuggestionsControl _loadSuggestionsElementSlowPath();
        bool _suggestionsControlIs(winrt::Windows::UI::Xaml::Visibility visibility);

        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowDialogHelper(const std::wstring_view& name);

        void _ShowAboutDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowConfirmCloseDialog(ConfirmCloseDialogKind kind);
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseReadOnlyDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowMultiLinePasteWarningDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowLargePasteWarningDialog();

        void _CreateNewTabFlyout();
        std::vector<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItemBase> _CreateNewTabFlyoutItems(winrt::Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::NewTabMenuEntry> entries);
        winrt::Windows::UI::Xaml::Controls::IconElement _CreateNewTabFlyoutIcon(const winrt::hstring& icon);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutProfile(const Microsoft::Terminal::Settings::Model::Profile profile, int profileIndex, const winrt::hstring& iconPathOverride);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutAction(const winrt::hstring& actionId, const winrt::hstring& iconPathOverride);

        void _OpenNewTabDropdown();
        HRESULT _OpenNewTab(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs);
        TerminalApp::Tab _CreateNewTabFromPane(std::shared_ptr<Pane> pane, uint32_t insertPosition = -1);
        void _OpenAgentManagerTab(); // Agentmaster
        void _InitAgentmasterEngine(); // Agentmaster: start the SessionRegistry + hooks bridge
        void _SpawnClaudeSession(winrt::hstring workingDir, winrt::hstring title); // Agentmaster
        TerminalApp::Tab _LaunchClaudeSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromId = {}); // Agentmaster (returns the created tab; forkFromId set => fork that conversation into a new id)
        winrt::fire_and_forget _RestoreClaudeSessions(); // Agentmaster: load persisted sessions as ARCHIVED (restorable) — does NOT auto-launch (Rule #6)
        void _RestoreWindowTabs(); // Agentmaster (M10 window-grouped restore): re-home THIS window's persisted tabs — resume each Claude session + replay each Other (shell) tab from its WindowRecord, in order. Only a claimed record (a reopened window) restores.
        void _AttachClaudeOverlay(const TerminalApp::Tab& tab, const std::wstring& sessionId); // Agentmaster: build + install the per-tab link badge (gated on AppSettings.showTabOverlay)
        void _SetTabActivityBadge(const TerminalApp::Tab& tab, const std::wstring& wtSession, const std::wstring& kind); // Agentmaster (OBSERVER.md §4/§11d): attach-or-update a registry-less "○ <kind> · unlinked" badge (pwsh / cmd / claude / codex) on a non-bound tab
        void _DropPendingOverlay(const std::wstring& wtSession); // Agentmaster: collapse + release this window's observe badge for a tab (bound / claude exited / tab gone)
        void _SetTabAgentDot(const TerminalApp::Tab& tab, const std::optional<winrt::Windows::UI::Color>& color); // Agentmaster (tab status dot): show/recolor (nullopt = hide) the tab-strip "[icon] ● <title>" dot via Tab.TabStatus(); idempotent on an unchanged color
        void _UpdateTabAgentDot(const std::wstring& sessionId, ::Agentmaster::SessionState state, bool live); // Agentmaster (tab status dot): the registry-observer reaction — recolor (or hide, !live) the hosting tab's dot; UI thread; no-op when this window doesn't host the session
        void _ActivateClaudeSession(winrt::hstring sessionId); // Agentmaster: jump to a session's tab — local first, then fan out to the hosting window (ActivateSessionInOtherWindows)
        bool _FocusClaudeSessionTab(const std::wstring& sessionId, bool bringWindowToFront); // Agentmaster (cross-window activate): select the session's tab IN THIS WINDOW (no fan-out); optionally foreground this window's HWND (the receiving half of the activate sink). Returns false on a miss.
        void _ArchiveClaudeSession(winrt::hstring sessionId); // Agentmaster: archive (shut down + keep restorable) via the tab-close seam
        void _RestoreArchivedSession(winrt::hstring sessionId); // Agentmaster: re-launch (claude --resume / codex resume) an archived session — kind-aware
        void _AdoptExternalClaude(uint32_t pid, winrt::hstring cwd, bool fork); // Agentmaster (Fleet Observer): bring an EXTERNAL claude's conversation under management (fork==true => --fork-session into a NEW transcript [safe on a live external]; else --resume the same; fresh if none)
        // Agentmaster (Codex managed-session support): launch / restore a codex.exe on a ConPTY as a
        // MANAGED tab, on the same path as Claude. Codex can't pin a session id (no --session-id), so
        // OUR minted id is the durable handle and the real rollout uuid (SessionInfo.codexSessionId,
        // filled by the Fleet Observer) is the `codex resume` target. Lifecycle + state only — no
        // injector / Autopilot (driving the Codex TUI is a later phase).
        void _SpawnCodexSession(winrt::hstring workingDir, winrt::hstring title); // fresh codex in a dir
        TerminalApp::Tab _LaunchCodexSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromCodexUuid = {}); // fresh, `codex resume <uuid>`, or `codex fork <uuid>` (all rollout-gated); returns the created tab
        void _AdoptExternalCodex(uint32_t pid, winrt::hstring cwd, bool fork); // bring an EXTERNAL codex's rollout under management (fork==true => `codex fork` into a NEW rollout [safe on a live external]; else `codex resume` the same; fresh if none)
        void _ReconcileManagedCodex(const std::wstring& sessionId, const ::Agentmaster::TabActivityRow& act); // fill codexSessionId + map the C2 turn-state onto a managed Codex record (UI-lane, per probe)
        std::wstring _ClaudeSessionForTab(const TerminalApp::Tab& tab); // Agentmaster: reverse-lookup _claudeTabs (which session, if any, hosts this tab)
        std::wstring _ClaudeSessionForConnection(const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& conn); // Agentmaster: which managed session is BOUND to this connection (by tabToken == WT_SESSION) — archive on pane-close + re-point injector on restartConnection
        void _DetachClaudeTabForMove(const winrt::com_ptr<Tab>& tab); // Agentmaster (cross-window move): a Claude tab is moving to ANOTHER window (tear-out / moveTab) — evict this window's per-window binding (NOT the injector/live) so teardown can't archive a session now alive elsewhere; the destination re-homes it
        void _DetachClaudePaneForMove(const winrt::com_ptr<Tab>& tab, const std::shared_ptr<Pane>& movingPane); // Agentmaster (cross-window move, pane-level): the movePane-to-window case — evict only if the LEAVING pane is the session's bound (first-terminal) pane; the tab may survive with its other panes
        void _RenameClaudeSession(winrt::hstring sessionId, winrt::hstring title); // Agentmaster: Explorer-tree rename -> registry title (persist) + retitle the session's tab
        void _SyncClaudeTitleFromTab(const TerminalApp::Tab& tab); // Agentmaster: a Claude tab rename -> mirror back into the registry title (the one title)
        void _SyncClaudeTabTitleFromRegistry(const std::wstring& sessionId); // Agentmaster (cross-window rename, Rule #11): the registry-observer reaction — re-pin THIS window's hosting tab to the CURRENT registry title (read fresh, never a stale captured value); UI thread; equality- + latch-guarded, map-miss no-op
        void _SetClaudeTabTextPinned(const winrt::com_ptr<Tab>& tabImpl, const winrt::hstring& title); // Agentmaster (Rule #11): pin a tab title from a registry-driven source WITHOUT bouncing back into the tab->registry mirror (the _pinningClaudeTabTitle latch makes the synchronous _UpdateTitle re-entry a no-op)
        bool _pinningClaudeTabTitle{ false }; // Agentmaster: true while WE programmatically pin a Claude tab's title (registry->tab); makes the synchronous _SyncClaudeTitleFromTab re-entry skip the write-back so the two sync directions can't ping-pong (the /clear re-home title-swap + [Unknown] notify flood)
        void _ApplyDirColorToTab(const TerminalApp::Tab& tab, const std::wstring& dir); // Agentmaster: paint a tab from its working dir's persisted/auto color
        void _ApplyDirColorToTabs(const std::wstring& dir, const std::optional<std::wstring>& colorHex); // Agentmaster: recolor every live tab in a dir
        void _OnClaudeTabColorChanged(const TerminalApp::Tab& tab); // Agentmaster: user changed a tab color -> persist per dir + propagate to same-dir tabs
        winrt::Windows::Foundation::IAsyncAction _ArchiveAndCloseClaudeTab(TerminalApp::Tab tab, std::wstring sessionId, bool skipConfirm); // Agentmaster: confirm -> archive bookkeeping -> close
        void _ArchiveWindowSessionsOnTeardown(); // Agentmaster (lifecycle gap #1): on window close/quit, archive this window's live sessions (live=false + unbind injector -> claude.exe exits) so they don't linger as phantom cards / orphaned processes
        void _PinManagerTabFirst(); // Agentmaster: keep the non-closable Manager tab pinned at index 0 after any reorder
        winrt::fire_and_forget _AdoptExternalSession(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken); // Agentmaster: bind a hand-typed `claude` to its ConPTY
        winrt::fire_and_forget _SweepClaudeLiveness(); // Agentmaster: archive this window's claude tabs whose ConPTY has Closed (scanner-ticked)
        winrt::fire_and_forget _ReconcileClaudeTabs(); // Agentmaster: poll backstop — bind/attach + re-home claude tabs by stable WT_SESSION (scanner-ticked)
        winrt::fire_and_forget _ObserverProbe(); // Agentmaster: the Fleet Observer UI lane — publish this window's tab roster, then bind via the observer's correlation table (replaces _DiscoverClaudeTabsByCwd; OBSERVER.md §10)
        winrt::fire_and_forget _RefreshObserverData(); // Agentmaster: the Explorer Tree "refresh" button's action — Wake the observer (force a survey now) + re-probe + force a Manager redraw once it lands
        void _BindClaudeSessionToTab(const TerminalApp::Tab& hostTab, const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& conn, const std::wstring& id, const std::wstring& cwd, const std::wstring& origin); // Agentmaster: shared bind tail for adoption + discovery
        void _WireAgentManagerContent(const winrt::com_ptr<implementation::AgentManagerContent>& content); // Agentmaster
        // Agentmaster (M10; PERSISTENCE.md §13): capture this window's record (geometry + ordered
        // tab refs + Manager lens) and persist it to windows/<windowId>.json. _CaptureWindowRecord
        // reads live state (mirrors PersistState's geometry recipe); _ScheduleWindowRecordSave is
        // the debounced autosave trigger (structural/lens change); _FlushWindowRecord captures +
        // saves synchronously (close-flush, so the last move/resize isn't lost past the debounce).
        ::Agentmaster::WindowRecord _CaptureWindowRecord();
        void _ScheduleWindowRecordSave();
        void _FlushWindowRecord();

        // Agentmaster (Archive page): the redesigned archive surface — a full-window "page" mounted over
        // TerminalPage's Root content rows (covering all panes below the tab strip), opened by the Archived button via
        // SetOpenArchiveHandler. LEFT = a dense sortable table of archived sessions (+ which saved window
        // each belongs to); RIGHT = a detail/preview of the selected row (metadata + read-only Flight Plan
        // + restore actions); a search filter; multi-select bulk restore. Replaces the in-content overlay.
        void _ShowArchivePage(); // build-if-needed + gather + render + show
        void _HideArchivePage(); // hide (the Back button)
        void _BuildArchivePageShell(); // one-time: host + header (Back/title/search) + table/detail split + footer
        void _GatherArchiveRows(); // fill _archiveRows from RecoverableWindows() + loose archived sessions (+ transcript-stat timing)
        void _RenderArchiveTable(); // apply _archiveFilter + sort to _archiveRows -> rebuild the table + sortable header + selection
        void _UpdateArchiveSelectionHighlight(); // recolor row highlights for _archiveSelectedId WITHOUT a rebuild (deferred row-tap path)
        void _ShowArchiveDetail(const std::wstring& sessionId); // populate the right pane for one row
        void _RestoreCheckedArchived(); // bulk: restore every checked archived session
        void _UpdateArchiveBulkButton(); // refresh the footer "Restore selected (N)" label + enabled
        void _RefreshArchivePageIfVisible(); // re-gather + re-render an OPEN page (registry-observer / backfill poke; UI thread, clean tick)
        winrt::fire_and_forget _BackfillArchiveBranches(std::vector<std::pair<std::wstring, std::wstring>> idDirs); // (id, dir) pairs: head-read gitBranch off-thread -> UpdateQuiet + ONE SaveSessions + a page refresh
        winrt::fire_and_forget _LoadArchiveAssistantTail(std::wstring sessionId, std::wstring dir, int64_t mtime); // tail-read the transcript's LAST assistant message off-thread -> cache (id, mtime) + re-show the detail
        winrt::fire_and_forget _OpenArchiveTranscript(std::wstring path); // detail "Open transcript": ShellExecute the .jsonl (system open/picker; Explorer /select fallback) — read-only, off the UI thread
        void _ReopenSavedWindowById(int fallbackIndex, const std::wstring& windowId); // re-resolve the live record index from the STABLE windowId at action time (a gather-time index goes stale), then _ReopenSavedWindow — shared by the detail "Reopen its window" + the row double-click

        // Agentmaster (Sessions page; SESSIONS.md): the global Claude-sessions browser — every
        // on-disk session in a selectable window (default 1 month), two-phase searched (fast
        // sidecar-index + history.jsonl; slow rg-prefiltered transcript content), rows enriched by
        // the registry (OPEN sessions get the per-dir color + state) + the observer's presence
        // table. Opened by the Manager's "Sessions" button via SetOpenSessionsHandler.
        void _ShowSessionsPage(); // build-if-needed + gather + render + show (deferred — the page-open crash class)
        void _HideSessionsPage(); // hide (the Back button), deferred
        void _BuildSessionsPageShell(); // one-time: host + search bar (text + scope toggles + window selector) + table/detail split
        winrt::fire_and_forget _RefreshSessionsRows(); // BG: enumerate the window + load-or-refresh each sidecar index -> UI: rows + render
        void _RenderSessionsTable(); // apply the current search result set + sort -> rebuild the table
        void _ShowSessionsDetail(const std::wstring& sessionId); // populate the right pane (metadata + hit snippets + prompts + actions)
        winrt::fire_and_forget _RunSessionsSearch(); // the two-phase search: fast inline, slow on a background pass (generation-cancelled)
        winrt::fire_and_forget _LoadSessionsPrompts(std::wstring sessionId, std::wstring dir, int64_t mtime); // detail: off-thread prompt-list read (head), cached by (id, mtime)
        void _CycleSessionsWindow(); // [1 month] click: 1d -> 3d -> 7d -> 14d -> 1mo -> 3mo -> wrap (clears a custom range)
        void _ApplySessionsRange(); // the hover popup's Apply: parse From/To (YYYY-MM-DD) into a custom range
        int64_t _SessionsCutoffFromMs() const; // the active window's from-cutoff (custom range or preset)
        void _ResumeSessionFromDisk(const std::wstring& sessionId, const std::wstring& dir, const std::wstring& title); // resume ANY on-disk session into a managed tab: live here -> Jump; archived -> Restore; unknown -> minimal record + the transcript-gated resume seam
        void _ForkSessionFromDisk(const std::wstring& parentId, const std::wstring& dir, const std::wstring& title); // fork ANY on-disk session into a NEW managed conversation (`--resume <parent> --fork-session --session-id <new>`) — the parent transcript is untouched, so it is safe even while the parent is LIVE; transcript-gated (no transcript -> fresh), the duplicate-tab fork's convention ("<title> (fork)")
        std::wstring _ResolveRestoreChainTail(const std::wstring& clickedId, const std::wstring& dirHint, const std::wstring& titleHint); // Agentmaster: follow a /clear+plan-restart continuation chain to its TAIL (TranscriptStore) so resume/restore land where the user LEFT OFF, not the earliest link they recognize by title; upserts a minimal archived-shaped record for an unmanaged tail. Returns clickedId when there is no newer continuation (or for a Codex record).
        void _UpdateSessionsSelectionHighlight(); // recolor row highlights for _sessionsSelectedId WITHOUT a rebuild (row-tap + keyboard nav)
        void _MoveSessionsSelection(int delta); // Up/Down keyboard nav over _sessionsVisibleOrder: none selected => Down=first / Up=last; wraps (rotates) at the ends
        void _HideSessionFromList(const std::wstring& sessionId); // Sessions-page row right-click "Hide from list": append to AppSettings.hiddenSessionIds (freshest-disk RMW) + drop it from the table (the transcript on disk is untouched)
        void _ResetHiddenSessions(); // Settings cog "Reset hidden sessions" (via SetResetHiddenSessionsHandler): clear AppSettings.hiddenSessionIds (RMW) + re-render so every hidden session reappears
        // Agentmaster: the generic window-level page-overlay seam (_agentPageOverlays) — register
        // at page build; dismiss-all from any global site (the tab-switch handler). See the struct.
        void _RegisterAgentPageOverlay(const winrt::Windows::UI::Xaml::Controls::Grid& host, std::atomic<bool>* visibleMirror, std::function<void()> onDismiss);
        void _DismissAgentPageOverlays(); // synchronous collapse of EVERY registered page (safe outside in-page pointer handlers)
        void _MoveArchiveSelection(int delta); // Up/Down keyboard nav over _archiveVisibleOrder (same rotate semantics as the Sessions page)

        std::wstring _evaluatePathForCwd(std::wstring_view path);

        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _CreateConnectionFromSettings(Microsoft::Terminal::Settings::Model::Profile profile, Microsoft::Terminal::Control::IControlSettings settings, const bool inheritCursor);
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _duplicateConnectionForRestart(const TerminalApp::TerminalPaneContent& paneContent);
        void _restartPaneConnection(const TerminalApp::TerminalPaneContent&, const winrt::Windows::Foundation::IInspectable&);

        safe_void_coroutine _OpenNewWindow(const Microsoft::Terminal::Settings::Model::INewContentArgs newContentArgs);
        // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): reopen every saved window NOT currently
        // open, each via `wt -w -1 -s <idx>` (the same new-window-by-persisted-index path the Emperor
        // uses at startup) — the Manager's "Reopen Windows (N)" recover button.
        safe_void_coroutine _ReopenSavedWindows();
        // Agentmaster (M10 window-grouped restore): reopen ONE saved window by its canonical record index
        // (`agentmaster -w -1 -s <idx>`) — the per-window "Reopen window" button in the grouped Archived
        // overlay. Same single-instance handoff as _ReopenSavedWindows, for a single record.
        safe_void_coroutine _ReopenSavedWindow(int index);

        void _OpenNewTerminalViaDropdown(const Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        bool _displayingCloseDialog{ false };
        void _SettingsButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _CommandPaletteButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _AboutButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);

        void _KeyDownHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        void _HookupKeyBindings(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap) noexcept;
        void _RegisterActionCallbacks();

        void _UpdateTitle(const Tab& tab);
        void _UpdateTabIcon(Tab& tab);
        void _UpdateTabView();
        void _UpdateTabWidthMode();
        void _SetBackgroundImage(const winrt::Microsoft::Terminal::Settings::Model::IAppearanceConfig& newAppearance);

        void _DuplicateFocusedTab();
        void _DuplicateTab(const Tab& tab);

        safe_void_coroutine _ExportTab(const Tab& tab, winrt::hstring filepath);

        winrt::Windows::Foundation::IAsyncAction _HandleCloseTabRequested(winrt::TerminalApp::Tab tab, bool skipConfirmClose = false);
        void _CloseTabAtIndex(uint32_t index);
        void _RemoveTab(const winrt::TerminalApp::Tab& tab);
        safe_void_coroutine _RemoveTabs(const std::vector<winrt::TerminalApp::Tab> tabs);

        void _InitializeTab(winrt::com_ptr<Tab> newTabImpl, uint32_t insertPosition = -1);
        void _RegisterTerminalEvents(Microsoft::Terminal::Control::TermControl term);
        void _RegisterTabEvents(Tab& hostingTab);

        void _DismissTabContextMenus();
        void _FocusCurrentTab(const bool focusAlways);
        bool _HasMultipleTabs() const;

        void _SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode);
        bool _SelectTab(uint32_t tabIndex);
        bool _MoveFocus(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _SwapPane(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool _MovePane(const Microsoft::Terminal::Settings::Model::MovePaneArgs args);
        bool _MoveTab(winrt::com_ptr<Tab> tab, const Microsoft::Terminal::Settings::Model::MoveTabArgs args);

        std::shared_ptr<ThrottledFunc<>> _adjustProcessPriorityThrottled;
        void _adjustProcessPriority() const;

        template<typename F>
        bool _ApplyToActiveControls(F f) const
        {
            if (const auto tab{ _GetFocusedTabImpl() })
            {
                if (const auto activePane = tab->GetActivePane())
                {
                    activePane->WalkTree([&](auto p) {
                        if (const auto& control{ p->GetTerminalControl() })
                        {
                            f(control);
                        }
                    });

                    return true;
                }
            }
            return false;
        }

        winrt::Microsoft::Terminal::Control::TermControl _GetActiveControl() const;
        std::optional<uint32_t> _GetFocusedTabIndex() const noexcept;
        std::optional<uint32_t> _GetTabIndex(const TerminalApp::Tab& tab) const noexcept;
        TerminalApp::Tab _GetFocusedTab() const noexcept;
        winrt::com_ptr<Tab> _GetFocusedTabImpl() const noexcept;
        TerminalApp::Tab _GetTabByTabViewItem(const IInspectable& tabViewItem) const noexcept;

        void _HandleClosePaneRequested(std::shared_ptr<Pane> pane);
        bool _ShouldWarnOnClose() const;
        bool _ShouldWarnOnCloseTab(const winrt::com_ptr<Tab>& tab) const;
        safe_void_coroutine _SetFocusedTab(const winrt::TerminalApp::Tab tab);
        safe_void_coroutine _CloseFocusedPane();
        safe_void_coroutine _ClosePanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds);
        void _CloseRemainingPanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds);
        winrt::Windows::Foundation::IAsyncOperation<bool> _PaneConfirmCloseReadOnly(std::shared_ptr<Pane> pane);
        void _AddPreviouslyClosedPaneOrTab(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>&& args);

        void _Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll);

        void _SplitPane(const winrt::com_ptr<Tab>& tab,
                        const Microsoft::Terminal::Settings::Model::SplitDirection splitType,
                        const float splitSize,
                        std::shared_ptr<Pane> newPane);
        bool _ResizePane(const Microsoft::Terminal::Settings::Model::ResizeDirection& direction);
        void _ToggleSplitOrientation();

        void _ScrollPage(ScrollDirection scrollDirection);
        void _ScrollToBufferEdge(ScrollDirection scrollDirection);
        void _SetAcceleratorForMenuItem(Windows::UI::Xaml::Controls::MenuFlyoutItem& menuItem, const winrt::Microsoft::Terminal::Control::KeyChord& keyChord);

        safe_void_coroutine _PasteFromClipboardHandler(const IInspectable sender,
                                                       const Microsoft::Terminal::Control::PasteFromClipboardEventArgs eventArgs);

        safe_void_coroutine _OpenHyperlinkHandler(const IInspectable sender, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs);
        static bool _IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri);
        bool _IsUriConsideredSomewhatSafe(const winrt::Windows::Foundation::Uri& parsedUri) const;

        void _ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri);
        bool _CopyText(bool dismissSelection, bool singleLine, bool withControlSequences, Microsoft::Terminal::Control::CopyFormat formats);

        safe_void_coroutine _SetTaskbarProgressHandler(const IInspectable sender, const IInspectable eventArgs);

        void _copyToClipboard(IInspectable, Microsoft::Terminal::Control::WriteToClipboardEventArgs args) const;
        void _PasteText();

        safe_void_coroutine _ControlNoticeRaisedHandler(const IInspectable sender, const Microsoft::Terminal::Control::NoticeEventArgs eventArgs);
        void _ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message);

        safe_void_coroutine _LaunchSettings(const Microsoft::Terminal::Settings::Model::SettingsTarget target);

        void _TabDragStarted(const IInspectable& sender, const IInspectable& eventArgs);
        void _TabDragCompleted(const IInspectable& sender, const IInspectable& eventArgs);

        // BODGY: WinUI's TabView has a broken close event handler:
        // If the close button is disabled, middle-clicking the tab raises no close
        // event. Because that's dumb, we implement our own middle-click handling.
        // `_tabItemMiddleClickHookEnabled` is true whenever the close button is hidden,
        // and that enables all of the rest of this machinery (and this workaround).
        bool _tabItemMiddleClickHookEnabled = false;
        bool _tabItemMiddleClickExited = false;
        PointerEntered_revoker _tabItemMiddleClickPointerEntered;
        PointerExited_revoker _tabItemMiddleClickPointerExited;
        PointerCaptureLost_revoker _tabItemMiddleClickPointerCaptureLost;
        void _OnTabPointerPressed(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        safe_void_coroutine _OnTabPointerReleasedCloseTab(IInspectable sender);

        void _OnTabSelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _OnTabItemsChanged(const IInspectable& sender, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs);
        void _OnTabCloseRequested(const IInspectable& sender, const Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& eventArgs);
        void _OnFirstLayout(const IInspectable& sender, const IInspectable& eventArgs);
        void _UpdatedSelectedTab(const winrt::TerminalApp::Tab& tab);
        void _UpdateBackground(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);

        void _OnDispatchCommandRequested(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& command);
        void _OnCommandLineExecutionRequested(const IInspectable& sender, const winrt::hstring& commandLine);
        void _OnSwitchToTabRequested(const IInspectable& sender, const winrt::TerminalApp::Tab& tab);

        void _Find(const Tab& tab);

        winrt::Microsoft::Terminal::Control::TermControl _CreateNewControlAndContent(const winrt::Microsoft::Terminal::Settings::TerminalSettingsCreateResult& settings,
                                                                                     const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& connection);
        winrt::Microsoft::Terminal::Control::TermControl _SetupControl(const winrt::Microsoft::Terminal::Control::TermControl& term);
        winrt::Microsoft::Terminal::Control::TermControl _AttachControlToContent(const uint64_t& contentGuid);

        TerminalApp::IPaneContent _makeSettingsContent();
        std::shared_ptr<Pane> _MakeTerminalPane(const Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs = nullptr,
                                                const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                                winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);
        std::shared_ptr<Pane> _MakePane(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs = nullptr,
                                        const winrt::TerminalApp::Tab& sourceTab = nullptr,
                                        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);

        void _RefreshUIForSettingsReload();

        void _SetNewTabButtonColor(til::color color, til::color accentColor);
        void _ClearNewTabButtonColor();

        safe_void_coroutine _CompleteInitialization();

        void _FocusActiveControl(IInspectable sender, IInspectable eventArgs);

        void _UnZoomIfNeeded();

        static int _ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll);
        static uint32_t _ReadSystemRowsToScroll();

        void _UpdateMRUTab(const winrt::TerminalApp::Tab& tab);

        void _TryMoveTab(const uint32_t currentTabIndex, const int32_t suggestedNewTabIndex);

        void _PreviewAction(const Microsoft::Terminal::Settings::Model::ActionAndArgs& args);
        void _PreviewActionHandler(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& args);
        void _EndPreview();
        void _RunRestorePreviews();
        void _PreviewColorScheme(const Microsoft::Terminal::Settings::Model::SetColorSchemeArgs& args);
        void _PreviewAdjustOpacity(const Microsoft::Terminal::Settings::Model::AdjustOpacityArgs& args);
        void _PreviewSendInput(const Microsoft::Terminal::Settings::Model::SendInputArgs& args);

        winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs _lastPreviewedAction{ nullptr };
        std::vector<std::function<void()>> _restorePreviewFuncs{};

        HRESULT _OnNewConnection(const winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection& connection);
        void _HandleToggleInboundPty(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

        void _WindowRenamerActionClick(const IInspectable& sender, const IInspectable& eventArgs);
        void _RequestWindowRename(const winrt::hstring& newName);
        void _WindowRenamerKeyDown(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _WindowRenamerKeyUp(const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);

        void _UpdateTeachingTipTheme(winrt::Windows::UI::Xaml::FrameworkElement element);

        winrt::Microsoft::Terminal::Settings::Model::Profile GetClosestProfileForDuplicationOfProfile(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile) const noexcept;

        bool _maybeElevate(const winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs,
                           const winrt::Microsoft::Terminal::Settings::TerminalSettingsCreateResult& controlSettings,
                           const winrt::Microsoft::Terminal::Settings::Model::Profile& profile);
        void _OpenElevatedWT(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        safe_void_coroutine _ConnectionStateChangedHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args);
        void _CloseOnExitInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        void _KeyboardServiceWarningInfoDismissHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable& args) const;
        static bool _IsMessageDismissed(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);
        static void _DismissMessage(const winrt::Microsoft::Terminal::Settings::Model::InfoBarMessage& message);

        void _updateThemeColors();
        void _updateAllTabCloseButtons();
        void _updatePaneResources(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme);

        safe_void_coroutine _ControlCompletionsChangedHandler(const winrt::Windows::Foundation::IInspectable sender, const winrt::Microsoft::Terminal::Control::CompletionsChangedEventArgs args);

        void _OpenSuggestions(const Microsoft::Terminal::Control::TermControl& sender, Windows::Foundation::Collections::IVector<winrt::Microsoft::Terminal::Settings::Model::Command> commandsCollection, winrt::TerminalApp::SuggestionsMode mode, winrt::hstring filterText);

        void _ShowWindowChangedHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::ShowWindowArgs args);
        Windows::Foundation::IAsyncAction _SearchMissingCommandHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::SearchMissingCommandEventArgs args);
        static Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Management::Deployment::MatchResult>> _FindPackageAsync(hstring query);

        void _WindowSizeChanged(const IInspectable sender, const winrt::Microsoft::Terminal::Control::WindowSizeChangedEventArgs args);
        void _windowPropertyChanged(const IInspectable& sender, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args);

        void _onTabDragStarting(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs& e);
        void _onTabStripDragOver(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::DragEventArgs& e);
        void _onTabStripDrop(winrt::Windows::Foundation::IInspectable sender, winrt::Windows::UI::Xaml::DragEventArgs e);
        void _onTabDroppedOutside(winrt::Windows::Foundation::IInspectable sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabDroppedOutsideEventArgs e);

        void _DetachPaneFromWindow(std::shared_ptr<Pane> pane);
        void _DetachTabFromWindow(const winrt::com_ptr<Tab>& tabImpl);
        void _MoveContent(std::vector<winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs>&& actions,
                          const winrt::hstring& windowName,
                          const uint32_t tabIndex,
                          const std::optional<winrt::Windows::Foundation::Point>& dragPoint = std::nullopt);
        void _sendDraggedTabToWindow(const winrt::hstring& windowId, const uint32_t tabIndex, std::optional<winrt::Windows::Foundation::Point> dragPoint);

        void _PopulateContextMenu(const Microsoft::Terminal::Control::TermControl& control, const Microsoft::UI::Xaml::Controls::CommandBarFlyout& sender, const bool withSelection);
        void _PopulateQuickFixMenu(const Microsoft::Terminal::Control::TermControl& control, const Windows::UI::Xaml::Controls::MenuFlyout& sender);
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _CreateRunAsAdminFlyout(int profileIndex);

        winrt::Microsoft::Terminal::Control::TermControl _senderOrActiveControl(const winrt::Windows::Foundation::IInspectable& sender);
        winrt::com_ptr<Tab> _senderOrFocusedTab(const IInspectable& sender);

        void _activePaneChanged(winrt::TerminalApp::Tab tab, Windows::Foundation::IInspectable args);
        safe_void_coroutine _doHandleSuggestions(Microsoft::Terminal::Settings::Model::SuggestionsArgs realArgs);

        void _SendDesktopNotification(const winrt::hstring& tabTitle, const winrt::hstring& body, const winrt::com_ptr<Tab>& tab, const winrt::TerminalApp::IPaneContent& content);

#pragma region ActionHandlers
        // These are all defined in AppActionHandlers.cpp
#define ON_ALL_ACTIONS(action) DECLARE_ACTION_HANDLER(action);
        ALL_SHORTCUT_ACTIONS
        INTERNAL_SHORTCUT_ACTIONS
#undef ON_ALL_ACTIONS
#pragma endregion

        friend class TerminalAppLocalTests::TabTests;
        friend class TerminalAppLocalTests::SettingsTests;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalPage);
}
