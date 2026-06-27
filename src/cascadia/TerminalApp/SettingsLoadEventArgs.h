// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once

#include "SettingsLoadEventArgs.g.h"
#include <inc/cppwinrt_utils.h>
namespace winrt::TerminalApp::implementation
{
    struct SettingsLoadEventArgs : SettingsLoadEventArgsT<SettingsLoadEventArgs>
    {
        WINRT_PROPERTY(bool, Reload, false);
        WINRT_PROPERTY(uint64_t, Result, S_OK);
        WINRT_PROPERTY(winrt::hstring, ExceptionText, L"");
        WINRT_PROPERTY(winrt::Windows::Foundation::Collections::IVectorView<Microsoft::Terminal::Settings::Model::SettingsLoadWarnings>, Warnings, nullptr);
        WINRT_PROPERTY(Microsoft::Terminal::Settings::Model::CascadiaSettings, NewSettings, nullptr);
        // Agentmaster: true when this reload was triggered by a keyboard-layout change and only
        // the keybindings need re-resolving (not the full per-pane settings reapply). See
        // AppLogic::_reloadSettingsImpl / TerminalPage::RefreshKeybindings.
        WINRT_PROPERTY(bool, KeybindingsOnly, false);

    public:
        SettingsLoadEventArgs(bool reload,
                              uint64_t result,
                              winrt::hstring exceptionText,
                              winrt::Windows::Foundation::Collections::IVectorView<Microsoft::Terminal::Settings::Model::SettingsLoadWarnings> warnings,
                              Microsoft::Terminal::Settings::Model::CascadiaSettings newSettings,
                              bool keybindingsOnly = false) :
            _Reload{ reload },
            _Result{ result },
            _ExceptionText{ std::move(exceptionText) },
            _Warnings{ std::move(warnings) },
            _NewSettings{ std::move(newSettings) },
            _KeybindingsOnly{ keybindingsOnly } {};
    };
}
