// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once

#include "SearchBoxControl.h"
#include "TermControl.g.h"
#include "../../buffer/out/search.h"
#include "../../cascadia/TerminalCore/Terminal.hpp"
#include "../../renderer/uia/UiaRenderer.hpp"
#include "../../tsf/Handle.h"

#include "ControlInteractivity.h"

namespace Microsoft::Console::VirtualTerminal
{
    struct MouseButtonState;
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct TermControl;

    struct TsfDataProvider : ::Microsoft::Console::TSF::IDataProvider
    {
        explicit TsfDataProvider(TermControl* termControl) noexcept;
        virtual ~TsfDataProvider() = default;

        STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;

        HWND GetHwnd() override;
        RECT GetViewport() override;
        RECT GetCursorPosition() override;
        void HandleOutput(std::wstring_view text) override;
        ::Microsoft::Console::Render::Renderer* GetRenderer() override;

    private:
        ControlCore* _getCore() const noexcept;

        TermControl* _termControl = nullptr;
        HWND _hwnd = nullptr;
    };

    struct TermControl : TermControlT<TermControl>
    {
        TermControl(Control::ControlInteractivity content);

        TermControl(IControlSettings settings, Control::IControlAppearance unfocusedAppearance, TerminalConnection::ITerminalConnection connection);

        static Control::TermControl NewControlByAttachingContent(Control::ControlInteractivity content);
        // Agentmaster (RENDERING.md): the projected setter of the process-wide software-rendering
        // override — see TermControl.idl and ControlCore::SetSoftwareRenderingOverride.
        // Parameters: enabled — true draws every terminal on the CPU (WARP). Never throws.
        static void SetSoftwareRenderingOverride(bool enabled) noexcept;
        // Agentmaster (RENDERING.md §4c): the projected setter of the process-wide hidden-terminal
        // policy — see TermControl.idl and ControlCore::SetHiddenRenderingPolicy. It must not be named
        // Set<X>: the XAML compiler reads a two-parameter static Set<X> on this XAML type as an attached
        // property and generates type info that does not compile (the warning in TermControl.idl).
        // Parameters: pauseHidden — hidden terminals draw no frames; releaseDelayMs — hidden time
        // before the GPU device is released (0xFFFFFFFF = never). Never throws.
        static void ConfigureHiddenRenderingPolicy(bool pauseHidden, uint32_t releaseDelayMs) noexcept;

        void UpdateControlSettings(Control::IControlSettings settings);
        void UpdateControlSettings(Control::IControlSettings settings, Control::IControlAppearance unfocusedAppearance);
        IControlSettings Settings() const;

        void KeyBindings(const Control::IKeyBindings& bindings) { _keyBindings = bindings; }

        uint64_t ContentId() const;

        hstring GetStartingTitle() const;

        bool CopySelectionToClipboard(bool dismissSelection, bool singleLine, bool withControlSequences, const CopyFormat formats);
        void PasteTextFromClipboard();
        void SelectAll();
        bool ToggleBlockSelection();
        void ToggleMarkMode();
        bool SwitchSelectionEndpoint();
        bool ExpandSelectionToWord();
        void RestoreFromPath(winrt::hstring path);
        void PersistTo(int64_t handle) const;
        void OpenCWD();
        void Close();
        bool InitializeWithSize(double width, double height, float scale); // Agentmaster (eager-init): see TermControl.idl
        bool StartDormantConnection(); // Agentmaster (deactivate/reactivate): Start an initialized control's parked NotConnected connection — see TermControl.idl
        // Agentmaster (RENDERING.md): re-point this terminal's live renderer at the effective software
        // flag — see TermControl.idl and ControlCore::ReapplySoftwareRendering. UI thread only. Can
        // throw a winrt::hresult_error when a projected settings call fails (the caller catches).
        void ReapplySoftwareRendering();
        // Agentmaster (RENDERING.md §4c): push this terminal's current on-screen state with the current
        // process-wide policy to its renderer (a Settings change) — see TermControl.idl. UI thread only.
        // Never throws: a failure is logged and the terminal keeps its previous state.
        void ReapplyHiddenRenderingPolicy();
        Windows::Foundation::Size CharacterDimensions() const;
        Windows::Foundation::Size MinimumSize();
        float SnapDimensionToGrid(const bool widthOrHeight, const float dimension);
        void PreviewInput(const winrt::hstring& text);

        Windows::Foundation::Point CursorPositionInDips();
        double QuickFixButtonWidth();
        double QuickFixButtonCollapsedWidth();

        void WindowVisibilityChanged(const bool showOrHide);

        void ColorSelection(Control::SelectionColor fg, Control::SelectionColor bg, Core::MatchMode matchMode);

#pragma region ICoreState
        const uint64_t TaskbarState() const noexcept;
        const uint64_t TaskbarProgress() const noexcept;

        hstring Title();
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() noexcept;
        hstring WorkingDirectory() const;

        TerminalConnection::ConnectionState ConnectionState() const;

        int ScrollOffset() const;
        int ViewHeight() const;
        int BufferHeight() const;

        bool HasSelection() const;
        bool HasMultiLineSelection() const;
        winrt::hstring SelectedText(bool trimTrailingWhitespace) const;

        bool BracketedPasteEnabled() const noexcept;

        float BackgroundOpacity() const;

        uint64_t OwningHwnd();
        void OwningHwnd(uint64_t owner);

        Windows::Foundation::Collections::IVector<Control::ScrollMark> ScrollMarks() const;
        void AddMark(const Control::ScrollMark& mark);
        void ClearMark();
        void ClearAllMarks();
        void ScrollToMark(const Control::ScrollToMarkDirection& direction);
        void SelectCommand(const bool goUp);
        void SelectOutput(const bool goUp);
#pragma endregion

        void ScrollViewport(int viewTop);
        int32_t JumpToConversationPrompt(const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& messages, uint32_t index); // Agentmaster (SUMMARY_JUMP.md)
        winrt::Windows::Foundation::Collections::IVector<int32_t> ResolveConversationPromptRows(const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& messages); // Agentmaster (SUMMARY_JUMP.md): icon eligibility
        int32_t ScrollToAdjacentConversationPrompt(const winrt::Windows::Foundation::Collections::IVector<winrt::hstring>& messages, bool up); // Agentmaster (alt+up/down): center on the nearest off-screen prompt up/down; returns its 0-based message index (for the summary highlight), -1 (boundary -> limit sound), or -2 (down past the last prompt -> bottom/live tail). At the up end it stops at the topmost matched prompt (returns its index).
        winrt::hstring ReadPendingInputDraft(); // Agentmaster (PENDING_INPUT.md): the unsent input-box draft (read-only); empty => none
        winrt::hstring ReadScreenSnapshot(); // Agentmaster (MESSAGING.md section 21.24): the live screen as a styled snapshot (ScreenSnapshot.h) for the Telegram card's Screen Image; empty => not initialized
        int32_t ReadPendingInputBoxState(); // Agentmaster (DELIVERY_PLAN.md R5): the tri-state verdict of that same cached scan (InputBoxState as Int32)
        winrt::hstring ReadInputBoxProbe(int32_t maxRows); // Agentmaster (DELIVERY_PLAN.md R4/R5): "<state digit><draft>" fresh read at a chosen window; "" => not initialized
        int32_t ReadInputBoxBodyRows(); // Agentmaster (PENDING_INPUT.md §9): the input box's body height in visual rows (newlines + 1); > 1 => leftover blank "" rows for the clear ladders; 0 => no box

        void AdjustFontSize(float fontSizeDelta);
        void ResetFontSize();
        winrt::Windows::Foundation::Size GetFontSize() const;

        void SendInput(const winrt::hstring& input);
        void ClearBuffer(Control::ClearBufferType clearType);

        void ToggleShaderEffects();

        void RenderEngineSwapChainChanged(IInspectable sender, IInspectable args);
        void _AttachDxgiSwapChainToXaml(HANDLE swapChainHandle);
        safe_void_coroutine _RendererEnteredErrorState(IInspectable sender, IInspectable args);

        void _RenderRetryButton_Click(const IInspectable& button, const IInspectable& args);
        safe_void_coroutine _RendererWarning(IInspectable sender,
                                             Control::RendererWarningArgs args);

        void CreateSearchBoxControl();
        void SetSearchBoxRightInset(double rightInsetPx); // Agentmaster: shift the search box left of the per-tab overlay badge

        void SearchMatch(const bool goForward);

        bool SearchBoxEditInFocus() const;

        bool OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down);

        bool OnMouseWheel(const Windows::Foundation::Point location, const winrt::Microsoft::Terminal::Core::Point delta, const bool leftButtonDown, const bool midButtonDown, const bool rightButtonDown);

        ~TermControl();

        Windows::UI::Xaml::Automation::Peers::AutomationPeer OnCreateAutomationPeer();
        const Windows::UI::Xaml::Thickness GetPadding();

        static Windows::Foundation::Size GetProposedDimensions(const IControlSettings& settings,
                                                               const uint32_t dpi,
                                                               int32_t commandlineCols,
                                                               int32_t commandlineRows);
        static Windows::Foundation::Size GetProposedDimensions(const IControlSettings& settings, const uint32_t dpi, const winrt::Windows::Foundation::Size& initialSizeInChars);

        winrt::Windows::Foundation::Size GetNewDimensions(const winrt::Windows::Foundation::Size& initialSizeInChars);

        void BellLightOn();

        bool ReadOnly() const noexcept;
        void ToggleReadOnly();
        void SetReadOnly(const bool readOnlyState);

        static Control::MouseButtonState GetPressedMouseButtons(const winrt::Windows::UI::Input::PointerPoint point);
        static unsigned int GetPointerUpdateKind(const winrt::Windows::UI::Input::PointerPoint point);
        static Windows::UI::Xaml::Thickness ParseThicknessFromPadding(const hstring padding);

        hstring ReadEntireBuffer() const;
        Control::CommandHistoryContext CommandHistory() const;
        void UpdateWinGetSuggestions(Windows::Foundation::Collections::IVector<hstring> suggestions);

        void AdjustOpacity(const float opacity, const bool relative);

        bool RawWriteKeyEvent(const WORD vkey, const WORD scanCode, const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers, const bool keyDown);
        bool RawWriteChar(const wchar_t character, const WORD scanCode, const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers);
        void RawWriteString(const winrt::hstring& text);

        void ShowContextMenu();
        bool OpenQuickFixMenu();
        void RefreshQuickFixMenu();
        void ClearQuickFix();

        void Detach();

        TerminalConnection::ITerminalConnection Connection();
        void Connection(const TerminalConnection::ITerminalConnection& connection);
        void HardResetWithoutErase();

        Control::CursorDisplayState CursorVisibility() const noexcept;
        void CursorVisibility(Control::CursorDisplayState cursorVisibility);

        void ApplyPreviewColorScheme(const Core::ICoreScheme& scheme) { _core.ApplyPreviewColorScheme(scheme); }
        void ResetPreviewColorScheme() { _core.ResetPreviewColorScheme(); }
        void SetOverrideColorScheme(const Core::ICoreScheme& scheme) { _core.SetOverrideColorScheme(scheme); }

        // -------------------------------- WinRT Events ---------------------------------
        // clang-format off
        til::property_changed_event PropertyChanged;

        til::typed_event<IInspectable, Control::OpenHyperlinkEventArgs> OpenHyperlink;
        til::typed_event<IInspectable, Control::NoticeEventArgs> RaiseNotice;
        til::typed_event<> HidePointerCursor;
        til::typed_event<> RestorePointerCursor;
        til::typed_event<> ReadOnlyChanged;
        til::typed_event<IInspectable, IInspectable> FocusFollowMouseRequested;
        til::typed_event<Control::TermControl, Windows::UI::Xaml::RoutedEventArgs> Initialized;
        til::typed_event<> WarningBell;
        til::typed_event<IInspectable, Control::KeySentEventArgs> KeySent;
        til::typed_event<IInspectable, Control::CharSentEventArgs> CharSent;
        til::typed_event<IInspectable, Control::StringSentEventArgs> StringSent;
        til::typed_event<IInspectable, Control::SearchMissingCommandEventArgs> SearchMissingCommand;
        til::typed_event<IInspectable, Control::WindowSizeChangedEventArgs> WindowSizeChanged;

        // UNDER NO CIRCUMSTANCES SHOULD YOU ADD A (PROJECTED_)FORWARDED_TYPED_EVENT HERE
        // Those attach the handler to the core directly, and will explode if
        // the core ever gets detached & reattached to another window.
        BUBBLED_FORWARDED_TYPED_EVENT(TitleChanged,             IInspectable, Control::TitleChangedEventArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(TabColorChanged,          IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(SetTaskbarProgress,       IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(ConnectionStateChanged,   IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(ShowWindowChanged,        IInspectable, Control::ShowWindowArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(CloseTerminalRequested,   IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(CompletionsChanged,       IInspectable, Control::CompletionsChangedEventArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(RestartTerminalRequested, IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(WriteToClipboard,         IInspectable, Control::WriteToClipboardEventArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(PasteFromClipboard,       IInspectable, Control::PasteFromClipboardEventArgs);

        // clang-format on

        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, BackgroundBrush, PropertyChanged.raise, nullptr);

    private:
        friend struct TermControlT<TermControl>; // friend our parent so it can bind private event handlers
        friend struct TsfDataProvider;

        // NOTE: _uiaEngine must be ordered before _core.
        //
        // ControlCore::AttachUiaEngine receives a IRenderEngine as a raw pointer, which we own.
        // We must ensure that we first destroy the ControlCore before the UiaEngine instance
        // in order to safely resolve this unsafe pointer dependency. Otherwise, a deallocated
        // IRenderEngine is accessed when ControlCore calls Renderer::TriggerTeardown.
        // (C++ class members are destroyed in reverse order.)
        // Further, the TermControlAutomationPeer must be destructed after _uiaEngine!
        Control::TermControlAutomationPeer _automationPeer{ nullptr };
        Control::ControlInteractivity _interactivity{ nullptr };
        Control::ControlCore _core{ nullptr };
        Control::IKeyBindings _keyBindings{ nullptr };
        TsfDataProvider _tsfDataProvider{ this };
        winrt::com_ptr<SearchBoxControl> _searchBox;

        enum class AltNumpadEncoding
        {
            OEM,
            ANSI,
            Unicode,
        };
        struct AltNumpadState
        {
            struct CachedKey
            {
                WORD vkey;
                WORD scanCode;
                ::Microsoft::Terminal::Core::ControlKeyStates modifiers;
                bool keyDown;
            };
            AltNumpadEncoding encoding = AltNumpadEncoding::OEM;
            uint32_t accumulator = 0;
            // Checking for accumulator != 0 to see if we have an ongoing Alt+Numpad composition is insufficient.
            // The state can be active, while the accumulator is 0, if the user pressed Alt+Numpad0 which enabled
            // the OEM encoding mode (= active), and then pressed Numpad0 again (= accumulator is still 0).
            bool active = false;
            til::small_vector<CachedKey, 4> cachedKeyEvents;
        };
        AltNumpadState _altNumpadState;

        bool _closing{ false };
        bool _focused{ false };
        bool _initializedTerminal{ false };
        bool _quickFixButtonCollapsible{ false };
        bool _quickFixesAvailable{ false };
        til::CoordType _quickFixBufferPos{};

        std::shared_ptr<ThrottledFunc<>> _playWarningBell;

        struct ScrollBarUpdate
        {
            std::optional<double> newValue;
            double newMaximum;
            double newMinimum;
            double newViewportSize;
        };

        std::shared_ptr<ThrottledFunc<ScrollBarUpdate>> _updateScrollBar;

        bool _isInternalScrollBarUpdate;

        // Auto scroll occurs when user, while selecting, drags cursor outside
        // viewport. View is then scrolled to 'follow' the cursor.
        double _autoScrollVelocity;
        std::optional<Windows::UI::Input::PointerPoint> _autoScrollingPointerPoint;
        SafeDispatcherTimer _autoScrollTimer;
        std::optional<std::chrono::high_resolution_clock::time_point> _lastAutoScrollUpdateTime;
        bool _pointerPressedInBounds{ false };

        // Agentmaster: while a pane is read-only ("locked" — e.g. an Agentmaster draft-swap that
        // temporarily owns the input box, or the toggleReadOnlyMode action) we show the Windows
        // "unavailable" mouse cursor (UniversalNo — the circle-with-a-slash "no" icon) over the
        // terminal, so a locked tab reads as intentionally locked instead of the pointer merely
        // vanishing while you type into it. _pointerInsideForReadOnly tracks the hover so a lock
        // that lands while hovering can refresh the cursor; _readOnlyCursorApplied gates us to
        // touching the (window-global) CoreWindow cursor on transitions only.
        bool _pointerInsideForReadOnly{ false };
        bool _readOnlyCursorApplied{ false };

        winrt::Windows::UI::Composition::ScalarKeyFrameAnimation _bellLightAnimation{ nullptr };
        winrt::Windows::UI::Composition::ScalarKeyFrameAnimation _bellDarkAnimation{ nullptr };
        SafeDispatcherTimer _bellLightTimer;

        // Agentmaster (UI_THREAD.md §10a #3): the SwapChainPanel's LayoutUpdated subscription that starts
        // this terminal on its first non-zero layout. Held ONLY while the control is loaded and not yet
        // initialized (_UpdateLayoutInitHook) — no longer from construction until the first start. Since
        // 2026-10-05 it is the BELT: the panel's SizeChanged starts the terminal in the pass that first
        // sizes it, Loaded or not (_StartTerminalInLayoutPass, UI_THREAD.md §10k).
        winrt::Windows::UI::Xaml::Controls::SwapChainPanel::LayoutUpdated_revoker _layoutUpdatedRevoker;
        winrt::hstring _restorePath;
        bool _showMarksInScrollbar{ false };

        bool _isBackgroundLight{ false };
        bool _detached{ false };
        // Agentmaster (RENDERING.md §4c): false while the hosting window is minimized
        // (WindowVisibilityChanged). One of the two inputs of "is this terminal on screen?"; the other
        // is IsLoaded() (in the window's visual tree). Starts true: a control created in a minimized
        // window gets no call (TerminalPage::_SetupControl only reports a visible window), and the
        // next restore reports true anyway.
        bool _windowVisible{ true };
        // Agentmaster (RENDERING.md §4c): suspend this terminal's renderer while it is off screen and
        // resume it with one full repaint when it is back (see ControlCore::SetRenderingSuspended).
        // Called on Loaded, Unloaded, a window minimize/restore, right before painting is first
        // enabled, and on a policy reapply. A no-op while closing or after Detach (the core may then
        // belong to another window's control). Never throws (logged), because it runs inside XAML
        // event handlers.
        void _UpdateRenderingSuspension() noexcept;
        til::CoordType _searchScrollOffset = 0;
        double _searchBoxRightInset = 0; // Agentmaster: right margin applied to the search box so it sits left of the per-tab overlay badge
        void _applySearchBoxInset(); // Agentmaster: (re)apply _searchBoxRightInset to the (lazily-created) search box

        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalPrimaryElements{ nullptr };
        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalSecondaryElements{ nullptr };
        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalSelectedPrimaryElements{ nullptr };
        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalSelectedSecondaryElements{ nullptr };

        Control::CursorDisplayState _cursorVisibility{ Control::CursorDisplayState::Default };

        inline bool _IsClosing() const noexcept
        {
#ifndef NDEBUG
            // _closing isn't atomic and may only be accessed from the main thread.
            if (const auto dispatcher = Dispatcher())
            {
                assert(dispatcher.HasThreadAccess());
            }
#endif
            return _closing;
        }

        void _initializeForAttach();

        void _UpdateSettingsFromUIThread();
        void _UpdateAppearanceFromUIThread(Control::IControlAppearance newAppearance);

        void _ApplyUISettings();
        void UpdateAppearance(Control::IControlAppearance newAppearance);
        void _SetBackgroundImage(const IControlAppearance& newAppearance);

        void _InitializeBackgroundBrush();
        safe_void_coroutine _coreBackgroundColorChanged(const IInspectable& sender, const IInspectable& args);
        void _changeBackgroundColor(til::color bg);
        static bool _isColorLight(til::color bg) noexcept;
        void _changeBackgroundOpacity();

        enum InitializeReason : bool
        {
            Create,
            Reattach
        };
        bool _InitializeTerminal(const InitializeReason reason);
        // Agentmaster (eager-init): the body of _InitializeTerminal with the size supplied explicitly
        // instead of read off the SwapChainPanel. _InitializeTerminal reads the panel and delegates here;
        // InitializeWithSize passes a placeholder size so a never-laid-out background tab can initialize
        // + start its connection in place. Same AV-safe Initialize()->Start() order.
        bool _InitializeTerminalWithSize(const InitializeReason reason, const double panelWidthDip, const double panelHeightDip, const float panelScaleX, const float panelScaleY);
        // Agentmaster (UI_THREAD.md §10a #3): arm or disarm _layoutUpdatedRevoker, the hook that starts
        // this terminal on its first non-zero layout. XAML calls EVERY LayoutUpdated handler of the UI
        // thread after EVERY layout pass, in every window, controls outside any visual tree included. The
        // constructor used to subscribe unconditionally, so each restored tab never opened (~537 on the
        // 550-tab release) ran a handler on every pass that read its size and returned (35% of the
        // LayoutUpdated raise, UI_THREAD.md §4c). A control outside the visual tree has no size, so it
        // cannot start anyway: the hook is now held only while the control is loaded and not yet
        // initialized, and dropped while closing or after Detach (the core then belongs to another
        // window's control). Called from the Loaded / Unloaded handlers and _initializeForAttach; it
        // reads IsLoaded(), the live answer, because XAML can raise a STALE Unloaded after the next Loaded
        // when an element is re-parented within one tick (a zoom, a pane move).
        // Whenever the hook is wanted it also tries the start at once: the layout pass that sized the
        // panel can run BEFORE XAML raises Loaded, and a hook armed after that pass would otherwise wait
        // for the next one. _InitializeTerminal refuses a zero size and a second start, so the attempt is
        // free when it cannot succeed, and the hook stays armed for the pass that sizes the panel.
        // Never throws (it runs from XAML event handlers): a failed read, subscribe or start is logged.
        // The hook is armed BEFORE the immediate start, so a start that throws is retried by the next
        // layout pass, exactly as the hook-only path always did.
        // ⚠ Not enough on its own (the 2026-10-05 crash, UI_THREAD.md §10k): XAML raises Loaded only inside
        // its frame tick, but it also runs layout passes OUTSIDE the tick — one before EVERY pointer hit
        // test (CUIElement::BoundsTestEntry) — and such a pass makes the control hit-testable while Loaded
        // is still pending. _StartTerminalInLayoutPass covers that pass; this hook is the belt behind it.
        void _UpdateLayoutInitHook() noexcept;
        // Agentmaster (the first-sizing start, UI_THREAD.md §10k): start this terminal in the layout pass
        // that first gives its SwapChainPanel a size — called from the panel's SizeChanged
        // (_SwapChainSizeChanged), which XAML raises inside EVERY layout pass, its frame tick's and the
        // ones it runs outside the tick, before every pointer hit test. That outside pass is what made the
        // 2026-10-05 crash possible: 071031fdf (§10a #3) moved the start to Loaded, which XAML raises only
        // in the tick, so a mouse move processed between a new tab's content swap and the next tick laid
        // the new control out, hit it, and delivered PointerMoved to a terminal with no text buffer
        // (TextBuffer::GetSize on a null buffer, via the hyperlink hover lookup). The construction-time
        // LayoutUpdated hook of upstream started the terminal inside that same pass; SizeChanged restores
        // the guarantee per element, so the never-shown restored tabs keep their §10a #3 saving (a
        // control outside the tree is never laid out, so it gets no SizeChanged).
        // Conditions: the terminal has not started, the control is not closing, and it is not detached
        // (the core then belongs to another window's control). Deliberately NOT IsLoaded(): Loaded is
        // still pending in exactly the pass this covers. It performs _layoutInitReason, like the hook.
        // The start runs inside a XAML layout pass, as upstream's always did (in LayoutUpdated): the
        // connection starts and Initialized is raised there, and the renderer starts suspended until
        // Loaded reports the control on screen (_UpdateRenderingSuspension). A start in a pass that runs
        // before the panel's final size self-corrects through the next SizeChanged (_core.SizeChanged).
        // No parameters. Returns nothing. Never throws (it runs from a XAML event handler): a failed start
        // is logged (CATCH_LOG), and the Loaded attempt and the layout hook retry it.
        void _StartTerminalInLayoutPass() noexcept;
        // Agentmaster (UI_THREAD.md §10a #3): the start the layout hook performs. Create for a new
        // control (build the core, then start its connection); Reattach after _initializeForAttach (the
        // core came from another window's control and usually only needs a resize). A member, read at
        // every start attempt, so the reason survives the hook being dropped and re-armed across
        // Unloaded / Loaded (the upstream code baked it into the one handler it subscribed).
        InitializeReason _layoutInitReason{ InitializeReason::Create };
        safe_void_coroutine _restoreInBackground();
        void _TappedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::TappedRoutedEventArgs& e);
        void _KeyDownHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _KeyUpHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _CharacterHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::CharacterReceivedRoutedEventArgs& e);
        void _PointerPressedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _PointerMovedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _PointerReleasedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _PointerExitedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _UpdateReadOnlyPointerCursor(bool pointerInside); // Agentmaster: UniversalNo cursor over a read-only (locked) terminal
        void _PointerCaptureLostHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e); // Agentmaster
        void _MouseWheelHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _ScrollbarChangeHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs& e);

        void _QuickFixButton_PointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _QuickFixButton_PointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        void _GotFocusHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _LostFocusHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);

        safe_void_coroutine _DragDropHandler(Windows::Foundation::IInspectable sender, Windows::UI::Xaml::DragEventArgs e);
        void _DragOverHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::DragEventArgs& e);

        safe_void_coroutine _HyperlinkHandler(Windows::Foundation::IInspectable sender, Control::OpenHyperlinkEventArgs e);

        void _BellLightOff(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& e);

        void _SetEndSelectionPointAtCursor(const Windows::Foundation::Point& cursorPosition);

        void _SwapChainSizeChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::SizeChangedEventArgs& e);
        void _SwapChainScaleChanged(const Windows::UI::Xaml::Controls::SwapChainPanel& sender, const Windows::Foundation::IInspectable& args);

        void _ScrollPositionChanged(const IInspectable& sender, const Control::ScrollPositionChangedArgs& args);

        bool _CapturePointer(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        bool _ReleasePointerCapture(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        void _TryStartAutoScroll(const Windows::UI::Input::PointerPoint& pointerPoint, const double scrollVelocity);
        void _TryStopAutoScroll(const uint32_t pointerId);
        void _UpdateAutoScroll(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& e);

        void _KeyHandler(const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e, const bool keyDown);
        bool _KeyHandler(WORD vkey, WORD scanCode, ::Microsoft::Terminal::Core::ControlKeyStates modifiers, bool keyDown);
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        bool _TryHandleKeyBinding(const WORD vkey, const WORD scanCode, ::Microsoft::Terminal::Core::ControlKeyStates modifiers) const;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        bool _TrySendKeyEvent(const WORD vkey, const WORD scanCode, ::Microsoft::Terminal::Core::ControlKeyStates modifiers, const bool keyDown);

        winrt::Windows::Foundation::Point _toControlOrigin(const til::point terminalPosition);
        Core::Point _toTerminalOrigin(winrt::Windows::Foundation::Point cursorPosition);

        double _GetAutoScrollSpeed(double cursorDistanceFromBorder) const;

        void _Search(const winrt::hstring& text, const bool goForward, const bool caseSensitive, const bool regularExpression);
        void _SearchChanged(const winrt::hstring& text, const bool goForward, const bool caseSensitive, const bool regularExpression);
        void _CloseSearchBoxControl(const winrt::Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _refreshSearch();
        void _handleSearchResults(SearchResults results);

        void _hoveredHyperlinkChanged(const IInspectable& sender, const IInspectable& args);
        safe_void_coroutine _updateSelectionMarkers(IInspectable sender, Control::UpdateSelectionMarkersEventArgs args);

        void _coreFontSizeChanged(const IInspectable& s, const Control::FontSizeChangedArgs& args);
        void _coreTransparencyChanged(IInspectable sender, Control::TransparencyChangedEventArgs args);
        void _coreRaisedNotice(const IInspectable& s, const Control::NoticeEventArgs& args);
        void _coreWarningBell(const IInspectable& sender, const IInspectable& args);
        void _coreOutputIdle(const IInspectable& sender, const IInspectable& args);

        winrt::Windows::Foundation::Point _toPosInDips(const Core::Point terminalCellPos);
        void _throttledUpdateScrollbar(const ScrollBarUpdate& update);

        void _pasteTextWithBroadcast(const winrt::hstring& text);

        void _contextMenuHandler(IInspectable sender, Control::ContextMenuRequestedEventArgs args);
        void _showContextMenuAt(const winrt::Windows::Foundation::Point& controlRelativePos);

        void _bubbleSearchMissingCommand(const IInspectable& sender, const Control::SearchMissingCommandEventArgs& args);
        winrt::fire_and_forget _bubbleWindowSizeChanged(const IInspectable& sender, Control::WindowSizeChangedEventArgs args);
        til::CoordType _calculateSearchScrollOffset() const;

        void _PasteCommandHandler(const IInspectable& sender, const IInspectable& args);
        void _CopyCommandHandler(const IInspectable& sender, const IInspectable& args);
        void _SearchCommandHandler(const IInspectable& sender, const IInspectable& args);

        void _SelectCommandHandler(const IInspectable& sender, const IInspectable& args);
        void _SelectOutputHandler(const IInspectable& sender, const IInspectable& args);

        struct Revokers
        {
            Control::ControlCore::ScrollPositionChanged_revoker coreScrollPositionChanged;
            Control::ControlCore::WarningBell_revoker WarningBell;
            Control::ControlCore::RendererEnteredErrorState_revoker RendererEnteredErrorState;
            Control::ControlCore::BackgroundColorChanged_revoker BackgroundColorChanged;
            Control::ControlCore::FontSizeChanged_revoker FontSizeChanged;
            Control::ControlCore::TransparencyChanged_revoker TransparencyChanged;
            Control::ControlCore::RaiseNotice_revoker RaiseNotice;
            Control::ControlCore::HoveredHyperlinkChanged_revoker HoveredHyperlinkChanged;
            Control::ControlCore::OutputIdle_revoker OutputIdle;
            Control::ControlCore::UpdateSelectionMarkers_revoker UpdateSelectionMarkers;
            Control::ControlCore::OpenHyperlink_revoker coreOpenHyperlink;
            Control::ControlCore::TitleChanged_revoker TitleChanged;
            Control::ControlCore::WriteToClipboard_revoker WriteToClipboard;
            Control::ControlCore::TabColorChanged_revoker TabColorChanged;
            Control::ControlCore::TaskbarProgressChanged_revoker TaskbarProgressChanged;
            Control::ControlCore::ConnectionStateChanged_revoker ConnectionStateChanged;
            Control::ControlCore::ShowWindowChanged_revoker ShowWindowChanged;
            Control::ControlCore::CloseTerminalRequested_revoker CloseTerminalRequested;
            Control::ControlCore::CompletionsChanged_revoker CompletionsChanged;
            Control::ControlCore::RestartTerminalRequested_revoker RestartTerminalRequested;
            Control::ControlCore::SearchMissingCommand_revoker SearchMissingCommand;
            Control::ControlCore::RefreshQuickFixUI_revoker RefreshQuickFixUI;
            Control::ControlCore::WindowSizeChanged_revoker WindowSizeChanged;

            // These are set up in _InitializeTerminal
            Control::ControlCore::RendererWarning_revoker RendererWarning;
            Control::ControlCore::SwapChainChanged_revoker SwapChainChanged;
            Windows::UI::ViewManagement::AccessibilitySettings::HighContrastChanged_revoker HighContrastChanged;

            Control::ControlInteractivity::OpenHyperlink_revoker interactivityOpenHyperlink;
            Control::ControlInteractivity::ScrollPositionChanged_revoker interactivityScrollPositionChanged;
            Control::ControlInteractivity::PasteFromClipboard_revoker PasteFromClipboard;
            Control::ControlInteractivity::ContextMenuRequested_revoker ContextMenuRequested;

            // Agentmaster (RENDERING.md §4c): this control's own tree membership drives the renderer's
            // suspension. Kept here so Close() and Detach() (both reset _revokers) also stop it.
            Windows::UI::Xaml::FrameworkElement::Loaded_revoker Loaded;
            Windows::UI::Xaml::FrameworkElement::Unloaded_revoker Unloaded;
        } _revokers{};
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(TermControl);
}
