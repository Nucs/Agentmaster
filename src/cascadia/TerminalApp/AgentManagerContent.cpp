// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCopyActions.h" // CopySessionField — the shared copy-menu action (same path as the per-tab overlay's copy button)
#include "AgentStatusColors.h" // ParseArgbHexColor / FormatArgbHexColor — the cog's "status flashing color" picker <-> AppSettings::flashRingColor
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/ProfileBootstrap.h" // the cog's Profile row (active dir + Change… picker)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/Engine.h" // RecoverableWindows (the "Reopen Windows (N)" recover button)
#include "AgentMaster/ProcessInspect.h" // ReadTranscriptInfo (read-only Flight Plan of an external) + BringClaudeWindowToFront (EXTERNAL menu)
#include "AgentMaster/TranscriptStore.h" // ReadTranscriptQuickFacts — resolve a launch-box session id's cwd
#include "AgentMaster/Updater.h" // the in-app updater: the cog's "Check for updates" + the "vX available!" label

// Agentmaster: the build-stamped git commit + branch (the Settings page header). Generated into
// $(GeneratedFilesDir) by TerminalAppLib.vcxproj's AgentmasterGenerateBuildInfo target, which is
// on the include path. The __has_include guard + fallback defines keep this file compilable if
// the generator hasn't run yet (e.g. opened in an IDE before any build); a real build always
// regenerates the header first (BeforeTargets ClCompile).
#if __has_include("AgentmasterBuildInfo.g.h")
#include "AgentmasterBuildInfo.g.h"
#endif
#ifndef AGENTMASTER_COMMIT_HASH
#define AGENTMASTER_COMMIT_HASH L"unknown"
#endif
#ifndef AGENTMASTER_COMMIT_BRANCH
#define AGENTMASTER_COMMIT_BRANCH L"unknown"
#endif

#include <algorithm>
#include <chrono>
#include <cmath> // std::pow — relative-luminance black/white contrast pick for the card title band
#include <filesystem> // create_directories — the "Create & Launch" affordance for a not-yet-existing dir
#include <system_error> // std::error_code — non-throwing create_directories
#include <thread> // background transcript read for an external's read-only plan
#include <shobjidl.h> // IFileOpenDialog — Browse for claude.exe (native-exe-only policy)

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
// The shared tooltip recipe (AgentTipHelpers.h) — a using-DECLARATION so the file-scope
// helpers below (e.g. TimingText) can call it unqualified too.
using winrt::TerminalApp::implementation::AgentSetTip;

namespace
{
    // Agentmaster: Triage-Board cards hold their hover tooltips back to a deliberate 4s (vs the
    // global ~1/3-system-hover-time fast open used on every other surface), so panning the mouse
    // across a dense board doesn't flash a tip over every card. Passed as AgentSetTip's optional
    // open-delay override at each card tip call site (both the managed _MakeCard and the external
    // census _MakeExternalCard).
    constexpr std::chrono::milliseconds kCardTipDelay{ 4000 };

    SolidColorBrush Fill(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
    }

    // Agentmaster (PENDING_INPUT.md): a small animated "3 dots" cluster for a board card / row that has
    // an UNSENT draft — three goldenrod dots pulsing their opacity 0.3<->1.0 in a 160ms-phase-shifted
    // wave (the classic "typing"/waiting cue), mirroring the per-tab strip pulse. The storyboard targets
    // the dots by ref (no name/resource lookup) and BEGINS on Loaded — so it runs only while the element
    // is in the tree; a board rebuild drops the card and its storyboard. The Loaded closure keeps the
    // storyboard alive. dotPx sizes the dots (cards: 5px).
    StackPanel BuildPendingDots(double dotPx = 5.0)
    {
        namespace MA = winrt::Windows::UI::Xaml::Media::Animation;
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(3);
        row.VerticalAlignment(VerticalAlignment::Center);
        const auto gold = ColorHelper::FromArgb(0xFF, 0xE0, 0xA9, 0x2B);
        std::vector<winrt::Windows::UI::Xaml::Shapes::Ellipse> dots;
        for (int i = 0; i < 3; ++i)
        {
            winrt::Windows::UI::Xaml::Shapes::Ellipse e;
            e.Width(dotPx);
            e.Height(dotPx);
            e.Fill(SolidColorBrush{ gold });
            row.Children().Append(e);
            dots.push_back(e);
        }
        MA::Storyboard sb;
        for (int i = 0; i < 3; ++i)
        {
            MA::DoubleAnimation a;
            a.From(0.3);
            a.To(1.0);
            a.Duration(Duration{ std::chrono::milliseconds(500) });
            a.BeginTime(TimeSpan{ std::chrono::milliseconds(160 * i) });
            a.AutoReverse(true);
            MA::RepeatBehavior forever;
            forever.Type = MA::RepeatBehaviorType::Forever; // a value struct — its members are fields, not setters
            a.RepeatBehavior(forever);
            MA::Storyboard::SetTarget(a, dots[i]);
            MA::Storyboard::SetTargetProperty(a, L"Opacity");
            sb.Children().Append(a);
        }
        row.Loaded([sb](auto&&, auto&&) {
            try
            {
                sb.Begin();
            }
            catch (...)
            {
            }
        });
        return row;
    }

    // Agentmaster: "#RRGGBB" -> opaque Color (the per-working-directory tab color, for the Triage
    // card's title band). Mirrors the Sessions page's SessHexToColor. nullopt on anything malformed.
    std::optional<Color> HexToColor(const std::wstring& hex)
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

    // Agentmaster: pick BLACK vs WHITE text for the best contrast over a solid fill, so the Triage
    // card's title stays legible on ANY working-dir band color — black on a LIGHT band, white on a
    // DARK one. Uses the WCAG relative-luminance crossover (~0.179): above it black has the higher
    // contrast ratio, below it white does. Returns true when DARK (black) text should be used.
    bool PreferDarkTextOn(const Color& c)
    {
        const auto lin = [](uint8_t v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        const double luminance = 0.2126 * lin(c.R) + 0.7152 * lin(c.G) + 0.0722 * lin(c.B);
        return luminance > 0.179;
    }

    // Agentmaster: put text on the system clipboard (the context menus' "Copy Session Id"). Mirrors
    // the Archive page's ArchiveCopyToClipboard. Flush so the content survives the app losing focus
    // (it can refuse — non-fatal). Best-effort.
    void CopyTextToClipboard(const std::wstring& text)
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

    // Agentmaster (native-exe-only policy): a modal file picker for locating claude.exe. COM is already
    // initialized STA on the XAML-Islands UI thread; CALL THIS OFF THE CLICK TICK (a COM modal needs the
    // message pump — the same rule the profile picker follows). Empty optional on cancel/failure.
    std::optional<std::wstring> PickClaudeExe(HWND owner)
    {
        winrt::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        {
            return std::nullopt;
        }
        const COMDLG_FILTERSPEC filters[] = {
            { L"claude.exe", L"claude.exe" },
            { L"Executables (*.exe)", L"*.exe" },
            { L"All files (*.*)", L"*.*" },
        };
        dlg->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
        dlg->SetTitle(L"Locate claude.exe (the native build)");
        FILEOPENDIALOGOPTIONS opts{};
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        if (FAILED(dlg->Show(owner)))
        {
            return std::nullopt; // user canceled
        }
        winrt::com_ptr<IShellItem> item;
        if (FAILED(dlg->GetResult(item.put())))
        {
            return std::nullopt;
        }
        PWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
        {
            return std::nullopt;
        }
        std::wstring out{ path };
        ::CoTaskMemFree(path);
        return out;
    }

    // Agentmaster: a native folder picker (IFileOpenDialog in FOS_PICKFOLDERS mode — the modern
    // folder browser) for the Launch path box's "Browse…" row. Seeds the dialog at `initialDir`
    // when that's an existing folder, so Browse opens where the box currently points rather than
    // the last shell location. Returns the chosen folder, or nullopt on cancel.
    std::optional<std::wstring> PickFolder(HWND owner, const std::wstring& initialDir)
    {
        winrt::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        {
            return std::nullopt;
        }
        dlg->SetTitle(L"Choose a working directory");
        FILEOPENDIALOGOPTIONS opts{};
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (!initialDir.empty())
        {
            winrt::com_ptr<IShellItem> start;
            if (SUCCEEDED(::SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(start.put()))) && start)
            {
                dlg->SetFolder(start.get()); // open AT this folder (not just a default), since the user already typed a path
            }
        }
        if (FAILED(dlg->Show(owner)))
        {
            return std::nullopt; // user canceled
        }
        winrt::com_ptr<IShellItem> item;
        if (FAILED(dlg->GetResult(item.put())))
        {
            return std::nullopt;
        }
        PWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
        {
            return std::nullopt;
        }
        std::wstring out{ path };
        ::CoTaskMemFree(path);
        return out;
    }

    // Agentmaster: a small stable palette to color-code the EXTERNAL tree's pid underline by host
    // window/shell — claudes sharing a terminal window/tab carry the same host shell pid, so they get
    // the same color and are easy to spot at a glance (even across cwd groups). Vivid-on-dark, visually
    // distinct hues; the key (the host pid) is spread across the palette by a multiplicative (Fibonacci)
    // hash so small / sequential pids don't land on the same color. Color order is {A, R, G, B}.
    Color WindowKeyColor(uint32_t key)
    {
        static const Color kPalette[] = {
            { 0xFF, 0x6E, 0xA8, 0xFF }, // blue
            { 0xFF, 0x7F, 0xD1, 0x7F }, // green
            { 0xFF, 0xFF, 0xB8, 0x6C }, // orange
            { 0xFF, 0xFF, 0x7F, 0x9E }, // pink
            { 0xFF, 0xC7, 0x92, 0xEA }, // purple
            { 0xFF, 0x8B, 0xE9, 0xFD }, // cyan
            { 0xFF, 0xF1, 0xFA, 0x8C }, // yellow
            { 0xFF, 0x50, 0xC8, 0x78 }, // emerald
            { 0xFF, 0xFF, 0x9F, 0x40 }, // amber
            { 0xFF, 0xBD, 0x93, 0xF9 }, // violet
        };
        constexpr uint32_t n = sizeof(kPalette) / sizeof(kPalette[0]);
        const uint32_t h = (key ^ (key >> 16)) * 0x9E3779B1u; // Fibonacci hash -> spread
        return kPalette[(h >> 24) % n];
    }

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Agentmaster (Waiting-for-you "unread" model): render a minutes count as a compact "Xd Yh Zm"
    // for the Settings cog's Waiting-for-you timeout slider header (e.g. 60 -> "1h", 90 -> "1h 30m",
    // 4320 -> "3d"). 0 reads as "never" (the slider is disabled then; the "Never" toggle owns 0).
    std::wstring FormatMinutesFriendly(uint32_t m)
    {
        if (m == 0)
        {
            return L"never";
        }
        const uint32_t d = m / 1440;
        m %= 1440;
        const uint32_t h = m / 60;
        const uint32_t mi = m % 60;
        std::wstring out;
        const auto add = [&](uint32_t v, const wchar_t* u) {
            if (v)
            {
                if (!out.empty())
                {
                    out += L' ';
                }
                out += std::to_wstring(v);
                out += u;
            }
        };
        add(d, L"d");
        add(h, L"h");
        add(mi, L"m");
        return out;
    }

    // Agentmaster (context-window adornment): a compact token count for the board card — "182K",
    // "8.3K", "1.05M". Whole-K once past 10K (the common context range), one decimal below that,
    // two-decimal M past a million. PR feedback (Eli): show the raw token count, not a %, because
    // the context-window denominator (200K vs 1M) can't be reliably known from the model id.
    std::wstring FormatTokenCount(int64_t n)
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

    // Agentmaster: group a non-negative integer with thousands separators ("182,341") for the
    // context-tokens tooltip (the exact count behind the compact "182K").
    std::wstring GroupDigits(int64_t n)
    {
        if (n < 0)
        {
            n = 0;
        }
        std::wstring raw = std::to_wstring(n);
        std::wstring out;
        int count = 0;
        for (auto it = raw.rbegin(); it != raw.rend(); ++it)
        {
            if (count && count % 3 == 0)
            {
                out.push_back(L',');
            }
            out.push_back(*it);
            ++count;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    // Agentmaster: format a duration (ms) as a consolidated span. Units descend month / day / hour /
    // minute / second; month(=30d) and minute SHARE the letter 'm', disambiguated by position (the
    // sequence is always largest->smallest), per the requested format: "1m4d6h" (1 month 4 days 6
    // hours), "2h30m" (2 hours 30 min), "5m30s" (5 min 30 s), "45s". Non-zero units only, from the
    // largest down to the floor. `allowSeconds` enables the seconds unit — but only when the whole
    // span is < 1 hour ("seconds if hours not present, otherwise minutes minimum"). Always emits at
    // least the floor unit (value 0) so a just-started span reads "0m" / "0s", never "".
    std::wstring FormatSpan(int64_t ms, bool allowSeconds)
    {
        if (ms < 0)
        {
            ms = 0;
        }
        int64_t t = ms / 1000; // seconds
        const int64_t months = t / (30LL * 24 * 3600);
        t %= (30LL * 24 * 3600);
        const int64_t days = t / (24 * 3600);
        t %= (24 * 3600);
        const int64_t hours = t / 3600;
        t %= 3600;
        const int64_t mins = t / 60;
        const int64_t secs = t % 60;
        const bool showSeconds = allowSeconds && months == 0 && days == 0 && hours == 0;

        std::wstring out;
        const auto add = [&out](int64_t v, const wchar_t* unit) {
            if (v != 0)
            {
                out += std::to_wstring(v);
                out += unit;
            }
        };
        add(months, L"m");
        add(days, L"d");
        add(hours, L"h");
        add(mins, L"m");
        if (showSeconds)
        {
            add(secs, L"s");
        }
        if (out.empty())
        {
            out = showSeconds ? L"0s" : L"0m"; // floor unit when everything rounded below it
        }
        return out;
    }

    // Agentmaster: the consolidated per-session timing string "-{createdAgo}/{activeFor}/-{lastAgo}",
    // e.g. "-2m7d/12h/-2h30m" = created 2 months 7 days ago, active for 12 h, last activity 2 h 30 m
    // ago. The "ago" parts are signed (-); the active span (lastActivity - created) is not. The first
    // two parts floor at minutes; the last allows seconds (when < 1 h). Empty when we have no
    // creation time (a never-prompted session — nothing meaningful to show).
    std::wstring FormatSessionTiming(int64_t createdMs, int64_t lastActivityMs)
    {
        if (createdMs <= 0)
        {
            return {};
        }
        const int64_t now = NowMs();
        if (lastActivityMs < createdMs)
        {
            lastActivityMs = createdMs; // no activity recorded / clock skew -> 0 active span
        }
        const std::wstring createdAgo = FormatSpan(now - createdMs, false);
        const std::wstring activeFor = FormatSpan(lastActivityMs - createdMs, false);
        const std::wstring lastAgo = FormatSpan(now - lastActivityMs, true);
        return L"-" + createdAgo + L"/" + activeFor + L"/-" + lastAgo;
    }

    // The human-readable tooltip explaining the cryptic timing string.
    constexpr const wchar_t* kTimingTooltip =
        L"created ago  /  active for  /  last activity ago\n(e.g. -2m7d/12h/-2h30m — m=month or minute by position, d=day, h=hour, s=second)";

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

    // Phase C2: an OBSERVED Codex session's rollout-derived turn state -> the SAME status palette as
    // managed sessions (Running blue / Waiting goldenrod / Idle-or-unknown gray). Codex exposes no
    // NeedsApproval/Error via PULL (the rollout records neither — that is C3), so it maps onto a
    // 3-state subset. Colors the External Codex row's state dot.
    Color CodexStateColor(::Agentmaster::CodexState s)
    {
        switch (s)
        {
        case ::Agentmaster::CodexState::Running:
            return Colors::DodgerBlue();
        case ::Agentmaster::CodexState::Waiting:
            return Colors::Goldenrod();
        case ::Agentmaster::CodexState::Idle:
        case ::Agentmaster::CodexState::Unknown:
        default:
            return Color{ 0xFF, 0x9E, 0x9E, 0x9E }; // the external observe-only gray
        }
    }

    winrt::hstring CodexStateLabel(::Agentmaster::CodexState s)
    {
        switch (s)
        {
        case ::Agentmaster::CodexState::Running:
            return L"running";
        case ::Agentmaster::CodexState::Waiting:
            return L"waiting-for-you";
        case ::Agentmaster::CodexState::Idle:
            return L"idle";
        case ::Agentmaster::CodexState::Unknown:
        default:
            return L"observe-only";
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

    // No longer used for the Explorer tree (the tree now uses StateDot, the tab-strip Ellipse), but kept
    // for parity with the overlay's own StateGlyph + potential reuse. [[maybe_unused]] avoids C4505 under
    // /W4 /WX now that nothing references it.
    [[maybe_unused]] winrt::hstring StateGlyph(SessionState s)
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

    // Agentmaster: the Explorer-tree state dot — IDENTICAL to the tab-strip status dot
    // (TabHeaderControl.xaml HeaderAgentStatusDot): a 10x10 filled Ellipse with a state-colored Fill and
    // a thin black Stroke so it stays legible on any row / selection background, vertically centered. A
    // real filled shape renders bolder + more uniform than the per-state geometric glyphs (○/●/◐/⚠/✕/✓)
    // the tree used before, and matches the tab so the two views speak ONE visual language. The StateLabel
    // text beside it still names the state, so dropping the varied glyphs loses no information.
    winrt::Windows::UI::Xaml::Shapes::Ellipse StateDot(Color fill)
    {
        winrt::Windows::UI::Xaml::Shapes::Ellipse e{};
        e.Width(10);
        e.Height(10);
        e.Fill(SolidColorBrush{ fill });
        e.Stroke(SolidColorBrush{ Colors::Black() });
        e.StrokeThickness(1);
        e.VerticalAlignment(VerticalAlignment::Center);
        return e;
    }

    // Agentmaster (eager-init / "Activate Tab"): the DORMANT half-hollow twin of StateDot — for a live
    // session whose claude hasn't STARTED yet (a window-restored / re-homed tab the user never clicked;
    // ConnectionState == NotConnected, SessionInfo::started == false). It reads as "not initialized;
    // Activate to wake it". Same 10x10 geometry as StateDot so the dormant<->started swap is seamless;
    // identical to the tab strip's half-hollow dot (TabHeaderControl.xaml): a HALF-FILL (a state-colored
    // ellipse CLIPPED to its right half) under a RING (transparent fill + black stroke = the full outline,
    // so the unfilled half reads as hollow). Returns a Grid composing the two layers; callers append it
    // exactly like StateDot's Ellipse (both are UIElement).
    winrt::Windows::UI::Xaml::Controls::Grid StateDotDormant(Color fill)
    {
        winrt::Windows::UI::Xaml::Controls::Grid g{};
        g.Width(10);
        g.Height(10);
        g.VerticalAlignment(VerticalAlignment::Center);
        // Half-fill: a state-colored ellipse clipped to its RIGHT half (x in [5,10]).
        winrt::Windows::UI::Xaml::Shapes::Ellipse half{};
        half.Width(10);
        half.Height(10);
        half.Fill(SolidColorBrush{ fill });
        winrt::Windows::UI::Xaml::Media::RectangleGeometry clip{};
        clip.Rect(winrt::Windows::Foundation::Rect{ 5, 0, 5, 10 });
        half.Clip(clip);
        g.Children().Append(half);
        // Ring: the full outline (so the unfilled half reads as a hollow circle, not a half-disc). The
        // ring is the STATE COLOR (not black): the Manager is ALWAYS dark and a board card's title band is
        // an arbitrary dir color, so a black ring would vanish; the status hue reads on both. The filled
        // half is the same color, so the dot reads as "half solid, half outline" in one hue.
        winrt::Windows::UI::Xaml::Shapes::Ellipse ring{};
        ring.Width(10);
        ring.Height(10);
        ring.Fill(SolidColorBrush{ Colors::Transparent() });
        ring.Stroke(SolidColorBrush{ fill });
        ring.StrokeThickness(1);
        g.Children().Append(ring);
        return g;
    }

    // Agentmaster (eager-init): a live MANAGED session whose ConPTY/claude hasn't started yet — the
    // half-hollow-dot / "Activate" gate. External (observe-only) sessions are never "dormant" (we host
    // no control). Mirrors the tab-strip dormant predicate in TerminalPage.
    bool IsSessionDormant(const SessionInfo& s)
    {
        return s.live && !s.started && !s.external;
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

    // Agentmaster: collapse a (possibly multi-line) title to ONE line for the dense Explorer
    // rows / board cards / detail headers — each run of CR/LF/TAB becomes a single space, with
    // no leading/trailing filler. Titles can now carry newlines (the tab-rename + tree-rename
    // boxes accept Return), and the tab HEADER renders the true multi-line form; the lists,
    // which assume single-line rows, would otherwise grow/clip. Rule #11 keeps the stored value
    // identical across surfaces — this only changes how the compact views PRESENT it.
    winrt::hstring OneLine(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        bool pendingSpace = false;
        for (const wchar_t c : s)
        {
            if (c == L'\r' || c == L'\n' || c == L'\t')
            {
                pendingSpace = !out.empty(); // collapse the whitespace run; never lead with a space
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

    // Agentmaster: paint a button a SOLID color (white text) whose color SURVIVES hover/press. WinUI's
    // default Button template overrides a locally-set Background in its PointerOver/Pressed visual states
    // with the ButtonBackground{PointerOver,Pressed} theme brushes (and likewise the foreground), so a
    // plain b.Background(...) reverts to the subtle theme hover brush the instant the pointer enters — the
    // "colored button loses its color (and the text recolors) on mouse-over" bug. Overriding those theme-
    // resource KEYS in the button's OWN Resources makes the template resolve them to our colors in every
    // state, so hover/press just shifts shade instead of dropping the color. base/hover/pressed are
    // 0xAARRGGBB; reuse-safe (each call rebuilds the four overrides). Pair with ClearHoldButton to return
    // to default chrome.
    void PaintHoldButton(const Button& b, uint32_t base, uint32_t hover, uint32_t pressed)
    {
        if (!b)
        {
            return;
        }
        const auto brush = [](uint32_t c) {
            return Fill(static_cast<uint8_t>((c >> 24) & 0xFF), static_cast<uint8_t>((c >> 16) & 0xFF), static_cast<uint8_t>((c >> 8) & 0xFF), static_cast<uint8_t>(c & 0xFF));
        };
        const auto white = Fill(0xFF, 0xFF, 0xFF, 0xFF);
        auto res = b.Resources();
        res.Clear(); // our button; nothing else lives in its dictionary — also drops a prior mode's overrides
        res.Insert(winrt::box_value(L"ButtonBackgroundPointerOver"), brush(hover));
        res.Insert(winrt::box_value(L"ButtonBackgroundPressed"), brush(pressed));
        res.Insert(winrt::box_value(L"ButtonForegroundPointerOver"), white);
        res.Insert(winrt::box_value(L"ButtonForegroundPressed"), white);
        b.Background(brush(base));
        b.Foreground(white);
    }

    // Agentmaster: undo PaintHoldButton — drop the per-state overrides and revert to the default subtle
    // toolbar-button chrome (so its hover matches the sibling Sessions/Pause buttons again).
    void ClearHoldButton(const Button& b)
    {
        if (!b)
        {
            return;
        }
        b.Resources().Clear();
        // ClearValue (NOT Background(nullptr)): a NULL background brush is not hit-test-visible, which would
        // leave this button's body dead (clicks/hover only on its text). Clearing the local value lets the
        // default Style's {ThemeResource ButtonBackground} apply — which the Manager seeds non-null at _root
        // scope (see the AgentManagerContent ctor) — so an Off-state Keep-Awake button stays clickable.
        b.ClearValue(winrt::Windows::UI::Xaml::Controls::Control::BackgroundProperty());
        b.ClearValue(winrt::Windows::UI::Xaml::Controls::Control::ForegroundProperty());
    }

    // Agentmaster: emphasize the PRIMARY header toggle. The LOCAL/GLOBAL/EXTERNAL scope button is the one
    // control in the board/tree header that changes WHAT you are looking at, so it should read louder than
    // its siblings (sort, refresh, Clear, Show all — all secondary): a filled accent background + bold white
    // text makes it the "primary" button among the default subtle-chrome ones. Applied once at construction;
    // the label content still changes later via the _Update*ScopeButton setters without disturbing this
    // styling. Uses PaintHoldButton so the accent HOLDS through hover/press (shifting shade) instead of
    // reverting to the subtle theme hover brush — previously only the normal-state Background was set, so the
    // accent dropped to gray on mouse-over (the same revert the keep-awake button hit) and only the bold
    // weight carried the emphasis there.
    void EmphasizeScopeButton(const Button& b)
    {
        if (!b)
        {
            return;
        }
        PaintHoldButton(b, 0xFF356AB8, 0xFF3E7DCE, 0xFF2B5391); // accent blue, lighter on hover / darker on press
        b.FontWeight(FontWeights::SemiBold());
    }

    // Agentmaster: the dim per-session timing adornment "-created/active/-lastAgo" + an explanatory
    // tooltip. Returns a null TextBlock (falsy) when there is no creation time to show, so callers
    // can `if (auto t = TimingText(...)) row.Children().Append(t);`.
    TextBlock TimingText(int64_t createdMs, int64_t lastActivityMs)
    {
        const std::wstring s = FormatSessionTiming(createdMs, lastActivityMs);
        if (s.empty())
        {
            return nullptr;
        }
        auto t = Text(winrt::hstring{ s }, 10, false, 0.45);
        AgentSetTip(t, winrt::hstring{ kTimingTooltip });
        return t;
    }

    // Agentmaster: PointerEntered/PointerExited are BUBBLING routed events, so a hit-test-visible
    // CHILD of a card/row raises its OWN enter/exit that bubbles up to the parent's hover handler.
    // Every label inside a card MUST stay hit-testable (that is how its AgentSetTip tooltip opens),
    // so merely crossing the mouse between two labels makes a child PointerExited bubble to the card
    // and a naive card.PointerExited then fires — toggling the hover ring + the Linked-Lenses tab
    // pill OFF, before the next label's bubbled PointerEntered turns them back ON. That on/off churn
    // is the "highlight flickers as I move over text" bug (the "\x22EF" dots only LOOK steady because
    // their OpacityTransition smooths the dip; the ring + tab pill snap). This guard returns true
    // when the pointer is STILL inside `sender`'s own bounds — i.e. the exit bubbled from a child,
    // not a real leave — so a card/row PointerExited handler can early-out and ignore it, keeping
    // the labels hit-testable (tooltips intact) instead of making them clickthrough (which kills the
    // tooltips). A genuine leave samples at/outside an edge => returns false, so it is never missed.
    // `IInspectable` is fully qualified here on purpose: at this anonymous-namespace file scope the
    // unqualified name is AMBIGUOUS between the global COM ::IInspectable (from <inspectable.h>) and
    // winrt::Windows::Foundation::IInspectable (the using-directive). The card/row lambdas below dodge
    // this only because, inside AgentManagerContent's member functions, the inherited
    // winrt::implements<...>::IInspectable typedef wins lookup — a free function has no such scope.
    bool PointerStillWithin(const winrt::Windows::Foundation::IInspectable& sender, const PointerRoutedEventArgs& e)
    {
        const auto fe = sender.try_as<FrameworkElement>();
        if (!fe)
        {
            return false;
        }
        const auto p = e.GetCurrentPoint(fe).Position();
        // Treat the outermost ~1px as "left" so a real leave sampled right at the boundary is never
        // swallowed (which would strand a pill). Every card/row label sits well inside the 8px band/
        // body padding (the dots ≥4px in), so a child-bubbled exit is always still within this inset.
        constexpr double kEdge = 1.0;
        return p.X > kEdge && p.Y > kEdge && p.X < fe.ActualWidth() - kEdge && p.Y < fe.ActualHeight() - kEdge;
    }

    // ---- Explorer Tree sort (Agentmaster) ------------------------------------
    // A normalized sort key extracted from EITHER a managed SessionInfo or an observed external row,
    // so one comparator orders LOCAL/GLOBAL (managed sessions) and EXTERNAL (observe-only claudes).

    struct SortKey
    {
        int64_t created; // conversation creation (~age). INT64_MAX when unknown — a fresh / never-
                         // prompted session has no transcript yet, so it is treated as just-created
                         // (== newest), floating to the top of NEWEST and the bottom of OLDEST.
        int64_t last; // last activity (mtime); 0 when unknown.
        bool active; // currently running (managed Running state) — floats to the top of MOST ACTIVE.
        std::wstring title; // display title for A-Z (case-insensitive).
        uint32_t pid; // BY PID key: the host window/shell pid (externals) or the claude pid (managed) — groups same-window rows together (matches the color-coded underline), UINT32_MAX when unknown (sorts last).
    };

    SortKey MakeSortKey(const ::Agentmaster::SessionInfo& s)
    {
        SortKey k{};
        k.created = s.convCreatedUnixMs > 0 ? s.convCreatedUnixMs : INT64_MAX;
        k.last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
        k.active = (s.state == ::Agentmaster::SessionState::Running);
        k.title = s.title;
        k.pid = s.pid ? s.pid : UINT32_MAX; // managed: the claude pid (unique -> the most-active secondary rarely breaks ties)
        return k;
    }

    SortKey MakeSortKey(const ::Agentmaster::ExternalClaudeRow& ex)
    {
        SortKey k{};
        const int64_t created = ex.createdUnixMs ? ex.createdUnixMs : ex.startUnixMs;
        k.created = created > 0 ? created : INT64_MAX;
        k.last = ex.lastActivityUnixMs;
        k.active = false; // externals carry no run-state — rank by recency only
        k.title = !ex.title.empty() ? ex.title : ex.cwd;
        k.pid = ex.hostPid ? ex.hostPid : (ex.pid ? ex.pid : UINT32_MAX); // BY PID groups by host window/shell (the underline color key), so same-window externals sit together
        return k;
    }

    // Case-insensitive (ordinal) less, matching PathEq's basis. Used for A-Z and as the deterministic
    // tiebreaker for every mode so equal keys keep a stable on-screen order.
    bool CiLess(const std::wstring& a, const std::wstring& b)
    {
#ifdef _WIN32
        return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
#else
        return a < b;
#endif
    }

    bool SortKeyLess(::Agentmaster::ExplorerSort mode, const SortKey& a, const SortKey& b)
    {
        using ::Agentmaster::ExplorerSort;
        switch (mode)
        {
        case ExplorerSort::Newest:
            if (a.created != b.created)
                return a.created > b.created; // newest first
            break;
        case ExplorerSort::Oldest:
            if (a.created != b.created)
                return a.created < b.created; // oldest first
            break;
        case ExplorerSort::MostActive:
            if (a.active != b.active)
                return a.active; // a running session outranks any idle one
            if (a.last != b.last)
                return a.last > b.last; // then most-recent activity first
            break;
        case ExplorerSort::Alpha:
            break; // name is the primary key — handled by the tiebreak below
        case ExplorerSort::ByPid:
            if (a.pid != b.pid)
                return a.pid < b.pid; // group by host window/shell pid (ascending)
            if (a.active != b.active)
                return a.active; // then by most active: running first
            if (a.last != b.last)
                return a.last > b.last; // ...then most-recent activity
            break;
        }
        if (CiLess(a.title, b.title))
            return true;
        if (CiLess(b.title, a.title))
            return false;
        return false;
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

    // Agentmaster: whether `p` is a well-formed ABSOLUTE path we could create as a brand-new working
    // directory — the gate for the launch box's "Create & Launch" affordance (a folder the user typed
    // that doesn't exist yet). We require an absolute/rooted path (a drive root "X:\…" or a UNC
    // "\\server\share…") so a stray relative token ("agent") never offers to create a folder at some
    // surprising process cwd, and we reject the Win32-illegal path characters so the eventual
    // create_directories can't fail on garbage. Pass an already-NormPath'd string (separators unified).
    // The caller checks existence separately — this says only "this looks like a path we could make".
    bool LooksLikeCreatableDir(const std::wstring& p)
    {
#ifdef _WIN32
        if (p.size() < 3)
        {
            return false; // shortest creatable rooted path is "X:\" (a bare drive root already exists)
        }
        // Reject characters illegal in a Windows path. The drive ':' (index 1) is allowed and checked
        // below; wildcards / redirection / control chars can never be a real folder name.
        for (const wchar_t c : p)
        {
            if (c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*' || c < 0x20)
            {
                return false;
            }
        }
        const auto isDriveLetter = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); };
        // Drive-rooted "X:\…" (NormPath already turned '/' into '\'). A bare "X:" or drive-relative
        // "X:rel" is rejected: no ':' may appear past the drive separator, and a real subfolder must
        // follow the root.
        if (isDriveLetter(p[0]) && p[1] == L':' && p[2] == L'\\')
        {
            return p.size() > 3 && p.find(L':', 2) == std::wstring::npos;
        }
        // UNC "\\server\share\…" — needs a share component past the host, and carries no drive ':'.
        if (p[0] == L'\\' && p[1] == L'\\')
        {
            const auto host = p.find(L'\\', 2);
            return host != std::wstring::npos && host + 1 < p.size() && p.find(L':') == std::wstring::npos;
        }
        return false;
#else
        return p.size() > 1 && p.front() == L'/'; // POSIX: absolute paths only
#endif
    }

    // Case-insensitive (the Windows filesystem default) leaf tests for the path-picker's
    // type-to-filter: whether `name` begins with `prefix`, and whether the two are equal.
    // Both compare ordinally, so a partial leaf "Agent" matches "Agentmaster" (prefix) while
    // a complete leaf "Agentmaster" matches it exactly (then hoisted to the top of the list).
    bool LeafStartsWith(const std::wstring& name, const std::wstring& prefix)
    {
        if (prefix.empty())
        {
            return true;
        }
        if (name.size() < prefix.size())
        {
            return false;
        }
        return ::CompareStringOrdinal(name.c_str(), static_cast<int>(prefix.size()),
                                      prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
    }

    bool LeafEquals(const std::wstring& a, const std::wstring& b)
    {
        return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    // Lower-case a string for the path-picker's fuzzy matching. Uses the OS full-Unicode case
    // fold (the same LCMapStringEx the search paths use), keyed to LOCALE_NAME_INVARIANT so a
    // folder match is deterministic regardless of the user's locale.
    std::wstring ToLowerInvariant(const std::wstring& s)
    {
        if (s.empty())
        {
            return {};
        }
#ifdef _WIN32
        const int needed = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                           s.c_str(), static_cast<int>(s.size()),
                                           nullptr, 0, nullptr, nullptr, 0);
        if (needed > 0)
        {
            std::wstring out(static_cast<size_t>(needed), L'\0');
            const int wrote = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                              s.c_str(), static_cast<int>(s.size()),
                                              out.data(), needed, nullptr, nullptr, 0);
            if (wrote > 0)
            {
                out.resize(static_cast<size_t>(wrote));
                return out;
            }
        }
        return s; // mapping failed (shouldn't, for paths): fall back to the original
#else
        std::wstring out = s;
        for (auto& c : out)
        {
            c = static_cast<wchar_t>(::towlower(static_cast<wint_t>(c)));
        }
        return out;
#endif
    }

    // Approximate-substring edit distance: the MINIMUM number of single-character edits
    // (insert / delete / substitute) to turn `pattern` into SOME substring of `text` — i.e.
    // the match may begin AND end anywhere in `text` (row 0 is all zeros; the answer is the
    // min over the last row). 0 == `pattern` occurs verbatim. The result never exceeds
    // pattern.size() (turning the pattern into the empty substring costs exactly that), so it
    // doubles as a "how far off" score in [0, m]. Used to rank recent directories against a
    // typed bare token, closest (smallest) first. BOTH inputs must already be lower-cased by
    // the caller (so the compare is a plain wchar_t == ).
    size_t FuzzySubstringDistance(const std::wstring& pattern, const std::wstring& text)
    {
        const size_t m = pattern.size();
        const size_t n = text.size();
        if (m == 0)
        {
            return 0;
        }
        if (n == 0)
        {
            return m;
        }
        std::vector<size_t> prev(n + 1, 0); // row i-1; row 0 = all zeros => a match may start anywhere
        std::vector<size_t> cur(n + 1, 0);
        for (size_t i = 1; i <= m; ++i)
        {
            cur[0] = i; // the first i pattern chars against an empty text prefix == i inserts
            for (size_t j = 1; j <= n; ++j)
            {
                const size_t cost = (pattern[i - 1] == text[j - 1]) ? 0u : 1u;
                const size_t del = prev[j] + 1; // drop pattern[i-1]
                const size_t ins = cur[j - 1] + 1; // skip text[j-1]
                const size_t sub = prev[j - 1] + cost; // match / substitute
                cur[j] = (std::min)(del, (std::min)(ins, sub));
            }
            std::swap(prev, cur);
        }
        return *std::min_element(prev.begin(), prev.end()); // a match may end anywhere
    }

    // Agentmaster: the Launch box accepts EITHER a working directory OR a Claude session id. A
    // UUID-shaped token (8-4-4-4-12 hex, with optional surrounding braces / whitespace) is treated
    // as a session id (resume / fork); anything else is a path. A directory is never UUID-shaped and
    // a session id is never a valid path, so the discrimination is unambiguous. Returns the bare id
    // (braces + whitespace stripped) on a match, nullopt otherwise.
    std::optional<std::wstring> LooksLikeSessionId(const std::wstring& raw)
    {
        std::wstring s = raw;
        const auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (!s.empty() && isws(s.front()))
        {
            s.erase(s.begin());
        }
        while (!s.empty() && isws(s.back()))
        {
            s.pop_back();
        }
        if (s.size() >= 2 && s.front() == L'{' && s.back() == L'}')
        {
            s = s.substr(1, s.size() - 2);
        }
        if (s.size() != 36)
        {
            return std::nullopt;
        }
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != L'-')
                {
                    return std::nullopt;
                }
            }
            else
            {
                const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
                if (!hex)
                {
                    return std::nullopt;
                }
            }
        }
        return s;
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
        // Agentmaster: the Manager pane is ALWAYS dark, regardless of the Windows / Windows Terminal
        // theme (a hard product requirement). RequestedTheme(Dark) forces every built-in control in
        // this subtree (buttons, text boxes, scrollbars, combo lists, …) to render dark. Surfaces that
        // render OUTSIDE this subtree's visual root — the Popup-hosted path picker and the modal
        // overlay cards — re-assert Dark on themselves, and the confirm ContentDialogs follow
        // _root.ActualTheme() (now Dark). WT's theme passes only push brush *resources* into panes
        // (rootPane->UpdateResources) and never set RequestedTheme on pane content, so this sticks
        // across light/dark theme switches.
        _root.RequestedTheme(ElementTheme::Dark);
        _dispatcher = DispatcherQueue::GetForCurrentThread();
        _templates = ::Agentmaster::LoadTemplates(); // persisted plan templates (M8)
        _recentDirs = ::Agentmaster::LoadRecentDirs(); // MRU for the Launch path-picker
        _layout = ::Agentmaster::LoadLayout(); // persisted splitter geometry (pane sizes)

        // Agentmaster: paint the Manager pane an OPAQUE background. This is LOAD-BEARING for input,
        // not cosmetics. IPaneContent::BackgroundBrush() returns _root.Background(), and the WT pane
        // root is left transparent on purpose (Pane.cpp — so vintage/acrylic opacity shows through),
        // so the CONTENT must paint its own fill. A Grid with a NULL Background is not hit-test-visible,
        // so a click on the gaps between widgets (the tip of a text run, a thin border, any empty spot)
        // falls THROUGH the transparent pane -> the XAML island -> NonClientIslandWindow::_OnNcHitTest,
        // which returns HTCAPTION for any client point the island doesn't consume: the window then DRAGS
        // on left-drag and pops the system/caption menu on right-click, exactly as if you'd grabbed the
        // title bar (the user-reported "the tabs bar moves / its context menu opens" bug).
        //
        // The fill is the DARK TabViewBackground (#2e2e2e) UNCONDITIONALLY. We force this pane to dark
        // (RequestedTheme above), so the gaps must be dark too. This used to look the brush up via
        // Application.Resources().Lookup(L"UnfocusedBorderBrush"), but that resolves the resource
        // against the *application* theme — which returns the LIGHT value (#e8e8e8) whenever Windows /
        // Windows Terminal is in light mode, bleeding a light-gray rectangle through the widget gaps.
        // App.xaml defines the dark UnfocusedBorderBrush == TabViewBackground == #2e2e2e, so hardcoding
        // it here is exactly the theme-correct dark value with no light-mode bleed. (Must stay non-null /
        // opaque for the hit-testing reason above.)
        _root.Background(Fill(0xFF, 0x2E, 0x2E, 0x2E)); // opaque #2e2e2e == TabViewBackground (dark)

        // Agentmaster: explicit Button STATE brushes for the WHOLE Manager subtree. SAME root cause as the
        // opaque _root fill above — a NULL background brush is NOT hit-test-visible. Under the forced-Dark
        // island theme (RequestedTheme above), the default {ThemeResource ButtonBackground} / *PointerOver /
        // *Pressed brushes do NOT resolve here (they come back null), so a plain Button's BODY isn't
        // hit-testable: hover + clicks only land on its text CONTENT, never the button's padding — the
        // "buttons don't click or hover; it hits the label/text instead" report. The board CARDS dodge this
        // only because each sets an explicit Fill() Background (and the keep-awake/scope buttons because
        // PaintHoldButton seeds these very keys per-button). Seed them ONCE at _root scope so every default
        // Button under the Manager (the toolbar cog / Pause / Sessions / Keep-Awake, the launch + header
        // toggles, the Flight-Plan compose buttons, the settings-overlay buttons, …) is hit-testable across
        // its whole body and shows a real hover/press. A near-invisible rest fill (alpha 0x01 — the
        // "~invisible yet hit-testable" value used elsewhere here) keeps the flat look; hover/press lift.
        // A button with its OWN Background/Resources (cards, PaintHoldButton) overrides these locally. This
        // is the global twin of PaintHoldButton (which proved per-button Resources resolve in this subtree).
        // Inserted BEFORE _BuildLayout so the buttons it creates resolve these on first style-apply.
        {
            auto br = _root.Resources();
            br.Insert(winrt::box_value(L"ButtonBackground"), Fill(0x01, 0xFF, 0xFF, 0xFF)); // flat rest, hit-testable
            br.Insert(winrt::box_value(L"ButtonBackgroundPointerOver"), Fill(0x22, 0xFF, 0xFF, 0xFF)); // subtle hover lift
            br.Insert(winrt::box_value(L"ButtonBackgroundPressed"), Fill(0x33, 0xFF, 0xFF, 0xFF)); // a touch more on press
            br.Insert(winrt::box_value(L"ButtonForeground"), Fill(0xFF, 0xE6, 0xE6, 0xE6)); // light text on the dark UI
            br.Insert(winrt::box_value(L"ButtonForegroundPointerOver"), Fill(0xFF, 0xFF, 0xFF, 0xFF));
            br.Insert(winrt::box_value(L"ButtonForegroundPressed"), Fill(0xFF, 0xFF, 0xFF, 0xFF));
        }

        _BuildLayout();

        // Agentmaster: an INVISIBLE, caret-less keyboard-focus SINK, parked-on by Focus() when this pane
        // is activated. The Manager pane's key handlers (_KeyDownHandler / _ManagerPaneNavPreviewKeyDown —
        // alt+left/right, ctrl+tab, shift+home) are routed events on the pane ROOT and only fire when
        // keyboard focus is INSIDE the pane subtree; Focus() used to leave focus untouched (to avoid a
        // blinking caret in the cwd box), which left focus on the tab HEADER after a tab switch, so those
        // chords did nothing until you clicked an element in the pane. A 1x1, opacity-0, no-focus-visual
        // Button gives the pane keyboard focus with NO visible caret and NO path-picker popup (the popup
        // keys off the cwd box's OWN focus, not this). Appended AFTER _BuildLayout so it is not cleared.
        _focusSink = winrt::Windows::UI::Xaml::Controls::Button{};
        _focusSink.Width(1.0);
        _focusSink.Height(1.0);
        _focusSink.MinWidth(0.0);
        _focusSink.MinHeight(0.0);
        _focusSink.Opacity(0.0);
        _focusSink.IsTabStop(true);
        _focusSink.UseSystemFocusVisuals(false);
        _focusSink.HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Left);
        _focusSink.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Top);
        _root.Children().Append(_focusSink);

        // Agentmaster (Waiting-for-you "unread" model): a low-frequency board refresh so TIME-derived
        // adornments stay current without a hook event — the card's ⚡ "still cached" hint (a few-minute
        // window) and the "-2h30m" timing text. _Refresh() recomputes them from the live snapshot; it is
        // idempotent and already runs on every registry event, so this only covers quiet periods. Weak
        // self so a closed window never leaks a ticking timer.
        _cardRefreshTimer = DispatcherTimer{};
        _cardRefreshTimer.Interval(std::chrono::seconds(30));
        _cardRefreshTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_Refresh();
            }
            else if (const auto t = sender.try_as<DispatcherTimer>())
            {
                t.Stop();
            }
        });
        _cardRefreshTimer.Start();
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
        if (_cardRefreshTimer)
        {
            _cardRefreshTimer.Stop(); // UI thread; stop the periodic ⚡/timing refresh
        }
        if (_progressTimer)
        {
            _progressTimer.Stop(); // UI thread; stop the Waiting-for-you countdown-bar drainer
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
    void AgentManagerContent::SetActivateDormantHandler(std::function<void(winrt::hstring)> handler)
    {
        _activateDormantHandler = std::move(handler);
    }
    void AgentManagerContent::SetActivateAllHandler(std::function<void(bool)> handler)
    {
        _activateAllHandler = std::move(handler);
    }
    void AgentManagerContent::SetHoverSessionHandler(std::function<void(winrt::hstring, bool)> handler)
    {
        _hoverSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetArchiveHandler(std::function<void(winrt::hstring)> handler)
    {
        _archiveHandler = std::move(handler);
    }
    void AgentManagerContent::SetRestoreHandler(std::function<void(winrt::hstring)> handler)
    {
        _restoreHandler = std::move(handler);
    }
    void AgentManagerContent::SetResumeSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler)
    {
        _resumeSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetForkSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler)
    {
        _forkSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetRestartSessionHandler(std::function<void(winrt::hstring)> handler)
    {
        _restartSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetForkManagedSessionHandler(std::function<void(winrt::hstring)> handler)
    {
        _forkManagedSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetRenameHandler(std::function<void(winrt::hstring, winrt::hstring)> handler)
    {
        _renameHandler = std::move(handler);
    }
    void AgentManagerContent::SetAdoptExternalHandler(std::function<void(uint32_t, winrt::hstring, bool)> handler)
    {
        _adoptExternalHandler = std::move(handler);
    }
    void AgentManagerContent::SetCodexLaunchHandler(std::function<void(uint32_t, winrt::hstring, bool, bool)> handler)
    {
        _codexLaunchHandler = std::move(handler);
    }
    void AgentManagerContent::SetLocalScopeProvider(std::function<std::unordered_set<std::wstring>()> provider)
    {
        _localScopeProvider = std::move(provider);
        // Agentmaster: the provider DEFINES the Explorer Tree's LOCAL scope (the set of sessions THIS
        // window hosts) — installing it changes what the tree should show, so re-render now. Wiring
        // installs the provider AFTER SetRegistry's initial _Refresh() (TerminalPage::_WireAgentManagerContent),
        // so without this the first paint — and every paint until the next registry event — runs with
        // haveLocal==false and skips the filter: a freshly opened window lists EVERY window's sessions
        // under the "LOCAL" label until unrelated hook activity happens to trigger a rebuild. Re-render
        // here so the scope takes effect immediately, independent of wiring order. _Refresh() no-ops
        // until the layout exists (it always does — the ctor runs _BuildLayout before any Set*), and is
        // cheap + idempotent.
        _Refresh();
    }
    void AgentManagerContent::SetWindowForegroundProvider(std::function<bool()> provider)
    {
        _windowForegroundProvider = std::move(provider);
    }
    void AgentManagerContent::SetPauseHandler(std::function<void(bool)> handler)
    {
        _pauseHandler = std::move(handler);
    }
    void AgentManagerContent::SetReopenWindowsHandler(std::function<void()> handler)
    {
        _reopenWindowsHandler = std::move(handler);
    }
    void AgentManagerContent::SetOpenSessionsHandler(std::function<void()> handler)
    {
        _openSessionsHandler = std::move(handler);
    }
    void AgentManagerContent::SetResetHiddenSessionsHandler(std::function<void()> handler)
    {
        _resetHiddenSessionsHandler = std::move(handler);
    }
    void AgentManagerContent::SetRefreshHandler(std::function<void()> handler)
    {
        _refreshHandler = std::move(handler);
    }
    // Agentmaster: force a UI redraw from the current data sources (registry snapshot + the last
    // pushed external census). Called by the page after an out-of-band reload (the observer survey
    // lands asynchronously) so the freshly enriched / re-surveyed data shows. Marshal to the UI
    // thread is the caller's responsibility (the page resumes on the dispatcher before calling).
    void AgentManagerContent::RefreshNow()
    {
        _Refresh();
    }
    // Agentmaster (Linked Lenses — per-tab -> Manager sync): drive the lens selection from the page when
    // the user switches to a managed session's terminal tab. Routes through _SelectSession (the same path
    // a board-card single-click takes), so the board card + tree row highlight and the Flight Plan show
    // that session. _SelectSession early-outs when the id is already selected, so a re-select is cheap.
    void AgentManagerContent::SelectSession(winrt::hstring id)
    {
        const std::wstring sid{ id };
        if (sid.empty())
        {
            return; // not a managed session (e.g. a pwsh/cmd/external tab) -> leave the current selection
        }
        _SelectSession(sid);
    }
    // Agentmaster (Linked Lenses — selection VISIBILITY sync): scroll the currently-selected board card
    // (and its Explorer-tree row) into view within their scrolling regions. The page calls this when the
    // user switches TO the Manager tab: while the Manager was hidden the selection followed the user's
    // tab switches (SelectSession from _SyncManagerSelectionToTab), or a state change moved the card into
    // a column where it sits below the fold — so on return the HIGHLIGHTED card can be scrolled
    // off-screen. Revealing it ON tab-entry — deliberately NOT on every _Refresh, which would fight the
    // user's own scrolling while they sit on the Manager tab and undo _RebuildBoard's offset
    // preservation — synchronizes the selection's visibility with its highlight. A no-op when nothing
    // managed is selected (an external selection has no tracked card) or the selected session has no
    // visible card/row (archived / out of the current scope / its directory group collapsed).
    void AgentManagerContent::BringSelectedIntoView()
    {
        if (_selectedId.empty())
        {
            return;
        }
        auto weak = get_weak();
        auto disp = _dispatcher;
        if (!disp)
        {
            return;
        }
        // Defer to a clean tick: the Manager content was just made the visible tab, so its board may not
        // have completed a layout pass yet (MUX TabView hosts only the selected tab's content). Realize
        // it (UpdateLayout) so each card has a real extent, then StartBringIntoView walks up to the
        // card's column ScrollViewer and scrolls it into view (a no-op if already fully visible) — the
        // same UpdateLayout-then-scroll recipe the Flight-Plan auto-scroll-to-bottom uses. Run at LOW
        // priority so this lands AFTER the framework's own restore work this attach triggers (each fresh
        // column ScrollViewer re-applies its saved offset on Loaded; see _MakeBoardColumn) — our reveal
        // must be the last word on the selected card's column, else the offset restore would re-hide it.
        // _selectedId is re-read inside (it may change before this runs).
        disp.TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::Low, [weak]() {
            auto self = weak.get();
            if (!self || self->_selectedId.empty())
            {
                return;
            }
            if (self->_boardHost)
            {
                self->_boardHost.UpdateLayout();
            }
            if (const auto it = self->_boardCardsById.find(self->_selectedId); it != self->_boardCardsById.end() && it->second)
            {
                it->second.StartBringIntoView();
            }
            if (const auto it = self->_treeRowsById.find(self->_selectedId); it != self->_treeRowsById.end() && it->second)
            {
                it->second.StartBringIntoView();
            }
        });
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
            _ValidateLaunchBox(); // re-validate explicitly: don't lean on TextChanged for this programmatic
                                  // set. It works today only because SetSettings runs while the Manager tab
                                  // is active (attached), but a future detached caller (cross-window settings
                                  // push) would otherwise leave the underline/button stale (see _SelectSession).
        }
        // Reflect the (global, persisted) Explorer Tree + Triage Board sorts on their toggles. Safe
        // before the UI is built (each updater no-ops while its button is null); the views adopt the
        // order on the next data-driven rebuild. Lets a window pick up the loaded/changed sorts, not
        // just the ctor defaults — including a board sort changed in another window (adopted on launch).
        _UpdateTreeSortButton();
        _UpdateBoardSortButton();
        _UpdatePlanPaneTab(); // reflect the (global, persisted) Flight-Plan pane tab; no-op while its controls are null
    }
    void AgentManagerContent::SetSettingsHandler(std::function<void(::Agentmaster::AppSettings)> handler)
    {
        _settingsSink = std::move(handler);
    }

    // Agentmaster (cross-window settings broadcast): a GLOBAL setting changed in ANOTHER window — adopt
    // the merged settings and re-apply what this window renders live. Runs on THIS window's UI thread
    // (the engine sink marshals here). We do NOT push these back through _settingsSink (that would loop
    // / re-persist what the source already saved) and we deliberately skip SetSettings's cwd-box reseed
    // (it would stomp in-progress typing). Adopt _appSettings wholesale (the broadcast value is the
    // freshest-disk-merged truth, so future spawns + the cog's next open use it), repaint both sort
    // toggles, then _Refresh so the board + tree re-sort with the new (global) order immediately.
    void AgentManagerContent::ApplyExternalSettings(const ::Agentmaster::AppSettings& settings)
    {
        _appSettings = settings;
        _UpdateTreeSortButton();
        _UpdateBoardSortButton();
        _UpdatePlanPaneTab(); // adopt another window's Flight-Plan pane tab choice (GLOBAL setting)
        _Refresh();
    }

    // ---- Per-window Manager lens (M10; PERSISTENCE.md §13) ------------------

    ::Agentmaster::ManagerState AgentManagerContent::GetManagerState() const
    {
        ::Agentmaster::ManagerState st;
        st.selectedId = _selectedId;
        st.scopeDir = _scopeDir;
        st.selectedPromptId = _selectedPromptId;
        st.collapsedDirs.assign(_collapsedDirs.begin(), _collapsedDirs.end());
        st.layout = _layout;
        st.treeScope = static_cast<int>(_treeScope); // the shared tree/board scope (persisted)
        return st;
    }

    void AgentManagerContent::SetManagerState(const ::Agentmaster::ManagerState& state)
    {
        _ResetPromptHistory(); // a lens (re)seed changes the selected session — start history fresh
        _selectedId = state.selectedId;
        _scopeDir = state.scopeDir;
        _selectedPromptId = state.selectedPromptId;
        _collapsedDirs.clear();
        _collapsedDirs.insert(state.collapsedDirs.begin(), state.collapsedDirs.end());
        // The shared tree/board scope. Direct assignment, NOT _SetTreeScope: this is a seed, not a
        // user transition — no enter/leave-External selection cleanup (the persisted lens is already
        // self-consistent: entering External cleared the managed selection before it was saved), and
        // no lens push (we are APPLYING the lens). EXTERNAL (2) is a transient observe-only view, never
        // a window's OPENING scope: a window left in EXTERNAL at close reopens in LOCAL (the default
        // working lens) rather than staring at other hosts' claudes. So only LOCAL (0) / GLOBAL (1)
        // restore as saved; EXTERNAL — and any out-of-range value — fall back to LOCAL.
        _treeScope = (state.treeScope == 1) ? TreeScope::Global : TreeScope::Local;
        _UpdateTreeScopeButton();
        _UpdateBoardScopeButton();
        // A restored per-window layout overrides the global default loaded in the ctor; push the
        // fractions into the live tracks so the splitters land where the window left them.
        _layout = state.layout;
        _ApplyLayoutToTracks();
        _Refresh();
    }

    void AgentManagerContent::SetLensChangedHandler(std::function<void(::Agentmaster::ManagerState)> handler)
    {
        _lensChangedHandler = std::move(handler);
    }

    void AgentManagerContent::_NotifyLensChanged()
    {
        if (_lensChangedHandler)
        {
            _lensChangedHandler(GetManagerState());
        }
    }

    void AgentManagerContent::_ApplyLayoutToTracks()
    {
        if (_boardRow)
        {
            _boardRow.Height(GridLengthHelper::FromValueAndType(_layout.boardFraction, GridUnitType::Star));
        }
        if (_bottomRow)
        {
            _bottomRow.Height(GridLengthHelper::FromValueAndType(1.0 - _layout.boardFraction, GridUnitType::Star));
        }
        if (_treeCol)
        {
            _treeCol.Width(GridLengthHelper::FromValueAndType(_layout.treeFraction, GridUnitType::Star));
        }
        if (_planCol)
        {
            _planCol.Width(GridLengthHelper::FromValueAndType(1.0 - _layout.treeFraction, GridUnitType::Star));
        }
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
    void AgentManagerContent::Focus(FocusState /*reason*/)
    {
        // Agentmaster: park keyboard focus on the INVISIBLE, caret-less sink (built in the ctor) — NOT the
        // cwd box. The host calls IPaneContent::Focus whenever the Manager pane is activated (app open, tab
        // open, tab switch). We must put focus SOMEWHERE inside the pane subtree, or the pane-root routed
        // key handlers (_KeyDownHandler / _ManagerPaneNavPreviewKeyDown — alt+left/right, ctrl+tab,
        // shift+home) never fire (focus stays on the tab header), the bug where those chords did nothing
        // until you clicked an element. Focusing the cwd box was the ORIGINAL behavior but it put a blinking
        // caret in it on every open (and popped the path-picker) — which the user disliked — so we focus a
        // 1x1, opacity-0, no-focus-visual Button instead: keyboard works immediately, NOTHING looks focused,
        // and the path-picker (which keys off the cwd box's own focus) stays shut. Programmatic focus shows
        // no focus visual; if the sink isn't focusable yet (pre-layout on first realize) retry once next tick.
        if (!_focusSink)
        {
            return;
        }
        if (!_focusSink.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic) && _dispatcher)
        {
            auto sink = _focusSink;
            _dispatcher.TryEnqueue([sink]() {
                if (sink)
                {
                    sink.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
                }
            });
        }
    }
    void AgentManagerContent::Close()
    {
    }
    INewContentArgs AgentManagerContent::GetNewTerminalArgs(const BuildStartupKind /*kind*/) const
    {
        return BaseContentArgs(L"agentManager");
    }
    winrt::hstring AgentManagerContent::Title()
    {
        // The Manager tab's title doubles as the at-a-glance which-install-am-I marker: the
        // DEV package (AgentmasterDev — the loose-layout deploy) reads "Agent Manager Dev",
        // the release install plain "Agent Manager". Runtime identity, not a build flag, so
        // one binary serves both installs; cached — the package family never changes mid-run.
        static const winrt::hstring title = ::Agentmaster::Profiles::IsDevPackage() ?
                                                winrt::hstring{ L"Agent Manager Dev" } :
                                                winrt::hstring{ L"Agent Manager" };
        return title;
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
            // Agentmaster: the toolbar is a VERTICAL stack — a TOP row (the "Agentmaster" title +
            // the launch controls) over a compact ACTIONS row (Settings, Pause Autopilot, Sessions,
            // Keep Awake) tucked just below the title in the top-left. The actions buttons are
            // deliberately thinner (smaller font + slim padding), matching the header-toggle idiom.
            _toolbarCol = StackPanel{};
            _toolbarCol.Orientation(Orientation::Vertical);
            _toolbarCol.Spacing(6);
            _toolbarCol.Margin(Thickness{ 12, 8, 12, 0 });
            auto& toolbarCol = _toolbarCol;

            _launchBar = StackPanel{}; // top row: the title + launch controls
            _launchBar.Orientation(Orientation::Horizontal);
            _launchBar.Spacing(8);
            _launchBar.VerticalAlignment(VerticalAlignment::Center);
            auto& bar = _launchBar;

            // The compact actions row, left-aligned directly under the title. Its buttons are
            // appended below as each is built; the row itself is added to toolbarCol at the end.
            auto actionsRow = StackPanel{};
            actionsRow.Orientation(Orientation::Horizontal);
            actionsRow.Spacing(6);
            actionsRow.HorizontalAlignment(HorizontalAlignment::Left);
            actionsRow.VerticalAlignment(VerticalAlignment::Center);

            // Agentmaster (responsive launch bar): "Agentmaster" + "\x2014" stay; "launch a" / "session in"
            // (below) collapse first when the pane narrows (_ReflowLaunchBar), leaving "Agentmaster \x2014
            // [\x25CF Claude] [box]". Built as members so reflow can toggle their Visibility.
            _agentmasterText = Text(L"Agentmaster", 18, true, 1.0);
            bar.Children().Append(_agentmasterText);
            _dashText = Text(L"\x2014", 13, false, 0.6); // em-dash, always shown
            bar.Children().Append(_dashText);
            _launchAText = Text(L"launch a", 13, false, 0.6); // collapsible
            bar.Children().Append(_launchAText);

            // Agentmaster (Codex-launch): the agent toggle — Claude (default) <-> Codex. Click cycles it
            // (the scope/sort/autopilot toggle idiom). It retargets the SAME cwd box + Launch button, so a
            // managed Codex launches exactly the way a Claude does ("do what we do for Claude"). A Codex
            // launch is directory-only — Codex has no typed-id resume/fork here (Codex resume is reached via
            // the Archive page / window-restore / EXTERNAL Adopt), so _ValidateLaunchBox suppresses those.
            _launchAgentBtn = Button{};
            _launchAgentBtn.FontSize(11);
            _launchAgentBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_launchAgentBtn, L"Agent to launch \x2014 click to toggle between Claude and Codex (the Launch button and box retarget to match).");
            _launchAgentBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _launchCodex = !_launchCodex;
                _UpdateLaunchAgentButton();
                _ValidateLaunchBox(); // repaint the Launch button text + resume/fork affordances for the new agent
            });
            bar.Children().Append(_launchAgentBtn);
            _UpdateLaunchAgentButton();

            _sessionInText = Text(L"session in", 13, false, 0.6); // collapsible (with "launch a")
            bar.Children().Append(_sessionInText);

            _cwdBox = TextBox{};
            // Agentmaster: 504 (the old fixed width) is the COMFORTABLE width; the box grows with its typed
            // content — a NoWrap TextBox in the horizontal launch bar measures to its text — between the
            // live MinWidth/MaxWidth that _ReflowLaunchBar sets per stage (504 floor when there's room; a
            // 240/160 floor once the pane narrows enough to shrink / wrap), so a long path never pushes the
            // Launch button off-screen. Left alignment keeps it content-sized rather than stretched-to-fill.
            _cwdBox.MinWidth(504);
            _cwdBox.MaxWidth(504); // seed; _ReflowLaunchBar (re)computes Min/Max per stage on first layout
            _cwdBox.HorizontalAlignment(HorizontalAlignment::Left);
            _cwdBox.PlaceholderText(L"working directory (the M axis)");
            AgentSetTip(_cwdBox, L"Where to launch: a working directory for a new session, or a Claude session id to resume or fork. Start typing to pick from recent and matching folders."); // Agentmaster: the box accepts EITHER a working dir (new session) OR a session id (Resume / Fork)
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
                // Agentmaster: paint the validation underline + enable/disable launch on every edit
                // (a working dir OR a session id). Runs before the picker logic's focus early-out so
                // it always reflects the current text.
                _ValidateLaunchBox();
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
            // Agentmaster: the box + a validation underline beneath it, in a vertical column so the
            // underline tracks the box width. The path-picker Popup anchors via _cwdBox.TransformToVisual
            // (robust to this wrapping), so the dropdown placement is unaffected.
            auto cwdCol = StackPanel{};
            cwdCol.Orientation(Orientation::Vertical);
            cwdCol.Spacing(2);
            cwdCol.VerticalAlignment(VerticalAlignment::Center);
            cwdCol.Children().Append(_cwdBox);
            _cwdUnderline = Border{};
            _cwdUnderline.Height(2);
            // Stretch (no fixed width) so the underline always spans the box's CURRENT width: the box now
            // grows with content and cwdCol's width tracks it, so a stretched underline stays matched.
            _cwdUnderline.HorizontalAlignment(HorizontalAlignment::Stretch);
            _cwdUnderline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
            _cwdUnderline.Background(Fill(0x00, 0x00, 0x00, 0x00)); // transparent = neutral; kept present so painting it never reflows the bar
            cwdCol.Children().Append(_cwdUnderline);
            bar.Children().Append(cwdCol);

            // Agentmaster (responsive launch bar): the cwd box's width + the whole top row reflow as the
            // pane narrows — _ReflowLaunchBar stages it (full label -> collapse "launch a"/"session in" ->
            // shrink the box to its floor -> wrap the launch buttons to their own line). Driven off _root's
            // SizeChanged; the box grows with its content between the live Min/Max the reflow sets. Setting
            // a child's width / visibility / parent never resizes _root (the pane owns _root's size), so
            // there's no layout loop.
            if (_root)
            {
                _root.SizeChanged([this](const IInspectable&, const SizeChangedEventArgs& e) {
                    _lastRootWidth = e.NewSize().Width;
                    _ReflowLaunchBar();
                });
            }

            // Agentmaster (responsive launch bar): the launch buttons (Launch / Fork / Reopen / Activate)
            // live in their OWN panel so _ReflowLaunchBar can move the whole group to a 2nd line (below the
            // title row) when the bar can no longer fit them inline beside a floored cwd box.
            _launchBtns = StackPanel{};
            _launchBtns.Orientation(Orientation::Horizontal);
            _launchBtns.Spacing(8);
            _launchBtns.VerticalAlignment(VerticalAlignment::Center);

            _launchBtn = Button{};
            _launchBtn.Content(winrt::box_value(L"Launch Claude"));
            AgentSetTip(_launchBtn, L"Start the selected agent in the working directory above \x2014 or resume the conversation when a session id is entered.");
            _launchBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnLaunch(); });
            _launchBtns.Children().Append(_launchBtn);

            // Fork — hidden unless the box holds a FOUND session id; forks that conversation into a
            // NEW one (the original transcript is untouched), mirroring the Sessions page's "Fork here".
            _forkBtn = Button{};
            _forkBtn.Content(winrt::box_value(L"Fork"));
            _forkBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_forkBtn, L"Fork the entered session into a NEW, independent conversation \x2014 the original transcript is left untouched.");
            _forkBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnForkFromBox(); });
            _launchBtns.Children().Append(_forkBtn);

            _ValidateLaunchBox(); // initial state for the seeded cwd (USERPROFILE -> neutral, enabled)

            // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): the "Reopen Windows (N)" recover
            // button — the "if I answered No" path. It reopens saved windows that are NOT currently
            // open (the runtime analog of the WindowEmperor's startup reopen loop). Hidden when there
            // is nothing to recover (N==0); _UpdateReopenButton (driven from _Refresh) maintains both.
            _reopenBtn = Button{};
            _reopenBtn.Content(winrt::box_value(L"Reopen Windows"));
            _reopenBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_reopenBtn, L"Reopen saved windows that aren't currently open \x2014 restores each window's tabs, layout, and sessions.");
            _reopenBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnReopenWindows(); });
            _launchBtns.Children().Append(_reopenBtn);

            // Agentmaster (eager-init): "Activate All Tabs (N)" — wake every DORMANT managed tab in this
            // window (a window-restored / re-homed tab spawns its claude lazily, only when first shown; this
            // starts them all IN PLACE without switching tabs). Hidden when N==0 (the hide-when-idle idiom,
            // like Reopen Windows); _UpdateActivateAllButton (driven from _Refresh) maintains label + show.
            // If OTHER windows also have dormant tabs, the click prompts to choose the scope (_OnActivateAllTabs).
            _activateAllBtn = Button{};
            _activateAllBtn.Content(winrt::box_value(L"Activate All Tabs"));
            _activateAllBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_activateAllBtn, L"Start every Claude session in this window that hasn't initialized yet (restored tabs you haven't opened) \x2014 in place, without switching tabs. Their half-hollow dots fill as they start.");
            _activateAllBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnActivateAllTabs(); });
            _launchBtns.Children().Append(_activateAllBtn);

            // The launch-buttons group sits inline at the end of the title row by default; _ReflowLaunchBar
            // moves it to its own line (between the title row and the actions row) when the bar is too narrow.
            bar.Children().Append(_launchBtns);

            // Settings cog (opens the in-content settings overlay; built at the end of layout).
            // Lives in the compact actions row below the title — thinner, smaller font.
            _settingsBtn = Button{};
            _settingsBtn.FontSize(11);
            _settingsBtn.Padding(Thickness{ 8, 1, 8, 1 });
            {
                FontIcon cog;
                cog.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
                cog.Glyph(L"\xE713"); // Settings (cog)
                cog.FontSize(13);
                _settingsBtn.Content(cog);
            }
            AgentSetTip(_settingsBtn, L"Settings \x2014 model & launch options, Autopilot defaults, the Claude binary, the active profile, and app behavior.");
            _settingsBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ShowSettings(); });
            actionsRow.Children().Append(_settingsBtn);

            // Global Autopilot backstop: Pause-all / Resume-all. Built here but appended LAST, so it
            // sits at the RIGHT end of the actions row (after cog, Sessions, Keep Awake).
            _pauseBtn = Button{};
            _pauseBtn.FontSize(11);
            _pauseBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _pauseBtn.Content(winrt::box_value(L"Pause Autopilot"));
            AgentSetTip(_pauseBtn, L"Global Autopilot backstop \x2014 pauses or resumes auto-sending across ALL sessions at once.");
            _pauseBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _globalPaused = !_globalPaused;
                ::Agentmaster::LogNav(_globalPaused ? L"pause-all on (global Autopilot backstop)" : L"pause-all off (global Autopilot resumed)");
                if (_pauseHandler)
                {
                    _pauseHandler(_globalPaused);
                }
                if (_pauseBtn)
                {
                    _pauseBtn.Content(winrt::box_value(_globalPaused ? L"Resume Autopilot" : L"Pause Autopilot"));
                }
            });
            // (appended LAST — see below, after Keep Awake)

            // Agentmaster (Sessions page; SESSIONS.md / FAVORITES.md): the global on-disk Claude-sessions
            // browser — EVERY session on the machine in a selectable window, searchable, with the ★
            // Favorite column + filter. This is the SOLE history view (the separate "Archived" button +
            // page were removed: closing a session keeps it here, resumable, marked by Favorite).
            _sessionsBtn = Button{};
            _sessionsBtn.FontSize(11);
            _sessionsBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _sessionsBtn.Content(winrt::box_value(L"Sessions"));
            AgentSetTip(_sessionsBtn, L"Browse and search every Claude Code session on this machine \x2014 not just managed ones (last month by default). Star the ones you want to keep.");
            _sessionsBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { if (_openSessionsHandler) { _openSessionsHandler(); } });
            actionsRow.Children().Append(_sessionsBtn);

            // Agentmaster: tri-mode "Keep Awake" button — prevents the PC (and display) from sleeping
            // while a long unattended run is in flight, mirroring the user's stay-awake.ps1. It calls
            // SetThreadExecutionState from this (persistent) UI thread, so ES_CONTINUOUS holds the flag
            // until released — no timer/loop needed (the per-thread state persists for the thread's life).
            // Click cycles Off -> Always -> While-Running; While-Running holds only while a session is
            // actively working (re-evaluated each _Refresh) so the machine can still sleep when all idle.
            _keepAwakeBtn = Button{};
            _keepAwakeBtn.FontSize(11);
            _keepAwakeBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_keepAwakeBtn, L"Keep this PC (and display) awake. Click to cycle: Off \x2192 Always \x2192 While Running (holds only while a session is actively working, so the machine can still sleep once every agent is idle). Released when set Off or the window closes.");
            _keepAwakeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleKeepAwake(); });
            actionsRow.Children().Append(_keepAwakeBtn);
            _UpdateKeepAwakeButton();

            // Pause/Resume Autopilot — the rightmost button in the actions row (built far above).
            actionsRow.Children().Append(_pauseBtn);

            // Stack the compact actions row directly below the top (title + launch) row.
            toolbarCol.Children().Append(bar);
            toolbarCol.Children().Append(actionsRow);
            Grid::SetRow(toolbarCol, 0);
            _root.Children().Append(toolbarCol);
            _ReflowLaunchBar(); // initial pass (no-op until laid out — SizeChanged drives the first real reflow)
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
            // Agentmaster: the board's LOCAL/GLOBAL scope toggle — ONE state with the Explorer
            // Tree's 3-way toggle (same style, same per-window lens persistence): LOCAL shows only
            // this window's sessions, GLOBAL every window's. While the tree sits in EXTERNAL the
            // board reads GLOBAL (it has no External mode); a click then flips the shared scope to
            // LOCAL. _SetTreeScope is the one mutator behind both buttons.
            _boardScopeBtn = Button{};
            _boardScopeBtn.FontSize(11);
            _boardScopeBtn.Padding(Thickness{ 8, 1, 8, 1 });
            EmphasizeScopeButton(_boardScopeBtn); // Agentmaster: the primary header toggle — louder than sort/refresh/Clear
            AgentSetTip(_boardScopeBtn, L"Which sessions the board shows \x2014 LOCAL (this window) or GLOBAL (all windows). Shares one setting with the Explorer Tree's scope; remembered per window.");
            _boardScopeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _SetTreeScope(_treeScope == TreeScope::Local ? TreeScope::Global : TreeScope::Local);
            });
            header.Children().Append(_boardScopeBtn);
            _UpdateBoardScopeButton();
            // Agentmaster: the board's SORT toggle, right after the scope toggle (mirroring the Explorer
            // Tree's scope-then-sort layout). Cycles MOST ACTIVE -> NEWEST -> OLDEST -> A-Z (the tree's set
            // minus BY PID — host/shell grouping is meaningless once cards split across state columns).
            // Default MOST ACTIVE. A SEPARATE global setting from the tree's sort (AppSettings::boardSort),
            // so each remembers its own; persisted + shared by every window (the changing window re-sorts
            // live; others adopt on next launch — the treeSort idiom). _CycleBoardSort advances + persists
            // through the settings sink; _UpdateBoardSortButton paints the label.
            _boardSortBtn = Button{};
            _boardSortBtn.FontSize(11);
            _boardSortBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_boardSortBtn, L"Sort the cards within each column \x2014 MOST ACTIVE (most recent activity first \x2014 the default) \xB7 NEWEST \xB7 OLDEST \xB7 A\x2013Z. Global across windows; saved.");
            _boardSortBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleBoardSort(); });
            header.Children().Append(_boardSortBtn);
            _UpdateBoardSortButton();
            // Agentmaster: a refresh button AFTER the sort toggle — the twin of the Explorer Tree's
            // _treeRefreshBtn. Re-scans + redraws the ENTIRE tab: _Refresh() rebuilds the board, tree
            // AND flight plan (recomputing the live "ago" timing), and _refreshHandler forces the
            // Fleet Observer to re-survey NOW (re-enrich the registry + recompute the external census)
            // instead of waiting for the next tick. Same handler as the tree button by design.
            _boardRefreshBtn = Button{};
            _boardRefreshBtn.FontSize(11);
            _boardRefreshBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _boardRefreshBtn.Content(winrt::box_value(L"\x21BB")); // ↻ refresh glyph
            AgentSetTip(_boardRefreshBtn, L"Refresh now \x2014 re-scan and redraw the whole tab (also re-detects external sessions).");
            _boardRefreshBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _Refresh(); // immediate redraw from current data (board + tree + flight plan; recomputes the "ago" timing)
                if (_refreshHandler)
                {
                    _refreshHandler(); // page: wake the observer + re-probe -> fresh data lands shortly
                }
            });
            header.Children().Append(_boardRefreshBtn);
            // Agentmaster: a "Clear" button right next to LOCAL/GLOBAL — deselect the current card/row
            // (the Flight Plan then shows nothing-selected). Hidden while nothing is selected (kept in
            // sync by _RebuildBoard, like "Show all"); shown once a session/external is selected.
            _clearSelBtn = Button{};
            _clearSelBtn.Content(winrt::box_value(L"Clear"));
            _clearSelBtn.FontSize(11);
            _clearSelBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _clearSelBtn.Visibility(Visibility::Collapsed); // nothing selected at build; _RebuildBoard syncs
            AgentSetTip(_clearSelBtn, L"Deselect the current card / row \x2014 nothing stays selected and the Flight Plan empties.");
            _clearSelBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ClearSelection(); });
            header.Children().Append(_clearSelBtn);
            // The directory-scope label appears ONLY while a directory is scoped ("[scope: <dir>]"
            // next to the "Show all" clear button). The old unscoped "[all directories]"
            // placeholder is gone — it was display-only, restating the default.
            _boardScope = Text(L"", 12, false, 0.6);
            _boardScope.Visibility(Visibility::Collapsed);
            header.Children().Append(_boardScope);
            _showAllBtn = Button{};
            _showAllBtn.Content(winrt::box_value(L"Show all"));
            _showAllBtn.Padding(Thickness{ 6, 0, 6, 0 });
            AgentSetTip(_showAllBtn, L"Show sessions from every directory again \x2014 clears the directory filter.");
            // Hidden while we ARE showing all (the default scope is "" == all directories); it
            // reappears once a directory is scoped. _RebuildBoard keeps this in sync on every refresh.
            _showAllBtn.Visibility(_scopeDir.empty() ? Visibility::Collapsed : Visibility::Visible);
            _showAllBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _SetScope(L""); });
            header.Children().Append(_showAllBtn);
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
                EmphasizeScopeButton(_treeScopeBtn); // Agentmaster: the primary header toggle — louder than sort/refresh
                AgentSetTip(_treeScopeBtn, L"Which sessions the tree shows \x2014 LOCAL (this window), GLOBAL (all windows), or EXTERNAL (claudes running outside Agentmaster, observe-only). Right-click an EXTERNAL row to Adopt it, start a session, or bring its window forward.");
                _treeScopeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ToggleTreeScope(); });
                hdrow.Children().Append(_treeScopeBtn);
                _UpdateTreeScopeButton();

                // Agentmaster: a sort toggle AFTER the scope toggle — NEWEST / OLDEST / MOST ACTIVE
                // (currently-running first) / A-Z. Orders the directory groups AND the rows within
                // each, in every scope (LOCAL/GLOBAL/EXTERNAL). GLOBAL setting: a click persists it
                // (AppSettings::treeSort) so the choice survives restart and applies to new windows.
                _treeSortBtn = Button{};
                _treeSortBtn.FontSize(11);
                _treeSortBtn.Padding(Thickness{ 8, 1, 8, 1 });
                AgentSetTip(_treeSortBtn, L"Sort order for directories and the sessions in them \x2014 NEWEST \xB7 OLDEST \xB7 MOST ACTIVE (running first) \xB7 A\x2013Z \xB7 BY PID (group by host window). Applies to every scope and is saved across windows.");
                _treeSortBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleTreeSort(); });
                hdrow.Children().Append(_treeSortBtn);
                _UpdateTreeSortButton();

                // Agentmaster: a refresh button AFTER the sort toggle — reload the tree's data for the
                // CURRENTLY DISPLAYED scope (LOCAL/GLOBAL: re-pull the registry; EXTERNAL: re-pull the
                // observer's external census). Redraws immediately (recomputes the live "ago" timing)
                // and fires _refreshHandler so the page forces the Fleet Observer to re-survey NOW
                // (re-enrich + recompute the external census) instead of waiting for the next tick.
                _treeRefreshBtn = Button{};
                _treeRefreshBtn.FontSize(11);
                _treeRefreshBtn.Padding(Thickness{ 8, 1, 8, 1 });
                _treeRefreshBtn.Content(winrt::box_value(L"\x21BB")); // ↻ refresh glyph
                AgentSetTip(_treeRefreshBtn, L"Refresh now \x2014 re-scan and redraw the current view (also re-detects external sessions).");
                _treeRefreshBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                    _Refresh(); // immediate redraw from current data (recomputes the "ago" timing)
                    if (_refreshHandler)
                    {
                        _refreshHandler(); // page: wake the observer + re-probe -> fresh data lands shortly
                    }
                });
                hdrow.Children().Append(_treeRefreshBtn);
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
                _planScroll = sv; // Agentmaster: kept so the plan can default-scroll to the bottom (see _PinPlanToBottomOnSubjectChange)

                // Action bar (persistent).
                auto actions = StackPanel{};
                actions.Spacing(6);

                // Autopilot mode now lives as a toggle in the FLIGHT PLAN header (Agentmaster) —
                // _autopilotBtn / _CycleAutopilot / _UpdateAutopilotButton, mirroring the EXPLORER
                // TREE LOCAL/GLOBAL toggle but acting on the selected session. (No combo here.)

                // Compose row (Agentmaster): the action icons stick to the TOP-LEFT and the prompt
                // textarea fills the rest, growing downward as it becomes multiline —
                //   [eye = Focus]  [! = Send now (asks first)]  [envelope = Add to queue]  |  [textarea]
                // Per-message actions (Move up / Move down / Delete / Archive) live on the message
                // right-click menu now (see _MakePromptMenu, attached in _RebuildPlan), not a button row.
                auto fluentGlyph = [](const winrt::hstring& g) {
                    FontIcon fi;
                    fi.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
                    fi.Glyph(g);
                    fi.FontSize(16);
                    return fi;
                };
                // The "!" isn't an icon-font glyph, so render it as a FontIcon in the UI text font
                // (bold). A FontIcon centers its glyph the same way the Segoe Fluent ones above do,
                // so the exclamation lines up with the eye/envelope — a plain TextBlock rode high
                // because its line box reserves descent space below the glyph.
                auto textIconGlyph = [](const winrt::hstring& g) {
                    FontIcon fi;
                    fi.FontFamily(FontFamily{ L"Segoe UI" });
                    fi.Glyph(g);
                    fi.FontSize(16);
                    fi.FontWeight(FontWeights::Bold());
                    return fi;
                };
                auto mkIconBtn = [&](const winrt::hstring& tip, const IInspectable& glyph, std::function<void()> fn) {
                    auto btn = Button{};
                    btn.Content(glyph);
                    btn.Padding(Thickness{ 9, 6, 9, 6 });
                    btn.VerticalAlignment(VerticalAlignment::Top);
                    AgentSetTip(btn, tip);
                    btn.Click([fn](const IInspectable&, const RoutedEventArgs&) { fn(); });
                    return btn;
                };

                auto composeRow = Grid{};
                composeRow.ColumnDefinitions().Append(autoCol()); // icon column (sticks top-left)
                composeRow.ColumnDefinitions().Append(starCol(1)); // textarea fills the rest
                composeRow.ColumnDefinitions().Append(autoCol()); // templates (paper) icon, top-right

                auto iconCol = StackPanel{};
                iconCol.Orientation(Orientation::Horizontal);
                iconCol.Spacing(4);
                iconCol.VerticalAlignment(VerticalAlignment::Top); // stay at the top as the box grows
                iconCol.Margin(Thickness{ 0, 0, 6, 0 });
                // Eye = Focus the session (jump to its live tab).
                iconCol.Children().Append(mkIconBtn(L"Jump to this session's live terminal tab", fluentGlyph(L"\xE7B3"), [this]() {
                    if (_activateHandler && !_selectedId.empty())
                    {
                        _activateHandler(winrt::hstring{ _selectedId });
                    }
                }));
                // Exclamation point = Send now (a literal bold "!"; confirmed before it fires).
                iconCol.Children().Append(mkIconBtn(L"Send the composed prompt now \x2014 confirms first, and skips the queue", textIconGlyph(L"!"), [this]() { _OnSendNow(); }));
                // Envelope = Add the composed prompt to the queue.
                iconCol.Children().Append(mkIconBtn(L"Add the composed prompt to this session's queue", fluentGlyph(L"\xE715"), [this]() { _OnAddPrompt(); }));
                Grid::SetColumn(iconCol, 0);
                composeRow.Children().Append(iconCol);

                _addPromptBox = TextBox{};
                _addPromptBox.PlaceholderText(L"queue a prompt for the selected session\x2026");
                _addPromptBox.AcceptsReturn(true);
                _addPromptBox.TextWrapping(TextWrapping::Wrap);
                _addPromptBox.MinHeight(34);
                _addPromptBox.MaxHeight(160);
                _addPromptBox.VerticalAlignment(VerticalAlignment::Top);
                _addPromptBox.VerticalContentAlignment(VerticalAlignment::Top);
                // TextBox has no direct VerticalScrollBarVisibility in this projection — it's the
                // attached ScrollViewer property (see Gotchas: this XAML projection differs from WPF).
                ScrollViewer::SetVerticalScrollBarVisibility(_addPromptBox, ScrollBarVisibility::Auto);
                // Agentmaster (prompt history): recall the selected session's previously SENT prompts
                // (the shell/REPL idiom) — see _BuildPromptHistory / _ApplyPromptHistoryText. On the live
                // DRAFT (the "bottom prompt") Up enters history once the caret reaches the FIRST VISUAL ROW
                // (so it walks within a wrapped/multi-line draft and only recalls at the very top), while
                // Down is plain caret motion — nothing is newer than the draft, so only it is "moved by
                // Down". Once BROWSING history Up/Down walk older/newer FREELY (no caret gate — a recalled
                // prompt is navigated, not caret-edited; Esc cancels back to the draft), and Down off the
                // newest entry restores the draft. PreviewKeyDown (tunneling) runs BEFORE the TextBox's own
                // arrow handling — the only place we can read the caret's pre-move position AND suppress the
                // default caret motion via Handled (the rename box uses PreviewKeyDown for Enter likewise).
                _addPromptBox.PreviewKeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                    const auto key = e.Key();
                    // Modifier snapshot (mirrors the rename box's CoreWindow::GetKeyState check).
                    bool shift = false, ctrl = false, alt = false;
                    if (const auto w = CoreWindow::GetForCurrentThread())
                    {
                        const auto down = winrt::Windows::UI::Core::CoreVirtualKeyStates::Down;
                        shift = WI_IsFlagSet(w.GetKeyState(VirtualKey::Shift), down);
                        ctrl = WI_IsFlagSet(w.GetKeyState(VirtualKey::Control), down);
                        alt = WI_IsFlagSet(w.GetKeyState(VirtualKey::Menu), down);
                    }
                    // Esc while browsing history cancels back to the draft you started from.
                    if (key == VirtualKey::Escape)
                    {
                        if (_promptHistoryIndex >= 0)
                        {
                            _promptHistoryIndex = -1;
                            _ApplyPromptHistoryText(_promptHistoryDraft);
                            e.Handled(true);
                        }
                        return;
                    }
                    if (key != VirtualKey::Up && key != VirtualKey::Down)
                    {
                        return; // only the bare Up/Down arrows drive history
                    }
                    if (shift || ctrl || alt)
                    {
                        return; // a MODIFIED arrow belongs to the TextBox (Shift selects, Ctrl/Alt move)
                    }
                    if (key == VirtualKey::Up)
                    {
                        if (_promptHistoryIndex < 0)
                        {
                            // On the draft: move the caret up within a wrapped/multi-line draft until the
                            // first VISUAL ROW; only THERE enter history (the "cursor at the top" trigger).
                            if (!_PromptCaretOnFirstRow())
                            {
                                return;
                            }
                            _promptHistory = _BuildPromptHistory();
                            if (_promptHistory.empty())
                            {
                                return; // nothing to recall — leave Up alone
                            }
                            _promptHistoryDraft = _addPromptBox ? std::wstring{ _addPromptBox.Text() } : std::wstring{};
                            _promptHistoryIndex = 0;
                            _ApplyPromptHistoryText(_promptHistory[_promptHistoryIndex]);
                        }
                        else if (_promptHistoryIndex + 1 < static_cast<int>(_promptHistory.size()))
                        {
                            // Browsing history: Up walks OLDER freely (no caret gate).
                            ++_promptHistoryIndex;
                            _ApplyPromptHistoryText(_promptHistory[_promptHistoryIndex]);
                        }
                        // else: already at the oldest — fall through to swallow so the caret doesn't jump.
                        e.Handled(true);
                    }
                    else // VirtualKey::Down (others returned above)
                    {
                        if (_promptHistoryIndex < 0)
                        {
                            return; // the bottom prompt (draft) — nothing newer; let Down move the caret
                        }
                        // Browsing history: Down walks NEWER freely (no caret gate). Off the newest entry
                        // it restores the draft you had before entering history (back to the bottom prompt).
                        if (_promptHistoryIndex == 0)
                        {
                            _promptHistoryIndex = -1;
                            _ApplyPromptHistoryText(_promptHistoryDraft);
                        }
                        else
                        {
                            --_promptHistoryIndex; // newer
                            _ApplyPromptHistoryText(_promptHistory[_promptHistoryIndex]);
                        }
                        e.Handled(true);
                    }
                });
                // Agentmaster (prompt history): a real user edit leaves history navigation — the recalled
                // text becomes the new draft, so the next Up at the top line walks history fresh. Our own
                // recall writes set _promptHistoryNavigating, which suppresses this reset.
                _addPromptBox.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) {
                    if (!_promptHistoryNavigating)
                    {
                        _ResetPromptHistory();
                    }
                });
                // Discoverability: surface the keyboard affordances (they have no on-screen control).
                AgentSetTip(_addPromptBox, L"Compose a prompt for the selected session.\n\x2191 / \x2193  recall previously sent prompts");
                Grid::SetColumn(_addPromptBox, 1);
                composeRow.Children().Append(_addPromptBox);

                // Paper icon at the textarea's TOP-RIGHT: toggles the (collapsed-by-default)
                // Templates row open/closed (Agentmaster). Kept inline (not a Flyout) so its
                // TextBox keeps receiving keypresses — a text box in a popup/ContentDialog gets
                // none in XAML Islands (see Gotchas).
                auto paperBtn = mkIconBtn(L"Templates \x2014 save the current queue as a plan, or apply a saved one", fluentGlyph(L"\xE8A5"), [this]() {
                    if (_templatesRow)
                    {
                        _templatesRow.Visibility(_templatesRow.Visibility() == Visibility::Visible ? Visibility::Collapsed : Visibility::Visible);
                    }
                });
                paperBtn.Margin(Thickness{ 6, 0, 0, 0 });
                Grid::SetColumn(paperBtn, 2);
                composeRow.Children().Append(paperBtn);

                actions.Children().Append(composeRow);

                // mkBtn — the plain text buttons used by the Templates row below.
                auto mkBtn = [&](const winrt::hstring& label, const winrt::hstring& tip, std::function<void()> fn) {
                    auto btn = Button{};
                    btn.Content(winrt::box_value(label));
                    AgentSetTip(btn, tip);
                    btn.Click([fn](const IInspectable&, const RoutedEventArgs&) { fn(); });
                    return btn;
                };

                // Templates row (M8): save the current plan, apply a saved plan to this session or
                // broadcast it to every session in the directory. Collapsed by default — the paper
                // icon at the textarea's top-right toggles it open (Agentmaster).
                _templatesRow = StackPanel{};
                _templatesRow.Orientation(Orientation::Horizontal);
                _templatesRow.Spacing(6);
                _templatesRow.Visibility(Visibility::Collapsed);
                _templateNameBox = TextBox{};
                _templateNameBox.Width(150);
                _templateNameBox.PlaceholderText(L"template name");
                AgentSetTip(_templateNameBox, L"Name to save the current queue under as a reusable template");
                _templatesRow.Children().Append(_templateNameBox);
                _templatesRow.Children().Append(mkBtn(L"Save as template", L"Save the selected session's current queue as a reusable plan, under the name on the left", [this]() { _OnSaveTemplate(); }));
                _templateCombo = ComboBox{};
                _templateCombo.MinWidth(140);
                AgentSetTip(_templateCombo, L"Pick a saved plan template to apply");
                _templatesRow.Children().Append(_templateCombo);
                _templatesRow.Children().Append(mkBtn(L"Apply", L"Append the selected template's prompts to this session's queue", [this]() { _OnApplyTemplate(false); }));
                _templatesRow.Children().Append(mkBtn(L"Apply to dir", L"Append the selected template's prompts to EVERY session in this directory", [this]() { _OnApplyTemplate(true); }));
                actions.Children().Append(_templatesRow);
                _RefreshTemplateCombo();

                Grid::SetRow(actions, 2);
                outer.Children().Append(actions);

                // Top line (Agentmaster): a two-state [Summary | Flight Plan] segmented toggle that
                // REPLACES the old "FLIGHT PLAN" label — a COMPACT pill split in two, only one half
                // "checked" at a time. Both halves share ONE width (symmetric), sized to fit the LONGER
                // label ("Flight Plan", measured in its bold/selected form so it never clips), and the
                // pill is LEFT-aligned rather than stretched across the pane. The selected half is accent-
                // filled (holds through hover/press via PaintHoldButton — the scope-toggle accent) + bold;
                // the other reads as the inactive segment. Summary is the default and the choice is GLOBAL
                // (AppSettings::flightPlanShowsSummary), so it persists + syncs across every window (see
                // _SelectPlanPaneTab / _UpdatePlanPaneTab). The Summary tab is empty for now; the Flight
                // Plan tab holds the existing pane (Autopilot + queue + compose box).
                auto tabBar = Grid{};
                tabBar.HorizontalAlignment(HorizontalAlignment::Left); // compact — size to the two segments, don't stretch the pane width
                tabBar.ColumnDefinitions().Append(autoCol()); // Summary segment (fixed symmetric width)
                tabBar.ColumnDefinitions().Append(autoCol()); // Flight Plan segment
                // Symmetric segment width = the wider label's measured width (measure the SemiBold form —
                // the selected state — so a bold label never clips) + horizontal padding + a little slack.
                // Both segments take this one width, so "Summary" is simply padded out to match "Flight Plan".
                const double tabFont = 11.0;
                const double tabHPad = 10.0;
                const auto measureLabel = [tabFont](const winrt::hstring& s) -> double {
                    TextBlock t;
                    t.Text(s);
                    t.FontSize(tabFont);
                    t.FontWeight(FontWeights::SemiBold());
                    t.Measure(winrt::Windows::Foundation::Size{ 10000.0f, 10000.0f });
                    return static_cast<double>(t.DesiredSize().Width);
                };
                const double wSummary = measureLabel(L"Summary");
                const double wFlight = measureLabel(L"Flight Plan");
                const double tabLabelW = wFlight > wSummary ? wFlight : wSummary;
                const double tabSegW = (tabLabelW > 1.0 ? tabLabelW : 80.0) + tabHPad * 2 + 8.0; // + padding + slack (fallback if Measure runs pre-tree)
                auto mkTabBtn = [&](const winrt::hstring& label, const winrt::hstring& tip, const CornerRadius& cr, bool summary) {
                    auto btn = Button{};
                    btn.Content(winrt::box_value(label));
                    btn.FontSize(tabFont);
                    btn.Padding(Thickness{ tabHPad, 2, tabHPad, 2 });
                    btn.Width(tabSegW); // both segments equal -> symmetric, sized to fit the longer label
                    btn.HorizontalContentAlignment(HorizontalAlignment::Center);
                    btn.CornerRadius(cr); // outer edges rounded, the middle seam square -> reads as one segmented pill
                    AgentSetTip(btn, tip);
                    btn.Click([this, summary](const IInspectable&, const RoutedEventArgs&) { _SelectPlanPaneTab(summary); });
                    return btn;
                };
                _summaryTabBtn = mkTabBtn(L"Summary", L"Summary \x2014 a per-session overview (coming soon).", CornerRadius{ 6, 0, 0, 6 }, true);
                _flightPlanTabBtn = mkTabBtn(L"Flight Plan", L"Flight Plan \x2014 the selected session's prompt queue, Autopilot, and compose box.", CornerRadius{ 0, 6, 6, 0 }, false);
                Grid::SetColumn(_summaryTabBtn, 0);
                tabBar.Children().Append(_summaryTabBtn);
                Grid::SetColumn(_flightPlanTabBtn, 1);
                tabBar.Children().Append(_flightPlanTabBtn);

                // Flight Plan TAB body: a thin strip carrying the Autopilot toggle (relocated from the
                // old header — mirrors the EXPLORER TREE toggle but acts on the SELECTED session; a colored
                // state dot cycles Off -> Semi-auto -> Full, dim/disabled with no live session) over the
                // existing prompt list / compose box (`outer`).
                _autopilotBtn = Button{};
                _autopilotBtn.FontSize(11);
                _autopilotBtn.Padding(Thickness{ 8, 1, 8, 1 });
                AgentSetTip(_autopilotBtn, L"Autopilot for the selected session \x2014 click to cycle: Off (manual) \xB7 Semi-auto (you confirm each send) \xB7 Full (auto-send the queue when a turn completes).");
                _autopilotBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleAutopilot(); });
                _UpdateAutopilotButton(AutopilotMode::Off, false);
                auto apStrip = StackPanel{};
                apStrip.Orientation(Orientation::Horizontal);
                apStrip.HorizontalAlignment(HorizontalAlignment::Right);
                apStrip.Margin(Thickness{ 0, 0, 0, 6 });
                apStrip.Children().Append(_autopilotBtn);

                _flightPlanBody = Grid{};
                _flightPlanBody.RowDefinitions().Append(autoRow()); // 0: Autopilot strip
                _flightPlanBody.RowDefinitions().Append(starRow(1)); // 1: the existing body (`outer`)
                Grid::SetRow(apStrip, 0);
                _flightPlanBody.Children().Append(apStrip);
                Grid::SetRow(outer, 1);
                _flightPlanBody.Children().Append(outer);

                // Summary TAB body (Agentmaster): the SAME session-summary box the Sessions page + the
                // per-tab overlay render (RenderSessionSummaryBox), shown for the selected managed Claude
                // session — analyzed off-thread + cached (see _RefreshSummaryTab / _LoadSummaryForSession),
                // user MESSAGES reversed to newest-first, and the WHOLE box inside ONE inner ScrollViewer
                // so the narrow Flight-Plan pane scrolls a long summary instead of clipping it.
                _summaryHost = Grid{};
                _summaryBoxHost = StackPanel{};
                _summaryBoxHost.Spacing(0); // the rendered box manages its own spacing (mono TextBlocks + rules)
                _summaryBoxHost.Margin(Thickness{ 0, 6, 6, 0 }); // a little top gap below the toggle + right gap clear of the scrollbar
                {
                    auto ssv = ScrollViewer{};
                    ssv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto); // the inner scrollbar
                    ssv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
                    ssv.Content(_summaryBoxHost);
                    _summaryScroll = ssv;
                    _summaryHost.Children().Append(ssv);
                }

                // Both tab bodies share one grid cell; _UpdatePlanPaneTab toggles which is Visible.
                auto contentArea = Grid{};
                contentArea.Children().Append(_summaryHost);
                contentArea.Children().Append(_flightPlanBody);

                auto wrap = Grid{};
                wrap.RowDefinitions().Append(autoRow()); // 0: the [Summary | Flight Plan] toggle
                wrap.RowDefinitions().Append(starRow(1)); // 1: the selected tab's body
                Grid::SetRow(tabBar, 0);
                wrap.Children().Append(tabBar);
                Grid::SetRow(contentArea, 1);
                wrap.Children().Append(contentArea);

                _UpdatePlanPaneTab(); // initial paint + visibility from _appSettings (default: Summary)

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

        // Agentmaster (FAVORITES.md): both the in-content archive overlay AND the full-window Archive
        // page are GONE — the Sessions browser is the sole history view (closed sessions stay there,
        // resumable, marked by Favorite). Nothing archive-related is built here anymore.
        _BuildSettingsOverlay(); // modal settings layer, appended last so it renders on top
        _BuildClaudeMissingOverlay(); // native-exe-only gate modal (shown when no claude.exe is detected)
    }

    // ---- Refresh / rebuild --------------------------------------------------

    void AgentManagerContent::_Refresh()
    {
        if (!_root || !_boardHost || !_treeHost || !_planListHost)
        {
            return;
        }

        // Agentmaster: preserve keyboard focus across the rebuild below. _Refresh fires on ANY
        // registry notification — including a mere title change (a rename, or claude floating its
        // OSC title) — and _RebuildBoard/_RebuildTree CLEAR + recreate every card/row Button, which
        // would otherwise drop keyboard focus off the card the user just clicked (the selection
        // highlight survives via _selectedId; the focused ELEMENT does not). Capture the focused
        // card/row's lens+id from its "b:<id>"/"t:<id>" Tag, then re-focus the rebuilt element after.
        //
        // Agentmaster (focus-steal fix): do this ONLY when this Manager's window is the OS FOREGROUND
        // window. In XAML Islands, Control.Focus() escalates to Win32 activation of the island's host
        // window — so re-focusing a card here while this window is in the BACKGROUND (the Triage Board
        // window B sitting behind a Claude tab you're working in, in window A) would yank the OS
        // foreground to B on every ~2s observer/registry tick (the reported "an interval steals focus
        // to the triage board window"). When backgrounded the user isn't keyboard-navigating this
        // board, so there is nothing to preserve — leave refocusTag empty and the restore block below
        // no-ops. No provider (standalone/tests) ⇒ keep the original always-restore behavior.
        const bool windowIsForeground = !_windowForegroundProvider || _windowForegroundProvider();
        std::wstring refocusTag;
        auto refocusState = FocusState::Unfocused;
        if (const auto xr = windowIsForeground ? _root.XamlRoot() : nullptr)
        {
            if (const auto fe = winrt::Windows::UI::Xaml::Input::FocusManager::GetFocusedElement(xr).try_as<FrameworkElement>())
            {
                const std::wstring tag{ winrt::unbox_value_or<winrt::hstring>(fe.Tag(), winrt::hstring{}) };
                if (tag.rfind(L"b:", 0) == 0 || tag.rfind(L"t:", 0) == 0)
                {
                    refocusTag = tag;
                    if (const auto ctrl = fe.try_as<Control>())
                    {
                        refocusState = ctrl.FocusState();
                    }
                }
            }
        }

        std::vector<SessionInfo> sessions;
        if (_registry)
        {
            sessions = _registry->Snapshot();
        }
        _RebuildBoard(sessions);
        _SyncProgressTimer(); // Agentmaster: run the 1s countdown-bar drainer iff any Waiting-for-you bar is now tracked
        _RebuildTree(sessions);
        _RebuildPlan(sessions);
        _RefreshSummaryTab(); // Agentmaster: refresh the Summary tab (cheap; no-op unless that tab is active)
        _UpdateReopenButton();
        _UpdateActivateAllButton(); // Agentmaster (eager-init): recount this window's dormant tabs -> label + show/hide
        _RefreshKeepAwakeHold(&sessions); // Agentmaster: WhileRunning mode tracks the fleet live (no-op for Off/Always)

        // Re-focus the same card/row if a tagged one held focus and still exists post-rebuild (it may
        // have moved columns on a state change, or be gone if archived — then we leave focus be). The
        // maps are layout-independent (filled during the rebuild), so this is reliable pre-layout.
        if (!refocusTag.empty() && refocusState != FocusState::Unfocused)
        {
            const auto& map = (refocusTag.front() == L'b') ? _boardCardsById : _treeRowsById;
            const auto it = map.find(refocusTag.substr(2));
            if (it != map.end() && it->second)
            {
                // A freshly-rebuilt card/row isn't focusable until it Loads (the same reason the
                // in-place rename box focuses in its Loaded). Try now; if it isn't ready yet, re-try
                // on Loaded. The handler captures only the state (the sender IS the element), so there
                // is no element<->handler cycle to leak the card.
                if (!it->second.Focus(refocusState))
                {
                    it->second.Loaded([refocusState](const IInspectable& s, const RoutedEventArgs&) {
                        if (const auto c = s.try_as<Control>())
                        {
                            c.Focus(refocusState);
                        }
                    });
                }
            }
        }
    }

    // Agentmaster (Waiting-for-you countdown bar): drain every tracked bar to its current fraction.
    // Cheap — sets ScaleX on a handful of ScaleTransforms (a render-transform write, no layout/rebuild).
    // Ticked ~1s by _progressTimer. A bar at 0 has expired (a read card decays to Idle on the scanner's
    // next tick, which rebuilds the board and drops the track); an unread one sits empty until read.
    void AgentManagerContent::_UpdateCardProgress()
    {
        if (_cardProgress.empty())
        {
            return;
        }
        const int64_t now = NowMs();
        for (const auto& p : _cardProgress)
        {
            if (!p.bar || p.timeoutMs <= 0)
            {
                continue;
            }
            const auto st = p.bar.RenderTransform().try_as<ScaleTransform>();
            if (!st)
            {
                continue;
            }
            double frac = 1.0 - static_cast<double>(now - p.lastActivityUnixMs) / static_cast<double>(p.timeoutMs);
            frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
            st.ScaleX(frac);
        }
    }

    // Agentmaster (Waiting-for-you countdown bar): start the ~1s drainer iff any bar is tracked, else
    // stop it (no Waiting-for-you cards -> no per-second work). Built lazily; weak self so a closed
    // window never leaks a ticking timer. Called at the end of each board rebuild (from _Refresh).
    void AgentManagerContent::_SyncProgressTimer()
    {
        if (_cardProgress.empty())
        {
            if (_progressTimer)
            {
                _progressTimer.Stop();
            }
            return;
        }
        if (!_progressTimer)
        {
            _progressTimer = DispatcherTimer{};
            _progressTimer.Interval(std::chrono::seconds(1));
            _progressTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
                if (auto self = weak.get())
                {
                    self->_UpdateCardProgress();
                }
                else if (const auto t = sender.try_as<DispatcherTimer>())
                {
                    t.Stop(); // page destroyed — stop ticking (UI thread, safe)
                }
            });
        }
        if (!_progressTimer.IsEnabled())
        {
            _progressTimer.Start();
        }
        _UpdateCardProgress(); // paint the correct fractions NOW (don't wait up to 1s for the first tick)
    }

    Button AgentManagerContent::_MakeCard(const SessionInfo& s)
    {
        const bool selected = (s.id == _selectedId);
        const auto accent = StateColor(s.state);

        // Agentmaster: the card BODY (everything below the colored title band) — codex pill,
        // working dir, model·effort, timing, autopilot badge. The title itself lives in the band.
        auto stack = StackPanel{};
        stack.Spacing(2);

        // Agentmaster: a colored TITLE BAND across the top of the card, painted the session's
        // working-directory color — the SAME permanent color the dir's terminal TABS wear (Rule
        // #12 / dir-colors.json) — so a card reads its folder at a glance and clusters with its
        // siblings across the state columns. Its TOP corners follow the card's rounding while its
        // BOTTOM is a straight edge (square corners) where it meets the neutral body: it covers
        // ONLY the title. The title text flips black/white for contrast (PreferDarkTextOn) so it
        // stays legible on a LIGHT or DARK band. Falls back to the neutral card fill (white text)
        // if the dir has no resolvable color. Persisted color first (matches the tab exactly),
        // else the deterministic auto color — the same precedence the Sessions-page chip uses.
        std::optional<Color> bandColor;
        {
            const auto hex = ::Agentmaster::GetDirColor(s.workingDir);
            bandColor = HexToColor(hex ? *hex : ::Agentmaster::AutoDirColorHex(s.workingDir));
        }
        const std::wstring_view fullTitle = s.title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ s.title };
        auto titleText = Text(OneLine(fullTitle), 14, true, 1.0);
        if (bandColor)
        {
            const uint8_t ink = PreferDarkTextOn(*bandColor) ? 0x10 : 0xFF; // near-black on light, white on dark
            titleText.Foreground(Fill(0xFF, ink, ink, ink));
        }
        Border band;
        band.Background(bandColor ? SolidColorBrush{ *bandColor } : Fill(selected ? 0x40 : 0x20, 0x80, 0x80, 0x80));
        band.CornerRadius(CornerRadius{ 4, 4, 0, 0 }); // rounded top (matches the card), straight bottom edge
        band.Padding(Thickness{ 8, 4, 8, 4 });
        // Agentmaster (eager-init / "Activate Tab"): a DORMANT card (live but its claude hasn't started —
        // a restored tab the user never opened) leads its title band with the half-hollow state dot, so the
        // board flags which cards still need waking (the card has no always-on state dot otherwise — its
        // column conveys state). A started card is unchanged (no dot). Right-click / "Activate All Tabs (N)"
        // wakes them; the dot fills once the control starts.
        if (IsSessionDormant(s))
        {
            auto bandRow = StackPanel{};
            bandRow.Orientation(Orientation::Horizontal);
            bandRow.Spacing(6);
            bandRow.VerticalAlignment(VerticalAlignment::Center);
            bandRow.Children().Append(StateDotDormant(StateColor(s.state)));
            bandRow.Children().Append(titleText);
            band.Child(bandRow);
        }
        else
        {
            band.Child(titleText);
        }
        // Agentmaster: hovering the title band (the card's "top label") shows the FULL title \x2014 the
        // band trims with an ellipsis on a narrow card and OneLine() collapses a multi-line title for
        // the dense card, so the complete name is otherwise unreadable here. When Claude Code has written
        // an idle RECAP for this session (the >5-min "what we did / what's next" away_summary, mirrored
        // onto SessionInfo.recap by the scanner), append it below the title \x2014 so a hover tells the
        // sessions apart at a glance (the whole point of the recap), not just by name. Shown in FULL \x2014
        // never length-capped (the tooltip wraps / grows as needed).
        std::wstring bandTip{ fullTitle };
        if (!s.recap.empty())
        {
            bandTip += L"\n\n";
            bandTip += s.recap;
        }
        AgentSetTip(band, winrt::hstring{ bandTip }, kCardTipDelay);

        // Agentmaster (PENDING_INPUT.md): an UNSENT-DRAFT pulse at the top of the card body — when the
        // Fleet Observer detects the user has typed but not yet submitted a message in this session's
        // input box (the debounced SessionInfo::pendingInput), a goldenrod "3 dots" animation rides here,
        // mirroring the tab-strip pulse. The card rebuilds on the pending flip notify (SetPendingInput
        // notifies on the empty<->non-empty transition), so the dots appear/clear with the draft; a hover
        // previews the draft's first line. (Claude-only — Codex sessions aren't draft-scanned in v1.)
        if (!s.pendingInput.empty())
        {
            auto dots = BuildPendingDots();
            std::wstring tip = L"Unsent draft \x2014 a message is typed into this session's input box but hasn't been sent yet.";
            auto firstLine = s.pendingInput.substr(0, s.pendingInput.find(L'\n'));
            if (firstLine.size() > 120)
            {
                firstLine = firstLine.substr(0, 120) + L"\x2026";
            }
            tip += L"\n\n\x201C" + firstLine + L"\x201D";
            AgentSetTip(dots, winrt::hstring{ tip }, kCardTipDelay);
            stack.Children().Append(dots);
        }

        // Agentmaster (Codex-launch): a teal "codex" agent pill so a MANAGED Codex card reads distinct
        // from Claude (the implicit default — no pill, visuals unchanged).
        if (s.kind == AgentKind::Codex)
        {
            auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
            cp.Opacity(0.9);
            cp.HorizontalAlignment(HorizontalAlignment::Left);
            AgentSetTip(cp, L"Codex agent \x2014 this managed session runs the OpenAI Codex CLI instead of Claude.", kCardTipDelay);
            stack.Children().Append(cp);
        }
        {
            // The working dir reads as plain gray text under the title; name it AND explain the
            // per-directory color (a non-obvious concept) in one tip.
            auto dirText = Text(winrt::hstring{ s.workingDir }, 11, false, 0.6);
            AgentSetTip(dirText, L"Working directory \x2014 where this session runs. Every session in this folder shares the title-band color.", kCardTipDelay);
            stack.Children().Append(dirText);
        }

        // Agentmaster (API-error triage): when this card is in the Error state, show WHY the turn died
        // \x2014 the preserved error message + its HTTP status code (SessionInfo.errorMessage/errorStatus,
        // set by the scanner's recon-error). So the Error column is actionable at a glance ("\x26A0 429:
        // API Error: Server is temporarily limiting requests \x2026 Rate limited") instead of a bare
        // crimson dot. The code is prefixed so it stays visible if the line wraps/clips; the FULL,
        // untruncated message is in the hover tooltip. Crimson, matching the column + the state dot.
        if (s.state == SessionState::Error && !s.errorMessage.empty())
        {
            std::wstring shown = s.errorMessage;
            for (auto& c : shown)
            {
                if (c == L'\r' || c == L'\n' || c == L'\t')
                {
                    c = L' '; // a tidy card line; the tooltip keeps the original
                }
            }
            if (shown.size() > 200)
            {
                shown.resize(200);
                shown += L"\x2026"; // bound the card height; full text in the tip
            }
            std::wstring prefix = L"\x26A0 "; // ⚠
            if (s.errorStatus > 0)
            {
                prefix += std::to_wstring(s.errorStatus) + L": ";
            }
            auto errText = Text(winrt::hstring{ prefix + shown }, 11, false, 1.0);
            errText.Foreground(SolidColorBrush{ StateColor(SessionState::Error) });
            errText.TextWrapping(TextWrapping::Wrap); // show the reason fully (up to the 200-char bound)
            std::wstring tip = s.errorMessage;
            if (s.errorStatus > 0)
            {
                tip = L"HTTP " + std::to_wstring(s.errorStatus) + L"\n\n" + tip;
            }
            AgentSetTip(errText, winrt::hstring{ tip }, kCardTipDelay);
            stack.Children().Append(errText);
        }

        // Per-session timing (created-ago / active-for / last-activity-ago) from the transcript.
        {
            const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
            if (auto t = TimingText(s.convCreatedUnixMs, last))
            {
                stack.Children().Append(t);
            }
        }

        // Agentmaster: context-window occupancy as a raw TOKEN COUNT (PR feedback, Eli). A % needs a
        // context-window denominator, and the 200K-vs-1M window can't be reliably known from the model
        // id (Opus 4.8 doesn't advertise its 1M variant), so a % gave misleading numbers — the raw
        // token count is unambiguous and matches what Claude Code reports for the session. This is the
        // newest assistant turn's usage (input + cache_creation + cache_read + output ≈ what's in the
        // session's context right now), filled by the SessionScanner. Shown once usage exists.
        if (s.contextTokens > 0)
        {
            auto ctxText = Text(winrt::hstring{ L"ctx " } + winrt::hstring{ FormatTokenCount(s.contextTokens) }, 10, false, 0.45);
            const auto tip = std::wstring{ L"Context: " } + GroupDigits(s.contextTokens) +
                             L" tokens in the session (newest turn: input + cache + output).";
            AgentSetTip(ctxText, winrt::hstring{ tip }, kCardTipDelay);
            stack.Children().Append(ctxText);
        }

        // autopilot badge ⚙ sent/total + the "still server-cached" ⚡ indicator, on ONE row (⚡ to the
        // right of ⚙ N/M). The ⚙ badge shows only when there's a queue; the ⚡ shows whenever the
        // session is still inside Claude's server-side prompt-cache window (serverCacheMinutes).
        {
            auto metaRow = StackPanel{};
            metaRow.Orientation(Orientation::Horizontal);
            metaRow.Spacing(8);

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
                AgentSetTip(bt, L"Flight Plan queue \x2014 prompts sent / total queued (\x2699). Shown in blue while Autopilot is on for this session.", kCardTipDelay);
                metaRow.Children().Append(bt);
            }

            // Agentmaster (Waiting-for-you "unread" model): a ⚡ "still server-cached" hint. Claude's
            // server-side prompt cache stays warm for ~serverCacheMinutes after the last turn, so a
            // follow-up within the window reuses the cached prefix (cheaper & faster). Purely cosmetic;
            // shown only while the card is inside that window. The board's periodic refresh (a 30s
            // timer + every registry event) clears it once the window lapses.
            {
                const uint32_t cacheMin = _appSettings.serverCacheMinutes ? _appSettings.serverCacheMinutes : 5;
                const int64_t effLast = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
                if (s.live && effLast > 0 && (NowMs() - effLast) < static_cast<int64_t>(cacheMin) * 60000)
                {
                    auto cacheGlyph = Text(L"\x26A1", 11, false, 0.95); // ⚡ warm cache
                    cacheGlyph.Foreground(Fill(0xFF, 0xFF, 0xC1, 0x07)); // amber
                    AgentSetTip(cacheGlyph, winrt::hstring{ L"Still server-cached \x2014 Claude's prompt cache stays warm for ~" } + winrt::to_hstring(static_cast<int>(cacheMin)) + L" min after the last turn, so a follow-up now reuses the cached context (cheaper & faster).", kCardTipDelay);
                    metaRow.Children().Append(cacheGlyph);
                }
            }

            if (metaRow.Children().Size() > 0)
            {
                stack.Children().Append(metaRow);
            }
        }

        // Agentmaster: a hover-revealed "\x22EF" more-button in the card's top-right corner — a
        // discoverable twin of the right-click menu (some users never right-click). Wrap the content
        // in a Grid so the dots float over the top-right; they carry the SAME session menu as a
        // Button.Flyout (a click opens it). Hidden at rest, faded in on the card's hover (wired on
        // PointerEntered/Exited below; OpacityTransition animates the Opacity change — no Storyboard
        // to manage). IsHitTestVisible(false) at rest keeps the transparent corner from eating a card
        // click. Built per-card; cheap.
        auto dotsBtn = Button{};
        {
            FontIcon moreGlyph;
            moreGlyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            moreGlyph.Glyph(L"\xE712"); // "More" — three dots
            moreGlyph.FontSize(14);
            dotsBtn.Content(moreGlyph);
        }
        dotsBtn.Padding(Thickness{ 4, 0, 4, 0 });
        dotsBtn.MinWidth(26);
        dotsBtn.Height(20);
        dotsBtn.HorizontalAlignment(HorizontalAlignment::Right);
        dotsBtn.VerticalAlignment(VerticalAlignment::Top);
        dotsBtn.Margin(Thickness{ 0, 4, 6, 0 }); // inset from the top-right corner (the card no longer pads its content); floats over the title band, clear of the rounded corner
        dotsBtn.Background(Fill(0x66, 0x30, 0x30, 0x30)); // faint chip so the glyph reads over the title behind it
        dotsBtn.Foreground(Fill(0xF0, 0xFF, 0xFF, 0xFF));
        dotsBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        dotsBtn.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
        dotsBtn.Opacity(0.0); // hidden at rest; the card's hover fades it in
        dotsBtn.IsHitTestVisible(false); // an invisible corner must not swallow a card click
        dotsBtn.IsTabStop(false); // a hover affordance — keep the invisible button out of the keyboard tab order (the menu is reachable via right-click / the context-menu key)
        {
            ScalarTransition st;
            st.Duration(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 140 } });
            dotsBtn.OpacityTransition(st); // genuine fade on any Opacity change
        }
        AgentSetTip(dotsBtn, L"More \x2014 session actions (same as right-click)", kCardTipDelay);
        dotsBtn.Flyout(_MakeSessionMenu(s.id, s.workingDir)); // a click opens the session menu
        const auto dotsWeak = winrt::make_weak(dotsBtn);

        // Agentmaster: the body carries the inset the card used to own (card Padding is now 0 so
        // the colored band can bleed to the card's rounded top corners + side edges); the outer
        // StackPanel pins the band over the body with spacing 0 so the band's straight bottom sits
        // flush against the body.
        auto bodyBorder = Border{};
        bodyBorder.Padding(Thickness{ 8, 6, 8, 6 });
        bodyBorder.Child(stack);

        auto outer = StackPanel{};
        outer.Spacing(0);
        outer.Children().Append(band);
        outer.Children().Append(bodyBorder);

        // Agentmaster: the hover/selection outline is drawn as an OVERLAY ring, NOT on the card
        // Button's own border. A Button's BorderThickness is part of its layout box, so toggling it
        // on hover grows the card (every card below shifts) AND insets the title band off its rounded
        // corners (the content shifts in) — the visible "card jumps when I mouse over it" bug. This
        // ring is a transparent Border layered in the same Grid cell (drawn ON TOP, IsHitTestVisible
        // false), so changing its thickness redraws the outline inward over the card edges WITHOUT
        // resizing the card or moving anything. 0 at rest, 1 on hover, 2 when selected. Its corners
        // match the card so the outline rounds with the edge. (As a bonus this also stops a SELECTED
        // card from being 2px larger than its unselected siblings — both are now the same size.)
        auto ring = Border{};
        ring.CornerRadius(CornerRadius{ 4, 4, 4, 4 }); // matches the card rounding
        ring.BorderBrush(SolidColorBrush{ accent });
        ring.BorderThickness(selected ? Thickness{ 2, 2, 2, 2 } : Thickness{ 0, 0, 0, 0 });
        ring.IsHitTestVisible(false); // a decorative overlay must not eat card clicks/hover
        ring.HorizontalAlignment(HorizontalAlignment::Stretch);
        ring.VerticalAlignment(VerticalAlignment::Stretch);
        const auto ringWeak = winrt::make_weak(ring);

        auto grid = Grid{};
        grid.Children().Append(outer);
        grid.Children().Append(ring); // over the content, under the dots
        grid.Children().Append(dotsBtn);

        // Agentmaster (Waiting-for-you countdown bar): a 1px goldenrod bar pinned INSIDE the card's
        // bottom edge that drains from full width (100% of the waiting window) to 0 as the
        // Waiting-for-you timeout approaches. At empty the wait has expired — a READ card then decays to
        // Idle / Done (an unread one keeps waiting until read). Shown only for a WaitingForInput card with
        // a finite timeout that isn't manually held unread (a Mark-Unread / "Never" card never time-decays,
        // so it has no countdown). Overlaid in the grid OVER the hover/selected ring so it stays visible,
        // and IsHitTestVisible(false) so the 1px strip never eats a card click. ScaleX (origin LEFT) =
        // fraction remaining; _progressTimer drains it live in place, and each _RebuildBoard re-seeds it.
        if (s.state == SessionState::WaitingForInput && !s.manualUnread && _appSettings.waitingForYouTimeoutMinutes > 0 && s.lastActivityUnixMs > 0)
        {
            const int64_t timeoutMs = static_cast<int64_t>(_appSettings.waitingForYouTimeoutMinutes) * 60000;
            double frac = 1.0 - static_cast<double>(NowMs() - s.lastActivityUnixMs) / static_cast<double>(timeoutMs);
            frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);

            auto barScale = ScaleTransform{};
            barScale.ScaleX(frac);

            auto bar = Border{};
            bar.Height(1);
            bar.VerticalAlignment(VerticalAlignment::Bottom);
            bar.HorizontalAlignment(HorizontalAlignment::Stretch);
            bar.Background(SolidColorBrush{ StateColor(SessionState::WaitingForInput) }); // goldenrod, matching the state
            bar.RenderTransformOrigin(Point{ 0.0f, 0.0f }); // drain from the RIGHT (the left edge stays pinned)
            bar.RenderTransform(barScale);
            bar.IsHitTestVisible(false); // a decorative 1px overlay must never swallow a card click
            grid.Children().Append(bar);

            _cardProgress.push_back(CardProgress{ bar, s.lastActivityUnixMs, timeoutMs });
        }

        auto card = Button{};
        card.Content(grid);
        card.HorizontalAlignment(HorizontalAlignment::Stretch);
        card.HorizontalContentAlignment(HorizontalAlignment::Stretch); // let the Grid fill so the band reaches the card edges + the dots reach the true top-right corner
        card.Padding(Thickness{ 0, 0, 0, 0 }); // band + body own their insets now, so the title band can reach the rounded top corners
        card.Margin(Thickness{ 0, 0, 0, 6 });
        card.CornerRadius(CornerRadius{ 4, 4, 4, 4 }); // explicit, so the title band's top corners (4,4,0,0) line up with the card rounding
        card.Background(Fill(selected ? 0x40 : 0x20, 0x80, 0x80, 0x80));
        // Agentmaster: the state-colored outline lives on the `ring` OVERLAY above, NOT on this
        // Button's own border — toggling a Button BorderThickness grows the card and nudges the
        // title band, which is the shift we're avoiding. The card's own border stays a constant 0;
        // only the overlay ring's thickness toggles (0 rest / 1 hover / 2 selected).
        card.BorderThickness(Thickness{ 0, 0, 0, 0 });
        if (!selected)
        {
            // Grow the overlay ring on hover (a selected card keeps its fixed 2). ringWeak is a
            // weak_ref so the card's handler never strong-captures a child that chains back to the
            // card (the no-self-capture rule — a strong ring ref would cycle
            // card -> handler -> ring -> grid -> card and leak the whole tree).
            card.PointerEntered([ringWeak](const IInspectable&, const PointerRoutedEventArgs&) {
                if (const auto r = ringWeak.get())
                {
                    r.BorderThickness(Thickness{ 1, 1, 1, 1 });
                }
            });
            card.PointerExited([ringWeak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                if (PointerStillWithin(sender, e))
                {
                    return; // a child label's exit bubbled up — the pointer never left the card; don't flicker the ring
                }
                if (const auto r = ringWeak.get())
                {
                    r.BorderThickness(Thickness{ 0, 0, 0, 0 });
                }
            });
        }
        const auto id = s.id;
        // Agentmaster (Linked Lenses): report hover so the page pills THIS session's terminal tab
        // while the Manager tab is active (a live preview that follows the mouse). Capture id by
        // value + `this` (never the Button into its own handler — a self-capture leaks the element);
        // fires for selected cards too, so hovering the selected card keeps its tab pilled.
        // Also fade the "\x22EF" more-button in (and arm its hit-testing) while the card is hovered;
        // fade it out on exit. dotsWeak is a weak_ref so the handler never strong-captures the button
        // it lives under (the codebase's no-self-capture rule). Hovering the dots (a child) keeps the
        // card "entered" — a child within the card's bounds doesn't raise the card's PointerExited —
        // so the dots stay up while you aim for them.
        card.PointerEntered([this, id, dotsWeak](const IInspectable&, const PointerRoutedEventArgs&) {
            _ReportHover(id, true);
            if (const auto d = dotsWeak.get())
            {
                d.IsHitTestVisible(true);
                d.Opacity(0.85);
            }
        });
        card.PointerExited([this, id, dotsWeak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            if (PointerStillWithin(sender, e))
            {
                return; // a child label's exit bubbled up — keep the tab pill + dots up (Linked-Lenses anti-flicker)
            }
            _ReportHover(id, false);
            if (const auto d = dotsWeak.get())
            {
                d.Opacity(0.0);
                d.IsHitTestVisible(false);
            }
        });
        // Single click = select; double click (within the OS threshold) = Activate (jump to
        // the session's live terminal tab — the page fans out to the hosting WINDOW when the
        // tab lives in another one), mirroring the Explorer Tree rows. A Button swallows
        // DoubleTapped, so we time the successive clicks ourselves.
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
                // Agentmaster (Linked Lenses): a managed board-card single-click syncs the Explorer
                // Tree scope to where THIS session lives — LOCAL when this window hosts it, else
                // GLOBAL (hosted by another window). The managed twin of an External card click
                // switching the tree to EXTERNAL (_SelectExternal), so all three regions agree on the
                // clicked card's lens. Select FIRST (aims the Launch box, sets the selection), THEN
                // sync the scope LAST with its own refresh: _SetTreeScope no-ops (no refresh) when the
                // scope is already correct, and refreshes when it changes — so a re-click of the
                // already-selected card (where _SelectSession early-outs without refreshing) still
                // repaints if the user toggled the scope away in between. Only when the locality is
                // knowable: with no provider (mid-init / standalone tests) leave the scope as-is,
                // exactly like the board's own LOCAL filter (see _RebuildBoard).
                _SelectSession(id);
                if (_localScopeProvider)
                {
                    const auto localIds = _localScopeProvider();
                    const bool isLocal = localIds.find(id) != localIds.end();
                    _SetTreeScope(isLocal ? TreeScope::Local : TreeScope::Global, /*refresh*/ true);
                }
                // Agentmaster (double-click fix): _SelectSession (+ the scope sync above) just TORE
                // DOWN and rebuilt this board — every card Button is cleared + recreated from scratch
                // (_RebuildBoard: _boardHost.Children().Clear() then _MakeCard per session). The
                // replacement card for THIS id is in the tree but NOT yet arranged (a fresh element
                // has 0x0 bounds until the next async layout pass), so a second mouse-down arriving
                // microseconds later (the user double-clicking to Activate) hit-tests to nothing and
                // its Click never fires — a double-click on an UNSELECTED card silently did nothing
                // ("if it's not selected it doesn't work"), while an already-selected card worked (its
                // _SelectSession early-outs, no rebuild). Force a synchronous layout so the replacement
                // card has real bounds NOW and the follow-up click lands on it. Only this not-selected
                // path rebuilds, so it is the only one that needs it; cheap (a card click is rare).
                if (_boardHost)
                {
                    _boardHost.UpdateLayout();
                }
            }
        });
        // Right-click (or context key / long-press): the SAME menu as the Explorer-Tree session
        // row — Rename… / Archive… / Open New Session Here — one card/row, one action set
        // (Linked Lenses). The menu acts on the captured id/cwd, never "the selected session".
        card.ContextFlyout(_MakeSessionMenu(id, s.workingDir));
        // Agentmaster: tag + register the card so _Refresh can RESTORE keyboard focus onto it after
        // a rebuild (a title/state change recreates every card). "b:" marks the board lens, so the
        // focused element's id + lens are read off its Tag alone — no visual-tree ancestry walk.
        card.Tag(winrt::box_value(winrt::hstring{ L"b:" + s.id }));
        _boardCardsById[s.id] = card;
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
        // Agentmaster: the grab bar is draggable but easy to miss (it is near-invisible at rest);
        // name what it resizes so the affordance is discoverable beyond the hover cursor change.
        AgentSetTip(bar, vertical ?
                             winrt::hstring{ L"Drag to resize \x2014 the Explorer Tree and the Flight Plan share this divider." } :
                             winrt::hstring{ L"Drag to resize \x2014 the Triage Board and the panels below it share this divider." });

        const auto cursorType = vertical ? CoreCursorType::SizeWestEast : CoreCursorType::SizeNorthSouth;

        bar.PointerEntered([this, grip, cursorType, hotGrip](const IInspectable&, const PointerRoutedEventArgs&) {
            ApplyCursor(cursorType);
            grip.Background(hotGrip);
        });
        bar.PointerExited([this, grip, idleGrip](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            // The grip is a hit-testable child filling the bar's center band, so moving off it onto the
            // bar's own margin area bubbles the grip's PointerExited here — a false leave that would dim
            // the grip (drop the "draggable" highlight) while the pointer is still on the divider. Swallow
            // those; only a real leave (pointer outside the bar) resets. (PointerStillWithin, like the cards.)
            if (PointerStillWithin(sender, e))
            {
                return;
            }
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
        _NotifyLensChanged(); // M10: splitter sizes ride in the per-window record too

        ApplyCursor(CoreCursorType::Arrow);
        e.Handled(true);
    }

    void AgentManagerContent::_RebuildBoard(const std::vector<SessionInfo>& sessions)
    {
        // Agentmaster: preserve each column's vertical scroll offset across this rebuild. _RebuildBoard
        // recreates the per-column ScrollViewers from scratch (a fresh ScrollViewer sits at offset 0),
        // so without this a mere _Refresh — a select, a state/title change, an observer enrichment —
        // would snap the board to the TOP, losing the card the user just clicked near the bottom of a
        // tall column. The remembered offsets live in the DURABLE member _boardColumnOffsets (keyed by
        // column title), not a per-rebuild local: a refresh that lands before a PRIOR rebuild's
        // restore-on-Loaded has fired would otherwise read that rebuild's fresh, not-yet-restored
        // ScrollViewer sitting at 0 and PERMANENTLY lose the saved scroll — the "a click jumps the scroll
        // to top" race when a background scanner/observer refresh coincides with the user's click. Capture
        // the live offsets now (the old scrollers are still valid), but ONLY trust a LOADED ScrollViewer:
        // a not-yet-loaded SV (IsLoaded == false) reports a meaningless 0 — keep the remembered offset
        // rather than clobber it; a genuinely-scrolled-to-top LOADED column records its real 0. Each new
        // column re-applies its remembered offset on Loaded (see _MakeBoardColumn).
        for (const auto& [key, sv] : _boardColumnScrollers)
        {
            if (!sv)
            {
                continue;
            }
            const auto off = sv.VerticalOffset();
            if (off > 0.0)
            {
                _boardColumnOffsets[key] = off; // a real, restored scroll position
            }
            else if (sv.IsLoaded())
            {
                _boardColumnOffsets[key] = 0.0; // genuinely at the top (the SV has loaded, so 0 is real)
            }
            // else: a fresh / not-yet-restored SV reading 0 — preserve the remembered offset (its
            // restore-on-Loaded is still pending), so a coincident refresh can't lose the user's scroll.
        }
        _boardColumnScrollers.clear(); // refilled by _MakeBoardColumn below

        _boardHost.Children().Clear();
        _boardCardsById.clear(); // refilled by _MakeCard below (focus-restore map; see _Refresh)
        _cardProgress.clear(); // Agentmaster: refilled by _MakeCard for each Waiting-for-you countdown bar (drained by _progressTimer)
        if (_boardScope)
        {
            // The label exists only WHILE a directory is scoped (paired with "Show all"); unscoped
            // it collapses — the old "[all directories]" placeholder was display-only noise.
            _boardScope.Text(_scopeDir.empty() ? winrt::hstring{} : (winrt::hstring{ L"[scope: " } + winrt::hstring{ _scopeDir } + L"]"));
            _boardScope.Visibility(_scopeDir.empty() ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_showAllBtn)
        {
            // "Show all" disappears when we ARE showing all (no scope) and reappears once a dir is scoped.
            _showAllBtn.Visibility(_scopeDir.empty() ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_clearSelBtn)
        {
            // "Clear" is shown only while something is selected (managed OR external), like "Show all".
            const bool hasSel = !_selectedId.empty() || !_selectedExternalSessionId.empty();
            _clearSelBtn.Visibility(hasSel ? Visibility::Visible : Visibility::Collapsed);
        }

        // Agentmaster: the board's LOCAL/GLOBAL scope (the toggle next to the title — ONE state
        // with the Explorer Tree's; External there reads GLOBAL here). LOCAL keeps only THIS
        // window's sessions (the page's _claudeTabs via _localScopeProvider), computed once for
        // all five columns; with no provider (mid-init / standalone tests) everything counts as
        // local — no filter — exactly like the tree. The External (N) census column below is NOT
        // scoped by this: externals are not managed sessions of any window.
        const bool boardLocal = (_treeScope == TreeScope::Local) && static_cast<bool>(_localScopeProvider);
        std::unordered_set<std::wstring> boardLocalIds;
        if (boardLocal)
        {
            boardLocalIds = _localScopeProvider();
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

            // collect matching sessions (respecting the LOCAL/GLOBAL scope + the directory scope)
            std::vector<const SessionInfo*> matches;
            for (const auto& s : sessions)
            {
                if (!s.live)
                {
                    continue; // closed sessions live in the Sessions browser, not the board (FAVORITES.md)
                }
                if (boardLocal && boardLocalIds.find(s.id) == boardLocalIds.end())
                {
                    continue; // LOCAL scope: hosted by another window
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

            // Agentmaster: the Error column is SPECIAL — it collapses out of the board entirely when
            // it holds no cards and reappears in place (between Needs-approval and Idle / Done) the
            // moment a session errors. Skipping the Append below means the horizontal StackPanel
            // (_boardHost) reserves NO width — nor its 8px inter-column spacing — for it, so an empty
            // Error column costs zero board space; and because the columns are appended in fixed order
            // on every rebuild, it always returns to the SAME slot when it reappears. The other four
            // states are always shown — they are the steady-state columns of the triage model.
            if (col.state == SessionState::Error && matches.empty())
            {
                continue;
            }

            // Agentmaster: order the cards WITHIN this state column by the (global, persisted) board
            // sort — default MostActive (most-recently-active first). stable_sort so equal keys keep
            // their prior on-screen order across refreshes. The board reuses the SAME SortKey/SortKeyLess
            // comparator as the Explorer Tree, just driven by _appSettings.boardSort (its own setting) and
            // applied to the flat per-column list (no directory grouping — cards are grouped by STATE here).
            std::stable_sort(matches.begin(), matches.end(), [&](const SessionInfo* a, const SessionInfo* b) {
                return SortKeyLess(_appSettings.boardSort, MakeSortKey(*a), MakeSortKey(*b));
            });

            auto hdr = StackPanel{};
            hdr.Orientation(Orientation::Horizontal);
            hdr.Spacing(6);
            hdr.Margin(Thickness{ 0, 0, 0, 6 });
            auto dot = Text(L"\x25CF", 12, false, 1.0);
            dot.Foreground(SolidColorBrush{ StateColor(col.state) });
            hdr.Children().Append(dot);
            hdr.Children().Append(Text(col.title, 12, true, 0.9));
            hdr.Children().Append(Text(winrt::to_hstring(static_cast<int>(matches.size())), 12, false, 0.6));
            // Agentmaster: explain what each Triage state means — the board's five columns ARE the
            // state model, so naming them on hover is the core learning-curve aid.
            const wchar_t* colTip =
                col.state == SessionState::Running        ? L"Running \x2014 the agent is actively working on a turn." :
                col.state == SessionState::WaitingForInput ? L"Waiting-for-you \x2014 the turn is complete; the agent is waiting for your next prompt. With Autopilot on, the next queued prompt sends automatically." :
                col.state == SessionState::NeedsApproval  ? L"Needs-approval \x2014 the agent is paused on a tool-permission prompt or a question and needs your response to continue." :
                col.state == SessionState::Error          ? L"Error \x2014 the agent's last turn ended in an error." :
                                                            L"Idle / Done \x2014 no turn in progress: freshly launched, just resumed, or finished.";
            AgentSetTip(hdr, colTip);
            // colStack holds the cards only; _MakeBoardColumn pins the header above a vertically
            // scrolling card list so a tall column scrolls within the board height instead of
            // clipping past the bottom edge (the board ScrollViewer's vertical scroll is disabled).
            for (const auto* s : matches)
            {
                colStack.Children().Append(_MakeCard(*s));
            }

            // Preserve this column's remembered scroll offset (keyed by its title; durable across rebuilds).
            const std::wstring colKey{ col.title };
            const auto savedIt = _boardColumnOffsets.find(colKey);
            const double restore = (savedIt != _boardColumnOffsets.end()) ? savedIt->second : 0.0;
            _boardHost.Children().Append(_MakeBoardColumn(hdr, colStack, true, colKey, restore));
        }

        // Agentmaster (O6): a trailing observe-only "External (N)" group for real-WindowsTerminal
        // claudes the observer detected (NOT our tabs — no registry session, no Flight Plan). Shown
        // unscoped (it is a global census, not part of the managed directory tree).
        if (!_externalClaudes.empty())
        {
            const auto extIt = _boardColumnOffsets.find(L"External");
            _boardHost.Children().Append(_MakeExternalColumn(extIt != _boardColumnOffsets.end() ? extIt->second : 0.0));
        }
    }

    // Agentmaster: assemble one Triage Board column. When `fill` is true the column fills the board
    // height with a pinned `header` (Grid row 0) over a vertically-scrolling `cards` list (row 1),
    // so a tall column (e.g. a large External census) scrolls within the board instead of clipping
    // past the bottom edge — the board's own ScrollViewer (BuildUI) has vertical scroll disabled.
    // When `fill` is false the box hugs its content (a collapsed column: header only, no scroll).
    // Shared by the per-state columns and the External group so they stay visually in lockstep.
    Border AgentManagerContent::_MakeBoardColumn(const UIElement& header, const UIElement& cards, bool fill, const std::wstring& columnKey, double restoreOffset)
    {
        auto col_border = Border{};
        col_border.Width(220);
        col_border.Padding(Thickness{ 8, 8, 8, 8 });
        col_border.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
        col_border.Background(Fill(0x14, 0x80, 0x80, 0x80));

        if (fill)
        {
            auto grid = Grid{};
            auto rdHeader = RowDefinition{};
            rdHeader.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            grid.RowDefinitions().Append(rdHeader);
            auto rdCards = RowDefinition{};
            rdCards.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            grid.RowDefinitions().Append(rdCards);

            Grid::SetRow(header.as<FrameworkElement>(), 0); // Grid::SetRow takes a FrameworkElement; header is typed UIElement
            grid.Children().Append(header);

            auto cardsSv = ScrollViewer{};
            cardsSv.VerticalScrollMode(ScrollMode::Enabled);
            cardsSv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            cardsSv.HorizontalScrollMode(ScrollMode::Disabled);
            cardsSv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            cardsSv.Content(cards);
            Grid::SetRow(cardsSv, 1);
            grid.Children().Append(cardsSv);

            // Agentmaster: track this column's ScrollViewer so the NEXT _RebuildBoard can capture its
            // offset, and re-apply the offset this rebuild inherited. A fresh ScrollViewer sits at 0
            // until restored; do it on Loaded with animation disabled (an instant restore, no visible
            // jump). Capture only the offset — the sender IS the ScrollViewer, never self-capture the
            // element (that leaks it via the delegate).
            if (!columnKey.empty())
            {
                _boardColumnScrollers[columnKey] = cardsSv;
                if (restoreOffset > 0.0)
                {
                    cardsSv.Loaded([restoreOffset](const IInspectable& sender, const RoutedEventArgs&) {
                        if (const auto sv = sender.try_as<ScrollViewer>())
                        {
                            // Agentmaster (scroll-jump fix): when Loaded fires, a fresh ScrollViewer's
                            // CONTENT extent often isn't realized yet — ScrollableHeight still reads 0 —
                            // so a bare ChangeView(restoreOffset) CLAMPS to 0 and the column snaps to the
                            // TOP. That is the "sometimes a click jumps the scroll to top" report:
                            // _RebuildBoard recreates these ScrollViewers on EVERY refresh (a select, a
                            // state/title change, an observer enrichment), and the restore raced the
                            // content's first measure. Force the content to measure+arrange first so
                            // ScrollableHeight reflects the real height, THEN restore — the same realize-
                            // then-scroll recipe BringSelectedIntoView uses (UpdateLayout before the
                            // scroll). UpdateLayout is synchronous + idempotent, so it is a no-op when the
                            // extent is already valid.
                            sv.UpdateLayout();
                            sv.ChangeView(nullptr, restoreOffset, nullptr, true);
                        }
                    });
                }
            }

            // Stretch so the board's horizontal StackPanel gives the column the full viewport height
            // (the board SV's vertical scroll is off, so the cross-axis is bounded) -> the inner
            // ScrollViewer has a real height to scroll within.
            col_border.VerticalAlignment(VerticalAlignment::Stretch);
            col_border.Child(grid);
        }
        else
        {
            auto stack = StackPanel{};
            stack.Spacing(0);
            stack.Children().Append(header);
            stack.Children().Append(cards);
            col_border.VerticalAlignment(VerticalAlignment::Top);
            col_border.Child(stack);
        }
        return col_border;
    }

    // Agentmaster (O6): the "External (N)" board column. Observe-only — each card is a real
    // Windows Terminal claude the observer correlated out-of-band but will never bind (Rule #9/#13).
    Border AgentManagerContent::_MakeExternalColumn(double restoreOffset)
    {
        auto colStack = StackPanel{};
        colStack.Spacing(0);

        auto hdr = StackPanel{};
        hdr.Orientation(Orientation::Horizontal);
        hdr.Spacing(6);
        auto dot = Text(L"\x25CF", 12, false, 1.0);
        dot.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // gray — external / observe-only
        hdr.Children().Append(dot);
        hdr.Children().Append(Text(L"External", 12, true, 0.9));
        hdr.Children().Append(Text(winrt::to_hstring(static_cast<int>(_externalClaudes.size())), 12, false, 0.6));
        hdr.Children().Append(Text(_externalCollapsed ? winrt::hstring{ L"\x25B8" } : winrt::hstring{ L"\x25BE" }, 11, false, 0.6)); // ▸ / ▾

        // The header doubles as the collapse toggle (a Button styled to read like the other column
        // headers — transparent, borderless, left-aligned).
        auto hdrBtn = Button{};
        hdrBtn.Content(hdr);
        hdrBtn.Background(Fill(0x00, 0, 0, 0));
        hdrBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        hdrBtn.Padding(Thickness{ 0, 0, 0, 0 });
        hdrBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
        hdrBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
        hdrBtn.Margin(Thickness{ 0, 0, 0, 6 });
        AgentSetTip(hdrBtn, L"Agents running outside Agentmaster (observe-only census) \x2014 click to collapse or expand this column.");
        hdrBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            _externalCollapsed = !_externalCollapsed;
            _Refresh();
        });
        // Collapsed: just the header in a hugging box (no card list to scroll). Expanded: the header
        // pinned above a vertically-scrolling card list (fills the board height) so a large External
        // census scrolls within the board instead of clipping past the bottom edge. colStack holds
        // the cards only (the header is pinned by _MakeBoardColumn, not stacked above them).
        if (_externalCollapsed)
        {
            return _MakeBoardColumn(hdrBtn, colStack, false);
        }

        // Agentmaster: order the External census cards by the same (global) board sort as the managed
        // columns, so the whole board reads in one consistent order (the rows arrive pid-sorted from the
        // observer; re-sort a pointer copy here, leaving _externalClaudes untouched). Externals carry no
        // run-state (MakeSortKey sets active=false), so MostActive ranks them by recency only.
        std::vector<const ::Agentmaster::ExternalClaudeRow*> exts;
        exts.reserve(_externalClaudes.size());
        for (const auto& ex : _externalClaudes)
        {
            exts.push_back(&ex);
        }
        std::stable_sort(exts.begin(), exts.end(), [&](const ::Agentmaster::ExternalClaudeRow* a, const ::Agentmaster::ExternalClaudeRow* b) {
            return SortKeyLess(_appSettings.boardSort, MakeSortKey(*a), MakeSortKey(*b));
        });
        for (const auto* ex : exts)
        {
            colStack.Children().Append(_MakeExternalCard(*ex));
        }
        return _MakeBoardColumn(hdrBtn, colStack, true, L"External", restoreOffset);
    }

    winrt::Windows::UI::Xaml::Controls::Button AgentManagerContent::_MakeExternalCard(const ::Agentmaster::ExternalClaudeRow& ex)
    {
        auto stack = StackPanel{};
        stack.Spacing(2);

        // Title: the conversation's first prompt (from the transcript), else the cwd leaf, else
        // "claude" (recent transcripts carry no summary — verified — so the first prompt is the title).
        std::wstring title = ex.title;
        if (title.empty())
        {
            std::wstring leaf = ex.cwd;
            const auto slash = leaf.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash + 1 < leaf.size())
            {
                leaf = leaf.substr(slash + 1);
            }
            title = leaf.empty() ? std::wstring{ L"claude" } : leaf;
        }
        if (title.size() > 64)
        {
            title = title.substr(0, 61) + L"\x2026";
        }
        // Phase C2: a Codex row leads its title with a state dot (rollout-derived turn state — blue
        // running / gold waiting / gray idle). A Claude external carries no PULL state -> plain title.
        if (ex.kind == AgentKind::Codex)
        {
            auto titleRow = StackPanel{};
            titleRow.Orientation(Orientation::Horizontal);
            titleRow.Spacing(6);
            titleRow.VerticalAlignment(VerticalAlignment::Center);
            auto sd = Text(L"\x25CF", 11, false, 1.0);
            sd.Foreground(SolidColorBrush{ CodexStateColor(ex.codexState) });
            AgentSetTip(sd, winrt::hstring{ L"Codex turn state \x2014 " } + CodexStateLabel(ex.codexState) + winrt::hstring{ L", derived from its rollout transcript" }, kCardTipDelay);
            titleRow.Children().Append(sd);
            titleRow.Children().Append(Text(winrt::hstring{ title }, 13, true, 0.9));
            stack.Children().Append(titleRow);
        }
        else
        {
            stack.Children().Append(Text(winrt::hstring{ title }, 13, true, 0.9));
        }
        // Agentmaster (Phase C1): a Codex row carries a teal "codex" agent pill so a mixed External
        // group reads at a glance (Claude is the implicit default — no pill, visuals unchanged).
        if (ex.kind == AgentKind::Codex)
        {
            auto p = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
            p.Opacity(0.9);
            p.HorizontalAlignment(HorizontalAlignment::Left);
            AgentSetTip(p, L"Codex agent \x2014 this external session runs the OpenAI Codex CLI (observed, not managed by Agentmaster).", kCardTipDelay);
            stack.Children().Append(p);
        }
        if (!ex.cwd.empty())
        {
            auto cwdText = Text(winrt::hstring{ ex.cwd }, 11, false, 0.55);
            AgentSetTip(cwdText, L"Working directory of this external session.", kCardTipDelay);
            stack.Children().Append(cwdText);
        }

        // host (the foreign terminal) · git branch
        {
            // The Fleet Observer resolves a clear host label by the hosting terminal's identity
            // (package family / image path): "Windows Terminal" (real WT) vs "Agentmaster" /
            // "Agentmaster Dev" (another of our instances) vs a shell leaf — see ResolveExternalHostLabel.
            std::wstring hostLabel = ex.hostLabel;
            if (hostLabel.empty())
            {
                // Fallback for an older/missing reading: the parent shell leaf, else generic.
                if (!ex.hostImage.empty())
                {
                    hostLabel = ex.hostImage;
                    const auto dot = hostLabel.rfind(L".exe");
                    if (dot != std::wstring::npos)
                    {
                        hostLabel = hostLabel.substr(0, dot);
                    }
                }
                else
                {
                    hostLabel = (ex.host == RunningApp::WindowsTerminal) ? L"Windows Terminal" : L"external";
                }
            }
            std::wstring hb = L"via " + hostLabel;
            if (!ex.gitBranch.empty())
            {
                hb += L"  \x00B7  [" + ex.gitBranch + L"]";
            }
            auto hbText = Text(winrt::hstring{ hb }, 10, false, 0.5);
            AgentSetTip(hbText, L"The terminal application hosting this external session, and \x2014 in [brackets] \x2014 its current git branch.", kCardTipDelay);
            stack.Children().Append(hbText);
        }

        // model · effort · bg · pid
        {
            std::wstring me;
            const auto addPart = [&](const std::wstring& part) {
                if (part.empty())
                {
                    return;
                }
                if (!me.empty())
                {
                    me += L"  \x00B7  ";
                }
                me += part;
            };
            addPart(ex.model);
            addPart(ex.effort);
            addPart(ex.sandbox); // Codex only (empty for Claude) — model · effort · sandbox · approval
            addPart(ex.approvalMode); // Codex only
            if (ex.background)
            {
                addPart(L"bg");
            }
            me += (me.empty() ? L"pid " : L"  \x00B7  pid ") + std::to_wstring(ex.pid);
            auto meText = Text(winrt::hstring{ me }, 10, false, 0.5);
            AgentSetTip(meText, L"Model \xB7 reasoning effort \xB7 (Codex: sandbox \xB7 approval) \xB7 bg = running in the background \xB7 pid = OS process id.", kCardTipDelay);
            stack.Children().Append(meText);
        }

        // timing (created-ago / active-for / last-activity-ago)
        {
            const int64_t created = ex.createdUnixMs ? ex.createdUnixMs : ex.startUnixMs;
            if (auto t = TimingText(created, ex.lastActivityUnixMs))
            {
                stack.Children().Append(t);
            }
        }

        // The whole card is clickable — left-click SELECTS this external, EXACTLY like clicking its
        // row in the Explorer Tree (_SelectExternal): the Flight Plan shows its conversation read-only
        // and the tree syncs to EXTERNAL with this one highlighted (Linked Lenses). Right-click opens
        // the SAME menu the tree row uses — Adopt / Open New Session Here / Bring Window To Front. (No
        // inline "observe"/"Adopt" affordance: the card itself is the observe action; the rest lives
        // on the right-click menu.)
        const bool selected = !ex.sessionId.empty() && ex.sessionId == _selectedExternalSessionId;

        // Agentmaster: the same hover-revealed "\x22EF" more-button as the managed cards (_MakeCard) —
        // here it opens the EXTERNAL menu (Adopt / Open New Session Here / Bring Window To Front). Wrap
        // the content in a Grid so the dots float top-right; faded in on the card's hover (wired below).
        auto dotsBtn = Button{};
        {
            FontIcon moreGlyph;
            moreGlyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            moreGlyph.Glyph(L"\xE712"); // "More" — three dots
            moreGlyph.FontSize(14);
            dotsBtn.Content(moreGlyph);
        }
        dotsBtn.Padding(Thickness{ 4, 0, 4, 0 });
        dotsBtn.MinWidth(26);
        dotsBtn.Height(20);
        dotsBtn.HorizontalAlignment(HorizontalAlignment::Right);
        dotsBtn.VerticalAlignment(VerticalAlignment::Top);
        dotsBtn.Background(Fill(0x66, 0x30, 0x30, 0x30));
        dotsBtn.Foreground(Fill(0xF0, 0xFF, 0xFF, 0xFF));
        dotsBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        dotsBtn.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
        dotsBtn.Opacity(0.0); // hidden at rest; the card's hover fades it in
        dotsBtn.IsHitTestVisible(false); // an invisible corner must not swallow a card click
        dotsBtn.IsTabStop(false); // a hover affordance — keep the invisible button out of the keyboard tab order (the menu is reachable via right-click / the context-menu key)
        {
            ScalarTransition st;
            st.Duration(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 140 } });
            dotsBtn.OpacityTransition(st);
        }
        AgentSetTip(dotsBtn, L"More \x2014 actions (same as right-click)", kCardTipDelay);
        dotsBtn.Flyout(_MakeExternalTreeMenu(ex)); // a click opens the external menu
        const auto dotsWeak = winrt::make_weak(dotsBtn);

        auto grid = Grid{};
        grid.Children().Append(stack);
        grid.Children().Append(dotsBtn);

        auto card = Button{};
        card.Content(grid);
        card.HorizontalAlignment(HorizontalAlignment::Stretch);
        card.HorizontalContentAlignment(HorizontalAlignment::Stretch); // let the Grid fill so the dots reach the true top-right corner
        card.Padding(Thickness{ 8, 6, 8, 6 });
        card.Margin(Thickness{ 0, 0, 0, 6 });
        card.Background(Fill(selected ? 0x40 : 0x18, 0x80, 0x80, 0x80));
        card.BorderBrush(Fill(selected ? 0xFF : 0x60, 0x9E, 0x9E, 0x9E)); // gray — external / observe-only
        card.BorderThickness(selected ? Thickness{ 2, 2, 2, 2 } : Thickness{ 1, 1, 1, 1 });
        card.ContextFlyout(_MakeExternalTreeMenu(ex));
        const auto exId = ex.sessionId;
        const auto exCwd = ex.cwd;
        const auto exTitle = title;
        const auto exKind = ex.kind; // Phase C1: Claude vs Codex selects the read-only-plan reader
        const auto exRollout = ex.rolloutPath; // Codex rollout path (empty for Claude)
        card.Click([this, exId, exCwd, exTitle, exKind, exRollout](const IInspectable&, const RoutedEventArgs&) {
            _SelectExternal(exId, exCwd, exTitle, exKind, exRollout);
        });
        // Agentmaster: when the Fleet Observer has tailed an idle RECAP (away_summary) for this external
        // — read out-of-band from the SAME transcript-tail region a managed session's recap comes from
        // (ExternalClaudeRow.recap; see ProcessObserver) — append it below the base hint so a hover tells
        // the external sessions apart by what they were last doing, exactly like the managed card band
        // tooltip does with SessionInfo.recap. Shown in FULL (the tooltip wraps); no recap == base hint only.
        std::wstring cardTip{ L"An agent running outside Agentmaster (observe-only). Click to view its conversation read-only; right-click to Adopt it, start a session, or bring its window forward." };
        if (!ex.recap.empty())
        {
            cardTip += L"\n\nRecap: " + ex.recap;
        }
        AgentSetTip(card, winrt::hstring{ cardTip }, kCardTipDelay);
        // Fade the "\x22EF" more-button in (and arm its hit-testing) while the card is hovered; fade it
        // out on exit. dotsWeak is a weak_ref so the handler never strong-captures the button it lives
        // under. (No _ReportHover here — an external has no managed tab for the page to pill.)
        card.PointerEntered([dotsWeak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (const auto d = dotsWeak.get())
            {
                d.IsHitTestVisible(true);
                d.Opacity(0.85);
            }
        });
        card.PointerExited([dotsWeak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            if (PointerStillWithin(sender, e))
            {
                return; // a child label's exit bubbled up — the pointer is still on the card; keep the dots up
            }
            if (const auto d = dotsWeak.get())
            {
                d.Opacity(0.0);
                d.IsHitTestVisible(false);
            }
        });
        return card;
    }

    void AgentManagerContent::SetExternalClaudes(std::vector<::Agentmaster::ExternalClaudeRow> rows)
    {
        // Diff vs the current list (the observer pushes every probe tick) so an unchanged set is a
        // no-op — no board rebuild churn. Rows arrive pid-sorted from the observer, a stable order.
        bool same = (rows.size() == _externalClaudes.size());
        for (size_t i = 0; same && i < rows.size(); ++i)
        {
            const auto& a = rows[i];
            const auto& b = _externalClaudes[i];
            // Include the enrichment fields (id/title/host/branch) so a row that gains its title or
            // host a tick after first sight triggers one refresh. Timestamps are deliberately NOT
            // compared — mtime ticks constantly; the "ago" is recomputed live on any rebuild.
            if (a.pid != b.pid || a.cwd != b.cwd || a.model != b.model || a.effort != b.effort || a.background != b.background ||
                a.sessionId != b.sessionId || a.title != b.title || a.host != b.host || a.hostLabel != b.hostLabel || a.gitBranch != b.gitBranch || a.hostPid != b.hostPid ||
                a.kind != b.kind || a.sandbox != b.sandbox || a.approvalMode != b.approvalMode || // Phase C1: a codex row gaining its model/sandbox a tick after first sight triggers one refresh
                a.codexState != b.codexState || // Phase C2: a Codex turn flip (running<->waiting) repaints the row's state dot
                a.recap != b.recap) // Agentmaster: a fresh idle recap (away_summary) the observer tailed repaints the card/tree tooltip + read-only plan
            {
                same = false;
            }
        }
        if (same)
        {
            return;
        }
        _externalClaudes = std::move(rows);
        _Refresh();
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
        _treeRowsById.clear(); // refilled below (focus-restore map; see _Refresh). Cleared ONLY on
                               // the full-rebuild path — the rename early-return above keeps the tree
                               // (and so its existing map) intact.

        // Agentmaster: EXTERNAL scope renders the Fleet Observer's observe-only external claudes
        // (the _externalClaudes table, a different source than the registry snapshot) grouped by cwd,
        // with an Adopt / Open New Session Here / Bring Window To Front right-click menu. Delegate
        // and return.
        if (_treeScope == TreeScope::External)
        {
            _RebuildExternalTree();
            return;
        }

        // Agentmaster: Explorer Tree scope + ordering. localIds == the sessions hosted in THIS
        // window (the page's _claudeTabs, surfaced by _localScopeProvider). LOCAL (default) keeps
        // ONLY those; GLOBAL keeps every window's session (the whole process-wide registry) but
        // ORDERS this window's first and tags the rest "outside" (see the dir partition + the row
        // bucketing below). With no provider (mid-init) everything counts as local — no filter, no
        // reorder, no tag. isLocal() answers "is this session hosted in this window?".
        const bool haveLocal = static_cast<bool>(_localScopeProvider);
        std::unordered_set<std::wstring> localIds;
        if (haveLocal)
        {
            localIds = _localScopeProvider();
        }
        const auto isLocal = [&](const SessionInfo& s) { return !haveLocal || localIds.find(s.id) != localIds.end(); };

        std::vector<SessionInfo> scopedStore;
        const std::vector<SessionInfo>* scopedPtr = &sessions;
        if (_treeScope == TreeScope::Local && haveLocal)
        {
            scopedStore.reserve(sessions.size());
            for (const auto& s : sessions)
            {
                if (isLocal(s))
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
                continue; // closed sessions are shown in the Sessions browser, not the tree (FAVORITES.md)
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
            const wchar_t* empty = (_treeScope == TreeScope::Local && haveLocal)
                                       ? L"No sessions in this window \x2014 Launch above, or switch to GLOBAL for all windows."
                                       : L"No sessions yet \x2014 use Launch Claude above.";
            _treeHost.Children().Append(Text(empty, 12, false, 0.6));
            return;
        }

        // Agentmaster: order the directory groups by the active (global) sort. A dir's rank is an
        // aggregate over its live sessions — NEWEST: its newest session; OLDEST: its oldest; MOST
        // ACTIVE: its most-recent activity (a running session pins it to the top); A-Z: the dir name.
        // The local-first stable_partition below runs AFTER this and preserves the order within each
        // group, so the sort governs ordering while GLOBAL still floats this window's dirs first.
        const auto sortMode = _appSettings.treeSort;
        {
            struct DirAgg
            {
                int64_t maxCreated{ 0 };
                int64_t minCreated{ INT64_MAX };
                int64_t bestLast{ 0 };
                uint32_t minPid{ UINT32_MAX }; // BY PID: a dir's rank is its lowest host/claude pid
            };
            std::vector<std::pair<std::wstring, DirAgg>> ranked;
            ranked.reserve(dirs.size());
            for (const auto& dir : dirs)
            {
                DirAgg agg;
                for (const auto& s : scoped)
                {
                    if (s.live && PathEq(s.workingDir, dir))
                    {
                        const auto k = MakeSortKey(s);
                        agg.maxCreated = (std::max)(agg.maxCreated, k.created);
                        agg.minCreated = (std::min)(agg.minCreated, k.created);
                        agg.bestLast = (std::max)(agg.bestLast, k.active ? INT64_MAX : k.last);
                        agg.minPid = (std::min)(agg.minPid, k.pid);
                    }
                }
                ranked.push_back({ dir, agg });
            }
            std::stable_sort(ranked.begin(), ranked.end(), [&](const std::pair<std::wstring, DirAgg>& A, const std::pair<std::wstring, DirAgg>& B) {
                const auto& a = A.second;
                const auto& b = B.second;
                switch (sortMode)
                {
                case ::Agentmaster::ExplorerSort::Newest:
                    if (a.maxCreated != b.maxCreated)
                        return a.maxCreated > b.maxCreated;
                    break;
                case ::Agentmaster::ExplorerSort::Oldest:
                    if (a.minCreated != b.minCreated)
                        return a.minCreated < b.minCreated;
                    break;
                case ::Agentmaster::ExplorerSort::MostActive:
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast;
                    break;
                case ::Agentmaster::ExplorerSort::ByPid:
                    if (a.minPid != b.minPid)
                        return a.minPid < b.minPid;
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast; // then most active
                    break;
                case ::Agentmaster::ExplorerSort::Alpha:
                    break;
                }
                return CiLess(A.first, B.first); // A-Z primary, and the deterministic tiebreak for all modes
            });
            dirs.clear();
            for (auto& p : ranked)
            {
                dirs.push_back(std::move(p.first));
            }
        }

        // Agentmaster: surface directories holding at least one of THIS window's sessions before
        // directories that are entirely from other windows (stable within each group). In LOCAL
        // scope every dir is local, so this is a no-op; it only reorders the GLOBAL view.
        std::stable_partition(dirs.begin(), dirs.end(), [&](const std::wstring& d) {
            for (const auto& s : scoped)
            {
                if (s.live && PathEq(s.workingDir, d) && isLocal(s))
                {
                    return true;
                }
            }
            return false;
        });

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
            AgentSetTip(dirBtn, L"Working directory \x2014 click to scope the board to its sessions; click the scoped one again to collapse it.");
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

            // Order this dir's rows THIS window's sessions first, then "outside" ones (other
            // windows), each group ordered by the active (global) sort (Agentmaster). The pointers
            // stay valid: `scoped` is not mutated past this point. In LOCAL scope the outside group
            // is empty, so the sort governs the whole list.
            std::vector<const SessionInfo*> rowOrder;
            {
                std::vector<const SessionInfo*> localRows, outsideRows;
                for (const auto& s : scoped)
                {
                    if (s.live && PathEq(s.workingDir, dir))
                    {
                        (isLocal(s) ? localRows : outsideRows).push_back(&s);
                    }
                }
                const auto rowLess = [&](const SessionInfo* a, const SessionInfo* b) {
                    return SortKeyLess(sortMode, MakeSortKey(*a), MakeSortKey(*b));
                };
                std::stable_sort(localRows.begin(), localRows.end(), rowLess);
                std::stable_sort(outsideRows.begin(), outsideRows.end(), rowLess);
                rowOrder.reserve(localRows.size() + outsideRows.size());
                rowOrder.insert(rowOrder.end(), localRows.begin(), localRows.end());
                rowOrder.insert(rowOrder.end(), outsideRows.begin(), outsideRows.end());
            }

            for (const auto* sp : rowOrder)
            {
                const auto& s = *sp;
                const auto id = s.id;

                // In-place rename editor for this row (see _renameBox note in the header).
                // Editing inline sidesteps the XAML-Islands trap where a text box inside a
                // ContentDialog receives no keypresses.
                if (!_renamingId.empty() && _renamingId == id)
                {
                    auto box = TextBox{};
                    box.Text(s.title);
                    box.Margin(Thickness{ 16, 0, 0, 4 });
                    // Multi-line titles: a NON-commit Return INSERTS a newline (AcceptsReturn) rather
                    // than committing — matching the WT tab-rename box (TabHeaderControl). Focus-loss
                    // (click away / select another row) ALWAYS commits; Enter / Shift+Enter optionally
                    // commit per the GLOBAL TabRenameCommitMode (Rule #11 keeps this path == the tab
                    // renamer). In every case the TextBox or our PreviewKeyDown marks Return handled, so
                    // it won't bubble to the row's Enter=Activate. Escape still cancels.
                    box.AcceptsReturn(true);
                    box.TextWrapping(TextWrapping::Wrap);
                    _renameCommitOnKeyUp = false;
                    // PreviewKeyDown (tunneling) runs before the box's own key handling — the one place
                    // we can both see Enter reliably and SUPPRESS the AcceptsReturn newline for the commit
                    // combo. We don't commit on the down event (it rebuilds the tree and tears out this
                    // box mid-keystroke); the matching KeyUp commits, exactly like TabHeaderControl.
                    box.PreviewKeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (e.Key() != VirtualKey::Enter)
                        {
                            return;
                        }
                        const auto mode = _appSettings.tabRenameCommitMode;
                        if (mode == TabRenameCommitMode::ClickAwayOnly)
                        {
                            return; // both keys just insert a newline; commit by clicking away
                        }
                        // Shift distinguishes the commit key from the newline key (both arrive as Enter).
                        auto shiftDown = false;
                        if (const auto w = CoreWindow::GetForCurrentThread())
                        {
                            shiftDown = WI_IsFlagSet(w.GetKeyState(VirtualKey::Shift), winrt::Windows::UI::Core::CoreVirtualKeyStates::Down);
                        }
                        const bool commit = (mode == TabRenameCommitMode::ClickAwayOrShiftEnter) ? shiftDown : !shiftDown;
                        if (commit)
                        {
                            _renameCommitOnKeyUp = true;
                            e.Handled(true); // suppress the newline + stop the down bubbling; KeyUp commits
                        }
                    });
                    box.KeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (e.Key() == VirtualKey::Escape)
                        {
                            _CancelRename();
                            e.Handled(true);
                        }
                    });
                    box.KeyUp([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (_renameCommitOnKeyUp)
                        {
                            _renameCommitOnKeyUp = false;
                            e.Handled(true);
                            _CommitRename(); // == clicking away (idempotent; the LostFocus that follows no-ops)
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
                // State dot — the SAME filled Ellipse as the tab strip (StateDot == HeaderAgentStatusDot),
                // not the old per-state glyph, so the tree and the tab speak one visual language. The
                // StateLabel text appended below still names the state. A DORMANT session (live but its
                // claude hasn't started — a restored tab you haven't opened) shows the half-hollow twin.
                if (IsSessionDormant(s))
                {
                    auto dot = StateDotDormant(StateColor(s.state));
                    AgentSetTip(dot, L"Not started yet \x2014 the half-hollow dot means this session's claude hasn't initialized (a restored tab you haven't opened). Right-click \x2192 Activate Tab (or open the tab) to start it.");
                    row.Children().Append(dot);
                }
                else
                {
                    auto dot = StateDot(StateColor(s.state));
                    AgentSetTip(dot, L"Session state \x2014 the dot color matches the Triage Board column; the label to the right names it.");
                    row.Children().Append(dot);
                }
                row.Children().Append(Text(OneLine(s.title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ s.title }), 13, false, 1.0));
                // Agentmaster (Codex-launch): a teal "codex" agent pill on a MANAGED Codex row, mirroring
                // the Board card — distinguishes it from a Claude row at a glance (Claude = no pill).
                if (s.kind == AgentKind::Codex)
                {
                    auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
                    cp.Opacity(0.9);
                    AgentSetTip(cp, L"Codex agent \x2014 this managed session runs the OpenAI Codex CLI instead of Claude.");
                    row.Children().Append(cp);
                }
                row.Children().Append(Text(StateLabel(s.state), 11, false, 0.5));
                // Agentmaster: a gray "outside" tag marks a session hosted in another window (only
                // possible in GLOBAL scope; in LOCAL every row is this window's). It sits at the end
                // of the row, after the state label.
                if (!isLocal(s))
                {
                    auto outside = Pill(L"outside", Color{ 0xFF, 0x8A, 0x8A, 0x8A });
                    outside.Opacity(0.85);
                    AgentSetTip(outside, L"This session's tab lives in another window (shown only in GLOBAL scope). Double-click to jump to it.");
                    row.Children().Append(outside);
                }
                // Per-session timing (created-ago / active-for / last-activity-ago) from the transcript.
                {
                    const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
                    if (auto t = TimingText(s.convCreatedUnixMs, last))
                    {
                        row.Children().Append(t);
                    }
                }

                auto rowBtn = Button{};
                rowBtn.Content(row);
                rowBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
                rowBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
                rowBtn.Margin(Thickness{ 16, 0, 0, 0 });
                rowBtn.Padding(Thickness{ 4, 2, 4, 2 });
                rowBtn.Background(Fill(selected ? 0x40 : 0x00, 0x80, 0x80, 0x80));
                rowBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });

                // Agentmaster (Linked Lenses): report hover so the page pills this session's terminal
                // tab while the Manager tab is active (the board-card twin, above). Capture id by value
                // + `this`, never the Button into its own handler (a self-capture leaks the element).
                rowBtn.PointerEntered([this, id](const IInspectable&, const PointerRoutedEventArgs&) { _ReportHover(id, true); });
                rowBtn.PointerExited([this, id](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                    if (PointerStillWithin(sender, e))
                    {
                        return; // a child label's exit bubbled up — don't un-pill the tab as the mouse crosses the row's labels
                    }
                    _ReportHover(id, false);
                });
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
                        // Agentmaster (double-click fix): _SelectSession rebuilt the tree — every row
                        // Button is cleared + recreated (_RebuildTree), so this row's replacement isn't
                        // arranged until the next async layout pass and a follow-up second click (a
                        // double-click to Activate) would miss it. Force a synchronous layout so the
                        // replacement row has real bounds immediately and the second click lands — the
                        // board-card twin above. (An already-selected re-click early-outs in
                        // _SelectSession with no rebuild, so only this not-selected path needs it.)
                        if (_treeHost)
                        {
                            _treeHost.UpdateLayout();
                        }
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
                // Right-click (or context key / long-press) menu: Rename / Archive / Open New Session Here.
                rowBtn.ContextFlyout(_MakeSessionMenu(id, s.workingDir));
                AgentSetTip(rowBtn, L"Click to select this session \x2014 double-click or Enter jumps to its live tab; F2 renames, Del archives, right-click for more.");
                // Agentmaster: tag + register the row so _Refresh can RESTORE keyboard focus onto it
                // after a rebuild (see _MakeCard for the board-lens twin). "t:" marks the tree lens.
                rowBtn.Tag(winrt::box_value(winrt::hstring{ L"t:" + id }));
                _treeRowsById[id] = rowBtn;
                _treeHost.Children().Append(rowBtn);
            }
        }
    }

    // Agentmaster: render the Explorer Tree's EXTERNAL scope — observe-only external claudes (real
    // Windows Terminal AND cmd-/console-hosted; the observer correlated them but will never bind,
    // Rule #9/#13), grouped by working dir, each row enriched from its transcript (title / host /
    // branch / timing). Left-click SELECTS a row -> the Flight Plan shows its conversation read-only
    // (we host no ConPTY, so it is never drivable). Right-click -> Open New Session Here (spawn a
    // managed session in that cwd) / Adopt (resume its conversation into a managed tab).
    void AgentManagerContent::_RebuildExternalTree()
    {
        if (_externalClaudes.empty())
        {
            _treeHost.Children().Append(Text(L"No external claudes detected \x2014 these are claudes running in real Windows Terminal or other hosts.", 12, false, 0.6));
            return;
        }

        // Ordered, de-duplicated working dirs (filesystem-aware, like the session tree). The
        // first-seen spelling is the display name; an empty cwd (denied/unreadable PEB) groups under
        // "(unknown)". Rows arrive pid-sorted from the observer, a stable order.
        const auto dirOf = [](const ::Agentmaster::ExternalClaudeRow& ex) -> std::wstring {
            return ex.cwd.empty() ? std::wstring{ L"(unknown)" } : ex.cwd;
        };
        std::vector<std::wstring> dirs;
        for (const auto& ex : _externalClaudes)
        {
            const std::wstring d = dirOf(ex);
            if (std::find_if(dirs.begin(), dirs.end(), [&](const std::wstring& x) { return PathEq(x, d); }) == dirs.end())
            {
                dirs.push_back(d);
            }
        }

        // Agentmaster: order the external dirs by the active (global) sort, exactly like the managed
        // tree — a dir's rank aggregates its externals (NEWEST/OLDEST by creation, MOST ACTIVE by
        // recency, A-Z by name). Rows within each dir are sorted the same way below.
        const auto sortMode = _appSettings.treeSort;
        {
            struct DirAgg
            {
                int64_t maxCreated{ 0 };
                int64_t minCreated{ INT64_MAX };
                int64_t bestLast{ 0 };
                uint32_t minPid{ UINT32_MAX }; // BY PID: a dir's rank is its lowest host-window pid
            };
            std::vector<std::pair<std::wstring, DirAgg>> ranked;
            ranked.reserve(dirs.size());
            for (const auto& dir : dirs)
            {
                DirAgg agg;
                for (const auto& ex : _externalClaudes)
                {
                    if (PathEq(dirOf(ex), dir))
                    {
                        const auto k = MakeSortKey(ex);
                        agg.maxCreated = (std::max)(agg.maxCreated, k.created);
                        agg.minCreated = (std::min)(agg.minCreated, k.created);
                        agg.bestLast = (std::max)(agg.bestLast, k.active ? INT64_MAX : k.last);
                        agg.minPid = (std::min)(agg.minPid, k.pid);
                    }
                }
                ranked.push_back({ dir, agg });
            }
            std::stable_sort(ranked.begin(), ranked.end(), [&](const std::pair<std::wstring, DirAgg>& A, const std::pair<std::wstring, DirAgg>& B) {
                const auto& a = A.second;
                const auto& b = B.second;
                switch (sortMode)
                {
                case ::Agentmaster::ExplorerSort::Newest:
                    if (a.maxCreated != b.maxCreated)
                        return a.maxCreated > b.maxCreated;
                    break;
                case ::Agentmaster::ExplorerSort::Oldest:
                    if (a.minCreated != b.minCreated)
                        return a.minCreated < b.minCreated;
                    break;
                case ::Agentmaster::ExplorerSort::MostActive:
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast;
                    break;
                case ::Agentmaster::ExplorerSort::ByPid:
                    if (a.minPid != b.minPid)
                        return a.minPid < b.minPid;
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast; // then most active
                    break;
                case ::Agentmaster::ExplorerSort::Alpha:
                    break;
                }
                return CiLess(A.first, B.first);
            });
            dirs.clear();
            for (auto& p : ranked)
            {
                dirs.push_back(std::move(p.first));
            }
        }

        for (const auto& dir : dirs)
        {
            const bool collapsed = _collapsedDirs.find(dir) != _collapsedDirs.end();
            int count = 0;
            for (const auto& ex : _externalClaudes)
            {
                if (PathEq(dirOf(ex), dir))
                {
                    ++count;
                }
            }

            // dir header (collapsible). Unlike the session tree it does NOT change the board scope:
            // externals are a global census, not part of the managed directory tree.
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
            dirBtn.Background(Fill(0x00, 0x80, 0x80, 0x80));
            dirBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            dirBtn.Padding(Thickness{ 4, 2, 4, 2 });
            AgentSetTip(dirBtn, L"Working directory of these external sessions \x2014 click to collapse or expand the group.");
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
                _NotifyLensChanged(); // collapsed dirs are part of the per-window lens (M10)
                _Refresh();
            });
            _treeHost.Children().Append(dirBtn);

            if (collapsed)
            {
                continue;
            }

            // This dir's external rows, ordered by the active (global) sort. Pointers into
            // _externalClaudes stay valid — it is not mutated during the render.
            std::vector<const ::Agentmaster::ExternalClaudeRow*> dirRows;
            for (const auto& ex : _externalClaudes)
            {
                if (PathEq(dirOf(ex), dir))
                {
                    dirRows.push_back(&ex);
                }
            }
            std::stable_sort(dirRows.begin(), dirRows.end(), [&](const ::Agentmaster::ExternalClaudeRow* a, const ::Agentmaster::ExternalClaudeRow* b) {
                return SortKeyLess(sortMode, MakeSortKey(*a), MakeSortKey(*b));
            });
            for (const auto* exp : dirRows)
            {
                const auto& ex = *exp;

                const bool selExt = !ex.sessionId.empty() && ex.sessionId == _selectedExternalSessionId;

                // Title: the conversation's first prompt (from the transcript), else the cwd leaf,
                // else "claude". Truncated for the row.
                std::wstring title = ex.title;
                if (title.empty())
                {
                    std::wstring leaf = ex.cwd;
                    const auto slash = leaf.find_last_of(L"\\/");
                    if (slash != std::wstring::npos && slash + 1 < leaf.size())
                    {
                        leaf = leaf.substr(slash + 1);
                    }
                    title = leaf.empty() ? std::wstring{ L"claude" } : leaf;
                }
                if (title.size() > 60)
                {
                    title = title.substr(0, 57) + L"\x2026";
                }

                auto row = StackPanel{};
                row.Orientation(Orientation::Horizontal);
                row.Spacing(6);
                // State dot — the SAME filled Ellipse as the tab strip (StateDot == HeaderAgentStatusDot).
                // A Claude external carries no PULL state -> gray (observe-only). For a Codex row (Phase
                // C2) the rollout-derived turn state colors it: blue running / gold waiting / gray idle.
                auto g = StateDot(ex.kind == AgentKind::Codex ? CodexStateColor(ex.codexState) : Color{ 0xFF, 0x9E, 0x9E, 0x9E });
                if (ex.kind == AgentKind::Codex)
                {
                    AgentSetTip(g, winrt::hstring{ L"Codex turn state \x2014 " } + CodexStateLabel(ex.codexState) + winrt::hstring{ L", derived from its rollout transcript" });
                }
                else
                {
                    AgentSetTip(g, L"Observed only \x2014 Agentmaster doesn't track an external Claude session's turn state.");
                }
                row.Children().Append(g);
                // Agentmaster (Phase C1): a teal "codex" agent pill on Codex rows (Claude = default, no pill).
                if (ex.kind == AgentKind::Codex)
                {
                    auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
                    cp.Opacity(0.9);
                    AgentSetTip(cp, L"Codex agent \x2014 this external session runs the OpenAI Codex CLI (observed, not managed).");
                    row.Children().Append(cp);
                }
                {
                    auto titleText = Text(winrt::hstring{ title }, 13, false, 1.0);
                    // Agentmaster: surface the observer-tailed idle RECAP (away_summary) on hover — the
                    // external analog of the managed row's recap, read from the SAME transcript-tail
                    // region (ExternalClaudeRow.recap; see ProcessObserver). Full text (the tooltip wraps).
                    if (!ex.recap.empty())
                    {
                        AgentSetTip(titleText, winrt::hstring{ title } + winrt::hstring{ L"\n\nRecap: " } + winrt::hstring{ ex.recap });
                    }
                    row.Children().Append(titleText);
                }

                // host tag: the foreign host this claude runs in — "Windows Terminal" (real WT) vs
                // "Agentmaster" / "Agentmaster Dev" (another of our instances) vs cmd / pwsh. Resolved by
                // the hosting terminal's package family / image path (ResolveExternalHostLabel), so our
                // fork is never mislabeled "WindowsTerminal" (its exe leaf) and dev/release are distinct.
                {
                    std::wstring hostLabel = ex.hostLabel;
                    if (hostLabel.empty())
                    {
                        if (!ex.hostImage.empty())
                        {
                            hostLabel = ex.hostImage;
                            const auto dot = hostLabel.rfind(L".exe");
                            if (dot != std::wstring::npos)
                            {
                                hostLabel = hostLabel.substr(0, dot);
                            }
                        }
                        else
                        {
                            hostLabel = (ex.host == RunningApp::WindowsTerminal) ? L"wt" : L"ext";
                        }
                    }
                    auto hp = Pill(winrt::hstring{ hostLabel }, Color{ 0xFF, 0x6E, 0x7B, 0x8A });
                    hp.Opacity(0.85);
                    AgentSetTip(hp, L"Host \x2014 the terminal application this external session runs in (e.g. Windows Terminal, a sibling Agentmaster, cmd, or pwsh).");
                    row.Children().Append(hp);
                }

                if (!ex.gitBranch.empty())
                {
                    row.Children().Append(Text(winrt::hstring{ L"[" } + winrt::hstring{ ex.gitBranch } + L"]", 11, false, 0.5));
                }

                // model · effort · bg · pid
                {
                    std::wstring me;
                    const auto addPart = [&](const std::wstring& part) {
                        if (part.empty())
                        {
                            return;
                        }
                        if (!me.empty())
                        {
                            me += L"  \x00B7  ";
                        }
                        me += part;
                    };
                    addPart(ex.model);
                    addPart(ex.effort);
                    addPart(ex.sandbox); // Codex only (empty for Claude)
                    addPart(ex.approvalMode); // Codex only
                    if (ex.background)
                    {
                        addPart(L"bg");
                    }
                    if (!me.empty())
                    {
                        auto meText = Text(winrt::hstring{ me }, 11, false, 0.5);
                        AgentSetTip(meText, L"Model \xB7 reasoning effort \xB7 (Codex adds sandbox \xB7 approval policy) \xB7 bg = running in the background.");
                        row.Children().Append(meText);
                    }
                }
                // pid — its UNDERLINE is COLOR-CODED by the host window/shell (ex.hostPid): claudes
                // running in the same terminal window/tab share a host shell, so they get the same
                // underline color and are easy to identify at a glance, even across cwd groups.
                {
                    const uint32_t key = ex.hostPid ? ex.hostPid : ex.pid;
                    auto pidCol = StackPanel{};
                    pidCol.Spacing(1);
                    pidCol.Children().Append(Text(winrt::hstring{ L"pid " } + winrt::to_hstring(ex.pid), 11, false, 0.55));
                    auto underline = Border{};
                    underline.Height(2);
                    underline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
                    underline.HorizontalAlignment(HorizontalAlignment::Stretch); // span the "pid N" width
                    underline.Background(SolidColorBrush{ WindowKeyColor(key) });
                    AgentSetTip(underline, winrt::hstring{ L"Host window / shell PID " } + winrt::to_hstring(key) + L" \x2014 rows sharing this underline color run in the same terminal window / tab");
                    pidCol.Children().Append(underline);
                    row.Children().Append(pidCol);
                }

                // timing (created-ago / active-for / last-activity-ago) — transcript ctime/mtime,
                // falling back to the process start time when there is no transcript yet.
                {
                    const int64_t created = ex.createdUnixMs ? ex.createdUnixMs : ex.startUnixMs;
                    if (auto t = TimingText(created, ex.lastActivityUnixMs))
                    {
                        row.Children().Append(t);
                    }
                }

                auto rowBtn = Button{};
                rowBtn.Content(row);
                rowBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
                rowBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
                rowBtn.Margin(Thickness{ 16, 0, 0, 0 });
                rowBtn.Padding(Thickness{ 4, 2, 4, 2 });
                rowBtn.Background(Fill(selExt ? 0x40 : 0x00, 0x80, 0x80, 0x80));
                rowBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });

                // Left-click SELECTS this external -> the Flight Plan shows its conversation prompts
                // read-only (observe-only; we host no ConPTY so we can't drive it). Right-click -> the
                // Adopt / Open New Session Here / Bring Window To Front menu.
                rowBtn.ContextFlyout(_MakeExternalTreeMenu(ex));
                const auto exId = ex.sessionId;
                const auto exCwd = ex.cwd;
                const auto exTitle = title;
                const auto exKind = ex.kind; // Phase C1
                const auto exRollout = ex.rolloutPath;
                rowBtn.Click([this, exId, exCwd, exTitle, exKind, exRollout](const IInspectable&, const RoutedEventArgs&) {
                    _SelectExternal(exId, exCwd, exTitle, exKind, exRollout);
                });
                AgentSetTip(rowBtn, L"An agent running outside Agentmaster (observe-only). Click to view its conversation read-only; right-click to Adopt it, start a session, or bring its window forward.");
                _treeHost.Children().Append(rowBtn);
            }
        }
    }

    // Agentmaster: the EXTERNAL-tree row right-click menu. For a CLAUDE row: Adopt (resume the
    // conversation into a managed, controllable tab), Open New Session Here (spawn a managed session
    // in the cwd), and Bring Window To Front. For a CODEX row (kind=Codex, observe-only in Phase C1):
    // Adopt + Open-New are omitted (both drive CLAUDE; Codex control is a later phase) — a disabled
    // note says so — and only the agent-agnostic Bring Window To Front is offered. All clickable items
    // defer one tick like _MakeSessionMenu so the closing flyout's focus restore doesn't race the
    // spawn / tree rebuild / foreground hand-off. Acts on the row's (pid, cwd) — an external/observe-
    // only row has no registry session id.
    MenuFlyout AgentManagerContent::_MakeExternalTreeMenu(const ::Agentmaster::ExternalClaudeRow& ex)
    {
        MenuFlyout menu;
        auto disp = _dispatcher;
        auto weak = get_weak();
        const uint32_t pid = ex.pid;
        const std::wstring cwd = ex.cwd;

        if (ex.kind == AgentKind::Codex)
        {
            // Codex-launch (lifecycle + state): Adopt resumes this codex's rollout into a MANAGED tab
            // (`codex resume <uuid>`; the original keeps running), and Open New Codex Session Here
            // launches a fresh managed codex in the cwd. Both route to _codexLaunchHandler (adopt flag).
            // No injector/Autopilot yet — you type into the tab directly (driving Codex is a later phase).
            const std::wstring sid = ex.sessionId;
            const std::wstring adoptTitle = ex.title;
            MenuFlyoutItem adopt;
            adopt.Text(L"Adopt");
            AgentSetTip(adopt, sid.empty() ?
                                   winrt::hstring{ L"This Codex session hasn't been prompted yet (no rollout) \x2014 Adopt launches a fresh managed Codex here" } :
                                   winrt::hstring{ L"Bring this Codex session's conversation under management \x2014 Fork a safe copy (codex fork) or Resume the same rollout" });
            // Two processes can't safely share one rollout, and the external is still running, so OFFER
            // the choice (the chosen "warn and let me choose"): Fork a copy (codex fork -> a NEW rollout,
            // the source untouched -> safe) vs. Resume anyway (codex resume -> the same rollout; stop the
            // original first). A never-prompted codex (no rollout) just launches fresh, no dialog.
            adopt.Click([weak, disp, pid, cwd, sid, adoptTitle](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, pid, cwd, sid, adoptTitle]() {
                    auto self = weak.get();
                    if (!self || !self->_codexLaunchHandler)
                    {
                        return;
                    }
                    if (sid.empty())
                    {
                        self->_codexLaunchHandler(pid, winrt::hstring{ cwd }, true, false); // no rollout -> launch fresh
                        return;
                    }
                    const winrt::hstring label = adoptTitle.empty() ? winrt::hstring{ L"This Codex session" } : winrt::hstring{ L"\x201C" + adoptTitle + L"\x201D" };
                    self->_ConfirmChoice(
                        L"Adopt Codex session",
                        label + winrt::hstring{ L" is still running outside Agentmaster. Two processes can't safely share one rollout.\n\nFork a copy: branch its current state into a controllable session (codex fork) \x2014 safe, the original is untouched.\nResume anyway: take over the same rollout \x2014 stop the original first to avoid two writers." },
                        L"Fork a copy",
                        L"Resume anyway",
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_codexLaunchHandler) { s->_codexLaunchHandler(pid, winrt::hstring{ cwd }, true, true); } } },
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_codexLaunchHandler) { s->_codexLaunchHandler(pid, winrt::hstring{ cwd }, true, false); } } });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(adopt);

            MenuFlyoutItem openHereCx;
            openHereCx.Text(L"Open New Codex Session Here");
            AgentSetTip(openHereCx, L"Launch a managed Codex session in this directory (a new, independent conversation)");
            openHereCx.Click([weak, disp, cwd](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, cwd]() { if (auto self = weak.get()) { if (self->_codexLaunchHandler) { self->_codexLaunchHandler(0, winrt::hstring{ cwd }, false, false); } } });
                }
                else if (auto self = weak.get())
                {
                    if (self->_codexLaunchHandler) { self->_codexLaunchHandler(0, winrt::hstring{ cwd }, false, false); }
                }
            });
            menu.Items().Append(openHereCx);

            MenuFlyoutItem copyIdCx;
            copyIdCx.Text(L"Copy Session Id");
            if (sid.empty())
            {
                copyIdCx.IsEnabled(false);
                AgentSetTip(copyIdCx, L"No rollout id yet (this Codex session hasn't been prompted)");
            }
            else
            {
                AgentSetTip(copyIdCx, L"Copy this Codex rollout's conversation id to the clipboard");
                copyIdCx.Click([sid](const IInspectable&, const RoutedEventArgs&) { CopyTextToClipboard(sid); });
            }
            menu.Items().Append(copyIdCx);
        }
        else
        {
            const std::wstring sid = ex.sessionId;
            const std::wstring adoptTitle = ex.title;
            MenuFlyoutItem adopt;
            adopt.Text(L"Adopt");
            AgentSetTip(adopt, sid.empty() ?
                                   winrt::hstring{ L"This Claude session hasn't been prompted yet (no transcript) \x2014 Adopt launches a fresh managed session here" } :
                                   winrt::hstring{ L"Bring this external Claude's conversation under management \x2014 Fork a safe copy (--fork-session) or Resume the same conversation" });
            // Two processes can't safely share one transcript, and the external is still running, so OFFER
            // the choice (the chosen "warn and let me choose"): Fork a copy (claude --fork-session -> a NEW
            // transcript, the source untouched -> safe) vs. Resume anyway (claude --resume -> the same
            // conversation; stop the original first). A never-prompted claude (no transcript) launches fresh.
            adopt.Click([weak, disp, pid, cwd, sid, adoptTitle](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, pid, cwd, sid, adoptTitle]() {
                    auto self = weak.get();
                    if (!self || !self->_adoptExternalHandler)
                    {
                        return;
                    }
                    // Native-exe-only policy: Adopt resumes/forks the external's conversation into a NEW
                    // managed claude (a launch under the hood), so it needs a native claude.exe just like
                    // the Launch/Fork buttons. Without this gate the launch silently no-ops at the engine
                    // backstop ([launch-blocked]) and the button "does nothing". EnsureClaudeAvailable
                    // re-resolves first, so a claude installed since launch clears the gate automatically.
                    if (!::Agentmaster::EnsureClaudeAvailable())
                    {
                        self->_ShowClaudeMissing();
                        return;
                    }
                    if (sid.empty())
                    {
                        self->_adoptExternalHandler(pid, winrt::hstring{ cwd }, false); // no transcript -> launch fresh
                        return;
                    }
                    const winrt::hstring label = adoptTitle.empty() ? winrt::hstring{ L"This Claude session" } : winrt::hstring{ L"\x201C" + adoptTitle + L"\x201D" };
                    self->_ConfirmChoice(
                        L"Adopt Claude session",
                        label + winrt::hstring{ L" is still running outside Agentmaster. Two processes can't safely share one transcript.\n\nFork a copy: branch its current state into a controllable session (claude --fork-session) \x2014 safe, the original is untouched.\nResume anyway: take over the same conversation \x2014 stop the original first to avoid two writers." },
                        L"Fork a copy",
                        L"Resume anyway",
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_adoptExternalHandler) { s->_adoptExternalHandler(pid, winrt::hstring{ cwd }, true); } } },
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_adoptExternalHandler) { s->_adoptExternalHandler(pid, winrt::hstring{ cwd }, false); } } });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(adopt);

            // Open New Session Here — offered in every scope (matches _MakeSessionMenu's LOCAL/GLOBAL
            // ordering): spawn a managed session in this external's cwd (a new, independent
            // conversation — distinct from Adopt, which resumes the external's existing conversation).
            MenuFlyoutItem openHere;
            openHere.Text(L"Open New Session Here");
            AgentSetTip(openHere, L"Launch a managed Claude session in this directory (a new, independent conversation)");
            openHere.Click([weak, disp, cwd](const IInspectable&, const RoutedEventArgs&) {
                // Spawning a managed Claude session needs a native claude.exe (native-exe-only policy) —
                // gate with the same re-resolve-then-prompt the Adopt action above uses, so this never
                // silently no-ops at the engine backstop when Claude isn't installed.
                auto act = [weak, cwd]() {
                    auto self = weak.get();
                    if (!self || !self->_spawnHandler)
                    {
                        return;
                    }
                    if (!::Agentmaster::EnsureClaudeAvailable())
                    {
                        self->_ShowClaudeMissing();
                        return;
                    }
                    self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{});
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(openHere);

            // Copy Session Id — the resolved conversation id. Empty for a never-prompted external
            // (no transcript id yet, Rule #14) -> the item is disabled. Synchronous clipboard write,
            // no defer needed (matches the Archive page's "Copy id").
            MenuFlyoutItem copyId;
            copyId.Text(L"Copy Session Id");
            // `sid` is already declared at the top of this branch (the Adopt dialog gate reuses it).
            if (sid.empty())
            {
                copyId.IsEnabled(false);
                AgentSetTip(copyId, L"No conversation id yet (this Claude session hasn't been prompted)");
            }
            else
            {
                AgentSetTip(copyId, L"Copy this session's conversation id to the clipboard");
                copyId.Click([sid](const IInspectable&, const RoutedEventArgs&) {
                    CopyTextToClipboard(sid);
                });
            }
            menu.Items().Append(copyId);
        }

        // Bring Window To Front — the LAST option, for BOTH agents: surface the window HOSTING this
        // session (unminimize + foreground; a Windows Terminal-class host also gets the tab selected,
        // best-effort). Observe-only safe: window activation only — it never writes into the foreign
        // session (Rule #13). Defers a tick so the closing flyout's focus restore lands before
        // foreground is handed to the other window.
        MenuFlyoutItem bringFront;
        bringFront.Text(L"Bring Window To Front");
        AgentSetTip(bringFront, L"Unminimize + foreground the window hosting this session; a Windows Terminal host also gets its tab selected (best-effort)");
        bringFront.Click([weak, disp, pid, cwd](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, pid, cwd]() { if (auto self = weak.get()) { self->_BringExternalToFront(pid, cwd); } });
            }
            else if (auto self = weak.get())
            {
                self->_BringExternalToFront(pid, cwd);
            }
        });
        menu.Items().Append(bringFront);

        return menu;
    }

    // Agentmaster: Bring Window To Front (the EXTERNAL right-click's last item). Resolve the row's
    // host facts from the latest observer snapshot — hostPid roots the window walk when the claude
    // already exited; the title feeds the WT tab-match heuristics (the menu captured only pid +
    // cwd) — then hand the actual window work to a BACKGROUND thread: BringClaudeWindowToFront
    // takes a Toolhelp snapshot and does cross-process UI Automation reads (tens of ms, can block),
    // so it must never run on the UI thread. Fire-and-forget — no UI mutation afterwards, so
    // nothing posts back (and nothing captures `this` past the detach).
    void AgentManagerContent::_BringExternalToFront(uint32_t pid, const std::wstring& cwd)
    {
        uint32_t hostPid = 0;
        std::wstring sessionId;
        std::wstring title;
        for (const auto& ex : _externalClaudes)
        {
            if (ex.pid == pid)
            {
                hostPid = ex.hostPid;
                sessionId = ex.sessionId; // lets the worker read the transcript (custom title + prompt corpus) for the tab pick
                title = ex.title;
                break;
            }
        }
        // Nav audit: the user asked to surface an EXTERNAL claude's hosting window (the external row menu's
        // "Bring Window To Front"). The OS work (walk ancestors -> foreground -> UIA tab pick) runs on a
        // detached worker, so this records the intent + what was picked (pid / host shell / conversation).
        ::Agentmaster::LogNav(L"manager bring-to-front pid=" + std::to_wstring(pid) + L" host=" + std::to_wstring(hostPid) + L" " + ::Agentmaster::ShortId(sessionId) + L" cwd=" + cwd);
        std::thread([pid, hostPid, sessionId = std::move(sessionId), title = std::move(title), cwd]() {
            ::Agentmaster::BringClaudeWindowToFront(pid, hostPid, sessionId, title, cwd);
        }).detach();
    }

    // Agentmaster: advance the Explorer Tree scope LOCAL -> GLOBAL -> EXTERNAL -> LOCAL. The scope
    // is ONE state shared with the Triage Board's 2-way toggle and persisted per window in the lens
    // (ManagerState.treeScope); _SetTreeScope is the single mutator behind both buttons.
    void AgentManagerContent::_ToggleTreeScope()
    {
        switch (_treeScope)
        {
        case TreeScope::Local:
            _SetTreeScope(TreeScope::Global);
            break;
        case TreeScope::Global:
            _SetTreeScope(TreeScope::External);
            break;
        default:
            _SetTreeScope(TreeScope::Local);
            break;
        }
    }

    // Agentmaster: the ONE scope mutator behind BOTH toggles — the tree's 3-way cycle above and the
    // board's LOCAL/GLOBAL flip (which reads External as Global, so from EXTERNAL its click lands on
    // LOCAL). Handles the External enter/leave selection cleanup, reflects both buttons, pushes the
    // lens (the scope is persisted per window now), and refreshes — skippable when the caller
    // refreshes itself (e.g. _OnRenameSession, which refreshes once after its other mutations).
    void AgentManagerContent::_SetTreeScope(TreeScope scope, bool refresh)
    {
        if (_treeScope == scope)
        {
            return;
        }
        _treeScope = scope;
        // Entering EXTERNAL: externals are observe-only, so drop any managed session selection — the
        // Flight Plan then reads "nothing selected" until an external row is clicked (read-only).
        if (_treeScope == TreeScope::External && !_selectedId.empty())
        {
            _selectedId.clear();
            _selectedPromptId.clear();
        }
        // Leaving EXTERNAL: drop the external (read-only) selection so the Flight Plan returns to the
        // managed view cleanly.
        if (_treeScope != TreeScope::External && !_selectedExternalTitle.empty())
        {
            _selectedExternalSessionId.clear();
            _selectedExternalCwd.clear();
            _selectedExternalTitle.clear();
            _externalPlanLoadedFor.clear();
            _externalPlanPrompts.clear();
        }
        _UpdateTreeScopeButton();
        _UpdateBoardScopeButton();
        _NotifyLensChanged(); // the scope (and any selection it cleared) is part of the per-window lens
        if (refresh)
        {
            // _Refresh (not just _RebuildTree) so the board re-filters (LOCAL/GLOBAL), the selection
            // highlight tracks, and the Flight Plan re-renders when entering/leaving EXTERNAL.
            _Refresh();
        }
    }

    // Reflect the current scope on the toggle button's label.
    void AgentManagerContent::_UpdateTreeScopeButton()
    {
        if (_treeScopeBtn)
        {
            const wchar_t* label = (_treeScope == TreeScope::Global)     ? L"GLOBAL"
                                   : (_treeScope == TreeScope::External) ? L"EXTERNAL"
                                                                         : L"LOCAL";
            _treeScopeBtn.Content(winrt::box_value(label));
        }
    }

    // Reflect the shared scope on the BOARD toggle's label — 2-way: the board has no External mode,
    // so EXTERNAL (a tree-only view) reads as GLOBAL here (the board then shows every window's
    // sessions, which is what it renders in that scope).
    void AgentManagerContent::_UpdateBoardScopeButton()
    {
        if (_boardScopeBtn)
        {
            const wchar_t* label = (_treeScope == TreeScope::Local) ? L"LOCAL" : L"GLOBAL";
            _boardScopeBtn.Content(winrt::box_value(label));
        }
    }

    // Agentmaster: advance the Triage Board sort MOST ACTIVE -> NEWEST -> OLDEST -> A-Z -> MOST ACTIVE
    // (it never visits BY PID — pid grouping is meaningless once cards split across the state columns).
    // A SEPARATE global setting from the tree's sort (AppSettings::boardSort), so the board and tree
    // remember their own order. Like treeSort it is GLOBAL + persisted: mutate _appSettings.boardSort,
    // refresh the label, push it through the settings sink (the page persists settings.json), so the
    // choice survives restart and seeds every other / future window; _Refresh re-sorts THIS window's board now.
    void AgentManagerContent::_CycleBoardSort()
    {
        using ::Agentmaster::ExplorerSort;
        switch (_appSettings.boardSort)
        {
        case ExplorerSort::MostActive:
            _appSettings.boardSort = ExplorerSort::Newest;
            break;
        case ExplorerSort::Newest:
            _appSettings.boardSort = ExplorerSort::Oldest;
            break;
        case ExplorerSort::Oldest:
            _appSettings.boardSort = ExplorerSort::Alpha;
            break;
        case ExplorerSort::Alpha:
        case ExplorerSort::ByPid: // not produced by the board cycle, but treat as "wrap to the default"
        default:
            _appSettings.boardSort = ExplorerSort::MostActive;
            break;
        }
        _UpdateBoardSortButton();
        if (_settingsSink)
        {
            _settingsSink(_appSettings); // persist globally (settings.json) + re-materialize
        }
        _Refresh();
    }

    // Reflect the current (global) board sort on the toggle button's label.
    void AgentManagerContent::_UpdateBoardSortButton()
    {
        if (_boardSortBtn)
        {
            using ::Agentmaster::ExplorerSort;
            const wchar_t* label = (_appSettings.boardSort == ExplorerSort::Newest) ? L"NEWEST"
                                   : (_appSettings.boardSort == ExplorerSort::Oldest) ? L"OLDEST"
                                   : (_appSettings.boardSort == ExplorerSort::Alpha)  ? L"A\x2013Z"
                                                                                      : L"MOST ACTIVE"; // MostActive (default) + any stray ByPid
            _boardSortBtn.Content(winrt::box_value(label));
        }
    }

    // Agentmaster: advance the Explorer Tree sort NEWEST -> OLDEST -> MOST ACTIVE -> A-Z -> BY PID ->
    // NEWEST. The sort is a GLOBAL setting: mutate _appSettings.treeSort, refresh the label, then push
    // it through the settings sink (the page persists it to settings.json and re-materializes), so the
    // choice survives restart and seeds every other / future window. _Refresh re-sorts THIS window's
    // tree (and re-renders the board/plan) immediately.
    void AgentManagerContent::_CycleTreeSort()
    {
        using ::Agentmaster::ExplorerSort;
        switch (_appSettings.treeSort)
        {
        case ExplorerSort::Newest:
            _appSettings.treeSort = ExplorerSort::Oldest;
            break;
        case ExplorerSort::Oldest:
            _appSettings.treeSort = ExplorerSort::MostActive;
            break;
        case ExplorerSort::MostActive:
            _appSettings.treeSort = ExplorerSort::Alpha;
            break;
        case ExplorerSort::Alpha:
            _appSettings.treeSort = ExplorerSort::ByPid;
            break;
        case ExplorerSort::ByPid:
        default:
            _appSettings.treeSort = ExplorerSort::Newest;
            break;
        }
        _UpdateTreeSortButton();
        if (_settingsSink)
        {
            _settingsSink(_appSettings); // persist globally (settings.json) + re-materialize
        }
        _Refresh();
    }

    // Reflect the current (global) sort on the toggle button's label.
    void AgentManagerContent::_UpdateTreeSortButton()
    {
        if (_treeSortBtn)
        {
            using ::Agentmaster::ExplorerSort;
            const wchar_t* label = (_appSettings.treeSort == ExplorerSort::Oldest)       ? L"OLDEST"
                                   : (_appSettings.treeSort == ExplorerSort::MostActive) ? L"MOST ACTIVE"
                                   : (_appSettings.treeSort == ExplorerSort::Alpha)      ? L"A\x2013Z"
                                   : (_appSettings.treeSort == ExplorerSort::ByPid)      ? L"BY PID"
                                                                                         : L"NEWEST";
            _treeSortBtn.Content(winrt::box_value(label));
        }
    }

    // ---- Explorer-tree session actions (right-click menu, rename, delete) ----

    MenuFlyout AgentManagerContent::_MakeSessionMenu(const std::wstring& id, const std::wstring& cwd)
    {
        MenuFlyout menu;
        auto disp = _dispatcher;
        auto weak = get_weak();

        // Agentmaster: resolve the session's kind + state ONCE so this menu can mirror the WT tab's
        // right-click session ops (New Session Here / Restart session / Fork session — kind-aware:
        // Codex spawns/forks codex, not claude) and offer the Waiting-for-you triage "Move to Idle/Done".
        // Read at flyout-creation time — each _Refresh rebuilds the card so the menu tracks the latest
        // state; the click handlers re-resolve where it matters (the Move-to-Idle mutator re-checks under
        // the registry lock).
        std::optional<SessionInfo> info;
        if (_registry)
        {
            info = _registry->Get(id);
        }
        const bool isCodex = info && info->kind == AgentKind::Codex;
        const SessionState state = info ? info->state : SessionState::Idle;

        // A Segoe Fluent glyph icon for a menu item — so this menu reads like the WT tab's right-click
        // menu, which icons every item. The same glyphs the tab menu uses (Tab.cpp): Add \xE710,
        // RestartConnection \xE72C, Duplicate \xF5ED, Rename \xE8AC, Copy \xE8C8, Close \xE711.
        const auto glyphIcon = [](const wchar_t* glyph) {
            FontIcon fi;
            fi.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            fi.Glyph(glyph);
            return fi;
        };

        // All items defer one tick: a MenuFlyout restores focus to its target as it closes,
        // which would otherwise yank focus out of the freshly-shown rename editor / dialog (and the
        // spawn / tree rebuild for Open New Session Here).

        // Agentmaster (eager-init): Activate Tab — start a DORMANT session's claude IN PLACE (no focus
        // change), shown only when this session hasn't initialized yet (a window-restored / re-homed tab
        // the user never opened; SessionInfo::started == false) AND it is hosted in THIS window (a
        // single-session start can only target the window owning the control; a remote dormant session in
        // GLOBAL scope is woken via "Jump to Tab" or "Activate All Tabs"). FIRST item when present —
        // distinct from "Jump to Tab" (which switches to it + starts it as a side effect); this wakes it
        // where you are. Disappears once it starts.
        bool dormantLocal = false;
        if (info && IsSessionDormant(*info))
        {
            if (_localScopeProvider)
            {
                const auto localIds = _localScopeProvider(); // bind ONCE — find()/end() must be the same container
                dormantLocal = localIds.find(id) != localIds.end();
            }
            else
            {
                dormantLocal = true; // no provider wired => treat as local
            }
        }
        if (dormantLocal)
        {
            MenuFlyoutItem activate;
            activate.Text(L"Activate Tab");
            activate.Icon(glyphIcon(L"\xE768")); // Play — "start it"
            AgentSetTip(activate, L"Start this session's claude now, in place \x2014 it hasn't initialized yet (a restored tab you never opened). The view doesn't switch; use Jump to Tab for that.");
            activate.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, id]() { if (auto self = weak.get()) { if (self->_activateDormantHandler) { self->_activateDormantHandler(winrt::hstring{ id }); } } };
                if (disp)
                {
                    disp.TryEnqueue(act);
                }
                else
                {
                    act();
                }
            });
            menu.Items().Append(activate);
        }

        // Jump to Tab — Activate: switch to this session's live terminal tab (the page fans out to the
        // hosting WINDOW when the tab lives in another one). The menu twin of a double-click on the
        // card / tree row (and Enter on a tree row). First item — it's the most common action; a
        // separator sets the navigate action apart from the session-edit ops below.
        MenuFlyoutItem jump;
        jump.Text(L"Jump to Tab");
        jump.Icon(glyphIcon(L"\xE7B3")); // RedEye — matches the Flight Plan's "jump to the live tab" eye
        AgentSetTip(jump, L"Switch to this session's live terminal tab (jumps to its hosting window if it lives elsewhere)");
        jump.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { if (self->_activateHandler) { self->_activateHandler(winrt::hstring{ id }); } } });
            }
            else if (auto self = weak.get())
            {
                if (self->_activateHandler)
                {
                    self->_activateHandler(winrt::hstring{ id });
                }
            }
        });
        menu.Items().Append(jump);

        // Move to Idle/Done — Waiting-for-you triage only. The immediate, user-driven twin of the timed
        // WaitingForInput -> Idle decay (SessionScanner::_maybeDecayWaiting): demote this card so it
        // leaves the "Waiting-for-you" column for "Idle / Done". A deliberate state transition layered on
        // the hook-derived machine (the decay sets the SAME state the SAME way, so it sticks — the scanner
        // only re-promotes to Waiting/Running on NEW turn activity, never bouncing a quiescent Idle back).
        if (state == SessionState::WaitingForInput)
        {
            MenuFlyoutItem moveIdle;
            moveIdle.Text(L"Move to Idle/Done");
            moveIdle.Icon(glyphIcon(L"\xE73E")); // CheckMark — "I've handled this; stop waiting on me"
            AgentSetTip(moveIdle, L"Dismiss this \x201CWaiting-for-you\x201D card to the Idle / Done column \x2014 the manual version of the unread-timeout decay. It returns to Waiting-for-you on the session's next turn.");
            moveIdle.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, id]() {
                    auto self = weak.get();
                    if (!self || !self->_registry)
                    {
                        return;
                    }
                    // Re-check under the registry lock against the LIVE record: a new turn may have moved
                    // the state since the menu was built — never demote a session that is now Running.
                    // Stamp readUnixMs (mark it read) + clear manualUnread so a prior "Mark Unread" can't
                    // keep it pinned, mirroring the decay's read-gated semantics.
                    self->_registry->Update(id, [](SessionInfo& s) {
                        if (s.state == SessionState::WaitingForInput)
                        {
                            s.state = SessionState::Idle;
                            s.manualUnread = false;
                            s.readUnixMs = NowMs();
                        }
                    });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(moveIdle);
        }

        // Move to Waiting-for-you — Idle/Done triage only (the reverse of "Move to Idle/Done"): a PLAIN
        // promote of a quiescent card back into the "Waiting-for-you" column. EXPLICITLY separate from
        // "Mark Unread" (the tab strip's sticky/flashy promote) — this sets NO sticky manualUnread and
        // raises no red-ring flash; it just moves columns. Refresh the decay anchor + leave it unread so it
        // behaves like a fresh turn-complete (waits the full Waiting-for-you timeout, then decays normally)
        // instead of instantly decaying off an ancient lastActivity.
        if (info && (state == SessionState::Idle || state == SessionState::Done))
        {
            MenuFlyoutItem moveWaiting;
            moveWaiting.Text(L"Move to Waiting-for-you");
            moveWaiting.Icon(glyphIcon(L"\xE823")); // Clock — put it back in the Waiting-for-you column
            AgentSetTip(moveWaiting, L"Move this card into the \x201CWaiting-for-you\x201D column \x2014 a plain move (no red-ring flash; unlike \x201CMark Unread\x201D it decays normally). It returns to Idle / Done after the unread timeout.");
            moveWaiting.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, id]() {
                    auto self = weak.get();
                    if (!self || !self->_registry)
                    {
                        return;
                    }
                    // Re-check under the registry lock against the LIVE record: only promote a still-
                    // quiescent (Idle/Done) card — never bump a session that has since gone Running.
                    self->_registry->Update(id, [](SessionInfo& s) {
                        if (s.state == SessionState::Idle || s.state == SessionState::Done)
                        {
                            s.state = SessionState::WaitingForInput;
                            s.manualUnread = false; // plain move (NOT the sticky "Mark Unread")
                            s.lastActivityUnixMs = NowMs(); // restart the Waiting-for-you window (don't instant-decay)
                            s.readUnixMs = 0; // unread for this "turn", like a real turn-complete
                        }
                    });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(moveWaiting);
        }

        menu.Items().Append(MenuFlyoutSeparator{});

        MenuFlyoutItem rename;
        rename.Text(L"Rename (F2)");
        rename.Icon(glyphIcon(L"\xE8AC")); // Rename (matches the WT tab menu)
        // Advertise the in-place editor's commit keys + that they're configurable. Which key commits
        // (Enter vs Shift+Enter) follows the GLOBAL TabRenameCommitMode setting; the other inserts a
        // newline (titles can be multi-line), Esc cancels, and clicking away always commits.
        AgentSetTip(rename, L"Rename this session \x2014 its Explorer name and tab title.\nBy default, Shift+Enter commits the new name and Enter inserts a line break (swap them in Settings (\x2699) \x2192 \x201CTab rename: commit with\x201D).\nEsc cancels; clicking away always commits.");
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

        // Copy — a submenu mirroring the per-tab link badge's copy button (DESIGN §9.7 / TAB_OVERLAY.md).
        // Placed directly below Rename (at the user's request). It routes through the SAME shared
        // CopySessionField action the overlay's copy menu uses, so the two menus can never drift: Session
        // Id / Path / Branch / the REAL Claude & Codex launch CLIs / the full Summary box / the whole
        // Transcript. Each item is a pure clipboard write (cases 0-4) or an off-thread read that hops back
        // to copy (Transcript/Summary) — none mutate the tree, so unlike the rename/spawn/archive items
        // elsewhere in this menu they need no defer (matches the old single "Copy Session Id").
        MenuFlyoutSubItem copySub;
        copySub.Text(L"Copy");
        copySub.Icon(glyphIcon(L"\xE8C8")); // Copy (matches the WT tab menu's "Copy >")
        AgentSetTip(copySub, L"Copy this session's id, path, branch, launch command line, transcript, or full summary");
        const auto addCopyItem = [&copySub, weak, id](const wchar_t* text, const wchar_t* tip, int which) {
            MenuFlyoutItem item;
            item.Text(text);
            AgentSetTip(item, tip);
            item.Click([weak, id, which](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    if (self->_registry)
                    {
                        // The Summary case renders with this window's GLOBAL summary-panel flags, so a
                        // copied Summary matches what the panels show (wrap/truncate).
                        CopySessionField(*self->_registry, id, which, self->_dispatcher,
                                         self->_appSettings.summaryPanelWrapNewlines, self->_appSettings.summaryPanelTruncate);
                    }
                }
            });
            copySub.Items().Append(item);
        };
        addCopyItem(L"Session Id", L"Copy the resumable conversation id (Codex: its rollout uuid)", 0);
        addCopyItem(L"Copy Path", L"Copy the session's working-directory path", 1);
        addCopyItem(L"Copy Branch Name", L"Copy the session's current git branch name", 2);
        addCopyItem(L"Claude Launch CLI", L"Copy the full claude.exe launch command line (with --settings hooks and flags)", 3);
        addCopyItem(L"Codex Launch CLI", L"Copy the full codex launch command line", 4);
        addCopyItem(L"Summary", L"Copy the FULL session summary \x2014 the complete box (id, resume CLI, dir, folder, branch, duration, tasks, messages, files)", 6);
        addCopyItem(L"Transcript", L"Copy the whole conversation as text (your prompts + the agent's replies)", 5);
        menu.Items().Append(copySub);

        // The three WT-tab-menu session ops (mirrored here at the user's request): New Session Here /
        // Restart session / Fork session — kind-aware (a Codex card spawns + forks codex, a Claude card
        // claude). Uses the row's cwd captured at build time (a session's workingDir is fixed at launch).
        // A separator sets these session ops apart from the Rename / Copy items above.
        menu.Items().Append(MenuFlyoutSeparator{});

        // New Session Here — spawn a NEW, independent managed session in this row's working dir.
        MenuFlyoutItem openHere;
        openHere.Text(isCodex ? L"Open New Codex Session Here" : L"Open New Session Here");
        openHere.Icon(glyphIcon(L"\xE710")); // Add (matches the WT tab menu's "New Session Here")
        AgentSetTip(openHere, isCodex ? L"Launch a managed Codex session in this directory (a new, independent conversation)" : L"Launch a managed Claude session in this directory (a new, independent conversation)");
        openHere.Click([weak, disp, cwd, isCodex](const IInspectable&, const RoutedEventArgs&) {
            auto act = [weak, cwd, isCodex]() {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                if (isCodex)
                {
                    // Codex spawn (adopt=false, fork ignored) — no native-exe gate (Codex isn't exe-only;
                    // the launcher falls back to a bare `codex` token + surfaces any error).
                    if (self->_codexLaunchHandler)
                    {
                        self->_codexLaunchHandler(0, winrt::hstring{ cwd }, false, false);
                    }
                    return;
                }
                // Native-exe-only policy: spawning a managed Claude session needs a native claude.exe —
                // gate (re-resolve-then-prompt) so it surfaces the install modal instead of silently
                // no-op'ing at the engine backstop when Claude isn't installed.
                if (!self->_spawnHandler)
                {
                    return;
                }
                if (!::Agentmaster::EnsureClaudeAvailable())
                {
                    self->_ShowClaudeMissing();
                    return;
                }
                self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{});
            };
            if (disp) { disp.TryEnqueue(act); } else { act(); }
        });
        menu.Items().Append(openHere);

        // Restart session — rebuild THIS session's live ConPTY connection in place (the page resumes the
        // current conversation; never replays the launch commandline). Kind-agnostic at this seam: the
        // page's _RestartManagedSession handles both Claude (with its own claude.exe gate) and Codex.
        MenuFlyoutItem restart;
        restart.Text(L"Restart session");
        restart.Icon(glyphIcon(L"\xE72C")); // RestartConnection (matches the WT tab menu)
        AgentSetTip(restart, L"Restart this session \x2014 relaunch the agent and resume its current conversation in place (the terminal pane is reused).");
        restart.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { if (self->_restartSessionHandler) { self->_restartSessionHandler(winrt::hstring{ id }); } } });
            }
            else if (auto self = weak.get())
            {
                if (self->_restartSessionHandler)
                {
                    self->_restartSessionHandler(winrt::hstring{ id });
                }
            }
        });
        menu.Items().Append(restart);

        // Fork session — branch this conversation into a NEW, independent one (Claude: `--resume <id>
        // --fork-session`; Codex: `codex fork <rolloutUuid>`) opened in this window, the source untouched.
        // The page's _ForkManagedSessionById is the kind-aware fork shared with the WT tab's "Fork session".
        MenuFlyoutItem fork;
        fork.Text(L"Fork session");
        fork.Icon(glyphIcon(L"\xF5ED")); // Duplicate (matches the WT tab menu's "Fork session")
        AgentSetTip(fork, L"Fork this session \x2014 branch its conversation into a new, independent session (named \x201C\x2026 (fork)\x201D); the original is untouched.");
        fork.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { if (self->_forkManagedSessionHandler) { self->_forkManagedSessionHandler(winrt::hstring{ id }); } } });
            }
            else if (auto self = weak.get())
            {
                if (self->_forkManagedSessionHandler)
                {
                    self->_forkManagedSessionHandler(winrt::hstring{ id });
                }
            }
        });
        menu.Items().Append(fork);

        // Close — the LAST item, set apart by a separator and carrying the X glyph, exactly like the WT
        // tab's right-click menu (its terminal Close item, glyph \xE711). Close shuts the session down
        // (FAVORITES.md: the "Close" verb that replaced Archive/Delete) but KEEPS the record (always
        // archived) so it stays in Sessions, resumable anytime — the conversation on disk is never
        // deleted. Routed through _RequestArchive (the archive seam).
        menu.Items().Append(MenuFlyoutSeparator{});
        MenuFlyoutItem closeItem;
        closeItem.Text(L"Close");
        closeItem.Icon(glyphIcon(L"\xE711")); // Close (the X — matches the WT tab menu's Close)
        AgentSetTip(closeItem, L"Close this session \x2014 shut its tab down. It stays in Sessions and can be resumed anytime; star it there to keep it in your favorites (the conversation on disk is never deleted).");
        closeItem.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { self->_RequestArchive(id); } });
            }
            else if (auto self = weak.get())
            {
                self->_RequestArchive(id);
            }
        });
        menu.Items().Append(closeItem);

        return menu;
    }

    // Agentmaster: the Flight-Plan message right-click menu. Copy (this prompt's text) is offered on
    // EVERY row; the per-prompt queue ops (Move up / Move down / Delete) appear only on UPCOMING rows
    // (a sent/historical row can't be reordered or unqueued). The queue ops act on `promptId` (the
    // right-clicked row, selecting it first) and — like _MakeSessionMenu — defer one tick so the
    // closing flyout's focus restore doesn't race the list rebuild. (No session-level item here — the
    // whole-session verbs live on the board card / tree row menu, _MakeSessionMenu.)
    MenuFlyout AgentManagerContent::_MakePromptMenu(const std::wstring& promptId, bool upcoming)
    {
        MenuFlyout menu;
        auto disp = _dispatcher;
        auto weak = get_weak();

        // Copy this prompt's text to the clipboard — every row, sent or upcoming. Reads the live
        // queue at click time so it copies the current body; no list rebuild, so no defer needed.
        MenuFlyoutItem copyItem;
        copyItem.Text(L"Copy");
        AgentSetTip(copyItem, L"Copy this prompt's text to the clipboard.");
        copyItem.Click([weak, promptId](const IInspectable&, const RoutedEventArgs&) {
            auto self = weak.get();
            if (!self || !self->_registry || self->_selectedId.empty())
            {
                return;
            }
            std::wstring text;
            if (const auto s = self->_registry->Get(self->_selectedId))
            {
                for (const auto& p : s->queue)
                {
                    if (p.id == promptId)
                    {
                        text = p.text.empty() ? p.label : p.text;
                        break;
                    }
                }
            }
            if (!text.empty())
            {
                CopyTextToClipboard(text);
            }
        });
        menu.Items().Append(copyItem);

        if (upcoming)
        {
            menu.Items().Append(MenuFlyoutSeparator{}); // divide Copy from the queue ops

            MenuFlyoutItem up;
            up.Text(L"Move up");
            AgentSetTip(up, L"Move this queued prompt earlier in the send order.");
            up.Click([weak, disp, promptId](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, promptId]() { if (auto self = weak.get()) { self->_selectedPromptId = promptId; self->_OnMovePrompt(-1); } });
                }
                else if (auto self = weak.get())
                {
                    self->_selectedPromptId = promptId;
                    self->_OnMovePrompt(-1);
                }
            });
            menu.Items().Append(up);

            MenuFlyoutItem down;
            down.Text(L"Move down");
            AgentSetTip(down, L"Move this queued prompt later in the send order.");
            down.Click([weak, disp, promptId](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, promptId]() { if (auto self = weak.get()) { self->_selectedPromptId = promptId; self->_OnMovePrompt(1); } });
                }
                else if (auto self = weak.get())
                {
                    self->_selectedPromptId = promptId;
                    self->_OnMovePrompt(1);
                }
            });
            menu.Items().Append(down);

            MenuFlyoutItem del;
            del.Text(L"Delete");
            AgentSetTip(del, L"Remove this prompt from the queue \x2014 it won't be sent.");
            del.Click([weak, disp, promptId](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, promptId]() { if (auto self = weak.get()) { self->_selectedPromptId = promptId; self->_OnDeletePrompt(); } });
                }
                else if (auto self = weak.get())
                {
                    self->_selectedPromptId = promptId;
                    self->_OnDeletePrompt();
                }
            });
            menu.Items().Append(del);
        }

        return menu;
    }

    void AgentManagerContent::_OnRenameSession(const std::wstring& id)
    {
        if (id.empty())
        {
            return;
        }
        // Agentmaster: the rename editor is IN-PLACE in the Explorer Tree — it only exists once
        // _RebuildTree renders this session's row. Invoked from a Triage-Board card (the board
        // shows the WHOLE fleet) that row may not currently render: the tree sits in EXTERNAL
        // scope, the session is hosted by ANOTHER window while the scope is LOCAL, or its
        // directory group is collapsed. Make the row renderable first — widen the lens, never the
        // data — so the editor always appears. The tree/F2 path (whose row is already visible)
        // passes every check unchanged.
        if (_treeScope == TreeScope::External)
        {
            // _SetTreeScope handles the leaving-EXTERNAL selection cleanup + both toggle labels +
            // the lens push; skip its refresh — this method refreshes once at the end.
            _SetTreeScope(TreeScope::Local, /*refresh*/ false);
        }
        if (_treeScope == TreeScope::Local && _localScopeProvider)
        {
            const auto localIds = _localScopeProvider();
            if (localIds.find(id) == localIds.end())
            {
                _SetTreeScope(TreeScope::Global, /*refresh*/ false); // hosted by another window — its row only renders in GLOBAL
            }
        }
        if (_registry)
        {
            if (const auto s = _registry->Get(id))
            {
                // Un-collapse the session's directory group (PathEq-aware: the collapsed set keeps
                // the first-seen spelling, which can differ from workingDir by case/slashes).
                bool uncollapsed = false;
                for (auto it = _collapsedDirs.begin(); it != _collapsedDirs.end();)
                {
                    if (PathEq(*it, s->workingDir))
                    {
                        it = _collapsedDirs.erase(it);
                        uncollapsed = true;
                    }
                    else
                    {
                        ++it;
                    }
                }
                if (uncollapsed)
                {
                    _NotifyLensChanged(); // collapsed dirs are part of the per-window lens (M10)
                }
            }
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
    // Used for the Close confirm (_RequestArchive) and Reopen-windows.
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
            // Never let a confirm callback throw out of the XAML event handler: an escaped exception
            // would reach the app's unhandled-exception path (an assert/FailFast in Debug).
            try
            {
                if (cb)
                {
                    cb();
                }
            }
            CATCH_LOG();
        });
        try
        {
            dialog.ShowAsync();
        }
        catch (...)
        {
        }
    }

    // Agentmaster: a THREE-way buttons-only choice (Primary / Secondary / Cancel) — the XAML-Islands-safe
    // dialog with both an extra button and a second callback. Used by Adopt to offer "Fork a copy" (safe:
    // a new transcript/rollout) vs. "Resume anyway" (take over the same conversation). Primary is the
    // DEFAULT (the safe Fork choice); Close (Cancel) does nothing. Both callbacks are exception-guarded
    // like _Confirm so nothing escapes into the XAML handler.
    void AgentManagerContent::_ConfirmChoice(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, const winrt::hstring& secondary, std::function<void()> onPrimary, std::function<void()> onSecondary)
    {
        ContentDialog dialog;
        dialog.Title(winrt::box_value(title));
        dialog.Content(winrt::box_value(body));
        dialog.PrimaryButtonText(primary);
        dialog.SecondaryButtonText(secondary);
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary); // safe default = the primary (Fork) choice
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
        auto p = std::move(onPrimary);
        auto s = std::move(onSecondary);
        dialog.PrimaryButtonClick([p](const ContentDialog&, const ContentDialogButtonClickEventArgs&) {
            try
            {
                if (p)
                {
                    p();
                }
            }
            CATCH_LOG();
        });
        dialog.SecondaryButtonClick([s](const ContentDialog&, const ContentDialogButtonClickEventArgs&) {
            try
            {
                if (s)
                {
                    s();
                }
            }
            CATCH_LOG();
        });
        try
        {
            dialog.ShowAsync();
        }
        catch (...)
        {
        }
    }

    // ---- Archived-sessions overlay: REMOVED (FAVORITES.md) ------------------
    // The in-content archive overlay AND the full-window Archive page are gone — the Sessions
    // browser is the sole history view. Closing a session keeps it (always archived), resumable
    // from Sessions and marked by Favorite.

    // Agentmaster: keep-awake control (tri-mode). SetThreadExecutionState's ES_CONTINUOUS flag is per-thread
    // and persists for the life of the calling thread (or until reset) — this runs on the window's UI thread,
    // which lives as long as the window, so no timer/poll loop is needed (unlike stay-awake.ps1, which loops
    // only because its host PowerShell would otherwise exit). The flag is system-wide while ANY thread holds
    // it; per-window holds compose fine (the PC stays awake while any window holds it). On window close the UI
    // thread exits and the per-thread flag is auto-released — so no explicit teardown is needed.
    //
    // _CycleKeepAwake advances the user's selected MODE; _RefreshKeepAwakeHold maps the mode to a desired hold.
    void AgentManagerContent::_CycleKeepAwake()
    {
        switch (_keepAwakeMode)
        {
        case KeepAwakeMode::Off: _keepAwakeMode = KeepAwakeMode::Always; break;
        case KeepAwakeMode::Always: _keepAwakeMode = KeepAwakeMode::WhileRunning; break;
        case KeepAwakeMode::WhileRunning: _keepAwakeMode = KeepAwakeMode::Off; break;
        }
        // Re-evaluate the hold for the new mode immediately (queries the registry for WhileRunning) + repaint.
        _RefreshKeepAwakeHold(nullptr);
    }

    // Compute the DESIRED execution-state hold for the current mode and apply it on a transition only (so a
    // per-_Refresh call while nothing changed is a no-op OS-wise). WhileRunning holds iff a live session is
    // actively Running — counted across the whole fleet (the hold is machine-global, and the Manager shows the
    // whole fleet anyway), so the PC stays awake mid-turn but is allowed to sleep once every agent is at rest.
    // `sessions`, when non-null, is the snapshot _Refresh already fetched (avoids a second Snapshot() copy).
    void AgentManagerContent::_RefreshKeepAwakeHold(const std::vector<::Agentmaster::SessionInfo>* sessions)
    {
        bool desired = false;
        switch (_keepAwakeMode)
        {
        case KeepAwakeMode::Off:
            desired = false;
            break;
        case KeepAwakeMode::Always:
            desired = true;
            break;
        case KeepAwakeMode::WhileRunning:
        {
            const auto anyRunning = [](const std::vector<::Agentmaster::SessionInfo>& v) {
                for (const auto& s : v)
                {
                    if (s.live && s.state == ::Agentmaster::SessionState::Running)
                    {
                        return true;
                    }
                }
                return false;
            };
            if (sessions)
            {
                desired = anyRunning(*sessions);
            }
            else if (_registry)
            {
                desired = anyRunning(_registry->Snapshot());
            }
            break;
        }
        }

        if (desired != _keepAwakeHeld)
        {
            _keepAwakeHeld = desired;
            constexpr DWORD esContinuous = 0x80000000; // ES_CONTINUOUS
            constexpr DWORD esSystem = 0x00000001; // ES_SYSTEM_REQUIRED
            constexpr DWORD esDisplay = 0x00000002; // ES_DISPLAY_REQUIRED
            // Hold: continuous + system + display. Release: continuous alone clears the prior requirements.
            ::SetThreadExecutionState(_keepAwakeHeld ? (esContinuous | esSystem | esDisplay) : esContinuous);
        }
        _UpdateKeepAwakeButton();
    }

    void AgentManagerContent::_UpdateKeepAwakeButton()
    {
        if (!_keepAwakeBtn)
        {
            return;
        }
        // Repaint only on an actual (mode, held) transition — _RefreshKeepAwakeHold calls this on every
        // _Refresh, and rebuilding the content + resource overrides each tick would churn (and flicker).
        if (_keepAwakeRendered && _keepAwakeRenderedMode == _keepAwakeMode && _keepAwakeRenderedHeld == _keepAwakeHeld)
        {
            return;
        }
        _keepAwakeRendered = true;
        _keepAwakeRenderedMode = _keepAwakeMode;
        _keepAwakeRenderedHeld = _keepAwakeHeld;

        const wchar_t* glyph = L"\xE708"; // default (Off): QuietHours-ish moon
        const wchar_t* label = L"Keep Awake";
        switch (_keepAwakeMode)
        {
        case KeepAwakeMode::Off:
            glyph = L"\xE708";
            label = L"Keep Awake";
            break;
        case KeepAwakeMode::Always:
            glyph = L"\xEC46"; // PowerButton
            label = L"Awake On";
            break;
        case KeepAwakeMode::WhileRunning:
            glyph = L"\xE768"; // Play -> "while running"
            label = L"Running Awake";
            break;
        }

        auto content = StackPanel{};
        content.Orientation(Orientation::Horizontal);
        content.Spacing(5);
        FontIcon icon;
        icon.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
        icon.Glyph(glyph);
        icon.FontSize(12); // compact, matching the thinner actions-row buttons
        content.Children().Append(icon);
        content.Children().Append(Text(label, 11, false, 1.0));
        _keepAwakeBtn.Content(content);

        // Color: Off -> theme default; actively holding -> green; WhileRunning but idle (armed, not holding)
        // -> amber, so the user can tell at a glance whether the machine is being kept awake right now. The
        // colored states use PaintHoldButton so the fill (and white text) hold through hover/press instead of
        // reverting to the subtle theme hover brush — base/hover(lighter)/pressed(darker) per state.
        if (_keepAwakeMode == KeepAwakeMode::Off)
        {
            ClearHoldButton(_keepAwakeBtn);
        }
        else if (_keepAwakeHeld)
        {
            PaintHoldButton(_keepAwakeBtn, 0xFF2E7D32, 0xFF3C9A42, 0xFF21601F); // green = holding now
        }
        else
        {
            PaintHoldButton(_keepAwakeBtn, 0xFF8A6D1B, 0xFFA8851F, 0xFF6B5414); // amber = armed (WhileRunning), nothing running
        }
    }

    std::pair<int, int> AgentManagerContent::_DormantCounts() const
    {
        int thisWindow = 0, fleet = 0;
        if (!_registry)
        {
            return { 0, 0 };
        }
        std::unordered_set<std::wstring> localIds;
        const bool haveLocal = static_cast<bool>(_localScopeProvider);
        if (haveLocal)
        {
            localIds = _localScopeProvider();
        }
        for (const auto& s : _registry->Snapshot())
        {
            if (!IsSessionDormant(s))
            {
                continue;
            }
            ++fleet;
            if (!haveLocal || localIds.find(s.id) != localIds.end())
            {
                ++thisWindow;
            }
        }
        return { thisWindow, fleet };
    }

    // Agentmaster (responsive launch bar): stage-collapse the toolbar's top row to the live pane width.
    // Measures each piece's natural width (DesiredSize — DPI/theme-proof) and picks the LEAST-collapsed
    // stage that fits: (A) full label + comfortable box -> (B) drop "launch a"/"session in" -> (C) shrink
    // the cwd box toward its floor (buttons stay inline) -> (D) wrap the launch buttons onto their own line
    // (the box reclaims the freed width). Idempotent + cheap; safe to call on every resize and whenever a
    // button's content/visibility changes. No layout loop — nothing here resizes _root (the pane owns it).
    void AgentManagerContent::_ReflowLaunchBar()
    {
        if (!_launchBar || !_launchBtns || !_cwdBox || !_toolbarCol)
        {
            return;
        }
        const double W = _lastRootWidth > 0.0 ? _lastRootWidth : (_root ? _root.ActualWidth() : 0.0);
        if (W <= 1.0)
        {
            return; // not laid out yet — SizeChanged drives the first real reflow
        }

        // Natural width of an element (0 when collapsed). Measure with a large finite bound (NoWrap text /
        // non-wrapping buttons treat it like infinity), so DesiredSize is the content width regardless of
        // how the element is currently arranged (e.g. _launchBtns while it sits on the wrapped 2nd line).
        const auto desired = [](const FrameworkElement& el) -> double {
            if (!el || el.Visibility() == Visibility::Collapsed)
            {
                return 0.0;
            }
            el.Measure(winrt::Windows::Foundation::Size{ 100000.0f, 100000.0f });
            return el.DesiredSize().Width;
        };

        // The two collapsible words read 0 once hidden, so cache their last visible width — we need it to
        // decide when there's room to UN-collapse them again.
        double laW = desired(_launchAText);
        if (laW > 0.0) { _measLaunchA = laW; } else { laW = _measLaunchA; }
        double siW = desired(_sessionInText);
        if (siW > 0.0) { _measSessionIn = siW; } else { siW = _measSessionIn; }

        const double amW = desired(_agentmasterText);
        const double dashW = desired(_dashText);
        const double togW = desired(_launchAgentBtn);
        const double btnsW = desired(_launchBtns); // Launch + visible Fork/Reopen/Activate + their inner spacing

        constexpr double kGap = 8.0; // _launchBar.Spacing
        constexpr double kMargin = 24.0; // _toolbarCol Margin L(12)+R(12)
        constexpr double kSlack = 12.0; // right-edge breathing room (+ a touch of hysteresis)
        constexpr double kFillSlack = 6.0; // wrapped: how far short of the content's padded end the FILLED box stops
        constexpr double kBoxPref = 504.0; // comfortable box width when there's room
        constexpr double kBoxFloor = 240.0; // smallest inline box (still shows the placeholder / a path tail)
        constexpr double kBoxHardMin = 160.0; // smallest box once the buttons have wrapped away

        const double avail = W - kMargin;

        // Row total = sum(child widths) + (childCount-1)*Spacing. Fixed (non-box) parts per config:
        //   Full inline    : [AM][dash][launchA][toggle][sessionIn] <box> [btns]  -> 6 gaps
        //   Collapsed inline: [AM][dash][toggle] <box> [btns]                     -> 4 gaps
        //   Wrapped (row 1): [AM][dash][toggle] <box FILLS ->|                     -> 3 gaps (btns on row 2)
        const double fullFixed = amW + dashW + laW + togW + siW + btnsW + 6.0 * kGap + kSlack;
        const double collapsedFixed = amW + dashW + togW + btnsW + 4.0 * kGap + kSlack;

        bool labelCollapsed = true;
        bool wrap = false;
        double boxMin = kBoxFloor; // non-wrap: content-grow bounds (the box sizes to its text between these)
        double boxMax = kBoxFloor;
        double fillW = 0.0; // wrap only: the explicit width that fills row 1 to the padded end

        if (avail - fullFixed >= kBoxPref)
        {
            // (A) Full label, comfortable box, buttons inline.
            labelCollapsed = false;
            boxMin = kBoxPref;
            boxMax = avail - fullFixed;
        }
        else if (avail - collapsedFixed >= kBoxPref)
        {
            // (B) Collapse the words; box still comfortable.
            boxMin = kBoxPref;
            boxMax = avail - collapsedFixed;
        }
        else if (avail - collapsedFixed >= kBoxFloor)
        {
            // (C) Shrink the box (floor..pref) but keep the buttons inline & visible.
            boxMin = kBoxFloor;
            boxMax = avail - collapsedFixed;
        }
        else
        {
            // (D) Wrap the buttons onto their own line. Nothing follows the box on row 1 now, so it FILLS
            // the row to the padded end (an explicit width — a horizontal StackPanel won't stretch a child
            // along its axis, so the apply below pins Min==Max==fillW) rather than sit content-sized with a gap.
            wrap = true;
            const double rowPrefix = amW + dashW + togW + 3.0 * kGap; // [AM][dash][toggle] + the 3 gaps before the box
            fillW = avail - rowPrefix - kFillSlack;
            if (fillW < kBoxHardMin)
            {
                fillW = kBoxHardMin; // pathologically narrow pane: floor it (may clip) rather than go invisible
            }
        }

        if (boxMax < boxMin)
        {
            boxMax = boxMin;
        }

        // Apply: label visibility (B+ hides the two words), the box width, then the wrap state (move
        // _launchBtns to / from its own line). Inline (A/B/C) the box is content-sized between Min/Max;
        // wrapped (D) it's PINNED (Min==Max==fillW) so it stretches to fill row 1 to the padded end.
        const auto vis = labelCollapsed ? Visibility::Collapsed : Visibility::Visible;
        if (_launchAText && _launchAText.Visibility() != vis) { _launchAText.Visibility(vis); }
        if (_sessionInText && _sessionInText.Visibility() != vis) { _sessionInText.Visibility(vis); }
        if (wrap)
        {
            _cwdBox.MinWidth(fillW);
            _cwdBox.MaxWidth(fillW);
        }
        else
        {
            _cwdBox.MinWidth(boxMin);
            _cwdBox.MaxWidth(boxMax);
        }

        if (wrap != _launchBtnsWrapped)
        {
            if (wrap)
            {
                uint32_t idx = 0;
                if (_launchBar.Children().IndexOf(_launchBtns, idx))
                {
                    _launchBar.Children().RemoveAt(idx);
                }
                _launchBtns.HorizontalAlignment(HorizontalAlignment::Left);
                _launchBtns.VerticalAlignment(VerticalAlignment::Center);
                // -2 top margin tightens _toolbarCol's 6px row Spacing to a 4px gap below the title row
                // (without changing the shared Spacing, which also sets the gap above the actions row).
                _launchBtns.Margin(Thickness{ 0, -2, 0, 0 });
                _toolbarCol.Children().InsertAt(1, _launchBtns); // directly below the title row, above the actions row
            }
            else
            {
                uint32_t idx = 0;
                if (_toolbarCol.Children().IndexOf(_launchBtns, idx))
                {
                    _toolbarCol.Children().RemoveAt(idx);
                }
                _launchBtns.Margin(Thickness{ 0, 0, 0, 0 });
                _launchBtns.HorizontalAlignment(HorizontalAlignment::Left);
                _launchBtns.VerticalAlignment(VerticalAlignment::Center);
                _launchBar.Children().Append(_launchBtns); // back inline at the end of the title row
            }
            _launchBtnsWrapped = wrap;
        }
    }

    void AgentManagerContent::_UpdateActivateAllButton()
    {
        if (!_activateAllBtn)
        {
            return;
        }
        // Headline = THIS window's dormant count (the default, no-prompt action scope). The button hides
        // at 0; cross-window escalation is offered by the click prompt only when other windows also have
        // dormant tabs. Reads SessionInfo::started (maintained by the page from ConnectionState()).
        const auto [thisWindow, fleet] = _DormantCounts();
        (void)fleet;
        _activateAllBtn.Content(winrt::box_value(winrt::hstring{ L"Activate All Tabs (" } + winrt::to_hstring(thisWindow) + L")"));
        _activateAllBtn.Visibility(thisWindow > 0 ? Visibility::Visible : Visibility::Collapsed);
        _ReflowLaunchBar(); // the button just appeared/vanished/relabeled -> re-fit the row
    }

    void AgentManagerContent::_OnActivateAllTabs()
    {
        const auto [thisWindow, fleet] = _DormantCounts();
        if (thisWindow <= 0 && fleet <= 0)
        {
            return; // nothing dormant anywhere
        }
        const int other = fleet - thisWindow;
        if (other <= 0)
        {
            // Only this window has dormant tabs (or it's the only window) -> just wake them, no prompt.
            if (_activateAllHandler)
            {
                _activateAllHandler(false);
            }
            return;
        }
        // Other windows ALSO have dormant tabs -> let the user choose the scope (the user's design:
        // "prompt for choice; if 1 window then automatically just that window").
        _ConfirmChoice(
            L"Activate dormant tabs",
            winrt::hstring{ std::wstring{ L"Start the Claude sessions that haven't initialized yet (restored tabs you haven't opened).\n\nThis window: " } + std::to_wstring(thisWindow) + L"   \x2022   All windows: " + std::to_wstring(fleet) + L"." },
            winrt::hstring{ std::wstring{ L"This window (" } + std::to_wstring(thisWindow) + L")" },
            winrt::hstring{ std::wstring{ L"All windows (" } + std::to_wstring(fleet) + L")" },
            [weak = get_weak()]() { if (auto self = weak.get()) { if (self->_activateAllHandler) { self->_activateAllHandler(false); } } },
            [weak = get_weak()]() { if (auto self = weak.get()) { if (self->_activateAllHandler) { self->_activateAllHandler(true); } } });
    }

    void AgentManagerContent::_UpdateReopenButton()
    {
        if (!_reopenBtn)
        {
            return;
        }
        // Recoverable == saved window records NOT currently open in this process (Engine-tracked).
        // Reads windows/*.json off disk, so keep this on the _Refresh cadence (registry changes), not
        // a hot path. Show the button only when there is something to recover.
        const auto n = static_cast<int>(::Agentmaster::RecoverableWindows().size());
        _reopenBtn.Content(winrt::box_value(winrt::hstring{ L"Reopen Windows (" } + winrt::to_hstring(n) + L")"));
        _reopenBtn.Visibility(n > 0 ? Visibility::Visible : Visibility::Collapsed);
        _ReflowLaunchBar(); // the button just appeared/vanished/relabeled -> re-fit the row
    }

    void AgentManagerContent::_OnReopenWindows()
    {
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] button clicked\n");
        if (!_reopenWindowsHandler)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] no handler bound\n");
            return;
        }
        int n = 0;
        try
        {
            n = static_cast<int>(::Agentmaster::RecoverableWindows().size());
        }
        CATCH_LOG();
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] recoverable N=" + std::to_wstring(n) + L"\n");
        if (n <= 0)
        {
            return;
        }
        const auto reopen = _reopenWindowsHandler;
        _Confirm(L"Reopen saved windows?",
                 winrt::hstring{ L"This reopens " } + winrt::to_hstring(n) + L" previously-saved window(s) at their saved position and layout. Their sessions stay archived until you restore them.",
                 L"Reopen",
                 [reopen]() {
                     ::Agentmaster::AppendStateLog(L"hooks.log", L"[reopen] confirm accepted -> handler\n");
                     if (reopen)
                     {
                         reopen();
                     }
                 });
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

        // Agentmaster: build identity line at the very top — the release VERSION (read live from the
        // package manifest; the release pipeline stamps Package-Rel.appxmanifest, a dev loose layout
        // shows its own manifest version), the git COMMIT this build was made from (build-stamped via
        // AgentmasterBuildInfo.g.h), and whether this is the RELEASE or DEV install (package family:
        // Agentmaster vs AgentmasterDev) plus the compile CONFIGURATION (Debug/Release). Full detail
        // (branch, package family name) rides a hover tooltip.
        {
            std::wstring version{ L"?" };
            try
            {
                version = std::wstring{ CascadiaSettings::ApplicationVersion() };
            }
            CATCH_LOG();

            const std::wstring pfn = ::Agentmaster::Profiles::PackageFamilyName();
            const std::wstring channel = pfn.empty()                                 ? std::wstring{ L"Unpackaged" } :
                                         ::Agentmaster::Profiles::IsDevPackage()     ? std::wstring{ L"Dev" } :
                                                                                       std::wstring{ L"Release" };
#if defined(_DEBUG)
            const std::wstring config{ L"Debug" };
#else
            const std::wstring config{ L"Release" };
#endif
            const std::wstring commit{ AGENTMASTER_COMMIT_HASH };
            const std::wstring branch{ AGENTMASTER_COMMIT_BRANCH };

            // e.g. "v0.0.1.0  ·  3c4e2a942  ·  Dev · Debug"  (· = U+00B7, always trailed by a space
            // so the \x00B7 hex escape can't swallow a following hex digit).
            const std::wstring line = L"v" + version + L"  \x00B7  " + commit + L"  \x00B7  " + channel + L" \x00B7 " + config;
            auto sub = Text(winrt::hstring{ line }, 12, false, 0.6);

            std::wstring tip = L"Version " + version + L"\nCommit " + commit + L" (" + branch + L")\nChannel " + channel;
            if (!pfn.empty())
            {
                tip += L" (" + pfn + L")";
            }
            tip += L"\nConfiguration " + config;
            AgentSetTip(sub, winrt::hstring{ tip });
            panel.Children().Append(sub);
        }

        // UPDATES (Agentmaster updater; Updater.h) — check GitHub Releases for a newer version
        // (the same prompt the startup check shows) + the pre-release opt-in. The status label
        // beside the button shows "vX.Y.Z available!" in dark green when a newer release exists
        // (filled by a silent check kicked when the cog opens — see _ShowSettings/_CheckForUpdates).
        panel.Children().Append(Text(L"UPDATES", 11, true, 0.6));
        {
            auto row = StackPanel{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(8);
            row.VerticalAlignment(VerticalAlignment::Center);
            _setCheckUpdates = Button{};
            _setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
            AgentSetTip(_setCheckUpdates, L"Check GitHub for a newer Agentmaster release, then choose to update now, postpone (3 / 7 / 30 days), or skip this version.");
            _setCheckUpdates.Click([this](const IInspectable&, const RoutedEventArgs&) { _CheckForUpdates(true); });
            row.Children().Append(_setCheckUpdates);

            // "Current version changelog" — opens THIS build's GitHub release page (the changelog fixated
            // on the installed version). Always shown; the URL is fixed for the process lifetime. Opened
            // off-thread (a browser launch can stall) like the per-tab overlay's Open-Path.
            const std::wstring curChangelogUrl =
                ::Agentmaster::Updater::ReleasePageForTag(::Agentmaster::Updater::VersionToString(::Agentmaster::Updater::CurrentPackageVersion()));
            _setCurrentChangelog = HyperlinkButton{};
            _setCurrentChangelog.Content(winrt::box_value(L"Current version changelog"));
            _setCurrentChangelog.Padding(Thickness{ 4, 2, 4, 2 });
            _setCurrentChangelog.FontSize(12);
            AgentSetTip(_setCurrentChangelog, L"Open the GitHub release notes for the version you're running");
            _setCurrentChangelog.Click([curChangelogUrl](const IInspectable&, const RoutedEventArgs&) {
                if (!curChangelogUrl.empty())
                {
                    std::thread([curChangelogUrl]() { ::ShellExecuteW(nullptr, L"open", curChangelogUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }).detach();
                }
            });
            row.Children().Append(_setCurrentChangelog);

            // "Update's changelog" — opens the AVAILABLE update's release page; revealed only after a
            // check finds one (_lastUpdateChangelogUrl + Visibility set in _CheckForUpdates' completion).
            _setUpdateChangelog = HyperlinkButton{};
            _setUpdateChangelog.Content(winrt::box_value(L"Update's changelog"));
            _setUpdateChangelog.Padding(Thickness{ 4, 2, 4, 2 });
            _setUpdateChangelog.FontSize(12);
            _setUpdateChangelog.Visibility(Visibility::Collapsed);
            AgentSetTip(_setUpdateChangelog, L"Open the GitHub release notes for the available update");
            _setUpdateChangelog.Click([this](const IInspectable&, const RoutedEventArgs&) {
                const std::wstring u = _lastUpdateChangelogUrl;
                if (!u.empty())
                {
                    std::thread([u]() { ::ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }).detach();
                }
            });
            row.Children().Append(_setUpdateChangelog);
            panel.Children().Append(row);
        }
        // Status label ("vX.Y.Z available!" dark green / "up to date" / "Checking…") on its OWN row
        // beneath the action row — kept off the action row so the button + the two changelog links fit
        // the 460-wide card without clipping (the cog's ScrollViewer scrolls vertically only).
        _setUpdateStatus = TextBlock{};
        _setUpdateStatus.Opacity(0.9);
        _setUpdateStatus.FontSize(12);
        _setUpdateStatus.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(_setUpdateStatus);

        _setAllowPrerelease = ToggleSwitch{};
        _setAllowPrerelease.Header(winrt::box_value(L"Allow updating to pre-release versions"));
        AgentSetTip(_setAllowPrerelease, L"When on, update checks also consider GitHub pre-releases (beta builds), not just stable releases. Off by default.");
        panel.Children().Append(_setAllowPrerelease);

        // "Uninstall Agentmaster…" — removes THIS install (the current package family) via the same
        // embedded am-update.ps1 (-Uninstall). Shown only for packaged installs (gated in _ShowSettings);
        // per-user, no admin, and the profile data (~/.agentmaster) is kept. Confirms, then quits so the
        // package isn't in use while it's removed.
        _setUninstallBtn = Button{};
        _setUninstallBtn.Content(winrt::box_value(L"Uninstall Agentmaster\x2026"));
        _setUninstallBtn.Margin(Thickness{ 0, 10, 0, 0 });
        AgentSetTip(_setUninstallBtn, L"Remove this Agentmaster install. Your data (sessions, settings, e.g. %USERPROFILE%\\.agentmaster) is kept. Agentmaster closes to finish.");
        _setUninstallBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            _Confirm(L"Uninstall Agentmaster?",
                     L"This removes the installed Agentmaster package. Your data (sessions, settings, e.g. %USERPROFILE%\\.agentmaster) is kept. Agentmaster will close to finish uninstalling.",
                     L"Uninstall",
                     [this]() {
                         const std::wstring stateDir = ::Agentmaster::Profiles::ResolveProfileDir();
                         if (::Agentmaster::Updater::LaunchUninstaller(stateDir) && _quitForUpdateHandler)
                         {
                             _quitForUpdateHandler();
                         }
                     });
        });
        panel.Children().Append(_setUninstallBtn);

        // CLAUDE SESSIONS
        panel.Children().Append(Text(L"CLAUDE SESSIONS", 11, true, 0.6));
        _setSkipPermissions = ToggleSwitch{};
        _setSkipPermissions.Header(winrt::box_value(L"Skip permission prompts (bypass)"));
        AgentSetTip(_setSkipPermissions, L"Launch new sessions with --dangerously-skip-permissions \x2014 auto-accepts tool prompts and the per-folder trust dialog so an unattended session never wedges. Off pins normal prompts instead.");
        panel.Children().Append(_setSkipPermissions);
        _setModel = TextBox{};
        _setModel.Header(winrt::box_value(L"Model"));
        _setModel.PlaceholderText(L"As Is \x2014 blank keeps Claude's default (e.g. opus / sonnet)");
        AgentSetTip(_setModel, L"Model new sessions launch with (like /model) \x2014 e.g. opus or sonnet. Blank keeps Claude's own default.");
        panel.Children().Append(_setModel);
        _setIncludeCoAuthored = ToggleSwitch{};
        _setIncludeCoAuthored.Header(winrt::box_value(L"Include co-authored-by in commits"));
        AgentSetTip(_setIncludeCoAuthored, L"When off, commits Claude makes omit the \x201C" L"Co-authored-by\x201D trailer. Applies to new sessions.");
        panel.Children().Append(_setIncludeCoAuthored);
        // ENV_VARS.md: the two-tab "Environment variables" area (Global / Per-directory) replaces the old
        // single ;-delimited box. Built in its own method to keep this builder readable + the diff local.
        _BuildEnvVarsArea(panel);

        // CLAUDE HISTORY (ENV_VARS.md §8): cleanupPeriodDays lives in the user's GLOBAL ~/.claude/settings.json
        // (NOT an Agentmaster setting, NOT an env var). Read on open / written on save through the
        // ClaudeUserSettings repository (a managed layer that preserves every other key in that file). 36500
        // (~100y) ships by default so Claude never purges global history; blank removes our key (Claude's
        // 30-day default). A separate field from the env area on purpose — it's a Claude settings key, not env.
        panel.Children().Append(Text(L"CLAUDE HISTORY", 11, true, 0.6));
        _setCleanupDays = TextBox{};
        _setCleanupDays.Header(winrt::box_value(L"Keep Claude history (days)"));
        _setCleanupDays.PlaceholderText(L"e.g. 36500 (~never) \x2014 blank = Claude default (30 days)");
        AgentSetTip(_setCleanupDays, L"Sets cleanupPeriodDays in your global ~/.claude/settings.json \x2014 how many days Claude keeps session transcripts before deleting them at startup (your GLOBAL Claude history, used by Resume + the Sessions browser). 36500 \x2248 never. Blank removes the key (Claude's 30-day default). Avoid 0: in Claude it DISABLES history entirely.");
        panel.Children().Append(_setCleanupDays);

        // CLAUDE BINARY (native-exe-only policy): the auto-detected native claude.exe + an optional
        // explicit override. The whole app gates launch/fork/resume on resolving one (ResolveClaudeExe);
        // the override must be a real *.exe (a .cmd/.bat or the Node CLI is rejected).
        panel.Children().Append(Text(L"CLAUDE BINARY (native build required)", 11, true, 0.6));
        _setClaudeDetected = TextBlock{};
        _setClaudeDetected.TextWrapping(TextWrapping::Wrap);
        _setClaudeDetected.Opacity(0.85);
        _setClaudeDetected.FontSize(12);
        AgentSetTip(_setClaudeDetected, L"The native claude.exe Agentmaster resolved (PATH \xB7 %USERPROFILE%\\.local\\bin \xB7 behind an npm claude.cmd). The fleet view drives this binary, so a pure-Node Claude is unsupported \x2014 launch / resume / fork stay disabled until one is found.");
        panel.Children().Append(_setClaudeDetected);
        _setClaudeExePath = TextBox{};
        _setClaudeExePath.Header(winrt::box_value(L"Override claude.exe path"));
        _setClaudeExePath.PlaceholderText(L"blank \x2014 auto-detect; or a full path to claude.exe");
        AgentSetTip(_setClaudeExePath, L"Force a specific claude.exe instead of auto-detecting \x2014 must be a real *.exe (a .cmd / .bat or the Node CLI is rejected). Blank auto-detects.");
        panel.Children().Append(_setClaudeExePath);
        {
            auto browse = Button{};
            browse.Content(winrt::box_value(L"Browse for claude.exe\x2026"));
            AgentSetTip(browse, L"Pick claude.exe with a file dialog \x2014 sets the override above and re-resolves the binary immediately, no restart.");
            browse.Click([this](const IInspectable&, const RoutedEventArgs&) { _BrowseForClaudeExe(true); });
            panel.Children().Append(browse);
        }

        // AUTOPILOT
        panel.Children().Append(Text(L"AUTOPILOT (defaults for new sessions)", 11, true, 0.6));
        _setDefaultMode = ComboBox{};
        _setDefaultMode.Header(winrt::box_value(L"New-session mode"));
        _setDefaultMode.Items().Append(winrt::box_value(L"Off"));
        _setDefaultMode.Items().Append(winrt::box_value(L"SemiAuto"));
        _setDefaultMode.Items().Append(winrt::box_value(L"Full"));
        AgentSetTip(_setDefaultMode, L"Autopilot mode each new session starts in \x2014 Off (manual) \xB7 SemiAuto (you confirm each send) \xB7 Full (auto-send the queue on turn-complete). Per-session, changeable from the Flight Plan.");
        panel.Children().Append(_setDefaultMode);
        _setMaxAutoSends = TextBox{};
        _setMaxAutoSends.Header(winrt::box_value(L"Max auto-sends per run"));
        _setMaxAutoSends.PlaceholderText(L"100");
        AgentSetTip(_setMaxAutoSends, L"Backstop cap on how many prompts Autopilot may auto-send in one run before stopping. Blank or 0 resets to 100.");
        panel.Children().Append(_setMaxAutoSends);
        _setStopOnError = ToggleSwitch{};
        _setStopOnError.Header(winrt::box_value(L"Stop on error"));
        AgentSetTip(_setStopOnError, L"When on, Autopilot halts a session's queue as soon as it enters the Error state instead of sending the next prompt.");
        panel.Children().Append(_setStopOnError);
        _setPauseOnHuman = ToggleSwitch{};
        _setPauseOnHuman.Header(winrt::box_value(L"Pause on human input"));
        AgentSetTip(_setPauseOnHuman, L"When on, typing into a session's terminal yourself pauses its Autopilot so a manual interruption isn't overwritten by the next queued send.");
        panel.Children().Append(_setPauseOnHuman);

        // BEHAVIOR
        panel.Children().Append(Text(L"BEHAVIOR", 11, true, 0.6));
        _setConfirmKill = ToggleSwitch{};
        _setConfirmKill.Header(winrt::box_value(L"Confirm before closing a session"));
        AgentSetTip(_setConfirmKill, L"When on, closing a session (tab X, the tree's Del, or the Close menu) first asks to confirm. Off closes without the prompt. Closing always keeps the session in Sessions, resumable \x2014 nothing on disk is deleted either way.");
        panel.Children().Append(_setConfirmKill);
        // How the tab/session rename box commits via the keyboard. Clicking away (focus loss) ALWAYS
        // commits; this only governs the Enter / Shift+Enter shortcut. The box is multi-line, so the
        // key that ISN'T the commit key inserts a newline. GLOBAL across windows (TabRenameCommitMode).
        _setRenameCommit = ComboBox{};
        _setRenameCommit.Header(winrt::box_value(L"Tab rename: commit with"));
        _setRenameCommit.Items().Append(winrt::box_value(L"Click away only"));
        _setRenameCommit.Items().Append(winrt::box_value(L"Click away + Shift+Enter"));
        _setRenameCommit.Items().Append(winrt::box_value(L"Click away + Enter"));
        AgentSetTip(_setRenameCommit, L"Which key commits a tab rename: Shift+Enter (default) or Enter. The other inserts a line break (titles can be multi-line); clicking away always commits.");
        panel.Children().Append(_setRenameCommit);
        // Agentmaster (Waiting-for-you "unread" model): the Waiting-for-you -> Idle timeout. A "Never"
        // toggle (stay Waiting until read), else a slider 1m .. 3d (default 1h). This is the "unread
        // inbox" lifetime; it was split from Claude's ~5-min server cache, which is now the separate
        // "Server-side cache lifetime" below (driving only the card's ⚡ hint).
        _setWaitingNever = ToggleSwitch{};
        _setWaitingNever.Header(winrt::box_value(L"Never decay Waiting-for-you (keep until read)"));
        AgentSetTip(_setWaitingNever, L"When on, a Waiting-for-you session never auto-demotes to Idle by time \x2014 it stays until you read (visit) its tab. When off, it decays after the timeout below (and only once you've read it).");
        _setWaitingNever.Toggled([this](const IInspectable&, const RoutedEventArgs&) {
            if (_setWaitingDecaySlider && _setWaitingNever)
            {
                _setWaitingDecaySlider.IsEnabled(!_setWaitingNever.IsOn());
            }
        });
        panel.Children().Append(_setWaitingNever);

        _setWaitingDecaySlider = Slider{};
        _setWaitingDecaySlider.Minimum(1); // 1 minute
        _setWaitingDecaySlider.Maximum(4320); // 3 days
        _setWaitingDecaySlider.StepFrequency(1);
        _setWaitingDecaySlider.Header(winrt::box_value(L"Waiting-for-you \x2192 Idle after"));
        AgentSetTip(_setWaitingDecaySlider, L"How long a Waiting-for-you session waits before it may demote to Idle \x2014 1 minute \x2026 3 days. It only demotes once you've READ it (an unread session keeps waiting past the timeout). Use the toggle above for \x201Cnever\x201D.");
        _setWaitingDecaySlider.ValueChanged([this](const IInspectable&, const Primitives::RangeBaseValueChangedEventArgs&) {
            if (_setWaitingDecaySlider)
            {
                _setWaitingDecaySlider.Header(winrt::box_value(winrt::hstring{ L"Waiting-for-you \x2192 Idle after: " } + winrt::hstring{ FormatMinutesFriendly(static_cast<uint32_t>(_setWaitingDecaySlider.Value())) }));
            }
        });
        panel.Children().Append(_setWaitingDecaySlider);

        _setServerCache = TextBox{};
        _setServerCache.Header(winrt::box_value(L"Server-side cache lifetime (minutes)"));
        _setServerCache.PlaceholderText(L"5");
        AgentSetTip(_setServerCache, L"How long after a turn Claude's server-side prompt cache stays warm \x2014 drives the card's \x26A1 \x201Cstill cached\x201D hint (a follow-up within the window is cheaper & faster). Default 5.");
        panel.Children().Append(_setServerCache);
        _setLaunchDir = TextBox{};
        _setLaunchDir.Header(winrt::box_value(L"Default Launch directory"));
        _setLaunchDir.PlaceholderText(L"blank \x2014 defaults to %USERPROFILE%");
        AgentSetTip(_setLaunchDir, L"Directory the Launch box is pre-filled with. Blank defaults to %USERPROFILE%.");
        panel.Children().Append(_setLaunchDir);
        _setRecentDirsLimit = TextBox{};
        _setRecentDirsLimit.Header(winrt::box_value(L"Recent Launch directories to remember"));
        _setRecentDirsLimit.PlaceholderText(L"10");
        AgentSetTip(_setRecentDirsLimit, L"How many recently-used directories the Launch box's path-picker keeps in its history. Blank or 0 resets to 10.");
        panel.Children().Append(_setRecentDirsLimit);

        // Sessions browser: un-hide every session removed via the Sessions page's right-click
        // "Hide from list". The list lives in AppSettings.hiddenSessionIds, owned by the page
        // (TerminalPage), so this fires the handler there rather than reading a count the cog
        // doesn't track; the button gives inline confirmation. Re-enabled/relabeled per _ShowSettings.
        _setResetHidden = Button{};
        _setResetHidden.Content(winrt::box_value(L"Reset hidden sessions"));
        AgentSetTip(_setResetHidden, L"Un-hide every session you removed from the Sessions browser with \x201CHide from list\x201D");
        _setResetHidden.Click([this](const IInspectable& sender, const RoutedEventArgs&) {
            ::Agentmaster::LogNav(L"reset-hidden-sessions (un-hide every Sessions-browser row)"); // Nav audit: the cog "Reset hidden sessions"
            if (_resetHiddenSessionsHandler)
            {
                _resetHiddenSessionsHandler();
            }
            // The list lives in TerminalPage; confirm locally (the cog doesn't track the count).
            if (const auto b = sender.try_as<Button>())
            {
                b.Content(winrt::box_value(L"Hidden sessions cleared"));
                b.IsEnabled(false);
            }
        });
        panel.Children().Append(_setResetHidden);

        // TABS — close affordances on the terminal tab strip (GLOBAL across windows, applied live
        // on Save via TerminalPage::_updateAllTabCloseButtons + the cross-window broadcast).
        panel.Children().Append(Text(L"TABS", 11, true, 0.6));
        _setShowTabCloseButton = ToggleSwitch{};
        _setShowTabCloseButton.Header(winrt::box_value(L"Show close (\x00D7) button on tabs"));
        AgentSetTip(_setShowTabCloseButton, L"When off, the close (\x00D7) button is hidden on every tab (you can still close with the tab's right-click menu, the middle-mouse button below, or Ctrl+Shift+W). The pinned Manager tab is always X-less. Default on.");
        panel.Children().Append(_setShowTabCloseButton);
        _setCloseTabOnMiddleClick = ToggleSwitch{};
        _setCloseTabOnMiddleClick.Header(winrt::box_value(L"Close tab with middle-mouse click"));
        AgentSetTip(_setCloseTabOnMiddleClick, L"When off, middle-clicking a tab no longer closes it \x2014 handy if you keep closing tabs by accident. Default on.");
        panel.Children().Append(_setCloseTabOnMiddleClick);
        _setAlwaysShowHomeButton = ToggleSwitch{};
        _setAlwaysShowHomeButton.Header(winrt::box_value(L"Always display Home button"));
        AgentSetTip(_setAlwaysShowHomeButton, L"Keep the tab-strip \x201CHome\x201D button (jump to the pinned Agent Manager tab) visible whenever you're on another tab. When off, it appears only once the Manager tab has scrolled out of view. Default on.");
        panel.Children().Append(_setAlwaysShowHomeButton);
        // FAVORITES.md §5a: which glyph marks a FAVORITE (starred) session on its live tab — Crown
        // (default, a gold crown at the status dot's NW) or Star (the status dot foregrounded on a white,
        // golden-tipped star). GLOBAL across windows; applied live on Save + cross-window broadcast.
        _setFavoriteIcon = ComboBox{};
        _setFavoriteIcon.Header(winrt::box_value(L"Favorite marker"));
        _setFavoriteIcon.Items().Append(winrt::box_value(L"Crown")); // index 0 == FavoriteIcon::Crown (default)
        _setFavoriteIcon.Items().Append(winrt::box_value(L"Star")); // index 1 == FavoriteIcon::Star
        AgentSetTip(_setFavoriteIcon, L"The marker shown on a favorited (\x2605) session's live tab, over its status dot \x2014 Crown (default, a small gold crown at the dot's corner) or Star (the status dot becomes the centre of a white, golden-tipped star).");
        panel.Children().Append(_setFavoriteIcon);

        // TABS: the "status flashing color" — color (and OPACITY) of the unread FLASH RING that pulses
        // around a managed session's tab status dot when it leaves Running for a needs-you state on an
        // unvisited tab (and the manual "Mark Unread" ring). To keep the cog SHORT, this is a COMPACT
        // swatch button (a preview of the current color) that opens the muxc::ColorPicker in a FLYOUT —
        // the full picker is huge inline, so it only appears on demand (the Windows Terminal tab-color
        // idiom). The picker's ALPHA slider IS the OPACITY control (one control sets hue + opacity).
        // GLOBAL (AppSettings::flashRingColor, "#AARRGGBB"); applied live on Save + cross-window
        // broadcast (TerminalPage::_RefreshFlashRingBrush). Default fully-opaque red == the prior ring.
        {
            auto row = StackPanel{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(10);
            row.VerticalAlignment(VerticalAlignment::Center);

            auto label = Text(L"Status flashing color", 13, false, 0.9);
            label.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(label);

            // The swatch preview — shows the current color (its opacity blends over the dark card); a
            // thin light border keeps a fully-transparent pick visible as an outlined box.
            _flashRingSwatch = Border{};
            _flashRingSwatch.Width(36);
            _flashRingSwatch.Height(18);
            _flashRingSwatch.CornerRadius(CornerRadius{ 3, 3, 3, 3 });
            _flashRingSwatch.BorderThickness(Thickness{ 1, 1, 1, 1 });
            _flashRingSwatch.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0xFF, 0xFF, 0xFF) });
            _flashRingSwatch.Background(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0xFF, 0x00, 0x00) });

            // The picker lives INSIDE the flyout (built once, opened on demand). Reading its Color() at
            // Save works whether or not the flyout was ever opened.
            _setFlashRingPicker = winrt::Microsoft::UI::Xaml::Controls::ColorPicker{};
            _setFlashRingPicker.IsAlphaEnabled(true); // the OPACITY slider + alpha in the chosen Color (the "add opacity" ask)
            _setFlashRingPicker.IsMoreButtonVisible(true); // tuck the RGB / HSV / Hex / Alpha text inputs behind a "More" expander
            _setFlashRingPicker.IsHexInputVisible(true);
            _setFlashRingPicker.IsAlphaTextInputVisible(true);
            _setFlashRingPicker.IsColorChannelTextInputVisible(true);
            _setFlashRingPicker.Color(ColorHelper::FromArgb(0xFF, 0xFF, 0x00, 0x00)); // seed pass (_ShowSettings) sets the real saved color
            // Live-preview the swatch as the user drags the picker.
            _setFlashRingPicker.ColorChanged([this](auto&&, const winrt::Microsoft::UI::Xaml::Controls::ColorChangedEventArgs& e) {
                if (_flashRingSwatch)
                {
                    _flashRingSwatch.Background(SolidColorBrush{ e.NewColor() });
                }
            });

            auto pickerPanel = StackPanel{};
            pickerPanel.Spacing(8);
            pickerPanel.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark; the flyout renders in the popup root
            pickerPanel.Children().Append(_setFlashRingPicker);
            {
                // A clear way back to the default (the picker has no built-in "default") — sets the
                // picker to fully-opaque red; Save then writes "#FFFF0000".
                auto reset = HyperlinkButton{};
                reset.Content(winrt::box_value(L"Reset to default (red)"));
                reset.Padding(Thickness{ 4, 2, 4, 2 });
                reset.FontSize(12);
                reset.Click([this](const IInspectable&, const RoutedEventArgs&) {
                    if (_setFlashRingPicker)
                    {
                        _setFlashRingPicker.Color(ColorHelper::FromArgb(0xFF, 0xFF, 0x00, 0x00));
                    }
                });
                pickerPanel.Children().Append(reset);
            }

            auto flyout = Flyout{};
            flyout.Content(pickerPanel);

            auto swatchBtn = Button{};
            swatchBtn.Padding(Thickness{ 4, 3, 4, 3 });
            swatchBtn.Content(_flashRingSwatch);
            swatchBtn.Flyout(flyout);
            AgentSetTip(swatchBtn, L"Pick the tab status-dot \x201Cunread\x201D flash-ring color. The picker's alpha slider sets its opacity. Default: fully-opaque red.");
            row.Children().Append(swatchBtn);

            panel.Children().Append(row);
        }

        // PROFILE — the per-install state folder (NOT an AppSettings field: it is the pointer
        // TO settings.json, resolved by ProfileBootstrap BEFORE any state loads, so it lives in
        // the choice file / env, never inside the profile it selects). Read-only display +
        // "Change…", which re-runs the same picker the first launch shows and applies on the
        // NEXT start (the running engine cannot re-home its state mid-run).
        panel.Children().Append(Text(L"PROFILE", 11, true, 0.6));
        _setProfileDir = TextBlock{};
        _setProfileDir.TextWrapping(TextWrapping::Wrap);
        _setProfileDir.Opacity(0.85);
        _setProfileDir.FontSize(12);
        AgentSetTip(_setProfileDir, L"This install's active profile folder \x2014 where all sessions, settings, hooks, and window layouts are stored. A staged change shows as current \x2192 new (after restart).");
        panel.Children().Append(_setProfileDir);
        auto changeProfile = Button{};
        changeProfile.Content(winrt::box_value(L"Change profile folder\x2026"));
        AgentSetTip(changeProfile, L"Point this install at a different profile folder \x2014 applies on the next start (the running app can't re-home its state mid-run).");
        changeProfile.Click([this](const IInspectable&, const RoutedEventArgs&) {
            // Defer off the click tick (the XAML-Islands pointer-handler rule), then run the
            // pure-Win32 picker — a Win32 modal gets its keyboard input directly in islands.
            if (_dispatcher)
            {
                _dispatcher.TryEnqueue([this]() {
                    const std::wstring active = ::Agentmaster::Profiles::ResolveProfileDir();
                    const auto pick = ::Agentmaster::Profiles::ShowProfilePicker(::GetActiveWindow(), false, active);
                    if (!pick.chosen)
                    {
                        return;
                    }
                    ::Agentmaster::Profiles::SaveChoice(pick.dir);
                    // Nav audit: the user re-pointed this install at a different profile folder (the cog's
                    // "Change profile folder…"). It re-homes ALL persisted state and applies on the NEXT
                    // start (never mid-run), so the trail records the staged from -> to.
                    ::Agentmaster::LogNav(L"profile-change " + active + L" -> " + pick.dir + (pick.migrate ? std::wstring{ L" (migrate data)" } : std::wstring{}) + L" (applies on restart)");
                    if (pick.migrate)
                    {
                        ::Agentmaster::Profiles::MigrateProfileData(active, pick.dir);
                    }
                    ::Agentmaster::Profiles::SeedTerminalSettings(pick.dir);
                    if (_setProfileDir)
                    {
                        _setProfileDir.Text(winrt::hstring{ active + L"  \x2192  " + pick.dir + L" (after restart)" });
                    }
                    ::MessageBoxW(::GetActiveWindow(),
                                  (L"Profile saved:\n\n    " + pick.dir + L"\n\nIt applies the next time Agentmaster starts.").c_str(),
                                  L"Agentmaster",
                                  MB_OK | MB_ICONINFORMATION);
                });
            }
        });
        panel.Children().Append(changeProfile);

        // Cancel / Save
        auto buttons = StackPanel{};
        buttons.Orientation(Orientation::Horizontal);
        buttons.HorizontalAlignment(HorizontalAlignment::Right);
        buttons.Spacing(8);
        buttons.Margin(Thickness{ 0, 8, 0, 0 });
        auto cancel = Button{};
        cancel.Content(winrt::box_value(L"Cancel"));
        AgentSetTip(cancel, L"Close without saving \x2014 discard any changes made here.");
        cancel.Click([this](const IInspectable&, const RoutedEventArgs&) { _HideSettings(); });
        auto save = Button{};
        save.Content(winrt::box_value(L"Save"));
        AgentSetTip(save, L"Save these settings and apply them \x2014 they persist to disk and govern future sessions (and live-apply where possible, e.g. the Claude binary and tab options).");
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
        // ENV_VARS.md: seed both env tabs (global text + the per-directory draft from dir-env.json).
        _LoadEnvVarsArea();
        if (_setClaudeExePath)
        {
            _setClaudeExePath.Text(winrt::hstring{ _appSettings.claudeExePath });
        }
        if (_setCleanupDays)
        {
            // ENV_VARS.md §8: reflect the LIVE value from the user's global ~/.claude/settings.json (blank if unset).
            const auto days = ::Agentmaster::GetClaudeCleanupPeriodDays();
            _setCleanupDays.Text(winrt::hstring{ days ? std::to_wstring(*days) : std::wstring{} });
        }
        if (_setClaudeDetected)
        {
            const auto& exe = ::Agentmaster::SharedEngine().claudeExePath;
            _setClaudeDetected.Text(winrt::hstring{ exe.empty() ? std::wstring{ L"Detected: none \x2014 Claude launch/fork/resume is disabled until a native claude.exe is found" } : (L"Detected: " + exe) });
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
        if (_setRenameCommit)
        {
            // Items are ordered to match TabRenameCommitMode (0 click-away / 1 +Shift+Enter / 2 +Enter).
            _setRenameCommit.SelectedIndex(_appSettings.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrEnter      ? 2 :
                                           _appSettings.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter ? 1 :
                                                                                                                            0);
        }
        if (_setWaitingDecaySlider && _setWaitingNever)
        {
            const bool never = (_appSettings.waitingForYouTimeoutMinutes == 0);
            _setWaitingNever.IsOn(never);
            uint32_t m = _appSettings.waitingForYouTimeoutMinutes;
            if (m < 1)
            {
                m = 60; // a sane slider position when "never" is on (toggling off then lands on 1h)
            }
            if (m > 4320)
            {
                m = 4320;
            }
            _setWaitingDecaySlider.Value(static_cast<double>(m));
            _setWaitingDecaySlider.IsEnabled(!never);
            _setWaitingDecaySlider.Header(winrt::box_value(winrt::hstring{ L"Waiting-for-you \x2192 Idle after: " } + winrt::hstring{ FormatMinutesFriendly(m) }));
        }
        if (_setServerCache)
        {
            _setServerCache.Text(winrt::hstring{ std::to_wstring(_appSettings.serverCacheMinutes) });
        }
        if (_setLaunchDir)
        {
            _setLaunchDir.Text(winrt::hstring{ _appSettings.defaultLaunchDir });
        }
        if (_setRecentDirsLimit)
        {
            _setRecentDirsLimit.Text(winrt::hstring{ std::to_wstring(_appSettings.recentDirsLimit) });
        }
        if (_setShowTabCloseButton)
        {
            _setShowTabCloseButton.IsOn(_appSettings.showTabCloseButton);
        }
        if (_setCloseTabOnMiddleClick)
        {
            _setCloseTabOnMiddleClick.IsOn(_appSettings.closeTabOnMiddleClick);
        }
        if (_setAlwaysShowHomeButton)
        {
            _setAlwaysShowHomeButton.IsOn(_appSettings.alwaysShowHomeButton);
        }
        if (_setFavoriteIcon)
        {
            // Items: 0 == Crown (default), 1 == Star.
            _setFavoriteIcon.SelectedIndex(_appSettings.favoriteIcon == FavoriteIcon::Star ? 1 : 0);
        }
        if (_setFlashRingPicker)
        {
            // The status flashing color (with opacity in the alpha byte). Malformed/empty -> opaque red.
            const auto c = ParseArgbHexColor(_appSettings.flashRingColor, ColorHelper::FromArgb(0xFF, 0xFF, 0x00, 0x00));
            _setFlashRingPicker.Color(c); // also raises ColorChanged -> updates the swatch preview
            if (_flashRingSwatch)
            {
                _flashRingSwatch.Background(SolidColorBrush{ c }); // set directly too (don't rely on a programmatic ColorChanged firing)
            }
        }
        if (_setResetHidden)
        {
            // The "cleared" state is per-click feedback; restore the actionable label each open.
            _setResetHidden.Content(winrt::box_value(L"Reset hidden sessions"));
            _setResetHidden.IsEnabled(true);
        }
        if (_setProfileDir)
        {
            // The ACTIVE profile (this run) — a pending Change… is re-shown as pending until restart.
            const std::wstring active = ::Agentmaster::Profiles::ResolveProfileDir();
            const std::wstring saved = ::Agentmaster::Profiles::ReadSavedChoice();
            if (!saved.empty() && !::Agentmaster::Profiles::detail::SamePath(saved, active))
            {
                _setProfileDir.Text(winrt::hstring{ active + L"  \x2192  " + saved + L" (after restart)" });
            }
            else
            {
                _setProfileDir.Text(winrt::hstring{ active });
            }
        }
        if (_setAllowPrerelease)
        {
            _setAllowPrerelease.IsOn(_appSettings.allowUpdatePrerelease);
        }
        if (_setUpdateChangelog)
        {
            // Hide "Update's changelog" until THIS open's check confirms an update is available
            // (the silent check below, or the explicit button, reveals it).
            _setUpdateChangelog.Visibility(Visibility::Collapsed);
        }
        // Updater is RELEASE-channel only (Updater::IsUpdaterChannel). On a dev/unpackaged build the
        // GitHub release is NOT a self-update (different package + always-"behind" the 0.0.1.0
        // placeholder), so don't check or offer it: disable the button, hide the changelog links, and
        // explain. Only the release install checks (silently on open) + shows "vX.Y.Z available!".
        const bool updaterChannel = ::Agentmaster::Updater::IsUpdaterChannel();
        if (_setCheckUpdates)
        {
            _setCheckUpdates.IsEnabled(updaterChannel);
        }
        if (_setUninstallBtn)
        {
            // Uninstall removes the CURRENT package family (release OR dev), so it's offered for any
            // packaged install — independent of the release-only updater channel. An unpackaged build
            // has nothing registered to remove, so hide it there.
            _setUninstallBtn.Visibility(::Agentmaster::Updater::IsPackaged() ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_setCurrentChangelog)
        {
            // The current build's release page only exists for a published version; hide it on dev.
            _setCurrentChangelog.Visibility(updaterChannel ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_setUpdateStatus)
        {
            if (updaterChannel)
            {
                _setUpdateStatus.Text(L""); // cleared until the silent check (below) finds an update
            }
            else
            {
                _setUpdateStatus.Text(L"Dev build \x2014 the updater manages the Release install (rebuild to update this one).");
                _setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x99, 0x99, 0x99) });
            }
        }
        _settingsOverlay.Visibility(Visibility::Visible);
        // Updater: a silent check on open — if a newer release exists, the label next to "Check for
        // updates" reads "vX.Y.Z available!" in dark green. Quiet on no-update / no-network (the
        // explicit button gives that feedback). Runs off the UI thread (Updater.h uses WinHTTP).
        // RELEASE channel only — a dev build neither auto-checks nor is offered the release as an update.
        if (updaterChannel)
        {
            _CheckForUpdates(false);
        }
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
        // ENV_VARS.md: global editor -> _appSettings.env; flush the per-directory draft -> dir-env.json.
        _SaveEnvVarsArea();
        if (_setClaudeExePath)
        {
            std::wstring p{ _setClaudeExePath.Text() };
            const auto a = p.find_first_not_of(L" \t");
            const auto b = p.find_last_not_of(L" \t");
            _appSettings.claudeExePath = (a == std::wstring::npos) ? std::wstring{} : p.substr(a, b - a + 1);
        }
        if (_setCleanupDays)
        {
            // ENV_VARS.md §8: write cleanupPeriodDays back to the user's global ~/.claude/settings.json.
            // Blank => remove our key (revert to Claude's 30-day default); a pure non-negative integer => set
            // it; any other text => leave the file untouched. (0 is accepted but is a Claude footgun — the
            // tooltip warns; we don't second-guess a deliberate entry.)
            std::wstring t{ _setCleanupDays.Text() };
            const auto a = t.find_first_not_of(L" \t\r\n");
            const auto b = t.find_last_not_of(L" \t\r\n");
            const std::wstring s = (a == std::wstring::npos) ? std::wstring{} : t.substr(a, b - a + 1);
            if (s.empty())
            {
                ::Agentmaster::SetClaudeCleanupPeriodDays(std::nullopt);
            }
            else
            {
                bool pure = true;
                for (const wchar_t c : s)
                {
                    if (c < L'0' || c > L'9')
                    {
                        pure = false;
                        break;
                    }
                }
                if (pure)
                {
                    int64_t v = 0;
                    for (const wchar_t c : s)
                    {
                        v = v * 10 + static_cast<int64_t>(c - L'0');
                        if (v > 100000000)
                        {
                            v = 100000000; // clamp; cleanupPeriodDays far past ~100y is meaningless
                        }
                    }
                    ::Agentmaster::SetClaudeCleanupPeriodDays(v);
                }
            }
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
        if (_setRenameCommit)
        {
            const int idx = _setRenameCommit.SelectedIndex();
            _appSettings.tabRenameCommitMode = idx == 2 ? TabRenameCommitMode::ClickAwayOrEnter :
                                               idx == 0 ? TabRenameCommitMode::ClickAwayOnly :
                                                          TabRenameCommitMode::ClickAwayOrShiftEnter;
        }
        if (_setWaitingDecaySlider && _setWaitingNever)
        {
            // "Never" => 0 (never time-decay; stay Waiting until read). Else the slider's 1..4320 minutes.
            if (_setWaitingNever.IsOn())
            {
                _appSettings.waitingForYouTimeoutMinutes = 0;
            }
            else
            {
                uint32_t v = static_cast<uint32_t>(_setWaitingDecaySlider.Value());
                if (v < 1)
                {
                    v = 1;
                }
                if (v > 4320)
                {
                    v = 4320;
                }
                _appSettings.waitingForYouTimeoutMinutes = v;
            }
        }
        if (_setServerCache)
        {
            const std::wstring t{ _setServerCache.Text() };
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
            _appSettings.serverCacheMinutes = (any && v > 0) ? v : 5; // blank/0 -> 5 (the cosmetic default)
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
        if (_setShowTabCloseButton)
        {
            _appSettings.showTabCloseButton = _setShowTabCloseButton.IsOn();
        }
        if (_setCloseTabOnMiddleClick)
        {
            _appSettings.closeTabOnMiddleClick = _setCloseTabOnMiddleClick.IsOn();
        }
        if (_setAlwaysShowHomeButton)
        {
            _appSettings.alwaysShowHomeButton = _setAlwaysShowHomeButton.IsOn();
        }
        if (_setFavoriteIcon)
        {
            // Items: 0 == Crown (default), 1 == Star.
            _appSettings.favoriteIcon = _setFavoriteIcon.SelectedIndex() == 1 ? FavoriteIcon::Star : FavoriteIcon::Crown;
        }
        if (_setFlashRingPicker)
        {
            // The picker always yields a valid Color; store it as "#AARRGGBB" (opacity in the alpha byte).
            _appSettings.flashRingColor = FormatArgbHexColor(_setFlashRingPicker.Color());
        }
        if (_setAllowPrerelease)
        {
            _appSettings.allowUpdatePrerelease = _setAllowPrerelease.IsOn(); // UPDATES: the form OWNS this field
        }
        // Preserve fields owned by out-of-cog UI actions, freshest from disk (the page's settings handler
        // does the same for hiddenSessionIds/showSummaryPanel): the summary panel SIZE (width/height
        // fractions, TAB_OVERLAY.md) is written by the panel's resize grips, not this form, so a form Save
        // must not regress a resize done since the modal was seeded (incl. from another window). The
        // updater's skip/postpone state (Updater.h) is likewise written outside this form (a JSON RMW),
        // so preserve it too — only allowUpdatePrerelease (above) is the form's to write.
        {
            const auto disk = ::Agentmaster::LoadAppSettings();
            _appSettings.summaryPanelWidthFraction = disk.summaryPanelWidthFraction;
            _appSettings.summaryPanelHeightFraction = disk.summaryPanelHeightFraction;
            _appSettings.summaryPanelWrapNewlines = disk.summaryPanelWrapNewlines; // wrap-line toggle (panel times bar), out-of-cog UI action
            _appSettings.updateSkippedVersion = disk.updateSkippedVersion; // updater "Skip this version" (out-of-cog JSON RMW)
            _appSettings.updatePostponedUntilUnixMs = disk.updatePostponedUntilUnixMs; // updater "Postpone N days" (out-of-cog JSON RMW)
            _appSettings.envDefaultsVersion = disk.envDefaultsVersion; // shipped-default seed marker (engine-init, out-of-cog) — a Save must never reset it (would re-add a deleted default)
            _appSettings.claudeCleanupDaysSeeded = disk.claudeCleanupDaysSeeded; // shipped-default seed marker (engine-init, out-of-cog)
        }
        if (_settingsSink)
        {
            // Nav audit: the user saved the global Settings (cog) — the one consequential cog action; it
            // governs every future session. Record the most behavior-impacting fields (the rest persist to
            // settings.json, the durable record).
            ::Agentmaster::LogNav(std::wstring{ L"settings-save skipPerms=" } + (_appSettings.skipPermissions ? L"1" : L"0") +
                                  L" model=" + (_appSettings.model.empty() ? std::wstring{ L"(default)" } : _appSettings.model) +
                                  L" autopilot=" + (_appSettings.defaultAutopilotMode == AutopilotMode::Full ? L"Full" : _appSettings.defaultAutopilotMode == AutopilotMode::SemiAuto ? L"Semi" : L"Off") +
                                  L" claudeExe=" + (_appSettings.claudeExePath.empty() ? std::wstring{ L"(auto)" } : _appSettings.claudeExePath));
            _settingsSink(_appSettings); // page persists + applies to future spawns
        }
        // Native-exe-only policy: re-resolve the claude.exe now, so a changed/cleared override (or a
        // freshly-installed binary) takes effect this run — no restart needed (RefreshClaudeExe updates
        // the shared engine's cached path; ClaudeAvailable() flips accordingly).
        ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
        _HideSettings();
    }

    // === ENV_VARS.md: the "Environment variables" area (Global / Per-directory two-tab editor) ===========
    // The plumbing already merges global + per-dir at spawn (ResolveSessionEnv); this is the editor.

    void AgentManagerContent::_BuildEnvVarsArea(const StackPanel& panel)
    {
        // Section header (matches the "CLAUDE SESSIONS" style above it).
        panel.Children().Append(Text(L"ENVIRONMENT VARIABLES", 11, true, 0.6));

        // Tab toggle: [ Global ][ Per-directory ] — two Buttons swapping the two panels (the LOCAL/GLOBAL
        // scope-toggle idiom; not a Pivot, which themes unreliably under XAML Islands).
        auto tabs = StackPanel{};
        tabs.Orientation(Orientation::Horizontal);
        tabs.Spacing(0);
        tabs.Margin(Thickness{ 0, 2, 0, 4 });
        _setEnvTabGlobal = Button{};
        _setEnvTabGlobal.Content(box_value(L"Global"));
        _setEnvTabGlobal.FontSize(12);
        _setEnvTabGlobal.Padding(Thickness{ 12, 2, 12, 2 });
        _setEnvTabGlobal.BorderThickness(Thickness{ 0, 0, 0, 0 });
        AgentSetTip(_setEnvTabGlobal, L"Variables applied to EVERY session (Claude and Codex), in every directory.");
        _setEnvTabGlobal.Click([this](const IInspectable&, const RoutedEventArgs&) { _SwitchEnvTab(false); });
        _setEnvTabDir = Button{};
        _setEnvTabDir.Content(box_value(L"Per-directory"));
        _setEnvTabDir.FontSize(12);
        _setEnvTabDir.Padding(Thickness{ 12, 2, 12, 2 });
        _setEnvTabDir.BorderThickness(Thickness{ 0, 0, 0, 0 });
        AgentSetTip(_setEnvTabDir, L"Variables added only for sessions launched in a chosen working directory \x2014 they OVERRIDE a Global variable of the same name.");
        _setEnvTabDir.Click([this](const IInspectable&, const RoutedEventArgs&) { _SwitchEnvTab(true); });
        tabs.Children().Append(_setEnvTabGlobal);
        tabs.Children().Append(_setEnvTabDir);
        panel.Children().Append(tabs);

        // A multi-line NAME=VALUE editor, monospaced; its OWN border is suppressed so the wrapping Border
        // (recolored by the live lexer) is the status indicator.
        const auto makeEditor = [](TextBox& box) {
            box = TextBox{};
            box.AcceptsReturn(true);
            box.TextWrapping(TextWrapping::Wrap);
            box.FontFamily(FontFamily{ L"Consolas" });
            box.FontSize(12);
            box.MinHeight(84);
            box.MaxHeight(168);
            box.BorderThickness(Thickness{ 0, 0, 0, 0 });
            ScrollViewer::SetVerticalScrollBarVisibility(box, ScrollBarVisibility::Auto);
        };
        const auto wrapInBorder = [](const TextBox& box, Border& border) {
            border = Border{};
            border.BorderThickness(Thickness{ 1, 1, 1, 1 });
            border.CornerRadius(CornerRadius{ 2, 2, 2, 2 });
            border.BorderBrush(Fill(0x60, 0x80, 0x80, 0x80)); // subtle neutral until the lexer paints it
            border.Child(box);
        };

        // --- Global panel ---
        _envGlobalPanel = StackPanel{};
        _envGlobalPanel.Spacing(2);
        {
            auto hint = Text(L"One NAME=VALUE per line (e.g. HTTPS_PROXY=http://h:8080). Applied to every session.", 11, false, 0.55);
            hint.TextWrapping(TextWrapping::Wrap);
            _envGlobalPanel.Children().Append(hint);
        }
        makeEditor(_setEnv);
        _setEnv.PlaceholderText(L"NAME=VALUE\nNAME=VALUE");
        AgentSetTip(_setEnv, L"Environment variables applied to every launched session (Claude and Codex). One NAME=VALUE per line; '#' starts a comment.");
        _setEnv.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _RefreshEnvLex(false); });
        wrapInBorder(_setEnv, _setEnvBorder);
        _envGlobalPanel.Children().Append(_setEnvBorder);
        _setEnvStatus = Text(L"", 11, false, 0.7);
        _envGlobalPanel.Children().Append(_setEnvStatus);
        panel.Children().Append(_envGlobalPanel);

        // --- Per-directory panel (hidden until its tab is picked) ---
        _envDirPanel = StackPanel{};
        _envDirPanel.Spacing(2);
        _envDirPanel.Visibility(Visibility::Collapsed);
        {
            auto hint = Text(L"Pick a working directory, then add variables for sessions launched there \x2014 they override the Global ones of the same name.  \x25CF = already has variables.", 11, false, 0.55);
            hint.TextWrapping(TextWrapping::Wrap);
            _envDirPanel.Children().Append(hint);
        }
        _setEnvDirFilter = TextBox{};
        _setEnvDirFilter.PlaceholderText(L"type to filter directories\x2026");
        _setEnvDirFilter.FontSize(12);
        _setEnvDirFilter.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _RebuildEnvDirList(); });
        _envDirPanel.Children().Append(_setEnvDirFilter);
        _setEnvDirList = ListBox{};
        _setEnvDirList.MaxHeight(120);
        _setEnvDirList.SelectionChanged([this](const IInspectable&, const SelectionChangedEventArgs&) {
            if (!_setEnvDirList)
            {
                return;
            }
            const auto sel = _setEnvDirList.SelectedItem().try_as<ListBoxItem>();
            if (!sel)
            {
                return; // a Clear() during rebuild fires SelectionChanged with no item — keep editing the current dir
            }
            const std::wstring composite{ winrt::unbox_value_or<winrt::hstring>(sel.Tag(), L"") };
            const auto sepPos = composite.find(L'\x1f');
            if (sepPos == std::wstring::npos)
            {
                return;
            }
            _SelectEnvDir(composite.substr(0, sepPos), composite.substr(sepPos + 1));
        });
        _envDirPanel.Children().Append(_setEnvDirList);
        makeEditor(_setEnvDir);
        _setEnvDir.IsEnabled(false);
        _setEnvDir.PlaceholderText(L"select a directory above");
        _setEnvDir.Header(box_value(L"Variables for the selected directory"));
        _setEnvDir.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) { _RefreshEnvLex(true); });
        wrapInBorder(_setEnvDir, _setEnvDirBorder);
        _envDirPanel.Children().Append(_setEnvDirBorder);
        _setEnvDirStatus = Text(L"", 11, false, 0.7);
        _envDirPanel.Children().Append(_setEnvDirStatus);
        panel.Children().Append(_envDirPanel);

        _SwitchEnvTab(false); // start on Global (also styles the tab buttons)
    }

    void AgentManagerContent::_SwitchEnvTab(bool perDir)
    {
        _envTabIsDir = perDir;
        if (_envGlobalPanel)
        {
            _envGlobalPanel.Visibility(perDir ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_envDirPanel)
        {
            _envDirPanel.Visibility(perDir ? Visibility::Visible : Visibility::Collapsed);
        }
        const auto style = [](const Button& b, bool active) {
            if (!b)
            {
                return;
            }
            b.Background(active ? Fill(0xFF, 0x0E, 0x63, 0x9C) : Fill(0x00, 0x00, 0x00, 0x00));
            b.Foreground(active ? Fill(0xFF, 0xFF, 0xFF, 0xFF) : Fill(0xFF, 0xB0, 0xB0, 0xB0));
        };
        style(_setEnvTabGlobal, !perDir);
        style(_setEnvTabDir, perDir);
        if (perDir)
        {
            _RebuildEnvDirList();
        }
    }

    void AgentManagerContent::_RebuildEnvDirList()
    {
        if (!_setEnvDirList)
        {
            return;
        }
        const auto lc = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        const auto isBlank = [](const std::wstring& v) {
            for (const wchar_t c : v)
            {
                if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
                {
                    return false;
                }
            }
            return true;
        };

        // Candidates: every live session's working dir UNION every dir already holding env in the draft.
        std::vector<std::pair<std::wstring, std::wstring>> cands; // (NormDirKey, displayPath)
        std::unordered_set<std::wstring> seen;
        if (_registry)
        {
            for (const auto& s : _registry->Snapshot())
            {
                if (s.workingDir.empty())
                {
                    continue;
                }
                const std::wstring key = ::Agentmaster::NormDirKey(s.workingDir);
                if (seen.insert(key).second)
                {
                    cands.emplace_back(key, s.workingDir);
                }
            }
        }
        std::unordered_set<std::wstring> withEnv;
        for (const auto& [k, env] : _dirEnvDraft)
        {
            if (!isBlank(env))
            {
                withEnv.insert(k);
            }
            if (seen.insert(k).second)
            {
                cands.emplace_back(k, k); // no live session in this dir — show the key itself
            }
        }

        const std::wstring filt = lc(std::wstring{ _setEnvDirFilter ? _setEnvDirFilter.Text() : winrt::hstring{} });
        std::vector<std::pair<std::wstring, std::wstring>> shown;
        for (const auto& c : cands)
        {
            if (filt.empty() || lc(c.second).find(filt) != std::wstring::npos)
            {
                shown.push_back(c);
            }
        }
        std::sort(shown.begin(), shown.end(), [&](const auto& a, const auto& b) {
            const bool ea = withEnv.count(a.first) != 0;
            const bool eb = withEnv.count(b.first) != 0;
            if (ea != eb)
            {
                return ea; // dirs that already have env sort first
            }
            return lc(a.second) < lc(b.second);
        });

        _setEnvDirList.Items().Clear();
        for (const auto& [key, disp] : shown)
        {
            ListBoxItem item{};
            const bool has = withEnv.count(key) != 0;
            item.Content(box_value(winrt::hstring{ (has ? std::wstring{ L"\x25CF  " } : std::wstring{ L"     " }) + disp }));
            item.Tag(box_value(winrt::hstring{ key + L"\x1f" + disp })); // (NormDirKey, displayPath) for SelectionChanged
            item.FontSize(12);
            _setEnvDirList.Items().Append(item);
        }
    }

    void AgentManagerContent::_SelectEnvDir(const std::wstring& normKey, const std::wstring& displayPath)
    {
        if (!_setEnvDir)
        {
            return;
        }
        // The previously-edited dir's text is already mirrored into _dirEnvDraft by _RefreshEnvLex(true)
        // (fired on every keystroke), so switching keys loses nothing.
        _envEditingDirKey = normKey;
        std::wstring text;
        for (const auto& [k, env] : _dirEnvDraft)
        {
            if (k == normKey)
            {
                text = env;
                break;
            }
        }
        _setEnvDir.IsEnabled(true);
        _setEnvDir.Header(box_value(winrt::hstring{ L"Variables for: " + displayPath }));
        _setEnvDir.Text(winrt::hstring{ text }); // fires _RefreshEnvLex(true) -> lex + (idempotent) draft upsert under normKey
    }

    void AgentManagerContent::_RefreshEnvLex(bool perDir)
    {
        const auto box = perDir ? _setEnvDir : _setEnv;
        const auto border = perDir ? _setEnvDirBorder : _setEnvBorder;
        const auto status = perDir ? _setEnvDirStatus : _setEnvStatus;
        if (!box)
        {
            return;
        }
        const std::wstring text{ box.Text() };

        // Keep the per-dir draft current as the user types (the global side writes _appSettings only on Save).
        if (perDir && !_envEditingDirKey.empty())
        {
            bool found = false;
            for (auto& [k, env] : _dirEnvDraft)
            {
                if (k == _envEditingDirKey)
                {
                    env = text;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                _dirEnvDraft.emplace_back(_envEditingDirKey, text);
            }
        }

        const auto res = ::Agentmaster::LexEnvText(text);

        // Same palette as _ValidateLaunchBox: green ok / amber warn / red error; a subtle gray at rest.
        const SolidColorBrush green = Fill(0xFF, 0x4C, 0xAF, 0x50);
        const SolidColorBrush red = Fill(0xFF, 0xE5, 0x39, 0x35);
        const SolidColorBrush amber = Fill(0xFF, 0xDA, 0xA5, 0x20);
        const SolidColorBrush neutral = Fill(0x60, 0x80, 0x80, 0x80);
        const SolidColorBrush dim = Fill(0xFF, 0x99, 0x99, 0x99);

        const bool empty = (res.ok == 0 && res.warn == 0 && res.error == 0);
        if (border)
        {
            const SolidColorBrush bc = empty ? neutral :
                                       res.worst == ::Agentmaster::EnvLineKind::Error ? red :
                                       res.worst == ::Agentmaster::EnvLineKind::Warn  ? amber :
                                                                                        green;
            border.BorderBrush(bc);
        }
        if (status)
        {
            std::wstring msg;
            SolidColorBrush fg = dim;
            if (empty)
            {
                msg = (perDir && _envEditingDirKey.empty()) ? L"Select a directory to edit its variables." : L"No variables.";
            }
            else if (res.error > 0)
            {
                msg = L"\x2715 " + res.firstIssue + L" (line " + std::to_wstring(res.firstIssueLine) + L")";
                fg = red;
            }
            else if (res.warn > 0)
            {
                msg = L"\x26A0 " + std::to_wstring(res.ok) + (res.ok == 1 ? L" variable \x00B7 " : L" variables \x00B7 ") + res.firstIssue + L" (line " + std::to_wstring(res.firstIssueLine) + L")";
                fg = amber;
            }
            else
            {
                msg = L"\x2713 " + std::to_wstring(res.ok) + (res.ok == 1 ? L" variable" : L" variables");
                fg = green;
            }
            status.Text(winrt::hstring{ msg });
            status.Foreground(fg);
        }
    }

    void AgentManagerContent::_LoadEnvVarsArea()
    {
        // Global: show the stored env one-per-line. A legacy ';'-delimited value reads as multi-line ('; '
        // is only ever a separator — values can't contain it — so this is lossless), then saves back
        // newline-delimited (ParseEnvAssignments accepts both).
        if (_setEnv)
        {
            std::wstring disp = _appSettings.env;
            for (auto& c : disp)
            {
                if (c == L';')
                {
                    c = L'\n';
                }
            }
            _setEnv.Text(winrt::hstring{ disp });
        }
        // Per-directory: load the on-disk map into the editable draft; reset the selector + editor (the
        // draft is the working copy until Save, so Cancel discards any per-dir edits made in the modal).
        _dirEnvDraft = ::Agentmaster::LoadDirEnv();
        _envEditingDirKey.clear();
        if (_setEnvDirFilter)
        {
            _setEnvDirFilter.Text(L"");
        }
        if (_setEnvDir)
        {
            _setEnvDir.Text(L"");
            _setEnvDir.IsEnabled(false);
            _setEnvDir.Header(box_value(L"Variables for the selected directory"));
        }
        _RebuildEnvDirList();
        _RefreshEnvLex(false);
        _RefreshEnvLex(true);
        _SwitchEnvTab(false); // always open on the Global tab
    }

    void AgentManagerContent::_SaveEnvVarsArea()
    {
        if (_setEnv)
        {
            _appSettings.env = std::wstring{ _setEnv.Text() }; // newline-delimited; ParseEnvAssignments handles it
        }
        // Flush the per-directory draft (the active editor is already mirrored in by _RefreshEnvLex). Drop
        // blank entries so a cleared editor removes a dir (matches SetDirEnv / DeserializeDirEnv semantics).
        std::vector<std::pair<std::wstring, std::wstring>> clean;
        clean.reserve(_dirEnvDraft.size());
        for (const auto& [k, env] : _dirEnvDraft)
        {
            bool blank = true;
            for (const wchar_t c : env)
            {
                if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
                {
                    blank = false;
                    break;
                }
            }
            if (!k.empty() && !blank)
            {
                clean.emplace_back(k, env);
            }
        }
        ::Agentmaster::SaveDirEnv(clean);
    }

    void AgentManagerContent::SetQuitForUpdateHandler(std::function<void()> handler)
    {
        _quitForUpdateHandler = std::move(handler);
    }

    void AgentManagerContent::_CheckForUpdates(bool interactive)
    {
        // A double-click of the button must not stack two prompts; a silent on-open check is allowed
        // to overlap (both are idempotent GitHub reads — the later result just wins the label).
        if (interactive && _interactiveUpdateInFlight)
        {
            return;
        }
        // The worker marshals its result back via the dispatcher; with no dispatcher it could never
        // re-enable the button / clear the in-flight flag, so bail before we touch either.
        if (!_dispatcher)
        {
            return;
        }
        // Release-channel only (Updater::IsUpdaterChannel): a dev/unpackaged build must NOT present a
        // GitHub release as a self-update — it's a different package and always-"behind" the 0.0.1.0
        // placeholder. _ShowSettings already disables the button + shows the explanatory note there;
        // this is the backstop so a stray call never runs the misleading check.
        if (!::Agentmaster::Updater::IsUpdaterChannel())
        {
            return;
        }
        if (interactive)
        {
            _interactiveUpdateInFlight = true;
            if (_setCheckUpdates)
            {
                _setCheckUpdates.IsEnabled(false);
                _setCheckUpdates.Content(winrt::box_value(L"Checking\x2026"));
            }
            if (_setUpdateStatus)
            {
                _setUpdateStatus.Text(L"Checking GitHub\x2026");
                _setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0xB0, 0xB0, 0xB0) });
            }
        }

        // Prefer the LIVE pre-release toggle (so toggling it, then clicking Check, uses the new value
        // before a Save) over the saved setting.
        bool prerelease = _appSettings.allowUpdatePrerelease;
        if (_setAllowPrerelease)
        {
            prerelease = _setAllowPrerelease.IsOn();
        }
        // Nav audit: the user clicked "Check for updates" (interactive only — the silent on-cog-open check
        // is automatic, never logged). The result lands in the cog label, not the trail.
        if (interactive)
        {
            ::Agentmaster::LogNav(std::wstring{ L"check-for-updates (allowPrerelease=" } + (prerelease ? L"1" : L"0") + L")");
        }

        const std::wstring stateDir = ::Agentmaster::Profiles::ResolveProfileDir();
        const auto cur = ::Agentmaster::Updater::CurrentPackageVersion();

        auto weak = get_weak();
        auto disp = _dispatcher;
        std::thread([weak, disp, interactive, prerelease, stateDir, cur]() {
            // The network round-trip (Updater.h uses WinHTTP) — bounded; a longer budget for the
            // explicit button than the silent on-open check.
            const ::Agentmaster::Updater::UpdateInfo info =
                ::Agentmaster::Updater::CheckForUpdate(cur, prerelease, interactive ? 8000 : 5000);
            if (!disp)
            {
                return;
            }
            disp.TryEnqueue([weak, interactive, stateDir, info]() {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                if (interactive)
                {
                    self->_interactiveUpdateInFlight = false;
                    if (self->_setCheckUpdates)
                    {
                        self->_setCheckUpdates.IsEnabled(true);
                        self->_setCheckUpdates.Content(winrt::box_value(L"Check for updates"));
                    }
                }
                if (self->_setUpdateStatus)
                {
                    if (info.available)
                    {
                        // "v0.4.3 available!" — dark green, legible on the dark settings card.
                        self->_setUpdateStatus.Text(winrt::hstring{ ::Agentmaster::Updater::DisplayVersion(info) + L" available!" });
                        self->_setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x2E, 0xA0, 0x43) });
                    }
                    else if (interactive)
                    {
                        self->_setUpdateStatus.Text(info.checked ? winrt::hstring{ L"You're on the latest version" } : winrt::hstring{ L"Couldn't reach GitHub" });
                        self->_setUpdateStatus.Foreground(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x99, 0x99, 0x99) });
                    }
                    else
                    {
                        self->_setUpdateStatus.Text(L""); // silent check: stay quiet unless there IS an update
                    }
                }
                // "Update's changelog" link: reveal it (and remember its release page) when an update is
                // available — from EITHER the silent on-open check or the explicit button — else hide it.
                if (info.available)
                {
                    self->_lastUpdateChangelogUrl = ::Agentmaster::Updater::ChangelogUrl(info);
                    if (self->_setUpdateChangelog)
                    {
                        self->_setUpdateChangelog.Visibility(Visibility::Visible);
                    }
                }
                else
                {
                    self->_lastUpdateChangelogUrl.clear();
                    if (self->_setUpdateChangelog)
                    {
                        self->_setUpdateChangelog.Visibility(Visibility::Collapsed);
                    }
                }
                // Interactive only: prompt + apply on a found update (the silent on-open check just labels).
                if (interactive && info.available)
                {
                    const HWND owner = ::GetActiveWindow();
                    const auto d = ::Agentmaster::Updater::ShowUpdatePrompt(owner, info);
                    if (::Agentmaster::Updater::ApplyDecision(stateDir, info, d, owner))
                    {
                        // The installer was launched detached; close the app gracefully so the package
                        // isn't in use while it upgrades + relaunches (the page's RequestQuit).
                        if (self->_quitForUpdateHandler)
                        {
                            self->_quitForUpdateHandler();
                        }
                    }
                }
            });
        }).detach();
    }

    // ---- "Claude not detected" overlay (native-exe-only policy gate) --------

    void AgentManagerContent::_BuildClaudeMissingOverlay()
    {
        // Mirrors the settings overlay: a dimmed modal in the main tree (NOT a ContentDialog, so its
        // buttons + text behave in XAML Islands). Shown when a launch/fork is attempted with no native
        // claude.exe resolved (ClaudeAvailable() false).
        _claudeMissingOverlay = Grid{};
        _claudeMissingOverlay.Visibility(Visibility::Collapsed);
        _claudeMissingOverlay.Background(SolidColorBrush{ ColorHelper::FromArgb(0xA0, 0x00, 0x00, 0x00) });
        Grid::SetRow(_claudeMissingOverlay, 0);
        Grid::SetRowSpan(_claudeMissingOverlay, 99);
        Grid::SetColumnSpan(_claudeMissingOverlay, 99);
        _claudeMissingOverlay.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
            _HideClaudeMissing();
        });

        auto card = Border{};
        card.Background(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x25, 0x25, 0x25) });
        card.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x90, 0x80, 0x80, 0x80) });
        card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        card.Padding(Thickness{ 20, 16, 20, 16 });
        card.Width(500);
        card.HorizontalAlignment(HorizontalAlignment::Center);
        card.VerticalAlignment(VerticalAlignment::Center);
        card.RequestedTheme(ElementTheme::Dark);
        card.Tapped([](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs& e) {
            e.Handled(true);
        });

        auto panel = StackPanel{};
        panel.Spacing(10);
        panel.Children().Append(Text(L"\x26A0 Claude Code (native) not found", 18, true, 1.0));
        {
            auto body = TextBlock{};
            body.TextWrapping(TextWrapping::Wrap);
            body.Opacity(0.9);
            body.FontSize(13);
            body.Text(L"Agentmaster drives the native claude.exe. None was found on PATH, in "
                      L"%USERPROFILE%\\.local\\bin, or behind an npm claude.cmd. Launching, resuming, and "
                      L"forking are disabled until one is available \x2014 a pure-Node `claude` is not "
                      L"supported (the fleet view, adoption, and process insight all need the native binary).");
            panel.Children().Append(body);
        }
        {
            auto how = TextBlock{};
            how.TextWrapping(TextWrapping::Wrap);
            how.Opacity(0.9);
            how.FontSize(13);
            how.Text(L"Install it: open a terminal and run   claude install   (migrates an existing "
                     L"Claude Code to the native build), then click Re-check. Or get Claude Code below, "
                     L"or Browse\x2026 to a claude.exe you already have.");
            panel.Children().Append(how);
        }
        _claudeMissingStatus = TextBlock{};
        _claudeMissingStatus.TextWrapping(TextWrapping::Wrap);
        _claudeMissingStatus.Opacity(0.7);
        _claudeMissingStatus.FontSize(12);
        panel.Children().Append(_claudeMissingStatus);

        auto buttons = StackPanel{};
        buttons.Orientation(Orientation::Horizontal);
        buttons.HorizontalAlignment(HorizontalAlignment::Right);
        buttons.Spacing(8);
        buttons.Margin(Thickness{ 0, 8, 0, 0 });
        auto getClaude = Button{};
        getClaude.Content(winrt::box_value(L"Get Claude Code"));
        AgentSetTip(getClaude, L"Open the Claude Code setup docs in your browser.");
        getClaude.Click([](const IInspectable&, const RoutedEventArgs&) {
            try
            {
                winrt::Windows::System::Launcher::LaunchUriAsync(Uri{ L"https://code.claude.com/docs/en/setup" });
            }
            CATCH_LOG();
        });
        auto browse = Button{};
        browse.Content(winrt::box_value(L"Browse for claude.exe\x2026"));
        AgentSetTip(browse, L"Pick an existing claude.exe with a file dialog \x2014 sets it as the override and re-resolves.");
        browse.Click([this](const IInspectable&, const RoutedEventArgs&) { _BrowseForClaudeExe(false); });
        auto recheck = Button{};
        recheck.Content(winrt::box_value(L"Re-check"));
        AgentSetTip(recheck, L"Look for claude.exe again \x2014 click after running claude install or installing the native build.");
        recheck.Click([this](const IInspectable&, const RoutedEventArgs&) {
            const auto exe = ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
            if (!exe.empty())
            {
                _HideClaudeMissing();
                _ValidateLaunchBox();
            }
            else if (_claudeMissingStatus)
            {
                _claudeMissingStatus.Text(L"Still not found. Run `claude install`, or Browse to a claude.exe.");
            }
        });
        auto close = Button{};
        close.Content(winrt::box_value(L"Close"));
        AgentSetTip(close, L"Dismiss this notice \x2014 Claude launch / resume / fork stay disabled until a native claude.exe is found.");
        close.Click([this](const IInspectable&, const RoutedEventArgs&) { _HideClaudeMissing(); });
        buttons.Children().Append(getClaude);
        buttons.Children().Append(browse);
        buttons.Children().Append(recheck);
        buttons.Children().Append(close);
        panel.Children().Append(buttons);

        card.Child(panel);
        _claudeMissingOverlay.Children().Append(card);
        _root.Children().Append(_claudeMissingOverlay);
    }

    void AgentManagerContent::_ShowClaudeMissing()
    {
        if (!_claudeMissingOverlay)
        {
            return;
        }
        if (_claudeMissingStatus)
        {
            const auto& exe = ::Agentmaster::SharedEngine().claudeExePath;
            _claudeMissingStatus.Text(winrt::hstring{ exe.empty() ? std::wstring{ L"Status: no native claude.exe detected." } : (L"Status: using " + exe) });
        }
        _claudeMissingOverlay.Visibility(Visibility::Visible);
    }

    void AgentManagerContent::_HideClaudeMissing()
    {
        if (_claudeMissingOverlay)
        {
            _claudeMissingOverlay.Visibility(Visibility::Collapsed);
        }
    }

    void AgentManagerContent::_BrowseForClaudeExe(bool fromSettings)
    {
        if (!_dispatcher)
        {
            return;
        }
        // Defer off the click tick, then run the COM modal (it needs the message pump — the XAML-Islands
        // rule the profile picker follows). A picked .exe becomes the persisted override + re-resolves.
        _dispatcher.TryEnqueue([this, fromSettings]() {
            const auto picked = PickClaudeExe(::GetActiveWindow());
            if (!picked || picked->empty())
            {
                return;
            }
            _appSettings.claudeExePath = *picked;
            if (_settingsSink)
            {
                _settingsSink(_appSettings); // persist the override
            }
            const auto exe = ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
            if (fromSettings && _setClaudeExePath)
            {
                _setClaudeExePath.Text(winrt::hstring{ _appSettings.claudeExePath });
            }
            if (fromSettings && _setClaudeDetected)
            {
                _setClaudeDetected.Text(winrt::hstring{ exe.empty() ? std::wstring{ L"Detected: none" } : (L"Detected: " + exe) });
            }
            if (!exe.empty())
            {
                _HideClaudeMissing();
                _ValidateLaunchBox();
            }
            else if (_claudeMissingStatus)
            {
                _claudeMissingStatus.Text(L"That file isn't a usable claude.exe \x2014 pick the native claude.exe.");
            }
        });
    }

    void AgentManagerContent::_RebuildPlan(const std::vector<SessionInfo>& sessions)
    {
        _planHeaderHost.Children().Clear();
        _planListHost.Children().Clear();

        // EXTERNAL scope: the Flight Plan is READ-ONLY (externals are observe-only — we host no
        // ConPTY, so nothing to drive). With an external selected, show its conversation's prompts;
        // with none selected, the nothing-selected hint.
        if (_treeScope == TreeScope::External)
        {
            _UpdateAutopilotButton(AutopilotMode::Off, false); // not drivable
            if (!_selectedExternalTitle.empty())
            {
                _RebuildExternalPlan();
            }
            else
            {
                _planHeaderHost.Children().Append(Text(L"External claudes are observe-only \x2014 click one in the tree to see its conversation (read-only), or right-click to Adopt / Open New Session Here / Bring Window To Front.", 13, false, 0.6));
                _PinPlanToBottomOnSubjectChange(L""); // nothing scrollable shown — re-arm for the next real selection
            }
            return;
        }

        const auto sel = _Selected(sessions);
        if (!sel || !sel->live) // an archived (closed) session isn't planned here — restore it first
        {
            _planHeaderHost.Children().Append(Text(L"Select a session to plan its prompts.", 13, false, 0.6));
            _UpdateAutopilotButton(AutopilotMode::Off, false); // no live session: dim the header toggle
            _PinPlanToBottomOnSubjectChange(L""); // re-arm so re-selecting a session pins to bottom again
            return;
        }

        // header
        auto titleRow = StackPanel{};
        titleRow.Orientation(Orientation::Horizontal);
        titleRow.Spacing(8);
        titleRow.Children().Append(Text(OneLine(sel->title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ sel->title }), 16, true, 1.0));
        {
            auto statePill = Pill(StateLabel(sel->state), StateColor(sel->state));
            AgentSetTip(statePill, L"Current state \x2014 this session's Triage state (hover a Triage Board column header for what each state means).");
            titleRow.Children().Append(statePill);
        }
        _planHeaderHost.Children().Append(titleRow);
        _planHeaderHost.Children().Append(Text(winrt::hstring{ sel->workingDir }, 12, false, 0.6));

        // reflect autopilot mode on the header toggle
        _UpdateAutopilotButton(sel->autopilot.mode, true);

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
            AgentSetTip(sendBtn, L"Semi-auto: send the next queued prompt that Autopilot armed.");
            sendBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                if (_confirmHandler && !_selectedId.empty())
                {
                    _confirmHandler(winrt::hstring{ _selectedId }, true);
                }
            });
            auto skipBtn = Button{};
            skipBtn.Content(winrt::box_value(L"Skip"));
            AgentSetTip(skipBtn, L"Semi-auto: skip this armed prompt without sending it.");
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
            _PinPlanToBottomOnSubjectChange(L""); // re-arm: the first message that arrives will pin to bottom
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
                auto originPill = Pill(typed ? winrt::hstring{ L"typed" } : winrt::hstring{ L"flight" },
                                       typed ? ColorHelper::FromArgb(0xFF, 0xD9, 0xA6, 0x2E) : ColorHelper::FromArgb(0xFF, 0x4F, 0x8B, 0xD0));
                AgentSetTip(originPill, typed ?
                                            winrt::hstring{ L"Typed \x2014 you typed this prompt straight into the terminal." } :
                                            winrt::hstring{ L"Flight \x2014 Agentmaster queued this prompt and sent it for you (Autopilot or Send now)." });
                row.Children().Append(originPill);
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
                _NotifyLensChanged(); // M10
                _Refresh();
            });
            // Right-click menu: Copy (this prompt's text) on every row, plus queue ops (Move up /
            // Move down / Delete) on UPCOMING rows only (a sent/historical row can't be reordered).
            // showOrigin is true for the SENT summary, false for the UPCOMING queue.
            rowBtn.ContextFlyout(_MakePromptMenu(pid, !showOrigin));
            AgentSetTip(rowBtn, showOrigin ?
                                    winrt::hstring{ L"A message this session already received \x2014 right-click to copy it." } :
                                    winrt::hstring{ L"A queued prompt \x2014 click to select it; right-click to copy, move or delete it." });
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
        // Default the view to the bottom (latest sent + the upcoming queue) the first time this
        // session's plan is shown — but not on a same-session _Refresh (keep the user's scroll).
        _PinPlanToBottomOnSubjectChange(sel->id);
    }

    // Agentmaster: the READ-ONLY Flight Plan for a selected EXTERNAL session — its conversation's
    // human prompts (read from the transcript on a background thread by _LoadExternalPlan). No
    // queue, no actions, no Autopilot: an external runs outside Agentmaster and we never drive it
    // (Rule #9/#13). The header offers Adopt as the path to make it controllable.
    void AgentManagerContent::_RebuildExternalPlan()
    {
        const bool isCodex = (_selectedExternalKind == ::Agentmaster::AgentKind::Codex);
        auto titleRow = StackPanel{};
        titleRow.Orientation(Orientation::Horizontal);
        titleRow.Spacing(8);
        titleRow.Children().Append(Text(_selectedExternalTitle.empty() ? winrt::hstring{ isCodex ? L"codex" : L"claude" } : winrt::hstring{ _selectedExternalTitle }, 16, true, 1.0));
        if (isCodex)
        {
            auto op = Pill(L"codex \x00B7 observe-only", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
            AgentSetTip(op, L"Observe-only \x2014 a Codex session running outside Agentmaster: you can read its conversation but not drive it. Adopt it to take control.");
            titleRow.Children().Append(op);
        }
        else
        {
            auto op = Pill(L"external \x00B7 observe-only", Colors::Gray());
            AgentSetTip(op, L"Observe-only \x2014 a Claude session running outside Agentmaster: you can read its conversation but not drive it. Adopt it to take control.");
            titleRow.Children().Append(op);
        }
        _planHeaderHost.Children().Append(titleRow);
        if (!_selectedExternalCwd.empty())
        {
            _planHeaderHost.Children().Append(Text(winrt::hstring{ _selectedExternalCwd }, 12, false, 0.6));
        }
        // Observe-only here (we host no ConPTY). Both agents can be adopted from the tree's right-click
        // menu — Adopt offers Fork a copy (safe while the original runs) or Resume the same conversation.
        _planHeaderHost.Children().Append(Text(isCodex ? winrt::hstring{ L"Read-only \x2014 an OpenAI Codex session running outside Agentmaster. Right-click it in the tree and \x201C" L"Adopt\x201D to bring its conversation under management (Fork a copy, or Resume)." } : winrt::hstring{ L"Read-only \x2014 runs outside Agentmaster. Right-click it in the tree and \x201C" L"Adopt\x201D to bring its conversation under management (Fork a copy, or Resume)." }, 11, false, 0.5));

        // Agentmaster: the idle RECAP (away_summary) above the prompts — the same ">5-min what we did /
        // what's next" synthesis the MANAGED summary box shows, here for an OBSERVE-ONLY external. The
        // recap rides ExternalClaudeRow.recap, filled by the Fleet Observer from the transcript TAIL —
        // the SAME region the SessionScanner pulls a managed session's recap from (an external has no
        // scanner cursor, so the observer is its provider; see ProcessObserver). Looked up live by the
        // selected id from the cached rows, so it refreshes as the observer re-tails it. Shown in FULL
        // (wrapping), never length-capped, matching the managed summary box's "Recap:" section.
        for (const auto& exr : _externalClaudes)
        {
            if (exr.sessionId == _selectedExternalSessionId && !exr.recap.empty())
            {
                auto recapText = Text(winrt::hstring{ L"Recap: " + exr.recap }, 12, false, 0.85);
                recapText.TextWrapping(TextWrapping::Wrap);
                recapText.TextTrimming(TextTrimming::None);
                recapText.Margin(Thickness{ 0, 6, 0, 0 });
                AgentSetTip(recapText, L"Claude Code's idle recap (away_summary) \x2014 a >5-min \x201C" L"what we did / what's next\x201D synthesis, read from this session's transcript tail.");
                _planHeaderHost.Children().Append(recapText);
                break;
            }
        }

        if (_selectedExternalSessionId.empty())
        {
            _planListHost.Children().Append(Text(L"This external session hasn't been prompted yet \x2014 no conversation to show.", 12, false, 0.6));
            _PinPlanToBottomOnSubjectChange(L""); // nothing scrollable
            return;
        }
        if (_externalPlanLoadedFor != _selectedExternalSessionId)
        {
            _planListHost.Children().Append(Text(L"Loading conversation\x2026", 12, false, 0.6));
            _PinPlanToBottomOnSubjectChange(L""); // re-arm: pin once the prompts finish loading (the deferred render reaches the bottom call below)
            return;
        }
        if (_externalPlanPrompts.empty())
        {
            _planListHost.Children().Append(Text(L"No human prompts found in this conversation.", 12, false, 0.6));
            _PinPlanToBottomOnSubjectChange(L""); // nothing scrollable
            return;
        }

        const size_t total = _externalPlanPrompts.size();
        const size_t cap = 300; // bound the XAML we build for a very long conversation
        const size_t startIdx = (total > cap) ? (total - cap) : 0;
        {
            std::wstring capn = L"PROMPTS \x2014 " + std::to_wstring(total);
            if (startIdx > 0)
            {
                capn += L"  (showing last " + std::to_wstring(cap) + L")";
            }
            auto c = Text(winrt::hstring{ capn }, 11, true, 0.5);
            c.Margin(Thickness{ 0, 0, 0, 4 });
            _planListHost.Children().Append(c);
        }
        for (size_t i = startIdx; i < total; ++i)
        {
            const std::wstring& p = _externalPlanPrompts[i];
            std::wstring oneLine = p;
            const auto nl = oneLine.find_first_of(L"\r\n");
            if (nl != std::wstring::npos)
            {
                oneLine = oneLine.substr(0, nl);
            }
            if (oneLine.size() > 200)
            {
                oneLine = oneLine.substr(0, 197) + L"\x2026";
            }

            auto rowSp = StackPanel{};
            rowSp.Orientation(Orientation::Horizontal);
            rowSp.Spacing(8);
            rowSp.Children().Append(Text(winrt::to_hstring(static_cast<int>(i + 1)), 11, false, 0.4));
            auto lbl = Text(winrt::hstring{ oneLine }, 13, false, 0.95);
            lbl.MaxWidth(360);
            rowSp.Children().Append(lbl);

            auto rowBorder = Border{};
            rowBorder.Padding(Thickness{ 6, 3, 6, 3 });
            rowBorder.Margin(Thickness{ 0, 0, 0, 4 });
            rowBorder.Background(Fill(0x14, 0x80, 0x80, 0x80));
            rowBorder.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            rowBorder.Child(rowSp);
            _planListHost.Children().Append(rowBorder);
        }
        _PinPlanToBottomOnSubjectChange(L"x:" + _selectedExternalSessionId);
    }

    // Agentmaster: scroll the Flight Plan to the bottom the FIRST time a subject is shown (a managed
    // session's plan, or an external's read-only conversation). The newest SENT message + the UPCOMING
    // queue live at the bottom, so a freshly-opened plan defaults to "where things stand" rather than
    // the oldest message. Gated on the subject CHANGING — a same-subject _Refresh (a background state
    // change, claude floating its OSC title, a queue edit) must keep the user's current scroll, so we
    // never re-pin while you're reading history. The scroll is deferred to a clean tick and forces a
    // layout pass first, because _planListHost was just (re)populated this frame and the ScrollViewer's
    // ScrollableHeight isn't valid until it re-measures.
    void AgentManagerContent::_PinPlanToBottomOnSubjectChange(const std::wstring& subjectKey)
    {
        if (subjectKey == _planAutoScrolledFor)
        {
            return; // same subject as last pin (or both empty) — leave the user's scroll alone
        }
        _planAutoScrolledFor = subjectKey;
        if (subjectKey.empty() || !_planScroll)
        {
            return; // nothing scrollable shown (a hint / loading / empty state)
        }
        auto weak = get_weak();
        auto disp = _dispatcher;
        if (!disp)
        {
            return;
        }
        disp.TryEnqueue([weak]() {
            auto self = weak.get();
            if (!self || !self->_planScroll)
            {
                return;
            }
            self->_planScroll.UpdateLayout(); // realize the rows appended this frame so ScrollableHeight is real
            self->_planScroll.ChangeView(nullptr, self->_planScroll.ScrollableHeight(), nullptr, true); // jump (no animation) to the bottom
        });
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
        // Agentmaster: selecting a managed session — by a card/row click OR a tab switch (the page's
        // SelectSession() routes here) — drops that session's working directory into the Launch box, so
        // "Launch Claude" / Open-New-Session is pre-aimed where you're working. Done BEFORE the
        // already-selected early-out so a re-select re-aims it. The box is not focused during a card
        // click / tab switch, so its TextChanged path-picker logic early-outs (it never pops the
        // dropdown). We must re-validate EXPLICITLY here, not lean on TextChanged: a programmatic
        // _cwdBox.Text() set lands while the Manager content is OFF the live visual tree (a tab switch
        // makes another tab active, and MUX TabView hosts only the selected tab's content). The Text DP
        // value persists (you see the right path on return), but TextChanged does NOT reliably fire while
        // detached — so without this call the underline/button stay frozen on the previously-typed
        // value's RED/disabled state even though a valid working dir now shows (the reported bug).
        if (_cwdBox && _registry && !id.empty())
        {
            if (const auto s = _registry->Get(id); s && !s->workingDir.empty())
            {
                _cwdBox.Text(winrt::hstring{ s->workingDir });
                _ValidateLaunchBox();
            }
        }

        // Selecting a managed session clears any external (read-only) selection — the Flight Plan is
        // one surface; a managed selection wins (it is drivable).
        const bool hadExternal = !_selectedExternalSessionId.empty();
        if (_selectedId == id && !hadExternal)
        {
            return;
        }
        if (_selectedId != id)
        {
            _ResetPromptHistory(); // prompt history is per-session — a new session starts fresh
        }
        _selectedExternalSessionId.clear();
        _selectedExternalCwd.clear();
        _selectedExternalTitle.clear();
        _selectedId = id;
        _selectedPromptId.clear();
        _NotifyLensChanged(); // M10: selection is part of the per-window lens
        _Refresh();
    }

    // Agentmaster: the board header's "Clear" button — deselect whatever managed session OR external is
    // selected (the Flight Plan then shows nothing-selected). A no-op when nothing is selected. Clears
    // BOTH selection kinds at once (the Launch box is left as-is — it is an independent launch target).
    void AgentManagerContent::_ClearSelection()
    {
        if (_selectedId.empty() && _selectedExternalSessionId.empty())
        {
            return; // nothing selected
        }
        _ResetPromptHistory(); // no session selected -> no history to recall
        _selectedId.clear();
        _selectedPromptId.clear();
        _selectedExternalSessionId.clear();
        _selectedExternalCwd.clear();
        _selectedExternalTitle.clear();
        _NotifyLensChanged(); // selection is part of the per-window lens
        _Refresh();
    }

    // Agentmaster (Linked Lenses): a managed board card / tree row was entered or left by the
    // pointer. Forward the raw event (id, entering) to the page, which owns the effective-hover
    // bookkeeping (the enter-B-before-leave-A matching + the "clear on leaving the Manager tab"
    // reset) — so this control holds no hover state to desync from the page. The page pills that
    // session's terminal tab while the Manager tab is active. NOT part of the lens — hover is
    // transient and per-pointer, never persisted.
    void AgentManagerContent::_ReportHover(const std::wstring& id, bool entering)
    {
        if (_hoverSessionHandler)
        {
            _hoverSessionHandler(winrt::hstring{ id }, entering);
        }
    }

    // Agentmaster: select an EXTERNAL (observe-only) row -> the Flight Plan shows its conversation
    // READ-ONLY. We host no ConPTY for it (Rule #9/#13), so this never binds an injector; it only
    // surfaces what was prompted. Clears the managed selection (one Flight-Plan surface).
    void AgentManagerContent::_SelectExternal(const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& title, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath)
    {
        _ResetPromptHistory(); // an external's Flight Plan is read-only — no managed history to recall
        // Nav audit: the user selected an EXTERNAL (unmanaged) session to inspect — board External
        // card or Explorer-Tree EXTERNAL row — surfacing its read-only conversation. Distinct from a
        // managed select (no tab-focus equivalent — we host no tab for it).
        ::Agentmaster::LogNav(L"manager select-external " + ::Agentmaster::ShortId(sessionId) + L" cwd=" + cwd);
        _selectedId.clear();
        _selectedPromptId.clear();
        _selectedExternalSessionId = sessionId;
        _selectedExternalCwd = cwd;
        _selectedExternalTitle = title;
        // Agentmaster: like a managed select, aim the Launch box at this external's cwd (so Launch /
        // Open-New-Session here is one keystroke). Unfocused box => no path-picker pop, and re-validate
        // EXPLICITLY — a programmatic Text set on the (detached, non-active-tab) Manager content does not
        // reliably raise TextChanged, so the launch underline/button would otherwise stay stuck on a
        // previously-typed invalid value's RED/disabled state (see _SelectSession).
        if (_cwdBox && !cwd.empty())
        {
            _cwdBox.Text(winrt::hstring{ cwd });
            _ValidateLaunchBox();
        }
        _selectedExternalKind = kind; // Phase C1: the read-only plan reader (Claude transcript vs Codex rollout)
        _selectedExternalRolloutPath = rolloutPath;
        // Linked Lenses: selecting an external — from the Explorer Tree OR a Triage-Board External
        // card — puts all three regions in agreement. Switch the tree to EXTERNAL so it lists the
        // externals with this one highlighted, and the Flight Plan renders its read-only conversation
        // (its render is gated on EXTERNAL scope, see _RebuildPlan). A no-op when invoked from the
        // tree (already EXTERNAL); the meaningful case is a board card click from LOCAL/GLOBAL.
        if (_treeScope != TreeScope::External)
        {
            _treeScope = TreeScope::External;
            _UpdateTreeScopeButton();
            _UpdateBoardScopeButton(); // keep the board's 2-way toggle in step (External reads GLOBAL there) — it shares the one scope state
        }
        _LoadExternalPlan(sessionId, cwd, kind, rolloutPath); // kicks off the (cached) background transcript/rollout read
        _NotifyLensChanged();
        _Refresh();
    }

    // Read the external conversation's human prompts on a BACKGROUND thread (a transcript can be
    // multi-MB; never parse it on the UI thread), then post the result back via the dispatcher. Cached
    // per id (_externalPlanLoadedFor) so re-selecting the same external doesn't re-read. A session
    // with no transcript yet (empty id) loads nothing (the plan shows "not prompted yet").
    void AgentManagerContent::_LoadExternalPlan(const std::wstring& sessionId, const std::wstring& cwd, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath)
    {
        if (_externalPlanLoadedFor == sessionId && !sessionId.empty())
        {
            return; // already loaded for this id
        }
        _externalPlanPrompts.clear();
        _externalPlanLoadedFor.clear(); // empty => loading / none
        if (sessionId.empty())
        {
            return; // never-prompted external: nothing to read
        }
        auto weak = get_weak();
        auto disp = _dispatcher;
        std::thread([weak, disp, sessionId, cwd, kind, rolloutPath]() {
            // Whole transcript (maxBytes 0), cap the prompt count so a giant conversation stays
            // bounded. Codex (Phase C1) reads its date-sharded rollout (the path carried on the row);
            // Claude reads <projects>/<encode(cwd)>/<id>.jsonl. Both yield the human prompts in order.
            std::vector<std::wstring> prompts = (kind == ::Agentmaster::AgentKind::Codex)
                                                    ? ::Agentmaster::ReadCodexRolloutInfo(rolloutPath, 0, 1000).userPrompts
                                                    : ::Agentmaster::ReadTranscriptInfo(cwd, sessionId, 0, 1000).userPrompts;
            if (!disp)
            {
                return;
            }
            disp.TryEnqueue([weak, sessionId, prompts = std::move(prompts)]() mutable {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                // Stale guard: the user may have selected a different external while we were reading.
                if (self->_selectedExternalSessionId != sessionId)
                {
                    return;
                }
                self->_externalPlanLoadedFor = sessionId;
                self->_externalPlanPrompts = std::move(prompts);
                self->_Refresh();
            });
        }).detach();
    }

    void AgentManagerContent::_SetScope(const std::wstring& dir)
    {
        _scopeDir = dir;
        _NotifyLensChanged(); // M10: scope + collapsed-dir toggles funnel through here
        _Refresh();
    }

    // ---- action handlers ----------------------------------------------------

    // Agentmaster (Codex-launch): repaint the launch-bar agent toggle from _launchCodex. Codex wears the
    // SAME teal (0xFF4EC9B0) as every other codex surface (the EXTERNAL pill, the managed Board/tree pill),
    // so the agent reads one color everywhere; Claude is a neutral blue accent (the default, unchanged).
    void AgentManagerContent::_UpdateLaunchAgentButton()
    {
        if (!_launchAgentBtn)
        {
            return;
        }
        const bool codex = _launchCodex;
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(6);
        auto g = Text(L"\x25CF", 11, true, 1.0); // ●
        g.Foreground(SolidColorBrush{ codex ? Color{ 0xFF, 0x4E, 0xC9, 0xB0 } : Color{ 0xFF, 0x4F, 0x9C, 0xFF } });
        row.Children().Append(g);
        row.Children().Append(Text(codex ? L"Codex" : L"Claude", 11, false, 0.95));
        _launchAgentBtn.Content(row);
        _ReflowLaunchBar(); // the toggle width changed (Claude<->Codex) -> re-fit the row
    }

    void AgentManagerContent::_OnLaunch()
    {
        if (!_cwdBox)
        {
            return;
        }
        _NormalizeCwdBox(); // launch with — and remember — a normalized path (a session id is unaffected)
        std::wstring text{ _cwdBox.Text() };
        { // trim surrounding whitespace so the launch target matches exactly what _ValidateLaunchBox judged
            const auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
            while (!text.empty() && isws(text.front())) { text.erase(text.begin()); }
            while (!text.empty() && isws(text.back())) { text.pop_back(); }
        }
        // Agentmaster (Codex-launch): a Codex launch is DIRECTORY-ONLY — codex has no typed-id resume/fork
        // here (Codex resume is reached via the Archive page / window-restore / EXTERNAL Adopt), and while
        // Codex is selected _ValidateLaunchBox keeps the box in directory semantics. Spawn a managed codex in
        // that dir, mirroring the EXTERNAL menu's "Open New Codex Session Here" (pid 0 = no source external,
        // adopt=false = a fresh independent session). _LaunchCodexSession registers the card immediately.
        if (_launchCodex)
        {
            if (_codexLaunchHandler)
            {
                if (!_EnsureLaunchDirExists(text)) // "Create & Launch": make the folder first if it doesn't exist yet
                {
                    return; // creation failed (already warned) — don't launch into a missing dir
                }
                _PushRecentDir(text); // remember it as "recently selected" (shared MRU with Claude launches)
                _ClosePathPicker();
                _codexLaunchHandler(0, winrt::hstring{ text }, false, false); // fresh launch: adopt=false, fork=false
            }
            return;
        }
        // Native-exe-only policy: every CLAUDE interaction (new session or resume) needs a native
        // claude.exe. With none detected, show the install/Browse modal instead of launching. (Codex
        // launches above are a separate runtime and are not gated on claude.exe.)
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _ShowClaudeMissing();
            return;
        }
        // A session id in the box = resume that conversation. The button reads "Resume session" and
        // is only ENABLED when the id was FOUND (_ValidateLaunchBox), so this path is reachable only
        // for a real on-disk transcript; resolve its dir/title and hand off to the page.
        if (const auto sid = LooksLikeSessionId(text))
        {
            std::wstring dir, title;
            if (_resumeSessionHandler && _ResolveSessionDirTitle(*sid, dir, title))
            {
                _ClosePathPicker();
                _resumeSessionHandler(winrt::hstring{ *sid }, winrt::hstring{ dir }, winrt::hstring{ title });
                _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after resume
                _ValidateLaunchBox(); // re-validate explicitly: the resume handler just opened+selected a new tab, so the Manager content is detached and TextChanged won't reliably fire to reset the underline/buttons
            }
            return;
        }
        // A working directory = a new, independent session there.
        if (_spawnHandler)
        {
            if (!_EnsureLaunchDirExists(text)) // "Create & Launch Claude": make the folder first if it doesn't exist yet
            {
                return; // creation failed (already warned) — don't launch into a missing dir
            }
            _PushRecentDir(text); // remember it as "recently selected"
            _ClosePathPicker();
            _spawnHandler(winrt::hstring{ text }, winrt::hstring{});
        }
    }

    // Agentmaster: ensure the launch target directory exists, creating it (and any missing parents)
    // when the user typed a not-yet-existing absolute path — the "Create & Launch" affordance. The
    // launch button only reads "Create & Launch …" (and only enables for a missing path) when
    // _ValidateLaunchBox judged the path creatable (LooksLikeCreatableDir), so this is the matching
    // commit step. Returns true if the dir exists (already, or after a successful create) and the
    // launch may proceed; false if creation failed (a buttons-only error is shown and the caller
    // aborts). An already-existing dir is a no-op pass-through; a non-creatable path can't reach here
    // (the button is disabled for it) but is rejected defensively.
    bool AgentManagerContent::_EnsureLaunchDirExists(const std::wstring& dir)
    {
        const std::wstring norm = NormPath(dir);
        if (IsDir(norm))
        {
            return true; // already there — nothing to create
        }
        if (!LooksLikeCreatableDir(norm))
        {
            return false; // not an absolute path we offered to create (button is disabled for this)
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ norm }, ec);
        if (ec || !IsDir(norm))
        {
            // Surface the failure (permission denied, a missing drive, a path too long, …) instead of
            // silently doing nothing after the user clicked "Create & Launch". Buttons-only ContentDialog
            // (a text box inside one gets no keypresses in XAML Islands, but an info dialog needs none).
            ContentDialog dialog;
            dialog.Title(winrt::box_value(L"Couldn't create folder"));
            dialog.Content(winrt::box_value(winrt::hstring{ L"Could not create the working directory:\n\n" } + winrt::hstring{ norm }));
            dialog.CloseButtonText(L"OK");
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
            try
            {
                dialog.ShowAsync();
            }
            catch (...)
            {
            }
            return false;
        }
        return true;
    }

    // Agentmaster: the Fork button (shown only for a FOUND session id) -> fork that conversation
    // into a NEW one in the same dir (the page's _ForkSessionFromDisk: claude --resume --fork-session).
    void AgentManagerContent::_OnForkFromBox()
    {
        if (!_cwdBox)
        {
            return;
        }
        // Native-exe-only policy: a fork is a claude launch -> requires a native claude.exe.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _ShowClaudeMissing();
            return;
        }
        const auto sid = LooksLikeSessionId(std::wstring{ _cwdBox.Text() });
        if (!sid) // the button is hidden for non-session-id input, but guard anyway
        {
            return;
        }
        std::wstring dir, title;
        if (_forkSessionHandler && _ResolveSessionDirTitle(*sid, dir, title))
        {
            _ClosePathPicker();
            _forkSessionHandler(winrt::hstring{ *sid }, winrt::hstring{ dir }, winrt::hstring{ title });
            _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after fork
            _ValidateLaunchBox(); // re-validate explicitly: the fork handler just opened+selected a new tab, so the Manager content is detached and TextChanged won't reliably fire to reset the underline/buttons
        }
    }

    // Agentmaster: paint the Launch box's validation underline + drive the launch/fork buttons.
    // EMPTY -> neutral, "Launch Claude" enabled (defaults). A UUID -> session id: FOUND = green +
    // "Resume session" enabled + Fork shown; NOT found = red + disabled. Otherwise a directory:
    // EXISTS = neutral + "Launch Claude" enabled; a not-yet-existing ABSOLUTE path = amber +
    // "Create & Launch Claude" enabled (the folder is created on launch); a malformed / relative path
    // = red + disabled. (Per the design: green is reserved for a found session id; a valid folder
    // stays neutral; amber flags a folder that will be created.)
    void AgentManagerContent::_ValidateLaunchBox()
    {
        if (!_cwdBox || !_launchBtn)
        {
            return;
        }
        // Agentmaster (responsive launch bar): the launch button's text + the Fork button's visibility
        // change below (Launch Claude / Create & Launch Claude / Resume session / +Fork / the Codex twins),
        // which changes the launch-buttons group width — re-fit the row on EVERY exit path.
        auto reflowOnExit = wil::scope_exit([this]() noexcept { _ReflowLaunchBar(); });
        std::wstring trimmed{ _cwdBox.Text() };
        const auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (!trimmed.empty() && isws(trimmed.front()))
        {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && isws(trimmed.back()))
        {
            trimmed.pop_back();
        }

        const auto paint = [this](int state) { // 0 neutral (hidden), 1 green, 2 red, 3 amber (will create)
            if (!_cwdUnderline)
            {
                return;
            }
            // transparent (neutral) / green (found id) / red (missing dir or unknown id) / amber (a
            // not-yet-existing dir we'll CREATE on launch); kept always present (Height 2) so toggling
            // color never reflows the toolbar.
            const SolidColorBrush b = state == 1 ? Fill(0xFF, 0x4C, 0xAF, 0x50) :
                                      state == 2 ? Fill(0xFF, 0xE5, 0x39, 0x35) :
                                      state == 3 ? Fill(0xFF, 0xDA, 0xA5, 0x20) :
                                                   Fill(0x00, 0x00, 0x00, 0x00);
            _cwdUnderline.Background(b);
        };
        const auto showFork = [this](bool v) {
            if (_forkBtn)
            {
                _forkBtn.Visibility(v ? Visibility::Visible : Visibility::Collapsed);
            }
        };

        // Agentmaster (Codex-launch): while Codex is the selected agent the box is DIRECTORY-ONLY — codex
        // has no --session-id, so there is no typed-id resume (the green found-id state) and no Fork. Empty
        // or an existing dir => enabled "Launch Codex" (neutral underline); a not-yet-existing absolute path
        // => amber + "Create & Launch Codex"; a malformed / relative path => red + disabled.
        if (_launchCodex)
        {
            showFork(false);
            if (trimmed.empty())
            {
                paint(0);
                _launchBtn.IsEnabled(true);
                _launchBtn.Content(winrt::box_value(L"Launch Codex"));
            }
            else
            {
                // An existing dir launches as-is; a not-yet-existing absolute path flips to amber +
                // "Create & Launch Codex" (made on launch); a malformed/relative path stays red+disabled.
                const std::wstring norm = NormPath(trimmed);
                const bool exists = IsDir(norm);
                const bool creatable = !exists && LooksLikeCreatableDir(norm);
                paint(exists ? 0 : (creatable ? 3 : 2));
                _launchBtn.IsEnabled(exists || creatable);
                _launchBtn.Content(winrt::box_value(creatable ? L"Create & Launch Codex" : L"Launch Codex"));
            }
            return;
        }

        if (trimmed.empty())
        {
            paint(0);
            _launchBtn.IsEnabled(true);
            _launchBtn.Content(winrt::box_value(L"Launch Claude"));
            showFork(false);
            return;
        }
        if (const auto sid = LooksLikeSessionId(trimmed))
        {
            const bool found = ::Agentmaster::ClaudeConversationExists(*sid);
            paint(found ? 1 : 2);
            _launchBtn.IsEnabled(found);
            _launchBtn.Content(winrt::box_value(L"Resume session"));
            showFork(found);
            return;
        }
        // a working directory: green is reserved for session ids, so a valid dir stays neutral. A
        // not-yet-existing absolute path flips to amber + "Create & Launch Claude" (_OnLaunch makes the
        // folder first); a malformed / relative path that we won't create stays red + disabled.
        const std::wstring norm = NormPath(trimmed);
        const bool exists = IsDir(norm);
        const bool creatable = !exists && LooksLikeCreatableDir(norm);
        paint(exists ? 0 : (creatable ? 3 : 2));
        _launchBtn.IsEnabled(exists || creatable);
        _launchBtn.Content(winrt::box_value(creatable ? L"Create & Launch Claude" : L"Launch Claude"));
        showFork(false);
    }

    // Agentmaster: resolve a session id to its (working dir, title) for resume / fork. The registry
    // knows a managed (live or archived) session directly; otherwise read the cwd straight off the
    // on-disk transcript (the project FOLDER name is a lossy encoding, so read the line's real cwd).
    // Title is left empty in the transcript case — the resume/fork seam derives a smart name from the
    // dir. Returns false if no dir could be found (caller no-ops).
    bool AgentManagerContent::_ResolveSessionDirTitle(const std::wstring& id, std::wstring& dir, std::wstring& title)
    {
        dir.clear();
        title.clear();
        if (_registry)
        {
            if (const auto info = _registry->Get(id); info && !info->workingDir.empty())
            {
                dir = info->workingDir;
                title = info->title;
                return true;
            }
        }
        const std::wstring path = ::Agentmaster::ResolveClaudeTranscriptPath(id);
        if (path.empty())
        {
            return false;
        }
        const auto facts = ::Agentmaster::ReadTranscriptQuickFacts(path, 0);
        dir = facts.cwd;
        return !dir.empty();
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
        // Nav audit: the user QUEUED a prompt to this session (the envelope). The first line is the
        // identifying context; Autopilot/Send-now later consumes it (scheduler [send]/[confirm-send]).
        ::Agentmaster::LogNav(L"queue " + ::Agentmaster::ShortId(id) + L" \"" + label + L"\"");
        _addPromptBox.Text(L"");
        _Refresh();
        _FocusPromptBox(); // Agentmaster: keep focus in the editor so the user can queue the next prompt
    }

    void AgentManagerContent::_OnSendNow()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }
        // The "!" button ASKS before it fires — "Send now" injects into a live claude immediately.
        // Build a short preview of what WILL be sent (the compose box if non-empty, else the
        // selected / first-Pending queued prompt — the same target _DoSendNow picks), then confirm.
        std::wstring preview = _addPromptBox ? std::wstring{ _addPromptBox.Text() } : std::wstring{};
        if (preview.empty())
        {
            if (auto s = _registry->Get(_selectedId))
            {
                const QueuedPrompt* target = nullptr;
                if (!_selectedPromptId.empty())
                {
                    for (const auto& p : s->queue)
                    {
                        if (p.id == _selectedPromptId)
                        {
                            target = &p;
                            break;
                        }
                    }
                }
                if (!target)
                {
                    for (const auto& p : s->queue)
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
                    preview = target->label.empty() ? target->text : target->label;
                }
            }
        }
        if (preview.empty())
        {
            return; // nothing composed and nothing pending to send
        }
        std::wstring shown = preview.substr(0, 200);
        std::replace(shown.begin(), shown.end(), L'\n', L' ');
        std::replace(shown.begin(), shown.end(), L'\r', L' ');
        auto weak = get_weak();
        _Confirm(L"Send now?",
                 winrt::hstring{ L"Send this prompt to the session right now?\n\n\x201C" } + winrt::hstring{ shown } + winrt::hstring{ L"\x201D" },
                 L"Send",
                 [weak]() { if (auto self = weak.get()) { self->_DoSendNow(); } });
    }

    // The actual inject for "Send now" (runs after the confirm).
    void AgentManagerContent::_DoSendNow()
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
        std::wstring sentPromptId; // Agentmaster: the prompt just marked Sent, for rollback on a failed inject
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
                sentPromptId = p.id; // remember it so a failed inject can roll it back (Rule #4)
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
                    sentPromptId = target->id; // remember it so a failed inject can roll it back (Rule #4)
                    target->status = PromptStatus::Sent;
                    target->attempts += 1;
                    target->sentAtUnixMs = NowMs();
                    target->echoed = false; // await this injection's UserPromptSubmit echo
                    target->enterRetries = 0; // fresh send -> reset the scheduler's Enter-retry watch
                }
            });
        }

        if (!textToSend.empty())
        {
            // Inject + submit via a bracketed paste so a multi-line body lands as ONE message
            // (BuildPromptSubmission, #6) instead of submitting on the first embedded line break.
            // Agentmaster: check the result and roll the prompt back to Pending on a
            // failed inject. "Send now" marked it Sent above; if the selected session has no stdin
            // injector bound (not a live/bound tab yet, or an observe-only external), injecting fails
            // and the prompt would otherwise be a stranded phantom Sent that was never delivered
            // (Correctness Rule #4). Reverting to Pending keeps it in the queue to retry.
            const bool delivered = _registry->Inject(_selectedId, ::Agentmaster::BuildPromptSubmission(textToSend));
            // Nav audit: the user hit Send-now (the !) for this session — the prompt's first line +
            // whether it actually reached a bound injector (an unbound/observe-only target rolls back).
            std::wstring snLabel = textToSend.substr(0, 56);
            std::replace(snLabel.begin(), snLabel.end(), L'\n', L' ');
            std::replace(snLabel.begin(), snLabel.end(), L'\r', L' ');
            ::Agentmaster::LogNav(L"send-now " + ::Agentmaster::ShortId(_selectedId) + L" \"" + snLabel + L"\"" + (delivered ? L"" : L" (no injector \x2014 rolled back to Pending)"));
            if (!delivered && !sentPromptId.empty())
            {
                _registry->Update(_selectedId, [&](SessionInfo& s) {
                    for (auto& p : s.queue)
                    {
                        if (p.id == sentPromptId && p.status == PromptStatus::Sent)
                        {
                            p.status = PromptStatus::Pending;
                            p.echoed = false;
                            if (p.attempts > 0)
                            {
                                p.attempts -= 1;
                            }
                            break;
                        }
                    }
                });
            }
        }
        _Refresh();
        _FocusPromptBox(); // Agentmaster: return focus to the editor so the user can keep composing
    }

    // Agentmaster: return keyboard focus to the compose box after a queue/send — clicking the icon
    // button moved focus to it, so this lets the user immediately type the next prompt. Programmatic
    // focus places the caret in the box. A no-op if the box isn't present.
    void AgentManagerContent::_FocusPromptBox()
    {
        if (!_addPromptBox)
        {
            return;
        }
        // Defer the focus to the dispatcher. When this follows the Send-now ContentDialog confirm, the
        // dialog restores focus to its pre-open element (the "!" button) AS it closes — which happens
        // AFTER _DoSendNow (the PrimaryButtonClick callback) returns — so a synchronous Focus() here
        // would be clobbered. A queued focus runs after the click handler unwinds, so the compose box
        // keeps it. (The queue path has no dialog, so this is just a harmless one-tick delay there.)
        if (_dispatcher)
        {
            auto weak = get_weak();
            _dispatcher.TryEnqueue([weak]() {
                auto self = weak.get();
                if (self && self->_addPromptBox)
                {
                    self->_addPromptBox.Focus(FocusState::Programmatic);
                }
            });
        }
        else
        {
            _addPromptBox.Focus(FocusState::Programmatic);
        }
    }

    // Agentmaster (prompt history): the selected session's previously SENT prompts (Flight + Typed),
    // newest first, with consecutive duplicates collapsed (the shell HISTCONTROL=ignoredups idiom).
    // Pending/Held queue items are the FUTURE, not history, so they're excluded; only status==Sent
    // (which both an injected Flight prompt and a Typed-into-the-terminal capture carry) is history.
    std::vector<std::wstring> AgentManagerContent::_BuildPromptHistory() const
    {
        std::vector<std::wstring> out;
        if (_selectedId.empty() || !_registry)
        {
            return out;
        }
        const auto sel = _registry->Get(_selectedId);
        if (!sel)
        {
            return out;
        }
        std::vector<const QueuedPrompt*> sent;
        for (const auto& p : sel->queue)
        {
            if (p.status == PromptStatus::Sent && !p.text.empty())
            {
                sent.push_back(&p);
            }
        }
        // Newest first (descending send time); stable so equal-stamp items keep their queue order.
        std::stable_sort(sent.begin(), sent.end(), [](const QueuedPrompt* a, const QueuedPrompt* b) {
            return a->sentAtUnixMs > b->sentAtUnixMs;
        });
        for (const auto* p : sent)
        {
            if (out.empty() || out.back() != p->text)
            {
                out.push_back(p->text);
            }
        }
        return out;
    }

    // Agentmaster (prompt history): write a recalled body into the compose box and park the caret at
    // the end (ready to edit / Enter). The _promptHistoryNavigating latch suppresses the box's
    // TextChanged -> _ResetPromptHistory so this recall isn't mistaken for a user edit. The UWP
    // TextBox raises TextChanged SYNCHRONOUSLY from the Text setter, so a plain bool around the set
    // is enough (set -> Text() -> any re-entrant TextChanged no-ops -> cleared, one stack frame).
    void AgentManagerContent::_ApplyPromptHistoryText(const std::wstring& text)
    {
        if (!_addPromptBox)
        {
            return;
        }
        _promptHistoryNavigating = true;
        // Always clear the latch, even if a setter throws — otherwise the TextChanged reset would stay
        // disabled and a subsequent real edit wouldn't leave history navigation.
        try
        {
            _addPromptBox.Text(winrt::hstring{ text });
            const int32_t len = static_cast<int32_t>(text.size());
            _addPromptBox.SelectionStart(len); // caret to the end (collapsed selection)
            _addPromptBox.SelectionLength(0);
        }
        catch (...)
        {
        }
        _promptHistoryNavigating = false;
    }

    // Agentmaster (prompt history): leave navigation — drop the snapshot/draft and return to the
    // "live draft" state (index -1). Called when the user edits the box (TextChanged), it's cleared
    // after a send, or the selected session changes (history is per-session).
    void AgentManagerContent::_ResetPromptHistory()
    {
        _promptHistoryIndex = -1;
        _promptHistory.clear();
        _promptHistoryDraft.clear();
    }

    // Agentmaster (prompt history): is the caret on the FIRST VISUAL ROW of the compose box? This is
    // the enter-history gate on the draft. "Visual row" (not logical line) so that with word-wrap on, a
    // long first logical line that wraps to several rows still lets Up move the caret up a row before
    // recalling — Up only enters history at the very top row. GetRectFromCharacterIndex respects wrap;
    // we compare the caret's Y to the first character's Y (same top => first row). Falls back to the
    // logical-line scan (no \n/\r before the caret — a UWP TextBox uses \r, a programmatic set may leave
    // \n) if the rect API is unavailable. An empty box / caret at 0 counts as the first row.
    bool AgentManagerContent::_PromptCaretOnFirstRow() const
    {
        if (!_addPromptBox)
        {
            return false;
        }
        const std::wstring text{ _addPromptBox.Text() };
        const int32_t caret = _addPromptBox.SelectionStart();
        if (caret <= 0 || text.empty())
        {
            return true; // start of the box (or empty) is always the first row
        }
        try
        {
            const auto firstR = _addPromptBox.GetRectFromCharacterIndex(0, false);
            // For the caret use the leading edge of the char at it; at end-of-text use the trailing
            // edge of the last char (index == length is out of range for the leading-edge form).
            const auto caretR = (caret >= static_cast<int32_t>(text.size())) ?
                                    _addPromptBox.GetRectFromCharacterIndex(static_cast<int32_t>(text.size()) - 1, true) :
                                    _addPromptBox.GetRectFromCharacterIndex(caret, false);
            // The caret is at/below the first row, so its top is >= the first row's top; "same row" if
            // within ~half a line height (tolerant of sub-pixel/baseline differences).
            const double tol = firstR.Height > 0 ? firstR.Height * 0.5 : 2.0;
            return caretR.Y <= firstR.Y + tol;
        }
        catch (...)
        {
            const int32_t limit = std::min<int32_t>(caret, static_cast<int32_t>(text.size()));
            for (int32_t i = 0; i < limit; ++i)
            {
                if (text[i] == L'\n' || text[i] == L'\r')
                {
                    return false;
                }
            }
            return true;
        }
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
        // Nav audit: the user changed this session's Autopilot mode (the Flight-Plan header toggle).
        ::Agentmaster::LogNav(L"autopilot " + ::Agentmaster::ShortId(_selectedId) + L" -> " + (mode == AutopilotMode::Full ? L"Full" : mode == AutopilotMode::SemiAuto ? L"Semi" : L"Off"));
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

    // Agentmaster: the FLIGHT-PLAN-header Autopilot toggle — advance the selected session's mode
    // (Off -> Semi-auto -> Full -> Off), reusing _OnAutopilotChanged's apply logic.
    void AgentManagerContent::_CycleAutopilot()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }
        const auto s = _registry->Get(_selectedId);
        if (!s || !s->live)
        {
            return; // nothing live to drive (the button is disabled in this state anyway)
        }
        // Off(0) -> Semi-auto(1) -> Full(2) -> Off — same index order the old combo used.
        const int next = s->autopilot.mode == AutopilotMode::Off ? 1 :
                                                                    (s->autopilot.mode == AutopilotMode::SemiAuto ? 2 : 0);
        _OnAutopilotChanged(next); // writes the registry + clears the per-run backstops
        _Refresh(); // repaint the header toggle now (the registry observer also refreshes)
    }

    // Paint the Autopilot toggle: a colored state dot (gray circle = Off, amber half = Semi, green
    // disc = Full) + a label. Dim + disabled when no live session is selected.
    void AgentManagerContent::_UpdateAutopilotButton(AutopilotMode mode, bool enabled)
    {
        if (!_autopilotBtn)
        {
            return;
        }
        winrt::hstring glyph;
        winrt::hstring label;
        Color dot{};
        switch (mode)
        {
        case AutopilotMode::Full:
            glyph = L"\x25CF"; // ●
            label = L"Autopilot: Full";
            dot = Colors::MediumSeaGreen();
            break;
        case AutopilotMode::SemiAuto:
            glyph = L"\x25D0"; // ◐
            label = L"Autopilot: Semi";
            dot = Colors::Goldenrod();
            break;
        case AutopilotMode::Off:
        default:
            glyph = L"\x25CB"; // ○
            label = L"Autopilot: Off";
            dot = Colors::Gray();
            break;
        }
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(6);
        auto g = Text(glyph, 12, true, enabled ? 1.0 : 0.4);
        g.Foreground(SolidColorBrush{ dot });
        row.Children().Append(g);
        row.Children().Append(Text(label, 11, false, enabled ? 0.95 : 0.5));
        _autopilotBtn.Content(row);
        _autopilotBtn.IsEnabled(enabled);
    }

    // Agentmaster: select the Flight-Plan pane's [Summary | Flight Plan] tab. The choice is a GLOBAL app
    // setting (AppSettings::flightPlanShowsSummary), so — exactly like the Explorer-Tree / Triage-Board
    // sort toggles — mutate _appSettings, re-paint, then push it through the settings sink (the page
    // persists settings.json + broadcasts it to every OTHER window, which adopt it in ApplyExternalSettings).
    // A no-op when unchanged, so re-clicking the active tab doesn't churn persistence/broadcast.
    void AgentManagerContent::_SelectPlanPaneTab(bool summary)
    {
        if (_appSettings.flightPlanShowsSummary == summary)
        {
            return; // already on this tab
        }
        _appSettings.flightPlanShowsSummary = summary;
        _UpdatePlanPaneTab();
        if (_settingsSink)
        {
            _settingsSink(_appSettings); // persist globally (settings.json) + broadcast to other windows
        }
    }

    // Agentmaster: reflect the current Flight-Plan pane tab — accent the selected segment (held through
    // hover via PaintHoldButton, the scope-toggle accent) + bold it, leave the other on default chrome,
    // and show ONLY that tab's body (the empty Summary host vs the Flight Plan body). No-op until the
    // controls exist, so it's safe to call from SetSettings before _BuildLayout has run.
    void AgentManagerContent::_UpdatePlanPaneTab()
    {
        if (!_summaryTabBtn || !_flightPlanTabBtn)
        {
            return;
        }
        const bool summary = _appSettings.flightPlanShowsSummary;
        const auto paintSegment = [](const Button& b, bool selected) {
            if (selected)
            {
                PaintHoldButton(b, 0xFF356AB8, 0xFF3E7DCE, 0xFF2B5391); // accent blue, holds through hover/press
                b.FontWeight(FontWeights::SemiBold());
            }
            else
            {
                ClearHoldButton(b); // default subtle chrome = the inactive segment
                b.FontWeight(FontWeights::Normal());
            }
        };
        paintSegment(_summaryTabBtn, summary);
        paintSegment(_flightPlanTabBtn, !summary);
        if (_summaryHost)
        {
            _summaryHost.Visibility(summary ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_flightPlanBody)
        {
            _flightPlanBody.Visibility(summary ? Visibility::Collapsed : Visibility::Visible);
        }
        _RefreshSummaryTab(); // populate/refresh the Summary tab when it becomes active (self-guards otherwise)
    }

    // Agentmaster (Summary tab): show the selected managed Claude session's summary box (the SAME
    // RenderSessionSummaryBox the Sessions page + per-tab overlay render) in the Flight-Plan Summary
    // tab. Cheap + idempotent: early-returns unless the Summary tab is active; renders from the
    // single-entry cache when (id, mtime) is unchanged; otherwise kicks the off-thread analyze. The
    // user messages are reversed to newest-first; the whole box rides one inner scrollbar.
    void AgentManagerContent::_RefreshSummaryTab()
    {
        if (!_summaryBoxHost || !_appSettings.flightPlanShowsSummary)
        {
            return; // Summary tab not built, or not the active tab — nothing to do
        }
        // Subject = the selected MANAGED CLAUDE session. Codex / external / nothing-selected get a
        // placeholder (the Flight Plan tab still serves those). The analyze cache key is the registry's
        // convLastActivityUnixMs — the same "last activity" signal the timing adornment uses.
        std::wstring id, dir;
        int64_t mtime = 0;
        if (!_selectedId.empty() && _registry)
        {
            if (const auto s = _registry->Get(_selectedId); s && s->kind == ::Agentmaster::AgentKind::Claude)
            {
                id = _selectedId;
                dir = s->workingDir;
                mtime = s->convLastActivityUnixMs;
            }
        }
        if (id.empty())
        {
            // Nothing summarizable selected — render the placeholder ONCE (sentinel), not every refresh.
            if (_summaryShownId != L"\x01none")
            {
                _summaryBoxHost.Children().Clear();
                auto hint = Text(_selectedId.empty() ? L"Select a Claude session to see its summary." : L"Summary is shown for Claude sessions.", 12, false, 0.5);
                hint.TextWrapping(TextWrapping::Wrap);
                hint.Margin(Thickness{ 2, 10, 2, 0 });
                _summaryBoxHost.Children().Append(hint);
                _summaryShownId = L"\x01none";
                _summaryShownMtime = -1;
            }
            return;
        }
        // Warm cache for this exact (id, mtime): render it (unless it's already on screen).
        if (_summaryCacheId == id && _summaryCacheMtime == mtime)
        {
            if (_summaryShownId != id || _summaryShownMtime != mtime)
            {
                _RenderSummaryBox(_summaryCacheText);
                _summaryShownId = id;
                _summaryShownMtime = mtime;
            }
            return;
        }
        // Need a (re)analyze. Show a loading line ONLY when switching to a different subject — if THIS
        // id's box (an older mtime) is already on screen, keep showing it (no flash) until the new one
        // lands. A busy session's mtime keeps changing, so flashing a spinner each tick would churn.
        if (_summaryShownId != id && _summaryShownId != L"\x01loading")
        {
            _summaryBoxHost.Children().Clear();
            auto hint = Text(L"Loading summary\x2026", 12, false, 0.5);
            hint.Margin(Thickness{ 2, 10, 2, 0 });
            _summaryBoxHost.Children().Append(hint);
            _summaryShownId = L"\x01loading";
            _summaryShownMtime = -1;
        }
        if (_summaryLoadingId != id) // one in-flight analyze per id — a busy session can't stack loads
        {
            _LoadSummaryForSession(id, dir, mtime);
        }
    }

    // Off-thread analyze + render of a Claude session's summary box (the Sessions-page recipe), posted
    // back via the dispatcher. Messages are reversed to newest-first before rendering. Cached single-
    // entry by (id, mtime); a stale result (selection moved on, or the Summary tab was left) is dropped.
    void AgentManagerContent::_LoadSummaryForSession(const std::wstring& id, const std::wstring& dir, int64_t mtime)
    {
        _summaryLoadingId = id;
        auto weak = get_weak();
        auto disp = _dispatcher;
        std::thread([weak, disp, id, dir, mtime]() {
            std::wstring text;
            const std::wstring path = ::Agentmaster::ResolveClaudeTranscriptPath(id);
            if (!path.empty())
            {
                auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
                // Newest-first: reverse the chronological user messages so RenderSessionSummaryBox (which
                // numbers in vector order) lists 1 = the most recent. Only MESSAGES are reordered.
                std::reverse(a.userMsgs.begin(), a.userMsgs.end());
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
                // full=false: the trimmed, space-saving variant (the per-tab overlay's view) — omits the
                // id / Dir / Folder / Resume / Branch header (redundant for an already-selected session),
                // leaving the value-add (Recap, Tasks, Messages, Files). Fits the narrow pane.
                text = ::Agentmaster::RenderSessionSummaryBox(a, id, dir, path, L"claude --resume " + id, L"", L"", planFile, /*full*/ false);
            }
            if (!disp)
            {
                return;
            }
            disp.TryEnqueue([weak, id, mtime, text = std::move(text)]() {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                // Cache the analyze regardless of current selection (a quick re-select stays warm).
                self->_summaryCacheId = id;
                self->_summaryCacheMtime = mtime;
                self->_summaryCacheText = text;
                if (self->_summaryLoadingId == id)
                {
                    self->_summaryLoadingId.clear();
                }
                // Render only if the Summary tab is still active AND this id is still the selected subject.
                if (!self->_appSettings.flightPlanShowsSummary || self->_selectedId != id)
                {
                    return;
                }
                self->_RenderSummaryBox(text);
                self->_summaryShownId = id;
                self->_summaryShownMtime = mtime;
            });
        }).detach();
    }

    // Render a RenderSessionSummaryBox result into _summaryBoxHost — mirrors the Sessions page's
    // SessAppendSummaryBox: split on '\n'; a lone kSummarySepMark line becomes a full-width rule; every
    // other run becomes a monospace, wrapped, selectable TextBlock. Replaces the panel's content.
    void AgentManagerContent::_RenderSummaryBox(const std::wstring& text)
    {
        if (!_summaryBoxHost)
        {
            return;
        }
        _summaryBoxHost.Children().Clear();
        if (text.empty())
        {
            auto hint = Text(L"No messages recorded for this session yet.", 12, false, 0.5);
            hint.TextWrapping(TextWrapping::Wrap);
            hint.Margin(Thickness{ 2, 10, 2, 0 });
            _summaryBoxHost.Children().Append(hint);
            return;
        }
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
            _summaryBoxHost.Children().Append(tb);
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
                rule.Background(Fill(0x40, 0xFF, 0xFF, 0xFF));
                rule.Margin(Thickness{ 0, 4, 0, 4 });
                _summaryBoxHost.Children().Append(rule);
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
        // Nav audit: the user saved the selected session's queue as a reusable plan template.
        ::Agentmaster::LogNav(L"template-save \"" + name + L"\" prompts=" + std::to_wstring(sel->queue.size()) + L" from=" + ::Agentmaster::ShortId(_selectedId));
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
            // Nav audit: the user applied a template's prompts to THIS session's queue (consequential — it
            // adds what Autopilot will send; more than a single `queue`, so logged even though the per-prompt
            // queue micro-edits aren't).
            ::Agentmaster::LogNav(L"template-apply \"" + tmpl.name + L"\" prompts=" + std::to_wstring(tmpl.prompts.size()) + L" -> " + ::Agentmaster::ShortId(_selectedId));
            _FocusPromptBox(); // Agentmaster: return focus to the compose box after applying a template
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
        int applied = 0;
        for (const auto& s : _registry->Snapshot())
        {
            if (PathEq(s.workingDir, dir))
            {
                _registry->Update(s.id, [&](SessionInfo& ss) { ::Agentmaster::AppendTemplateToQueue(ss.queue, tmpl); });
                ++applied;
            }
        }
        // Nav audit: the user broadcast a template to EVERY session in a directory — the most consequential
        // template action (queues prompts across many sessions at once).
        ::Agentmaster::LogNav(L"template-apply \"" + tmpl.name + L"\" prompts=" + std::to_wstring(tmpl.prompts.size()) + L" -> dir=" + dir + L" sessions=" + std::to_wstring(applied));
        _FocusPromptBox(); // Agentmaster: return focus to the compose box after applying a template
    }

    // ---- Launch path-picker drop-down ---------------------------------------

    std::vector<std::wstring> AgentManagerContent::_CollectRecentDirs(const std::wstring& current, const std::wstring& query) const
    {
        // The RECENT section length is a global setting (AppSettings::recentDirsLimit, default
        // 10); 0/garbage falls back to 10.
        const size_t limit = _appSettings.recentDirsLimit > 0 ? _appSettings.recentDirsLimit : 10;

        // Gather the full candidate pool first — recents (MRU) over live-session dirs — deduped,
        // with the box's own path excluded. In MRU mode we'd cap while gathering; in query mode
        // we rank the WHOLE pool before truncating, so the best matches surface even when they're
        // not the most recent. The pool is small (recents are capped at the same limit on save,
        // plus the live fleet), so gathering it whole is cheap.
        std::vector<std::wstring> pool;
        auto add = [&](const std::wstring& d) {
            if (d.empty())
            {
                return;
            }
            if (!current.empty() && PathEq(d, current))
            {
                return; // exclude the path that's currently in the box
            }
            for (const auto& e : pool)
            {
                if (PathEq(e, d))
                {
                    return; // dedup
                }
            }
            pool.push_back(d);
        };

        for (const auto& d : _recentDirs)
        {
            add(d);
        }
        // Supplement from live sessions (most-recently-active first) so the list is useful even
        // before anything has been launched this run. In query mode we need the WHOLE pool to
        // rank; in MRU mode we can stop (and skip the snapshot entirely) once it's full.
        const bool ranking = !query.empty();
        if (_registry && (ranking || pool.size() < limit))
        {
            auto snap = _registry->Snapshot();
            std::sort(snap.begin(), snap.end(), [](const SessionInfo& a, const SessionInfo& b) {
                return a.lastActivityUnixMs > b.lastActivityUnixMs;
            });
            for (const auto& s : snap)
            {
                add(s.workingDir);
                if (!ranking && pool.size() >= limit)
                {
                    break;
                }
            }
        }

        if (query.empty())
        {
            // Plain MRU order (recents first, then live-session dirs), capped to the limit — the
            // historical behavior when the box is empty or holds a rooted path.
            if (pool.size() > limit)
            {
                pool.resize(limit);
            }
            return pool;
        }

        // Query mode: a bare token was typed with no root path. Rank the pool by case-insensitive
        // fuzzy closeness to the token (Levenshtein approximate-substring distance over the whole
        // path — so a mid-path segment like "...\Agentmaster" matches "agent"), keep only the
        // genuine matches, and order them closest-first. The threshold allows roughly half the
        // token to differ, which keeps near-misses ("agen", "agnet") while dropping unrelated
        // recents; the gather order (MRU) is the stable tiebreaker so equally-close recents keep
        // their recency order. Very short tokens (<=2 chars) require an EXACT substring — one
        // allowed edit on a 2-char token would otherwise match almost anything — and the filter
        // tightens naturally as more characters are typed.
        const std::wstring q = ToLowerInvariant(query);
        const size_t threshold = q.size() <= 2 ? 0 : q.size() / 2;
        struct Scored
        {
            std::wstring dir;
            size_t dist;
            size_t order;
        };
        std::vector<Scored> scored;
        for (size_t i = 0; i < pool.size(); ++i)
        {
            const size_t dist = FuzzySubstringDistance(q, ToLowerInvariant(pool[i]));
            if (dist <= threshold)
            {
                scored.push_back({ pool[i], dist, i });
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
            if (a.dist != b.dist)
            {
                return a.dist < b.dist; // closest match topmost
            }
            return a.order < b.order; // ...then most-recent first
        });
        std::vector<std::wstring> out;
        for (const auto& s : scored)
        {
            if (out.size() >= limit)
            {
                break;
            }
            out.push_back(s.dir);
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

    // Agentmaster: the "Browse…" row — the FIRST option in the path-picker dropdown. Visually it
    // mirrors _MakePathRow (a transparent, focus-neutral row) but its glyph is a folder icon and its
    // click opens the native folder dialog instead of drilling a typed path. Kept a TextBlock (not a
    // FontIcon) so its vertical rhythm matches the other rows' TextBlock glyphs; only the FontFamily
    // is swapped to Segoe Fluent Icons so the folder glyph renders.
    Button AgentManagerContent::_MakeBrowseRow()
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        {
            auto glyph = Text(L"\xE8B7", 13, false, 0.9); // Segoe Fluent Icons: Folder
            glyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            row.Children().Append(glyph);
        }
        row.Children().Append(Text(L"Browse\x2026", 13, true, 1.0)); // bold: this is the primary "pick a folder" action

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        // Like the path rows: must NOT take focus from the cwd box — a row grabbing focus would let
        // the box's LostFocus race ahead and tear down the popup (and this button) before the click
        // registers. Keeping focus on the box also keeps the popup open across picks.
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        AgentSetTip(btn, L"Browse for a working directory \x2014 the pick fills the box and is added to recents.");
        btn.Click([this](const IInspectable&, const RoutedEventArgs&) { _BrowseForLaunchDir(); });
        return btn;
    }

    // Agentmaster: open the native folder dialog for the Launch path box ("Browse…" row). A pick is
    // dropped into the box AND pushed onto the recent-dirs MRU immediately (so Browse picks are part
    // of recents even before the session is launched), then the box is re-validated + refocused. The
    // COM modal is deferred off the click tick (it needs the message pump — the same XAML-Islands rule
    // the claude.exe Browse and the profile picker both follow).
    void AgentManagerContent::_BrowseForLaunchDir()
    {
        if (!_dispatcher || !_cwdBox)
        {
            return;
        }
        // Seed the dialog at the box's current path when it points at a real folder (or, for a partial
        // leaf being typed, that leaf's existing parent), captured before the async hop.
        std::wstring seed;
        {
            const std::wstring norm = NormPath(std::wstring{ _cwdBox.Text() });
            if (IsDir(norm))
            {
                seed = norm;
            }
            else if (const auto parent = ParentDir(norm); parent && IsDir(*parent))
            {
                seed = *parent;
            }
        }
        _dispatcher.TryEnqueue([this, seed]() {
            const auto picked = PickFolder(::GetActiveWindow(), seed);
            if (!picked || picked->empty() || !_cwdBox)
            {
                return;
            }
            const std::wstring dir = NormPath(*picked);
            _PushRecentDir(dir); // a Browse pick joins the recents right away (shared MRU with launches)
            _cwdBox.Text(winrt::hstring{ dir }); // set the launch target (fires TextChanged -> _ValidateLaunchBox)
            _cwdBox.Select(static_cast<int32_t>(dir.size()), 0); // caret to end
            _ValidateLaunchBox(); // explicit: a dismissed picker can early-out of the TextChanged path
            _cwdBox.Focus(FocusState::Programmatic); // keep the box focused (don't strand focus on the dialog)
            // Refresh the open picker so the just-added recent shows; leave a closed one closed (the
            // user made a definitive choice via the modal).
            if (_pathPopup && _pathPopup.IsOpen())
            {
                _RebuildPathPicker();
            }
        });
    }

    Button AgentManagerContent::_MakePathRow(const std::wstring& fullPath, const winrt::hstring& glyph, const winrt::hstring& displayText, const std::wstring& branch)
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(Text(glyph, 13, false, 0.7));
        // Recents show their full path (displayText empty); folders show just the leaf —
        // the section header already states which directory they live in.
        row.Children().Append(Text(displayText.empty() ? winrt::hstring{ fullPath } : displayText, 13, false, 1.0));
        // Agentmaster: a non-empty `branch` (the RECENT rows pass it, via ReadGitBranchForDir) appends
        // "— <branch>" at the end — the GIT WORKTREES row's "<path> — <branch>" idiom — so a recent dir
        // that's a git repo shows the branch it's on. Empty (folders / non-repos) renders nothing.
        if (!branch.empty())
        {
            row.Children().Append(Text(L"\x2014", 13, false, 0.35));
            row.Children().Append(Text(winrt::hstring{ branch }, 13, false, 0.7));
        }

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
        AgentSetTip(btn, winrt::hstring{ L"Use this folder \x2014 " } + winrt::hstring{ fullPath });
        btn.Click([this, captured](const IInspectable&, const RoutedEventArgs&) { _PickPath(captured); });
        return btn;
    }

    // Agentmaster: a "GIT WORKTREES" row — "<name> — <path> — <branch>" (branch also in the tooltip).
    // Mirrors _MakePathRow (a transparent, focus-neutral row that must NOT steal focus from the cwd
    // box — or the box's LostFocus would race ahead and tear down the popup before the click lands)
    // and, like it, a click drills the launch box into the worktree path via _PickPath, so a session
    // launches THERE. The leaf name is the worktree's identity; the path follows dim so the launch
    // destination is unambiguous (worktrees of one repo share a folder, differ by branch).
    Button AgentManagerContent::_MakeWorktreeRow(const std::wstring& fullPath, const std::wstring& name, const std::wstring& branch, bool isCurrent)
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        {
            auto glyph = Text(L"\xF1D3", 13, false, isCurrent ? 0.9 : 0.7); // Segoe Fluent Icons: BranchFork2
            glyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            row.Children().Append(glyph);
        }
        row.Children().Append(Text(winrt::hstring{ name }, 13, true, 1.0));
        row.Children().Append(Text(L"\x2014", 13, false, 0.35)); // em-dash separator: "<name> — <path>"
        row.Children().Append(Text(winrt::hstring{ fullPath }, 13, false, 0.55));
        if (!branch.empty())
        {
            row.Children().Append(Text(L"\x2014", 13, false, 0.35)); // second separator: "<path> — <branch>"
            row.Children().Append(Text(winrt::hstring{ branch }, 13, false, 0.7)); // the worktree's checked-out branch, at the end (a touch brighter than the path)
        }
        if (isCurrent)
        {
            row.Children().Append(Text(L"(current)", 11, false, 0.45)); // the worktree the box already points into
        }

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        const auto captured = fullPath;
        const winrt::hstring tip = branch.empty() ?
            (winrt::hstring{ L"Use this worktree \x2014 " } + winrt::hstring{ fullPath }) :
            (winrt::hstring{ L"Worktree on branch " } + winrt::hstring{ branch } + winrt::hstring{ L" \x2014 " } + winrt::hstring{ fullPath });
        AgentSetTip(btn, tip);
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

        // Agentmaster: "Browse…" is always the FIRST option — a native folder dialog whose pick fills
        // the box and joins the recents. Appended before any section so it sits at the top in every
        // mode (empty box, fuzzy query, or a rooted path). The empty-state hint below keys off this
        // baseline so it still shows when Browse is the ONLY row.
        _pathListHost.Children().Append(_MakeBrowseRow());
        const uint32_t browseRowCount = _pathListHost.Children().Size();

        auto sectionLabel = [](const winrt::hstring& s) {
            auto lbl = Text(s, 11, true, 0.5);
            lbl.Margin(Thickness{ 6, 8, 6, 2 });
            return lbl;
        };

        const std::wstring current = _cwdBox ? std::wstring{ _cwdBox.Text() } : std::wstring{};

        // "No root path" = a bare token typed with no path anchor: no separator AND not drive-
        // qualified ("agent", not "K:\..." / "K:" / "/home" / "\\server"). In that case the box
        // text can't anchor a subfolder listing, so instead of the (useless) "(path not found)"
        // we treat the token as a fuzzy QUERY over the recent directories — matched case-
        // insensitively and ranked by Levenshtein closeness (closest first). A session id is a
        // bare token too but is never a directory query, so it stays in plain MRU mode.
        auto looksRooted = [](const std::wstring& s) {
            if (s.find_first_of(L"\\/") != std::wstring::npos)
            {
                return true; // has a path separator
            }
            if (s.size() >= 2 && s[1] == L':' &&
                ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')))
            {
                return true; // drive-qualified ("K:" / "K:foo")
            }
            return false;
        };
        const bool queryMode = !current.empty() && !looksRooted(current) && !LooksLikeSessionId(current).has_value();

        // RECENT (up to AppSettings::recentDirsLimit, current excluded). In query mode the section
        // is filtered + ranked by closeness to the typed token; otherwise it's plain MRU order.
        const auto recents = _CollectRecentDirs(current, queryMode ? current : std::wstring{});
        if (!recents.empty())
        {
            _pathListHost.Children().Append(sectionLabel(queryMode ?
                (winrt::hstring{ L"RECENT MATCHES FOR \"" } + winrt::hstring{ current } + winrt::hstring{ L"\"" }) :
                winrt::hstring{ L"RECENT" }));
            for (const auto& d : recents)
            {
                _pathListHost.Children().Append(_MakePathRow(d, L"\x21BB", winrt::hstring{}, ReadGitBranchForDir(d))); // Agentmaster: show the recent dir's git branch at the end, like the worktree rows
            }
        }

        // GIT WORKTREES of the repo containing the typed path (if any). ListGitWorktrees mirrors
        // ReadGitBranchForDir's pure-filesystem .git walk (a few small reads + one dir enum), so it is
        // cheap enough for this per-keystroke rebuild. Shown only when the path resolves into a repo
        // that has LINKED worktrees (size >= 2: the main worktree + at least one linked) — a plain repo
        // with no extra worktrees adds no section. Clicking a row drills the launch box into that
        // worktree exactly like picking a folder, so a session launches there.
        if (!current.empty() && !queryMode)
        {
            const auto worktrees = ListGitWorktrees(NormPath(current));
            if (worktrees.size() >= 2)
            {
                _pathListHost.Children().Append(sectionLabel(winrt::hstring{ L"GIT WORKTREES" }));
                for (const auto& w : worktrees)
                {
                    _pathListHost.Children().Append(_MakeWorktreeRow(w.path, w.name, w.branch, w.isCurrent));
                }
            }
        }

        // SUBFOLDERS of the current path (+ a parent up-nav).
        //
        // Agentmaster: typing a partial leaf FILTERS. The box text splits at its LAST separator
        // into (listDir, leaf):
        //   - a path ending in a separator ("K:\source\") has an empty leaf => list ALL of
        //     listDir, unfiltered;
        //   - a partial leaf ("K:\source\Agent") lists listDir's folders whose name STARTS WITH
        //     the leaf ("Agent*"), case-insensitively, with the EXACT match hoisted to the top.
        // When the WHOLE text is itself an existing directory ("K:\source", no trailing
        // separator), a SECOND section below ALSO lists that directory's own subfolders — so
        // "K:\source" shows the K:\source* siblings (above) AND everything inside K:\source.
        // (Clicking any folder row appends the separator, drilling into it.)
        //
        // Skipped in query mode: a bare token (no root path) has no directory to enumerate, so
        // this would only ever produce "(path not found)" — the RECENT MATCHES section above is
        // the whole answer there.
        if (!current.empty() && !queryMode)
        {
            std::wstring listDir = current; // the directory whose subfolders we enumerate
            std::wstring filter; // leaf prefix to match; empty => list everything
            const wchar_t lastCh = current.back();
            const bool endsSep = (lastCh == L'\\' || lastCh == L'/');
            if (!endsSep)
            {
                if (const auto pos = current.find_last_of(L"\\/"); pos != std::wstring::npos)
                {
                    filter = current.substr(pos + 1); // the leaf being typed
                    listDir = current.substr(0, pos + 1); // its parent (keep the trailing sep)
                }
                // else: a bare relative token with no separator — there's no parent in the box
                // to anchor a listing to, so fall through and list `current` itself (no filter).
            }

            // Append `dir`'s subfolders, optionally keeping only names that start with `filt`
            // (case-insensitive) and hoisting an exact match to the front (a directory's leaf is
            // unique within its parent, so at most one); the rest stay newest-modified-first.
            // Returns how many folder rows were appended.
            auto appendFolderRows = [&](const std::wstring& dir, const std::wstring& filt) -> size_t {
                auto subs = EnumSubdirs(dir);
                if (!filt.empty())
                {
                    std::wstring exact;
                    std::vector<std::wstring> matched;
                    for (auto& leaf : subs)
                    {
                        if (!LeafStartsWith(leaf, filt))
                        {
                            continue;
                        }
                        if (exact.empty() && LeafEquals(leaf, filt))
                        {
                            exact = std::move(leaf);
                        }
                        else
                        {
                            matched.push_back(std::move(leaf));
                        }
                    }
                    subs.clear();
                    if (!exact.empty())
                    {
                        subs.push_back(std::move(exact));
                    }
                    for (auto& m : matched)
                    {
                        subs.push_back(std::move(m));
                    }
                }
                for (const auto& leaf : subs)
                {
                    _pathListHost.Children().Append(_MakePathRow(JoinDir(dir, leaf), L"\x25B8", winrt::hstring{ leaf }));
                }
                return subs.size();
            };

            // Section A — matches in listDir (the typed leaf's siblings, or — with no leaf — the
            // whole directory). Carries the parent up-nav (one level above listDir).
            {
                const winrt::hstring hdr = filter.empty() ?
                    (winrt::hstring{ L"SUBFOLDERS OF " } + winrt::hstring{ listDir }) :
                    (winrt::hstring{ L"MATCHES FOR \"" } + winrt::hstring{ filter } + winrt::hstring{ L"\" IN " } + winrt::hstring{ listDir });
                _pathListHost.Children().Append(sectionLabel(hdr));
            }
            if (const auto parent = ParentDir(listDir))
            {
                _pathListHost.Children().Append(_MakePathRow(*parent, L"\x2191", winrt::hstring{ L".. (parent)" }));
            }
            if (IsDir(listDir))
            {
                if (appendFolderRows(listDir, filter) == 0)
                {
                    _pathListHost.Children().Append(Text(filter.empty() ? L"(no subfolders)" : L"(no matches)", 12, false, 0.5));
                }
            }
            else
            {
                _pathListHost.Children().Append(Text(L"(path not found)", 12, false, 0.5));
            }

            // Section B — when the WHOLE text names an existing directory (and a leaf was matched
            // above), ALSO list everything INSIDE it, below the sibling matches.
            if (!endsSep && !filter.empty() && IsDir(current))
            {
                _pathListHost.Children().Append(sectionLabel(winrt::hstring{ L"SUBFOLDERS OF " } + winrt::hstring{ current }));
                if (appendFolderRows(NormPath(current), std::wstring{}) == 0)
                {
                    _pathListHost.Children().Append(Text(L"(no subfolders)", 12, false, 0.5));
                }
            }
        }

        if (_pathListHost.Children().Size() == browseRowCount)
        {
            // Only the Browse row is present (no recents / subfolders). In query mode that means the
            // token matched no recent directory; otherwise it's the initial empty-box hint.
            _pathListHost.Children().Append(Text(queryMode ?
                L"No recent directory matches — type a full path, or Browse\x2026 above." :
                L"Type a path, pick a recent directory, or Browse\x2026 above.",
                12, false, 0.6));
        }
    }

    void AgentManagerContent::_OpenPathPicker()
    {
        if (!_pathPopup || !_cwdBox)
        {
            return;
        }
        _pathPickerUserDismissed = false; // opening clears the dismiss latch

        // Anchor the popup just under the cwd box (left-aligned with it). Capture its left edge in
        // _root coordinates so the width below can be measured against the window's right edge.
        double leftInRoot = 0.0;
        try
        {
            const auto xform = _cwdBox.TransformToVisual(_root);
            const auto pt = xform.TransformPoint(Point{ 0.0f, static_cast<float>(_cwdBox.ActualHeight()) });
            leftInRoot = static_cast<double>(pt.X);
            _pathPopup.HorizontalOffset(leftInRoot);
            _pathPopup.VerticalOffset(static_cast<double>(pt.Y) + 2.0);
        }
        catch (...)
        {
        }

        if (_pathPanelBorder)
        {
            // Width is set EXPLICITLY here, not left to content/MaxWidth: the rows are short folder
            // leaf names, so a MaxWidth cap alone never grows the panel (it sizes to content and sits
            // at the floor). Instead the panel fills from its left anchor out to 10% short of the
            // window's right edge, with a readable floor when the window is narrow. The cwd box width
            // is the lower bound so it's never narrower than the box it drops from.
            const double minWidth = std::max<double>(560.0, _cwdBox.ActualWidth());
            const double rootWidth = _root ? _root.ActualWidth() : 0.0;
            const double rightLimit = rootWidth * 0.9; // leave a 10% gap on the window's right edge
            const double width = std::max<double>(minWidth, rightLimit - leftInRoot);
            _pathPanelBorder.Width(width);
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
        std::wstring norm = NormPath(dir); // selecting from the list normalizes too
        // Clicking a directory row DRILLS INTO it: append a separator so the rebuilt picker
        // lists that folder's children unfiltered (the trailing-separator rule above), turning
        // a click into a drill-down like the old picker. The slash is cosmetic — _NormalizeCwdBox
        // strips it again on commit / launch, and a drive root ("K:\") already carries one.
        if (IsDir(norm) && !norm.empty() && norm.back() != L'\\' && norm.back() != L'/')
        {
            norm += L'\\';
        }
        _cwdBox.Text(winrt::hstring{ norm }); // fires TextChanged -> _RebuildPathPicker (popup open)
        _cwdBox.Select(static_cast<int32_t>(norm.size()), 0); // caret to end
        _cwdBox.Focus(FocusState::Programmatic); // keep the box focused so the popup stays open
    }
}
