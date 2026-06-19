// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the full-window Sessions page (SESSIONS.md): a browser over EVERY on-disk
// Claude Code session in a selectable time window (default 1 month), opened by the Manager's
// "Sessions" button (right after Archived). Duplicates the Archive page's structure with a
// SEARCH BAR at the top: [ search for sessions ] (👤)(🤖)(📁)(📄)(F) [1 month] —
//   👤 = also search user (typed) messages      🤖 = also search agent + tools text
//   📁 = match directories accessed             📄 = match files accessed
//   (F) = fuzzy   ·   both message scopes OFF ⇒ title + directory only (§1a)
//   Defaults: 📁+📄 ON (fast-phase-only — in-memory, no IO); 👤/🤖/(F) OFF (either message
//   scope flips on the SLOW rg+transcript content scan; fuzzy is a noisy default).
//   [1 month] cycles 1d/3d/7d/14d/1mo/3mo on click; HOVER opens a From/To range popup (Q4).
// Query grammar (ParseSessionQuery, SessionSearch.h): whitespace-split terms AND-match;
// "quoted phrase" = exact contiguous match ((F) never applies inside quotes); a bare whole
// session-id GUID matches that session + its forks by IDENTITY (paste from hooks.log works).
// Backed by the TranscriptStore sidecar index (~/.agentmaster/sessions-index/<sid>.json,
// (size,mtime)-invalidated, incrementally accumulated) and the two-phase SessionSearch:
// FAST = in-memory over the index + the history.jsonl accelerator; SLOW = rg-prefiltered
// transcript content, scope-attributed in-process, generation-cancelled on re-type.
// Rows are enriched from the registry (an OPEN session gets its per-dir color chip + Jump)
// and the observer's presence table (claude's own busy/idle/waiting heartbeat).
// XAML-Islands hard rule throughout (the Archive page's discipline): every pointer handler
// DEFERS its visual-tree mutation to a clean dispatcher tick.
//
// This file implements TerminalPage methods (same class, separate TU — the TabManagement.cpp
// pattern).

#include "pch.h"
#include "TerminalPage.h"

#include "AgentTipHelpers.h" // AgentSetTip / AgentCloseTipsIn — the shared tooltip-dismissal recipe
#include "AgentMaster/ClaudeSpawn.h" // ClaudeProjectsDir / AppendStateLog
#include "AgentMaster/Persistence.h" // GetDirColor / AutoDirColorHex (the per-dir color chip)
#include "AgentMaster/ProcessInspect.h" // AnalyzeSessionTranscript / RenderSessionSummaryBox / FindPlanFileInTranscript / kSummarySepMark (detail summary box)
#include "AgentMaster/ProcessObserver.h" // Presence()
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionSearch.h" // the two-phase search
#include "AgentMaster/TranscriptStore.h" // EnumerateTranscripts / LoadOrRefreshSessionIndex / PickDisplayTitle

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace std::chrono_literals;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        int64_t SessNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        SolidColorBrush SessBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
        {
            return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
        }

        TextBlock SessText(const winrt::hstring& text, double size, bool bold, double opacity)
        {
            TextBlock t;
            t.Text(text);
            t.FontSize(size);
            if (bold)
            {
                t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            t.Opacity(opacity);
            t.TextTrimming(TextTrimming::CharacterEllipsis);
            t.VerticalAlignment(VerticalAlignment::Center);
            return t;
        }

        // Render a session-summary box (RenderSessionSummaryBox output, full=true) into `host`: split
        // on '\n'; a lone kSummarySepMark sentinel line becomes a full-width rule (border to border),
        // every other run becomes a monospace, wrapped, selectable TextBlock — mirroring the overlay
        // panel's _SetSummaryContent so the Sessions detail and the per-tab summary panel look the same.
        void SessAppendSummaryBox(StackPanel host, const std::wstring& text)
        {
            std::wstring seg;
            const auto flush = [&]() {
                if (seg.empty())
                {
                    return;
                }
                TextBlock tb{};
                tb.FontFamily(FontFamily{ L"Cascadia Mono" });
                tb.FontSize(11);
                tb.TextWrapping(TextWrapping::Wrap);
                tb.IsTextSelectionEnabled(true);
                tb.Opacity(0.85);
                tb.Text(winrt::hstring{ seg });
                host.Children().Append(tb);
                seg.clear();
            };
            size_t i = 0;
            while (i <= text.size())
            {
                const size_t nl = text.find(L'\n', i);
                const size_t end = (nl == std::wstring::npos) ? text.size() : nl;
                const std::wstring lineStr = text.substr(i, end - i);
                if (lineStr.size() == 1 && lineStr[0] == ::Agentmaster::kSummarySepMark)
                {
                    flush(); // close the run above the rule
                    Border rule{};
                    rule.Height(1);
                    rule.HorizontalAlignment(HorizontalAlignment::Stretch); // border to border
                    rule.Background(SessBrush(0x40, 0xFF, 0xFF, 0xFF));
                    rule.Margin(Thickness{ 0, 4, 0, 4 });
                    host.Children().Append(rule);
                }
                else
                {
                    if (!seg.empty())
                    {
                        seg += L"\n";
                    }
                    seg += lineStr;
                }
                if (nl == std::wstring::npos)
                {
                    break;
                }
                i = nl + 1;
            }
            flush();
        }

        // "now" / "5m" / "3h" / "2d" / "3mo" / "1y" (the Archive page's compact-ago shape).
        std::wstring SessAgo(int64_t unixMs, int64_t nowMs)
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

        // Absolute local datetime ("2026-06-10 14:32") for the Created/Active hover tooltips — the
        // cells show only the compact relative age ("3h" / "2d"), which answers "how long ago?" but
        // not "which day was that?". "" when unknown (0) so the tooltip helper no-ops (mirrors the
        // Archive page's ArchiveLocalDateTime).
        std::wstring SessLocalDateTime(int64_t unixMs)
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

        // "#RRGGBB" -> Color (the dir-color chip). nullopt on anything malformed.
        std::optional<Color> SessHexToColor(const std::wstring& hex)
        {
            if (hex.size() != 7 || hex[0] != L'#')
            {
                return std::nullopt;
            }
            auto nib = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9')
                {
                    return c - L'0';
                }
                if (c >= L'a' && c <= L'f')
                {
                    return 10 + (c - L'a');
                }
                if (c >= L'A' && c <= L'F')
                {
                    return 10 + (c - L'A');
                }
                return -1;
            };
            int v[6];
            for (int i = 0; i < 6; ++i)
            {
                v[i] = nib(hex[1 + i]);
                if (v[i] < 0)
                {
                    return std::nullopt;
                }
            }
            return ColorHelper::FromArgb(0xFF,
                                         static_cast<uint8_t>(v[0] * 16 + v[1]),
                                         static_cast<uint8_t>(v[2] * 16 + v[3]),
                                         static_cast<uint8_t>(v[4] * 16 + v[5]));
        }

        // The window presets the [1 month] button cycles through (answer Q1/spec).
        struct SessPreset
        {
            const wchar_t* label;
            int days;
        };
        constexpr SessPreset kSessPresets[] = {
            { L"1 day", 1 }, { L"3 days", 3 }, { L"7 days", 7 }, { L"14 days", 14 }, { L"1 month", 30 }, { L"3 months", 90 }
        };
        constexpr int kSessPresetCount = static_cast<int>(sizeof(kSessPresets) / sizeof(kSessPresets[0]));

        // `<claude home>\history.jsonl` — the 👤 accelerator's path (sibling of projects\).
        std::wstring SessHistoryPath()
        {
            std::wstring projects = ::Agentmaster::ClaudeProjectsDir();
            while (!projects.empty() && (projects.back() == L'\\' || projects.back() == L'/'))
            {
                projects.pop_back();
            }
            const size_t cut = projects.find_last_of(L"\\/");
            return cut == std::wstring::npos ? std::wstring{} : projects.substr(0, cut) + L"\\history.jsonl";
        }

        // The page's table columns. 0=color/live chip · 1=Title · 2=Directory · 3=Branch ·
        // 4=Created · 5=Active · 6=Msgs·Tools · 7=Hits (populated while searching).
        void SessAddColumns(Grid& g)
        {
            const auto col = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                g.ColumnDefinitions().Append(c);
            };
            col(26, GridUnitType::Pixel); // chip
            col(2.2, GridUnitType::Star); // title
            col(1.6, GridUnitType::Star); // directory
            col(0.8, GridUnitType::Star); // branch
            col(58, GridUnitType::Pixel); // created
            col(58, GridUnitType::Pixel); // active
            col(74, GridUnitType::Pixel); // msgs·tools
            col(48, GridUnitType::Pixel); // hits
        }

        // Hover tooltips with working dismissal — TU-local names over the ONE shared recipe
        // (AgentTipHelpers.h: an explicit ToolTip closed on the owner's PointerExited +
        // Unloaded; the sweep walks Panel/Border/Popup/ContentControl — ScrollViewer is a
        // ContentControl, and the rows/detail hosts live inside ScrollViewers).
        void SessSetTip(const UIElement& el, const winrt::hstring& tip)
        {
            AgentSetTip(el, tip);
        }

        // Force-close every tooltip under root — for hosts about to Clear() or be HIDDEN
        // (a Visibility toggle doesn't unload; a collapsed host does not hide a popup).
        void SessCloseTipsIn(const UIElement& root)
        {
            AgentCloseTipsIn(root);
        }

        // Build the toggle buttons of the search bar: a compact glyph ToggleButton with a tooltip.
        Primitives::ToggleButton SessToggle(const winrt::hstring& glyph, const winrt::hstring& tip)
        {
            Primitives::ToggleButton b;
            b.Content(winrt::box_value(glyph));
            b.Padding(Thickness{ 6, 2, 6, 2 });
            b.MinWidth(0);
            b.MinHeight(0);
            SessSetTip(b, tip);
            return b;
        }

        // "YYYY-MM-DD" (trimmed) -> unix ms via the store's ISO parser; 0 on anything else.
        int64_t SessParseDateBox(const winrt::hstring& text)
        {
            std::wstring t{ text };
            const auto notWs = [](wchar_t c) { return c != L' ' && c != L'\t'; };
            while (!t.empty() && !notWs(t.front()))
            {
                t.erase(t.begin());
            }
            while (!t.empty() && !notWs(t.back()))
            {
                t.pop_back();
            }
            if (t.size() != 10)
            {
                return 0;
            }
            return ::Agentmaster::ParseTranscriptTimestamp(t + L"T00:00:00Z");
        }
    }

    // Build the page shell ONCE — host + search-bar header + table/detail split — mounted over
    // Root's CONTENT rows (1-2), exactly like the Archive page (covering the titlebar row crashes
    // XAML input routing; see _BuildArchivePageShell's mount note).
    void TerminalPage::_BuildSessionsPageShell()
    {
        if (_sessionsPageHost)
        {
            return;
        }

        Grid host;
        host.Background(SessBrush(0xFF, 0x1B, 0x1B, 0x1B));
        host.RequestedTheme(ElementTheme::Dark);
        host.Visibility(Visibility::Collapsed);
        {
            const auto row = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                host.RowDefinitions().Append(r);
            };
            row(0, GridUnitType::Auto); // 0 header (Back · title · search bar)
            row(1, GridUnitType::Star); // 1 body (table | detail)
        }

        // --- header: Back · title/count · [search](👤)(🤖)(📁)(📄)(F)[window] ---
        Grid header;
        header.Margin(Thickness{ 16, 10, 16, 8 });
        {
            const auto hcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                header.ColumnDefinitions().Append(c);
            };
            hcol(0, GridUnitType::Auto); // back
            hcol(0, GridUnitType::Auto); // title/count — sized to content so the search bar sits right after it
            hcol(0, GridUnitType::Auto); // the search bar cluster — now left-aligned, next to the title
            hcol(1, GridUnitType::Star); // trailing spacer absorbs the rest, keeping the cluster on the LEFT
        }
        Button back;
        back.Content(winrt::box_value(winrt::hstring{ L"\x2190  Back" }));
        back.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) { _HideSessionsPage(); });
        Grid::SetColumn(back, 0);
        header.Children().Append(back);

        StackPanel titleStack;
        titleStack.Margin(Thickness{ 14, 0, 0, 0 });
        titleStack.VerticalAlignment(VerticalAlignment::Center);
        titleStack.Children().Append(SessText(L"Claude Code Sessions", 18, true, 1.0));
        _sessionsCountText = SessText(L"", 12, false, 0.6);
        titleStack.Children().Append(_sessionsCountText);
        Grid::SetColumn(titleStack, 1);
        header.Children().Append(titleStack);

        StackPanel bar;
        bar.Orientation(Orientation::Horizontal);
        bar.Spacing(6);
        bar.VerticalAlignment(VerticalAlignment::Center);
        bar.Margin(Thickness{ 18, 0, 0, 0 }); // gap from the title (the cluster now sits next to it, not at the right edge)

        TextBox search;
        search.PlaceholderText(L"search for sessions");
        search.Width(240);
        search.VerticalAlignment(VerticalAlignment::Center);
        SessSetTip(search, L"Filter the list \x2014 every word must match (each may match a different field) \x00B7 \"quoted phrase\" = exact match \x00B7 paste a session-id GUID to find that session and its forks");
        _sessionsSearchBox = search;
        search.TextChanged([this](const winrt::Windows::Foundation::IInspectable& s, const TextChangedEventArgs&) {
            if (const auto tb = s.try_as<TextBox>())
            {
                _sessionsQueryText = std::wstring{ tb.Text() };
                if (_sessionsSearchThrottled)
                {
                    _sessionsSearchThrottled->Run(); // debounce a typing burst into one search
                }
            }
        });
        bar.Children().Append(search);

        // The scope toggles. A toggle flip re-runs the search (deferred through the same throttle).
        const auto onToggle = [this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            if (_sessionsSearchThrottled)
            {
                _sessionsSearchThrottled->Run();
            }
        };
        // Defaults: 📁/📄 ON — they ride the FAST phase only (in-memory match over the sidecar
        // index's pathsAccessed: no rg, no transcript IO — effectively free at the debounce),
        // so path queries "just work". 👤/🤖 OFF — either one flips on the SLOW phase (rg across
        // every transcript in the window + in-process rescans per search; 🤖 is the heaviest:
        // tool dumps raw-match almost any query, so the prefilter passes most files). (F) OFF —
        // a semantics toggle (subsequence matching is noisy as a default, and its `.*?`-joined
        // rg patterns inflate the slow phase's candidate set). IsChecked is set BEFORE Click is
        // wired — and programmatic IsChecked never raises Click anyway (no spurious search).
        _sessScopeUserBtn = SessToggle(L"\U0001F464", L"Also match inside user messages \x2014 the prompts you typed. Scans transcript text (slower).");
        _sessScopeUserBtn.Click(onToggle);
        bar.Children().Append(_sessScopeUserBtn);
        _sessScopeAgentBtn = SessToggle(L"\U0001F916", L"Also match inside Claude's replies and tool calls/results \x2014 everything except your messages. Scans transcript text (slower).");
        _sessScopeAgentBtn.Click(onToggle);
        bar.Children().Append(_sessScopeAgentBtn);
        _sessScopeDirsBtn = SessToggle(L"\U0001F4C1", L"Match the directories a session worked in \x2014 its working dir plus folders its tools touched. On by default (instant, no transcript scan).");
        _sessScopeDirsBtn.IsChecked(true);
        _sessScopeDirsBtn.Click(onToggle);
        bar.Children().Append(_sessScopeDirsBtn);
        _sessScopeFilesBtn = SessToggle(L"\U0001F4C4", L"Match the files a session read or edited (tool-call paths). On by default (instant, no transcript scan).");
        _sessScopeFilesBtn.IsChecked(true);
        _sessScopeFilesBtn.Click(onToggle);
        bar.Children().Append(_sessScopeFilesBtn);
        _sessFuzzyBtn = SessToggle(L"F", L"Fuzzy matching \x2014 the query's characters must appear in order, with gaps allowed (\"agmst\" matches \"agentmaster\").");
        _sessFuzzyBtn.Click(onToggle);
        bar.Children().Append(_sessFuzzyBtn);

        // [1 month] — click cycles the presets; hover opens the From/To range popup (Q4).
        _sessWindowBtn = Button{};
        _sessWindowBtn.Content(winrt::box_value(winrt::hstring{ kSessPresets[_sessionsWindowPreset].label }));
        SessSetTip(_sessWindowBtn, L"Time window \x2014 only list sessions active within this span. Click to cycle 1d \x2192 3d \x2192 7d \x2192 14d \x2192 1mo \x2192 3mo \x00B7 hover to set a custom From/To range.");
        _sessWindowBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            // Defer — the cycle re-gathers + re-renders the table (tree mutation).
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    self->_CycleSessionsWindow();
                }
            });
        });
        _sessWindowBtn.PointerEntered([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
            if (!_sessRangePopup || !_sessWindowBtn || !_sessionsPageHost)
            {
                return;
            }
            // Anchor the popup under the window button's CURRENT position (root-relative). The header
            // cluster moved from the right edge to just after the title, so a fixed offset no longer
            // points at the button — read its live top-left within the host instead.
            const auto pt = _sessWindowBtn.TransformToVisual(_sessionsPageHost).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
            _sessRangePopup.HorizontalOffset(pt.X);
            _sessRangePopup.VerticalOffset(pt.Y + _sessWindowBtn.ActualHeight() + 4);
            _sessRangePopup.IsOpen(true);
        });
        bar.Children().Append(_sessWindowBtn);

        // ↻ Refresh — re-run the same gather pass that opening the page does: re-enumerate the
        // window's transcripts and load-or-refresh every sidecar index, so a session created (or
        // grown) since the page opened shows up and its new content folds into the search index.
        // The _sessionsIndexing flag dedupes against an in-flight pass, so a double-click is safe.
        _sessRefreshBtn = Button{};
        _sessRefreshBtn.Content(winrt::box_value(winrt::hstring{ L"\x21BB" }));
        SessSetTip(_sessRefreshBtn, L"Refresh \x2014 rescan the folder for new or changed sessions and re-index them.");
        _sessRefreshBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            // Defer off the click tick (the page's pointer-handler discipline); the gather itself
            // runs on a background pass and re-renders when it lands.
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    self->_RefreshSessionsRows();
                }
            });
        });
        bar.Children().Append(_sessRefreshBtn);

        Grid::SetColumn(bar, 2);
        header.Children().Append(bar);
        Grid::SetRow(header, 0);
        host.Children().Append(header);

        // --- the hover range popup: From/To date boxes + Apply (answer Q4: text boxes). Parented
        // into the page host (top-right aligned under the header) — main-tree, so typing works
        // (the XAML-Islands ContentDialog keyboard trap). ---
        {
            StackPanel card;
            card.Background(SessBrush(0xFF, 0x26, 0x26, 0x26));
            card.BorderBrush(SessBrush(0xFF, 0x3A, 0x3A, 0x3A));
            card.BorderThickness(Thickness{ 1, 1, 1, 1 });
            card.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 6, 6, 6, 6 });
            card.Padding(Thickness{ 10, 8, 10, 8 });
            card.Spacing(6);
            card.Children().Append(SessText(L"Range (overrides the preset)", 11, true, 0.7));
            _sessFromBox = TextBox{};
            _sessFromBox.PlaceholderText(L"from: 2026-05-10");
            _sessFromBox.Width(180);
            card.Children().Append(_sessFromBox);
            _sessToBox = TextBox{};
            _sessToBox.PlaceholderText(L"to: 2026-06-10 (empty = now)");
            _sessToBox.Width(180);
            card.Children().Append(_sessToBox);
            StackPanel actions;
            actions.Orientation(Orientation::Horizontal);
            actions.Spacing(6);
            Button apply;
            apply.Content(winrt::box_value(winrt::hstring{ L"Apply" }));
            apply.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                    if (auto self = weak.get())
                    {
                        self->_ApplySessionsRange();
                    }
                });
            });
            actions.Children().Append(apply);
            Button clear;
            clear.Content(winrt::box_value(winrt::hstring{ L"Preset" }));
            SessSetTip(clear, L"Clear the custom range and go back to the time-window preset.");
            clear.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                    if (auto self = weak.get())
                    {
                        self->_sessionsFromMs = 0;
                        self->_sessionsToMs = 0;
                        if (self->_sessWindowBtn)
                        {
                            self->_sessWindowBtn.Content(winrt::box_value(winrt::hstring{ kSessPresets[self->_sessionsWindowPreset].label }));
                        }
                        if (self->_sessRangePopup)
                        {
                            self->_sessRangePopup.IsOpen(false);
                        }
                        self->_RefreshSessionsRows();
                    }
                });
            });
            actions.Children().Append(clear);
            card.Children().Append(actions);
            card.PointerExited([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
                if (_sessRangePopup)
                {
                    _sessRangePopup.IsOpen(false); // leave the card -> dismiss (Apply/Preset close it too)
                }
            });

            _sessRangePopup = Primitives::Popup{};
            _sessRangePopup.Child(card);
            // Top/left aligned so HorizontalOffset/VerticalOffset are root-relative (the path-picker
            // Popup recipe). The hover handler positions it under the window button's CURRENT spot via
            // TransformToVisual — the header cluster is left-aligned now, not pinned to the right edge.
            _sessRangePopup.HorizontalAlignment(HorizontalAlignment::Left);
            _sessRangePopup.HorizontalOffset(0);
            _sessRangePopup.VerticalOffset(52);
            Grid::SetRow(_sessRangePopup, 0);
            Grid::SetRowSpan(_sessRangePopup, 2);
            host.Children().Append(_sessRangePopup);
        }

        // --- body: table | detail (fixed 60/40 split — the draggable splitter is the Archive
        // page's polish; this page reuses the simpler static divider for v1). ---
        Grid body;
        body.Margin(Thickness{ 16, 0, 16, 12 });
        {
            const auto bcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                body.ColumnDefinitions().Append(c);
            };
            bcol(0.6, GridUnitType::Star);
            bcol(0, GridUnitType::Auto);
            bcol(0.4, GridUnitType::Star);
        }

        Grid left;
        {
            const auto lrow = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                left.RowDefinitions().Append(r);
            };
            lrow(0, GridUnitType::Auto);
            lrow(1, GridUnitType::Star);
        }
        _sessionsHeaderRow = Grid{};
        Grid::SetRow(_sessionsHeaderRow, 0);
        left.Children().Append(_sessionsHeaderRow);
        _sessionsRowsHost = StackPanel{};
        _sessionsRowsHost.Spacing(2);
        ScrollViewer leftScroll;
        leftScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        leftScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        leftScroll.Padding(Thickness{ 0, 0, 8, 0 });
        leftScroll.Content(_sessionsRowsHost);
        Grid::SetRow(leftScroll, 1);
        left.Children().Append(leftScroll);
        Grid::SetColumn(left, 0);
        body.Children().Append(left);

        Border divider;
        divider.Width(1);
        divider.Margin(Thickness{ 8, 10, 8, 10 });
        divider.Background(SessBrush(0x40, 0x80, 0x80, 0x80));
        Grid::SetColumn(divider, 1);
        body.Children().Append(divider);

        _sessionsDetailHost = StackPanel{};
        _sessionsDetailHost.Spacing(6);
        ScrollViewer rightScroll;
        rightScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        rightScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        rightScroll.Padding(Thickness{ 4, 0, 4, 0 });
        rightScroll.Content(_sessionsDetailHost);
        Grid::SetColumn(rightScroll, 2);
        body.Children().Append(rightScroll);

        Grid::SetRow(body, 1);
        host.Children().Append(body);

        this->Root().Children().Append(host);
        Grid::SetRow(host, 1);
        Grid::SetRowSpan(host, 2);
        _sessionsPageHost = host;

        // Generic overlay registration: the tab-switch seam dismisses every registered page
        // (TabManagement.cpp) — the extra hook closes the range Popup, which a collapsed host
        // would NOT hide (popups render in the popup root, not under the parent).
        _RegisterAgentPageOverlay(host, &_sessionsPageVisible, [weak = get_weak()]() {
            if (const auto self = weak.get())
            {
                if (self->_sessRangePopup)
                {
                    self->_sessRangePopup.IsOpen(false);
                }
                if (self->_sessionsPageHost)
                {
                    SessCloseTipsIn(self->_sessionsPageHost); // tooltips don't collapse with the host either
                }
            }
        });

        // Up/Down = move the selection through the visible rows (wraps; none selected => Down
        // picks the first, Up the last). PREVIEW (tunneling) so it wins over the focused search
        // box; the move itself is deferred to a clean tick (the page's mutation discipline —
        // _ShowSessionsDetail rebuilds the detail pane).
        host.PreviewKeyDown([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
            const auto k = e.Key();
            if (k != winrt::Windows::System::VirtualKey::Up && k != winrt::Windows::System::VirtualKey::Down)
            {
                return;
            }
            e.Handled(true);
            const int delta = (k == winrt::Windows::System::VirtualKey::Down) ? 1 : -1;
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), delta]() {
                if (auto self = weak.get())
                {
                    self->_MoveSessionsSelection(delta);
                }
            });
        });

        // Keystroke/toggle debounce -> one search per burst (the Archive page's throttle shape).
        _sessionsSearchThrottled = std::make_shared<ThrottledFunc<>>(
            winrt::Windows::System::DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 250 },
                .debounce = true,
                .trailing = true,
            },
            [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    self->_RunSessionsSearch();
                }
            });
    }

    void TerminalPage::_ShowSessionsPage()
    {
        // DEFER the build/show off the opening click (the Archive page's pinned crash class:
        // restructuring the tree while the pointer event is still routing AVs the hit-test).
        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
            auto self = weak.get();
            if (!self)
            {
                return;
            }
            self->_BuildSessionsPageShell();
            if (!self->_sessionsPageHost)
            {
                return;
            }
            self->_sessionsPageHost.Visibility(Visibility::Visible);
            self->_sessionsPageVisible.store(true, std::memory_order_relaxed);
            if (self->_sessionsSearchBox)
            {
                // Focus INTO the page so keyboard events route through its host (a covered
                // element behind the page would otherwise keep them on a sibling branch) —
                // typing searches immediately, Up/Down navigate the rows.
                self->_sessionsSearchBox.Focus(FocusState::Programmatic);
            }
            self->_RefreshSessionsRows(); // gather + index on a background pass, then render
        });
    }

    void TerminalPage::_HideSessionsPage()
    {
        if (!_sessionsPageHost)
        {
            return;
        }
        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
            auto self = weak.get();
            if (self && self->_sessionsPageHost)
            {
                // Tooltips are popups too — collapsing the host does NOT hide an open one
                // (the same popup-root reason the range popup is closed explicitly here).
                SessCloseTipsIn(self->_sessionsPageHost);
                self->_sessionsPageHost.Visibility(Visibility::Collapsed);
                self->_sessionsPageVisible.store(false, std::memory_order_relaxed);
                if (self->_sessRangePopup)
                {
                    self->_sessRangePopup.IsOpen(false);
                }
            }
        });
    }

    int64_t TerminalPage::_SessionsCutoffFromMs() const
    {
        if (_sessionsFromMs > 0)
        {
            return _sessionsFromMs; // custom range wins
        }
        const int days = kSessPresets[std::clamp(_sessionsWindowPreset, 0, kSessPresetCount - 1)].days;
        return SessNowMs() - static_cast<int64_t>(days) * 86400000LL;
    }

    void TerminalPage::_CycleSessionsWindow()
    {
        _sessionsFromMs = 0; // a preset click drops any custom range
        _sessionsToMs = 0;
        _sessionsWindowPreset = (_sessionsWindowPreset + 1) % kSessPresetCount;
        if (_sessWindowBtn)
        {
            _sessWindowBtn.Content(winrt::box_value(winrt::hstring{ kSessPresets[_sessionsWindowPreset].label }));
        }
        if (_sessRangePopup)
        {
            _sessRangePopup.IsOpen(false);
        }
        _RefreshSessionsRows();
    }

    void TerminalPage::_ApplySessionsRange()
    {
        if (!_sessFromBox || !_sessToBox)
        {
            return;
        }
        const int64_t from = SessParseDateBox(_sessFromBox.Text());
        int64_t to = SessParseDateBox(_sessToBox.Text());
        if (to > 0)
        {
            to += 86400000LL - 1; // inclusive end-of-day
        }
        if (from <= 0 && to <= 0)
        {
            return; // nothing parseable — keep the current window
        }
        _sessionsFromMs = from > 0 ? from : 1; // 1 = "explicit range, open start"
        _sessionsToMs = to;
        if (_sessWindowBtn)
        {
            _sessWindowBtn.Content(winrt::box_value(winrt::hstring{ L"custom" }));
        }
        if (_sessRangePopup)
        {
            _sessRangePopup.IsOpen(false);
        }
        _RefreshSessionsRows();
    }

    // Gather + index the window's sessions OFF-THREAD, then render. The first-ever pass over a
    // large window builds every sidecar (a full transcript parse — tens of seconds on a cold
    // multi-GB corpus); every later pass is (size,mtime) sidecar hits + incremental suffixes (ms).
    winrt::fire_and_forget TerminalPage::_RefreshSessionsRows()
    {
        if (_sessionsIndexing.exchange(true))
        {
            co_return; // one pass at a time; the runner re-renders when it lands
        }
        auto weakThis{ get_weak() };
        const int64_t fromMs = _SessionsCutoffFromMs();
        const int64_t toMs = _sessionsToMs;
        if (_sessionsCountText)
        {
            _sessionsCountText.Text(L"indexing\x2026");
        }

        co_await winrt::resume_background();

        const auto refs = ::Agentmaster::EnumerateTranscripts(fromMs);
        std::vector<::Agentmaster::SessionIndexEntry> entries;
        entries.reserve(refs.size());
        std::vector<_SessionsRow> rows;
        rows.reserve(refs.size());
        const int64_t now = SessNowMs();
        for (const auto& ref : refs)
        {
            auto e = ::Agentmaster::LoadOrRefreshSessionIndex(ref);
            if (!e.valid)
            {
                continue; // swept mid-listing
            }
            _SessionsRow r;
            r.id = ref.sessionId;
            r.path = ref.path;
            r.title = ::Agentmaster::PickDisplayTitle(e.stats.customTitle, e.stats.aiTitle, e.stats.summary, e.stats.firstUserPrompt);
            if (r.title.empty())
            {
                r.title = L"(no prompt yet)";
            }
            r.dir = !e.stats.cwd.empty() ? e.stats.cwd : ref.projectDirLeaf;
            r.branch = e.stats.gitBranch;
            r.fork = !e.stats.forkedFromId.empty();
            r.forkedFromId = e.stats.forkedFromId;
            // Fork-aware created; line-derived last activity with mtime as the fallback (§5/§6).
            r.createdMs = r.fork ? ref.birthMs : (e.stats.firstTimestampMs != 0 ? e.stats.firstTimestampMs : ref.birthMs);
            r.lastActivityMs = e.stats.lastTimestampMs != 0 ? e.stats.lastTimestampMs : ref.mtimeMs;
            r.sizeBytes = ref.sizeBytes;
            r.msgs = e.stats.userPrompts;
            r.tools = e.stats.toolUses;
            // The custom To bound filters on last activity (the From bound rode the enumeration).
            if (toMs > 0 && r.lastActivityMs > toMs)
            {
                continue;
            }
            rows.push_back(std::move(r));
            entries.push_back(std::move(e));
        }
        (void)now;

        co_await winrt::resume_foreground(Dispatcher());
        auto self = weakThis.get();
        if (!self)
        {
            co_return;
        }
        self->_sessionsIndexing.store(false);
        self->_sessionsRows = std::move(rows);
        self->_sessionsEntries = std::move(entries);
        self->_RunSessionsSearch(); // re-applies the current query (incl. the empty one) + renders
    }

    // The two-phase search. FAST runs inline (in-memory over the index entries); the history
    // accelerator + the SLOW content phase run on a background pass, generation-cancelled.
    winrt::fire_and_forget TerminalPage::_RunSessionsSearch()
    {
        const uint64_t gen = ++_sessionsSearchGen;

        ::Agentmaster::SessionQuery q;
        q.text = _sessionsQueryText;
        q.scopeUser = _sessScopeUserBtn && _sessScopeUserBtn.IsChecked() && _sessScopeUserBtn.IsChecked().Value();
        q.scopeAgent = _sessScopeAgentBtn && _sessScopeAgentBtn.IsChecked() && _sessScopeAgentBtn.IsChecked().Value();
        q.scopeDirs = _sessScopeDirsBtn && _sessScopeDirsBtn.IsChecked() && _sessScopeDirsBtn.IsChecked().Value();
        q.scopeFiles = _sessScopeFilesBtn && _sessScopeFilesBtn.IsChecked() && _sessScopeFilesBtn.IsChecked().Value();
        q.fuzzy = _sessFuzzyBtn && _sessFuzzyBtn.IsChecked() && _sessFuzzyBtn.IsChecked().Value();

        // Phase 1 — fast, inline.
        const auto fastIds = ::Agentmaster::SearchIndexFast(_sessionsEntries, q);
        _sessionsFastIds.clear();
        _sessionsFastIds.insert(fastIds.begin(), fastIds.end());
        _sessionsHitCounts.clear();
        _sessionsHitSnippets.clear();
        _RenderSessionsTable();

        if (q.text.empty() || (!q.scopeUser && !q.scopeAgent))
        {
            co_return; // title/dir/path matching is fully covered by the fast phase (§1a)
        }

        // Phase 2 — background: the history accelerator (👤) + the rg-prefiltered content scan.
        std::vector<::Agentmaster::TranscriptRef> refs;
        refs.reserve(_sessionsEntries.size());
        for (const auto& e : _sessionsEntries)
        {
            ::Agentmaster::TranscriptRef r;
            r.sessionId = e.sessionId;
            r.path = e.path;
            r.sizeBytes = e.sizeBytes;
            r.mtimeMs = e.mtimeMs;
            r.birthMs = e.birthMs;
            refs.push_back(std::move(r));
        }
        auto weakThis{ get_weak() };

        co_await winrt::resume_background();

        const auto cancelled = [weakThis, gen]() {
            const auto s = weakThis.get();
            return !s || s->_sessionsSearchGen.load() != gen;
        };
        std::unordered_map<std::wstring, int> counts;
        std::unordered_map<std::wstring, std::vector<std::wstring>> snippets;
        if (q.scopeUser)
        {
            for (auto& [sid, hit] : ::Agentmaster::SearchHistoryPrompts(SessHistoryPath(), q, 3))
            {
                counts[sid] += hit.hitCount;
                auto& sn = snippets[sid];
                for (auto& s : hit.snippets)
                {
                    if (sn.size() < 3)
                    {
                        sn.push_back(L"\U0001F464 " + s);
                    }
                }
            }
        }
        if (!cancelled())
        {
            for (auto& hit : ::Agentmaster::SearchTranscriptsSlow(refs, q, 3, cancelled))
            {
                counts[hit.sessionId] += hit.hitCount;
                auto& sn = snippets[hit.sessionId];
                for (auto& s : hit.snippets)
                {
                    if (sn.size() < 6)
                    {
                        sn.push_back(std::move(s));
                    }
                }
            }
        }

        co_await winrt::resume_foreground(Dispatcher());
        auto self = weakThis.get();
        if (!self || self->_sessionsSearchGen.load() != gen)
        {
            co_return; // a newer query superseded this pass
        }
        self->_sessionsHitCounts = std::move(counts);
        self->_sessionsHitSnippets = std::move(snippets);
        self->_RenderSessionsTable();
        if (!self->_sessionsSelectedId.empty())
        {
            self->_ShowSessionsDetail(self->_sessionsSelectedId); // refresh the snippets pane
        }
    }

    void TerminalPage::_RenderSessionsTable()
    {
        if (!_sessionsRowsHost || !_sessionsHeaderRow)
        {
            return;
        }
        const int64_t now = SessNowMs();
        const bool searching = !_sessionsQueryText.empty();

        // --- sortable header ---
        _sessionsHeaderRow.Children().Clear();
        _sessionsHeaderRow.ColumnDefinitions().Clear();
        SessAddColumns(_sessionsHeaderRow);
        _sessionsHeaderRow.Margin(Thickness{ 8, 0, 8, 4 });
        const auto addHeader = [this](int col, winrt::hstring label, bool sortable, winrt::hstring tip = L"") {
            if (!sortable)
            {
                auto t = SessText(label, 11, true, 0.5);
                SessSetTip(t, tip);
                Grid::SetColumn(t, col);
                _sessionsHeaderRow.Children().Append(t);
                return;
            }
            winrt::hstring arrow{};
            if (_sessionsSortColumn == col)
            {
                arrow = _sessionsSortAscending ? winrt::hstring{ L" \x25B2" } : winrt::hstring{ L" \x25BC" };
            }
            Button b;
            b.Background(SessBrush(0, 0, 0, 0));
            b.BorderThickness(Thickness{ 0, 0, 0, 0 });
            b.Padding(Thickness{ 0, 0, 0, 0 });
            b.MinWidth(0);
            b.MinHeight(0);
            const bool leftAlign = (col == 1 || col == 2);
            b.HorizontalAlignment(HorizontalAlignment::Stretch);
            b.HorizontalContentAlignment(leftAlign ? HorizontalAlignment::Left : HorizontalAlignment::Center);
            b.Content(SessText(label + arrow, 11, true, 0.7));
            SessSetTip(b, tip);
            b.Click([this, col](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), col]() {
                    auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    if (self->_sessionsSortColumn == col)
                    {
                        self->_sessionsSortAscending = !self->_sessionsSortAscending;
                    }
                    else
                    {
                        self->_sessionsSortColumn = col;
                        self->_sessionsSortAscending = (col == 1 || col == 2 || col == 3); // text asc; time/counts desc
                    }
                    self->_RenderSessionsTable();
                });
            });
            Grid::SetColumn(b, col);
            _sessionsHeaderRow.Children().Append(b);
        };
        addHeader(0, L"", false, L"Working-directory color \x00B7 solid = open now, dim = on disk");
        addHeader(1, L"Title", true, L"Session title \x2014 its first prompt, or a custom/AI title. Click to sort.");
        addHeader(2, L"Directory", true, L"The session's working directory. Click to sort.");
        addHeader(3, L"Branch", true, L"Git branch the session was on. Click to sort.");
        addHeader(4, L"Created", true, L"When the session was first created. Click to sort.");
        addHeader(5, L"Active", true, L"When the session was last active. Click to sort.");
        addHeader(6, L"Msgs\x00B7Tools", true, L"User messages \x00B7 tool calls. Click to sort.");
        addHeader(7, searching ? winrt::hstring{ L"Hits" } : winrt::hstring{ L"" }, false, searching ? winrt::hstring{ L"Number of search matches in this session" } : winrt::hstring{ L"" });

        // --- the visible set: window rows ∩ the current search result (fast ∪ content hits),
        // minus the user's "Hide from list" set. This render is the single chokepoint both the
        // empty-query and search paths flow through, so filtering hidden ids here covers both;
        // resetting from the Settings cog just re-renders (the rows stay in _sessionsRows). A
        // hidden session is untouched on disk — purely a browse-list preference. ---
        const std::unordered_set<std::wstring> hidden(_appSettings.hiddenSessionIds.begin(), _appSettings.hiddenSessionIds.end());
        int hiddenInWindow = 0;
        std::vector<const _SessionsRow*> view;
        for (const auto& r : _sessionsRows)
        {
            if (!hidden.empty() && hidden.count(r.id))
            {
                ++hiddenInWindow;
                continue;
            }
            if (!searching || _sessionsFastIds.count(r.id) || _sessionsHitCounts.count(r.id))
            {
                view.push_back(&r);
            }
        }
        _sessionsVisibleOrder.clear(); // rebuilt below in final (sorted) order — the Up/Down nav list
        const int sortCol = _sessionsSortColumn;
        const bool asc = _sessionsSortAscending;
        std::sort(view.begin(), view.end(), [sortCol, asc](const _SessionsRow* a, const _SessionsRow* b) {
            const auto cmpS = [](const std::wstring& x, const std::wstring& y) {
                const auto lx = ::Agentmaster::FoldLower(x), ly = ::Agentmaster::FoldLower(y);
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
                c = cmpI(a->createdMs, b->createdMs);
                break;
            case 5:
                c = cmpI(a->lastActivityMs, b->lastActivityMs);
                break;
            case 6:
                c = cmpI(a->msgs, b->msgs);
                break;
            default:
                break;
            }
            if (c == 0)
            {
                return a->lastActivityMs > b->lastActivityMs;
            }
            return asc ? (c < 0) : (c > 0);
        });

        // --- live enrichment lookups (UI thread; cheap) ---
        const auto presence = _observer ? _observer->Presence() : std::vector<::Agentmaster::SessionPresenceRow>{};
        const auto presenceFor = [&presence](const std::wstring& sid) -> const ::Agentmaster::SessionPresenceRow* {
            for (const auto& p : presence)
            {
                if (p.sessionId == sid)
                {
                    return &p;
                }
            }
            return nullptr;
        };

        SessCloseTipsIn(_sessionsRowsHost); // a re-render under the pointer must not orphan an open tip
        _sessionsRowsHost.Children().Clear();
        for (const auto* rp : view)
        {
            const auto& r = *rp;
            _sessionsVisibleOrder.push_back(r.id);
            Grid g;
            SessAddColumns(g);
            g.Padding(Thickness{ 6, 4, 6, 4 });

            // chip: the per-dir color (the SAME color the session's tab wears — answer "color
            // coding of the tab"), solid for an OPEN session, dim for an on-disk one; presence
            // (busy/waiting) rendered as the tooltip + a brighter ring.
            const auto reg = _sessionRegistry ? _sessionRegistry->Get(r.id) : std::nullopt;
            const bool live = reg && reg->live;
            const auto* pres = presenceFor(r.id);
            {
                auto hex = ::Agentmaster::GetDirColor(r.dir);
                const auto color = SessHexToColor(hex ? *hex : ::Agentmaster::AutoDirColorHex(r.dir));
                Border chip;
                chip.Width(10);
                chip.Height(10);
                chip.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 5, 5, 5, 5 });
                chip.VerticalAlignment(VerticalAlignment::Center);
                chip.HorizontalAlignment(HorizontalAlignment::Center);
                chip.Background(color ? SolidColorBrush{ *color } : SessBrush(0xFF, 0x60, 0x60, 0x60));
                chip.Opacity(live ? 1.0 : 0.35);
                std::wstring tip = live ? L"Open in this app now" : (reg ? L"Archived \x2014 closed but restorable" : L"On disk \x2014 not opened in this app");
                tip += L"\nDot color = this session's working-directory color (matches its tab)";
                if (pres)
                {
                    tip += L"\nClaude is " + pres->status; // its own busy / idle / waiting heartbeat
                    chip.BorderBrush(SessBrush(0xFF, 0xE8, 0xC0, 0x60));
                    chip.BorderThickness(Thickness{ 1.5, 1.5, 1.5, 1.5 });
                }
                if (r.fork)
                {
                    tip += L"\nFork of " + r.forkedFromId.substr(0, 8);
                }
                SessSetTip(chip, winrt::hstring{ tip });
                Grid::SetColumn(chip, 0);
                g.Children().Append(chip);
            }

            auto title = SessText(winrt::hstring{ (r.fork ? L"\x2442 " : L"") + r.title }, 12, false, live ? 1.0 : 0.85);
            SessSetTip(title, winrt::hstring{ r.title + L"\nSession id: " + r.id });
            Grid::SetColumn(title, 1);
            g.Children().Append(title);

            auto dir = SessText(winrt::hstring{ r.dir }, 11, false, 0.6);
            SessSetTip(dir, winrt::hstring{ r.dir }); // the full path — the cell end-trims, losing the leaf
            Grid::SetColumn(dir, 2);
            g.Children().Append(dir);

            auto branch = SessText(winrt::hstring{ r.branch }, 11, false, 0.6);
            SessSetTip(branch, winrt::hstring{ r.branch }); // the full branch name — no-op when empty
            Grid::SetColumn(branch, 3);
            g.Children().Append(branch);

            auto created = SessText(winrt::hstring{ SessAgo(r.createdMs, now) }, 11, false, 0.6);
            created.HorizontalAlignment(HorizontalAlignment::Center);
            if (const auto abs = SessLocalDateTime(r.createdMs); !abs.empty())
            {
                SessSetTip(created, winrt::hstring{ L"Created " + abs }); // the exact moment behind the relative age
            }
            Grid::SetColumn(created, 4);
            g.Children().Append(created);

            auto active = SessText(winrt::hstring{ SessAgo(r.lastActivityMs, now) }, 11, false, 0.75);
            active.HorizontalAlignment(HorizontalAlignment::Center);
            if (const auto abs = SessLocalDateTime(r.lastActivityMs); !abs.empty())
            {
                SessSetTip(active, winrt::hstring{ L"Last active " + abs });
            }
            Grid::SetColumn(active, 5);
            g.Children().Append(active);

            const std::wstring weight = std::to_wstring(r.msgs) + L"\x00B7" + std::to_wstring(r.tools);
            auto w = SessText(winrt::hstring{ weight }, 11, false, 0.6);
            w.HorizontalAlignment(HorizontalAlignment::Center);
            SessSetTip(w, winrt::hstring{ std::to_wstring(r.msgs) + L" messages \x00B7 " + std::to_wstring(r.tools) + L" tool calls \x00B7 " + std::to_wstring(r.sizeBytes / 1024) + L" KB" });
            Grid::SetColumn(w, 6);
            g.Children().Append(w);

            if (searching)
            {
                const auto hit = _sessionsHitCounts.find(r.id);
                if (hit != _sessionsHitCounts.end())
                {
                    auto h = SessText(winrt::hstring{ std::to_wstring(hit->second) }, 11, true, 0.9);
                    h.HorizontalAlignment(HorizontalAlignment::Center);
                    Grid::SetColumn(h, 7);
                    g.Children().Append(h);
                }
            }

            Border rowB;
            rowB.Child(g);
            rowB.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            rowB.Background(r.id == _sessionsSelectedId ? SessBrush(0x30, 0x60, 0xA0, 0xE0) : SessBrush(0x14, 0xFF, 0xFF, 0xFF));
            rowB.Tag(winrt::box_value(winrt::hstring{ r.id }));
            rowB.PointerPressed([this](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
                const auto b = s.try_as<Border>();
                if (!b)
                {
                    return;
                }
                SessCloseTipsIn(b); // a click dismisses the row's tip (the standard behavior the islands stack drops)
                const std::wstring id{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") };
                e.Handled(true);
                // Defer: selection re-renders the detail pane (tree mutation) — the page's crash class.
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), id]() {
                    if (auto self = weak.get())
                    {
                        self->_sessionsSelectedId = id;
                        self->_UpdateSessionsSelectionHighlight(); // recolor only — no table rebuild per click
                        self->_ShowSessionsDetail(id);
                    }
                });
            });
            rowB.DoubleTapped([this](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::DoubleTappedRoutedEventArgs& e) {
                const auto b = s.try_as<Border>();
                if (!b)
                {
                    return;
                }
                const std::wstring id{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") };
                e.Handled(true);
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), id]() {
                    auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    // If the session is already open in a tab (any window), jump straight to it —
                    // the same as the right-click "Jump to tab". Only resume from disk when it is NOT
                    // open: the resume seam jumps a live session too, but going direct here skips its
                    // /clear-chain tail resolution and lands on the clicked session's own tab.
                    if (self->_sessionRegistry)
                    {
                        if (const auto reg = self->_sessionRegistry->Get(id); reg && reg->live)
                        {
                            self->_ActivateClaudeSession(winrt::hstring{ id });
                            return;
                        }
                    }
                    // Not open: don't resume silently — prompt Resume / Fork / Cancel (the adopt
                    // dialog's idiom), since forking a recognized conversation is just as common as
                    // continuing it. (The detail pane + right-click menu offer both as buttons; the
                    // double-click used to pick Resume for you.)
                    for (const auto& row : self->_sessionsRows)
                    {
                        if (row.id == id)
                        {
                            // never-prompted -> empty fork title so the fork seam derives a smart name
                            // (matches the detail-pane Fork button + the right-click "Fork here").
                            const std::wstring forkTitle = row.msgs > 0 ? row.title : std::wstring{};
                            self->_PromptResumeOrForkSession(row.id, row.dir, row.title, forkTitle);
                            break;
                        }
                    }
                });
            });
            // Right-click: "Hide from list" — drop this session from the browser (persisted in
            // AppSettings.hiddenSessionIds; resettable from the Settings cog). Deferred one tick (the
            // MenuFlyout restores focus to its target as it closes, and the action mutates the tree —
            // the page's pointer-handler discipline). The transcript on disk is never touched.
            // Right-click menu: the SAME actions as the detail pane's right-side buttons, but the
            // OPEN actions create their tab in the BACKGROUND and keep the Sessions list open (the
            // _openClaudeTabInBackground flag), so you can BULK-open several rows without focus jumping
            // to each new tab. (The detail-pane buttons stay foreground — a single open that lands on
            // the tab.) Each item defers one tick (the MenuFlyout restores focus to its target as it
            // closes + the action mutates the tree — the page's pointer-handler discipline). The flag
            // is set around ONE open and reset via wil::scope_exit so a throw can't strand it.
            {
                MenuFlyout rowMenu;
                const std::wstring rid = r.id, rdir = r.dir, rtitle = r.title;
                const std::wstring forkTitle = r.msgs > 0 ? r.title : std::wstring{}; // never-prompted -> let the fork seam derive a smart name
                const bool rIsOpen = _claudeTabs.find(rid) != _claudeTabs.end();

                if (rIsOpen)
                {
                    MenuFlyoutItem jump;
                    jump.Text(L"Jump to tab");
                    SessSetTip(jump, L"Switch to this session's already-open tab");
                    jump.Click([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid]() {
                            if (auto self = weak.get())
                            {
                                self->_ActivateClaudeSession(winrt::hstring{ rid });
                            }
                        });
                    });
                    rowMenu.Items().Append(jump);
                }
                else
                {
                    MenuFlyoutItem resume;
                    resume.Text(L"Resume here");
                    SessSetTip(resume, L"claude --resume into a NEW BACKGROUND tab \x2014 the list stays open, so you can bulk-open more");
                    resume.Click([this, rid, rdir, rtitle](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid, rdir, rtitle]() {
                            if (auto self = weak.get())
                            {
                                self->_openClaudeTabInBackground = true;
                                auto reset = wil::scope_exit([self]() { self->_openClaudeTabInBackground = false; });
                                self->_ResumeSessionFromDisk(rid, rdir, rtitle);
                            }
                        });
                    });
                    rowMenu.Items().Append(resume);
                }

                MenuFlyoutItem forkBtn;
                forkBtn.Text(L"Fork here");
                SessSetTip(forkBtn, L"Fork into a NEW BACKGROUND tab (claude --resume --fork-session) \x2014 the original is untouched; the list stays open");
                forkBtn.Click([this, rid, rdir, forkTitle](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid, rdir, forkTitle]() {
                        if (auto self = weak.get())
                        {
                            self->_openClaudeTabInBackground = true;
                            auto reset = wil::scope_exit([self]() { self->_openClaudeTabInBackground = false; });
                            self->_ForkSessionFromDisk(rid, rdir, forkTitle);
                        }
                    });
                });
                rowMenu.Items().Append(forkBtn);

                MenuFlyoutItem fresh;
                fresh.Text(L"Open New Session Here");
                SessSetTip(fresh, L"Start a FRESH BACKGROUND session in this directory \x2014 the list stays open");
                fresh.Click([this, rdir](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rdir]() {
                        if (auto self = weak.get())
                        {
                            self->_openClaudeTabInBackground = true;
                            auto reset = wil::scope_exit([self]() { self->_openClaudeTabInBackground = false; });
                            self->_SpawnClaudeSession(winrt::hstring{ rdir }, winrt::hstring{ L"" });
                        }
                    });
                });
                rowMenu.Items().Append(fresh);

                rowMenu.Items().Append(MenuFlyoutSeparator{});

                MenuFlyoutItem hideItem;
                hideItem.Text(L"Hide from list");
                SessSetTip(hideItem, L"Hide this session from the list \x2014 it stays on disk and can be brought back from Settings.");
                hideItem.Click([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid]() {
                        if (auto self = weak.get())
                        {
                            self->_HideSessionFromList(rid);
                        }
                    });
                });
                rowMenu.Items().Append(hideItem);
                rowB.ContextFlyout(rowMenu);
            }
            _sessionsRowsHost.Children().Append(rowB);
        }

        if (_sessionsCountText)
        {
            std::wstring counts = std::to_wstring(view.size());
            if (view.size() != _sessionsRows.size())
            {
                counts += L" of " + std::to_wstring(_sessionsRows.size());
            }
            counts += L" sessions \x00B7 ";
            counts += (_sessionsFromMs > 0) ? L"custom range" : kSessPresets[std::clamp(_sessionsWindowPreset, 0, kSessPresetCount - 1)].label;
            if (hiddenInWindow > 0)
            {
                counts += L" \x00B7 " + std::to_wstring(hiddenInWindow) + L" hidden"; // resettable in Settings
            }
            if (_sessionsIndexing.load())
            {
                counts += L" \x00B7 indexing\x2026";
            }
            _sessionsCountText.Text(winrt::hstring{ counts });
        }
    }

    void TerminalPage::_ShowSessionsDetail(const std::wstring& sessionId)
    {
        if (!_sessionsDetailHost)
        {
            return;
        }
        SessCloseTipsIn(_sessionsDetailHost); // a detail re-render under the pointer must not orphan an open tip
        _sessionsDetailHost.Children().Clear();
        const _SessionsRow* row = nullptr;
        for (const auto& r : _sessionsRows)
        {
            if (r.id == sessionId)
            {
                row = &r;
                break;
            }
        }
        if (!row)
        {
            _sessionsDetailHost.Children().Append(SessText(L"Select a session", 12, false, 0.5));
            return;
        }
        const int64_t now = SessNowMs();

        _sessionsDetailHost.Children().Append(SessText(winrt::hstring{ row->title }, 15, true, 1.0));
        const auto meta = [&](const std::wstring& k, const std::wstring& v) {
            if (v.empty())
            {
                return;
            }
            auto t = SessText(winrt::hstring{ k + L"  " + v }, 11, false, 0.65);
            t.TextWrapping(TextWrapping::Wrap);
            _sessionsDetailHost.Children().Append(t);
        };
        // id / dir / branch are shown by the full summary box below (full=true), so don't repeat
        // them here — keep only what the box LACKS: the timing, the weight (msgs · tools · KB), the
        // fork lineage, and (below) the live presence line.
        meta(L"created", SessAgo(row->createdMs, now) + L" ago \x00B7 active " + SessAgo(row->lastActivityMs, now) + L" ago");
        {
            std::wstring weight = std::to_wstring(row->msgs) + L" messages \x00B7 " + std::to_wstring(row->tools) + L" tool calls \x00B7 " + std::to_wstring(row->sizeBytes / 1024) + L" KB";
            if (row->fork)
            {
                weight += L" \x00B7 fork of " + row->forkedFromId.substr(0, 8) + L"\x2026";
            }
            meta(L"weight", weight);
        }
        const auto reg = _sessionRegistry ? _sessionRegistry->Get(row->id) : std::nullopt;
        if (reg && reg->live)
        {
            meta(L"state", reg->presenceStatus.empty() ? L"OPEN in this app" : (L"OPEN \x00B7 claude: " + reg->presenceStatus));
        }

        // Actions: Jump (live here) / Resume here (transcript-gated seam) / Open New Session Here.
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(6);
        actions.Margin(Thickness{ 0, 4, 0, 6 });
        const std::wstring id = row->id, dir = row->dir, title = row->title;
        if (_claudeTabs.find(id) != _claudeTabs.end())
        {
            Button jump;
            jump.Content(winrt::box_value(winrt::hstring{ L"Jump to tab" }));
            SessSetTip(jump, L"Switch to this session's open tab.");
            jump.Click([this, id](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), id]() {
                    if (auto self = weak.get())
                    {
                        self->_ActivateClaudeSession(winrt::hstring{ id });
                    }
                });
            });
            actions.Children().Append(jump);
        }
        else
        {
            Button resume;
            resume.Content(winrt::box_value(winrt::hstring{ L"Resume here" }));
            SessSetTip(resume, L"Resume this conversation in a managed tab \x2014 continue where it left off, with Flight Plan + Autopilot (claude --resume).");
            resume.Click([this, id, dir, title](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), id, dir, title]() {
                    if (auto self = weak.get())
                    {
                        self->_ResumeSessionFromDisk(id, dir, title);
                    }
                });
            });
            actions.Children().Append(resume);
        }
        // Fork here — ALWAYS offered (unlike Resume it is safe on a LIVE session too: the fork
        // writes its OWN new transcript, the parent's is untouched — no two-writers hazard).
        {
            Button forkBtn;
            forkBtn.Content(winrt::box_value(winrt::hstring{ L"Fork here" }));
            SessSetTip(forkBtn, L"Fork a NEW conversation from this one \x2014 a copy you can diverge freely; the original transcript is untouched (--fork-session).");
            // A never-prompted row's display title is the page's placeholder — pass empty so the
            // fork seam derives a smart name instead of "(no prompt yet) (fork)".
            const std::wstring forkTitle = row->msgs > 0 ? title : std::wstring{};
            forkBtn.Click([this, id, dir, forkTitle](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), id, dir, forkTitle]() {
                    if (auto self = weak.get())
                    {
                        self->_ForkSessionFromDisk(id, dir, forkTitle);
                    }
                });
            });
            actions.Children().Append(forkBtn);
        }
        Button fresh;
        fresh.Content(winrt::box_value(winrt::hstring{ L"Open New Session Here" }));
        SessSetTip(fresh, L"Start a fresh Claude session in this session's working directory.");
        fresh.Click([this, dir](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), dir]() {
                if (auto self = weak.get())
                {
                    self->_SpawnClaudeSession(winrt::hstring{ dir }, winrt::hstring{ L"" });
                }
            });
        });
        actions.Children().Append(fresh);
        _sessionsDetailHost.Children().Append(actions);

        // Hit snippets (when a content search produced them) — the matched messages in scope tags.
        if (const auto sn = _sessionsHitSnippets.find(row->id); sn != _sessionsHitSnippets.end() && !sn->second.empty())
        {
            _sessionsDetailHost.Children().Append(SessText(L"MATCHES", 11, true, 0.5));
            for (const auto& s : sn->second)
            {
                auto t = SessText(winrt::hstring{ s }, 11, false, 0.8);
                t.TextWrapping(TextWrapping::Wrap);
                _sessionsDetailHost.Children().Append(t);
            }
        }

        // The full session-summary box — the SAME session-end.js analyzer the per-tab overlay's
        // summary panel renders (RenderSessionSummaryBox), here full=true so it carries id / Dir /
        // Folder / Resume / Branch / Tasks + the numbered Messages (deduped, whole-file, noise-
        // filtered) + Files Read/Created/Edited + any plan-start/plan-end lineage. Off-thread whole-
        // file analyze, cached by (id, transcript mtime). It SUPERSEDES the old flat PROMPTS list —
        // the box's Messages section is the same prompts, deduped over the WHOLE transcript.
        if (_sessionsDetailTiId == row->id && _sessionsDetailTiMtime == row->lastActivityMs)
        {
            if (!_sessionsDetailSummary.empty())
            {
                SessAppendSummaryBox(_sessionsDetailHost, _sessionsDetailSummary);
            }
        }
        else if (!_sessionsDetailPending)
        {
            _LoadSessionsSummary(row->id, row->dir, row->lastActivityMs);
        }
    }

    // Off-thread: resolve the transcript, run the WHOLE-FILE session-end.js analyzer, and render the
    // FULL summary box (full=true) — the SAME RenderSessionSummaryBox the per-tab overlay's summary
    // panel uses, so the two renderings can never drift. An EMPTY live glyph/label means the box's
    // header line appears ONLY for a plan-start/plan-end session (auto-detected) — the live state is
    // shown by the detail's own rows, not duplicated. resumeCmd is the descriptive `claude --resume
    // <id>` (the page's Resume/Fork buttons do the real, hook-wired launch). Cached by (id, mtime).
    winrt::fire_and_forget TerminalPage::_LoadSessionsSummary(std::wstring sessionId, std::wstring dir, int64_t mtime)
    {
        _sessionsDetailPending = true;
        auto weakThis{ get_weak() };
        co_await winrt::resume_background();

        std::wstring text;
        const std::wstring path = ::Agentmaster::ResolveClaudeTranscriptPath(sessionId);
        if (!path.empty())
        {
            const auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
            std::wstring planFile = a.planFilePath;
            if (planFile.empty() && a.hasPlanContent && !a.parentSessionId.empty())
            {
                // A plan-start session's plan file lives in its PARENT transcript (session-end.js).
                const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(a.parentSessionId);
                if (!parentPath.empty())
                {
                    planFile = ::Agentmaster::FindPlanFileInTranscript(parentPath);
                }
            }
            text = ::Agentmaster::RenderSessionSummaryBox(a, sessionId, dir, path, L"claude --resume " + sessionId, L"" /* no live glyph */, L"" /* empty label => header only for plan */, planFile, /*full*/ true);
        }

        co_await winrt::resume_foreground(Dispatcher());
        auto self = weakThis.get();
        if (!self)
        {
            co_return;
        }
        self->_sessionsDetailPending = false;
        self->_sessionsDetailTiId = sessionId;
        self->_sessionsDetailTiMtime = mtime;
        self->_sessionsDetailSummary = std::move(text);
        if (self->_sessionsSelectedId == sessionId && self->_sessionsPageVisible.load(std::memory_order_relaxed))
        {
            self->_ShowSessionsDetail(sessionId);
        }
    }

    // A logical Claude conversation that was `/clear`ed (or plan-restarted, or continued by pasting a
    // handover) lives across several UNLINKED on-disk session files (see TranscriptStore's continuation
    // notes). The user recognizes the conversation by its ORIGINAL first-prompt title — which is the
    // chain HEAD — but "restore my conversation" means "where I LEFT OFF" = the TAIL. Resolve the tail
    // (same cwd + time-adjacency, read-only) and, when it differs, make sure a restorable registry
    // record exists for it (upsert a minimal archived-shaped one for a never-managed on-disk tail,
    // inheriting the source dir). Returns `clickedId` unchanged when there is no newer continuation, the
    // dir is unreadable, or the record is a Codex one (Codex doesn't /clear into new files). Idempotent.
    std::wstring TerminalPage::_ResolveRestoreChainTail(const std::wstring& clickedId, const std::wstring& dirHint, const std::wstring& titleHint)
    {
        if (!_sessionRegistry || clickedId.empty())
        {
            return clickedId;
        }
        std::wstring cwd = dirHint;
        std::wstring titleFallback = titleHint;
        if (const auto rec = _sessionRegistry->Get(clickedId))
        {
            if (rec->kind == ::Agentmaster::AgentKind::Codex)
            {
                return clickedId; // the continuation chain is a Claude /clear concept
            }
            if (!rec->workingDir.empty())
            {
                cwd = rec->workingDir;
            }
            if (titleFallback.empty())
            {
                titleFallback = rec->title;
            }
        }
        if (cwd.empty())
        {
            return clickedId;
        }
        const auto tail = ::Agentmaster::ResolveContinuationTailOnDisk(clickedId, cwd);
        if (tail.tailId.empty() || tail.tailId == clickedId)
        {
            return clickedId;
        }
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[resume->continuation] " + clickedId + L" -> " + tail.tailId + L" (" + std::to_wstring(tail.hops) + L" /clear hops, same dir)\n");
        if (!_sessionRegistry->Get(tail.tailId))
        {
            ::Agentmaster::SessionInfo s;
            s.id = tail.tailId;
            s.workingDir = !tail.tailCwd.empty() ? tail.tailCwd : cwd;
            s.title = !tail.tailTitle.empty() ? tail.tailTitle : titleFallback;
            s.kind = ::Agentmaster::AgentKind::Claude;
            s.state = ::Agentmaster::SessionState::Idle;
            s.live = false; // archived-shaped: exactly what _RestoreArchivedSession expects
            _sessionRegistry->Upsert(std::move(s));
        }
        return tail.tailId;
    }

    // Resume ANY on-disk session into a managed tab: live here -> Jump; known-archived -> the
    // normal Restore seam; unknown to the registry -> upsert a minimal ARCHIVED record first,
    // then the SAME transcript-gated resume seam (claude --resume; fresh if the transcript
    // vanished). This reuses every existing guarantee: title pinning, per-dir tab color, hooks
    // correlation by the same id, Rule #6's gating. The clicked id is first resolved to its
    // continuation-chain TAIL (a /clear'd conversation), so resuming lands where the user left off.
    void TerminalPage::_ResumeSessionFromDisk(const std::wstring& sessionId, const std::wstring& dir, const std::wstring& title)
    {
        if (!_sessionRegistry || sessionId.empty() || dir.empty())
        {
            return;
        }
        const std::wstring target = _ResolveRestoreChainTail(sessionId, dir, title); // follow /clear chain to the tail
        const auto existing = _sessionRegistry->Get(target);
        if (existing && existing->live)
        {
            // Already OPEN somewhere in this app. Foreground: jump to it. Background bulk-open: leave
            // focus where it is (don't yank to an already-open tab) — it's already there to switch to.
            if (!_openClaudeTabInBackground)
            {
                _ActivateClaudeSession(winrt::hstring{ target });
            }
            return;
        }
        if (!existing)
        {
            ::Agentmaster::SessionInfo s;
            s.id = target;
            s.workingDir = dir;
            s.title = title;
            s.state = ::Agentmaster::SessionState::Idle;
            s.live = false; // archived-shaped: exactly what _RestoreArchivedSession expects
            _sessionRegistry->Upsert(std::move(s));
        }
        _RestoreArchivedSession(winrt::hstring{ target });
        if (!_openClaudeTabInBackground)
        {
            _HideSessionsPage(); // foreground: land on the freshly opened tab. Background bulk-open keeps the list open.
        }
    }

    // Fork ANY on-disk session into a NEW managed conversation — the duplicate-tab fork's exact
    // recipe (TabManagement.cpp): `claude --resume <parent> --fork-session --session-id <new>`
    // (BuildClaudeSpawn mints the new id, so hooks/registry correlate from the first event), the
    // parent transcript untouched. Transcript-gated: a parent with no transcript (or one the
    // cleanup sweep deleted mid-view) degrades to a FRESH session in the same dir rather than
    // dying on "No conversation found". Safe on a LIVE parent — the fork writes its own file.
    void TerminalPage::_ForkSessionFromDisk(const std::wstring& parentId, const std::wstring& dir, const std::wstring& title)
    {
        if (!_sessionRegistry || parentId.empty() || dir.empty())
        {
            return;
        }
        // Fork the LATEST link of the conversation (the /clear chain tail), not an earlier checkpoint
        // the user happens to recognize by its original title — so the fork branches from where they
        // left off. The tail's own title seeds the "(fork)" name; its cwd is identical (same-dir chain).
        std::wstring forkParentId = parentId;
        std::wstring forkDir = dir;
        std::wstring forkBaseTitle = title;
        {
            const auto tail = ::Agentmaster::ResolveContinuationTailOnDisk(parentId, dir);
            if (!tail.tailId.empty() && tail.tailId != parentId)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[sessions-page->fork] continuation " + parentId + L" -> " + tail.tailId + L" (" + std::to_wstring(tail.hops) + L" /clear hops)\n");
                forkParentId = tail.tailId;
                if (!tail.tailCwd.empty())
                {
                    forkDir = tail.tailCwd;
                }
                if (!tail.tailTitle.empty())
                {
                    forkBaseTitle = tail.tailTitle;
                }
            }
        }
        const std::wstring forkBase = !forkBaseTitle.empty() ? forkBaseTitle : ::Agentmaster::DeriveSessionTitle(forkDir);
        const std::wstring ttl = ::Agentmaster::DeriveForkTitle(forkBase); // bump " (fork N)" instead of stacking
        const std::wstring forkFrom = ::Agentmaster::ClaudeConversationExists(forkParentId) ? forkParentId : std::wstring{};
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[sessions-page->fork] source=" + forkParentId + (forkFrom.empty() ? L" (no transcript -> fresh session)" : L"") + L"\n");
        _LaunchClaudeSession(winrt::hstring{ forkDir }, winrt::hstring{ ttl }, std::nullopt, forkFrom);
        if (!_openClaudeTabInBackground)
        {
            _HideSessionsPage(); // foreground: land on the fork. Background bulk-open keeps the list open.
        }
    }

    // Agentmaster: the Sessions-page double-click prompt — Resume / Fork / Cancel. Double-clicking a
    // CLOSED on-disk session used to resume it silently; offer the choice instead (the adopt dialog's
    // Fork-a-copy-vs-Resume idiom — _ConfirmChoice), since forking a recognized conversation is just as
    // common as continuing it. Resume (Primary) = continue this conversation (claude --resume); Fork
    // (Secondary) = branch a NEW conversation from it (--fork-session, the original transcript
    // untouched); Cancel = nothing. Shown only for a NOT-live row — a live session's double-click jumps
    // straight to its tab (handled before this is called). forkTitle is empty for a never-prompted row
    // so the fork seam derives a smart name.
    winrt::fire_and_forget TerminalPage::_PromptResumeOrForkSession(std::wstring sessionId, std::wstring dir, std::wstring title, std::wstring forkTitle)
    {
        const auto presenter{ _dialogPresenter.get() };
        if (!presenter)
        {
            // No presenter to confirm with -> preserve the prior behavior (resume) rather than
            // stranding the double-click.
            _ResumeSessionFromDisk(sessionId, dir, title);
            co_return;
        }

        ContentDialog dialog;
        dialog.Title(winrt::box_value(L"Open session"));
        dialog.Content(winrt::box_value(
            title.empty() ?
                winrt::hstring{ L"Resume continues this conversation in a managed tab (claude --resume). Fork branches a NEW conversation from it \x2014 a copy you can diverge freely; the original transcript is untouched (--fork-session)." } :
                winrt::hstring{ L"\x201C" + title + L"\x201D \x2014 Resume continues this conversation in a managed tab (claude --resume). Fork branches a NEW conversation from it \x2014 a copy you can diverge freely; the original transcript is untouched (--fork-session)." }));
        dialog.PrimaryButtonText(L"Resume");
        dialog.SecondaryButtonText(L"Fork");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary); // safe default = Resume (continue where you left off)

        const auto weak = get_weak();
        const auto result = co_await presenter.ShowDialog(dialog);
        const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
        if (!strong)
        {
            co_return;
        }
        if (result == ContentDialogResult::Primary)
        {
            _ResumeSessionFromDisk(sessionId, dir, title);
        }
        else if (result == ContentDialogResult::Secondary)
        {
            _ForkSessionFromDisk(sessionId, dir, forkTitle);
        }
        // else Close/Cancel -> do nothing
    }

    // Recolor the row highlights for _sessionsSelectedId WITHOUT rebuilding the table (the
    // archive page's _UpdateArchiveSelectionHighlight pattern) — row taps + keyboard nav.
    void TerminalPage::_UpdateSessionsSelectionHighlight()
    {
        if (!_sessionsRowsHost)
        {
            return;
        }
        for (const auto& child : _sessionsRowsHost.Children())
        {
            const auto border = child.try_as<Border>();
            if (!border)
            {
                continue;
            }
            const std::wstring id{ winrt::unbox_value_or<winrt::hstring>(border.Tag(), L"") };
            border.Background(id == _sessionsSelectedId ? SessBrush(0x30, 0x60, 0xA0, 0xE0) : SessBrush(0x14, 0xFF, 0xFF, 0xFF));
        }
    }

    // Up/Down keyboard navigation over the VISIBLE (sorted + filtered) rows. No selection yet:
    // Down picks the first row, Up the last; with one, the selection moves ±1 and WRAPS at the
    // ends (rotates). The selected row is scrolled into view.
    void TerminalPage::_MoveSessionsSelection(int delta)
    {
        if (_sessionsVisibleOrder.empty() || !_sessionsRowsHost)
        {
            return;
        }
        const int n = static_cast<int>(_sessionsVisibleOrder.size());
        int idx = -1;
        if (!_sessionsSelectedId.empty())
        {
            for (int i = 0; i < n; ++i)
            {
                if (_sessionsVisibleOrder[i] == _sessionsSelectedId)
                {
                    idx = i;
                    break;
                }
            }
        }
        // A selection filtered out of view counts as none (idx -1): Down = first, Up = last.
        const int next = (idx < 0) ? (delta > 0 ? 0 : n - 1) : (((idx + delta) % n + n) % n);
        _sessionsSelectedId = _sessionsVisibleOrder[next];
        _UpdateSessionsSelectionHighlight();
        _ShowSessionsDetail(_sessionsSelectedId);
        // Key-nav scrolls WITHOUT pointer input — a row tip open under the stationary mouse
        // never gets the PointerExited that would close it when its row scrolls away.
        SessCloseTipsIn(_sessionsRowsHost);
        for (const auto& child : _sessionsRowsHost.Children())
        {
            if (const auto b = child.try_as<Border>(); b && std::wstring{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") } == _sessionsSelectedId)
            {
                b.StartBringIntoView();
                break;
            }
        }
    }

    // Row right-click "Hide from list": append this id to AppSettings.hiddenSessionIds via a
    // freshest-disk read-modify-write (so it sticks across restarts AND doesn't clobber another
    // field / another window's concurrent write — the splitter/treeSort pattern), refresh THIS
    // window's in-memory copy, and re-render the table (the render chokepoint filters it out).
    // Resettable from the Settings cog. The transcript on disk is NEVER touched — a browse-list
    // preference only. Already-hidden ids are a no-op (no duplicate write).
    void TerminalPage::_HideSessionFromList(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto s = ::Agentmaster::LoadAppSettings();
        if (std::find(s.hiddenSessionIds.begin(), s.hiddenSessionIds.end(), sessionId) == s.hiddenSessionIds.end())
        {
            s.hiddenSessionIds.push_back(sessionId);
            ::Agentmaster::SaveAppSettings(s);
        }
        _appSettings.hiddenSessionIds = s.hiddenSessionIds;
        // If the hidden row was selected, drop the selection so the detail pane doesn't keep
        // showing a session that's no longer in the list.
        if (_sessionsSelectedId == sessionId)
        {
            _sessionsSelectedId.clear();
            _ShowSessionsDetail(_sessionsSelectedId); // -> "Select a session"
        }
        _RenderSessionsTable();
    }

    // Settings cog "Reset hidden sessions" (wired via SetResetHiddenSessionsHandler): clear the
    // whole hidden set (freshest-disk RMW) and re-render the Sessions page if it is built, so every
    // hidden session reappears. No re-gather needed — the rows are still in _sessionsRows; only the
    // render-time filter changes. A no-op when nothing is hidden.
    void TerminalPage::_ResetHiddenSessions()
    {
        auto s = ::Agentmaster::LoadAppSettings();
        if (s.hiddenSessionIds.empty())
        {
            _appSettings.hiddenSessionIds.clear();
            return;
        }
        s.hiddenSessionIds.clear();
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.hiddenSessionIds.clear();
        _RenderSessionsTable(); // no-op if the page was never built (host null)
    }

    // ===== the generic window-level page-overlay seam (_agentPageOverlays) ===================
    // Cross-page infrastructure (it lives in this TU as the newest page's home): each full-window
    // page registers ONCE at build; any global dismiss site — today the tab-switch handler in
    // TabManagement.cpp — closes ALL of them without naming any page, so a future page binds
    // automatically by registering.

    void TerminalPage::_RegisterAgentPageOverlay(const Grid& host, std::atomic<bool>* visibleMirror, std::function<void()> onDismiss)
    {
        _agentPageOverlays.push_back(_AgentPageOverlay{ host, visibleMirror, std::move(onDismiss) });
    }

    void TerminalPage::_DismissAgentPageOverlays()
    {
        for (auto& p : _agentPageOverlays)
        {
            if (p.host)
            {
                p.host.Visibility(Visibility::Collapsed);
            }
            if (p.visibleMirror)
            {
                p.visibleMirror->store(false, std::memory_order_relaxed);
            }
            if (p.onDismiss)
            {
                p.onDismiss(); // e.g. close an owned Popup — a collapsed host does NOT hide those
            }
        }
    }
}
