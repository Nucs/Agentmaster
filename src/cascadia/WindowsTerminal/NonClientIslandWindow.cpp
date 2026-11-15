/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/
#include "pch.h"
#include "NonClientIslandWindow.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include "../types/inc/utils.hpp"

// Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): the auto-hiding title bar's needs beyond the pch — the focus manager
// and the pointer events (Xaml.Input), a pointer's position (UI.Input's PointerPoint), the device kind of a pointer
// (Devices.Input), and the EXE-safe hooks.log writer (the EXE links TerminalApp.dll, not the engine lib that owns
// AppendStateLog).
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include "../TerminalApp/AgentMaster/StartupTiming.h"

using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::Foundation::Numerics;
using namespace ::Microsoft::Console;

static constexpr int AutohideTaskbarSize = 2;

namespace
{
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): the two timers the auto-hiding title bar runs on the window's
    // own HWND. IslandWindow's taskbar frame uses 0x414D'0001 / 0x414D'0002 ("AM" + a serial); these continue the series.
    constexpr UINT_PTR kTabRowWatchTimerId = 0x414D'0003; // the watch: the cursor while the row is out, the end of a rest / delay / peek
    constexpr UINT_PTR kTabRowSlideEndTimerId = 0x414D'0004; // one-shot: a slide's end (the drag bar window waits for it)

    // Agentmaster: the XAML input namespace by a name of its own. This file uses both winrt::Windows::UI and
    // winrt::Windows::UI::Xaml, so a bare "Input" is ambiguous between Windows.UI.Input and Windows.UI.Xaml.Input (C2872).
    namespace XamlInput = winrt::Windows::UI::Xaml::Input;

    // Agentmaster: log one "[tabrow] ..." line through the EXE-safe hooks.log writer. Never throws: building the line can only
    // fail with bad_alloc, and the writer swallows its own failures. The profile is resolved long before any window exists,
    // so Startup::Note's "no line before the profile" contract (Rule #15) holds.
    //   what - the line after the tag.
    void LogTabRow(const std::wstring_view what) noexcept
    {
        try
        {
            ::Agentmaster::Startup::Note(std::wstring{ L"[tabrow] " } + std::wstring{ what });
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (Rule #18's logging-path exemption): only bad_alloc can land here, while building the
            // line for the logger itself. Degradation: this one hooks.log line is lost.
        }
    }

    // Agentmaster: report the exception being handled — wil's trace plus one "[tabrow] <context> threw (0x...)" line (Rule
    // #18: a catch may recover silently for the user, never for us). Call it ONLY from inside a catch block: it classifies
    // the current exception (LOG_CAUGHT_EXCEPTION rethrows it internally). Never throws.
    //   context - what was being done, for the line.
    void LogTabRowCaught(const std::wstring_view context) noexcept
    {
        const HRESULT caught = LOG_CAUGHT_EXCEPTION(); // wil's own trace, and the classified HRESULT for the line below
        try
        {
            wchar_t code[16]{};
            ::swprintf(code, 16, L"0x%08X", static_cast<unsigned>(caught));
            LogTabRow(std::wstring{ context } + L" threw (" + code + L")");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (Rule #18's logging-path exemption): only bad_alloc can land here, while building the log
            // line itself, and the logger needs the same allocation. Degradation: no hooks.log line for this failure.
        }
    }

    // Agentmaster: the COM identity of a projected object, for "is this the same element" — two projections of one XAML
    // element can be different interface pointers, so `==` on projections is not an identity test; IUnknown is (the
    // AgentPanelFocus.h rule, which this EXE cannot include: it pulls in the engine's logger).
    //   o - the object; null gives null.
    //   returns - the identity pointer, valid for comparisons while `o` lives (no reference is kept).
    //   throws  - winrt::hresult_error if the object refuses IUnknown (never for a XAML element); callers contain it.
    void* TabRowIdentity(const winrt::Windows::Foundation::IInspectable& o)
    {
        return o ? winrt::get_abi(o.as<winrt::Windows::Foundation::IUnknown>()) : nullptr;
    }

    // Agentmaster: are Windows' "Animation effects" on (Settings > Accessibility > Visual effects)? The taskbar itself does not
    // slide while they are off, and neither does the row. A failed query answers yes (the slide is short and harmless).
    //   returns - true when animations should play.
    //   throws  - nothing.
    bool TabRowAnimationsEnabled() noexcept
    {
        BOOL enabled = TRUE;
        if (!::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        {
            return true;
        }
        return enabled != FALSE;
    }

    // Agentmaster: the brush the floating title bar wears — the app's brush, made OPAQUE, because the bar now lies over the
    // terminal and a see-through bar would mix the tabs with the terminal's text. A solid color loses its transparency (its
    // color kept); a fully transparent solid or a missing brush (Mica, a theme's transparent tab row, the moment before the
    // app sets one) gets the default tab-row color of the theme. An acrylic brush stays (it blurs what is under it, which
    // reads fine), and so does any other kind (a gradient, an image: their opacity cannot be told from here).
    //   brush - the app's brush (may be null).
    //   theme - the window's requested theme; Light picks the light default, anything else the dark one.
    //   returns - the brush to set; never null.
    //   throws  - winrt::hresult_error when XAML refuses to make a brush (the caller contains it).
    winrt::Windows::UI::Xaml::Media::Brush TabRowOpaqueBrush(const winrt::Windows::UI::Xaml::Media::Brush& brush, const ElementTheme theme)
    {
        if (brush)
        {
            const auto solid = brush.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
            if (!solid)
            {
                return brush; // acrylic, a gradient, an image: kept as the app set it
            }
            auto color = solid.Color();
            if (color.A == 255)
            {
                return brush; // already opaque: the very brush the app set (its theme updates keep flowing)
            }
            if (color.A != 0)
            {
                color.A = 255;
                return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ color };
            }
        }
        // Fully transparent, or no brush yet: the theme's tab row (Windows Terminal's dark #2E2E2E, the light #F3F3F3).
        const auto fallback = theme == ElementTheme::Light ? ColorHelper::FromArgb(255, 0xF3, 0xF3, 0xF3) : ColorHelper::FromArgb(255, 0x2E, 0x2E, 0x2E);
        return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ fallback };
    }
}

NonClientIslandWindow::NonClientIslandWindow(const ElementTheme& requestedTheme) noexcept :
    IslandWindow{},
    _backgroundBrushColor{ 0, 0, 0 },
    _theme{ requestedTheme },
    _isMaximized{ false }
{
}

NonClientIslandWindow::~NonClientIslandWindow()
{
    Close();
}

void NonClientIslandWindow::Close()
{
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): stop the auto-hide timers and cut its XAML handlers' link BEFORE
    // the XAML source closes, so no handler or WM_TIMER reaches a closing window.
    _AutoHideShutdown();
    // Avoid further callbacks into XAML/WinUI-land after we've Close()d the DesktopWindowXamlSource
    // inside `IslandWindow::Close()`. XAML thanks us for doing that by not crashing. Thank you XAML.
    SetWindowLongPtr(_dragBarWindow.get(), GWLP_USERDATA, 0);
    IslandWindow::Close();
}

static constexpr const wchar_t* dragBarClassName{ L"DRAG_BAR_WINDOW_CLASS" };

[[nodiscard]] LRESULT __stdcall NonClientIslandWindow::_StaticInputSinkWndProc(HWND const window, UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept
{
    WINRT_ASSERT(window);

    if (WM_NCCREATE == message)
    {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lparam);
        auto nonClientIslandWindow{ reinterpret_cast<NonClientIslandWindow*>(cs->lpCreateParams) };
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(nonClientIslandWindow));
        // fall through to default window procedure
    }
    else if (auto nonClientIslandWindow{ reinterpret_cast<NonClientIslandWindow*>(GetWindowLongPtr(window, GWLP_USERDATA)) })
    {
        return nonClientIslandWindow->_InputSinkMessageHandler(message, wparam, lparam);
    }

    return DefWindowProc(window, message, wparam, lparam);
}

void NonClientIslandWindow::MakeWindow() noexcept
{
    if (_window)
    {
        // no-op if we already have a window.
        return;
    }

    IslandWindow::MakeWindow();

    static auto dragBarWindowClass{ []() {
        WNDCLASSEX wcEx{};
        wcEx.cbSize = sizeof(wcEx);
        wcEx.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wcEx.lpszClassName = dragBarClassName;
        wcEx.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wcEx.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcEx.lpfnWndProc = &NonClientIslandWindow::_StaticInputSinkWndProc;
        wcEx.hInstance = wil::GetModuleInstanceHandle();
        wcEx.cbWndExtra = sizeof(NonClientIslandWindow*);
        return RegisterClassEx(&wcEx);
    }() };

    // The drag bar window is a child window of the top level window that is put
    // right on top of the drag bar. The XAML island window "steals" our mouse
    // messages which makes it hard to implement a custom drag area. By putting
    // a window on top of it, we prevent it from "stealing" the mouse messages.
    _dragBarWindow.reset(CreateWindowExW(WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
                                         dragBarClassName,
                                         L"",
                                         WS_CHILD,
                                         0,
                                         0,
                                         0,
                                         0,
                                         GetHandle(),
                                         nullptr,
                                         wil::GetModuleInstanceHandle(),
                                         this));
    THROW_HR_IF_NULL(E_UNEXPECTED, _dragBarWindow);
}

LRESULT NonClientIslandWindow::_dragBarNcHitTest(const til::point pointer)
{
    auto rcParent = GetWindowRect();
    // The size of the buttons doesn't change over the life of the application.
    const auto buttonWidthInDips{ _titlebar.CaptionButtonWidth() };

    // However, the DPI scaling might, so get the updated size of the buttons in pixels
    const auto buttonWidthInPixels{ buttonWidthInDips * GetCurrentDpiScale() };

    // make sure to account for the width of the window frame!
    const til::rect nonClientFrame{ GetNonClientFrame(_currentDpi) };
    const auto rightBorder{ rcParent.right - nonClientFrame.right };
    // From the right to the left,
    // * are we in the close button?
    // * the maximize button?
    // * the minimize button?
    // If we're not, then we're in either the top resize border, or just
    // generally in the titlebar.
    if ((rightBorder - pointer.x) < (buttonWidthInPixels))
    {
        return HTCLOSE;
    }
    else if ((rightBorder - pointer.x) < (buttonWidthInPixels * 2))
    {
        return HTMAXBUTTON;
    }
    else if ((rightBorder - pointer.x) < (buttonWidthInPixels * 3))
    {
        return HTMINBUTTON;
    }
    else
    {
        // If we're not on a caption button, then check if we're on the top
        // border. If we're not on the top border, then we're just generally in
        // the caption area.
        const auto resizeBorderHeight = _GetResizeHandleHeight();
        const auto isOnResizeBorder = pointer.y < rcParent.top + resizeBorderHeight;

        return isOnResizeBorder ? HTTOP : HTCAPTION;
    }
}

// Function Description:
// - The window procedure for the drag bar forwards clicks on its client area to
//   its parent as non-client clicks.
// - BODGY: It also _manually_ handles the caption buttons. They exist in the
//   titlebar, and work reasonably well with just XAML, if the drag bar isn't
//   covering them.
// - However, to get snap layout support (GH#9443), we need to actually return
//   HTMAXBUTTON where the maximize button is. If the drag bar doesn't cover the
//   caption buttons, then the core input site (which takes up the entirety of
//   the XAML island) will steal the WM_NCHITTEST before we get a chance to
//   handle it.
// - So, the drag bar covers the caption buttons, and manually handles hovering
//   and pressing them when needed. This gives the impression that they're
//   getting input as they normally would, even if they're not _really_ getting
//   input via XAML.
LRESULT NonClientIslandWindow::_InputSinkMessageHandler(UINT const message,
                                                        WPARAM const wparam,
                                                        LPARAM const lparam) noexcept
{
    switch (message)
    {
    case WM_NCHITTEST:
    {
        // Try to determine what part of the window is being hovered here. This
        // is absolutely critical to making sure Snap Layouts (GH#9443) works!
        return _dragBarNcHitTest({ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
    }
    break;

    case WM_NCMOUSEMOVE:
        // When we get this message, it's because the mouse moved when it was
        // over somewhere we said was the non-client area.
        //
        // We'll use this to communicate state to the title bar control, so that
        // it can update its visuals.
        // - If we're over a button, hover it.
        // - If we're over _anything else_, stop hovering the buttons.
        switch (wparam)
        {
        case HTTOP:
        case HTCAPTION:
        {
            _titlebar.ReleaseButtons();

            // Pass caption-related nonclient messages to the parent window.
            // Make sure to do this for the HTTOP, which is the top resize
            // border, so we can resize the window on the top.
            auto parentWindow{ GetHandle() };
            return SendMessage(parentWindow, message, wparam, lparam);
        }
        case HTMINBUTTON:
        case HTMAXBUTTON:
        case HTCLOSE:
            _titlebar.HoverButton(static_cast<winrt::TerminalApp::CaptionButton>(wparam));
            break;
        default:
            _titlebar.ReleaseButtons();
        }

        // If we haven't previously asked for mouse tracking, request mouse
        // tracking. We need to do this so we can get the WM_NCMOUSELEAVE
        // message when the mouse leave the titlebar. Otherwise, we won't always
        // get that message (especially if the user moves the mouse _real
        // fast_).
        if (!_trackingMouse &&
            (wparam == HTMINBUTTON || wparam == HTMAXBUTTON || wparam == HTCLOSE))
        {
            TRACKMOUSEEVENT ev{};
            ev.cbSize = sizeof(TRACKMOUSEEVENT);
            // TME_NONCLIENT is absolutely critical here. In my experimentation,
            // we'd get WM_MOUSELEAVE messages after just a HOVER_DEFAULT
            // timeout even though we're not requesting TME_HOVER, which kinda
            // ruined the whole point of this.
            ev.dwFlags = TME_LEAVE | TME_NONCLIENT;
            ev.hwndTrack = _dragBarWindow.get();
            ev.dwHoverTime = HOVER_DEFAULT; // we don't _really_ care about this.
            LOG_IF_WIN32_BOOL_FALSE(TrackMouseEvent(&ev));
            _trackingMouse = true;
        }
        break;

    case WM_NCMOUSELEAVE:
    case WM_MOUSELEAVE:
        // When the mouse leaves the drag rect, make sure to dismiss any hover.
        _titlebar.ReleaseButtons();
        _trackingMouse = false;
        break;

    // NB: *Shouldn't be forwarding these* when they're not over the caption
    // because they can inadvertently take action using the system's default
    // metrics instead of our own.
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        // Manual handling for mouse clicks in the drag bar. If it's in a
        // caption button, then tell the titlebar to "press" the button, which
        // should change its visual state.
        //
        // If it's not in a caption button, then just forward the message along
        // to the root HWND. Make sure to do this for the HTTOP, which is the
        // top resize border.
        switch (wparam)
        {
        case HTTOP:
        case HTCAPTION:
        {
            // Pass caption-related nonclient messages to the parent window.
            auto parentWindow{ GetHandle() };
            return SendMessage(parentWindow, message, wparam, lparam);
        }
        // The buttons won't work as you'd expect; we need to handle those
        // ourselves.
        case HTMINBUTTON:
        case HTMAXBUTTON:
        case HTCLOSE:
            _titlebar.PressButton(static_cast<winrt::TerminalApp::CaptionButton>(wparam));
            break;
        }
        return 0;

    case WM_NCLBUTTONUP:
        // Manual handling for mouse RELEASES in the drag bar. If it's in a
        // caption button, then manually handle what we'd expect for that button.
        //
        // If it's not in a caption button, then just forward the message along
        // to the root HWND.
        switch (wparam)
        {
        case HTTOP:
        case HTCAPTION:
        {
            // Pass caption-related nonclient messages to the parent window.
            // The buttons won't work as you'd expect; we need to handle those ourselves.
            auto parentWindow{ GetHandle() };
            return SendMessage(parentWindow, message, wparam, lparam);
        }
        break;

        // If we do find a button, then tell the titlebar to raise the same
        // event that would be raised if it were "tapped"
        case HTMINBUTTON:
        case HTMAXBUTTON:
        case HTCLOSE:
            _titlebar.ReleaseButtons();
            _titlebar.ClickButton(static_cast<winrt::TerminalApp::CaptionButton>(wparam));
            break;
        }
        return 0;

    // Make sure to pass along right-clicks in this region to our parent window
    // - we don't need to handle these.
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCRBUTTONUP:
        auto parentWindow{ GetHandle() };
        return SendMessage(parentWindow, message, wparam, lparam);
    }

    return DefWindowProc(_dragBarWindow.get(), message, wparam, lparam);
}

// Method Description:
// - Resizes and shows/hides the drag bar input sink window.
//   This window is used to capture clicks on the non-client area.
void NonClientIslandWindow::_ResizeDragBarWindow() noexcept
{
    const til::rect rect{ _GetDragAreaRect() };
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): while the auto-hiding row is away (hidden, or still sliding) the
    // drag bar window must not exist on screen: it is a real HWND above the island, so left where the row was it would keep
    // the caption buttons, the window drag and the top resize band over the terminal's first rows, invisibly. It comes back
    // when a slide in has finished (_AutoHideSlideFinished).
    if (_IsTitlebarVisible() && !_AutoHideRowAway() && rect.size().area() > 0)
    {
        SetWindowPos(_dragBarWindow.get(),
                     HWND_TOP,
                     rect.left,
                     rect.top + _GetTopBorderHeight(),
                     rect.width(),
                     rect.height(),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetLayeredWindowAttributes(_dragBarWindow.get(), 0, 255, LWA_ALPHA);
    }
    else
    {
        SetWindowPos(_dragBarWindow.get(), HWND_BOTTOM, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE);
    }
}

// Method Description:
// - Called when the app's size changes. When that happens, the size of the drag
//   bar may have changed. If it has, we'll need to update the WindowRgn of the
//   interop window.
// Arguments:
// - <unused>
// Return Value:
// - <none>
void NonClientIslandWindow::_OnDragBarSizeChanged(winrt::Windows::Foundation::IInspectable /*sender*/,
                                                  winrt::Windows::UI::Xaml::SizeChangedEventArgs /*eventArgs*/)
{
    _ResizeDragBarWindow();
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): a hidden row is parked exactly its own height above the island.
    // When its height changes (a DPI change, the cog's tab height, a title font), park it again at the new height, or a
    // taller row would show its bottom edge over the terminal. A sliding row is re-pinned at its slide's end anyway.
    if (_autoHideEnabled && _titlebar && !_autoHide.shown && !_autoHideSliding)
    {
        try
        {
            _titlebar.Translation(float3{ 0.0f, -static_cast<float>(_titlebar.ActualHeight()), 0.0f });
        }
        catch (...)
        {
            LogTabRowCaught(L"re-parking the hidden row after a size change");
        }
    }
}

void NonClientIslandWindow::OnAppInitialized()
{
    IslandWindow::OnAppInitialized();
}

void NonClientIslandWindow::Initialize()
{
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): a warm init builds a NEW root grid and title bar below, so any
    // auto-hide state belongs to the old ones: forget it (timers off, the old handlers' link cut). The page asks again
    // (SetTitlebarAutoHide) once its new content is laid out, and that call floats the NEW title bar.
    _AutoHideShutdown();
    IslandWindow::Initialize();

    _UpdateFrameMargins();

    // Set up our grid of content. We'll use _rootGrid as our root element.
    // There will be two children of this grid - the TitlebarControl, and the
    // "client content"
    _rootGrid.Children().Clear();
    Controls::RowDefinition titlebarRow{};
    Controls::RowDefinition contentRow{};
    titlebarRow.Height(GridLengthHelper::Auto());

    _rootGrid.RowDefinitions().Clear();
    _rootGrid.RowDefinitions().Append(titlebarRow);
    _rootGrid.RowDefinitions().Append(contentRow);

    // Create our titlebar control
    _titlebar = winrt::TerminalApp::TitlebarControl{ reinterpret_cast<uint64_t>(GetHandle()) };
    _dragBar = _titlebar.DragBar();

    _callbacks.dragBarSizeChanged = _dragBar.SizeChanged(winrt::auto_revoke, { this, &NonClientIslandWindow::_OnDragBarSizeChanged });
    _callbacks.rootGridSizeChanged = _rootGrid.SizeChanged(winrt::auto_revoke, { this, &NonClientIslandWindow::_OnDragBarSizeChanged });

    _rootGrid.Children().Append(_titlebar);

    Controls::Grid::SetRow(_titlebar, 0);

    // GH#3440 - When the titlebar is loaded (officially added to our UI tree),
    // then make sure to update its visual state to reflect if we're in the
    // maximized state on launch.
    _callbacks.titlebarLoaded = _titlebar.Loaded(winrt::auto_revoke, [this](auto&&, auto&&) { _OnMaximizeChange(); });

    // LOAD BEARING: call _ResizeDragBarWindow to update the position of our
    // XAML island to reflect our current bounds. In the case of a "warm init"
    // (i.e. re-using an existing window), we need to manually update the
    // island's position to fill the new window bounds.
    _ResizeDragBarWindow();
}

// Method Description:
// - Set the content of the "client area" of our window to the given content.
// Arguments:
// - content: the new UI element to use as the client content
// Return Value:
// - <none>
void NonClientIslandWindow::SetContent(winrt::Windows::UI::Xaml::UIElement content)
{
    _rootGrid.Children().Append(content);

    // SetRow only works on FrameworkElement's, so cast it to a FWE before
    // calling. We know that our content is a Grid, so we don't need to worry
    // about this.
    const auto fwe = content.try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
    if (fwe)
    {
        Controls::Grid::SetRow(fwe, 1);
    }
}

// Method Description:
// - Set the content of the "titlebar area" of our window to the given content.
// Arguments:
// - content: the new UI element to use as the titlebar content
// Return Value:
// - <none>
void NonClientIslandWindow::SetTitlebarContent(winrt::Windows::UI::Xaml::UIElement content)
{
    _titlebar.Content(content);

    // GH#4288 - add a SizeChanged handler to this content. It's possible that
    // this element's size will change after the dragbar's. When that happens,
    // the drag bar won't send another SizeChanged event, because the dragbar's
    // _size_ didn't change, only its position.
    const auto fwe = content.try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
    if (fwe)
    {
        fwe.SizeChanged({ this, &NonClientIslandWindow::_OnDragBarSizeChanged });
    }
}

// Method Description:
// - This method computes the height of the little border above the title bar
//   and returns it. If the border is disabled, then this method will return 0.
// Return Value:
// - the height of the border above the title bar or 0 if it's disabled
int NonClientIslandWindow::_GetTopBorderHeight() const noexcept
{
    // No border when maximized or fullscreen.
    // Yet we still need it in the focus mode to allow dragging (GH#7012)
    if (_isMaximized || _fullscreen)
    {
        return 0;
    }

    return topBorderVisibleHeight;
}

til::rect NonClientIslandWindow::_GetDragAreaRect() const noexcept
{
    if (_dragBar && _dragBar.Visibility() == Visibility::Visible)
    {
        const auto scale = GetCurrentDpiScale();
        const auto transform = _dragBar.TransformToVisual(_rootGrid);

        // GH#9443: Previously, we'd only extend the drag bar from the left of
        // the tabs to the right of the caption buttons. Now, we're extending it
        // all the way to the right side of the window, covering the caption
        // buttons. We'll manually handle input to those buttons, to make it
        // seem like they're still getting XAML input. We do this so we can get
        // snap layout support for the maximize button.
        const auto logicalDragBarRect = winrt::Windows::Foundation::Rect{
            0.0f,
            0.0f,
            static_cast<float>(_rootGrid.ActualWidth()),
            static_cast<float>(_dragBar.ActualHeight())
        };

        const auto clientDragBarRect = transform.TransformBounds(logicalDragBarRect);

        // Make sure to trim the right side of the rectangle, so that it doesn't
        // hang off the right side of the root window. This normally wouldn't
        // matter, but UIA will still think its bounds can extend past the right
        // of the parent HWND.
        //
        // x here is the width of the tabs.
        const auto x = gsl::narrow_cast<til::CoordType>(clientDragBarRect.X * scale);

        return {
            x,
            gsl::narrow_cast<til::CoordType>(clientDragBarRect.Y * scale),
            gsl::narrow_cast<til::CoordType>((clientDragBarRect.Width + clientDragBarRect.X) * scale) - x,
            gsl::narrow_cast<til::CoordType>((clientDragBarRect.Height + clientDragBarRect.Y) * scale),
        };
    }

    return {};
}

// Method Description:
// - Called when the size of the window changes for any reason. Updates the
//   XAML island to match our new sizing and also updates the maximize icon
//   if the window went from maximized to restored or the opposite.
void NonClientIslandWindow::OnSize(const UINT width, const UINT height)
{
    _UpdateMaximizedState();

    if (_interopWindowHandle)
    {
        _UpdateIslandPosition(width, height);
    }

    // GH#11367: We need to do this,
    // otherwise the titlebar may still be partially visible
    // when we move between different DPI monitors.
    RefreshCurrentDPI();
    _UpdateFrameMargins();

    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): a size, maximize, minimize or DPI change moves the top edge the
    // rule measures from, or makes the window unable to show the row (minimized): look again now.
    if (_autoHideEnabled)
    {
        _AutoHideEvaluate(L"size");
    }
}

// Method Description:
// - Checks if the window has been maximized or restored since the last time.
//   If it has been maximized or restored, then it updates the _isMaximized
//   flags and notifies of the change by calling
//   NonClientIslandWindow::_OnMaximizeChange.
void NonClientIslandWindow::_UpdateMaximizedState()
{
    const auto windowStyle = GetWindowStyle(_window.get());
    const auto newIsMaximized = WI_IsFlagSet(windowStyle, WS_MAXIMIZE);

    if (_isMaximized != newIsMaximized)
    {
        _isMaximized = newIsMaximized;
        _OnMaximizeChange();
    }
}

// Method Description:
// - Called when the windows goes from restored to maximized or from
//   maximized to restored. Updates the maximize button's icon and the frame
//   margins.
void NonClientIslandWindow::_OnMaximizeChange() noexcept
{
    if (_titlebar)
    {
        const auto windowStyle = GetWindowStyle(_window.get());
        const auto isIconified = WI_IsFlagSet(windowStyle, WS_ICONIC);

        const auto state = _isMaximized ? winrt::TerminalApp::WindowVisualState::WindowVisualStateMaximized :
                           isIconified  ? winrt::TerminalApp::WindowVisualState::WindowVisualStateIconified :
                                          winrt::TerminalApp::WindowVisualState::WindowVisualStateNormal;

        try
        {
            _titlebar.SetWindowVisualState(state);
        }
        CATCH_LOG();
    }

    // no frame margin when maximized
    _UpdateFrameMargins();
}

// Method Description:
// - Called when the size of the window changes for any reason. Updates the
//   sizes of our child XAML Islands to match our new sizing.
void NonClientIslandWindow::_UpdateIslandPosition(const UINT windowWidth, const UINT windowHeight)
{
    const auto originalTopHeight = _GetTopBorderHeight();
    // GH#7422
    // !! BODGY !!
    //
    // For inexplicable reasons, the top row of pixels on our tabs, new tab
    // button, and caption buttons is totally un-clickable. The mouse simply
    // refuses to interact with them. So when we're maximized, on certain
    // monitor configurations, this results in the top row of pixels not
    // reacting to clicks at all. To obey Fitt's Law, we're gonna shift
    // the entire island up one pixel. That will result in the top row of pixels
    // in the window actually being the _second_ row of pixels for those
    // buttons, which will make them clickable. It's perhaps not the right fix,
    // but it works.
    // _GetTopBorderHeight() returns 0 when we're maximized.
    const auto topBorderHeight = (originalTopHeight == 0) ? -1 : originalTopHeight;

    const til::point newIslandPos = { 0, topBorderHeight };

    winrt::check_bool(SetWindowPos(_interopWindowHandle,
                                   HWND_BOTTOM,
                                   newIslandPos.x,
                                   newIslandPos.y,
                                   windowWidth,
                                   windowHeight - topBorderHeight,
                                   SWP_SHOWWINDOW | SWP_NOACTIVATE));

    // This happens when we go from maximized to restored or the opposite
    // because topBorderHeight changes.
    if (!_oldIslandPos.has_value() || _oldIslandPos.value() != newIslandPos)
    {
        // The drag bar's position changed compared to the client area because
        // the island moved but we will not be notified about this in the
        // NonClientIslandWindow::OnDragBarSizeChanged method because this
        // method is only called when the position of the drag bar changes
        // **inside** the island which is not the case here.
        _ResizeDragBarWindow();

        _oldIslandPos = { newIslandPos };
    }
}

// Method Description:
// - Returns the height of the little space at the top of the window used to
//   resize the window.
// Return Value:
// - the height of the window's top resize handle
int NonClientIslandWindow::_GetResizeHandleHeight() const noexcept
{
    // there isn't a SM_CYPADDEDBORDER for the Y axis
    return ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, _currentDpi) +
           ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, _currentDpi);
}

// Method Description:
// - Responds to the WM_NCCALCSIZE message by calculating and creating the new
//   window frame.
[[nodiscard]] LRESULT NonClientIslandWindow::_OnNcCalcSize(const WPARAM wParam, const LPARAM lParam) noexcept
{
    if (!wParam)
    {
        return 0;
    }

    auto params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);

    // Store the original top before the default window proc applies the
    // default frame.
    const auto originalTop = params->rgrc[0].top;

    const auto originalSize = params->rgrc[0];

    // apply the default frame
    const auto ret = DefWindowProc(_window.get(), WM_NCCALCSIZE, wParam, lParam);
    if (ret != 0)
    {
        return ret;
    }

    auto newSize = params->rgrc[0];
    // Re-apply the original top from before the size of the default frame was applied.
    newSize.top = originalTop;

    // WM_NCCALCSIZE is called before WM_SIZE
    _UpdateMaximizedState();

    // We don't need this correction when we're fullscreen. We will have the
    // WS_POPUP size, so we don't have to worry about borders, and the default
    // frame will be fine.
    if (_isMaximized && !_fullscreen)
    {
        // When a window is maximized, its size is actually a little bit more
        // than the monitor's work area. The window is positioned and sized in
        // such a way that the resize handles are outside of the monitor and
        // then the window is clipped to the monitor so that the resize handle
        // do not appear because you don't need them (because you can't resize
        // a window when it's maximized unless you restore it).
        newSize.top += _GetResizeHandleHeight();
    }

    // GH#1438 - Attempt to detect if there's an autohide taskbar, and if there
    // is, reduce our size a bit on the side with the taskbar, so the user can
    // still mouse-over the taskbar to reveal it.
    // GH#5209 - make sure to use MONITOR_DEFAULTTONEAREST, so that this will
    // still find the right monitor even when we're restoring from minimized.
    auto hMon = MonitorFromWindow(_window.get(), MONITOR_DEFAULTTONEAREST);
    if (hMon && (_isMaximized || _fullscreen))
    {
        MONITORINFO monInfo{ 0 };
        monInfo.cbSize = sizeof(MONITORINFO);
        GetMonitorInfo(hMon, &monInfo);

        // First, check if we have an auto-hide taskbar at all:
        APPBARDATA autohide{ 0 };
        autohide.cbSize = sizeof(autohide);
        auto state = (UINT)SHAppBarMessage(ABM_GETSTATE, &autohide);
        if (WI_IsFlagSet(state, ABS_AUTOHIDE))
        {
            // This helper can be used to determine if there's an auto-hide
            // taskbar on the given edge of the monitor we're currently on.
            auto hasAutohideTaskbar = [&monInfo](const UINT edge) -> bool {
                APPBARDATA data{ 0 };
                data.cbSize = sizeof(data);
                data.uEdge = edge;
                data.rc = monInfo.rcMonitor;
                auto hTaskbar = (HWND)SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data);
                return hTaskbar != nullptr;
            };

            const auto onTop = hasAutohideTaskbar(ABE_TOP);
            const auto onBottom = hasAutohideTaskbar(ABE_BOTTOM);
            const auto onLeft = hasAutohideTaskbar(ABE_LEFT);
            const auto onRight = hasAutohideTaskbar(ABE_RIGHT);

            // If there's a taskbar on any side of the monitor, reduce our size
            // a little bit on that edge.
            //
            // Note to future code archaeologists:
            // This doesn't seem to work for fullscreen on the primary display.
            // However, testing a bunch of other apps with fullscreen modes
            // and an auto-hiding taskbar has shown that _none_ of them
            // reveal the taskbar from fullscreen mode. This includes Edge,
            // Firefox, Chrome, Sublime Text, PowerPoint - none seemed to
            // support this.
            //
            // This does however work fine for maximized.
            if (onTop)
            {
                // Peculiarly, when we're fullscreen,
                newSize.top += AutohideTaskbarSize;
            }
            if (onBottom)
            {
                newSize.bottom -= AutohideTaskbarSize;
            }
            if (onLeft)
            {
                newSize.left += AutohideTaskbarSize;
            }
            if (onRight)
            {
                newSize.right -= AutohideTaskbarSize;
            }
        }
    }

    params->rgrc[0] = newSize;

    return 0;
}

// Method Description:
// - Hit test the frame for resizing and moving.
// Arguments:
// - ptMouse: the mouse point being tested, in absolute (NOT WINDOW) coordinates.
// Return Value:
// - one of the values from
//   https://docs.microsoft.com/en-us/windows/desktop/inputdev/wm-nchittest#return-value
//   corresponding to the area of the window that was hit
[[nodiscard]] LRESULT NonClientIslandWindow::_OnNcHitTest(POINT ptMouse) const noexcept
{
    // This will handle the left, right and bottom parts of the frame because
    // we didn't change them.
    LPARAM lParam = MAKELONG(ptMouse.x, ptMouse.y);
    const auto originalRet = DefWindowProc(_window.get(), WM_NCHITTEST, 0, lParam);

    if (originalRet != HTCLIENT)
    {
        // If we're the quake window, suppress resizing on any side except the
        // bottom. I don't believe that this actually works on the top. That's
        // handled below.
        if (IsQuakeWindow())
        {
            switch (originalRet)
            {
            case HTBOTTOMRIGHT:
            case HTRIGHT:
            case HTTOPRIGHT:
            case HTTOP:
            case HTTOPLEFT:
            case HTLEFT:
            case HTBOTTOMLEFT:
                return HTCLIENT;
            }
        }
        return originalRet;
    }

    // At this point, we know that the cursor is inside the client area. Upstream
    // assumed this could only be the little top border or the drag bar — because
    // "the XAML Island handles WM_NCHITTEST on its own". That assumption only holds
    // while the XAML content under the cursor is HIT-TESTABLE. Any content with a
    // null/transparent Background is NOT, so the island returns HTTRANSPARENT and
    // the click falls through to here — and the old code then returned HTCAPTION
    // for it. That is why a click on a transparent spot of a pane (e.g. the
    // Agentmaster Manager tab, or the gaps around a TextBlock) would DRAG the
    // window, and a right-click there would open the system/caption menu.

    RECT rcWindow;
    winrt::check_bool(::GetWindowRect(_window.get(), &rcWindow));

    const auto resizeBorderHeight = _GetResizeHandleHeight();
    const auto isOnResizeBorder = ptMouse.y < rcWindow.top + resizeBorderHeight;

    // the top of the drag bar is used to resize the window
    if (!_isMaximized && isOnResizeBorder)
    {
        // However, if we're the quake window, then just return HTCAPTION so we
        // don't get a resize handle on the top.
        return IsQuakeWindow() ? HTCAPTION : HTTOP;
    }

    // Agentmaster: the caption / window-drag region is the TITLEBAR ONLY. Below the
    // titlebar is content (panes), so a click there that fell through transparent
    // XAML must be HTCLIENT — inert — instead of HTCAPTION, which would otherwise let
    // any pane whose content isn't opaque drag the window or pop the system menu.
    // This makes the drag region match what the user actually sees as the title bar,
    // and it is defense-in-depth: it holds no matter what background (or none) a
    // pane's content sets, so we don't have to chase an opaque Background onto every
    // element. Real titlebar dragging is unaffected — it is served by the drag-bar
    // input-sink child window and by the HTCAPTION below for the band itself; the
    // resize borders were handled above and by DefWindowProc.
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): an auto-hidden row is not a title bar — the terminal's first rows
    // are there — so while it is away (hidden or sliding) the band below the frame border is content too.
    const auto titlebarBottom = rcWindow.top +
                                _GetTopBorderHeight() +
                                ((_titlebar && !_AutoHideRowAway()) ? gsl::narrow_cast<long>(_titlebar.ActualHeight() * GetCurrentDpiScale()) : 0);
    if (ptMouse.y >= titlebarBottom)
    {
        return HTCLIENT;
    }

    return HTCAPTION;
}

// Method Description:
// - Sets the cursor to the sizing cursor when we hit-test the top sizing border.
//   We need to do this because we've covered it up with a child window.
[[nodiscard]] LRESULT NonClientIslandWindow::_OnSetCursor(WPARAM wParam, LPARAM lParam) const noexcept
{
    if (LOWORD(lParam) == HTCLIENT)
    {
        // Get the cursor position from the _last message_ and not from
        // `GetCursorPos` (which returns the cursor position _at the
        // moment_) because if we're lagging behind the cursor's position,
        // we still want to get the cursor position that was associated
        // with that message at the time it was sent to handle the message
        // correctly.
        const auto screenPtLparam{ GetMessagePos() };
        const auto hitTest{ SendMessage(GetHandle(), WM_NCHITTEST, 0, screenPtLparam) };
        if (hitTest == HTTOP)
        {
            // We have to set the vertical resize cursor manually on
            // the top resize handle because Windows thinks that the
            // cursor is on the client area because it asked the asked
            // the drag window with `WM_NCHITTEST` and it returned
            // `HTCLIENT`.
            // We don't want to modify the drag window's `WM_NCHITTEST`
            // handling to return `HTTOP` because otherwise, the system
            // would resize the drag window instead of the top level
            // window!
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return TRUE;
        }
        else
        {
            // reset cursor
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
    }

    return DefWindowProc(GetHandle(), WM_SETCURSOR, wParam, lParam);
}
// Method Description:
// - Get the dimensions of our non-client area, as a rect where each component
//   represents that side.
// - The .left will be a negative number, to represent that the actual side of
//   the non-client area is outside the border of our window. It's roughly 8px (
//   * DPI scaling) to the left of the visible border.
// - The .right component will be positive, indicating that the nonclient border
//   is in the positive-x direction from the edge of our client area.
// - This DOES NOT include our titlebar! It's in the client area for us.
// Arguments:
// - dpi: the scaling that we should use to calculate the border sizes.
// Return Value:
// - a til::rect whose components represent the margins of the nonclient area,
//   relative to the client area.
til::rect NonClientIslandWindow::GetNonClientFrame(UINT dpi) const noexcept
{
    const auto windowStyle = static_cast<DWORD>(GetWindowLong(_window.get(), GWL_STYLE));
    til::rect islandFrame;

    // If we failed to get the correct window size for whatever reason, log
    // the error and go on. We'll use whatever the control proposed as the
    // size of our window, which will be at least close.
    LOG_IF_WIN32_BOOL_FALSE(AdjustWindowRectExForDpi(islandFrame.as_win32_rect(), windowStyle, false, 0, dpi));

    islandFrame.top = -topBorderVisibleHeight;
    return islandFrame;
}

// Method Description:
// - Gets the difference between window and client area size.
// Arguments:
// - dpi: dpi of a monitor on which the window is placed
// Return Value
// - The size difference
til::size NonClientIslandWindow::GetTotalNonClientExclusiveSize(UINT dpi) const noexcept
{
    const auto islandFrame{ GetNonClientFrame(dpi) };
    const auto scale = GetCurrentDpiScale();

    // If we have a titlebar, this is being called after we've initialized, and
    // we can just ask that titlebar how big it wants to be.
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): a FLOATING auto-hiding title bar lies over the content and takes
    // none of the window's height, so it is no part of the non-client size (the window sizing and the snap-to-grid math). A
    // docked one (held out while the Agent Manager page shows) takes its row again, like the bar without the setting.
    const auto titleBarHeight = (_titlebar && (!_autoHideEnabled || _autoHideHold)) ? static_cast<LONG>(_titlebar.ActualHeight()) * scale : 0;

    return {
        islandFrame.right - islandFrame.left,
        islandFrame.bottom - islandFrame.top + static_cast<til::CoordType>(titleBarHeight)
    };
}

// Method Description:
// - Updates the borders of our window frame, using DwmExtendFrameIntoClientArea.
// Arguments:
// - <none>
// Return Value:
// - the HRESULT returned by DwmExtendFrameIntoClientArea.
void NonClientIslandWindow::_UpdateFrameMargins() const noexcept
{
    MARGINS margins = { 0, 0, 0, 0 };

    // GH#603: When we're in Focus Mode, hide the titlebar, by setting it to a single
    // pixel tall. Otherwise, the titlebar will be visible underneath controls with
    // vintage opacity set.
    //
    // We can't set it to all 0's unfortunately.
    if (_borderless)
    {
        margins.cyTopHeight = 1;
    }
    else if (_GetTopBorderHeight() != 0)
    {
        RECT frame = {};
        winrt::check_bool(::AdjustWindowRectExForDpi(&frame, GetWindowStyle(_window.get()), FALSE, 0, _currentDpi));

        // We removed the whole top part of the frame (see handling of
        // WM_NCCALCSIZE) so the top border is missing now. We add it back here.
        // Note #1: You might wonder why we don't remove just the title bar instead
        //  of removing the whole top part of the frame and then adding the little
        //  top border back. I tried to do this but it didn't work: DWM drew the
        //  whole title bar anyways on top of the window. It seems that DWM only
        //  wants to draw either nothing or the whole top part of the frame.
        // Note #2: For some reason if you try to set the top margin to just the
        //  top border height (what we want to do), then there is a transparency
        //  bug when the window is inactive, so I've decided to add the whole top
        //  part of the frame instead and then we will hide everything that we
        //  don't need (that is, the whole thing but the little 1 pixel wide border
        //  at the top) in the WM_PAINT handler. This eliminates the transparency
        //  bug and it's what a lot of Win32 apps that customize the title bar do
        //  so it should work fine.
        //
        // Notes #3 (circa late 2022): We want to make some changes here to
        // support Mica. This introduces some complications.
        // - If we leave the titlebar visible AT ALL, then a transparent
        //   titlebar (theme.tabRow.background:#ff00ff00 for example) will allow
        //   the DWM titlebar to be visible, underneath our content. EVEN MORE
        //   SO: Mica + "show accent color on title bars" will _always_ show the
        //   accent-colored strip of the titlebar, even on top of the Mica.
        // - It _seems_ like we can just set this to 0, and have it work. You'd
        //   be wrong. On Windows 10, setting this to 0 will cause the topmost
        //   pixel of our window to be just a little darker than the rest of the
        //   frame. So ONLY set this to 0 when the user has explicitly asked for
        //   Mica. Though it won't do anything on Windows 10, they should be
        //   able to opt back out of having that weird dark pixel.
        // - This is LOAD-BEARING. By having the titlebar a totally empty rect,
        //   DWM will know that we don't have the traditional titlebar, and will
        //   use NCHITTEST to determine where to place the Snap Flyout. The drag
        //   rect will handle that.

        margins.cyTopHeight = (_useMica || _titlebarOpacity < 1.0) ? 0 : -frame.top;
    }

    // Extend the frame into the client area. microsoft/terminal#2735 - Just log
    // the failure here, don't crash. If DWM crashes for any reason, calling
    // THROW_IF_FAILED() will cause us to take a trip upstate. Just log, and
    // we'll fix ourselves when DWM comes back.
    LOG_IF_FAILED(DwmExtendFrameIntoClientArea(_window.get(), &margins));
}

// Method Description:
// - Handle window messages from the message loop.
// Arguments:
// - message: A window message ID identifying the message.
// - wParam: The contents of this parameter depend on the value of the message parameter.
// - lParam: The contents of this parameter depend on the value of the message parameter.
// Return Value:
// - The return value is the result of the message processing and depends on the
//   message sent.
[[nodiscard]] LRESULT NonClientIslandWindow::MessageHandler(UINT const message,
                                                            WPARAM const wParam,
                                                            LPARAM const lParam) noexcept
{
    switch (message)
    {
    case WM_NCACTIVATE:
    {
        const bool activated = LOWORD(wParam) != 0;
        _titlebar.Focused(activated);
        break;
    }
    case WM_SETCURSOR:
        return _OnSetCursor(wParam, lParam);
    case WM_DISPLAYCHANGE:
        // GH#4166: When the DPI of the monitor changes out from underneath us,
        // resize our drag bar, to reflect its newly scaled size.
        _ResizeDragBarWindow();
        return 0;
    case WM_NCCALCSIZE:
        return _OnNcCalcSize(wParam, lParam);
    case WM_NCHITTEST:
        return _OnNcHitTest({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
    case WM_PAINT:
        return _OnPaint();
    case WM_NCRBUTTONUP:
        // The `DefWindowProc` function doesn't open the system menu for some
        // reason so we have to do it ourselves.
        if (wParam == HTCAPTION)
        {
            OpenSystemMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        break;
    case WM_TIMER:
        // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): the auto-hiding title bar's two timers. Any other id (the
        // taskbar frame's) belongs to IslandWindow below.
        if (wParam == kTabRowWatchTimerId)
        {
            _AutoHideEvaluate(L"watch");
            return 0;
        }
        if (wParam == kTabRowSlideEndTimerId)
        {
            _AutoHideSlideFinished();
            return 0;
        }
        break;
    case WM_NCMOUSEMOVE:
    case WM_MOUSEMOVE:
        // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): the pointer over the window ITSELF, not the island — on a
        // restored window that is the one-pixel frame border row above the island, the first row of the top edge band. No
        // XAML event sees it, so a rest that starts there starts here. The message then goes on to its usual handling.
        _AutoHideOnWindowPointer();
        break;
    }

    return IslandWindow::MessageHandler(message, wParam, lParam);
}

// Method Description:
// - This method is called when the window receives the WM_PAINT message.
// - It paints the client area with the color of the title bar to hide the
//   system's title bar behind the XAML Islands window during a resize.
//   Indeed, the XAML Islands window doesn't resize at the same time than
//   the top level window
//   (see https://github.com/microsoft/microsoft-ui-xaml/issues/759).
// Return Value:
// - The value returned from the window proc.
[[nodiscard]] LRESULT NonClientIslandWindow::_OnPaint() noexcept
{
    if (!_titlebar)
    {
        return 0;
    }

    PAINTSTRUCT ps{ 0 };
    const auto hdc = wil::BeginPaint(_window.get(), &ps);
    if (!hdc)
    {
        return 0;
    }

    const auto topBorderHeight = _GetTopBorderHeight();

    if (ps.rcPaint.top < topBorderHeight)
    {
        auto rcTopBorder = ps.rcPaint;
        rcTopBorder.bottom = topBorderHeight;

        // To show the original top border, we have to paint on top of it with
        // the alpha component set to 0. This page recommends to paint the area
        // in black using the stock BLACK_BRUSH to do this:
        // https://docs.microsoft.com/en-us/windows/win32/dwm/customframe#extending-the-client-frame
        ::FillRect(hdc.get(), &rcTopBorder, GetStockBrush(BLACK_BRUSH));
    }

    if (ps.rcPaint.bottom > topBorderHeight)
    {
        auto rcRest = ps.rcPaint;
        rcRest.top = topBorderHeight;

        const auto backgroundBrush = _titlebar.Background();
        const auto backgroundSolidBrush = backgroundBrush.try_as<Media::SolidColorBrush>();
        const auto backgroundAcrylicBrush = backgroundBrush.try_as<Media::AcrylicBrush>();

        til::color backgroundColor = Colors::Black();
        if (backgroundSolidBrush)
        {
            backgroundColor = backgroundSolidBrush.Color();
        }
        else if (backgroundAcrylicBrush)
        {
            backgroundColor = backgroundAcrylicBrush.FallbackColor();
        }

        if (!_backgroundBrush || backgroundColor != _backgroundBrushColor)
        {
            // Create brush for titlebar color.
            _backgroundBrush = wil::unique_hbrush(CreateSolidBrush(backgroundColor));
        }

        // To hide the original title bar, we have to paint on top of it with
        // the alpha component set to 255. This is a hack to do it with GDI.
        // See NonClientIslandWindow::_UpdateFrameMargins for more information.
        HDC opaqueDc;
        BP_PAINTPARAMS params = { sizeof(params), BPPF_NOCLIP | BPPF_ERASE };
        auto buf = BeginBufferedPaint(hdc.get(), &rcRest, BPBF_TOPDOWNDIB, &params, &opaqueDc);
        if (!buf || !opaqueDc)
        {
            // MSFT:34673647 - BeginBufferedPaint can fail, but it probably
            // shouldn't bring the whole Terminal down with it. So don't
            // throw_last_error here.
            LOG_LAST_ERROR();
            return 0;
        }

        ::FillRect(opaqueDc, &rcRest, _backgroundBrush.get());
        ::BufferedPaintSetAlpha(buf, nullptr, 255);
        ::EndBufferedPaint(buf, TRUE);
    }

    return 0;
}

// Method Description:
// - Called when the app wants to change its theme. We'll update the frame
//   theme to match the new theme.
// Arguments:
// - requestedTheme: the ElementTheme to use as the new theme for the UI
// Return Value:
// - <none>
void NonClientIslandWindow::OnApplicationThemeChanged(const ElementTheme& requestedTheme)
{
    IslandWindow::OnApplicationThemeChanged(requestedTheme);

    _theme = requestedTheme;
}

// Method Description:
// - Enable or disable borderless mode. When entering borderless mode, we'll
//   need to manually hide the entire titlebar.
// - See also IslandWindow::_SetIsBorderless, which does similar, but different work.
// Arguments:
// - borderlessEnabled: If true, we're entering borderless mode. If false, we're leaving.
// Return Value:
// - <none>
void NonClientIslandWindow::_SetIsBorderless(const bool borderlessEnabled)
{
    _borderless = borderlessEnabled;

    // Explicitly _don't_ call IslandWindow::_SetIsBorderless. That version will
    // change the window styles appropriately for the window with the default
    // titlebar, but for the tabs-in-titlebar mode, we can just get rid of the
    // title bar entirely.

    if (_titlebar)
    {
        _titlebar.Visibility(_IsTitlebarVisible() ? Visibility::Visible : Visibility::Collapsed);
    }

    // Update the margins when entering/leaving focus mode, so we can prevent
    // the titlebar from showing through transparent terminal controls
    _UpdateFrameMargins();

    // GH#4224 - When the auto-hide taskbar setting is enabled, then we don't
    // always get another window message to trigger us to remove the drag bar.
    // So, make sure to update the size of the drag region here, so that it
    // _definitely_ goes away.
    _ResizeDragBarWindow();

    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): focus mode hides the whole title bar, so an auto-hiding row
    // forgets its state on the way in and starts hidden on the way out (the rule's "a window that cannot show the row").
    if (_autoHideEnabled)
    {
        _AutoHideEvaluate(L"focus mode");
    }

    // Resize the window, with SWP_FRAMECHANGED, to trigger user32 to
    // recalculate the non/client areas
    const til::rect windowPos{ GetWindowRect() };
    SetWindowPos(GetHandle(),
                 HWND_TOP,
                 windowPos.left,
                 windowPos.top,
                 windowPos.width(),
                 windowPos.height(),
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

// Method Description:
// - Enable or disable fullscreen mode. When entering fullscreen mode, we'll
//   need to check whether to hide the titlebar.
// - See also IslandWindow::_SetIsFullscreen, which does additional work.
// Arguments:
// - fullscreenEnabled: If true, we're entering fullscreen mode. If false, we're leaving.
// Return Value:
// - <none>
void NonClientIslandWindow::_SetIsFullscreen(const bool fullscreenEnabled)
{
    IslandWindow::_SetIsFullscreen(fullscreenEnabled);
    _UpdateTitlebarVisibility();
    // GH#4224 - When the auto-hide taskbar setting is enabled, then we don't
    // always get another window message to trigger us to remove the drag bar.
    // So, make sure to update the size of the drag region here, so that it
    // _definitely_ goes away.
    _ResizeDragBarWindow();
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): full screen without tabs hides the title bar (forget, like focus
    // mode); full screen WITH tabs (showTabsFullscreen) keeps auto-hiding it, now against the screen's top edge.
    if (_autoHideEnabled)
    {
        _AutoHideEvaluate(L"full screen");
    }
}

void NonClientIslandWindow::SetShowTabsFullscreen(const bool newShowTabsFullscreen)
{
    IslandWindow::SetShowTabsFullscreen(newShowTabsFullscreen);

    // don't waste time recalculating UI elements if we're not
    // in fullscreen state - this setting doesn't affect other
    // window states
    if (_fullscreen)
    {
        _UpdateTitlebarVisibility();
        // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): the title bar appeared or vanished under full screen.
        if (_autoHideEnabled)
        {
            _AutoHideEvaluate(L"full screen tabs");
        }
    }
}

void NonClientIslandWindow::_UpdateTitlebarVisibility()
{
    if (!_titlebar)
    {
        return;
    }

    const auto showTitlebar = _IsTitlebarVisible();
    _titlebar.Visibility(showTitlebar ? Visibility::Visible : Visibility::Collapsed);
    _titlebar.FullscreenChanged(_fullscreen);
}

// Method Description:
// - Returns true if the titlebar is visible. For borderless mode (aka "focus mode"),
//   this will return false. For fullscreen, this will return false unless the user
//   has enabled fullscreen tabs.
// Arguments:
// - <none>
// Return Value:
// - true iff the titlebar is visible
bool NonClientIslandWindow::_IsTitlebarVisible() const
{
    return !_borderless && (!_fullscreen || _showTabsFullscreen);
}

void NonClientIslandWindow::SetTitlebarBackground(winrt::Windows::UI::Xaml::Media::Brush brush)
{
    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): remember the app's brush, so turning auto-hide off gives it back
    // unchanged; while auto-hide floats the bar over the content it wears an opaque version (_ApplyTitlebarBackground).
    _titlebarBrush = brush;
    _ApplyTitlebarBackground();
}

void NonClientIslandWindow::UseMica(const bool newValue, const double titlebarOpacity)
{
    // Stash internally if we're using Mica. If we aren't, we don't want to
    // totally blow away our titlebar with DwmExtendFrameIntoClientArea,
    // especially on Windows 10
    _useMica = newValue;
    _titlebarOpacity = titlebarOpacity;

    IslandWindow::UseMica(newValue, titlebarOpacity);

    _UpdateFrameMargins();
}

// =====================================================================================================================
// Agentmaster — the AUTO-HIDING TITLE BAR (TAB_STRIP.md "Auto-hiding the tab row"; the rule: AgentMaster/TabRowAutoHide.h).
//
// While the setting is on, the title bar (TitlebarControl: the page's tab row with Home, the tabs, + and ▾, plus the drag
// bar and the minimize / maximize / close buttons) leaves its own grid row and lies over the top of the content (row 1,
// above the page by ZIndex). Out, it covers the terminal's first rows; away, it is parked exactly its own height above the
// island (its Translation), where the island clips it, and takes no input. The terminal never resizes for it — only the
// switch itself changes the content's height, once each way.
//
// What decides "out" or "away" is the pure rule; this half only LOOKS (_AutoHideSample: the cursor against the window's
// top-edge band and the row's band, the buttons, the keyboard focus, the open popups) and ACTS (_AutoHideSlide: a 0.15 s
// composition slide of the Translation, or an instant move while Windows' animation effects are off; the drag bar window
// and the hit test follow). It looks on events — a pointer move near the top, a press, a move over the frame border, a
// size or mode change, the page's setting / hold / peek — and on one timer: every kTabRowWatchMs while the row is out (the
// pointer can leave without an event), and at the exact end of a rest, a hide delay or a peek.
// =====================================================================================================================

namespace
{
    // Agentmaster: does the window under the cursor belong to `top` — `top` itself, one of its children (the island, the
    // drag bar window), or a popup window it owns (a windowed XAML popup: a menu that hangs past the window's edge)? A
    // window of another app, or another Agentmaster window, covering ours is not ours: the row must not react to the
    // pointer over it.
    //   under - WindowFromPoint's answer (may be null).
    //   top   - this window.
    //   returns - true when the pointer is over this window or something of its own.
    //   throws  - nothing.
    bool TabRowWindowBelongsTo(HWND under, const HWND top) noexcept
    {
        for (int guard = 0; under && guard < 8; ++guard)
        {
            const HWND root = ::GetAncestor(under, GA_ROOT);
            if (root == top)
            {
                return true;
            }
            under = root ? ::GetWindow(root, GW_OWNER) : nullptr; // a popup's owner, then that one's root
        }
        return false;
    }
}

void NonClientIslandWindow::SetTitlebarAutoHide(const bool enabled, const bool hold)
{
    if (!_titlebar || !_rootGrid)
    {
        return; // before Initialize: the page asks again after its first layout, which comes later
    }
    try
    {
        const bool turningOn = enabled && !_autoHideEnabled;
        const bool turningOff = !enabled && _autoHideEnabled;
        const bool newHold = enabled && hold;
        const bool holdChanged = newHold != _autoHideHold;
        _autoHideHold = newHold;
        if (turningOn)
        {
            // Float the bar over the content row (or dock it, while held). It starts OUT: the rule then hides it
            // kTabRowHideDelayMs after the pointer is elsewhere, so the user sees the row leave instead of finding it gone.
            _autoHideEnabled = true;
            _autoHide = ::Agentmaster::TabRowAutoHideState{};
            _autoHide.shown = true;
            _autoHideSliding = false;
            _autoHidePress = AutoHidePress::None;
            _titlebar.Translation(float3{ 0.0f, 0.0f, 0.0f });
            _titlebar.IsHitTestVisible(true);
            _AutoHidePlace();
            _AutoHideAttachInput();
            _ApplyTitlebarBackground();
            _ResizeDragBarWindow();
            LogTabRow(newHold ? L"auto-hide on (held out, docked: the Agent Manager page is shown)" : L"auto-hide on");
        }
        else if (turningOff)
        {
            // Back above the content, exactly as without the setting. The handlers stay registered (they check the flag);
            // only Close / a re-Initialize cut them.
            _AutoHideStopTimers();
            _autoHideEnabled = false;
            _autoHide = ::Agentmaster::TabRowAutoHideState{};
            _autoHideSliding = false;
            _autoHidePress = AutoHidePress::None;
            // A STATIC Translation cancels a running slide and pushes nothing back (XAML's facade rule, the one the opacity
            // loops rely on: AgentAmbientAnimation.h).
            _titlebar.Translation(float3{ 0.0f, 0.0f, 0.0f });
            _titlebar.IsHitTestVisible(true);
            Controls::Canvas::SetZIndex(_titlebar, 0);
            Controls::Grid::SetRow(_titlebar, 0);
            _ApplyTitlebarBackground();
            _ResizeDragBarWindow();
            LogTabRow(L"auto-hide off");
            return;
        }
        else if (holdChanged && _autoHideEnabled)
        {
            // Docked while held, floating again when released (_AutoHidePlace): the rule below then keeps or hides the row.
            _AutoHidePlace();
            _ResizeDragBarWindow();
            LogTabRow(newHold ? L"held out, docked (the Agent Manager page is shown)" : L"hold released, floating again (a tab is shown)");
        }
        if (_autoHideEnabled)
        {
            _AutoHideEvaluate(turningOn ? L"setting on" : L"hold");
        }
    }
    catch (...)
    {
        LogTabRowCaught(L"SetTitlebarAutoHide");
    }
}

void NonClientIslandWindow::PeekTitlebar(const uint32_t durationMs, const bool instant)
{
    if (!_autoHideEnabled || !_titlebar)
    {
        return; // the row is always out without the setting: nothing to peek
    }
    try
    {
        const auto in = _AutoHideSample();
        if (!in.usable)
        {
            return; // focus mode, full screen without tabs, minimized: there is no row to show
        }
        const auto step = ::Agentmaster::TabRowAutoHidePeek(_autoHide, in.nowMs, durationMs);
        _AutoHideApply(step, in, instant ? L"peek, placed at once" : L"peek", !instant);
    }
    catch (...)
    {
        LogTabRowCaught(L"PeekTitlebar");
    }
}

// Is the auto-hiding row away from its place — hidden, or still sliding? Then it is no title bar: no drag bar window, no
// caption band in the window's hit test (the terminal's first rows are there). False whenever auto-hide is off.
bool NonClientIslandWindow::_AutoHideRowAway() const noexcept
{
    return _autoHideEnabled && (!_autoHide.shown || _autoHideSliding);
}

// Put the bar in the grid row its mode wants. FLOATING (auto-hide on, no hold): the content row, above the page by
// ZIndex — the terminal keeps the whole height and the bar comes and goes over it. DOCKED (auto-hide off, or the page holds
// the row out because its Agent Manager page is on screen): its own row above the content, exactly as without the setting,
// so the Manager page's own toolbar never lies under it (the user's mockup of "Stay out on the Manager page"). Docking places
// the bar out at once — no slide: the page moves down with it in the same layout pass, and a bar sliding in over an
// already-moved page would read as a glitch. The switch changes the content's height once; only the Manager page and the
// tab-switch funnel's swap see it (a terminal is not on screen while the page is). Throws what the XAML calls throw (the
// callers contain it).
void NonClientIslandWindow::_AutoHidePlace()
{
    const bool floating = _autoHideEnabled && !_autoHideHold;
    if (!floating)
    {
        ::KillTimer(GetHandle(), kTabRowSlideEndTimerId);
        _autoHideSliding = false;
        _titlebar.Translation(float3{ 0.0f, 0.0f, 0.0f }); // a static set also cancels a slide that was running
        _titlebar.IsHitTestVisible(true);
    }
    Controls::Grid::SetRow(_titlebar, floating ? 1 : 0);
    Controls::Canvas::SetZIndex(_titlebar, floating ? 1 : 0); // floating: above the page, which shares the row and comes later
}

// One look at everything the rule reads (TabRowAutoHideInputs), on the UI thread. Screen pixels throughout: the client
// area's top is the window's first visible row (a restored window keeps its one-pixel frame border in the client area,
// _OnNcCalcSize; a maximized one starts at the work area), the island's top is where the row's own band starts.
// Returns the inputs; geometry the window cannot report leaves the pointer "elsewhere". Throws what the XAML reads throw
// (winrt::hresult_error for a torn-down element); _AutoHideEvaluate / PeekTitlebar contain it.
::Agentmaster::TabRowAutoHideInputs NonClientIslandWindow::_AutoHideSample() const
{
    ::Agentmaster::TabRowAutoHideInputs in{};
    in.nowMs = ::GetTickCount64();
    const HWND hwnd = GetHandle();
    in.usable = _autoHideEnabled && _titlebar && _rootGrid && _IsTitlebarVisible() && ::IsWindowVisible(hwnd) && !::IsIconic(hwnd) &&
                _titlebar.ActualHeight() > 0.0;
    in.hold = _autoHideHold;
    if (!in.usable)
    {
        return in;
    }

    // The buttons: GetAsyncKeyState reads the physical state, which is what a drag holds (left, right or middle).
    const auto held = [](const int vk) noexcept { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    in.buttonHeld = held(VK_LBUTTON) || held(VK_RBUTTON) || held(VK_MBUTTON);
    in.pressInContent = in.buttonHeld && _autoHidePress == AutoHidePress::Content;
    in.pressInRow = in.buttonHeld && _autoHidePress == AutoHidePress::Row;

    // The cursor, and whether this window (or a popup it owns) is what lies under it.
    POINT cursor{};
    bool overThisWindow = false;
    if (::GetCursorPos(&cursor))
    {
        overThisWindow = TabRowWindowBelongsTo(::WindowFromPoint(cursor), hwnd);
    }
    RECT client{};
    POINT origin{ 0, 0 };
    RECT island{};
    const bool haveGeometry = ::GetClientRect(hwnd, &client) && ::ClientToScreen(hwnd, &origin) && ::GetWindowRect(_interopWindowHandle, &island);
    const double scale = GetCurrentDpiScale();
    if (overThisWindow && haveGeometry)
    {
        const auto left = static_cast<int32_t>(origin.x);
        const auto top = static_cast<int32_t>(origin.y);
        const auto right = static_cast<int32_t>(origin.x + (client.right - client.left));
        const auto rowBottom = static_cast<int32_t>(island.top) + ::Agentmaster::TabRowDipToPixels(_titlebar.ActualHeight(), scale, 0);
        const auto x = static_cast<int32_t>(cursor.x);
        const auto y = static_cast<int32_t>(cursor.y);
        in.atEdge = ::Agentmaster::TabRowBandContains(::Agentmaster::TabRowEdgeBand(left, top, right, scale), x, y);
        in.inRow = ::Agentmaster::TabRowBandContains(::Agentmaster::TabRowStayBand(left, top, right, rowBottom, scale), x, y);
    }

    // The open popups: the pointer over one keeps the row (a menu hanging below it, the + card, a tooltip), and so does the
    // keyboard inside one (the ▾ menu, a tab's menu, the + card's box).
    const auto xamlRoot = _rootGrid.XamlRoot();
    if (!xamlRoot)
    {
        return in;
    }
    std::vector<void*> popupChildren;
    for (const auto& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(xamlRoot))
    {
        const auto child = popup ? popup.Child() : nullptr;
        if (!child)
        {
            continue;
        }
        popupChildren.push_back(TabRowIdentity(child));
        if (in.inRow || !overThisWindow || !haveGeometry)
        {
            continue; // already kept, or the pointer is over another window: no bounds to test
        }
        if (const auto element = child.try_as<FrameworkElement>())
        {
            try
            {
                // The popup's child in island DIPs (a popup's TransformToVisual(nullptr) is relative to the island's content,
                // like any element of it — also for a windowed popup past the window's edge), then in screen pixels.
                const auto bounds = element.TransformToVisual(nullptr).TransformBounds(
                    winrt::Windows::Foundation::Rect{ 0.0f, 0.0f, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
                const double x0 = static_cast<double>(island.left) + static_cast<double>(bounds.X) * scale;
                const double y0 = static_cast<double>(island.top) + static_cast<double>(bounds.Y) * scale;
                const double x1 = x0 + static_cast<double>(bounds.Width) * scale;
                const double y1 = y0 + static_cast<double>(bounds.Height) * scale;
                const double px = static_cast<double>(cursor.x);
                const double py = static_cast<double>(cursor.y);
                in.inRow = px >= x0 && px < x1 && py >= y0 && py < y1;
            }
            catch (...)
            {
                // A popup opening this very moment (its child not in the tree yet) refuses the transform: skip that one popup,
                // never the whole look (which would stop the watch). Logged, but at most a few times per process: the watch
                // looks every 100 ms and the state can last a frame or two (Rule #18 — reported, not lost).
                static std::atomic<int> reported{ 0 };
                if (reported.fetch_add(1, std::memory_order_relaxed) < 3)
                {
                    LogTabRowCaught(L"measuring an open popup (skipped for this look)");
                }
            }
        }
    }

    // The keyboard: walk up from the focused element to the title bar or to an open popup's child.
    if (const auto focused = XamlInput::FocusManager::GetFocusedElement(xamlRoot).try_as<UIElement>())
    {
        const auto titlebarId = TabRowIdentity(_titlebar);
        bool inTitlebar = false;
        bool inPopup = false;
        DependencyObject current = focused;
        for (int guard = 0; current && guard < 512; ++guard)
        {
            const auto id = TabRowIdentity(current);
            if (id == titlebarId)
            {
                inTitlebar = true;
                break;
            }
            if (std::find(popupChildren.begin(), popupChildren.end(), id) != popupChildren.end())
            {
                inPopup = true;
                break;
            }
            current = Media::VisualTreeHelper::GetParent(current);
        }
        if (inTitlebar)
        {
            // Only the keyboard's own focus holds the row: a rename box, or a control reached by Tab / the arrows. A button
            // that kept the focus after a CLICK (Home, ▾) must not hold the row out forever.
            const auto control = focused.try_as<Controls::Control>();
            in.keyboardInRow = focused.try_as<Controls::TextBox>() != nullptr || (control && control.FocusState() == FocusState::Keyboard);
        }
        in.keyboardInPopup = inPopup;
    }
    return in;
}

// One look: sample, run the rule, act on its answer, re-arm the watch. Never throws: a failure is logged and stops the
// watch (a XAML refusal at every tick would otherwise flood the log); the next event looks again.
//   reason - what prompted the look ("watch", "move", "size", ...), for the reveal's log line.
void NonClientIslandWindow::_AutoHideEvaluate(const std::wstring_view reason) noexcept
{
    if (!_autoHideEnabled || !_titlebar)
    {
        _AutoHideStopTimers();
        return;
    }
    try
    {
        const auto in = _AutoHideSample();
        if (!in.buttonHeld)
        {
            _autoHidePress = AutoHidePress::None; // the press is over: where it went down means nothing any more
        }
        _AutoHideApply(::Agentmaster::NextTabRowAutoHide(_autoHide, in), in, reason, in.usable);
    }
    catch (...)
    {
        LogTabRowCaught(L"looking at the pointer");
        _AutoHideStopTimers();
    }
}

// Adopt the rule's step: remember its state, slide the row when it says so (one [tabrow] line per reveal or hide), and
// set the watch for the next look.
//   step    - the rule's answer.
//   in      - the look it answered (its moment, and what the reveal's log line names).
//   reason  - what prompted the look.
//   animate - slide (true), or place at once (false: a window that cannot show the row, an instant peek).
// Throws what the slide's XAML calls throw (the callers contain it).
void NonClientIslandWindow::_AutoHideApply(const ::Agentmaster::TabRowAutoHideStep& step, const ::Agentmaster::TabRowAutoHideInputs& in, const std::wstring_view reason, const bool animate)
{
    _autoHide = step.state;
    // A DOCKED bar (the page's hold) is already out in its own row (_AutoHidePlace): its "reveal" is only the rule's state,
    // never a slide over a page that already moved down for it.
    const bool docked = _autoHideEnabled && _autoHideHold;
    if (step.reveal)
    {
        _AutoHideSlide(true, animate && !docked);
        // Why it came out, in the rule's order of precedence (the hold, a peek, the keyboard, a finger, the top edge).
        const std::wstring_view why = in.hold                    ? std::wstring_view{ L"the Agent Manager page" } :
                                      step.state.peekUntilMs != 0 ? std::wstring_view{ L"peek" } :
                                      in.keyboardInRow            ? std::wstring_view{ L"the keyboard" } :
                                      step.state.touchShown       ? std::wstring_view{ L"a finger at the top" } :
                                      in.atEdge                   ? std::wstring_view{ L"the pointer rested at the top edge" } :
                                                                    reason;
        LogTabRow(std::wstring{ L"reveal (" } + std::wstring{ why } + L")");
    }
    else if (step.hide)
    {
        _AutoHideSlide(false, animate && !docked);
        LogTabRow(in.usable ? L"hide" : L"hide (the window cannot show its title bar: minimized, focus mode or full screen)");
    }
    _AutoHideArmWatch(step.wakeAtMs, in.nowMs);
}

// Move the row out (show) or away. Input follows the destination at once: an outgoing row takes no input from this moment
// (and lets go of a hovered caption button), an incoming one takes it where it is landing. The slide is a composition
// key-frame animation of the XAML Translation, 0.15 s, eased out coming and in going; while it runs, XAML follows the
// Translation for hit testing (per-frame UI work for those 0.15 s only — a one-shot, not the ambient loops that
// AgentAmbientAnimation.h keeps off XAML elements), and the drag bar window stays away until its end.
//   show    - true = out, false = away.
//   animate - slide (when Windows' animation effects are on too), or place at once.
// Throws what the XAML calls throw (the callers contain it). A refused animation falls back to the instant placement.
void NonClientIslandWindow::_AutoHideSlide(const bool show, const bool animate)
{
    const float height = static_cast<float>(_titlebar.ActualHeight());
    const float3 away{ 0.0f, -height, 0.0f };
    const float3 home{ 0.0f, 0.0f, 0.0f };
    const float3 target = show ? home : away;
    _titlebar.IsHitTestVisible(show);
    if (!show)
    {
        _titlebar.ReleaseButtons(); // a caption button hovered when the row left must not stay lit
    }
    const HWND hwnd = GetHandle();
    ::KillTimer(hwnd, kTabRowSlideEndTimerId);
    const bool reversing = _autoHideSliding; // a slide reversed midway leaves from wherever it got to
    _autoHideSliding = false;
    if (animate && TabRowAnimationsEnabled())
    {
        try
        {
            if (!_autoHideCompositor)
            {
                // _rootGrid is the island's root, a composition node already: asking its hand-out visual adds none.
                _autoHideCompositor = ElementCompositionPreview::GetElementVisual(_rootGrid).Compositor();
            }
            auto slide = _autoHideCompositor.CreateVector3KeyFrameAnimation();
            if (!reversing)
            {
                slide.InsertKeyFrame(0.0f, show ? away : home); // from its parked place (the static Translation)
            }
            const auto easing = show ? _autoHideCompositor.CreateCubicBezierEasingFunction(float2{ 0.0f, 0.0f }, float2{ 0.58f, 1.0f }) :
                                       _autoHideCompositor.CreateCubicBezierEasingFunction(float2{ 0.42f, 0.0f }, float2{ 1.0f, 1.0f });
            slide.InsertKeyFrame(1.0f, target, easing);
            slide.Duration(std::chrono::milliseconds{ static_cast<int64_t>(::Agentmaster::kTabRowSlideMs) });
            slide.Target(L"Translation");
            _titlebar.StartAnimation(slide);
            _autoHideSliding = true;
            // The end: a little past the slide, so the final frame is drawn before the static value is pinned.
            ::SetTimer(hwnd, kTabRowSlideEndTimerId, static_cast<UINT>(::Agentmaster::kTabRowSlideMs + 40), nullptr);
            _ResizeDragBarWindow(); // away while it slides
            return;
        }
        catch (...)
        {
            LogTabRowCaught(L"starting the slide"); // the instant placement below still moves the row
            _autoHideSliding = false;
        }
    }
    _titlebar.Translation(target);
    _ResizeDragBarWindow();
}

// The slide's end (WM_TIMER kTabRowSlideEndTimerId, one-shot): pin the static Translation to where the rule says the row is
// — a static set ends the animation for good and reads true afterwards — and, when it is out, bring the drag bar window
// back (the caption buttons, the window drag and the top resize band). Never throws (contained and logged).
void NonClientIslandWindow::_AutoHideSlideFinished() noexcept
{
    ::KillTimer(GetHandle(), kTabRowSlideEndTimerId);
    if (!_autoHideSliding)
    {
        return; // a stale tick: the slide was replaced or auto-hide turned off
    }
    _autoHideSliding = false;
    if (!_autoHideEnabled || !_titlebar)
    {
        return;
    }
    try
    {
        const float height = static_cast<float>(_titlebar.ActualHeight());
        _titlebar.Translation(float3{ 0.0f, _autoHide.shown ? 0.0f : -height, 0.0f });
    }
    catch (...)
    {
        LogTabRowCaught(L"pinning the row after its slide");
    }
    _ResizeDragBarWindow();
}

// Set the watch for the next look, or kill it: every kTabRowWatchMs while the row is out (and not held out by the page —
// nothing can hide it then, and the page's next call looks anyway), and exactly at `wakeAtMs` when sooner (a rest ending,
// the hide delay ending, a peek ending). A hidden row with nothing pending runs no timer: its next look comes from an event.
//   wakeAtMs - the rule's next moment (0 = none).
//   nowMs    - the moment of the look that produced it.
// Never throws.
void NonClientIslandWindow::_AutoHideArmWatch(const uint64_t wakeAtMs, const uint64_t nowMs) noexcept
{
    uint64_t dueIn = 0;
    if (_autoHideEnabled && _autoHide.shown && !_autoHideHold)
    {
        dueIn = ::Agentmaster::kTabRowWatchMs;
    }
    if (_autoHideEnabled && wakeAtMs != 0)
    {
        const uint64_t untilWake = wakeAtMs > nowMs ? wakeAtMs - nowMs : 1;
        dueIn = (dueIn == 0 || untilWake < dueIn) ? untilWake : dueIn;
    }
    const HWND hwnd = GetHandle();
    if (dueIn == 0)
    {
        if (_autoHideWatchArmed)
        {
            ::KillTimer(hwnd, kTabRowWatchTimerId);
            _autoHideWatchArmed = false;
        }
        return;
    }
    // SetTimer replaces the timer of the same id; it repeats at this period until the next look re-arms or kills it.
    const UINT period = static_cast<UINT>(dueIn < USER_TIMER_MINIMUM ? USER_TIMER_MINIMUM : (dueIn > 60000 ? 60000 : dueIn));
    _autoHideWatchArmed = ::SetTimer(hwnd, kTabRowWatchTimerId, period, nullptr) != 0;
}

// Kill both auto-hide timers. Never throws (KillTimer on a gone window just fails).
void NonClientIslandWindow::_AutoHideStopTimers() noexcept
{
    if (const HWND hwnd = GetHandle())
    {
        ::KillTimer(hwnd, kTabRowWatchTimerId);
        ::KillTimer(hwnd, kTabRowSlideEndTimerId);
    }
    _autoHideWatchArmed = false;
}

// Forget the auto-hiding row entirely (Close, a re-Initialize): the timers off, the handlers' link cut (the handlers stay
// registered on the old root grid and do nothing), the state cleared. The title bar's placement is not restored: it is
// going away with its root grid. Never throws.
void NonClientIslandWindow::_AutoHideShutdown() noexcept
{
    _AutoHideStopTimers();
    if (_autoHideLink)
    {
        _autoHideLink->window = nullptr;
        _autoHideLink.reset();
    }
    _autoHideEnabled = false;
    _autoHideHold = false;
    _autoHide = ::Agentmaster::TabRowAutoHideState{};
    _autoHideSliding = false;
    _autoHidePress = AutoHidePress::None;
}

// Register the pointer and focus handlers the rule's events come from, once per root grid (a later call is a no-op while
// the link stands). handledEventsToo: the terminal marks its pointer events handled, and the rule needs them all. Each
// handler reaches the window through the shared link, so a handler XAML raises after Close finds nothing to do. Throws
// what AddHandler throws (SetTitlebarAutoHide contains it).
void NonClientIslandWindow::_AutoHideAttachInput()
{
    if (_autoHideLink || !_rootGrid || !_titlebar)
    {
        return;
    }
    auto link = std::make_shared<AutoHideLink>();
    link->window = this;
    _rootGrid.AddHandler(UIElement::PointerMovedEvent(),
                         winrt::box_value(XamlInput::PointerEventHandler{ [link](const winrt::Windows::Foundation::IInspectable&, const XamlInput::PointerRoutedEventArgs& e) {
                             if (const auto self = link->window; self && self->_autoHideEnabled)
                             {
                                 self->_AutoHideOnPointerMoved(e);
                             }
                         } }),
                         true);
    _rootGrid.AddHandler(UIElement::PointerPressedEvent(),
                         winrt::box_value(XamlInput::PointerEventHandler{ [link](const winrt::Windows::Foundation::IInspectable&, const XamlInput::PointerRoutedEventArgs& e) {
                             if (const auto self = link->window; self && self->_autoHideEnabled)
                             {
                                 self->_AutoHideOnPointerPressed(e);
                             }
                         } }),
                         true);
    _rootGrid.AddHandler(UIElement::PointerReleasedEvent(),
                         winrt::box_value(XamlInput::PointerEventHandler{ [link](const winrt::Windows::Foundation::IInspectable&, const XamlInput::PointerRoutedEventArgs& e) {
                             if (const auto self = link->window; self && self->_autoHideEnabled)
                             {
                                 self->_AutoHideOnPointerReleased(e);
                             }
                         } }),
                         true);
    // The keyboard moving into a hidden row (a tab rename started by its key, Tab navigation) brings it out at once: the
    // rename box must not open where nobody can see it. GotFocus bubbles from the focused element up to the title bar.
    // A plain subscription: UWP XAML has no GotFocus routed-event identifier for AddHandler (and GotFocus cannot be marked
    // handled anyway). Its token is not kept: like the handlers above, it lives as long as the title bar and reaches the
    // window only through the link.
    _titlebar.GotFocus([link](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
        if (const auto self = link->window; self && self->_autoHideEnabled && !self->_autoHide.shown)
        {
            self->_AutoHideEvaluate(L"keyboard");
        }
    });
    _autoHideLink = std::move(link);
}

// A pointer move over the island. While the row is out the watch looks (a move here would cost a look per mouse event);
// while it is away, only the top matters: a move near the top starts a rest, a move away from it cancels a rest or lifts a
// dismissal, and every other move costs one coordinate read. A mouse or pen move also hands a finger-shown row back to
// the cursor's rule. Never throws (contained and logged).
//   e - the event (its device and position).
void NonClientIslandWindow::_AutoHideOnPointerMoved(const XamlInput::PointerRoutedEventArgs& e)
{
    try
    {
        const bool finger = e.Pointer().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Touch;
        if (!finger && _autoHide.touchShown)
        {
            _autoHide = ::Agentmaster::TabRowAutoHideMouseMoved(_autoHide);
        }
        if (_autoHide.shown || finger)
        {
            return; // out: the watch decides; a finger's move is a drag (its taps count, in the press handler)
        }
        // DIPs from the island's top. A little more than the edge band: the band itself is checked in screen pixels by the
        // look, this only decides whether a look is worth taking.
        const auto y = e.GetCurrentPoint(_rootGrid).Position().Y;
        const bool nearTop = y < static_cast<float>(::Agentmaster::kTabRowEdgeDip + 2.0);
        if (_autoHide.edgeSinceMs != 0 || _autoHide.dismissed)
        {
            if (!nearTop)
            {
                _AutoHideEvaluate(L"move"); // left the top: cancel the rest, or lift the dismissal
            }
            return; // still at the top: the rest's own wake decides
        }
        if (nearTop)
        {
            _AutoHideEvaluate(L"move");
        }
    }
    catch (...)
    {
        LogTabRowCaught(L"a pointer move");
    }
}

// A press over the island: remember where it went down (the row, or the content), which is how a tab drag keeps the row
// out and a selection pulled to the top never brings it. A FINGER's tap is the rule's own event: at the top it brings a
// hidden row out; elsewhere it lets go of a row a finger brought. Never throws (contained and logged).
//   e - the event (its device and position).
void NonClientIslandWindow::_AutoHideOnPointerPressed(const XamlInput::PointerRoutedEventArgs& e)
{
    try
    {
        const auto y = e.GetCurrentPoint(_rootGrid).Position().Y;
        const bool onRow = _autoHide.shown && y < static_cast<float>(_titlebar.ActualHeight());
        _autoHidePress = onRow ? AutoHidePress::Row : AutoHidePress::Content;
        if (e.Pointer().PointerDeviceType() != winrt::Windows::Devices::Input::PointerDeviceType::Touch)
        {
            return; // a mouse or pen press: the next look reads it (the watch while out, the next move while away)
        }
        const auto in = _AutoHideSample();
        if (!in.usable)
        {
            return;
        }
        const bool atTouchEdge = y < static_cast<float>(::Agentmaster::kTabRowTouchEdgeDip);
        _AutoHideApply(::Agentmaster::TabRowAutoHideFingerPress(_autoHide, in.nowMs, atTouchEdge, onRow), in, L"finger", true);
    }
    catch (...)
    {
        LogTabRowCaught(L"a pointer press");
    }
}

// A release over the island: the press is over, so its origin no longer holds the row or keeps it away. (A release the
// island never sees — outside the window — is caught by the next look, which re-reads the buttons.) Never throws.
//   e - the event (unused: any release ends the press this tracks).
void NonClientIslandWindow::_AutoHideOnPointerReleased(const XamlInput::PointerRoutedEventArgs& /*e*/)
{
    _autoHidePress = AutoHidePress::None;
}

// WM_NCMOUSEMOVE / WM_MOUSEMOVE on the window itself (not the island): on a restored window, the pointer on the one-pixel
// frame border row above the island — the first row of the top edge band, which no XAML event sees. Starts a look when the
// row is away and no rest is running yet. Never throws.
void NonClientIslandWindow::_AutoHideOnWindowPointer() noexcept
{
    if (!_autoHideEnabled || _autoHide.shown || _autoHide.edgeSinceMs != 0)
    {
        return; // off; out (watched); or a rest already running (its own wake decides)
    }
    _AutoHideEvaluate(L"frame border");
}

// The title bar's brush: the app's (SetTitlebarBackground), made opaque while auto-hide floats the bar over the content
// (TabRowOpaqueBrush). Never throws (contained and logged: the bar keeps its previous brush).
void NonClientIslandWindow::_ApplyTitlebarBackground()
{
    if (!_titlebar)
    {
        return;
    }
    try
    {
        _titlebar.Background(_autoHideEnabled ? TabRowOpaqueBrush(_titlebarBrush, _theme) : _titlebarBrush);
    }
    catch (...)
    {
        LogTabRowCaught(L"setting the title bar's brush");
    }
}
