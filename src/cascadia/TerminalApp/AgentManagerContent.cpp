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

        // Scaffold layout. M6 replaces this with the full C1 "Linked Lenses" layout:
        //   row 0           -> Triage Board
        //   row 1 (split)   -> [ Explorer Tree | Flight Plan ]
        // For M5 the engine (SessionRegistry + hooks bridge) is live; this provides the
        // minimal affordance to launch a real Claude session and watch state flow.
        auto stack = winrt::Windows::UI::Xaml::Controls::StackPanel{};
        stack.VerticalAlignment(VerticalAlignment::Center);
        stack.HorizontalAlignment(HorizontalAlignment::Center);
        stack.MaxWidth(760);

        auto title = winrt::Windows::UI::Xaml::Controls::TextBlock{};
        title.Text(L"Agent Manager");
        title.FontSize(28);
        title.HorizontalAlignment(HorizontalAlignment::Center);

        auto subtitle = winrt::Windows::UI::Xaml::Controls::TextBlock{};
        subtitle.Text(L"Triage Board · Explorer Tree · Flight Plan — engine live (M5); full C1 UI in M6");
        subtitle.Opacity(0.6);
        subtitle.Margin({ 0, 8, 0, 16 });
        subtitle.TextWrapping(TextWrapping::Wrap);
        subtitle.HorizontalAlignment(HorizontalAlignment::Center);

        // Launcher row: [ cwd TextBox ] [ Launch Claude session ]
        auto launchRow = winrt::Windows::UI::Xaml::Controls::StackPanel{};
        launchRow.Orientation(Orientation::Horizontal);
        launchRow.HorizontalAlignment(HorizontalAlignment::Center);
        launchRow.Spacing(8);

        _cwdBox = winrt::Windows::UI::Xaml::Controls::TextBox{};
        _cwdBox.Width(460);
        _cwdBox.PlaceholderText(L"working directory (the M axis)");
        {
            wchar_t up[MAX_PATH];
            const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                _cwdBox.Text(winrt::hstring{ up, n });
            }
        }

        auto launchBtn = winrt::Windows::UI::Xaml::Controls::Button{};
        launchBtn.Content(winrt::box_value(L"Launch Claude session"));
        launchBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            if (_spawnHandler)
            {
                const auto dir = _cwdBox ? _cwdBox.Text() : winrt::hstring{};
                _spawnHandler(dir, winrt::hstring{});
                _AppendStatus(L"launched a session in: " + dir);
            }
        });

        launchRow.Children().Append(_cwdBox);
        launchRow.Children().Append(launchBtn);

        _status = winrt::Windows::UI::Xaml::Controls::TextBlock{};
        _status.Opacity(0.7);
        _status.Margin({ 0, 16, 0, 0 });
        _status.TextWrapping(TextWrapping::Wrap);
        _status.HorizontalAlignment(HorizontalAlignment::Center);
        _status.Text(L"State is hook-driven; transitions are logged to %LOCALAPPDATA%\\Agentmaster\\hooks.log");

        stack.Children().Append(title);
        stack.Children().Append(subtitle);
        stack.Children().Append(launchRow);
        stack.Children().Append(_status);
        _root.Children().Append(stack);
    }

    void AgentManagerContent::SetSpawnHandler(std::function<void(winrt::hstring, winrt::hstring)> handler)
    {
        _spawnHandler = std::move(handler);
    }

    void AgentManagerContent::_AppendStatus(const winrt::hstring& line)
    {
        if (_status)
        {
            _status.Text(_status.Text() + L"\n" + line);
        }
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
    void AgentManagerContent::Focus(winrt::Windows::UI::Xaml::FocusState /*reason*/)
    {
        // A Grid/Panel isn't focusable and the placeholder has no focusable child yet.
        // M6 focuses the primary C1 control (Explorer Tree / search box).
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
