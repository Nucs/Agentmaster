#include "pch.h"
#include "ColorPickupFlyout.h"
#include "ColorPickupFlyout.g.cpp"

#include "AgentCatchLog.h" // Agentmaster: AgentLogCaughtException — the "Use Tab Color" handler's containment (Rule #18)
#include "AgentMaster/Persistence.h" // Agentmaster: Load/SaveAppSettings — the persisted Custom / advanced expansion states

// Agentmaster (persisted "advanced" expansion): depth-first walk for the muxc::ColorPicker's
// `MoreButton` TEMPLATE PART — the More/Less ToggleButton that reveals the RGB/HSV/Hex text inputs
// (Microsoft.UI.Xaml 2.8 Generic.xaml: the ColorPicker template's MoreEntriesPanel). The control
// exposes no property for its expanded state (only IsMoreButtonVisible, which HIDES the button
// entirely), and template parts live in the template's own namescope so FindName can't see them —
// a visual-tree walk is the only reach. Fail-soft by design: no part found => we leave the picker
// exactly as WinUI built it.
static winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton _FindColorPickerMoreButton(const winrt::Windows::UI::Xaml::DependencyObject& root)
{
    if (!root)
    {
        return nullptr;
    }
    const auto count = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int32_t i = 0; i < count; ++i)
    {
        const auto child = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(root, i);
        if (const auto tb = child.try_as<winrt::Windows::UI::Xaml::Controls::Primitives::ToggleButton>())
        {
            if (const auto fe = child.try_as<winrt::Windows::UI::Xaml::FrameworkElement>(); fe && fe.Name() == L"MoreButton")
            {
                return tb;
            }
        }
        if (auto found = _FindColorPickerMoreButton(child))
        {
            return found;
        }
    }
    return nullptr;
}

namespace winrt::TerminalApp::implementation
{
    // Method Description:
    // - Default constructor, localizes the buttons and hooks
    // up the event fired by the custom color picker, so that
    // the tab color is set on the fly when selecting a non-preset color
    // Arguments:
    // - <none>
    ColorPickupFlyout::ColorPickupFlyout()
    {
        InitializeComponent();

        OkButton().Content(winrt::box_value(RS_(L"Ok")));
        CustomColorButton().Content(winrt::box_value(RS_(L"TabColorCustomButton/Content")));
        ClearColorButton().Content(winrt::box_value(RS_(L"TabColorClearButton/Content")));
        UseTabColorButton().Content(winrt::box_value(RS_(L"TabColorUseTabColorButton/Content"))); // Agentmaster
    }

    // Agentmaster
    // Method Description:
    // - Tells us what color the tab we're about to be shown for is CURRENTLY wearing, so the
    //   "Use Tab Color" button can seed the custom picker with it. Without it the picker opens on
    //   black — and, being a singleton, thereafter on whatever the LAST tab was given — so the color
    //   the user actually set out to tweak was the one thing the picker never showed.
    // - This flyout is a per-window SINGLETON reused by every tab (TerminalPage::_tabColorPicker),
    //   so Tab::AttachColorPicker pushes this on EVERY attach — including a null, which is what
    //   keeps the previously-picked tab's color from being offered for a colorless one.
    // Arguments:
    // - color: the tab's effective color (Tab::GetTabColor), or null when it has none
    // Return Value:
    // - <none>
    void ColorPickupFlyout::SetCurrentTabColor(const Windows::Foundation::IReference<Windows::UI::Color>& color)
    {
        _currentTabColor = color;
        UseTabColorButton().IsEnabled(static_cast<bool>(color));
    }

    // Method Description:
    // - Handler of the click event for the preset color swatches.
    // Reads the color from the clicked rectangle and fires an event
    // with the selected color. After that hides the flyout
    // Arguments:
    // - sender: the rectangle that got clicked
    // Return Value:
    // - <none>
    void ColorPickupFlyout::ColorButton_Click(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        auto button{ sender.as<Windows::UI::Xaml::Controls::Button>() };
        auto rectClr{ button.Background().as<Windows::UI::Xaml::Media::SolidColorBrush>() };
        ColorSelected.raise(rectClr.Color());
        Hide();
    }

    // Method Description:
    // - Handler of the clear color button. Clears the current
    // color of the tab, if any. Hides the flyout after that
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::ClearColorButton_Click(const IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        ColorCleared.raise();
        Hide();
    }

    // Method Description:
    // - Handler of the select custom color button. Expands or collapses the flyout
    // to show the color picker. In order to accomplish this a FlyoutPresenterStyle is used,
    // in which a Style is embedded, containing the desired width
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::ShowColorPickerButton_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        auto visibility = customColorPanel().Visibility();
        const auto expanding = visibility == winrt::Windows::UI::Xaml::Visibility::Collapsed; // Agentmaster
        if (visibility == winrt::Windows::UI::Xaml::Visibility::Collapsed)
        {
            customColorPanel().Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
        }
        else
        {
            customColorPanel().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
        }

        // Agentmaster: remember how the user left it — the next open (any window, any run) starts here.
        _PersistCustomOpen(expanding);
        if (expanding)
        {
            // The picker was COLLAPSED, so XAML never measured it and its template (and therefore the
            // MoreButton part) may not exist yet — and Loaded does NOT re-fire on a visibility change,
            // since the element was in the tree all along. Queue the advanced apply for the clean tick
            // AFTER this expansion lays out, which is also the only safe time to mutate a control
            // inside an open popup (the E_LAYOUTCYCLE lesson).
            _QueueAdvancedApply();
        }
    }

    // Agentmaster
    // Method Description:
    // - Fired just before the flyout is shown, from EVERY entry point (tab context menu, the
    //   openTabColorPicker action, the command palette). Re-reads the two persisted expansion
    //   states FRESH from disk — so a change made in another window (or another run) is honored —
    //   and applies the Custom one, which is a plain property set and needs no template.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::Flyout_Opening(const Windows::Foundation::IInspectable&, const Windows::Foundation::IInspectable&)
    {
        // CONTAINED: a XAML event handler, so an escape is a process fail-fast. A failed read just
        // means the flyout opens however it last looked - never a lost picker.
        try
        {
            const auto s = ::Agentmaster::LoadAppSettings();
            _advancedWanted = s.tabColorPickerAdvancedOpen;

            const bool custom = s.tabColorPickerCustomOpen;
            // IsChecked is set programmatically here, which raises Checked/Unchecked but NOT Click —
            // and Click is what ShowColorPickerButton_Click listens on. So the toggle and the panel
            // stay in step with no re-entrancy and nothing is persisted by this seeding.
            CustomColorButton().IsChecked(custom);
            customColorPanel().Visibility(custom ? winrt::Windows::UI::Xaml::Visibility::Visible :
                                                   winrt::Windows::UI::Xaml::Visibility::Collapsed);
            if (custom)
            {
                _QueueAdvancedApply();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"ColorPickupFlyout Opening");
        }
    }

    // Agentmaster
    // Method Description:
    // - The custom picker entered the visual tree, so its template has been applied and the
    //   MoreButton part exists: the synchronous (flicker-free) chance to apply the persisted
    //   "advanced" state, before the popup's first frame.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::ColorPicker_Loaded(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        _ApplyAdvancedNow();
    }

    // Agentmaster
    // Method Description:
    // - Coalescing scheduler for the advanced apply: does the reads + mutations on a CLEAN dispatcher
    //   tick, after layout settles. This is the house idiom for touching a control inside an open
    //   popup (TabHeaderControl::_PositionTagBadges) — mutating one synchronously from a
    //   layout-driven trigger re-enters the pass and trips XAML's E_LAYOUTCYCLE detector.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::_QueueAdvancedApply()
    {
        if (_advancedApplyQueued)
        {
            return; // already scheduled — triggers coalesce
        }
        _advancedApplyQueued = true;
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak()]() {
                if (auto self = weakThis.get())
                {
                    self->_advancedApplyQueued = false;
                    self->_ApplyAdvancedNow();
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"ColorPickupFlyout advanced apply schedule");
            _advancedApplyQueued = false; // no dispatcher (teardown) — drop the request
        }
    }

    // Agentmaster
    // Method Description:
    // - Drive the ColorPicker's own More/Less expander to the persisted "advanced" state, and (once)
    //   hook it so the user's own toggling is what gets remembered from then on.
    // - Idempotent and fail-soft: no part (template not applied yet, or a future WinUI renamed it)
    //   simply leaves the picker as WinUI built it.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::_ApplyAdvancedNow()
    {
        // CONTAINED: reached from a XAML handler and from a dispatcher callback — an escape in
        // either is a process fail-fast, and the whole feature is cosmetic.
        try
        {
            // Re-resolved every time rather than cached-forever: a template re-apply (a theme change
            // re-runs OnApplyTemplate) mints a NEW MoreButton and orphans the old one, and driving an
            // orphan would silently stop working. The walk is over the picker's own small subtree and
            // runs at most a couple of times per flyout open, so re-finding is cheaper than the bug.
            const auto more = _FindColorPickerMoreButton(customColorPicker());
            if (!more)
            {
                return; // not templated yet (a collapsed picker is never measured) / part renamed
            }
            if (more != _moreButton)
            {
                _moreButton = more;
                _moreButtonHooked = false; // a different button — its Checked/Unchecked need wiring
            }

            if (!_moreButtonHooked)
            {
                _moreButtonHooked = true;
                const auto persist = [weakThis = get_weak()](bool open) {
                    if (auto self = weakThis.get())
                    {
                        if (!self->_applyingAdvanced) // our own seeding is not a user toggle
                        {
                            self->_PersistAdvancedOpen(open);
                        }
                    }
                };
                _moreButton.Checked([persist](auto&&, auto&&) { persist(true); });
                _moreButton.Unchecked([persist](auto&&, auto&&) { persist(false); });
            }

            const auto checkedRef = _moreButton.IsChecked();
            const bool current = checkedRef && checkedRef.Value(); // null == indeterminate == collapsed
            if (current != _advancedWanted)
            {
                _applyingAdvanced = true;
                auto clearApplying = wil::scope_exit([this]() noexcept { _applyingAdvanced = false; });
                _moreButton.IsChecked(_advancedWanted);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"ColorPickupFlyout apply advanced");
        }
    }

    // Agentmaster
    // Method Description:
    // - Persist one of the two expansion states: a FRESHEST-DISK read-modify-write of just that
    //   field (the _ToggleSummaryPanel idiom), so a concurrent cog Save or another window's flip is
    //   never clobbered by a stale in-memory copy.
    // - Best-effort: a failed write costs only the remembered state, never the toggle the user just
    //   made (the panel is already showing it).
    // Arguments:
    // - open: the state the user just left the toggle in
    // Return Value:
    // - <none>
    void ColorPickupFlyout::_PersistCustomOpen(bool open)
    {
        try
        {
            auto s = ::Agentmaster::LoadAppSettings();
            if (s.tabColorPickerCustomOpen != open)
            {
                s.tabColorPickerCustomOpen = open;
                ::Agentmaster::SaveAppSettings(s);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"ColorPickupFlyout persist custom-open");
        }
    }

    void ColorPickupFlyout::_PersistAdvancedOpen(bool open)
    {
        try
        {
            _advancedWanted = open; // this open's live intent, so a re-apply in the same session agrees
            auto s = ::Agentmaster::LoadAppSettings();
            if (s.tabColorPickerAdvancedOpen != open)
            {
                s.tabColorPickerAdvancedOpen = open;
                ::Agentmaster::SaveAppSettings(s);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"ColorPickupFlyout persist advanced-open");
        }
    }

    // Method Description:
    // - Handles the color selection of the color pickup. Gets
    // the currently selected color and fires an event with it
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::CustomColorButton_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        auto color = customColorPicker().Color();
        ColorSelected.raise(color);
        Hide();
    }

    // Agentmaster
    // Method Description:
    // - Handler of the "Use Tab Color" button. Loads the tab's CURRENT color into the custom color
    //   picker, so the user tweaks the color the tab already wears instead of starting from black /
    //   the previous tab's pick.
    // - Deliberately commits NOTHING: the seed is suppressed out of the on-the-fly
    //   ColorChanged -> ColorSelected relay, so the tab keeps looking exactly as it does until the
    //   user actually drags the picker or presses OK. (Without that, seeding a tab whose color comes
    //   from its PROFILE would silently promote it to a user-chosen runtime color.)
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ColorPickupFlyout::UseTabColorButton_Click(const Windows::Foundation::IInspectable&, const Windows::UI::Xaml::RoutedEventArgs&)
    {
        if (!_currentTabColor)
        {
            return; // nothing to seed from (the button is disabled in this state anyway)
        }

        // CONTAINED: this runs as a XAML event handler, so an escaping exception is a process
        // fail-fast, not a failed seed. Scoped, so the latch is cleared on the throw path too --
        // leaking it true would silently kill the on-the-fly ColorChanged -> ColorSelected relay
        // for the rest of this flyout's life (it is a per-window singleton), turning a one-off
        // failure into "dragging the picker stopped recoloring the tab".
        try
        {
            // Cleared by the scope, NOT from the ColorChanged handler: the set raises it
            // synchronously today, but a set that raises nothing (the picker already holds this
            // color) would otherwise wedge the latch on. Worst case of clearing here is a harmless
            // re-apply of the tab's own color.
            auto clearSeeding = wil::scope_exit([this]() noexcept { _seedingPicker = false; });
            _seedingPicker = true;
            customColorPicker().Color(_currentTabColor.Value());
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"ColorPickupFlyout Use Tab Color");
        }
    }

    void ColorPickupFlyout::ColorPicker_ColorChanged(const Microsoft::UI::Xaml::Controls::ColorPicker&, const Microsoft::UI::Xaml::Controls::ColorChangedEventArgs& args)
    {
        // Agentmaster: a "Use Tab Color" seed is not a user pick — see UseTabColorButton_Click.
        if (_seedingPicker)
        {
            return;
        }
        ColorSelected.raise(args.NewColor());
    }
}
