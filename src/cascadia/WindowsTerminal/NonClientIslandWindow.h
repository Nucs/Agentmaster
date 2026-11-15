/*++
Copyright (c) Microsoft Corporation

Module Name:
- NonClientIslandWindow.h

Abstract:
- This class represents a window hosting two XAML Islands. One is in the client
  area of the window, as it is in the base IslandWindow class. The second is in
  the titlebar of the window, in the "non-client" area of the window. This
  enables an app to place xaml content in the titlebar.
- Placing content in the frame is enabled with DwmExtendFrameIntoClientArea. See
  https://docs.microsoft.com/en-us/windows/desktop/dwm/customframe
  for information on how this is done.

Author(s):
    Mike Griese (migrie) April-2019
--*/

#pragma once
#include "IslandWindow.h"

#include "../TerminalApp/AgentMaster/TabRowAutoHide.h" // Agentmaster: the auto-hiding tab row's pure rule (TAB_STRIP.md "Auto-hiding the tab row")

class NonClientIslandWindow : public IslandWindow
{
public:
    // this is the same for all DPIs
    static constexpr const int topBorderVisibleHeight = 1;

    NonClientIslandWindow(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme) noexcept;
    ~NonClientIslandWindow();

    virtual void Close() override;
    void MakeWindow() noexcept override;
    virtual void OnSize(const UINT width, const UINT height) override;

    [[nodiscard]] virtual LRESULT MessageHandler(UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept;

    virtual til::rect GetNonClientFrame(UINT dpi) const noexcept override;
    virtual til::size GetTotalNonClientExclusiveSize(UINT dpi) const noexcept override;

    void Initialize() override;

    void OnAppInitialized() override;
    void SetContent(winrt::Windows::UI::Xaml::UIElement content) override;
    void SetTitlebarContent(winrt::Windows::UI::Xaml::UIElement content);
    void OnApplicationThemeChanged(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme) override;

    void SetTitlebarBackground(winrt::Windows::UI::Xaml::Media::Brush brush);
    void SetShowTabsFullscreen(const bool newShowTabsFullscreen) override;

    virtual void UseMica(const bool newValue, const double titlebarOpacity) override;

    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): turn the auto-hiding title bar on or off, and say whether the
    // page holds it out. ON floats the title bar (the tab row, the drag bar, the caption buttons) over the top of the content
    // instead of above it — the content gains the row's height ONCE, here, and never resizes for the row again — and starts
    // it OUT, so the row leaves 0.6 s after the pointer is elsewhere (the user sees where it went). OFF puts it back above
    // the content (the content shrinks once). `hold` (the Agent Manager page is on screen) keeps the row out while it is set,
    // DOCKED in its own row above the page (the page's toolbar must not lie under it); released, it floats again.
    // Idempotent: a call that changes neither value only re-evaluates the rule. Called by the AppHost on every
    // TabRowAutoHideChanged. UI thread. Never throws: a XAML refusal is logged ("[tabrow] ... threw") and leaves the row
    // where it was.
    void SetTitlebarAutoHide(const bool enabled, const bool hold);

    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): PEEK — bring an auto-hidden row out now and keep it out for
    // `durationMs` (a keyboard tab switch: 1.5 s; the launcher opened by its keys: a moment, then the card's own keyboard
    // focus keeps it). `instant` places the row without the slide, so a caller that measures the row's buttons right after
    // (the launcher anchors under +) reads where they finally are. No-op while auto-hide is off or the window cannot show its
    // title bar (focus mode, full screen without tabs, minimized). UI thread. Never throws (logged like SetTitlebarAutoHide).
    void PeekTitlebar(const uint32_t durationMs, const bool instant);

private:
    std::optional<til::point> _oldIslandPos;

    winrt::TerminalApp::TitlebarControl _titlebar{ nullptr };

    wil::unique_hbrush _backgroundBrush;
    til::color _backgroundBrushColor;

    winrt::Windows::UI::Xaml::Controls::Border _dragBar{ nullptr };
    wil::unique_hwnd _dragBarWindow;

    winrt::Windows::UI::Xaml::ElementTheme _theme;

    bool _useMica{ false };
    double _titlebarOpacity{ 1.0 };

    bool _isMaximized;
    bool _trackingMouse{ false };

    // Agentmaster (TAB_STRIP.md "Auto-hiding the tab row"): the AUTO-HIDING TITLE BAR. While _autoHideEnabled the title bar
    // sits in the content row (Grid row 1, above the content by ZIndex) instead of its own row — except while the page holds
    // it out (its Agent Manager page on screen), when it docks back in row 0 (_AutoHidePlace) — and hides by its Translation
    // (moved up by its own height, out of the island's bounds, taking no input) — never by Visibility, which would re-measure
    // the whole tab strip at every reveal. The rule (TabRowAutoHide.h) decides when; these members are what it acts on.
    // UI thread only, every one.
    bool _autoHideEnabled{ false }; // the setting is on for this window (SetTitlebarAutoHide)
    bool _autoHideHold{ false }; // the page holds the row out (its Agent Manager page is on screen): the bar is DOCKED in its own row meanwhile (_AutoHidePlace); only with _autoHideEnabled
    ::Agentmaster::TabRowAutoHideState _autoHide{}; // the rule's memory (shown, the rest, the hide clock, a peek, a dismissal)
    bool _autoHideSliding{ false }; // a slide is running: the drag bar window waits for its end (kTabRowSlideEndTimerId)
    bool _autoHideWatchArmed{ false }; // the watch timer (kTabRowWatchTimerId) is set
    // Where the mouse button now held went down: the row (a tab drag keeps the row out), the content (a selection pulled to
    // the top never brings it), or not seen (a press on the drag bar window or in a popup, or an OLE drag from another app).
    enum class AutoHidePress : uint8_t
    {
        None,
        Row,
        Content,
    };
    AutoHidePress _autoHidePress{ AutoHidePress::None };
    // The XAML handlers' way back to this window: they are registered on _rootGrid for its whole life (XAML offers no
    // reliable removal of a boxed handler), so they reach the window through this link, which Close() / Initialize() cut.
    // A handler that finds `window` null does nothing. Created by the first _AutoHideAttachInput of each _rootGrid.
    struct AutoHideLink
    {
        NonClientIslandWindow* window{ nullptr };
    };
    std::shared_ptr<AutoHideLink> _autoHideLink;
    // The brush the app asked the title bar to wear (SetTitlebarBackground). While auto-hide floats the bar over the content
    // it wears an OPAQUE version of it (_ApplyTitlebarBackground): a transparent or Mica bar would show the terminal's text
    // through the tabs.
    winrt::Windows::UI::Xaml::Media::Brush _titlebarBrush{ nullptr };
    // The compositor that renders this window's XAML, asked once (a hand-out visual of _rootGrid, which is already a
    // composition node), for the slide's key-frame animations.
    winrt::Windows::UI::Composition::Compositor _autoHideCompositor{ nullptr };

    [[nodiscard]] bool _AutoHideRowAway() const noexcept; // the row is not fully out: hidden, or still sliding (no drag bar window, no caption band in the hit test)
    void _AutoHidePlace(); // the bar's grid row for its mode: floating over the content (row 1) or docked in its own row (row 0: auto-hide off, or held out by the Agent Manager page)
    [[nodiscard]] ::Agentmaster::TabRowAutoHideInputs _AutoHideSample() const; // one look at the cursor, the buttons, the keyboard focus and the popups
    void _AutoHideEvaluate(const std::wstring_view reason) noexcept; // sample + the rule + act + re-arm the watch
    void _AutoHideApply(const ::Agentmaster::TabRowAutoHideStep& step, const ::Agentmaster::TabRowAutoHideInputs& in, const std::wstring_view reason, const bool animate); // adopt the rule's step: slide, log, re-arm
    void _AutoHideSlide(const bool show, const bool animate); // move the row in or out (a 0.15 s composition slide, or at once)
    void _AutoHideSlideFinished() noexcept; // the slide's end: pin the final place, show the drag bar window when out
    void _AutoHideArmWatch(const uint64_t wakeAtMs, const uint64_t nowMs) noexcept; // set or kill the watch timer
    void _AutoHideStopTimers() noexcept; // kill the watch and the slide-end timers (turning auto-hide off, a failed look)
    void _AutoHideAttachInput(); // register the pointer / focus handlers on _rootGrid and the title bar (once per _rootGrid)
    void _AutoHideShutdown() noexcept; // kill the timers and cut the handlers' link (Close, a re-Initialize)
    void _AutoHideOnPointerMoved(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // a move over the island: start or cancel a rest at the top
    void _AutoHideOnPointerPressed(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // a press: remember its origin; a finger's tap
    void _AutoHideOnPointerReleased(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // a release: forget the press
    void _AutoHideOnWindowPointer() noexcept; // WM_(NC)MOUSEMOVE on the window itself: the frame border row above the island
    void _ApplyTitlebarBackground(); // the title bar's brush: the app's, made opaque while auto-hide floats it

    [[nodiscard]] static LRESULT __stdcall _StaticInputSinkWndProc(HWND const window, UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept;
    [[nodiscard]] LRESULT _InputSinkMessageHandler(UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept;

    void _ResizeDragBarWindow() noexcept;

    int _GetResizeHandleHeight() const noexcept;
    til::rect _GetDragAreaRect() const noexcept;
    int _GetTopBorderHeight() const noexcept;
    LRESULT _dragBarNcHitTest(const til::point pointer);

    [[nodiscard]] LRESULT _OnNcCalcSize(const WPARAM wParam, const LPARAM lParam) noexcept;
    [[nodiscard]] LRESULT _OnNcHitTest(POINT ptMouse) const noexcept;
    [[nodiscard]] LRESULT _OnPaint() noexcept;
    [[nodiscard]] LRESULT _OnSetCursor(WPARAM wParam, LPARAM lParam) const noexcept;
    void _OnMaximizeChange() noexcept;
    void _OnDragBarSizeChanged(winrt::Windows::Foundation::IInspectable sender, winrt::Windows::UI::Xaml::SizeChangedEventArgs eventArgs);

    void _SetIsBorderless(const bool borderlessEnabled) override;
    void _SetIsFullscreen(const bool fullscreenEnabled) override;
    bool _IsTitlebarVisible() const;

    void _UpdateFrameMargins() const noexcept;
    void _UpdateMaximizedState();
    void _UpdateIslandPosition(const UINT windowWidth, const UINT windowHeight);
    void _UpdateTitlebarVisibility();

    struct Revokers
    {
        winrt::Windows::UI::Xaml::Controls::Border::SizeChanged_revoker dragBarSizeChanged;
        winrt::Windows::UI::Xaml::Controls::Grid::SizeChanged_revoker rootGridSizeChanged;
        winrt::TerminalApp::TitlebarControl::Loaded_revoker titlebarLoaded;
    } _callbacks{};
};
