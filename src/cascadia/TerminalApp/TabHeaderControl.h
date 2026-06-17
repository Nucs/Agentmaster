// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "winrt/Microsoft.UI.Xaml.Controls.h"

#include "TabHeaderControl.g.h"

namespace winrt::TerminalApp::implementation
{
    // Agentmaster: the process-global tab-rename commit mode (mirrors AppSettings::tabRenameCommitMode;
    // see SessionModels.h). It is GLOBAL across windows, so rather than plumb the value into every
    // Tab's header on each settings change, TerminalPage writes this one process-wide value (on
    // AppSettings load + cog Save) and every header's rename box reads it live on a keypress. The raw
    // int avoids coupling this leaf control to the engine model; values match TabRenameCommitMode
    // (0 = click-away only, 1 = +Shift+Enter, 2 = +Enter), locked by a static_assert in
    // TerminalPage.AgentEngine.cpp.
    void SetTabRenameCommitMode(int32_t mode) noexcept;

    struct TabHeaderControl : TabHeaderControlT<TabHeaderControl>
    {
        TabHeaderControl();
        void BeginRename();

        void RenameBoxLostFocusHandler(const winrt::Windows::Foundation::IInspectable& sender,
                                       const winrt::Windows::UI::Xaml::RoutedEventArgs& e);

        bool InRename();

        til::event<TerminalApp::TitleChangeRequestedArgs> TitleChangeRequested;
        til::typed_event<> RenameEnded;

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Title, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(double, RenamerMaxWidth, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::TerminalApp::TerminalTabStatus, TabStatus, PropertyChanged.raise);

    private:
        bool _receivedKeyDown{ false };
        bool _renameCancelled{ false };
        // Agentmaster: set in PreviewKeyDown when the commit combo (Enter / Shift+Enter, per the
        // global mode) is pressed — where we also suppress the AcceptsReturn newline — and consumed
        // on the matching KeyUp to commit (the original upstream note warns that closing the box on a
        // *down* event lets the up bubble to the NewTabButton, so we defer the close to key-up).
        bool _commitOnKeyUp{ false };

        void _CloseRenameBox();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TabHeaderControl);
}
