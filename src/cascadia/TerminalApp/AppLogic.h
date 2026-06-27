// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once

#include "AppLogic.g.h"

#include "Jumplist.h"
#include "LanguageProfileNotifier.h"
#include "AppCommandlineArgs.h"
#include "TerminalWindow.h"
#include "ContentManager.h"

#include <inc/cppwinrt_utils.h>
#include <ThrottledFunc.h>

#ifdef UNIT_TESTING
// fwdecl unittest classes
namespace TerminalAppLocalTests
{
    class CommandlineTest;
};
#endif

namespace winrt::TerminalApp::implementation
{
    struct AppLogic : AppLogicT<AppLogic>
    {
    public:
        static AppLogic* Current() noexcept;
        static const Microsoft::Terminal::Settings::Model::CascadiaSettings CurrentAppSettings();

        AppLogic();

        void Create();
        bool IsRunningElevated() const noexcept;
        bool CanDragDrop() const noexcept;
        void ReloadSettings();
        void ReloadSettingsThrottled();
        void NotifyRootInitialized();

        bool HasSettingsStartupActions() const noexcept;

        Microsoft::Terminal::Settings::Model::CascadiaSettings Settings() const noexcept;

        Windows::Foundation::Collections::IMapView<Microsoft::Terminal::Control::KeyChord, Microsoft::Terminal::Settings::Model::Command> GlobalHotkeys();

        TerminalApp::TerminalWindow CreateNewWindow();

        winrt::TerminalApp::ContentManager ContentManager();

        til::typed_event<winrt::Windows::Foundation::IInspectable, winrt::TerminalApp::SettingsLoadEventArgs> SettingsChanged;

    private:
        bool _isElevated{ false };
        bool _canDragDrop{ false };
        std::atomic<bool> _notifyRootInitializedCalled{ false };

        Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };

        winrt::hstring _settingsLoadExceptionText;
        HRESULT _settingsLoadedResult = S_OK;
        bool _loadedInitialSettings = false;

        bool _hasSettingsStartupActions{ false };
        ::TerminalApp::AppCommandlineArgs _settingsAppArgs;

        std::shared_ptr<ThrottledFunc<>> _reloadSettings;
        // Agentmaster: a keyboard-layout change routes here instead of _reloadSettings, so the
        // reload applies ONLY keybindings to the live UI (no per-pane reapply). See AppLogic.cpp.
        // Declared before _languageProfileNotifier so it outlives the notifier that calls it.
        std::shared_ptr<ThrottledFunc<>> _reloadKeybindingsForLayout;

        std::vector<Microsoft::Terminal::Settings::Model::SettingsLoadWarnings> _warnings{};

        // These fields invoke _reloadSettings and must be destroyed before _reloadSettings.
        // (C++ destroys members in reverse-declaration-order.)
        winrt::com_ptr<LanguageProfileNotifier> _languageProfileNotifier;
        wil::unique_folder_change_reader_nothrow _reader;

        TerminalApp::ContentManager _contentManager{ winrt::make<implementation::ContentManager>() };

        void _ApplyLanguageSettingChange() noexcept;
        // Agentmaster: shared body of ReloadSettings(); keybindingsOnly==true tags the
        // SettingsLoadEventArgs so windows skip the heavy per-pane reapply on a layout change.
        void _reloadSettingsImpl(bool keybindingsOnly);

        [[nodiscard]] HRESULT _TryLoadSettings() noexcept;
        void _ProcessLazySettingsChanges();
        void _RegisterSettingsChange();

        void _setupFolderPathEnvVar();

#ifdef UNIT_TESTING
        friend class TerminalAppLocalTests::CommandlineTest;
#endif
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(AppLogic);
}
