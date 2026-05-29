// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentManagerContent.h"

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::TerminalApp::implementation
{
    AgentManagerContent::AgentManagerContent()
    {
        _root = winrt::Windows::UI::Xaml::Controls::Grid{};
        // Vertical and HorizontalAlignment are Stretch by default.

        auto res = Windows::UI::Xaml::Application::Current().Resources();
        auto bg = res.Lookup(winrt::box_value(L"UnfocusedBorderBrush"));
        _root.Background(bg.try_as<Media::Brush>());

        // Scaffold placeholder. M6 replaces this with the C1 "Linked Lenses" layout:
        //   row 0           -> Triage Board
        //   row 1 (split)   -> [ Explorer Tree | Flight Plan ]
        auto stack = winrt::Windows::UI::Xaml::Controls::StackPanel{};
        stack.VerticalAlignment(VerticalAlignment::Center);
        stack.HorizontalAlignment(HorizontalAlignment::Center);

        auto title = winrt::Windows::UI::Xaml::Controls::TextBlock{};
        title.Text(L"Agent Manager");
        title.FontSize(28);
        title.HorizontalAlignment(HorizontalAlignment::Center);

        auto subtitle = winrt::Windows::UI::Xaml::Controls::TextBlock{};
        subtitle.Text(L"Triage Board · Explorer Tree · Flight Plan  (scaffold — M6)");
        subtitle.Opacity(0.6);
        subtitle.Margin({ 0, 8, 0, 0 });
        subtitle.HorizontalAlignment(HorizontalAlignment::Center);

        stack.Children().Append(title);
        stack.Children().Append(subtitle);
        _root.Children().Append(stack);
    }

    void AgentManagerContent::UpdateSettings(const CascadiaSettings& /*settings*/)
    {
        // Nothing to do yet.
    }

    winrt::Windows::UI::Xaml::FrameworkElement AgentManagerContent::GetRoot()
    {
        return _root;
    }
    winrt::Windows::Foundation::Size AgentManagerContent::MinimumSize()
    {
        return { 1, 1 };
    }
    void AgentManagerContent::Focus(winrt::Windows::UI::Xaml::FocusState reason)
    {
        _root.Focus(reason);
    }
    void AgentManagerContent::Close()
    {
    }

    INewContentArgs AgentManagerContent::GetNewTerminalArgs(const BuildStartupKind /* kind */) const
    {
        return BaseContentArgs(L"agentManager");
    }

    winrt::hstring AgentManagerContent::Icon() const
    {
        static constexpr std::wstring_view glyph{ L"\xE71D" }; // AllApps — the "manager of all sessions"
        return winrt::hstring{ glyph };
    }

    winrt::Windows::UI::Xaml::Media::Brush AgentManagerContent::BackgroundBrush()
    {
        return _root.Background();
    }
}
