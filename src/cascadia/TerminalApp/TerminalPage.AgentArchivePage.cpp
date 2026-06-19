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

#include "AgentTipHelpers.h" // AgentSetTip / AgentCloseTipsIn — the shared tooltip-dismissal recipe
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / AgentmasterStateDir (record-file stat) / ClaudeProjectsDir (transcript path)
#include "AgentMaster/Engine.h" // RecoverableWindows
#include "AgentMaster/Persistence.h" // SaveSessions (branch backfill)
#include "AgentMaster/ProcessInspect.h" // TranscriptTimes / ReadTranscriptInfo / EncodeCwdToProjectDir
#include "AgentMaster/SessionRegistry.h" // snapshot + UpdateQuiet + the live-refresh observer
#include "AgentMaster/SessionScanner.h" // ParseTranscriptDelta (the detail pane's last-assistant tail read)

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

        // '\' <-> '/' flipped copy — a dir's search twin, so "k:/source" matches "k:\source" and
        // vice versa (dirs are filesystem paths, not plain text — Rule #8 spirit). Applied to the
        // DIR only, never the filter or branch ('/' is meaningful in "feature/x").
        std::wstring ArchiveFlipSlashes(std::wstring s)
        {
            for (auto& ch : s)
            {
                ch = (ch == L'\\') ? L'/' : (ch == L'/' ? L'\\' : ch);
            }
            return s;
        }

        // Append one lowercased field + a '\n' fence to a row's search blob (the fence stops a
        // token from false-matching across a field boundary). No-op on empty. Blobs are built ONCE
        // per row at gather; _RenderArchiveTable AND-matches the filter's tokens against them.
        void ArchiveAppendBlob(std::wstring& blob, const std::wstring& field)
        {
            if (!field.empty())
            {
                blob += ArchiveLower(field);
                blob += L'\n';
            }
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
        // abbreviate ("3h", "3/7", "W2") — the tooltip carries the full value. TU-local name over
        // the ONE shared dismissal recipe (AgentTipHelpers.h: an explicit ToolTip closed on the
        // owner's PointerExited + Unloaded — ToolTipService's own auto-dismiss is unreliable
        // under XAML Islands).
        void ArchiveSetTip(const winrt::Windows::UI::Xaml::UIElement& el, const std::wstring& tip)
        {
            AgentSetTip(el, winrt::hstring{ tip });
        }

        // Force-close every tooltip under root — for hosts about to Clear() or be HIDDEN
        // (a Visibility toggle doesn't unload; a collapsed host does not hide a popup). The
        // shared walk passes through ScrollViewers (ContentControl) to the rows/detail hosts.
        void ArchiveCloseTipsIn(const winrt::Windows::UI::Xaml::UIElement& root)
        {
            AgentCloseTipsIn(root);
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

        // The transcript .jsonl path for a (cwd, sessionId) — the same composition TranscriptTimesIn
        // uses internally (projects root + encoded cwd + <id>.jsonl), for the detail pane's "Open
        // transcript" action and the last-assistant tail read.
        std::wstring ArchiveTranscriptPath(const std::wstring& cwd, const std::wstring& sessionId)
        {
            if (cwd.empty() || sessionId.empty())
            {
                return {};
            }
            return ::Agentmaster::ClaudeProjectsDir() + L"\\" + ::Agentmaster::EncodeCwdToProjectDir(cwd) + L"\\" + sessionId + L".jsonl";
        }

        // Read up to maxBytes from the END of a file (the head-read twin of ProcessInspect's
        // ReadFileHead, same share flags so a live transcript reads while Claude writes). Empty on
        // failure. The chunk may start mid-line / mid-UTF-8-sequence — callers skip to the first '\n'
        // (0x0A never occurs inside a multi-byte UTF-8 sequence, so that also re-aligns the encoding).
        std::string ArchiveReadFileTail(const std::wstring& path, size_t maxBytes)
        {
            if (path.empty() || maxBytes == 0)
            {
                return {};
            }
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return {};
            }
            LARGE_INTEGER sz{};
            ::GetFileSizeEx(h, &sz);
            const uint64_t size = static_cast<uint64_t>(sz.QuadPart);
            const uint64_t want = std::min<uint64_t>(size, maxBytes);
            LARGE_INTEGER start{};
            start.QuadPart = static_cast<LONGLONG>(size - want);
            std::string bytes;
            if (::SetFilePointerEx(h, start, nullptr, FILE_BEGIN))
            {
                bytes.resize(static_cast<size_t>(want), '\0');
                size_t off = 0;
                while (off < bytes.size())
                {
                    DWORD got = 0;
                    const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(bytes.size() - off, 1u << 20));
                    if (!::ReadFile(h, bytes.data() + off, chunk, &got, nullptr) || got == 0)
                    {
                        break;
                    }
                    off += got;
                }
                bytes.resize(off);
            }
            ::CloseHandle(h);
            return bytes;
        }

        // Clamp a display string to maxLen chars (+ ellipsis), never splitting a surrogate pair.
        std::wstring ArchiveClampText(std::wstring s, size_t maxLen)
        {
            if (s.size() > maxLen)
            {
                s.resize(maxLen);
                if (!s.empty() && s.back() >= 0xD800 && s.back() <= 0xDBFF) // dangling high surrogate
                {
                    s.pop_back();
                }
                s += L" \x2026";
            }
            return s;
        }

        // The LAST assistant message in a transcript-tail byte chunk: skip the partial first line,
        // UTF-8 -> wide, ParseTranscriptDelta (the scanner's pure parser), walk the events backwards
        // for the last non-empty Assistant text. "" when none. ("Where did this conversation leave
        // off?" — the head read can't answer it; ReadTranscriptInfo parses only the file's start.)
        std::wstring ArchiveLastAssistantText(const std::string& tailBytes)
        {
            if (tailBytes.empty())
            {
                return {};
            }
            size_t begin = 0;
            if (const auto nl = tailBytes.find('\n'); nl != std::string::npos)
            {
                begin = nl + 1; // re-align to a line (and UTF-8) boundary; a full-file read keeps 0 fine too
            }
            if (begin >= tailBytes.size())
            {
                return {};
            }
            const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, tailBytes.data() + begin, static_cast<int>(tailBytes.size() - begin), nullptr, 0);
            if (wideLen <= 0)
            {
                return {};
            }
            std::wstring wide(static_cast<size_t>(wideLen), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, tailBytes.data() + begin, static_cast<int>(tailBytes.size() - begin), wide.data(), wideLen);
            const auto parsed = ::Agentmaster::ParseTranscriptDelta(wide);
            for (auto it = parsed.events.rbegin(); it != parsed.events.rend(); ++it)
            {
                if (it->kind == ::Agentmaster::TranscriptEvent::Kind::Assistant && !it->text.empty())
                {
                    return it->text;
                }
            }
            return {};
        }

        // Copy text to the system clipboard (the detail pane's "Copy id" / "Copy path"). Best-effort;
        // Flush so the content survives the app losing focus (it can refuse — non-fatal).
        void ArchiveCopyToClipboard(const std::wstring& text)
        {
            try
            {
                winrt::Windows::ApplicationModel::DataTransfer::DataPackage pkg;
                pkg.RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
                pkg.SetText(winrt::hstring{ text });
                winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(pkg);
                winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            }
            CATCH_LOG();
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

        // Agentmaster (permanent remove): a QUIET trash (Delete) button — the record-only remove twin
        // beside the restore/reopen actions. Subdued background (like the detail's mini-buttons) so it
        // never competes with the positive primary action; the destructive step always confirms first.
        winrt::Windows::UI::Xaml::Controls::Button ArchiveTrashButton(winrt::hstring label)
        {
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;
            Button b;
            StackPanel sp;
            sp.Orientation(Orientation::Horizontal);
            sp.Spacing(6);
            sp.VerticalAlignment(VerticalAlignment::Center);
            FontIcon fi;
            fi.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ L"Segoe Fluent Icons" });
            fi.Glyph(L"\xE74D"); // Delete (trash can)
            fi.FontSize(14);
            sp.Children().Append(fi);
            if (!label.empty())
            {
                sp.Children().Append(ArchiveText(label, 13, false, 0.85));
            }
            b.Content(sp);
            b.Background(ArchiveBrush(0x22, 0x80, 0x80, 0x80)); // quiet — distinct from the accented primary
            return b;
        }

        // Agentmaster: collapse a (now possibly multi-line) title to ONE line for the dense table
        // row — each CR/LF/TAB run becomes a single space. Titles can carry newlines (the rename
        // boxes accept Return); the full form rides the row tooltip (ArchiveSetTip) and the wrapping
        // detail header. Mirrors AgentManagerContent::OneLine (separate TU — no shared header).
        winrt::hstring ArchiveOneLine(std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size());
            bool pendingSpace = false;
            for (const wchar_t c : s)
            {
                if (c == L'\r' || c == L'\n' || c == L'\t')
                {
                    pendingSpace = !out.empty();
                    continue;
                }
                if (pendingSpace)
                {
                    out.push_back(L' ');
                    pendingSpace = false;
                }
                out.push_back(c);
            }
            return winrt::hstring{ out };
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

        // Set the window pointer cursor (the table|detail splitter's ↔ on hover, Arrow on exit).
        // No per-element cursor in this XAML projection (ProtectedCursor needs a subclass), so we
        // drive the CoreWindow cursor — the Manager tab's splitters do the same.
        void ArchiveApplyCursor(winrt::Windows::UI::Core::CoreCursorType type)
        {
            if (const auto w = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
            {
                w.PointerCursor(winrt::Windows::UI::Core::CoreCursor{ type, 0 });
            }
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
        search.PlaceholderText(L"Search title, dir, branch, prompts\x2026");
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

        // --- body: table | detail, divided by the draggable splitter below. The split is seeded
        // from the persisted GLOBAL fraction (AppSettings::archiveSplitFraction, settings.json —
        // written by the splitter's drag release) as STAR ratios, so it is window-size-RELATIVE:
        // resizing the window keeps the proportion, and every window shares one setting. ---
        Grid body;
        body.Margin(Thickness{ 16, 0, 16, 0 });
        {
            const auto bcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                body.ColumnDefinitions().Append(c);
            };
            const double split = _appSettings.archiveSplitFraction; // sane-clamped on load
            bcol(split, GridUnitType::Star); // table
            bcol(0, GridUnitType::Auto); // separator
            bcol(1.0 - split, GridUnitType::Star); // detail
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

        // Splitter — a draggable grab-bar between the table and the detail (replaces the old static
        // 1px separator): pin both star columns' sizes at press, then track the pointer 1:1 (the
        // Manager tab's _MakeSplitter recipe, self-contained — drag state lives in a shared_ptr the
        // handlers capture, so no TerminalPage members). Width writes are LAYOUT-property changes,
        // not tree mutations, so they are safe synchronously inside pointer handlers (this page's
        // defer-rule is about adding/removing elements mid-click). Pointer deltas are read relative
        // to nullptr (the island origin) — only the delta matters, and not capturing an ancestor
        // element in the handlers avoids a parent<->child delegate reference cycle.
        {
            struct SplitDrag
            {
                bool active{};
                double origin{};
                double sizeA{};
                double sizeB{};
            };
            auto drag = std::make_shared<SplitDrag>();
            const auto tableCol = body.ColumnDefinitions().GetAt(0);
            const auto detailCol = body.ColumnDefinitions().GetAt(2);
            const auto idleGrip = ArchiveBrush(0x40, 0x80, 0x80, 0x80);
            const auto hotGrip = ArchiveBrush(0x90, 0xC0, 0xC0, 0xC0);

            Border grip;
            grip.Width(2);
            grip.HorizontalAlignment(HorizontalAlignment::Center);
            grip.VerticalAlignment(VerticalAlignment::Stretch);
            grip.Margin(Thickness{ 0, 10, 0, 10 });
            grip.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 1, 1, 1, 1 });
            grip.Background(idleGrip);

            Border bar;
            bar.Width(10);
            bar.VerticalAlignment(VerticalAlignment::Stretch);
            bar.Margin(Thickness{ 2, 4, 2, 4 });
            bar.Background(ArchiveBrush(0x01, 0x80, 0x80, 0x80)); // ~invisible, yet hit-testable
            bar.Child(grip);

            bar.PointerEntered([grip, hotGrip](const winrt::IInspectable&, const Input::PointerRoutedEventArgs&) {
                ArchiveApplyCursor(CoreCursorType::SizeWestEast);
                grip.Background(hotGrip);
            });
            bar.PointerExited([drag, grip, idleGrip](const winrt::IInspectable&, const Input::PointerRoutedEventArgs&) {
                if (!drag->active) // mid-drag the pointer may leave the thin bar — keep it hot
                {
                    ArchiveApplyCursor(CoreCursorType::Arrow);
                    grip.Background(idleGrip);
                }
            });
            bar.PointerPressed([drag, grip, hotGrip, tableCol, detailCol](const winrt::IInspectable& s, const Input::PointerRoutedEventArgs& e) {
                drag->active = true;
                drag->origin = e.GetCurrentPoint(nullptr).Position().X;
                // ActualWidth is the exact star-space allotment, so star weights set to pixel
                // values land pixel-perfect (the Manager splitter's trick).
                drag->sizeA = tableCol.ActualWidth();
                drag->sizeB = detailCol.ActualWidth();
                if (const auto el = s.try_as<UIElement>())
                {
                    el.CapturePointer(e.Pointer());
                }
                grip.Background(hotGrip);
                ArchiveApplyCursor(CoreCursorType::SizeWestEast);
                e.Handled(true);
            });
            bar.PointerMoved([drag, tableCol, detailCol](const winrt::IInspectable&, const Input::PointerRoutedEventArgs& e) {
                if (!drag->active)
                {
                    ArchiveApplyCursor(CoreCursorType::SizeWestEast); // re-assert while hovering (covers post-release)
                    return;
                }
                const double cur = e.GetCurrentPoint(nullptr).Position().X;
                const double total = drag->sizeA + drag->sizeB;
                constexpr double minPx = 160.0; // keep both halves usable
                if (total < (minPx * 2.0) + 1.0)
                {
                    return; // not enough room to split sensibly
                }
                const double newA = std::clamp(drag->sizeA + (cur - drag->origin), minPx, total - minPx);
                tableCol.Width(GridLengthHelper::FromValueAndType(newA, GridUnitType::Star));
                detailCol.Width(GridLengthHelper::FromValueAndType(total - newA, GridUnitType::Star));
                e.Handled(true);
            });
            const auto endDrag = [drag, grip, idleGrip, tableCol, detailCol, weakThis = get_weak()](const winrt::IInspectable& s, const Input::PointerRoutedEventArgs& e) {
                if (!drag->active)
                {
                    return; // a capture-lost echo of our own release, or a stray event
                }
                drag->active = false; // clear BEFORE releasing capture so the re-entrant CaptureLost no-ops
                if (const auto el = s.try_as<UIElement>())
                {
                    el.ReleasePointerCaptures();
                }
                grip.Background(idleGrip);
                ArchiveApplyCursor(CoreCursorType::Arrow);
                // Persist the split GLOBALLY as the table's FRACTION of the two columns. a/(a+b)
                // is correct whether the weights are still the seed fractions (a stray click, sum
                // == 1.0) or post-drag pixels (sum in the hundreds) — the Manager splitter's
                // trick. Normalizing the columns back to (f, 1-f) star weights keeps the split
                // window-size-relative from here on. Read-modify-write against the freshest
                // settings.json (only this field — minimal clobber, the treeSort pattern), and
                // keep this window's in-memory copy in step so a later cog Save can't regress it.
                const double a = tableCol.Width().Value;
                const double b = detailCol.Width().Value;
                if (a + b > 0.0)
                {
                    const double f = std::clamp(a / (a + b), 0.05, 0.95);
                    tableCol.Width(GridLengthHelper::FromValueAndType(f, GridUnitType::Star));
                    detailCol.Width(GridLengthHelper::FromValueAndType(1.0 - f, GridUnitType::Star));
                    if (const auto self = weakThis.get())
                    {
                        auto s2 = ::Agentmaster::LoadAppSettings();
                        s2.archiveSplitFraction = f;
                        ::Agentmaster::SaveAppSettings(s2);
                        self->_appSettings.archiveSplitFraction = f;
                    }
                }
                e.Handled(true);
            };
            bar.PointerReleased(endDrag);
            bar.PointerCaptureLost(endDrag);

            Grid::SetColumn(bar, 1);
            body.Children().Append(bar);
        }

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
        // Agentmaster (permanent remove): the bulk Delete twin, to the LEFT of Restore (the footer is
        // right-aligned + horizontal, so appending it FIRST places it left — the trash-left vision).
        // Subdued styling; confirms before removing. Record-only — the conversation files on disk are kept.
        _archiveDeleteSelBtn = ArchiveTrashButton(L"Delete selected");
        _archiveDeleteSelBtn.IsEnabled(false);
        ArchiveSetTip(_archiveDeleteSelBtn, L"Permanently remove the checked sessions from Agentmaster \x2014 record-only; their conversation files on disk are kept (still in Sessions)");
        _archiveDeleteSelBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            _PromptDeleteCheckedArchived(); // coroutine: confirm (suspends off this click) -> delete checked ∩ visible -> refresh
        });
        footer.Children().Append(_archiveDeleteSelBtn);
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

        // Agentmaster: register with the GENERIC window-level overlay seam — the tab-switch
        // handler (TabManagement.cpp) dismisses every registered page, so this page (and any
        // future one) closes on tab switch without a page-specific call there. The dismiss
        // hook closes any open tooltip — a popup, which a collapsed host does NOT hide.
        _RegisterAgentPageOverlay(host, &_archivePageVisible, [weak = get_weak()]() {
            if (const auto self = weak.get(); self && self->_archivePageHost)
            {
                ArchiveCloseTipsIn(self->_archivePageHost);
            }
        });

        // Agentmaster: Up/Down = move the selection through the visible rows (wraps; none
        // selected => Down picks the first, Up the last). PREVIEW (tunneling) so it wins over
        // the focused search box; deferred to a clean tick (_ShowArchiveDetail mutates the tree).
        host.PreviewKeyDown([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
            const auto k = e.Key();
            if (k != winrt::Windows::System::VirtualKey::Up && k != winrt::Windows::System::VirtualKey::Down)
            {
                return;
            }
            e.Handled(true);
            const int delta = (k == winrt::Windows::System::VirtualKey::Down) ? 1 : -1;
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), delta]() {
                if (auto self = weak.get())
                {
                    self->_MoveArchiveSelection(delta);
                }
            });
        });

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
            if (self->_archiveSearchBox)
            {
                // Focus INTO the page so keyboard events route through its host (focus left on a
                // covered element keeps them on a sibling branch) — typing filters immediately,
                // Up/Down navigate the rows.
                self->_archiveSearchBox.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
            }
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
                // Tooltips are popups — collapsing the host does NOT hide an open one.
                ArchiveCloseTipsIn(self->_archivePageHost);
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
            // The row's search haystack (tokenized AND-match in _RenderArchiveTable): title / dir
            // (+ its slash-flipped twin) / branch / session id (paste a UUID from hooks.log to find
            // its session), then every queued prompt's label + text below — so a half-remembered
            // prompt ("that fix-the-parser one I queued") finds its session. The transcript's
            // prompts are deliberately NOT here — indexing them would head-read N files at gather
            // (the exact per-row I/O the detail pane's cache exists to avoid); the queue is already
            // in memory.
            ArchiveAppendBlob(r.searchBlob, s.title);
            ArchiveAppendBlob(r.searchBlob, s.workingDir);
            ArchiveAppendBlob(r.searchBlob, ArchiveFlipSlashes(s.workingDir));
            ArchiveAppendBlob(r.searchBlob, s.branch);
            ArchiveAppendBlob(r.searchBlob, s.id);
            for (const auto& p : s.queue)
            {
                ++r.totalCount;
                if (p.status == ::Agentmaster::PromptStatus::Sent)
                {
                    ++r.sentCount;
                }
                ArchiveAppendBlob(r.searchBlob, p.label);
                ArchiveAppendBlob(r.searchBlob, p.text);
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
                // Its search haystack: the synthetic title + the chip tip (composition / geometry /
                // launch mode — "maximized" finds maximized saved windows) + the record's GUID.
                ArchiveAppendBlob(r.searchBlob, r.title);
                ArchiveAppendBlob(r.searchBlob, wtip);
                ArchiveAppendBlob(r.searchBlob, rw.record.windowId);
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
        // Tokenized AND-match over each row's gather-built searchBlob (title / dir + slash-flipped
        // twin / branch / id / queued prompt labels+texts): every whitespace-separated token must
        // appear somewhere in the row, order-free — so "agent parser" finds the session whose dir
        // says agentmaster and whose QUEUED PROMPT says parser. Replaces the verbatim single-
        // substring over a title/dir/branch haystack rebuilt per row per render. An all-whitespace
        // filter tokenizes to nothing => unfiltered, same as empty.
        std::vector<std::wstring> tokens;
        for (size_t i = 0; i < _archiveFilter.size();)
        {
            const size_t j = _archiveFilter.find_first_of(L" \t", i);
            const size_t end = (j == std::wstring::npos) ? _archiveFilter.size() : j;
            if (end > i)
            {
                tokens.emplace_back(_archiveFilter.substr(i, end - i));
            }
            i = end + 1;
        }
        std::vector<const _ArchiveRow*> view;
        for (const auto& r : _archiveRows)
        {
            bool match = true;
            for (const auto& t : tokens)
            {
                if (r.searchBlob.find(t) == std::wstring::npos)
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                view.push_back(&r);
            }
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
        ArchiveCloseTipsIn(_archiveRowsHost); // a re-render under the pointer must not orphan an open tip
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
            auto title = ArchiveText(r.title.empty() ? winrt::hstring{ L"(untitled)" } : ArchiveOneLine(r.title), 13, true, r.windowOnly ? 0.7 : 0.95);
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
            row.Tapped([this, rid](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
                if (const auto b = s.try_as<Border>())
                {
                    ArchiveCloseTipsIn(b); // a click dismisses the row's tip (popup close — not a tree mutation)
                }
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
            // Double-click = the row's PRIMARY action (the detail pane's first button): a session row
            // restores into this window; a synthetic "saved window" row reopens that window. The most
            // common action took select -> travel to the detail pane -> click "Restore here"; this
            // collapses it to one gesture. XAML raises Tapped for the FIRST tap and DoubleTapped for the
            // second, so the first click still selects (above) and the detail pane previews what is
            // about to be restored. DEFERRED like every pointer handler here (restore creates a tab,
            // reopen launches a window — tree mutations, the page's documented crash class). Both seams
            // are re-fire-safe: _RestoreArchivedSession no-ops on a live/unknown id (and the sentinel
            // "window:<guid>" id is unknown by construction), _ReopenSavedWindow on idx < 0.
            {
                const bool isWindowRow = r.windowOnly;
                const int fbIdx = r.windowIndex;
                const std::wstring wid = r.windowId;
                row.DoubleTapped([this, rid, isWindowRow, fbIdx, wid](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::DoubleTappedRoutedEventArgs& e) {
                    e.Handled(true); // consume the gesture — property-only here; the action runs on a clean tick
                    Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), rid, isWindowRow, fbIdx, wid]() {
                        auto self = weak.get();
                        if (!self)
                        {
                            return;
                        }
                        if (isWindowRow)
                        {
                            self->_ReopenSavedWindowById(fbIdx, wid);
                        }
                        else
                        {
                            // Follow a /clear+plan-restart chain to its tail so we restore where the conversation left off.
                            self->_RestoreArchivedSession(winrt::hstring{ self->_ResolveRestoreChainTail(std::wstring{ rid }, L"", L"") });
                        }
                        self->_HideArchivePage();
                    });
                });
            }
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
    // Agentmaster: Up/Down keyboard navigation over the VISIBLE (sorted + filtered) rows
    // (_archiveVisibleOrder — already maintained per render for the bulk-restore ordering). No
    // selection (or one filtered out of view): Down picks the first row, Up the last; otherwise
    // the selection moves ±1 and WRAPS at the ends (rotates). Scrolls the row into view.
    void TerminalPage::_MoveArchiveSelection(int delta)
    {
        if (_archiveVisibleOrder.empty() || !_archiveRowsHost)
        {
            return;
        }
        const int n = static_cast<int>(_archiveVisibleOrder.size());
        int idx = -1;
        if (!_archiveSelectedId.empty())
        {
            for (int i = 0; i < n; ++i)
            {
                if (_archiveVisibleOrder[i] == _archiveSelectedId)
                {
                    idx = i;
                    break;
                }
            }
        }
        const int next = (idx < 0) ? (delta > 0 ? 0 : n - 1) : (((idx + delta) % n + n) % n);
        _archiveSelectedId = _archiveVisibleOrder[next];
        _UpdateArchiveSelectionHighlight();
        _ShowArchiveDetail(_archiveSelectedId);
        // Key-nav scrolls WITHOUT pointer input — a row tip open under the stationary mouse
        // never gets the PointerExited that would close it when its row scrolls away.
        ArchiveCloseTipsIn(_archiveRowsHost);
        for (const auto& child : _archiveRowsHost.Children())
        {
            if (const auto b = child.try_as<winrt::Windows::UI::Xaml::Controls::Border>(); b && std::wstring{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") } == _archiveSelectedId)
            {
                b.StartBringIntoView();
                break;
            }
        }
    }

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
        ArchiveCloseTipsIn(_archiveDetailHost); // a detail re-render under the pointer must not orphan an open tip
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
                // off the in-flight pointer event is the crash class; see "Restore here" below). The stale-index
                // re-resolution lives in _ReopenSavedWindowById (shared with the row double-click gesture).
                Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), fallbackIdx, wid]() {
                    auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    self->_ReopenSavedWindowById(fallbackIdx, wid);
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
            {
                // Permanent remove: delete this saved-window RECORD (geometry + tab list). Record-only —
                // the sessions' conversation files on disk are kept. A simple trash icon (tooltip explains),
                // to the LEFT of Reopen (the vision).
                auto del = ArchiveTrashButton(L"");
                ArchiveSetTip(del, L"Permanently delete this saved window's layout (geometry + tab list). The sessions' conversation files on disk are kept (still in Sessions).");
                const std::wstring wid = row->windowId;
                del.Click([this, wid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    _PromptDeleteArchivedWindow(wid); // coroutine: confirm (suspends off this click) -> DeleteWindowRecord -> refresh
                });
                actions.Children().Append(del);
            }
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
        // The stat outcome stays in scope: it also gates the "Open transcript" action + the
        // last-assistant tail read below (a never-prompted session has no file to open or tail).
        int64_t statCreated = 0, statM = 0;
        const bool transcriptOnDisk = ::Agentmaster::TranscriptTimes(info->workingDir, id, statCreated, statM);
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

        // --- id + read-only file actions row: the truncated conversation id (hover = full) with
        // Copy id / Copy path / Open transcript. All read-only (no-delete design): the copies are
        // clipboard-only, and Open hands the .jsonl to the system opener (never writes/moves it).
        {
            StackPanel idRow;
            idRow.Orientation(Orientation::Horizontal);
            idRow.Spacing(8);
            idRow.Margin(Thickness{ 0, 2, 0, 0 });
            auto idTb = ArchiveText(winrt::hstring{ L"id " + (id.size() > 12 ? id.substr(0, 8) + L"\x2026" : id) }, 11, false, 0.45);
            ArchiveSetTip(idTb, id); // the full conversation UUID
            idRow.Children().Append(idTb);

            // Small, quiet action buttons (property-only click feedback — a content text change never
            // restructures the tree, so these handlers are safe to run synchronously mid-click).
            const auto makeMiniBtn = [](winrt::hstring label) {
                Button b;
                b.Background(ArchiveBrush(0x22, 0x80, 0x80, 0x80));
                b.BorderThickness(Thickness{ 0, 0, 0, 0 });
                b.Padding(Thickness{ 8, 1, 8, 1 });
                b.MinWidth(0);
                b.MinHeight(0);
                b.Content(ArchiveText(label, 11, false, 0.8));
                return b;
            };
            auto copyIdBtn = makeMiniBtn(L"Copy id");
            ArchiveSetTip(copyIdBtn, L"Copy the conversation id");
            {
                const auto fb = copyIdBtn.Content().try_as<TextBlock>();
                copyIdBtn.Click([sid = id, fb](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    ArchiveCopyToClipboard(sid);
                    if (fb)
                    {
                        fb.Text(L"Copied \x2713");
                    }
                });
            }
            idRow.Children().Append(copyIdBtn);
            if (!info->workingDir.empty())
            {
                auto copyPathBtn = makeMiniBtn(L"Copy path");
                ArchiveSetTip(copyPathBtn, L"Copy the working directory path");
                const auto fb = copyPathBtn.Content().try_as<TextBlock>();
                copyPathBtn.Click([dir = std::wstring{ info->workingDir }, fb](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    ArchiveCopyToClipboard(dir);
                    if (fb)
                    {
                        fb.Text(L"Copied \x2713");
                    }
                });
                idRow.Children().Append(copyPathBtn);
            }
            if (transcriptOnDisk)
            {
                const std::wstring tpath = ArchiveTranscriptPath(info->workingDir, id);
                auto openBtn = makeMiniBtn(L"Open transcript");
                ArchiveSetTip(openBtn, L"Open the conversation transcript (read-only)\n" + tpath);
                openBtn.Click([this, tpath](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    _OpenArchiveTranscript(tpath); // fire-and-forget; ShellExecutes off-thread — no tree mutation here
                });
                idRow.Children().Append(openBtn);
            }
            _archiveDetailHost.Children().Append(idRow);
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
        else
        {
            // The transcript prompts used to render HERE as a fallback under the "Flight Plan" header
            // (mislabeled); they now have their own Conversation section below, shown ALWAYS.
            _archiveDetailHost.Children().Append(ArchiveText(_archiveDetailTiPrompts.empty() ? winrt::hstring{ L"(no recorded prompts)" } : winrt::hstring{ L"(no plan recorded)" }, 12, false, 0.5, true));
        }

        // --- Conversation: the transcript's human prompts, shown EVEN when a queue exists — the old
        // else-fallback let the queue hide them, but the queue is only what the manager recorded
        // while the transcript is the conversation's full human history (pre-adoption prompts, other
        // windows, hook-less runs). Capped by the head read (60); each row display-clamped.
        if (!_archiveDetailTiPrompts.empty())
        {
            const size_t n = _archiveDetailTiPrompts.size();
            const std::wstring convHeader = (n >= 60) ?
                                                std::wstring{ L"Conversation (first 60 prompts)" } :
                                                L"Conversation (" + std::to_wstring(n) + (n == 1 ? L" prompt)" : L" prompts)");
            auto h = ArchiveText(winrt::hstring{ convHeader }, 13, true, 0.9);
            h.Margin(Thickness{ 0, 8, 0, 0 });
            ArchiveSetTip(h, L"The transcript's human prompts, in order \x2014 the conversation's full history; the Flight Plan above is only what the manager recorded.");
            _archiveDetailHost.Children().Append(h);
            for (const auto& up : _archiveDetailTiPrompts)
            {
                _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ L"\x2023 " + ArchiveClampText(up, 400) }, 12, false, 0.75, true));
            }
        }

        // --- Last assistant reply: where the conversation left off. The head read can't see the tail,
        // so it is read OFF-THREAD once per (id, mtime) by _LoadArchiveAssistantTail, which re-shows
        // this detail on completion (then the cache hits here). An attempted-but-empty tail caches ""
        // (render nothing) so a reply-less transcript isn't re-read on every re-show.
        if (id == _archiveDetailTailId && statM == _archiveDetailTailMtime)
        {
            if (!_archiveDetailTailText.empty())
            {
                auto h = ArchiveText(L"Last assistant reply", 13, true, 0.9);
                h.Margin(Thickness{ 0, 8, 0, 0 });
                ArchiveSetTip(h, L"The transcript's final assistant message \x2014 where this conversation left off.");
                _archiveDetailHost.Children().Append(h);
                _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ ArchiveClampText(_archiveDetailTailText, 600) }, 12, false, 0.7, true));
            }
        }
        else if (transcriptOnDisk && !_archiveDetailTailPending)
        {
            _archiveDetailTailPending = true;
            _LoadArchiveAssistantTail(id, std::wstring{ info->workingDir }, statM);
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
        {
            // Permanent remove: a simple trash icon (tooltip explains) to the LEFT of Restore (the
            // vision). Record-only — drops the Agentmaster record; the conversation .jsonl on disk is
            // kept (it still appears in Sessions).
            auto del = ArchiveTrashButton(L"");
            ArchiveSetTip(del, L"Permanently remove this session from Agentmaster \x2014 it will NOT be restorable from the Archive. The conversation file on disk is kept (still in Sessions).");
            const std::wstring did{ id };
            del.Click([this, did](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                _PromptDeleteArchivedSession(did); // coroutine: confirm (suspends off this click) -> _DeleteClaudeSession -> refresh
            });
            actions.Children().Append(del);
        }
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
                // Follow a /clear+plan-restart chain to its tail so we restore where the conversation left off.
                self->_RestoreArchivedSession(winrt::hstring{ self->_ResolveRestoreChainTail(std::wstring{ hid }, L"", L"") });
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

    // Agentmaster: reopen a saved window by its STABLE windowId, re-resolving the live record index at
    // action time. A captured windowIndex is the record's slot in the sorted set AT GATHER TIME; if the
    // record set shifted while the page was open (a background window closed, or a record was pruned),
    // that slot now names a DIFFERENT window — so re-resolve against a fresh RecoverableWindows() here.
    // idx<0 (no longer recoverable — already reopened, or deleted) makes _ReopenSavedWindow a safe
    // no-op. (A residual sub-ms cross-process race remains: the spawned agentmaster.exe re-loads records
    // for `-s <idx>`; the full fix is to pass the windowId on the command line — a larger M10-Increment-3
    // change.) Shared by the detail pane's "Reopen its window" and the table's row double-click; callers
    // arrive on a clean dispatcher tick (the launch mutates no tree, but the callers also hide the page).
    void TerminalPage::_ReopenSavedWindowById(int fallbackIndex, const std::wstring& windowId)
    {
        int idx = fallbackIndex;
        if (!windowId.empty())
        {
            idx = -1; // not found -> no-op (don't fall back to a possibly-stale slot)
            try
            {
                for (const auto& rw : ::Agentmaster::RecoverableWindows())
                {
                    if (rw.record.windowId == windowId)
                    {
                        idx = rw.index;
                        break;
                    }
                }
            }
            CATCH_LOG();
            if (idx < 0)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] window " + windowId + L" no longer recoverable (already open or pruned)\n");
            }
        }
        _ReopenSavedWindow(idx);
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
            // Follow each checked session's /clear+plan-restart chain to its tail before restoring (two
            // checked links of one chain collapse to the tail; _RestoreArchivedSession's !live guard dedupes).
            _RestoreArchivedSession(winrt::hstring{ _ResolveRestoreChainTail(id, L"", L"") });
            _archiveChecked.erase(id);
        }
        _HideArchivePage();
    }

    void TerminalPage::_UpdateArchiveBulkButton()
    {
        if (!_archiveRestoreSelBtn && !_archiveDeleteSelBtn)
        {
            return;
        }
        // Agentmaster: count only checked rows that are currently VISIBLE (pass the filter), so the button's
        // "(N)" agrees with what _RestoreCheckedArchived / _PromptDeleteCheckedArchived will actually act on.
        size_t n = 0;
        for (const auto& id : _archiveChecked)
        {
            if (_archiveVisibleIds.find(id) != _archiveVisibleIds.end())
            {
                ++n;
            }
        }
        if (_archiveRestoreSelBtn)
        {
            _archiveRestoreSelBtn.Content(winrt::box_value(n > 0 ?
                                                               winrt::hstring{ L"Restore selected (" + std::to_wstring(n) + L")" } :
                                                               winrt::hstring{ L"Restore selected" }));
            _archiveRestoreSelBtn.IsEnabled(n > 0);
        }
        if (_archiveDeleteSelBtn)
        {
            _archiveDeleteSelBtn.IsEnabled(n > 0); // label stays "Delete selected" (icon+text); the count rides Restore
        }
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

    // Agentmaster (detail pane "last assistant reply"): tail-read the transcript OFF-THREAD (64 KB —
    // ample for the final message), parse it with the scanner's pure ParseTranscriptDelta, keep the
    // LAST non-empty assistant text, then cache it by (id, mtime) and re-show the detail (which now
    // hits the cache and renders the line). An empty result still caches — attempted-but-reply-less
    // must not re-read on every detail re-show. The pending flag (UI-thread-only) stops a re-show
    // that arrives mid-read from kicking a second read of the same transcript.
    winrt::fire_and_forget TerminalPage::_LoadArchiveAssistantTail(std::wstring sessionId, std::wstring dir, int64_t mtime)
    {
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        co_await winrt::resume_background();
        std::wstring text;
        try
        {
            text = ArchiveLastAssistantText(ArchiveReadFileTail(ArchiveTranscriptPath(dir, sessionId), 65536));
        }
        CATCH_LOG();
        co_await wil::resume_foreground(dispatcher);
        if (auto self = weakThis.get())
        {
            self->_archiveDetailTailPending = false;
            self->_archiveDetailTailId = sessionId;
            self->_archiveDetailTailMtime = mtime;
            self->_archiveDetailTailText = std::move(text);
            // Re-render only if this session is still the one on display (the user may have moved on —
            // the next _ShowArchiveDetail of THIS id will hit the cache anyway).
            if (self->_archiveSelectedId == sessionId && self->_archivePageVisible.load(std::memory_order_relaxed))
            {
                self->_ShowArchiveDetail(sessionId);
            }
        }
    }

    // Agentmaster (detail pane "Open transcript"): hand the .jsonl to the system opener — read-only
    // (the no-delete design: the transcript is never written, moved, or deleted; Claude owns it).
    // ShellExecuteExW may block, so dispatch from a background thread (the _ReopenSavedWindow
    // pattern). With no .jsonl association Windows shows its "open with" picker; if the open verb
    // FAILS outright, fall back to revealing the file in Explorer (/select).
    winrt::fire_and_forget TerminalPage::_OpenArchiveTranscript(std::wstring path)
    {
        if (path.empty())
        {
            co_return;
        }
        co_await winrt::resume_background();
        try
        {
            SHELLEXECUTEINFOW seInfo{ 0 };
            seInfo.cbSize = sizeof(seInfo);
            seInfo.fMask = SEE_MASK_NOASYNC;
            seInfo.lpVerb = L"open";
            seInfo.lpFile = path.c_str();
            seInfo.nShow = SW_SHOWNORMAL;
            bool ok = !!ShellExecuteExW(&seInfo);
            if (!ok)
            {
                const std::wstring params = L"/select,\"" + path + L"\"";
                SHELLEXECUTEINFOW fb{ 0 };
                fb.cbSize = sizeof(fb);
                fb.fMask = SEE_MASK_NOASYNC;
                fb.lpVerb = L"open";
                fb.lpFile = L"explorer.exe";
                fb.lpParameters = params.c_str();
                fb.nShow = SW_SHOWNORMAL;
                ok = !!ShellExecuteExW(&fb);
            }
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[archive] open transcript ok=" + std::wstring{ ok ? L"1" : L"0" } + L" " + path + L"\n");
        }
        CATCH_LOG();
        co_return;
    }

    // Agentmaster (Archive page — permanent remove, record-only): confirm, then drop the session's
    // record via _DeleteClaudeSession (the conversation .jsonl on disk is KEPT — it still appears in
    // Sessions). The confirm SUSPENDS this coroutine off the originating click, so no archive-page tree
    // mutation lands mid-pointer-routing (the page's crash class); the refresh runs on the clean
    // continuation. (The registry observer would also poke a refresh — Remove notifies — but the direct
    // call is snappier.) No presenter (UI down) => proceed; the trash click is itself the intent.
    winrt::fire_and_forget TerminalPage::_PromptDeleteArchivedSession(std::wstring sessionId)
    {
        if (sessionId.empty() || !_sessionRegistry)
        {
            co_return;
        }
        std::wstring titleStr;
        if (const auto s = _sessionRegistry->Get(sessionId))
        {
            titleStr = s->title;
        }
        if (const auto presenter{ _dialogPresenter.get() })
        {
            winrt::Windows::UI::Xaml::Controls::ContentDialog dialog;
            dialog.Title(winrt::box_value(winrt::hstring{ L"Delete permanently?" }));
            dialog.Content(winrt::box_value(winrt::hstring{ (titleStr.empty() ? std::wstring{ L"This session" } : (L"\x201C" + titleStr + L"\x201D")) + L" will be removed from Agentmaster. The conversation file on disk is KEPT \x2014 it still appears in Sessions and can be reopened from there." }));
            dialog.PrimaryButtonText(L"\U0001F5D1 Delete");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(winrt::Windows::UI::Xaml::Controls::ContentDialogButton::Close);
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }
            if (result != winrt::Windows::UI::Xaml::Controls::ContentDialogResult::Primary)
            {
                co_return;
            }
        }
        _DeleteClaudeSession(winrt::hstring{ sessionId });
        _RefreshArchivePageIfVisible();
    }

    // Agentmaster (Archive page — permanent remove): delete a SAVED-WINDOW record (geometry + tab list).
    // Record-only: the referenced sessions' conversation files on disk are kept. Confirm, then
    // DeleteWindowRecord + refresh. (RecoverableWindows() reads disk each call, so the row drops out.)
    winrt::fire_and_forget TerminalPage::_PromptDeleteArchivedWindow(std::wstring windowId)
    {
        if (windowId.empty())
        {
            co_return;
        }
        if (const auto presenter{ _dialogPresenter.get() })
        {
            winrt::Windows::UI::Xaml::Controls::ContentDialog dialog;
            dialog.Title(winrt::box_value(winrt::hstring{ L"Delete saved window?" }));
            dialog.Content(winrt::box_value(winrt::hstring{ L"This removes the saved window's layout (geometry + tab list) from Agentmaster. The sessions' conversation files on disk are kept and still appear in Sessions." }));
            dialog.PrimaryButtonText(L"\U0001F5D1 Delete");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(winrt::Windows::UI::Xaml::Controls::ContentDialogButton::Close);
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }
            if (result != winrt::Windows::UI::Xaml::Controls::ContentDialogResult::Primary)
            {
                co_return;
            }
        }
        try
        {
            ::Agentmaster::DeleteWindowRecord(windowId);
        }
        CATCH_LOG();
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[delete] saved window " + windowId + L"\n");
        _RefreshArchivePageIfVisible();
    }

    // Agentmaster (Archive page — permanent remove): bulk delete every CHECKED ∩ VISIBLE session, in
    // table order (mirrors _RestoreCheckedArchived's checked-∩-visible discipline). One confirm for the
    // batch; record-only — the conversation files on disk are kept.
    winrt::fire_and_forget TerminalPage::_PromptDeleteCheckedArchived()
    {
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
            co_return;
        }
        if (const auto presenter{ _dialogPresenter.get() })
        {
            winrt::Windows::UI::Xaml::Controls::ContentDialog dialog;
            dialog.Title(winrt::box_value(winrt::hstring{ L"Delete " + std::to_wstring(ids.size()) + (ids.size() == 1 ? L" session permanently?" : L" sessions permanently?") }));
            dialog.Content(winrt::box_value(winrt::hstring{ L"They will be removed from Agentmaster. Their conversation files on disk are KEPT \x2014 they still appear in Sessions and can be reopened from there." }));
            dialog.PrimaryButtonText(L"\U0001F5D1 Delete");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(winrt::Windows::UI::Xaml::Controls::ContentDialogButton::Close);
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }
            if (result != winrt::Windows::UI::Xaml::Controls::ContentDialogResult::Primary)
            {
                co_return;
            }
        }
        for (const auto& id : ids)
        {
            _DeleteClaudeSession(winrt::hstring{ id });
            _archiveChecked.erase(id);
        }
        _RefreshArchivePageIfVisible();
    }
}
