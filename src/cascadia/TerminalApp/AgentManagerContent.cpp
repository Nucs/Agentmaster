// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/ProfileBootstrap.h" // the cog's Profile row (active dir + Change… picker)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/Engine.h" // RecoverableWindows (the "Reopen Windows (N)" recover button)
#include "AgentMaster/ProcessInspect.h" // ReadTranscriptInfo (read-only Flight Plan of an external) + BringClaudeWindowToFront (EXTERNAL menu)
#include "AgentMaster/TranscriptStore.h" // ReadTranscriptQuickFacts — resolve a launch-box session id's cwd

#include <algorithm>
#include <chrono>
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
    SolidColorBrush Fill(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
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
    void AgentManagerContent::SetResumeSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler)
    {
        _resumeSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetForkSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler)
    {
        _forkSessionHandler = std::move(handler);
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
    void AgentManagerContent::SetPauseHandler(std::function<void(bool)> handler)
    {
        _pauseHandler = std::move(handler);
    }
    void AgentManagerContent::SetReopenWindowsHandler(std::function<void()> handler)
    {
        _reopenWindowsHandler = std::move(handler);
    }
    void AgentManagerContent::SetReopenWindowHandler(std::function<void(int)> handler)
    {
        _reopenWindowHandler = std::move(handler);
    }
    void AgentManagerContent::SetOpenArchiveHandler(std::function<void()> handler)
    {
        _openArchiveHandler = std::move(handler);
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
        // Reflect the (global, persisted) Explorer Tree sort on its toggle. Safe before the UI is
        // built (the updater no-ops while _treeSortBtn is null); the tree itself adopts the order on
        // the next data-driven rebuild. Lets a window pick up the loaded/changed sort, not just the
        // ctor default.
        _UpdateTreeSortButton();
    }
    void AgentManagerContent::SetSettingsHandler(std::function<void(::Agentmaster::AppSettings)> handler)
    {
        _settingsSink = std::move(handler);
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
        _selectedId = state.selectedId;
        _scopeDir = state.scopeDir;
        _selectedPromptId = state.selectedPromptId;
        _collapsedDirs.clear();
        _collapsedDirs.insert(state.collapsedDirs.begin(), state.collapsedDirs.end());
        // The shared tree/board scope. Direct assignment, NOT _SetTreeScope: this is a seed, not a
        // user transition — no enter/leave-External selection cleanup (the persisted lens is already
        // self-consistent: entering External cleared the managed selection before it was saved), and
        // no lens push (we are APPLYING the lens). Range-guarded like the loader (a hand-built
        // state isn't necessarily clamped).
        _treeScope = (state.treeScope >= 0 && state.treeScope <= 2) ? static_cast<TreeScope>(state.treeScope) : TreeScope::Local;
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
            auto bar = StackPanel{};
            bar.Orientation(Orientation::Horizontal);
            bar.Spacing(8);
            bar.Margin(Thickness{ 12, 8, 12, 0 });
            bar.VerticalAlignment(VerticalAlignment::Center);

            bar.Children().Append(Text(L"Agentmaster", 18, true, 1.0));
            bar.Children().Append(Text(L"\x2014  launch a", 13, false, 0.6));

            // Agentmaster (Codex-launch): the agent toggle — Claude (default) <-> Codex. Click cycles it
            // (the scope/sort/autopilot toggle idiom). It retargets the SAME cwd box + Launch button, so a
            // managed Codex launches exactly the way a Claude does ("do what we do for Claude"). A Codex
            // launch is directory-only — Codex has no typed-id resume/fork here (Codex resume is reached via
            // the Archive page / window-restore / EXTERNAL Adopt), so _ValidateLaunchBox suppresses those.
            _launchAgentBtn = Button{};
            _launchAgentBtn.FontSize(11);
            _launchAgentBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_launchAgentBtn, L"Agent to launch \x2014 click to switch between Claude and Codex");
            _launchAgentBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _launchCodex = !_launchCodex;
                _UpdateLaunchAgentButton();
                _ValidateLaunchBox(); // repaint the Launch button text + resume/fork affordances for the new agent
            });
            bar.Children().Append(_launchAgentBtn);
            _UpdateLaunchAgentButton();

            bar.Children().Append(Text(L"session in", 13, false, 0.6));

            _cwdBox = TextBox{};
            _cwdBox.Width(360);
            _cwdBox.PlaceholderText(L"working directory (the M axis)");
            AgentSetTip(_cwdBox, L"path to workdir / session id"); // Agentmaster: the box accepts EITHER a working dir (new session) OR a session id (Resume / Fork)
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
            _cwdUnderline.Width(360); // match _cwdBox.Width(360)
            _cwdUnderline.HorizontalAlignment(HorizontalAlignment::Left);
            _cwdUnderline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
            _cwdUnderline.Background(Fill(0x00, 0x00, 0x00, 0x00)); // transparent = neutral; kept present so painting it never reflows the bar
            cwdCol.Children().Append(_cwdUnderline);
            bar.Children().Append(cwdCol);

            _launchBtn = Button{};
            _launchBtn.Content(winrt::box_value(L"Launch Claude"));
            _launchBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnLaunch(); });
            bar.Children().Append(_launchBtn);

            // Fork — hidden unless the box holds a FOUND session id; forks that conversation into a
            // NEW one (the original transcript is untouched), mirroring the Sessions page's "Fork here".
            _forkBtn = Button{};
            _forkBtn.Content(winrt::box_value(L"Fork"));
            _forkBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_forkBtn, L"Fork this conversation into a NEW session (claude --resume --fork-session) \x2014 the original is untouched");
            _forkBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnForkFromBox(); });
            bar.Children().Append(_forkBtn);

            _ValidateLaunchBox(); // initial state for the seeded cwd (USERPROFILE -> neutral, enabled)

            // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): the "Reopen Windows (N)" recover
            // button — the "if I answered No" path. It reopens saved windows that are NOT currently
            // open (the runtime analog of the WindowEmperor's startup reopen loop). Hidden when there
            // is nothing to recover (N==0); _UpdateReopenButton (driven from _Refresh) maintains both.
            _reopenBtn = Button{};
            _reopenBtn.Content(winrt::box_value(L"Reopen Windows"));
            _reopenBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_reopenBtn, L"Reopen previously-saved windows that aren't currently open");
            _reopenBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnReopenWindows(); });
            bar.Children().Append(_reopenBtn);

            // Settings cog (opens the in-content settings overlay; built at the end of layout).
            _settingsBtn = Button{};
            {
                FontIcon cog;
                cog.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
                cog.Glyph(L"\xE713"); // Settings (cog)
                _settingsBtn.Content(cog);
            }
            AgentSetTip(_settingsBtn, L"Settings");
            _settingsBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ShowSettings(); });
            bar.Children().Append(_settingsBtn);

            // Global Autopilot backstop: Pause-all / Resume-all. Placed AFTER the Settings cog.
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
            // Placed AFTER the Settings cog (alongside Pause Autopilot).
            _archivedBtn = Button{};
            _archivedBtn.Content(winrt::box_value(L"Archived"));
            AgentSetTip(_archivedBtn, L"Restore archived (closed) sessions");
            _archivedBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { if (_openArchiveHandler) { _openArchiveHandler(); } });
            bar.Children().Append(_archivedBtn);

            // Agentmaster (Sessions page; SESSIONS.md): the global on-disk Claude-sessions browser —
            // EVERY session on the machine in a selectable window, searchable. RIGHT AFTER Archived.
            _sessionsBtn = Button{};
            _sessionsBtn.Content(winrt::box_value(L"Sessions"));
            AgentSetTip(_sessionsBtn, L"Browse + search ALL Claude Code sessions on this machine (last month by default)");
            _sessionsBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { if (_openSessionsHandler) { _openSessionsHandler(); } });
            bar.Children().Append(_sessionsBtn);

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
            // Agentmaster: the board's LOCAL/GLOBAL scope toggle — ONE state with the Explorer
            // Tree's 3-way toggle (same style, same per-window lens persistence): LOCAL shows only
            // this window's sessions, GLOBAL every window's. While the tree sits in EXTERNAL the
            // board reads GLOBAL (it has no External mode); a click then flips the shared scope to
            // LOCAL. _SetTreeScope is the one mutator behind both buttons.
            _boardScopeBtn = Button{};
            _boardScopeBtn.FontSize(11);
            _boardScopeBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_boardScopeBtn, L"Scope \x2014 LOCAL: this window's sessions; GLOBAL: all windows. One state with the Explorer Tree's toggle (EXTERNAL there reads as GLOBAL here); persisted per window.");
            _boardScopeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _SetTreeScope(_treeScope == TreeScope::Local ? TreeScope::Global : TreeScope::Local);
            });
            header.Children().Append(_boardScopeBtn);
            _UpdateBoardScopeButton();
            // The directory-scope label appears ONLY while a directory is scoped ("[scope: <dir>]"
            // next to the "Show all" clear button). The old unscoped "[all directories]"
            // placeholder is gone — it was display-only, restating the default.
            _boardScope = Text(L"", 12, false, 0.6);
            _boardScope.Visibility(Visibility::Collapsed);
            header.Children().Append(_boardScope);
            _showAllBtn = Button{};
            _showAllBtn.Content(winrt::box_value(L"Show all"));
            _showAllBtn.Padding(Thickness{ 6, 0, 6, 0 });
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
                AgentSetTip(_treeScopeBtn, L"Scope \x2014 LOCAL: this window's sessions; GLOBAL: all windows; EXTERNAL: observe-only claudes in other hosts (right-click a row: Adopt / Open New Session Here / Bring Window To Front)");
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
                AgentSetTip(_treeSortBtn, L"Sort \x2014 NEWEST / OLDEST / MOST ACTIVE (currently-running first) / A\x2013Z / BY PID (group by host window/shell \x2014 same as the pid underline color \x2014 then most active). Applies to every scope; global \x2014 it persists and applies to all windows.");
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
                AgentSetTip(_treeRefreshBtn, L"Refresh \x2014 reload the tree's data for the current scope (LOCAL / GLOBAL / EXTERNAL): re-survey now + redraw.");
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
                iconCol.Children().Append(mkIconBtn(L"Focus session (jump to its tab)", fluentGlyph(L"\xE7B3"), [this]() {
                    if (_activateHandler && !_selectedId.empty())
                    {
                        _activateHandler(winrt::hstring{ _selectedId });
                    }
                }));
                // Exclamation point = Send now (a literal bold "!"; confirmed before it fires).
                iconCol.Children().Append(mkIconBtn(L"Send now (asks first)", textIconGlyph(L"!"), [this]() { _OnSendNow(); }));
                // Envelope = Add the composed prompt to the queue.
                iconCol.Children().Append(mkIconBtn(L"Add to the queue", fluentGlyph(L"\xE715"), [this]() { _OnAddPrompt(); }));
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
                Grid::SetColumn(_addPromptBox, 1);
                composeRow.Children().Append(_addPromptBox);

                // Paper icon at the textarea's TOP-RIGHT: toggles the (collapsed-by-default)
                // Templates row open/closed (Agentmaster). Kept inline (not a Flyout) so its
                // TextBox keeps receiving keypresses — a text box in a popup/ContentDialog gets
                // none in XAML Islands (see Gotchas).
                auto paperBtn = mkIconBtn(L"Templates \x2014 save / apply prompt plans", fluentGlyph(L"\xE8A5"), [this]() {
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
                auto mkBtn = [&](const winrt::hstring& label, std::function<void()> fn) {
                    auto btn = Button{};
                    btn.Content(winrt::box_value(label));
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
                _templatesRow.Children().Append(_templateNameBox);
                _templatesRow.Children().Append(mkBtn(L"Save as template", [this]() { _OnSaveTemplate(); }));
                _templateCombo = ComboBox{};
                _templateCombo.MinWidth(140);
                _templatesRow.Children().Append(_templateCombo);
                _templatesRow.Children().Append(mkBtn(L"Apply", [this]() { _OnApplyTemplate(false); }));
                _templatesRow.Children().Append(mkBtn(L"Apply to dir", [this]() { _OnApplyTemplate(true); }));
                actions.Children().Append(_templatesRow);
                _RefreshTemplateCombo();

                Grid::SetRow(actions, 2);
                outer.Children().Append(actions);

                // Header row: the title + an Autopilot mode toggle (Agentmaster) — mirrors the
                // EXPLORER TREE LOCAL/GLOBAL toggle, but acts on the SELECTED session. A colored
                // state dot (gray circle = Off, amber half = Semi, green disc = Full) emphasizes
                // the mode; clicking cycles Off -> Semi-auto -> Full. Dim/disabled with no live
                // session selected.
                auto headerLabel = StackPanel{};
                headerLabel.Orientation(Orientation::Horizontal);
                headerLabel.Spacing(8);
                headerLabel.VerticalAlignment(VerticalAlignment::Center);
                headerLabel.Children().Append(Text(L"FLIGHT PLAN", 12, true, 0.8));
                _autopilotBtn = Button{};
                _autopilotBtn.FontSize(11);
                _autopilotBtn.Padding(Thickness{ 8, 1, 8, 1 });
                AgentSetTip(_autopilotBtn, L"Autopilot \x2014 click to cycle Off / Semi-auto / Full for the selected session");
                _autopilotBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleAutopilot(); });
                headerLabel.Children().Append(_autopilotBtn);
                _UpdateAutopilotButton(AutopilotMode::Off, false);

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

        // Agentmaster (Archive page): the old in-content archive overlay is RETIRED — the Archived
        // button now opens the full-window Archive page hosted by TerminalPage (over the tab strip,
        // SetOpenArchiveHandler). _BuildArchiveOverlay() is intentionally NOT called; its methods stay
        // dormant (every call site is null-guarded on _archiveOverlay / _archiveListHost).
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
        std::wstring refocusTag;
        auto refocusState = FocusState::Unfocused;
        if (const auto xr = _root.XamlRoot())
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
        _RebuildTree(sessions);
        _RebuildPlan(sessions);
        _UpdateArchivedButton(sessions);
        _UpdateReopenButton();
        if (_archiveOverlay && _archiveOverlay.Visibility() == Visibility::Visible)
        {
            _RebuildArchiveList(); // keep the open archive list current as sessions archive/restore
        }

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

    Button AgentManagerContent::_MakeCard(const SessionInfo& s)
    {
        const bool selected = (s.id == _selectedId);
        const auto accent = StateColor(s.state);

        auto stack = StackPanel{};
        stack.Spacing(2);

        stack.Children().Append(Text(OneLine(s.title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ s.title }), 14, true, 1.0));
        // Agentmaster (Codex-launch): a teal "codex" agent pill so a MANAGED Codex card reads distinct
        // from Claude (the implicit default — no pill, visuals unchanged).
        if (s.kind == AgentKind::Codex)
        {
            auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
            cp.Opacity(0.9);
            cp.HorizontalAlignment(HorizontalAlignment::Left);
            stack.Children().Append(cp);
        }
        stack.Children().Append(Text(winrt::hstring{ s.workingDir }, 11, false, 0.6));

        // model · effort · kind adornment (O6, Fleet Observer enrichment): only the parts we know.
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
            addPart(s.model);
            addPart(s.effort);
            if (s.background)
            {
                addPart(L"bg");
            }
            // Presence heartbeat (#5): claude's OWN ~/.claude/sessions/<pid>.json status
            // (busy/idle/waiting), observer-validated against the live process snapshot — an
            // independent PULL signal beside the hook-derived state dot, so a stuck push state
            // (dropped hooks) is visible at a glance. Display FACT only, never SessionState
            // (Rule #13); empty (cleared by ObserveClaude) when no live heartbeat row exists.
            if (!s.presenceStatus.empty())
            {
                addPart(L"hb:" + s.presenceStatus);
            }
            if (!me.empty())
            {
                auto meText = Text(winrt::hstring{ me }, 10, false, 0.55);
                if (!s.presenceStatus.empty())
                {
                    AgentSetTip(meText, L"hb = claude's own heartbeat (busy/idle/waiting), observer-validated — independent of the hook-derived state dot");
                }
                stack.Children().Append(meText);
            }
        }

        // Per-session timing (created-ago / active-for / last-activity-ago) from the transcript.
        {
            const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
            if (auto t = TimingText(s.convCreatedUnixMs, last))
            {
                stack.Children().Append(t);
            }
        }

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
        // Agentmaster: the state-colored border is visual noise at rest on a busy board — show it
        // only when the card is SELECTED or HOVERED. The brush stays the state accent (also pushed
        // onto the Button's PointerOver/Pressed states so a hover shows the accent, not the theme's
        // gray hover border); only the THICKNESS toggles: 0 at rest, 1 on hover, 2 when selected.
        card.BorderBrush(SolidColorBrush{ accent });
        card.Resources().Insert(winrt::box_value(L"ButtonBorderBrushPointerOver"), SolidColorBrush{ accent });
        card.Resources().Insert(winrt::box_value(L"ButtonBorderBrushPressed"), SolidColorBrush{ accent });
        card.BorderThickness(selected ? Thickness{ 2, 2, 2, 2 } : Thickness{ 0, 0, 0, 0 });
        if (!selected)
        {
            // sender == the card; toggle border thickness on hover. Use the sender (never capture
            // the Button into its OWN handler — a strong self-capture leaks the element via the
            // delegate). The brush is owned by the template's PointerOver state (accent, above).
            card.PointerEntered([](const IInspectable& s, const PointerRoutedEventArgs&) {
                if (const auto c = s.try_as<Control>())
                {
                    c.BorderThickness(Thickness{ 1, 1, 1, 1 });
                }
            });
            card.PointerExited([](const IInspectable& s, const PointerRoutedEventArgs&) {
                if (const auto c = s.try_as<Control>())
                {
                    c.BorderThickness(Thickness{ 0, 0, 0, 0 });
                }
            });
        }
        const auto id = s.id;
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
                _SelectSession(id);
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
        _NotifyLensChanged(); // M10: splitter sizes ride in the per-window record too

        ApplyCursor(CoreCursorType::Arrow);
        e.Handled(true);
    }

    void AgentManagerContent::_RebuildBoard(const std::vector<SessionInfo>& sessions)
    {
        _boardHost.Children().Clear();
        _boardCardsById.clear(); // refilled by _MakeCard below (focus-restore map; see _Refresh)
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
                    continue; // archived (closed) sessions live in the "Archived" overlay, not the board
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

            auto hdr = StackPanel{};
            hdr.Orientation(Orientation::Horizontal);
            hdr.Spacing(6);
            hdr.Margin(Thickness{ 0, 0, 0, 6 });
            auto dot = Text(L"\x25CF", 12, false, 1.0);
            dot.Foreground(SolidColorBrush{ StateColor(col.state) });
            hdr.Children().Append(dot);
            hdr.Children().Append(Text(col.title, 12, true, 0.9));
            hdr.Children().Append(Text(winrt::to_hstring(static_cast<int>(matches.size())), 12, false, 0.6));
            // colStack holds the cards only; _MakeBoardColumn pins the header above a vertically
            // scrolling card list so a tall column scrolls within the board height instead of
            // clipping past the bottom edge (the board ScrollViewer's vertical scroll is disabled).
            for (const auto* s : matches)
            {
                colStack.Children().Append(_MakeCard(*s));
            }

            _boardHost.Children().Append(_MakeBoardColumn(hdr, colStack));
        }

        // Agentmaster (O6): a trailing observe-only "External (N)" group for real-WindowsTerminal
        // claudes the observer detected (NOT our tabs — no registry session, no Flight Plan). Shown
        // unscoped (it is a global census, not part of the managed directory tree).
        if (!_externalClaudes.empty())
        {
            _boardHost.Children().Append(_MakeExternalColumn());
        }
    }

    // Agentmaster: assemble one Triage Board column. When `fill` is true the column fills the board
    // height with a pinned `header` (Grid row 0) over a vertically-scrolling `cards` list (row 1),
    // so a tall column (e.g. a large External census) scrolls within the board instead of clipping
    // past the bottom edge — the board's own ScrollViewer (BuildUI) has vertical scroll disabled.
    // When `fill` is false the box hugs its content (a collapsed column: header only, no scroll).
    // Shared by the per-state columns and the External group so they stay visually in lockstep.
    Border AgentManagerContent::_MakeBoardColumn(const UIElement& header, const UIElement& cards, bool fill)
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
    Border AgentManagerContent::_MakeExternalColumn()
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

        for (const auto& ex : _externalClaudes)
        {
            colStack.Children().Append(_MakeExternalCard(ex));
        }
        return _MakeBoardColumn(hdrBtn, colStack, true);
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
            AgentSetTip(sd, winrt::hstring{ L"Codex turn state: " } + CodexStateLabel(ex.codexState));
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
            stack.Children().Append(p);
        }
        if (!ex.cwd.empty())
        {
            stack.Children().Append(Text(winrt::hstring{ ex.cwd }, 11, false, 0.55));
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
            stack.Children().Append(Text(winrt::hstring{ hb }, 10, false, 0.5));
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
            stack.Children().Append(Text(winrt::hstring{ me }, 10, false, 0.5));
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
        auto card = Button{};
        card.Content(stack);
        card.HorizontalAlignment(HorizontalAlignment::Stretch);
        card.HorizontalContentAlignment(HorizontalAlignment::Left);
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
                a.codexState != b.codexState) // Phase C2: a Codex turn flip (running<->waiting) repaints the row's state dot
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
                    // Multi-line titles: Return INSERTS a newline (AcceptsReturn) rather than
                    // committing — matching the WT tab-rename box (TabHeaderControl). The commit
                    // is focus-loss (click away / select another row); Escape still cancels. The
                    // TextBox marks Return handled, so it won't bubble to the row's Enter=Activate.
                    box.AcceptsReturn(true);
                    box.TextWrapping(TextWrapping::Wrap);
                    box.KeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (e.Key() == VirtualKey::Escape)
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
                row.Children().Append(Text(OneLine(s.title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ s.title }), 13, false, 1.0));
                // Agentmaster (Codex-launch): a teal "codex" agent pill on a MANAGED Codex row, mirroring
                // the Board card — distinguishes it from a Claude row at a glance (Claude = no pill).
                if (s.kind == AgentKind::Codex)
                {
                    auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
                    cp.Opacity(0.9);
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
                // Right-click (or context key / long-press) menu: Rename / Archive / Open New Session Here.
                rowBtn.ContextFlyout(_MakeSessionMenu(id, s.workingDir));
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
                // ● state dot. A Claude external carries no PULL state -> gray (observe-only). For a
                // Codex row (Phase C2) the rollout-derived turn state colors it: blue running / gold
                // waiting / gray idle.
                auto g = Text(L"\x25CF", 12, false, 1.0);
                if (ex.kind == AgentKind::Codex)
                {
                    g.Foreground(SolidColorBrush{ CodexStateColor(ex.codexState) });
                    AgentSetTip(g, winrt::hstring{ L"Codex turn state: " } + CodexStateLabel(ex.codexState));
                }
                else
                {
                    g.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E));
                }
                row.Children().Append(g);
                // Agentmaster (Phase C1): a teal "codex" agent pill on Codex rows (Claude = default, no pill).
                if (ex.kind == AgentKind::Codex)
                {
                    auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
                    cp.Opacity(0.9);
                    row.Children().Append(cp);
                }
                row.Children().Append(Text(winrt::hstring{ title }, 13, false, 1.0));

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
                        row.Children().Append(Text(winrt::hstring{ me }, 11, false, 0.5));
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
                    AgentSetTip(underline, winrt::hstring{ L"Host window/shell pid " } + winrt::to_hstring(key) + L" \x2014 rows with the same underline color share a terminal window/tab");
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
                                   winrt::hstring{ L"This codex hasn't been prompted yet (no rollout) \x2014 Adopt launches a fresh managed codex here" } :
                                   winrt::hstring{ L"Bring this codex's conversation under management \x2014 Fork a safe copy (codex fork) or Resume the same rollout" });
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
                AgentSetTip(copyIdCx, L"No rollout id yet (this codex hasn't been prompted)");
            }
            else
            {
                AgentSetTip(copyIdCx, L"Copy this codex rollout's conversation id to the clipboard");
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
                                   winrt::hstring{ L"This claude hasn't been prompted yet (no transcript) \x2014 Adopt launches a fresh managed session here" } :
                                   winrt::hstring{ L"Bring this external claude's conversation under management \x2014 Fork a safe copy (--fork-session) or Resume the same conversation" });
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
                if (disp)
                {
                    disp.TryEnqueue([weak, cwd]() { if (auto self = weak.get()) { if (self->_spawnHandler) { self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{}); } } });
                }
                else if (auto self = weak.get())
                {
                    if (self->_spawnHandler)
                    {
                        self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{});
                    }
                }
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
                AgentSetTip(copyId, L"No conversation id yet (this claude hasn't been prompted)");
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

        // All items defer one tick: a MenuFlyout restores focus to its target as it closes,
        // which would otherwise yank focus out of the freshly-shown rename editor / dialog (and the
        // spawn / tree rebuild for Open New Session Here).
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

        // Open New Session Here — the LAST option in every scope (LOCAL/GLOBAL here, EXTERNAL in
        // _MakeExternalTreeMenu): spawn a managed Claude session in THIS row's working dir, a new
        // independent conversation. Uses the row's cwd captured at build time (a session's workingDir
        // is fixed at launch).
        MenuFlyoutItem openHere;
        openHere.Text(L"Open New Session Here");
        AgentSetTip(openHere, L"Launch a managed Claude session in this directory (a new, independent conversation)");
        openHere.Click([weak, disp, cwd](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, cwd]() { if (auto self = weak.get()) { if (self->_spawnHandler) { self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{}); } } });
            }
            else if (auto self = weak.get())
            {
                if (self->_spawnHandler)
                {
                    self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{});
                }
            }
        });
        menu.Items().Append(openHere);

        // Copy Session Id — put this session's conversation id (UUID) on the clipboard. Synchronous:
        // a pure clipboard write has no focus/tree-rebuild race, so it needs no defer like the items
        // above (matches the Archive page's "Copy id").
        MenuFlyoutItem copyId;
        copyId.Text(L"Copy Session Id");
        AgentSetTip(copyId, L"Copy this session's conversation id to the clipboard");
        copyId.Click([id](const IInspectable&, const RoutedEventArgs&) {
            CopyTextToClipboard(id);
        });
        menu.Items().Append(copyId);

        return menu;
    }

    // Agentmaster: the Flight-Plan message right-click menu. Per-prompt queue ops (Move up / Move
    // down / Delete) appear only on UPCOMING rows (a sent/historical row can't be reordered);
    // Archive (the whole session) is always offered. The queue ops act on `promptId` (the
    // right-clicked row, selecting it first) and — like _MakeSessionMenu — defer one tick so the
    // closing flyout's focus restore doesn't race the list rebuild.
    MenuFlyout AgentManagerContent::_MakePromptMenu(const std::wstring& promptId, bool upcoming)
    {
        MenuFlyout menu;
        auto disp = _dispatcher;
        auto weak = get_weak();

        if (upcoming)
        {
            MenuFlyoutItem up;
            up.Text(L"Move up");
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

            menu.Items().Append(MenuFlyoutSeparator{});
        }

        MenuFlyoutItem archive;
        archive.Text(L"Archive session\x2026");
        archive.Click([weak, disp](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak]() { if (auto self = weak.get()) { if (!self->_selectedId.empty()) { self->_RequestArchive(self->_selectedId); } } });
            }
            else if (auto self = weak.get())
            {
                if (!self->_selectedId.empty())
                {
                    self->_RequestArchive(self->_selectedId);
                }
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

        // Saved window records NOT currently open (Engine-tracked, read off disk). Each is reopenable as
        // a WHOLE — geometry + lens + ALL its tabs re-homed (the window-grouped restore) — distinct from
        // the per-session Restore (cherry-pick ONE session into the CURRENT window). Read only when the
        // overlay is shown (this rebuild), never a hot path.
        std::vector<::Agentmaster::RecoverableWindow> recoverable;
        try
        {
            recoverable = ::Agentmaster::RecoverableWindows();
        }
        CATCH_LOG();

        // Find an archived (!live) session by id (linear scan — the archived set is small).
        const auto findArchived = [&sessions](const std::wstring& id) -> const SessionInfo* {
            for (const auto& s : sessions)
            {
                if (!s.live && s.id == id)
                {
                    return &s;
                }
            }
            return nullptr;
        };

        // One archived-session row: title + dir + prompt count, with a per-session Restore (cherry-pick
        // into the CURRENT window). Under a window group the label reads "Restore here" so it is visibly
        // distinct from that group's whole-window "Reopen window".
        const auto makeSessionRow = [this](const SessionInfo& s, bool groupedInWindow) -> Border {
            auto infoCol = StackPanel{};
            infoCol.Spacing(1);
            infoCol.VerticalAlignment(VerticalAlignment::Center);
            infoCol.Children().Append(Text(OneLine(s.title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ s.title }), 14, true, 1.0));
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
            restore.Content(winrt::box_value(groupedInWindow ? winrt::hstring{ L"Restore here" } : winrt::hstring{ L"Restore" }));
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
            return border;
        };

        bool any = false;
        std::unordered_set<std::wstring> grouped;

        // ---- Saved windows: each reopenable as a whole, its archived sessions listed under it ----
        int ordinal = 0;
        for (const auto& rw : recoverable)
        {
            ++ordinal;
            std::vector<const SessionInfo*> winSessions; // this window's archived Claude sessions, in tab order
            int shells = 0; // its non-Claude (pwsh / cmd) tabs — recreated by "Reopen window", not individually restorable
            for (const auto& t : rw.record.tabs)
            {
                if (t.kind == ::Agentmaster::TabKind::Claude)
                {
                    if (const auto* hit = t.sessionId.empty() ? nullptr : findArchived(t.sessionId))
                    {
                        winSessions.push_back(hit);
                    }
                }
                else if (!t.actionsJson.empty())
                {
                    ++shells;
                }
            }
            if (winSessions.empty() && shells == 0)
            {
                continue; // this window's tabs are all gone (or its sessions were reopened elsewhere)
            }

            auto cardPanel = StackPanel{};
            cardPanel.Spacing(6);

            // Header: composition + the whole-window "Reopen window" (geometry + lens + every tab re-homed).
            auto header = Grid{};
            {
                ColumnDefinition c0;
                c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                ColumnDefinition c1;
                c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
                header.ColumnDefinitions().Append(c0);
                header.ColumnDefinitions().Append(c1);
            }
            auto titleCol = StackPanel{};
            titleCol.Spacing(1);
            titleCol.VerticalAlignment(VerticalAlignment::Center);
            titleCol.Children().Append(Text(winrt::hstring{ L"Saved window " } + winrt::to_hstring(ordinal), 14, true, 0.95));
            std::wstring comp = std::to_wstring(winSessions.size()) + (winSessions.size() == 1 ? L" session" : L" sessions");
            if (shells > 0)
            {
                comp += L" \x00B7 " + std::to_wstring(shells) + (shells == 1 ? L" shell" : L" shells");
            }
            titleCol.Children().Append(Text(winrt::hstring{ comp }, 11, false, 0.6));
            auto reopenBtn = Button{};
            reopenBtn.Content(winrt::box_value(L"Reopen window"));
            reopenBtn.VerticalAlignment(VerticalAlignment::Center);
            const int idx = rw.index;
            reopenBtn.Click([this, idx](const IInspectable&, const RoutedEventArgs&) {
                if (_reopenWindowHandler)
                {
                    _reopenWindowHandler(idx);
                }
                _HideArchive();
            });
            Grid::SetColumn(titleCol, 0);
            Grid::SetColumn(reopenBtn, 1);
            header.Children().Append(titleCol);
            header.Children().Append(reopenBtn);
            cardPanel.Children().Append(header);

            for (const auto* s : winSessions)
            {
                cardPanel.Children().Append(makeSessionRow(*s, true));
                grouped.insert(s->id);
            }

            auto group = Border{};
            group.Background(Fill(0x14, 0x88, 0x99, 0xCC));
            group.BorderBrush(SolidColorBrush{ ColorHelper::FromArgb(0x40, 0x80, 0x90, 0xC0) });
            group.BorderThickness(Thickness{ 1, 1, 1, 1 });
            group.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
            group.Padding(Thickness{ 10, 8, 10, 8 });
            group.Child(cardPanel);
            _archiveListHost.Children().Append(group);
            any = true;
        }

        // ---- Loose archived sessions (closed individually, not part of any saved window) ----
        bool looseHeaderShown = false;
        for (const auto& s : sessions)
        {
            if (s.live || grouped.find(s.id) != grouped.end())
            {
                continue;
            }
            if (any && !looseHeaderShown)
            {
                _archiveListHost.Children().Append(Text(L"Other archived sessions", 12, true, 0.7));
                looseHeaderShown = true;
            }
            _archiveListHost.Children().Append(makeSessionRow(s, false));
            any = true;
        }

        if (!any)
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

        // CLAUDE BINARY (native-exe-only policy): the auto-detected native claude.exe + an optional
        // explicit override. The whole app gates launch/fork/resume on resolving one (ResolveClaudeExe);
        // the override must be a real *.exe (a .cmd/.bat or the Node CLI is rejected).
        panel.Children().Append(Text(L"CLAUDE BINARY (native build required)", 11, true, 0.6));
        _setClaudeDetected = TextBlock{};
        _setClaudeDetected.TextWrapping(TextWrapping::Wrap);
        _setClaudeDetected.Opacity(0.85);
        _setClaudeDetected.FontSize(12);
        panel.Children().Append(_setClaudeDetected);
        _setClaudeExePath = TextBox{};
        _setClaudeExePath.Header(winrt::box_value(L"Override claude.exe path"));
        _setClaudeExePath.PlaceholderText(L"blank \x2014 auto-detect; or a full path to claude.exe");
        panel.Children().Append(_setClaudeExePath);
        {
            auto browse = Button{};
            browse.Content(winrt::box_value(L"Browse for claude.exe\x2026"));
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
        _setWaitingDecay = TextBox{};
        _setWaitingDecay.Header(winrt::box_value(L"Waiting-for-you \x2192 Idle after (minutes)"));
        // Claude's SERVER-SIDE prompt cache expires ~5 minutes after the last turn — past that the
        // session is no longer "hot", so the Triage Board demotes it out of Waiting-for-you.
        _setWaitingDecay.PlaceholderText(L"5 \x2014 Claude's server cache lifetime; 0 = never");
        panel.Children().Append(_setWaitingDecay);
        _setLaunchDir = TextBox{};
        _setLaunchDir.Header(winrt::box_value(L"Default Launch directory"));
        _setLaunchDir.PlaceholderText(L"blank \x2014 defaults to %USERPROFILE%");
        panel.Children().Append(_setLaunchDir);
        _setRecentDirsLimit = TextBox{};
        _setRecentDirsLimit.Header(winrt::box_value(L"Recent Launch directories to remember"));
        _setRecentDirsLimit.PlaceholderText(L"10");
        panel.Children().Append(_setRecentDirsLimit);

        // Sessions browser: un-hide every session removed via the Sessions page's right-click
        // "Hide from list". The list lives in AppSettings.hiddenSessionIds, owned by the page
        // (TerminalPage), so this fires the handler there rather than reading a count the cog
        // doesn't track; the button gives inline confirmation. Re-enabled/relabeled per _ShowSettings.
        _setResetHidden = Button{};
        _setResetHidden.Content(winrt::box_value(L"Reset hidden sessions"));
        AgentSetTip(_setResetHidden, L"Un-hide every session you removed from the Sessions browser with \x201CHide from list\x201D");
        _setResetHidden.Click([this](const IInspectable& sender, const RoutedEventArgs&) {
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
        panel.Children().Append(_setProfileDir);
        auto changeProfile = Button{};
        changeProfile.Content(winrt::box_value(L"Change profile folder\x2026"));
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
        if (_setClaudeExePath)
        {
            _setClaudeExePath.Text(winrt::hstring{ _appSettings.claudeExePath });
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
        if (_setWaitingDecay)
        {
            _setWaitingDecay.Text(winrt::hstring{ std::to_wstring(_appSettings.waitingDecayMinutes) });
        }
        if (_setLaunchDir)
        {
            _setLaunchDir.Text(winrt::hstring{ _appSettings.defaultLaunchDir });
        }
        if (_setRecentDirsLimit)
        {
            _setRecentDirsLimit.Text(winrt::hstring{ std::to_wstring(_appSettings.recentDirsLimit) });
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
        if (_setClaudeExePath)
        {
            std::wstring p{ _setClaudeExePath.Text() };
            const auto a = p.find_first_not_of(L" \t");
            const auto b = p.find_last_not_of(L" \t");
            _appSettings.claudeExePath = (a == std::wstring::npos) ? std::wstring{} : p.substr(a, b - a + 1);
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
        if (_setWaitingDecay)
        {
            const std::wstring t{ _setWaitingDecay.Text() };
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
            // Unlike maxAutoSends, an explicit 0 is MEANINGFUL here (= never decay); only an
            // empty/garbage box falls back to the 5-minute default (the cache lifetime).
            _appSettings.waitingDecayMinutes = any ? v : 5;
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
        // Native-exe-only policy: re-resolve the claude.exe now, so a changed/cleared override (or a
        // freshly-installed binary) takes effect this run — no restart needed (RefreshClaudeExe updates
        // the shared engine's cached path; ClaudeAvailable() flips accordingly).
        ::Agentmaster::RefreshClaudeExe(_appSettings.claudeExePath);
        _HideSettings();
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
        getClaude.Click([](const IInspectable&, const RoutedEventArgs&) {
            try
            {
                winrt::Windows::System::Launcher::LaunchUriAsync(Uri{ L"https://code.claude.com/docs/en/setup" });
            }
            CATCH_LOG();
        });
        auto browse = Button{};
        browse.Content(winrt::box_value(L"Browse for claude.exe\x2026"));
        browse.Click([this](const IInspectable&, const RoutedEventArgs&) { _BrowseForClaudeExe(false); });
        auto recheck = Button{};
        recheck.Content(winrt::box_value(L"Re-check"));
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
            }
            return;
        }

        const auto sel = _Selected(sessions);
        if (!sel || !sel->live) // an archived (closed) session isn't planned here — restore it first
        {
            _planHeaderHost.Children().Append(Text(L"Select a session to plan its prompts.", 13, false, 0.6));
            _UpdateAutopilotButton(AutopilotMode::Off, false); // no live session: dim the header toggle
            return;
        }

        // header
        auto titleRow = StackPanel{};
        titleRow.Orientation(Orientation::Horizontal);
        titleRow.Spacing(8);
        titleRow.Children().Append(Text(OneLine(sel->title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ sel->title }), 16, true, 1.0));
        titleRow.Children().Append(Pill(StateLabel(sel->state), StateColor(sel->state)));
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
                _NotifyLensChanged(); // M10
                _Refresh();
            });
            // Right-click menu: queue ops (Move up / Move down / Delete) on UPCOMING rows only
            // (a sent/historical row can't be reordered), plus Archive session always. showOrigin
            // is true for the SENT summary, false for the UPCOMING queue.
            rowBtn.ContextFlyout(_MakePromptMenu(pid, !showOrigin));
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
            titleRow.Children().Append(Pill(L"codex \x00B7 observe-only", Color{ 0xFF, 0x4E, 0xC9, 0xB0 }));
        }
        else
        {
            titleRow.Children().Append(Pill(L"external \x00B7 observe-only", Colors::Gray()));
        }
        _planHeaderHost.Children().Append(titleRow);
        if (!_selectedExternalCwd.empty())
        {
            _planHeaderHost.Children().Append(Text(winrt::hstring{ _selectedExternalCwd }, 12, false, 0.6));
        }
        // Observe-only here (we host no ConPTY). Both agents can be adopted from the tree's right-click
        // menu — Adopt offers Fork a copy (safe while the original runs) or Resume the same conversation.
        _planHeaderHost.Children().Append(Text(isCodex ? winrt::hstring{ L"Read-only \x2014 an OpenAI Codex session running outside Agentmaster. Right-click it in the tree and \x201C" L"Adopt\x201D to bring its conversation under management (Fork a copy, or Resume)." } : winrt::hstring{ L"Read-only \x2014 runs outside Agentmaster. Right-click it in the tree and \x201C" L"Adopt\x201D to bring its conversation under management (Fork a copy, or Resume)." }, 11, false, 0.5));

        if (_selectedExternalSessionId.empty())
        {
            _planListHost.Children().Append(Text(L"This external session hasn't been prompted yet \x2014 no conversation to show.", 12, false, 0.6));
            return;
        }
        if (_externalPlanLoadedFor != _selectedExternalSessionId)
        {
            _planListHost.Children().Append(Text(L"Loading conversation\x2026", 12, false, 0.6));
            return;
        }
        if (_externalPlanPrompts.empty())
        {
            _planListHost.Children().Append(Text(L"No human prompts found in this conversation.", 12, false, 0.6));
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
        // Selecting a managed session clears any external (read-only) selection — the Flight Plan is
        // one surface; a managed selection wins (it is drivable).
        const bool hadExternal = !_selectedExternalSessionId.empty();
        if (_selectedId == id && !hadExternal)
        {
            return;
        }
        _selectedExternalSessionId.clear();
        _selectedExternalCwd.clear();
        _selectedExternalTitle.clear();
        _selectedId = id;
        _selectedPromptId.clear();
        _NotifyLensChanged(); // M10: selection is part of the per-window lens
        _Refresh();
    }

    // Agentmaster: select an EXTERNAL (observe-only) row -> the Flight Plan shows its conversation
    // READ-ONLY. We host no ConPTY for it (Rule #9/#13), so this never binds an injector; it only
    // surfaces what was prompted. Clears the managed selection (one Flight-Plan surface).
    void AgentManagerContent::_SelectExternal(const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& title, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath)
    {
        _selectedId.clear();
        _selectedPromptId.clear();
        _selectedExternalSessionId = sessionId;
        _selectedExternalCwd = cwd;
        _selectedExternalTitle = title;
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
    }

    void AgentManagerContent::_OnLaunch()
    {
        if (!_cwdBox)
        {
            return;
        }
        _NormalizeCwdBox(); // launch with — and remember — a normalized path (a session id is unaffected)
        const std::wstring text{ _cwdBox.Text() };
        // Agentmaster (Codex-launch): a Codex launch is DIRECTORY-ONLY — codex has no typed-id resume/fork
        // here (Codex resume is reached via the Archive page / window-restore / EXTERNAL Adopt), and while
        // Codex is selected _ValidateLaunchBox keeps the box in directory semantics. Spawn a managed codex in
        // that dir, mirroring the EXTERNAL menu's "Open New Codex Session Here" (pid 0 = no source external,
        // adopt=false = a fresh independent session). _LaunchCodexSession registers the card immediately.
        if (_launchCodex)
        {
            if (_codexLaunchHandler)
            {
                _PushRecentDir(text); // remember it as "recently selected" (shared MRU with Claude launches)
                _ClosePathPicker();
                _codexLaunchHandler(0, winrt::hstring{ text }, false, false); // fresh launch: adopt=false, fork=false
            }
            return;
        }
        // Native-exe-only policy: every CLAUDE interaction (new session or resume) needs a native
        // claude.exe. With none detected, show the install/Browse modal instead of launching. (Codex
        // launches above are a separate runtime and are not gated on claude.exe.)
        if (!::Agentmaster::ClaudeAvailable())
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
                _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after resume (TextChanged -> _ValidateLaunchBox resets the underline/buttons)
            }
            return;
        }
        // A working directory = a new, independent session there.
        if (_spawnHandler)
        {
            _PushRecentDir(text); // remember it as "recently selected"
            _ClosePathPicker();
            _spawnHandler(winrt::hstring{ text }, winrt::hstring{});
        }
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
        if (!::Agentmaster::ClaudeAvailable())
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
            _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after fork (TextChanged -> _ValidateLaunchBox resets the underline/buttons)
        }
    }

    // Agentmaster: paint the Launch box's validation underline + drive the launch/fork buttons.
    // EMPTY -> neutral, "Launch Claude" enabled (defaults). A UUID -> session id: FOUND = green +
    // "Resume session" enabled + Fork shown; NOT found = red + disabled. Otherwise a directory:
    // EXISTS = neutral + "Launch Claude" enabled; MISSING = red + disabled. (Per the design: green
    // is reserved for a found session id; a valid folder stays neutral.)
    void AgentManagerContent::_ValidateLaunchBox()
    {
        if (!_cwdBox || !_launchBtn)
        {
            return;
        }
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

        const auto paint = [this](int state) { // 0 neutral (hidden), 1 green, 2 red
            if (!_cwdUnderline)
            {
                return;
            }
            // transparent (neutral) / green (found id) / red (missing dir or unknown id); kept always
            // present (Height 2) so toggling color never reflows the toolbar.
            _cwdUnderline.Background(state == 0 ? Fill(0x00, 0x00, 0x00, 0x00) : (state == 1 ? Fill(0xFF, 0x4C, 0xAF, 0x50) : Fill(0xFF, 0xE5, 0x39, 0x35)));
        };
        const auto showFork = [this](bool v) {
            if (_forkBtn)
            {
                _forkBtn.Visibility(v ? Visibility::Visible : Visibility::Collapsed);
            }
        };

        // Agentmaster (Codex-launch): while Codex is the selected agent the box is DIRECTORY-ONLY — codex
        // has no --session-id, so there is no typed-id resume (the green found-id state) and no Fork. Empty
        // or an existing dir => enabled "Launch Codex" (neutral underline); a missing dir => red + disabled.
        if (_launchCodex)
        {
            showFork(false);
            if (trimmed.empty())
            {
                paint(0);
                _launchBtn.IsEnabled(true);
            }
            else
            {
                const bool exists = IsDir(NormPath(trimmed));
                paint(exists ? 0 : 2);
                _launchBtn.IsEnabled(exists);
            }
            _launchBtn.Content(winrt::box_value(L"Launch Codex"));
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
        // a working directory: green is reserved for session ids, so a valid dir stays neutral.
        const bool exists = IsDir(NormPath(trimmed));
        paint(exists ? 0 : 2);
        _launchBtn.IsEnabled(exists);
        _launchBtn.Content(winrt::box_value(L"Launch Claude"));
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
        _addPromptBox.Text(L"");
        _Refresh();
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
                }
            });
        }

        if (!textToSend.empty())
        {
            // Inject + submit. (Multiline bodies submit on the first CR for now; bracketed
            // paste for true multi-line prompts is a follow-up.)
            // Agentmaster: check the result and roll the prompt back to Pending on a
            // failed inject. "Send now" marked it Sent above; if the selected session has no stdin
            // injector bound (not a live/bound tab yet, or an observe-only external), injecting fails
            // and the prompt would otherwise be a stranded phantom Sent that was never delivered
            // (Correctness Rule #4). Reverting to Pending keeps it in the queue to retry.
            const bool delivered = _registry->Inject(_selectedId, textToSend + L"\r");
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
