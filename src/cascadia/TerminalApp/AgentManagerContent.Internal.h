// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster Manager tab content -- C1 'Linked Lenses' (7 partial files)
// The pinned leftmost tab's UI (DESIGN section 9): a Triage Board + Explorer Tree + Auto Testing over
// ONE shared SessionRegistry, built imperatively. ONE class (AgentManagerContent) split from the
// former 10864-line .cpp into by-area TUs that share AgentManagerContent.Internal.h.
//
// Partial files in this group (★ marks THIS file):
//   AgentManagerContent.cpp             - CORE: ctor/dtor, the Set* wiring, IPaneContent, the per-window lens, _BuildLayout, _Refresh
// ★ AgentManagerContent.Internal.h      - the ~48 shared file-local helpers: StateColor/Pill/StateDot/Text/Fill + path/sort utils (anonymous namespace, a per-TU copy)
//   AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
//   AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
//   AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
//   AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
//   AgentManagerContent.Launch.cpp      - the Launch bar: cwd validation, the Claude/Codex toggle, launch/create/fork, the path-picker drop-down
// ======================================================================================
//
// Agentmaster: AgentManagerContent file-local helpers (the StateColor/Pill/StateDot/Text/Fill/
// path + sort utilities, ~48 of them). Factored out of AgentManagerContent.cpp so the by-area
// partial TUs (AgentManagerContent.{cpp,Board,Tree,Settings,AutoTesting,Launch}.cpp) all share ONE
// copy. Kept in an ANONYMOUS namespace exactly as before (internal linkage, a per-TU copy) -- no
// behavior change. IMPORTANT: this header is NOT standalone -- it must be included AFTER pch.h and
// the file-scope using-directives that each AgentManagerContent.*.cpp replicates (it relies on them,
// just as the original anonymous namespace relied on the using-directives above it).
#pragma once

#include "AgentClipboard.h" // RobustCopyTextToClipboard — the retry-looped Win32 writer CopyTextToClipboard routes through

namespace
{
    // Agentmaster (cog "Overlay opacity" dual-thumb slider): the track + dot geometry, shared by the build
    // and the layout/drag helpers so the value<->pixel mapping can't drift. A FIXED track width keeps the
    // mapping layout-independent (no SizeChanged needed). A dot's value v in [0,1] maps to Canvas.Left =
    // v * (kOverlayTrackW - kOverlayThumb), so a dot stays fully inside the rail at both ends.
    constexpr double kOverlayTrackW = 240.0;
    constexpr double kOverlayThumb = 16.0;
    constexpr double kOverlayTrackH = 22.0;

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
    // the dots by ref (no name/resource lookup), BEGINS on Loaded and STOPS on Unloaded. The Unloaded
    // Stop() is load-bearing: a STARTED Forever storyboard is held by the XAML animation clock (it
    // never completes), and that hold pins its target Ellipses — so a board rebuild alone did NOT
    // release a dropped card's dots (one leaked cluster per unsent-draft card per rebuild). The Loaded
    // closure keeps the storyboard alive by design; the pair is symmetric, so a row re-entering the
    // tree re-Begins. dotPx sizes the dots (cards: 5px). `color` is the contrast-picked pending-dots
    // color (PendingDotsColorFor) — defaulting to the historical gold so a caller that doesn't care is
    // unchanged.
    StackPanel BuildPendingDots(double dotPx = 5.0, Color color = ColorHelper::FromArgb(0xFF, 0xE0, 0xA9, 0x2B))
    {
        namespace MA = winrt::Windows::UI::Xaml::Media::Animation;
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(3);
        row.VerticalAlignment(VerticalAlignment::Center);
        std::vector<winrt::Windows::UI::Xaml::Shapes::Ellipse> dots;
        for (int i = 0; i < 3; ++i)
        {
            winrt::Windows::UI::Xaml::Shapes::Ellipse e;
            e.Width(dotPx);
            e.Height(dotPx);
            e.Fill(SolidColorBrush{ color });
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
        // Release the animation clock's hold on the storyboard + its dot targets (header note above).
        row.Unloaded([sb](auto&&, auto&&) {
            try
            {
                sb.Stop();
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

    // Agentmaster: put text on the system clipboard (the context menus' "Copy Session Id"). Routes
    // through the robust, retry-looped Win32 writer (AgentClipboard.h) rather than the WinRT Clipboard
    // (SetContent/Flush): the latter opens the OLE clipboard once with NO retry, so it silently lost
    // the copy whenever a FOCUSED terminal tab was contending for it (the same flaw behind the "Copy
    // ... does not work well in a focused tab" report). Call on the UI thread. Best-effort.
    void CopyTextToClipboard(const std::wstring& text)
    {
        winrt::TerminalApp::implementation::RobustCopyTextToClipboard(text);
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

    // Agentmaster (Waiting-for-you "unread" model): render minutes as a COMPACT, no-space "12h5m" form
    // for the timeout TEXTBOX to the left of the slider (e.g. 4320 -> "3d", 725 -> "12h5m", 90 -> "1h30m",
    // 5 -> "5m"). Distinct from FormatMinutesFriendly's spaced "1h 30m". 0 -> "" (the "Never" toggle owns 0,
    // and the box is disabled then).
    std::wstring FormatMinutesCompact(uint32_t m)
    {
        if (m == 0)
        {
            return L"";
        }
        const uint32_t d = m / 1440;
        m %= 1440;
        const uint32_t h = m / 60;
        const uint32_t mi = m % 60;
        std::wstring out;
        if (d)
        {
            out += std::to_wstring(d);
            out += L'd';
        }
        if (h)
        {
            out += std::to_wstring(h);
            out += L'h';
        }
        if (mi)
        {
            out += std::to_wstring(mi);
            out += L'm';
        }
        return out;
    }

    // Agentmaster (Waiting-for-you "unread" model): parse a compact duration string into total MINUTES —
    // the inverse of FormatMinutesCompact, for the timeout textbox. Accepts any combination of d/h/m units
    // (case-insensitive, whitespace tolerated): "3d", "12h5m", "2d4h30m", "1h", "45m", or a bare number =
    // minutes ("90" -> 90). Returns false (the caller paints the box red) on: no digits at all, an unknown
    // unit/char, a number dangling after a unit ("2h30"), or overflow past UINT32. The slider only spans
    // 1m..7d, but this deliberately accepts far more (the box may exceed the slider, which then sits maxed);
    // the caller clamps to a sane ceiling on save.
    bool ParseDurationToMinutes(const std::wstring& s, uint32_t& outMinutes)
    {
        constexpr uint64_t kU32Max = 0xFFFFFFFFull;
        uint64_t total = 0; // accumulate wide to detect overflow
        uint64_t cur = 0; // the number currently being read
        bool haveDigit = false; // digits seen in the CURRENT number (since the last unit)
        bool anyDigit = false; // any digit anywhere (an empty / units-only string is invalid)
        bool sawUnit = false; // any unit consumed (a trailing bare number is only valid if no unit was used)
        const auto flush = [&](uint64_t mult) -> bool {
            if (!haveDigit)
            {
                return false; // a unit with no preceding number ("h5m") is invalid
            }
            total += cur * mult;
            if (total > kU32Max)
            {
                return false; // overflow
            }
            cur = 0;
            haveDigit = false;
            return true;
        };
        for (const wchar_t c : s)
        {
            if (c == L' ' || c == L'\t')
            {
                continue; // tolerate spaces between terms
            }
            if (c >= L'0' && c <= L'9')
            {
                cur = cur * 10 + static_cast<uint64_t>(c - L'0');
                if (cur > kU32Max)
                {
                    return false; // a single number too big
                }
                haveDigit = true;
                anyDigit = true;
                continue;
            }
            uint64_t mult = 0;
            switch (c)
            {
            case L'd':
            case L'D':
                mult = 1440;
                break;
            case L'h':
            case L'H':
                mult = 60;
                break;
            case L'm':
            case L'M':
                mult = 1;
                break;
            default:
                return false; // any other character => unparsable
            }
            if (!flush(mult))
            {
                return false;
            }
            sawUnit = true;
        }
        // A trailing bare number (no unit) counts as MINUTES, but only if NO unit was used in the string
        // (mixing "2h30" leaves a dangling, ambiguous "30" -> reject it).
        if (haveDigit)
        {
            if (sawUnit)
            {
                return false;
            }
            total += cur;
            if (total > kU32Max)
            {
                return false;
            }
        }
        if (!anyDigit)
        {
            return false; // empty or units-only
        }
        outMinutes = static_cast<uint32_t>(total);
        return true;
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

    // Agentmaster ("Activate Tab" via Shift+Click): is Shift currently held? Mirrors the rename box's
    // CoreWindow::GetKeyState modifier check (XAML Islands-safe — CoreWindow may be null off the input
    // thread, in which case we treat Shift as up). Lets a Shift+Click on a board card / tree row START a
    // dormant session IN PLACE (no tab switch), the click twin of the "Activate Tab (Shift+Click)" menu item.
    bool ShiftHeld()
    {
        if (const auto w = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
        {
            return WI_IsFlagSet(w.GetKeyState(winrt::Windows::System::VirtualKey::Shift), winrt::Windows::UI::Core::CoreVirtualKeyStates::Down);
        }
        return false;
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
