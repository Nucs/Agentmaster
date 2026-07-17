// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

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
#include "AgentMaster/SessionStore.h" // Agentmaster (bookmark tags): GlobalTagInfo (by-value member — the Tag panel's cached universe)
#include "AgentMaster/TranscriptStore.h" // Agentmaster (Sessions page): SessionIndexEntry (by-value member)
#include "AgentLocalTooltip.h" // Agentmaster (LocalTooltip): the Sessions page's designated-area tooltip panel (by-value member)
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
        // Agentmaster: lightweight apply for a keyboard-layout change — re-resolve keybindings
        // only, without the per-pane settings reapply that froze the window on a language switch.
        void RefreshKeybindings(Microsoft::Terminal::Settings::Model::CascadiaSettings settings);

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
        void SetAgentmasterContentWindow(bool isContentWindow); // Agentmaster: this window hosts MOVED content (tab tear-out / cross-window move) — mint a fresh record, never adopt a leftover one

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

        // Agentmaster: the two tab-strip nav buttons in the TabStripHeader (immediately left of the
        // `<` scroll arrow), driven by _UpdateManagerNavButtons. _managerHomeButton appears when the
        // pinned Manager tab scrolls out of view (jump TO it); _managerJumpBackButton is its inverse —
        // shown while the Manager tab is active with a session card selected, it jumps BACK to that
        // session's tab. Plus the TabView's internal horizontal ScrollViewer they key off (found lazily
        // once the strip is templated) and the last-known Manager tab width (cached because the
        // container virtualizes away once scrolled off, so its live ActualWidth reads 0).
        Windows::UI::Xaml::Controls::Button _managerHomeButton{ nullptr };
        Windows::UI::Xaml::Controls::Button _managerJumpBackButton{ nullptr };
        Windows::UI::Xaml::Controls::ScrollViewer _tabStripScrollViewer{ nullptr };
        double _managerTabWidthCache{ 0.0 };
        Windows::UI::Xaml::Controls::ScrollViewer::ViewChanged_revoker _tabStripViewChangedRevoker;
        Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker _tabStripSizeChangedRevoker;

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
        // Agentmaster (Linked Lenses): per-tab "selected/active" pill state. _managerHoverSessionId is
        // the managed session the pointer is over in the Manager (a board card / tree row), pushed by
        // the content's hover handler; _pilledSessionId is the session whose tab currently wears the
        // pill (so the next update can clear the old one). The pill shows ONLY while the Manager tab is
        // the active tab; hover takes precedence over the lens selection (a live preview that follows
        // the mouse). Driven by _UpdateManagerSelectionHighlight / _SetTabSelectionPill.
        std::wstring _managerHoverSessionId;
        std::wstring _pilledSessionId;
        // Agentmaster (Linked Lenses — selection follows into view): the last Manager SELECTION the
        // reveal pass saw. When the selection changes WHILE the Manager tab is active (a board card /
        // tree row click), the selected session's tab is revealed in the strip (_RevealTabInStrip) so
        // the selection pill isn't left scrolled off-screen. Tracked on EVERY highlight pass — also
        // off-Manager, where the selection follows tab switches via the funnel — so only an on-Manager
        // selection CHANGE scrolls: hover previews, re-selects, and merely RETURNING to the Manager tab
        // (after the selection moved underneath it) never yank the strip.
        std::wstring _selectionBroughtIntoView;

        // Agentmaster: the session-management engine (see AgentMaster/). SessionRegistry is
        // the single source of truth; HooksBridge feeds it authoritative state from Claude
        // Code hooks over a local named pipe. M9: these are the ONE process-wide engine
        // (::Agentmaster::SharedEngine()), shared by every window; this page just holds copies
        // of the shared_ptrs. Forward-declared here (HooksBridge's dtor joins its threads).
        std::shared_ptr<::Agentmaster::SessionRegistry> _sessionRegistry{ nullptr };
        std::shared_ptr<::Agentmaster::HooksBridge> _hooksBridge{ nullptr };
        std::shared_ptr<::Agentmaster::Scheduler> _scheduler{ nullptr }; // Agentmaster: Autorunner
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
        // another window's Activate (board/tree double-click, tree Enter, the Auto Testing's eye) on
        // a session hosted HERE hops to this window's UI thread, selects the session's tab, and
        // brings this window to the foreground. Detached in ~TerminalPage (Rule #10).
        uint64_t _windowActivateToken{ 0 };
        // Agentmaster (cross-window restart): this window's restart sink on the shared engine — another
        // window's "Restart session" (Triage Board card / Explorer-tree row) on a session hosted HERE
        // hops to this window's UI thread and rebuilds its ConPTY connection in place. Detached in
        // ~TerminalPage (Rule #10).
        uint64_t _windowRestartToken{ 0 };
        // Agentmaster (eager-init / "Activate All Tabs"): this window's "wake all dormant tabs" sink on
        // the shared engine — the Manager's fleet-wide "Activate All" in ANOTHER window fans out here, and
        // this window eager-inits its own dormant controls. Detached in ~TerminalPage (Rule #10).
        uint64_t _windowActivateAllToken{ 0 };
        // Agentmaster (eager-init / "Activate All Tabs" PACING): waking N dormant tabs in one burst spawns
        // N claude.exe + N swapchains on one UI-thread pass and freezes the app (enough of them, the PC),
        // so the wake is DRIP-FED: _activateAllQueue holds the session ids still to wake and
        // _activateAllTimer paces them — kActivateAllSpacingMs (500ms) between wakes, at most
        // kActivateAllBatchSize (4) wakes per kActivateAllBatchPeriodMs (10s) batch (constants in
        // TerminalPage.AgentObserver.cpp). Only a REAL wake consumes a pacing slot (an id that started/
        // closed meanwhile skips free at pop time); a second "Activate All" while dripping MERGES
        // (dedupes) into the running queue instead of bursting. The timer self-stops when the queue
        // drains, in ~TerminalPage, and on a dead page (weak Tick). UI thread only.
        std::vector<std::wstring> _activateAllQueue;
        winrt::Windows::UI::Xaml::DispatcherTimer _activateAllTimer{ nullptr };
        int _activateAllBatchWoken{ 0 }; // wakes in the current 4-per-10s batch
        int _activateAllWokenTotal{ 0 }; // total woken by the running drip (the drained-log count)
        // Agentmaster (cross-window settings broadcast): this window's settings sink on the shared
        // engine — a GLOBAL settings change in ANOTHER window (the cog Save, or the Explorer-Tree /
        // Triage-Board sort toggle) hops to this window's UI thread and re-applies it live (the sort
        // toggles + a board/tree re-sort). Detached in ~TerminalPage (Rule #10).
        uint64_t _settingsChangedToken{ 0 };
        ::Agentmaster::AppSettings _appSettings{}; // Agentmaster: global settings (the cog); loaded at engine init
        // Agentmaster: sessionId -> its terminal tab, so the Manager can Activate (jump) or
        // Kill a session. Weak so closing a tab the normal way doesn't keep it alive.
        std::unordered_map<std::wstring, winrt::weak_ref<TerminalApp::Tab>> _claudeTabs;
        // Agentmaster (TAB_OVERLAY.md): sessionId -> its per-tab "link badge" overlay. STRONG ref
        // (the page builds + owns it); erased alongside _claudeTabs on archive/close/liveness so the
        // overlay's registry observer detaches. Type completed in AgentTabOverlay.h (TerminalPage.cpp).
        std::unordered_map<std::wstring, winrt::com_ptr<implementation::AgentTabOverlay>> _claudeOverlays;
        // Agentmaster (PENDING_INPUT.md): per-session consecutive-empty-read counter for the unsent-draft
        // scan's CLEAR debounce (eager show / lazy hide). A non-empty read shows the pending dots
        // immediately (and zeroes this); an empty read only CLEARS once it has been empty for
        // kPendingClearConfirmTicks consecutive scans, so a single mid-repaint frame can't flicker the
        // indicator off. Keyed by sessionId; entries are pruned with their tab in the liveness sweep.
        std::unordered_map<std::wstring, int> _pendingClearStreak;
        // Agentmaster (tab color modes — InferredWorkingDirectory + the home-dir forcing): per-session
        // state for the
        // inferred-workdir scan (_ScanInferredTabColors): the resolved transcript path (globbed once,
        // cached), the transcript mtime the last inference ran at (re-infer only when it GREW), and a
        // per-session next-run throttle (a busy transcript grows every tick — the sidecar accumulate is
        // incremental, but there's no need to re-infer more than every ~15s). Touched UI-thread only;
        // entries pruned with their tab; holds ONLY admitted sessions (SessionInfersWorkingDir — the
        // whole fleet under the Inferred mode, just %USERPROFILE%-launched ones in the other modes).
        // Sized like _claudeTabs (a handful).
        struct InferredColorScan
        {
            std::wstring transcriptPath; // resolved once (empty => not on disk yet — retry next pass)
            int64_t lastMtimeMs{ 0 }; // transcript mtime at the last inference
            int64_t nextRunMs{ 0 }; // GetTickCount64 floor for the next inference (throttle)
        };
        std::unordered_map<std::wstring, InferredColorScan> _inferredColorScan;
        // Agentmaster (OBSERVER.md §11d): wtSession -> a registry-LESS "claude · unlinked" pending
        // overlay for a tab whose claude the observer correlated but can't resolve a conversation id
        // for yet (never prompted). Keyed by WT_SESSION (there is no sessionId). Replaced by the real
        // _claudeOverlays entry once the session resolves; pruned when the tab leaves this window's roster.
        std::unordered_map<std::wstring, winrt::com_ptr<implementation::AgentTabOverlay>> _pendingOverlays;
        // Agentmaster (alt+up/down prompt nav): per-session cache of the transcript's sent prompts + the
        // (path, mtime) they were read at, so stepping between off-screen prompts re-reads the transcript
        // only when it GREW. Populated off-thread by _ScrollAdjacentPrompt; touched UI-thread only.
        struct PromptNavCache
        {
            std::wstring path;
            int64_t mtime{ 0 };
            std::vector<std::wstring> prompts;
        };
        std::unordered_map<std::wstring, PromptNavCache> _promptNavCache;

        // Agentmaster (tab status-dot RED FLASH): when a hosted session goes from Running to a "now it's
        // on you / at rest" state — Idle / WaitingForInput / NeedsApproval (NOT Done or Error) — while
        // its tab is NOT the active/visited one, a RED RING blinks around that tab's status dot (a
        // separate ellipse behind the dot, peeking out around its constant black stroke) until you
        // switch to it (the current tab is always considered visited, so it never flashes).
        // _agentFlashLastState remembers each hosted session's last state so the registry-observer
        // reaction can detect the Running -> {Idle/WaitingForInput/NeedsApproval} edge; _flashingSessions is the set of
        // sessions whose tab is currently flashing. ONE shared per-window DispatcherTimer toggles
        // _agentFlashPhase every 600ms and shows/hides EVERY flashing tab's red ring together, so
        // multiple flashing tabs blink in lockstep (the synchronization requirement) — and a tab that
        // starts flashing mid-cycle joins at the current phase. UI thread only.
        std::unordered_map<std::wstring, ::Agentmaster::SessionState> _agentFlashLastState;
        std::unordered_set<std::wstring> _flashingSessions;
        // Agentmaster (Mark Unread): sessions MANUALLY marked unread via the tab context menu. Drives the
        // SAME red ring on the SAME shared timer (a session's tab flashes if it is in EITHER set), but is
        // STICKY: the automatic state logic never clears it, and it flashes even the currently-FOCUSED tab
        // — only a VISIT (a switch TO the tab, _VisitTabClearFlash) or archive clears it.
        std::unordered_set<std::wstring> _manualUnreadSessions;
        winrt::Windows::UI::Xaml::DispatcherTimer _agentFlashTimer{ nullptr };
        bool _agentFlashPhase{ false };
        // Agentmaster (System notifications): the toast tracker's per-session state — DELIBERATELY
        // separate from _agentFlashLastState (the flash erases/holds its map on its own rules; coupling
        // the two would let a flash change silently break the toast edge detection, and vice versa).
        // _agentNotifyLastState remembers each hosted session's last seen state so the registry-observer
        // reaction can detect the Running -> X edge; _agentNotifyRunningSinceMs stamps when a session
        // ENTERED Running (only on a SEEN edge — a session first observed already-Running, e.g. adopted
        // mid-turn, gets no stamp and its toast omits the "after <duration>" clause rather than
        // under-reporting). Both erased on !live (incl. when the archive seams dropped _claudeTabs before
        // the queued observer hop landed — the cleanup runs ahead of the host gate) and at the move-out
        // seams, so a later restore starts a fresh track (no phantom Running -> X toast). UI thread only.
        std::unordered_map<std::wstring, ::Agentmaster::SessionState> _agentNotifyLastState;
        std::unordered_map<std::wstring, int64_t> _agentNotifyRunningSinceMs;
        bool _agentToastFailLogged{ false }; // log the first toast failure only (an unpackaged build throws on every Show — once is signal, per-fire is noise)
        // Agentmaster (System notifications — spurious-completion HOLD + double-toast guard): a
        // Running -> Idle/WaitingForInput toast is HELD (not shown) while the session's external-work
        // signal (PresenceIsWorking / fresh subagent side files) says a shell / background agent /
        // teammate is still live — the scanner's outlived-turn promotion re-lights such a session
        // Running ~20s after the real Stop, and a shown toast can't be recalled (SessionScanner.h's
        // toast gates; proven live on 513d1366). _agentToastHeld carries the deferred payload (the
        // edge's target state + the consumed Running-entry stamp + when the hold began); the liveness
        // tick sweeps it (_SweepAgentPendingToasts -> DecideHeldToast): re-lit Running => DROP, signal
        // cleared / moved to a hard needs-you state / kNotifyExternalHoldCapMs => FIRE with the
        // CURRENT state. _agentToastLastShownMs rate-limits: at most one SHOWN toast per session per
        // kNotifyDuplicateToastMs (a Waiting->Running->Waiting flap otherwise toasts on every Waiting
        // entry). Both erased at the same seams as _agentNotifyLastState (!live, move-out) so a later
        // restore starts fresh. UI thread only.
        struct AgentHeldToast
        {
            ::Agentmaster::SessionState state{}; // the target state at the edge (logging; the fire re-reads the CURRENT state)
            int64_t runningSinceMs{}; // the Running-entry stamp consumed at the edge (0 == entry unseen -> no "after <span>" clause)
            int64_t heldAtMs{}; // when the hold began (the cap clock)
        };
        std::unordered_map<std::wstring, AgentHeldToast> _agentToastHeld;
        std::unordered_map<std::wstring, int64_t> _agentToastLastShownMs;
        // Agentmaster (status-dot flash-ring COLOR): the ONE brush every flashing tab in THIS window
        // shares as its ring Fill — the user-configurable "status flashing color" (Settings cog ->
        // AppSettings::flashRingColor; its alpha channel = the ring opacity). Built lazily on the first
        // flash from the current setting (_EnsureFlashRingBrush); a settings Save / cross-window
        // broadcast mutates its Color in place (_RefreshFlashRingBrush), which live-updates every tab
        // bound to it. Null until the first flash (the ring is collapsed until then). UI thread only.
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _flashRingBrush{ nullptr };
        // Agentmaster (alt+up/down prompt nav, SUMMARY_JUMP.md §7): while a managed Claude tab is focused,
        // re-read its sent prompts (mtime-gated) + re-resolve the summary panel's jump eligibility every
        // 30 s, so the jump data stays in sync with the live buffer without a keypress. Free-running; each
        // tick no-ops unless the focused tab is a Claude session. Started in _InitAgentmasterEngine, stopped
        // in ~TerminalPage.
        winrt::Windows::UI::Xaml::DispatcherTimer _promptNavRefreshTimer{ nullptr };

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
        // Agentmaster (discard Manager-only windows): a debounced check that self-closes (or, for the
        // last window, simply un-persists) a window that has ended up holding ONLY the pinned Manager
        // tab. Debounced (not immediate) so a window mid-restore — transiently Manager-only while its
        // shell tabs are still being re-homed async — is evaluated only once the tab set SETTLES, and so
        // a burst of tab churn collapses to one decision. _managerOnlyDiscard latches "this window is
        // Manager-only via settle/user-action, so its record must not persist": set by
        // _CloseWindowIfManagerOnly, honored by _FlushWindowRecord (delete the record instead of saving),
        // cleared the moment a real tab returns. NEVER set during restore (the flag's only writer is the
        // post-startup check), so an only-shells reopen is never mistaken for a discard.
        std::shared_ptr<ThrottledFunc<>> _managerOnlyCheckThrottled{ nullptr };
        bool _managerOnlyDiscard{ false };
        // True when _windowRecord was CLAIMED from disk (a real prior layout) vs freshly minted.
        // Only a claimed record seeds the Manager lens on wire — a fresh window keeps the content's
        // ctor-loaded global splitter sizes, so opening a new window never resets them to default.
        bool _windowRecordClaimed{ false };
        // Agentmaster (splash): the deferred launch-splash dismiss watcher (see _ScheduleSplashDismiss).
        // _OnFirstLayout returning is ~20s too early — the restored tabs' TermControls + claude.exe init
        // LAZILY after it — so a DispatcherTimer polls until the window has SETTLED (foreground terminal
        // connected + the UI thread responsive again) and dismisses the process-wide splash then.
        // Transient, launch-only state; the timer self-stops on dismiss.
        winrt::Windows::UI::Xaml::DispatcherTimer _splashDismissTimer{ nullptr };
        uint64_t _splashDismissStart{ 0 }; // GetTickCount64 at watch begin (end of _OnFirstLayout)
        uint64_t _splashLastTick{ 0 }; // last tick time — tick punctuality measures UI-thread idleness
        int _splashSmoothTicks{ 0 }; // consecutive punctual ticks => the UI thread has drained the restore work
        bool _splashFgConnectedLogged{ false }; // one-shot [startup] log latch for "foreground terminal connected"
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
        // Agentmaster: true when this window was created from MOVED content — a tab torn out into a new
        // window (_onTabDroppedOutside) or moved cross-window (moveTab). Set by TerminalWindow before
        // _OnFirstLayout. Such a window's content comes from the moved-content startup actions, so it must
        // NOT front-pop a leftover on-disk record in _InitAgentmasterEngine: claiming one sets
        // _windowRecordClaimed, which makes _OnFirstLayout skip ProcessStartupActions (the moved content)
        // and makes _RestoreWindowTabs replay the stale record instead — silently dropping the dragged
        // session. Mirrors upstream's "moved content wins over a persisted layout" in TerminalWindow.
        bool _isContentWindow{ false };

        // Agentmaster (FAVORITES.md): the full-window Archive page + all its state were REMOVED — the
        // Sessions browser is the sole history view. (The retired _archive* members/struct lived here.)
        // Agentmaster (tab status dot): this window's registry observer driving the tab-strip
        // "[icon] ● <title>" dot — a state change recolors the hosting tab's dot in place (the same
        // push that redraws the Manager board). Registered at engine init; detached in ~TerminalPage
        // (an ::Agentmaster::ObserverToken; uint64_t to avoid pulling SessionRegistry.h here — the
        // _adoptionToken pattern).
        uint64_t _agentDotObserverToken{ 0 };

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
            int64_t contextTokens{ 0 }; // newest assistant turn's usage (≈ context occupancy); the compact "Ctx" column — the same value the Triage Board shows as "ctx N"
            bool fork{ false };
            std::wstring forkedFromId;
        };
        winrt::Windows::UI::Xaml::Controls::Grid _sessionsPageHost{ nullptr }; // full-bleed page over Root rows 1-2
        // Agentmaster (LocalTooltip, AgentLocalTooltip.h): the page's hover-description panel — every
        // tip inside the page renders in a click-through panel at the page's TOP-RIGHT (the header's
        // empty right column, above the detail pane) instead of as a floating ToolTip; hidden on the
        // explicit open/close seams (a tab-switch away keeps it, like the saved scroll offset).
        AgentLocalTooltip _sessionsLocalTip;
        winrt::Windows::UI::Xaml::Controls::Grid _sessionsHeaderRow{ nullptr }; // LEFT: sortable column header
        winrt::Windows::UI::Xaml::Controls::StackPanel _sessionsRowsHost{ nullptr }; // LEFT: table data rows
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _sessionsRowsScroll{ nullptr }; // LEFT: the table's ScrollViewer — kept so a tab-switch away can snapshot/restore its scroll offset (keep the page "as I left it")
        double _sessionsSavedScrollOffset{ 0 }; // table scroll offset captured on tab-switch dismiss; re-applied (deferred) when the Manager tab is re-selected
        winrt::Windows::UI::Xaml::Controls::StackPanel _sessionsDetailHost{ nullptr }; // RIGHT: detail/preview
        winrt::Windows::UI::Xaml::Controls::TextBox _sessionsSearchBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _sessionsCountText{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeUserBtn{ nullptr }; // 👤 search user messages
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeAgentBtn{ nullptr }; // 🤖 search agent + tools
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeDirsBtn{ nullptr }; // 📁 dirs accessed
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeFilesBtn{ nullptr }; // 📄 files accessed
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessScopeTitleBtn{ nullptr }; // 🏷 match session title (incl. an open session's live tab title); default ON
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessFuzzyBtn{ nullptr }; // (F) fuzzy
        winrt::Windows::UI::Xaml::Controls::CheckBox _sessOpenOnlyBtn{ nullptr }; // "Open" — filter the list to sessions live in any Agentmaster window (registry live)
        winrt::Windows::UI::Xaml::Controls::CheckBox _sessHiddenBtn{ nullptr }; // "Hidden" — REVEAL sessions in AppSettings.hiddenSessionIds (manually-hidden + auto-hidden on delete); default OFF (they are filtered out)
        winrt::Windows::UI::Xaml::Controls::CheckBox _sessFavOnlyBtn{ nullptr }; // "Favorite" (FAVORITES.md) — filter the list to favorited sessions (the star column / SessionStore "favorite" key); default OFF
        winrt::Windows::UI::Xaml::Controls::Button _sessWindowBtn{ nullptr }; // [1 month] — click cycles presets, hover opens the range popup
        winrt::Windows::UI::Xaml::Controls::Button _sessRefreshBtn{ nullptr }; // ↻ — re-enumerate the window + load-or-refresh each sidecar index (pick up new/updated sessions)
        winrt::Windows::UI::Xaml::Controls::Button _sessFilterChip{ nullptr }; // "✕ filter: …" — shown only while a row right-click "Filter" facet is active; click clears ALL facets
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _sessRangePopup{ nullptr }; // hover: From/To range picker (answer Q4 — text boxes)
        winrt::Windows::UI::Xaml::DispatcherTimer _sessRangeCloseTimer{ nullptr }; // hover-intent: button-exit schedules a close; entering the popup cancels it (bridges the button->popup gap)
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
        std::unordered_set<std::wstring> _sessionsFavorites; // FAVORITES.md: the favorited session ids (SessionStore "favorite" key), loaded off-thread in _RefreshSessionsRows; drives the ★ column + the "Favorite" filter
        // Bookmark tags on the Sessions page: sid -> its tag list (SessionStore "tags" key), loaded
        // off-thread in the gather beside the favorites; drives the header TAG CHIPS row + the tag
        // facet of the row filter. Live-synced by _ToggleSessionTag (the tab-menu Tag panel) so an
        // open page reflects a toggle without a re-gather.
        std::unordered_map<std::wstring, std::vector<std::wstring>> _sessionsTags;
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _sessTagChipsScroll{ nullptr }; // the TAG CHIPS row host (header row 2, table-width) — collapsed while no tags exist; horizontal-scrolls past ~a screenful of chips
        winrt::Windows::UI::Xaml::Controls::StackPanel _sessTagChipsPanel{ nullptr }; // the chips themselves — one blue, partially-transparent toggle chip per GLOBAL tag, rebuilt by _RebuildSessionsTagChips
        std::wstring _sessionsSelectedId;
        int _sessionsSortColumn{ 7 }; // default: Active (last activity), newest first — col 7 after the leftmost ★ column (FAVORITES.md) and the Tags column (after Branch) each shifted the rest +1
        bool _sessionsSortAscending{ false };
        std::wstring _sessionsQueryText; // the raw search text (folding happens in the engine)
        std::atomic<bool> _sessionsPageVisible{ false };
        std::atomic<uint64_t> _sessionsSearchGen{ 0 }; // bumps per query — a stale slow search self-cancels
        std::atomic<bool> _sessionsIndexing{ false }; // a background gather/index pass is running
        std::shared_ptr<ThrottledFunc<>> _sessionsSearchThrottled{ nullptr }; // keystroke debounce
        // Detail SUMMARY cache — keyed by id, validated by transcript mtime. The whole-file analyze
        // + box render happen off-thread once per (id, mtime); re-shows AND background PREFETCHES of
        // adjacent rows hit the cache for an instant, spinner-free render. A MULTI-entry map (was a
        // single entry) so Up/Down/click can warm neighbors ahead of navigation. The text carries
        // kSummarySepMark sentinel lines (rendered as full-width rules). UI-thread access only.
        struct _SessionsSummaryEntry
        {
            int64_t mtime{ 0 };
            std::wstring text;
        };
        std::unordered_map<std::wstring, _SessionsSummaryEntry> _sessionsSummaryCache;
        std::unordered_set<std::wstring> _sessionsSummaryLoading; // ids with an analyze in flight — dedupes a foreground select racing its own prefetch
        // Agentmaster: when set, a launched/restored/forked Claude tab is created WITHOUT focus (a
        // BACKGROUND tab) and the Sessions page is kept open — the Sessions-page right-click "bulk
        // open" path. _InitializeTab skips the SelectedItem switch; _ResumeSessionFromDisk /
        // _ForkSessionFromDisk skip _HideSessionsPage. Set around ONE open, reset right after.
        bool _openClaudeTabInBackground{ false };
        // Agentmaster (native-exe-only policy): true while the page-level "Claude not found" dialog
        // (_PromptClaudeMissing) is up, so a burst of not-found gates — e.g. a bulk Restore looping over
        // checked Claude sessions — collapses to a SINGLE dialog instead of stacking one per session.
        bool _claudeMissingPromptShowing{ false };
        // The visible row ids in TABLE (sorted+filtered) order — the Up/Down keyboard
        // navigation list (the archive page's _archiveVisibleOrder pattern). Rebuilt each render.
        std::vector<std::wstring> _sessionsVisibleOrder;

        // Agentmaster: IN-PLACE TITLE EDITING in the Sessions browser (the Manager's Explorer-tree
        // rename idiom, here over the Title cell). Two entry points — the row right-click "Edit
        // Title" and a "slow double-click" (re-clicking the already-selected row, the Windows-
        // Explorer rename gesture) — both call _BeginSessionsRename, which swaps the Title cell for a
        // focused TextBox (a ContentDialog is ruled out: a text box inside one gets no keypresses
        // under XAML Islands). Commit writes the durable SessionStore title and, for a session known
        // to the registry, routes through _RenameClaudeSession so the live tab + Explorer/board lens
        // track it (Rule #11).
        std::wstring _sessRenamingId; // the row whose Title cell is currently an editable TextBox ("" = none)
        winrt::Windows::UI::Xaml::Controls::TextBox _sessRenameBox{ nullptr }; // the live editor; while it exists a re-render is suppressed so a background tick can't tear it out
        std::wstring _sessRenamePendingId; // the row armed for slow-double-click rename (consumed by the next selected-row click or disarmed by a double-tap)
        winrt::Windows::UI::Xaml::DispatcherTimer _sessRenameArmTimer{ nullptr }; // disambiguates the slow second click from a fast double-click (interval = GetDoubleClickTime); a row double-tap disarms it (a double-click resumes, never renames)

        // Agentmaster: a ROW-LEVEL filter set from a session row's right-click "Filter \xBB" submenu —
        // narrows the visible set to rows matching the clicked ("anchor") row in one or more
        // dimensions. It COMPOSES with the search text + the scope/Open/Hidden toggles as AND (it is
        // applied at the same _RenderSessionsTable chokepoint as those). Multiple facets stack, one
        // per dimension: directory, branch, a created-time bucket (day/week/month — mutually
        // exclusive granularities of the one time axis), and the fork family. Picking a facet a row
        // already matches toggles that facet OFF (so the same menu item is a clean toggle). Purely a
        // transient browse-list view-state — nothing persisted, nothing touched on disk.
        enum class _SessionsRowFilterKind
        {
            SameDirectory,
            SameBranch,
            SameDay, // created within the same local calendar day as the anchor row
            SameWeek, // created within the same local week (Monday-start)
            SameMonth, // created within the same local calendar month
            ForkFamily, // the anchor session, its fork parent, and everything sharing that lineage (within the listed window)
        };
        struct _SessionsRowFilterState
        {
            enum class TimeGran
            {
                None,
                Day,
                Week,
                Month
            };
            bool hasDir{ false };
            std::wstring dir; // matched filesystem-aware (NormDirKey — Rule #8)
            bool hasBranch{ false };
            std::wstring branch; // exact match (git refs are case-sensitive)
            TimeGran timeGran{ TimeGran::None };
            int64_t timeStartMs{ 0 }; // [start, end) created-time window (local bucket), DST-safe
            int64_t timeEndMs{ 0 };
            std::wstring timeLabel; // "day 2026-06-20" / "week of 2026-06-15" / "month 2026-06"
            bool hasFamily{ false };
            std::unordered_set<std::wstring> familyIds; // ForkFamily — the connected fork-graph component
            // Bookmark tags (the header chips row): the SELECTED tag names, in click order. Matched
            // case-insensitively (FoldTagName); multiple selected tags NARROW (a row must carry ALL
            // of them — facets AND, like every other dimension). Rides this struct so the ✕ chip /
            // "Clear filters" / the "· filtered" count line all cover it for free.
            std::vector<std::wstring> tags;
            bool Any() const { return hasDir || hasBranch || timeGran != TimeGran::None || hasFamily || !tags.empty(); }
        };
        _SessionsRowFilterState _sessionsRowFilter;

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
            std::function<void()> onRestore; // optional: re-apply transient view state (scroll/focus) a collapse drops, when the Manager tab is re-selected; may be empty
            bool restoreOnReturn{ false }; // "logically open" intent — set true by the page's Show, false by its Hide (NOT by dismiss/restore); _RestoreAgentPageOverlays re-shows it on return to the Manager tab. Order-independent, so a Resume's synchronous tab-switch dismiss can't out-race the page's own Hide.
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

        // Agentmaster (Shift+Click background-activate): a Shift+Click on a managed session tab must
        // ACTIVATE the session in place WITHOUT switching to it. A MUX TabViewItem drives its selection
        // on pointer-RELEASE (after our press handler), so the switch can't be cancelled at press time;
        // _OnTabPointerPressed instead ARMS a one-shot veto here and _OnTabSelectionChanged snaps the
        // selection back to _tabSelectRevertTo before any content-swap side effects run.
        // _revertingTabSelection guards the revert's own re-entrant SelectionChanged.
        bool _suppressTabSelectForActivate{ false };
        bool _revertingTabSelection{ false };
        winrt::Windows::Foundation::IInspectable _tabSelectRevertTo{ nullptr };

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
        void _SpawnClaudeSession(winrt::hstring workingDir, winrt::hstring title, uint32_t insertPosition = -1, winrt::hstring model = {}); // Agentmaster (insertPosition: -1 == end/NewTabPosition default; a tab-context-menu spawn passes clickedIndex+1 so the new tab lands next to the clicked tab. model: the launch-model picker's per-LAUNCH `--model <id>` pick from an "Open New Session Here" submenu; "" = Default, the settings model)
        TerminalApp::Tab _LaunchClaudeSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromId = {}, uint32_t insertPosition = -1, const std::wstring& modelOverride = {}); // Agentmaster (returns the created tab; forkFromId set => fork that conversation into a new id; insertPosition threads tab placement, default -1 == end; modelOverride: launch-model picker — non-empty adds ` --model <id>` to THIS launch's commandline)
        winrt::fire_and_forget _RestoreClaudeSessions(); // Agentmaster: load persisted sessions as ARCHIVED (restorable) — does NOT auto-launch (Rule #6)
        void _RestoreWindowTabs(); // Agentmaster (M10 window-grouped restore): re-home THIS window's persisted tabs — resume each Claude session + replay each Other (shell) tab from its WindowRecord, in order. Only a claimed record (a reopened window) restores.
        void _AttachClaudeOverlay(const TerminalApp::Tab& tab, const std::wstring& sessionId); // Agentmaster: build + install the per-tab link badge (gated on AppSettings.showTabOverlay)
        int _JumpToPromptInSession(const std::wstring& sessionId, const std::vector<std::wstring>& msgs, int index); // Agentmaster (SUMMARY_JUMP.md): center the session tab's view on the i-th prompt; returns the row or -1
        std::vector<int> _JumpEligibilityInSession(const std::wstring& sessionId, const std::vector<std::wstring>& msgs); // Agentmaster (SUMMARY_JUMP.md): a row per prompt (-1 == not on screen) for icon dimming
        winrt::Microsoft::Terminal::Control::TermControl _ControlForSession(const std::wstring& sessionId); // Agentmaster (SUMMARY_JUMP.md): the live control hosting a session's tab, or null
        std::wstring _FocusedPromptNavSession(); // Agentmaster (alt+up/down): the focused tab's managed CLAUDE sessionId, or empty (=> the handler falls back to MoveFocus)
        winrt::fire_and_forget _ScrollAdjacentPrompt(std::wstring sessionId, bool up); // Agentmaster (alt+up/down): re-read the sent prompts (mtime-gated), then center the view on the nearest OFF-SCREEN sent prompt up/down (fresh resolve every press); boundary sound at the ends
        winrt::fire_and_forget _RefreshPromptNavCache(std::wstring sessionId); // Agentmaster (alt+up/down, SUMMARY_JUMP.md §7): the 30s focused refresh — re-read sent prompts (mtime-gated) into _promptNavCache + re-resolve the overlay's jump eligibility, WITHOUT navigating
        void _NavigateAdjacentPrompt(const std::wstring& sessionId, const std::vector<std::wstring>& prompts, bool up); // Agentmaster (alt+up/down): the UI-thread half of _ScrollAdjacentPrompt (resolve control + scroll, else sound)
        void _PlayPromptNavLimitSound(); // Agentmaster (alt+up/down): boundary feedback when there is no further off-screen prompt
        void _ToggleSummaryPanel(); // Agentmaster (TAB_OVERLAY.md): pencil button -> flip the GLOBAL AppSettings.showSummaryPanel (RMW settings.json) + apply live to every linked overlay in this window
        void _ToggleSummaryWrap(); // Agentmaster (TAB_OVERLAY.md): wrap-line toggle (panel times bar) -> flip the GLOBAL AppSettings.summaryPanelWrapNewlines (RMW settings.json) + apply live to every linked overlay in this window
        void _ToggleSummaryTruncate(); // Agentmaster (TAB_OVERLAY.md): truncate toggle (panel times bar) -> flip the GLOBAL AppSettings.summaryPanelTruncate (RMW settings.json) + apply live to every linked overlay in this window
        void _ToggleSummaryPrevious(); // Agentmaster (conversation lineage): previous-session toggle (panel times bar) -> flip the GLOBAL AppSettings.summaryPanelShowPrevious (RMW settings.json) + apply live to every linked overlay in this window
        void _SetTabActivityBadge(const TerminalApp::Tab& tab, const std::wstring& wtSession, const std::wstring& kind); // Agentmaster (OBSERVER.md §4/§11d): attach-or-update a registry-less "○ <kind> · unlinked" badge (pwsh / cmd / claude / codex) on a non-bound tab
        void _DropPendingOverlay(const std::wstring& wtSession); // Agentmaster: collapse + release this window's observe badge for a tab (bound / claude exited / tab gone)
        void _SetTabAgentDot(const TerminalApp::Tab& tab, const std::optional<winrt::Windows::UI::Color>& color, bool dormant = false); // Agentmaster (tab status dot): show/recolor (nullopt = hide) the tab-strip "[icon] ● <title>" dot via Tab.TabStatus(); dormant=true => the half-hollow "not started" variant; idempotent on unchanged color+presentation
        void _SetTabPending(const TerminalApp::Tab& tab, bool on, const std::optional<winrt::Windows::UI::Color>& dotsColor = std::nullopt); // Agentmaster (PENDING_INPUT.md): show/hide the unsent-draft "3 dots" pulse below a tab's status dot via Tab.TabStatus().AgentPendingVisible, painting them dotsColor (the contrast-picked pending color) when shown; idempotent (no-ops when visibility + color are unchanged); UI thread
        winrt::Windows::UI::Color _PendingDotsColorForTab(const TerminalApp::Tab& tab, const ::Agentmaster::SessionInfo& info); // Agentmaster (PENDING_INPUT.md): contrast-pick the "3 dots" color from the tab's CURRENT effective header background (selected/unselected aware) over the session's MODE-AWARE tab color (ResolveSessionColorHex — per-dir / individual / inferred); UI thread
        void _RefreshPendingDotsContrast(); // Agentmaster (PENDING_INPUT.md): re-pick the "3 dots" color for tabs currently showing a draft (no buffer read) so they re-contrast on a selected<->unselected shift; called from _OnTabSelectionChanged; UI thread
        void _UpdateTabAgentDot(const std::wstring& sessionId, ::Agentmaster::SessionState state, bool live, bool dormant); // Agentmaster (tab status dot): the registry-observer reaction — recolor (or hide, !live) the hosting tab's dot; dormant => the half-hollow "not started" variant; UI thread; no-op when this window doesn't host the session
        void _UpdateTabAgentToolTip(const TerminalApp::Tab& tab, const std::wstring& sessionId, bool swapWhileOpen = false, bool kickSummary = true); // Agentmaster (tab tooltip): build + host the rich session hover tooltip — a dark, summary-style card (● title · folder/branch header, state·age·why line, kind·model·perm line, then the session-end.js Summary box with NUMBERED messages + files, Cascadia Mono) — on a managed session's tab; clears it when the session is gone/archived; swapWhileOpen = the one-shot open-swap grant for the async summary-body arrival (Tab::SetAgentToolTip); kickSummary=false = host the header card WITHOUT kicking the off-thread transcript analyze (the arm/bind pre-hosts — the analyze belongs to a real hover, else a 60-tab restore would burst 60 whole-transcript parses); UI thread. LAZY: called on hover (_ArmTabAgentToolTipHover), bind, tag toggle, and the !live clear — never per tick.
        void _ArmTabAgentToolTipHover(const TerminalApp::Tab& tab); // Agentmaster (LAZY tab tooltip — the CPU fix): wire (once, idempotent + cheap when already armed) the tab's PointerEntered -> build-now hook so the rich card is built only when a human is about to see it, replacing the old every-sweep rebuild of EVERY tab's card; the first arm also builds once so ToolTipService has a hosted tip before the first hover; resolves the tab's CURRENT session at hover time (re-home safe); UI thread
        // Agentmaster (tab tooltip): the summary-box BODY (RenderSessionSummaryBox text, \x1F-separated)
        // backing the rich hover tooltip, cached per session + rebuilt off-thread by _EnsureTabTooltipSummary
        // only when the transcript grows (mtime) — the AgentTabOverlay summary-panel caching pattern, so a
        // quiet tab costs one stat. _tabTooltipSig skips re-hosting an unchanged card; the in-flight set
        // collapses overlapping background loads for one session. (transient — never persisted.)
        struct _AgentTooltipSummary
        {
            std::wstring path;        // resolved transcript / Codex rollout path (cached so we don't re-glob)
            int64_t mtime{ 0 };       // transcript mtime when `body` was rendered (0 = not loaded yet)
            int64_t lastCheckMs{ 0 }; // last time we kicked a background freshness check (throttle)
            winrt::hstring body;      // RenderSessionSummaryBox(full=false) text, \x1F section sentinels
        };
        std::unordered_map<std::wstring, _AgentTooltipSummary> _tabTooltipSummary;
        std::unordered_set<std::wstring> _tabTooltipSummaryInFlight;
        std::unordered_map<std::wstring, std::wstring> _tabTooltipSig;
        winrt::fire_and_forget _EnsureTabTooltipSummary(winrt::TerminalApp::Tab tab, winrt::hstring sessionId, bool codex, winrt::hstring codexId, winrt::hstring cwd); // Agentmaster (tab tooltip): off-thread resolve+stat+analyze the transcript; on mtime growth render the Summary box + re-host the card; mtime-cached + in-flight-guarded
        // Agentmaster (tab status-dot RED FLASH): a hosted session that goes from Running to a resting
        // state (Idle / WaitingForInput / NeedsApproval — NOT Done or Error) on an UNVISITED tab blinks a
        // RED RING around that tab's status dot (a separate ellipse behind the dot, peeking out around
        // its constant black stroke; synchronized across tabs via one shared timer) until you switch to
        // it. _EvaluateAgentFlash detects the Running -> {Idle/Waiting/NeedsApproval} edge per registry
        // update; _Set/_Stop/_Start/_Ensure drive the shared 600ms timer + per-tab ring. UI thread only.
        void _EvaluateAgentFlash(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState newState, bool live); // detect the Running -> {Idle/WaitingForInput/NeedsApproval} edge + start/stop the flash (never the active/visited tab; not for ->Done/->Error); forgets state on !live so a later restore re-tracks fresh
        void _StartAgentFlash(const std::wstring& sessionId); // begin blinking this session's tab-dot red RING; joins the shared timer in-phase with any others
        void _StopAgentFlash(const std::wstring& sessionId); // stop flashing + hide the red ring; stops the shared timer when none remain
        void _VisitTabClearFlash(const TerminalApp::Tab& tab); // visiting (selecting) a tab marks it seen -> stop its red flash (from _OnTabSelectionChanged)
        void _RefreshFocusedTabSummary(const TerminalApp::Tab& tab); // Agentmaster (TAB_OVERLAY.md summary panel): on a tab switch, kick a cheap mtime-gated content re-read of that tab's summary panel so a focused "here-and-now lens" is current instead of up to ~5s stale (from _OnTabSelectionChanged; no-op for a non-session tab / panel off / unchanged transcript)
        void _EnsureAgentFlashTimer(); // lazily create + (re)start the shared 600ms flash timer (a fresh burst begins on the red phase)
        void _StopAgentFlashTimer(); // stop the shared flash timer (no flashing tabs remain)
        void _OnAgentFlashTick(); // shared-timer tick: toggle the phase + show/hide every flashing tab's red RING together (the synchronized blink)
        void _ApplyAgentFlashRingForSession(const std::wstring& sessionId); // show/hide one flashing session's red ring at the CURRENT shared phase (used when it joins mid-flash)
        void _SetTabFlashRing(const TerminalApp::Tab& tab, bool on); // show/hide a tab's flash RING (the ellipse behind the dot) via Tab.TabStatus().AgentFlashRingVisible, painting it with the shared _flashRingBrush when shown; the dot's own black stroke + fill stay constant
        winrt::Windows::UI::Color _FlashRingColorFromSettings() const; // Agentmaster: parse AppSettings::flashRingColor ("#AARRGGBB", alpha = opacity) -> Color, falling back to opaque red on a malformed value
        void _EnsureFlashRingBrush(); // Agentmaster: lazily build the per-window shared flash-ring brush (_flashRingBrush) from the current setting, on the first flash
        void _RefreshFlashRingBrush(); // Agentmaster: re-point the shared flash-ring brush at the (possibly changed) AppSettings::flashRingColor — mutates its Color in place so every tab sharing it updates live (cog Save / cross-window broadcast)
        void _RefreshOverlayOpacities(); // Agentmaster (TAB_OVERLAY.md): push the (possibly changed) AppSettings::tabOverlayRest/HoverOpacity to every per-tab overlay (linked + observe) so the badge/summary dim<->bright opacities update live (cog Save / cross-window broadcast)
        void _MarkSessionUnread(const std::wstring& sessionId); // Agentmaster (Mark Unread): force the red ring on this session's tab until VISITED — even if it is the focused tab (no active-tab skip); sticky vs automatic state changes; ALSO sets the engine manualUnread + promotes Idle/Done -> WaitingForInput (board state, every window)
        void _ClearSessionUnread(const std::wstring& sessionId); // Agentmaster (Mark Unread): clear a manual unread mark + hide the ring if the automatic flash isn't also active (from _VisitTabClearFlash / archive); ALSO clears the engine manualUnread
        void _MarkSessionRead(const std::wstring& sessionId); // Agentmaster (Waiting-for-you "unread" model): stamp readUnixMs=now (quiet) so a past-timeout WaitingForInput card may decay to Idle; from _VisitTabClearFlash (a visit) + _EvaluateAgentFlash (the focused tab)
        void _MoveSessionTriageState(const std::wstring& sessionId); // Agentmaster (Waiting-for-you + Error triage): the tab context-menu's status-adaptive move — demote a WaitingForInput session to Idle ("Move to Idle/Done"), DISMISS an Error one the same way (records the errorDismissed ack so the scanner's level-derived Error can't bounce it back), or plainly promote an Idle/Done one to WaitingForInput ("Move to Waiting-for-you"); direction re-derived from the live registry state. EXPLICITLY separate from _MarkSessionUnread (no sticky manualUnread, no ring flash)
        void _EvaluateAgentNotification(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState newState, bool live); // Agentmaster (System notifications): detect the Running -> X edge per registry update on a HOSTED tab and raise the Windows toast per the Notifications settings (per-state switches, focused-tab skip); tracks the Running-entry time for the "after <duration>" clause; HOLDS an Idle/Waiting toast while the external-work signal is live (spurious-completion suppression — SessionScanner's ShouldHoldCompletionToast) and DROPS the hold on a Running re-entry; forgets state on !live like _EvaluateAgentFlash; UI thread
        void _FireAgentCompletionToast(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState state, int64_t runningSinceMs, int64_t heldForMs); // Agentmaster (System notifications): the FIRE half shared by the immediate edge and the sweep's deferred fire — per-state cog switch, focused-tab skip, the kNotifyDuplicateToastMs double-toast guard, title/body build, the [notify] log (+ "held Ns" when deferred), the toast itself; UI thread
        winrt::fire_and_forget _SweepAgentPendingToasts(); // Agentmaster (System notifications): liveness-ticked sweep of _agentToastHeld — DecideHeldToast per held session (DROP re-lit / FIRE with the current state / KEEP); the _ScanPendingInput marshal + terminate-net shape
        winrt::Windows::Foundation::IAsyncAction _SweepAgentPendingToastsImpl();
        void _ShowAgentSessionToast(const std::wstring& sessionId, const std::wstring& title, const std::wstring& body, bool silent); // Agentmaster (System notifications): raise one Windows toast — line 1 = the session title, line 2 = the completion body; tagged per session (a newer toast replaces the older); clicking it while the app is alive brings the hosting window to the FRONT (restore + foreground) and jumps to the session's tab (local or via the activate fan-out). Best-effort: an unpackaged build (no AUMID) logs once and no-ops
        void _SetTabSelectionPill(const TerminalApp::Tab& tab, bool on); // Agentmaster (Linked Lenses): show/hide the "selected/active" accent pill behind a tab's header via Tab.TabStatus(); UI thread
        void _SetTabAgentFavorite(const TerminalApp::Tab& tab, bool on); // Agentmaster (FAVORITES.md §5a): show/hide the FAVORITE marker (CROWN or STAR per AppSettings::favoriteIcon) over a tab's status dot via Tab.TabStatus(); the two markers are mutually exclusive; UI thread, idempotent
        void _RefreshTabFavoriteCrown(const std::wstring& sessionId); // Agentmaster (FAVORITES.md): re-read IsSessionFavorite(sid) and (re)assert the marker on this window's hosting tab; no-op when this window doesn't host the session. Called at bind/launch + on toggle (same-window instant; cross-window catches up on next bind)
        void _RefreshAllFavoriteIcons(); // Agentmaster (FAVORITES.md §5a): re-assert the favorite marker on every hosted tab — used when the GLOBAL favoriteIcon (Crown<->Star) changes (cog Save / cross-window broadcast) so the glyph switches live
        // Agentmaster (bookmark tags): the tab-header BOOKMARK badges + the "Tags" panel (opened from
        // the WT tab context menu, a Sessions-page row menu, or a Triage-Board card / Explorer-tree row
        // menu). Tags are the durable SessionStore "tags" key (like the favorite star — same-window
        // instant, cross-window catches up on next bind); each tag's COLOR is the profile-level
        // tag-colors.json entry the panel's picker writes (name-hash fallback). The panel is an
        // islands-safe raw Popup the page owns (a Flyout-hosted TextBox gets no keypresses — the
        // documented text-input trap), parented into Root() and anchored under the invoking element.
        void _SetTabAgentTags(const TerminalApp::Tab& tab, const std::vector<std::wstring>& tags); // low-level: resolve each tag's color (user-picked tag-colors.json > name-hash) + write TabStatus.AgentTagsSpec as "name\t#AARRGGBB" lines (idempotent); UI thread
        void _RefreshTabTags(const std::wstring& sessionId); // re-read GetSessionTags(sid) and (re)assert the badges + the tooltip's tag row on this window's hosting tab; map-miss no-op. Called at bind/launch + on toggle
        void _RefreshAllTabTags(); // re-assert badges on EVERY hosted tab — the recolor fan-out (an explicit swatch pick recoloring an existing tag repaints each hosted carrier)
        void _RefreshManagerBoardTags(); // rebuild THIS window's Manager content now — a tag edit never touches the registry (no notify), so the Triage-Board cards' ribbons (re-read per _RebuildBoard) would otherwise stale until the next unrelated event
        void _OpenTagEditorForTab(const TerminalApp::Tab& tab); // context-menu "Tags" (WT tab menu): open the panel for this tab's session, anchored under its TabViewItem
        void _OpenTagEditorForElement(const std::wstring& sessionId, const winrt::Windows::UI::Xaml::FrameworkElement& anchor); // the Sessions-row / Triage-Board-card / Explorer-tree-row "Tags" items: open for ANY session id, anchored under the clicked element
        void _OpenTagEditorAt(const std::wstring& sessionId, double x, double y); // the shared open: place + show at root-relative (x, y), deferred past the invoking flyout's close (its refocus must not fight the name box); re-randomizes the color pre-pick
        void _EnsureTagEditorPopup(); // lazily build the panel ONCE (card + [name box | +] row + color-picker swatches + hint + tag list) and parent it into Root(); wires Esc / Enter / outside-press dismissal
        void _RebuildTagEditorList(); // re-list the GLOBAL tag universe (CollectGlobalTags over sessions ∪ the tags.json registry — max session activity desc, 0-carrier known tags last) with this session's on/off state per row, a right-aligned ✕ delete on 0-carrier rows, + the "K of N tags" footer
        void _CommitTagEditorAdd(); // the "+" button / Enter: normalize the typed name; an existing tag just applies to this session, a NEW name is cap-gated (AppSettings::maxTags) + takes the picker's color (an explicit pick also recolors an existing tag); re-randomizes the pick after a successful add
        void _UpdateTagEditorAddState(); // TextChanged: show the "+" only when the box holds a usable name; disable + hint when a NEW name would exceed the cap
        void _SelectTagEditorColor(const std::wstring& hex, bool userPicked); // ring the matching swatch + paint the "+" with the pick; userPicked marks an EXPLICIT choice (the only kind allowed to recolor an existing tag)
        void _RandomizeTagEditorColor(); // roll a fresh random (non-explicit) pre-pick — on every panel open and after every added tag; always moves off the current pick
        void _ToggleSessionTag(const std::wstring& sessionId, const std::wstring& tag); // add/remove one tag on a session (SessionStore) + refresh its tab badges + re-render the panel list; an untag first registers the tag (tags.json) so losing its last carrier can't vanish it
        void _RemoveGlobalTag(const std::wstring& tag); // the tag-list row's ✕ (0-carrier rows only): explicitly delete the tag from the durable registry — the ONE tag-removal path (a carried tag stays alive via the derived union; the color entry is kept)
        void _CloseTagEditorPopup(); // dismiss the panel (Esc / outside press / tab switch)
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _tagEditorPopup{ nullptr }; // the Tags panel (bookmark tags) — built once, parented into Root()
        winrt::Windows::UI::Xaml::Controls::Border _tagEditorCard{ nullptr }; // the panel card (the outside-press dismissal's inside/outside boundary)
        winrt::Windows::UI::Xaml::Controls::TextBox _tagEditorBox{ nullptr }; // row 1: the focusable "name the tag" box
        winrt::Windows::UI::Xaml::Controls::Button _tagEditorAddBtn{ nullptr }; // row 1: the "+" beside the box — appears once text is typed; wears the picked color as its background
        winrt::Windows::UI::Xaml::Controls::StackPanel _tagEditorSwatchRow{ nullptr }; // row 2: the color-picker swatches (one per kTagPalette color; the pick ringed white)
        std::wstring _tagEditorPickedHex; // the picker's current "#AARRGGBB" (random pre-pick or an explicit swatch tap)
        bool _tagEditorUserPicked{ false }; // true only after an explicit swatch tap this open — gates the existing-tag recolor
        winrt::Windows::UI::Xaml::Controls::TextBlock _tagEditorHint{ nullptr }; // the cap message ("Tag limit reached (N)"), collapsed until relevant
        winrt::Windows::UI::Xaml::Controls::StackPanel _tagEditorList{ nullptr }; // the tag rows (sorted by max session activity desc)
        winrt::Windows::UI::Xaml::Controls::TextBlock _tagEditorCount{ nullptr }; // the dim "K of N tags" footer
        std::wstring _tagEditorSessionId; // the session the open panel edits
        bool _tagEditorOutsideHooked{ false }; // the Root() outside-press dismissal handler is registered (once)
        std::vector<::Agentmaster::GlobalTagInfo> _tagEditorUniverse; // the global tag universe CACHED at open/toggle — the per-keystroke cap check must not re-scan the session-store dir
        // Agentmaster (bookmark tags): the rich TAG HOVER PANEL — hovering a tab-header bookmark
        // badge opens a popup listing EVERY session carrying that tag (title + status dot; a LIVE
        // row click jumps to its tab via _ActivateClaudeSession — cross-window). A popup rather
        // than a ToolTip because a tooltip can't take clicks (AgentSetTip's are hit-test-invisible
        // by design). Open/close ride hover-intent timers (the Sessions range-popup recipe): a
        // short open delay so panning the strip doesn't flash panels, a grace close so the pointer
        // can cross the badge->panel gap (panel-enter cancels it).
        void _OnTagBadgeHoverBegin(const winrt::hstring& tag, const winrt::Windows::UI::Xaml::UIElement& anchor); // badge enter: cancel the grace close, arm the open delay
        void _OnTagBadgeHoverEnd(); // badge exit: cancel a pending open; grace-close an open panel
        void _EnsureTagHoverPopup(); // lazily build the popup + card + the two timers, parent into Root()
        void _ShowTagHoverPanelNow(); // the open timer fired: gather the tag's sessions + render rows + place at the pending anchor
        void _CloseTagHoverPopup(); // dismiss (grace timer / a row jump / tab switch)
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _tagHoverPopup{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _tagHoverCard{ nullptr }; // the hover-keepalive boundary (enter cancels the grace close)
        winrt::Windows::UI::Xaml::Controls::StackPanel _tagHoverBody{ nullptr }; // header + session rows, rebuilt per show
        winrt::Windows::UI::Xaml::DispatcherTimer _tagHoverOpenTimer{ nullptr }; // one-shot ~160ms open delay
        winrt::Windows::UI::Xaml::DispatcherTimer _tagHoverCloseTimer{ nullptr }; // one-shot ~300ms grace close
        winrt::hstring _tagHoverPendingTag; // the tag to show when the open timer fires
        winrt::Windows::UI::Xaml::UIElement _tagHoverPendingAnchor{ nullptr }; // the hovered badge (transformed into Root() space for placement)
        void _UpdateManagerSelectionHighlight(); // Agentmaster (Linked Lenses): re-evaluate which tab (if any) wears the pill — the hovered-or-selected managed session, only while the Manager tab is the active tab; called on lens change, hover, and tab switch
        void _RevealTabInStrip(const winrt::TerminalApp::Tab& tab); // Agentmaster (Linked Lenses): scroll the tab strip so this tab is visible CLEAR of the `<`/`>` overlay scroll buttons; virtualization-aware — a derealized (scrolled-off) TabViewItem is realized via the TabListView's ScrollIntoView first, then fine-adjusted
        bool _AdjustStripToRevealItem(const Microsoft::UI::Xaml::Controls::TabViewItem& tvi); // Agentmaster: the precise reveal pass — one ChangeView on _tabStripScrollViewer landing the item inside [pad, viewport-pad]; returns false while the item has no realized layout (virtualized out / pre-arrange) so the caller realizes it and retries
        void _RevealTabRetryAdjust(Microsoft::UI::Xaml::Controls::TabViewItem tvi, int attempts); // Agentmaster: bounded Low-priority retries of _AdjustStripToRevealItem after ScrollIntoView — realization lands on a later layout pass, never this tick
        void _ActivateClaudeSession(winrt::hstring sessionId); // Agentmaster: jump to a session's tab — local first, then fan out to the hosting window (ActivateSessionInOtherWindows)
        bool _ActivateDormantSession(const std::wstring& sessionId); // Agentmaster (eager-init): start a DORMANT session's claude IN PLACE (no focus change) via TermControl::InitializeWithSize + SetStarted(true); returns true if it woke one (false: not hosted here / already started). UI thread.
        void _ActivateAllDormantTabsLocal(); // Agentmaster (eager-init): DRIP-FEED-wake every dormant managed tab hosted in THIS window — 500ms between wakes, 4 wakes per 10s batch — so a many-tab activate never bursts N claude.exe spawns onto one UI pass. Merges into a running drip. The receiving half of the activate-all fan-out.
        void _ActivateAllDripStep(); // Agentmaster (eager-init pacing): one drip step — pop queued ids until one actually wakes, then re-arm _activateAllTimer (500ms in-batch; the long post-batch gap after the 4th wake so batches start 10s apart); self-stops + logs the total when the queue drains
        void _SetActivateAllBusy(bool busy); // Agentmaster (eager-init pacing): tell THIS window's Manager content the drip is running (true at drip-start / false at drain) so it shows the "Activating N tabs…" busy state (disabled button + disabled cwd box)
        void _TrackSessionStarted(const std::wstring& sessionId); // Agentmaster (eager-init): mark SessionInfo::started true the moment this session's control initializes (already started => now; else one-shot on TermControl.Initialized) so a focused tab's dot flips full without the ~2s liveness-sweep lag
        void _ScheduleSplashDismiss(); // Agentmaster (splash): start the deferred-dismiss watcher at the end of _OnFirstLayout — the restored tabs init LAZILY after, so dismissing there uncovers a blank window
        void _TickSplashDismiss(); // Agentmaster (splash): the watcher tick — dismiss the launch splash once the foreground terminal is connected AND the UI thread has been responsive ~1.5s (or a hard timeout)
        std::wstring _wid_NoThrow() const; // Agentmaster (splash/[startup]): the " [win <id>]" tag for log lines (empty until _windowId is set)
        bool _FocusClaudeSessionTab(const std::wstring& sessionId, bool bringWindowToFront); // Agentmaster (cross-window activate): select the session's tab IN THIS WINDOW (no fan-out); optionally foreground this window's HWND (the receiving half of the activate sink). Returns false on a miss.
        void _RestartClaudeSession(winrt::hstring sessionId); // Agentmaster (Triage Board / Explorer-tree "Restart session"): restart a managed session's connection in place — local first, then fan out to the hosting window (RestartSessionInOtherWindows), mirroring _ActivateClaudeSession
        bool _RestartClaudeSessionLocal(const std::wstring& sessionId); // Agentmaster (cross-window restart): restart the session's tab IN THIS WINDOW via _restartPaneConnection (the NotConnected guard + _RestartManagedSession); the receiving half of the restart sink. Returns false when this window doesn't host the session's tab.
        void _ArchiveClaudeSession(winrt::hstring sessionId); // Agentmaster: archive (shut down + keep restorable) via the tab-close seam
        TerminalApp::Tab _RestoreArchivedSession(winrt::hstring sessionId); // Agentmaster: re-launch (claude --resume / codex resume) an archived session — kind-aware; returns the created tab (null on gate/no-op) so the resume nav-END can log the actually-launched id
        void _AdoptExternalClaude(uint32_t pid, winrt::hstring cwd, bool fork); // Agentmaster (Fleet Observer): bring an EXTERNAL claude's conversation under management (fork==true => --fork-session into a NEW transcript [safe on a live external]; else --resume the same; fresh if none)
        winrt::fire_and_forget _PromptClaudeMissing(); // Agentmaster (native-exe-only policy): the page-level "Claude Code (native) not found" notice — shown by the launch choke points (Restore/Resume/Fork/Adopt/Spawn) when EnsureClaudeAvailable() is false and the Manager tab's rich modal can't render (full-window page / tab / CLI). Idempotent via _claudeMissingPromptShowing (collapses a bulk loop to one dialog); no Re-check by design — the next attempt re-resolves.
        // Agentmaster (Codex managed-session support): launch / restore a codex.exe on a ConPTY as a
        // MANAGED tab, on the same path as Claude. Codex can't pin a session id (no --session-id), so
        // OUR minted id is the durable handle and the real rollout uuid (SessionInfo.codexSessionId,
        // filled by the Fleet Observer) is the `codex resume` target. Lifecycle + state only — no
        // injector / Autorunner (driving the Codex TUI is a later phase).
        void _SpawnCodexSession(winrt::hstring workingDir, winrt::hstring title, uint32_t insertPosition = -1); // fresh codex in a dir (insertPosition: -1 == end; a tab-context-menu spawn passes clickedIndex+1)
        TerminalApp::Tab _LaunchCodexSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromCodexUuid = {}, uint32_t insertPosition = -1); // fresh, `codex resume <uuid>`, or `codex fork <uuid>` (all rollout-gated); returns the created tab; insertPosition threads tab placement (default -1 == end)
        void _AdoptExternalCodex(uint32_t pid, winrt::hstring cwd, bool fork); // bring an EXTERNAL codex's rollout under management (fork==true => `codex fork` into a NEW rollout [safe on a live external]; else `codex resume` the same; fresh if none)
        void _ReconcileManagedCodex(const std::wstring& sessionId, const ::Agentmaster::TabActivityRow& act); // fill codexSessionId + map the C2 turn-state onto a managed Codex record (UI-lane, per probe)
        std::wstring _ClaudeSessionForTab(const TerminalApp::Tab& tab); // Agentmaster: reverse-lookup _claudeTabs (which session, if any, hosts this tab)
        void _SyncManagerSelectionToTab(const TerminalApp::Tab& tab); // Agentmaster (Linked Lenses): on a tab switch, select that tab's managed session in the Manager lens, so returning to the Manager shows the session you were just in (no-op pre-startup / Manager tab / non-session tab)
        void _BringManagerSelectionIntoView(); // Agentmaster (Linked Lenses): on switching TO the Manager tab, scroll the selected board card / tree row into view — the selection followed tab switches while the Manager was hidden, so it can be off-screen (no-op pre-startup)
        void _ApplyBroadcastSettings(const ::Agentmaster::AppSettings& settings); // Agentmaster (cross-window settings broadcast): adopt GLOBAL settings changed in another window (cog Save / sort toggle) — update _appSettings + hand the Manager content the new settings (repaints the sort toggles + re-sorts). Runs on this window's UI thread (the engine sink marshals here)
        std::wstring _ClaudeSessionForConnection(const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& conn); // Agentmaster: which managed session is BOUND to this connection (by tabToken == WT_SESSION) — archive on pane-close + re-point injector on restartConnection
        winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection _BuildAgentConnection(const std::wstring& commandline, const std::wstring& dir, const std::wstring& title, const std::vector<std::pair<std::wstring, std::wstring>>& env, bool inheritCursor = false); // Agentmaster: the shared managed-agent ConPTY builder (commandline + cwd + child env + this window's AM_SESSION stamp) — behind _LaunchClaudeSession / _LaunchCodexSession (fresh pane => inheritCursor false) AND the in-place restart (reuses the pane buffer => inheritCursor true)
        std::wstring _ManagedSessionForConnection(const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& conn); // Agentmaster: which MANAGED session (Claude OR Codex) owns this connection, matched by connection IDENTITY across _claudeTabs (agent-agnostic; works for a Codex tab with no tabToken yet)
        bool _RestartManagedSession(const TerminalApp::TerminalPaneContent& paneContent); // Agentmaster: in-place "Restart session" for a managed Claude/Codex pane — rebuild the connection from the CURRENT conversation (resume, transcript/rollout-gated; never replay the launch commandline), swap it in, re-point the injector. Returns true if handled; false => not a managed agent (fall through to the upstream restart).
        bool _ForkManagedSessionById(const std::wstring& sourceId, uint32_t insertPosition = -1, const std::wstring& modelOverride = {}); // Agentmaster (Triage Board / Explorer-tree "Fork session"): fork a managed session by id — the kind-aware fork shared with _DuplicateTab (Claude: `--resume <id> --fork-session`; Codex: `codex fork <rolloutUuid>`; both transcript/rollout-gated -> fresh). Opens the fork in THIS window (window-agnostic: reads the shared registry, no live tab needed). Returns false if the id isn't a known managed session. modelOverride: the launch-model picker's per-LAUNCH `--model <id>` pick from a "Fork session" submenu ("" = Default; Claude only — a codex fork takes no --model)
        void _DetachClaudeTabForMove(const winrt::com_ptr<Tab>& tab); // Agentmaster (cross-window move): a Claude tab is moving to ANOTHER window (tear-out / moveTab) — evict this window's per-window binding (NOT the injector/live) so teardown can't archive a session now alive elsewhere; the destination re-homes it
        void _DetachClaudePaneForMove(const winrt::com_ptr<Tab>& tab, const std::shared_ptr<Pane>& movingPane); // Agentmaster (cross-window move, pane-level): the movePane-to-window case — evict only if the LEAVING pane is the session's bound (first-terminal) pane; the tab may survive with its other panes
        void _RenameClaudeSession(winrt::hstring sessionId, winrt::hstring title); // Agentmaster: Explorer-tree rename -> registry title (persist) + retitle the session's tab
        void _SyncClaudeTitleFromTab(const TerminalApp::Tab& tab); // Agentmaster: a Claude tab rename -> mirror back into the registry title (the one title)
        void _SyncClaudeTabTitleFromRegistry(const std::wstring& sessionId); // Agentmaster (cross-window rename, Rule #11): the registry-observer reaction — re-pin THIS window's hosting tab to the CURRENT registry title (read fresh, never a stale captured value); UI thread; equality- + latch-guarded, map-miss no-op
        void _SetClaudeTabTextPinned(const winrt::com_ptr<Tab>& tabImpl, const winrt::hstring& title); // Agentmaster (Rule #11): pin a tab title from a registry-driven source WITHOUT bouncing back into the tab->registry mirror (the _pinningClaudeTabTitle latch makes the synchronous _UpdateTitle re-entry a no-op)
        bool _pinningClaudeTabTitle{ false }; // Agentmaster: true while WE programmatically pin a Claude tab's title (registry->tab); makes the synchronous _SyncClaudeTitleFromTab re-entry skip the write-back so the two sync directions can't ping-pong (the /clear re-home title-swap + [Unknown] notify flood)
        void _ApplyDirColorToTab(const TerminalApp::Tab& tab, const std::wstring& dir); // Agentmaster: paint a tab from its working dir's persisted/auto color
        void _ApplyDirColorToTabs(const std::wstring& dir, const std::optional<std::wstring>& colorHex); // Agentmaster: recolor every live tab whose session's color-KEY dir matches (mode-aware: the cwd, or the inferred dir under InferredWorkingDirectory)
        void _ApplySessionTabColor(const TerminalApp::Tab& tab, const std::wstring& sessionId, const std::wstring& dir); // Agentmaster (tab color modes): THE mode-aware paint seam every managed-tab launch/restore/bind routes through — per-dir (default), per-session (Individual: deals + persists the session's own color), or per-INFERRED-dir
        void _ReapplyManagedTabColors(); // Agentmaster (tab color modes): repaint every hosted managed tab per the CURRENT AppSettings::tabColorMode (cog Save + cross-window broadcast — the _RefreshFlashRingBrush idiom)
        void _OnClaudeTabColorChanged(const TerminalApp::Tab& tab); // Agentmaster: user changed a tab color -> persist per the ACTIVE color mode (dir map + same-key fan-out; Individual writes the session record alone, no fan-out)
        winrt::Windows::Foundation::IAsyncAction _ArchiveAndCloseClaudeTab(TerminalApp::Tab tab, std::wstring sessionId, bool skipConfirm); // Agentmaster: confirm -> archive bookkeeping -> close
        void _ArchiveWindowSessionsOnTeardown(); // Agentmaster (lifecycle gap #1): on window close/quit, archive this window's live sessions (live=false + unbind injector -> claude.exe exits) so they don't linger as phantom cards / orphaned processes
        // Agentmaster (permanent remove — record-only, transcript on disk KEPT): the trash-icon seam
        // beside Archive everywhere it appears (close dialog, Manager menus, Archive page). Archive keeps
        // the session restorable; this DROPS the Agentmaster record. _RemoveSessionRecord does the shared
        // bookkeeping (registry Remove + persist + clear this window's maps + strip the id from SAVED
        // (non-live) window records); _DeleteClaudeSession is the UI seam (closes a live tab here; refuses
        // a session still running in ANOTHER window — Rule #7; removes an archived one);
        // _StripSessionFromSavedWindows prunes dangling tab refs so a reopen can't resurrect it.
        void _DeleteClaudeSession(winrt::hstring sessionId);
        void _RemoveSessionRecord(const std::wstring& sessionId);
        void _StripSessionFromSavedWindows(const std::wstring& sessionId);
        void _PinManagerTabFirst(); // Agentmaster: keep the non-closable Manager tab pinned at index 0 after any reorder
        void _SettleTabStripLayout(); // Agentmaster: commit the strip's item->container mapping synchronously after a TabItems() mutation (MUX drag-start AV guard, layer 1 — a belt; see TerminalPage.AgentEngine.cpp)
        void _GuardTabDragUntilRegistered(const Microsoft::UI::Xaml::Controls::TabViewItem& tabViewItem); // Agentmaster: a (re)inserted tab stays undraggable until ContainerFromItem resolves it (MUX drag AV guard, layer 2 — a belt: keeps a not-yet-mapped tab ungrabbable)
        std::wstring _DescribeTabForLog(const TerminalApp::Tab& tab); // Agentmaster: `<sid8> "<title>"` (title-only for a shell tab) — the tab-strip forensic log identity; never throws
        winrt::fire_and_forget _AdoptExternalSession(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken); // Agentmaster: bind a hand-typed `claude` to its ConPTY
        winrt::Windows::Foundation::IAsyncAction _SweepClaudeLivenessImpl(); // Agentmaster (terminate-net): the body of _SweepClaudeLiveness, awaited inside its try/catch so a throw can't escape the fire_and_forget (std::terminate)
        winrt::fire_and_forget _SweepClaudeLiveness(); // Agentmaster: archive this window's claude tabs whose ConPTY has Closed (scanner-ticked)
        winrt::Windows::Foundation::IAsyncAction _ReconcileClaudeTabsImpl(); // Agentmaster (terminate-net): the body of _ReconcileClaudeTabs, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ReconcileClaudeTabs(); // Agentmaster: poll backstop — bind/attach + re-home claude tabs by stable WT_SESSION (scanner-ticked)
        winrt::Windows::Foundation::IAsyncAction _ObserverProbeImpl(); // Agentmaster (terminate-net): the body of _ObserverProbe, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ObserverProbe(); // Agentmaster: the Fleet Observer UI lane — publish this window's tab roster, then bind via the observer's correlation table (replaces _DiscoverClaudeTabsByCwd; OBSERVER.md §10)
        winrt::Windows::Foundation::IAsyncAction _ScanPendingInputImpl(); // Agentmaster (terminate-net): the body of _ScanPendingInput, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ScanPendingInput(); // Agentmaster (PENDING_INPUT.md): read each bound Claude tab's unsent input-box draft from its buffer + record it on the session (scanner-ticked)
        winrt::Windows::Foundation::IAsyncAction _ScanInferredTabColorsImpl(); // Agentmaster (terminate-net): the body of _ScanInferredTabColors, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ScanInferredTabColors(); // Agentmaster (tab color modes — InferredWorkingDirectory + the home-dir forcing): mtime-gated, throttled off-thread re-inference of each ADMITTED (SessionInfersWorkingDir) hosted Claude session's ACTUAL working dir from its tool-touched paths (the sessions-index sidecar), recoloring the tab when the inference changes (scanner-ticked; every session under the Inferred mode, only %USERPROFILE%-launched ones in the other modes)
        winrt::Windows::Foundation::IAsyncAction _RefreshObserverDataImpl(); // Agentmaster (terminate-net): the body of _RefreshObserverData, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
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
        // Agentmaster (discard Manager-only windows): a window that ends up holding nothing but the
        // pinned Manager tab must not persist as a restorable window, and unless it is the LAST
        // Agentmaster window it self-closes (every window has a Manager tab, so an empty one is noise).
        // _IsManagerOnlyWindow is the predicate; _CloseWindowIfManagerOnly is the debounced action
        // (re-validates, then ReserveManagerOnlyClose -> silent close, or keep-but-discard for the last
        // window); _ScheduleManagerOnlyCheck runs the debounced check (fed by _tabs.VectorChanged + the
        // end of startup, so a window settling into Manager-only — by a tab close, a tear-out, or an
        // empty/failed restore — is caught once it settles).
        bool _IsManagerOnlyWindow() const;
        void _CloseWindowIfManagerOnly();
        void _ScheduleManagerOnlyCheck();
        // Agentmaster (updater; Updater.h): quit the app for an in-app update — the post-confirm half
        // of RequestQuit (flush this window's record, then raise QuitRequested) WITHOUT RequestQuit's
        // "close all tabs?" confirmation. The user already confirmed in the update dialog, and the
        // embedded installer would force-close the app anyway, so a second generic close-confirm (and
        // its cancel-then-force-kill trap) is wrong here. Fired by the cog's "Update now" after the
        // installer is launched detached.
        void _QuitForUpdate();

        // Agentmaster (FAVORITES.md): the full-window Archive page + its methods were REMOVED — the
        // Sessions browser (below) is the sole history view. _RestoreArchivedSession (the resume seam)
        // stays; it's now reached only from the Sessions page's Resume.

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
        void _ShowSessionsDetail(const std::wstring& sessionId); // populate the right pane (metadata + actions + hit snippets + the full session-summary box)
        winrt::fire_and_forget _RunSessionsSearch(); // the two-phase search: fast inline, slow on a background pass (generation-cancelled)
        winrt::fire_and_forget _LoadSessionsSummary(std::wstring sessionId, std::wstring dir, int64_t mtime); // detail: off-thread whole-file analyze + RenderSessionSummaryBox(full) into _sessionsSummaryCache; on completion re-renders the detail IFF its id is the selected row (clears that row's spinner). Deduped via _sessionsSummaryLoading.
        void _PrefetchSessionsSummaries(const std::wstring& anchorId, int direction); // warm neighbors' summaries off-thread so navigation lands on a cache hit: +1 = the next 2 rows (Down look-ahead), -1 = the previous 2 (Up), 0 = the upper + lower neighbor (a click). No wrap at the ends; deduped + cheap when warm.
        void _InvalidateSessionsSummaryForToggle(); // a wrap/truncate toggle changed the rendered summary: drop _sessionsSummaryCache (it bakes the flags) + re-render the open detail
        void _CycleSessionsWindow(); // [1 month] click: 1d -> 3d -> 7d -> 14d -> 1mo -> 3mo -> wrap (clears a custom range)
        void _ApplySessionsRange(); // the hover popup's Apply: parse From/To (YYYY-MM-DD) into a custom range
        int64_t _SessionsCutoffFromMs() const; // the active window's from-cutoff (custom range or preset)
        void _ResumeSessionFromDisk(const std::wstring& sessionId, const std::wstring& dir, const std::wstring& title); // resume ANY on-disk session into a managed tab: live here -> Jump; archived -> Restore; unknown -> minimal record + the transcript-gated resume seam
        void _ForkSessionFromDisk(const std::wstring& parentId, const std::wstring& dir, const std::wstring& title, const std::wstring& modelOverride = {}); // fork ANY on-disk session into a NEW managed conversation (`--resume <parent> --fork-session --session-id <new>`) — the parent transcript is untouched, so it is safe even while the parent is LIVE; transcript-gated (no transcript -> fresh), the duplicate-tab fork's convention ("<title> (fork)"). modelOverride: the launch-model picker's per-LAUNCH `--model <id>` pick from a "Fork here" submenu ("" = Default)
        winrt::fire_and_forget _PromptResumeOrForkSession(std::wstring sessionId, std::wstring dir, std::wstring title, std::wstring forkTitle); // Sessions-page double-click on a NOT-live row: a Resume / Fork / Cancel ContentDialog (the adopt dialog's idiom) instead of resuming silently. Primary=Resume (claude --resume), Secondary=Fork (--fork-session, parent untouched), Close=Cancel; forkTitle is empty for a never-prompted row so the fork seam derives a smart name
        void _UpdateSessionsSelectionHighlight(); // recolor row highlights for _sessionsSelectedId WITHOUT a rebuild (row-tap + keyboard nav)
        void _MoveSessionsSelection(int delta); // Up/Down keyboard nav over _sessionsVisibleOrder: none selected => Down=first / Up=last; wraps (rotates) at the ends
        bool _AddSessionIdToHiddenList(const std::wstring& sessionId); // Agentmaster: append an id to AppSettings.hiddenSessionIds (freshest-disk RMW + in-memory copy); idempotent, returns true if newly added. Shared by the row right-click hide AND the auto-hide-on-delete seam. NO UI side effects — the caller refreshes.
        void _HideSessionFromList(const std::wstring& sessionId); // Sessions-page row right-click "Hide from list": _AddSessionIdToHiddenList + drop it from the table (the transcript on disk is untouched)
        void _UnhideSessionFromList(const std::wstring& sessionId); // Sessions-page row right-click "Unhide" (shown on a revealed hidden row): remove from AppSettings.hiddenSessionIds (freshest-disk RMW) + re-render so it returns to the list normally
        void _ResetHiddenSessions(); // Settings cog "Reset hidden sessions" (via SetResetHiddenSessionsHandler): clear AppSettings.hiddenSessionIds (RMW) + re-render so every hidden session reappears
        void _ToggleSessionFavorite(const std::wstring& sessionId); // FAVORITES.md: flip the durable star (SessionStore "favorite" key), update _sessionsFavorites, re-render. Shared by the ★ column click, the row right-click "Favorite"/"Unfavorite", and the session tab's context menu.
        // Agentmaster: in-place TITLE editing in the Sessions browser (the durable SessionStore title).
        void _BeginSessionsRename(const std::wstring& sessionId); // "Edit Title" / slow-double-click: select the row + swap its Title cell for an in-place editor (the Manager's _OnRenameSession idiom)
        void _CommitSessionsRename(); // commit the editor: persist the trimmed title (durable store + the live rename seam for a known session — Rule #11), update the in-memory rows/index, re-render. Idempotent (a deferred Enter-commit + the LostFocus that follows collapse to one). Blank keeps the old title.
        void _CancelSessionsRename(); // abandon the editor (Escape) — re-render without writing
        void _PersistEditedSessionTitle(const std::wstring& sessionId, const std::wstring& title); // the persistence core: SetStoredSessionTitle (pure on-disk row) or _RenameClaudeSession (registry-known: registry + tab + the Engine observer mirrors to the store), then reflect into _sessionsRows/_sessionsEntries
        void _ArmSessionsRenameTimer(const std::wstring& sessionId); // (re)start the slow-double-click timer for this row
        void _DisarmSessionsRenameTimer(); // stop the timer + clear the armed id (a fast double-tap / a different row / a key-nav move / page hide)
        // Agentmaster: the Sessions-page row right-click "Filter \xBB" submenu (composes AND with the
        // search text + the scope/Open/Hidden toggles — applied at the _RenderSessionsTable chokepoint).
        void _ApplySessionsRowFilter(int kind, const std::wstring& anchorId); // toggle the dimension's facet to the anchor row's value (or OFF if the anchor already matches it); re-renders
        void _ClearSessionsRowFilter(); // drop EVERY facet (the chip click / submenu "Clear filters") + re-render
        bool _SessionsRowPassesRowFilter(const _SessionsRow& r) const; // the AND-predicate over all active facets (true == keep) — the render chokepoint calls this
        void _RebuildSessionsTagChips(); // bookmark tags: re-list the header TAG CHIPS row from _sessionsTags (CollectGlobalTags — max row activity desc), selected state from _sessionsRowFilter.tags; prunes selected tags whose last carrier vanished; collapses the row when no tags exist
        void _ToggleSessionsTagFilter(const std::wstring& tag); // bookmark tags: flip one tag in the facet (case-insensitive), then chip + chips row + table re-render
        bool _SessionsRowFilterMatchesAnchor(int kind, const _SessionsRow& r) const; // does the facet for `kind`'s dimension exist AND equal row r's value? (drives the submenu ✓ + the apply-toggle direction)
        std::unordered_set<std::wstring> _ComputeForkFamily(const std::wstring& anchorId) const; // the connected fork-graph component containing anchorId, over the gathered rows (forkedFromId edges, undirected)
        void _UpdateSessionsFilterChip(); // refresh the "✕ filter: …" chip's label + visibility from _sessionsRowFilter
        // Agentmaster: the generic window-level page-overlay seam (_agentPageOverlays) — register
        // at page build; dismiss-all from any global site (the tab-switch handler). See the struct.
        void _RegisterAgentPageOverlay(const winrt::Windows::UI::Xaml::Controls::Grid& host, std::atomic<bool>* visibleMirror, std::function<void()> onDismiss, std::function<void()> onRestore = {});
        void _DismissAgentPageOverlays(); // synchronous collapse of EVERY registered page (safe outside in-page pointer handlers)
        void _RestoreAgentPageOverlays(); // re-show pages logically-open when the Manager tab was last left (their collapsed tree kept the search text/rows); called on RETURN to the Manager tab
        void _SetAgentPageOverlayOpenIntent(std::atomic<bool>* visibleMirror, bool open); // a page's Show/Hide flags whether _RestoreAgentPageOverlays should re-open it (found by its visibility mirror)
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
        void _ManagerPaneNavPreviewKeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e); // Agentmaster: tunneling handler on the Manager pane root — dispatch TAB-SWITCHING chords (NextTab/PrevTab/SwitchToTab) so alt+left/alt+right (and ctrl+tab) work even when a Manager box is focused (the bubbling _KeyDownHandler is eaten by the box); other keys fall through.
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        void _HookupKeyBindings(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap) noexcept;
        void _RegisterActionCallbacks();

        void _UpdateTitle(const Tab& tab);
        void _UpdateTabIcon(Tab& tab);
        void _UpdateAllTabIcons(); // Agentmaster: re-apply the GLOBAL "Show icons on tabs" (AppSettings::showTabIcon) to every tab live (cog Save + cross-window broadcast — the _updateAllTabCloseButtons twin)
        void _UpdateReservedTabTitleLines(); // Agentmaster: push the MAX title-line-count across ALL tabs to every header's line-reserver, so the tab strip's height stays consistent regardless of virtualization (a multi-line title tab scrolled out of view no longer snaps the row shorter). Called on any title change + tab add/remove.
        void _UpdateTabView();
        void _UpdateTabWidthMode();
        void _SetBackgroundImage(const winrt::Microsoft::Terminal::Settings::Model::IAppearanceConfig& newAppearance);

        void _DuplicateFocusedTab();
        void _DuplicateTab(const Tab& tab, uint32_t insertPosition = -1); // Agentmaster: insertPosition threads tab placement (default -1 == end/NewTabPosition; a "Fork session" context-menu invoke passes clickedIndex+1 so the fork lands next to the clicked tab)

        safe_void_coroutine _ExportTab(const Tab& tab, winrt::hstring filepath);

        winrt::Windows::Foundation::IAsyncAction _HandleCloseTabRequested(winrt::TerminalApp::Tab tab, bool skipConfirmClose = false);
        void _CloseTabAtIndex(uint32_t index);
        void _CloseTabsBefore(const winrt::TerminalApp::Tab& tab); // Agentmaster: context-menu "Close tabs to the left"
        void _CloseAllTabs(const bool favoriteFirst); // Agentmaster: context-menu "Close > Close all tabs" / "★ Favorite & close all tabs"
        bool _WindowHasManagedSession() const; // Agentmaster (FAVORITES.md): does this window host >=1 managed Claude/Codex session? (gates "★ Favorite & close all tabs" visibility)
        void _RemoveTab(const winrt::TerminalApp::Tab& tab);
        safe_void_coroutine _RemoveTabs(const std::vector<winrt::TerminalApp::Tab> tabs, const bool forceFavorite = false); // Agentmaster: forceFavorite pre-commits the "★ Favorite & Close All" disposition (a 2-button confirm) for the favorite-&-close-all path

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
        void _MoveTabToEdge(winrt::com_ptr<Tab> tab, bool toEnd); // Agentmaster: context-menu "Move to start/end"

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
        // Agentmaster: it is ALSO gated off when AppSettings.closeTabOnMiddleClick is false.
        bool _tabItemMiddleClickHookEnabled = false;
        // Agentmaster: set by _OnTabPointerPressed on a middle-button press over a tab, consumed
        // (one-shot) by _OnTabCloseRequested. WinUI closes a *visible*-X tab natively on middle
        // click and gives us no way there to tell it from an X-button click — so we remember that
        // the in-flight close came from the middle button and can suppress it when the user turned
        // "Close tab with middle-mouse click" off. Cleared on any non-middle press.
        bool _middleClickClosePending = false;
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

        // Agentmaster: the two tab-strip nav buttons. _UpdateManagerNavButtons recomputes both: Home
        // (jump TO the Manager when it has scrolled off the strip) and Jump Back (its inverse — return
        // to the selected session's tab while the Manager tab is active). _EnsureTabStripScrollViewer
        // binds (lazily) to the TabView's internal horizontal scroller so the Home half can watch its
        // offset. (All defined in TerminalPage.AgentEngine.cpp.)
        void _OnManagerHomeButtonClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _OnManagerJumpBackButtonClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _EnsureTabStripScrollViewer();
        void _UpdateManagerNavButtons();
        std::wstring _ManagerSelectedSessionId() const; // the Manager lens's currently-selected managed session (empty if none / an external is selected)

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
