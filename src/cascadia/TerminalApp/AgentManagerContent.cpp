// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/SessionRegistry.h"

#include <algorithm>
#include <chrono>

using namespace winrt::Windows::Foundation;
// Using-DECLARATIONS (not a directive) for the color helpers: a `using namespace
// winrt::Windows::UI;` would also pull the nested `Text` namespace into scope and collide
// with our Text() TextBlock helper below.
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Colors;
using winrt::Windows::UI::Core::CoreCursor;
using winrt::Windows::UI::Core::CoreCursorType;
using winrt::Windows::UI::Core::CoreWindow;
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

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Set the window pointer cursor (used by the resize splitters: a ↔/↕ on hover, Arrow on
    // exit). There is no per-element cursor in this XAML projection (ProtectedCursor is only
    // reachable from a subclass), so we drive the CoreWindow cursor like the rest of the app.
    void ApplyCursor(CoreCursorType type)
    {
        if (const auto w = CoreWindow::GetForCurrentThread())
        {
            w.PointerCursor(CoreCursor{ type, 0 });
        }
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

    // ---- path helpers for the Launch path-picker drop-down -------------------
    // Plain Win32 + STL (the same toolbox the rest of the engine uses). FindFirstFileW /
    // GetFileAttributesW / CompareStringOrdinal are already in scope via pch (this TU
    // already calls ::GetEnvironmentVariableW with MAX_PATH).

    // Normalize a path for comparison. Separator + trailing-slash rules are filesystem-
    // dependent, so this splits by platform.
    std::wstring NormPath(std::wstring s)
    {
#ifdef _WIN32
        // Windows: '/' and '\' are interchangeable separators; a trailing separator is
        // insignificant (except on the drive root "C:\").
        for (auto& ch : s)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
        }
        while (s.size() > 3 && s.back() == L'\\')
        {
            s.pop_back();
        }
#else
        // POSIX: '\' is an ordinary filename character; only '/' separates, and a trailing
        // one is insignificant (except on the root "/").
        while (s.size() > 1 && s.back() == L'/')
        {
            s.pop_back();
        }
#endif
        return s;
    }

    // Whether two paths denote the same directory. Case-INsensitive on Windows (NTFS/ReFS
    // default), case-SENSITIVE on Linux/POSIX. Windows Terminal builds Windows-only today;
    // the POSIX branch keeps the semantics correct should the engine ever be ported.
    bool PathEq(const std::wstring& a, const std::wstring& b)
    {
        const auto na = NormPath(a);
        const auto nb = NormPath(b);
#ifdef _WIN32
        return ::CompareStringOrdinal(na.c_str(), -1, nb.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
        return na == nb;
#endif
    }

    bool IsDir(const std::wstring& d)
    {
        if (d.empty())
        {
            return false;
        }
        const DWORD a = ::GetFileAttributesW(d.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::wstring JoinDir(const std::wstring& base, const std::wstring& leaf)
    {
        std::wstring b = base;
        while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/'))
        {
            b.pop_back();
        }
        if (b.empty())
        {
            return leaf;
        }
        if (b.back() != L'\\' && b.back() != L'/')
        {
            b += L'\\';
        }
        return b + leaf;
    }

    // The directory one level up, if there is a sensible one (drive-letter paths). A drive
    // root ("C:\") returns nullopt; so does a bare/relative token.
    std::optional<std::wstring> ParentDir(const std::wstring& dir)
    {
        std::wstring b = dir;
        while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/'))
        {
            b.pop_back();
        }
        if (b.size() <= 3)
        {
            return std::nullopt; // "C:\" or shorter: no parent to navigate to
        }
        const auto pos = b.find_last_of(L"\\/");
        if (pos == std::wstring::npos || pos == 0)
        {
            return std::nullopt;
        }
        if (pos <= 2 && b.size() >= 2 && b[1] == L':')
        {
            return b.substr(0, 2) + L"\\"; // parent is the drive root
        }
        return b.substr(0, pos);
    }

    // The immediate subdirectories of `dir` (leaf names only), most-recently-MODIFIED first
    // (the folder's last-write FILETIME, newest at the top — the Explorer "Date modified" sort
    // order, which surfaces the directories you've touched lately). Each leaf is paired with
    // its 64-bit FILETIME so the sort is a cheap integer compare; equal timestamps fall back to
    // a case-insensitive name sort so the order stays stable. Skips ".", "..", and SYSTEM dirs
    // (e.g. $Recycle.Bin, System Volume Information).
    std::vector<std::wstring> EnumSubdirs(const std::wstring& dir)
    {
        std::vector<std::pair<std::wstring, unsigned long long>> items;
        if (dir.empty())
        {
            return {};
        }
        std::wstring pattern = dir;
        while (pattern.size() > 3 && (pattern.back() == L'\\' || pattern.back() == L'/'))
        {
            pattern.pop_back();
        }
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
        {
            pattern += L'\\';
        }
        pattern += L'*';

        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
            {
                continue;
            }
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const unsigned long long mtime =
                (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                static_cast<unsigned long long>(fd.ftLastWriteTime.dwLowDateTime);
            items.emplace_back(name, mtime);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);

        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second)
            {
                return a.second > b.second; // newest (largest FILETIME) first
            }
            return ::CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });

        std::vector<std::wstring> out;
        out.reserve(items.size());
        for (auto& it : items)
        {
            out.push_back(std::move(it.first));
        }
        return out;
    }
}

namespace winrt::TerminalApp::implementation
{
    AgentManagerContent::AgentManagerContent()
    {
        _root = Grid{};
        _dispatcher = DispatcherQueue::GetForCurrentThread();
        _templates = ::Agentmaster::LoadTemplates(); // persisted plan templates (M8)
        _recentDirs = ::Agentmaster::LoadRecentDirs(); // MRU for the Launch path-picker
        _layout = ::Agentmaster::LoadLayout(); // persisted splitter geometry (pane sizes)

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

    // Agentmaster (M9): the registry is a process singleton shared by every window. Detach this
    // lens's observer on teardown so the shared registry stops invoking a dead window's refresh
    // (and stops pinning its DispatcherQueue alive). The observer captures get_weak(), so a stray
    // late call is already a no-op; this also keeps the observer list bounded across many window
    // open/close cycles. _registry (held by SharedEngine) outlives us, so the call stays valid.
    AgentManagerContent::~AgentManagerContent()
    {
        if (_registry && _observerToken)
        {
            _registry->RemoveObserver(_observerToken);
        }
    }

    void AgentManagerContent::SetRegistry(std::shared_ptr<::Agentmaster::SessionRegistry> registry)
    {
        // Detach any previous observer (defensive — SetRegistry is normally called exactly once).
        if (_registry && _observerToken)
        {
            _registry->RemoveObserver(_observerToken);
            _observerToken = 0;
        }
        _registry = std::move(registry);
        if (_registry)
        {
            auto weak = get_weak();
            auto disp = _dispatcher;
            _observerToken = _registry->AddObserver([weak, disp](const SessionInfo&, HookEvent) {
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
    void AgentManagerContent::SetArchiveHandler(std::function<void(winrt::hstring)> handler)
    {
        _archiveHandler = std::move(handler);
    }
    void AgentManagerContent::SetRestoreHandler(std::function<void(winrt::hstring)> handler)
    {
        _restoreHandler = std::move(handler);
    }
    void AgentManagerContent::SetRenameHandler(std::function<void(winrt::hstring, winrt::hstring)> handler)
    {
        _renameHandler = std::move(handler);
    }
    void AgentManagerContent::SetLocalScopeProvider(std::function<std::unordered_set<std::wstring>()> provider)
    {
        _localScopeProvider = std::move(provider);
    }
    void AgentManagerContent::SetPauseHandler(std::function<void(bool)> handler)
    {
        _pauseHandler = std::move(handler);
    }
    void AgentManagerContent::SetConfirmHandler(std::function<void(winrt::hstring, bool)> handler)
    {
        _confirmHandler = std::move(handler);
    }
    void AgentManagerContent::SetSettings(const ::Agentmaster::AppSettings& settings)
    {
        _appSettings = settings;
        // Seed the Launch cwd box with the configured default (wiring runs after _BuildLayout,
        // which had defaulted the box to %USERPROFILE%). Only override when a default is set.
        if (_cwdBox && !settings.defaultLaunchDir.empty())
        {
            _cwdBox.Text(winrt::hstring{ settings.defaultLaunchDir });
        }
    }
    void AgentManagerContent::SetSettingsHandler(std::function<void(::Agentmaster::AppSettings)> handler)
    {
        _settingsSink = std::move(handler);
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
        auto autoCol = []() {
            ColumnDefinition cd;
            cd.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
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

        // Rows: toolbar (auto) · board (★) · splitter (auto) · bottom (★). The two ★ rows are
        // seeded from the persisted fraction and are what the horizontal splitter resizes.
        _boardRow = starRow(_layout.boardFraction);
        _bottomRow = starRow(1.0 - _layout.boardFraction);
        _root.RowDefinitions().Append(autoRow()); // 0: toolbar
        _root.RowDefinitions().Append(_boardRow); // 1: board
        _root.RowDefinitions().Append(autoRow()); // 2: splitter
        _root.RowDefinitions().Append(_bottomRow); // 3: bottom

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
            // Path-picker drop-down. Open it only on *user* focus (Pointer/Keyboard) so the
            // dropdown doesn't pop every time the tab is programmatically activated.
            _cwdBox.GotFocus([this](const IInspectable&, const RoutedEventArgs&) {
                if (!_cwdBox)
                {
                    return;
                }
                // (Re)focusing the box clears any prior Esc/Enter/blur dismissal, so the next
                // keystroke brings the list back; when the focus itself came from the user
                // (pointer/keyboard) open it right away.
                _pathPickerUserDismissed = false;
                const auto fs = _cwdBox.FocusState();
                if (fs == FocusState::Pointer || fs == FocusState::Keyboard)
                {
                    _OpenPathPicker();
                }
            });
            // Clicking back into an ALREADY-focused box fires no GotFocus, so a tap also re-shows
            // a previously dismissed picker (open only if needed; a tap is not a drag, so it
            // won't fight text selection).
            _cwdBox.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
                if (!_cwdBox)
                {
                    return;
                }
                _pathPickerUserDismissed = false;
                if (!_pathPopup || !_pathPopup.IsOpen())
                {
                    _OpenPathPicker();
                }
            });
            _cwdBox.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) {
                if (!_cwdBox)
                {
                    return;
                }
                // Typing should ALWAYS surface the list. The prior version only refreshed an
                // already-open popup, so whenever the box held focus while the popup was closed
                // (focus arrived programmatically, a stray LostFocus closed it, etc.) typing
                // showed nothing — exactly the reported "list doesn't show" symptom. Now we
                // reopen when closed and refresh when open. The lone exception is an explicit
                // Esc/Enter dismissal, which sticks until the box is refocused or tapped.
                if (_cwdBox.FocusState() == FocusState::Unfocused || _pathPickerUserDismissed)
                {
                    return;
                }
                if (_pathPopup && _pathPopup.IsOpen())
                {
                    _RebuildPathPicker();
                }
                else
                {
                    _OpenPathPicker();
                }
            });
            // Close only when focus truly left (not when a row button briefly takes it):
            // defer the check a tick, and bail if the box has refocused itself (after a pick).
            {
                auto weak = get_weak();
                auto disp = _dispatcher;
                _cwdBox.LostFocus([weak, disp](const IInspectable&, const RoutedEventArgs&) {
                    if (!disp)
                    {
                        return;
                    }
                    disp.TryEnqueue([weak]() {
                        auto self = weak.get();
                        if (!self || !self->_cwdBox)
                        {
                            return;
                        }
                        if (self->_cwdBox.FocusState() != FocusState::Unfocused)
                        {
                            return; // regained focus (e.g. after clicking a row) — leave it alone
                        }
                        // Focus truly left the box: close the picker (if open) and normalize
                        // what's there. Mark it dismissed so the normalize's TextChanged won't
                        // reopen the (now-closed) picker; refocusing/tapping the box clears it.
                        self->_pathPickerUserDismissed = true;
                        self->_ClosePathPicker();
                        self->_NormalizeCwdBox();
                    });
                });
            }
            _cwdBox.KeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                if (e.Key() == VirtualKey::Escape)
                {
                    if (_pathPopup && _pathPopup.IsOpen())
                    {
                        _pathPickerUserDismissed = true; // stays dismissed until refocus/tap
                        _ClosePathPicker();
                        e.Handled(true);
                    }
                }
                else if (e.Key() == VirtualKey::Enter)
                {
                    _pathPickerUserDismissed = true; // committing dismisses; keep normalize from reopening it
                    _ClosePathPicker();
                    _NormalizeCwdBox(); // commit: normalize what the user typed
                }
            });
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

            // Archived sessions: opens the in-content archive overlay (built at the end of
            // layout). Closing a session tab archives it (shut down, kept restorable) rather than
            // discarding it; this is where you bring those back. Label carries a live count.
            _archivedBtn = Button{};
            _archivedBtn.Content(winrt::box_value(L"Archived"));
            ToolTipService::SetToolTip(_archivedBtn, winrt::box_value(L"Restore archived (closed) sessions"));
            _archivedBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ShowArchive(); });
            bar.Children().Append(_archivedBtn);

            // Settings cog (opens the in-content settings overlay; built at the end of layout).
            _settingsBtn = Button{};
            {
                FontIcon cog;
                cog.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
                cog.Glyph(L"\xE713"); // Settings (cog)
                _settingsBtn.Content(cog);
            }
            ToolTipService::SetToolTip(_settingsBtn, winrt::box_value(L"Settings"));
            _settingsBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ShowSettings(); });
            bar.Children().Append(_settingsBtn);

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

        // ---- Bottom: Explorer Tree | Flight Plan (row 3) ----
        {
            auto bottom = Grid{};
            // Columns: tree (★) · splitter (auto) · plan (★). The two ★ cols are seeded from
            // the persisted fraction and are what the vertical splitter resizes.
            _treeCol = starCol(_layout.treeFraction);
            _planCol = starCol(1.0 - _layout.treeFraction);
            bottom.ColumnDefinitions().Append(_treeCol); // 0: tree
            bottom.ColumnDefinitions().Append(autoCol()); // 1: splitter
            bottom.ColumnDefinitions().Append(_planCol); // 2: plan

            // Explorer Tree
            {
                auto outer = Grid{};
                outer.RowDefinitions().Append(autoRow());
                outer.RowDefinitions().Append(starRow(1));
                // Header row: the title + a LOCAL/GLOBAL scope toggle (Agentmaster). LOCAL shows
                // only this window's sessions (the page's _claudeTabs, via _localScopeProvider);
                // GLOBAL shows every window's sessions (the whole process-wide registry). The
                // button's label is the current mode; clicking flips it and rebuilds the tree.
                auto hdrow = StackPanel{};
                hdrow.Orientation(Orientation::Horizontal);
                hdrow.Spacing(8);
                hdrow.VerticalAlignment(VerticalAlignment::Center);
                hdrow.Children().Append(Text(L"EXPLORER TREE", 12, true, 0.8));
                _treeScopeBtn = Button{};
                _treeScopeBtn.FontSize(11);
                _treeScopeBtn.Padding(Thickness{ 8, 1, 8, 1 });
                ToolTipService::SetToolTip(_treeScopeBtn, winrt::box_value(L"Scope \x2014 LOCAL: this window's sessions; GLOBAL: all windows"));
                _treeScopeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ToggleTreeScope(); });
                hdrow.Children().Append(_treeScopeBtn);
                _UpdateTreeScopeButton();
                Grid::SetRow(hdrow, 0);
                outer.Children().Append(hdrow);

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
                btnRow.Children().Append(mkBtn(L"Archive", [this]() {
                    if (!_selectedId.empty())
                    {
                        _RequestArchive(_selectedId);
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
                Grid::SetColumn(b, 2);
                bottom.Children().Append(b);
            }

            // Vertical splitter between Tree and Flight Plan (drag = resize ↔).
            {
                auto vbar = _MakeSplitter(true);
                Grid::SetColumn(vbar, 1);
                bottom.Children().Append(vbar);
            }

            Grid::SetRow(bottom, 3);
            _root.Children().Append(bottom);
        }

        // ---- Horizontal splitter between Triage Board and the bottom (drag = resize ↕) ----
        {
            auto hbar = _MakeSplitter(false);
            Grid::SetRow(hbar, 2);
            _root.Children().Append(hbar);
        }

        // ---- Launch path-picker drop-down (a Popup anchored under the cwd box) ----
        // Parented into _root (top-left aligned) so its offset is _root-relative; it renders
        // in the overlay above the board. A fixed dark theme keeps the list readable over
        // whatever the app theme is.
        {
            _pathListHost = StackPanel{};
            _pathListHost.Spacing(0);

            auto sv = ScrollViewer{};
            sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            sv.MaxHeight(380);
            sv.AllowFocusOnInteraction(false); // scrollbar drags mustn't steal focus from the box either
            sv.IsTabStop(false); // ...and the scroll viewer must never become the focused element itself
            sv.Content(_pathListHost);

            _pathPanelBorder = Border{};
            _pathPanelBorder.Background(Fill(0xFF, 0x20, 0x20, 0x20));
            _pathPanelBorder.BorderBrush(Fill(0x90, 0x80, 0x80, 0x80));
            _pathPanelBorder.BorderThickness(Thickness{ 1, 1, 1, 1 });
            _pathPanelBorder.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
            _pathPanelBorder.Padding(Thickness{ 4, 4, 4, 6 });
            _pathPanelBorder.Width(380);
            _pathPanelBorder.RequestedTheme(ElementTheme::Dark);
            _pathPanelBorder.Child(sv);

            _pathPopup = winrt::Windows::UI::Xaml::Controls::Primitives::Popup{};
            _pathPopup.HorizontalAlignment(HorizontalAlignment::Left);
            _pathPopup.VerticalAlignment(VerticalAlignment::Top);
            _pathPopup.Child(_pathPanelBorder);
            Grid::SetRow(_pathPopup, 0);
            _root.Children().Append(_pathPopup);
        }

        _BuildArchiveOverlay(); // modal archived-sessions layer
        _BuildSettingsOverlay(); // modal settings layer, appended last so it renders on top
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
        _UpdateArchivedButton(sessions);
        if (_archiveOverlay && _archiveOverlay.Visibility() == Visibility::Visible)
        {
            _RebuildArchiveList(); // keep the open archive list current as sessions archive/restore
        }
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
        // Single click = select; double click (within the OS threshold) = Activate (jump to
        // the session's live terminal tab), mirroring the Explorer Tree rows. A Button
        // swallows DoubleTapped, so we time the successive clicks ourselves.
        card.Click([this, id](const IInspectable&, const RoutedEventArgs&) {
            const auto nowTick = ::GetTickCount64();
            const bool dbl = (id == _lastCardClickId) && (nowTick - _lastCardClickTick) <= ::GetDoubleClickTime();
            _lastCardClickId = id;
            _lastCardClickTick = nowTick;
            if (dbl && _activateHandler)
            {
                _activateHandler(winrt::hstring{ id });
            }
            else
            {
                _SelectSession(id);
            }
        });
        return card;
    }

    // ---- Resizable splitters ------------------------------------------------

    Border AgentManagerContent::_MakeSplitter(bool vertical)
    {
        // A thin grab-bar in its own auto-sized grid track. The near-transparent fill keeps
        // the whole bar hit-testable; a centered grip line + hover highlight signal it is
        // draggable, and the OS cursor flips to the resize arrow while the pointer is over it.
        const auto idleGrip = Fill(0x40, 0x80, 0x80, 0x80);
        const auto hotGrip = Fill(0x90, 0xC0, 0xC0, 0xC0);

        auto grip = Border{};
        grip.Background(idleGrip);
        grip.CornerRadius(CornerRadius{ 1, 1, 1, 1 });

        auto bar = Border{};
        if (vertical)
        {
            bar.Width(10);
            bar.VerticalAlignment(VerticalAlignment::Stretch);
            grip.Width(2);
            grip.HorizontalAlignment(HorizontalAlignment::Center);
            grip.VerticalAlignment(VerticalAlignment::Stretch);
            grip.Margin(Thickness{ 0, 10, 0, 10 });
        }
        else
        {
            bar.Height(10);
            bar.HorizontalAlignment(HorizontalAlignment::Stretch);
            grip.Height(2);
            grip.VerticalAlignment(VerticalAlignment::Center);
            grip.HorizontalAlignment(HorizontalAlignment::Stretch);
            grip.Margin(Thickness{ 10, 0, 10, 0 });
        }
        bar.Background(Fill(0x01, 0x80, 0x80, 0x80)); // ~invisible, yet hit-testable
        bar.Child(grip);

        const auto cursorType = vertical ? CoreCursorType::SizeWestEast : CoreCursorType::SizeNorthSouth;

        bar.PointerEntered([this, grip, cursorType, hotGrip](const IInspectable&, const PointerRoutedEventArgs&) {
            ApplyCursor(cursorType);
            grip.Background(hotGrip);
        });
        bar.PointerExited([this, grip, idleGrip](const IInspectable&, const PointerRoutedEventArgs&) {
            if (_dragKind == DragKind::None) // mid-drag the pointer may leave the thin bar — keep it hot
            {
                ApplyCursor(CoreCursorType::Arrow);
                grip.Background(idleGrip);
            }
        });
        bar.PointerPressed([this, vertical, grip, hotGrip](const IInspectable& s, const PointerRoutedEventArgs& e) {
            grip.Background(hotGrip);
            _OnSplitterPressed(s, e, vertical);
        });
        bar.PointerMoved([this, vertical, cursorType](const IInspectable&, const PointerRoutedEventArgs& e) {
            if (_dragKind == DragKind::None)
            {
                ApplyCursor(cursorType); // re-assert the resize cursor while hovering (covers post-release)
                return;
            }
            _OnSplitterMoved(e, vertical);
        });
        bar.PointerReleased([this, grip, idleGrip](const IInspectable& s, const PointerRoutedEventArgs& e) {
            _OnSplitterReleased(s, e);
            grip.Background(idleGrip);
        });
        bar.PointerCaptureLost([this, grip, idleGrip](const IInspectable& s, const PointerRoutedEventArgs& e) {
            _OnSplitterReleased(s, e);
            grip.Background(idleGrip);
        });
        return bar;
    }

    void AgentManagerContent::_OnSplitterPressed(const IInspectable& sender, const PointerRoutedEventArgs& e, bool vertical)
    {
        if (!_root)
        {
            return;
        }
        _dragKind = vertical ? DragKind::Cols : DragKind::Rows;
        // Pin the two tracks' sizes + the pointer's root-relative coord at press; the move
        // handler derives everything from these fixed values, so the boundary tracks the
        // cursor 1:1 with no feedback from the live re-layout. Track ActualWidth/Height is the
        // exact star-space allotment, so star weights set to pixels land pixel-perfect.
        const auto pos = e.GetCurrentPoint(_root).Position();
        if (vertical)
        {
            _dragOrigin = pos.X;
            _dragSizeA = _treeCol ? _treeCol.ActualWidth() : 0.0;
            _dragSizeB = _planCol ? _planCol.ActualWidth() : 0.0;
        }
        else
        {
            _dragOrigin = pos.Y;
            _dragSizeA = _boardRow ? _boardRow.ActualHeight() : 0.0;
            _dragSizeB = _bottomRow ? _bottomRow.ActualHeight() : 0.0;
        }
        if (const auto el = sender.try_as<UIElement>())
        {
            el.CapturePointer(e.Pointer());
        }
        ApplyCursor(vertical ? CoreCursorType::SizeWestEast : CoreCursorType::SizeNorthSouth);
        e.Handled(true);
    }

    void AgentManagerContent::_OnSplitterMoved(const PointerRoutedEventArgs& e, bool vertical)
    {
        if (_dragKind == DragKind::None || !_root)
        {
            return;
        }
        const auto pos = e.GetCurrentPoint(_root).Position();
        const double cur = vertical ? static_cast<double>(pos.X) : static_cast<double>(pos.Y);
        const double total = _dragSizeA + _dragSizeB;
        constexpr double minPx = 80.0; // never let a pane shrink below this
        if (total < (minPx * 2.0) + 1.0)
        {
            return; // not enough room to split sensibly — leave the panes alone
        }
        const double newA = std::clamp(_dragSizeA + (cur - _dragOrigin), minPx, total - minPx);
        const double newB = total - newA;
        const auto star = [](double v) { return GridLengthHelper::FromValueAndType(v, GridUnitType::Star); };
        if (vertical)
        {
            if (_treeCol)
            {
                _treeCol.Width(star(newA));
            }
            if (_planCol)
            {
                _planCol.Width(star(newB));
            }
        }
        else
        {
            if (_boardRow)
            {
                _boardRow.Height(star(newA));
            }
            if (_bottomRow)
            {
                _bottomRow.Height(star(newB));
            }
        }
        e.Handled(true);
    }

    void AgentManagerContent::_OnSplitterReleased(const IInspectable& sender, const PointerRoutedEventArgs& e)
    {
        if (_dragKind == DragKind::None)
        {
            return; // a capture-lost echo of our own release, or a stray event — nothing to do
        }
        const bool vertical = (_dragKind == DragKind::Cols);
        _dragKind = DragKind::None; // clear BEFORE releasing capture so the re-entrant CaptureLost no-ops

        if (const auto el = sender.try_as<UIElement>())
        {
            el.ReleasePointerCaptures();
        }

        // Persist the new split as a fraction read straight from the star weights we just
        // applied (synchronous + exact, unlike ActualWidth which trails by a layout pass).
        // a/t is correct whether the weights are the seed FRACTIONS (sum == 1.0, e.g. a stray
        // click with no drag) or post-drag PIXELS (sum in the hundreds); guard only t == 0.
        const auto fraction = [](double a, double b) {
            const double t = a + b;
            return t > 0.0 ? std::clamp(a / t, 0.1, 0.9) : 0.4;
        };
        if (vertical)
        {
            if (_treeCol && _planCol)
            {
                _layout.treeFraction = fraction(_treeCol.Width().Value, _planCol.Width().Value);
            }
        }
        else
        {
            if (_boardRow && _bottomRow)
            {
                _layout.boardFraction = fraction(_boardRow.Height().Value, _bottomRow.Height().Value);
            }
        }
        ::Agentmaster::SaveLayout(_layout);

        ApplyCursor(CoreCursorType::Arrow);
        e.Handled(true);
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
                if (!s.live)
                {
                    continue; // archived (closed) sessions live in the "Archived" overlay, not the board
                }
                if (!_scopeDir.empty() && !PathEq(s.workingDir, _scopeDir))
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
        // While an in-place rename editor is live, leave the tree untouched so the frequent
        // hook-driven refreshes can't steal its focus or discard typed text. Commit/cancel
        // clears _renamingId and rebuilds. (_OnRenameSession nulls _renameBox to bootstrap
        // the one rebuild that creates the editor.)
        if (!_renamingId.empty() && _renameBox)
        {
            return;
        }
        _treeHost.Children().Clear();

        // Agentmaster: apply the Explorer Tree scope. LOCAL (default) keeps only the sessions
        // hosted in THIS window (the page's _claudeTabs, surfaced by _localScopeProvider); GLOBAL
        // keeps every window's session (the whole process-wide registry). All the dir grouping +
        // counts + rows below iterate `scoped`, so the filter flows through uniformly. GLOBAL (or
        // an unwired provider, e.g. mid-init) is a no-op view onto the original snapshot.
        std::vector<SessionInfo> scopedStore;
        const std::vector<SessionInfo>* scopedPtr = &sessions;
        if (!_treeGlobalScope && _localScopeProvider)
        {
            const auto localIds = _localScopeProvider();
            scopedStore.reserve(sessions.size());
            for (const auto& s : sessions)
            {
                if (localIds.find(s.id) != localIds.end())
                {
                    scopedStore.push_back(s);
                }
            }
            scopedPtr = &scopedStore;
        }
        const std::vector<SessionInfo>& scoped = *scopedPtr;

        // Ordered, de-duplicated working directories. Paths that differ only by case (on
        // Windows) collapse into one root; the first-seen spelling becomes its display name.
        std::vector<std::wstring> dirs;
        for (const auto& s : scoped)
        {
            if (!s.live)
            {
                continue; // archived sessions are shown in the "Archived" overlay, not the tree
            }
            if (std::find_if(dirs.begin(), dirs.end(), [&](const std::wstring& d) { return PathEq(d, s.workingDir); }) == dirs.end())
            {
                dirs.push_back(s.workingDir);
            }
        }

        if (dirs.empty())
        {
            // In LOCAL scope an empty tree can simply mean other windows hold the sessions; say so
            // (and hint at GLOBAL) rather than implying the whole fleet is empty.
            const wchar_t* empty = (!_treeGlobalScope && _localScopeProvider)
                                       ? L"No sessions in this window \x2014 Launch above, or switch to GLOBAL for all windows."
                                       : L"No sessions yet \x2014 use Launch session above.";
            _treeHost.Children().Append(Text(empty, 12, false, 0.6));
            return;
        }

        // Tracks the directory header rendered just above the current one, so collapsing the
        // selected directory can move the scope to its predecessor ("" — all directories — when
        // the selected one is the topmost header).
        std::wstring prevDir;
        for (const auto& dir : dirs)
        {
            const bool collapsed = _collapsedDirs.find(dir) != _collapsedDirs.end();

            // dir header. A click resolves the select-vs-collapse collision by state:
            //   collapsed             -> uncollapse + select
            //   expanded + unselected -> select (stay expanded)
            //   expanded + selected   -> collapse + select the previous directory
            int count = 0;
            for (const auto& s : scoped)
            {
                if (s.live && PathEq(s.workingDir, dir))
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
            dirBtn.Background(Fill(PathEq(dir, _scopeDir) ? 0x30 : 0x00, 0x80, 0x80, 0x80));
            dirBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            dirBtn.Padding(Thickness{ 4, 2, 4, 2 });
            const auto capturedDir = dir;
            const auto capturedPrevDir = prevDir; // predecessor at build time, for "collapse + select previous"
            dirBtn.Click([this, capturedDir, capturedPrevDir](const IInspectable&, const RoutedEventArgs&) {
                const bool isCollapsed = _collapsedDirs.find(capturedDir) != _collapsedDirs.end();
                const bool isSelected = PathEq(capturedDir, _scopeDir);
                if (isCollapsed)
                {
                    // click + collapsed -> uncollapse + select
                    _collapsedDirs.erase(capturedDir);
                    _SetScope(capturedDir);
                }
                else if (!isSelected)
                {
                    // click + expanded + unselected -> select (leave it expanded)
                    _SetScope(capturedDir);
                }
                else
                {
                    // click + expanded + selected -> collapse + select the previous directory
                    // (capturedPrevDir is "" for the topmost header, which scopes to all dirs)
                    _collapsedDirs.insert(capturedDir);
                    _SetScope(capturedPrevDir);
                }
            });
            _treeHost.Children().Append(dirBtn);

            // This header is the predecessor of the next one. Set before the collapse-skip
            // below so a collapsed directory still counts as a predecessor.
            prevDir = dir;

            if (collapsed)
            {
                continue;
            }

            for (const auto& s : scoped)
            {
                if (!s.live || !PathEq(s.workingDir, dir))
                {
                    continue;
                }
                const auto id = s.id;

                // In-place rename editor for this row (see _renameBox note in the header).
                // Editing inline sidesteps the XAML-Islands trap where a text box inside a
                // ContentDialog receives no keypresses.
                if (!_renamingId.empty() && _renamingId == id)
                {
                    auto box = TextBox{};
                    box.Text(s.title);
                    box.Margin(Thickness{ 16, 0, 0, 4 });
                    box.KeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (e.Key() == VirtualKey::Enter)
                        {
                            _CommitRename();
                            e.Handled(true);
                        }
                        else if (e.Key() == VirtualKey::Escape)
                        {
                            _CancelRename();
                            e.Handled(true);
                        }
                    });
                    box.LostFocus([this](const IInspectable&, const RoutedEventArgs&) { _CommitRename(); });
                    // Focus + select-all once it is actually in the tree (Loaded) so the first
                    // keystroke replaces the old name.
                    box.Loaded([box](const IInspectable&, const RoutedEventArgs&) {
                        box.Focus(FocusState::Programmatic);
                        box.SelectAll();
                    });
                    _renameBox = box;
                    _treeHost.Children().Append(box);
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

                // Single click = select; double click (within the OS threshold) = Activate
                // (jump to the live tab). A Button swallows DoubleTapped, so we time the
                // successive clicks ourselves.
                rowBtn.Click([this, id](const IInspectable&, const RoutedEventArgs&) {
                    const auto nowTick = ::GetTickCount64();
                    const bool dbl = (id == _lastTreeClickId) && (nowTick - _lastTreeClickTick) <= ::GetDoubleClickTime();
                    _lastTreeClickId = id;
                    _lastTreeClickTick = nowTick;
                    if (dbl && _activateHandler)
                    {
                        _activateHandler(winrt::hstring{ id });
                    }
                    else
                    {
                        _SelectSession(id);
                    }
                });
                // Enter = Activate (jump to live tab) — NEVER inject (Correctness Rule #2).
                // Delete = confirm-guarded delete; F2 = rename.
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
                        _RequestArchive(id); // Del archives (shut down + keep restorable), not discard
                        e.Handled(true);
                    }
                    else if (key == VirtualKey::F2)
                    {
                        _OnRenameSession(id);
                        e.Handled(true);
                    }
                });
                // Right-click (or context key / long-press) menu: Rename / Delete.
                rowBtn.ContextFlyout(_MakeSessionMenu(id));
                _treeHost.Children().Append(rowBtn);
            }
        }
    }

    // Agentmaster: flip the Explorer Tree between LOCAL (this window's sessions) and GLOBAL (all
    // windows), then rebuild from the current snapshot. The toggle is per-window, in-memory state.
    void AgentManagerContent::_ToggleTreeScope()
    {
        _treeGlobalScope = !_treeGlobalScope;
        _UpdateTreeScopeButton();
        if (_registry)
        {
            _RebuildTree(_registry->Snapshot());
        }
    }

    // Reflect the current scope on the toggle button's label.
    void AgentManagerContent::_UpdateTreeScopeButton()
    {
        if (_treeScopeBtn)
        {
            _treeScopeBtn.Content(winrt::box_value(_treeGlobalScope ? L"GLOBAL" : L"LOCAL"));
        }
    }

    // ---- Explorer-tree session actions (right-click menu, rename, delete) ----

    MenuFlyout AgentManagerContent::_MakeSessionMenu(const std::wstring& id)
    {
        MenuFlyout menu;
        auto disp = _dispatcher;
        auto weak = get_weak();

        // Both items defer one tick: a MenuFlyout restores focus to its target as it closes,
        // which would otherwise yank focus out of the freshly-shown rename editor / dialog.
        MenuFlyoutItem rename;
        rename.Text(L"Rename\x2026");
        rename.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { self->_OnRenameSession(id); } });
            }
            else if (auto self = weak.get())
            {
                self->_OnRenameSession(id);
            }
        });
        menu.Items().Append(rename);

        MenuFlyoutItem archive;
        archive.Text(L"Archive\x2026");
        archive.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { self->_RequestArchive(id); } });
            }
            else if (auto self = weak.get())
            {
                self->_RequestArchive(id);
            }
        });
        menu.Items().Append(archive);

        return menu;
    }

    void AgentManagerContent::_OnRenameSession(const std::wstring& id)
    {
        if (id.empty())
        {
            return;
        }
        _renamingId = id;
        _renameBox = nullptr; // bootstrap: force the next rebuild to create + focus the editor
        _Refresh();
    }

    void AgentManagerContent::_CommitRename()
    {
        if (_renamingId.empty())
        {
            return;
        }
        const auto id = _renamingId;
        std::wstring name = _renameBox ? std::wstring{ _renameBox.Text() } : std::wstring{};
        // Trim surrounding whitespace; an empty/whitespace name keeps the old title.
        const auto first = name.find_first_not_of(L" \t\r\n");
        const auto last = name.find_last_not_of(L" \t\r\n");
        name = (first == std::wstring::npos) ? std::wstring{} : name.substr(first, last - first + 1);

        _renamingId.clear();
        _renameBox = nullptr;
        if (!name.empty())
        {
            // The title is ONE value: the Explorer-tree name == the WT tab title == the persisted
            // SessionInfo.title. Route through the page so it updates the shared registry AND
            // retitles the session's tab in lockstep; the direct registry write is the fallback
            // when unwired (e.g. the standalone tests).
            if (_renameHandler)
            {
                _renameHandler(winrt::hstring{ id }, winrt::hstring{ name });
            }
            else if (_registry)
            {
                _registry->Update(id, [&](SessionInfo& s) { s.title = name; });
            }
        }
        _Refresh();
    }

    void AgentManagerContent::_CancelRename()
    {
        if (_renamingId.empty())
        {
            return;
        }
        _renamingId.clear();
        _renameBox = nullptr;
        _Refresh();
    }

    void AgentManagerContent::_RequestArchive(const std::wstring& id)
    {
        // "Delete"/"Archive" in the Manager routes to the page's archive seam, which PRESENTS THE
        // CONSEQUENCE (gated by the cog's confirmBeforeKill) and closes the tab — the SAME path as
        // clicking the tab's own X. We deliberately do not confirm here, to avoid a double dialog.
        if (id.empty())
        {
            return;
        }
        if (_archiveHandler)
        {
            _archiveHandler(winrt::hstring{ id });
        }
    }

    // A buttons-only confirm (XAML-Islands-safe: a text box inside a ContentDialog gets no
    // keypresses, but buttons work — see the _renameBox note). Runs onYes when the user accepts.
    // Used for Restore / Restore-all (the archive confirm itself lives at the page level).
    void AgentManagerContent::_Confirm(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, std::function<void()> onYes)
    {
        ContentDialog dialog;
        dialog.Title(winrt::box_value(title));
        dialog.Content(winrt::box_value(body));
        dialog.PrimaryButtonText(primary);
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel
        if (_root)
        {
            try
            {
                dialog.XamlRoot(_root.XamlRoot());
                dialog.RequestedTheme(_root.ActualTheme());
            }
            catch (...)
            {
            }
        }
        auto cb = std::move(onYes);
        dialog.PrimaryButtonClick([cb](const ContentDialog&, const ContentDialogButtonClickEventArgs&) {
            if (cb)
            {
                cb();
            }
        });
        try
        {
            dialog.ShowAsync();
        }
        catch (...)
        {
        }
    }

    // ---- Archived-sessions overlay ------------------------------------------

    void AgentManagerContent::_BuildArchiveOverlay()
    {
        // A dimmed modal layer (like the settings overlay), built into the main visual tree and
        // toggled by Visibility. Lists archived (closed) sessions; each can be Restored. The
        // backdrop tap cancels; the card swallows taps so inside-clicks don't close it.
        _archiveOverlay = Grid{};
        _archiveOverlay.Visibility(Visibility::Collapsed);
        _archiveOverlay.Background(SolidColorBrush{ ColorHelper::FromArgb(0xA0, 0x00, 0x00, 0x00) });
        Grid::SetRow(_archiveOverlay, 0);
        Grid::SetRowSpan(_archiveOverlay, 99);
        Grid::SetColumnSpan(_archiveOverlay, 99);
        _archiveOverlay.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
            _HideArchive();
        });

        auto card = Border{};
        card.Background(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x25, 0x25, 0x25) });
        card.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0x80, 0x80, 0x80) });
        card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        card.Padding(Thickness{ 20, 16, 20, 16 });
        card.Width(560);
        card.MaxHeight(620);
        card.HorizontalAlignment(HorizontalAlignment::Center);
        card.VerticalAlignment(VerticalAlignment::Center);
        card.RequestedTheme(ElementTheme::Dark);
        card.Tapped([](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs& e) {
            e.Handled(true);
        });

        auto panel = StackPanel{};
        panel.Spacing(10);

        auto headerRow = StackPanel{};
        headerRow.Orientation(Orientation::Horizontal);
        headerRow.Spacing(12);
        headerRow.VerticalAlignment(VerticalAlignment::Center);
        headerRow.Children().Append(Text(L"Archived sessions", 18, true, 1.0));
        auto restoreAll = Button{};
        restoreAll.Content(winrt::box_value(L"Restore all"));
        restoreAll.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnRestoreAll(); });
        headerRow.Children().Append(restoreAll);
        panel.Children().Append(headerRow);

        panel.Children().Append(Text(L"These sessions were closed (archived). Restoring re-launches a session and resumes its conversation + Flight Plan (claude --resume). Nothing here is deleted \x2014 the conversation transcript is kept on disk.", 12, false, 0.7));

        _archiveListHost = StackPanel{};
        _archiveListHost.Spacing(6);
        _archiveListHost.Margin(Thickness{ 0, 4, 0, 0 });
        auto scroll = ScrollViewer{};
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.MaxHeight(440);
        scroll.Content(_archiveListHost);
        panel.Children().Append(scroll);

        auto buttons = StackPanel{};
        buttons.Orientation(Orientation::Horizontal);
        buttons.HorizontalAlignment(HorizontalAlignment::Right);
        buttons.Spacing(8);
        buttons.Margin(Thickness{ 0, 8, 0, 0 });
        auto close = Button{};
        close.Content(winrt::box_value(L"Close"));
        close.Click([this](const IInspectable&, const RoutedEventArgs&) { _HideArchive(); });
        buttons.Children().Append(close);
        panel.Children().Append(buttons);

        card.Child(panel);
        _archiveOverlay.Children().Append(card);
        _root.Children().Append(_archiveOverlay);
    }

    void AgentManagerContent::_ShowArchive()
    {
        if (!_archiveOverlay)
        {
            return;
        }
        _RebuildArchiveList();
        _archiveOverlay.Visibility(Visibility::Visible);
    }

    void AgentManagerContent::_HideArchive()
    {
        if (_archiveOverlay)
        {
            _archiveOverlay.Visibility(Visibility::Collapsed);
        }
    }

    void AgentManagerContent::_RebuildArchiveList()
    {
        if (!_archiveListHost)
        {
            return;
        }
        _archiveListHost.Children().Clear();

        std::vector<SessionInfo> sessions;
        if (_registry)
        {
            sessions = _registry->Snapshot();
        }

        int shown = 0;
        for (const auto& s : sessions)
        {
            if (s.live)
            {
                continue; // only archived (closed) sessions appear here
            }
            ++shown;

            auto infoCol = StackPanel{};
            infoCol.Spacing(1);
            infoCol.VerticalAlignment(VerticalAlignment::Center);
            infoCol.Children().Append(Text(s.title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ s.title }, 14, true, 1.0));
            infoCol.Children().Append(Text(winrt::hstring{ s.workingDir }, 11, false, 0.6));
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
                infoCol.Children().Append(Text(winrt::hstring{ L"\x2699 " } + winrt::to_hstring(sent) + L"/" + winrt::to_hstring(static_cast<int>(s.queue.size())) + L" prompts", 11, false, 0.6));
            }

            auto restore = Button{};
            restore.Content(winrt::box_value(L"Restore"));
            restore.VerticalAlignment(VerticalAlignment::Center);
            const auto id = s.id;
            restore.Click([this, id](const IInspectable&, const RoutedEventArgs&) { _OnRestoreSession(id); });

            auto rowGrid = Grid{};
            {
                ColumnDefinition c0;
                c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                ColumnDefinition c1;
                c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
                rowGrid.ColumnDefinitions().Append(c0);
                rowGrid.ColumnDefinitions().Append(c1);
            }
            Grid::SetColumn(infoCol, 0);
            Grid::SetColumn(restore, 1);
            rowGrid.Children().Append(infoCol);
            rowGrid.Children().Append(restore);

            auto border = Border{};
            border.Background(Fill(0x18, 0x80, 0x80, 0x80));
            border.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            border.Padding(Thickness{ 8, 6, 8, 6 });
            border.Child(rowGrid);
            _archiveListHost.Children().Append(border);
        }

        if (shown == 0)
        {
            _archiveListHost.Children().Append(Text(L"No archived sessions. Closing a session's tab archives it here.", 12, false, 0.6));
        }
    }

    void AgentManagerContent::_OnRestoreSession(const std::wstring& id)
    {
        if (id.empty() || !_restoreHandler)
        {
            return;
        }
        std::wstring title;
        if (_registry)
        {
            if (const auto s = _registry->Get(id))
            {
                title = s->title;
            }
        }
        const winrt::hstring body = title.empty() ?
            winrt::hstring{ L"This re-launches the session and resumes its conversation + Flight Plan (claude --resume)." } :
            winrt::hstring{ L"\x201C" + title + L"\x201D will re-launch and resume its conversation + Flight Plan (claude --resume)." };
        const auto restore = _restoreHandler;
        const winrt::hstring hid{ id };
        _Confirm(L"Restore session?", body, L"Restore", [restore, hid, this]() {
            restore(hid);
            _HideArchive();
        });
    }

    void AgentManagerContent::_OnRestoreAll()
    {
        if (!_restoreHandler || !_registry)
        {
            return;
        }
        std::vector<std::wstring> ids;
        for (const auto& s : _registry->Snapshot())
        {
            if (!s.live)
            {
                ids.push_back(s.id);
            }
        }
        if (ids.empty())
        {
            return;
        }
        const auto restore = _restoreHandler;
        _Confirm(L"Restore all archived sessions?",
                 winrt::hstring{ L"This re-launches and resumes " } + winrt::to_hstring(static_cast<int>(ids.size())) + L" session(s), each with its Flight Plan.",
                 L"Restore all",
                 [restore, ids, this]() {
                     for (const auto& id : ids)
                     {
                         restore(winrt::hstring{ id });
                     }
                     _HideArchive();
                 });
    }

    void AgentManagerContent::_UpdateArchivedButton(const std::vector<SessionInfo>& sessions)
    {
        if (!_archivedBtn)
        {
            return;
        }
        int archived = 0;
        for (const auto& s : sessions)
        {
            if (!s.live)
            {
                ++archived;
            }
        }
        _archivedBtn.Content(winrt::box_value(archived > 0 ?
                                                  winrt::hstring{ L"Archived (" } + winrt::to_hstring(archived) + L")" :
                                                  winrt::hstring{ L"Archived" }));
        _archivedBtn.IsEnabled(archived > 0);
    }

    void AgentManagerContent::_BuildSettingsOverlay()
    {
        // A dimmed, full-bleed modal layer over the whole Manager. Deliberately NOT a
        // ContentDialog: a text box inside a ContentDialog gets no keypresses in XAML Islands
        // (see the _renameBox note), and this surface has free-text fields (model, dir, count).
        // Living in the main visual tree, its TextBoxes behave normally.
        _settingsOverlay = Grid{};
        _settingsOverlay.Visibility(Visibility::Collapsed);
        _settingsOverlay.Background(SolidColorBrush{ ColorHelper::FromArgb(0xA0, 0x00, 0x00, 0x00) });
        Grid::SetRow(_settingsOverlay, 0);
        Grid::SetRowSpan(_settingsOverlay, 99); // cover every row of _root regardless of count
        Grid::SetColumnSpan(_settingsOverlay, 99);
        // Click on the backdrop = Cancel; the card swallows taps so inside-clicks don't close.
        _settingsOverlay.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
            _HideSettings();
        });

        auto card = Border{};
        card.Background(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x25, 0x25, 0x25) });
        card.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0x80, 0x80, 0x80) });
        card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        card.Padding(Thickness{ 20, 16, 20, 16 });
        card.Width(460);
        card.HorizontalAlignment(HorizontalAlignment::Center);
        card.VerticalAlignment(VerticalAlignment::Center);
        card.RequestedTheme(ElementTheme::Dark);
        card.Tapped([](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs& e) {
            e.Handled(true);
        });

        auto panel = StackPanel{};
        panel.Spacing(10);
        panel.Children().Append(Text(L"Agentmaster Settings", 18, true, 1.0));

        // CLAUDE SESSIONS
        panel.Children().Append(Text(L"CLAUDE SESSIONS", 11, true, 0.6));
        _setSkipPermissions = ToggleSwitch{};
        _setSkipPermissions.Header(winrt::box_value(L"Skip permission prompts (bypass)"));
        panel.Children().Append(_setSkipPermissions);
        _setModel = TextBox{};
        _setModel.Header(winrt::box_value(L"Model"));
        _setModel.PlaceholderText(L"As Is \x2014 blank keeps Claude's default (e.g. opus / sonnet)");
        panel.Children().Append(_setModel);
        _setIncludeCoAuthored = ToggleSwitch{};
        _setIncludeCoAuthored.Header(winrt::box_value(L"Include co-authored-by in commits"));
        panel.Children().Append(_setIncludeCoAuthored);
        _setEnv = TextBox{};
        _setEnv.Header(winrt::box_value(L"Environment variables (applied to every session)"));
        _setEnv.PlaceholderText(L"NAME=VALUE;NAME=VALUE  (e.g. FOO=bar;HTTPS_PROXY=http://h:8080)");
        _setEnv.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setEnv);

        // AUTOPILOT
        panel.Children().Append(Text(L"AUTOPILOT (defaults for new sessions)", 11, true, 0.6));
        _setDefaultMode = ComboBox{};
        _setDefaultMode.Header(winrt::box_value(L"New-session mode"));
        _setDefaultMode.Items().Append(winrt::box_value(L"Off"));
        _setDefaultMode.Items().Append(winrt::box_value(L"SemiAuto"));
        _setDefaultMode.Items().Append(winrt::box_value(L"Full"));
        panel.Children().Append(_setDefaultMode);
        _setMaxAutoSends = TextBox{};
        _setMaxAutoSends.Header(winrt::box_value(L"Max auto-sends per run"));
        _setMaxAutoSends.PlaceholderText(L"100");
        panel.Children().Append(_setMaxAutoSends);
        _setStopOnError = ToggleSwitch{};
        _setStopOnError.Header(winrt::box_value(L"Stop on error"));
        panel.Children().Append(_setStopOnError);
        _setPauseOnHuman = ToggleSwitch{};
        _setPauseOnHuman.Header(winrt::box_value(L"Pause on human input"));
        panel.Children().Append(_setPauseOnHuman);

        // BEHAVIOR
        panel.Children().Append(Text(L"BEHAVIOR", 11, true, 0.6));
        _setConfirmKill = ToggleSwitch{};
        _setConfirmKill.Header(winrt::box_value(L"Confirm before archiving a session"));
        panel.Children().Append(_setConfirmKill);
        _setLaunchDir = TextBox{};
        _setLaunchDir.Header(winrt::box_value(L"Default Launch directory"));
        _setLaunchDir.PlaceholderText(L"blank \x2014 defaults to %USERPROFILE%");
        panel.Children().Append(_setLaunchDir);
        _setRecentDirsLimit = TextBox{};
        _setRecentDirsLimit.Header(winrt::box_value(L"Recent Launch directories to remember"));
        _setRecentDirsLimit.PlaceholderText(L"10");
        panel.Children().Append(_setRecentDirsLimit);

        // Cancel / Save
        auto buttons = StackPanel{};
        buttons.Orientation(Orientation::Horizontal);
        buttons.HorizontalAlignment(HorizontalAlignment::Right);
        buttons.Spacing(8);
        buttons.Margin(Thickness{ 0, 8, 0, 0 });
        auto cancel = Button{};
        cancel.Content(winrt::box_value(L"Cancel"));
        cancel.Click([this](const IInspectable&, const RoutedEventArgs&) { _HideSettings(); });
        auto save = Button{};
        save.Content(winrt::box_value(L"Save"));
        save.Click([this](const IInspectable&, const RoutedEventArgs&) { _SaveSettings(); });
        buttons.Children().Append(cancel);
        buttons.Children().Append(save);
        panel.Children().Append(buttons);

        auto scroll = ScrollViewer{};
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.MaxHeight(560);
        scroll.Content(panel);
        card.Child(scroll);

        _settingsOverlay.Children().Append(card);
        _root.Children().Append(_settingsOverlay);
    }

    void AgentManagerContent::_ShowSettings()
    {
        if (!_settingsOverlay)
        {
            return;
        }
        // Populate every control from the current settings before revealing.
        if (_setSkipPermissions)
        {
            _setSkipPermissions.IsOn(_appSettings.skipPermissions);
        }
        if (_setModel)
        {
            _setModel.Text(winrt::hstring{ _appSettings.model });
        }
        if (_setIncludeCoAuthored)
        {
            _setIncludeCoAuthored.IsOn(_appSettings.includeCoAuthoredBy);
        }
        if (_setEnv)
        {
            _setEnv.Text(winrt::hstring{ _appSettings.env });
        }
        if (_setDefaultMode)
        {
            _setDefaultMode.SelectedIndex(_appSettings.defaultAutopilotMode == AutopilotMode::Full ? 2 :
                                          _appSettings.defaultAutopilotMode == AutopilotMode::SemiAuto ? 1 :
                                                                                                         0);
        }
        if (_setMaxAutoSends)
        {
            _setMaxAutoSends.Text(winrt::hstring{ std::to_wstring(_appSettings.maxAutoSends) });
        }
        if (_setStopOnError)
        {
            _setStopOnError.IsOn(_appSettings.stopOnError);
        }
        if (_setPauseOnHuman)
        {
            _setPauseOnHuman.IsOn(_appSettings.pauseOnHumanInput);
        }
        if (_setConfirmKill)
        {
            _setConfirmKill.IsOn(_appSettings.confirmBeforeKill);
        }
        if (_setLaunchDir)
        {
            _setLaunchDir.Text(winrt::hstring{ _appSettings.defaultLaunchDir });
        }
        if (_setRecentDirsLimit)
        {
            _setRecentDirsLimit.Text(winrt::hstring{ std::to_wstring(_appSettings.recentDirsLimit) });
        }
        _settingsOverlay.Visibility(Visibility::Visible);
    }

    void AgentManagerContent::_HideSettings()
    {
        if (_settingsOverlay)
        {
            _settingsOverlay.Visibility(Visibility::Collapsed);
        }
    }

    void AgentManagerContent::_SaveSettings()
    {
        if (_setSkipPermissions)
        {
            _appSettings.skipPermissions = _setSkipPermissions.IsOn();
        }
        if (_setModel)
        {
            std::wstring m{ _setModel.Text() };
            const auto a = m.find_first_not_of(L" \t");
            const auto b = m.find_last_not_of(L" \t");
            _appSettings.model = (a == std::wstring::npos) ? std::wstring{} : m.substr(a, b - a + 1);
        }
        if (_setIncludeCoAuthored)
        {
            _appSettings.includeCoAuthoredBy = _setIncludeCoAuthored.IsOn();
        }
        if (_setEnv)
        {
            _appSettings.env = std::wstring{ _setEnv.Text() };
        }
        if (_setDefaultMode)
        {
            const int idx = _setDefaultMode.SelectedIndex();
            _appSettings.defaultAutopilotMode = idx == 2 ? AutopilotMode::Full : idx == 1 ? AutopilotMode::SemiAuto :
                                                                                            AutopilotMode::Off;
        }
        if (_setMaxAutoSends)
        {
            const std::wstring t{ _setMaxAutoSends.Text() };
            uint32_t v = 0;
            bool any = false;
            for (const wchar_t c : t)
            {
                if (c >= L'0' && c <= L'9')
                {
                    v = v * 10 + static_cast<uint32_t>(c - L'0');
                    any = true;
                }
            }
            _appSettings.maxAutoSends = (any && v > 0) ? v : 100; // empty/zero/garbage -> default backstop
        }
        if (_setStopOnError)
        {
            _appSettings.stopOnError = _setStopOnError.IsOn();
        }
        if (_setPauseOnHuman)
        {
            _appSettings.pauseOnHumanInput = _setPauseOnHuman.IsOn();
        }
        if (_setConfirmKill)
        {
            _appSettings.confirmBeforeKill = _setConfirmKill.IsOn();
        }
        if (_setLaunchDir)
        {
            _appSettings.defaultLaunchDir = std::wstring{ _setLaunchDir.Text() };
        }
        if (_setRecentDirsLimit)
        {
            const std::wstring t{ _setRecentDirsLimit.Text() };
            uint32_t v = 0;
            bool any = false;
            for (const wchar_t c : t)
            {
                if (c >= L'0' && c <= L'9')
                {
                    v = v * 10 + static_cast<uint32_t>(c - L'0');
                    any = true;
                }
            }
            _appSettings.recentDirsLimit = (any && v > 0) ? v : 10; // empty/zero/garbage -> default
        }
        if (_settingsSink)
        {
            _settingsSink(_appSettings); // page persists + applies to future spawns
        }
        _HideSettings();
    }

    void AgentManagerContent::_RebuildPlan(const std::vector<SessionInfo>& sessions)
    {
        _planHeaderHost.Children().Clear();
        _planListHost.Children().Clear();

        const auto sel = _Selected(sessions);
        if (!sel || !sel->live) // an archived (closed) session isn't planned here — restore it first
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

        // The Flight Plan reflects EVERY message this session received — not only ones queued
        // here. Split the queue into a chronological "sent" summary (each tagged by origin:
        // queued-by-flight vs typed-straight-into-the-terminal) followed by the upcoming queue.
        std::vector<const QueuedPrompt*> sent;
        std::vector<const QueuedPrompt*> upcoming;
        for (const auto& p : sel->queue)
        {
            if (p.status == PromptStatus::Pending || p.status == PromptStatus::Held)
            {
                upcoming.push_back(&p);
            }
            else
            {
                sent.push_back(&p);
            }
        }
        // Order the summary by send time; an un-timestamped item (e.g. Skipped) sinks to the
        // bottom of the summary, just above the upcoming queue.
        std::stable_sort(sent.begin(), sent.end(), [](const QueuedPrompt* a, const QueuedPrompt* b) {
            const int64_t ka = a->sentAtUnixMs > 0 ? a->sentAtUnixMs : INT64_MAX;
            const int64_t kb = b->sentAtUnixMs > 0 ? b->sentAtUnixMs : INT64_MAX;
            return ka < kb;
        });

        if (sent.empty() && upcoming.empty())
        {
            _planListHost.Children().Append(Text(L"No messages yet. Type into the session, or queue one below.", 12, false, 0.6));
            return;
        }

        // One clickable prompt row. `showOrigin` (the sent summary) swaps the gate badge for a
        // flight/typed chip so you can see which messages the Manager sent vs. you typed.
        auto appendRow = [&](const QueuedPrompt& p, bool showOrigin) {
            const bool selected = (p.id == _selectedPromptId);

            auto row = StackPanel{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(8);
            row.Children().Append(Text(PromptGlyph(p.status), 13, false, 0.9));
            auto lbl = Text(p.label.empty() ? winrt::hstring{ p.text } : winrt::hstring{ p.label }, 13, false, 1.0);
            lbl.MaxWidth(320);
            row.Children().Append(lbl);
            if (showOrigin)
            {
                const bool typed = (p.origin == PromptOrigin::Typed);
                // Amber "typed" (a human keystroke) vs. blue "flight" (queued + injected by us).
                row.Children().Append(Pill(typed ? winrt::hstring{ L"typed" } : winrt::hstring{ L"flight" },
                                           typed ? ColorHelper::FromArgb(0xFF, 0xD9, 0xA6, 0x2E) : ColorHelper::FromArgb(0xFF, 0x4F, 0x8B, 0xD0)));
            }
            else
            {
                row.Children().Append(Text(GateBadge(p.gate), 11, false, 0.5));
            }
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
        };

        // A small dim section caption (e.g. "SENT — 4  (1 typed)").
        auto caption = [&](const winrt::hstring& s, double topMargin) {
            auto c = Text(s, 11, true, 0.5);
            c.Margin(Thickness{ 0, topMargin, 0, 4 });
            _planListHost.Children().Append(c);
        };

        if (!sent.empty())
        {
            int typedCount = 0;
            for (const auto* p : sent)
            {
                if (p->origin == PromptOrigin::Typed)
                {
                    ++typedCount;
                }
            }
            caption(winrt::hstring{ L"SENT \x2014 " } + winrt::to_hstring(static_cast<int>(sent.size())) + L"  (" + winrt::to_hstring(typedCount) + L" typed)", 0.0);
            for (const auto* p : sent)
            {
                appendRow(*p, true);
            }
        }
        if (!upcoming.empty())
        {
            caption(winrt::hstring{ L"UPCOMING \x2014 " } + winrt::to_hstring(static_cast<int>(upcoming.size())), sent.empty() ? 0.0 : 8.0);
            for (const auto* p : upcoming)
            {
                appendRow(*p, false);
            }
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
            _NormalizeCwdBox(); // launch with — and remember — a normalized path
            const auto dir = _cwdBox ? _cwdBox.Text() : winrt::hstring{};
            _PushRecentDir(std::wstring{ dir }); // remember it as "recently selected"
            _ClosePathPicker();
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
                p.sentAtUnixMs = NowMs(); // timestamp so it sorts into the "sent" summary
                p.echoed = false; // await this injection's UserPromptSubmit echo (don't double-record)
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
                    target->sentAtUnixMs = NowMs();
                    target->echoed = false; // await this injection's UserPromptSubmit echo
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
            if (PathEq(s.workingDir, dir))
            {
                _registry->Update(s.id, [&](SessionInfo& ss) { ::Agentmaster::AppendTemplateToQueue(ss.queue, tmpl); });
            }
        }
    }

    // ---- Launch path-picker drop-down ---------------------------------------

    std::vector<std::wstring> AgentManagerContent::_CollectRecentDirs(const std::wstring& current) const
    {
        // The RECENT section length is a global setting (AppSettings::recentDirsLimit, default
        // 10); 0/garbage falls back to 10.
        const size_t limit = _appSettings.recentDirsLimit > 0 ? _appSettings.recentDirsLimit : 10;
        std::vector<std::wstring> out;
        auto add = [&](const std::wstring& d) {
            if (d.empty() || out.size() >= limit)
            {
                return;
            }
            if (!current.empty() && PathEq(d, current))
            {
                return; // exclude the path that's currently in the box
            }
            for (const auto& e : out)
            {
                if (PathEq(e, d))
                {
                    return; // dedup
                }
            }
            out.push_back(d);
        };

        for (const auto& d : _recentDirs)
        {
            add(d);
        }
        // Supplement from live sessions (most-recently-active first) so the list is useful
        // even before anything has been launched this run.
        if (out.size() < limit && _registry)
        {
            auto snap = _registry->Snapshot();
            std::sort(snap.begin(), snap.end(), [](const SessionInfo& a, const SessionInfo& b) {
                return a.lastActivityUnixMs > b.lastActivityUnixMs;
            });
            for (const auto& s : snap)
            {
                add(s.workingDir);
            }
        }
        return out;
    }

    void AgentManagerContent::_PushRecentDir(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return;
        }
        _recentDirs.erase(std::remove_if(_recentDirs.begin(), _recentDirs.end(), [&](const std::wstring& e) { return PathEq(e, dir); }), _recentDirs.end());
        _recentDirs.insert(_recentDirs.begin(), dir);
        const size_t cap = _appSettings.recentDirsLimit > 0 ? _appSettings.recentDirsLimit : 10;
        if (_recentDirs.size() > cap)
        {
            _recentDirs.resize(cap);
        }
        ::Agentmaster::SaveRecentDirs(_recentDirs);
    }

    Button AgentManagerContent::_MakePathRow(const std::wstring& fullPath, const winrt::hstring& glyph, const winrt::hstring& displayText)
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(Text(glyph, 13, false, 0.7));
        // Recents show their full path (displayText empty); folders show just the leaf —
        // the section header already states which directory they live in.
        row.Children().Append(Text(displayText.empty() ? winrt::hstring{ fullPath } : displayText, 13, false, 1.0));

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        // The list is click-only and must NOT take focus from the cwd box. If a row grabbed
        // focus, the box's LostFocus could race ahead of this Click and tear down the popup
        // (and this very button) before the pick registers — so clicking a row would appear
        // to do nothing. Keeping focus on the box also keeps the popup open across picks.
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        const auto captured = fullPath;
        btn.Click([this, captured](const IInspectable&, const RoutedEventArgs&) { _PickPath(captured); });
        return btn;
    }

    void AgentManagerContent::_RebuildPathPicker()
    {
        if (!_pathListHost)
        {
            return;
        }
        _pathListHost.Children().Clear();

        auto sectionLabel = [](const winrt::hstring& s) {
            auto lbl = Text(s, 11, true, 0.5);
            lbl.Margin(Thickness{ 6, 8, 6, 2 });
            return lbl;
        };

        const std::wstring current = _cwdBox ? std::wstring{ _cwdBox.Text() } : std::wstring{};

        // RECENT (up to AppSettings::recentDirsLimit, current excluded).
        const auto recents = _CollectRecentDirs(current);
        if (!recents.empty())
        {
            _pathListHost.Children().Append(sectionLabel(L"RECENT"));
            for (const auto& d : recents)
            {
                _pathListHost.Children().Append(_MakePathRow(d, L"\x21BB", winrt::hstring{}));
            }
        }

        // SUBFOLDERS of the current path (+ a parent up-nav).
        if (!current.empty())
        {
            _pathListHost.Children().Append(sectionLabel(winrt::hstring{ L"SUBFOLDERS OF " } + winrt::hstring{ current }));

            if (const auto parent = ParentDir(current))
            {
                _pathListHost.Children().Append(_MakePathRow(*parent, L"\x2191", winrt::hstring{ L".. (parent)" }));
            }

            if (IsDir(current))
            {
                const auto subs = EnumSubdirs(current);
                for (const auto& leaf : subs)
                {
                    _pathListHost.Children().Append(_MakePathRow(JoinDir(current, leaf), L"\x25B8", winrt::hstring{ leaf }));
                }
                if (subs.empty())
                {
                    _pathListHost.Children().Append(Text(L"(no subfolders)", 12, false, 0.5));
                }
            }
            else
            {
                _pathListHost.Children().Append(Text(L"(path not found)", 12, false, 0.5));
            }
        }

        if (_pathListHost.Children().Size() == 0)
        {
            _pathListHost.Children().Append(Text(L"Type a path or pick a recent directory.", 12, false, 0.6));
        }
    }

    void AgentManagerContent::_OpenPathPicker()
    {
        if (!_pathPopup || !_cwdBox)
        {
            return;
        }
        _pathPickerUserDismissed = false; // opening clears the dismiss latch
        if (_pathPanelBorder)
        {
            _pathPanelBorder.Width(std::max<double>(380.0, _cwdBox.ActualWidth()));
        }
        try
        {
            const auto xform = _cwdBox.TransformToVisual(_root);
            const auto pt = xform.TransformPoint(Point{ 0.0f, static_cast<float>(_cwdBox.ActualHeight()) });
            _pathPopup.HorizontalOffset(static_cast<double>(pt.X));
            _pathPopup.VerticalOffset(static_cast<double>(pt.Y) + 2.0);
        }
        catch (...)
        {
        }
        _RebuildPathPicker();
        _pathPopup.IsOpen(true);
    }

    void AgentManagerContent::_ClosePathPicker()
    {
        if (_pathPopup)
        {
            _pathPopup.IsOpen(false);
        }
    }

    // Normalize the Launch path box in place. Platform-sensitive via NormPath (Windows: flip
    // '/'->'\' and trim insignificant trailing separators; POSIX: trim trailing '/'). Called
    // on commit (Enter), blur, list-pick, and launch — deliberately NOT per-keystroke, which
    // would eat a trailing separator the user types to descend into a folder.
    void AgentManagerContent::_NormalizeCwdBox()
    {
        if (!_cwdBox)
        {
            return;
        }
        const std::wstring cur{ _cwdBox.Text() };
        const std::wstring norm = NormPath(cur);
        if (norm != cur)
        {
            _cwdBox.Text(winrt::hstring{ norm });
            _cwdBox.Select(static_cast<int32_t>(norm.size()), 0); // caret to end
        }
    }

    void AgentManagerContent::_PickPath(const std::wstring& dir)
    {
        if (!_cwdBox)
        {
            return;
        }
        const std::wstring norm = NormPath(dir); // selecting from the list normalizes too
        _cwdBox.Text(winrt::hstring{ norm }); // fires TextChanged -> _RebuildPathPicker (popup open)
        _cwdBox.Select(static_cast<int32_t>(norm.size()), 0); // caret to end
        _cwdBox.Focus(FocusState::Programmatic); // keep the box focused so the popup stays open
    }
}
