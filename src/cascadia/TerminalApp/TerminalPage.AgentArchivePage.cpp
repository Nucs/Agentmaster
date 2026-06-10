// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the full-window Archive page (C1 UI): a page mounted over TerminalPage's
// Root content rows, opened by the Manager's Archived button. LEFT = a dense sortable +
// searchable table of archived sessions (+ synthetic "saved window" rows); RIGHT = the
// selected row's detail (metadata + read-only Flight Plan + Restore here / Reopen its
// window); multi-select bulk restore; registry-observer live refresh; branch backfill.
// XAML-Islands hard rule throughout: every pointer handler DEFERS its visual-tree
// mutation to a clean dispatcher tick (synchronous mid-click tree changes AV the
// hit-test — pinned from crash dumps).
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / AgentmasterStateDir (record-file stat)
#include "AgentMaster/Engine.h" // RecoverableWindows
#include "AgentMaster/Persistence.h" // SaveSessions (branch backfill)
#include "AgentMaster/ProcessInspect.h" // TranscriptTimes / ReadTranscriptInfo
#include "AgentMaster/SessionRegistry.h" // snapshot + UpdateQuiet + the live-refresh observer

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

namespace winrt::TerminalApp::implementation
{
    // ===== Agentmaster: Archive page (full-window redesign) =================================
    // A "page" mounted over TerminalPage's Root CONTENT rows (covering every pane, below the tab strip;
    // covering the titlebar row crashes XAML input — see the mount note in _BuildArchivePageShell), opened
    // by the Manager's Archived button (SetOpenArchiveHandler -> _ShowArchivePage). LEFT half = a dense sortable table
    // of archived sessions (+ which saved window each belongs to); RIGHT half = a detail/preview of the
    // selected row (metadata + read-only Flight Plan + restore actions). Back returns to the tabs.
    // Replaces the Manager's old in-content modal overlay. (A slide/fade transition is a deferred
    // polish — v1 toggles Visibility; the page is in the main visual tree so its search/sort typing
    // works, unlike a ContentDialog — the XAML-Islands keyboard trap.)
    namespace
    {
        int64_t ArchiveNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        std::wstring ArchiveLower(std::wstring s)
        {
            if (!s.empty())
            {
                // Full Unicode simple lowercase (kernel32). The old ASCII-only A-Z fold silently broke
                // case-insensitive search for any non-ASCII title/branch (Cyrillic/Greek/accented Latin).
                // In-place src == dst is explicitly allowed for LCMAP_LOWERCASE.
                ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr, 0);
            }
            return s;
        }

        // "now" / "5m" / "3h" / "2d" / "3mo" / "1y" from a unix-ms timestamp; "" when unknown (0).
        // Single largest unit (table-cell compact); months(=30d)/years(=365d) so an old archive
        // reads "3mo", not "97d" (the old days cap).
        std::wstring ArchiveAgo(int64_t unixMs, int64_t nowMs)
        {
            if (unixMs <= 0)
            {
                return L"";
            }
            int64_t s = (nowMs - unixMs) / 1000;
            if (s < 0)
            {
                s = 0;
            }
            if (s < 60)
            {
                return L"now";
            }
            if (s < 3600)
            {
                return std::to_wstring(s / 60) + L"m";
            }
            if (s < 86400)
            {
                return std::to_wstring(s / 3600) + L"h";
            }
            if (s < 30LL * 86400)
            {
                return std::to_wstring(s / 86400) + L"d";
            }
            if (s < 365LL * 86400)
            {
                return std::to_wstring(s / (30LL * 86400)) + L"mo";
            }
            return std::to_wstring(s / (365LL * 86400)) + L"y";
        }

        // The prose variant for the detail pane: "just now" / "5m ago" / "" — composing the raw
        // ArchiveAgo into "created <x> ago" read "created now ago" for a <1-minute timestamp.
        std::wstring ArchiveAgoPhrase(int64_t unixMs, int64_t nowMs)
        {
            const auto a = ArchiveAgo(unixMs, nowMs);
            if (a.empty())
            {
                return a;
            }
            return a == L"now" ? std::wstring{ L"just now" } : a + L" ago";
        }

        // Absolute local datetime ("2026-06-10 14:32") for hover tooltips — the table cells show only
        // the compact relative age ("3h" / "2d"), which answers "how long ago?" but not "which day was
        // that?"; the tooltip carries the exact moment. "" when unknown (0) so the tooltip helper no-ops.
        std::wstring ArchiveLocalDateTime(int64_t unixMs)
        {
            if (unixMs <= 0)
            {
                return L"";
            }
            const __time64_t t = unixMs / 1000;
            struct tm local
            {
            };
            if (_localtime64_s(&local, &t) != 0)
            {
                return L"";
            }
            wchar_t buf[24]{};
            swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
            return buf;
        }

        // Attach a hover tooltip to any element; no-op on empty (a cell with nothing beyond what it
        // already shows stays tooltip-less). The table's cells truncate (CharacterEllipsis) or
        // abbreviate ("3h", "3/7", "W2") — the tooltip carries the full value.
        void ArchiveSetTip(const winrt::Windows::UI::Xaml::UIElement& el, const std::wstring& tip)
        {
            if (!tip.empty())
            {
                winrt::Windows::UI::Xaml::Controls::ToolTipService::SetToolTip(el, winrt::box_value(winrt::hstring{ tip }));
            }
        }

        // The W{n} chip's tooltip: what that saved window IS — tab composition + geometry from its
        // WindowRecord, e.g. "W2 · 4 tabs (2 claude, 2 shell) · 1466×780 @ 14,173 · maximized". The
        // chip alone says "W2" and nothing anywhere on the page said what W2 was. Built once per
        // record at gather time and stamped on every row of that window.
        std::wstring ArchiveWindowTip(int ordinal, int claudeTabs, int shellTabs, const ::Agentmaster::WindowGeometry& geo)
        {
            std::wstring tip = L"W" + std::to_wstring(ordinal);
            const int total = claudeTabs + shellTabs;
            tip += L" \x00B7 " + std::to_wstring(total) + (total == 1 ? L" tab" : L" tabs");
            if (total > 0)
            {
                std::wstring comp;
                if (claudeTabs > 0)
                {
                    comp += std::to_wstring(claudeTabs) + L" claude";
                }
                if (shellTabs > 0)
                {
                    comp += (comp.empty() ? std::wstring{} : std::wstring{ L", " }) + std::to_wstring(shellTabs) + L" shell";
                }
                tip += L" (" + comp + L")";
            }
            if (geo.hasSize)
            {
                tip += L" \x00B7 " + std::to_wstring(static_cast<long long>(geo.width)) + L"\x00D7" + std::to_wstring(static_cast<long long>(geo.height));
            }
            if (geo.hasPosition)
            {
                tip += (geo.hasSize ? std::wstring{ L" @ " } : std::wstring{ L" \x00B7 @ " }) + std::to_wstring(static_cast<long long>(geo.x)) + L"," + std::to_wstring(static_cast<long long>(geo.y));
            }
            if (!geo.launchMode.empty() && geo.launchMode != L"default")
            {
                tip += L" \x00B7 " + geo.launchMode;
            }
            return tip;
        }

        // File times of a saved window's record (windows/<id>.json): ctime ≈ when the workspace was
        // first saved, mtime ≈ its last autosave. Sorts/labels the synthetic "saved window" rows.
        // (Same FILETIME→unix-ms conversion ProcessInspect uses internally — not exported there.)
        bool ArchiveWindowRecordTimes(const std::wstring& windowId, int64_t& createdMs, int64_t& lastMs)
        {
            createdMs = 0;
            lastMs = 0;
            if (windowId.empty())
            {
                return false;
            }
            const std::wstring path = ::Agentmaster::AgentmasterStateDir() + L"\\windows\\" + windowId + L".json";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                return false;
            }
            const auto toUnixMs = [](const FILETIME& ft) {
                ULARGE_INTEGER u;
                u.LowPart = ft.dwLowDateTime;
                u.HighPart = ft.dwHighDateTime;
                return static_cast<int64_t>(u.QuadPart / 10000) - 11644473600000LL; // 1601 -> 1970 epoch
            };
            createdMs = toUnixMs(fad.ftCreationTime);
            lastMs = toUnixMs(fad.ftLastWriteTime);
            return true;
        }

        winrt::Windows::UI::Xaml::Media::SolidColorBrush ArchiveBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
        {
            return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b) };
        }

        // A single-line (optionally wrapping) TextBlock with size / weight / opacity. Named to avoid the
        // `Text(...)` helper / `winrt::Windows::UI::Text` namespace clash (a known C2872 gotcha).
        winrt::Windows::UI::Xaml::Controls::TextBlock ArchiveText(winrt::hstring text, double size, bool bold, double opacity, bool wrap = false)
        {
            winrt::Windows::UI::Xaml::Controls::TextBlock tb;
            tb.Text(text);
            tb.FontSize(size);
            if (bold)
            {
                tb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            tb.Opacity(opacity);
            tb.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Center);
            if (wrap)
            {
                tb.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::Wrap);
            }
            else
            {
                tb.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::NoWrap);
                tb.TextTrimming(winrt::Windows::UI::Xaml::TextTrimming::CharacterEllipsis);
            }
            return tb;
        }

        // The dense table's 8 columns (shared by the header + every data row): select · Title · Dir ·
        // Branch · Created · Active · Plan · Window. Pixel for the fixed ends, star for the elastic
        // middle. The fixed time/window columns are sized for "Created ▼" at 11px SemiBold — at the
        // old 52/58px the header's CharacterEllipsis trimmed the sort ARROW off the very column being
        // sorted (the default sort column, no less).
        void ArchiveAddColumns(const winrt::Windows::UI::Xaml::Controls::Grid& g)
        {
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;
            const auto col = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                g.ColumnDefinitions().Append(c);
            };
            col(30, GridUnitType::Pixel); // 0 select
            col(2.2, GridUnitType::Star); // 1 title
            col(3.0, GridUnitType::Star); // 2 dir
            col(1.4, GridUnitType::Star); // 3 branch
            col(64, GridUnitType::Pixel); // 4 created
            col(64, GridUnitType::Pixel); // 5 last activity
            col(46, GridUnitType::Pixel); // 6 plan (sent/total prompts)
            col(64, GridUnitType::Pixel); // 7 window
        }

        // A stable per-window chip color (cycled palette), keyed by the 1-based display ordinal.
        winrt::Windows::UI::Color ArchiveWindowColor(int ordinal)
        {
            static const winrt::Windows::UI::Color kPalette[] = {
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x5E, 0x9C, 0xD6),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x57, 0xA6, 0x73),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xC4, 0x8A, 0x4E),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xB1, 0x6B, 0xC4),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xCB, 0x5C, 0x5C),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x4F, 0xA8, 0xA8),
            };
            const int n = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
            const int i = ((ordinal - 1) % n + n) % n;
            return kPalette[i];
        }
    }

    // Build the page shell ONCE (host + header + table/detail split + footer), mounted over TerminalPage's
    // Root content rows (1-2) so it covers every pane below the tab strip (see the mount note below).
    void TerminalPage::_BuildArchivePageShell()
    {
        if (_archivePageHost)
        {
            return;
        }
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;

        Grid host;
        host.Background(ArchiveBrush(0xFF, 0x1B, 0x1B, 0x1B)); // opaque -> fully hides the tabs behind it
        host.RequestedTheme(ElementTheme::Dark);
        host.Visibility(Visibility::Collapsed);
        {
            const auto row = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                host.RowDefinitions().Append(r);
            };
            row(0, GridUnitType::Auto); // 0 header
            row(1, GridUnitType::Star); // 1 body
            row(0, GridUnitType::Auto); // 2 footer
        }

        // --- header: Back · title/counts · search ---
        Grid header;
        header.Margin(Thickness{ 16, 10, 16, 8 });
        {
            const auto hcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                header.ColumnDefinitions().Append(c);
            };
            hcol(0, GridUnitType::Auto); // back
            hcol(1, GridUnitType::Star); // title/counts
            hcol(0, GridUnitType::Auto); // search
        }
        Button back;
        back.Content(winrt::box_value(winrt::hstring{ L"\x2190  Back" }));
        back.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) { _HideArchivePage(); });
        Grid::SetColumn(back, 0);
        header.Children().Append(back);

        StackPanel titleStack;
        titleStack.Margin(Thickness{ 14, 0, 0, 0 });
        titleStack.VerticalAlignment(VerticalAlignment::Center);
        titleStack.Children().Append(ArchiveText(L"Archive", 18, true, 1.0));
        _archiveCountText = ArchiveText(L"", 12, false, 0.6);
        titleStack.Children().Append(_archiveCountText);
        Grid::SetColumn(titleStack, 1);
        header.Children().Append(titleStack);

        TextBox search;
        search.PlaceholderText(L"Search title, dir, branch\x2026");
        search.Width(260);
        search.VerticalAlignment(VerticalAlignment::Center);
        _archiveSearchBox = search;
        search.TextChanged([this](const winrt::Windows::Foundation::IInspectable& s, const TextChangedEventArgs&) {
            if (const auto tb = s.try_as<TextBox>())
            {
                _archiveFilter = ArchiveLower(std::wstring{ tb.Text() });
                // Agentmaster: DEBOUNCE the rebuild — a synchronous _RenderArchiveTable here rebuilt the
                // entire table (header + every row) AND re-showed the detail pane on EVERY keystroke;
                // the trailing throttle collapses a burst of typing into one rebuild.
                if (_archiveFilterThrottled)
                {
                    _archiveFilterThrottled->Run();
                }
                else
                {
                    _RenderArchiveTable();
                }
            }
        });
        Grid::SetColumn(search, 2);
        header.Children().Append(search);
        Grid::SetRow(header, 0);
        host.Children().Append(header);

        // --- body: 50/50 table | detail, divided by a thin separator ---
        Grid body;
        body.Margin(Thickness{ 16, 0, 16, 0 });
        {
            const auto bcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                body.ColumnDefinitions().Append(c);
            };
            bcol(1, GridUnitType::Star); // table
            bcol(0, GridUnitType::Auto); // separator
            bcol(1, GridUnitType::Star); // detail
        }

        // LEFT — sortable header over a scrolling rows host.
        Grid left;
        {
            const auto lrow = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                left.RowDefinitions().Append(r);
            };
            lrow(0, GridUnitType::Auto); // column header
            lrow(1, GridUnitType::Star); // rows
        }
        _archiveHeaderRow = Grid{};
        Grid::SetRow(_archiveHeaderRow, 0);
        left.Children().Append(_archiveHeaderRow);
        _archiveRowsHost = StackPanel{};
        _archiveRowsHost.Spacing(2);
        ScrollViewer leftScroll;
        leftScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        leftScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        leftScroll.Padding(Thickness{ 0, 0, 8, 0 });
        leftScroll.Content(_archiveRowsHost);
        Grid::SetRow(leftScroll, 1);
        left.Children().Append(leftScroll);
        Grid::SetColumn(left, 0);
        body.Children().Append(left);

        Border sep;
        sep.Width(1);
        sep.Background(ArchiveBrush(0x30, 0xC0, 0xC0, 0xC0));
        sep.Margin(Thickness{ 10, 4, 10, 4 });
        Grid::SetColumn(sep, 1);
        body.Children().Append(sep);

        // RIGHT — detail/preview.
        _archiveDetailHost = StackPanel{};
        _archiveDetailHost.Spacing(6);
        ScrollViewer rightScroll;
        rightScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        rightScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        rightScroll.Padding(Thickness{ 12, 0, 4, 0 });
        rightScroll.Content(_archiveDetailHost);
        Grid::SetColumn(rightScroll, 2);
        body.Children().Append(rightScroll);

        Grid::SetRow(body, 1);
        host.Children().Append(body);

        // --- footer: bulk restore (right-aligned) ---
        StackPanel footer;
        footer.Orientation(Orientation::Horizontal);
        footer.HorizontalAlignment(HorizontalAlignment::Right);
        footer.Spacing(8);
        footer.Margin(Thickness{ 16, 10, 16, 10 });
        _archiveRestoreSelBtn = Button{};
        _archiveRestoreSelBtn.Content(winrt::box_value(winrt::hstring{ L"Restore selected" }));
        _archiveRestoreSelBtn.IsEnabled(false);
        _archiveRestoreSelBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            // Agentmaster: DEFER — _RestoreCheckedArchived loops _RestoreArchivedSession (creates N tabs).
            // Mutating the tree synchronously inside this Click is the page's documented crash class.
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    self->_RestoreCheckedArchived();
                }
            });
        });
        footer.Children().Append(_archiveRestoreSelBtn);
        Grid::SetRow(footer, 2);
        host.Children().Append(footer);

        // Mount over Root's CONTENT rows (1-2: the InfoBar area + TabContent), NOT row 0 (the
        // titlebar / tab strip). Covering row 0 put an opaque element over WT's titlebar input region;
        // toggling it during the very click that opened it crashed XAML's pointer/hit-test path — an AV
        // READ of 0x1d5 (null + field offset) deep in the ninput.dll -> Windows.UI.dll ->
        // Windows.UI.Xaml.dll input stack (pinned from the crash dump). The CommandPalette + ContentDialogs
        // live at row 2 and toggle safely during input; we match that. So the page covers every pane
        // (everything below the tabs); the tab strip + min/max/close caption buttons stay visible + usable.
        this->Root().Children().Append(host);
        Grid::SetRow(host, 1);
        Grid::SetRowSpan(host, 2);
        _archivePageHost = host;

        // Agentmaster: the search-filter debounce (see the TextChanged handler above) — 200ms trailing,
        // so a typing burst rebuilds once instead of per keystroke.
        _archiveFilterThrottled = std::make_shared<ThrottledFunc<>>(
            winrt::Windows::System::DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 200 },
                .debounce = true,
                .trailing = true,
            },
            [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    self->_RenderArchiveTable();
                }
            });

        // Agentmaster: keep an OPEN page LIVE. The registry changes under it (a tab ✕ archives a session
        // while the page is showing, another window restores one, the ~2s liveness sweep archives a dead
        // claude) — the retired in-content overlay re-listed on every registry change while visible; the
        // page didn't, so it sat stale (and a row restored elsewhere even kept counting in the bulk
        // button). The observer fires on ARBITRARY threads → pre-filter on the atomic visibility mirror
        // (XAML properties are UI-thread-only), bounce to the dispatcher, and let a trailing throttle
        // collapse hook-storms into one re-gather. Token detached in ~TerminalPage (Rule #10).
        _archiveRefreshThrottled = std::make_shared<ThrottledFunc<>>(
            winrt::Windows::System::DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 400 },
                .debounce = true,
                .trailing = true,
            },
            [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    self->_RefreshArchivePageIfVisible();
                }
            });
        if (_sessionRegistry && !_archiveRegistryObserverToken)
        {
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _archiveRegistryObserverToken = _sessionRegistry->AddObserver(
                [weak = get_weak(), dispatcher](const ::Agentmaster::SessionInfo&, ::Agentmaster::HookEvent) {
                    const auto self = weak.get();
                    if (!self || !self->_archivePageVisible.load(std::memory_order_relaxed))
                    {
                        return; // page closed — don't even post
                    }
                    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weak]() {
                        if (auto s = weak.get(); s && s->_archiveRefreshThrottled)
                        {
                            s->_archiveRefreshThrottled->Run();
                        }
                    });
                });
        }
    }

    void TerminalPage::_ShowArchivePage()
    {
        // DEFER the build/show OFF the current input event. This is invoked from the Archived button's
        // Click, i.e. while that pointer event is still being ROUTED to the button. Restructuring the
        // visual tree there — appending the page to Root and toggling an opaque full-bleed element
        // Visible — left XAML routing the in-flight pointer against a tree that changed under it and
        // dereferencing a stale/null target: AV READ null+offset at Windows.UI.Xaml.dll+0x16344d, deep
        // in the ninput.dll -> Windows.UI.dll -> Windows.UI.Xaml.dll pointer-routing stack (pinned from
        // two crash dumps; identical whether mounted over the titlebar row or the content rows). Posting
        // to the dispatcher lets the click finish routing against the OLD tree; the page then builds +
        // appears on a clean tick — the same reason ContentDialog.ShowAsync + the CommandPalette defer.
        Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak()]() {
            auto self = weak.get();
            if (!self)
            {
                return;
            }
            self->_BuildArchivePageShell();
            if (!self->_archivePageHost)
            {
                return;
            }
            self->_GatherArchiveRows();
            self->_RenderArchiveTable();
            self->_archivePageHost.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
            self->_archivePageVisible.store(true, std::memory_order_relaxed); // observer pre-filter mirror
        });
    }

    void TerminalPage::_HideArchivePage()
    {
        if (!_archivePageHost)
        {
            return;
        }
        // Defer: this is called from Back / Restore here / Reopen — buttons INSIDE the page. Collapsing
        // the page synchronously would remove the clicked button from the tree mid-click (the same XAML
        // hit-test AV as the open/row crashes). Collapse on a clean tick instead.
        Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak()]() {
            auto self = weak.get();
            if (self && self->_archivePageHost)
            {
                self->_archivePageHost.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
                self->_archivePageVisible.store(false, std::memory_order_relaxed); // observer pre-filter mirror
            }
        });
    }

    // Gather the archive data set: every saved-window record's archived Claude sessions (tagged with that
    // window's index + a 1-based display ordinal), then the loose archived sessions (in no record). Each
    // row's timing comes from a cheap transcript stat (TranscriptTimes). Reads disk -> called on show +
    // after an action, NOT per keystroke (the filter/sort in _RenderArchiveTable run over this cache).
    void TerminalPage::_GatherArchiveRows()
    {
        _archiveRows.clear();
        if (!_sessionRegistry)
        {
            return;
        }
        const auto sessions = _sessionRegistry->Snapshot();
        std::vector<::Agentmaster::RecoverableWindow> recoverable;
        try
        {
            recoverable = ::Agentmaster::RecoverableWindows();
        }
        CATCH_LOG();

        // Agentmaster: index the archived sessions by id ONCE (O(n)) instead of a linear scan per window tab
        // (the old findArchived was O(tabs · sessions)). Snapshot() ids are unique (keyed off _order), so
        // a flat map is exact; `sessions` outlives `archivedById`, so the borrowed pointers stay valid.
        std::unordered_map<std::wstring, const ::Agentmaster::SessionInfo*> archivedById;
        archivedById.reserve(sessions.size());
        for (const auto& s : sessions)
        {
            if (!s.live)
            {
                archivedById.emplace(s.id, &s);
            }
        }
        const auto findArchived = [&archivedById](const std::wstring& id) -> const ::Agentmaster::SessionInfo* {
            const auto it = archivedById.find(id);
            return it != archivedById.end() ? it->second : nullptr;
        };
        const auto buildRow = [](const ::Agentmaster::SessionInfo& s, int windowIndex, int windowOrdinal) {
            _ArchiveRow r;
            r.id = s.id;
            r.title = s.title;
            r.dir = s.workingDir;
            r.branch = s.branch;
            r.windowIndex = windowIndex;
            r.windowOrdinal = windowOrdinal;
            for (const auto& p : s.queue)
            {
                ++r.totalCount;
                if (p.status == ::Agentmaster::PromptStatus::Sent)
                {
                    ++r.sentCount;
                }
            }
            int64_t created = 0, last = 0;
            if (::Agentmaster::TranscriptTimes(s.workingDir, s.id, created, last))
            {
                r.createdUnixMs = created;
                r.lastActivityUnixMs = last;
            }
            return r;
        };

        std::unordered_set<std::wstring> grouped;
        int ordinal = 0;
        for (const auto& rw : recoverable)
        {
            // Collect this record's still-archived Claude sessions, CLAIMING each into `grouped` as we go
            // one session id can appear in more than one record's tab refs (e.g. it was
            // restored from window A into window B, and both records persist) — claim-on-collect attributes
            // it to the FIRST window only, so it never yields two rows / two "W{n}" chips. Every recoverable
            // record is REPRESENTED (session rows, or the synthetic window-only row below), so W{n} is the
            // record's position in the recoverable order.
            std::vector<const ::Agentmaster::SessionInfo*> winSessions;
            int claudeRefs = 0, shellRefs = 0; // the record's tab composition (drives the synthetic row + its detail)
            for (const auto& t : rw.record.tabs)
            {
                if (t.kind == ::Agentmaster::TabKind::Claude)
                {
                    ++claudeRefs;
                    if (!t.sessionId.empty() && grouped.find(t.sessionId) == grouped.end())
                    {
                        if (const auto* hit = findArchived(t.sessionId))
                        {
                            winSessions.push_back(hit);
                            grouped.insert(t.sessionId);
                        }
                    }
                }
                else
                {
                    ++shellRefs;
                }
            }
            ++ordinal;
            // The chip tooltip — composition + geometry — once per record; stamped on every row below.
            const std::wstring wtip = ArchiveWindowTip(ordinal, claudeRefs, shellRefs, rw.record.geometry);
            if (winSessions.empty())
            {
                // Agentmaster: a recoverable window with NO archived Claude sessions (a pure-shell
                // workspace, or its sessions are live/restored elsewhere) was previously skipped — i.e.
                // INVISIBLE and un-reopenable from the very page billed as the grouped archive (only the
                // toolbar's "Reopen Windows (N)" covered it, with a count this page then seemed to
                // contradict). Represent it as a synthetic, checkbox-less "saved window" row; its detail
                // offers "Reopen its window". The id is a sentinel ("window:<guid>") that can never
                // collide with a session UUID, so every session-only path (restore, checks) no-ops on it.
                _ArchiveRow r;
                r.id = L"window:" + rw.record.windowId;
                r.windowOnly = true;
                r.windowIndex = rw.index;
                r.windowId = rw.record.windowId;
                r.windowOrdinal = ordinal;
                r.winClaudeTabs = claudeRefs;
                r.winShellTabs = shellRefs;
                r.windowTip = wtip;
                std::wstring rowTitle = L"Saved window";
                if (claudeRefs > 0)
                {
                    rowTitle += L" \x00B7 " + std::to_wstring(claudeRefs) + L" claude";
                }
                if (shellRefs > 0)
                {
                    rowTitle += L" \x00B7 " + std::to_wstring(shellRefs) + (shellRefs == 1 ? L" shell tab" : L" shell tabs");
                }
                if (claudeRefs == 0 && shellRefs == 0)
                {
                    rowTitle += L" \x00B7 empty";
                }
                r.title = std::move(rowTitle);
                ArchiveWindowRecordTimes(rw.record.windowId, r.createdUnixMs, r.lastActivityUnixMs);
                _archiveRows.push_back(std::move(r));
                continue;
            }
            for (const auto* s : winSessions)
            {
                _archiveRows.push_back(buildRow(*s, rw.index, ordinal));
                _archiveRows.back().windowId = rw.record.windowId; // Agentmaster: reopen re-resolves the live index from this
                _archiveRows.back().windowTip = wtip;
            }
        }
        for (const auto& s : sessions)
        {
            if (s.live || grouped.find(s.id) != grouped.end())
            {
                continue;
            }
            _archiveRows.push_back(buildRow(s, -1, 0));
        }

        // Agentmaster (branch backfill): SessionInfo.branch had NO live writer — only the JSON loader —
        // so the Branch column + the branch term of the search were permanently empty for every session
        // this app ever created (the detail pane masked it via its per-row transcript fallback). Backfill
        // it lazily: head-read each archived row's transcript AT MOST ONCE per run, off-thread (the
        // gitBranch rides the first user line), write back via the registry + ONE save, then poke the
        // open page so the cells fill in. Gated on a known transcript (createdUnixMs from the stat above)
        // — a never-prompted session has nothing to read.
        std::vector<std::pair<std::wstring, std::wstring>> needBranch; // (id, dir)
        for (const auto& r : _archiveRows)
        {
            if (!r.windowOnly && r.branch.empty() && r.createdUnixMs > 0 &&
                _archiveBranchBackfilled.insert(r.id).second)
            {
                needBranch.emplace_back(r.id, r.dir);
            }
        }
        if (!needBranch.empty())
        {
            _BackfillArchiveBranches(std::move(needBranch));
        }

        // Agentmaster: drop checks for sessions that are no longer archived (restored / became live / removed)
        // so a stale check can't bulk-restore a gone or already-open session, and the button count stays
        // honest. (_RestoreArchivedSession also guards on !live, but pruning keeps the selection set clean.)
        if (!_archiveChecked.empty())
        {
            std::unordered_set<std::wstring> present;
            for (const auto& r : _archiveRows)
            {
                present.insert(r.id);
            }
            for (auto it = _archiveChecked.begin(); it != _archiveChecked.end();)
            {
                it = (present.find(*it) != present.end()) ? std::next(it) : _archiveChecked.erase(it);
            }
        }
    }

    // Apply the search filter + the active sort to _archiveRows and (re)build the column header + data
    // rows. Also keeps the selection valid (defaults to the first row) and drives the detail pane.
    void TerminalPage::_RenderArchiveTable()
    {
        if (!_archiveRowsHost || !_archiveHeaderRow)
        {
            return;
        }
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        const int64_t now = ArchiveNowMs();

        // --- sortable column header ---
        _archiveHeaderRow.Children().Clear();
        _archiveHeaderRow.ColumnDefinitions().Clear();
        ArchiveAddColumns(_archiveHeaderRow);
        _archiveHeaderRow.Margin(Thickness{ 8, 0, 8, 4 });
        const auto addHeader = [this](int col, winrt::hstring label, bool sortable) {
            if (!sortable)
            {
                auto t = ArchiveText(label, 11, true, 0.5);
                Grid::SetColumn(t, col);
                _archiveHeaderRow.Children().Append(t);
                return;
            }
            winrt::hstring arrow{};
            if (_archiveSortColumn == col)
            {
                arrow = _archiveSortAscending ? winrt::hstring{ L" \x25B2" } : winrt::hstring{ L" \x25BC" };
            }
            Button b;
            b.Background(ArchiveBrush(0, 0, 0, 0));
            b.BorderThickness(Thickness{ 0, 0, 0, 0 });
            b.Padding(Thickness{ 0, 0, 0, 0 });
            b.MinWidth(0);
            b.MinHeight(0);
            const bool leftAlign = (col == 1 || col == 2); // Title + Directory read left; the rest center
            b.HorizontalAlignment(HorizontalAlignment::Stretch);
            b.HorizontalContentAlignment(leftAlign ? HorizontalAlignment::Left : HorizontalAlignment::Center);
            b.Content(ArchiveText(label + arrow, 11, true, 0.7));
            b.Click([this, col](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                // Defer: rebuilding the header here would destroy this very sort button mid-click (XAML
                // hit-test AV). Let the click finish routing, then re-sort + rebuild on a clean tick.
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), col]() {
                    auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    if (self->_archiveSortColumn == col)
                    {
                        self->_archiveSortAscending = !self->_archiveSortAscending;
                    }
                    else
                    {
                        self->_archiveSortColumn = col;
                        self->_archiveSortAscending = (col == 1 || col == 2 || col == 3); // text ascending, time/window descending
                    }
                    self->_RenderArchiveTable();
                });
            });
            Grid::SetColumn(b, col);
            _archiveHeaderRow.Children().Append(b);
        };
        addHeader(0, L"", false);
        addHeader(1, L"Title", true);
        addHeader(2, L"Directory", true);
        addHeader(3, L"Branch", true);
        addHeader(4, L"Created", true);
        addHeader(5, L"Active", true);
        addHeader(6, L"Plan", true);
        addHeader(7, L"Window", true);

        // --- filter + sort over the gathered rows ---
        std::vector<const _ArchiveRow*> view;
        for (const auto& r : _archiveRows)
        {
            if (!_archiveFilter.empty())
            {
                // Agentmaster: include a slash-flipped twin of the dir so "k:/source" matches "k:\source"
                // (and vice versa) — dirs are filesystem paths, not plain text (Rule #8 spirit). Branch
                // stays verbatim ('/' is meaningful in "feature/x", so we never mangle the filter itself).
                const std::wstring dirLow = ArchiveLower(r.dir);
                std::wstring dirAlt = dirLow;
                for (auto& ch : dirAlt)
                {
                    ch = (ch == L'\\') ? L'/' : (ch == L'/' ? L'\\' : ch);
                }
                const std::wstring hay = ArchiveLower(r.title) + L"\n" + dirLow + L"\n" + dirAlt + L"\n" + ArchiveLower(r.branch);
                if (hay.find(_archiveFilter) == std::wstring::npos)
                {
                    continue;
                }
            }
            view.push_back(&r);
        }
        const int sortCol = _archiveSortColumn;
        const bool asc = _archiveSortAscending;
        std::sort(view.begin(), view.end(), [sortCol, asc](const _ArchiveRow* a, const _ArchiveRow* b) {
            const auto cmpS = [](const std::wstring& x, const std::wstring& y) {
                const auto lx = ArchiveLower(x), ly = ArchiveLower(y);
                return lx < ly ? -1 : (lx > ly ? 1 : 0);
            };
            const auto cmpI = [](int64_t x, int64_t y) { return x < y ? -1 : (x > y ? 1 : 0); };
            int c = 0;
            switch (sortCol)
            {
            case 1:
                c = cmpS(a->title, b->title);
                break;
            case 2:
                c = cmpS(a->dir, b->dir);
                break;
            case 3:
                c = cmpS(a->branch, b->branch);
                break;
            case 4:
                c = cmpI(a->createdUnixMs, b->createdUnixMs);
                break;
            case 5:
                c = cmpI(a->lastActivityUnixMs, b->lastActivityUnixMs);
                break;
            case 6: // plan: by queue size, then by how much of it was sent
                c = cmpI(a->totalCount, b->totalCount);
                if (c == 0)
                {
                    c = cmpI(a->sentCount, b->sentCount);
                }
                break;
            case 7:
                c = cmpI(a->windowOrdinal, b->windowOrdinal);
                break;
            default:
                break;
            }
            if (c == 0)
            {
                return a->createdUnixMs > b->createdUnixMs; // stable tiebreak: newest first
            }
            return asc ? (c < 0) : (c > 0);
        });

        // Agentmaster: snapshot the ids currently visible (passing the filter) so the bulk-restore button's
        // "(N)" count + _RestoreCheckedArchived operate on checked ∩ visible only — a check hidden by a
        // later search must never be silently bulk-restored. _archiveVisibleOrder keeps them in TABLE
        // order too, so a bulk restore opens tabs in display order (iterating the unordered_set restored
        // them in hash order).
        _archiveVisibleIds.clear();
        _archiveVisibleOrder.clear();
        for (const auto* r : view)
        {
            _archiveVisibleIds.insert(r->id);
            _archiveVisibleOrder.push_back(r->id);
        }

        // --- keep a valid selection (default to the first visible row) ---
        bool selValid = false;
        for (const auto* r : view)
        {
            if (r->id == _archiveSelectedId)
            {
                selValid = true;
                break;
            }
        }
        if (!selValid)
        {
            _archiveSelectedId = view.empty() ? std::wstring{} : view.front()->id;
        }

        // --- data rows ---
        _archiveRowsHost.Children().Clear();
        if (view.empty())
        {
            _archiveRowsHost.Children().Append(ArchiveText(_archiveRows.empty() ?
                                                               winrt::hstring{ L"No archived sessions. Closing a session's tab archives it here." } :
                                                               winrt::hstring{ L"No matches." },
                                                           12, false, 0.6, true));
        }
        for (const auto* rp : view)
        {
            const _ArchiveRow& r = *rp;
            const std::wstring rid = r.id;
            Grid g;
            ArchiveAddColumns(g);

            if (!r.windowOnly) // a synthetic "saved window" row has no session to bulk-restore — no checkbox
            {
                CheckBox cb;
                cb.MinWidth(0);
                cb.VerticalAlignment(VerticalAlignment::Center);
                cb.HorizontalAlignment(HorizontalAlignment::Center);
                cb.IsChecked(_archiveChecked.find(r.id) != _archiveChecked.end()); // set BEFORE handlers (no spurious fire)
                cb.Checked([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    _archiveChecked.insert(rid);
                    _UpdateArchiveBulkButton();
                });
                cb.Unchecked([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    _archiveChecked.erase(rid);
                    _UpdateArchiveBulkButton();
                });
                Grid::SetColumn(cb, 0);
                g.Children().Append(cb);
            }

            // Tooltips throughout: every cell either truncates (CharacterEllipsis on Title/Dir/Branch)
            // or abbreviates ("3h", "3/7", "W2") — hover carries the full value / exact moment.
            auto title = ArchiveText(r.title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ r.title }, 13, true, r.windowOnly ? 0.7 : 0.95);
            title.Margin(Thickness{ 2, 0, 6, 0 });
            ArchiveSetTip(title, r.title);
            Grid::SetColumn(title, 1);
            g.Children().Append(title);
            auto dir = ArchiveText(winrt::hstring{ r.dir }, 12, false, 0.6);
            dir.Margin(Thickness{ 0, 0, 6, 0 });
            ArchiveSetTip(dir, r.dir); // the full path — the cell end-trims, losing the leaf
            Grid::SetColumn(dir, 2);
            g.Children().Append(dir);
            auto br = ArchiveText(winrt::hstring{ r.branch }, 12, false, 0.55);
            br.HorizontalAlignment(HorizontalAlignment::Center);
            ArchiveSetTip(br, r.branch);
            Grid::SetColumn(br, 3);
            g.Children().Append(br);
            auto cr = ArchiveText(winrt::hstring{ ArchiveAgo(r.createdUnixMs, now) }, 11, false, 0.6);
            cr.HorizontalAlignment(HorizontalAlignment::Center);
            ArchiveSetTip(cr, ArchiveLocalDateTime(r.createdUnixMs)); // absolute local datetime behind the relative age
            Grid::SetColumn(cr, 4);
            g.Children().Append(cr);
            auto la = ArchiveText(winrt::hstring{ ArchiveAgo(r.lastActivityUnixMs, now) }, 11, false, 0.6);
            la.HorizontalAlignment(HorizontalAlignment::Center);
            ArchiveSetTip(la, ArchiveLocalDateTime(r.lastActivityUnixMs));
            Grid::SetColumn(la, 5);
            g.Children().Append(la);
            {
                // Plan progress "sent/total" — gathered since the redesign but never rendered (the retired
                // overlay showed "⚙ 3/7 prompts"); an en-dash for a session with no recorded plan.
                const bool hasPlan = r.totalCount > 0;
                auto pl = ArchiveText(hasPlan ?
                                          winrt::hstring{ std::to_wstring(r.sentCount) + L"/" + std::to_wstring(r.totalCount) } :
                                          winrt::hstring{ L"\x2013" },
                                      11,
                                      false,
                                      hasPlan ? 0.6 : 0.3);
                pl.HorizontalAlignment(HorizontalAlignment::Center);
                if (hasPlan)
                {
                    ArchiveSetTip(pl, std::to_wstring(r.sentCount) + L" of " + std::to_wstring(r.totalCount) + L" prompts sent");
                }
                Grid::SetColumn(pl, 6);
                g.Children().Append(pl);
            }
            if (r.windowOrdinal > 0)
            {
                Border chip;
                chip.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush{ ArchiveWindowColor(r.windowOrdinal) });
                chip.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 3, 3, 3, 3 });
                chip.Padding(Thickness{ 5, 1, 5, 1 });
                chip.HorizontalAlignment(HorizontalAlignment::Center);
                chip.VerticalAlignment(VerticalAlignment::Center);
                auto wt = ArchiveText(winrt::hstring{ L"W" + std::to_wstring(r.windowOrdinal) }, 10, true, 1.0);
                chip.Child(wt);
                // What W{n} IS — the chip alone said "W2" and nothing on the page said what that was.
                ArchiveSetTip(chip, r.windowTip);
                Grid::SetColumn(chip, 7);
                g.Children().Append(chip);
            }

            Border row;
            row.Padding(Thickness{ 8, 5, 8, 5 });
            row.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            row.Background(r.id == _archiveSelectedId ? ArchiveBrush(0x50, 0x4A, 0x6E, 0xA8) : ArchiveBrush(0x14, 0x80, 0x80, 0x80));
            row.Tag(winrt::box_value(winrt::hstring{ rid })); // id, so _UpdateArchiveSelectionHighlight can recolor without a rebuild
            row.Child(g);
            // Selecting a row must NOT rebuild the list synchronously here: this Tapped is mid-routing on
            // the row, and clearing _archiveRowsHost would destroy the very element handling the event ->
            // the XAML hit-test AV (the crash the user hit clicking a row). Defer to a clean tick, and only
            // recolor the highlight (a property change) + refresh the detail pane — no structural change to
            // the tapped row.
            row.Tapped([this, rid](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), rid]() {
                    auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    self->_archiveSelectedId = rid;
                    self->_UpdateArchiveSelectionHighlight();
                    self->_ShowArchiveDetail(rid);
                });
            });
            _archiveRowsHost.Children().Append(row);
        }

        // --- header counts ---
        if (_archiveCountText)
        {
            // Agentmaster: when a filter is active, report what's SHOWN ("K of N archived sessions") and count the
            // saved windows over the VISIBLE rows — so the summary tracks the table instead of always citing
            // the full gathered set (which made "12 archived sessions" sit above a 1-row filtered view).
            std::unordered_set<int> ws;
            for (const auto* r : view)
            {
                if (r->windowOrdinal > 0)
                {
                    ws.insert(r->windowOrdinal);
                }
            }
            // Session counts EXCLUDE the synthetic window-only rows (they are windows, not sessions —
            // they already count via `ws`, which now reflects every represented saved window).
            size_t total = 0;
            for (const auto& r : _archiveRows)
            {
                if (!r.windowOnly)
                {
                    ++total;
                }
            }
            size_t shown = 0;
            for (const auto* r : view)
            {
                if (!r->windowOnly)
                {
                    ++shown;
                }
            }
            std::wstring counts;
            if (!_archiveFilter.empty() && shown != total)
            {
                counts = std::to_wstring(shown) + L" of " + std::to_wstring(total) + L" archived sessions";
            }
            else
            {
                counts = std::to_wstring(total) + (total == 1 ? L" archived session" : L" archived sessions");
            }
            if (!ws.empty())
            {
                counts += L"  \x00B7  " + std::to_wstring(ws.size()) + (ws.size() == 1 ? L" saved window" : L" saved windows");
            }
            _archiveCountText.Text(winrt::hstring{ counts });
        }

        _UpdateArchiveBulkButton();
        _ShowArchiveDetail(_archiveSelectedId);
    }

    // Recolor the row backgrounds to reflect _archiveSelectedId WITHOUT rebuilding the list (each row
    // Border carries its session id as its Tag). A property change only, so it never restructures the
    // tree under an in-flight pointer — safe to run from the deferred row-tap reaction.
    void TerminalPage::_UpdateArchiveSelectionHighlight()
    {
        if (!_archiveRowsHost)
        {
            return;
        }
        using namespace winrt::Windows::UI::Xaml::Controls;
        for (const auto& child : _archiveRowsHost.Children())
        {
            const auto border = child.try_as<Border>();
            if (!border)
            {
                continue;
            }
            const std::wstring id{ winrt::unbox_value_or<winrt::hstring>(border.Tag(), L"") };
            border.Background(id == _archiveSelectedId ? ArchiveBrush(0x50, 0x4A, 0x6E, 0xA8) : ArchiveBrush(0x14, 0x80, 0x80, 0x80));
        }
    }

    // Populate the right pane for one archived session: metadata + a read-only Flight Plan (the persisted
    // queue; the transcript's human prompts as a fallback) + restore actions.
    void TerminalPage::_ShowArchiveDetail(const std::wstring& id)
    {
        if (!_archiveDetailHost)
        {
            return;
        }
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        _archiveDetailHost.Children().Clear();
        if (id.empty() || !_sessionRegistry)
        {
            _archiveDetailHost.Children().Append(ArchiveText(L"Select a session to preview its details and Flight Plan.", 12, false, 0.6, true));
            return;
        }
        const _ArchiveRow* row = nullptr;
        for (const auto& r : _archiveRows)
        {
            if (r.id == id)
            {
                row = &r;
                break;
            }
        }
        const int64_t now = ArchiveNowMs();

        // Shared "Reopen its window" builder — both the session detail and the synthetic window-only
        // detail offer the whole-window reopen with the same stale-index re-resolution.
        const auto makeReopenButton = [this](int fallbackIdx, std::wstring wid) {
            Button reopen;
            reopen.Content(winrt::box_value(winrt::hstring{ L"Reopen its window" }));
            reopen.Click([this, fallbackIdx, wid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                // Agentmaster: DEFER — run on a clean tick like the page's other handlers (collapsing/launching
                // off the in-flight pointer event is the crash class; see "Restore here" below).
                // Agentmaster: the captured windowIndex is the record's slot in the sorted set AT GATHER TIME; if
                // the record set shifted while the page was open (a background window closed, or a record was
                // pruned), that slot now names a DIFFERENT window. Re-resolve the live index from the stable
                // windowId against a fresh RecoverableWindows() here. idx<0 (no longer recoverable — already
                // reopened, or deleted) makes _ReopenSavedWindow a safe no-op. (A residual sub-ms cross-process
                // race remains: the spawned agentmaster.exe re-loads records for `-s <idx>`; the full fix is to
                // pass the windowId on the command line — a larger M10-Increment-3 change.)
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), fallbackIdx, wid]() {
                    auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    int idx = fallbackIdx;
                    if (!wid.empty())
                    {
                        idx = -1; // not found -> no-op (don't fall back to a possibly-stale slot)
                        try
                        {
                            for (const auto& rw : ::Agentmaster::RecoverableWindows())
                            {
                                if (rw.record.windowId == wid)
                                {
                                    idx = rw.index;
                                    break;
                                }
                            }
                        }
                        CATCH_LOG();
                        if (idx < 0)
                        {
                            ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] window " + wid + L" no longer recoverable (already open or pruned)\n");
                        }
                    }
                    self->_ReopenSavedWindow(idx);
                    self->_HideArchivePage();
                });
            });
            return reopen;
        };

        // Agentmaster: a synthetic "saved window" row (no archived sessions — id "window:<guid>"): there
        // is no session record to Get(); show the window's tab composition + record timing, and offer
        // the whole-window reopen.
        if (row && row->windowOnly)
        {
            _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ L"Saved window W" + std::to_wstring(row->windowOrdinal) }, 18, true, 1.0, true));
            const std::wstring comp = std::to_wstring(row->winClaudeTabs) + (row->winClaudeTabs == 1 ? L" Claude tab" : L" Claude tabs") +
                                      L" \x00B7 " + std::to_wstring(row->winShellTabs) + (row->winShellTabs == 1 ? L" shell tab" : L" shell tabs");
            _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ comp }, 12, false, 0.7, true));
            std::wstring wmeta;
            if (row->createdUnixMs)
            {
                wmeta += L"saved " + ArchiveAgoPhrase(row->createdUnixMs, now);
            }
            if (row->lastActivityUnixMs)
            {
                wmeta += (wmeta.empty() ? std::wstring{} : std::wstring{ L"   \x00B7   " }) + L"last updated " + ArchiveAgoPhrase(row->lastActivityUnixMs, now);
            }
            if (!wmeta.empty())
            {
                auto wmetaTb = ArchiveText(winrt::hstring{ wmeta }, 12, false, 0.6, true);
                // Hover = the exact moments behind the relative phrases.
                std::wstring wtipAbs;
                if (const auto a = ArchiveLocalDateTime(row->createdUnixMs); !a.empty())
                {
                    wtipAbs += L"saved " + a;
                }
                if (const auto a = ArchiveLocalDateTime(row->lastActivityUnixMs); !a.empty())
                {
                    wtipAbs += (wtipAbs.empty() ? std::wstring{} : std::wstring{ L"  \x00B7  " }) + L"last updated " + a;
                }
                ArchiveSetTip(wmetaTb, wtipAbs);
                _archiveDetailHost.Children().Append(wmetaTb);
            }
            {
                Border d;
                d.Height(1);
                d.Background(ArchiveBrush(0x24, 0xC0, 0xC0, 0xC0));
                d.Margin(Thickness{ 0, 8, 0, 4 });
                _archiveDetailHost.Children().Append(d);
            }
            _archiveDetailHost.Children().Append(ArchiveText(L"None of this window's Claude sessions are archived \x2014 reopening restores the whole workspace (geometry + tabs) as a new window.", 12, false, 0.6, true));
            StackPanel actions;
            actions.Orientation(Orientation::Horizontal);
            actions.Spacing(8);
            actions.Margin(Thickness{ 0, 8, 0, 0 });
            actions.Children().Append(makeReopenButton(row->windowIndex, row->windowId));
            _archiveDetailHost.Children().Append(actions);
            return;
        }

        const auto info = _sessionRegistry->Get(id);
        if (!info)
        {
            _archiveDetailHost.Children().Append(ArchiveText(L"This session is no longer available.", 12, false, 0.6, true));
            return;
        }

        // Out-of-band transcript read (branch + prompts + authoritative timing) — CACHED by (id, mtime).
        // _RenderArchiveTable re-shows the detail on every rebuild (search keystrokes, the registry-
        // observer refresh), and the uncached path was a synchronous <=128 KB UI-thread read + parse
        // EACH time. Stat first (cheap); re-read only when the id or the transcript mtime changed.
        {
            int64_t statCreated = 0, statM = 0;
            ::Agentmaster::TranscriptTimes(info->workingDir, id, statCreated, statM);
            if (id != _archiveDetailTiId || statM != _archiveDetailTiMtime)
            {
                ::Agentmaster::TranscriptInfo fresh{};
                try
                {
                    fresh = ::Agentmaster::ReadTranscriptInfo(info->workingDir, id, 131072, 60);
                }
                CATCH_LOG();
                _archiveDetailTiId = id;
                _archiveDetailTiMtime = statM;
                _archiveDetailTiCreated = fresh.createdUnixMs;
                _archiveDetailTiLast = fresh.lastActivityUnixMs;
                _archiveDetailTiBranch = fresh.gitBranch;
                _archiveDetailTiPrompts = std::move(fresh.userPrompts);
            }
        }

        _archiveDetailHost.Children().Append(ArchiveText(info->title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ info->title }, 18, true, 1.0, true));
        _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ info->workingDir }, 12, false, 0.7, true));

        const int64_t created = _archiveDetailTiCreated ? _archiveDetailTiCreated : (row ? row->createdUnixMs : 0);
        const int64_t last = _archiveDetailTiLast ? _archiveDetailTiLast : (row ? row->lastActivityUnixMs : 0);
        const std::wstring branch = !info->branch.empty() ? info->branch : _archiveDetailTiBranch;
        std::wstring meta;
        if (!branch.empty())
        {
            meta += L"\x2387 " + branch + L"    ";
        }
        if (created)
        {
            meta += L"created " + ArchiveAgoPhrase(created, now); // "just now" / "5m ago" ("created now ago" read broken)
        }
        if (last)
        {
            meta += (created ? std::wstring{ L"   \x00B7   " } : std::wstring{}) + L"last active " + ArchiveAgoPhrase(last, now);
        }
        if (!meta.empty())
        {
            auto metaTb = ArchiveText(winrt::hstring{ meta }, 12, false, 0.6, true);
            // Hover = the exact moments behind the relative phrases.
            std::wstring metaTip;
            if (const auto a = ArchiveLocalDateTime(created); !a.empty())
            {
                metaTip += L"created " + a;
            }
            if (const auto a = ArchiveLocalDateTime(last); !a.empty())
            {
                metaTip += (metaTip.empty() ? std::wstring{} : std::wstring{ L"  \x00B7  " }) + L"last active " + a;
            }
            ArchiveSetTip(metaTb, metaTip);
            _archiveDetailHost.Children().Append(metaTb);
        }

        {
            Border d;
            d.Height(1);
            d.Background(ArchiveBrush(0x24, 0xC0, 0xC0, 0xC0));
            d.Margin(Thickness{ 0, 8, 0, 4 });
            _archiveDetailHost.Children().Append(d);
        }

        _archiveDetailHost.Children().Append(ArchiveText(L"Flight Plan (read-only)", 13, true, 0.9));
        if (!info->queue.empty())
        {
            for (const auto& p : info->queue)
            {
                winrt::hstring tag;
                switch (p.status)
                {
                case ::Agentmaster::PromptStatus::Sent:
                    tag = L"\x2713 ";
                    break;
                case ::Agentmaster::PromptStatus::Held:
                    tag = L"\x23F8 ";
                    break;
                case ::Agentmaster::PromptStatus::Failed:
                    tag = L"\x2717 ";
                    break;
                case ::Agentmaster::PromptStatus::Skipped:
                    tag = L"\x2014 ";
                    break;
                default:
                    tag = L"\x2022 ";
                    break;
                }
                const std::wstring bodyText = !p.label.empty() ? std::wstring{ p.label } : std::wstring{ p.text };
                const winrt::hstring suffix = (p.origin == ::Agentmaster::PromptOrigin::Typed) ? winrt::hstring{ L"   (typed)" } : winrt::hstring{};
                _archiveDetailHost.Children().Append(ArchiveText(tag + winrt::hstring{ bodyText } + suffix, 12, false, 0.8, true));
            }
        }
        else if (!_archiveDetailTiPrompts.empty())
        {
            for (const auto& up : _archiveDetailTiPrompts)
            {
                _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ L"\x2023 " + up }, 12, false, 0.75, true));
            }
        }
        else
        {
            _archiveDetailHost.Children().Append(ArchiveText(L"(no recorded prompts)", 12, false, 0.5, true));
        }

        {
            Border d;
            d.Height(1);
            d.Background(ArchiveBrush(0x24, 0xC0, 0xC0, 0xC0));
            d.Margin(Thickness{ 0, 10, 0, 6 });
            _archiveDetailHost.Children().Append(d);
        }
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(8);
        Button restore;
        restore.Content(winrt::box_value(winrt::hstring{ L"Restore here" }));
        const winrt::hstring hid{ id };
        restore.Click([this, hid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            // Agentmaster: DEFER — _RestoreArchivedSession creates a tab (mutates the visual tree). Doing that
            // synchronously inside this button's Click (while the pointer event is still routing) is the
            // exact class that crashed this page 3× (XAML hit-test AV). Run it on a clean tick like the
            // page's other handlers; _HideArchivePage already defers too.
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), hid]() {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                self->_RestoreArchivedSession(hid);
                self->_HideArchivePage();
            });
        });
        actions.Children().Append(restore);
        if (row && row->windowIndex >= 0)
        {
            actions.Children().Append(makeReopenButton(row->windowIndex, row->windowId));
        }
        _archiveDetailHost.Children().Append(actions);
    }

    // Bulk: restore every checked archived session into the current window, then close the page.
    void TerminalPage::_RestoreCheckedArchived()
    {
        if (_archiveChecked.empty())
        {
            return;
        }
        // Agentmaster: restore ONLY checked rows that are currently visible (pass the filter) — a check hidden
        // by a later search must not be silently bulk-restored. Leave any hidden checks intact (they
        // restore once the filter reveals them again). _RestoreArchivedSession additionally guards on
        // !live, so an id that became open/removed since checking is a safe no-op. Walk
        // _archiveVisibleOrder (the TABLE order), not _archiveChecked — iterating the unordered_set
        // restored the tabs in hash order, so they opened jumbled relative to the list the user checked.
        std::vector<std::wstring> ids;
        for (const auto& id : _archiveVisibleOrder)
        {
            if (_archiveChecked.find(id) != _archiveChecked.end())
            {
                ids.push_back(id);
            }
        }
        if (ids.empty())
        {
            return;
        }
        for (const auto& id : ids)
        {
            _RestoreArchivedSession(winrt::hstring{ id });
            _archiveChecked.erase(id);
        }
        _HideArchivePage();
    }

    void TerminalPage::_UpdateArchiveBulkButton()
    {
        if (!_archiveRestoreSelBtn)
        {
            return;
        }
        // Agentmaster: count only checked rows that are currently VISIBLE (pass the filter), so the button's
        // "(N)" agrees with what _RestoreCheckedArchived will actually restore.
        size_t n = 0;
        for (const auto& id : _archiveChecked)
        {
            if (_archiveVisibleIds.find(id) != _archiveVisibleIds.end())
            {
                ++n;
            }
        }
        _archiveRestoreSelBtn.Content(winrt::box_value(n > 0 ?
                                                           winrt::hstring{ L"Restore selected (" + std::to_wstring(n) + L")" } :
                                                           winrt::hstring{ L"Restore selected" }));
        _archiveRestoreSelBtn.IsEnabled(n > 0);
    }

    // Agentmaster: re-gather + re-render an OPEN page. The poke behind the registry-observer refresh
    // (a session archived/restored/renamed anywhere while the page is showing) and the branch
    // backfill's completion. UI thread, clean tick (both callers arrive via the dispatcher).
    void TerminalPage::_RefreshArchivePageIfVisible()
    {
        if (!_archivePageHost || _archivePageHost.Visibility() != winrt::Windows::UI::Xaml::Visibility::Visible)
        {
            return;
        }
        _GatherArchiveRows();
        _RenderArchiveTable();
    }

    // Agentmaster (branch backfill): head-read each (id, dir)'s transcript OFF-THREAD for its gitBranch
    // (it rides the first user line — 64 KB is ample) and write it back through the registry's QUIET
    // path: the engine's persistence observer saves sessions.json on EVERY notify, so N notifying
    // Updates would mean N full-document writes — instead quiet-update them all and save ONCE. The
    // quiet path fires no observer, so poke the open page ourselves once back on the UI thread.
    winrt::fire_and_forget TerminalPage::_BackfillArchiveBranches(std::vector<std::pair<std::wstring, std::wstring>> idDirs)
    {
        const auto weakThis = get_weak();
        const auto registry = _sessionRegistry; // strong — the engine outlives any window
        const auto dispatcher = Dispatcher();
        if (!registry)
        {
            co_return;
        }
        co_await winrt::resume_background();
        bool any = false;
        for (const auto& [id, dir] : idDirs)
        {
            ::Agentmaster::TranscriptInfo ti{};
            try
            {
                ti = ::Agentmaster::ReadTranscriptInfo(dir, id, 65536, 1);
            }
            CATCH_LOG();
            if (ti.gitBranch.empty())
            {
                continue; // no branch recorded (non-git dir) — the attempted-set keeps us from re-reading every gather
            }
            registry->UpdateQuiet(id, [&ti](::Agentmaster::SessionInfo& s) {
                if (s.branch.empty())
                {
                    s.branch = ti.gitBranch;
                }
            });
            any = true;
        }
        if (!any)
        {
            co_return;
        }
        try
        {
            ::Agentmaster::SaveSessions(registry->Snapshot());
        }
        CATCH_LOG();
        co_await wil::resume_foreground(dispatcher);
        if (auto self = weakThis.get())
        {
            self->_RefreshArchivePageIfVisible(); // fill the Branch cells in
        }
    }
}
