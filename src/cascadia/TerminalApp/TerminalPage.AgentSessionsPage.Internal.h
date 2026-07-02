// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster TerminalPage implementation (7 partial files)
// The TerminalPage-side Agentmaster glue -- engine wiring + session lifecycle + observer +
// window-record + the Sessions browser -- connecting the Manager engine to WT's TerminalPage.
// Same class (TerminalPage, declared in TerminalPage.h), split across same-class partial TUs (the
// upstream TabManagement.cpp pattern) so TerminalPage.cpp stays close to upstream.
//
// Partial files in this group (★ marks THIS file):
//   TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Manager tab, _WireAgentManagerContent
//   TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
//   TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
//   TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
//   TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
// ★ TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the two SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster: TerminalPage Sessions-browser file-local helpers (SessSetTip / SessAppendSummaryBox /
// SessText / SessBrush / the local date-bucket + preset helpers). Factored out of
// TerminalPage.AgentSessionsPage.cpp so its two partial TUs (.AgentSessionsPage.cpp +
// .AgentSessionsPageActions.cpp) share ONE copy. Kept in the SAME nested ANONYMOUS namespace inside
// the implementation namespace exactly as before (internal linkage, a per-TU copy) -- no behavior
// change. NOT standalone: include AFTER the file-scope using-directives each TU replicates.
#pragma once

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

        // The page's table columns. The bookmark-tags Tags column is the LEFTMOST (a fixed 4-ribbon,
        // HEADER-LESS adornment), then the ★ favorite column, then the status chip + title etc.:
        // 0=Tags (fixed 4-ribbon cell, no header) · 1=★ favorite · 2=color/live chip · 3=Title ·
        // 4=Directory · 5=Branch · 6=Created · 7=Active · 8=Msgs·Tools · 9=Ctx (context tokens) ·
        // 10=Hits (populated while searching).
        void SessAddColumns(Grid& g, bool showHits)
        {
            const auto col = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                g.ColumnDefinitions().Append(c);
            };
            col(44, GridUnitType::Pixel); // tags (leftmost) — FIXED for exactly 4 bookmark ribbons in a row (6.5px each + 2px gaps, centered); >4 shows 3 ribbons + a dim "+N"
            col(22, GridUnitType::Pixel); // ★ favorite (clickable)
            col(26, GridUnitType::Pixel); // chip (color/live status)
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
}
