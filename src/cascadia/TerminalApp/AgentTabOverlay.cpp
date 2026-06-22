// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentTabOverlay.h"

#include "AgentCopyActions.h" // the shared CopySessionField (reused by the Triage Board's Copy submenu)
#include "AgentStatusColors.h" // the ONE shared state->color palette (board / overlay / tab dot)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeTranscriptPath / BuildClaude|CodexCommandline (row 3 CLI + transcript)
#include "AgentMaster/ProcessInspect.h" // ReadProcessCommandLine / ReadConversationText / Codex rollout resolve (row 3)
#include "AgentMaster/Persistence.h" // LoadAppSettings (skipPermissions, for the would-use CLI builder)
#include "AgentMaster/Engine.h" // SharedEngine (claudeExePath / codexExePath, for the real launch CLI)

#include <winrt/Windows.UI.h> // Color / ColorHelper / Colors
#include <winrt/Windows.UI.Core.h> // CoreWindow / CoreCursor (summary-panel resize-grip cursors)
#include <winrt/Windows.UI.Text.h> // FontWeights
#include <winrt/Windows.UI.Xaml.Documents.h> // Run / Inlines
#include <winrt/Windows.UI.Xaml.Input.h> // PointerRoutedEventArgs
#include <winrt/Windows.UI.Xaml.Media.h> // SolidColorBrush / FontFamily
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h> // FlyoutBase (Button.Flyout)
#include <winrt/Windows.ApplicationModel.DataTransfer.h> // Clipboard / DataPackage (row 3 copy)

#include <shellapi.h> // ShellExecuteExW (row 3 folder button)
#include <mmsystem.h> // PlaySoundW (row 3 copy/open confirmation chime)
#pragma comment(lib, "winmm.lib")

#include <algorithm> // std::clamp / std::min (summary-panel size fractions)
#include <cmath> // NAN (summary-panel "auto" size sentinel on drag release)
#include <chrono> // DispatcherTimer interval (summary times-line ticker)
#include <string>
#include <unordered_set> // summary file-list de-dup (Edited/Created take over Read)
#include <vector>

using namespace winrt::Windows::Foundation;
// Narrow using-DECLARATIONS for the color helpers: a `using namespace winrt::Windows::UI;` would
// also pull the nested `Text` namespace into scope and clash with the Text() helpers (see the
// AgentManagerContent gotcha / CLAUDE.md).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Core::CoreCursorType; // summary-panel resize-grip cursors
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Documents; // Run / Inlines
using namespace winrt::Windows::UI::Xaml::Input; // PointerRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue
using namespace Agentmaster;

namespace
{
    // Geometric, monochrome glyphs only (match the Manager / Triage Board; no wide color emoji).
    constexpr const wchar_t* kDot = L"\x00B7"; // ·
    constexpr const wchar_t* kHourglass = L"\x23F3"; // ⏳
    // Summary-box section separator SENTINEL: the renderers emit this as a lone line; the DISPLAYED
    // panel turns each into a full-width Border rule (border to border, re-fills on resize), and the
    // COPYABLE summary turns each into a plain-text ─ rule. \x1F (ASCII Unit Separator) never occurs
    // in transcript content, so it's an unambiguous marker.
    constexpr wchar_t kSepMark = L'\x1F';

    // Summary-panel resize bounds (TAB_OVERLAY.md), as FRACTIONS of the pane. The panel is anchored
    // top-right: the left grip grows it leftward (width), the bottom grip downward (height). A 0 stored
    // fraction means "auto" — width capped at kSummaryDefWFrac (the original 20%), height content-driven
    // up to kSummaryDefMaxH. A drag pins an explicit fraction, clamped to these bands.
    constexpr double kSummaryMinWFrac = 0.08; // never thinner than 8% of the pane
    constexpr double kSummaryMaxWFrac = 0.50; // never wider than HALF the pane
    constexpr double kSummaryDefWFrac = 0.20; // the original 20% cap when unset (auto)
    constexpr double kSummaryMinHFrac = 0.06; // never shorter than 6% of the pane
    constexpr double kSummaryMaxHFrac = 0.75; // never taller than THREE-QUARTERS of the pane
    constexpr double kSummaryDefMaxH = 480.0; // the original auto-height cap (px), still capped at 0.75*pane

    // Summary-panel opacity: EXACTLY the badge's values + mechanism (the overlay panel above —
    // _root.Opacity / _SetExpanded): dim at rest, full (bright) on hover.
    constexpr double kSummaryRestOpacity = 0.55; // == the badge's rest opacity (_root.Opacity dim)
    constexpr double kSummaryHoverOpacity = 1.0; // == the badge's hovered opacity (_SetExpanded bright)

    SolidColorBrush Fill(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
    }

    // Drive the window pointer cursor for the resize grips (no per-element cursor in this XAML
    // projection — ProtectedCursor needs a subclass; the Manager/Archive splitters do the same).
    void ApplyCursor(winrt::Windows::UI::Core::CoreCursorType type)
    {
        if (const auto w = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
        {
            w.PointerCursor(winrt::Windows::UI::Core::CoreCursor{ type, 0 });
        }
    }

    // Is SHIFT held right now? A summary-panel resize started with SHIFT down is LOCAL-only (this tab,
    // ephemeral — not persisted, not cross-tab-shared). CoreWindow is available in this app's XAML
    // islands (the splitter cursors above rely on it); if it's somehow absent, treat SHIFT as up so the
    // resize falls back to the shared/persisted default.
    bool IsShiftDown()
    {
        if (const auto w = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
        {
            const auto st = w.GetKeyState(winrt::Windows::System::VirtualKey::Shift);
            return (st & winrt::Windows::UI::Core::CoreVirtualKeyStates::Down) == winrt::Windows::UI::Core::CoreVirtualKeyStates::Down;
        }
        return false;
    }

    // Color-matched to the Triage Board — now via the ONE shared palette (AgentStatusColors.h, the
    // factoring the old hand-synced copy's comment promised), so the overlay badge and the
    // tab-strip status dot can never drift apart.
    Color StateColor(SessionState s)
    {
        return winrt::TerminalApp::implementation::AgentStatusColorFor(s);
    }

    const wchar_t* StateGlyph(SessionState s)
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

    const wchar_t* StateLabel(SessionState s)
    {
        switch (s)
        {
        case SessionState::Running:
            return L"running";
        case SessionState::WaitingForInput:
            return L"waiting";
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

    const wchar_t* ModeLabel(AutopilotMode m)
    {
        switch (m)
        {
        case AutopilotMode::SemiAuto:
            return L"Semi";
        case AutopilotMode::Full:
            return L"Full";
        case AutopilotMode::Off:
        default:
            return L"Off";
        }
    }

    // A short confirmation chime for a completed row-3 action (copy / open). Async so it never blocks
    // the UI thread; SystemAsterisk is the soft Windows notification sound. Best-effort (silent if the
    // user has system sounds off). winmm is already in the link (TerminalPaneContent's WarningBell).
    void PlayActionSound()
    {
        ::PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
    }

    // Put text on the system clipboard (row 3's copy menu). Mirrors AgentManagerContent's
    // CopyTextToClipboard. Flush so the content survives the app losing focus (it can refuse —
    // non-fatal). WinRT Clipboard is STA, so call this on the UI thread. Plays the confirmation
    // chime once the copy actually lands. Best-effort.
    void CopyTextToClipboard(const std::wstring& text)
    {
        try
        {
            winrt::Windows::ApplicationModel::DataTransfer::DataPackage pkg;
            pkg.RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
            pkg.SetText(winrt::hstring{ text });
            winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(pkg);
            winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            PlayActionSound(); // "copy is done" feedback (every row-3 copy routes through here)
        }
        CATCH_LOG();
    }

    // Open a directory via explorer.exe OFF the UI thread (ShellExecuteExW may block; SEE_MASK_NOASYNC
    // makes it safe off the main thread — the AppActionHandlers idiom). The user asked specifically for
    // explorer.exe, so launch it with the (quoted) path as its argument. Best-effort.
    winrt::fire_and_forget OpenPathInExplorerAsync(std::wstring dir)
    {
        co_await winrt::resume_background();
        const std::wstring args = L"\"" + dir + L"\"";
        SHELLEXECUTEINFOW seInfo{ 0 };
        seInfo.cbSize = sizeof(seInfo);
        seInfo.fMask = SEE_MASK_NOASYNC;
        seInfo.lpVerb = L"open";
        seInfo.lpFile = L"explorer.exe";
        seInfo.lpParameters = args.c_str();
        seInfo.nShow = SW_SHOWNORMAL;
        LOG_IF_WIN32_BOOL_FALSE(ShellExecuteExW(&seInfo));
    }

    // The REAL launch command for a session's agent — "the one the tab had / would use", the full
    // thing with hooks (NOT a toy `--resume <id>`). For the session's OWN agent we return its LIVE
    // process commandline (read from the PEB: the exact claude.exe/codex.exe invocation, with the
    // full path, --settings <hooks>, --dangerously-skip-permissions, --session-id/--resume, ...).
    // For the OTHER agent (or when the pid isn't known yet) we reproduce what Agentmaster WOULD use
    // to launch that agent in this dir, via the SAME builders the launch/restore path uses.
    std::wstring BuildLaunchCli(const SessionInfo& s, bool wantCodex)
    {
        const bool ownCodex = (s.kind == AgentKind::Codex);
        auto& eng = ::Agentmaster::SharedEngine();
        if (wantCodex == ownCodex && s.pid)
        {
            const std::wstring live = ::Agentmaster::ReadProcessCommandLine(s.pid);
            if (!live.empty())
            {
                return live; // exactly the command this tab is running
            }
        }
        if (wantCodex)
        {
            const std::wstring resumeUuid = ownCodex ? s.codexSessionId : std::wstring{};
            return ::Agentmaster::BuildCodexCommandline(resumeUuid, {}, eng.codexExePath);
        }
        const std::wstring settingsPath = ::Agentmaster::ToForwardSlashes(::Agentmaster::AgentmasterStateDir() + L"\\hooks-settings.json");
        const bool skipPerms = ::Agentmaster::LoadAppSettings().skipPermissions;
        const bool resume = !ownCodex && !s.id.empty() && ::Agentmaster::ClaudeConversationExists(s.id);
        const std::wstring id = (!ownCodex && !s.id.empty()) ? s.id : ::Agentmaster::NewSessionId();
        return ::Agentmaster::BuildClaudeCommandline(settingsPath, id, resume, skipPerms, {}, eng.claudeExePath);
    }

    // Resolve a session's transcript OFF the UI thread (the claude glob is shallow, but the codex
    // rollout glob recurses the date-sharded sessions tree), read it into a plain-text conversation
    // (user + assistant TEXT only — no tools/results/thinking), then hop back to `disp` to copy
    // (WinRT Clipboard is UI-thread only). No-op when there is no transcript / nothing to copy.
    winrt::fire_and_forget CopyConversationAsync(winrt::Windows::System::DispatcherQueue disp, bool codex, std::wstring claudeId, std::wstring codexId)
    {
        co_await winrt::resume_background();
        std::wstring path;
        if (codex)
        {
            if (!codexId.empty())
            {
                path = ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), codexId);
            }
        }
        else
        {
            path = ::Agentmaster::ResolveClaudeTranscriptPath(claudeId);
        }
        if (path.empty())
        {
            co_return; // no transcript yet (never prompted)
        }
        const std::wstring convo = ::Agentmaster::ReadConversationText(path, codex, 0 /* whole file */);
        if (convo.empty() || !disp)
        {
            co_return;
        }
        disp.TryEnqueue([convo]() { CopyTextToClipboard(convo); });
    }

    // The project-folder name = basename(dirname(transcriptPath)) — session-end.js getFolderName.
    std::wstring FolderFromTranscriptPath(const std::wstring& p)
    {
        const auto s1 = p.find_last_of(L"/\\");
        if (s1 == std::wstring::npos)
        {
            return {};
        }
        const std::wstring dir = p.substr(0, s1);
        const auto s2 = dir.find_last_of(L"/\\");
        return s2 == std::wstring::npos ? dir : dir.substr(s2 + 1);
    }

    // Escape a message for the summary box, then optionally truncate. Newline handling has two modes:
    //  - wrapNewlines == false (default, the session-end.js look): collapse to ONE line — real newlines
    //    become a literal "\n", tabs a literal "\t".
    //  - wrapNewlines == true: PRESERVE the message's real newlines (and tabs) so a multi-line prompt
    //    reads as multiple lines in the panel. \r is dropped either way (CRLF -> LF).
    // Truncation is governed by `truncate` (the GLOBAL summaryPanelTruncate toggle):
    //  - truncate == true (default): cap each message — 6 lines if wrapped (the 7th line onward becomes
    //    "..."), else 500 chars (overflow becomes "...") for the one-line view. The panel scrolls + is
    //    height-capped, so the bound is about per-message readability, not layout safety.
    //  - truncate == false: show the WHOLE message, no cap.
    std::wstring SummaryEscapeMsg(const std::wstring& m, bool wrapNewlines, bool truncate)
    {
        // When collapsing to ONE line (wrap off), first collapse an embedded table — drop its horizontal
        // rule rows ("├────┼────┤" / "|---|---|") and de-frame its data rows ("│ Name │ Age │" ->
        // "Name · Age") — so the box-drawing noise doesn't bury the content (ProcessInspect::
        // StripSummaryTableRules). Wrap ON keeps the table verbatim: a real multi-line table renders
        // aligned in the monospace panel, so its bars + rules are meaningful there.
        std::wstring collapsed;
        if (!wrapNewlines)
        {
            collapsed = ::Agentmaster::StripSummaryTableRules(m);
        }
        const std::wstring& src = wrapNewlines ? m : collapsed;
        std::wstring esc;
        for (const wchar_t ch : src)
        {
            if (ch == L'\n')
                esc += wrapNewlines ? L"\n" : L"\\n";
            else if (ch == L'\r')
                ; // dropped (CRLF -> LF)
            else if (ch == L'\t')
                esc += wrapNewlines ? L"\t" : L"\\t";
            else
                esc += ch;
        }
        if (!truncate)
        {
            return esc; // show everything (the default)
        }
        if (wrapNewlines)
        {
            // Keep at most 6 lines; a 7th (or beyond) collapses to a trailing "..." line.
            size_t seen = 0;
            size_t sixthNl = std::wstring::npos;
            for (size_t i = 0; i < esc.size(); ++i)
            {
                if (esc[i] == L'\n' && ++seen == 6)
                {
                    sixthNl = i;
                    break;
                }
            }
            if (sixthNl != std::wstring::npos)
            {
                // Only elide if there's real content past line 6 — a bare trailing newline (6 lines + an
                // empty remainder) shouldn't sprout a misleading "..." line.
                const bool hasMore = esc.find_first_not_of(L" \t\r\n", sixthNl + 1) != std::wstring::npos;
                esc.erase(sixthNl); // drop the 6th newline + everything after -> keep 6 lines
                if (hasMore)
                {
                    esc += L"\n...";
                }
            }
        }
        else if (esc.size() > 500)
        {
            esc = esc.substr(0, 500) + L"...";
        }
        return esc;
    }

    // ---- "ago" timing for the summary panel's times line (age / last user msg / last activity) ----
    // All timestamps are UTC; "now" is UTC too, so deltas are correct regardless of local TZ.
    constexpr uint64_t kFtEpoch1970 = 116444736000000000ULL; // 100ns ticks 1601-01-01 -> 1970-01-01

    int64_t NowUnixMs()
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<int64_t>((u.QuadPart - kFtEpoch1970) / 10000ULL);
    }

    // Parse "YYYY-MM-DDThh:mm:ss[...]" (the transcript entry.timestamp, UTC) -> unix ms. 0 if unparseable.
    int64_t IsoToUnixMs(const std::wstring& iso)
    {
        if (iso.size() < 19)
        {
            return 0;
        }
        const auto num = [&](size_t pos, int len) -> int {
            int v = 0;
            for (int k = 0; k < len; ++k)
            {
                const wchar_t c = iso[pos + k];
                if (c < L'0' || c > L'9')
                {
                    return -1;
                }
                v = v * 10 + (c - L'0');
            }
            return v;
        };
        const int y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), s = num(17, 2);
        if (y < 1970 || mo < 1 || d < 1 || h < 0 || mi < 0 || s < 0)
        {
            return 0;
        }
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(y);
        st.wMonth = static_cast<WORD>(mo);
        st.wDay = static_cast<WORD>(d);
        st.wHour = static_cast<WORD>(h);
        st.wMinute = static_cast<WORD>(mi);
        st.wSecond = static_cast<WORD>(s);
        FILETIME ft{};
        if (!::SystemTimeToFileTime(&st, &ft))
        {
            return 0;
        }
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<int64_t>((u.QuadPart - kFtEpoch1970) / 10000ULL);
    }

    // Compact "ago" from a unix-ms instant to now: 2d4h12m / 4h12m / 34m / 1m13s / 45s. "" if ms<=0.
    std::wstring FormatAgoMs(int64_t tsUnixMs)
    {
        if (tsUnixMs <= 0)
        {
            return {};
        }
        int64_t sec = (NowUnixMs() - tsUnixMs) / 1000;
        if (sec < 0)
        {
            sec = 0;
        }
        const int64_t d = sec / 86400, h = (sec / 3600) % 24, m = (sec / 60) % 60, s = sec % 60;
        const auto n = [](int64_t v) { return std::to_wstring(v); };
        if (d > 0)
        {
            return n(d) + L"d" + n(h) + L"h" + n(m) + L"m";
        }
        if (h > 0)
        {
            return n(h) + L"h" + n(m) + L"m";
        }
        if (m >= 10)
        {
            return n(m) + L"m";
        }
        if (m > 0)
        {
            return n(m) + L"m" + n(s) + L"s";
        }
        return n(s) + L"s";
    }

    // The one-line times summary: "age 2d4h12m, last user msg 34m, last activity 1m13s". Each part is
    // omitted when its instant is unknown (ms<=0) — so a never-prompted session shows just age, etc.
    std::wstring FormatTimesLine(int64_t createdMs, int64_t lastUserMs, int64_t lastActivityMs)
    {
        std::wstring o;
        const auto add = [&](const wchar_t* label, int64_t ms) {
            const std::wstring ago = FormatAgoMs(ms);
            if (ago.empty())
            {
                return;
            }
            if (!o.empty())
            {
                o += L", ";
            }
            o += label;
            o += L" ";
            o += ago;
        };
        add(L"age", createdMs);
        add(L"last user msg", lastUserMs);
        add(L"last activity", lastActivityMs);
        return o;
    }

    // Render the session-end.js box, adapted to the narrow (20%) summary panel: the same labels +
    // section dividers + mapping, but wrapped (no fixed-width rules). plan-start/plan-end override the
    // header glyph+label; otherwise the live state glyph+label (passed in) is used.
    //
    // `full` picks the audience:
    //  - full=false (the DISPLAYED panel): omit everything the link badge (overlay panel 1) ALREADY
    //    shows — the live-state header (kept only for the plan-start/plan-end signal, which the badge
    //    does NOT show), the session id, Dir, Folder, the launch/resume CLI, and Branch. What's left is
    //    the value-add: Parent/Plan, Duration, Tasks, Messages, Files Read/Edited.
    //  - full=true (the COPYABLE "Summary" — copy menu): the COMPLETE box, including everything trimmed
    //    above (id + resume CLI + Dir + Folder + Branch + the state header), so a copy loses nothing.
    std::wstring RenderSummaryBox(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full, bool wrapNewlines, bool truncate)
    {
        std::wstring glyph = liveGlyph, label = liveLabel;
        const bool isPlan = a.hasPlanContent || a.hasExitPlanMode;
        if (a.hasPlanContent)
        {
            glyph = L"\U0001F680"; // 🚀
            label = L"plan-start";
        }
        else if (a.hasExitPlanMode)
        {
            glyph = L"\U0001F4CB"; // 📋
            label = L"plan-end";
        }

        std::wstring o;
        const auto line = [&o](const std::wstring& s) { o += s; o += L"\n"; };
        // A section divider: a lone sentinel line, suppressed at the very top (a leading rule with
        // nothing above it reads as a stray bar). The display turns it into a full-width Border rule.
        const auto sep = [&o]() { if (!o.empty()) { o += kSepMark; o += L"\n"; } };

        // Header: full => always (live state / plan). UI => only the plan-start/plan-end signal (the
        // badge already shows the live state, so a non-plan header would just duplicate it).
        if (full || isPlan)
        {
            line(glyph + L"  " + label);
        }
        if (full)
        {
            line(id);
        }
        if (a.hasPlanContent && !a.parentSessionId.empty())
        {
            line(L"Parent: " + a.parentSessionId);
            if (!planFile.empty())
            {
                line(L"Plan:   " + planFile);
            }
        }
        else if (!planFile.empty())
        {
            line(L"Plan:   " + planFile);
        }
        if (full)
        {
            line(L"Dir:    " + cwd);
            if (const std::wstring folder = FolderFromTranscriptPath(transcriptPath); !folder.empty())
            {
                line(L"Folder: " + folder);
            }
            line(L"Resume: " + resumeCmd);
        }
        if (full && !a.branch.empty())
        {
            line(L"Branch: " + a.branch);
        }
        if (a.tasksCompleted > 0 || a.tasksPending > 0)
        {
            line(L"Tasks:  " + std::to_wstring(a.tasksCompleted) + L" done / " + std::to_wstring(a.tasksPending) + L" pending");
        }
        // Agentmaster: the Claude Code idle RECAP, rendered as its own section directly BELOW the times
        // line (panel 1's age/last-user-msg/last-activity bar, drawn above this box) and ABOVE the user
        // messages — the "where we are / what's next" header over the prompt history. Honors the wrap /
        // truncate toggles like a message body. Present in the displayed panel (full=false) and the
        // copyable Summary (full=true).
        if (!a.awaySummary.empty())
        {
            sep();
            line(L"Recap:");
            line(SummaryEscapeMsg(a.awaySummary, wrapNewlines, truncate));
        }
        if (!a.userMsgs.empty())
        {
            sep();
            int i = 1;
            for (const auto& m : a.userMsgs)
            {
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m, wrapNewlines, truncate));
            }
        }
        // De-duplicate the file lists so each path appears in exactly ONE section. Precedence is
        // Created > Edited > Read: a file that was read AND edited shows ONLY under Edited (the
        // "edited takes over read" rule — keep the edit, drop the read), and a created file never
        // also shows as edited or read. Files compare on a normalized key (ASCII case-folded +
        // forward-slashed) so the same path collapses across sections even if its casing/separators
        // differ; the ORIGINAL string is displayed. Within-list repeats are dropped too.
        const auto fileKey = [](const std::wstring& p) {
            std::wstring k;
            k.reserve(p.size());
            for (wchar_t c : p)
            {
                if (c == L'\\')
                {
                    c = L'/';
                }
                else if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
                k.push_back(c);
            }
            return k;
        };
        std::unordered_set<std::wstring> createdKeys, editedKeys;
        for (const auto& f : a.filesCreated)
        {
            createdKeys.insert(fileKey(f));
        }
        for (const auto& f : a.filesEdited)
        {
            editedKeys.insert(fileKey(f));
        }
        std::vector<std::wstring> readShown, editedShown;
        {
            std::unordered_set<std::wstring> seen;
            for (const auto& f : a.filesRead)
            {
                const auto k = fileKey(f);
                if (createdKeys.count(k) || editedKeys.count(k) || !seen.insert(k).second)
                {
                    continue; // Edited/Created take over Read; drop within-list repeats
                }
                readShown.push_back(f);
            }
        }
        {
            std::unordered_set<std::wstring> seen;
            for (const auto& f : a.filesEdited)
            {
                const auto k = fileKey(f);
                if (createdKeys.count(k) || !seen.insert(k).second)
                {
                    continue; // Created takes over Edited; drop within-list repeats
                }
                editedShown.push_back(f);
            }
        }
        if (!readShown.empty())
        {
            sep();
            line(L"Files Read:");
            for (const auto& f : readShown)
            {
                line(L"* " + f);
            }
        }
        if (!a.filesCreated.empty())
        {
            sep();
            line(L"Files Created:");
            for (const auto& f : a.filesCreated)
            {
                line(L"* " + f);
            }
        }
        if (!editedShown.empty())
        {
            sep();
            line(L"Files Edited:");
            for (const auto& f : editedShown)
            {
                line(L"* " + f);
            }
        }
        while (!o.empty() && o.back() == L'\n')
        {
            o.pop_back();
        }
        return o;
    }

    // Codex: a reduced box from the rollout (the rollout exposes no tool files / tasks) — model /
    // effort / sandbox, branch, and the human prompts. Same `full` split as RenderSummaryBox: the UI
    // panel (full=false) shows ONLY the value-add the badge doesn't (the prompts), since the badge
    // already carries state / model·effort / branch; the copyable Summary (full=true) is the complete
    // box (header + id + Dir + Folder + Resume + Model + Branch + prompts).
    std::wstring RenderCodexSummary(const CodexRolloutInfo& info, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, bool full, bool wrapNewlines, bool truncate)
    {
        std::wstring o;
        const auto line = [&o](const std::wstring& s) { o += s; o += L"\n"; };
        const auto sep = [&o]() { if (!o.empty()) { o += kSepMark; o += L"\n"; } };

        if (full)
        {
            line(liveGlyph + L"  " + liveLabel + L"  \x00B7 codex");
            line(id);
            line(L"Dir:    " + cwd);
            if (const std::wstring folder = FolderFromTranscriptPath(transcriptPath); !folder.empty())
            {
                line(L"Folder: " + folder);
            }
            line(L"Resume: " + resumeCmd);
            std::wstring me;
            const auto add = [&me](const std::wstring& p) { if (!p.empty()) { if (!me.empty()) me += L" \x00B7 "; me += p; } };
            add(info.model);
            add(info.effort);
            add(info.sandbox);
            if (!me.empty())
            {
                line(L"Model:  " + me);
            }
            if (!info.gitBranch.empty())
            {
                line(L"Branch: " + info.gitBranch);
            }
        }
        if (!info.userPrompts.empty())
        {
            sep();
            int i = 1;
            for (const auto& m : info.userPrompts)
            {
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m, wrapNewlines, truncate));
            }
        }
        while (!o.empty() && o.back() == L'\n')
        {
            o.pop_back();
        }
        return o;
    }

    // Copy the FULL textual session summary (the complete session-end.js box — id + resume CLI + Dir +
    // Folder + Branch + Model + Duration + Tasks + Messages + Files, i.e. everything the DISPLAYED panel
    // trims because the badge already shows it). Mirrors CopyConversationAsync: resolve + analyze OFF the
    // UI thread, render full=true, then hop back to copy. No-op if there's no transcript / nothing to copy.
    winrt::fire_and_forget CopySummaryAsync(winrt::Windows::System::DispatcherQueue disp, bool codex, std::wstring claudeId, std::wstring codexId, std::wstring cwd, std::wstring resumeCmd, std::wstring glyph, std::wstring label, bool wrapNewlines, bool truncate)
    {
        co_await winrt::resume_background();
        const std::wstring id = codex ? codexId : claudeId;
        std::wstring path;
        if (codex)
        {
            if (!codexId.empty())
            {
                path = ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), codexId);
            }
        }
        else
        {
            path = ::Agentmaster::ResolveClaudeTranscriptPath(claudeId);
        }
        if (path.empty())
        {
            co_return; // no transcript yet (never prompted)
        }
        std::wstring text;
        std::wstring times; // age / last user msg / last activity, snapshotted at copy time
        if (codex)
        {
            const auto info = ::Agentmaster::ReadCodexRolloutInfo(path, 0 /* whole file */, 200 /* prompts */);
            times = FormatTimesLine(info.createdUnixMs, 0 /* no last-user ts in a rollout */, info.lastActivityUnixMs);
            text = RenderCodexSummary(info, id, cwd, path, resumeCmd, glyph, label, /*full*/ true, wrapNewlines, truncate);
        }
        else
        {
            const auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
            times = FormatTimesLine(IsoToUnixMs(a.firstTs), IsoToUnixMs(a.lastUserTs), IsoToUnixMs(a.lastTs));
            std::wstring planFile = a.planFilePath;
            if (planFile.empty() && a.hasPlanContent && !a.parentSessionId.empty())
            {
                const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(a.parentSessionId);
                if (!parentPath.empty())
                {
                    planFile = ::Agentmaster::FindPlanFileInTranscript(parentPath);
                }
            }
            text = RenderSummaryBox(a, id, cwd, path, resumeCmd, glyph, label, planFile, /*full*/ true, wrapNewlines, truncate);
        }
        if (text.empty() || !disp)
        {
            co_return;
        }
        if (!times.empty())
        {
            text = times + L"\n" + text; // the times line leads the copied box (the display shows it live)
        }
        // The clipboard gets PLAIN text, so turn each separator sentinel into a visible ─ rule (the
        // display path turns the same sentinel into a Border instead).
        for (size_t p = text.find(kSepMark); p != std::wstring::npos; p = text.find(kSepMark, p))
        {
            text.replace(p, 1, std::wstring(48, L'\x2500')); // ──────────────────────────────────────────────── (48)
        }
        disp.TryEnqueue([text]() { CopyTextToClipboard(text); });
    }
}

namespace winrt::TerminalApp::implementation
{
    AgentTabOverlay::AgentTabOverlay()
    {
        _dispatcher = DispatcherQueue::GetForCurrentThread();

        // Row 1: a horizontal strip of DISCRETE parts (status · model·effort · Autopilot[button] · queue
        // · link) so every part can carry its OWN tooltip and the Autopilot part can be a clickable button.
        // _Refresh / ShowActivity fill _row1.Children(); right-aligned so the badge hugs the right edge.
        _row1 = StackPanel{};
        _row1.Orientation(Orientation::Horizontal);
        _row1.HorizontalAlignment(HorizontalAlignment::Right);

        // Row 2 label: "<root workdir folder>/<branch>" — secondary (smaller + dimmer), width-capped +
        // ellipsized so a long branch path can't balloon the HUD. Collapsed until _Refresh() fills it;
        // stays collapsed on the registry-less observe badge (no session). (The action buttons used to sit
        // to its left in row 2; they now live in row 1, right after the status block.)
        _subline = TextBlock{};
        _subline.FontSize(11);
        _subline.Foreground(Fill(0xFF, 0xB0, 0xB0, 0xB0));
        _subline.IsTextSelectionEnabled(false);
        _subline.TextWrapping(TextWrapping::NoWrap);
        _subline.TextTrimming(TextTrimming::CharacterEllipsis);
        _subline.VerticalAlignment(VerticalAlignment::Center);
        _subline.MaxWidth(380);
        _subline.Visibility(Visibility::Collapsed);

        // Row 2: just the "<root workdir folder>/<branch>" label, right-aligned so it hugs the right edge.
        // (The action buttons used to live here at index 0; they now sit in ROW 1, immediately after the
        // status block — see _BuildActionsRow / _Refresh.)
        _row2 = StackPanel{};
        _row2.Orientation(Orientation::Horizontal);
        _row2.HorizontalAlignment(HorizontalAlignment::Right);
        _row2.Margin(ThicknessHelper::FromLengths(0, 1, 0, 0)); // a 1px gap below row 1
        _row2.Children().Append(_subline);

        _stack = StackPanel{};
        _stack.Orientation(Orientation::Vertical);
        _stack.Children().Append(_row1);
        _stack.Children().Append(_row2);

        _root = Border{};
        _root.Background(Fill(0xCC, 0x20, 0x20, 0x20)); // dark translucent so it reads on any terminal
        _root.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _root.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _root.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        _root.Padding(ThicknessHelper::FromLengths(7, 2, 7, 2));
        _root.Opacity(0.55); // dim at rest; full on hover (the chosen interaction)
        _root.Child(_stack);

        // Hover handlers are wired in _WireHover() (from Initialize / first ShowActivity), NOT here:
        // they capture get_weak() so they can also reveal row 3 + honor the pinned (menu-open) state,
        // and get_weak() is only valid once the object is fully constructed + ref-counted.
    }

    AgentTabOverlay::~AgentTabOverlay()
    {
        _Detach();
    }

    void AgentTabOverlay::_Detach()
    {
        if (_registry && _observerToken)
        {
            _registry->RemoveObserver(_observerToken);
            _observerToken = 0;
        }
    }

    void AgentTabOverlay::Initialize(const std::wstring& sessionId, std::shared_ptr<::Agentmaster::SessionRegistry> registry)
    {
        _sessionId = sessionId;
        _registry = std::move(registry);
        if (!_dispatcher)
        {
            _dispatcher = DispatcherQueue::GetForCurrentThread();
        }
        if (_registry)
        {
            auto weak = get_weak();
            auto disp = _dispatcher;
            const std::wstring id = _sessionId;
            // Id-filtered observer marshaled to THIS UI thread; self-detached in the dtor (Rule #10).
            _observerToken = _registry->AddObserver([weak, disp, id](const SessionInfo& s, HookEvent) {
                if (s.id != id)
                {
                    return;
                }
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
        _WireHover(); // pointer-over brighten (opacity)
        _BuildActionsRow(); // row-2 actions: folder + copy menu + pencil (linked sessions only), left of the dir/branch label
        _BuildSummaryPanel(); // the 2nd slot (summary panel), collapsed until the pencil toggles it on
        // No single line-wide tooltip here: _Refresh builds row 1 as DISCRETE parts, each with its OWN
        // concise tooltip (status / model·effort / Autopilot / queue / link); the row-2 label + action
        // buttons carry theirs too.
        _Refresh();
    }

    void AgentTabOverlay::ShowActivity(const std::wstring& kind)
    {
        // A tab the observer classified but that is NOT a linked Claude session: a shell ("pwsh" /
        // "cmd"), a never-prompted claude ("claude", no transcript id yet, §11d), or codex. Registry-
        // LESS static badge (no id to observe). The real Initialize()-bound overlay replaces this whole
        // element once a claude resolves its conversation id (first prompt).
        _pending = true;
        _sessionId.clear();
        if (!_dispatcher)
        {
            _dispatcher = DispatcherQueue::GetForCurrentThread();
        }
        _WireHover(); // observe badges still brighten on hover (no actions — that's linked-only)
        if (!_row1 || !_root)
        {
            return;
        }
        if (kind == _lastActivitySig)
        {
            return; // unchanged -> no XAML churn (this runs every probe tick)
        }
        _lastActivitySig = kind;
        _root.Visibility(Visibility::Visible);
        // An observe badge is ONE logical statement ("○ <kind> · unlinked"), so it stays a single
        // TextBlock with one tooltip (unlike the linked badge's per-part row-1 strip built in _Refresh).
        _row1.Children().Clear();
        TextBlock tb{};
        tb.FontSize(12);
        tb.IsTextSelectionEnabled(false);
        tb.TextWrapping(TextWrapping::NoWrap);
        tb.VerticalAlignment(VerticalAlignment::Center);
        Run glyph{};
        glyph.Text(winrt::hstring{ L"\x25CB" }); // ○ gray — observed, but not a linked session
        glyph.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // gray
        glyph.FontWeight(FontWeights::SemiBold());
        tb.Inlines().Append(glyph);
        Run text{};
        text.Text(winrt::hstring{ std::wstring{ L" " } + kind + L"  " + kDot + L"  unlinked" });
        tb.Inlines().Append(text);
        ToolTipService::SetToolTip(tb, winrt::box_value(winrt::hstring{
            L"Observed by Agentmaster, not a linked session. A claude links on its first prompt;\n"
            L"shells (pwsh/cmd) and external codex stay observe-only here." }));
        _row1.Children().Append(tb);
    }

    void AgentTabOverlay::_Refresh()
    {
        if (_pending)
        {
            return; // a pending badge is static — it has no registry session to refresh from
        }
        if (!_row1 || !_registry)
        {
            return;
        }
        const auto info = _registry->Get(_sessionId);
        if (!info)
        {
            if (_root)
            {
                _root.Visibility(Visibility::Collapsed);
            }
            return;
        }
        if (_root)
        {
            _root.Visibility(Visibility::Visible);
        }
        const auto& s = *info;

        int pending = 0;
        for (const auto& p : s.queue)
        {
            if (p.status == PromptStatus::Pending)
            {
                ++pending;
            }
        }

        const bool bound = _registry->HasInjector(_sessionId);
        // Link state is surfaced ONLY when NOT linked (observe / unlinked). When linked (bound) the badge
        // shows NOTHING for link state — a managed tab's badge being present already implies the link, so
        // only the abnormal states are worth calling out.
        const std::wstring link = s.external ? std::wstring{ L"observe" } : std::wstring{ L"unlinked" };

        // Row 1 is built as DISCRETE, individually-tooltipped parts: status · Autopilot[button] · queue
        // · link. A small helper appends a text part (optional tooltip); separators reproduce the
        // one-line look ("  ·  ") as their own tooltip-less elements so the strip reads as one line.
        _row1.Children().Clear();
        const auto fg = Fill(0xFF, 0xEC, 0xEC, 0xEC);
        const auto appendText = [&](const std::wstring& t, const wchar_t* tip, const SolidColorBrush& brush) {
            TextBlock tb{};
            tb.FontSize(12);
            tb.IsTextSelectionEnabled(false);
            tb.TextWrapping(TextWrapping::NoWrap);
            tb.VerticalAlignment(VerticalAlignment::Center);
            tb.Foreground(brush);
            tb.Text(winrt::hstring{ t });
            if (tip)
            {
                ToolTipService::SetToolTip(tb, winrt::box_value(winrt::hstring{ tip }));
            }
            _row1.Children().Append(tb);
        };
        const std::wstring sepText = std::wstring{ L"  " } + kDot + L"  ";
        const auto appendSep = [&]() { appendText(sepText, nullptr, fg); };

        // Status: the colored state glyph + its label, one tooltip for the pair.
        {
            TextBlock tb{};
            tb.FontSize(12);
            tb.IsTextSelectionEnabled(false);
            tb.TextWrapping(TextWrapping::NoWrap);
            tb.VerticalAlignment(VerticalAlignment::Center);
            Run g{};
            g.Text(winrt::hstring{ StateGlyph(s.state) });
            g.Foreground(SolidColorBrush{ StateColor(s.state) });
            g.FontWeight(FontWeights::SemiBold());
            tb.Inlines().Append(g);
            Run lbl{};
            lbl.Text(winrt::hstring{ std::wstring{ L" " } + StateLabel(s.state) });
            lbl.Foreground(fg);
            tb.Inlines().Append(lbl);
            ToolTipService::SetToolTip(tb, winrt::box_value(winrt::hstring{
                L"Live session status, colour-matched to the Triage Board\n"
                L"(running / waiting / needs-approval / error / done / idle)." }));
            _row1.Children().Append(tb);
        }

        // Action buttons (folder / copy menu / pencil) sit immediately AFTER the status block, so the strip
        // reads status (leftmost) -> actions -> autopilot · queue · link. Built once by _BuildActionsRow
        // (a LINKED session only); re-appended here every pass because _row1 is cleared+rebuilt above.
        if (_actions)
        {
            _row1.Children().Append(_actions);
        }

        // Autopilot mode — a CLICKABLE button that cycles Off -> Semi -> Full -> Off (task 2). Colored
        // by mode (gray Off / amber Semi / green Full) to match the Triage Board's Autopilot language.
        appendSep();
        {
            const auto mode = s.autopilot.mode;
            const auto modeBrush = (mode == AutopilotMode::Full)     ? Fill(0xFF, 0x3C, 0xB3, 0x71) :  // MediumSeaGreen
                                   (mode == AutopilotMode::SemiAuto) ? Fill(0xFF, 0xDA, 0xA5, 0x20) :  // Goldenrod
                                                                       Fill(0xFF, 0xB0, 0xB0, 0xB0);   // gray (Off)
            Button b{};
            b.Background(Fill(0x00, 0, 0, 0)); // transparent — still hit-testable; the template gives a hover highlight ("appears clickable")
            b.BorderThickness(ThicknessHelper::FromUniformLength(0));
            b.Padding(ThicknessHelper::FromLengths(2, 0, 2, 0));
            b.MinWidth(0);
            b.MinHeight(0);
            b.IsTabStop(false); // never pull keyboard focus off the ConPTY
            b.VerticalAlignment(VerticalAlignment::Center);
            b.VerticalContentAlignment(VerticalAlignment::Center);
            TextBlock t{};
            t.FontSize(12);
            t.VerticalAlignment(VerticalAlignment::Center);
            t.Foreground(modeBrush);
            t.Text(winrt::hstring{ ModeLabel(mode) });
            b.Content(t);
            ToolTipService::SetToolTip(b, winrt::box_value(winrt::hstring{
                L"Autopilot \x2014 click to cycle Off \x2192 Semi \x2192 Full.\n"
                L"Off: you drive. Semi: it proposes the next queued prompt, you confirm.\n"
                L"Full: it auto-sends the queue on each turn-complete." }));
            const auto weak = get_weak();
            b.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_CycleAutopilot();
                }
            });
            // "appear clickable on hover": a hand cursor over the button (the Button template adds the
            // background highlight). Restore the arrow on exit. CoreWindow drives the cursor (no
            // per-element cursor in this XAML projection — same approach as the summary-panel grips).
            b.PointerEntered([](const IInspectable&, const PointerRoutedEventArgs&) { ApplyCursor(CoreCursorType::Hand); });
            b.PointerExited([](const IInspectable&, const PointerRoutedEventArgs&) { ApplyCursor(CoreCursorType::Arrow); });
            _row1.Children().Append(b);
        }

        // queued (Pending) count — ⏳N.
        if (pending > 0)
        {
            appendSep();
            appendText(std::wstring{ kHourglass } + std::to_wstring(pending),
                       L"Prompts queued and waiting to be sent (Pending).", fg);
        }

        // link state — surfaced ONLY when NOT linked (observe / unlinked); a linked session shows nothing.
        if (!bound)
        {
            appendSep();
            appendText(link,
                       L"Link state \x2014 Agentmaster cannot drive this session.\n"
                       L"observe: read-only (hosted elsewhere). unlinked: not bound.",
                       fg);
        }

        // Row 2: "<root workdir folder>/<branch>" — the leaf of the session's working dir joined with
        // its git branch (e.g. C:/folder/myworkdir + "feature/issue123" -> "myworkdir/feature/issue123").
        // Prefer the persisted M-axis workingDir (the dir the session belongs to); fall back to the live
        // PEB cwd. Hidden when neither a folder nor a branch is known.
        if (_subline)
        {
            std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
            while (!dir.empty() && (dir.back() == L'/' || dir.back() == L'\\'))
            {
                dir.pop_back(); // strip trailing separators so the leaf isn't empty
            }
            std::wstring leaf = dir;
            if (const auto pos = dir.find_last_of(L"/\\"); pos != std::wstring::npos)
            {
                leaf = dir.substr(pos + 1);
            }
            std::wstring sub = leaf;
            if (!s.branch.empty())
            {
                sub = sub.empty() ? s.branch : (sub + L"/" + s.branch);
            }
            if (sub.empty())
            {
                _subline.Visibility(Visibility::Collapsed);
            }
            else
            {
                _subline.Text(winrt::hstring{ sub });
                // Row 2 is width-capped + ellipsized and shows only the leaf folder; the tooltip names
                // what it is and reveals the FULL working path (+ branch) behind it (the Archive-page
                // reveal-behind-truncation pattern).
                std::wstring detail = dir;
                if (!s.branch.empty())
                {
                    detail = detail.empty() ? s.branch : (detail + L"  " + kDot + L"  " + s.branch);
                }
                const std::wstring tip = std::wstring{ L"Working directory \x00B7 git branch\n" } + detail;
                ToolTipService::SetToolTip(_subline, winrt::box_value(winrt::hstring{ tip }));
                _subline.Visibility(Visibility::Visible);
            }
        }

        // Summary panel (2nd slot): show/hide per the persisted toggle + (re)load when the transcript grew.
        _UpdateSummary(s);
    }

    void AgentTabOverlay::_WireHover()
    {
        if (_hoverWired || !_root)
        {
            return;
        }
        _hoverWired = true;
        auto weak = get_weak();
        // Expanded == pointer over the badge OR the copy menu pinned open. Weak captures only, so the
        // handlers (owned by _root) never keep the overlay (which owns _root) alive -> no ref cycle.
        _root.PointerEntered([weak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_hovering = true;
                self->_SetExpanded(true);
            }
        });
        _root.PointerExited([weak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_hovering = false;
                self->_SetExpanded(self->_pinned); // stay open while the copy menu is up
            }
        });
    }

    void AgentTabOverlay::_SetExpanded(bool on)
    {
        // The action buttons are ALWAYS shown now (they live in row 1, right after the status block), so
        // this only dims/brightens the WHOLE badge: dim at rest, full on hover OR while the copy menu is
        // pinned open.
        if (_root)
        {
            _root.Opacity(on ? 1.0 : 0.55);
        }
    }

    // The row-1 Autopilot button: cycle THIS session's mode Off -> Semi-auto -> Full -> Off, mirroring
    // AgentManagerContent::_CycleAutopilot / _OnAutopilotChanged. The overlay already holds the SHARED
    // registry, so we mutate it directly: SessionRegistry::Update fires observers, which (a) marshals our
    // own _Refresh to repaint the button and (b) wakes the scheduler's OnObserved — cycling to Semi/Full
    // while the session sits Idle/WaitingForInput naturally kicks a queued plan (Correctness Rule #1). No
    // new send path is invented (Rule #2); we only set the mode + reset the per-run backstops on arming.
    void AgentTabOverlay::_CycleAutopilot()
    {
        if (!_registry || _sessionId.empty())
        {
            return;
        }
        const auto info = _registry->Get(_sessionId);
        if (!info || !info->live)
        {
            return; // nothing live to drive (same guard as the Manager's header toggle)
        }
        const auto cur = info->autopilot.mode;
        const AutopilotMode next = (cur == AutopilotMode::Off)      ? AutopilotMode::SemiAuto :
                                   (cur == AutopilotMode::SemiAuto) ? AutopilotMode::Full :
                                                                      AutopilotMode::Off;
        _registry->Update(_sessionId, [&](SessionInfo& s) {
            s.autopilot.mode = next;
            if (next != AutopilotMode::Off)
            {
                // Arming resets the per-run backstop counter + clears any stale confirm (== _OnAutopilotChanged).
                s.autopilot.autoSendsThisRun = 0;
                s.pendingConfirmPromptId.clear();
            }
        });
        _Refresh(); // immediate repaint (the async registry observer also refreshes)
    }

    void AgentTabOverlay::_BuildActionsRow()
    {
        if (_actions || !_row1)
        {
            return; // built once, and only for a LINKED session (never an observe badge)
        }
        auto weak = get_weak();

        // A minimal transparent icon button: no chrome at rest, the default template still gives a
        // hover highlight. IsTabStop(false) so it never pulls keyboard focus off the ConPTY.
        const auto mkIconBtn = [](const wchar_t* glyph, const wchar_t* tip) {
            Button b{};
            b.Background(Fill(0x00, 0, 0, 0)); // transparent (alpha 0) — still hit-testable, unlike null
            b.BorderThickness(ThicknessHelper::FromUniformLength(0));
            b.Padding(ThicknessHelper::FromLengths(4, 0, 4, 0));
            b.MinWidth(0);
            b.IsTabStop(false);
            FontIcon fi{};
            fi.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            fi.Glyph(glyph);
            fi.FontSize(12);
            b.Content(fi);
            ToolTipService::SetToolTip(b, winrt::box_value(winrt::hstring{ tip }));
            return b;
        };

        Button folderBtn = mkIconBtn(L"\xE8B7", L"Open the working folder in Explorer"); // Folder
        folderBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_OpenFolder();
            }
        });

        Button copyBtn = mkIconBtn(L"\xE8C8", L"Copy session details\x2026 (id, path, branch, launch CLI, summary, transcript)"); // Copy
        MenuFlyout flyout{};
        // Each menu item carries a tooltip that says exactly WHAT gets copied (the labels are terse;
        // the tip spells out the value), mirroring _CopyField's per-case behavior.
        const auto addItem = [&flyout, weak](const wchar_t* text, const wchar_t* tip, int which) {
            MenuFlyoutItem item{};
            item.Text(text);
            ToolTipService::SetToolTip(item, winrt::box_value(winrt::hstring{ tip }));
            item.Click([weak, which](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_CopyField(which);
                }
            });
            flyout.Items().Append(item);
        };
        addItem(L"Session Id", L"Copy the resumable conversation id (Codex: its rollout uuid)", 0);
        addItem(L"Copy Path", L"Copy the session's working-directory path", 1);
        addItem(L"Copy Branch Name", L"Copy the session's current git branch name", 2);
        addItem(L"Claude Launch CLI", L"Copy the full claude.exe launch command line (with --settings hooks and flags)", 3);
        addItem(L"Codex Launch CLI", L"Copy the full codex launch command line", 4);
        addItem(L"Summary", L"Copy the FULL session summary \x2014 the complete box (id, resume CLI, dir, folder, branch, duration, tasks, messages, files), including everything the displayed panel trims", 6);
        addItem(L"Transcript", L"Copy the whole conversation as text (your prompts + the agent's replies)", 5);
        // The pointer must LEAVE the badge to reach the menu, so pin the expanded state while it's open.
        flyout.Opened([weak](const IInspectable&, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_pinned = true;
                self->_SetExpanded(true);
            }
        });
        flyout.Closed([weak](const IInspectable&, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_pinned = false;
                self->_SetExpanded(self->_hovering);
            }
        });
        copyBtn.Flyout(flyout);

        // Pencil: toggle the SUMMARY PANEL (the 2nd overlay slot, below this badge). The visibility is
        // a GLOBAL setting (AppSettings::showSummaryPanel) — shared across windows + persisted — so the
        // pencil hands off to the page (_ToggleSummary -> _onToggleSummary), which flips it everywhere.
        Button pencilBtn = mkIconBtn(L"\xE70F", L"Show/hide the session summary panel (all tabs)"); // Edit (pencil)
        // Render the pencil from "Segoe MDL2 Assets" rather than "Segoe Fluent Icons": at the SAME codepoint
        // (E70F) the MDL2 glyph draws the pencil WITH a line across its bottom (the line it's drawing), while
        // the Fluent variant is a plain pencil with no line. The with-line look reads as "edit/notes" far
        // better here, so override just this one button's font (folder/copy stay Fluent). MDL2 first, Fluent
        // as the graceful fallback if MDL2 isn't present.
        if (auto pencilIcon = pencilBtn.Content().try_as<FontIcon>())
        {
            pencilIcon.FontFamily(FontFamily{ L"Segoe MDL2 Assets, Segoe Fluent Icons" });
        }
        pencilBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummary();
            }
        });

        _actions = StackPanel{};
        _actions.Orientation(Orientation::Horizontal);
        _actions.VerticalAlignment(VerticalAlignment::Center); // line up with the row-1 status/autopilot parts
        _actions.Spacing(2);
        _actions.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0)); // a small gap after the status block to its left
        _actions.Children().Append(folderBtn);
        _actions.Children().Append(copyBtn);
        _actions.Children().Append(pencilBtn);
        // ALWAYS shown (no longer hover-only). The buttons live in ROW 1 now, immediately to the RIGHT of
        // the status block (so the strip reads status -> actions -> autopilot · queue · link). The actual
        // append is done by _Refresh (it clears + rebuilds row 1 each pass), right after the status part.
    }

    void AgentTabOverlay::_OpenFolder()
    {
        if (!_registry || _sessionId.empty())
        {
            return;
        }
        const auto info = _registry->Get(_sessionId);
        if (!info)
        {
            return;
        }
        // Prefer the persisted M-axis workingDir; fall back to the live PEB cwd (same as row 2).
        std::wstring dir = !info->workingDir.empty() ? info->workingDir : info->liveCwd;
        if (!dir.empty())
        {
            OpenPathInExplorerAsync(dir);
            PlayActionSound(); // same click feedback as the copy menu (Open Path)
        }
    }

    // Agentmaster: the shared copy-field action (declared in AgentCopyActions.h) — the SINGLE
    // implementation behind both this overlay's copy menu (_CopyField, below) and the Triage Board /
    // Explorer-tree session menu's Copy submenu (AgentManagerContent::_MakeSessionMenu). It lives here
    // because the launch-CLI / transcript / summary helpers it leans on are this file's anon-namespace
    // helpers; both menus call it so they can never drift apart. `which` is the copy-menu code (see the
    // header). Synchronous clipboard writes (cases 0-4) run on the calling UI thread; the transcript /
    // summary cases (5/6) read off-thread and hop back via `dispatcher`.
    void CopySessionField(SessionRegistry& registry, const std::wstring& sessionId, int which, const DispatcherQueue& dispatcher, bool wrapNewlines, bool truncate)
    {
        if (sessionId.empty())
        {
            return;
        }
        const auto info = registry.Get(sessionId);
        if (!info)
        {
            return;
        }
        const auto& s = *info;
        const bool codex = (s.kind == AgentKind::Codex);
        switch (which)
        {
        case 0: // Session Id — the resumable conversation id (Codex: its rollout uuid)
        {
            const std::wstring convId = codex ? (s.codexSessionId.empty() ? s.id : s.codexSessionId) : s.id;
            if (!convId.empty())
            {
                CopyTextToClipboard(convId);
            }
            break;
        }
        case 1: // Copy Path — the session's working directory
        {
            const std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
            if (!dir.empty())
            {
                CopyTextToClipboard(dir);
            }
            break;
        }
        case 2: // Copy Branch Name — the session's git branch (live-observed; empty => no-op)
            if (!s.branch.empty())
            {
                CopyTextToClipboard(s.branch);
            }
            break;
        case 3: // Claude Launch CLI — the REAL full command (live commandline / would-use builder)
            CopyTextToClipboard(BuildLaunchCli(s, /*wantCodex*/ false));
            break;
        case 4: // Codex Launch CLI — the REAL full command
            CopyTextToClipboard(BuildLaunchCli(s, /*wantCodex*/ true));
            break;
        case 5: // Transcript — the whole conversation (user + assistant text only), off-thread
            CopyConversationAsync(dispatcher, codex, s.id, s.codexSessionId);
            break;
        case 6: // Summary — the FULL textual session-end.js box (everything; the panel shows a trimmed view)
        {
            const std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
            const std::wstring resume = BuildLaunchCli(s, codex); // the same REAL launch CLI as cases 3/4
            CopySummaryAsync(dispatcher, codex, s.id, s.codexSessionId, dir, resume,
                             std::wstring{ StateGlyph(s.state) }, std::wstring{ StateLabel(s.state) }, wrapNewlines, truncate);
            break;
        }
        default:
            break;
        }
    }

    void AgentTabOverlay::_CopyField(int which)
    {
        if (!_registry)
        {
            return;
        }
        // Delegate to the shared action (reused by the Triage Board's Copy submenu); the overlay's
        // mirrored GLOBAL flags drive the Summary case so its render matches the displayed panel.
        CopySessionField(*_registry, _sessionId, which, _dispatcher, _summaryWrapNewlines, _summaryTruncate);
    }

    void AgentTabOverlay::_BuildSummaryPanel()
    {
        if (_summaryRoot)
        {
            return; // built once (Initialize), and only for a LINKED session (never an observe badge)
        }
        // The times line (age / last user msg / last activity) is a SEPARATE, pinned-at-top TextBlock —
        // NOT part of the mtime-gated content below — so a DispatcherTimer can re-render its "ago"
        // deltas live (every few seconds) without re-reading the transcript. Collapsed until it has text.
        _summaryTimesText = TextBlock{};
        _summaryTimesText.FontFamily(FontFamily{ L"Cascadia Mono" });
        _summaryTimesText.FontSize(11);
        _summaryTimesText.TextWrapping(TextWrapping::Wrap);
        _summaryTimesText.IsTextSelectionEnabled(true);
        _summaryTimesText.Foreground(Fill(0xFF, 0xB0, 0xB0, 0xB0)); // dimmer than the body
        _summaryTimesText.Margin(ThicknessHelper::FromLengths(0, 0, 0, 3)); // a small gap above the content
        _summaryTimesText.Visibility(Visibility::Collapsed);
        ToolTipService::SetToolTip(_summaryTimesText, winrt::box_value(winrt::hstring{
            L"age = time since the conversation started \x00B7 last user msg = since your last prompt"
            L" \x00B7 last activity = since the transcript last changed (updates live)." }));

        // The body is a vertical StackPanel (not one TextBlock) so a section separator can be a
        // full-width Border rule that fills the panel border-to-border + re-fills on resize — a fixed
        // run of ─ chars can't do that in a wrapping block. _SetSummaryContent fills it: monospace,
        // wrapped, selectable TextBlocks for text runs, interleaved with the Border rules.
        _summaryStack = StackPanel{};
        _summaryStack.Orientation(Orientation::Vertical);

        _summaryScroll = ScrollViewer{};
        _summaryScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _summaryScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        // No fixed MaxHeight here — the panel's height is RESIZABLE: _ApplySummarySize sets this
        // viewport's MaxHeight from the (global, persisted) height fraction, defaulting to
        // min(480, 0.75*pane). A long session scrolls past it.
        _summaryScroll.Content(_summaryStack);

        // Header row pinned at the panel top: the live "ago" times line on the LEFT, and a small
        // wrap-line TOGGLE on the RIGHT (same glyph size as the times font). The toggle flips whether the
        // panel preserves a message's real newlines (multi-line) or collapses them to a literal "\n" — a
        // GLOBAL, persisted setting (AppSettings::summaryPanelWrapNewlines), so the overlay can't write it
        // directly: the page handles the flip + broadcast (_onToggleSummaryWrap). A 2-column Grid: times =
        // star (wraps in the remaining width), toggle = auto (hugs the right edge, top-aligned).
        _summaryWrapIcon = FontIcon{};
        _summaryWrapIcon.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // a TEXT font carrying U+21B5 (the icon font would tofu a non-PUA glyph)
        _summaryWrapIcon.Glyph(L"\x21B5"); // ↵ — the newline / line-wrap symbol
        _summaryWrapIcon.FontSize(11); // "same size as the font" (the times line is 11)
        _summaryWrapIcon.FontWeight(FontWeights::SemiBold()); // a touch bolder so the small glyph reads better
        // Enlarge the glyph ~1-2px WITHOUT growing the times-bar row: a centered RenderTransform scale,
        // NOT a bigger FontSize. RenderTransform is applied AFTER layout, so the icon's measured box (and
        // thus the row's line height) is unchanged — the ↵ just renders a hair larger about its center.
        {
            ScaleTransform wrapScale{};
            wrapScale.ScaleX(1.18); // ~11px -> ~13px visual (within the requested 1-2px), height unaffected
            wrapScale.ScaleY(1.18);
            _summaryWrapIcon.RenderTransform(wrapScale);
            _summaryWrapIcon.RenderTransformOrigin(Point{ 0.5f, 0.5f }); // scale about the glyph's center
        }
        // The icon color (dim when off / lighter when on) is set by _UpdateSummaryWrapButtonVisual.

        Button wrapBtn{};
        wrapBtn.Background(Fill(0x00, 0, 0, 0)); // transparent (alpha 0) — still hit-testable; the template still gives a hover highlight
        wrapBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        wrapBtn.Padding(ThicknessHelper::FromLengths(3, 0, 1, 0));
        wrapBtn.MinWidth(0);
        wrapBtn.MinHeight(0);
        wrapBtn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        wrapBtn.VerticalAlignment(VerticalAlignment::Top);
        wrapBtn.HorizontalAlignment(HorizontalAlignment::Right);
        wrapBtn.Content(_summaryWrapIcon);
        ToolTipService::SetToolTip(wrapBtn, winrt::box_value(winrt::hstring{
            L"Wrap message newlines: keep a multi-line prompt as multiple lines instead of a literal \\n (all tabs)" }));
        wrapBtn.Click([weak = get_weak()](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummaryWrap();
            }
        });

        // truncate TOGGLE, to the LEFT of the wrap toggle (same glyph size + scale). ON (default) caps
        // each message — 6 lines when wrapped (7th+ -> "..."), else 500 chars; OFF shows every message
        // in full. Also a GLOBAL, persisted setting (AppSettings::summaryPanelTruncate) flipped via the
        // page (_onToggleSummaryTruncate) so it broadcasts to every linked overlay.
        _summaryTruncateIcon = FontIcon{};
        _summaryTruncateIcon.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // carries U+2026 (the horizontal ellipsis)
        _summaryTruncateIcon.Glyph(L"\x2026"); // … — the truncate / elision symbol
        _summaryTruncateIcon.FontSize(11); // match the times font + the wrap toggle
        _summaryTruncateIcon.FontWeight(FontWeights::SemiBold());
        {
            ScaleTransform truncScale{};
            truncScale.ScaleX(1.18); // same ~1-2px visual bump as the wrap toggle, row height unaffected
            truncScale.ScaleY(1.18);
            _summaryTruncateIcon.RenderTransform(truncScale);
            _summaryTruncateIcon.RenderTransformOrigin(Point{ 0.5f, 0.5f });
        }
        // The icon color (dim when off / lighter when on) is set by _UpdateSummaryTruncateButtonVisual.

        Button truncBtn{};
        truncBtn.Background(Fill(0x00, 0, 0, 0)); // transparent — still hit-testable + hover highlight
        truncBtn.BorderThickness(ThicknessHelper::FromUniformLength(0));
        truncBtn.Padding(ThicknessHelper::FromLengths(3, 0, 1, 0));
        truncBtn.MinWidth(0);
        truncBtn.MinHeight(0);
        truncBtn.IsTabStop(false); // never pull keyboard focus off the ConPTY
        truncBtn.VerticalAlignment(VerticalAlignment::Top);
        truncBtn.HorizontalAlignment(HorizontalAlignment::Right);
        truncBtn.Content(_summaryTruncateIcon);
        ToolTipService::SetToolTip(truncBtn, winrt::box_value(winrt::hstring{
            L"Truncate long messages: cap each to 6 lines (when wrapping) or 500 characters; off shows everything" }));
        truncBtn.Click([weak = get_weak()](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummaryTruncate();
            }
        });

        Grid timesRow{};
        {
            ColumnDefinition cStar{};
            cStar.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); // times line takes the remaining width
            ColumnDefinition cAuto{};
            cAuto.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto)); // toggles hug the right edge
            timesRow.ColumnDefinitions().Append(cStar);
            timesRow.ColumnDefinitions().Append(cAuto);
        }
        // Both toggles live in col1, in a horizontal strip: truncate on the LEFT, wrap on the RIGHT.
        StackPanel toggles{};
        toggles.Orientation(Orientation::Horizontal);
        toggles.VerticalAlignment(VerticalAlignment::Top);
        toggles.HorizontalAlignment(HorizontalAlignment::Right);
        toggles.Children().Append(truncBtn);
        toggles.Children().Append(wrapBtn);
        Grid::SetColumn(_summaryTimesText, 0);
        Grid::SetColumn(toggles, 1);
        timesRow.Children().Append(_summaryTimesText);
        timesRow.Children().Append(toggles);

        // A pinned TITLE row at the very TOP of the panel — above the times line AND the numbered messages
        // — showing the session's title (SessionInfo.title; the ONE value shared by the tab header + the
        // Explorer name, Rule #11). Set live by _UpdateSummary, so a rename updates it in place. Brighter +
        // a touch bolder than the body so it reads as the heading; a long title wraps within the panel's
        // width cap (never balloons it). Collapsed until it has text.
        _summaryTitleText = TextBlock{};
        _summaryTitleText.FontFamily(FontFamily{ L"Cascadia Mono" });
        _summaryTitleText.FontSize(12);
        _summaryTitleText.FontWeight(FontWeights::SemiBold());
        _summaryTitleText.TextWrapping(TextWrapping::Wrap);
        _summaryTitleText.IsTextSelectionEnabled(true);
        _summaryTitleText.Foreground(Fill(0xFF, 0xF0, 0xF0, 0xF0)); // brighter than the body — it's the heading
        _summaryTitleText.Margin(ThicknessHelper::FromLengths(0, 0, 0, 2)); // a small gap above the times line
        _summaryTitleText.Visibility(Visibility::Collapsed);
        ToolTipService::SetToolTip(_summaryTitleText, winrt::box_value(winrt::hstring{
            L"This session's title \x2014 the same value as the tab name." }));

        StackPanel outer{}; // title + header (times + truncate/wrap toggles) pinned over the scrolling content
        outer.Orientation(Orientation::Vertical);
        outer.Children().Append(_summaryTitleText);
        outer.Children().Append(timesRow);
        outer.Children().Append(_summaryScroll);

        _UpdateSummaryWrapButtonVisual(); // seed the wrap toggle's color from _summaryWrapNewlines (default: dim/off)
        _UpdateSummaryTruncateButtonVisual(); // seed the truncate toggle's color from _summaryTruncate (default: lighter/on)

        // The padded content sits in its own inner border so the resize grips (siblings below) can hug
        // the TRUE panel edges (outside the content's 8/6px inset) while the text keeps its padding.
        Border contentBorder{};
        contentBorder.Padding(ThicknessHelper::FromLengths(8, 6, 8, 6));
        contentBorder.Child(outer);

        // Resize grips (TAB_OVERLAY.md): the panel is anchored top-right, so the LEFT edge grows width,
        // the BOTTOM edge grows height, and the BOTTOM-LEFT corner does both. Each is a thin,
        // ~invisible-but-hit-testable strip that brightens + shows a resize cursor on hover. They live
        // INSIDE _summaryRoot (a Grid child), so they collapse with the panel — no extra visibility wiring.
        auto weak = get_weak();
        const auto idleGrip = Fill(0x01, 0xFF, 0xFF, 0xFF); // ~invisible, yet hit-testable
        const auto hotGrip = Fill(0x55, 0xC0, 0xC0, 0xC0); // subtle highlight on hover / drag
        const auto wireGrip = [weak, idleGrip, hotGrip](const Border& grip, bool left, bool bottom, CoreCursorType cursor) {
            grip.Background(idleGrip);
            grip.PointerEntered([grip, hotGrip, cursor](const IInspectable&, const PointerRoutedEventArgs&) {
                ApplyCursor(cursor);
                grip.Background(hotGrip);
            });
            grip.PointerExited([weak, grip, idleGrip](const IInspectable&, const PointerRoutedEventArgs&) {
                if (const auto self = weak.get(); self && self->_summaryDragging)
                {
                    return; // mid-drag the pointer may leave the thin grip — keep it hot
                }
                ApplyCursor(CoreCursorType::Arrow);
                grip.Background(idleGrip);
            });
            grip.PointerPressed([weak, left, bottom, cursor](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                const auto self = weak.get();
                if (!self)
                {
                    return;
                }
                self->_summaryDragging = true;
                self->_summaryDragLeft = left;
                self->_summaryDragBottom = bottom;
                self->_summaryDragShift = IsShiftDown(); // SHIFT at gesture start => local-only resize (decided at release)
                const auto p = e.GetCurrentPoint(nullptr).Position(); // island-relative; only the delta matters
                self->_summaryDragStartX = p.X;
                self->_summaryDragStartY = p.Y;
                // Start from the ACTUAL rendered size, NOT the fraction cap. At rest the panel hugs THIS
                // tab's content (Width/Height == auto), which on a short transcript is much smaller than the
                // shared cap — and the grip sits at that content edge. Seeding the start from the cap would
                // desync the grip from the pointer (the panel jumps to the cap on the first move). Seeding
                // from ActualWidth/Height makes the grip track the pointer 1:1, so the panel grows smoothly
                // PAST this tab's content up to the max band (the forced exact size in _ApplySummarySize).
                // Fall back to the cap only before first layout (ActualWidth/Height == 0). BOTH dims read
                // from _summaryRoot (the OUTER box) — that is the element _ApplySummarySize now forces, so
                // the grip tracks the box edge 1:1 (height used to read the inner scroll viewport, which is
                // ~one header shorter than the box and would desync the bottom grip).
                const double aw = self->_summaryRoot ? self->_summaryRoot.ActualWidth() : 0.0;
                const double ah = self->_summaryRoot ? self->_summaryRoot.ActualHeight() : 0.0;
                self->_summaryDragStartW = aw > 0.0 ? aw : self->_CurrentSummaryWidthPx();
                self->_summaryDragStartH = ah > 0.0 ? ah : self->_CurrentSummaryHeightPx();
                if (const auto el = sender.try_as<UIElement>())
                {
                    el.CapturePointer(e.Pointer());
                }
                ApplyCursor(cursor);
                e.Handled(true);
            });
            grip.PointerMoved([weak](const IInspectable&, const PointerRoutedEventArgs& e) {
                const auto self = weak.get();
                if (!self || !self->_summaryDragging)
                {
                    return;
                }
                const auto p = e.GetCurrentPoint(nullptr).Position();
                self->_OnSummaryDragMove(p.X, p.Y);
                e.Handled(true);
            });
            const auto endHandler = [weak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                if (const auto self = weak.get())
                {
                    self->_OnSummaryDragEnd(sender);
                    e.Handled(true);
                }
            };
            grip.PointerReleased(endHandler);
            grip.PointerCaptureLost(endHandler);
        };

        Border leftGrip{};
        leftGrip.Width(6);
        leftGrip.HorizontalAlignment(HorizontalAlignment::Left);
        leftGrip.VerticalAlignment(VerticalAlignment::Stretch);
        wireGrip(leftGrip, true, false, CoreCursorType::SizeWestEast);
        ToolTipService::SetToolTip(leftGrip, winrt::box_value(winrt::hstring{
            L"Drag to resize the panel width (shared across tabs).\n"
            L"Hold Shift to size this tab only." }));

        Border bottomGrip{};
        bottomGrip.Height(6);
        bottomGrip.HorizontalAlignment(HorizontalAlignment::Stretch);
        bottomGrip.VerticalAlignment(VerticalAlignment::Bottom);
        wireGrip(bottomGrip, false, true, CoreCursorType::SizeNorthSouth);
        ToolTipService::SetToolTip(bottomGrip, winrt::box_value(winrt::hstring{
            L"Drag to resize the panel height (shared across tabs).\n"
            L"Hold Shift to size this tab only." }));

        Border cornerGrip{};
        cornerGrip.Width(14);
        cornerGrip.Height(14);
        cornerGrip.HorizontalAlignment(HorizontalAlignment::Left);
        cornerGrip.VerticalAlignment(VerticalAlignment::Bottom);
        wireGrip(cornerGrip, true, true, CoreCursorType::SizeNortheastSouthwest); // bottom-left corner == NE/SW diagonal
        ToolTipService::SetToolTip(cornerGrip, winrt::box_value(winrt::hstring{
            L"Drag to resize the panel width and height at once (shared across tabs).\n"
            L"Hold Shift to size this tab only." }));

        Grid layout{};
        layout.Children().Append(contentBorder);
        layout.Children().Append(leftGrip);
        layout.Children().Append(bottomGrip);
        layout.Children().Append(cornerGrip); // last == on top, so the corner wins over the edge grips

        _summaryRoot = Border{};
        _summaryRoot.Background(Fill(0xE6, 0x20, 0x20, 0x20)); // near-opaque dark, matching the badge
        _summaryRoot.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _summaryRoot.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _summaryRoot.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        // Padding now lives on contentBorder (so the grips reach the panel edges).
        _summaryRoot.Child(layout);
        _summaryRoot.Visibility(Visibility::Collapsed); // shown only while the GLOBAL showSummaryPanel is ON
        _summaryRoot.Opacity(kSummaryRestOpacity); // dim at rest — same value as the badge (_root.Opacity)
        // Same hover MECHANISM as the badge (_WireHover/_SetExpanded): brighten to full on pointer-over,
        // back to dim on exit. PointerExited fires only when the pointer truly leaves the panel — moving
        // onto a child grip keeps the parent "entered" — so it stays bright while hovering anywhere on
        // the panel (incl. while resizing).
        _summaryRoot.PointerEntered([weak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (const auto self = weak.get())
            {
                self->_summaryRoot.Opacity(kSummaryHoverOpacity);
            }
        });
        _summaryRoot.PointerExited([weak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (const auto self = weak.get())
            {
                self->_summaryRoot.Opacity(kSummaryRestOpacity);
            }
        });

        // Right-click anywhere on the summary panel => a "Copy Summary" context menu, the SAME action as
        // the badge copy menu's "Summary" item (_CopyField(6) -> the FULL session-end.js box, rendered
        // with this overlay's mirrored wrap/truncate flags so it matches the displayed panel). Built once
        // as a shared MenuFlyout and assigned as the ContextFlyout of the panel root (covers the title /
        // times bar / padding / separators / scroll gaps) AND of every selectable text block the panel
        // renders (the title, the times line, and each body run in _SetSummaryContent) — the panel is
        // text-heavy, and a selectable TextBlock shadows the parent's context menu, so without this a
        // right-click landing on text would offer nothing. Text selection + Ctrl+C still work (only the
        // right-click menu is overridden, not SelectionFlyout).
        _summaryContextMenu = MenuFlyout{};
        {
            MenuFlyoutItem copyItem{};
            copyItem.Text(L"Copy Summary");
            ToolTipService::SetToolTip(copyItem, winrt::box_value(winrt::hstring{
                L"Copy the FULL session summary \x2014 the complete box (id, resume CLI, dir, folder, branch, duration, tasks, messages, files), including everything the displayed panel trims" }));
            copyItem.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_CopyField(6); // == the copy menu's "Summary" (the full textual box, CopySummaryAsync)
                }
            });
            _summaryContextMenu.Items().Append(copyItem);
            // Keep the panel bright while the menu is up: the right-tap moves the pointer onto the popup,
            // which fires the panel's PointerExited and would otherwise dim it (mirrors the badge copy
            // menu's pinned-while-open behaviour). Restore the dim rest state on close.
            _summaryContextMenu.Opened([weak](const IInspectable&, const IInspectable&) {
                if (const auto self = weak.get(); self && self->_summaryRoot)
                {
                    self->_summaryRoot.Opacity(kSummaryHoverOpacity);
                }
            });
            _summaryContextMenu.Closed([weak](const IInspectable&, const IInspectable&) {
                if (const auto self = weak.get(); self && self->_summaryRoot)
                {
                    self->_summaryRoot.Opacity(kSummaryRestOpacity);
                }
            });
        }
        _summaryRoot.ContextFlyout(_summaryContextMenu);
        if (_summaryTitleText)
        {
            _summaryTitleText.ContextFlyout(_summaryContextMenu); // the title is selectable too
        }
        if (_summaryTimesText)
        {
            _summaryTimesText.ContextFlyout(_summaryContextMenu); // the times line is selectable too
        }

        _ApplySummarySize(); // seed MaxWidth/scroll-MaxHeight from the (default/seeded) fractions

        // The live "ago" ticker for the times line. Tick fires on the UI thread; it self-stops once the
        // overlay is gone (weak), so a closed tab never leaks a ticking timer (and we never Stop() it off
        // the UI thread from the dtor). Started/stopped by SetSummaryEnabled.
        _summaryTimer = DispatcherTimer{};
        _summaryTimer.Interval(std::chrono::seconds(5));
        // (reuses the `weak` captured above for the grip handlers)
        _summaryTimer.Tick([weak](const IInspectable& sender, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_UpdateTimesLine();
                self->_RefreshJumpEligibility(); // the gated interval (5 s, visible-only): re-dim stale icons
            }
            else if (const auto t = sender.try_as<DispatcherTimer>())
            {
                t.Stop(); // overlay destroyed — stop ticking (UI thread, safe)
            }
        });
    }

    // The panel's current effective MAX WIDTH in px: an explicit width fraction (clamped to band) times
    // the pane width, or the 20% default when unset. 0 until the host has pushed a pane size.
    double AgentTabOverlay::_CurrentSummaryWidthPx() const
    {
        const double wf = (_summaryWidthFraction > 0.0) ? std::clamp(_summaryWidthFraction, kSummaryMinWFrac, kSummaryMaxWFrac) : kSummaryDefWFrac;
        return (_summaryPaneW > 0.0) ? wf * _summaryPaneW : 0.0;
    }

    // The scroll viewport's current effective MAX HEIGHT in px: an explicit height fraction (clamped)
    // times the pane height, or the original auto cap min(480, 0.75*pane). Always sane (defaults to 480).
    double AgentTabOverlay::_CurrentSummaryHeightPx() const
    {
        if (_summaryHeightFraction > 0.0 && _summaryPaneH > 0.0)
        {
            return std::clamp(_summaryHeightFraction, kSummaryMinHFrac, kSummaryMaxHFrac) * _summaryPaneH;
        }
        return (_summaryPaneH > 0.0) ? std::min(kSummaryDefMaxH, kSummaryMaxHFrac * _summaryPaneH) : kSummaryDefMaxH;
    }

    // Re-apply the panel size from the (global) fractions + the cached pane size. WIDTH is a MaxWidth
    // cap on the panel border — the wrapping monospace body fills to it, and since the panel hugs the
    // top-right the growth is leftward. HEIGHT is the scroll viewport's MaxHeight — the body scrolls
    // past it. Called on a fraction change (SetSummarySize), a pane resize (OnSummaryPaneSize), and live
    // during a grip drag, so the panel stays a constant % of the pane as the window resizes.
    //
    // `forced` (mid grip-drag): ALSO pin an EXPLICIT Width/Height so the panel visibly grows PAST its own
    // content — the size is a GLOBAL/shared setting and other tabs may have more content to fill it, so a
    // drag must be able to reach the max bands even when THIS tab's transcript is short (TAB_OVERLAY.md
    // summary-panel resize). At rest (!forced) we clear that explicit size (NaN == auto), so the panel
    // falls back to the MaxWidth/MaxHeight caps and snaps to hug its own content again — the
    // "release the drag => reset to fit content" the user asked for.
    void AgentTabOverlay::_ApplySummarySize(bool forced)
    {
        if (!_summaryRoot)
        {
            return;
        }
        const double wpx = _CurrentSummaryWidthPx();
        if (wpx > 0.0)
        {
            _summaryRoot.MaxWidth(wpx);
        }
        const double hpx = _CurrentSummaryHeightPx();
        if (_summaryScroll)
        {
            _summaryScroll.MaxHeight(hpx); // the at-rest height cap (content-driven below this)
        }
        // NaN is the special value XAML uses for "Auto" sizing (cf. TabManagement::_UpdateTabView).
        //
        // Grow PAST this tab's content during a drag by pinning an explicit size on the OUTER panel border
        // ONLY — a single DEFINITE box the inner content (title / times / scroll) lays out *within*.
        // CRASH FIX: the previous approach forced the inner ScrollViewer's Height instead. That viewport
        // has an Auto vertical scrollbar + wrapping content, so a forced height made its scrollbar-reflow
        // feed back into the panel's measure and never converge — XAML raised a non-continuable "Layout
        // cycle detected" fail-fast (0xc000027b in Windows.UI.Xaml.dll) on resize. A forced outer box can't
        // cycle: size flows ONE way (box -> content), and nothing observes _summaryRoot's size. When the
        // box is forced taller/wider than this tab's content the surplus is just empty space (the drag
        // preview); on release (!forced) we clear it (NaN == auto) so the panel snaps back to hug content
        // (still capped by MaxWidth and the scroll's MaxHeight).
        if (forced)
        {
            if (wpx > 0.0)
            {
                _summaryRoot.Width(wpx); // force the exact width: grow leftward past content, up to the shared max
            }
            if (hpx > 0.0)
            {
                _summaryRoot.Height(hpx); // force the exact height on the OUTER box (never the scroll viewport)
            }
        }
        else
        {
            _summaryRoot.Width(NAN); // auto => fit content (still capped by MaxWidth)
            _summaryRoot.Height(NAN); // auto => fit content (the scroll's MaxHeight still bounds it)
            if (_summaryScroll)
            {
                _summaryScroll.Height(NAN); // clear any stale forced viewport height from the old (cycle-prone) approach
            }
        }
    }

    void AgentTabOverlay::OnSummaryPaneSize(double paneWidth, double paneHeight)
    {
        _summaryPaneW = paneWidth;
        _summaryPaneH = paneHeight;
        _ApplySummarySize();
    }

    void AgentTabOverlay::SetSummarySize(double widthFraction, double heightFraction)
    {
        // Page-driven mirror of the GLOBAL AppSettings size fractions — on attach (seed) and on a resize
        // anywhere (broadcast). Pure apply; never calls the resize handler (so a broadcast can't loop).
        // A SHIFT-resize detached THIS tab from the shared size (_summarySizeLocalOverride): keep its
        // own size and IGNORE the broadcast until the user's next no-Shift drag re-attaches it.
        if (_summarySizeLocalOverride)
        {
            return;
        }
        _summaryWidthFraction = widthFraction;
        _summaryHeightFraction = heightFraction;
        _ApplySummarySize();
    }

    void AgentTabOverlay::SetSummaryResizeHandler(std::function<void(double, double)> handler)
    {
        _onResizeSummary = std::move(handler);
    }

    // Live grip drag: translate the pointer delta (island-relative; only the delta matters) into the
    // dragged size fraction(s), clamped to band, and re-apply. The panel is anchored top-right, so the
    // LEFT edge widens as the pointer moves left and the BOTTOM edge grows as it moves down.
    void AgentTabOverlay::_OnSummaryDragMove(double pointerX, double pointerY)
    {
        if (!_summaryDragging)
        {
            return;
        }
        if (_summaryDragLeft && _summaryPaneW > 0.0)
        {
            const double newW = _summaryDragStartW - (pointerX - _summaryDragStartX);
            _summaryWidthFraction = std::clamp(newW / _summaryPaneW, kSummaryMinWFrac, kSummaryMaxWFrac);
        }
        if (_summaryDragBottom && _summaryPaneH > 0.0)
        {
            const double newH = _summaryDragStartH + (pointerY - _summaryDragStartY);
            _summaryHeightFraction = std::clamp(newH / _summaryPaneH, kSummaryMinHFrac, kSummaryMaxHFrac);
        }
        _ApplySummarySize(true); // FORCED while dragging: track the pointer past this tab's content, up to the max
    }

    // Grip release: drop pointer capture, restore the cursor, then commit the new size. The fractions are
    // already band-clamped by _OnSummaryDragMove and applied to THIS panel. How it commits depends on
    // whether SHIFT was held at the gesture's start:
    //  - SHIFT held  => LOCAL-only: mark this tab detached (_summarySizeLocalOverride) and do NOT persist
    //                   or broadcast — the size stays on this tab and survives tab switches (in-memory),
    //                   but is NOT saved to settings.json nor pushed to sibling tabs.
    //  - no SHIFT    => SHARED: re-attach this tab and call the resize handler, which does the freshest-
    //                   disk RMW + the live broadcast to every linked overlay in the window (so switching
    //                   tabs shows the same size, and it persists across restart / seeds new windows).
    void AgentTabOverlay::_OnSummaryDragEnd(const IInspectable& sender)
    {
        if (!_summaryDragging)
        {
            return; // a capture-lost echo of our own release, or a stray event
        }
        _summaryDragging = false; // clear BEFORE releasing capture so the re-entrant CaptureLost no-ops
        if (const auto el = sender.try_as<UIElement>())
        {
            el.ReleasePointerCaptures();
        }
        ApplyCursor(CoreCursorType::Arrow);
        // Drop the forced exact Width/Height pinned during the drag and snap back to fit-content (the
        // MaxWidth/MaxHeight caps). The dragged FRACTION is preserved either way — the cap now carries
        // it — so this tab hugs its own (possibly short) content while the shared max we just set still
        // lets fuller tabs fill out. This is the user's "release the drag => reset to fit content".
        _ApplySummarySize(false);
        if (_summaryDragShift)
        {
            _summarySizeLocalOverride = true; // this tab now keeps its own size + ignores shared broadcasts
            return; // ephemeral: no persist, no cross-tab share
        }
        _summarySizeLocalOverride = false; // a no-Shift drag re-attaches this tab to the shared size
        if (_onResizeSummary)
        {
            _onResizeSummary(_summaryWidthFraction, _summaryHeightFraction);
        }
    }

    // Re-render the times line ("age 2d4h12m, last user msg 34m, last activity 1m13s") from the cached
    // instants + NOW, so the "ago" deltas grow live between content reloads (the DispatcherTimer drives
    // this). Last-activity prefers the transcript's CURRENT mtime (a cheap stat) so it keeps ticking
    // even when no content reload happened. Hidden when nothing is known yet.
    void AgentTabOverlay::_UpdateTimesLine()
    {
        if (!_summaryTimesText)
        {
            return;
        }
        int64_t lastAct = _summaryLastActivityMs;
        if (!_summaryPath.empty())
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(_summaryPath.c_str(), GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER li{};
                li.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                const int64_t mtimeMs = static_cast<int64_t>((li.QuadPart - kFtEpoch1970) / 10000ULL);
                if (mtimeMs > lastAct)
                {
                    lastAct = mtimeMs;
                }
            }
        }
        const std::wstring line = FormatTimesLine(_summaryCreatedMs, _summaryLastUserMs, lastAct);
        _summaryTimesText.Text(winrt::hstring{ line });
        _summaryTimesText.Visibility(line.empty() ? Visibility::Collapsed : Visibility::Visible);
        _ApplySummaryVisibility(); // the times line may have just appeared/cleared — re-evaluate the pane
    }

    // Show the summary pane ONLY when it's enabled AND has something to render — content rows OR a
    // non-empty times line. Otherwise it's just an empty box (a never-prompted / no-transcript session,
    // or a not-yet-loaded one), so collapse it. All show/hide of _summaryRoot funnels through here.
    void AgentTabOverlay::_ApplySummaryVisibility()
    {
        if (!_summaryRoot)
        {
            return;
        }
        const bool hasContent = _summaryStack && _summaryStack.Children().Size() > 0;
        const bool hasTimes = _summaryTimesText && !_summaryTimesText.Text().empty();
        _summaryRoot.Visibility((_summaryEnabled && (hasContent || hasTimes)) ? Visibility::Visible : Visibility::Collapsed);
    }

    // Render the rendered-text box into the StackPanel: contiguous text lines become one monospace,
    // wrapped, selectable TextBlock; each separator sentinel line (kSepMark) becomes a full-width
    // Border rule (HorizontalAlignment::Stretch => border-to-border, re-fills as the pane resizes).
    // Agentmaster (SUMMARY_JUMP.md): a rendered Messages line is " <n>. <msg>" (RenderSummaryBox emits
    // exactly one leading space + the 1-based number + ". "). Parse that number to the 0-based prompt
    // index, or -1 if the line isn't a numbered message (files use "* ", tasks use "Tasks:", etc., so the
    // " <digits>. " shape is unique to messages). Tolerates >=1 leading spaces; bails without ". ".
    static int ParseSummaryMsgIndex(const std::wstring& line)
    {
        size_t i = 0;
        while (i < line.size() && line[i] == L' ')
        {
            ++i;
        }
        const size_t ds = i;
        while (i < line.size() && line[i] >= L'0' && line[i] <= L'9')
        {
            ++i;
        }
        if (i == ds || i + 1 >= line.size() || line[i] != L'.' || line[i + 1] != L' ')
        {
            return -1;
        }
        int n = 0;
        for (size_t k = ds; k < i; ++k)
        {
            n = n * 10 + (line[k] - L'0');
            if (n > 1'000'000)
            {
                return -1; // absurd; not a real index
            }
        }
        return n >= 1 ? n - 1 : -1; // 1-based render -> 0-based prompt index
    }

    void AgentTabOverlay::_SetSummaryContent(const std::wstring& text)
    {
        if (!_summaryStack)
        {
            return;
        }
        _summaryStack.Children().Clear();
        _jumpButtons.clear(); // rebuilt below; stale Button refs from the prior render are dropped
        _summaryMsgRows.clear(); // rebuilt below; the highlight (_highlightedMsgIndex) is re-applied after
        std::wstring seg; // accumulated contiguous text lines
        const auto flushSeg = [&]() {
            if (seg.empty())
            {
                return;
            }
            TextBlock tb{};
            tb.FontFamily(FontFamily{ L"Cascadia Mono" });
            tb.FontSize(11);
            tb.TextWrapping(TextWrapping::Wrap);
            tb.IsTextSelectionEnabled(true);
            tb.Foreground(Fill(0xFF, 0xDC, 0xDC, 0xDC));
            tb.Text(winrt::hstring{ seg });
            if (_summaryContextMenu)
            {
                tb.ContextFlyout(_summaryContextMenu); // right-click a body line => the shared "Copy Summary" menu (a selectable TextBlock shadows the parent's)
            }
            _summaryStack.Children().Append(tb);
            seg.clear();
        };
        // Agentmaster (SUMMARY_JUMP.md): a numbered message renders as a 2-column Grid — a JUMP button
        // (col 0, auto) + the wrapping message text (col 1, *) — so the text still wraps within the panel
        // while the button stays put. The button asks the page to center the session's terminal view on
        // where this prompt is rendered (a chime confirms a hit).
        const auto makeJumpRow = [this](const std::wstring& lineStr, int idx) -> winrt::Windows::UI::Xaml::UIElement {
            Grid g{};
            ColumnDefinition c0{};
            c0.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition c1{};
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            g.ColumnDefinitions().Append(c0);
            g.ColumnDefinitions().Append(c1);

            Button jb{};
            jb.Background(Fill(0, 0, 0, 0));
            jb.BorderThickness(ThicknessHelper::FromUniformLength(0));
            jb.Padding(ThicknessHelper::FromLengths(0, 0, 4, 0));
            jb.Margin(ThicknessHelper::FromLengths(0, 0, 0, 0));
            jb.VerticalAlignment(VerticalAlignment::Top);
            jb.Opacity(0.7);
            FontIcon ji{};
            ji.FontFamily(FontFamily{ L"Segoe UI Symbol" }); // a text font carrying U+25B8 (the icon font would tofu it)
            ji.Glyph(L"\x25B8"); // ▸ — "go to / jump"
            ji.FontSize(10);
            jb.Content(ji);
            ToolTipService::SetToolTip(jb, winrt::box_value(winrt::hstring{ L"Jump to where this prompt is on screen" }));
            const auto weak = get_weak();
            jb.Click([weak, idx](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
                if (const auto self = weak.get())
                {
                    if (self->_onJumpToPrompt && idx >= 0 && idx < static_cast<int>(self->_summaryUserMsgs.size()))
                    {
                        const int row = self->_onJumpToPrompt(self->_summaryUserMsgs, idx);
                        if (row >= 0)
                        {
                            ::PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
                            self->HighlightSummaryMessage(idx); // mark the row we jumped to
                        }
                        self->_RefreshJumpEligibility(); // a click makes the others eligible to re-check
                    }
                }
            });
            _jumpButtons.emplace_back(idx, jb); // register for eligibility dimming
            Grid::SetColumn(jb, 0);
            g.Children().Append(jb);

            TextBlock tb{};
            tb.FontFamily(FontFamily{ L"Cascadia Mono" });
            tb.FontSize(11);
            tb.TextWrapping(TextWrapping::Wrap);
            tb.IsTextSelectionEnabled(true);
            tb.Foreground(Fill(0xFF, 0xDC, 0xDC, 0xDC));
            tb.Text(winrt::hstring{ lineStr });
            if (_summaryContextMenu)
            {
                tb.ContextFlyout(_summaryContextMenu);
            }
            Grid::SetColumn(tb, 1);
            g.Children().Append(tb);
            _summaryMsgRows.emplace_back(idx, g); // register the row so HighlightSummaryMessage can band it
            return g;
        };
        size_t i = 0;
        // RenderSummaryBox numbers the messages strictly sequentially (1..N, one per prompt). Only treat a
        // line as a real message-START when its number is the NEXT expected one — so in wrap-ON mode a
        // multi-line prompt's continuation line that merely LOOKS like "2. foo" isn't mis-detected as a
        // numbered message (and mis-mapped to the wrong prompt). expectedMsg is the next 0-based index.
        int expectedMsg = 0;
        while (i <= text.size())
        {
            const size_t nl = text.find(L'\n', i);
            const size_t end = (nl == std::wstring::npos) ? text.size() : nl;
            const std::wstring lineStr = text.substr(i, end - i);
            if (lineStr.size() == 1 && lineStr[0] == kSepMark)
            {
                flushSeg(); // close the run above the rule
                Border rule{};
                rule.Height(1);
                rule.HorizontalAlignment(HorizontalAlignment::Stretch); // border to border
                rule.Background(Fill(0x40, 0xFF, 0xFF, 0xFF));
                rule.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
                _summaryStack.Children().Append(rule);
            }
            else if (const int mi = ParseSummaryMsgIndex(lineStr); _onJumpToPrompt && mi == expectedMsg && mi < static_cast<int>(_summaryUserMsgs.size()))
            {
                flushSeg(); // close the run above this numbered message; render it with a jump button
                _summaryStack.Children().Append(makeJumpRow(lineStr, mi));
                ++expectedMsg; // the next message-start must be the following number
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
        flushSeg();
        _RefreshJumpEligibility(); // dim the jump buttons whose prompt isn't currently on screen
        _ApplySummaryHighlight(); // re-apply the jumped-to band onto the freshly-rebuilt rows
    }

    void AgentTabOverlay::SetSummaryToggleHandler(std::function<void()> handler)
    {
        _onToggleSummary = std::move(handler);
    }

    void AgentTabOverlay::SetJumpHandler(std::function<int(const std::vector<std::wstring>&, int)> handler)
    {
        _onJumpToPrompt = std::move(handler);
    }

    void AgentTabOverlay::SetEligibilityHandler(std::function<std::vector<int>(const std::vector<std::wstring>&)> handler)
    {
        _onResolveEligibility = std::move(handler);
    }

    // Agentmaster (SUMMARY_JUMP.md): resolve every numbered prompt against the live buffer in one pass and
    // DIM the jump buttons whose prompt currently won't resolve (scrolled off / not rendered), so the dead
    // icons are visually distinct from the working ones. Called on (re)build, on the 5 s times-line tick
    // (visible-only), and after a click. Bounded + opt-in (only when the summary panel is shown), so the
    // cost is a single linearize+resolve at most every few seconds per visible panel.
    void AgentTabOverlay::_RefreshJumpEligibility()
    {
        if (!_onResolveEligibility || _jumpButtons.empty() || _summaryUserMsgs.empty())
        {
            return;
        }
        const auto rows = _onResolveEligibility(_summaryUserMsgs);
        for (const auto& [idx, btn] : _jumpButtons)
        {
            if (!btn)
            {
                continue;
            }
            const bool ok = idx >= 0 && idx < static_cast<int>(rows.size()) && rows[idx] >= 0;
            btn.Opacity(ok ? 0.75 : 0.2); // match: normal; no-match: clearly dim (still clickable -> re-checks)
        }
    }

    // Agentmaster (SUMMARY_JUMP.md): remember the message we jumped to and paint the band on its row.
    // Called from the ▸ button (idx known directly) and from the page after alt+up / alt+down nav (the
    // landed 0-based index). The mark moves to the new target on the next jump.
    void AgentTabOverlay::HighlightSummaryMessage(int index)
    {
        _highlightedMsgIndex = index;
        _ApplySummaryHighlight();
        // Bring the freshly-targeted row into view within the (scrolling) panel — only on an explicit jump,
        // not on the rebuild re-apply, so a transcript-growth refresh doesn't keep yanking the panel scroll.
        for (const auto& [idx, row] : _summaryMsgRows)
        {
            if (row && idx == index)
            {
                row.StartBringIntoView();
                break;
            }
        }
    }

    // Paint a translucent accent band behind _highlightedMsgIndex's row; clear every other row. Safe any
    // time — a no-op when the panel isn't built or the index isn't currently rendered — and re-applied at
    // the end of each _SetSummaryContent so the mark survives a transcript-growth re-render.
    void AgentTabOverlay::_ApplySummaryHighlight()
    {
        for (const auto& [idx, row] : _summaryMsgRows)
        {
            if (!row)
            {
                continue;
            }
            // accent band (alpha ~0x66) on the target; a fully-transparent fill clears the rest
            row.Background(idx == _highlightedMsgIndex ? Fill(0x66, 0x3B, 0x82, 0xF6) : Fill(0, 0, 0, 0));
        }
    }

    void AgentTabOverlay::SetSummaryEnabled(bool on)
    {
        // The summary panel's visibility is a GLOBAL setting (AppSettings::showSummaryPanel), mirrored
        // into the overlay here by the page — on attach (seed) and on every pencil toggle (broadcast to
        // every linked overlay in the window). _Refresh reads _summaryEnabled too, so the panel stays in
        // step on subsequent registry events.
        _summaryEnabled = on;
        if (!_summaryRoot)
        {
            return; // no panel on the observe badge
        }
        if (!on)
        {
            if (_summaryTimer)
            {
                _summaryTimer.Stop(); // no need to tick the times line while hidden
            }
            _summaryRoot.Visibility(Visibility::Collapsed);
            return;
        }
        if (_summaryTimer)
        {
            _summaryTimer.Start(); // drive the live "ago" times line
        }
        _UpdateTimesLine(); // show the times immediately from whatever is cached
        // Enabled: show + (re)load from the freshest session snapshot.
        if (_registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
                return;
            }
        }
        _ApplySummaryVisibility(); // enabled but no session yet — stay hidden until there's something to show
    }

    void AgentTabOverlay::_ToggleSummary()
    {
        // The pencil flips the GLOBAL setting, not per-session state: hand off to the page, which does
        // the freshest-disk read-modify-write of settings.json AND applies it live to every linked
        // overlay in the window (SetSummaryEnabled). No-op if no handler is wired (defensive).
        if (_onToggleSummary)
        {
            _onToggleSummary();
        }
    }

    void AgentTabOverlay::SetSummaryWrapToggleHandler(std::function<void()> handler)
    {
        _onToggleSummaryWrap = std::move(handler);
    }

    void AgentTabOverlay::_ToggleSummaryWrap()
    {
        // The wrap-line icon flips the GLOBAL setting (AppSettings::summaryPanelWrapNewlines), not
        // per-session state: hand off to the page, which does the freshest-disk read-modify-write of
        // settings.json AND applies it live to every linked overlay in the window (SetSummaryWrapNewlines).
        if (_onToggleSummaryWrap)
        {
            _onToggleSummaryWrap();
        }
    }

    // Recolor the wrap-line icon to reflect the toggle: a dim gray when OFF (the literal-\n look),
    // a clearly lighter shade when ON (newlines preserved) — the "slight color lighter change on true".
    void AgentTabOverlay::_UpdateSummaryWrapButtonVisual()
    {
        if (!_summaryWrapIcon)
        {
            return;
        }
        _summaryWrapIcon.Foreground(_summaryWrapNewlines ? Fill(0xFF, 0xE6, 0xE6, 0xE6)  // ON: lighter (active)
                                                         : Fill(0xFF, 0x8C, 0x8C, 0x8C)); // OFF: dim (inactive)
    }

    void AgentTabOverlay::SetSummaryWrapNewlines(bool on)
    {
        // The newline-wrap mode is a GLOBAL setting (AppSettings::summaryPanelWrapNewlines), mirrored into
        // the overlay here by the page — on attach (seed) and on every wrap-toggle (broadcast to every
        // linked overlay in the window). The flag is BAKED into the rendered text (SummaryEscapeMsg), so a
        // real change must force a re-analyze+render: reset the mtime gate and re-pull from the freshest
        // snapshot. Always refresh the icon color (the seed may match the default but still needs painting).
        const bool changed = (_summaryWrapNewlines != on);
        _summaryWrapNewlines = on;
        _UpdateSummaryWrapButtonVisual();
        if (!changed)
        {
            return; // seed with the same value — nothing baked differently, no reload
        }
        _summaryMtime = 0; // invalidate the mtime gate so _LoadSummaryAsync re-renders with the new flag
        if (_summaryLoading)
        {
            // A load is in flight with the OLD flag; _UpdateSummary would no-op on the guard. Mark dirty so
            // the load's completion re-renders with the now-current flag (else the toggle wouldn't take
            // until the transcript next grew).
            _summaryWrapDirty = true;
            return;
        }
        if (_summaryEnabled && _registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
            }
        }
    }

    void AgentTabOverlay::SetSummaryTruncateToggleHandler(std::function<void()> handler)
    {
        _onToggleSummaryTruncate = std::move(handler);
    }

    void AgentTabOverlay::_ToggleSummaryTruncate()
    {
        // The truncate icon flips the GLOBAL setting (AppSettings::summaryPanelTruncate), not per-session
        // state: hand off to the page, which does the freshest-disk read-modify-write of settings.json AND
        // applies it live to every linked overlay in the window (SetSummaryTruncate).
        if (_onToggleSummaryTruncate)
        {
            _onToggleSummaryTruncate();
        }
    }

    // Recolor the truncate icon to reflect the toggle: a dim gray when OFF (everything shown), a clearly
    // lighter shade when ON (messages capped) — same dim/active palette as the wrap toggle.
    void AgentTabOverlay::_UpdateSummaryTruncateButtonVisual()
    {
        if (!_summaryTruncateIcon)
        {
            return;
        }
        _summaryTruncateIcon.Foreground(_summaryTruncate ? Fill(0xFF, 0xE6, 0xE6, 0xE6)  // ON: lighter (active)
                                                         : Fill(0xFF, 0x8C, 0x8C, 0x8C)); // OFF: dim (inactive)
    }

    void AgentTabOverlay::SetSummaryTruncate(bool on)
    {
        // The truncate mode is a GLOBAL setting (AppSettings::summaryPanelTruncate), mirrored into the
        // overlay here by the page — on attach (seed) and on every truncate-toggle (broadcast to every
        // linked overlay in the window). The flag is BAKED into the rendered text (SummaryEscapeMsg), so a
        // real change must force a re-analyze+render: reset the mtime gate and re-pull from the freshest
        // snapshot. Always refresh the icon color (the seed may match the default but still needs painting).
        const bool changed = (_summaryTruncate != on);
        _summaryTruncate = on;
        _UpdateSummaryTruncateButtonVisual();
        if (!changed)
        {
            return; // seed with the same value — nothing baked differently, no reload
        }
        _summaryMtime = 0; // invalidate the mtime gate so _LoadSummaryAsync re-renders with the new flag
        if (_summaryLoading)
        {
            // A load is in flight with the OLD flag; _UpdateSummary would no-op on the guard. Mark dirty so
            // the load's completion re-renders with the now-current flag (else the toggle wouldn't take
            // until the transcript next grew).
            _summaryTruncateDirty = true;
            return;
        }
        if (_summaryEnabled && _registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
            }
        }
    }

    void AgentTabOverlay::_UpdateSummary(const SessionInfo& s)
    {
        if (!_summaryRoot)
        {
            return; // no panel on the observe badge (built only by Initialize, for a linked session)
        }
        if (!_summaryEnabled)
        {
            _summaryRoot.Visibility(Visibility::Collapsed);
            return;
        }
        // Pinned TITLE row (top of the panel): the session's title == the tab name (Rule #11). Set it every
        // refresh — synchronously on the UI thread, independent of the off-thread transcript load below — so
        // a rename updates it live. Guarded so an unchanged title doesn't relayout the wrapping block each
        // pass. (The panel as a whole still only SHOWS when there's times/content to render —
        // _ApplySummaryVisibility governs that — so this never opens a title-only box.)
        if (_summaryTitleText)
        {
            const winrt::hstring title{ s.title };
            if (_summaryTitleText.Text() != title)
            {
                _summaryTitleText.Text(title);
                _summaryTitleText.Visibility(title.empty() ? Visibility::Collapsed : Visibility::Visible);
            }
        }
        _ApplySummaryVisibility(); // show only if there's already something to render (else stay hidden until the load lands)
        if (_summaryLoading)
        {
            return; // one analyze+render in flight; the next _Refresh picks up any growth
        }
        const bool codex = (s.kind == AgentKind::Codex);
        const std::wstring convId = codex ? (s.codexSessionId.empty() ? s.id : s.codexSessionId) : s.id;
        if (convId.empty())
        {
            return; // a codex not yet reconciled (no rollout uuid) — nothing to analyze
        }
        std::wstring cwd = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
        std::wstring liveGlyph{ StateGlyph(s.state) };
        std::wstring liveLabel{ StateLabel(s.state) };
        _summaryLoading = true;
        _LoadSummaryAsync(_summaryPath, codex, convId, std::move(cwd), std::move(liveGlyph), std::move(liveLabel), _summaryMtime, _summaryWrapNewlines, _summaryTruncate);
    }

    winrt::fire_and_forget AgentTabOverlay::_LoadSummaryAsync(std::wstring transcriptPath, bool codex, std::wstring sessionId, std::wstring cwd, std::wstring liveGlyph, std::wstring liveLabel, int64_t prevMtime, bool wrapNewlines, bool truncate)
    {
        auto strong = get_strong(); // keep the overlay alive across the co_await (it owns _summaryStack)
        co_await winrt::resume_background();

        // Resolve the transcript path once (cached in _summaryPath across reloads). Claude: a shallow
        // glob by conversation id; Codex: a recursive date-sharded glob by rollout uuid (worth caching).
        std::wstring path = transcriptPath;
        if (path.empty())
        {
            path = codex ? ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), sessionId)
                         : ::Agentmaster::ResolveClaudeTranscriptPath(sessionId);
        }

        std::wstring text;
        std::vector<std::wstring> userMsgs; // Agentmaster (SUMMARY_JUMP.md): the raw prompts, aligned with the rendered " N. " lines
        int64_t mtime = prevMtime;
        int64_t createdMs = 0, lastUserMs = 0, lastActivityMs = 0; // times-line instants (computed on reload)
        bool timesComputed = false;
        if (!path.empty())
        {
            // Cheap stat: only do the heavy read+analyze when the transcript grew (mtime advanced) or
            // we've never loaded it (prevMtime == 0). A quiet tab thus costs one GetFileAttributesEx.
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER li{};
                li.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                mtime = static_cast<int64_t>(li.QuadPart);
            }
            if (mtime != prevMtime || prevMtime == 0)
            {
                // The DISPLAYED panel is the TRIMMED view (full=false): it omits everything the link
                // badge already shows. id + resumeCmd are passed empty here — full=false never renders
                // them (the full box, with both, is the copy-menu "Summary" via CopySummaryAsync).
                if (codex)
                {
                    const auto info = ::Agentmaster::ReadCodexRolloutInfo(path, 0 /* whole file */, 200 /* prompts */);
                    createdMs = info.createdUnixMs;
                    lastActivityMs = info.lastActivityUnixMs;
                    lastUserMs = 0; // a rollout carries no per-message timestamp for the last human prompt
                    text = RenderCodexSummary(info, sessionId, cwd, path, L"", liveGlyph, liveLabel, /*full*/ false, wrapNewlines, truncate);
                }
                else
                {
                    const auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
                    userMsgs = a.userMsgs; // the prompts, in order — aligns with the rendered " N. " jump rows
                    createdMs = IsoToUnixMs(a.firstTs);
                    lastUserMs = IsoToUnixMs(a.lastUserTs);
                    lastActivityMs = IsoToUnixMs(a.lastTs);
                    // plan-start: the plan file lives in the PARENT transcript (session-end.js
                    // getPlanFileFromParent) — resolve + scan it when this session points at one.
                    std::wstring planFile = a.planFilePath;
                    if (planFile.empty() && a.hasPlanContent && !a.parentSessionId.empty())
                    {
                        const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(a.parentSessionId);
                        if (!parentPath.empty())
                        {
                            planFile = ::Agentmaster::FindPlanFileInTranscript(parentPath);
                        }
                    }
                    text = RenderSummaryBox(a, sessionId, cwd, path, L"", liveGlyph, liveLabel, planFile, /*full*/ false, wrapNewlines, truncate);
                }
                timesComputed = true;
            }
        }

        // Hop back to the UI thread to publish (the StackPanel build + member writes are UI-thread only).
        if (auto disp = _dispatcher)
        {
            disp.TryEnqueue([weak = get_weak(), text, userMsgs, path, mtime, createdMs, lastUserMs, lastActivityMs, timesComputed]() {
                if (auto self = weak.get())
                {
                    if (timesComputed)
                    {
                        self->_summaryUserMsgs = userMsgs; // set BEFORE _SetSummaryContent so jump rows resolve the right prompt
                    }
                    if (!text.empty())
                    {
                        self->_SetSummaryContent(text); // text runs -> TextBlocks, sentinels -> full-width rules
                    }
                    if (timesComputed)
                    {
                        self->_summaryCreatedMs = createdMs; // refresh the times-line instants (the ticker re-renders the deltas)
                        self->_summaryLastUserMs = lastUserMs;
                        self->_summaryLastActivityMs = lastActivityMs;
                    }
                    self->_summaryPath = path; // cache the resolved path for the next reload
                    self->_summaryMtime = mtime;
                    self->_summaryLoading = false;
                    self->_UpdateTimesLine(); // reflect the (possibly refreshed) instants right away
                    if (self->_summaryWrapDirty || self->_summaryTruncateDirty)
                    {
                        // The wrap and/or truncate mode flipped mid-load: this render used the OLD flag(s).
                        // Re-render now with the current flags (invalidate the mtime gate so it isn't skipped).
                        self->_summaryWrapDirty = false;
                        self->_summaryTruncateDirty = false;
                        self->_summaryMtime = 0;
                        if (self->_summaryEnabled && self->_registry && !self->_sessionId.empty())
                        {
                            if (const auto reinfo = self->_registry->Get(self->_sessionId))
                            {
                                self->_UpdateSummary(*reinfo);
                            }
                        }
                    }
                }
            });
        }
    }
}
