// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "InteractionViewModel.h"
#include "InteractionViewModel.g.cpp"
#include "EnumEntry.h"

using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    InteractionViewModel::InteractionViewModel(Model::GlobalAppSettings globalSettings) :
        _GlobalSettings{ globalSettings }
    {
        INITIALIZE_BINDABLE_ENUM_SETTING(TabSwitcherMode, TabSwitcherMode, TabSwitcherMode, L"Globals_TabSwitcherMode", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(CopyFormat, CopyFormat, winrt::Microsoft::Terminal::Control::CopyFormat, L"Globals_CopyFormat", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(ConfirmOnClose, ConfirmOnClose, Model::ConfirmOnClose, L"Globals_ConfirmOnClose", L"Content");

        // Agentmaster: "Never" (fully suppress close confirmations) is not an allowed choice — drop it
        // from the picker so it can't be selected. The model also coerces any persisted "never" to
        // "automatic" (GlobalAppSettings::LayerJson), so a close confirmation is never permanently
        // suppressible from anywhere.
        for (uint32_t i = 0; i < _ConfirmOnCloseList.Size(); ++i)
        {
            if (winrt::unbox_value<Model::ConfirmOnClose>(_ConfirmOnCloseList.GetAt(i).EnumValue()) == Model::ConfirmOnClose::Never)
            {
                _ConfirmOnCloseList.RemoveAt(i);
                break;
            }
        }
    }
}
