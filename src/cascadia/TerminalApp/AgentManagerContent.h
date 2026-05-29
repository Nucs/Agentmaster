// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster: content for the pinned, leftmost "Agent Manager" tab.
//
// Implemented as an IPaneContent, exactly like ScratchpadContent / SettingsPaneContent,
// so it plugs into Windows Terminal's pane/tab model with no IDL/projection changes.
// The C1 "Linked Lenses" layout (Triage Board + Explorer Tree + Flight Plan) is built
// inside GetRoot(); this scaffold renders a placeholder until M6.
// See doc/agentmaster/IMPLEMENTATION.md.

#pragma once
#include "winrt/TerminalApp.h"
#include "BasicPaneEvents.h"

namespace winrt::TerminalApp::implementation
{
    class AgentManagerContent : public winrt::implements<AgentManagerContent, IPaneContent>, public BasicPaneEvents
    {
    public:
        AgentManagerContent();

        winrt::Windows::UI::Xaml::FrameworkElement GetRoot();

        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        winrt::Windows::Foundation::Size MinimumSize();

        void Focus(winrt::Windows::UI::Xaml::FocusState reason = winrt::Windows::UI::Xaml::FocusState::Programmatic);
        void Close();
        winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(BuildStartupKind kind) const;

        winrt::hstring Title() { return L"Agent Manager"; }
        uint64_t TaskbarState() { return 0; }
        uint64_t TaskbarProgress() { return 0; }
        bool ReadOnly() { return false; }
        winrt::hstring Icon() const;
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept { return nullptr; }
        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush();

        // See BasicPaneEvents for most generic event definitions

    private:
        winrt::Windows::UI::Xaml::Controls::Grid _root{ nullptr };
    };
}
