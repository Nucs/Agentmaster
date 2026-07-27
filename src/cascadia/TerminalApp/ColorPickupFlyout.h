#pragma once
#include "ColorPickupFlyout.g.h"

namespace winrt::TerminalApp::implementation
{
    struct ColorPickupFlyout : ColorPickupFlyoutT<ColorPickupFlyout>
    {
        ColorPickupFlyout();

        void ColorButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void ShowColorPickerButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void CustomColorButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void ClearColorButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void UseTabColorButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args); // Agentmaster
        void ColorPicker_ColorChanged(const Microsoft::UI::Xaml::Controls::ColorPicker&, const Microsoft::UI::Xaml::Controls::ColorChangedEventArgs& args);

        void SetCurrentTabColor(const Windows::Foundation::IReference<Windows::UI::Color>& color); // Agentmaster

        til::event<TerminalApp::ColorClearedArgs> ColorCleared;
        til::event<TerminalApp::ColorSelectedArgs> ColorSelected;

    private:
        // Agentmaster: what the attached tab currently wears (null => nothing to seed from, and the
        // "Use Tab Color" button is disabled). Re-pushed on every attach by Tab::AttachColorPicker.
        Windows::Foundation::IReference<Windows::UI::Color> _currentTabColor{ nullptr };
        // Agentmaster: set while WE write customColorPicker().Color(), so the on-the-fly
        // ColorChanged -> ColorSelected relay doesn't fire for a seed the user hasn't committed.
        bool _seedingPicker{ false };
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(ColorPickupFlyout);
}
