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
        void Flyout_Opening(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args); // Agentmaster
        void ColorPicker_Loaded(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args); // Agentmaster

        void SetCurrentTabColor(const Windows::Foundation::IReference<Windows::UI::Color>& color); // Agentmaster

        til::event<TerminalApp::ColorClearedArgs> ColorCleared;
        til::event<TerminalApp::ColorSelectedArgs> ColorSelected;

    private:
        // Agentmaster (persisted expansion): apply / queue / persist the two open-state settings.
        void _ApplyAdvancedNow();
        void _QueueAdvancedApply();
        void _PersistCustomOpen(bool open);
        void _PersistAdvancedOpen(bool open);

        // Agentmaster: what the attached tab currently wears (null => nothing to seed from, and the
        // "Use Tab Color" button is disabled). Re-pushed on every attach by Tab::AttachColorPicker.
        Windows::Foundation::IReference<Windows::UI::Color> _currentTabColor{ nullptr };
        // Agentmaster: set while WE write customColorPicker().Color(), so the on-the-fly
        // ColorChanged -> ColorSelected relay doesn't fire for a seed the user hasn't committed.
        bool _seedingPicker{ false };

        // Agentmaster (persisted expansion): WinUI's ColorPicker has no public property for its
        // More/Less ("advanced") state, so we drive its `MoreButton` TEMPLATE PART. Cached once
        // found; null while the template hasn't been applied yet (a collapsed panel is never
        // measured) or if a future WinUI renames the part — in which case the picker just opens
        // collapsed, exactly as it did before this feature.
        Windows::UI::Xaml::Controls::Primitives::ToggleButton _moreButton{ nullptr };
        bool _moreButtonHooked{ false }; // its Checked/Unchecked -> persist, wired once
        bool _applyingAdvanced{ false }; // our own IsChecked write is not a user toggle -> don't persist it
        bool _advancedApplyQueued{ false }; // coalesces the deferred (post-layout) apply
        bool _advancedWanted{ true }; // the persisted state this open should show; re-read per open
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(ColorPickupFlyout);
}
