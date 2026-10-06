// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "BaseWindow.h"

struct SystemMenuItemInfo
{
    winrt::hstring label;
    winrt::delegate<void()> callback;
};

class IslandWindow :
    public BaseWindow<IslandWindow>
{
public:
    static bool IsCursorHidden() noexcept;
    static void HideCursor() noexcept;
    static void ShowCursorMaybe(const UINT message) noexcept;

    IslandWindow() noexcept;
    ~IslandWindow();

    virtual void MakeWindow() noexcept;
    virtual void Close();

    virtual void OnSize(const UINT width, const UINT height);
    HWND GetInteropHandle() const;

    [[nodiscard]] virtual LRESULT MessageHandler(UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept;

    [[nodiscard]] LRESULT OnNcCreate(WPARAM wParam, LPARAM lParam) noexcept;

    void OnResize(const UINT width, const UINT height);
    void OnMinimize();
    void OnRestore();
    virtual void OnAppInitialized();
    virtual void SetContent(winrt::Windows::UI::Xaml::UIElement content);
    virtual void OnApplicationThemeChanged(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme);
    virtual til::rect GetNonClientFrame(const UINT dpi) const noexcept;
    virtual til::size GetTotalNonClientExclusiveSize(const UINT dpi) const noexcept;

    virtual void Initialize();

    void SetCreateCallback(std::function<void(const HWND, const til::rect&)> pfn) noexcept;

    void SetSnapDimensionCallback(std::function<float(bool widthOrHeight, float dimension)> pfn) noexcept;

    void FocusModeChanged(const bool focusMode);
    void FullscreenChanged(const bool fullscreen);
    void SetAlwaysOnTop(const bool alwaysOnTop);
    void ShowWindowChanged(const bool showOrHide);
    virtual void SetShowTabsFullscreen(const bool newShowTabsFullscreen);

    void FlashTaskbar();
    void SetTaskbarProgress(const size_t state, const size_t progress);
    void SetTaskbarBadge(const uint32_t count); // Agentmaster (taskbar unread badge): paint `count` on this window's taskbar button (0 clears it)
    void SetTaskbarUnreadFrame(const uint32_t argb); // Agentmaster (taskbar preview frame, NOTIFICATIONS.md section 9): frame THIS window's taskbar / Alt+Tab preview in `argb` (0xAARRGGBB) while it holds unread tabs; 0 removes the frame and gives the preview back to DWM

    void SummonWindow(winrt::TerminalApp::SummonWindowBehavior args);

    bool IsQuakeWindow() const noexcept;
    void IsQuakeWindow(bool isQuakeWindow) noexcept;
    void SetAutoHideWindow(bool autoHideWindow) noexcept;

    void HideWindow();

    void SetMinimizeToNotificationAreaBehavior(bool MinimizeToNotificationArea) noexcept;

    void OpenSystemMenu(const std::optional<int> mouseX, const std::optional<int> mouseY) const noexcept;
    void AddToSystemMenu(const winrt::hstring& itemLabel, winrt::delegate<void()> callback);
    void RemoveFromSystemMenu(const winrt::hstring& itemLabel);

    void UseDarkTheme(const bool v);
    virtual void UseMica(const bool newValue, const double titlebarOpacity);

    til::event<winrt::delegate<>> DragRegionClicked;
    til::event<winrt::delegate<>> WindowCloseButtonClicked;
    til::event<winrt::delegate<void(winrt::Windows::Foundation::Point, winrt::Microsoft::Terminal::Core::Point)>> MouseScrolled;
    til::event<winrt::delegate<void(bool)>> WindowActivated;
    til::event<winrt::delegate<void()>> NotifyNotificationIconPressed;
    til::event<winrt::delegate<void()>> NotifyWindowHidden;
    til::event<winrt::delegate<void(HMENU, UINT)>> NotifyNotificationIconMenuItemSelected;
    til::event<winrt::delegate<void()>> NotifyReAddNotificationIcon;
    til::event<winrt::delegate<void()>> ShouldExitFullscreen;
    til::event<winrt::delegate<void(bool)>> MaximizeChanged;

    til::event<winrt::delegate<void()>> WindowMoved;
    til::event<winrt::delegate<void(bool)>> WindowVisibilityChanged;

protected:
    void ForceResize()
    {
        // Do a quick resize to force the island to paint
        const auto size = GetPhysicalSize();
        OnSize(size.width, size.height);
    }

    HWND _interopWindowHandle;

    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource _source; // nulled in ctor
    winrt::Windows::UI::Xaml::Controls::Grid _rootGrid; // nulled in ctor
    wil::com_ptr<ITaskbarList3> _taskbar;

    // Agentmaster (taskbar unread badge; NOTIFICATIONS.md section 8 / TaskbarBadge.h): the count this window's taskbar
    // button should carry (0 = no overlay). REMEMBERED, not fire-and-forget, because Explorer forgets every overlay
    // when it restarts and a button that doesn't exist yet ignores one: both cases are healed by re-applying this value
    // when TaskbarButtonCreated arrives. _taskbarButtonReady flips on that first message — before it, a failed
    // SetOverlayIcon is expected and silent. _taskbarBadgeLastHr de-duplicates the failure log (a policy-disabled
    // overlay would otherwise log at every count change).
    uint32_t _taskbarBadgeCount{ 0 };
    bool _taskbarButtonReady{ false };
    HRESULT _taskbarBadgeLastHr{ S_OK };
    void _ApplyTaskbarBadge() noexcept;
    static UINT _TaskbarButtonCreatedMessage() noexcept;

    // Agentmaster (taskbar preview frame; NOTIFICATIONS.md section 9 / TaskbarThumbnail.h): while this window holds
    // unread tabs, its taskbar / Alt+Tab preview is OUR bitmap -- a capture of the window with a frame painted on it --
    // through DWM's "iconic representation". The captures run on a thread-pool worker, because every Agentmaster
    // window shares ONE UI thread and a 4K capture takes tens of milliseconds. TaskbarFrameState (defined in the .cpp)
    // is shared between this window and that worker, so a worker still running when the window closes holds its own
    // reference and finds `hwnd` null instead of a freed window.
    struct TaskbarFrameState;
    struct TaskbarFramePassInput; // one worker pass's inputs, copied out of TaskbarFrameState under its lock
    std::shared_ptr<TaskbarFrameState> _taskbarFrame; // created on the first frame; lives until the window closes
    uint32_t _taskbarFrameArgb{ 0 }; // the frame color as last set by SetTaskbarUnreadFrame; 0 = no frame (UI thread only)
    void _TaskbarFrameEnable(const bool turningOn); // set the iconic attributes + the refresh timer (when turning on), store the color, and queue a refresh
    void _TaskbarFrameDisable() noexcept; // clear the iconic attributes and the timers, drop pending work and the remembered capture: DWM shows the live preview again
    void _TaskbarFrameShutdown() noexcept; // the window is closing: stop the worker from touching it (Close / the destructor)
    void _TaskbarFrameRequestPush() noexcept; // queue a refresh (capture + invalidate + push) on the worker, coalesced with any pending one
    void _TaskbarFrameArmSettle() noexcept; // (re)start the short debounce after a resize / restore / DPI change; it queues one refresh when it fires
    void _TaskbarFrameStartWorker() noexcept; // submit the worker for a JobBook that just claimed it; undoes the claim if the pool refuses
    [[nodiscard]] bool _TaskbarFrameOnThumbnailRequest(const LPARAM lparam) noexcept; // WM_DWMSENDICONICTHUMBNAIL: learn DWM's box and queue an answer; false when not framed (default handling)
    [[nodiscard]] bool _TaskbarFrameOnPeekRequest() noexcept; // WM_DWMSENDICONICLIVEPREVIEWBITMAP: queue a full-size peek answer; false when not framed or minimized (DWM then shows its own placeholder)
    static void CALLBACK _TaskbarFrameWorkCallback(PTP_CALLBACK_INSTANCE instance, void* context) noexcept; // the thread-pool worker: drain the JobBook pass by pass
    static void _TaskbarFrameRunPass(TaskbarFrameState& state, const TaskbarFramePassInput& input); // one worker pass: capture once, answer the peek, store the framed preview

    std::function<void(const HWND, const til::rect&)> _pfnCreateCallback;
    std::function<float(bool, float)> _pfnSnapDimensionCallback;

    void _HandleCreateWindow(const WPARAM wParam, const LPARAM lParam) noexcept;
    [[nodiscard]] LRESULT _OnSizing(const WPARAM wParam, const LPARAM lParam);
    [[nodiscard]] LRESULT _OnMoving(const WPARAM wParam, const LPARAM lParam);

    bool _borderless{ false };
    bool _alwaysOnTop{ false };
    bool _fullscreen{ false };
    bool _showTabsFullscreen{ false };
    bool _fWasMaximizedBeforeFullscreen{ false };
    RECT _rcWindowBeforeFullscreen{};
    RECT _rcWorkBeforeFullscreen{};
    UINT _dpiBeforeFullscreen{ 96 };

    virtual void _SetIsBorderless(const bool borderlessEnabled);
    virtual void _SetIsFullscreen(const bool fullscreenEnabled);

    void _RestoreFullscreenPosition(const RECT& rcWork);
    void _SetFullscreenPosition(const RECT& rcMonitor, const RECT& rcWork);

    LONG _getDesiredWindowStyle() const;

    void _OnGetMinMaxInfo(const WPARAM wParam, const LPARAM lParam);

    void _globalActivateWindow(const uint32_t dropdownDuration,
                               const winrt::TerminalApp::MonitorBehavior toMonitor);
    void _dropdownWindow(const uint32_t dropdownDuration,
                         const winrt::TerminalApp::MonitorBehavior toMonitor);
    void _slideUpWindow(const uint32_t dropdownDuration);
    void _doSlideAnimation(const uint32_t dropdownDuration, const bool down);
    void _globalDismissWindow(const uint32_t dropdownDuration);

    static MONITORINFO _getMonitorForCursor();
    static MONITORINFO _getMonitorForWindow(HWND foregroundWindow);
    void _moveToMonitor(HWND foregroundWindow, const winrt::TerminalApp::MonitorBehavior toMonitor);
    void _moveToMonitorOfMouse();
    void _moveToMonitorOf(HWND foregroundWindow);
    void _moveToMonitor(const MONITORINFO activeMonitor);

    bool _isQuakeWindow{ false };
    bool _autoHideWindow{ false };

    void _enterQuakeMode();
    til::rect _getQuakeModeSize(HMONITOR hmon);

    bool _minimizeToNotificationArea{ false };

    std::unordered_map<UINT, SystemMenuItemInfo> _systemMenuItems;
    UINT _systemMenuNextItemId = 0;
    void _resetSystemMenu();

private:
    // This minimum width allows for width the tabs fit
    static constexpr float minimumWidth = 460;

    // We run with no height requirement for client area,
    // though the total height will take into account the non-client area
    // and the requirements of components hosted in the client area
    static constexpr float minimumHeight = 0;

    inline static bool _cursorHidden;
};
