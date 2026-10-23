// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once

#include <ThrottledFunc.h>
#include <atomic>
#include <map> // Agentmaster: the side panel's cached tag-color map (_sidePanelTagColors)
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
#include "AgentMaster/DraftCarry.h" // Agentmaster (PENDING_INPUT.md §11): PasteJournalEntry / DraftFillPaste — the fork carry's by-value pump state
#include "AgentMaster/InterruptedTurns.h" // Agentmaster (HOOKS.md "Interrupted turns"): RestartTurnChoice / ForkTurnChoice / the fork stop watch's pure decision (by-value members + parameters)
#include "AgentMaster/LaunchPresets.h" // Agentmaster (PRESETS, HARNESSES.md): LaunchArgs / CompiledClaudeLaunch — the launch seams' by-value snapshot + compile
#include "AgentMaster/RemoteLaunch.h" // Agentmaster (MESSAGING.md §8.2): RemoteLaunchRequest — what the Telegram bot's launch sink asks this window to open
#include "AgentMaster/RestoreWake.h" // Agentmaster (PERSISTENCE.md §14): RestoreWakeWindowJob — the restore wake's per-window job (by-value member)
#include "AgentMaster/SessionModels.h"
#include "AgentMaster/SessionStore.h" // Agentmaster (bookmark tags): GlobalTagInfo (by-value member — the Tag panel's cached universe)
#include "AgentMaster/TranscriptStore.h" // Agentmaster (Sessions page): SessionIndexEntry (by-value member)
#include "AgentMaster/WindowTabTools.h" // Agentmaster (SESSIONS.md §9.14): WindowToolRequest / WindowToolOutcome — the AI search's window and tab tools
#include "AgentLocalTooltip.h" // Agentmaster (LocalTooltip): the Sessions page's designated-area tooltip panel (by-value member)
#include <optional>
namespace Agentmaster
{
    class SessionRegistry;
    class HooksBridge;
    class Scheduler;
    class SessionScanner;
    class ProcessObserver;
    struct SessionSearchEvent; // Agentmaster: SessionSearchAgent.h — one event of the Sessions page's AI search (complete type included by TerminalPage.AgentSessionsAi.cpp)
    struct SessionSearchSnapshot; // Agentmaster: SessionSearchAgent.h — the AI search's conversation as a page shows it (same TU)
    struct TabDragBand; // Agentmaster: TabDragMath.h — the pointer-owned tab reorder gesture's band record (complete type included by the .cpp)
    enum class DockProbeMode : int; // Agentmaster: Engine.h — cross-window tab docking, what a drag asks a dock target (complete type included by the .cpp)
    struct DockProbeResult; // Agentmaster: Engine.h — cross-window tab docking, a dock target's answer (slot + numeric window id)
    class ProcessExitWait; // Agentmaster: ApiErrorRewind.h — handles on the claude.exe processes a resume must outlive (HOOKS.md "API-error turns"; complete type included by TerminalPage.AgentSessions.cpp)
}

namespace winrt::TerminalApp::implementation
{
    struct TerminalSettingsCache;
    class AgentManagerContent; // Agentmaster: the Agent Manager PAGE's content (an IPaneContent, hosted directly in _tabContent — never in a tab); complete only in the TUs that include AgentManagerContent.h, which are the only ones that dereference _managerPage
    class AgentTabOverlay; // Agentmaster: the per-tab "link badge" overlay (TAB_OVERLAY.md)
    class AgentTranscriptOverlay; // Agentmaster: the window-level transcript reading view ("Show Transcript"); complete in the TUs that include AgentTranscriptOverlay.h (the ctor / dtor TUs + TerminalPage.AgentTranscript.cpp)
    class AgentLaunchBar; // Agentmaster: the shared LAUNCH BAR (the Agent Manager's launch row) the side panel's "+" launcher card hosts; complete in the TUs that include AgentLaunchBar.h (TerminalPage.AgentSidePanel.cpp builds it, TerminalPage.AgentEngine.cpp's destructor shuts it down)
    class AgentStatusGlyph; // Agentmaster (SIDE_PANEL.md §2g): the tab strip's status icon built in code — every side-panel card's dot with the tab's 3 dots, flash ring and spark crown; complete only in TerminalPage.AgentSidePanel.cpp (the one TU that builds and drives it), held by AgentSidePanelCardView through a shared_ptr, which a TU may destroy without the complete type

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

    // Agentmaster (SIDE_PANEL.md §2c — the card list, updated IN PLACE): every value ONE side-panel card
    // shows, computed from its live session on each list update (SidePanelCardModelFor in
    // TerminalPage.AgentSidePanel.cpp). TerminalPage::_SidePanelApplyCardModel compares a new model with
    // the one the card shows now and writes only the elements whose value changed — so a registry change
    // that moved one session's state costs that card's dot, label and border, not a rebuild of the list.
    // Plain values only (strings, flags, packed 0xAARRGGBB colors): comparable with ==, no XAML.
    struct AgentSidePanelCardModel
    {
        std::wstring title; // the band's text: the session title, else its id
        std::wstring bandHex; // the band color (ResolveSessionColorHex; "" = the neutral #3A3A3A band — Remove colors)
        bool darkInk{ false }; // the band is light enough for dark title ink (also the "+N" tag count's ink)
        ::Agentmaster::SessionState state{ ::Agentmaster::SessionState::Idle }; // the dot, the label, the border color
        bool codex{ false }; // the teal `codex` pill (a managed Codex session)
        // The STATUS ICON's three indicators — the tab header's, on the card's dot (SIDE_PANEL.md §2g, AgentStatusGlyph):
        bool pending{ false }; // an unsent draft: the 3 dots in the band under the dot
        bool pendingAnimated{ false }; // the dots pulse (a started/external session, Windows animations on) rather than stand still
        uint32_t pendingArgb{ 0 }; // the dots' color, 0xAARRGGBB (PendingDotsColorFor over the card body: the light half of the configured pair; 0 without a draft)
        bool flashRing{ false }; // the session is UNREAD — its tab's red ring flashes (the process-wide unread census, CurrentUnreadSessionIds): the same ring blinks around the card's dot
        bool cacheWarm{ false }; // the spark crown: ServerCacheStillWarm holds (at rest, last real API turn younger than the cache's lifetime)
        uint32_t cacheWarmArgb{ 0 }; // the crown's fire color, 0xAARRGGBB (CacheWarmSparkColorFor over the card body; 0 when cold)
        bool animationsEnabled{ true }; // Windows' "Animation effects" at this update: a change re-decides the icon's motions (the crown's has no other model field that would move)
        std::wstring timing; // the "created 4d1h, active 2m4s ago" line (FormatSessionTiming; "" = no timing text yet)
        std::wstring dirLine; // "<leaf>[ / <branch>]"
        std::vector<std::wstring> tags; // the bookmark tags, in the order they were added
        std::vector<uint32_t> tagArgb; // each tag's resolved display color (ResolveTagDisplayColor), 0xAARRGGBB, parallel to `tags`
        std::wstring tipTitle; // the card tooltip's title (the raw session title)
        std::wstring tipText; // the card tooltip's text (full dir, branch, every tag, the gestures)
    };

    // Agentmaster (SIDE_PANEL.md §2c): ONE side-panel card — the elements an in-place update writes to, and
    // the model they show right now. Created by TerminalPage::_SidePanelCreateCardView when its row is
    // REALIZED (SIDE_PANEL.md §2f: only the rows near the viewport have cards) and kept until the row leaves
    // the realized range or the list; an update never rebuilds a card, it adds or drops only the optional
    // parts (the codex pill, the timing text, the tag strip) whose presence changed. The status icon's
    // indicators (the 3 dots, the flash ring, the spark crown — SIDE_PANEL.md §2g) are never added or
    // dropped: the glyph holds them all and an update only shows or hides them.
    // ⚠ The ROUNDED shapes are CHILDLESS (UI_THREAD.md §10h / §14): XAML gives an element with a non-zero
    // CornerRadius AND children its own composition node, and every frame of the window walks every node.
    // So the card's rounded background + border (`cardBack`) and the band's rounded color (`band`) are
    // childless Borders drawn BEHIND the content, which sits beside them in a plain Grid cell. Never give
    // either of them a child, and never put a CornerRadius on `card` or `bandArea`.
    struct AgentSidePanelCardView
    {
        winrt::Windows::UI::Xaml::Controls::Grid card{ nullptr }; // the card's root (the list's child; every handler + both tooltips live on it): [cardBack][body] in one cell, no corner radius
        winrt::Windows::UI::Xaml::Controls::Border cardBack{ nullptr }; // CHILDLESS: the card's rounded #333 background + its constant 1px border (selection / hover recolor it)
        winrt::Windows::UI::Xaml::Controls::Grid bandArea{ nullptr }; // the title row: [band][title | bandGrid] in one cell, no corner radius
        winrt::Windows::UI::Xaml::Controls::Border band{ nullptr }; // CHILDLESS: the colored title band (rounded top corners), drawn behind the title
        winrt::Windows::UI::Xaml::Controls::TextBlock title{ nullptr }; // the title (bandArea's second child while untagged — the band padding as its margin — or bandGrid's column 0 while tagged)
        winrt::Windows::UI::Xaml::Controls::Grid bandGrid{ nullptr }; // [ title * ][ tag strip Auto ] while the session is tagged (bandArea's second child, the band padding as its margin); null untagged
        winrt::Windows::UI::Xaml::Controls::StackPanel tagStrip{ nullptr }; // the ribbons in the band's top-right corner; null untagged
        // The state line: a 2-column Grid, [ stateRow Auto ][ timing * ]. The timing sits in the STAR column, not in the
        // row's StackPanel, because a horizontal StackPanel offers its children unlimited width: there the "created 4d1h,
        // active 2m4s ago" line (twice the length of the old "-7h5m/7h6m/2m4s") was cut off mid-letter at the card's edge on
        // a narrow panel. In the star column it gets the width that is left and ends in "…" instead.
        winrt::Windows::UI::Xaml::Controls::Grid stateLine{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel stateRow{ nullptr }; // [status icon][label][codex pill?] — the state line's Auto column
        // The STATUS ICON (SIDE_PANEL.md §2g): the tab header's 18 x 22 icon — the 10 DIP dot with its black stroke, the
        // unsent-draft 3 dots in the band under it, the unread flash ring behind it, the cache-warm glow + embers around
        // it — with the tab's composition motions. Its root is the state row's first child. Created with the card,
        // shut down (Shutdown) BEFORE the card leaves the list (an update's Remove, a reset), so its motions stop and
        // the flash ring gives back its hold on the UI thread's shared blink clock. Never null on a built card.
        std::shared_ptr<AgentStatusGlyph> glyph;
        winrt::Windows::UI::Xaml::Controls::TextBlock stateText{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock codexPill{ nullptr }; // null unless a Codex session
        winrt::Windows::UI::Xaml::Controls::TextBlock timing{ nullptr }; // the state line's star column; null until the session has a creation time
        winrt::Windows::UI::Xaml::Controls::TextBlock dirLine{ nullptr };
        AgentSidePanelCardModel model; // what the elements show now — the left side of every comparison
        bool hovered{ false }; // the pointer is over the card: kept so a state change repaints the hover ring in the new color
        // The card's RICH SESSION TOOLTIP host (SIDE_PANEL.md §2d) — the SAME lifecycle a tab header runs
        // (AgentSessionToolTipHost: one framework-managed ToolTip on `card`, closed-only Content swaps, the
        // Loaded re-attach, the lazy hover build, the wheel scroll), placed beside the side panel. Created the
        // first time the panel's summary area is hidden ("Sum" off) while this card exists; null before that.
        // Cleared (detached) when the summary comes back, and shut down before the card leaves the list — a
        // ToolTip still attached to a dropped element is the islands leak class AgentTipHelpers.h describes.
        std::shared_ptr<AgentSessionToolTipHost> tipHost;
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
        uint32_t TaskbarUnreadCount() const noexcept; // Agentmaster (taskbar unread badge, NOTIFICATIONS.md section 8): the PROCESS-WIDE unread-session count to paint on this window's taskbar button, already gated by AppSettings::taskbarUnreadBadge (0 = no badge); re-read by the AppHost on every TaskbarUnreadCountChanged
        uint32_t TaskbarUnreadFrameArgb() const noexcept; // Agentmaster (taskbar preview frame, NOTIFICATIONS.md section 9): THIS window's preview-frame color (0xAARRGGBB, the flash-ring color with its alpha raised to at least 50%) while any of its tabs is unread and AppSettings::taskbarUnreadThumbnailFrame is on; 0 = no frame. Re-read by the AppHost on every TaskbarUnreadFrameChanged

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
        til::typed_event<IInspectable, IInspectable> TaskbarUnreadCountChanged; // Agentmaster (taskbar unread badge): raised only when TaskbarUnreadCount() changes
        til::typed_event<IInspectable, IInspectable> TaskbarUnreadFrameChanged; // Agentmaster (taskbar preview frame): raised only when TaskbarUnreadFrameArgb() changes
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

        // Agentmaster: the tab-strip nav button in the TabStripHeader (immediately left of the `<` scroll
        // arrow), driven by _UpdateManagerNavButtons. _managerHomeButton IS the Agent Manager's place in
        // the strip now that the Manager is a page, not a tab: always shown (once the page exists), the
        // ONLY way to open the page, and painted as the active tab while the page is on screen. It has two
        // FACES (_ApplyManagerHomeButtonFace): Home off the page, and — on the page, while a session this
        // window hosts is selected in the Manager — the jump-back arrow, whose click returns to that
        // session's tab (until 2026-10-12 that was a separate Jump Back button beside Home).
        // Plus the TabView's internal horizontal ScrollViewer (found lazily once the strip is templated)
        // that the strip reveal and the drag gesture's auto-scroll drive.
        Windows::UI::Xaml::Controls::Button _managerHomeButton{ nullptr };
        // The tooltip text _ApplyManagerHomeButtonFace last applied to the Home button — its change gate.
        // The text encodes the whole face (Home off the page / Home on the page / the jump-back arrow plus
        // the name of the session it returns to), so equal text == nothing to rewrite. Empty == never
        // applied (the first update always writes), and a failed apply leaves it untouched so the next
        // update retries.
        std::wstring _managerHomeButtonTip;
        // GetTickCount64() of the last Home-button click that ACTED (it opened the page, or it jumped back
        // off it); 0 == none yet. _OnManagerHomeButtonClick ignores a click within the system double-click
        // time of it: a double-click on Home would otherwise open the page and jump straight back (the
        // second click lands on the jump-back face), and a double-click on the arrow would jump back and
        // reopen the page.
        uint64_t _managerHomeButtonFlipTick{ 0 };
        Windows::UI::Xaml::Controls::ScrollViewer _tabStripScrollViewer{ nullptr };
        // Agentmaster: the POINTER-OWNED tab reorder/tear-out gesture's state (native MUX drag is
        // permanently OFF — the drag-AV resolution; TerminalPage.AgentEngine.cpp). One gesture at a
        // time: armed on a header press, active once the threshold is crossed (pointer captured on
        // the TabView), reset on release/cancel/capture-loss.
        struct TabReorderGestureState
        {
            bool armed{ false }; // pressed on a draggable tab header; below the drag threshold
            bool active{ false }; // threshold crossed — the gesture owns the pointer
            uint32_t pointerId{ 0 }; // the one pointer this gesture tracks
            winrt::Windows::Foundation::Point pressPoint{}; // TabView coords at press
            winrt::Windows::Foundation::Point lastPoint{}; // TabView coords, refreshed each move (the auto-scroll tick reuses it)
            winrt::Windows::Foundation::Point dragOffset{}; // the native tear-out recipe's client-cursor offset, captured at activation
            winrt::TerminalApp::Tab tab{ nullptr }; // the dragged tab (identity — indexes re-resolve each event)
            winrt::Microsoft::UI::Xaml::Controls::TabViewItem tvi{ nullptr }; // its header (the opacity "lifted" cue)
            int originalIndex{ -1 }; // where it sat at activation (forensics)
            int lastSlot{ -1 }; // the last decided insertion slot; -1 = none (tear-out zone / undecided)
        };
        TabReorderGestureState _tabReorder;
        winrt::Windows::UI::Xaml::Controls::Border _tabDragIndicator{ nullptr }; // the insertion caret
        winrt::Windows::UI::Xaml::Controls::Panel _tabDragIndicatorHost{ nullptr }; // the caret's parent panel — the TabView's template root grid (same tree as the strip in BOTH hosting modes: in Root() or re-homed into the titlebar), Root() as the fallback
        SafeDispatcherTimer _tabDragAutoScrollTimer; // hold-still edge auto-scroll while active (guarded Destroy — teardown-safe)
        winrt::Windows::UI::Xaml::UIElement::PreviewKeyDown_revoker _tabReorderKeyRevoker; // Esc-cancel, armed only while active
        void* _dockHoverHwnd{ nullptr }; // Agentmaster (cross-window docking): the dock TARGET window whose insertion caret this window's in-flight drag is currently lighting (Preview) — cleared (HideCaret) on leave/commit/reset. UI thread only

        Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };

        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _tabs;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab> _mruTabs;
        static winrt::com_ptr<Tab> _GetTabImpl(const TerminalApp::Tab& tab);

        void _UpdateTabIndices();

        TerminalApp::Tab _settingsTab{ nullptr };
        // Agentmaster (the Agent Manager PAGE — it used to be the pinned, leftmost, non-closable Manager
        // TAB): this window's one AgentManagerContent, OWNED here for the window's whole life (the tab's
        // pane owned it before) and never a member of _tabs, so no tab loop, drag, move, close, split or
        // rename can ever reach it. Created (hidden) at first layout by _CreateAgentManagerPage and shown
        // by _ShowManagerPage: its root is swapped into the SAME host the tab's content used (_tabContent,
        // so the display and the side panel are exactly as before) and the tab strip is DESELECTED, so no
        // tab reads as focused (tab actions no-op) and the Home button paints as the active tab. Entry
        // points: the Home button (the ONLY one a user drives) plus the three cases where the strip has
        // nothing else to show — the last tab closed, a close whose every remaining tab is dormant, and a
        // reopened window that was on the Manager at exit.
        winrt::com_ptr<implementation::AgentManagerContent> _managerPage{ nullptr };
        // Agentmaster: THE authority for "the Agent Manager page is on screen". _GetFocusedTabIndex answers
        // nullopt while it is set — even if MUX quietly re-selected a tab under the page (it auto-selects a
        // neighbor when an item is removed from a selection-less strip) — so tab-targeting actions can never
        // hit a tab the user can't see. Set by _ShowManagerPage, cleared by _HideManagerPage. UI thread only.
        bool _managerPageVisible{ false };
        // Agentmaster: true only while _DeselectTabStripForManagerPage clears the strip's selection — the
        // SelectionChanged that raises (re-entrantly) is our own bookkeeping, not a user switch, so
        // _OnTabSelectionChanged ignores it. UI thread only.
        bool _managerPageDeselecting{ false };
        // Agentmaster: one-shot latch for a reopened window that was on the Manager page at exit AND has
        // shell tabs to replay. The replay (ProcessStartupActions) creates those tabs ASYNC and each one
        // selects itself, so showing the page up front would be undone by the next shell. _RestoreWindowTabs
        // sets this right before starting the batch; the batch consumes it in its synchronous prologue (so
        // no other batch can steal it) and shows the page after its LAST tab exists. UI thread only.
        bool _showManagerPageAfterStartupBatch{ false };
        // Agentmaster (Fleet Observer O6): a WEAK handle to the Manager page's content (held as its
        // projected IPaneContent — recovered to the impl via winrt::get_self), so _ObserverProbe can
        // push the observer's External (WindowsTerminal) census to it each tick. Weak because every
        // reader is tolerant of "not built"; the content itself is owned by _managerPage above.
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
        // selection CHANGE scrolls here: hover previews and merely RETURNING to the Manager tab (after the
        // selection moved underneath it) never yank the strip. A deliberate RE-click of the already-
        // selected card/row is the one exception, and it bypasses this tracking on purpose: the click
        // itself asks for the reveal (_RevealClickedSessionTab).
        std::wstring _selectionBroughtIntoView;
        // Agentmaster (tab-strip reveal): bumped by every _RevealTabInStrip call. A virtualized-out tab is
        // revealed in two steps — ScrollIntoView now, then a precise pass on later Low-priority ticks
        // (_RevealTabRetryAdjust) — and that retry chain carries the value it started with and stops the
        // moment a newer reveal has run. Every tab selection reveals, so without it a quick run of
        // selections (Ctrl+Tab held down, the tab switcher, a batch close) could have an older chain
        // finish late and scroll the strip back to a tab you already moved past. UI thread only.
        uint64_t _revealTabStripGeneration{ 0 };
        // Agentmaster (tab-strip reveal — the rename box): the strip ignores bring-into-view requests
        // (_EnsureTabStripScrollViewer), so a tab's rename box taking focus no longer scrolls it into view, and
        // neither does MUX when the Equal-width rename hold widens the tab. _TrackRenamingTabInStrip follows the
        // ONE tab whose box is open — _renamingTabViewItem, with _renamingTabSizeChangedRevoker on its
        // SizeChanged — and reveals it through _QueueRevealRenamingTab. _renameRevealTarget is the tab the next
        // queued reveal scrolls to (the newest request wins), and _renameRevealQueued coalesces a burst — a tab
        // that grows on every keystroke (Fit to title) queues one reveal, not one per key. UI thread only.
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::TabViewItem> _renamingTabViewItem;
        winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker _renamingTabSizeChangedRevoker;
        winrt::weak_ref<winrt::TerminalApp::Tab> _renameRevealTarget;
        bool _renameRevealQueued{ false };

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
        // Agentmaster (ui-stall escalation): this window's message-only RESCUE window — the engine
        // watchdog SENDS into it when the UI lane stops beating (a sent message executes on the UI
        // thread even while posted/dispatcher work is jammed — the 2026-08-22 CoreMessaging wedge),
        // proving the pump alive + draining the dispatcher queue. Created on the UI thread at engine
        // init; destroyed via a sent message in ~TerminalPage (Engine.h's escalation block).
        HWND _uiRescueWnd{ nullptr };
        // Agentmaster (cross-window restart): this window's restart sink on the shared engine — another
        // window's "Restart session" (Triage Board card / Explorer-tree row) on a session hosted HERE
        // hops to this window's UI thread and rebuilds its ConPTY connection in place. Detached in
        // ~TerminalPage (Rule #10).
        uint64_t _windowRestartToken{ 0 };
        // Agentmaster (cross-window "Close ▸ Of Same Folder"): this window's close-session sink on the
        // shared engine — a folder-close initiated in ANOTHER window fans a session hosted HERE out to
        // this sink, which hops to this window's UI thread and closes its tab (skip-confirm — the
        // initiating window's one dialog already covered the batch). Detached in ~TerminalPage (Rule #10).
        uint64_t _windowCloseSessionToken{ 0 };
        // Agentmaster (cross-window "Deactivate ▸ Of Same Folder"): this window's park-session sink on the
        // shared engine — a folder-scoped Deactivate started in ANOTHER window fans a session hosted HERE
        // out to this sink, which hops to this window's UI thread and parks its tab (_DeactivateSessionLocal).
        // 0 until engine init registered it. Detached in ~TerminalPage (Rule #10): without that the engine
        // would keep calling a sink whose window is gone (harmless — it holds a weak page reference — but
        // the sink list would grow with every closed window).
        uint64_t _windowDeactivateSessionToken{ 0 };
        // Agentmaster (MESSAGING.md §8.2): this window's two sinks for a REMOTE surface (the Telegram bot): the
        // session-action sink (start a dormant tab, read its screen, mark it unread, rename it, repaint its tag
        // badges / favorite marker — acted on only when THIS window hosts the session) and the launch sink (open a
        // resumed / forked / new session as a background tab here; the engine sends a launch to the most recently
        // activated window). 0 until engine init registered them; detached in ~TerminalPage (Rule #10).
        uint64_t _windowSessionActionToken{ 0 };
        uint64_t _windowLaunchToken{ 0 };
        // Agentmaster (SESSIONS.md §9.14): this window's agent-tool sink on the shared engine — the AI search's window and
        // tab tools (a census of this window's strip, a resume / fork placed at a chosen position here, a park of a tab
        // this window hosts), each ANSWERED from this window's UI thread (_RunAgentWindowTool). 0 until engine init
        // registered it; detached in ~TerminalPage (Rule #10).
        uint64_t _windowAgentToolsToken{ 0 };
        // Agentmaster (eager-init / "Activate All Tabs"): this window's "wake all dormant tabs" sink on
        // the shared engine — the Manager's fleet-wide "Activate All" in ANOTHER window fans out here, and
        // this window eager-inits its own dormant controls. Detached in ~TerminalPage (Rule #10).
        uint64_t _windowActivateAllToken{ 0 };
        // Agentmaster (cross-window tab DOCKING): this window's dock-target sink on the shared engine —
        // another window's in-flight tab drag probes here ("is this screen point over MY strip row, and
        // at which insertion slot?") for the drag-time caret preview and the release-time dock. Detached
        // in ~TerminalPage (Rule #10).
        uint64_t _windowDockTargetToken{ 0 };
        // Agentmaster (COMMANDS.md — command actions): this window's command-action sink on the shared
        // engine — a CommandWatch binding's await resolved (v1: /handover's markdown is on disk); the
        // sink hops to this window's UI thread and acts only when this window HOSTS the origin
        // session's tab (spawn the "<title> (handover)" successor beside it). Detached in
        // ~TerminalPage (Rule #10).
        uint64_t _commandActionToken{ 0 };
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
        // Agentmaster (the RESTORE WAKE — PERSISTENCE.md §14, the user's request of 2026-10-19): after a window restore,
        // the tabs whose session was in the middle of something when Agentmaster went down (persisted Running /
        // NeedsApproval / Error) are woken by themselves — once the tab EVERY reopened window opens to has finished
        // launching (the process-wide RestoreWakeBarrier) and this window's UI thread is idle — one tab per slot of the
        // process-wide RestoreWakePacer (the drip's 500 ms / 4 per 10 s, but across every window), never through the
        // per-window drip above, so the windows released together cannot burst.
        // _restoreWake is the job: its barrier key (set by SetAgentmasterWindowId's announce, or at the join), the
        // candidates _RestoreWindowTabs picked, the session the restore focused, the gate's memory, and the wake queue of
        // the WAKING phase (RestoreWake.h). _restoreWakeTimer ticks _TickRestoreWake every kRestoreWakeTickMs from the end
        // of the re-home until the queue is drained (or the job fails); it is stopped by _EndRestoreWake and in
        // ~TerminalPage, and a dead page's weak Tick stops it. UI thread only, except the key, which ~TerminalPage reads
        // (any thread) to leave the barrier.
        ::Agentmaster::RestoreWakeWindowJob _restoreWake{};
        winrt::Windows::UI::Xaml::DispatcherTimer _restoreWakeTimer{ nullptr };
        // Agentmaster (cross-window settings broadcast): this window's settings sink on the shared
        // engine — a GLOBAL settings change in ANOTHER window (the cog Save, or the Explorer-Tree /
        // Triage-Board sort toggle) hops to this window's UI thread and re-applies it live (the sort
        // toggles + a board/tree re-sort). Detached in ~TerminalPage (Rule #10).
        uint64_t _settingsChangedToken{ 0 };
        // Agentmaster (taskbar unread badge; NOTIFICATIONS.md section 8): this window's count sink on the shared engine's
        // unread census — ANY window's flash / Mark-Unread change that moves the process-wide count hops here, and this
        // window repaints its own taskbar button with the same number (the taskbar combines our windows into one
        // button whose overlay is "the last one received", so they must all agree). Detached in ~TerminalPage (Rule #10).
        uint64_t _unreadCountToken{ 0 };
        // Agentmaster (the GLOBAL PAUSE — DELIVERY.md §17): this window's listener on the shared scheduler's
        // process-wide "pause every automatic send" state (Scheduler::AddGlobalPauseObserver). ANY window's
        // Pause / Resume click reaches it; it hops to this window's UI thread and repaints this window's
        // button and its per-tab badges (_ApplyGlobalPauseShown), so every window shows the ONE state
        // that the scheduler owns — a per-window flag is what once let two windows disagree. Detached
        // in ~TerminalPage (Rule #10): a listener that outlived its page would be called with a dead
        // dispatcher. 0 == not registered (engine init has not run, or the engine has no scheduler).
        uint64_t _globalPauseObserverToken{ 0 };
        ::Agentmaster::AppSettings _appSettings{}; // Agentmaster: global settings (the cog); loaded at engine init
        // Agentmaster (tab sizing): the Equal-mode width bounds THIS window's strip was last sized with (0 ==
        // not yet applied). The bounds themselves live in process-wide app resources, so only this per-window
        // record can tell a second window — reached by the broadcast after the saving window already wrote
        // them — that its own tabs still need MUX's width pass (_ApplyTabSizingSettings).
        uint32_t _tabSizingAppliedMinWidth{ 0 };
        uint32_t _tabSizingAppliedMaxWidth{ 0 };
        // Agentmaster: sessionId -> its terminal tab, so the Manager can Activate (jump) or
        // Kill a session. Weak so closing a tab the normal way doesn't keep it alive.
        std::unordered_map<std::wstring, winrt::weak_ref<TerminalApp::Tab>> _claudeTabs;
        // Agentmaster (TAB_OVERLAY.md): sessionId -> its per-tab "link badge" overlay. STRONG ref
        // (the page builds + owns it); erased alongside _claudeTabs on archive/close/liveness so the
        // overlay's registry observer detaches. Type completed in AgentTabOverlay.h (TerminalPage.cpp).
        std::unordered_map<std::wstring, winrt::com_ptr<implementation::AgentTabOverlay>> _claudeOverlays;
        // --- Agentmaster SIDE PANEL state (TerminalPage.AgentSidePanel.cpp; built by _InitAgentSidePanel) ---
        // UI elements. All UI-thread-only; null until _InitAgentSidePanel builds them (every method
        // early-outs on !_sidePanelBuilt so a pre-init call is a safe no-op).
        winrt::Windows::UI::Xaml::Controls::Grid _sidePanelHost{ nullptr }; // the panel column's root (dark, RequestedTheme Dark): header row + body; also hosts the collapsed expander strip
        winrt::Windows::UI::Xaml::Controls::Border _sidePanelSplitter{ nullptr }; // the draggable VERTICAL separator between the terminal content and the panel (its own grid column)
        winrt::Windows::UI::Xaml::Controls::Grid _sidePanelBody{ nullptr }; // rows: cards area (star*split) / horizontal splitter / summary area (star*(1-split)); hidden while collapsed
        winrt::Windows::UI::Xaml::Controls::Border _sidePanelHSplitter{ nullptr }; // the draggable HORIZONTAL separator between the cards list and the summary area
        winrt::Windows::UI::Xaml::Controls::TextBox _sidePanelSearchBox{ nullptr }; // the header's SECOND line: the full-width filter box (ParseSessionQuery grammar over title/dir/branch/id) — slim (FontSize 11, a ~22px floor via a per-box TextControlThemeMinHeight override) so it sits cleanly under the buttons
        // top-row "Sum" toggle (the second of the right-aligned buttons — after the "+" launcher, left of LOCAL/GLOBAL): checked == the
        // selected session's summary area is SHOWN below the cards (the default); unchecked == hidden, the cards
        // fill the panel and each card shows the rich session tooltip beside the panel on hover (SIDE_PANEL.md
        // §2d). A REAL ToggleButton (the ⌖ idiom) so the accent fill IS the state — kept pinned to
        // !_sidePanelSummaryHidden by _SidePanelUpdateSumButton (a click flips IsChecked itself before Click fires).
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sidePanelSumBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _sidePanelScopeBtn{ nullptr }; // top-row LOCAL/GLOBAL card-list scope toggle (right after "Sum", before the sort toggle; the board's _boardScopeBtn accent look) — flips _sidePanelGlobal for THIS window only
        winrt::Windows::UI::Xaml::Controls::Button _sidePanelSortBtn{ nullptr }; // top-row order toggle — cycles the SHARED AppSettings::boardSort via AgentManagerContent::CycleBoardSort
        // top-row ⌖ "scroll to the selected tab" icon toggle (between the sort toggle and the ⇄ dock flip):
        // checked == AppSettings::sidePanelScrollToSelectedTab (GLOBAL, default ON). A REAL ToggleButton (the
        // Sessions page's scope-toggle idiom), so its accent fill IS the on/off state — keep IsChecked pinned to
        // the setting via _SidePanelUpdateScrollToTabButton (a click flips IsChecked itself before Click fires).
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sidePanelScrollToTabBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _sidePanelCollapseBtn{ nullptr }; // top-row collapse chevron (points TOWARD the docked edge; _SidePanelApplyLayout re-glyphs it on dock flips)
        winrt::Windows::UI::Xaml::Controls::Button _sidePanelStripBtn{ nullptr }; // the collapsed strip's expand chevron (points AWAY from the docked edge)
        // The vertical card list (inside its own ScrollViewer). VIRTUALIZED (SIDE_PANEL.md §2f): its children are
        // ALWAYS [top spacer][the cards of the realized rows, in row order][bottom spacer] — the spacers stand in
        // for every row without a card, so the list's height is the same whichever rows are realized.
        winrt::Windows::UI::Xaml::Controls::StackPanel _sidePanelCards{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _sidePanelCardsScroll{ nullptr }; // the card list's ScrollViewer: its offset + viewport decide which rows are realized (ViewChanged / SizeChanged re-check)
        winrt::Windows::UI::Xaml::Controls::Border _sidePanelCardsTopSpacer{ nullptr }; // _sidePanelCards' FIRST child: the height of the rows above the realized range (first * stride); transparent, so a wheel over it still reaches the ScrollViewer
        winrt::Windows::UI::Xaml::Controls::Border _sidePanelCardsBottomSpacer{ nullptr }; // _sidePanelCards' LAST child: the height of the rows below the realized range ((count - last) * stride)
        winrt::Windows::UI::Xaml::Controls::TextBlock _sidePanelCountText{ nullptr }; // dim "N shown / M live" line under the header (reads the filter's effect at a glance)
        winrt::Windows::UI::Xaml::Controls::Grid _sidePanelSummaryHost{ nullptr }; // the bottom area hosting the fill-mode AgentTabOverlay summary panel (or the no-selection hint)
        winrt::Windows::UI::Xaml::Controls::TextBlock _sidePanelSummaryHint{ nullptr }; // "select a session…" placeholder shown while nothing is bound below
        winrt::Windows::UI::Xaml::Controls::StackPanel _sidePanelStrip{ nullptr }; // the COLLAPSED state's thin expander strip (a chevron button), shown instead of the body
        winrt::Windows::UI::Xaml::Controls::Grid _sidePanelExpandedRoot{ nullptr }; // the EXPANDED state's whole content (header + count + body) — one element to toggle against the strip, so collapse never squeezes live controls into a 20px column
        // The bottom area's summary renderer: a real AgentTabOverlay in fill-host mode for the SELECTED
        // session — maximal reuse of the per-tab summary business logic (analyze/render/JUMP/times/
        // wrap-truncate-previous toggles). DELIBERATELY NOT in _claudeOverlays: that map is keyed by
        // session id and already holds the session's own per-tab overlay when its tab lives here —
        // inserting this one would clobber it. Released (observer detached) on rebind + teardown.
        winrt::com_ptr<implementation::AgentTabOverlay> _sidePanelSummaryOverlay{ nullptr };
        std::wstring _sidePanelSummaryId; // which session the bottom overlay is bound to ("" == hint shown)
        std::wstring _sidePanelSelectedId; // the selected card's session id ("" == none) — kept in step with the Manager lens both ways
        std::wstring _sidePanelSearchText; // the filter box's current raw text (parsed per rebuild)
        // --- The card list, updated IN PLACE (SIDE_PANEL.md §2c) and VIRTUALIZED (§2f) ---
        // THE ROWS: every session the list shows (scope + filter + order), as of the last list update
        // (_SidePanelRebuildCards), and the model each row's card shows or would show (parallel vectors). Only the
        // rows of the realized range [_sidePanelRealizedFirst, _sidePanelRealizedLast) have cards; a scroll realizes
        // other rows from these stored models without touching the registry.
        std::vector<std::wstring> _sidePanelRowIds;
        std::vector<AgentSidePanelCardModel> _sidePanelRowModels;
        size_t _sidePanelRealizedFirst{ 0 }; // the first realized row (an index into _sidePanelRowIds)
        size_t _sidePanelRealizedLast{ 0 }; // one past the last realized row (== first: no card exists)
        // The height of one row — a card plus its vertical margins, DIPs — that the spacers and the realized range
        // are computed with: the MEDIAN of the realized cards' measured heights (AgentMaster/VirtualListWindow.h
        // MedianRowHeight), so one taller card cannot move it. 0 until a card was measured (the default estimate
        // kSidePanelCardDefaultStride applies until then).
        double _sidePanelCardStride{ 0.0 };
        bool _sidePanelRealizeQueued{ false }; // a coalesced realized-range re-check is queued (_SidePanelQueueRealize) — the ScrollViewer's SizeChanged burst and the post-layout measure fold into it
        bool _sidePanelRealizing{ false }; // a _SidePanelSyncRealizedCards pass is running — a ViewChanged it causes (a spacer resize can move the offset) never re-enters it
        // id -> its card: the elements + the model they show — the REALIZED rows only. A list update edits the
        // card of every realized session that stays (only the values that changed), creates cards for rows newly
        // realized, and drops the cards of rows that left the realized range or the list; a selection change
        // restyles two borders through it; the ⌖ reveal realizes its row first, then finds its card in it. Never
        // rebuilt wholesale except by _SidePanelResetCards.
        std::unordered_map<std::wstring, AgentSidePanelCardView> _sidePanelCardViews;
        // The card ids in EXACTLY the order of the cards between the two spacers of _sidePanelCards.Children() —
        // the "current" list the planner (AgentMaster/KeyedListDiff.h) turns into the new realized order with the
        // fewest removes / inserts / moves, edited op by op alongside the panel (child index = mirror index + 1, the
        // top spacer being child 0) so the two never differ (a mismatch resets the list).
        std::vector<std::wstring> _sidePanelCardOrder;
        std::wstring _sidePanelCountValue; // the count line's text as last written — written again only when it changes
        // The cards' bookmark tags, cached per session id: a row re-reads its session-store file only when
        // ::Agentmaster::SessionTagsEpoch() moved (a tag or tag color changed anywhere in this process) or the
        // session is new to the list — never one stat per row per update. Pruned to the listed rows (every row's
        // model carries its tags, realized or not).
        std::unordered_map<std::wstring, std::vector<std::wstring>> _sidePanelTagsById;
        std::map<std::wstring, std::wstring> _sidePanelTagColors; // one LoadAllTagColors() per epoch, read only once a rendered card is tagged
        bool _sidePanelTagColorsLoaded{ false }; // _sidePanelTagColors holds this epoch's read
        uint64_t _sidePanelTagsEpoch{ UINT64_MAX }; // the epoch the two caches were read at (UINT64_MAX = never read)
        // Live layout state — the per-window truth _CaptureWindowRecord persists. Fractions are of the
        // CONTENT AREA width (vertical) / the panel body height (horizontal split); both stay inside
        // the load-time bands ((0.10,0.50] / (0.15,0.85)) because the drag clamps write them.
        double _sidePanelWidthFraction{ 0.22 };
        double _sidePanelSplitFraction{ 0.5 };
        bool _sidePanelCollapsed{ false };
        // The "Sum" toggle's state: true == the summary area (and its splitter) is HIDDEN, the cards fill the
        // body, and every card shows the RICH session tooltip beside the panel on hover; false (the default) ==
        // the classic layout (cards over the selected session's summary, each card with its small floating tip).
        // Same persistence contract as _sidePanelCollapsed: per window (WindowRecord::sidePanelSummaryHidden) and
        // the last change sets the new-window default (AppSettings::sidePanelSummaryHidden).
        bool _sidePanelSummaryHidden{ false };
        // The card list's SCOPE: false == LOCAL — only the sessions whose tab is hosted in THIS window
        // (_HostedSessionIds), the default ALWAYS; true == GLOBAL — every window's live sessions.
        // Persisted PER WINDOW ONLY (WindowRecord::sidePanelGlobal, absent == LOCAL) and deliberately
        // NEVER exported as an AppSettings default (unlike the fractions/collapse above): a new window
        // must start on its own tabs, so one window's fleet-wide view can't leak into another's list.
        bool _sidePanelGlobal{ false };
        // The hosted-session set the LOCAL list was last built from. The periodic tick diffs the live
        // set against it because a tab docked into / torn out of this window changes _claudeTabs with
        // NO registry notify (the moved session stays live + unchanged — _DetachClaudeTabForMove drops
        // only the per-window binding), so the registry-drain trigger alone would leave a LOCAL list
        // stale. Empty while GLOBAL (the drain is complete there; nothing to diff).
        std::unordered_set<std::wstring> _sidePanelRenderedLocalIds;
        bool _sidePanelBuilt{ false }; // _InitAgentSidePanel completed — the master gate for every side panel entry point
        bool _sidePanelDirty{ false }; // a rebuild was deferred (min-gap throttle, or collapsed) — the periodic tick / expand catches up
        bool _sidePanelLayoutQueued{ false }; // a coalesced _SidePanelApplyLayout tick is queued (_SidePanelQueueLayout) — later triggers fold into it
        // The ⌖ scroll-to-tab REVEAL, one-shot per tab selection: the session whose card the list still owes a
        // scroll ("" == none owed). Set by _SidePanelRevealTabCard, consumed by the first reveal tick that finds
        // the card RENDERED — it deliberately survives a card that doesn't exist yet (a just-launched session
        // whose card lands on the next rebuild, a collapsed panel, a card the filter hides), so the NEXT rebuild
        // or the expand pays it. Cleared by a card click (the user's own pick outranks it), by selecting a tab
        // with no card, and by turning the toggle off — a stale one would yank the list somewhere you've left.
        std::wstring _sidePanelRevealPendingId;
        bool _sidePanelRevealQueued{ false }; // a coalesced reveal tick is queued (_SidePanelQueueReveal) — rapid Ctrl+Tab cycling folds into ONE scroll, to the LAST tab
        uint64_t _sidePanelLastRebuildMs{ 0 }; // GetTickCount64 of the last card rebuild (the min-gap throttle anchor)
        uint64_t _sidePanelNextTimingMs{ 0 }; // next GetTickCount64 at/after which the in-place timing-text refresh runs (~30s cadence)
        // --- The header's "+" LAUNCHER (SIDE_PANEL.md §2e) ---
        // A click on "+" opens a CARD at the click point holding the Agent Manager's own launch row — the shared
        // AgentLaunchBar, Claude only: [● Claude] [ box ] [Launch Claude] [Fork] — so a session starts from the side
        // panel without leaving the tab. The card is built on the FIRST click (_SidePanelEnsureLaunchCard) and kept for
        // the window's life; a window that never opens it pays nothing. Closed by a press outside it, Esc, "+" again,
        // a launch, a tab switch, a collapse and a dock flip. UI-thread-only, like every side panel member.
        // The "+" header button (line 1, column 1: the first of the right-aligned buttons, left of "Sum").
        winrt::Windows::UI::Xaml::Controls::Button _sidePanelLaunchBtn{ nullptr };
        // The card's Popup, parented into Root() across its three rows (the tag editor's recipe), so its offsets are
        // Root-relative and it can sit anywhere in the window; null until the first click.
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _sidePanelLaunchPopup{ nullptr };
        // The card itself (dark, the tag editor's look): the outside-press boundary, the key handlers' owner, the
        // placement's measured size, and the focus-restore test ("was focus inside the card when it closed?").
        winrt::Windows::UI::Xaml::Controls::Border _sidePanelLaunchCard{ nullptr };
        // The card's launch bar: the Agent Manager's launch code verbatim (AgentLaunchBar.h), a Claude-only host whose
        // drop-down lives in Root(). Shut down in ~TerminalPage (its hover timer is UI-affine).
        std::shared_ptr<AgentLaunchBar> _sidePanelLaunchBar;
        // Where the card's corner sits, in Root() coordinates: the "+" press point (the tooltip-like placement the user
        // asked for), else the "+" button's bottom center. Kept while open, so a re-placement after a size change
        // stays anchored to the same point.
        winrt::Windows::Foundation::Point _sidePanelLaunchAnchor{ 0.0f, 0.0f };
        // _sidePanelLaunchAnchor holds a press on "+" that its Click has not consumed yet. Cleared by the Click and by
        // the button's PointerCaptureLost (a press that ended off the button, without a click), so a Click without a
        // press of its own (UI Automation) never reuses a stale point.
        bool _sidePanelLaunchAnchorFromPress{ false };
        bool _sidePanelLaunchPlaceQueued{ false }; // a coalesced fit + placement tick is queued (_SidePanelQueueLaunchCardPlacement) — the card's / Root's SizeChanged burst folds into it
        bool _sidePanelLaunchHooked{ false }; // the card's two Root() hooks (the outside-press close, the resize re-placement) are registered — once per page, even when a failed build is retried
        bool _sidePanelLaunchCardDockLeft{ true }; // the dock side when the card opened: a settings broadcast that moved the dock closes the card (its "+" moved away)
        double _sidePanelLaunchBoxWidth{ 0.0 }; // the card box's width as last fitted (_SidePanelFitLaunchBox) — written again only when it changes
        // The key the card's PreviewKeyDown armed (Enter / Escape; None = nothing armed) and what its key-up does. The
        // card acts on the KEY-UP (the AgentModelPrompt.h rule): closing on the key-down would hand the rest of the
        // keystroke — the CR / ESC character — to the terminal that takes focus next (a submitted draft, an
        // interrupted turn).
        winrt::Windows::System::VirtualKey _sidePanelLaunchArmedKey{ winrt::Windows::System::VirtualKey::None };
        enum class SidePanelLaunchKeyAction : uint8_t
        {
            None, // nothing armed
            Close, // Escape with the drop-down closed: close the card
            Launch, // Enter on the focused Launch button: what its click does
            Fork, // Enter on the focused Fork button: what its click does
        };
        SidePanelLaunchKeyAction _sidePanelLaunchArmedAction{ SidePanelLaunchKeyAction::None };
        // Splitter drag state (one drag at a time; pointer captured on the splitter Border).
        bool _sidePanelDragWidth{ false }; // a vertical-separator drag is in flight
        bool _sidePanelDragSplit{ false }; // a horizontal-separator drag is in flight
        double _sidePanelDragStart{ 0.0 }; // pointer X (width drag) / Y (split drag) at press, island-relative — only the delta matters
        double _sidePanelDragStartVal{ 0.0 }; // panel width px (width drag) / cards-area height px (split drag) at press
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
        // Agentmaster (duplicate conversations, OBSERVER.md §20): the "<session id>|<lowercase WT_SESSION>"
        // pairs for which this window has already logged that it did NOT bind the tab, because the tab's
        // claude runs a conversation that another tab hosts (the observer marked the row a duplicate, the
        // registry lists the pair as a duplicate host, or the conversation is still bound to another tab
        // of this window whose claude still runs it). A pair is logged once, not once per ~2.5 s probe tick
        // — a duplicate can last for days. _ObserverProbeImpl prunes a pair when no source reports it any
        // more, so a later duplicate of the same pair logs again. UI thread only; empty in the normal case.
        std::unordered_set<std::wstring> _duplicateConversationLogged;
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
        // sessions whose tab is currently flashing. Multiple flashing tabs blink in LOCKSTEP (the synchronization
        // requirement) and a tab that starts flashing mid-cycle joins at the current phase — since UI_THREAD.md
        // §10i because each tab header blinks its ring on the UI thread's ONE shared composition clock
        // (AgentLockstepBlink); a per-window DispatcherTimer used to toggle a phase and flip every ring's
        // Visibility every 600ms (a layout pass + a XAML frame per flip). UI thread only.
        std::unordered_map<std::wstring, ::Agentmaster::SessionState> _agentFlashLastState;
        // Agentmaster: per-session record of the CURRENT Running span — when it started, and whether it
        // was entered from NeedsApproval (i.e. the user answering a question released it). Feeds the
        // pure ShouldSuppressAnswerBlipFlash floor so the brief "answered -> agent replies -> turn ends"
        // pass-through Running (~1-1.5s, measured) does not newly flash every answered question's tab.
        // Erased with _agentFlashLastState on !live so a restore re-tracks fresh.
        struct AgentRunSpan
        {
            int64_t sinceMs{};
            bool fromNeedsApproval{};
        };
        std::unordered_map<std::wstring, AgentRunSpan> _agentFlashRunSpan;
        std::unordered_set<std::wstring> _flashingSessions;
        // Agentmaster (Mark Unread): sessions MANUALLY marked unread via the tab context menu. Drives the
        // SAME red ring on the SAME shared timer (a session's tab flashes if it is in EITHER set), but is
        // STICKY: the automatic state logic never clears it, and it flashes even the currently-FOCUSED tab
        // — only a VISIT (a switch TO the tab, _VisitTabClearFlash) or archive clears it.
        std::unordered_set<std::wstring> _manualUnreadSessions;
        // Agentmaster (taskbar unread badge; NOTIFICATIONS.md section 8): the sorted, deduped union of _flashingSessions
        // and _manualUnreadSessions as last handed to the engine's unread census — so the 600 ms flash tick and every
        // idempotent flash call re-publish only on a REAL change (no census lock, no fan-out, no log line otherwise).
        std::vector<std::wstring> _publishedUnreadSessions;
        // The count this window last raised TaskbarUnreadCountChanged for (the census count, or 0 while the cog switch
        // is off): the de-dupe that keeps the AppHost from re-rendering an identical overlay.
        uint32_t _taskbarUnreadCount{ 0 };
        // Agentmaster (taskbar preview frame; NOTIFICATIONS.md section 9): the frame color this window last raised
        // TaskbarUnreadFrameChanged for -- non-zero while THIS window holds an unread tab and the switch is on, 0
        // otherwise. Unlike the count, it is PER WINDOW: the frame marks which window's preview to look at.
        uint32_t _taskbarUnreadFrameArgb{ 0 };
        // Agentmaster (tab status-dot RED FLASH): the per-window 600 ms flash timer, running while any session of this
        // window flashes. BOOKKEEPING only since UI_THREAD.md §10i (_OnAgentFlashTick): it re-asserts each flashing
        // tab's ring, prunes sessions whose tab went away and re-publishes the taskbar unread census. The blink
        // itself is the tab headers' composition animation; the phase this timer used to toggle
        // (_agentFlashPhase) is gone with the Visibility flip it drove.
        winrt::Windows::UI::Xaml::DispatcherTimer _agentFlashTimer{ nullptr };
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
        // Agentmaster (flows TAB-K1): once set, NOTHING writes the record again — _FlushWindowRecord refuses (and
        // logs `[window-save] <id> late save refused`) and _ScheduleWindowRecordSave arms no autosave, because an
        // autosave still pending at the close used to fire after the archive and save the Claude tabs as shells.
        // Set only after the user's final decision, so a window that stays open never stops saving.
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
            // Agentmaster (SESSIONS.md §10, the folder filter): where the session's tool calls concentrate —
            // InferWorkingDirectory over its sidecar's pathsAccessed (machine temp excluded, the git arm per
            // AppSettings::inferGitRoot): the rule the tab colors use. EMPTY while it is not computed yet (the
            // follow-up pass _InferSessionsRowDirs fills it shortly after a gather; a cache hit fills it in the
            // gather itself) and when it is the same folder as `dir`. Read by the folder dropdown's lists, the
            // folder filter, the row tooltip and the detail pane's "works in" line.
            std::wstring inferredDir;
            std::wstring branch;
            int64_t createdMs{ 0 }; // fork-aware (a fork's copied line stamps lie -> file birth)
            int64_t lastActivityMs{ 0 }; // last MESSAGE timestamp (mtime only as fallback)
            int64_t sizeBytes{ 0 };
            int msgs{ 0 }; // REAL user prompts
            int tools{ 0 }; // assistant tool_use blocks
            int64_t contextTokens{ 0 }; // newest assistant turn's usage (≈ context occupancy); the compact "Ctx" column — the same value the Triage Board shows as "ctx N"
            bool fork{ false };
            std::wstring forkedFromId;
            // Agentmaster (SESSIONS.md §9): true for a row that the AI search added — a session it
            // shows that is OUTSIDE the page's time window, so the gather did not list it. Such a row
            // (and its index entry, at the same position in _sessionsEntries) sits at the END of the
            // vectors, is shown only in the AI search's list, and is put back after every gather
            // (_SessionsAiMergeExtraRows). The normal list skips it.
            bool aiExtra{ false };
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
        // Agentmaster (PRESETS): true while the "preset can't launch" notice is up (or queued this tick), so a
        // burst — a window restore of several preset sessions with no key — shows ONE dialog. Re-armed when the
        // dialog closes, and in its catch, or the notice could never show again for the window's life.
        bool _presetLaunchErrorShowing{ false };
        bool _addPresetsHelpShowing{ false }; // Agentmaster (PRESETS): the "Add presets" dialog is up — a second click while it shows opens nothing (_PromptAddPresets)
        // Agentmaster (MESSAGING.md §21.23): true while _RemoteLaunch runs the launch seams for a request from the phone. A
        // refusal those seams would show as a MODAL on the PC — no native claude.exe (_PromptClaudeMissing), a preset that
        // cannot start (_PromptPresetLaunchError) — and the silent refusals of _LaunchClaudeSession are recorded in
        // _remoteLaunchRefusal instead (_NoteRemoteLaunchRefusal): the user is at the phone, nobody may be at the PC to read a
        // dialog, and the reason goes back to the phone in the launch's answer. Set and cleared on the UI thread around ONE
        // synchronous launch (the _openClaudeTabInBackground idiom), so no other launch ever sees it.
        bool _remoteLaunchInFlight{ false };
        std::optional<::Agentmaster::RemoteLaunchOutcome> _remoteLaunchRefusal; // the FIRST refusal a seam recorded while _remoteLaunchInFlight was set
        // The visible row ids in TABLE (sorted+filtered) order — the Up/Down keyboard
        // navigation list (the archive page's _archiveVisibleOrder pattern). Rebuilt each render.
        std::vector<std::wstring> _sessionsVisibleOrder;

        // ===== Agentmaster (SESSIONS.md §9): the Sessions page's AI SEARCH =======================
        // The "AI" toggle beside the search box turns the box into a request line: Enter sends the
        // text to the process's ONE search conversation (SharedSessionSearchAgent — a hosted Sonnet
        // with two tool servers), which searches with the page's own search and tells the page which
        // sessions to show. The page is a VIEW of that conversation: every window's Sessions page
        // shows the same one. All of this state is UI-thread-only; the agent's events arrive on its
        // threads and are marshalled here (_SessionsAiAttach). Implementation:
        // TerminalPage.AgentSessionsAi.cpp.
        bool _sessionsAiMode{ false }; // the AI toggle is on: the search box sends requests, and the model's list (when there is one) replaces the table's rows
        bool _sessionsAiInUse{ false }; // this page is counted in the process-wide tally of pages that show the AI search (the last one out frees the agent's index and parks its process)
        uint64_t _sessionsAiListenerToken{ 0 }; // the agent listener of this page; 0 = not attached. Removed in ~TerminalPage (Rule #10)
        uint64_t _sessionsAiEpoch{ 0 }; // the conversation epoch the page shows; an event of an older one (before a reset) is dropped
        bool _sessionsAiHasShown{ false }; // _sessionsAiShownIds is a list the model put on the screen (an EMPTY one means "nothing fits": show no row)
        bool _sessionsAiPreview{ false }; // _sessionsAiShownIds is the rows of a search that the model has not picked from yet (shown at once, replaced by its pick)
        uint64_t _sessionsAiListTurn{ 0 }; // the request the list (or preview) belongs to: a preview never replaces the final list of its own request
        std::vector<std::wstring> _sessionsAiShownIds; // the listed sessions, full ids, in the model's order
        std::vector<::Agentmaster::SessionIndexEntry> _sessionsAiEntries; // their index entries as the agent sent them: the source of the rows outside the page's time window
        // The model's evidence per listed session (SESSIONS.md §9.12): full id -> its `why`, the one line it saw when it
        // read the session. Drawn as a quote line under the row's title and in full in the detail pane
        // (_SessionsAiReasonFor). Empty for a search's preview, cleared with the list.
        std::unordered_map<std::wstring, std::wstring> _sessionsAiReasons;
        // The table's sort when the list arrived. While the sort still is this one, the list keeps
        // the MODEL's order; a click on a column header changes the sort, and that column then
        // sorts the list like any other.
        int _sessionsAiSortColumn{ 7 };
        bool _sessionsAiSortAscending{ false };
        winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _sessAiBtn{ nullptr }; // "AI": the mode toggle, INSIDE the search box at its right end (like an editor's regex toggle); a sibling of the box in the same grid cell, drawn over it
        bool _sessAiBoxPadded{ false }; // the search box's right padding was grown once, so its text ends before the toggle (_SessionsAiFitToggleIntoBox)
        bool _sessAiClearButtonMoved{ false }; // the search box's own clear (X) button was found in its template and shifted left of the toggle
        winrt::Windows::UI::Xaml::Controls::Button _sessAiResetBtn{ nullptr }; // the crossed circle: throw the conversation away. Shown in AI mode only
        winrt::Windows::UI::Xaml::Controls::Border _sessAiStrip{ nullptr }; // header row 3: the request, what happens now / the model's sentence, the context size. Shown in AI mode only
        winrt::Windows::UI::Xaml::Controls::Border _sessAiDot{ nullptr }; // the strip's state dot (working / ready / stopped / failed): a round Border, so this header needs no Shapes projection
        winrt::Windows::UI::Xaml::Controls::TextBlock _sessAiQuestionText{ nullptr }; // the strip's first line: the newest request
        winrt::Windows::UI::Xaml::Controls::TextBlock _sessAiStatusText{ nullptr }; // the strip's second line: the activity, the answer, or the hint
        winrt::Windows::UI::Xaml::Controls::TextBlock _sessAiUsageText{ nullptr }; // the strip's right end: context tokens of the conversation
        // The strip's "thinking <level>" toggle, left of the context size: a click moves the AI
        // search to the next thinking level (off -> low -> ... -> max -> off), persisted for every
        // window (AppSettings::sessionsAiThinkingLevel). Shown in AI mode only, with the strip.
        winrt::Windows::UI::Xaml::Controls::Button _sessAiThinkingBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _sessAiThinkingText{ nullptr }; // the toggle's label, "thinking off" ... "thinking max"

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
            // Agentmaster (SESSIONS.md §10): the FOLDER FILTER's picked folders (display spellings, in pick
            // order) from the filters row's "Folders" dropdown. Unlike every other facet these combine with
            // OR: a row passes when ANY picked folder covers its launch folder or its inferred folder
            // (SessionInPickedFolders — exact, or with the subfolders per _sessFolders.includeSubfolders).
            // The folder facet as a whole still ANDs with the rest. Rides this struct so the ✕ chip, "Clear
            // filters" and the "· filtered" count line cover it exactly like the tags.
            std::vector<std::wstring> folders;
            bool Any() const { return hasDir || hasBranch || timeGran != TimeGran::None || hasFamily || !tags.empty() || !folders.empty(); }
        };
        _SessionsRowFilterState _sessionsRowFilter;

        // Agentmaster (SESSIONS.md §10): the FOLDER FILTER dropdown's controls and state. The "📂 Folders"
        // toggle sits in the filters row; it opens ONE popup (built once, parented into the page host — a
        // main-tree popup, so its find box takes keypresses under XAML Islands) holding a find box, the
        // folder list split 50% / 50% — RECENT (the folder whose newest session was active last first) over
        // MOST USED (the folder more sessions worked in first), both over the page's gathered sessions —
        // the picked folders as removable chips, "Include subfolders" and Clear. Dismissed by a press
        // outside the card, Escape, the toggle, the page's hide and the tab-switch dismiss. All UI-thread.
        struct _SessionsFolderFilterUi
        {
            winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton button{ nullptr }; // the filters-row trigger; checked (accent) while any folder is picked
            winrt::Windows::UI::Xaml::Controls::Primitives::Popup popup{ nullptr }; // the dropdown, parented into the page host
            winrt::Windows::UI::Xaml::Controls::Border card{ nullptr }; // the popup's child: the inside/outside test of the outside-press dismissal
            winrt::Windows::UI::Xaml::Controls::TextBox findBox{ nullptr }; // narrows both halves; Enter adds (a typed absolute path, else the top RECENT match)
            winrt::Windows::UI::Xaml::Controls::Button addBtn{ nullptr }; // "+": the find box's Enter, shown only while Enter would add something
            winrt::Windows::UI::Xaml::Controls::Grid lists{ nullptr }; // the two halves (header, list, header, list — the lists in equal Star rows); its Height is set per open
            winrt::Windows::UI::Xaml::Controls::TextBlock recentHeader{ nullptr }; // "RECENT · N folders"
            winrt::Windows::UI::Xaml::Controls::StackPanel recentList{ nullptr }; // the RECENT half's rows
            winrt::Windows::UI::Xaml::Controls::TextBlock usedHeader{ nullptr }; // "MOST USED · N folders"
            winrt::Windows::UI::Xaml::Controls::StackPanel usedList{ nullptr }; // the MOST USED half's rows
            winrt::Windows::UI::Xaml::Controls::TextBlock hint{ nullptr }; // one line under the lists: inference running / nothing matches / what Enter adds
            winrt::Windows::UI::Xaml::Controls::StackPanel pickedPanel{ nullptr }; // the picked folders as "name ✕" chips (or a dim "none picked" line)
            winrt::Windows::UI::Xaml::Controls::CheckBox subfoldersBox{ nullptr }; // "Include subfolders" (mirrors includeSubfolders)
            winrt::Windows::UI::Xaml::Controls::Button clearBtn{ nullptr }; // drops every picked folder (keeps the subfolders preference)
            bool includeSubfolders{ false }; // a picked folder also covers the folders under it; a page preference kept across "Clear filters", not persisted
            bool outsideHooked{ false }; // the page host's handledEventsToo PointerPressed hook is registered (once per page)
            bool listsQueued{ false }; // a find-box rebuild is queued for the next tick (coalesces a typing burst)
            bool renderOwed{ false }; // inferred folders landed while the page was hidden: re-render the table when it comes back
            std::wstring topMatch; // the first RECENT row of the current find text — what Enter adds when the text is not a path
            std::unordered_map<std::wstring, std::wstring> knownSpelling; // NormDirKey -> the lists' spelling, so a typed path adopts the spelling the sessions use
        };
        _SessionsFolderFilterUi _sessFolders;
        // Agentmaster (SESSIONS.md §10): the inferred-folder CACHE — one entry per session, valid while its
        // transcript's (size, mtime) and the git-arm setting are unchanged, so only the first gather of a
        // window pays the inference (git probes on a cold disk cache measured 6.6 s for a month). UI-thread
        // map; the gather reads a COPY off-thread. Never pruned: bounded by the sessions ever listed here.
        struct _SessionsInferredDir
        {
            int64_t sizeBytes{ 0 }; // the transcript size the inference read
            int64_t mtimeMs{ 0 }; // the transcript mtime the inference read
            bool git{ false }; // AppSettings::inferGitRoot at that time (the git arm changes the answer)
            std::wstring inferred; // the inferred folder; EMPTY = the same folder as the launch folder
        };
        std::unordered_map<std::wstring, _SessionsInferredDir> _sessionsInferredDirCache;
        // One session the follow-up inference pass must compute (a cache miss of the gather): its id, launch
        // folder (the inference's fallback), the (size, mtime) key, and a COPY of its tool-touched paths.
        struct _SessionsInferJob
        {
            std::wstring id;
            std::wstring workingDir;
            int64_t sizeBytes{ 0 };
            int64_t mtimeMs{ 0 };
            std::vector<std::wstring> paths;
        };
        uint64_t _sessionsGatherGen{ 0 }; // bumps each time a gather lands; an inference pass applies its results to the rows only for ITS gather (the cache takes them either way)
        std::atomic<int> _sessionsInferPasses{ 0 }; // inference passes running now (a re-gather can start a second one); > 0 = the dropdown's hint says "still finding"

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
        // Agentmaster (tab move): true only while _TryMoveTab has the moved TabViewItem OUT of TabItems.
        // Taking the SELECTED item out makes the TabView select its nearest neighbor for that instant,
        // and _OnTabSelectionChanged would otherwise run the whole selection funnel on a tab the user
        // never visited — marking it read, clearing its red flash and any manual Mark Unread, waking it
        // if it was Deactivated, logging a false [nav] tab-focus, and scrolling the strip to it (the
        // hooks.log shows `tab-move` -> `tab-focus <neighbor>` -> `tab-focus <moved>`). The funnel ignores
        // selection changes while this is set; the one real selection of the moved tab runs right after
        // it clears. UI thread only.
        bool _movingTab{ false };
        // Agentmaster (tab-strip reveal — a NEW tab): true only while _InitializeTab makes a brand-new tab the
        // strip's selection, BEFORE the strip's first layout pass with that tab in it. _OnTabSelectionChanged
        // then HOLDS the switch (it records it in _tabSwitchHeldForNewTab and returns), and _InitializeTab runs
        // the held switch right after that layout pass. Two orderings are forced here:
        //   - the selection must come BEFORE the layout pass: the pass grows MUX's ItemsPresenter, and MUX
        //     answers that by scrolling the SELECTED tab into view (TabView::OnItemsPresenterSizeChanged ->
        //     BringSelectedTabIntoView). When the selection came after the pass, the selected tab during it was
        //     still the tab the user had been on, so a tab opened from ANOTHER tab's context menu ("New Session
        //     Here", "Fork session", "Duplicate tab") scrolled the strip back to the old tab and left the new
        //     one, opened right under the mouse, off screen (2026-10-03, release: 28 tabs away in a 453-tab
        //     window). MUX's request is processed after the switch already found the new tab on screen, so
        //     nothing undid it. Since 2026-10-05 the strip ignores such requests (_EnsureTabStripScrollViewer
        //     turns its bring-into-view off), so this order is a belt for a strip whose ScrollViewer is not bound
        //     yet; the next point still holds on its own.
        //   - the switch must come AFTER the layout pass: the switch reveals the tab in the strip, and a tab the
        //     strip has not laid out yet can only be revealed with ListView::ScrollIntoView, whose panel then
        //     places it at an ESTIMATED position (it recycles every header and anchors the tab index-times-the-
        //     average-width from the first realized one), far off on a strip of hundreds of tabs. The switch
        //     also swaps the tab's content in, which must keep its first layout on the next tick (a terminal
        //     control starts its connection on its first layout, and _LaunchClaudeSession wires the session up
        //     after _InitializeTab returns).
        // UI thread only.
        bool _holdTabSwitchForNewTab{ false };
        // Agentmaster (tab-strip reveal — a NEW tab): set by _OnTabSelectionChanged when it held a switch under
        // _holdTabSwitchForNewTab. _InitializeTab reads and clears it, and runs the held switch only when one was
        // actually raised: a selection that changed nothing raises no SelectionChanged, and running the funnel
        // then would re-run the switch for the tab the user was ALREADY on — scrolling the strip to it, the very
        // jump the hold prevents. UI thread only.
        bool _tabSwitchHeldForNewTab{ false };

        // Agentmaster (Shift+Click background-activate): a Shift+Click on a managed session tab must
        // ACTIVATE the session in place WITHOUT switching to it. A MUX TabViewItem drives its selection
        // on pointer-RELEASE (after our press handler), so the switch can't be cancelled at press time.
        // Since TAB-K4 (2026-10-23) the press keeps that release from selecting the tab at all
        // (_tabShiftActivatePress below); this one-shot veto is the FALLBACK for a press whose release
        // swallow could not be armed: _OnTabPointerPressed ARMS it and _OnTabSelectionChanged snaps the
        // selection back to _tabSelectRevertTo before any content-swap side effects run.
        // _revertingTabSelection guards the revert's own re-entrant SelectionChanged.
        bool _suppressTabSelectForActivate{ false };
        bool _revertingTabSelection{ false };
        winrt::Windows::Foundation::IInspectable _tabSelectRevertTo{ nullptr };
        // Agentmaster (TAB-K4, 2026-10-23): the Shift+Click press in flight on a managed session tab, the press
        // _OnTabPointerPressed turned into "activate in place". While it is engaged, a one-shot PointerReleased handler
        // sits on the pressed tab's template root and marks the press's release handled, so the item skips the selection
        // tap it queued at the press: no selection, no focus moved onto the tab header, no strip scroll
        // (_ArmTabShiftActivateRelease). Engaged by the press; reset, which revokes the handler, by that release and by
        // every later tab press. UI thread only.
        struct TabShiftActivatePress
        {
            uint32_t pointerId{ 0 }; // the pressing pointer; a release of any other pointer passes untouched
            winrt::Windows::UI::Xaml::UIElement::PointerReleased_revoker releaseRevoker; // removes the one-shot handler (on reset, or the page's destruction)
        };
        std::optional<TabShiftActivatePress> _tabShiftActivatePress;

        bool _activated{ false };
        bool _visible{ true };

        // Agentmaster: theme-refresh coalescing (the 2026-08-14 RDP freeze). While
        // _RefreshUIForSettingsReload walks the panes, every TermControl::UpdateControlSettings
        // raises PropertyChanged("BackgroundBrush") whose handler re-enters _updateThemeColors —
        // panes × tabs × the per-tab visual-state flip made one settings reload a multi-minute
        // UI-thread grind at fleet scale. The reload suppresses the re-entry and runs ONE trailing
        // pass; the new-tab-button repaint keys on its input pair so an unchanged repaint skips its
        // resource inserts + two VisualStateManager transitions.
        bool _themeColorUpdatesSuppressed{ false };
        std::optional<std::pair<til::color, til::color>> _lastNewTabButtonColorKey;

        std::vector<std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>> _previouslyClosedPanesAndTabs{};

        uint32_t _systemRowsToScroll{ DefaultRowsToScroll };

        // use a weak reference to prevent circular dependency with AppLogic
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _dialogPresenter;
        // Agentmaster (flows TAB-K3): this page's About dialog, BUILT IN CODE on the first "Open about dialog" and kept
        // for the page's life — the lifetime the declared, lazily realized x:Load element had. Kept rather than rebuilt
        // per open because AboutDialog checks for an update in its constructor: one instance = one check, as before.
        // Code-built (never in TerminalPage.xaml) so WinUI shows it as a whole popup that UI Automation and screen
        // readers can read (see _ShowDialogHelper). nullptr until the first open; UI thread only.
        winrt::TerminalApp::AboutDialog _aboutDialog{ nullptr };

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

        // Agentmaster (flows TAB-K3): show a dialog BUILT IN CODE through this window's presenter (it used to take the
        // x:Name of a dialog declared in TerminalPage.xaml; no dialog is declared there any more). A dialog that is in the
        // live tree is shown by WinUI with only its inner layout moved into a popup nested inside the page, which UI
        // Automation does not reach; a code-built one is shown as a whole popup named by its title. The one-dialog-at-a-time
        // rule is the presenter's (TerminalWindow::ShowDialog).
        //   dialog - the dialog to show (by value: the coroutine keeps it alive across its suspension).
        // Returns the button the user chose, or None when the window has no presenter or another dialog won the race.
        // Throws whatever ShowAsync throws (the callers' contract is unchanged).
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowDialogHelper(winrt::Windows::UI::Xaml::Controls::ContentDialog dialog);

        void _ShowAboutDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowConfirmCloseDialog(ConfirmCloseDialogKind kind);
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseReadOnlyDialog();
        // Agentmaster (flows TAB-K3): the multi-line paste warning, built in code; it now takes the preview it shows (the
        // declared dialog's ClipboardText block was filled by the paste handler before the call).
        //   clipboardPreview - the (already shortened) clipboard text to preview; cleared from the dialog after it closes.
        // Returns the button chosen (Primary = paste anyway). Throws what _ShowDialogHelper throws.
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowMultiLinePasteWarningDialog(winrt::hstring clipboardPreview);
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowLargePasteWarningDialog();

        void _CreateNewTabFlyout();
        std::vector<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItemBase> _CreateNewTabFlyoutItems(winrt::Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::NewTabMenuEntry> entries);
        winrt::Windows::UI::Xaml::Controls::IconElement _CreateNewTabFlyoutIcon(const winrt::hstring& icon);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutProfile(const Microsoft::Terminal::Settings::Model::Profile profile, int profileIndex, const winrt::hstring& iconPathOverride);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _CreateNewTabFlyoutAction(const winrt::hstring& actionId, const winrt::hstring& iconPathOverride);

        void _OpenNewTabDropdown();
        HRESULT _OpenNewTab(const Microsoft::Terminal::Settings::Model::INewContentArgs& newContentArgs);
        TerminalApp::Tab _CreateNewTabFromPane(std::shared_ptr<Pane> pane, uint32_t insertPosition = -1);
        // Agentmaster (the Agent Manager PAGE — see _managerPage): build this window's one Manager content,
        // wire it to the engine, and keep it OFF screen (lens hidden) until the Home button shows it. Once,
        // from _OnFirstLayout, before the side panel (which delegates sort/menu/selection to it).
        void _CreateAgentManagerPage();
        // Agentmaster: put the Agent Manager page on screen (idempotent — a second call while it is up is a
        // no-op, so a double-clicked Home never flickers). `reason` names the entry point for the [nav] log
        // line ("home", "last tab closed", ...). UI thread only; never throws (failures are logged).
        void _ShowManagerPage(std::wstring_view reason);
        // Agentmaster: take the page off screen because a TAB (`toTab`, named in the [nav] line) is about to
        // be shown in its place. Only flips the page state (flag, lens, Home paint); the caller MUST follow
        // with _UpdatedSelectedTab(toTab), whose content swap replaces the page in _tabContent and collapses
        // the full-window pages over it. No-op when the page is not up. UI thread; never throws.
        void _HideManagerPage(const TerminalApp::Tab& toTab);
        // Agentmaster: clear the tab strip's selection for the page, marking the re-entrant SelectionChanged
        // as ours (_managerPageDeselecting). Called by _ShowManagerPage and after any strip mutation the page
        // survives (MUX re-selects a neighbor when an item leaves a selection-less strip). UI thread only.
        void _DeselectTabStripForManagerPage();
        // Agentmaster: a caller is about to select `tab` while the page is up. When `tab` is ALREADY the
        // strip's selected item (MUX kept or re-acquired it under the page), selecting it again raises no
        // SelectionChanged, so this runs the switch the funnel would have run (hide the page, swap the tab
        // in, the Agentmaster selection effects). Otherwise a no-op: the caller's selection change reaches
        // _OnTabSelectionChanged, which leaves the page itself. Returns true when it switched. UI thread.
        bool _LeaveManagerPageOntoSelectedTab(const TerminalApp::Tab& tab);
        void _InitAgentmasterEngine(); // Agentmaster: start the SessionRegistry + hooks bridge
        std::wstring _SpawnClaudeSession(winrt::hstring workingDir, winrt::hstring title, uint32_t insertPosition = -1, winrt::hstring model = {}, const std::wstring& presetSessionId = {}); // Agentmaster (insertPosition: -1 == end/NewTabPosition default; a tab-context-menu spawn passes clickedIndex+1 so the new tab lands next to the clicked tab. model: the launch-model picker's per-LAUNCH `--model <id>` pick from an "Open New Session Here" submenu; "" = Default, the settings model. presetSessionId: MESSAGING.md §21.17 — the id the new session takes, minted by the caller (the Telegram bot's new-thread picker binds the user's thread to it before the tab exists); "" = mint one. Returns the new session's id, or "" when no tab opened — the claude.exe gate, a refused preset, a failed pane — which _RemoteLaunch turns into its answer, MESSAGING.md §21.23; every other caller ignores it)
        TerminalApp::Tab _LaunchClaudeSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromId = {}, uint32_t insertPosition = -1, const std::wstring& modelOverride = {}, const std::wstring& initialPrompt = {}, const std::wstring& inheritLaunchFromId = {}, const std::wstring& presetSessionId = {}); // Agentmaster (MESSAGING.md §21.17): presetSessionId = the id a FRESH launch takes (no `restored`, no fork) instead of a minted one — ignored by a resume, a restore and a fork. // Agentmaster (PRESETS): modelOverride is now a LAUNCH TARGET — "" (inherit / Default), a model id, or "@<presetId>" — resolved by _ResolveLaunchForSpawn; an empty target reuses the snapshot of the resumed session, else the fork source, else inheritLaunchFromId (the /handover origin), and the result is stored under the new session id (launchArgs) so every later relaunch keeps it // Agentmaster (returns the created tab; forkFromId set => fork that conversation into a new id; insertPosition threads tab placement, default -1 == end; modelOverride: launch-model picker — non-empty adds ` --model <id>` to THIS launch's commandline; initialPrompt: COMMANDS.md — non-empty rides the commandline as the positional prompt claude submits as the session's FIRST turn, the /handover successor's kickoff)
        void _HandleCommandHandover(const std::wstring& sessionId, const std::wstring& mdPayload, const std::wstring& commandArgs = {}, bool inPlace = false, bool standby = false); // Agentmaster (COMMANDS.md): a /handover-family await resolved — if THIS window hosts the origin session's tab, FAN OUT one successor PER collected markdown (mdPayload = the '|'-joined path set, SplitWatchPaths): each HANDOVER-*.md starts its OWN "<title> (handover N)" successor whose first user message is THAT file's content, via the per-file delivery tiers (commandline under the escaped budget; full-document bracketed-paste injection above it; NEVER truncated), tabs in write order right of the origin. commandArgs = the typed command's <command-args> verbatim — its LEADING words may name a §6b per-message successor-model hint ("/handover [fable] do x…", PickModelFromArgsHint), overriding the per-command Successor-model combo for THIS handover only. inPlace=false (/handover) spawns every successor as a new tab; inPlace=true (/handover-here) puts the FIRST file's successor INTO the origin tab via _RestartTabIntoFreshSession (additional files beside it), degrading to the new-tab spawn if the in-place swap is unavailable. standby=true (/handover-standby, §5b — mutually exclusive with inPlace) spawns new tabs like /handover but delivers NOTHING at launch: every file (content AND pointer alike) rides the pump's STANDBY lane, which TYPES it into the successor's input box without submitting (BuildPromptFill — one Enter away; the commandline tier is structurally excluded because a positional prompt auto-submits)
        std::wstring _RestartTabIntoFreshSession(const TerminalApp::Tab& originTab, const ::Agentmaster::SessionInfo& origin, const std::wstring& dir, const std::wstring& title, const std::wstring& initialPrompt, const std::wstring& modelOverride = {}); // Agentmaster (COMMANDS.md — /handover-here): REPLACE the origin session's live pane in place with a brand-new FRESH conversation — "Open New Session Here -> Default" spawn semantics (new minted id, the settings model unless the §6b per-command successor modelOverride is set, same dir) delivered through the Restart-session swap (_RestartManagedSession's recipe: HardResetWithoutErase + Connection swap + Start, scrollback kept); _BindClaudeSessionToTab's re-home block then archives the origin (live=false, injector cleared, resumable from Sessions) and re-keys the tab onto the successor. Returns the successor's session id ("" == refused/failed — the caller falls back to the new-tab spawn so the handover is never lost). UI thread only
        // Agentmaster (COMMANDS.md §5 — the over-budget /handover delivery): a successor spawned with a
        // document too large for the commandline carries it as a Pending queue prompt instead; this map
        // tracks {successor sessionId -> the queued prompt id + timing} and the PUMP (ticked by the
        // scanner's liveness probe) injects it as ONE bracketed paste (BuildPromptSubmission — the
        // stdin pipe has no CreateProcessW ceiling) once the session has STARTED (SessionInfo.started:
        // its ConPTY/claude actually launched; injecting earlier is silently dropped by a
        // pre-Connected WriteInput) plus a short settle. Delivered via the Send-now recipe (mark Sent
        // -> Inject -> roll back to Pending on failure), so the echo dedup + the scheduler's
        // Enter-retry watchdog back it exactly like any other sent prompt. Transient (not persisted):
        // after a restart the prompt is still Pending in the session's queue — visible in Auto
        // Testing, deliverable by Send-now — it just no longer auto-injects. UI thread only —
        // BOTH sweeps below marshal (fire_and_forget + resume_foreground, the _ScanPendingInput
        // idiom) since the scanner's liveness probe fires on the SCANNER thread while
        // _HandleCommandHandover writes these maps on the UI thread.
        //
        // The STANDBY lane (COMMANDS.md §5b — /handover-standby) rides the SAME map with a
        // different entry shape: promptId EMPTY + standbyText carrying the document. The pump then
        // FILLS instead of sends — Inject(BuildPromptFill(text)), the bracketed paste with NO
        // submit CR — and VERIFIES the draft actually landed by reading the session's input box
        // (ControlCore::ReadPendingInputDraft, the PENDING_INPUT.md primitive): a box still empty
        // after the verify window means the TUI ate the paste pre-raw-mode (the Enter-retry
        // gotcha's text-eaten sibling — there is no echo to watchdog a fill), so it re-fills, at
        // most kStandbyMaxAttempts times, NEVER while the box holds ANY text (a user draft
        // must never be appended to), and NEVER once the user has DRIVEN the session (the pure
        // StandbySessionTakenOver latch, both phases: a turn in flight now, or proof one ever
        // ran — a fast turn completed between ticks would otherwise read as an eaten paste and
        // re-fill the delivered briefing). Deliberately OUTSIDE the queue: a Pending row could be
        // auto-SENT by a Full autorunner and a Sent row would arm the Enter-retry watchdog —
        // either would defeat standby, whose whole contract is "nothing submits without the
        // user's Enter". Not restart-durable — the briefing FILE is the durable copy (standby
        // arms the §6b delete only AFTER a VERIFIED fill, so an undelivered draft always leaves
        // its file on disk).
        struct PendingHandoverInjection
        {
            std::wstring promptId; // the queued prompt carrying the full document ("" == a standby entry)
            int64_t armedMs{ 0 }; // when the successor spawned (drives the give-up deadline)
            int64_t startedSeenMs{ 0 }; // first tick that saw SessionInfo.started (inject after the settle delay)
            std::wstring standbyText; // STANDBY: the document to TYPE (non-empty == the standby lane)
            std::wstring standbyMdPath; // STANDBY: the briefing file — the §6b delete arms on a VERIFIED fill ("" == never, the pointer text names it)
            int64_t injectedAtMs{ 0 }; // STANDBY: when the last fill was injected (0 == not yet; drives the verify window)
            int32_t fillAttempts{ 0 }; // STANDBY: fills injected so far (capped at kStandbyMaxAttempts)
        };
        std::unordered_map<std::wstring, PendingHandoverInjection> _pendingHandoverInjections;
        winrt::Windows::Foundation::IAsyncAction _PumpHandoverInjectionsImpl(); // Agentmaster (terminate-net): the body, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _PumpHandoverInjections(); // ticked alongside _ScanPendingInput (TerminalPage.AgentObserver.cpp); self-marshals to the UI thread
        // Agentmaster (COMMANDS.md §6b — "delete the handover file after a successful hand-off"):
        // the opt-in delete is deferred until the successor has actually STARTED (its ConPTY/claude
        // launched — SessionInfo.started), not merely been created: a tab spawned in the background
        // starts lazily, and a launch that never comes up must LEAVE the briefing on disk so the
        // user can re-run. {successor sessionId -> the md path + arm time}; swept by
        // _SweepHandoverDeletes on the same liveness tick as the paste pump. Only the content +
        // paste tiers ever enter (the pointer tier's successor must READ the file), and a session
        // that dies/archives before starting drops WITHOUT deleting. IN-MEMORY only (not persisted),
        // so the wait spans a single app RUN — a successor un-started at exit is not re-armed next
        // launch and its briefing is KEPT (the safe direction); durable arming would be needed to
        // sweep across a restart. The sweep also drops every entry (keeping files) if the user turns
        // delete-after OFF mid-flight — the current setting wins, in the never-delete direction.
        struct PendingHandoverDelete
        {
            std::wstring mdPath;
            int64_t armedMs{ 0 };
        };
        std::unordered_map<std::wstring, PendingHandoverDelete> _pendingHandoverDeletes;
        winrt::Windows::Foundation::IAsyncAction _SweepHandoverDeletesImpl(); // Agentmaster (terminate-net): the body, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _SweepHandoverDeletes(); // ticked beside _PumpHandoverInjections (TerminalPage.AgentObserver.cpp); self-marshals to the UI thread
        // Agentmaster (PENDING_INPUT.md §10 — the restore RE-FILL): a REOPENED session whose record
        // carries a persisted unsent-draft MEMORY gets that draft TYPED BACK into its fresh claude's
        // input box once the tab actually starts — the /handover-standby channel verbatim
        // (Inject(BuildPromptFill) — a bracketed paste with NO submit CR — then VERIFY by reading the
        // box back via ReadPendingInputDraft, re-fill on an eaten paste, at most 2 attempts), so an
        // unsent message survives an app restart / close→resume as a REAL box draft instead of a
        // display-only memory that revalidation clears ~5s after the tab starts (claude never
        // restores its own input box). {sessionId -> the fill}. draftText is the FILL text — paste
        // placeholders already EXPANDED by the arm's off-thread cache resolve (claude re-collapses +
        // re-binds them at paste time); an unresolvable placeholder REFUSED the arm entirely (the §9
        // rule: re-typing a literal "[Pasted text #N]" label silently drops the content behind it on
        // submit). armedUnixMs (system clock, the resume-launch instant) is the HANDS-OFF baseline:
        // a prompt submitted at/after it means the user / the autorunner owns the session
        // (RestoredDraftSessionTakenOver — the resume twin of the standby latch, whose bare "!= 0"
        // test a resumed session's history would always trip). While an entry is armed,
        // _ScanPendingInput HOLDS that session's clear debounce (the freshly resumed box is empty
        // until the pump fills it — those first empty reads must not erase the very memory being
        // delivered) and drives the dots from the memory. UI-thread-only state: the arm
        // (fire_and_forget off _LaunchClaudeSession) and the pump both land on the dispatcher.
        // Gated by AppSettings::restoreDraftOnResume (cog, default ON); OFF = the classic
        // display-only memory. Not restart-durable ITSELF — the registry memory is the durable copy,
        // and the next resume re-arms from it.
        // Agentmaster (PENDING_INPUT.md §11 — the FORK CARRY shares this pump): WHY an entry is armed.
        // The two origins differ in exactly three ways — the setting that gates them
        // (restoreDraftOnResume vs carryDraftOnFork), the unresolved-placeholder policy (a restore across
        // a restart is STRICT: the journal is gone, so an unresolved placeholder is most likely a real
        // paste; a fork is LIVE: an unresolved MID-LINE placeholder is a typed quote, kept as text), and
        // the fill shape (a restore types ONE paste — its historical, proven recipe; a fork types the
        // PIECEWISE plan so the new box reads like the source box). Everything else — the started wait,
        // the settle, the hands-off latch, the scan hold, the deadlines — is shared.
        enum class DraftFillOrigin : uint8_t
        {
            Resume, // §10: a reopened session's own persisted draft memory, typed back
            Fork, // §11: the SOURCE session's live/remembered draft, copied into a brand-new fork
        };
        struct PendingDraftRestore
        {
            std::wstring draftText; // the fill (markers expanded); the registry memory keeps the rendered form. Fork: the whole expanded text (logs only — the fill is fillPastes)
            int64_t armedUnixMs{ 0 }; // system-clock arm instant — RestoredDraftSessionTakenOver's baseline
            int64_t startedSeenMs{ 0 }; // tick clock: first pump tick that saw SessionInfo.started (settle + started-deadline anchor)
            int64_t injectedAtMs{ 0 }; // tick clock: when the last fill was injected (0 == not yet; drives the verify window)
            int32_t fillAttempts{ 0 }; // fills injected so far (capped)
            DraftFillOrigin origin{ DraftFillOrigin::Resume }; // §11: which lane (gate / policy / fill shape — see the enum)
            std::wstring sourceId; // §11 Fork: the session the draft was copied FROM (diagnostic — every [draft-fork] line names it)
            std::vector<::Agentmaster::DraftFillPaste> fillPastes; // §11 Fork: the piecewise plan (BuildDraftFillPastes); empty == the Resume single fill of draftText
            bool deliveryInFlight{ false }; // §11 Fork: a _DeliverDraftFillPastes coroutine owns the box — the pump leaves the entry ENTIRELY alone until it hands it back
            // §11 Fork: how many LEADING pieces the last delivery SAW appear in the box. It separates the one
            // safe retry from the unsafe ones: an EMPTY box with 0 seen means claude ate the first paste
            // pre-raw-mode (re-type from piece 1 — the box is empty and the cursor at its start); an empty or
            // shortened box AFTER pieces were seen means the USER cleared/edited it — never re-type over that.
            size_t observedPieces{ 0 };
        };
        std::unordered_map<std::wstring, PendingDraftRestore> _pendingDraftRestores;
        // Agentmaster (PENDING_INPUT.md §10 + §11): resolve `draft`'s paste placeholders (journal first, the
        // paste-cache only when needed — OFF-THREAD whenever a placeholder is present), then arm the pump's
        // entry on the dispatcher, or REFUSE (logged) so a literal placeholder label is never re-typed.
        //   sessionId — the session whose box is filled (the reopened session, or the NEW fork).
        //   draft     — the RENDERED draft (the memory, or the fork source's box).
        //   journal   — the paste journal that backs `draft` (the session's own, or the fork SOURCE's).
        //   origin    — Resume (strict, single fill) or Fork (live, piecewise; also SEEDS the fork's memory +
        //               journals its blocks into the fork so the dots show and a later copy/restore resolves).
        //   sourceId  — Fork: the source session id (logs); "" for Resume.
        // Terminate-net: never throws (fire_and_forget); UI-thread state only touched on the dispatcher.
        winrt::fire_and_forget _ArmDraftRestore(std::wstring sessionId, std::wstring draft, std::vector<::Agentmaster::PasteJournalEntry> journal, DraftFillOrigin origin, std::wstring sourceId);
        // Agentmaster (PENDING_INPUT.md §11 — the FORK CARRY): what a fork copies from its source, captured
        // at FORK TIME (before the launch), so "the prompt in the box when I clicked Fork" is what arrives.
        struct ForkDraftCapture
        {
            std::wstring rendered; // the source's unsent draft AS RENDERED ("" == nothing to carry)
            bool fromLive{ false }; // true: read off the source's live box in THIS window; false: the observer's remembered draft (another window / dormant / closed source)
            std::vector<::Agentmaster::PasteJournalEntry> journal; // the SOURCE's paste journal — what its placeholders resolve against
        };
        // Snapshot a fork SOURCE's draft: the live box read (PickCurrentPromptText — live wins, else the
        // observer's remembered draft, which also covers a source hosted in another window or a closed
        // one's persisted memory) + a copy of its paste journal. Claude sources only (a Codex TUI has no
        // ❯ box); an unknown source / no draft / the setting OFF reads as nothing to carry. UI thread.
        //   sourceId — the session being forked.
        //   returns  — the capture ({} rendered == nothing to carry).
        //   throws   — nothing (the live read is wrapped; a failure reads as "no live draft").
        ForkDraftCapture _CaptureDraftForFork(const std::wstring& sourceId);
        // Hand a captured draft to the new fork: logs the intent and arms the Fork lane (_ArmDraftRestore).
        // A no-op for an empty capture / an empty fork id (the launch failed). UI thread.
        //   sourceId — the forked session (logs).
        //   forkId   — the NEW fork's session id (resolved from its tab).
        //   capture  — _CaptureDraftForFork's snapshot (moved in).
        //   throws   — nothing (arming is a terminate-netted fire_and_forget).
        void _CarryDraftIntoFork(const std::wstring& sourceId, const std::wstring& forkId, ForkDraftCapture capture);
        // Agentmaster (PENDING_INPUT.md §11): type a Fork entry's piecewise plan into its box, starting at
        // paste `fromIndex` — each piece a bracketed paste with NO submit CR, the next one sent only after
        // the box VISIBLY changed (claude's paste handler reads React props, so back-to-back pastes in one
        // input chunk could mint colliding paste ids). Owns the box for the duration exactly like the mail
        // clear: the §9 box-mutex (_draftSwapsInFlight), the delivery gate (kDeliveryGateFillTag — an
        // autorunner send declines instead of merging in), and a read-only lock (the user's keystrokes
        // can't interleave; handed back FIRST on every path). Hands the entry back to the pump with
        // injectedAtMs/fillAttempts stamped; the pump's verify phase judges the result. fire_and_forget
        // => the terminate-net wrapper; the Impl does the work (and unlocks on its own failure paths).
        winrt::fire_and_forget _DeliverDraftFillPastes(std::wstring sessionId, size_t fromIndex);
        winrt::Windows::Foundation::IAsyncAction _DeliverDraftFillPastesImpl(std::wstring sessionId, size_t fromIndex);
        // Agentmaster (PENDING_INPUT.md §2c — the paste journal's clipboard seam): which managed session's
        // control has this content id? The paste handler only knows the pasting ControlInteractivity's Id()
        // (== TermControl::ContentId()). Walks this window's _claudeTabs; "" for a shell tab / unknown id.
        // UI thread (reads controls).
        //   contentId — ControlInteractivity::Id() of the paste's sender.
        //   returns   — the session id, or "".
        //   throws    — nothing (a control torn down mid-walk reads as "no match").
        std::wstring _ClaudeSessionForContentId(uint64_t contentId);
        winrt::Windows::Foundation::IAsyncAction _PumpDraftRestoresImpl(); // Agentmaster (terminate-net): the body, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _PumpDraftRestores(); // ticked beside _PumpHandoverInjections (TerminalPage.AgentObserver.cpp); self-marshals to the UI thread
        winrt::fire_and_forget _RestoreClaudeSessions(); // Agentmaster: load persisted sessions as ARCHIVED (restorable) — does NOT auto-launch (Rule #6)
        void _RestoreWindowTabs(); // Agentmaster (M10 window-grouped restore): re-home THIS window's persisted tabs — resume each Claude session + replay each Other (shell) tab from its WindowRecord, in order. Only a claimed record (a reopened window) restores.
        // Agentmaster (the RESTORE WAKE — PERSISTENCE.md §14; RestoreWake.h is the pure half, TerminalPage.AgentWindowRecord.cpp
        // the glue): after a restore, start by themselves the restored tabs whose session was in the middle of something when
        // Agentmaster went down — once the tab every reopened window opens to has finished launching and the UI is idle.
        // _AnnounceRestoreWake — SetAgentmasterWindowId (at window creation, before any window lays out) enters this window
        //   in the process-wide barrier, so a window that finishes its restore first cannot wake its tabs while another
        //   window is still restoring. key = the Emperor-assigned record id. Never throws (a failure is logged; the window
        //   then joins at its restore instead).
        // _BeginRestoreWake — the end of _RestoreWindowTabs: join the barrier with the picked candidates (strip order) and
        //   the managed session the restore focused ("" = a shell / the Agent Manager page / none), then start the timer.
        //   Called even with no candidate: this window's focused tab still holds back the OTHER windows' wakes until it
        //   launched. Never throws; a failure leaves the barrier so no window waits on this one.
        // _TickRestoreWake — the timer: punctuality, the foreground gate (tells the barrier once), then the wake decision;
        //   in the WAKING phase it only drains the queue (_StepRestoreWakeQueue). Never throws; a failure ends the job (logged).
        // _ReadRestoreWakeForeground — what the window opens to right now, as the gate's facts: the restore-focused
        //   session's control while this window hosts it, else the current foreground (the Agent Manager page / no tab /
        //   no terminal read as "nothing to wait for"). Never throws: a failed read is logged and answers "nothing to wait
        //   for", so a broken read can delay no window.
        // _WakeRestoredBusyTabs — the barrier released this window: re-check every candidate (RestoreWakeWantsSession on the
        //   fresh record + hosted here), log who wakes and who is skipped and why, leave the barrier (the other windows no
        //   longer need this one), and enter the WAKING phase with the rest queued. Never throws.
        // _StepRestoreWakeQueue — one tick of the WAKING phase: skip the queued sessions that no longer want a wake (no slot
        //   spent), take a slot from the process-wide RestoreWakePacer (500 ms apart, at most 4 per 10 s across EVERY
        //   window) and wake ONE tab in place (_ActivateDormantSession, the "Activate Tab" path); log the total and end the
        //   job when the queue is empty. Never throws.
        // _EndRestoreWake — stop the timer, leave the barrier, drop the candidates and the queue; `why` non-empty is logged.
        //   UI thread (the timer). Never throws.
        // _LeaveRestoreWake — leave the barrier and forget the key (any thread: ~TerminalPage calls it from wherever the
        //   page dies); `why` non-empty is logged. Never throws — and `why` is a VIEW, so even building the argument (a
        //   literal at every call site, the destructor's included) cannot allocate and throw before the call.
        void _AnnounceRestoreWake(const std::wstring& key) noexcept;
        void _BeginRestoreWake(std::vector<std::wstring> candidates, const std::wstring& focusSessionId) noexcept;
        void _TickRestoreWake() noexcept;
        ::Agentmaster::RestoreWakeForeground _ReadRestoreWakeForeground() noexcept;
        void _WakeRestoredBusyTabs(::Agentmaster::RestoreWakeWakeReason reason) noexcept;
        void _StepRestoreWakeQueue(uint64_t nowTickMs) noexcept;
        void _EndRestoreWake(std::wstring_view why) noexcept;
        void _LeaveRestoreWake(std::wstring_view why) noexcept;
        void _AttachClaudeOverlay(const TerminalApp::Tab& tab, const std::wstring& sessionId); // Agentmaster: build + install the per-tab link badge (gated on AppSettings.showTabOverlay)
        int _JumpToPromptInSession(const std::wstring& sessionId, const std::vector<std::wstring>& msgs, int index); // Agentmaster (SUMMARY_JUMP.md): center the session tab's view on the i-th prompt; returns the row or -1
        std::vector<int> _JumpEligibilityInSession(const std::wstring& sessionId, const std::vector<std::wstring>& msgs); // Agentmaster (SUMMARY_JUMP.md): a row per prompt (-1 == not on screen) for icon dimming
        winrt::Microsoft::Terminal::Control::TermControl _ControlForSession(const std::wstring& sessionId); // Agentmaster (SUMMARY_JUMP.md): the live control hosting a session's tab, or null
        std::wstring _ReadLiveDraftForSession(const std::wstring& sessionId); // Agentmaster (PENDING_INPUT.md §8): the session's UNSENT input-box draft read from its LIVE buffer for "Copy Current Prompt" — fully wrapped: not hosted here / dormant / torn down / threw all read as "" (the caller then falls back to the observer's SessionInfo::pendingInput); UI thread
        // Agentmaster (PENDING_INPUT.md §9 — the DRAFT SWAP). _AcceptPromptSubmission is the
        // SessionRegistry::PromptSubmitter this window registers next to its injector; it is called on
        // whatever thread wants to send (the scheduler worker, or another window's Manager), so it does
        // nothing but the thread-safe HasInjector accept test before handing off to the coroutine below.
        // Everything the swap needs — the live box read, the read-only window, the injections — is UI
        // thread + this window, hence the split.
        bool _AcceptPromptSubmission(const ::Agentmaster::PromptSubmission& submission);
        winrt::fire_and_forget _SubmitPromptWithDraftSwap(::Agentmaster::PromptSubmission submission); // terminate-net wrapper (an escaped exception in a fire_and_forget is std::terminate)
        winrt::Windows::Foundation::IAsyncAction _SubmitPromptWithDraftSwapImpl(::Agentmaster::PromptSubmission submission); // read draft -> lock input -> Ctrl+U until VERIFIED empty (else abort) -> send -> await pickup -> Ctrl+Y (paste fallback) -> unlock
        std::wstring _ReadDraftForSwap(const std::wstring& sessionId, const std::wstring& remembered); // one verified box read: the live buffer, re-read on a thin/empty result, with the observer's remembered draft as the payload fallback
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> _RestoreDraftAfterSwap(std::wstring sessionId, std::wstring draft, bool allowPaste, bool viaStash); // put the draft back and VERIFY it against that ground truth: the TUI's own channel first (viaStash ? Ctrl+S un-stash, gated on a verified-EMPTY box since that key stashes otherwise : Ctrl+Y yank — both preserve a paste placeholder's binding), else clear + re-paste verbatim; returns what the box finally reads
        void _EndDraftSwap(const std::wstring& sessionId, bool restoreInteractive); // release the read-only window (only if WE took it) + drop the in-flight latch; UI thread, called on EVERY exit path
        // Agentmaster (DELIVERY.md §11 / DELIVERY_PLAN.md R4+R5 — VERIFIED PLACEMENT): one fresh
        // tri-state read of a session's input box (TermControl::ReadInputBoxProbe decoded —
        // {InputBoxState, draft text}), at a caller-chosen row window so a filled prompt taller than
        // the scan's 120 rows still reads back. Unknown + "" for a dormant/unhosted/throwing control.
        std::pair<int32_t, std::wstring> _ReadInputBoxProbeForSession(const std::wstring& sessionId, int32_t maxRows);
        // Agentmaster (PENDING_INPUT.md §9): the session input box's body height in VISUAL rows
        // (TermControl::ReadInputBoxBodyRows). > 1 means it still holds blank "" rows (leftover
        // newlines) the draft-clear ladders keep collapsing with Ctrl+U even after the draft text
        // reads empty (the detector trims trailing blanks). 0 for a dormant/unhosted/throwing control.
        int32_t _ReadInputBoxBodyRowsForSession(const std::wstring& sessionId);
        // The VERIFIED SEND core (R4): fill (bracketed paste, NO CR) -> settle -> read the box back ->
        // commit the lone CR only on a Verified/VerifiedCollapsed read. Eaten re-fills (<=2), Partial
        // clears-and-refills (it is OUR text), Foreign UNDOES the insertion (verified backspace
        // batches, budget == the folded prompt length so foreign text can never be eaten) + records
        // what remains as pendingInput and returns undelivered. Returns: 1 == delivered (CR fired);
        // 0 == not delivered, nothing irrevocable (caller rolls back); 2 == not delivered AND the
        // prompt was terminally FAILED here (verify strikes — caller must NOT roll back). Assumes the
        // caller holds the box (lock + _draftSwapsInFlight) and verified/knows it empty-ish.
        winrt::Windows::Foundation::IAsyncOperation<int32_t> _InjectPromptVerified(std::wstring sessionId, std::wstring sid8, std::wstring promptId, std::wstring text, std::wstring gateTag); // 0=roll back, 1=delivered, 2=Failed terminally (no rollback), 3=stale/ownership lost (no rollback — DELIVERY.md §12)
        // R4 verify-strike bookkeeping: promptId -> consecutive Foreign/unverifiable verdicts. Two
        // strikes mark the prompt Failed + pause the autorunner (a rollback would re-fire the same
        // doomed verify forever — the RC2 livelock shape, closed terminally + loudly instead).
        std::unordered_map<std::wstring, int32_t> _sendVerifyStrikes;
        // Agentmaster (DELIVERY_PLAN.md R8 — the VERIFIED PRESSER): the Enter-retry watchdog's lone
        // Enter routed through a LIVE box read at press time. Presses only into a verified-Empty box
        // or one holding the watched prompt; Foreign refreshes pendingInput, NoBox/MenuOpen refresh
        // pendingBoxState (an Enter into a menu SELECTS the highlighted option) — both refuse, logged.
        winrt::fire_and_forget _PressEnterForWatchedPrompt(std::wstring sessionId, std::wstring promptId, std::wstring promptText);
        // Sessions whose swap is running right now. Read by _ScanPendingInput, which SKIPS them: mid-swap
        // the box is deliberately empty, and letting the scan's clear debounce see that would erase the
        // very draft we are holding for the user. UI-thread-only state.
        std::unordered_map<std::wstring, bool> _draftSwapsInFlight; // sessionId -> "the control was ALREADY read-only before we took it" (so we never clear a read-only the user set)
        // Agentmaster (PENDING_INPUT.md §8d — the MAIL button's "move" mode): remove a session's unsent
        // input-box draft after it has been queued (the default mail click; Shift+Click keeps it). The
        // DRAFT SWAP's verified clear ladder + read-only lock STANDING ALONE — no send, no restore (the
        // draft is being intentionally removed, already safe in the queue). Shares _draftSwapsInFlight for
        // box mutual-exclusion with sends. Page-wired into the overlay (AgentTabOverlay::SetClearDraftHandler).
        winrt::fire_and_forget _ClearLiveDraftForSession(std::wstring sessionId); // terminate-net wrapper
        winrt::Windows::Foundation::IAsyncAction _ClearLiveDraftForSessionImpl(std::wstring sessionId); // resume-foreground -> resolve control -> decline if held -> lock -> DecideDraftClear until VERIFIED empty -> SetPendingInput("") -> unlock
        std::wstring _FocusedPromptNavSession(); // Agentmaster (alt+up/down): the focused tab's managed CLAUDE sessionId, or empty (=> the handler falls back to MoveFocus)
        winrt::fire_and_forget _ScrollAdjacentPrompt(std::wstring sessionId, bool up); // Agentmaster (alt+up/down): re-read the sent prompts (mtime-gated), then center the view on the nearest OFF-SCREEN sent prompt up/down (fresh resolve every press); boundary sound at the ends
        winrt::fire_and_forget _RefreshPromptNavCache(std::wstring sessionId); // Agentmaster (alt+up/down, SUMMARY_JUMP.md §7): the 30s focused refresh — re-read sent prompts (mtime-gated) into _promptNavCache + re-resolve the overlay's jump eligibility, WITHOUT navigating
        void _NavigateAdjacentPrompt(const std::wstring& sessionId, const std::vector<std::wstring>& prompts, bool up); // Agentmaster (alt+up/down): the UI-thread half of _ScrollAdjacentPrompt (resolve control + scroll, else sound)
        void _PlayPromptNavLimitSound(); // Agentmaster (alt+up/down): boundary feedback when there is no further off-screen prompt
        void _ToggleSummaryPanel(); // Agentmaster (TAB_OVERLAY.md): pencil button -> flip the GLOBAL AppSettings.showSummaryPanel (RMW settings.json) + apply live to every linked overlay in this window
        void _ToggleSummaryWrap(); // Agentmaster (TAB_OVERLAY.md): wrap-line toggle (panel times bar) -> flip the GLOBAL AppSettings.summaryPanelWrapNewlines (RMW settings.json) + apply live to every linked overlay in this window
        void _ToggleSummaryTruncate(); // Agentmaster (TAB_OVERLAY.md): truncate toggle (panel times bar) -> flip the GLOBAL AppSettings.summaryPanelTruncate (RMW settings.json) + apply live to every linked overlay in this window
        void _ToggleSummaryHistory(); // Agentmaster (the summary's conversation chain): ⏮ whole-conversation toggle (panel times bar) -> flip the GLOBAL AppSettings.summaryPanelShowHistory (RMW settings.json) + apply live to every linked overlay in this window and the side panel's
        void _SetTabActivityBadge(const TerminalApp::Tab& tab, const std::wstring& wtSession, const std::wstring& kind); // Agentmaster (OBSERVER.md §4/§11d): attach-or-update a registry-less "○ <kind> · unlinked" badge (pwsh / cmd / claude / codex) on a non-bound tab
        void _DropPendingOverlay(const std::wstring& wtSession); // Agentmaster: collapse + release this window's observe badge for a tab (bound / claude exited / tab gone)
        // Agentmaster (tab status dot): show/recolor (nullopt = hide) the tab-strip "[icon] ● <title>" dot via Tab.TabStatus();
        // dormant=true => the half-hollow "not started" variant; idempotent on unchanged color+presentation.
        // Accessibility (E2E_TESTING.md §6.3, 2026-10-23): the SAME call tells the tab who it hosts and in which state
        // (Tab::SetAgentAutomationSession: the TabViewItem's `am.tab.<key>` AutomationId and the first word of its
        // ItemStatus), so what UI Automation reads can never disagree with the dot. Every argument is explicit — no
        // default may silently clear the accessible facts.
        //   tab       - the tab; null is ignored.
        //   color     - the dot's color; nullopt hides the dot.
        //   dormant   - the session is live but its claude has not started (the half-hollow dot).
        //   sessionId - the session the tab hosts; "" for an observed-only tab or when the dot is cleared.
        //   stateWord - AgentStateWord(state) for a managed session, L"observed" for an observed tab, "" with no dot.
        // Never throws (CATCH_LOG — the dot is chrome).
        void _SetTabAgentDot(const TerminalApp::Tab& tab, const std::optional<winrt::Windows::UI::Color>& color, bool dormant, std::wstring_view sessionId, std::wstring_view stateWord);
        void _SetTabPending(const TerminalApp::Tab& tab, bool on, const std::optional<winrt::Windows::UI::Color>& dotsColor = std::nullopt); // Agentmaster (PENDING_INPUT.md): show/hide the unsent-draft "3 dots" pulse below a tab's status dot via Tab.TabStatus().AgentPendingVisible, painting them dotsColor (the contrast-picked pending color) when shown; idempotent (no-ops when visibility + color are unchanged); UI thread
        winrt::Windows::UI::Color _PendingDotsColorForTab(const TerminalApp::Tab& tab, const ::Agentmaster::SessionInfo& info); // Agentmaster (PENDING_INPUT.md): contrast-pick the "3 dots" color from the tab's CURRENT effective header background (selected/unselected aware) over the session's MODE-AWARE tab color (ResolveSessionColorHex — per-dir / individual / inferred); UI thread
        void _SetTabCacheWarm(const TerminalApp::Tab& tab, bool on, const std::optional<winrt::Windows::UI::Color>& sparkColor = std::nullopt); // Agentmaster (SPARK CROWN): show/hide the cache-warm glow + rising embers around a tab's status dot via Tab.TabStatus().AgentCacheWarmVisible, painting them sparkColor when shown; idempotent; UI thread
        winrt::Windows::UI::Color _CacheWarmSparkColorForTab(const TerminalApp::Tab& tab, const ::Agentmaster::SessionInfo& info); // Agentmaster (SPARK CROWN): contrast-pick the ember/glow FIRE color from the tab's CURRENT effective header background (the _PendingDotsColorForTab recipe, CacheWarmSparkColorFor palette); UI thread
        void _RefreshPendingDotsContrast(); // Agentmaster (PENDING_INPUT.md): re-pick the "3 dots" color for tabs currently showing a draft (no buffer read) so they re-contrast on a selected<->unselected shift; called from _OnTabSelectionChanged; UI thread
        void _UpdateTabAgentDot(const std::wstring& sessionId, ::Agentmaster::SessionState state, bool live, bool dormant); // Agentmaster (tab status dot): the registry-observer reaction — recolor (or hide, !live) the hosting tab's dot; dormant => the half-hollow "not started" variant; UI thread; no-op when this window doesn't host the session
        void _UpdateTabAgentToolTip(const TerminalApp::Tab& tab, const std::wstring& sessionId, bool swapWhileOpen = false, bool kickSummary = true); // Agentmaster (tab tooltip): build + host the rich session hover tooltip — a dark, summary-style card (● title · folder/branch header, state·age·why line, kind·model·perm line, then the session-end.js Summary box with NUMBERED messages + files, Cascadia Mono) — on a managed session's tab; clears it when the session is gone/archived; the card itself is the SHARED push (_PushSessionToolTipCard, key = the session id, the tab's resolved AgentTagsSpec as the tags, the flat 660px width); swapWhileOpen = the one-shot open-swap grant for the async summary-body arrival (Tab::SetAgentToolTip); kickSummary=false = host the header card WITHOUT kicking the off-thread transcript analyze (the arm/bind pre-hosts — the analyze belongs to a real hover, else a 60-tab restore would burst 60 whole-transcript parses); UI thread. LAZY: called on hover (_ArmTabAgentToolTipHover), bind, tag toggle, and the !live clear — never per tick.
        // Agentmaster (rich session tooltip): the SHARED body of every surface's card — the tab (_UpdateTabAgentToolTip)
        // and the side-panel card (_UpdateSidePanelCardToolTip): header strings, signature, height (80% of the window)
        // and width (flat 660, or the room beside the side panel) budgets, the closed-only push to `host`, the wheel-scroll
        // piece adoption under `surfaceKey`, and the throttled off-thread summary kick (registering `rehost` as a waiter of
        // the session's load). `s` must be LIVE (the caller handles gone/archived) and may be a lean copy (no queue/texts
        // read). Defined in TerminalPage.AgentObserver.cpp (beside the TU-local card builders); UI thread; throws what the
        // XAML build/push throws — every caller contains it.
        void _PushSessionToolTipCard(AgentSessionToolTipHost& host,
                                     const std::wstring& surfaceKey,
                                     const ::Agentmaster::SessionInfo& s,
                                     const std::wstring& tagsSpec,
                                     const std::vector<std::pair<std::wstring, winrt::Windows::UI::Color>>& tagChips,
                                     std::optional<double> roomWidth,
                                     bool swapWhileOpen,
                                     bool kickSummary,
                                     std::function<void()> rehost);
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
        std::unordered_map<std::wstring, _AgentTooltipSummary> _tabTooltipSummary; // keyed by SESSION id — a tab and a side-panel card of one session share it
        std::unordered_set<std::wstring> _tabTooltipSummaryInFlight; // keyed by SESSION id — one background load per session
        std::unordered_map<std::wstring, std::wstring> _tabTooltipSig; // keyed by SURFACE key (the session id for a tab's card; SidePanelToolTipKey(id) for a side-panel card) — each surface hosts its own card
        // Agentmaster (rich session tooltip): the surfaces waiting for a session's in-flight summary load — session id ->
        // surface key -> that surface's re-host (it passes kickSummary=false). Registered by _PushSessionToolTipCard whenever
        // a check is due (a repeat ask of one surface replaces its entry); taken and run by _EnsureTabTooltipSummary's
        // arrival. Without it, hovering a session's side-panel card while its tab's load ran (or the reverse) left the
        // second surface header-only: the in-flight guard dropped its ask. The lambdas hold the page / tab weakly.
        std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::function<void()>>> _tabTooltipSummaryWaiters;
        winrt::fire_and_forget _EnsureTabTooltipSummary(winrt::hstring sessionId, bool codex, winrt::hstring codexId, winrt::hstring forkParentId, winrt::hstring cwd); // Agentmaster (tab tooltip): off-thread resolve+stat+analyze the transcript (a fork with no messages yet: its fork source's, `forkParentId`, never cached as the path); on mtime growth render the Summary box + re-host every waiting surface (_tabTooltipSummaryWaiters); mtime-cached + in-flight-guarded. ⚠ Every re-host passes kickSummary=false — it must NEVER re-kick the analyze (an analyze outliving the 4s throttle would make every completion arm the next: the perpetual per-tab parse+rebuild loop that ground the release UI thread at ~80% of a core, 2026-08-29)
        // Agentmaster (tab tooltip — WHEEL SCROLL): a long conversation's Summary body is taller than the
        // card's height budget, so the body is a VIEWPORT the mouse wheel scrolls, with a slim overlay
        // scrollbar that fades out a couple of seconds after the last notch. The card itself stays inert +
        // hit-test-invisible (HANDOVER_tab-tooltip.md invariant 6a: no ScrollViewer, ever — its
        // DirectManipulation activation inside a ToolTip popup is the proven 0xC000027B fail-fast), so the
        // wheel is read on the TAB HEADER (Tab::EnsureAgentToolTipHoverHook) and scrolling is nothing but
        // a RenderTransform shift. These are WEAK element refs (a strong one would pin every superseded
        // card — the card is rebuilt on every content change) plus the three leaf transforms we drive.
        struct _AgentTooltipScroll
        {
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> viewport; // the clipping host (one screenful)
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> content; // the FULL body (its ActualHeight is the extent)
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> thumb; // the slim auto-hiding bar
            winrt::Windows::UI::Xaml::Media::TranslateTransform contentShift{ nullptr }; // scroll = shift the body up
            winrt::Windows::UI::Xaml::Media::ScaleTransform thumbScale{ nullptr }; // thumb LENGTH (visible share)
            winrt::Windows::UI::Xaml::Media::TranslateTransform thumbShift{ nullptr }; // thumb POSITION (scrolled fraction)
            double offset{ 0.0 }; // current scroll offset, px from the top of the body
        };
        std::unordered_map<std::wstring, _AgentTooltipScroll> _tabTooltipScroll; // keyed by SURFACE key (see _tabTooltipSig) — each surface's card has its own scroll pieces
        winrt::Windows::UI::Xaml::DispatcherTimer _tabTooltipScrollFadeTimer{ nullptr }; // ONE per window: only one tooltip is ever on screen
        std::wstring _tabTooltipScrollFadeSession; // whose bar the timer is currently holding/fading (a SURFACE key)
        bool _tabTooltipScrollFadeHolding{ false }; // phase 1 (hold after the last notch) vs phase 2 (step-down fade)
        double _SyncTabTooltipScrollBar(const std::wstring& surfaceKey, std::optional<double> offset, double opacity); // Agentmaster (tab tooltip WHEEL SCROLL): apply offset (nullopt = keep current) + re-fit/paint the slim bar of the card under `surfaceKey`; returns the max scrollable offset (0 = the body fits); RENDER-only writes (it also runs from the viewport's SizeChanged, where a layout write would be the popup layout-cycle fail-fast); UI thread
        bool _ScrollTabAgentToolTip(const TerminalApp::Tab& tab, int wheelDelta, bool fromTabItem); // Agentmaster (tab tooltip WHEEL SCROLL): a wheel notch over a tab whose rich card is OPEN — scroll the body (_ScrollSessionToolTipBody, key = the tab's CURRENT session — re-home safe); false = nothing to scroll here, leave the notch to the tab strip; sets _tooltipWheelClaimed for the row belt; UI thread
        bool _ScrollSessionToolTipBody(const std::wstring& surfaceKey, const std::wstring& sessionId, int wheelDelta, const wchar_t* src); // Agentmaster (rich session tooltip WHEEL SCROLL): the shared scroll body — move the OPEN card under `surfaceKey` one notch, log one "[tooltip-wheel] <sid8> src=<src> …" line, arm the bar's fade; true = this card took the notch (it has something to scroll), false = leave the wheel to the owner's scroller; UI thread
        void _WireTabStripTooltipWheel(); // Agentmaster (tab tooltip WHEEL SCROLL): wire (once per window) the BELT — a handledEventsToo PointerWheelChanged on the whole tab ROW that scrolls whichever tab's card is open, so a notch swallowed inside MUX's tab-strip internals still lands; UI thread
        bool _tabStripWheelWired{ false }; // the belt is wired once (retried while TabRow() isn't up yet)
        bool _tooltipWheelClaimed{ false }; // set by _ScrollTabAgentToolTip, consumed by the belt: the per-tab handler runs FIRST (deeper in the bubble), so this de-dupes one notch across the two handlers
        uint64_t _tooltipWheelLogTick{ 0 }; // throttle for the belt's one-line-per-gesture [tooltip-wheel] trace
        void _ResetTabAgentToolTipScroll(const std::wstring& surfaceKey); // Agentmaster (tab tooltip WHEEL SCROLL): a fresh hover starts at the top of the body of the card under `surfaceKey` (covers the unchanged-signature case, where the very same card element is re-shown); UI thread
        void _ArmTabTooltipScrollBarFade(const std::wstring& surfaceKey); // Agentmaster (tab tooltip WHEEL SCROLL): (re)start the scrollbar's auto-hide for the card under `surfaceKey` — every notch restarts the hold, so it fades only once you stop scrolling; UI thread
        void _OnTabTooltipScrollBarFadeTick(); // Agentmaster (tab tooltip WHEEL SCROLL): the two-phase auto-hide tick (hold, then a short step-down fade to invisible); UI thread
        // Agentmaster (tab status-dot RED FLASH): a hosted session that goes from Running to a resting
        // state (Idle / WaitingForInput / NeedsApproval — NOT Done or Error) on an UNVISITED tab blinks a
        // RED RING around that tab's status dot (a separate ellipse behind the dot, peeking out around
        // its constant black stroke; blinking in lockstep across tabs on one shared composition clock —
        // UI_THREAD.md §10i) until you switch to it. _EvaluateAgentFlash detects the Running ->
        // {Idle/Waiting/NeedsApproval} edge per registry update; _Set/_Stop/_Start/_Ensure show or hide the
        // per-tab ring (the header blinks it) and drive the shared 600ms bookkeeping timer. UI thread only.
        void _EvaluateAgentFlash(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState newState, bool live); // detect the Running -> {Idle/WaitingForInput/NeedsApproval} edge + start/stop the flash (never the active/visited tab; not for ->Done/->Error); forgets state on !live so a later restore re-tracks fresh
        void _StartAgentFlash(const std::wstring& sessionId); // begin blinking this session's tab-dot red RING; its header joins the shared composition blink clock in-phase with any others (UI_THREAD.md §10i)
        void _StopAgentFlash(const std::wstring& sessionId); // stop flashing + hide the red ring; stops the shared timer when none remain
        void _VisitTabClearFlash(const TerminalApp::Tab& tab); // visiting (selecting) a tab marks it seen -> stop its red flash (from _OnTabSelectionChanged)
        void _RefreshFocusedTabSummary(const TerminalApp::Tab& tab); // Agentmaster (TAB_OVERLAY.md summary panel): on a tab switch, kick a cheap mtime-gated content re-read of that tab's summary panel so a focused "here-and-now lens" is current instead of up to ~5s stale (from _OnTabSelectionChanged; no-op for a non-session tab / panel off / unchanged transcript)
        void _SyncOverlayFocusToTab(const TerminalApp::Tab& tab); // Agentmaster (SUMMARY_JUMP.md §4, perf): on a tab switch, mirror the selection onto every linked overlay so ONLY the focused panel runs the expensive periodic jump-eligibility resolve (from _OnTabSelectionChanged; a Manager/shell/null tab focuses none)
        void _EnsureAgentFlashTimer(); // lazily create + (re)start the shared 600ms flash timer — bookkeeping only since UI_THREAD.md §10i (the blink is the headers' composition clock, which starts a fresh burst on its "on" half)
        void _StopAgentFlashTimer(); // stop the shared flash timer (no flashing tabs remain)
        void _OnAgentFlashTick(); // shared-timer tick: re-assert every flashing tab's ring as SHOWN (idempotent — no XAML change in the steady state), prune sessions whose tab went away, re-publish the unread census; it no longer blinks anything (UI_THREAD.md §10i)
        void _ApplyAgentFlashRingForSession(const std::wstring& sessionId); // show one flashing session's red ring; its header blinks it on the shared composition clock, so it joins the current phase at once
        void _SetTabFlashRing(const TerminalApp::Tab& tab, bool on); // show/hide a tab's flash RING (the ellipse behind the dot) via Tab.TabStatus().AgentFlashRingVisible, painting it with the shared _flashRingBrush when shown; `on` = the ring FLASHES (the header blinks it — UI_THREAD.md §10i), so it changes only when a flash starts or ends; the dot's own black stroke + fill stay constant
        winrt::Windows::UI::Color _FlashRingColorFromSettings() const; // Agentmaster: parse AppSettings::flashRingColor ("#AARRGGBB", alpha = opacity) -> Color, falling back to opaque red on a malformed value
        void _EnsureFlashRingBrush(); // Agentmaster: lazily build the per-window shared flash-ring brush (_flashRingBrush) from the current setting, on the first flash
        void _RefreshFlashRingBrush(); // Agentmaster: re-point the shared flash-ring brush at the (possibly changed) AppSettings::flashRingColor — mutates its Color in place so every tab sharing it updates live (cog Save / cross-window broadcast)
        void _RefreshOverlayOpacities(); // Agentmaster (TAB_OVERLAY.md): push the (possibly changed) AppSettings::tabOverlayRest/HoverOpacity to every per-tab overlay (linked + observe) so the badge/summary dim<->bright opacities update live (cog Save / cross-window broadcast)
        void _MarkSessionUnread(const std::wstring& sessionId); // Agentmaster (Mark Unread): force the red ring on this session's tab until VISITED — even if it is the focused tab (no active-tab skip); sticky vs automatic state changes; ALSO sets the engine manualUnread + promotes Idle/Done -> WaitingForInput (board state, every window)
        void _ClearSessionUnread(const std::wstring& sessionId); // Agentmaster (Mark Unread): clear a manual unread mark + hide the ring if the automatic flash isn't also active (from _VisitTabClearFlash / archive); ALSO clears the engine manualUnread
        void _MarkSessionRead(const std::wstring& sessionId); // Agentmaster (Waiting-for-you "unread" model): stamp readUnixMs=now (quiet) so a past-timeout WaitingForInput card may decay to Idle; from _VisitTabClearFlash (a visit) + _EvaluateAgentFlash (the focused tab of the ACTIVE window) + _OnAgentWindowActivated
        void _PublishAgentUnreadSessions(); // Agentmaster (taskbar unread badge): hand this window's unread set (the union of _flashingSessions and _manualUnreadSessions) to the engine's census when it changed; called after every mutation of either set; UI thread
        void _ApplyTaskbarUnreadCount(); // Agentmaster (taskbar unread badge): re-read the census count, gate it by AppSettings::taskbarUnreadBadge, and raise TaskbarUnreadCountChanged if this window's number changed (the sink hop, engine init, the cog Save, the settings broadcast); UI thread
        void _ApplyTaskbarUnreadFrame(); // Agentmaster (taskbar preview frame, NOTIFICATIONS.md section 9): recompute THIS window's frame color from its own published unread set + AppSettings::taskbarUnreadThumbnailFrame + flashRingColor, and raise TaskbarUnreadFrameChanged if it changed (after every unread-set change, the cog Save, the settings broadcast); UI thread
        // Agentmaster (the GLOBAL PAUSE — DELIVERY.md §17): repaint everything THIS window shows of the
        // shared scheduler's CURRENT process-wide pause state — its "Pause / Resume Prompt Queue Runner"
        // toolbar button, and the mode button of every per-tab badge in _claudeOverlays, which reads
        // "<current> (Off globally)" while the pause is on (AgentTabOverlay::SetGlobalPauseShown). The
        // state is read fresh on every call instead of being carried by the caller, so two changes that
        // reach this window's UI thread out of order still end on the true state. Callers: the
        // global-pause observer's hop (engine init registers it) — a click in ANY window, this one
        // included, comes back through it. A no-op while the engine has no scheduler; a window with no
        // Manager page yet skips only the button (the page seeds it when it is wired,
        // _WireAgentManagerContent) and still repaints its badges. A badge attached LATER is not
        // reached by this call: _AttachClaudeOverlay seeds it.
        // Cost: one badge re-render per linked tab of this window, once per real pause change.
        // UI thread only (it touches XAML controls). Never throws: each button / badge is contained on
        // its own, a failure is logged through AgentLogCaughtException, and that one element keeps its
        // previous text.
        void _ApplyGlobalPauseShown() noexcept;
        void _OnAgentWindowActivated(bool activated); // Agentmaster (taskbar unread badge): the window came to the foreground — its focused tab is now being LOOKED AT, so stamp it read and drop its automatic flash (the flash ring's "current tab of the ACTIVE window is visited" rule; a manual Mark Unread stays sticky); from TerminalPage::WindowActivated; UI thread
        void _MoveSessionTriageState(const std::wstring& sessionId); // Agentmaster (Waiting-for-you + Error triage): the tab context-menu's status-adaptive move — demote a WaitingForInput session to Idle ("Move to Idle/Done"), DISMISS an Error one the same way (records the errorDismissed ack so the scanner's level-derived Error can't bounce it back), or plainly promote an Idle/Done one to WaitingForInput ("Move to Waiting-for-you"); direction re-derived from the live registry state. EXPLICITLY separate from _MarkSessionUnread (no sticky manualUnread, no ring flash)
        void _EvaluateAgentNotification(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState newState, bool live); // Agentmaster (System notifications): detect the Running -> X edge per registry update on a HOSTED tab and raise the Windows toast per the Notifications settings (per-state switches, focused-tab skip); tracks the Running-entry time for the "after <duration>" clause; HOLDS an Idle/Waiting toast while the external-work signal is live (spurious-completion suppression — SessionScanner's ShouldHoldCompletionToast) and DROPS the hold on a Running re-entry; forgets state on !live like _EvaluateAgentFlash; UI thread
        void _FireAgentCompletionToast(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState state, int64_t runningSinceMs, int64_t heldForMs); // Agentmaster (System notifications): the FIRE half shared by the immediate edge and the sweep's deferred fire — per-state cog switch, focused-tab skip, the kNotifyDuplicateToastMs double-toast guard, title/body build, the [notify] log (+ "held Ns" when deferred), the toast itself; UI thread
        winrt::fire_and_forget _SweepAgentPendingToasts(); // Agentmaster (System notifications): liveness-ticked sweep of _agentToastHeld — DecideHeldToast per held session (DROP re-lit / FIRE with the current state / KEEP); the _ScanPendingInput marshal + terminate-net shape
        winrt::Windows::Foundation::IAsyncAction _SweepAgentPendingToastsImpl();
        void _ShowAgentSessionToast(const std::wstring& sessionId, const std::wstring& title, const std::wstring& body, const std::wstring& detail, bool silent); // Agentmaster (System notifications): raise one Windows toast — line 1 = the session title, line 2 = the completion body, line 3 = `detail` (the quoted just-finished prompt; EMPTY omits the line entirely, giving the historical two-line toast); tagged per session (a newer toast replaces the older); clicking it while the app is alive brings the hosting window to the FRONT (restore + foreground) and jumps to the session's tab (local or via the activate fan-out). Best-effort: an unpackaged build (no AUMID) logs once and no-ops
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
        void _RefreshManagerBoardTags(); // tell THIS window's Manager content that a tag changed (AgentManagerContent::NoteTagsChanged) — a tag edit never touches the registry (no notify), so the Triage-Board cards' ribbons would otherwise stale until the next unrelated event. While the Manager page is hidden this only marks its lens dirty; while shown it rebuilds the board alone (since 2026-10-10; it was a full board + tree + plan rebuild)
        void _OpenTagEditorForTab(const TerminalApp::Tab& tab); // context-menu "Tags" (WT tab menu): open the panel for this tab's session, anchored under its TabViewItem
        void _OpenTagEditorForElement(const std::wstring& sessionId, const winrt::Windows::UI::Xaml::FrameworkElement& anchor); // the Sessions-row / Triage-Board-card / Explorer-tree-row "Tags" items: open for ANY session id, anchored under the clicked element
        void _OpenTagEditorAt(const std::wstring& sessionId, double x, double y); // the shared open: place + show at root-relative (x, y), deferred past the invoking flyout's close (its refocus must not fight the name box); re-randomizes the color pre-pick
        void _EnsureTagEditorPopup(); // lazily build the panel ONCE (card + [name box | +] row + color-picker swatches + hint + tag list) and parent it into Root(); wires Esc / Enter / outside-press dismissal
        // Agentmaster (launch-model picker -> "Specify..."): the type-any-model-id prompt, hosted in a
        // Root()-parented Popup (a ContentDialog text box gets no keypresses under XAML Islands). The
        // CARD comes from the shared AgentBuildSpecifyModelCard, so this and the Manager's prompt are
        // the same control. Serves the Sessions page's row menu + detail SplitButtons AND the WT tab
        // menu's "New Session Here"/"Fork session" submenus (Tab raises nothing — the page hands Tab
        // the opener at flyout-open). `onPicked` fires ONLY on commit; cancel launches nothing.
        //   onPicked - receives the committed, trimmed model id (never ""), on the UI thread.
        //   seed     - the id the box opens with; "" (every caller today) = the model last committed in
        //              this prompt in ANY window (model-list.json's "last" key), or an empty box if none.
        // Returns at once; the prompt opens on a later dispatcher tick, where a failure to build or show
        // it is logged ([exc] in hooks.log) and nothing opens.
        // Throws winrt::hresult_error when the window's dispatcher refuses the work (a closing window),
        // and std::bad_alloc.
        void _PromptForModel(std::function<void(winrt::hstring)> onPicked, const std::wstring& seed = {});
        // Close the "Specify a model" prompt (commit, Cancel, Escape, a backdrop press) and release its card. When the
        // prompt held the keyboard, give it back (TCM-K2): to _specifyModelReturnFocus, else to the current tab
        // (_ReturnKeyboardAfterPanel). A commit runs this BEFORE its launch, so a launch that opens its tab in front
        // takes the keyboard after it. Idempotent (a closed prompt moves no focus). UI thread only.
        // Throws what Popup::IsOpen / Child throw (winrt::hresult_error), as before; the focus step never throws.
        void _HideSpecifyModel();
        // Agentmaster (TCM-K2): give the keyboard back after a Root()-parented panel that held it closed (the Tags
        // panel, the "Specify a model" prompt). The panel's own close removed the focused element, and the focus
        // manager has moved the focus to whatever it found next (AgentPanelFocus.h, fact 3), so this puts it where it
        // was before the panel opened. Call it only when AgentPanelHoldsKeyboard said yes BEFORE the close: else a
        // press or a tab switch already gave the focus a new home, and this would take it away.
        //   target    - the Control that held the keyboard when the panel opened (AgentCaptureFocusReturn); when it is
        //               empty, gone, or cannot take the focus (it left the tree), the current tab gets the keyboard
        //               instead (_FocusCurrentTab(true): its terminal, or the Agent Manager page while that is up).
        //   panelName - what closed, for the one [focus] line this writes to hooks.log.
        // Synchronous on purpose: after an Escape key-up the next key must already find the terminal, and inside a
        // press handler the focus must be set before the press's own work (a menu that the press opens takes the
        // keyboard after this, a tab switch moves it to the new tab after this). UI thread only.
        // Exceptions: none (a failure is logged, and the keyboard stays where the focus manager put it).
        void _ReturnKeyboardAfterPanel(const winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::Control>& target, std::wstring_view panelName) noexcept;
        std::function<void(std::function<void(winrt::hstring)>)> _ModelSpecifyOpener(); // ONE opener, shared by every model submenu this page builds or feeds
        // Agentmaster (PRESETS, HARNESSES.md §8.2): the "Add presets…" opener every model submenu this page builds or
        // feeds passes to AgentFillModelPickItems (the Sessions page's row menu + detail split buttons, the WT tab
        // menu's submenus). Asked when the menu is built: it returns the opener ONLY while the user has no preset at all
        // (::Agentmaster::ShouldOfferAddPresets — the one shared question), else an empty function, so the item is left
        // out. The side panel's Launch hover menu asks the same question itself (AgentLaunchBar) and calls
        // _PromptAddPresets through its host.
        // Returns the opener (it reaches this page weakly), or an empty function. Throws std::bad_alloc only.
        std::function<void()> _AddPresetsOpener();
        // Show where presets are added: a buttons-only dialog through the window's dialog presenter, tagged
        // agentmaster-dark, with one OK button (the user's request) and the text AddPresetsHelpText(false) — which
        // starts with "open the Agent Manager (Home)", because every surface this page serves is outside it. Deduped by
        // _addPresetsHelpShowing (re-armed when the dialog closes, and in the catch). Never throws: the body is
        // contained, since an escape from a fire_and_forget would terminate the app.
        winrt::fire_and_forget _PromptAddPresets();
        void _RebuildTagEditorList(); // re-list the GLOBAL tag universe (CollectGlobalTags over sessions ∪ the tags.json registry — max session activity desc, 0-carrier known tags last) with this session's on/off state per row, a right-aligned ✕ delete on 0-carrier rows, + the "K of N tags" footer. Since 2026-10-10: one cached store scan + one lean registry read; while the store is COLD it shows "Loading tags…" instead and the read runs on a pool thread (_WarmTagStoreThenRefresh) — it never opens the store's files on the UI thread
        void _CommitTagEditorAdd(); // the "+" button / Enter: normalize the typed name; an existing tag just applies to this session, a NEW name is cap-gated (AppSettings::maxTags) + takes the picker's color (an explicit pick also recolors an existing tag); re-randomizes the pick after a successful add
        void _UpdateTagEditorAddState(); // TextChanged: show the "+" only when the box holds a usable name; disable + hint when a NEW name would exceed the cap
        void _SelectTagEditorColor(const std::wstring& hex, bool userPicked); // ring the matching swatch + paint the "+" with the pick; userPicked marks an EXPLICIT choice (the only kind allowed to recolor an existing tag)
        void _RandomizeTagEditorColor(); // roll a fresh random (non-explicit) pre-pick — on every panel open and after every added tag; always moves off the current pick
        void _ToggleSessionTag(const std::wstring& sessionId, const std::wstring& tag); // add/remove one tag on a session (SessionStore) + refresh its tab badges + re-render the panel list; an untag first registers the tag (tags.json) so losing its last carrier can't vanish it
        void _RemoveGlobalTag(const std::wstring& tag); // the tag-list row's ✕ (0-carrier rows only): explicitly delete the tag from the durable registry — the ONE tag-removal path (a carried tag stays alive via the derived union; the color entry is kept)
        void _CloseTagEditorPopup(); // dismiss the panel (the Escape key-UP / outside press / tab switch). It first ends the panel's tooltips: an open one closes, and one that still waits out its 3 s rest (kTagPanelTipDelay) is cancelled, so it cannot open on the next open of the panel. When the panel held the keyboard it gives it back (TCM-K2: _ReturnKeyboardAfterPanel to _tagEditorReturnFocus, else the current tab). Idempotent; the tooltip and focus steps never throw
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _specifyModelPopup{ nullptr }; // the "Specify a model..." prompt — built once, parented into Root(); its CHILD is rebuilt per open (fresh MRU + this invocation's callback)
        // Agentmaster (TCM-K2): the Control that held the keyboard when the "Specify a model" prompt opened (captured
        // right before the prompt focuses its box, AgentCaptureFocusReturn), where _HideSpecifyModel gives the keyboard
        // back. Empty = the current tab gets it (the prompt came from a tab's menu, or nothing usable held the focus).
        // Weak: the target may be closed or rebuilt while the prompt is up. Cleared by every close.
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::Control> _specifyModelReturnFocus;
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _tagEditorPopup{ nullptr }; // the Tags panel (bookmark tags) — built once, parented into Root()
        winrt::Windows::UI::Xaml::Controls::Border _tagEditorCard{ nullptr }; // the panel card (the outside-press dismissal's inside/outside boundary, and the root below which _CloseTagEditorPopup ends the panel's tooltips)
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
        // Agentmaster (TCM-K2): the Control that held the keyboard when the Tags panel opened (captured right before the
        // panel focuses its name box, AgentCaptureFocusReturn), where _CloseTagEditorPopup gives the keyboard back when
        // the panel held it. Empty = the current tab gets it: the panel came from a TAB's menu (GH#5750's rule — after
        // a tab menu the keyboard belongs to the terminal), or nothing usable held the focus. Weak: a board card can be
        // rebuilt, a Sessions row re-rendered, while the panel is up. Kept across an open while open; cleared by every close.
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::Control> _tagEditorReturnFocus;
        // Agentmaster (TCM-K2): an Escape key went DOWN inside the open panel, so its key-UP closes the panel. Closing on
        // the key-down handed the rest of the keystroke to whatever got the keyboard next, and once the keyboard goes back
        // to the terminal that is claude (an ESC interrupts a running turn). Set by the card's key-down hook, consumed by
        // its key-up hook, disarmed by any other key-down and by every open and close, so a key-up whose key-down
        // happened elsewhere (before the panel opened) never closes it.
        bool _tagEditorEscArmed{ false };
        std::vector<::Agentmaster::GlobalTagInfo> _tagEditorUniverse; // the global tag universe CACHED at open/toggle — the per-keystroke cap check must not re-scan the session-store dir
        // Agentmaster (tags perf, 2026-10-10): is _tagEditorUniverse the REAL universe? False while the
        // panel shows "Loading tags…" — the session store was cold when the list was last built, so it
        // is being read on a pool thread (_WarmTagStoreThenRefresh) — and the universe above is empty
        // only because it is unknown. While false the "+" stays hidden and Enter adds nothing
        // (_UpdateTagEditorAddState, _CommitTagEditorAdd): an add against an unknown universe would take
        // every name for new, so it would recolor an existing tag with the random pre-pick and could
        // pass the tag limit. Set by _RebuildTagEditorList on every build of the list.
        bool _tagEditorUniverseReady{ false };
        // Agentmaster (tags perf, 2026-10-10): the COLD-STORE load. A whole read of the session store
        // opens every record file the first time a process makes it (2,181 files on the release
        // profile). The Tags panel made that read on the UI thread at its first open — the freeze the
        // user reported, more than 5 s — and the tag hover panel at the first hover of a
        // ribbon. Both now ask ::Agentmaster::SessionStoreScanIsWarm() first; when it says no, they
        // show a "Loading" line and call _WarmTagStoreThenRefresh, which reads the store on a pool
        // thread and then calls _OnTagStoreWarm on the UI thread to fill whichever of them is still
        // open. The engine's start-up warm-up (Engine.cpp) normally makes the read first, so this path
        // is for a panel opened in the first seconds of a run.
        winrt::fire_and_forget _WarmTagStoreThenRefresh(); // read the store off the UI thread, then _OnTagStoreWarm; one pass at a time per window; holds no strong page reference across the hop; never throws
        void _OnTagStoreWarm(); // UI thread: the store was just read — rebuild the open Tags panel's list and refill an open, still-loading hover panel (in place); never throws
        std::shared_ptr<std::atomic<bool>> _tagStoreWarmInFlight{ std::make_shared<std::atomic<bool>>(false) }; // a _WarmTagStoreThenRefresh pass is running for this window. A shared atomic, not a plain member: the coroutine re-arms it on every exit, also from a pool thread after the window died, so it must not live in the page
        // Agentmaster (tags perf, 2026-10-10): the DEFERRED repaint of the other tag surfaces. A tag
        // edit in the Tags panel must also repaint the Sessions page (chips + the row), the Triage Board
        // (card ribbons + chips) and the side panel's cards. Those repaints used to run inline, BEFORE
        // the panel's own list was rebuilt, so a click on a tag row showed its check mark only after
        // the board and the Sessions table were done. Now the panel answers first and the rest follows
        // ~40 ms after the last edit; a burst of edits is repainted once.
        void _QueueTagSurfacesRefresh(const std::wstring& sessionId, bool recolored); // remember the edit (recolored = an existing tag's color changed, so every carrier must repaint) and (re)start the one-shot timer; never throws (falls back to repainting at once)
        void _RunTagSurfacesRefresh(); // the timer's tick: _SessionsNoteTagsChanged + _RefreshManagerBoardTags + _SidePanelNoteTagsChanged, each contained; never throws
        winrt::Windows::UI::Xaml::DispatcherTimer _tagSurfacesTimer{ nullptr }; // one-shot, restarted by every _QueueTagSurfacesRefresh; its tick stops it through the sender, so it stops even after the page died
        std::unordered_set<std::wstring> _tagSurfacesPendingIds; // the sessions whose tags changed since the last repaint (the Sessions page updates just their rows)
        bool _tagSurfacesPendingRecolor{ false }; // an existing tag's color changed since the last repaint (forces the Sessions page's full re-render)
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
        void _FillTagHoverPanel(); // Agentmaster (tags perf, 2026-10-10): the panel's CONTENT alone (header + one row per carrier of the pending tag), without placing it — _ShowTagHoverPanelNow calls it and then places the panel; _OnTagStoreWarm calls it to refill a panel that opened on a cold store, where it stands. Warm store: one cached scan + one lean registry read (HeadlinesOf) for all carriers. Cold store: the header says "loading…", _tagHoverLoading is set and the store is read on a pool thread
        void _CloseTagHoverPopup(); // dismiss (grace timer / a row jump / tab switch)
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _tagHoverPopup{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _tagHoverCard{ nullptr }; // the hover-keepalive boundary (enter cancels the grace close)
        winrt::Windows::UI::Xaml::Controls::StackPanel _tagHoverBody{ nullptr }; // header + session rows, rebuilt per show
        winrt::Windows::UI::Xaml::DispatcherTimer _tagHoverOpenTimer{ nullptr }; // one-shot ~160ms open delay
        winrt::Windows::UI::Xaml::DispatcherTimer _tagHoverCloseTimer{ nullptr }; // one-shot ~300ms grace close
        winrt::hstring _tagHoverPendingTag; // the tag to show when the open timer fires
        winrt::Windows::UI::Xaml::UIElement _tagHoverPendingAnchor{ nullptr }; // the hovered badge (transformed into Root() space for placement)
        bool _tagHoverLoading{ false }; // Agentmaster (tags perf, 2026-10-10): the open hover panel shows "loading…" instead of its rows — the store was cold when it was filled. _OnTagStoreWarm refills it only while this is set; _CloseTagHoverPopup clears it, so a load that lands after the close fills nothing
        void _UpdateManagerSelectionHighlight(); // Agentmaster (Linked Lenses): re-evaluate which tab (if any) wears the pill — the hovered-or-selected managed session, only while the Agent Manager page is on screen; called on lens change, hover, tab switch, and page show
        void _RevealTabInStrip(const winrt::TerminalApp::Tab& tab, bool leaveFullyVisibleInPlace = false); // Agentmaster (tab-strip reveal): THE way to scroll a tab into view — every tab selection (_UpdatedSelectedTab), an Activate of the already-selected tab, the Manager's lens + card clicks. Virtualization-aware: a derealized (scrolled-far-off) TabViewItem is realized via the TabListView's ScrollIntoView first, then fine-adjusted — a bare StartBringIntoView silently NO-OPS on it. leaveFullyVisibleInPlace (the selection funnel) leaves a tab that is already wholly on screen exactly where it is; false (the card clicks) always lands it inside the pads
        bool _AdjustStripToRevealItem(const Microsoft::UI::Xaml::Controls::TabViewItem& tvi, bool leaveFullyVisibleInPlace); // Agentmaster: the precise reveal pass — measures the header against the strip's scroll VIEWPORT and applies DecideTabStripRevealOffset (TabDragMath.h) with ONE instant ChangeView; returns false while the item has no realized layout (virtualized out / pre-arrange / strip not laid out) so the caller realizes it and retries, true otherwise (incl. "already placed, nothing to do")
        void _RevealTabRetryAdjust(Microsoft::UI::Xaml::Controls::TabViewItem tvi, int attempts, uint64_t generation); // Agentmaster: bounded Low-priority retries of the precise pass after ScrollIntoView — realization lands on a later layout pass, never this tick. `generation` is the _revealTabStripGeneration the chain belongs to; a newer reveal ends it
        void _TrackRenamingTabInStrip(const winrt::TerminalApp::Tab& tab); // Agentmaster (tab-strip reveal — the rename box): called on every Tab::RenamerOpenChanged. Box OPEN -> reveal the tab after the next layout pass and again after every size change while the box stays open (the Equal-width rename hold, a Fit-to-title tab growing with the text); box CLOSED -> stop following that tab. Needed because the strip ignores bring-into-view requests (_EnsureTabStripScrollViewer), which is what used to scroll a focused rename box into view. Never throws (CATCH_LOG)
        void _QueueRevealRenamingTab(const winrt::TerminalApp::Tab& tab); // Agentmaster (tab-strip reveal — the rename box): reveal `tab` (_RevealTabInStrip, leave-in-place) on the next Low-priority tick, i.e. after the layout pass that applied its new width; coalesced (one queued reveal at a time, aimed at the newest tab). Safe to call from SizeChanged, inside a layout pass. Never throws: a failure is logged and resets the coalescing flag
        int _TabHeaderStripVisibility(const winrt::TerminalApp::Tab& tab); // Agentmaster (tab-strip reveal): READ-ONLY — how much of this tab's header is inside the strip's scroll viewport right now: 0 = none (or not measurable: virtualized out / not loaded / strip not laid out), 1 = part of it, 2 = all of it. The same viewport-relative measurement _AdjustStripToRevealItem makes, without moving anything
        winrt::fire_and_forget _KeepTabHeaderInView(winrt::weak_ref<winrt::TerminalApp::Tab> weakTab, double stripOffsetBefore, double stripScrollableBefore, std::wstring why); // Agentmaster (tab-strip reveal): after an action that must leave the strip where it was (Deactivate), re-check twice (the next low-priority tick, then kStripKeepLateCheckMs later) that the tab is still WHOLLY on screen and reveal it in place if the strip moved it — a strip of hundreds of tabs estimates the width of headers it hasn't laid out, so a re-layout can leave the same offset on a different stretch of tabs. Logs [strip] with the before/after offset + scrollable width (the evidence of what moved). Weak refs only across the delay
        void _RevealClickedSessionTab(const std::wstring& sessionId); // Agentmaster (tab-strip reveal): a board card / tree row / side-panel card CLICK asks for its session's tab to be scrolled into view even when the selection did not change (a re-click of the selected card) — bypassing _SelectSession's same-id early-out and the _selectionBroughtIntoView change gate. Only while the Agent Manager page is on screen (off it a side-panel click is a PEEK) and only for a tab hosted in THIS window
        void _ActivateClaudeSession(winrt::hstring sessionId); // Agentmaster: jump to a session's tab — local first, then fan out to the hosting window (ActivateSessionInOtherWindows)
        bool _ActivateDormantSession(const std::wstring& sessionId); // Agentmaster (eager-init): start a DORMANT session's claude IN PLACE (no focus change) via TermControl::InitializeWithSize + SetStarted(true); returns true if it woke one (false: not hosted here / already started). UI thread.
        void _ActivateAllDormantTabsLocal(); // Agentmaster (eager-init): DRIP-FEED-wake every dormant managed tab hosted in THIS window — 500ms between wakes, 4 wakes per 10s batch — so a many-tab activate never bursts N claude.exe spawns onto one UI pass. Merges into a running drip. The receiving half of the activate-all fan-out.
        void _ActivateAllDripStep(); // Agentmaster (eager-init pacing): one drip step — pop queued ids until one actually wakes, then re-arm _activateAllTimer (500ms in-batch; the long post-batch gap after the 4th wake so batches start 10s apart); self-stops + logs the total when the queue drains
        void _SetActivateAllBusy(bool busy); // Agentmaster (eager-init pacing): tell THIS window's Manager content the drip is running (true at drip-start / false at drain) so it shows the "Activating N tabs…" busy state (disabled button + disabled cwd box)
        void _TrackSessionStarted(const std::wstring& sessionId); // Agentmaster (eager-init): mark SessionInfo::started true the moment this session's control initializes (already started => now; else one-shot on TermControl.Initialized) so a focused tab's dot flips full without the ~2s liveness-sweep lag. Tracks the tab's FIRST terminal control (_ControlForSession); use _TrackSessionStartedOn to name the control
        void _TrackSessionStartedOn(const std::wstring& sessionId, const winrt::Microsoft::Terminal::Control::TermControl& control); // Agentmaster: _TrackSessionStarted for an EXPLICIT control — Deactivate tracks the fresh control it just put in the session's own pane, which in a split need not be the tab's first one (tracking a running sibling shell would mark the parked session started at once)
        // Agentmaster (deactivate — the tab context menu's "Deactivate ▸", the inverse of "Activate
        // Tab"): put managed session tabs back in EXACTLY the state a window-restored tab is in before
        // anyone enters it. The session's pane gets a brand-new, NEVER-INITIALIZED terminal control built
        // by the restore path's own recipe (_MakePane(NewTerminalArgs{}, nullptr, <resume connection>))
        // — no buffer, no renderer, no GPU device, a connection that has never started — while the tab,
        // its title, color, tags, Auto-Testing queue and autorunner mode stay as they are. The old
        // control closes, and with it the old claude(/codex) (a graceful ConPTY close); the registry
        // retires the old connection as a host (RetireHostToken), so that dying process can no longer
        // stamp its token, pid or heartbeat back onto the parked record. Entering the tab lays the control
        // out, and its one-shot first-layout init starts the resume — the same wake every restored tab
        // gets; "Activate Tab" / "Activate All Tabs (N)" initialize it in place (InitializeWithSize).
        // All UI thread.
        void _DeactivateTabsFromMenu(const winrt::TerminalApp::Tab& anchorTab, int32_t scope); // scope: one of Tab's kDeactivateScope* codes — this tab · other tabs · all tabs · tabs to the left · tabs to the right (all five: this window) · of same folder · other of same folder (both: EVERY window — the tabs this window hosts are parked here, the rest are fanned out to their hosting window, DeactivateSessionInOtherWindows). An unknown code parks nothing and is logged. When the FOCUSED tab is among them, the Agent Manager page is shown FIRST, so the fresh control is never laid out on screen (laying it out would start it at once). The right-clicked tab stays in view (_KeepTabHeaderInView) — the 2026-10-01 strip jump
        bool _DeactivateSessionLocal(const std::wstring& sessionId); // Agentmaster (cross-window folder-deactivate): park the session's tab IN THIS WINDOW, the receiving half of the park-session sink. Runs the whole single-tab path (_DeactivateTabsFromMenu, this-tab scope), so THIS window's focused-tab rule applies: a session on screen here sends this window to its Agent Manager page first. Returns false when this window doesn't host the session's tab (the fan-out simply misses here). No confirmation, and a turn in progress is cut off
        // Agentmaster (session menu "Deactivate ▸" / "Close ▸" — the board card / tree row / side-panel card twins of the WT
        // tab menu's two submenus, AgentManagerContent::_MakeSessionScopeSubmenu): run ONE item of them for `sessionId`
        // exactly as that session's own tab menu would, in the window that HOSTS its tab — which need not be this one (the
        // board, the tree and the side panel can list every window's sessions). Local first (_RunSessionMenuScopeHere); a
        // tab that another window hosts gets the request through the session-action fan-out (Engine.h
        // RequestSessionActionInWindows, kSessionMenuDeactivateAction / kSessionMenuCloseAction, the scope code in decimal
        // as the argument), and that window runs the same _RunSessionMenuScopeHere. One case keeps the old "This Session"
        // cleanup instead: Close Tab of a session that no window can still host — its claude has exited and it is not a
        // dormant tab — archives the record in place (_ArchiveClaudeSession's dead branch), because no window would answer.
        //   sessionId — the menu's session; empty is a no-op.
        //   close     — false: "Deactivate ▸" (park; no confirm by design, a turn in flight is cut off); true: "Close ▸".
        //   scope     — Tab::kDeactivateScope* (0..6, the tab menu's own codes) or kSessionMenuScopeFavoriteAllTabs (Close
        //               only); an unknown code, or FavoriteAllTabs with Deactivate, does nothing but log.
        // Returns nothing: the outcome is in hooks.log ([nav] session-menu …, then the tab menu's own begin/done lines).
        // Throws what the tab-menu paths it calls throw on a torn-down tree (winrt::hresult_error) and std::bad_alloc — the
        // content's dispatcher callback contains and logs it. UI thread only.
        void _RunSessionMenuScope(const std::wstring& sessionId, bool close, int32_t scope);
        // Agentmaster (session menu): the hosting window's half of _RunSessionMenuScope — run the item on `sessionId`'s tab
        // IN THIS WINDOW through the very calls its tab menu makes: Deactivate -> _DeactivateTabsFromMenu(tab, scope);
        // Close -> _HandleCloseTabRequested (this tab) · _CloseTabsBefore · _RemoveTabs of the tabs right of it / of every
        // other tab · _CloseAllTabs(favoriteFirst) · _ConfirmAndCloseClaudeSessionsInFolder (the two folder scopes). A Close
        // request from ANOTHER window brings this window to the front first: its confirm shows HERE, and this window may
        // sit behind the one the user clicked in. A Deactivate asks nothing, so it leaves the windows' order alone.
        //   sessionId       — the session whose tab is the anchor of every scope.
        //   close           — the verb (see _RunSessionMenuScope).
        //   scope           — the scope code (see _RunSessionMenuScope); an unknown one does nothing but log, and still
        //                     returns true when the tab is here, so the caller asks no other window.
        //   fromOtherWindow — true when the session-action sink delivered it: logged as such, and the foreground rule.
        // Returns false, touching nothing, when this window does not host the session's tab; true otherwise.
        // Throws what the called tab-menu paths throw (see _RunSessionMenuScope). UI thread only.
        bool _RunSessionMenuScopeHere(const std::wstring& sessionId, bool close, int32_t scope, bool fromOtherWindow);
        // Agentmaster (session menu): "Close ▸ ★ Favorite & Close All Tabs" — the one session-menu scope with no Deactivate
        // twin and no Tab::kDeactivateScope* code, so it takes the next free number after them. AgentManagerContent's
        // SessionMenuScope::FavoriteAllTabs is static_asserted equal where the page wires the content.
        static constexpr int32_t kSessionMenuScopeFavoriteAllTabs{ 7 };
        // Agentmaster (session menu): the session-action words that carry a session-menu item to the window hosting the
        // session's tab (_RemoteSessionAction reads them); the argument is the scope code in decimal. Distinct from every
        // Telegram action word, so the bot can never send one by accident.
        static constexpr const wchar_t* kSessionMenuDeactivateAction{ L"session-menu-deactivate" };
        static constexpr const wchar_t* kSessionMenuCloseAction{ L"session-menu-close" };
        // Agentmaster (session menu): the hooks.log word of a session-menu scope — the tab menu's own words
        // (_DeactivateTabsFromMenu's "this" / "others" / "all" / "left" / "right" / "folder" / "folder-others"), so one grep
        // finds both menus' actions, plus "favorite-all" for Close's FavoriteAllTabs. It doubles as the validity check: the
        // two routers refuse a code it does not name rather than guess at a scope.
        //   scope — the code (Tab::kDeactivateScope* or kSessionMenuScopeFavoriteAllTabs).
        //   close — the verb: FavoriteAllTabs exists only under Close, so it has no word (nullptr) under Deactivate.
        // Returns a static string, or nullptr for a code no session-menu item of that verb raises. Never throws.
        static const wchar_t* _SessionMenuScopeWord(int32_t scope, bool close) noexcept;
        bool _DeactivateManagedTab(const winrt::TerminalApp::Tab& tab); // ONE tab -> dormant; false == skipped (not a managed session / adopted-external / already dormant / mid-delivery / no claude.exe / connection or pane build failed / the swap refused), each meaningful skip logged
        // Agentmaster (deactivate): resolve what parking `tab` would act on, WITHOUT acting or logging. On
        // true, `sessionId` / `info` / `pane` / `control` describe the session, its own leaf pane (the
        // connection whose WT_SESSION is the record's tabToken, else the tab's first terminal pane) and that
        // pane's current control. On false, `skipReason` says why (empty == not a managed live session at
        // all — nothing to say; non-empty == a reason worth logging). UI thread only. Never throws.
        bool _ResolveDeactivateTarget(const winrt::TerminalApp::Tab& tab,
                                      std::wstring& sessionId,
                                      std::optional<::Agentmaster::SessionInfo>& info,
                                      std::shared_ptr<Pane>& pane,
                                      winrt::Microsoft::Terminal::Control::TermControl& control,
                                      std::wstring& skipReason);
        void _WakeDeactivatedTab(const winrt::TerminalApp::Tab& tab); // the selection-funnel BELT: Start the parked connection of an INITIALIZED control sitting on a NotConnected connection. Deactivate no longer creates that shape (a parked tab now holds a never-initialized control that its first layout starts, like a restored tab), so this is a structural no-op today — a belt for any future path that leaves an initialized control parked on a never-started connection
        void _ScheduleSplashDismiss(); // Agentmaster (splash): start the deferred-dismiss watcher at the end of _OnFirstLayout — the restored tabs init LAZILY after, so dismissing there uncovers a blank window
        void _TickSplashDismiss(); // Agentmaster (splash): the watcher tick — dismiss the launch splash once the foreground terminal is connected AND the UI thread has been responsive ~1.5s (or a hard timeout)
        std::wstring _wid_NoThrow() const; // Agentmaster (splash/[startup]): the " [win <id>]" tag for log lines (empty until _windowId is set)
        bool _FocusClaudeSessionTab(const std::wstring& sessionId, bool bringWindowToFront); // Agentmaster (cross-window activate): select the session's tab IN THIS WINDOW (no fan-out); optionally foreground this window's HWND (the receiving half of the activate sink). Returns false on a miss.
        void _RestartClaudeSession(winrt::hstring sessionId); // Agentmaster (Triage Board / Explorer-tree "Restart session"): restart a managed session's connection in place — local first, then fan out to the hosting window (RestartSessionInOtherWindows), mirroring _ActivateClaudeSession
        bool _RestartClaudeSessionLocal(const std::wstring& sessionId, ::Agentmaster::RestartTurnChoice turnChoice = ::Agentmaster::RestartTurnChoice::Ask); // Agentmaster (cross-window restart): restart the session's tab IN THIS WINDOW via _restartPaneConnection (the NotConnected guard + _RestartManagedSession); the receiving half of the restart sink. turnChoice (HOOKS.md "Interrupted turns"): Ask = the tab menu's path (a turn the user interrupted is asked about HERE); a decided answer (another window asked, Telegram asked) restarts with it through _RestartPaneWithTurnChoice. Returns false when this window doesn't host the session's tab.
        // Agentmaster (MESSAGING.md §8.2 — TerminalPage.AgentMessaging.cpp): the receiving half of the Telegram bot's
        // window-hosted actions. Runs `action` on the session's tab when THIS window hosts it ("start" = start a
        // dormant tab's claude in place; "screen" = answer the terminal's visible text through `reply`; "markunread";
        // "rename" = `arg` is the new title, through _RenameClaudeSession (Rule #11); "refresh-tags" /
        // "refresh-favorite" = repaint after a remote store edit). Returns false, calling nothing, when this window
        // does not host the session (the fan-out's other windows). UI thread; never throws (Rule #18 inside).
        bool _RemoteSessionAction(const std::wstring& sessionId, const std::wstring& action, const std::wstring& arg, const std::function<void(std::wstring)>& reply);
        /// <summary>
        /// Agentmaster (MESSAGING.md §5.4 / D27 / §21.23): open a session the Telegram bot asked for — resume / fork / a new
        /// one — as a BACKGROUND tab of this window (no focus change: the user is at the phone, and the Sessions page's
        /// bulk-open latch _openClaudeTabInBackground is set around the call), and say what came of it. The seams run with
        /// _remoteLaunchInFlight set, so a refusal they would show as a dialog on the PC (no claude.exe, a refused preset) is
        /// recorded instead and becomes the answer.
        /// </summary>
        /// <param name="request">What the bot asked for (the engine routed it to this window).</param>
        /// <returns>The answer the bot is told (RemoteLaunch.h): Opened / AlreadyOpen with the session the tab hosts (and
        /// `fresh` when a new conversation opened in place of the asked one), else the refusal — NoFolder for a resume or a
        /// fork without its folder, ClaudeMissing, PresetRefused with the preset's words, Failed with what the PC knows.</returns>
        /// <remarks>
        /// UI thread. Never throws: a caught exception becomes a Failed answer (Rule #18 inside), because the sink hands
        /// this value to the bot, which waits for it. Pairs `[nav] remote launch …` with `[nav] remote launch done …`.
        /// </remarks>
        ::Agentmaster::RemoteLaunchOutcome _RemoteLaunch(const ::Agentmaster::RemoteLaunchRequest& request) noexcept;
        /// <summary>
        /// Agentmaster (MESSAGING.md §21.23): record why the remote launch in flight opened nothing, so _RemoteLaunch can
        /// answer with it — called by the launch seams at each refusal (the two prompts, and _LaunchClaudeSession's silent
        /// refusals). Only the FIRST refusal of a launch is kept: it is the cause, and what follows is its consequence.
        /// </summary>
        /// <param name="result">What refused (a RemoteLaunchResult other than Opened / AlreadyOpen).</param>
        /// <param name="detail">The PC's own words for it (plain text; "" when the result says it all).</param>
        /// <returns>True when a remote launch is in flight — the refusal was recorded, and the caller must NOT show its dialog
        /// (nobody may be at the PC); false otherwise (a launch the user started at the PC: the caller shows the dialog as
        /// before).</returns>
        /// <remarks>UI thread (the latch is UI-thread state). Never throws: a failed allocation keeps the dialog suppressed and
        /// the answer generic (Rule #18: logged).</remarks>
        bool _NoteRemoteLaunchRefusal(::Agentmaster::RemoteLaunchResult result, const std::wstring& detail) noexcept;
        /// <summary>
        /// Agentmaster (SESSIONS.md §9.14 — TerminalPage.AgentSessionsAi.cpp): the receiving half of the AI search's window
        /// and tab tools. Census: this window's strip (title, session, selected tab per tab). Resume / Fork: open the
        /// session as a tab HERE through the very seams a click runs (_ResumeSessionFromDisk; _ForkManagedSessionById for
        /// a session the registry knows, else _ForkSessionFromDisk), placed right after the requested tab (default: a
        /// fork after its source when the source is here, else the end of the strip), in the background unless the
        /// request asks for focus. Deactivate: park the session's tab when THIS window hosts it (the tab menu's
        /// Deactivate ▸ Deactivate Tab), answering handled == false otherwise.
        /// </summary>
        /// <param name="request">What the tool asked; the engine routed it here.</param>
        /// <returns>The outcome the model is told: what happened, the opened session and its position, or why not.</returns>
        /// <remarks>UI thread only (it reads and changes the strip). Never throws: every failure becomes an outcome with
        /// ok == false and a message (Rule #18 inside), because the caller waits for this answer.</remarks>
        ::Agentmaster::WindowToolOutcome _RunAgentWindowTool(const ::Agentmaster::WindowToolRequest& request) noexcept;
        // Agentmaster (MESSAGING.md §7.3 — /screen): the last `maxLines` lines of the session's terminal buffer as plain
        // text — what the owner would see on the screen — with trailing blank lines dropped. "" when this window does
        // not host the session, the tab is dormant, or the read threw (logged). UI thread.
        std::wstring _ReadScreenTextForSession(const std::wstring& sessionId, size_t maxLines);
        // Agentmaster (MESSAGING.md §21.24 — the Telegram card's 🖼 Screen Image): the session's live screen as a STYLED snapshot
        // (ScreenSnapshot.h: every cell's glyph, width and resolved colors, from TermControl::ReadScreenSnapshot), for the bot to
        // draw as a picture on a worker thread of its own. "" when this window does not host the session, the tab is dormant, or
        // the read threw (logged, Rule #18). UI thread; one viewport under the terminal's read lock.
        std::wstring _ReadScreenSnapshotForSession(const std::wstring& sessionId);
        void _ArchiveClaudeSession(winrt::hstring sessionId); // Agentmaster: archive (shut down + keep restorable) via the tab-close seam
        safe_void_coroutine _ConfirmAndCloseClaudeSessionsInFolder(winrt::hstring folder, winrt::hstring excludeId); // Agentmaster (the SINGLE folder-close confirm+close entry — Manager board/tree menu AND the WT tab-strip "Close ▸" submenu): show ONE confirm LISTING the tab titles, then _CloseClaudeSessionsInFolder. `excludeId` non-empty == "Other of Same Folder" (keep the clicked one open)
        void _CloseClaudeSessionsInFolder(winrt::hstring folder, winrt::hstring excludeId); // Agentmaster (board/tree "Close ▸ Of Same Folder" / "Other of Same Folder"): close every live managed session whose effective work dir == `folder`, SKIPPING `excludeId` when non-empty ("Other …" keeps the clicked one open) — local tabs directly (skip-confirm; _ConfirmAndClose… already showed the one confirm), remote ones fanned out via CloseSessionInOtherWindows
        bool _CloseClaudeSessionLocal(const std::wstring& sessionId); // Agentmaster (cross-window folder-close): close the session's tab IN THIS WINDOW (skip-confirm), the receiving half of the close-session sink. Returns false when this window doesn't host the session's tab.
        TerminalApp::Tab _RestoreArchivedSession(winrt::hstring sessionId, uint32_t insertPosition = static_cast<uint32_t>(-1)); // Agentmaster: re-launch (claude --resume / codex resume) an archived session — kind-aware; returns the created tab (null on gate/no-op) so the resume nav-END can log the actually-launched id. insertPosition: the 0-based strip slot for the tab (-1 = the end / Terminal's newTabPosition; the AI search's resume places it after a chosen tab, SESSIONS.md §9.14)
        void _AdoptExternalClaude(uint32_t pid, winrt::hstring cwd, bool fork); // Agentmaster (Fleet Observer): bring an EXTERNAL claude's conversation under management (fork==true => --fork-session into a NEW transcript [safe on a live external]; else --resume the same; fresh if none)
        // Agentmaster (PRESETS, HARNESSES.md §6.4): tell the user why a preset session did not start — no key,
        // no Base URL, or the picked preset was deleted. Buttons-only (XAML-Islands-safe), agentmaster-dark,
        // deduped by _presetLaunchErrorShowing so a burst of refusals shows one dialog. Never throws (the
        // body is contained; an escape from a fire_and_forget would terminate the app).
        //   message - the refusal reason (CompiledClaudeLaunch::error or the LaunchArgsForTarget error).
        winrt::fire_and_forget _PromptPresetLaunchError(std::wstring message);
        // Agentmaster (PRESETS): the snapshot a session relaunches with — its own stored launchArgs, else (for a
        // /clear successor, which runs in the SAME claude process as its predecessor) the predecessor's, walking
        // continuedFromId up to 8 hops. A /handover link is NOT followed: a successor launched on Default must
        // resume on Default, not inherit its origin's preset.
        //   sessionId - the session.
        // Returns the snapshot, or nullopt (the session launches on Default). UI thread or not: registry reads only.
        std::optional<::Agentmaster::LaunchArgs> _StoredLaunchArgsFollowingChain(const std::wstring& sessionId);
        // Agentmaster (PRESETS): the ONE launch-target decision every Claude launch seam makes. A non-empty
        // `token` (a menu pick: "@<presetId>" or a model id) takes a NEW snapshot; an empty one reuses the
        // snapshot of `inheritFromId` (the resumed / restarted / forked / handed-over-from session), if any.
        // Then it compiles for THIS launch (the Windows-tier values and the key are read now).
        //   token         - the menu pick ("" = none).
        //   inheritFromId - the session whose snapshot an empty token reuses ("" = none: Default).
        //   dir           - the launch directory (Per-directory env).
        //   settings      - the app settings.
        //   args          - receives the snapshot to store under the new session id (nullopt = Default).
        //   compiled      - receives the compiled launch (nullopt = the plain spawn).
        //   error         - receives the user-facing refusal reason.
        // Returns false when the launch must be REFUSED (the caller logs it, shows the notice, and spawns
        // nothing — a silent fallback would change the model and the account that pays for it).
        bool _ResolveLaunchForSpawn(const std::wstring& token, const std::wstring& inheritFromId, const std::wstring& dir, const ::Agentmaster::AppSettings& settings, std::optional<::Agentmaster::LaunchArgs>& args, std::optional<::Agentmaster::CompiledClaudeLaunch>& compiled, std::wstring& error);
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
        void _SyncManagerSelectionToTab(const TerminalApp::Tab& tab); // Agentmaster (Linked Lenses): on a tab switch, select that tab's managed session in the Manager lens, so opening the Manager page shows the session you were just in (no-op pre-startup / null tab / non-session tab)
        void _BringManagerSelectionIntoView(); // Agentmaster (Linked Lenses): on showing the Agent Manager page, scroll the selected board card / tree row into view — the selection followed tab switches while the page was hidden, so it can be off-screen (no-op pre-startup)
        // Agentmaster (the SLOW TAB SWITCH stopwatch — TabManagement.cpp _OnTabSelectionChanged): the
        // three lap instants _ApplyAgentTabSelection stamps, so the funnel's [tab-switch-slow] breakdown
        // keeps its exact steps now that the steps live in the helper.
        struct AgentTabSelectionLaps
        {
            std::chrono::steady_clock::time_point afterOverlayFocus{}; // lap 2 ends: lens visibility + tag popups + flash + deactivated wake + summary + overlay focus
            std::chrono::steady_clock::time_point afterManagerSync{}; // lap 3 ends: pending-dots contrast + the Manager selection sync (a full lens rebuild when the Manager is on screen)
            std::chrono::steady_clock::time_point afterManagerReveal{}; // lap 4 ends: selection pill + the Manager's bring-into-view
        };
        // Agentmaster: EVERY Agentmaster reaction to what is now on screen — the Manager lens's on-screen
        // flag, the tag popups, the red flash (a visit), the Deactivated-tab wake, the summary refresh, the
        // overlay focus gate, the pending dots, the Linked-Lenses selection sync, the pill, and the Manager's
        // bring-into-view. ONE body shared by the tab-switch funnel (_OnTabSelectionChanged), _RemoveTab's
        // close-successor selection (which the funnel never sees — its _removing guard), and the Agent
        // Manager page's show (_ShowManagerPage). `tab` is the newly selected tab, or NULL when the Agent
        // Manager page is what just came on screen (every tab-keyed step then no-ops and the page-only steps
        // — the pill, the bring-into-view — run); `laps` (nullable) receives the stopwatch lap instants. UI
        // thread only. Exceptions: whatever a step lets escape propagates — exactly the exposure these steps
        // had inline in the funnel (most catch + log their own failures); the _RemoveTab and _ShowManagerPage
        // call sites wrap it so a failure there can never abort their own bookkeeping.
        void _ApplyAgentTabSelection(const winrt::TerminalApp::Tab& tab, AgentTabSelectionLaps* laps);
        void _ApplyBroadcastSettings(const ::Agentmaster::AppSettings& settings); // Agentmaster (cross-window settings broadcast): adopt GLOBAL settings changed in another window (cog Save / sort toggle) — update _appSettings + hand the Manager content the new settings (repaints the sort toggles + re-sorts). Runs on this window's UI thread (the engine sink marshals here)
        // Agentmaster (RENDERING.md — "Software rendering"): switch the whole process to CPU (WARP) or
        // GPU drawing NOW. It sets the process-wide TermControl override, so terminals initialized
        // later pick it up themselves, then re-points every terminal THIS window has already
        // initialized (every pane of every tab). Each window re-points only its own: the cog's Save
        // handler calls this for the saving window, _ApplyBroadcastSettings for every other one.
        // Call it only when the value CHANGED, because the re-point repaints each terminal and, on a
        // real change, rebuilds its device + swap chain.
        //
        // Parameters:
        // - enabled: true = draw on the CPU (WARP); false = the GPU (unless the profile's own
        //   `rendering.software` is on).
        // - reason: a short tag for the hooks.log `[render]` line ("cog" / "broadcast").
        // Never throws: a terminal that fails to re-point is logged (Rule #18) and the loop moves on.
        void _ApplySoftwareRenderingSetting(bool enabled, std::wstring_view reason) noexcept;
        // Agentmaster (RENDERING.md §4c, "Hidden tabs"): publish the GLOBAL off-screen policy from
        // _appSettings (pauseHiddenTerminals + HiddenTerminalReleaseDelayMs) to the Control DLL, then
        // re-apply it to every terminal of THIS window (every pane of every tab), so a terminal that is
        // already hidden adopts a new delay — or resumes drawing when the pause was switched off — at
        // once. The cog's Save handler calls it for the saving window, _ApplyBroadcastSettings for every
        // other one, ONLY when the policy changed (each re-apply takes a terminal lock).
        //
        // Parameters:
        // - reason: a short tag for the hooks.log `[render]` line ("cog" / "broadcast").
        // Never throws: a terminal that fails to re-apply is logged (Rule #18) and the walk moves on.
        void _ApplyHiddenRenderingSetting(std::wstring_view reason) noexcept;
        // Agentmaster (RENDERING.md §4c): the human-readable policy for the `[render]` log lines, from
        // _appSettings ("pause ON, GPU release after 60 s hidden", ...). Returns the text; can throw
        // std::bad_alloc only (callers log inside a try).
        std::wstring _HiddenRenderingPolicyText() const;
        std::wstring _ClaudeSessionForConnection(const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& conn); // Agentmaster: which managed session is BOUND to this connection (by tabToken == WT_SESSION) — archive on pane-close + re-point injector on restartConnection
        winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection _BuildAgentConnection(const std::wstring& commandline, const std::wstring& dir, const std::wstring& title, const std::vector<std::pair<std::wstring, std::wstring>>& env, bool inheritCursor = false); // Agentmaster: the shared managed-agent ConPTY builder (commandline + cwd + child env + this window's AM_SESSION stamp) — behind _LaunchClaudeSession / _LaunchCodexSession (fresh pane => inheritCursor false) AND the in-place restart (reuses the pane buffer => inheritCursor true)
        std::wstring _ManagedSessionForConnection(const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& conn); // Agentmaster: which MANAGED session (Claude OR Codex) owns this connection, matched by connection IDENTITY across _claudeTabs (agent-agnostic; works for a Codex tab with no tabToken yet)
        bool _RestartManagedSession(const TerminalApp::TerminalPaneContent& paneContent, ::Agentmaster::RestartTurnChoice turnChoice = ::Agentmaster::RestartTurnChoice::KeepInterrupted); // Agentmaster: in-place "Restart session" for a managed Claude/Codex pane — rebuild the connection from the CURRENT conversation (resume, transcript/rollout-gated; never replay the launch commandline), swap it in, re-point the injector. turnChoice (HOOKS.md "Interrupted turns"): Uninterrupt takes back a user interrupt before the new claude starts (planned again here, held Start, _StartAfterInterruptRewind, CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 in its env) and clears a fork's stop intent; anything else restarts as before (and re-arms the fork stop watch for a fork whose user chose "Fork interrupted"). Returns true if handled; false => not a managed agent (fall through to the upstream restart).
        // Agentmaster (HOOKS.md "API-error turns" — the API-ERROR REWIND, ApiErrorRewind.h). Three seams resume a conversation
        // whose turn may have died on an API error; each appends the one rewind line BEFORE a claude reads the transcript, and
        // never while a claude still runs it (that claude's exit would re-stamp the error as the active leaf).
        //   _StartAfterApiErrorRewind — the RESTART half. _RestartManagedSession planned a due rewind, opened `holders` (the
        //       claude.exe processes running the conversation) BEFORE ending the old claude, and swapped `newConn` in WITHOUT
        //       starting it. Off the UI thread: wait for every holder to exit (at most kApiErrorRewindExitWaitMs), append the
        //       line; then on the UI thread start `newConn` — only while it is still NotConnected: a tab closed or restarted
        //       again meanwhile has CLOSED it (ConptyConnection::Close moves even a never-started connection to Closed), and a
        //       start then would spawn a claude no pane shows; a tab torn out into another window keeps it NotConnected and
        //       still starts there. A holder that never exits or a failed append still starts the connection, un-rewound.
        //       `sessionId` the conversation; `newConn` the held connection; `holders` may be null (nothing to wait for).
        //       fire_and_forget: every step is guarded, since an escaped exception here is std::terminate (Rule #18).
        winrt::fire_and_forget _StartAfterApiErrorRewind(std::wstring sessionId, winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection newConn, std::shared_ptr<::Agentmaster::ProcessExitWait> holders);
        //   _RewindApiErrorAfterExit — the DEACTIVATE half. The parked connection starts only when the tab is entered, so
        //       nothing is held: wait for the parked claude to exit off the UI thread, then append. A claude woken before the
        //       append is a live holder, and RewindApiErrorTail refuses rather than race it (logged; the next resume retries).
        //       `seam` names the caller in the log ("deactivate"). Never throws.
        winrt::fire_and_forget _RewindApiErrorAfterExit(std::wstring sessionId, std::shared_ptr<::Agentmaster::ProcessExitWait> holders, std::wstring seam);
        //   _RewindApiErrorBeforeRestore — the RESTORE half (_LaunchClaudeSession resuming a restored record: a window
        //       restore, a Sessions-page resume, a reopened closed session). No claude of this window runs the conversation,
        //       so the line is appended synchronously before the connection is built — and only for a record whose last
        //       known state says the turn was in flight or had failed (Error / midTurnAtRestore): a 600-tab window restore
        //       must not read 600 transcripts. Off with AppSettings::resumeApiErrorTurns. Logs every non-trivial outcome.
        void _RewindApiErrorBeforeRestore(const ::Agentmaster::SessionInfo& restored, const ::Agentmaster::AppSettings& settings);
        bool _ForkManagedSessionById(const std::wstring& sourceId, uint32_t insertPosition = -1, const std::wstring& modelOverride = {}, std::wstring* forkedIdOut = nullptr, ::Agentmaster::ForkTurnChoice forkTurn = ::Agentmaster::ForkTurnChoice::Uninterrupted); // Agentmaster (Triage Board / Explorer-tree "Fork session"): fork a managed session by id — the kind-aware fork shared with _DuplicateTab (Claude: `--resume <id> --fork-session`; Codex: `codex fork <rolloutUuid>`; both transcript/rollout-gated -> fresh). Opens the fork in THIS window (window-agnostic: reads the shared registry, no live tab needed). Returns false if the id isn't a known managed session. modelOverride: the launch-model picker's per-LAUNCH `--model <id>` pick from a "Fork session" submenu ("" = Default; Claude only — a codex fork takes no --model). forkedIdOut (optional): receives the NEW session's id ("" when no tab opened: the native-exe gate, a failed launch) — the AI search's fork tool reports it to the model (SESSIONS.md §9.14). forkTurn (HOOKS.md "Interrupted turns"): Interrupted arms the fork stop watch on the new session (a Claude fork only); interactive callers go through _ForkAskingAboutRunningTurn, which asks first

        // ===== INTERRUPTED TURNS (HOOKS.md "Interrupted turns"; AgentMaster/InterruptedTurns.h) — TerminalPage.AgentTurns.cpp =====
        // The user's request of 2026-10-22: a Restart of a turn the user interrupted asks whether to continue that turn (the
        // transcript gets one rewind line before the restarted claude starts), and a Fork of a session in the middle of a turn
        // asks whether the fork may continue that turn by itself or must be stopped (the fork's tab locked, ONE Esc pressed the
        // moment it starts running).

        // What the restart question shows (built by _UninterruptQuestionFor from PlanInterruptRewind + the session).
        struct UninterruptQuestion
        {
            std::wstring sessionId; // the session the question is about (a restart that finds another one on the pane does not apply the answer)
            std::wstring title; // its title (the dialog quotes it)
            bool duringToolUse{ false }; // the Esc came during a tool call
            bool resendsPrompt{ false }; // the rewind lands on the user's prompt (sent again), else on a finished tool result
            std::wstring promptText; // resendsPrompt: the prompt's first line
            ::Agentmaster::AutoContinueAtRestart autoContinue{ ::Agentmaster::AutoContinueAtRestart::On }; // how CLAUDE_CODE_RESUME_INTERRUPTED_TURN stands where the session runs
            std::wstring autoContinueSource; // where that value came from (the dialog names a settings file that turns it off)
        };
        // Should a restart of `sessionId` ask? Only for a managed CLAUDE session whose active branch ends in a user interrupt
        // that Claude Code would continue after a rewind (PlanInterruptRewindForSession == Rewind). A non-trivial "no" is
        // logged ([interrupt-rewind] … not offered). Reads the transcript's tail (one small read): an explicit-action path only.
        //   sessionId — the session.
        // Returns the question, or nullopt when there is nothing to ask. Never throws (a failure is logged and means "no").
        std::optional<UninterruptQuestion> _UninterruptQuestionFor(const std::wstring& sessionId);
        // _restartPaneConnection's question (the tab menu's "Restart session", the restartConnection action, a dead pane's
        // Enter): when the pane hosts a managed session with an interrupted turn, start the async question and return true —
        // its answer restarts THIS pane (_AskUninterruptThenRestart). false: restart as before.
        //   paneContent — the pane being restarted.
        // Returns true when the question is up. Never throws.
        bool _AskBeforeRestartIfInterrupted(const TerminalApp::TerminalPaneContent& paneContent);
        // Restart `paneContent` with a DECIDED turn choice: _restartPaneConnection's NotConnected guard, then
        // _RestartManagedSession(paneContent, turnChoice), and for a non-managed pane the upstream path (_restartPaneConnection).
        //   paneContent — the pane.
        //   turnChoice  — KeepInterrupted or Uninterrupt (Ask behaves like _restartPaneConnection).
        // Never throws past a logged catch.
        void _RestartPaneWithTurnChoice(const TerminalApp::TerminalPaneContent& paneContent, ::Agentmaster::RestartTurnChoice turnChoice);
        // The restart question's dialog (buttons only — the XAML Islands text-box rule does not apply), dark like every
        // Agentmaster dialog. Primary = "Continue the turn" (Uninterrupt), Secondary = "Keep it interrupted" (the DEFAULT:
        // a stray Enter must never make an agent run again), Close = Cancel (no restart).
        //   question — what to show.
        // Returns the dialog. Throws what XAML construction throws (callers are inside a try).
        winrt::Windows::UI::Xaml::Controls::ContentDialog _BuildUninterruptDialog(const UninterruptQuestion& question);
        // Ask, then restart the pane (the local path). The pane is held weakly: a tab closed while the dialog is up restarts
        // nothing. fire_and_forget with a terminate-net (Rule #18).
        //   weakContent — the pane.
        //   question    — what to ask.
        winrt::fire_and_forget _AskUninterruptThenRestart(winrt::weak_ref<TerminalApp::TerminalPaneContent> weakContent, UninterruptQuestion question);
        // Ask HERE (the window the user clicked in), then hand the restart to the window that hosts the tab with the answer
        // (RestartSessionInOtherWindows). fire_and_forget with a terminate-net.
        //   question — what to ask (its sessionId is the session restarted).
        winrt::fire_and_forget _AskUninterruptThenFanOut(UninterruptQuestion question);
        // The RESTART half of the uninterrupt (the _StartAfterApiErrorRewind recipe): off the UI thread wait for the old
        // claude(s) to exit (at most kApiErrorRewindExitWaitMs — its exit re-stamps its own leaf), append the rewind line
        // (RewindInterruptTailForSession re-plans on the file as it is then), log, and start `newConn` on the UI thread while
        // it is still NotConnected. A failed append still starts the connection, un-rewound. fire_and_forget, guarded.
        //   sessionId — the conversation; newConn — the held connection; holders — may be null (nothing to wait for).
        winrt::fire_and_forget _StartAfterInterruptRewind(std::wstring sessionId, winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection newConn, std::shared_ptr<::Agentmaster::ProcessExitWait> holders);

        // One interactive fork, captured so the question can be asked before it runs (the fork then runs exactly as the
        // call site would have run it).
        struct ForkLaunchRequest
        {
            std::wstring sourceId; // the session forked
            bool managed{ false }; // true: _ForkManagedSessionById (a session the registry knows); false: _ForkSessionFromDisk
            std::wstring dir; // _ForkSessionFromDisk: the working dir
            std::wstring title; // _ForkSessionFromDisk: the fork's base title ("" = derive one)
            std::wstring model; // the launch-model pick ("" = Default)
            uint32_t insertPosition{ static_cast<uint32_t>(-1) }; // the strip slot (-1 = the end)
            bool background{ false }; // _openClaudeTabInBackground for the open (the Sessions page's row menu)
            std::wstring origin; // the call site, for the [nav] line ("tab-menu", "board", "sessions-row", …)
        };
        // What the fork question shows (built by _ForkTurnQuestionFor).
        struct ForkTurnQuestion
        {
            std::wstring title; // the source's title
            bool resendsPrompt{ false }; // the fork would send the last prompt again
            std::wstring rerunTool; // a tool call in flight the fork re-runs ("" = none)
            bool sourceRunning{ false }; // the source runs right now (vs. a turn left unfinished on disk)
            std::wstring tailKind; // what the source's transcript ends in (the log)
        };
        // Should a fork of `sourceId` ask? Only for a Claude source with a transcript when CLAUDE_CODE_RESUME_INTERRUPTED_TURN
        // is on where the fork would run (ResolveClaudeAutoContinue) AND the fork would continue a turn by itself — the source's
        // tail continues (ClassifyResumeContinuationForSession) or the source is Running / NeedsApproval right now.
        //   sourceId — the session forked; dir — where the fork runs; title — its title for the dialog.
        // Returns the question, or nullopt. Never throws (a failure is logged and means "no").
        std::optional<ForkTurnQuestion> _ForkTurnQuestionFor(const std::wstring& sourceId, const std::wstring& dir, const std::wstring& title);
        // EVERY interactive fork goes through here (the tab menu, Ctrl+Shift+D, the board / tree / side-panel menus, the
        // Sessions page, the launch bar): no question => the fork runs at once, Uninterrupted (today's behavior); a question
        // => _AskForkTurnThenFork. Programmatic forks (the AI search's tool, Telegram — which asks with its own buttons) call
        // the fork seams directly with their answer.
        //   request — the fork as the call site would run it.
        // Never throws past a logged catch.
        void _ForkAskingAboutRunningTurn(ForkLaunchRequest request);
        // The fork question's dialog: Primary = "Fork interrupted" (the DEFAULT — two sessions running one turn side by side
        // is what a stray Enter must not cause), Secondary = "Fork uninterrupted", Close = Cancel (no fork). Then the fork
        // with the answer. fire_and_forget with a terminate-net.
        winrt::fire_and_forget _AskForkTurnThenFork(ForkLaunchRequest request, ForkTurnQuestion question);
        // Run `request` with `forkTurn` (Uninterrupted / Interrupted): _openClaudeTabInBackground set around the ONE open the
        // way the call site set it, then the matching fork seam.
        //   request — the fork; forkTurn — the answer.
        // Returns the NEW session's id ("" when no tab opened). Throws what the fork seams throw (callers are inside a try).
        std::wstring _LaunchForkRequest(const ForkLaunchRequest& request, ::Agentmaster::ForkTurnChoice forkTurn);

        // ---- the FORK STOP WATCH (InterruptedTurns.h DecideForkTurnStop) ----
        // One watched fork. Per-CONNECTION state resets when the tab's connection changes (a Restart / Deactivate gave it a
        // new one): the watch follows whatever claude the tab runs.
        struct ForkTurnStopWatch
        {
            std::wstring seam; // what armed it: "fork", "restore", "restart", "deactivate" (the log)
            winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection{ nullptr }; // the connection the state below belongs to
            winrt::weak_ref<winrt::Microsoft::Terminal::Control::TermControl> lockedControl; // the control this watch made read-only (released on every end)
            bool lockTaken{ false }; // we set the read-only (a read-only the user set is never cleared)
            int64_t armedTickMs{ 0 }; // GetTickCount64 at the arm
            int64_t startedAtMs{ 0 }; // first tick that saw the connection Connected (0 = dormant so far)
            uint32_t claudePid{ 0 }; // the fork's claude.exe under the connection's shell (0 = not found yet)
            int64_t claudeStartUnixMs{ 0 }; // its creation time (the heartbeat's PID-reuse guard)
            int64_t nextPidLookupMs{ 0 }; // the next process-tree lookup (throttled to kForkTurnStopPidLookupMs)
            int64_t idleBoxSinceMs{ 0 }; // DecideForkTurnStop's idle-with-the-box-up stretch
            int64_t lastBoxProbeMs{ 0 }; // the last input-box read (throttled: a buffer scan, not needed every 100 ms)
            bool lastBoxUp{ false }; // what that read found: the input box on screen (Empty / Draft)
            int32_t presses{ 0 }; // Esc presses on this connection
            int64_t lastPressMs{ 0 };
            std::wstring lastStatus; // the heartbeat status last logged (transitions only)
        };
        std::unordered_map<std::wstring, ForkTurnStopWatch> _forkTurnStopWatches; // fork session id -> its watch (UI thread only)
        winrt::Windows::UI::Xaml::DispatcherTimer _forkTurnStopTimer{ nullptr }; // fast (100 ms) while a watched fork runs, slow (1 s) while all are dormant, stopped with none
        int64_t _forkTurnStopTimerIntervalMs{ 0 }; // the interval _forkTurnStopTimer was last given (avoids re-setting an unchanged one)
        // Arm (or re-arm) the watch for `forkId`: the session must be a live Claude session hosted in THIS window; a watch that
        // already exists keeps its state. Starts the timer.
        //   forkId — the fork's session id; seam — what armed it (the log).
        // Never throws past a logged catch.
        void _ArmForkTurnStop(const std::wstring& forkId, const std::wstring& seam);
        // Re-tune the timer to the watches (fast / slow / stopped).
        void _UpdateForkTurnStopTimer();
        // One tick over every watch: gather the facts (the control, its connection, the fork's claude and its heartbeat, the
        // input box, the registry), DecideForkTurnStop, act (lock at start, press Esc, end). UI thread; never throws.
        void _ForkTurnStopTick();
        // End `forkId`'s watch: release the lock we took, re-anchor a waiting fork-carry fill (its box was ours), and decide
        // whether the persisted stop intent (SessionInfo::stopForkedTurn) stays for the next start (a resume of the fork would
        // still continue a turn) or is cleared. Logs the outcome.
        //   forkId — the watch; outcome — the final action; detail — log text.
        void _EndForkTurnStop(const std::wstring& forkId, ::Agentmaster::ForkTurnStopAction outcome, const std::wstring& detail);
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
        // Agentmaster (FAVORITES.md §4a — "closing a tab never wakes a dormant one"): would SELECTING this
        // tab start a process that is not running yet? True when any of its terminal panes still sits on a
        // never-started connection (ConnectionState::NotConnected) — a window-restored tab spawns its
        // claude.exe / shell lazily on its first layout, a Deactivated tab is woken by the selection funnel.
        // False for non-terminal tabs (Settings) and for started/ended panes. UI thread; never throws.
        bool _TabSelectionWouldStartProcess(const TerminalApp::Tab& tab);
        // Agentmaster (FAVORITES.md §4a): the tab _RemoveTab should focus after the FOCUSED tab closed —
        // upstream's pick (`preferred`) when selecting it starts nothing, else the most-recently-used
        // (MRU mode) / nearest (in-order) tab that starts nothing (the pure AgentMaster/TabCloseSuccessor.h
        // rule), so a close can never spawn another session's claude.exe as a side effect. Logs
        // [close-successor] when it overrides upstream. When EVERY remaining tab would start something it
        // returns `preferred`, and _RemoveTab shows the Agent Manager page instead of selecting it (the page
        // took over the no-wake harbor the pinned Manager tab used to be). UI thread; never throws (falls
        // back to `preferred`).
        winrt::TerminalApp::Tab _PickCloseSuccessorTab(const winrt::TerminalApp::Tab& preferred, uint32_t closedSlot, bool mruMode);
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
        void _SettleTabStripLayout(); // Agentmaster: commit the strip's item->container mapping synchronously after a TabItems() mutation (a belt; see TerminalPage.AgentEngine.cpp)
        // Agentmaster: the POINTER-OWNED tab reorder/tear-out gesture (the native-MUX-drag
        // replacement — the drag-AV resolution; implementation + rationale in
        // TerminalPage.AgentEngine.cpp, pure decision math in AgentMaster/TabDragMath.h):
        void _WireTabReorderGesture(); // AddHandler(handledEventsToo) the pointer events on the TabView, once from Create()
        void _TabReorderOnPressed(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // arm on a left-press over a draggable tab header
        void _TabReorderOnMoved(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // threshold -> activate; active -> update caret/auto-scroll
        void _TabReorderOnReleased(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // commit (reorder / tear-out) or disarm a plain click
        void _TabReorderOnCaptureLost(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // external capture loss cancels (our own release is guarded by the reset-first order)
        void _TabReorderActivate(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // capture the pointer + lift the tab + arm Esc/auto-scroll
        void _TabReorderUpdate(const winrt::Windows::Foundation::Point& pos); // decide slot + caret + edge auto-scroll (shared by Moved + the timer tick)
        void _TabReorderCommitRelease(const winrt::Windows::Foundation::Point& pos); // classify the release: reorder via _TryMoveTab, or tear out via _sendDraggedTabToWindow
        void _TabReorderCancel(const wchar_t* reason); // end without any mutation (Esc / capture loss / tab vanished)
        void _TabReorderReset(); // common teardown: flags first (re-entrancy), timer/Esc off, opacity+caret restored, captures released
        int _TabReorderIndexOf(const winrt::TerminalApp::Tab& tab) const; // the tab's CURRENT _tabs index, -1 when gone (closed under the gesture)
        std::vector<::Agentmaster::TabDragBand> _TabReorderBands(); // realized, on-strip header bands (TabView coords) — virtualized-out tabs simply have no band
        void _EnsureTabDragIndicator(); // lazily create the insertion caret in Root()
        void _PositionTabDragIndicator(double xInTabView); // place + show the caret at a TabView-x boundary
        void _HideTabDragIndicator();
        ::Agentmaster::DockProbeResult _DockProbeHit(long screenX, long screenY, ::Agentmaster::DockProbeMode mode); // Agentmaster (cross-window docking — the TARGET side): screen point -> strip-row hit + insertion slot over THIS window's realized bands (Preview also lights/hides its own caret); {-1, 0} on a miss
        void _UpdateDockPreview(); // Agentmaster (cross-window docking — the SOURCE side, per move/tick while in the tear-out zone): light the insertion caret on the Agentmaster window under the cursor, tracking target changes
        void _ClearDockPreview(); // Agentmaster (cross-window docking): hide the last previewed target's caret (pointer left it / gesture ended)
        void _ApplyTabDragPolicy(const Microsoft::UI::Xaml::Controls::TabViewItem& tabViewItem); // Agentmaster: settle the strip + pin the tab CanDrag(false) — native MUX drag is permanently OFF (the drag-AV resolution; the pointer-owned gesture replaces it)
        std::wstring _DescribeTabForLog(const TerminalApp::Tab& tab); // Agentmaster: `<sid8> "<title>"` (title-only for a shell tab) — the tab-strip forensic log identity; never throws
        winrt::Windows::Foundation::IAsyncAction _AdoptExternalSessionImpl(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken); // Agentmaster (terminate-net): the body of _AdoptExternalSession, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _AdoptExternalSession(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken); // Agentmaster: bind a hand-typed `claude` to its ConPTY
        winrt::Windows::Foundation::IAsyncAction _SweepClaudeLivenessImpl(); // Agentmaster (terminate-net): the body of _SweepClaudeLiveness, awaited inside its try/catch so a throw can't escape the fire_and_forget (std::terminate)
        winrt::fire_and_forget _SweepClaudeLiveness(); // Agentmaster: archive this window's claude tabs whose ConPTY has Closed (scanner-ticked)
        winrt::Windows::Foundation::IAsyncAction _ReconcileClaudeTabsImpl(); // Agentmaster (terminate-net): the body of _ReconcileClaudeTabs, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ReconcileClaudeTabs(); // Agentmaster: poll backstop — bind/attach + re-home claude tabs by stable WT_SESSION (scanner-ticked)
        winrt::Windows::Foundation::IAsyncAction _ObserverProbeImpl(); // Agentmaster (terminate-net): the body of _ObserverProbe, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ObserverProbe(); // Agentmaster: the Fleet Observer UI lane — publish this window's tab roster, then bind via the observer's correlation table (replaces _DiscoverClaudeTabsByCwd; OBSERVER.md §10)
        winrt::Windows::Foundation::IAsyncAction _ScanPendingInputImpl(); // Agentmaster (terminate-net): the body of _ScanPendingInput, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ScanPendingInput(); // Agentmaster (PENDING_INPUT.md): read each bound Claude tab's unsent input-box draft from its buffer + record it on the session (scanner-ticked)
        winrt::fire_and_forget _ResolvePendingPasteRefsFor(std::wstring sessionId, std::wstring draft); // Agentmaster (PENDING_INPUT.md §2b): resolve the draft's "[Pasted text …]" placeholders to their paste-cache file OFF-THREAD (content-anchored, arithmetic-verified) and record the annotation quietly; detached — captures the shared registry by value, never touches UI state after the hop
        int64_t _pendingDraftSaveMs{ 0 }; // Agentmaster (PENDING_INPUT.md §5): last direct sessions.json save for a QUIET draft text drift (a text-only edit raises no notify → no autosave; a ~10s-throttled direct save keeps the persisted draft memory fresh). UI thread only
        winrt::fire_and_forget _PersistSessionDraft(std::wstring sessionId, std::wstring draft); // Agentmaster (PENDING_INPUT.md §8c): upsert (or CLEAR, when `draft` is empty) the session's unsent draft in the DURABLE per-session store, paste-EXPANDED where the cache resolves it. Detached + off-thread (the expansion reads the paste cache); re-reads the registry as the ordering guard so a late write can't resurrect a sent draft
        std::unordered_map<std::wstring, int64_t> _pendingDraftStoreMs; // Agentmaster (PENDING_INPUT.md §8c): per-session throttle for the store UPSERT (~10s, like the sessions.json drift save) so one busy session can't starve another's; a CLEAR bypasses it. UI thread only
        winrt::Windows::Foundation::IAsyncAction _ScanCacheWarmTabsImpl(); // Agentmaster (terminate-net): the body of _ScanCacheWarmTabs, awaited inside its try/catch (see _SweepClaudeLivenessImpl)
        winrt::fire_and_forget _ScanCacheWarmTabs(); // Agentmaster (SPARK CROWN): re-evaluate ServerCacheStillWarm for each hosted session and drive its tab-strip glow + embers (scanner-ticked — warmth lapses on a CLOCK, so no event ever pushes its end)
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
        // Agentmaster (discard Manager-only windows): a window that ends up holding no tab at all — just
        // the Agent Manager page — must not persist as a restorable window, and unless it is the LAST
        // Agentmaster window it self-closes (every window has a Manager page, so an empty one is noise).
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
        std::wstring _ResumeSessionFromDisk(const std::wstring& sessionId, const std::wstring& dir, const std::wstring& title, uint32_t insertPosition = static_cast<uint32_t>(-1)); // resume ANY on-disk session into a managed tab: live here -> Jump; archived -> Restore; unknown -> minimal record + the transcript-gated resume seam. insertPosition: the 0-based strip slot (-1 = the end / Terminal's newTabPosition). Returns the session whose tab is open now — the target, or the FRESH id a resume without a transcript degraded to (Rule #6) — or "" when nothing opened (a gate, a failed launch, missing arguments); callers that do not need it ignore it (SESSIONS.md §9.14's resume tool reports it)
        std::wstring _ForkSessionFromDisk(const std::wstring& parentId, const std::wstring& dir, const std::wstring& title, const std::wstring& modelOverride = {}, uint32_t insertPosition = static_cast<uint32_t>(-1), ::Agentmaster::ForkTurnChoice forkTurn = ::Agentmaster::ForkTurnChoice::Uninterrupted); // fork ANY on-disk session into a NEW managed conversation (`--resume <parent> --fork-session --session-id <new>`) — the parent transcript is untouched, so it is safe even while the parent is LIVE; transcript-gated (no transcript -> fresh), the duplicate-tab fork's convention ("<title> (fork)"). modelOverride: the launch-model picker's per-LAUNCH `--model <id>` pick from a "Fork here" submenu ("" = Default). insertPosition: the 0-based strip slot (-1 = the end / Terminal's newTabPosition). forkTurn (HOOKS.md "Interrupted turns"): Interrupted arms the fork stop watch on the new session; interactive callers go through _ForkAskingAboutRunningTurn. Returns the NEW session's id, or "" when no tab opened
        winrt::fire_and_forget _PromptResumeOrForkSession(std::wstring sessionId, std::wstring dir, std::wstring title, std::wstring forkTitle); // Sessions-page double-click on a NOT-live row: a Resume / Fork / Cancel ContentDialog (the adopt dialog's idiom) instead of resuming silently. Primary=Resume (claude --resume), Secondary=Fork (--fork-session, parent untouched), Close=Cancel; forkTitle is empty for a never-prompted row so the fork seam derives a smart name
        void _UpdateSessionsSelectionHighlight(); // recolor row highlights for _sessionsSelectedId WITHOUT a rebuild (row-tap + keyboard nav)
        void _MoveSessionsSelection(int delta); // Up/Down keyboard nav over _sessionsVisibleOrder: none selected => Down=first / Up=last; wraps (rotates) at the ends

        // ----- Agentmaster (SESSIONS.md §9): the Sessions page's AI search — TerminalPage.AgentSessionsAi.cpp -----
        // Each is UI-thread-only unless it says otherwise; the full contracts are at the definitions.
        void _BuildSessionsAiHeader(const winrt::Windows::UI::Xaml::Controls::Grid& topLeft, const winrt::Windows::UI::Xaml::Controls::Grid& header); // one-time, from _BuildSessionsPageShell: the AI toggle inside the search box, the reset button right of it, the strip row under the header, and the box's Enter / Escape keys
        void _SessionsAiFitToggleIntoBox(); // make room in the search box for the toggle over its right end: grow the right padding, shift the box's own clear (X) button left of the toggle; each once, never throws
        void _SetSessionsAiMode(bool on); // turn the AI search on / off for this page (refuses "on" without a native claude.exe); re-renders
        void _SessionsAiAsk(); // send the search box's text to the conversation (the newest request wins), then clear the box
        void _SessionsAiAttach(); // register this page's agent listener once (events are marshalled to this window's UI thread)
        void _SessionsAiDetach() noexcept; // ~TerminalPage: remove the listener and leave the in-use tally. Thread-safe, never throws
        void _SessionsAiSetInUse(bool inUse) noexcept; // count this page in / out of the pages that show the AI search; the last one out frees the agent's index and parks its process
        void _SessionsAiNotePageShown(bool shown); // the page came on screen or left it (opened / closed, or uncovered / covered by a tab switch): keep the tally right, and warm the agent up when the page comes back in AI mode
        void _SessionsAiAfterGather(); // the page's own gather landed: put the AI list's extra rows back, and let the agent read the index now (the sidecar files are current)
        void _OnSessionsAiEvent(const ::Agentmaster::SessionSearchEvent& e); // one agent event, on the UI thread: update the list, the strip, the table
        void _SessionsAiAdoptSnapshot(const ::Agentmaster::SessionSearchSnapshot& snapshot); // take the conversation's shown list as the page's list (AI mode on, a reset, an answer that ends a preview)
        void _SessionsAiMergeExtraRows(); // make _sessionsRows / _sessionsEntries hold exactly the extra rows the current AI list needs (aiExtra rows, at the end)
        void _SessionsAiTakeReasons(const std::vector<std::wstring>& ids, const std::vector<std::wstring>& reasons); // _sessionsAiReasons := the model's whys of a shown list (index-aligned with ids; empty ones are not stored)
        std::wstring _SessionsAiReasonFor(const std::wstring& sessionId) const; // the why a row of the AI list on screen carries, or "" (AI off, no list, or a preview)
        bool _SessionsAiApplyView(std::vector<const _SessionsRow*>& view, bool& keepOrder) const; // _RenderSessionsTable: replace the view with the AI list when one is active; false = no AI list, the view is untouched
        void _UpdateSessionsAiStrip(const ::Agentmaster::SessionSearchSnapshot& snapshot); // repaint the strip, the reset button and the placeholder from the conversation's state
        void _CycleSessionsAiThinking(); // the strip's thinking toggle was clicked: the next level (skipping levels the model lacks), persisted for every window, applied by the agent from the next request
        static _SessionsRow _SessionsRowFromIndexEntry(const ::Agentmaster::SessionIndexEntry& entry); // a table row from an index entry alone (no directory listing): the rows of sessions outside the page's window
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
        // Agentmaster (tags perf, 2026-10-10): the Sessions page's reaction to a tag edit made in the
        // Tags panel. It used to be "_RebuildSessionsTagChips + _RenderSessionsTable" after EVERY edit —
        // a full rebuild of every row of the table (each with its eager context menu), also while the
        // page was hidden behind a tab, where nobody could see it. Now, from the cheapest case up:
        //   - the page was never built: nothing;
        //   - the page is HIDDEN: one flag (_sessionsTagsDirty); the page repaints when it comes back;
        //   - the page is shown and the edit can change neither which rows are listed nor another row's
        //     ribbons (no tag facet is picked, no color changed): the chips, and the changed rows' tag
        //     cells in place (_UpdateSessionsRowTags);
        //   - otherwise (a tag facet is picked — the row may have to appear or go — or a tag was
        //     recolored — every carrier's ribbon changes): the chips and the whole table, as before.
        // _sessionsTags must already hold the new tags (the Tags panel writes it before it queues this).
        //   sessionIds - the sessions whose tag list changed.
        //   recolored  - an existing tag's color changed.
        // UI thread only. Throws what the XAML rebuilds throw; its one caller (_RunTagSurfacesRefresh) contains it.
        void _SessionsNoteTagsChanged(const std::unordered_set<std::wstring>& sessionIds, bool recolored);
        // Replace ONE row's Tags cell with the session's current ribbons (_sessionsTags), leaving the
        // row, its other cells, its menu and its selection as they are.
        //   sessionId - the session whose row to update.
        //   returns   - true when the row is in the table (its cell was replaced or removed); false when
        //               the session is not listed right now (filtered out, outside the window) — nothing to do.
        // UI thread only; never call it from inside a pointer handler of that row (it edits the row's
        // children). Throws what the XAML element build throws.
        bool _UpdateSessionsRowTags(const std::wstring& sessionId);
        // Build a row's Tags cell: up to four bookmark ribbons, centered, each wired to the rich tag hover
        // panel; a fifth-or-more tag turns the fourth slot into a dim "+N". The ONE builder for the table
        // render and the one-row update, so the two can never draw a cell differently. The cell is marked
        // (its Tag) so the one-row update can find it again.
        //   tags      - the session's tags, in the order to draw (non-empty; the callers check).
        //   tagColors - one LoadAllTagColors() of the caller (folded name -> color; name-hash fallback per tag).
        //   returns   - the cell, already assigned to the Tags column (0).
        // UI thread only. Throws what the XAML element build throws.
        winrt::Windows::UI::Xaml::Controls::StackPanel _MakeSessionsTagsCell(const std::vector<std::wstring>& tags, const std::map<std::wstring, std::wstring>& tagColors);
        bool _sessionsTagsDirty{ false }; // a tag edit landed while the page was hidden: its chips and table are stale. Repainted, and cleared, when the page returns with the Manager (the overlay's onRestore) or re-gathers (_RefreshSessionsRows, which every explicit open runs)
        void _ToggleSessionsTagFilter(const std::wstring& tag); // bookmark tags: flip one tag in the facet (case-insensitive), then chip + chips row + table re-render
        bool _SessionsRowFilterMatchesAnchor(int kind, const _SessionsRow& r) const; // does the facet for `kind`'s dimension exist AND equal row r's value? (drives the submenu ✓ + the apply-toggle direction)
        std::unordered_set<std::wstring> _ComputeForkFamily(const std::wstring& anchorId) const; // the connected fork-graph component containing anchorId, over the gathered rows (forkedFromId edges, undirected)
        void _UpdateSessionsFilterChip(); // refresh the "✕ filter: …" chip's label + visibility from _sessionsRowFilter
        // ----- Agentmaster (SESSIONS.md §10): the FOLDER FILTER — TerminalPage.AgentSessionsPageActions.cpp -----
        // UI-thread only; each one's full contract (params, returns, throws) is at its definition.
        void _BuildSessionsFolderFilter(const winrt::Windows::UI::Xaml::Controls::StackPanel& bar, const winrt::Windows::UI::Xaml::Controls::Grid& host); // one-time, from _BuildSessionsPageShell: the "📂 Folders" toggle into the filters bar + the dropdown popup into the page host + the outside-press dismissal
        void _ToggleSessionsFoldersPopup(); // the toggle's click: open the dropdown when closed, close it when open
        void _OpenSessionsFoldersPopup(); // place the dropdown under the toggle, size its halves to the room below, rebuild the lists, focus the find box
        void _CloseSessionsFoldersPopup(bool refocusSearch); // close the dropdown (and its tooltips); optionally hand focus back to the page's search box
        void _RebuildSessionsFolderLists(); // re-list both halves (and the picked chips + hint) from the gathered rows and the find text
        void _RefreshSessionsFolderMarks(); // after a pick change: the rows' check marks in place, the chips, the hint, the toggle — no list rebuild (keeps the focused row)
        void _ToggleSessionsFolderFilter(const std::wstring& folder); // add / remove ONE folder in the OR set; refreshes the chip, the toggle, the dropdown and the table
        void _AddSessionsFolderFromFindBox(); // the find box's Enter / "+": add _SessionsFolderFindPick() (never removes), then clear the box
        std::wstring _SessionsFolderFindPick() const; // what Enter would add: a typed absolute path (in the sessions' spelling when known), else the top RECENT match; "" = nothing
        void _ClearSessionsFolderFilter(); // drop every picked folder (the dropdown's Clear); keeps "Include subfolders"
        void _SetSessionsFoldersIncludeSubfolders(bool on); // the "Include subfolders" box: store the preference, refresh the chip and the table
        void _UpdateSessionsFoldersButton(); // the toggle's label ("Folders" / the one folder / "N folders"), its checked state and its tip
        winrt::fire_and_forget _InferSessionsRowDirs(std::vector<_SessionsInferJob> jobs, bool useGit, uint64_t gatherGen); // the follow-up pass: infer each job's folder off-thread, cache it, fill the rows of the same gather, re-render
        // Agentmaster: the generic window-level page-overlay seam (_agentPageOverlays) — register
        // at page build; dismiss-all from any global site (the tab-switch handler). See the struct.
        void _RegisterAgentPageOverlay(const winrt::Windows::UI::Xaml::Controls::Grid& host, std::atomic<bool>* visibleMirror, std::function<void()> onDismiss, std::function<void()> onRestore = {});
        void _DismissAgentPageOverlays(); // synchronous collapse of EVERY registered page (safe outside in-page pointer handlers)
        void _RestoreAgentPageOverlays(); // re-show pages logically-open when the Manager tab was last left (their collapsed tree kept the search text/rows); called on RETURN to the Manager tab
        void _SetAgentPageOverlayOpenIntent(std::atomic<bool>* visibleMirror, bool open); // a page's Show/Hide flags whether _RestoreAgentPageOverlays should re-open it (found by its visibility mirror)
        void _MoveArchiveSelection(int delta); // Up/Down keyboard nav over _archiveVisibleOrder (same rotate semantics as the Sessions page)

        // --- Agentmaster TRANSCRIPT OVERLAY (TerminalPage.AgentTranscript.cpp; AgentTranscriptOverlay.h) ---
        // A session's conversation, rendered as Markdown at a detail level, in a reading card laid over
        // the WHOLE page (90% x 80% of the window). One per window, built on first use. Opened by every
        // "Show Transcript" / "Copy ▸ Transcript ▸ Show" item; closed by Esc / a click outside / its ✕,
        // and by the page on any tab switch (_ApplyAgentTabSelection), tab move (_TryMoveTab) or tab drag
        // (_TabReorderActivate / _sendDraggedTabToWindow).
        void _EnsureTranscriptOverlay(); // build the overlay into Root() once and wire its focus hand-back (_FocusCurrentTab); throws winrt::hresult_error when the XAML can't be built (callers contain it)
        void _ShowTranscriptOverlay(const std::wstring& sessionId, bool codex, const std::wstring& codexRolloutId, const std::wstring& title, std::wstring_view origin); // open (or retarget) the overlay on one conversation at the level the user last picked in any window (AgentTranscriptOverlay::kRememberedLevel — Default until a first pick) — DEFERRED one tick past the invoking menu's close (its focus hand-back must land first); `origin` names the menu for the [nav] transcript-show line. Never throws
        void _ShowTranscriptOverlayForSession(const std::wstring& sessionId, std::wstring_view origin); // the managed-session form: kind / Codex rollout id / title resolved from the registry (an id it doesn't know opens as a Claude transcript). Never throws
        void _CloseTranscriptOverlay(std::wstring_view reason) noexcept; // close it if open (no-op otherwise) and hand focus back to the current tab / the Manager page — the overlay's focus guard refused the focus move the closing tab switch itself made
        bool _TranscriptOverlayIsOpen() const noexcept; // is the overlay up? (_ManagerPaneNavPreviewKeyDown routes Esc + the tab-switch chords to it while it is)
        winrt::com_ptr<implementation::AgentTranscriptOverlay> _transcriptOverlay{ nullptr }; // the overlay (null until first use)

        // --- Agentmaster SIDE PANEL (TerminalPage.AgentSidePanel.cpp) ---------------------------------
        // A window-chrome (NOT a terminal pane, NOT per-tab) fleet panel docked beside the tab content:
        // a searchable/sortable vertical list of the LIVE session cards (the Triage-Board integration —
        // select / double-click-activate / the full lazy session context menu; LOCAL to this window by
        // default, a per-window LOCAL/GLOBAL toggle widens it to the fleet) over the SELECTED session's
        // summary (a fill-hosted AgentTabOverlay summary panel). Layout persists PER WINDOW
        // (WindowRecord::sidePanel*) and every LAYOUT adjustment also RMWs the GLOBAL AppSettings
        // defaults so the last window to adjust seeds new windows — the card scope is the exception
        // (per-window only, no global default: a new window always starts LOCAL). The header's ⌖ toggle
        // (GLOBAL, default ON) makes selecting a tab scroll the list to that tab's card.
        void _InitAgentSidePanel(); // build the panel UI into ContentArea (once, from _OnFirstLayout after the engine + window-record claim); seeds layout from the claimed record, else the AppSettings defaults
        void _SidePanelApplyDock(); // assign TabContent / splitter / panel host to the ContentArea columns per the GLOBAL AppSettings::sidePanelOnLeft, then re-apply widths (a dock flip rotates which column is the star)
        void _SidePanelApplyLayout(); // re-derive the three column widths + the cards/summary row ratios from the live fractions + collapse state; safe pre-layout (a 0-width area defers to the next SizeChanged)
        void _SidePanelQueueLayout(); // coalesced _SidePanelApplyLayout on a clean dispatcher tick — the ONLY legal path from a layout-driven trigger (ContentArea.SizeChanged): a synchronous column-width write inside the layout pass risks XAML's E_LAYOUTCYCLE fail-fast (the tag-badge lesson)
        void _SidePanelRebuildCards(); // bring the card list up to date IN PLACE (SIDE_PANEL.md §2c): SnapshotLiveForCards -> scope -> search filter (ParseSessionQuery/MatchesQueryText) -> boardSort order (MakeSortKey/SortKeyLess) -> one model per ROW (stored in _sidePanelRowIds / _sidePanelRowModels) -> _SidePanelSyncRealizedCards (only the rows near the viewport get cards, §2f: update only the changed values of the realized cards that stay, create only the newly realized ones, the fewest removes/inserts/moves to the new order); also refreshes the count line and pays an owed ⌖ reveal. Never throws: a failure is logged (Rule #18), the list reset, and the next tick rebuilds it
        // Bring the cards between the two spacers in step with the rows (SIDE_PANEL.md §2f): measure the realized cards
        // (the row stride = their median height + margins), plan the realized range (VirtualListWindow.h: kept while it
        // covers the viewport's NEAR rows, else the FAR rows; around `anchorRow` for a reveal), apply the rows' models
        // (to the cards that stay only when `modelsChanged`; a new card always gets its full model), reorder with the
        // fewest removes / inserts / moves (KeyedListDiff.h, child index = mirror index + 1) and size the spacers.
        // `modelsChanged`: true right after _SidePanelRebuildCards recomputed the rows; false for a scroll / size /
        // measure / reveal pass (the cards that stay already show their rows' models). `sessions`: the rebuild's lean
        // session row per row (parallel to _sidePanelRowIds) for a NEW card's rich-tooltip arm-build, or null (the
        // arm-build then reads GetWithoutQueue). `anchorRow`: a row to realize around (SIZE_MAX = plan from the viewport).
        // Returns false when the panel ended out of step with its mirror (the caller resets the list and marks it
        // dirty). Throws winrt::hresult_error when XAML refuses an edit, std::bad_alloc on allocation failure — every
        // caller contains it and resets the list. UI thread only; a no-op before the panel is built.
        bool _SidePanelSyncRealizedCards(bool modelsChanged, const std::vector<const ::Agentmaster::SessionInfo*>* sessions, size_t anchorRow);
        // The card list's ScrollViewer moved (ViewChanged): re-plan the realized range from the new offset and run a
        // realize pass only when the plan differs (VirtualListWindow.h PlanRealizedRows — usually not: the FAR range
        // keeps half a viewport of scrolling in hand). Never throws: a failed pass is logged (Rule #18), the list
        // reset and marked dirty. No parameters; returns nothing.
        void _SidePanelOnCardsViewChanged();
        // Coalesce a realized-range re-check onto ONE Low-priority dispatcher tick: after the ScrollViewer's SizeChanged
        // (a taller viewport needs more rows — a layout-driven trigger, so never handled synchronously), and after a pass
        // that built cards (their heights are known only after the next layout pass: the follow-up measures them and
        // corrects the stride and the spacers). A no-op while collapsed or already queued. Never throws: a dispatcher
        // that is gone is logged and the latch re-armed. No parameters; returns nothing.
        void _SidePanelQueueRealize();
        // Run ONE realize pass outside a list update — a scroll, a size change, the post-layout measure, a reveal —
        // with every failure contained: a pass that ends out of step, or a throw (a XAML edit refused, an allocation),
        // is logged with `context` (Rule #18), and the list is reset and marked dirty so the next periodic tick
        // rebuilds it. `context`: the trigger, for the log line ("scroll" / "measure" — a size change or the post-layout
        // re-check — / "reveal").
        // `anchorRow`: a row to realize around, or SIZE_MAX to plan from the viewport. Returns true when the pass
        // ended in step. Never throws. UI thread only.
        bool _SidePanelRunRealizePass(std::wstring_view context, size_t anchorRow);
        void _SidePanelResetCards(); // drop every card (Children().Clear() + the two spacers re-added at 0 height + the view map + the order mirror + the realized range + the count text): the recovery when the list and its mirror disagree or an update failed midway; the next update rebuilds the list from empty. Keeps the rows (data, not UI). Never throws (a failing Clear is logged)
        void _SidePanelScheduleRebuild(bool force); // coalesced rebuild: force==true rebuilds now (user action); otherwise a rebuild within the min-gap only marks _sidePanelDirty (the periodic tick catches up) — and a COLLAPSED panel always defers (no pixels to show)
        void _SidePanelNoteRegistryChange(); // the registry batch-drain hook (UI thread): a session changed somewhere -> schedule a non-forced rebuild
        void _SidePanelNoteTagsChanged(); // a bookmark tag changed from THIS window (the Tags panel's _ToggleSessionTag / _CommitTagEditorAdd, through the deferred _RunTagSurfacesRefresh since 2026-10-10): rebuild the cards NOW so their top-right ribbons follow (tags live in the SessionStore — no registry drain will report it); collapsed -> only marks dirty. Never throws (a failed rebuild is logged + left dirty for the periodic retry)
        // Agentmaster (SIDE_PANEL.md §2g): the process-wide unread census changed its count — some window's tab started or
        // stopped flashing (a turn left Running unwatched, a visit, a Mark Unread, a window activation). Called from this
        // window's census-sink hop (TerminalPage.AgentEngine.cpp), so EVERY window's panel hears every window's change —
        // a GLOBAL list shows other windows' sessions, whose flash state lives in their own pages. Re-derives every row's
        // flash ring from the census in place (_SidePanelRefreshIndicators(false)): no snapshot, no re-sort, only the
        // cards whose ring changed are written. No-op before the panel is built. UI thread. Never throws.
        void _SidePanelNoteUnreadChanged();
        // Agentmaster (SIDE_PANEL.md §2g): re-derive the two status-icon indicators that change WITHOUT a registry notify,
        // and write only the cards whose value moved — no snapshot of the fleet, no re-sort, no reorder:
        //   - the FLASH RING of every row, from the process-wide unread census (CurrentUnreadSessionIds): a tab visit,
        //     a Mark Unread or another window's flash bookkeeping changes it with no registry change;
        //   - with `rereadCacheWarm`, the SPARK CROWN of the REALIZED rows, from a fresh lean read of each
        //     (GetWithoutQueue -> ServerCacheStillWarm): warmth ENDS on a clock, which nothing pushes — the same poll
        //     the tab strip runs (_ScanCacheWarmTabs) on the same ~2.5 s tick. Rows without a card keep the value of the
        //     last list update (every update recomputes all rows; a row a scroll realizes is re-read on the next tick).
        // Both patch the stored row model AND its realized card, so a row realized later shows the patched value.
        // `rereadCacheWarm`: true from the periodic tick, false from the unread notification. No-op before the panel is
        // built, while a realize pass runs (it is marked dirty instead) or with no rows. UI thread only. Never throws: a
        // failure is logged (Rule #18) and the list reset + marked dirty, like every other card-list edit.
        void _SidePanelRefreshIndicators(bool rereadCacheWarm);
        winrt::fire_and_forget _SidePanelPeriodicTick(); // liveness-probe rider (SCANNER thread; self-marshals like _ScanCacheWarmTabs — a terminate-netted fire_and_forget over the awaitable Impl): run the deferred dirty rebuild + the ~30s in-place timing-text refresh (no rebuild churn for a clock tick)
        winrt::Windows::Foundation::IAsyncAction _SidePanelPeriodicTickImpl(); // the awaitable body (resume_foreground first) so an exception propagates to _SidePanelPeriodicTick's catch instead of std::terminate
        void _SidePanelSelectSession(const std::wstring& id, bool driveManagerLens); // select a card (highlight + bind the summary below); driveManagerLens==true (a card click) also drives AgentManagerContent::SelectSession so the Manager lens follows — false for calls that ORIGINATE from the lens/tab funnel (re-entrancy guard: an already-selected id is a no-op)
        void _SidePanelFollowFocusedTab(); // the PEEK-vs-FOLLOW rule: snap the side panel (card highlight + bottom summary) back to the FOCUSED terminal tab's session, ENDING any card "peek". Hooked to TabContent's PointerPressed (handledEventsToo — the click-back-to-work gesture) + bubbling GotFocus in _InitAgentSidePanel; gated on StartupState::Initialized (never clobber the restore-time selection) + a managed focused tab (Manager/pwsh/cmd/external resolve empty -> keep the current peek); driveManagerLens=false, so ONLY the side panel moves (the Manager board/tree/Auto-Testing keeps its deliberate selection). Cheap at rest — the same-id early-out in _SidePanelSelectSession makes the common "already on the focused tab" case free
        void _SidePanelBindSummary(const std::wstring& id); // point the bottom area's fill-hosted AgentTabOverlay summary at `id` (create/replace the overlay; empty id -> release it + show the hint); same-id rebinds just kick RefreshSummaryContent
        void _SidePanelApplySettings(); // adopt GLOBAL settings changes (cog Save / cross-window broadcast): sort label + dock side live, wrap/truncate/previous + color-mode pushed into the summary overlay, then a forced rebuild
        void _SidePanelPersistDefaults(bool width, bool split, bool collapsed, bool summaryHidden = false); // the "last one to adjust sets the default" half: freshest-disk RMW of ONLY the flagged AppSettings sidePanel* fields (granular, so a width drag never exports this window's collapse or summary state), keeping _appSettings in step
        void _SidePanelToggleSummary(); // the "Sum" header toggle: show / hide the summary area (and its splitter); hidden switches every card to the RICH session tooltip beside the panel, shown back to the classic floating tip + the bound summary. Persists per window (record autosave) AND as the new-window default (RMW). Never throws: a failed card switch is logged per card (Rule #18)
        void _SidePanelUpdateSumButton(); // pin the "Sum" toggle's IsChecked to !_sidePanelSummaryHidden (after a click and at build) — the member is the truth, the ToggleButton's self-flip is not
        void _SidePanelApplyCardTipMode(AgentSidePanelCardView& view, const std::wstring& id, bool rebuildTags, const ::Agentmaster::SessionInfo* lean); // switch ONE card to the tooltip mode the summary state asks for: hidden -> silence its floating tips (card, timing, "+N") and arm its rich tooltip host; shown -> clear the host and give the floating tips back. `rebuildTags` re-makes the tag strip (its "+N" tip follows the mode); `lean` is the card's lean session row for the arm-build (null -> a GetWithoutQueue). Never throws (logged)
        void _SidePanelArmCardToolTip(AgentSidePanelCardView& view, const std::wstring& id, const ::Agentmaster::SessionInfo* lean); // create the card's AgentSessionToolTipHost (placement beside the panel), wire its hover/wheel hooks once, and arm-build the header-only card (no transcript analyze) so ToolTipService has a hosted tooltip before the first hover. Throws what the host/XAML throws (callers contain it)
        void _UpdateSidePanelCardToolTip(const std::wstring& id, bool swapWhileOpen = false, bool kickSummary = true, const ::Agentmaster::SessionInfo* lean = nullptr); // the side-panel CARD surface of the rich session tooltip: resolve the session (lean row, else GetWithoutQueue), clear on gone/archived, else the SHARED _PushSessionToolTipCard with key SidePanelToolTipKey(id), the card model's resolved tags, and the room beside the panel as the width. A no-op unless the summary is hidden and the card has a host. Throws what the push throws (callers contain it)
        void _OnSidePanelCardToolTipHover(const std::wstring& id); // the card host's PointerEntered callback: the hover-time build (fresh state + "ago", kicks the mtime-gated summary load) and, for a hover that starts CLOSED, the scroll reset to the top of the body — the tab's own hover recipe
        bool _ScrollSidePanelCardToolTip(const std::wstring& id, int wheelDelta); // the card host's wheel callback: scroll the OPEN card's body (_ScrollSessionToolTipBody, src=card); false (body fits / no card / not hidden mode) leaves the notch to the cards list's own ScrollViewer. Never sets _tooltipWheelClaimed (that flag belongs to the tab row's belt)
        std::optional<AgentSessionToolTipPlacement> _SidePanelCardToolTipPlacement(const winrt::Windows::UI::Xaml::FrameworkElement& card); // the card host's placement provider: Right of the panel (docked left) / Left of it (docked right) — a PlacementRect spanning the whole panel + its splitter in the card's coordinate space, so the card opens just past the panel's edge, vertically on the hovered card. nullopt while the card / panel has no size. Never throws (a failed transform is logged, nullopt)
        double _SidePanelCardToolTipRoomWidth(); // the px the terminal side has for the card beside the panel (content area - panel - splitter - the framework's 20px gap - a margin); 0 when unmeasured (the push then uses the flat 660). Never throws
        void _SidePanelSuspendCardToolTip(const std::wstring& id); // a tag ribbon / "+N" of this card is hovered: DETACH its rich tooltip (the framework closes it — no IsOpen write) so it never floats beside the panel under the tag hover panel; the next hover over the card's body re-attaches it (the push's !Attached condition). A no-op in the classic mode
        void _SidePanelDropCardToolTip(AgentSidePanelCardView& view, const std::wstring& id); // before a card leaves the list (remove / reset): shut its tooltip host down (detach + drop callbacks — a ToolTip left attached to a dropped element is the islands leak class) and forget its surface-keyed signature + scroll state. Never throws
        void _SidePanelToggleCollapsed(); // collapse to the thin expander strip / expand back; persists per window (record autosave) AND as the global default (RMW)
        void _SidePanelFlipDock(); // flip left<->right: RMW the GLOBAL sidePanelOnLeft, re-dock THIS window live (other windows re-dock via the settings broadcast)
        // --- The header's "+" LAUNCHER (SIDE_PANEL.md §2e) ---
        // The "+" Click: close the open card, else open it at the press point (or the button, for a Click without a
        // press). Consumes the recorded press. Never throws (the open and the close contain their own failures).
        void _SidePanelToggleLaunchCard();
        // Build the card ONCE (the dark card, its six Button brushes, the Claude-only launch bar with its host, the key
        // handlers, the Root()-parented Popup) and register the two Root() hooks it needs (the outside-press close and
        // the window-resize re-placement). Returns true when the card exists (built now or before), false when the
        // build failed — logged (Rule #18); the next "+" click tries again from scratch.
        bool _SidePanelEnsureLaunchCard();
        // Open the card: build it if needed, anchor it (`fromPress` = at the recorded press point, else under the "+"
        // button), copy the Agent Manager's box text into it, fit the box to the window and place the card toward the
        // window's center (PointerCardPlacement.h), then queue _SidePanelFinishLaunchCardOpen for the next dispatcher
        // tick (the card's layout pass, the caret in its box, its folder list OPEN under the box — the user's rule of
        // 2026-10-19; before, the list stayed closed until a click or a keystroke). Logs [nav] sidepanel launcher open.
        // Never throws (a failure is logged and the card stays closed).
        //   fromPress - true when _sidePanelLaunchAnchor holds the press that led to this open.
        void _SidePanelOpenLaunchCard(bool fromPress);
        // The open's second half, one dispatcher tick after _SidePanelOpenLaunchCard (the card is realized by then):
        //   1. lay the card out at the offsets the open wrote — one synchronous layout pass (UIElement::UpdateLayout),
        //      because a popup's child moves to new offsets only in a layout pass, and a list anchored before it covered
        //      the box and Launch (2026-10-12);
        //   2. focus the box programmatically (the caret in it; a refused focus is logged — keys then still go to the
        //      terminal);
        //   3. open the box's folder list right under it (AgentLaunchBar::OpenPathPicker), so the list shows whenever the
        //      card does (the user's rule, 2026-10-19).
        // A no-op when the card closed before the tick ran. Never throws: each step is guarded and logged on its own
        // (Rule #18), so a failed layout or focus still opens the list (the bar's re-anchor round then corrects a stale
        // anchor one frame later).
        void _SidePanelFinishLaunchCardOpen();
        // Close the open card: drop an armed key, dismiss the bar (its drop-down closed, the box normalized), mirror the
        // box's final text into the Agent Manager's box, close the Popup and log [nav] sidepanel launcher close
        // (<reason>). When the card held keyboard focus, focus goes back to the current tab on a later tick
        // (_FocusCurrentTab) — a press on a focusable element (the terminal, a text box) keeps the focus it took. A
        // no-op when the card is closed. Never throws.
        //   reason - why it closed, for the log ("outside press", "esc", "+ again", "launch", "tab switch", ...).
        void _SidePanelCloseLaunchCard(std::wstring_view reason);
        // Coalesce a fit + placement of the OPEN card onto one later dispatcher tick: the only legal path from a
        // layout-driven trigger (the card's / Root's SizeChanged, the bar's layout callback) — moving a parented
        // Popup inside a layout pass is the E_LAYOUTCYCLE fail-fast. Never throws (a failed enqueue re-arms the latch).
        void _SidePanelQueueLaunchCardPlacement();
        // Place the card at _sidePanelLaunchAnchor with PlacePointerCard (corner at the anchor, extending toward the
        // window's center, clamped 8 DIP inside Root()), reading the card's live size (its DesiredSize before the first
        // arrange). Writes the Popup offsets only when they change. Never throws (logged; the card keeps its offsets).
        void _SidePanelPlaceLaunchCard();
        // Size the card's box for the window: from 160 to 504 DIPs (the Agent Manager's comfortable width), whatever
        // the window leaves beside the agent chip, Launch and Fork. A fixed width (Min = Max), so the card does not
        // change size while you type. Written only when it changes. Never throws (logged).
        void _SidePanelFitLaunchBox();
        // Mirror the card's box text into the Agent Manager's box (AgentManagerContent::SetLaunchBoxText) — the two
        // boxes show the same text (the user's "keep the last, same one we have in agent manager tab"). A no-op without
        // a card or a Manager. Never throws (logged).
        void _SidePanelSyncLaunchTextToManager();
        // True when a press whose source is `source` is INSIDE the launcher: the card, the bar's drop-down, the "+"
        // button (its Click toggles), or the content of another open popup (the "Specify a model" prompt the Launch
        // hover menu opens). Everything else is an outside press. Never throws (a failed walk answers false: close).
        //   source  - the press's OriginalSource.
        //   returns - true = keep the card open.
        bool _SidePanelLaunchPressIsInside(const winrt::Windows::UI::Xaml::DependencyObject& source);
        // True while the card's Popup is open. Never throws (a failed read answers false).
        bool _SidePanelLaunchCardOpen() const noexcept;
        void _SidePanelUpdateSortButton(); // reflect the current AppSettings::boardSort on the order button's label (the board's own label mapping)
        void _SidePanelToggleScope(); // flip the card list LOCAL <-> GLOBAL for THIS window: relabel, force a rebuild, persist via the window-record autosave ONLY — never _SidePanelPersistDefaults (the scope has no global default by design: every new window starts LOCAL) and never the Manager's own tree/board scope
        void _SidePanelUpdateScopeButton(); // reflect _sidePanelGlobal on the scope toggle's label (LOCAL / GLOBAL — the board's own words)
        void _SidePanelToggleScrollToTab(); // the ⌖ header toggle's click: flip the GLOBAL AppSettings::sidePanelScrollToSelectedTab (freshest-disk RMW of that one field) + BROADCAST it so every open window follows at once; turning it ON scrolls to the focused tab's card right away, OFF drops any reveal still owed
        void _SidePanelUpdateScrollToTabButton(); // pin the ⌖ toggle's IsChecked to _appSettings.sidePanelScrollToSelectedTab (after a click, a cog Save, or another window's broadcast) — the setting is the truth, the ToggleButton's self-flip is not
        void _SidePanelRevealTabCard(const std::wstring& id); // the selected TAB now shows session `id`: when the ⌖ toggle is ON, owe the card list a one-shot scroll to that card (coalesced + deferred to a clean tick); an empty id (a shell tab — no card) or the toggle OFF drops any reveal still owed
        void _SidePanelQueueReveal(); // coalesce the owed reveal onto ONE Low-priority dispatcher tick (after the rebuild/layout that may have just created the card); a no-op while collapsed or when nothing is owed — the expand path / next rebuild re-queues
        void _SidePanelRevealPendingNow(); // the reveal tick's body: if the owed session is a LISTED row, realize the rows around it (a row outside the realized range has no card yet — SIDE_PANEL.md §2f), UpdateLayout + StartBringIntoView its card (minimal scroll — a fully visible card never moves) and consume the debt; a session not listed yet keeps it owed. Never throws (XAML failures are logged, Rule #18) and never moves keyboard focus
        std::unordered_set<std::wstring> _HostedSessionIds() const; // the ids of the managed sessions whose tab lives in THIS window (the live _claudeTabs entries) — the ONE "is this session here?" predicate the Explorer Tree's LOCAL scope provider (_WireAgentManagerContent) AND the side panel's LOCAL card list share, so the two surfaces can never disagree; defined in TerminalPage.AgentSidePanel.cpp
        AgentSidePanelCardView _SidePanelCreateCardView(const std::wstring& id); // build ONE card's empty skeleton (card grid over a CHILDLESS rounded back / band area over a CHILDLESS rounded band / title / state row / dir line — the elements every card has; the rounded shapes stay childless so a card gets no composition node of its own) and wire its select / activate / hover / the shared lazy context menu (anchored on the card, so its Tags item opens under it); the caller fills it with _SidePanelApplyCardModel(fresh=true) and inserts it
        void _SidePanelApplyCardModel(AgentSidePanelCardView& view, AgentSidePanelCardModel model, const std::wstring& id, bool fresh); // write `model` into the card: with fresh==false only the values that differ from view.model (a text, a brush, a part added or dropped); fresh==true writes everything (a new card). Then view.model = model
        void _SidePanelApplyCardTags(AgentSidePanelCardView& view, const AgentSidePanelCardModel& model, const std::wstring& id); // give the band the layout `model.tags` needs: untagged = the band is exactly the title; tagged = [ title * ][ a NEW tag strip ] (a changed tag list, tag color or ink replaces the strip; the title element is re-parented, never rebuilt); `id` = the card's session (the strip's ribbons suspend its rich tooltip in the "Sum" off mode)
        winrt::Windows::UI::Xaml::Controls::StackPanel _SidePanelMakeTagStrip(const std::vector<std::wstring>& tags,
                                                                              const std::vector<winrt::Windows::UI::Color>& colors,
                                                                              bool darkInk,
                                                                              double hangPx,
                                                                              const winrt::Windows::UI::Xaml::UIElement& card,
                                                                              const std::wstring& id); // a card's top-right tag strip: one ribbon per tag in its resolved `colors` entry (capped at kSidePanelCardTagSlots, the last slot a "+N" — with its own floating tip in the classic mode only), each opening the shared tag hover panel; hung from the band's top edge by `hangPx`; a tooltip-FREE zone for `card` (held weakly) so the card's floating tip never stacks on the hover panel — and, with the summary hidden, a ribbon / "+N" entry detaches the card's rich tooltip (_SidePanelSuspendCardToolTip(id))

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
        void _ManagerPaneNavPreviewKeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e); // Agentmaster: tunneling handler on the page Root + the Manager page root — while the Agent Manager page is on screen, dispatch TAB-SWITCHING chords (NextTab/PrevTab/SwitchToTab) so ctrl+tab / ctrl+alt+<n> reach a tab even when a Manager box is focused (the bubbling _KeyDownHandler is eaten by the box); other keys fall through.
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        void _HookupKeyBindings(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap) noexcept;
        void _RegisterActionCallbacks();

        void _UpdateTitle(const Tab& tab);
        void _UpdateTabIcon(Tab& tab);
        void _UpdateAllTabIcons(); // Agentmaster: re-apply the GLOBAL "Show icons on tabs" (AppSettings::showTabIcon) to every tab live (cog Save + cross-window broadcast — the _updateAllTabCloseButtons twin)
        void _UpdateReservedTabTitleLines(); // Agentmaster: push the MAX title-line-count across ALL tabs to every header's line-reserver, so the tab strip's height stays consistent regardless of virtualization (a multi-line title tab scrolled out of view no longer snaps the row shorter). Called on any title change + tab add/remove.
        void _UpdateTabView();
        void _UpdateTabWidthMode(); // Agentmaster: applies the EFFECTIVE mode (_EffectiveTabWidthMode — the cog's override, else Terminal's own tabWidthMode); a real change re-applies the mode-dependent per-tab width bounds (_ApplyTabSizingToAllTabs) and, entering Equal, drops the stale cap the other modes leave on the strip's list (_ClearTabStripListMaxWidth)
        // Agentmaster (tab sizing — the cog's Tabs & Overlay "TAB SIZE" section; AppSettings::tabWidthMode /
        // tabMinWidth / tabMaxWidth / tabTitleFontSize / tabTitleFontWeight / tabMinHeight). All UI thread.
        winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode _EffectiveTabWidthMode() const; // the mode the strip should use: the cog's tabWidthMode, or Terminal's own GlobalSettings().TabWidthMode() while it is left on "Windows Terminal setting" (and before settings load)
        void _ApplyTabSizingSettings(bool relayoutStrip); // THE apply: publish the width-mode override for Tab's rename box + the Equal-mode bounds as MUX's TabViewItemMin/MaxWidth APPLICATION resources (process-wide), apply the mode, then every tab's height / title font / mode-dependent bounds; relayoutStrip (cog Save + broadcast — not engine init, where no tab exists yet) forces MUX to re-run its equal-width pass when only the bounds changed
        void _ApplyTabSizingToAllTabs(); // _ApplyTabSizingToTab for every tab + its rename box's width ceiling (Tab::RefreshHeaderRenamerMaxWidth)
        uint32_t _ApplyTabSizingToTab(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& tabViewItem); // one tab: MinHeight, the mode-dependent MinWidth/MaxWidth (MUX owns widths in Equal — cleared there), and the header's title font. Defaults CLEAR Agentmaster's local values (the XAML styles govern — byte-identical to before). The RENAME HOLD (tab title fit): while the header's rename box is open on the SELECTED tab of an Equal-width strip, MinWidth = RenamingTabWidth (the maximum tab width, 200 px floor) so the box — bounded by its tab since the title row — has room; only the selected tab, as designed (the original reason, that widening another made MUX scroll the strip to the selected one, is gone since the strip ignores bring-into-view requests; the page reveals the renaming tab itself, _TrackRenamingTabInStrip). Returns the held width in px, or 0 when no hold is in force. Called by _InitializeTab for every new tab, by _ApplyTabSizingToAllTabs, and on Tab::RenamerOpenChanged (rename box opened / closed)
        void _PublishTabWidthBounds(double minWidth, double maxWidth); // write the Equal-mode clamp bounds into Application.Resources (TabViewItemMinWidth / TabViewItemMaxWidth — the keys MUX's TabView::UpdateTabWidths re-reads on every pass); PROCESS-wide, and a no-op when already in force
        void _ForceTabStripWidthPass(); // make MUX re-run its private equal-width pass NOW (a mode flip-and-back within one UI tick — nothing paints in between), so new bounds land without waiting for a resize / tab add
        void _ClearTabStripListMaxWidth(); // drop the MaxWidth MUX's SizeToContent/Compact pass leaves on the inner TabListView (Equal mode never sets one, so a stale cap would keep the strip from growing with the window)
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
        // Agentmaster (TAB-K4): arm the one-shot handler that keeps a Shift+Click press's release from selecting its tab
        // (see _tabShiftActivatePress). routeChild = the pressed tab's template root; pointerId = the pressing pointer.
        // Returns false (nothing armed — the caller arms the selection veto) for a null routeChild or a failed
        // registration. Never throws (a failure is logged, Rule #18).
        bool _ArmTabShiftActivateRelease(const winrt::Windows::UI::Xaml::UIElement& routeChild, uint32_t pointerId) noexcept;
        safe_void_coroutine _OnTabPointerReleasedCloseTab(IInspectable sender);

        void _OnTabSelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _OnTabItemsChanged(const IInspectable& sender, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs);
        void _OnTabCloseRequested(const IInspectable& sender, const Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& eventArgs);
        void _OnFirstLayout(const IInspectable& sender, const IInspectable& eventArgs);

        // Agentmaster: the tab-strip Home button. _UpdateManagerNavButtons recomputes it: shown once the page
        // exists, painted as the active tab while the page is up, and its face — Home, or on the page the
        // jump-back arrow to the selected session's tab (_ManagerJumpBackSessionId picks that session,
        // _ApplyManagerHomeButtonFace writes the glyph / tooltip / accessible name). Its click opens the page
        // or jumps back, by the face it shows; hovering it re-runs the update so the tooltip names the
        // session as it is called right now. _EnsureTabStripScrollViewer binds (lazily) to the TabView's
        // internal horizontal scroller that the strip reveal + the drag gesture drive. (All defined in
        // TerminalPage.AgentEngine.cpp.)
        void _OnManagerHomeButtonClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _OnManagerHomeButtonPointerEntered(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args);
        void _EnsureTabStripScrollViewer();
        void _UpdateManagerNavButtons();
        std::wstring _ManagerSelectedSessionId() const; // the Manager lens's currently-selected managed session (empty if none / an external is selected)
        std::wstring _ManagerJumpBackSessionId() const; // the session the Home button's jump-back face returns to (empty: no jump back now)
        void _ApplyManagerHomeButtonFace(const std::wstring& backSessionId);

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

        // Agentmaster: staggered per-tab theme apply — _updateThemeColors snapshots the per-tab
        // ThemeColor arguments into one batch and _ApplyTabThemeColorsChunk applies it a chunk per
        // Low-priority dispatcher hop (first chunk inline), so a big strip's REAL theme change
        // (~0.5s/tab of dictionary rewrite + visual-state flips at fleet scale) degrades to a brief
        // visual trickle instead of one pump-blocking grind. _themeApplyGeneration supersedes an
        // in-flight trickle when a newer pass starts.
        struct ThemeColorApplyBatch
        {
            std::vector<winrt::TerminalApp::Tab> tabs;
            winrt::Microsoft::Terminal::Settings::Model::ThemeColor tabBackground{ nullptr };
            winrt::Microsoft::Terminal::Settings::Model::ThemeColor tabUnfocusedBackground{ nullptr };
            til::color tabRowColor{};
            size_t next{ 0 };
        };
        uint64_t _themeApplyGeneration{ 0 };
        void _ApplyTabThemeColorsChunk(const std::shared_ptr<ThemeColorApplyBatch>& batch, uint64_t generation);

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
