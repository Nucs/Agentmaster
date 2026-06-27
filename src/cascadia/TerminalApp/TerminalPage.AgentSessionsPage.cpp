// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the full-window Sessions page (SESSIONS.md): a browser over EVERY on-disk
// Claude Code session in a selectable time window (default 1 month), opened by the Manager's
// "Sessions" button (right after Archived). Duplicates the Archive page's structure with a
// SEARCH BAR at the top: [ search for sessions ] (👤)(🤖)(📁)(📄)(🏷)(F) [☐ Open] [1 month] —
//   👤 = also search user (typed) messages      🤖 = also search agent + tools text
//   📁 = match directories accessed             📄 = match files accessed
//   🏷 = match the session title (incl. an open session's live tab title)
//   (F) = fuzzy   ·   ☐ Open = show only sessions open in a window right now (a ROW filter)
//   With 🏷 off and both message scopes off, terms match the working directory (+ 📁/📄) only (§1a).
//   Defaults: 📁+📄+🏷 ON (fast-phase-only — in-memory, no IO); 👤/🤖/(F)/Open OFF (either message
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
#include "AgentMaster/Engine.h" // EnsureClaudeAvailable (native-exe-only launch gate)
#include "AgentMaster/Persistence.h" // GetDirColor / AutoDirColorHex (the per-dir color chip)
#include "AgentMaster/ProcessInspect.h" // AnalyzeSessionTranscript / RenderSessionSummaryBox / FindPlanFileInTranscript / kSummarySepMark (detail summary box)
#include "AgentMaster/ProcessObserver.h" // Presence()
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionSearch.h" // the two-phase search
#include "AgentMaster/SessionStore.h" // durable per-session store (LoadAllStoredSessionTitles — the title overlay)
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

        // A "loading…" row for the summary display area: a spinning ProgressRing + a dim label,
        // shown while the off-thread whole-file analyze runs. The detail pane re-renders when the
        // load lands, so the rendered box (or nothing, for an empty summary) replaces this.
        UIElement SessSummaryLoading()
        {
            StackPanel p;
            p.Orientation(Orientation::Horizontal);
            p.Spacing(8);
            p.Margin(Thickness{ 0, 10, 0, 0 });
            p.VerticalAlignment(VerticalAlignment::Center);
            winrt::Microsoft::UI::Xaml::Controls::ProgressRing ring;
            ring.IsActive(true);
            ring.Width(18);
            ring.Height(18);
            p.Children().Append(ring);
            p.Children().Append(SessText(L"Loading summary\x2026", 11, false, 0.6));
            return p;
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

        // The page's table columns (FAVORITES.md adds the leftmost ★ column, shifting the rest +1):
        // 0=★ favorite · 1=color/live chip · 2=Title · 3=Directory · 4=Branch · 5=Created ·
        // 6=Active · 7=Msgs·Tools · 8=Ctx (context tokens) · 9=Hits (populated while searching).
        void SessAddColumns(Grid& g, bool showHits)
        {
            const auto col = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                g.ColumnDefinitions().Append(c);
            };
            col(22, GridUnitType::Pixel); // ★ favorite (leftmost; clickable)
            col(26, GridUnitType::Pixel); // chip
            col(2.2, GridUnitType::Star); // title
            col(1.6, GridUnitType::Star); // directory
            col(0.8, GridUnitType::Star); // branch
            col(58, GridUnitType::Pixel); // created
            col(58, GridUnitType::Pixel); // active
            col(74, GridUnitType::Pixel); // msgs·tools
            col(40, GridUnitType::Pixel); // ctx (context tokens — compact: "182K" / "1.05M"); snug, fits the max value + the sort arrow
            // hits: reserve its width ONLY when shown (a content 👤/🤖 search) — otherwise it would sit
            // empty at the far right as trailing dead space after Ctx (the rightmost column otherwise).
            col(showHits ? 48.0 : 0.0, GridUnitType::Pixel); // hits
        }

        // Agentmaster: the compact context-token count for the "Ctx" column — "182K", "8.3K",
        // "1.05M". A faithful copy of AgentManagerContent.cpp's FormatTokenCount (the Triage-Board
        // "ctx N" formatter) so the two surfaces read identically; kept TU-local (that one lives in
        // an anonymous namespace) — converge into a shared header on a quiet day, like the StateColor
        // duplication noted in CLAUDE.md. Whole-K past 10K, one decimal below, two-decimal M past 1M.
        std::wstring SessFormatTokens(int64_t n)
        {
            if (n < 0)
            {
                n = 0;
            }
            wchar_t buf[32];
            if (n >= 1000000)
            {
                swprintf_s(buf, L"%.2fM", static_cast<double>(n) / 1000000.0);
            }
            else if (n >= 10000)
            {
                swprintf_s(buf, L"%lldK", static_cast<long long>((n + 500) / 1000)); // rounded whole-K
            }
            else if (n >= 1000)
            {
                swprintf_s(buf, L"%.1fK", static_cast<double>(n) / 1000.0);
            }
            else
            {
                swprintf_s(buf, L"%lld", static_cast<long long>(n));
            }
            return buf;
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

        // The LOCAL [start, end) ms covering the calendar day / week (Monday-start) / month that
        // `unixMs` falls in — the row right-click "By Same Day/Week/Month" buckets. DST-safe: the
        // end is computed by advancing the field on the normalized start tm and re-running mktime
        // (so a 23h/25h DST day still spans exactly one calendar day), NOT by adding a fixed 86400.
        // gran: 0 = day, 1 = week, 2 = month. {0,0} on a bad/zero input.
        std::pair<int64_t, int64_t> SessLocalBucket(int64_t unixMs, int gran)
        {
            if (unixMs <= 0)
            {
                return { 0, 0 };
            }
            const __time64_t t = unixMs / 1000;
            struct tm lt
            {
            };
            if (_localtime64_s(&lt, &t) != 0)
            {
                return { 0, 0 };
            }
            lt.tm_hour = 0;
            lt.tm_min = 0;
            lt.tm_sec = 0;
            if (gran == 1)
            {
                lt.tm_mday -= (lt.tm_wday + 6) % 7; // back up to Monday (tm_wday 0=Sun); mktime normalizes a <=0 mday
            }
            else if (gran == 2)
            {
                lt.tm_mday = 1;
            }
            struct tm startTm = lt;
            startTm.tm_isdst = -1; // let the CRT resolve DST for this local wall-clock time
            const __time64_t startT = _mktime64(&startTm); // normalizes startTm in place
            if (startT == static_cast<__time64_t>(-1))
            {
                return { 0, 0 };
            }
            struct tm endTm = startTm; // the normalized start; advance exactly one bucket
            endTm.tm_isdst = -1;
            if (gran == 0)
            {
                endTm.tm_mday += 1;
            }
            else if (gran == 1)
            {
                endTm.tm_mday += 7;
            }
            else
            {
                endTm.tm_mon += 1;
            }
            const __time64_t endT = _mktime64(&endTm);
            if (endT == static_cast<__time64_t>(-1))
            {
                return { 0, 0 };
            }
            return { static_cast<int64_t>(startT) * 1000, static_cast<int64_t>(endT) * 1000 };
        }

        // A short human label for a time bucket — "day 2026-06-20" / "week of 2026-06-15" (the
        // Monday) / "month 2026-06" — shown on the filter chip + the count line. "" on a bad input.
        std::wstring SessBucketLabel(int64_t unixMs, int gran)
        {
            int64_t base = unixMs;
            if (gran == 1)
            {
                base = SessLocalBucket(unixMs, 1).first; // the Monday of the week
            }
            if (base <= 0)
            {
                return L"";
            }
            const __time64_t t = base / 1000;
            struct tm lt
            {
            };
            if (_localtime64_s(&lt, &t) != 0)
            {
                return L"";
            }
            wchar_t buf[40]{};
            if (gran == 2)
            {
                swprintf_s(buf, L"month %04d-%02d", lt.tm_year + 1900, lt.tm_mon + 1);
            }
            else if (gran == 1)
            {
                swprintf_s(buf, L"week of %04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
            }
            else
            {
                swprintf_s(buf, L"day %04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
            }
            return buf;
        }

        // The last path segment of a working dir, for a compact chip label (trailing separators
        // trimmed). Falls back to the whole string when there is no separator.
        std::wstring SessLeaf(const std::wstring& dir)
        {
            std::wstring d = dir;
            while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
            {
                d.pop_back();
            }
            const size_t cut = d.find_last_of(L"\\/");
            return cut == std::wstring::npos ? d : d.substr(cut + 1);
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

        // --- header: a TOP row (Back · title/count · the search box, stretched to the table's right
        // edge) over a FILTERS row (the scope toggles, Open/Hidden/Favorite, time window, refresh) —
        // the same title-over-actions split the Manager toolbar uses. ---
        Grid header;
        header.Margin(Thickness{ 16, 10, 16, 8 });
        {
            const auto hrow = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                header.RowDefinitions().Append(r);
            };
            hrow(0, GridUnitType::Auto); // 0: Back · title · search
            hrow(0, GridUnitType::Auto); // 1: the filters
        }

        // TOP row — its columns MIRROR the body below (table 0.6* · divider · detail 0.4*) so the
        // search box, stretched to fill the left column after Back + the title, ends exactly at the
        // table's right edge. Only the left column is populated; the rest is alignment space.
        Grid topRow;
        {
            const auto tcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                topRow.ColumnDefinitions().Append(c);
            };
            tcol(0.6, GridUnitType::Star); // == body table column
            tcol(0, GridUnitType::Auto); // == body divider footprint (the spacer below)
            tcol(0.4, GridUnitType::Star); // == body detail column
        }
        Grid::SetRow(topRow, 0);
        header.Children().Append(topRow);

        // Left-column content: Back · title/count · search (the box fills the column's remainder).
        Grid topLeft;
        {
            const auto lcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                topLeft.ColumnDefinitions().Append(c);
            };
            lcol(0, GridUnitType::Auto); // back
            lcol(0, GridUnitType::Auto); // title/count
            lcol(1, GridUnitType::Star); // search box — fills the rest of the table-width column
        }
        Grid::SetColumn(topLeft, 0);
        topRow.Children().Append(topLeft);

        Button back;
        back.Content(winrt::box_value(winrt::hstring{ L"\x2190  Back" }));
        back.VerticalAlignment(VerticalAlignment::Center);
        SessSetTip(back, L"Back \x2014 close the Sessions browser and return to your tabs.");
        back.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) { ::Agentmaster::LogNav(L"sessions-page close (back)"); _HideSessionsPage(); }); // Nav audit: the EXPLICIT close (the programmatic _HideSessionsPage after a resume/fork is part of THAT action, so it isn't logged here)
        Grid::SetColumn(back, 0);
        topLeft.Children().Append(back);

        StackPanel titleStack;
        titleStack.Margin(Thickness{ 14, 0, 14, 0 });
        titleStack.VerticalAlignment(VerticalAlignment::Center);
        titleStack.Children().Append(SessText(L"Claude Code Sessions", 18, true, 1.0));
        _sessionsCountText = SessText(L"", 12, false, 0.6);
        titleStack.Children().Append(_sessionsCountText);
        Grid::SetColumn(titleStack, 1);
        topLeft.Children().Append(titleStack);

        TextBox search;
        search.PlaceholderText(L"search for sessions");
        search.HorizontalAlignment(HorizontalAlignment::Stretch); // stretch to the table's right edge
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
        Grid::SetColumn(search, 2);
        topLeft.Children().Append(search);

        // The divider-matching spacer (the body divider is Width 1 + 8px margins = a 17px footprint),
        // so topLeft's right edge lines up exactly with the table's right edge.
        Border topDividerSpacer;
        topDividerSpacer.Width(1);
        topDividerSpacer.Margin(Thickness{ 8, 0, 8, 0 });
        Grid::SetColumn(topDividerSpacer, 1);
        topRow.Children().Append(topDividerSpacer);

        // FILTERS row — its own line under the search box, spanning the table width via the SAME body
        // column mirror as the top row. The scope / Open / Hidden / Favorite / window / refresh controls
        // pack at the LEFT; the "✕ filter" chip is pinned to the RIGHT (the table's right edge, directly
        // under the search box's right end).
        Grid filtersRow;
        filtersRow.Margin(Thickness{ 0, 8, 0, 0 }); // gap below the search row
        {
            const auto fcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                filtersRow.ColumnDefinitions().Append(c);
            };
            fcol(0.6, GridUnitType::Star); // == body table column
            fcol(0, GridUnitType::Auto); // == body divider footprint (the spacer below)
            fcol(0.4, GridUnitType::Star); // == body detail column
        }
        Grid::SetRow(filtersRow, 1);
        header.Children().Append(filtersRow);

        // The table-width left cell: controls (Star, left) + the filter chip (Auto, right) — separate
        // columns so they never overlap; the controls column shrinks before the chip does.
        Grid filtersLeft;
        {
            const auto flcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                filtersLeft.ColumnDefinitions().Append(c);
            };
            flcol(1, GridUnitType::Star); // the controls
            flcol(0, GridUnitType::Auto); // the chip, right-aligned
        }
        Grid::SetColumn(filtersLeft, 0);
        filtersRow.Children().Append(filtersLeft);

        // The divider-matching spacer (Width 1 + 8px margins = 17px) so the left cell's right edge
        // lines up with the table's right edge, like the top row.
        Border filtersDividerSpacer;
        filtersDividerSpacer.Width(1);
        filtersDividerSpacer.Margin(Thickness{ 8, 0, 8, 0 });
        Grid::SetColumn(filtersDividerSpacer, 1);
        filtersRow.Children().Append(filtersDividerSpacer);

        StackPanel bar; // the left-packed controls
        bar.Orientation(Orientation::Horizontal);
        bar.Spacing(6);
        bar.VerticalAlignment(VerticalAlignment::Center);
        bar.HorizontalAlignment(HorizontalAlignment::Left);
        Grid::SetColumn(bar, 0);
        filtersLeft.Children().Append(bar);

        // "✕ filter: …" — the active row right-click "Filter" facet(s), shown only while one is set
        // (collapsed otherwise, so it takes no layout space). It reads as "search AND this filter";
        // clicking it clears ALL facets. Pinned to the RIGHT of the filters row (the table's right
        // edge). The label is rebuilt by _UpdateSessionsFilterChip; the render path keeps it in sync.
        _sessFilterChip = Button{};
        _sessFilterChip.Visibility(Visibility::Collapsed);
        _sessFilterChip.MinWidth(0);
        _sessFilterChip.HorizontalAlignment(HorizontalAlignment::Right);
        _sessFilterChip.VerticalAlignment(VerticalAlignment::Center);
        _sessFilterChip.Padding(Thickness{ 8, 2, 8, 2 });
        SessSetTip(_sessFilterChip, L"Active row filter \x2014 click to clear it.");
        _sessFilterChip.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    // Move focus OFF the chip before _ClearSessionsRowFilter collapses it — collapsing
                    // the focused element forces XAML to re-home focus, which is fragile under XAML
                    // Islands. The search box is the natural landing spot (and re-arms typing).
                    if (self->_sessionsSearchBox)
                    {
                        self->_sessionsSearchBox.Focus(FocusState::Programmatic);
                    }
                    self->_ClearSessionsRowFilter();
                }
            });
        });
        Grid::SetColumn(_sessFilterChip, 1);
        filtersLeft.Children().Append(_sessFilterChip); // right column of the table-width filters cell

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
        // 🏷 — match the session TITLE: its conversation title (custom / AI / first prompt) AND the
        // live tab title of an OPEN session (the liveTitle overlay), so a renamed session is found
        // by the name shown. Fast-phase-only (in-memory, no transcript IO). DEFAULT ON — titles were
        // always matched before, so this keeps current results; uncheck to leave titles out.
        _sessScopeTitleBtn = SessToggle(L"\U0001F3F7", L"Match the session title \x2014 its conversation title and the live tab name of an open session. On by default; uncheck to leave titles out of the search.");
        _sessScopeTitleBtn.IsChecked(true);
        _sessScopeTitleBtn.Click(onToggle);
        bar.Children().Append(_sessScopeTitleBtn);
        _sessFuzzyBtn = SessToggle(L"F", L"Fuzzy matching \x2014 the query's characters must appear in order, with gaps allowed (\"agmst\" matches \"agentmaster\").");
        _sessFuzzyBtn.Click(onToggle);
        bar.Children().Append(_sessFuzzyBtn);

        // "Open" — a row FILTER (distinct from the content-scope glyphs above, which only widen what
        // a query matches): show ONLY sessions currently live in any Agentmaster window (registry
        // live == the solid chip), hiding archived / on-disk ones. In-memory, default OFF; flips
        // through the same throttle so it composes (AND) with the search text + the scope toggles.
        _sessOpenOnlyBtn = CheckBox{};
        _sessOpenOnlyBtn.Content(winrt::box_value(winrt::hstring{ L"Open" }));
        _sessOpenOnlyBtn.MinWidth(0);
        _sessOpenOnlyBtn.VerticalAlignment(VerticalAlignment::Center);
        SessSetTip(_sessOpenOnlyBtn, L"Show only sessions open in an Agentmaster window right now (the solid color chip) \x2014 hides archived / on-disk ones.");
        _sessOpenOnlyBtn.Click(onToggle);
        bar.Children().Append(_sessOpenOnlyBtn);

        // "Hidden" — a row REVEAL filter for the hidden set (AppSettings.hiddenSessionIds): the
        // sessions you right-clicked "Hide from list" AND the ones auto-hidden when their tab was
        // deleted. Default OFF: hidden sessions are filtered out of the list (the chokepoint at the
        // render below). Checked: they are shown again — dimmed, and their right-click menu offers
        // "Unhide" — so you can find and resume/unhide a deleted-or-hidden session without clearing
        // the whole set from the Settings cog. In-memory, flips through the same throttle as "Open".
        _sessHiddenBtn = CheckBox{};
        _sessHiddenBtn.Content(winrt::box_value(winrt::hstring{ L"Hidden" }));
        _sessHiddenBtn.MinWidth(0);
        _sessHiddenBtn.VerticalAlignment(VerticalAlignment::Center);
        SessSetTip(_sessHiddenBtn, L"Reveal sessions hidden from the list \x2014 the ones you hid (their files are kept on disk). Off by default.");
        _sessHiddenBtn.Click(onToggle);
        bar.Children().Append(_sessHiddenBtn);

        // "Favorite" (FAVORITES.md) — a row filter: show ONLY favorited sessions (the ★ column /
        // SessionStore "favorite" key, loaded into _sessionsFavorites). Default OFF; flips through the
        // same throttle so it composes (AND) with the search text + the Open/Hidden toggles + facets.
        _sessFavOnlyBtn = CheckBox{};
        _sessFavOnlyBtn.Content(winrt::box_value(winrt::hstring{ L"Favorite" }));
        _sessFavOnlyBtn.MinWidth(0);
        _sessFavOnlyBtn.VerticalAlignment(VerticalAlignment::Center);
        SessSetTip(_sessFavOnlyBtn, L"Show only favorited sessions (the \x2605 star). Click a row's star on the left to favorite it.");
        _sessFavOnlyBtn.Click(onToggle);
        bar.Children().Append(_sessFavOnlyBtn);

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
            // Cancel any pending hover-intent close (re-entered the button, or arrived from the popup).
            if (_sessRangeCloseTimer)
            {
                _sessRangeCloseTimer.Stop();
            }
            // Anchor the popup under the window button's CURRENT position (root-relative). The header
            // cluster moved from the right edge to just after the title, so a fixed offset no longer
            // points at the button — read its live top-left within the host instead.
            const auto pt = _sessWindowBtn.TransformToVisual(_sessionsPageHost).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
            _sessRangePopup.HorizontalOffset(pt.X);
            _sessRangePopup.VerticalOffset(pt.Y + _sessWindowBtn.ActualHeight() + 4);
            _sessRangePopup.IsOpen(true);
        });
        // Leaving the button schedules a close (it doesn't close immediately): there's a 4px gap
        // between the button and the popup, so an immediate close would dismiss it before the pointer
        // could cross into the card. The popup's PointerEntered cancels this; if the pointer never
        // arrives (the user just moved away from the button), the timer fires and dismisses it — the
        // fix for "the panel stays open after leaving the button without entering it".
        _sessWindowBtn.PointerExited([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
            if (_sessRangeCloseTimer)
            {
                _sessRangeCloseTimer.Start(); // a DispatcherTimer restarts its interval on Start()
            }
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

        // (filtersRow — holding the left-packed controls bar + the right-pinned chip — was appended to
        // the header above; bar/chip live inside it, so nothing more to mount here.)
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
            SessSetTip(_sessFromBox, L"Range start \x2014 list sessions active on or after this date (YYYY-MM-DD). Overrides the time-window preset.");
            card.Children().Append(_sessFromBox);
            _sessToBox = TextBox{};
            _sessToBox.PlaceholderText(L"to: 2026-06-10 (empty = now)");
            _sessToBox.Width(180);
            SessSetTip(_sessToBox, L"Range end \x2014 list sessions last active on or before this date (YYYY-MM-DD); leave empty for now.");
            card.Children().Append(_sessToBox);
            StackPanel actions;
            actions.Orientation(Orientation::Horizontal);
            actions.Spacing(6);
            Button apply;
            apply.Content(winrt::box_value(winrt::hstring{ L"Apply" }));
            SessSetTip(apply, L"Apply the custom From/To range \x2014 re-lists the sessions active within it.");
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
            // Entering the popup cancels the pending close that leaving the button scheduled — the
            // pointer made it across the gap, so keep the card open.
            card.PointerEntered([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
                if (_sessRangeCloseTimer)
                {
                    _sessRangeCloseTimer.Stop();
                }
            });
            card.PointerExited([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
                // Leave the card -> schedule a close (Apply/Preset close it outright). Deferred (not an
                // immediate IsOpen(false)) so sliding back up onto the button re-cancels it via the
                // button's PointerEntered, the symmetric twin of the button-exit path above.
                if (_sessRangeCloseTimer)
                {
                    _sessRangeCloseTimer.Start();
                }
            });

            // The shared hover-intent close timer: a short grace period that bridges the button<->popup
            // gap. Started by either PointerExited, cancelled by either PointerEntered; one tick closes.
            _sessRangeCloseTimer = winrt::Windows::UI::Xaml::DispatcherTimer{};
            _sessRangeCloseTimer.Interval(std::chrono::milliseconds{ 250 });
            _sessRangeCloseTimer.Tick([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&) {
                if (auto self = weak.get())
                {
                    if (self->_sessRangeCloseTimer)
                    {
                        self->_sessRangeCloseTimer.Stop();
                    }
                    if (self->_sessRangePopup)
                    {
                        self->_sessRangePopup.IsOpen(false);
                    }
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
            if (!_sessRenamingId.empty())
            {
                return; // an in-place title editor owns the keyboard — don't hijack Up/Down for row nav
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

        // Slow-double-click TITLE EDIT (the Windows-Explorer rename gesture): re-clicking an
        // already-selected row arms this timer; if no fast double-tap (= resume) intervenes within
        // the system double-click time, it begins the in-place editor. The interval matches the OS so
        // the gesture feels native; a row DoubleTapped disarms it (a double-click resumes, never
        // renames). One-shot — the Tick stops it.
        _sessRenameArmTimer = winrt::Windows::UI::Xaml::DispatcherTimer{};
        _sessRenameArmTimer.Interval(std::chrono::milliseconds{ static_cast<int64_t>(::GetDoubleClickTime()) });
        _sessRenameArmTimer.Tick([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&) {
            auto self = weak.get();
            if (!self)
            {
                return;
            }
            if (self->_sessRenameArmTimer)
            {
                self->_sessRenameArmTimer.Stop(); // one-shot
            }
            const std::wstring id = self->_sessRenamePendingId;
            self->_sessRenamePendingId.clear();
            // Begin only if the armed row is still the selection and nothing else is mid-edit (a
            // double-tap would have disarmed us; a moved selection means the user went elsewhere).
            if (!id.empty() && self->_sessionsSelectedId == id && self->_sessRenamingId.empty())
            {
                self->_BeginSessionsRename(id);
            }
        });
    }

    void TerminalPage::_ShowSessionsPage()
    {
        // Nav audit BEGIN: the user opened the Sessions browser (the toolbar "Sessions" button). The build
        // is DEFERRED (the page-open crash class below), so log BEFORE it — an open-begin with no matching
        // "shown" pinpoints a crash building/showing the page.
        ::Agentmaster::LogNav(L"sessions-page open-begin");
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
            ::Agentmaster::LogNav(L"sessions-page shown"); // END (pairs with open-begin): the page is up; rows render async next
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
                // Drop any in-flight title editing so a re-open starts clean (no stray pending arm,
                // no half-built editor surviving the collapse).
                self->_DisarmSessionsRenameTimer();
                self->_sessRenamingId.clear();
                self->_sessRenameBox = nullptr;
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
            r.contextTokens = e.stats.contextTokens; // the compact "Ctx" column (the sidecar index carries it for EVERY on-disk session)
            // The custom To bound filters on last activity (the From bound rode the enumeration).
            if (toMs > 0 && r.lastActivityMs > toMs)
            {
                continue;
            }
            rows.push_back(std::move(r));
            entries.push_back(std::move(e));
        }

        // Durable-title overlay (SessionStore): a session we have a STORED title for — set in any
        // window when we launched / renamed / restored it (Rule #11), and kept across windows + runs
        // even after it closed — shows that title instead of the transcript-derived one, and folds it
        // into the search haystack (liveTitle) so a historical session is findable by the name we gave
        // it. ONE sparse dir scan (only titled sessions have a file); fine off-thread. The registry-
        // LIVE override below (UI thread) still wins for sessions open right now (the freshest tab
        // title). rows[i] <-> entries[i] are built 1:1 above.
        const auto storedTitles = ::Agentmaster::LoadAllStoredSessionTitles();
        if (!storedTitles.empty())
        {
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const auto it = storedTitles.find(rows[i].id);
                if (it != storedTitles.end() && !it->second.empty())
                {
                    rows[i].title = it->second;
                    if (i < entries.size())
                    {
                        entries[i].liveTitle = it->second;
                    }
                }
            }
        }
        // Favorites (FAVORITES.md): the durable star set, the same SessionStore — ONE sparse dir scan
        // (only favorited/titled sessions have a file). Drives the ★ column + the "Favorite" filter.
        // Assigned to the member on the foreground resume below (this is the background pass).
        auto favorites = ::Agentmaster::LoadAllFavoriteSessions();
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
        self->_sessionsFavorites = std::move(favorites); // FAVORITES.md: the ★ set drives the star column + the Favorite filter
        // A session OPEN in any Agentmaster window (live in the process-wide registry) shows its
        // REAL tab title — SessionInfo.title, the ONE value Explorer name / tab / persistence all
        // share (Rule #11) — instead of the transcript-derived PickDisplayTitle: an in-app rename
        // is reflected here, and a same-dir clash reads its tab name. Baked into the stored rows
        // (here, on the UI thread — the registry is a `this` member, off-limits to the background
        // gather) so display, sort, the detail pane, AND fork-naming all agree on it. The SAME real
        // title is mirrored onto the matching index entry's liveTitle overlay so the 🏷 title search
        // finds an open session by the name shown (rows ↔ entries are built 1:1; the id guard keeps
        // it safe either way). An on-disk (archived / never-opened) session keeps its derived title.
        // Registry Get is mutex-guarded.
        if (self->_sessionRegistry)
        {
            for (size_t i = 0; i < self->_sessionsRows.size(); ++i)
            {
                auto& r = self->_sessionsRows[i];
                const auto reg = self->_sessionRegistry->Get(r.id);
                if (!(reg && reg->live && !reg->title.empty()))
                {
                    continue;
                }
                r.title = reg->title;
                if (i < self->_sessionsEntries.size() && self->_sessionsEntries[i].sessionId == r.id)
                {
                    self->_sessionsEntries[i].liveTitle = reg->title; // make the live tab title searchable (🏷)
                }
            }
        }
        self->_RunSessionsSearch(); // re-applies the current query (incl. the empty one) + renders
    }

    // The two-phase search. FAST runs inline (in-memory over the index entries); the history
    // accelerator + the SLOW content phase run on a background pass, generation-cancelled.
    winrt::fire_and_forget TerminalPage::_RunSessionsSearch()
    {
        const uint64_t gen = ++_sessionsSearchGen;

        ::Agentmaster::SessionQuery q;
        q.text = _sessionsQueryText;
        // scopeTitle defaults ON (SessionQuery default true): a null button can't silently drop it.
        q.scopeTitle = !_sessScopeTitleBtn || (_sessScopeTitleBtn.IsChecked() && _sessScopeTitleBtn.IsChecked().Value());
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

        // Nav audit (debounced — one per settled query, not per keystroke): the query, the active
        // scopes, and the fast-phase hit count. The companion "search-done … content=N" below lands
        // when the background content scan finishes. Together they show exactly what a search
        // surfaced — the context that was missing when "browse" turned up an unexpected session.
        if (!q.text.empty())
        {
            std::wstring scopes;
            if (q.scopeTitle) scopes += L"title,";
            if (q.scopeUser) scopes += L"user,";
            if (q.scopeAgent) scopes += L"agent,";
            if (q.scopeDirs) scopes += L"dirs,";
            if (q.scopeFiles) scopes += L"files,";
            if (q.fuzzy) scopes += L"fuzzy,";
            if (!scopes.empty()) scopes.pop_back();
            ::Agentmaster::LogNav(L"sessions search q=\"" + q.text + L"\" scopes=[" + scopes + L"] fast=" + std::to_wstring(_sessionsFastIds.size()));
        }

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
        ::Agentmaster::LogNav(L"sessions search-done q=\"" + q.text + L"\" content=" + std::to_wstring(self->_sessionsHitCounts.size()) + L" (slow phase)"); // the content-scan matches just landed + re-sorted the table
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
        // An in-place title editor is live (the box exists) — suppress the re-render so a background
        // search/refresh tick can't tear the editor out mid-edit (the Manager's _RebuildTree guard).
        // _BeginSessionsRename nulls _sessRenameBox first, so the render that BUILDS the editor passes;
        // _Commit/_CancelSessionsRename clear _sessRenamingId, so the exit render passes too.
        if (!_sessRenamingId.empty() && _sessRenameBox)
        {
            return;
        }
        const int64_t now = SessNowMs();
        const bool searching = !_sessionsQueryText.empty();
        // The Hits column counts CONTENT matches (the slow 👤/🤖 phase). Those scopes default OFF, so a
        // plain title/dir/path search runs the fast phase only and produces NO hit counts — leaving the
        // column header with empty cells under it ("Hits appears not working"). So show the Hits column
        // ONLY when a content scope is active (== the only time it has data); otherwise it's hidden and
        // Ctx is the rightmost column.
        const bool contentScope =
            (_sessScopeUserBtn && _sessScopeUserBtn.IsChecked() && _sessScopeUserBtn.IsChecked().Value()) ||
            (_sessScopeAgentBtn && _sessScopeAgentBtn.IsChecked() && _sessScopeAgentBtn.IsChecked().Value());
        const bool showHits = searching && contentScope;
        // Relevance-tier ranking (name match on top) is the DEFAULT-VIEW behavior — it must NOT override
        // an EXPLICIT column sort, or clicking a header while searching appears to do nothing ("sorting
        // stopped working"). So apply tiers only while the sort is on the DEFAULT column (Active, col 6 —
        // either direction); the moment the user picks ANY other column (Created/Msgs/Ctx/Title/…), the
        // pure column sort governs. Active is the default sort, so a fresh search still ranks the best
        // match first (the case the tiers were added for) while every other column sorts as asked. Tying
        // it to the column (not its asc/desc) avoids a flip-flop where toggling Active's arrow also
        // toggled the tiers.
        const bool rankByRelevance = searching && _sessionsSortColumn == 6;

        // --- sortable header ---
        _sessionsHeaderRow.Children().Clear();
        _sessionsHeaderRow.ColumnDefinitions().Clear();
        SessAddColumns(_sessionsHeaderRow, showHits);
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
            const bool leftAlign = (col == 2 || col == 3 || col == 4); // Title, Directory, Branch (left, matching their left-rendered data cells; after the ★ shift, FAVORITES.md)
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
                        self->_sessionsSortAscending = (col == 2 || col == 3 || col == 4); // Title/Dir/Branch asc; time/counts desc (after the ★ shift)
                    }
                    self->_RenderSessionsTable();
                });
            });
            Grid::SetColumn(b, col);
            _sessionsHeaderRow.Children().Append(b);
        };
        addHeader(0, L"", false, L"Favorite \x2014 click the star to keep / find a session (the star column).");
        addHeader(1, L"", false, L"Working-directory color \x00B7 solid = open now, dim = on disk");
        addHeader(2, L"Title", true, L"Session title \x2014 its first prompt, or a custom/AI title. Click to sort.");
        addHeader(3, L"Directory", true, L"The session's working directory. Click to sort.");
        addHeader(4, L"Branch", true, L"Git branch the session was on. Click to sort.");
        addHeader(5, L"Created", true, L"When the session was first created. Click to sort.");
        addHeader(6, L"Active", true, L"When the session was last active. Click to sort.");
        addHeader(7, L"Msgs\x00B7Tools", true, L"User messages \x00B7 tool calls. Click to sort.");
        addHeader(8, L"Ctx", true, L"Context \x2014 tokens in the session's newest turn (input + cache + output), the same value the Triage Board shows as \x201C" L"ctx N\x201D. Blank until the first assistant reply. Click to sort.");
        addHeader(9, showHits ? winrt::hstring{ L"Hits" } : winrt::hstring{ L"" }, false, showHits ? winrt::hstring{ L"Number of content matches (\U0001F464/\U0001F916 scopes) in this session" } : winrt::hstring{ L"" });

        // --- the visible set: window rows ∩ the current search result (fast ∪ content hits),
        // minus the user's "Hide from list" set. This render is the single chokepoint both the
        // empty-query and search paths flow through, so filtering hidden ids here covers both;
        // resetting from the Settings cog just re-renders (the rows stay in _sessionsRows). A
        // hidden session is untouched on disk — purely a browse-list preference. ---
        const std::unordered_set<std::wstring> hidden(_appSettings.hiddenSessionIds.begin(), _appSettings.hiddenSessionIds.end());
        // "Hidden" reveal filter: OFF (default) drops hidden ids from the view; ON keeps them (shown
        // dimmed, with an "Unhide" menu). Either way they're tallied for the "N hidden" note.
        const bool showHidden = _sessHiddenBtn && _sessHiddenBtn.IsChecked() && _sessHiddenBtn.IsChecked().Value();
        // "Open" filter: when checked, keep only sessions live in the process-wide registry (open in
        // any Agentmaster window — the same `reg->live` the solid chip reflects). Registry Get is
        // mutex-guarded; queried per row only while the filter is on.
        const bool openOnly = _sessOpenOnlyBtn && _sessOpenOnlyBtn.IsChecked() && _sessOpenOnlyBtn.IsChecked().Value();
        // "Favorite" filter (FAVORITES.md): when checked, keep only favorited sessions (the ★ column /
        // SessionStore "favorite" key, loaded into _sessionsFavorites off-thread). In-memory; composes
        // AND with the search text + the Open/Hidden toggles + the row-filter facets (this chokepoint).
        // Favorites obey the time window like any row — there is NO all-time bypass (FAVORITES.md §3).
        const bool favOnly = _sessFavOnlyBtn && _sessFavOnlyBtn.IsChecked() && _sessFavOnlyBtn.IsChecked().Value();
        // The row right-click "Filter" facets (dir / branch / created-time bucket / fork family) AND
        // with everything above. Computed once; an inactive filter is a no-op (Any() == false).
        const bool rowFilterActive = _sessionsRowFilter.Any();
        // Ids referenced as a fork parent by a gathered row — used per row to decide whether to offer
        // "Filter \xBB By Fork Family" (a row has lineage when it is a fork, or something forked from it).
        std::unordered_set<std::wstring> forkParents;
        for (const auto& r : _sessionsRows)
        {
            if (!r.forkedFromId.empty())
            {
                forkParents.insert(r.forkedFromId);
            }
        }
        int hiddenInWindow = 0;
        std::vector<const _SessionsRow*> view;
        for (const auto& r : _sessionsRows)
        {
            if (!hidden.empty() && hidden.count(r.id))
            {
                ++hiddenInWindow;
                if (!showHidden)
                {
                    continue; // hidden by the "Hide from list" set — reveal with the "Hidden" filter
                }
            }
            if (rowFilterActive && !_SessionsRowPassesRowFilter(r))
            {
                continue; // dropped by an active right-click "Filter" facet (AND with search + toggles)
            }
            if (openOnly)
            {
                const auto reg = _sessionRegistry ? _sessionRegistry->Get(r.id) : std::nullopt;
                if (!(reg && reg->live))
                {
                    continue; // not open in any window — hidden by the "Open" filter
                }
            }
            if (favOnly && _sessionsFavorites.count(r.id) == 0)
            {
                continue; // not favorited — hidden by the "Favorite" filter (FAVORITES.md)
            }
            if (!searching || _sessionsFastIds.count(r.id) || _sessionsHitCounts.count(r.id))
            {
                view.push_back(&r);
            }
        }
        _sessionsVisibleOrder.clear(); // rebuilt below in final (sorted) order — the Up/Down nav list

        // Agentmaster (relevance ranking): when a search is active, order the results in TIERS —
        // a match in the session's NAME/identity (its displayed title, branch, or a pasted id)
        // ranks ABOVE a match only in its working dir / touched paths (tier 1), which ranks above a
        // match found ONLY in the conversation content (tier 2, the slow phase). The user's chosen
        // column sort applies WITHIN each tier. Without this, the column sort alone (default:
        // last-activity desc) floats a recently-active INCIDENTAL content match above the exact name
        // match the user searched for: searching "browse" surfaced an unrelated, just-active session
        // that merely mentions "browse" once in its transcript ABOVE the session actually titled
        // "Browse …" — and a click/resume/fork then acted on the wrong (top) row. Tier 0 reuses the
        // SAME match primitives the fast phase uses (ParseSessionQuery / MatchesQueryText / the guid
        // identity rule), restricted to the name fields, so "found it by name" lands on top.
        std::unordered_map<std::wstring, int> relevance;
        if (rankByRelevance)
        {
            const auto terms = ::Agentmaster::ParseSessionQuery(_sessionsQueryText);
            const bool qFuzzy = _sessFuzzyBtn && _sessFuzzyBtn.IsChecked() && _sessFuzzyBtn.IsChecked().Value();
            relevance.reserve(view.size());
            for (const auto* rp : view)
            {
                const auto& r = *rp;
                // Tier 0 — the NAME: EVERY term matches the displayed title, the branch, or (a guid
                // term) the session's own / fork-parent id (the "found it by name" tier). Empty terms
                // (a whitespace-only query) can't be a name match — every row then falls to tier 1
                // (the fast phase returns the whole window), so ordering is unchanged.
                bool nameMatch = !terms.empty();
                if (nameMatch)
                {
                    const std::wstring titleLower = ::Agentmaster::FoldLower(r.title);
                    const std::wstring branchLower = ::Agentmaster::FoldLower(r.branch);
                    const std::wstring idLower = ::Agentmaster::FoldLower(r.id);
                    const std::wstring forkLower = ::Agentmaster::FoldLower(r.forkedFromId);
                    for (const auto& t : terms)
                    {
                        const bool fz = ::Agentmaster::TermIsFuzzy(t, qFuzzy);
                        const bool hit =
                            (t.isGuid && (t.textLower == idLower || (!forkLower.empty() && t.textLower == forkLower))) ||
                            ::Agentmaster::MatchesQueryText(titleLower, t.textLower, fz) ||
                            ::Agentmaster::MatchesQueryText(branchLower, t.textLower, fz);
                        if (!hit)
                        {
                            nameMatch = false;
                            break;
                        }
                    }
                }
                // Tier 1 — a fast (structured) match that ISN'T the name: cwd or a tool-touched path.
                // Tier 2 — content-only: present in the slow-phase hit counts but not the fast set.
                const int tier = nameMatch ? 0 : (_sessionsFastIds.count(r.id) ? 1 : 2);
                relevance.emplace(r.id, tier);
            }
        }

        const int sortCol = _sessionsSortColumn;
        const bool asc = _sessionsSortAscending;
        std::sort(view.begin(), view.end(), [sortCol, asc, rankByRelevance, &relevance](const _SessionsRow* a, const _SessionsRow* b) {
            if (rankByRelevance)
            {
                // Relevance tier dominates the column sort (more-relevant tier first, independent of
                // the column's asc/desc) so the name match the user searched for can't be buried
                // under a recently-active incidental content match. Within a tier, the column sort
                // below decides order. A row missing from the map (shouldn't happen) sorts last.
                // Only while the sort is the pristine default (rankByRelevance) — an explicit column
                // click drops the tiers so the chosen sort governs purely (the "sorting stopped" fix).
                const auto ra = relevance.find(a->id);
                const auto rb = relevance.find(b->id);
                const int ta = ra != relevance.end() ? ra->second : 3;
                const int tb = rb != relevance.end() ? rb->second : 3;
                if (ta != tb)
                {
                    return ta < tb;
                }
            }
            const auto cmpS = [](const std::wstring& x, const std::wstring& y) {
                const auto lx = ::Agentmaster::FoldLower(x), ly = ::Agentmaster::FoldLower(y);
                return lx < ly ? -1 : (lx > ly ? 1 : 0);
            };
            const auto cmpI = [](int64_t x, int64_t y) { return x < y ? -1 : (x > y ? 1 : 0); };
            int c = 0;
            switch (sortCol) // col indices after the leftmost ★ column shifted everything +1 (FAVORITES.md)
            {
            case 2:
                c = cmpS(a->title, b->title);
                break;
            case 3:
                c = cmpS(a->dir, b->dir);
                break;
            case 4:
                c = cmpS(a->branch, b->branch);
                break;
            case 5:
                c = cmpI(a->createdMs, b->createdMs);
                break;
            case 6:
                c = cmpI(a->lastActivityMs, b->lastActivityMs);
                break;
            case 7:
                c = cmpI(a->msgs, b->msgs);
                break;
            case 8:
                c = cmpI(a->contextTokens, b->contextTokens);
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
            SessAddColumns(g, showHits);
            g.Padding(Thickness{ 6, 4, 6, 4 });

            // chip: the per-dir color (the SAME color the session's tab wears — answer "color
            // coding of the tab"), solid for an OPEN session, dim for an on-disk one; presence
            // (busy/waiting) rendered as the tooltip + a brighter ring.
            const auto reg = _sessionRegistry ? _sessionRegistry->Get(r.id) : std::nullopt;
            const bool live = reg && reg->live;
            const auto* pres = presenceFor(r.id);
            // The per-dir color (the same color the session's tab + the chip wear): its persisted
            // dir-colors.json entry, else the deterministic auto color. Drives BOTH the live chip
            // below AND the colored underline under the title, so a row reads its folder identity
            // the way its terminal tab does.
            const auto dirHex = ::Agentmaster::GetDirColor(r.dir);
            const auto dirColor = SessHexToColor(dirHex ? *dirHex : ::Agentmaster::AutoDirColorHex(r.dir));

            // ★ favorite (col 0, FAVORITES.md): hollow ☆ normally, filled yellow ★ when favorited.
            // This cell stays HIT-TESTABLE (the other cells are made clickthrough below) so a click
            // toggles the durable star (SessionStore) without selecting the row. A near-invisible
            // Background makes the whole 22px cell a click target (the splitter-bar trick).
            {
                const bool fav = _sessionsFavorites.count(r.id) != 0;
                auto starGlyph = SessText(winrt::hstring{ fav ? L"\x2605" : L"\x2606" }, 14, false, fav ? 1.0 : 0.45);
                starGlyph.HorizontalAlignment(HorizontalAlignment::Center);
                if (fav)
                {
                    starGlyph.Foreground(SessBrush(0xFF, 0xF5, 0xC2, 0x42)); // filled yellow
                }
                Border starCell;
                starCell.Background(SessBrush(0x01, 0x80, 0x80, 0x80)); // ~invisible yet hit-testable (full-cell target)
                starCell.Child(starGlyph);
                starCell.HorizontalAlignment(HorizontalAlignment::Stretch);
                starCell.VerticalAlignment(VerticalAlignment::Stretch);
                SessSetTip(starCell, winrt::hstring{ fav ? L"Favorited \x2014 click to unfavorite" : L"Click to favorite (keep / find this session)" });
                const std::wstring sid = r.id;
                starCell.PointerPressed([this, sid](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
                    e.Handled(true); // toggle only — don't fall through to the row's select handler
                    if (const auto b = s.try_as<Border>())
                    {
                        SessCloseTipsIn(b);
                    }
                    Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), sid]() {
                        if (auto self = weak.get())
                        {
                            self->_ToggleSessionFavorite(sid);
                        }
                    });
                });
                Grid::SetColumn(starCell, 0);
                g.Children().Append(starCell);
            }
            {
                Border chip;
                chip.Width(10);
                chip.Height(10);
                chip.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 5, 5, 5, 5 });
                chip.VerticalAlignment(VerticalAlignment::Center);
                chip.HorizontalAlignment(HorizontalAlignment::Center);
                chip.Background(dirColor ? SolidColorBrush{ *dirColor } : SessBrush(0xFF, 0x60, 0x60, 0x60));
                chip.Opacity(live ? 1.0 : 0.35);
                // The chip's status / presence / fork text rides the ONE consolidated row tooltip
                // (built below) instead of a per-cell tip — the chip is decorative and made
                // clickthrough (IsHitTestVisible below). Keep ONLY the visual presence ring here.
                chip.IsHitTestVisible(false);
                if (pres)
                {
                    chip.BorderBrush(SessBrush(0xFF, 0xE8, 0xC0, 0x60)); // claude's busy/idle/waiting heartbeat ring
                    chip.BorderThickness(Thickness{ 1.5, 1.5, 1.5, 1.5 });
                }
                Grid::SetColumn(chip, 1);
                g.Children().Append(chip);
            }

            // title — UNDERLINED with the working-directory color (the same persisted dir-colors.json
            // color the chip + the session's terminal tab wear), so the name carries its folder identity
            // at a glance. The rule rides a Left-aligned Border that hugs the text — a short title gets a
            // short underline, a long ellipsized one fills the column — with VerticalAlignment::Center so
            // it sits under the glyphs, not at the row's bottom edge. The dim for an on-disk row is on the
            // BRUSH (not the Border) so the title text keeps its own live/archived opacity.
            // Full-strength rule for a live row; a translucent one (alpha ~0.6) for an on-disk row,
            // baked into the brush's alpha — NOT the Border's Opacity, which would also fade the text.
            // (Computed before the title/editor branch — the Directory cell below reuses this brush.)
            const uint8_t ulAlpha = live ? 0xFF : 0x99;
            auto underline = dirColor ? SessBrush(ulAlpha, dirColor->R, dirColor->G, dirColor->B) : SessBrush(ulAlpha, 0x60, 0x60, 0x60);
            if (!_sessRenamingId.empty() && _sessRenamingId == r.id)
            {
                // In-place TITLE editor (the Manager's Explorer-tree rename idiom; a ContentDialog is
                // ruled out — a TextBox inside one gets no keypresses under XAML Islands). Commit on
                // Enter / focus-loss, cancel on Escape; both DEFERRED so the re-render the commit
                // triggers doesn't tear this box out mid-keystroke. Focus + select-all on Loaded so
                // the first keystroke replaces the old name.
                auto box = TextBox{};
                box.Text(winrt::hstring{ r.title });
                box.FontSize(12);
                box.Padding(Thickness{ 4, 1, 4, 1 });
                box.VerticalAlignment(VerticalAlignment::Center);
                box.HorizontalAlignment(HorizontalAlignment::Stretch);
                box.KeyDown([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
                    if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
                    {
                        e.Handled(true);
                        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() { if (auto self = weak.get()) { self->_CommitSessionsRename(); } });
                    }
                    else if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
                    {
                        e.Handled(true);
                        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() { if (auto self = weak.get()) { self->_CancelSessionsRename(); } });
                    }
                });
                box.LostFocus([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() { if (auto self = weak.get()) { self->_CommitSessionsRename(); } });
                });
                box.Loaded([](const winrt::Windows::Foundation::IInspectable& s, const RoutedEventArgs&) {
                    if (const auto tb = s.try_as<TextBox>())
                    {
                        tb.Focus(FocusState::Programmatic);
                        tb.SelectAll();
                    }
                });
                _sessRenameBox = box;
                Grid::SetColumn(box, 2);
                g.Children().Append(box);
            }
            else
            {
                auto title = SessText(winrt::hstring{ (r.fork ? L"\x2442 " : L"") + r.title }, 12, false, live ? 1.0 : 0.85);
                Border titleWrap;
                titleWrap.Child(title);
                titleWrap.HorizontalAlignment(HorizontalAlignment::Left);
                titleWrap.VerticalAlignment(VerticalAlignment::Center);
                titleWrap.BorderBrush(underline);
                titleWrap.BorderThickness(Thickness{ 0, 0, 0, 2 });
                titleWrap.Padding(Thickness{ 0, 0, 0, 1 }); // a hair of gap between the descenders and the rule
                titleWrap.IsHitTestVisible(false); // clickthrough -> the row Border is the one click target + tooltip
                Grid::SetColumn(titleWrap, 2);
                g.Children().Append(titleWrap);
            }

            auto dir = SessText(winrt::hstring{ r.dir }, 11, false, 0.6);
            // Same working-directory-color underline as the title (reusing the row's `underline`
            // brush) — the Directory cell is literally the folder, so it wears the folder's color too.
            Border dirWrap;
            dirWrap.Child(dir);
            dirWrap.HorizontalAlignment(HorizontalAlignment::Left);
            dirWrap.VerticalAlignment(VerticalAlignment::Center);
            dirWrap.BorderBrush(underline);
            dirWrap.BorderThickness(Thickness{ 0, 0, 0, 2 });
            dirWrap.Padding(Thickness{ 0, 0, 0, 1 });
            dirWrap.IsHitTestVisible(false);
            Grid::SetColumn(dirWrap, 3);
            g.Children().Append(dirWrap);

            auto branch = SessText(winrt::hstring{ r.branch }, 11, false, 0.6);
            branch.IsHitTestVisible(false);
            Grid::SetColumn(branch, 4);
            g.Children().Append(branch);

            auto created = SessText(winrt::hstring{ SessAgo(r.createdMs, now) }, 11, false, 0.6);
            created.HorizontalAlignment(HorizontalAlignment::Center);
            created.IsHitTestVisible(false);
            Grid::SetColumn(created, 5);
            g.Children().Append(created);

            auto active = SessText(winrt::hstring{ SessAgo(r.lastActivityMs, now) }, 11, false, 0.75);
            active.HorizontalAlignment(HorizontalAlignment::Center);
            active.IsHitTestVisible(false);
            Grid::SetColumn(active, 6);
            g.Children().Append(active);

            const std::wstring weight = std::to_wstring(r.msgs) + L"\x00B7" + std::to_wstring(r.tools);
            auto w = SessText(winrt::hstring{ weight }, 11, false, 0.6);
            w.HorizontalAlignment(HorizontalAlignment::Center);
            w.IsHitTestVisible(false);
            Grid::SetColumn(w, 7);
            g.Children().Append(w);

            // Ctx (col 8): the compact context-token count (the board's "ctx N" value, without the
            // prefix). Blank until an assistant turn carries usage (0), matching the board card. The
            // cell is clickthrough — the whole-row Border owns the one tooltip + click target.
            auto ctx = SessText(winrt::hstring{ r.contextTokens > 0 ? SessFormatTokens(r.contextTokens) : L"" }, 11, false, 0.6);
            ctx.HorizontalAlignment(HorizontalAlignment::Center);
            ctx.IsHitTestVisible(false);
            Grid::SetColumn(ctx, 8);
            g.Children().Append(ctx);

            // Hits (col 9): the content-match count — shown only when a content scope (👤/🤖) is active
            // (showHits), the only time the column is reserved + populated.
            if (showHits)
            {
                const auto hit = _sessionsHitCounts.find(r.id);
                if (hit != _sessionsHitCounts.end())
                {
                    auto h = SessText(winrt::hstring{ std::to_wstring(hit->second) }, 11, true, 0.9);
                    h.HorizontalAlignment(HorizontalAlignment::Center);
                    h.IsHitTestVisible(false);
                    Grid::SetColumn(h, 9);
                    g.Children().Append(h);
                }
            }

            // A row is in the hidden set only when the "Hidden" reveal filter is on (else it was
            // dropped from `view` above). Dim it as a visual cue that it's normally hidden.
            const bool rHidden = !hidden.empty() && hidden.count(r.id) != 0;

            // Agentmaster: every cell EXCEPT the ★ star is made CLICKTHROUGH (IsHitTestVisible(false)
            // above), and the content grid `g` itself has no Background (so its gaps pass through), so
            // the row Border below is the ONE click target + ONE tooltip surface for the whole row —
            // killing both the per-cell tooltip flicker and fragmented row selection. The ★ cell is the
            // one interactive child (FAVORITES.md): it keeps its own hit-testing + tip so a click there
            // toggles the favorite. (Previously the whole grid went clickthrough in one shot, before the
            // star column added an interactive child.)

            Border rowB;
            rowB.Child(g);
            rowB.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            rowB.Background(r.id == _sessionsSelectedId ? SessBrush(0x30, 0x60, 0xA0, 0xE0) : SessBrush(0x14, 0xFF, 0xFF, 0xFF));
            if (rHidden)
            {
                rowB.Opacity(0.55); // revealed-but-hidden cue (the "Hidden" filter is on)
            }
            rowB.Tag(winrt::box_value(winrt::hstring{ r.id }));
            // ONE consolidated row tooltip — folds in every field the per-cell tips used to show (identity,
            // full path, branch, exact created / last-active moments, the msgs/tools/size weight, and the
            // live/archived/on-disk + presence status), so no information is lost. Because it lives on the
            // single (child-free, clickthrough-content) row Border, hovering anywhere on the row shows the
            // full picture at once and never flickers.
            {
                std::wstring rowTip{ r.title };
                if (r.fork)
                {
                    rowTip += L"\n\x2442 fork of " + r.forkedFromId.substr(0, 8);
                }
                rowTip += L"\nSession id: " + r.id;
                rowTip += L"\n" + r.dir;
                if (!r.branch.empty())
                {
                    rowTip += L"\nBranch: " + r.branch;
                }
                if (const auto cabs = SessLocalDateTime(r.createdMs); !cabs.empty())
                {
                    rowTip += L"\nCreated " + cabs;
                }
                if (const auto aabs = SessLocalDateTime(r.lastActivityMs); !aabs.empty())
                {
                    rowTip += L"\nLast active " + aabs;
                }
                rowTip += L"\n" + std::to_wstring(r.msgs) + L" messages \x00B7 " + std::to_wstring(r.tools) + L" tool calls \x00B7 " + std::to_wstring(r.sizeBytes / 1024) + L" KB";
                if (r.contextTokens > 0)
                {
                    rowTip += L"\nContext: " + SessFormatTokens(r.contextTokens) + L" (" + std::to_wstring(r.contextTokens) + L" tokens, newest turn)";
                }
                rowTip += L"\n";
                rowTip += live ? L"Open in this app now" : (reg ? L"Closed \x2014 resume from here" : L"On disk \x2014 not opened in this app");
                rowTip += L"\nDot color = working-directory color (matches its tab)";
                if (pres)
                {
                    rowTip += L"\nClaude is " + pres->status;
                }
                SessSetTip(rowB, winrt::hstring{ rowTip });
            }
            rowB.PointerPressed([this](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
                const auto b = s.try_as<Border>();
                if (!b)
                {
                    return;
                }
                const std::wstring id{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") };
                if (!_sessRenamingId.empty() && _sessRenamingId == id)
                {
                    return; // this row is being edited — let the in-place box own the click (don't re-select)
                }
                SessCloseTipsIn(b); // a click dismisses the row's tip (the standard behavior the islands stack drops)
                e.Handled(true);
                // Slow-double-click TITLE EDIT (the Windows-Explorer rename gesture): a click that
                // RE-clicks the already-selected row arms a short timer; if no fast double-tap (=
                // resume) lands within the OS double-click time, _BeginSessionsRename fires. Reading
                // _sessionsSelectedId synchronously here is correct for the slow case (the prior
                // click's deferred select has long since run); a fast double-click disarms via
                // DoubleTapped below regardless. Any other click disarms a stale pending arm first.
                const bool wasSelected = (_sessionsSelectedId == id);
                _DisarmSessionsRenameTimer();
                if (wasSelected && _sessRenamingId.empty())
                {
                    _ArmSessionsRenameTimer(id);
                }
                // Defer: selection re-renders the detail pane (tree mutation) — the page's crash class.
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), id]() {
                    if (auto self = weak.get())
                    {
                        if (!self->_sessRenamingId.empty() && self->_sessRenamingId != id)
                        {
                            self->_CommitSessionsRename(); // an edit on another row is in flight — commit it before switching (a Border click didn't steal the editor's focus)
                        }
                        self->_sessionsSelectedId = id;
                        // Nav audit: which session the user looked at + WHY it's in the result set
                        // (name/dir = fast match, content = slow phase). This is the line that would
                        // have made the "browse surfaced the wrong session" report self-evident.
                        const std::wstring via = self->_sessionsQueryText.empty() ? std::wstring{ L"browse" } : (self->_sessionsFastIds.count(id) ? std::wstring{ L"name/dir" } : (self->_sessionsHitCounts.count(id) ? std::wstring{ L"content" } : std::wstring{ L"?" }));
                        ::Agentmaster::LogNav(L"sessions select " + ::Agentmaster::ShortId(id) + L" via=" + via + (self->_sessionsQueryText.empty() ? std::wstring{} : (L" q=\"" + self->_sessionsQueryText + L"\"")));
                        self->_UpdateSessionsSelectionHighlight(); // recolor only — no table rebuild per click
                        self->_ShowSessionsDetail(id);
                        self->_PrefetchSessionsSummaries(id, 0); // warm the upper + lower neighbor in the background
                    }
                });
            });
            rowB.DoubleTapped([this](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::DoubleTappedRoutedEventArgs& e) {
                _DisarmSessionsRenameTimer(); // a fast double-click is a RESUME, never a rename — cancel the pending slow-double-click arm
                const auto b = s.try_as<Border>();
                if (!b)
                {
                    return;
                }
                const std::wstring id{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") };
                if (!_sessRenamingId.empty() && _sessRenamingId == id)
                {
                    return; // this row is being edited — the box owns double-clicks (word select), not resume
                }
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

                // Edit Title — the durable per-session title (persisted in the SessionStore; for an
                // OPEN session it routes through the live rename so the tab + Explorer/board lens track
                // it, Rule #11). Opens the SAME in-place editor the slow-double-click gesture does — a
                // ContentDialog text box gets no keypresses under XAML Islands, so editing is inline.
                MenuFlyoutItem editTitle;
                editTitle.Text(L"Edit Title");
                SessSetTip(editTitle, L"Rename this session \x2014 edits the title in place and remembers it (persisted per session).");
                editTitle.Click([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid]() {
                        if (auto self = weak.get())
                        {
                            self->_BeginSessionsRename(rid);
                        }
                    });
                });
                rowMenu.Items().Append(editTitle);

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

                // --- Filter \xBB : narrow the list to sessions LIKE this one. Each facet ANDs with the
                // search box + the scope/Open/Hidden toggles (all applied together at this render's
                // chokepoint), and facets STACK across dimensions. A facet the anchor row already
                // matches is shown \x2713 and clicking it clears that facet (so each item is a clean
                // toggle). Picking a different time granularity replaces the time facet. Every item
                // defers one tick (the MenuFlyout restores focus + the action re-renders — the page's
                // pointer-handler discipline). The facets are pure browse-state; nothing is persisted. ---
                {
                    rowMenu.Items().Append(MenuFlyoutSeparator{});
                    MenuFlyoutSubItem filterSub;
                    filterSub.Text(L"Filter");
                    SessSetTip(filterSub, L"Narrow the list to sessions like this one \x2014 combines (AND) with the search box and the other filters.");
                    const auto addFacet = [&](_SessionsRowFilterKind kind, const std::wstring& label, const std::wstring& tip) {
                        MenuFlyoutItem it;
                        const bool active = _SessionsRowFilterMatchesAnchor(static_cast<int>(kind), r);
                        it.Text(winrt::hstring{ (active ? L"\x2713 " : L"") + label });
                        SessSetTip(it, winrt::hstring{ tip });
                        const int kindInt = static_cast<int>(kind);
                        // Defer the apply+render one tick off the menu click (the page's pointer-handler
                        // discipline — a synchronous tree rebuild mid-click is unsafe under XAML Islands).
                        // The crash that dogged this feature was NOT here — it was the chip's repeated
                        // tooltip wiring (fixed in AgentSetTip); the render path is the same proven one
                        // Hide/Resume use.
                        it.Click([this, kindInt, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), kindInt, rid]() {
                                if (auto self = weak.get())
                                {
                                    self->_ApplySessionsRowFilter(kindInt, rid);
                                }
                            });
                        });
                        filterSub.Items().Append(it);
                    };
                    addFacet(_SessionsRowFilterKind::SameDirectory, L"By Same Directory", L"Show only sessions whose working directory is this one (matched filesystem-aware). Click again on a matching row to clear.");
                    if (!r.branch.empty())
                    {
                        addFacet(_SessionsRowFilterKind::SameBranch, L"By Same Branch", L"Show only sessions on this git branch (\x201C" + r.branch + L"\x201D).");
                    }
                    filterSub.Items().Append(MenuFlyoutSeparator{});
                    addFacet(_SessionsRowFilterKind::SameDay, L"By Same Day", L"Show only sessions CREATED on the same calendar day as this one.");
                    addFacet(_SessionsRowFilterKind::SameWeek, L"By Same Week", L"Show only sessions CREATED in the same week (Monday\x2013Sunday) as this one.");
                    addFacet(_SessionsRowFilterKind::SameMonth, L"By Same Month", L"Show only sessions CREATED in the same calendar month as this one.");
                    if (r.fork || forkParents.count(r.id) != 0)
                    {
                        filterSub.Items().Append(MenuFlyoutSeparator{});
                        addFacet(_SessionsRowFilterKind::ForkFamily, L"By Fork Family", L"Show only this conversation together with its forks and fork-parent (the whole fork family within the listed window).");
                    }
                    if (_sessionsRowFilter.Any())
                    {
                        filterSub.Items().Append(MenuFlyoutSeparator{});
                        MenuFlyoutItem clearItem;
                        clearItem.Text(L"Clear filters");
                        SessSetTip(clearItem, L"Remove every active row filter.");
                        clearItem.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                                if (auto self = weak.get())
                                {
                                    self->_ClearSessionsRowFilter();
                                }
                            });
                        });
                        filterSub.Items().Append(clearItem);
                    }
                    rowMenu.Items().Append(filterSub);
                }

                rowMenu.Items().Append(MenuFlyoutSeparator{});

                // Favorite / Unfavorite (FAVORITES.md) — toggles the durable star (SessionStore), the
                // SAME toggle as clicking the ★ column or the session tab's right-click menu.
                {
                    const bool fav = _sessionsFavorites.count(rid) != 0;
                    MenuFlyoutItem favItem;
                    favItem.Text(fav ? L"Unfavorite" : L"Favorite");
                    SessSetTip(favItem, winrt::hstring{ fav ? L"Remove the star \x2014 stop keeping this session in your favorites." : L"Star this session \x2014 keep it in your favorites (filter with the \x2605 Favorite checkbox)." });
                    favItem.Click([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid]() {
                            if (auto self = weak.get())
                            {
                                self->_ToggleSessionFavorite(rid);
                            }
                        });
                    });
                    rowMenu.Items().Append(favItem);
                }

                rowMenu.Items().Append(MenuFlyoutSeparator{});

                // Hide / Unhide — toggles AppSettings.hiddenSessionIds. A revealed hidden row (only
                // visible while the "Hidden" filter is on) offers Unhide; every other row offers Hide.
                MenuFlyoutItem hideItem;
                if (rHidden)
                {
                    hideItem.Text(L"Unhide");
                    SessSetTip(hideItem, L"Bring this session back into the list (it was hidden, or auto-hidden when its tab was deleted).");
                    hideItem.Click([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid]() {
                            if (auto self = weak.get())
                            {
                                self->_UnhideSessionFromList(rid);
                            }
                        });
                    });
                }
                else
                {
                    hideItem.Text(L"Hide from list");
                    SessSetTip(hideItem, L"Hide this session from the list \x2014 it stays on disk and can be brought back from Settings, or shown again with the \x201CHidden\x201D filter.");
                    hideItem.Click([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), rid]() {
                            if (auto self = weak.get())
                            {
                                self->_HideSessionFromList(rid);
                            }
                        });
                    });
                }
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
            if (openOnly)
            {
                counts += L" \x00B7 open only"; // the "Open" filter is active (mirrors the "N hidden" note)
            }
            if (favOnly)
            {
                counts += L" \x00B7 favorites"; // FAVORITES.md: the "Favorite" filter is active
            }
            if (rowFilterActive)
            {
                counts += L" \x00B7 filtered"; // a row right-click "Filter" facet is narrowing the set (the chip shows which)
            }
            if (hiddenInWindow > 0)
            {
                // "N hidden" normally; "N hidden (shown)" while the reveal filter includes them.
                counts += L" \x00B7 " + std::to_wstring(hiddenInWindow) + (showHidden ? L" hidden (shown)" : L" hidden"); // resettable in Settings
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
        if (const auto it = _sessionsSummaryCache.find(row->id); it != _sessionsSummaryCache.end() && it->second.mtime == row->lastActivityMs)
        {
            // Warm cache — a prior view OR a background prefetch already analyzed this transcript;
            // render instantly, no spinner. (An empty analyze is cached as empty => nothing shown.)
            if (!it->second.text.empty())
            {
                SessAppendSummaryBox(_sessionsDetailHost, it->second.text);
            }
        }
        else
        {
            // Cold: show a loading icon in the summary area and analyze off-thread. The load's
            // completion re-renders this pane (cache hit => the box replaces the spinner). The call
            // dedupes against an in-flight prefetch of the same id.
            _sessionsDetailHost.Children().Append(SessSummaryLoading());
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
        // Dedupe: one analyze per id at a time — a foreground select and a background prefetch can
        // both target the same row; collapse them so the transcript is read once. The completion
        // below re-renders the detail IFF this id is the one currently selected, regardless of which
        // call started the load, so a prefetch finishing after the user lands on it clears its spinner.
        if (_sessionsSummaryLoading.count(sessionId))
        {
            co_return;
        }
        if (const auto it = _sessionsSummaryCache.find(sessionId); it != _sessionsSummaryCache.end() && it->second.mtime == mtime)
        {
            co_return; // already warm (a prior view or a completed prefetch)
        }
        _sessionsSummaryLoading.insert(sessionId);
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
        self->_sessionsSummaryLoading.erase(sessionId);
        self->_sessionsSummaryCache[sessionId] = _SessionsSummaryEntry{ mtime, std::move(text) };
        // Bound the cache over a long browse (each entry is a rendered box — up to tens of KB): past
        // a cap, keep only the current selection; neighbors re-prefetch on the next navigation.
        if (self->_sessionsSummaryCache.size() > 128)
        {
            std::optional<_SessionsSummaryEntry> keep;
            if (const auto sel = self->_sessionsSummaryCache.find(self->_sessionsSelectedId); sel != self->_sessionsSummaryCache.end())
            {
                keep = sel->second;
            }
            self->_sessionsSummaryCache.clear();
            if (keep)
            {
                self->_sessionsSummaryCache[self->_sessionsSelectedId] = std::move(*keep);
            }
        }
        // Re-render IFF this id is the row the user is looking at — replaces its spinner with the box.
        // A prefetch that completes for a non-selected row just warms the cache (no UI churn).
        if (self->_sessionsSelectedId == sessionId && self->_sessionsPageVisible.load(std::memory_order_relaxed))
        {
            self->_ShowSessionsDetail(sessionId);
        }
    }

    // PREFETCH adjacent rows' summaries into the background cache so navigation lands on a warm cache
    // (instant render, no spinner) — the "smooth experience" ask. direction +1 = the next two rows (a
    // Down look-ahead), -1 = the previous two (Up), 0 = the immediate upper + lower neighbor (a click).
    // No wrap at the ends (a wrapped row isn't "adjacent"); each not-yet-warm target kicks a background
    // _LoadSessionsSummary, which itself dedupes against an in-flight/cached load. UI thread only.
    void TerminalPage::_PrefetchSessionsSummaries(const std::wstring& anchorId, int direction)
    {
        if (anchorId.empty() || _sessionsVisibleOrder.empty())
        {
            return;
        }
        const int n = static_cast<int>(_sessionsVisibleOrder.size());
        int idx = -1;
        for (int i = 0; i < n; ++i)
        {
            if (_sessionsVisibleOrder[i] == anchorId)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            return;
        }
        // Two rows in the travel direction, or the two neighbors for a click (direction 0).
        int targets[2];
        if (direction > 0)
        {
            targets[0] = idx + 1;
            targets[1] = idx + 2;
        }
        else if (direction < 0)
        {
            targets[0] = idx - 1;
            targets[1] = idx - 2;
        }
        else
        {
            targets[0] = idx - 1;
            targets[1] = idx + 1;
        }
        for (const int t : targets)
        {
            if (t < 0 || t >= n)
            {
                continue; // off the ends
            }
            const std::wstring& tid = _sessionsVisibleOrder[t];
            // Already warm? (cache key = id, validated by lastActivityMs.) Else find the row's dir +
            // mtime and kick the background analyze (deduped inside _LoadSessionsSummary).
            for (const auto& r : _sessionsRows)
            {
                if (r.id != tid)
                {
                    continue;
                }
                const auto cit = _sessionsSummaryCache.find(tid);
                if (cit == _sessionsSummaryCache.end() || cit->second.mtime != r.lastActivityMs)
                {
                    _LoadSessionsSummary(r.id, r.dir, r.lastActivityMs);
                }
                break;
            }
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
        // Nav audit: the user picked this ROW to resume (right-click "Resume here", the detail-pane
        // button, or a double-click prompt). The clicked id is what they SEE; the continuation
        // resolve below + the downstream [resume]/[restore-fresh] carry what actually launched.
        ::Agentmaster::LogNav(L"sessions resume-click row=" + ::Agentmaster::ShortId(sessionId) + L" \"" + title.substr(0, 80) + L"\" dir=" + dir + (_openClaudeTabInBackground ? L" [bg]" : L""));
        const std::wstring target = _ResolveRestoreChainTail(sessionId, dir, title); // follow /clear chain to the tail
        const auto existing = _sessionRegistry->Get(target);
        if (existing && existing->live)
        {
            // Already OPEN somewhere in this app. Foreground: jump to it. Background bulk-open: leave
            // focus where it is (don't yank to an already-open tab) — it's already there to switch to.
            ::Agentmaster::LogNav(L"sessions resume -> jump " + ::Agentmaster::ShortId(target) + (target == sessionId ? L"" : (L" (resolved from " + ::Agentmaster::ShortId(sessionId) + L")")) + L" (already open)");
            if (!_openClaudeTabInBackground)
            {
                _ActivateClaudeSession(winrt::hstring{ target });
            }
            return;
        }
        // Native-exe-only policy gate (auto-recovering): everything past here LAUNCHES a claude (seed an
        // archived-shaped record, then _RestoreArchivedSession --resume), so gate + prompt here — BEFORE
        // seeding a phantom record we'd otherwise leave behind. The already-live branch above only jumps
        // to an existing tab, so it stays ungated.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::LogNav(L"sessions resume-done (blocked \x2014 no native claude.exe; install prompt shown)"); // pair the BEGIN so an unpaired begin always means a crash, never the gate
            _PromptClaudeMissing();
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
        const auto resumedTab = _RestoreArchivedSession(winrt::hstring{ target });
        // Nav audit END (pairs with the resume-click BEGIN above): the actually-opened id. Normally ==
        // target; a resume whose transcript vanished degrades to a FRESH id (Rule #6), reported here. A
        // resume-click with neither a "resume -> jump" nor a resume-done means the launch crashed mid-open.
        const std::wstring resumedId = resumedTab ? _ClaudeSessionForTab(resumedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"sessions resume-done " + (resumedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(resumedId) + (resumedId == target ? std::wstring{} : (L" (target was " + ::Agentmaster::ShortId(target) + L" \x2014 fresh)")))));
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
        // Nav audit: the user picked this ROW to fork. The clicked id is what they SEE; the
        // continuation resolve below logs the actual fork SOURCE, and the downstream [fork] line
        // carries the NEW forked id ("[fork] <new> (forked from <source>)") — so grepping [nav]+[fork]
        // gives the full row-clicked -> source -> new-id chain the user asked to be able to follow.
        ::Agentmaster::LogNav(L"sessions fork-click row=" + ::Agentmaster::ShortId(parentId) + L" \"" + title.substr(0, 80) + L"\" dir=" + dir + (_openClaudeTabInBackground ? L" [bg]" : L""));
        // Native-exe-only policy gate (auto-recovering): a fork is a claude launch (--fork-session).
        // _LaunchClaudeSession's backstop is SILENT, so gate + prompt here to surface the install notice.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::LogNav(L"sessions fork-done (blocked \x2014 no native claude.exe; install prompt shown)"); // pair the BEGIN so an unpaired begin always means a crash, never the gate
            _PromptClaudeMissing();
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
        const auto forkedTab = _LaunchClaudeSession(winrt::hstring{ forkDir }, winrt::hstring{ ttl }, std::nullopt, forkFrom);
        // Nav audit END (pairs with the fork-click BEGIN above): the NEW forked id — exactly "the id of the
        // new forked session" the user asked to be able to trace, alongside the source it forked from. A
        // fork-click with NO matching fork-done means the launch crashed/hung between the two (the
        // crash-resilience the begin/end pairing buys). The id is resolved from the just-created tab, so it
        // reflects what actually opened (a no-transcript parent degrades to a FRESH session, noted here).
        const std::wstring forkedId = forkedTab ? _ClaudeSessionForTab(forkedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"sessions fork-done " + (forkedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(forkedId))) + L" from=" + ::Agentmaster::ShortId(forkParentId) + (forkFrom.empty() ? std::wstring{ L" (fresh \x2014 no parent transcript)" } : std::wstring{}));
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
        dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
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
        _DisarmSessionsRenameTimer(); // moving the selection by keyboard cancels a pending slow-double-click arm
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
        ::Agentmaster::LogNav(L"sessions select " + ::Agentmaster::ShortId(_sessionsSelectedId) + L" via=key" + (delta > 0 ? L"\x2193" : L"\x2191") + (_sessionsQueryText.empty() ? std::wstring{} : (L" q=\"" + _sessionsQueryText + L"\"")));
        _UpdateSessionsSelectionHighlight();
        _ShowSessionsDetail(_sessionsSelectedId);
        // PREFETCH in the direction of travel so the FOLLOWING Up/Down lands on a warm cache —
        // Down warms the two rows below, Up the two above (background, deduped, no wrap at the ends).
        _PrefetchSessionsSummaries(_sessionsSelectedId, delta > 0 ? 1 : -1);
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

    // Append an id to AppSettings.hiddenSessionIds via a freshest-disk read-modify-write (so it
    // sticks across restarts AND doesn't clobber another field / another window's concurrent write
    // — the splitter/treeSort pattern) and refresh THIS window's in-memory copy. Idempotent — an
    // already-hidden id is a no-op (no duplicate write); returns true only when it was newly added.
    // NO UI side effects: the caller re-renders. Shared by the Sessions-page row right-click "Hide
    // from list" AND the auto-hide-on-delete seam (_RemoveSessionRecord). The transcript on disk is
    // NEVER touched — a browse-list preference only.
    bool TerminalPage::_AddSessionIdToHiddenList(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return false;
        }
        auto s = ::Agentmaster::LoadAppSettings();
        bool changed = false;
        if (std::find(s.hiddenSessionIds.begin(), s.hiddenSessionIds.end(), sessionId) == s.hiddenSessionIds.end())
        {
            s.hiddenSessionIds.push_back(sessionId);
            ::Agentmaster::SaveAppSettings(s);
            changed = true;
        }
        _appSettings.hiddenSessionIds = s.hiddenSessionIds;
        return changed;
    }

    // Row right-click "Hide from list": persist the id (the RMW above) and re-render the table (the
    // render chokepoint filters it out, unless the "Hidden" reveal filter is on). Resettable from
    // the Settings cog. Already-hidden ids are a no-op.
    void TerminalPage::_HideSessionFromList(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        ::Agentmaster::LogNav(L"sessions hide " + ::Agentmaster::ShortId(sessionId));
        _AddSessionIdToHiddenList(sessionId);
        // If the hidden row was selected, drop the selection so the detail pane doesn't keep
        // showing a session that's no longer in the list.
        if (_sessionsSelectedId == sessionId)
        {
            _sessionsSelectedId.clear();
            _ShowSessionsDetail(_sessionsSelectedId); // -> "Select a session"
        }
        _RenderSessionsTable();
    }

    // Row right-click "Unhide" — the inverse of Hide, offered on a row that is currently in the
    // hidden set (only reachable while the "Hidden" reveal filter shows it). Drop the id from
    // AppSettings.hiddenSessionIds (freshest-disk RMW, mirroring Hide) so the session returns to the
    // list normally, refresh the in-memory copy, and re-render. A no-op if the id wasn't hidden.
    void TerminalPage::_UnhideSessionFromList(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        ::Agentmaster::LogNav(L"sessions unhide " + ::Agentmaster::ShortId(sessionId));
        auto s = ::Agentmaster::LoadAppSettings();
        const auto it = std::find(s.hiddenSessionIds.begin(), s.hiddenSessionIds.end(), sessionId);
        if (it != s.hiddenSessionIds.end())
        {
            s.hiddenSessionIds.erase(it);
            ::Agentmaster::SaveAppSettings(s);
        }
        _appSettings.hiddenSessionIds = s.hiddenSessionIds;
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

    // FAVORITES.md: flip the durable star (SessionStore "favorite" key). The CURRENT state is read
    // from disk (authoritative), so this is correct whether called from the Sessions page (the ★
    // column / row menu) or the session tab's right-click menu (where the page — and _sessionsFavorites
    // — may not even be loaded). The in-memory set is kept in step for an open page, then re-render
    // (a no-op when the page was never built, so the tab-menu path costs nothing extra).
    void TerminalPage::_ToggleSessionFavorite(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        const bool nowFav = !::Agentmaster::IsSessionFavorite(sessionId); // flip the on-disk truth
        ::Agentmaster::SetSessionFavorite(sessionId, nowFav);
        ::Agentmaster::LogNav(L"favorite " + ::Agentmaster::ShortId(sessionId) + (nowFav ? L" on" : L" off"));
        if (nowFav)
        {
            _sessionsFavorites.insert(sessionId);
        }
        else
        {
            _sessionsFavorites.erase(sessionId);
        }
        _RefreshTabFavoriteCrown(sessionId); // FAVORITES.md: live-update the tab-strip crown if this window hosts the session
        _RenderSessionsTable(); // refresh the ★ glyph + (if the Favorite filter is on) the visible set
    }

    // ===== in-place TITLE editing (Edit Title / slow-double-click) ==========================
    // The Sessions browser lets you rename a session in place — the durable per-session TITLE the
    // SessionStore keeps (the SAME store the favorite star uses). Two entry points: the row
    // right-click "Edit Title" and the Windows-Explorer "slow double-click" gesture (re-click the
    // already-selected row). Both swap the row's Title cell for a focused TextBox (a ContentDialog
    // text box gets no keypresses under XAML Islands — the Manager's in-place rename idiom). Commit
    // persists the trimmed title; for a session known to the registry it routes through the live
    // rename so the open tab + Explorer/board lens track it (Rule #11).

    void TerminalPage::_BeginSessionsRename(const std::wstring& sessionId)
    {
        if (sessionId.empty() || !_sessionsRowsHost)
        {
            return;
        }
        if (!_sessRenamingId.empty())
        {
            if (_sessRenamingId == sessionId)
            {
                return; // already editing this row
            }
            _CommitSessionsRename(); // a different row is already mid-edit — commit it before switching (clicking a Border doesn't steal the editor's focus, so LostFocus wouldn't have fired)
        }
        _DisarmSessionsRenameTimer(); // any pending slow-double-click arm is now consumed
        // Make the target row the selection so the editor renders in a known, visible row and the
        // detail pane tracks it (the right-click path may fire on a row that wasn't selected yet).
        _sessionsSelectedId = sessionId;
        _sessRenamingId = sessionId;
        _sessRenameBox = nullptr; // bootstrap: the next render builds + focuses the editor (the guard lets THIS render through)
        _RenderSessionsTable(); // colors the row selected + swaps its Title cell for the editor
        _ShowSessionsDetail(sessionId);
    }

    void TerminalPage::_CommitSessionsRename()
    {
        if (_sessRenamingId.empty())
        {
            return; // idempotent: a deferred Enter-commit + the LostFocus that follows must collapse to one
        }
        const std::wstring id = _sessRenamingId;
        std::wstring name = _sessRenameBox ? std::wstring{ _sessRenameBox.Text() } : std::wstring{};
        // Trim surrounding whitespace/newlines — a blank/whitespace edit keeps the old title (matches
        // the Manager's _CommitRename + the WT tab renamer; a title never goes empty, Rule #11).
        const auto first = name.find_first_not_of(L" \t\r\n");
        const auto last = name.find_last_not_of(L" \t\r\n");
        name = (first == std::wstring::npos) ? std::wstring{} : name.substr(first, last - first + 1);

        _sessRenamingId.clear();
        _sessRenameBox = nullptr;
        if (!name.empty())
        {
            _PersistEditedSessionTitle(id, name);
        }
        // Exit edit mode: the Title cell renders as text again. With an active query, re-run the
        // search so the new title's membership + ranking update (the 🏷 title scope); otherwise a
        // plain re-render is enough. Refresh the detail header either way.
        if (!_sessionsQueryText.empty())
        {
            _RunSessionsSearch();
        }
        else
        {
            _RenderSessionsTable();
        }
        _ShowSessionsDetail(_sessionsSelectedId);
    }

    void TerminalPage::_CancelSessionsRename()
    {
        if (_sessRenamingId.empty())
        {
            return;
        }
        _sessRenamingId.clear();
        _sessRenameBox = nullptr;
        _RenderSessionsTable(); // back to the text Title cell, nothing written
    }

    // The persistence core. Rule #11 — the title is ONE value. A session KNOWN to the process-wide
    // registry (open OR archived) goes through the live rename seam: it writes SessionInfo.title
    // (-> the Engine observer mirrors it to <profile>/session-store/<sid>.json AND sessions.json) and
    // re-pins the WT tab when the session is open in this window. A pure ON-DISK session (never
    // managed by this app) isn't in the registry, so write the durable store directly — exactly the
    // overlay the Sessions browser reads for closed/historical rows (LoadAllStoredSessionTitles).
    // Either way the new title is reflected into the in-memory rows + the search index entry so the
    // table (display + sort + the 🏷 title search) and the detail header update without a re-gather.
    void TerminalPage::_PersistEditedSessionTitle(const std::wstring& sessionId, const std::wstring& title)
    {
        if (sessionId.empty() || title.empty())
        {
            return;
        }
        ::Agentmaster::LogNav(L"sessions edit-title " + ::Agentmaster::ShortId(sessionId) + L" -> \"" + title.substr(0, 80) + L"\"");
        if (_sessionRegistry && _sessionRegistry->Get(sessionId).has_value())
        {
            _RenameClaudeSession(winrt::hstring{ sessionId }, winrt::hstring{ title }); // registry (+ tab if open); the Engine observer mirrors to the durable store + sessions.json
        }
        else
        {
            ::Agentmaster::SetStoredSessionTitle(sessionId, title); // pure on-disk row — write the durable store directly
        }
        for (size_t i = 0; i < _sessionsRows.size(); ++i)
        {
            if (_sessionsRows[i].id == sessionId)
            {
                _sessionsRows[i].title = title;
                if (i < _sessionsEntries.size() && _sessionsEntries[i].sessionId == sessionId)
                {
                    _sessionsEntries[i].liveTitle = title; // keep the 🏷 title search in step with the edit
                }
                break;
            }
        }
    }

    void TerminalPage::_ArmSessionsRenameTimer(const std::wstring& sessionId)
    {
        if (!_sessRenameArmTimer || sessionId.empty())
        {
            return;
        }
        _sessRenamePendingId = sessionId;
        _sessRenameArmTimer.Stop();
        _sessRenameArmTimer.Start(); // a DispatcherTimer restarts its interval on Start()
    }

    void TerminalPage::_DisarmSessionsRenameTimer()
    {
        _sessRenamePendingId.clear();
        if (_sessRenameArmTimer)
        {
            _sessRenameArmTimer.Stop();
        }
    }

    // ===== the Sessions-page row right-click "Filter" facets ================================
    // A row's right-click "Filter \xBB" submenu narrows the visible set to rows matching the clicked
    // ("anchor") row in some dimension. Each facet ANDs with the search text + the scope/Open/Hidden
    // toggles (all applied together at the _RenderSessionsTable chokepoint), and facets stack across
    // dimensions (dir AND branch AND time-bucket AND fork-family). Picking a facet a row already
    // matches toggles it OFF. Transient browse-state — nothing persisted, nothing touched on disk.

    // The AND-predicate over every active facet: true == keep this row. An inactive facet imposes no
    // constraint. Directory is matched filesystem-aware (NormDirKey, Rule #8); branch is exact (git
    // refs are case-sensitive); a time bucket is the half-open [start, end) created-time window; the
    // fork family is set membership over the precomputed connected component.
    bool TerminalPage::_SessionsRowPassesRowFilter(const _SessionsRow& r) const
    {
        const auto& f = _sessionsRowFilter;
        if (f.hasDir && ::Agentmaster::NormDirKey(f.dir) != ::Agentmaster::NormDirKey(r.dir))
        {
            return false;
        }
        if (f.hasBranch && f.branch != r.branch)
        {
            return false;
        }
        if (f.timeGran != _SessionsRowFilterState::TimeGran::None && !(r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs))
        {
            return false;
        }
        if (f.hasFamily && f.familyIds.count(r.id) == 0)
        {
            return false;
        }
        return true;
    }

    // Does the facet for `kind`'s DIMENSION currently exist AND equal row r's value? Drives both the
    // submenu's \x2713 (so the item that would toggle the active facet OFF reads as "checked") and the
    // apply-toggle direction below. For the time dimension it is granularity-specific: with a WEEK
    // facet active, the SameDay query returns false (so picking SameDay REPLACES week with day).
    bool TerminalPage::_SessionsRowFilterMatchesAnchor(int kind, const _SessionsRow& r) const
    {
        const auto& f = _sessionsRowFilter;
        switch (static_cast<_SessionsRowFilterKind>(kind))
        {
        case _SessionsRowFilterKind::SameDirectory:
            return f.hasDir && ::Agentmaster::NormDirKey(f.dir) == ::Agentmaster::NormDirKey(r.dir);
        case _SessionsRowFilterKind::SameBranch:
            return f.hasBranch && f.branch == r.branch;
        case _SessionsRowFilterKind::SameDay:
            return f.timeGran == _SessionsRowFilterState::TimeGran::Day && r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs;
        case _SessionsRowFilterKind::SameWeek:
            return f.timeGran == _SessionsRowFilterState::TimeGran::Week && r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs;
        case _SessionsRowFilterKind::SameMonth:
            return f.timeGran == _SessionsRowFilterState::TimeGran::Month && r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs;
        case _SessionsRowFilterKind::ForkFamily:
            return f.hasFamily && f.familyIds.count(r.id) != 0;
        }
        return false;
    }

    // The connected component of the fork graph that contains anchorId, over the GATHERED rows: a BFS
    // treating each row's forkedFromId as an UNDIRECTED edge to its parent, so a fork, its parent, and
    // their siblings all land in one family. A parent outside the current window is harmless — it just
    // sits in the set (no such row renders). The anchor is always included (a lone session = {anchor}).
    std::unordered_set<std::wstring> TerminalPage::_ComputeForkFamily(const std::wstring& anchorId) const
    {
        std::unordered_multimap<std::wstring, std::wstring> adj;
        for (const auto& r : _sessionsRows)
        {
            if (!r.forkedFromId.empty())
            {
                adj.emplace(r.id, r.forkedFromId);
                adj.emplace(r.forkedFromId, r.id);
            }
        }
        std::unordered_set<std::wstring> seen{ anchorId }, out;
        std::vector<std::wstring> stack{ anchorId };
        while (!stack.empty())
        {
            const std::wstring cur = std::move(stack.back());
            stack.pop_back();
            out.insert(cur);
            const auto range = adj.equal_range(cur);
            for (auto it = range.first; it != range.second; ++it)
            {
                if (seen.insert(it->second).second)
                {
                    stack.push_back(it->second);
                }
            }
        }
        return out;
    }

    // Toggle the facet for `kind`'s dimension to the anchor row's value — or OFF when the anchor
    // already matches it (so the same submenu item is a clean toggle). Picking a different time
    // granularity REPLACES the time facet (one granularity at a time); the dir/branch/family facets
    // are independent dimensions that coexist as AND. Re-renders + refreshes the chip.
    void TerminalPage::_ApplySessionsRowFilter(int kind, const std::wstring& anchorId)
    {
        const _SessionsRow* a = nullptr;
        for (const auto& r : _sessionsRows)
        {
            if (r.id == anchorId)
            {
                a = &r;
                break;
            }
        }
        if (!a)
        {
            return; // the anchor scrolled out of the gathered set
        }
        auto& f = _sessionsRowFilter;
        const bool alreadyMatches = _SessionsRowFilterMatchesAnchor(kind, *a);
        switch (static_cast<_SessionsRowFilterKind>(kind))
        {
        case _SessionsRowFilterKind::SameDirectory:
            if (alreadyMatches)
            {
                f.hasDir = false;
                f.dir.clear();
            }
            else
            {
                f.hasDir = true;
                f.dir = a->dir;
            }
            break;
        case _SessionsRowFilterKind::SameBranch:
            if (a->branch.empty())
            {
                break; // nothing to pin on
            }
            if (alreadyMatches)
            {
                f.hasBranch = false;
                f.branch.clear();
            }
            else
            {
                f.hasBranch = true;
                f.branch = a->branch;
            }
            break;
        case _SessionsRowFilterKind::SameDay:
        case _SessionsRowFilterKind::SameWeek:
        case _SessionsRowFilterKind::SameMonth:
        {
            if (alreadyMatches)
            {
                f.timeGran = _SessionsRowFilterState::TimeGran::None;
                f.timeStartMs = 0;
                f.timeEndMs = 0;
                f.timeLabel.clear();
                break;
            }
            int gran = 0; // SameDay
            auto wantGran = _SessionsRowFilterState::TimeGran::Day;
            if (static_cast<_SessionsRowFilterKind>(kind) == _SessionsRowFilterKind::SameWeek)
            {
                gran = 1;
                wantGran = _SessionsRowFilterState::TimeGran::Week;
            }
            else if (static_cast<_SessionsRowFilterKind>(kind) == _SessionsRowFilterKind::SameMonth)
            {
                gran = 2;
                wantGran = _SessionsRowFilterState::TimeGran::Month;
            }
            const auto [s, e] = SessLocalBucket(a->createdMs, gran);
            if (s == 0 && e == 0)
            {
                break; // un-bucketable created time
            }
            f.timeGran = wantGran;
            f.timeStartMs = s;
            f.timeEndMs = e;
            f.timeLabel = SessBucketLabel(a->createdMs, gran);
            break;
        }
        case _SessionsRowFilterKind::ForkFamily:
            if (alreadyMatches)
            {
                f.hasFamily = false;
                f.familyIds.clear();
            }
            else
            {
                f.familyIds = _ComputeForkFamily(anchorId);
                f.hasFamily = !f.familyIds.empty();
            }
            break;
        }
        _UpdateSessionsFilterChip();
        _RenderSessionsTable();
    }

    // Drop EVERY facet (the chip click, and the submenu's "Clear filters") + re-render.
    void TerminalPage::_ClearSessionsRowFilter()
    {
        _sessionsRowFilter = _SessionsRowFilterState{};
        _UpdateSessionsFilterChip();
        _RenderSessionsTable();
    }

    // Refresh the "\x2715 filter: \x2026" chip beside the search box — collapsed when no facet is
    // active, else a "\xB7"-joined summary of the active facets (the same dimensions the count line
    // mentions). Clicking the chip clears ALL facets (wired in _BuildSessionsPageShell).
    void TerminalPage::_UpdateSessionsFilterChip()
    {
        if (!_sessFilterChip)
        {
            return;
        }
        const auto& f = _sessionsRowFilter;
        if (!f.Any())
        {
            _sessFilterChip.Visibility(Visibility::Collapsed);
            return;
        }
        std::wstring parts;
        const auto add = [&](const std::wstring& p) {
            if (!parts.empty())
            {
                parts += L" \x00B7 ";
            }
            parts += p;
        };
        if (f.hasDir)
        {
            add(L"dir " + SessLeaf(f.dir));
        }
        if (f.hasBranch)
        {
            add(L"branch " + f.branch);
        }
        if (f.timeGran != _SessionsRowFilterState::TimeGran::None)
        {
            add(f.timeLabel);
        }
        if (f.hasFamily)
        {
            add(L"fork family (" + std::to_wstring(f.familyIds.size()) + L")");
        }
        _sessFilterChip.Content(winrt::box_value(winrt::hstring{ L"\x2715 " + parts }));
        SessSetTip(_sessFilterChip, winrt::hstring{ L"Active row filter (AND-ed with the search) \x2014 " + parts + L". Click to clear it." });
        _sessFilterChip.Visibility(Visibility::Visible);
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
