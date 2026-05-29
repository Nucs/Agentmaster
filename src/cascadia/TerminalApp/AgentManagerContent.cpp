// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/SessionRegistry.h"

#include <algorithm>

using namespace winrt::Windows::Foundation;
// Using-DECLARATIONS (not a directive) for the color helpers: a `using namespace
// winrt::Windows::UI;` would also pull the nested `Text` namespace into scope and collide
// with our Text() TextBlock helper below.
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Colors;
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input; // KeyRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue, VirtualKey
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace Agentmaster;

namespace
{
    SolidColorBrush Fill(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
    }

    Color StateColor(SessionState s)
    {
        switch (s)
        {
        case SessionState::Running:
            return Colors::DodgerBlue();
        case SessionState::WaitingForInput:
            return Colors::Goldenrod();
        case SessionState::NeedsApproval:
            return Colors::OrangeRed();
        case SessionState::Error:
            return Colors::Crimson();
        case SessionState::Done:
            return Colors::MediumSeaGreen();
        case SessionState::Idle:
        default:
            return Colors::Gray();
        }
    }

    winrt::hstring StateLabel(SessionState s)
    {
        switch (s)
        {
        case SessionState::Running:
            return L"running";
        case SessionState::WaitingForInput:
            return L"waiting-for-you";
        case SessionState::NeedsApproval:
            return L"needs-approval";
        case SessionState::Error:
            return L"error";
        case SessionState::Done:
            return L"done";
        case SessionState::Idle:
        default:
            return L"idle";
        }
    }

    winrt::hstring StateGlyph(SessionState s)
    {
        switch (s)
        {
        case SessionState::Running:
            return L"\x25CF"; // ●
        case SessionState::WaitingForInput:
            return L"\x25D0"; // ◐
        case SessionState::NeedsApproval:
            return L"\x26A0"; // ⚠
        case SessionState::Error:
            return L"\x2715"; // ✕
        case SessionState::Done:
            return L"\x2713"; // ✓
        case SessionState::Idle:
        default:
            return L"\x25CB"; // ○
        }
    }

    winrt::hstring PromptGlyph(PromptStatus s)
    {
        switch (s)
        {
        case PromptStatus::Sent:
            return L"\x2713"; // ✓
        case PromptStatus::Held:
            return L"\x26D4"; // ⛔
        case PromptStatus::Skipped:
            return L"\x2014"; // —
        case PromptStatus::Failed:
            return L"\x2715"; // ✕
        case PromptStatus::Pending:
        default:
            return L"\x23F3"; // ⏳
        }
    }

    winrt::hstring GateBadge(PromptGate g)
    {
        switch (g)
        {
        case PromptGate::AfterDelay:
            return L"[delay]";
        case PromptGate::Manual:
            return L"[manual]";
        case PromptGate::OnTurnComplete:
        default:
            return L"[turn-done]";
        }
    }

    TextBlock Text(const winrt::hstring& s, double size, bool bold, double opacity)
    {
        TextBlock t;
        t.Text(s);
        t.FontSize(size);
        if (bold)
        {
            t.FontWeight(FontWeights::SemiBold());
        }
        t.Opacity(opacity);
        t.TextWrapping(TextWrapping::NoWrap);
        t.TextTrimming(TextTrimming::CharacterEllipsis);
        t.VerticalAlignment(VerticalAlignment::Center);
        return t;
    }

    // A small colored "pill" showing a state label.
    Border Pill(const winrt::hstring& label, const Color& accent)
    {
        Border b;
        b.Background(SolidColorBrush{ accent });
        b.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        b.Padding(Thickness{ 8, 1, 8, 1 });
        b.VerticalAlignment(VerticalAlignment::Center);
        auto t = Text(label, 11, true, 1.0);
        t.Foreground(Fill(0xFF, 0x10, 0x10, 0x10));
        b.Child(t);
        return b;
    }
}

namespace winrt::TerminalApp::implementation
{
    AgentManagerContent::AgentManagerContent()
    {
        _root = Grid{};
        _dispatcher = DispatcherQueue::GetForCurrentThread();
        _templates = ::Agentmaster::LoadTemplates(); // persisted plan templates (M8)

        try
        {
            auto res = Application::Current().Resources();
            if (res.HasKey(winrt::box_value(L"UnfocusedBorderBrush")))
            {
                _root.Background(res.Lookup(winrt::box_value(L"UnfocusedBorderBrush")).try_as<Brush>());
            }
        }
        catch (...)
        {
        }

        _BuildLayout();
    }

    void AgentManagerContent::SetRegistry(std::shared_ptr<::Agentmaster::SessionRegistry> registry)
    {
        _registry = std::move(registry);
        if (_registry)
        {
            auto weak = get_weak();
            auto disp = _dispatcher;
            _registry->AddObserver([weak, disp](const SessionInfo&, HookEvent) {
                if (disp)
                {
                    disp.TryEnqueue([weak]() {
                        if (auto self = weak.get())
                        {
                            self->_Refresh();
                        }
                    });
                }
            });
        }
        _Refresh();
    }

    void AgentManagerContent::SetSpawnHandler(std::function<void(winrt::hstring, winrt::hstring)> handler)
    {
        _spawnHandler = std::move(handler);
    }
    void AgentManagerContent::SetActivateHandler(std::function<void(winrt::hstring)> handler)
    {
        _activateHandler = std::move(handler);
    }
    void AgentManagerContent::SetKillHandler(std::function<void(winrt::hstring)> handler)
    {
        _killHandler = std::move(handler);
    }
    void AgentManagerContent::SetPauseHandler(std::function<void(bool)> handler)
    {
        _pauseHandler = std::move(handler);
    }
    void AgentManagerContent::SetConfirmHandler(std::function<void(winrt::hstring, bool)> handler)
    {
        _confirmHandler = std::move(handler);
    }

    // ---- IPaneContent -------------------------------------------------------

    FrameworkElement AgentManagerContent::GetRoot()
    {
        return _root;
    }
    void AgentManagerContent::UpdateSettings(const CascadiaSettings& /*settings*/)
    {
    }
    Size AgentManagerContent::MinimumSize()
    {
        return { 1, 1 };
    }
    void AgentManagerContent::Focus(FocusState reason)
    {
        // Focus the cwd box (a real Control) so the tab takes focus cleanly.
        if (_cwdBox)
        {
            _cwdBox.Focus(reason);
        }
    }
    void AgentManagerContent::Close()
    {
    }
    INewContentArgs AgentManagerContent::GetNewTerminalArgs(const BuildStartupKind /*kind*/) const
    {
        return BaseContentArgs(L"agentManager");
    }
    winrt::hstring AgentManagerContent::Icon() const
    {
        static constexpr std::wstring_view glyph{ L"\xE71D" }; // AllApps
        return winrt::hstring{ glyph };
    }
    Brush AgentManagerContent::BackgroundBrush()
    {
        return _root.Background();
    }

    // ---- Layout (built once) ------------------------------------------------

    void AgentManagerContent::_BuildLayout()
    {
        auto starRow = [](double v) {
            RowDefinition rd;
            rd.Height(GridLengthHelper::FromValueAndType(v, GridUnitType::Star));
            return rd;
        };
        auto autoRow = []() {
            RowDefinition rd;
            rd.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            return rd;
        };
        auto starCol = [](double v) {
            ColumnDefinition cd;
            cd.Width(GridLengthHelper::FromValueAndType(v, GridUnitType::Star));
            return cd;
        };

        const auto panelBorder = Fill(0x40, 0x80, 0x80, 0x80);

        auto section = [&](const FrameworkElement& child) {
            Border b;
            b.BorderBrush(SolidColorBrush{ panelBorder });
            b.BorderThickness(Thickness{ 1, 1, 1, 1 });
            b.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
            b.Margin(Thickness{ 6, 6, 6, 6 });
            b.Padding(Thickness{ 8, 6, 8, 8 });
            b.Child(child);
            return b;
        };

        _root.RowDefinitions().Append(autoRow()); // toolbar
        _root.RowDefinitions().Append(starRow(2)); // board
        _root.RowDefinitions().Append(starRow(3)); // bottom

        // ---- Toolbar ----
        {
            auto bar = StackPanel{};
            bar.Orientation(Orientation::Horizontal);
            bar.Spacing(8);
            bar.Margin(Thickness{ 12, 8, 12, 0 });
            bar.VerticalAlignment(VerticalAlignment::Center);

            bar.Children().Append(Text(L"Agentmaster", 18, true, 1.0));
            bar.Children().Append(Text(L"\x2014  launch a Claude session in", 13, false, 0.6));

            _cwdBox = TextBox{};
            _cwdBox.Width(360);
            _cwdBox.PlaceholderText(L"working directory (the M axis)");
            {
                wchar_t up[MAX_PATH];
                const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
                if (n > 0 && n < MAX_PATH)
                {
                    _cwdBox.Text(winrt::hstring{ up, n });
                }
            }
            bar.Children().Append(_cwdBox);

            auto launch = Button{};
            launch.Content(winrt::box_value(L"Launch session"));
            launch.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnLaunch(); });
            bar.Children().Append(launch);

            // Global Autopilot backstop: Pause-all / Resume-all.
            _pauseBtn = Button{};
            _pauseBtn.Content(winrt::box_value(L"Pause Autopilot"));
            _pauseBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _globalPaused = !_globalPaused;
                if (_pauseHandler)
                {
                    _pauseHandler(_globalPaused);
                }
                if (_pauseBtn)
                {
                    _pauseBtn.Content(winrt::box_value(_globalPaused ? L"Resume Autopilot" : L"Pause Autopilot"));
                }
            });
            bar.Children().Append(_pauseBtn);

            Grid::SetRow(bar, 0);
            _root.Children().Append(bar);
        }

        // ---- Triage Board (row 1) ----
        {
            auto outer = Grid{};
            outer.RowDefinitions().Append(autoRow());
            outer.RowDefinitions().Append(starRow(1));

            auto header = StackPanel{};
            header.Orientation(Orientation::Horizontal);
            header.Spacing(8);
            header.Children().Append(Text(L"TRIAGE BOARD", 12, true, 0.8));
            _boardScope = Text(L"[all directories]", 12, false, 0.6);
            header.Children().Append(_boardScope);
            auto showAll = Button{};
            showAll.Content(winrt::box_value(L"Show all"));
            showAll.Padding(Thickness{ 6, 0, 6, 0 });
            showAll.Click([this](const IInspectable&, const RoutedEventArgs&) { _SetScope(L""); });
            header.Children().Append(showAll);
            Grid::SetRow(header, 0);
            outer.Children().Append(header);

            _boardHost = StackPanel{};
            _boardHost.Orientation(Orientation::Horizontal);
            _boardHost.Spacing(8);
            _boardHost.Margin(Thickness{ 0, 8, 0, 0 });

            auto sv = ScrollViewer{};
            sv.HorizontalScrollMode(ScrollMode::Enabled);
            sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
            sv.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
            sv.Content(_boardHost);
            Grid::SetRow(sv, 1);
            outer.Children().Append(sv);

            auto b = section(outer);
            Grid::SetRow(b, 1);
            _root.Children().Append(b);
        }

        // ---- Bottom: Explorer Tree | Flight Plan (row 2) ----
        {
            auto bottom = Grid{};
            bottom.ColumnDefinitions().Append(starCol(2));
            bottom.ColumnDefinitions().Append(starCol(3));

            // Explorer Tree
            {
                auto outer = Grid{};
                outer.RowDefinitions().Append(autoRow());
                outer.RowDefinitions().Append(starRow(1));
                auto hd = Text(L"EXPLORER TREE", 12, true, 0.8);
                Grid::SetRow(hd, 0);
                outer.Children().Append(hd);

                _treeHost = StackPanel{};
                _treeHost.Margin(Thickness{ 0, 8, 0, 0 });
                auto sv = ScrollViewer{};
                sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
                sv.Content(_treeHost);
                Grid::SetRow(sv, 1);
                outer.Children().Append(sv);

                auto b = section(outer);
                Grid::SetColumn(b, 0);
                bottom.Children().Append(b);
            }

            // Flight Plan
            {
                auto outer = Grid{};
                outer.RowDefinitions().Append(autoRow()); // header
                outer.RowDefinitions().Append(starRow(1)); // prompt list
                outer.RowDefinitions().Append(autoRow()); // action bar

                _planHeaderHost = StackPanel{};
                _planHeaderHost.Spacing(2);
                Grid::SetRow(_planHeaderHost, 0);
                outer.Children().Append(_planHeaderHost);

                _planListHost = StackPanel{};
                _planListHost.Margin(Thickness{ 0, 8, 0, 8 });
                auto sv = ScrollViewer{};
                sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
                sv.Content(_planListHost);
                Grid::SetRow(sv, 1);
                outer.Children().Append(sv);

                // Action bar (persistent).
                auto actions = StackPanel{};
                actions.Spacing(6);

                auto apRow = StackPanel{};
                apRow.Orientation(Orientation::Horizontal);
                apRow.Spacing(8);
                apRow.Children().Append(Text(L"Autopilot", 13, true, 0.9));
                _autopilotCombo = ComboBox{};
                _autopilotCombo.Items().Append(winrt::box_value(L"Off"));
                _autopilotCombo.Items().Append(winrt::box_value(L"Semi-auto"));
                _autopilotCombo.Items().Append(winrt::box_value(L"Full"));
                _autopilotCombo.SelectedIndex(0);
                _autopilotCombo.SelectionChanged([this](const IInspectable&, const SelectionChangedEventArgs&) {
                    _OnAutopilotChanged(_autopilotCombo ? _autopilotCombo.SelectedIndex() : 0);
                });
                apRow.Children().Append(_autopilotCombo);
                actions.Children().Append(apRow);

                _addPromptBox = TextBox{};
                _addPromptBox.PlaceholderText(L"queue a prompt for the selected session\x2026");
                _addPromptBox.AcceptsReturn(true);
                _addPromptBox.TextWrapping(TextWrapping::Wrap);
                _addPromptBox.MaxHeight(96);
                actions.Children().Append(_addPromptBox);

                auto btnRow = StackPanel{};
                btnRow.Orientation(Orientation::Horizontal);
                btnRow.Spacing(6);
                auto mkBtn = [&](const winrt::hstring& label, std::function<void()> fn) {
                    auto btn = Button{};
                    btn.Content(winrt::box_value(label));
                    btn.Click([fn](const IInspectable&, const RoutedEventArgs&) { fn(); });
                    return btn;
                };
                btnRow.Children().Append(mkBtn(L"Add", [this]() { _OnAddPrompt(); }));
                btnRow.Children().Append(mkBtn(L"Send now", [this]() { _OnSendNow(); }));
                btnRow.Children().Append(mkBtn(L"\x2191", [this]() { _OnMovePrompt(-1); }));
                btnRow.Children().Append(mkBtn(L"\x2193", [this]() { _OnMovePrompt(1); }));
                btnRow.Children().Append(mkBtn(L"Delete", [this]() { _OnDeletePrompt(); }));
                btnRow.Children().Append(mkBtn(L"Focus", [this]() {
                    if (_activateHandler && !_selectedId.empty())
                    {
                        _activateHandler(winrt::hstring{ _selectedId });
                    }
                }));
                btnRow.Children().Append(mkBtn(L"Kill", [this]() {
                    if (_killHandler && !_selectedId.empty())
                    {
                        _killHandler(winrt::hstring{ _selectedId });
                    }
                }));
                actions.Children().Append(btnRow);

                // Templates row (M8): save the current plan, apply a saved plan to this
                // session or broadcast it to every session in the directory.
                auto tplRow = StackPanel{};
                tplRow.Orientation(Orientation::Horizontal);
                tplRow.Spacing(6);
                _templateNameBox = TextBox{};
                _templateNameBox.Width(150);
                _templateNameBox.PlaceholderText(L"template name");
                tplRow.Children().Append(_templateNameBox);
                tplRow.Children().Append(mkBtn(L"Save as template", [this]() { _OnSaveTemplate(); }));
                _templateCombo = ComboBox{};
                _templateCombo.MinWidth(140);
                tplRow.Children().Append(_templateCombo);
                tplRow.Children().Append(mkBtn(L"Apply", [this]() { _OnApplyTemplate(false); }));
                tplRow.Children().Append(mkBtn(L"Apply to dir", [this]() { _OnApplyTemplate(true); }));
                actions.Children().Append(tplRow);
                _RefreshTemplateCombo();

                Grid::SetRow(actions, 2);
                outer.Children().Append(actions);

                auto headerLabel = StackPanel{};
                headerLabel.Children().Append(Text(L"FLIGHT PLAN", 12, true, 0.8));

                auto wrap = Grid{};
                wrap.RowDefinitions().Append(autoRow());
                wrap.RowDefinitions().Append(starRow(1));
                Grid::SetRow(headerLabel, 0);
                wrap.Children().Append(headerLabel);
                Grid::SetRow(outer, 1);
                wrap.Children().Append(outer);

                auto b = section(wrap);
                Grid::SetColumn(b, 1);
                bottom.Children().Append(b);
            }

            Grid::SetRow(bottom, 2);
            _root.Children().Append(bottom);
        }
    }

    // ---- Refresh / rebuild --------------------------------------------------

    void AgentManagerContent::_Refresh()
    {
        if (!_root || !_boardHost || !_treeHost || !_planListHost)
        {
            return;
        }
        std::vector<SessionInfo> sessions;
        if (_registry)
        {
            sessions = _registry->Snapshot();
        }
        _RebuildBoard(sessions);
        _RebuildTree(sessions);
        _RebuildPlan(sessions);
    }

    Button AgentManagerContent::_MakeCard(const SessionInfo& s)
    {
        const bool selected = (s.id == _selectedId);
        const auto accent = StateColor(s.state);

        auto stack = StackPanel{};
        stack.Spacing(2);

        stack.Children().Append(Text(s.title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ s.title }, 14, true, 1.0));
        stack.Children().Append(Text(winrt::hstring{ s.workingDir }, 11, false, 0.6));

        // autopilot badge ⚙ sent/total
        if (!s.queue.empty())
        {
            int sent = 0;
            for (const auto& p : s.queue)
            {
                if (p.status == PromptStatus::Sent)
                {
                    ++sent;
                }
            }
            const auto badge = winrt::hstring{ L"\x2699 " } + winrt::to_hstring(sent) + L"/" + winrt::to_hstring(static_cast<int>(s.queue.size()));
            auto bt = Text(badge, 11, false, 0.8);
            if (s.autopilot.mode != AutopilotMode::Off)
            {
                bt.Foreground(SolidColorBrush{ Colors::DodgerBlue() });
            }
            stack.Children().Append(bt);
        }

        auto card = Button{};
        card.Content(stack);
        card.HorizontalAlignment(HorizontalAlignment::Stretch);
        card.HorizontalContentAlignment(HorizontalAlignment::Left);
        card.Padding(Thickness{ 8, 6, 8, 6 });
        card.Margin(Thickness{ 0, 0, 0, 6 });
        card.Background(Fill(selected ? 0x40 : 0x20, 0x80, 0x80, 0x80));
        card.BorderBrush(SolidColorBrush{ accent });
        card.BorderThickness(selected ? Thickness{ 2, 2, 2, 2 } : Thickness{ 1, 1, 1, 1 });
        const auto id = s.id;
        card.Click([this, id](const IInspectable&, const RoutedEventArgs&) { _SelectSession(id); });
        return card;
    }

    void AgentManagerContent::_RebuildBoard(const std::vector<SessionInfo>& sessions)
    {
        _boardHost.Children().Clear();
        if (_boardScope)
        {
            _boardScope.Text(_scopeDir.empty() ? winrt::hstring{ L"[all directories]" } : (winrt::hstring{ L"[scope: " } + winrt::hstring{ _scopeDir } + L"]"));
        }

        struct Col
        {
            winrt::hstring title;
            SessionState state;
        };
        const Col cols[] = {
            { L"Running", SessionState::Running },
            { L"Waiting-for-you", SessionState::WaitingForInput },
            { L"Needs-approval", SessionState::NeedsApproval },
            { L"Error", SessionState::Error },
            { L"Idle / Done", SessionState::Idle },
        };

        for (const auto& col : cols)
        {
            auto colStack = StackPanel{};
            colStack.Spacing(0);

            // collect matching sessions (respecting the directory scope)
            std::vector<const SessionInfo*> matches;
            for (const auto& s : sessions)
            {
                if (!_scopeDir.empty() && s.workingDir != _scopeDir)
                {
                    continue;
                }
                const bool isIdleDone = (s.state == SessionState::Idle || s.state == SessionState::Done);
                const bool match = (col.state == SessionState::Idle) ? isIdleDone : (s.state == col.state);
                if (match)
                {
                    matches.push_back(&s);
                }
            }

            auto hdr = StackPanel{};
            hdr.Orientation(Orientation::Horizontal);
            hdr.Spacing(6);
            hdr.Margin(Thickness{ 0, 0, 0, 6 });
            auto dot = Text(L"\x25CF", 12, false, 1.0);
            dot.Foreground(SolidColorBrush{ StateColor(col.state) });
            hdr.Children().Append(dot);
            hdr.Children().Append(Text(col.title, 12, true, 0.9));
            hdr.Children().Append(Text(winrt::to_hstring(static_cast<int>(matches.size())), 12, false, 0.6));
            colStack.Children().Append(hdr);

            for (const auto* s : matches)
            {
                colStack.Children().Append(_MakeCard(*s));
            }

            auto col_border = Border{};
            col_border.Width(220);
            col_border.Padding(Thickness{ 8, 8, 8, 8 });
            col_border.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
            col_border.Background(Fill(0x14, 0x80, 0x80, 0x80));
            col_border.VerticalAlignment(VerticalAlignment::Top);
            col_border.Child(colStack);
            _boardHost.Children().Append(col_border);
        }
    }

    void AgentManagerContent::_RebuildTree(const std::vector<SessionInfo>& sessions)
    {
        _treeHost.Children().Clear();

        // ordered unique directories
        std::vector<std::wstring> dirs;
        for (const auto& s : sessions)
        {
            if (std::find(dirs.begin(), dirs.end(), s.workingDir) == dirs.end())
            {
                dirs.push_back(s.workingDir);
            }
        }

        if (dirs.empty())
        {
            _treeHost.Children().Append(Text(L"No sessions yet \x2014 use Launch session above.", 12, false, 0.6));
            return;
        }

        for (const auto& dir : dirs)
        {
            const bool collapsed = _collapsedDirs.find(dir) != _collapsedDirs.end();

            // dir header (toggles collapse + scopes the board)
            int count = 0;
            for (const auto& s : sessions)
            {
                if (s.workingDir == dir)
                {
                    ++count;
                }
            }

            auto dh = StackPanel{};
            dh.Orientation(Orientation::Horizontal);
            dh.Spacing(6);
            dh.Children().Append(Text(collapsed ? L"\x25B8" : L"\x25BE", 12, false, 0.8)); // ▸ / ▾
            dh.Children().Append(Text(winrt::hstring{ dir }, 13, true, 0.95));
            dh.Children().Append(Text(winrt::to_hstring(count), 12, false, 0.5));

            auto dirBtn = Button{};
            dirBtn.Content(dh);
            dirBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
            dirBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
            dirBtn.Background(Fill((dir == _scopeDir) ? 0x30 : 0x00, 0x80, 0x80, 0x80));
            dirBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            dirBtn.Padding(Thickness{ 4, 2, 4, 2 });
            const auto capturedDir = dir;
            dirBtn.Click([this, capturedDir](const IInspectable&, const RoutedEventArgs&) {
                if (_collapsedDirs.find(capturedDir) != _collapsedDirs.end())
                {
                    _collapsedDirs.erase(capturedDir);
                }
                else
                {
                    _collapsedDirs.insert(capturedDir);
                }
                _SetScope(capturedDir);
            });
            _treeHost.Children().Append(dirBtn);

            if (collapsed)
            {
                continue;
            }

            for (const auto& s : sessions)
            {
                if (s.workingDir != dir)
                {
                    continue;
                }
                const bool selected = (s.id == _selectedId);

                auto row = StackPanel{};
                row.Orientation(Orientation::Horizontal);
                row.Spacing(6);
                auto g = Text(StateGlyph(s.state), 12, false, 1.0);
                g.Foreground(SolidColorBrush{ StateColor(s.state) });
                row.Children().Append(g);
                row.Children().Append(Text(s.title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ s.title }, 13, false, 1.0));
                row.Children().Append(Text(StateLabel(s.state), 11, false, 0.5));

                auto rowBtn = Button{};
                rowBtn.Content(row);
                rowBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
                rowBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
                rowBtn.Margin(Thickness{ 16, 0, 0, 0 });
                rowBtn.Padding(Thickness{ 4, 2, 4, 2 });
                rowBtn.Background(Fill(selected ? 0x40 : 0x00, 0x80, 0x80, 0x80));
                rowBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });

                const auto id = s.id;
                rowBtn.Click([this, id](const IInspectable&, const RoutedEventArgs&) { _SelectSession(id); });
                // Enter = Activate (jump to live tab) — NEVER inject (Correctness Rule #2).
                // Delete = kill (the page runs the close-confirm flow).
                rowBtn.KeyDown([this, id](const IInspectable&, const KeyRoutedEventArgs& e) {
                    const auto key = e.Key();
                    if (key == VirtualKey::Enter)
                    {
                        if (_activateHandler)
                        {
                            _activateHandler(winrt::hstring{ id });
                        }
                        e.Handled(true);
                    }
                    else if (key == VirtualKey::Delete)
                    {
                        if (_killHandler)
                        {
                            _killHandler(winrt::hstring{ id });
                        }
                        e.Handled(true);
                    }
                });
                _treeHost.Children().Append(rowBtn);
            }
        }
    }

    void AgentManagerContent::_RebuildPlan(const std::vector<SessionInfo>& sessions)
    {
        _planHeaderHost.Children().Clear();
        _planListHost.Children().Clear();

        const auto sel = _Selected(sessions);
        if (!sel)
        {
            _planHeaderHost.Children().Append(Text(L"Select a session to plan its prompts.", 13, false, 0.6));
            _suppressAutopilotEvent = true;
            if (_autopilotCombo)
            {
                _autopilotCombo.SelectedIndex(0);
            }
            _suppressAutopilotEvent = false;
            return;
        }

        // header
        auto titleRow = StackPanel{};
        titleRow.Orientation(Orientation::Horizontal);
        titleRow.Spacing(8);
        titleRow.Children().Append(Text(sel->title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ sel->title }, 16, true, 1.0));
        titleRow.Children().Append(Pill(StateLabel(sel->state), StateColor(sel->state)));
        _planHeaderHost.Children().Append(titleRow);
        _planHeaderHost.Children().Append(Text(winrt::hstring{ sel->workingDir }, 12, false, 0.6));

        // reflect autopilot mode without re-triggering the change handler
        _suppressAutopilotEvent = true;
        if (_autopilotCombo)
        {
            _autopilotCombo.SelectedIndex(sel->autopilot.mode == AutopilotMode::Full ? 2 : sel->autopilot.mode == AutopilotMode::SemiAuto ? 1 :
                                                                                                                                            0);
        }
        _suppressAutopilotEvent = false;

        // SemiAuto one-click confirm banner (the scheduler armed the next prompt).
        if (!sel->pendingConfirmPromptId.empty())
        {
            winrt::hstring label;
            for (const auto& p : sel->queue)
            {
                if (p.id == sel->pendingConfirmPromptId)
                {
                    label = p.label.empty() ? winrt::hstring{ p.text } : winrt::hstring{ p.label };
                    break;
                }
            }
            auto banner = StackPanel{};
            banner.Orientation(Orientation::Horizontal);
            banner.Spacing(8);
            banner.VerticalAlignment(VerticalAlignment::Center);
            banner.Children().Append(Text(L"\x2699 Autopilot ready:", 12, true, 1.0));
            auto lbl = Text(label, 12, false, 0.9);
            lbl.MaxWidth(220);
            banner.Children().Append(lbl);
            auto sendBtn = Button{};
            sendBtn.Content(winrt::box_value(L"Send"));
            sendBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                if (_confirmHandler && !_selectedId.empty())
                {
                    _confirmHandler(winrt::hstring{ _selectedId }, true);
                }
            });
            auto skipBtn = Button{};
            skipBtn.Content(winrt::box_value(L"Skip"));
            skipBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                if (_confirmHandler && !_selectedId.empty())
                {
                    _confirmHandler(winrt::hstring{ _selectedId }, false);
                }
            });
            banner.Children().Append(sendBtn);
            banner.Children().Append(skipBtn);

            auto bannerBorder = Border{};
            bannerBorder.Background(Fill(0x40, 0xDA, 0xA5, 0x20));
            bannerBorder.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            bannerBorder.Padding(Thickness{ 8, 4, 8, 4 });
            bannerBorder.Margin(Thickness{ 0, 6, 0, 0 });
            bannerBorder.Child(banner);
            _planHeaderHost.Children().Append(bannerBorder);
        }

        if (sel->queue.empty())
        {
            _planListHost.Children().Append(Text(L"No queued prompts. Type below and Add.", 12, false, 0.6));
            return;
        }

        for (const auto& p : sel->queue)
        {
            const bool selected = (p.id == _selectedPromptId);

            auto row = StackPanel{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(8);
            auto g = Text(PromptGlyph(p.status), 13, false, 0.9);
            row.Children().Append(g);
            auto lbl = Text(p.label.empty() ? winrt::hstring{ p.text } : winrt::hstring{ p.label }, 13, false, 1.0);
            lbl.MaxWidth(360);
            row.Children().Append(lbl);
            row.Children().Append(Text(GateBadge(p.gate), 11, false, 0.5));
            if (p.status == PromptStatus::Held)
            {
                row.Children().Append(Text(L"(held: agent asked a question)", 11, false, 0.6));
            }

            auto rowBtn = Button{};
            rowBtn.Content(row);
            rowBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
            rowBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
            rowBtn.Padding(Thickness{ 6, 3, 6, 3 });
            rowBtn.Margin(Thickness{ 0, 0, 0, 4 });
            rowBtn.Background(Fill(selected ? 0x40 : 0x14, 0x80, 0x80, 0x80));
            rowBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            const auto pid = p.id;
            rowBtn.Click([this, pid](const IInspectable&, const RoutedEventArgs&) {
                _selectedPromptId = pid;
                _Refresh();
            });
            _planListHost.Children().Append(rowBtn);
        }
    }

    // ---- selection / scope --------------------------------------------------

    std::optional<SessionInfo> AgentManagerContent::_Selected(const std::vector<SessionInfo>& sessions) const
    {
        for (const auto& s : sessions)
        {
            if (s.id == _selectedId)
            {
                return s;
            }
        }
        return std::nullopt;
    }

    void AgentManagerContent::_SelectSession(const std::wstring& id)
    {
        if (_selectedId == id)
        {
            return;
        }
        _selectedId = id;
        _selectedPromptId.clear();
        _Refresh();
    }

    void AgentManagerContent::_SetScope(const std::wstring& dir)
    {
        _scopeDir = dir;
        _Refresh();
    }

    // ---- action handlers ----------------------------------------------------

    void AgentManagerContent::_OnLaunch()
    {
        if (_spawnHandler)
        {
            const auto dir = _cwdBox ? _cwdBox.Text() : winrt::hstring{};
            _spawnHandler(dir, winrt::hstring{});
        }
    }

    void AgentManagerContent::_OnAddPrompt()
    {
        if (_selectedId.empty() || !_registry || !_addPromptBox)
        {
            return;
        }
        std::wstring text{ _addPromptBox.Text() };
        if (text.empty())
        {
            return;
        }
        std::wstring label = text.substr(0, 56);
        std::replace(label.begin(), label.end(), L'\n', L' ');
        std::replace(label.begin(), label.end(), L'\r', L' ');

        const auto id = _selectedId;
        _registry->Update(id, [&](SessionInfo& s) {
            QueuedPrompt p;
            p.id = NewSessionId();
            p.label = label;
            p.text = text;
            s.queue.push_back(std::move(p));
        });
        _addPromptBox.Text(L"");
        _Refresh();
    }

    void AgentManagerContent::_OnSendNow()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }

        // "Send now" sends what's in the compose box (and records it in the Flight Plan as a
        // Sent item) so typing + Send is one intuitive action. With an empty box it instead
        // sends the selected (or first Pending) already-queued prompt.
        std::wstring composed = _addPromptBox ? std::wstring{ _addPromptBox.Text() } : std::wstring{};

        std::wstring textToSend;
        if (!composed.empty())
        {
            std::wstring label = composed.substr(0, 56);
            std::replace(label.begin(), label.end(), L'\n', L' ');
            std::replace(label.begin(), label.end(), L'\r', L' ');
            _registry->Update(_selectedId, [&](SessionInfo& s) {
                QueuedPrompt p;
                p.id = ::Agentmaster::NewSessionId();
                p.label = label;
                p.text = composed;
                p.status = PromptStatus::Sent; // it's being sent right now
                p.attempts = 1;
                s.queue.push_back(std::move(p));
            });
            textToSend = composed;
            if (_addPromptBox)
            {
                _addPromptBox.Text(L"");
            }
        }
        else
        {
            const auto promptId = _selectedPromptId;
            _registry->Update(_selectedId, [&](SessionInfo& s) {
                QueuedPrompt* target = nullptr;
                if (!promptId.empty())
                {
                    for (auto& p : s.queue)
                    {
                        if (p.id == promptId)
                        {
                            target = &p;
                            break;
                        }
                    }
                }
                if (!target)
                {
                    for (auto& p : s.queue)
                    {
                        if (p.status == PromptStatus::Pending)
                        {
                            target = &p;
                            break;
                        }
                    }
                }
                if (target)
                {
                    textToSend = target->text;
                    target->status = PromptStatus::Sent;
                    target->attempts += 1;
                }
            });
        }

        if (!textToSend.empty())
        {
            // Inject + submit. (Multiline bodies submit on the first CR for now; bracketed
            // paste for true multi-line prompts is a follow-up.)
            _registry->Inject(_selectedId, textToSend + L"\r");
        }
        _Refresh();
    }

    void AgentManagerContent::_OnMovePrompt(int delta)
    {
        if (_selectedId.empty() || _selectedPromptId.empty() || !_registry)
        {
            return;
        }
        const auto pid = _selectedPromptId;
        _registry->Update(_selectedId, [&](SessionInfo& s) {
            auto& q = s.queue;
            for (size_t i = 0; i < q.size(); ++i)
            {
                if (q[i].id == pid)
                {
                    const long j = static_cast<long>(i) + delta;
                    if (j >= 0 && j < static_cast<long>(q.size()))
                    {
                        std::swap(q[i], q[static_cast<size_t>(j)]);
                    }
                    break;
                }
            }
        });
        _Refresh();
    }

    void AgentManagerContent::_OnDeletePrompt()
    {
        if (_selectedId.empty() || _selectedPromptId.empty() || !_registry)
        {
            return;
        }
        const auto pid = _selectedPromptId;
        _registry->Update(_selectedId, [&](SessionInfo& s) {
            auto& q = s.queue;
            q.erase(std::remove_if(q.begin(), q.end(), [&](const QueuedPrompt& p) { return p.id == pid; }), q.end());
        });
        _selectedPromptId.clear();
        _Refresh();
    }

    void AgentManagerContent::_OnAutopilotChanged(int index)
    {
        if (_suppressAutopilotEvent || _selectedId.empty() || !_registry)
        {
            return;
        }
        const AutopilotMode mode = index == 2 ? AutopilotMode::Full : index == 1 ? AutopilotMode::SemiAuto :
                                                                                   AutopilotMode::Off;
        _registry->Update(_selectedId, [&](SessionInfo& s) {
            s.autopilot.mode = mode;
            if (mode != AutopilotMode::Off)
            {
                // Arming resets the per-run backstop counter and clears any stale confirm.
                s.autopilot.autoSendsThisRun = 0;
                s.pendingConfirmPromptId.clear();
            }
        });
    }

    void AgentManagerContent::_RefreshTemplateCombo()
    {
        if (!_templateCombo)
        {
            return;
        }
        _templateCombo.Items().Clear();
        for (const auto& t : _templates)
        {
            _templateCombo.Items().Append(winrt::box_value(winrt::hstring{ t.name }));
        }
        if (!_templates.empty())
        {
            _templateCombo.SelectedIndex(0);
        }
    }

    void AgentManagerContent::_OnSaveTemplate()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }
        const auto sel = _registry->Get(_selectedId);
        if (!sel || sel->queue.empty())
        {
            return;
        }
        std::wstring name = _templateNameBox ? std::wstring{ _templateNameBox.Text() } : std::wstring{};
        if (name.empty())
        {
            name = (sel->title.empty() ? std::wstring{ L"plan" } : sel->title) + L"-plan";
        }
        _templates.push_back(::Agentmaster::MakeTemplateFromQueue(name, sel->queue));
        ::Agentmaster::SaveTemplates(_templates);
        if (_templateNameBox)
        {
            _templateNameBox.Text(L"");
        }
        _RefreshTemplateCombo();
        if (_templateCombo && !_templates.empty())
        {
            _templateCombo.SelectedIndex(static_cast<int32_t>(_templates.size()) - 1);
        }
    }

    void AgentManagerContent::_OnApplyTemplate(bool toWholeDirectory)
    {
        if (!_registry || !_templateCombo)
        {
            return;
        }
        const auto idx = _templateCombo.SelectedIndex();
        if (idx < 0 || static_cast<size_t>(idx) >= _templates.size())
        {
            return;
        }
        const auto tmpl = _templates[static_cast<size_t>(idx)];

        if (!toWholeDirectory)
        {
            if (_selectedId.empty())
            {
                return;
            }
            _registry->Update(_selectedId, [&](SessionInfo& s) { ::Agentmaster::AppendTemplateToQueue(s.queue, tmpl); });
            return;
        }

        // Apply-to-many: every session in the scoped (or selected) working directory.
        std::wstring dir = _scopeDir;
        if (dir.empty())
        {
            if (const auto sel = _Selected(_registry->Snapshot()))
            {
                dir = sel->workingDir;
            }
        }
        if (dir.empty())
        {
            return;
        }
        for (const auto& s : _registry->Snapshot())
        {
            if (s.workingDir == dir)
            {
                _registry->Update(s.id, [&](SessionInfo& ss) { ::Agentmaster::AppendTemplateToQueue(ss.queue, tmpl); });
            }
        }
    }
}
