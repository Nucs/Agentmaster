#include "pch.h"
#include "ColorPickupFlyout.h"
#include "ColorPickupFlyout.g.cpp"

#include "AgentCatchLog.h" // Agentmaster: AgentLogCaughtException — the "Use Tab Color" handler's containment (Rule #18)

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
        if (visibility == winrt::Windows::UI::Xaml::Visibility::Collapsed)
        {
            customColorPanel().Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
        }
        else
        {
            customColorPanel().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
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
