// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentTabOverlay.h"

#include "AgentStatusColors.h" // the ONE shared state->color palette (board / overlay / tab dot)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeTranscriptPath / BuildClaude|CodexCommandline (row 3 CLI + transcript)
#include "AgentMaster/ProcessInspect.h" // ReadProcessCommandLine / ReadConversationText / Codex rollout resolve (row 3)
#include "AgentMaster/Persistence.h" // LoadAppSettings (skipPermissions, for the would-use CLI builder)
#include "AgentMaster/Engine.h" // SharedEngine (claudeExePath / codexExePath, for the real launch CLI)

#include <winrt/Windows.UI.h> // Color / ColorHelper / Colors
#include <winrt/Windows.UI.Text.h> // FontWeights
#include <winrt/Windows.UI.Xaml.Documents.h> // Run / Inlines
#include <winrt/Windows.UI.Xaml.Input.h> // PointerRoutedEventArgs
#include <winrt/Windows.UI.Xaml.Media.h> // SolidColorBrush / FontFamily
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h> // FlyoutBase (Button.Flyout)
#include <winrt/Windows.ApplicationModel.DataTransfer.h> // Clipboard / DataPackage (row 3 copy)

#include <shellapi.h> // ShellExecuteExW (row 3 folder button)
#include <mmsystem.h> // PlaySoundW (row 3 copy/open confirmation chime)
#pragma comment(lib, "winmm.lib")

#include <string>

using namespace winrt::Windows::Foundation;
// Narrow using-DECLARATIONS for the color helpers: a `using namespace winrt::Windows::UI;` would
// also pull the nested `Text` namespace into scope and clash with the Text() helpers (see the
// AgentManagerContent gotcha / CLAUDE.md).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
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
    constexpr const wchar_t* kLink = L"\x26D3"; // ⛓
    // Summary-box section separator SENTINEL: the renderers emit this as a lone line; the DISPLAYED
    // panel turns each into a full-width Border rule (border to border, re-fills on resize), and the
    // COPYABLE summary turns each into a plain-text ─ rule. \x1F (ASCII Unit Separator) never occurs
    // in transcript content, so it's an unambiguous marker.
    constexpr wchar_t kSepMark = L'\x1F';

    SolidColorBrush Fill(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
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

    // Escape a message to ONE line (newlines/tabs -> \n / \t, like session-end.js) + truncate.
    std::wstring SummaryEscapeMsg(const std::wstring& m)
    {
        std::wstring esc;
        for (const wchar_t ch : m)
        {
            if (ch == L'\n')
                esc += L"\\n";
            else if (ch == L'\r')
                ; // dropped
            else if (ch == L'\t')
                esc += L"\\t";
            else
                esc += ch;
        }
        if (esc.size() > 240)
        {
            esc = esc.substr(0, 237) + L"...";
        }
        return esc;
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
    std::wstring RenderSummaryBox(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full)
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
        if (!a.userMsgs.empty())
        {
            sep();
            int i = 1;
            for (const auto& m : a.userMsgs)
            {
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m));
            }
        }
        if (!a.filesRead.empty())
        {
            sep();
            line(L"Files Read:");
            for (const auto& f : a.filesRead)
            {
                line(L"   * " + f);
            }
        }
        if (!a.filesEdited.empty())
        {
            sep();
            line(L"Files Edited:");
            for (const auto& f : a.filesEdited)
            {
                line(L"   * " + f);
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
    std::wstring RenderCodexSummary(const CodexRolloutInfo& info, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, bool full)
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
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m));
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
    winrt::fire_and_forget CopySummaryAsync(winrt::Windows::System::DispatcherQueue disp, bool codex, std::wstring claudeId, std::wstring codexId, std::wstring cwd, std::wstring resumeCmd, std::wstring glyph, std::wstring label)
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
        if (codex)
        {
            const auto info = ::Agentmaster::ReadCodexRolloutInfo(path, 0 /* whole file */, 200 /* prompts */);
            text = RenderCodexSummary(info, id, cwd, path, resumeCmd, glyph, label, /*full*/ true);
        }
        else
        {
            const auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
            std::wstring planFile = a.planFilePath;
            if (planFile.empty() && a.hasPlanContent && !a.parentSessionId.empty())
            {
                const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(a.parentSessionId);
                if (!parentPath.empty())
                {
                    planFile = ::Agentmaster::FindPlanFileInTranscript(parentPath);
                }
            }
            text = RenderSummaryBox(a, id, cwd, path, resumeCmd, glyph, label, planFile, /*full*/ true);
        }
        if (text.empty() || !disp)
        {
            co_return;
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

        _line = TextBlock{};
        _line.FontSize(12);
        _line.Foreground(Fill(0xFF, 0xEC, 0xEC, 0xEC));
        _line.IsTextSelectionEnabled(false);
        _line.TextWrapping(TextWrapping::NoWrap);
        _line.HorizontalAlignment(HorizontalAlignment::Right); // keep the right edge aligned when row 2 is wider

        // Row 2: "<root workdir folder>/<branch>" — secondary (smaller + dimmer), right-aligned to the
        // badge edge, width-capped + ellipsized so a long branch path can't balloon the HUD. Collapsed
        // until _Refresh() fills it; stays collapsed on the registry-less observe badge (no session).
        _subline = TextBlock{};
        _subline.FontSize(11);
        _subline.Foreground(Fill(0xFF, 0xB0, 0xB0, 0xB0));
        _subline.IsTextSelectionEnabled(false);
        _subline.TextWrapping(TextWrapping::NoWrap);
        _subline.TextTrimming(TextTrimming::CharacterEllipsis);
        _subline.HorizontalAlignment(HorizontalAlignment::Right);
        _subline.MaxWidth(380);
        _subline.Margin(ThicknessHelper::FromLengths(0, 1, 0, 0));
        _subline.Visibility(Visibility::Collapsed);

        _stack = StackPanel{};
        _stack.Orientation(Orientation::Vertical);
        _stack.Children().Append(_line);
        _stack.Children().Append(_subline);

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
        _WireHover(); // pointer-over expand (opacity + row 3)
        _BuildActionsRow(); // row 3: folder + copy menu + pencil (linked sessions only)
        _BuildSummaryPanel(); // the 2nd slot (summary panel), collapsed until the pencil toggles it on
        // Explain the dense row-1 label (glyph + abbreviations): the live values are in the text, the
        // tooltip says what each part MEANS. Linked-session wording; ShowActivity overrides it for an
        // observe badge (defensive — the two are normally separate elements).
        if (_line)
        {
            ToolTipService::SetToolTip(_line, winrt::box_value(winrt::hstring{
                L"Status \x00B7 model \x00B7 effort \x00B7 Autopilot mode \x00B7 \x23F3 queued \x00B7 link\n"
                L"\x26D3 linked = Agentmaster can drive it; observe = read-only; unlinked = not bound" }));
        }
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
        _WireHover(); // observe badges still brighten on hover (no row 3 — that's linked-only)
        if (!_line || !_root)
        {
            return;
        }
        if (kind == _lastActivitySig)
        {
            return; // unchanged -> no XAML churn (this runs every probe tick)
        }
        _lastActivitySig = kind;
        // This is an observe badge, not a linked session — explain that (overrides the linked-session
        // row-1 tooltip in case the same element was ever used both ways).
        ToolTipService::SetToolTip(_line, winrt::box_value(winrt::hstring{
            L"Observed by Agentmaster, not a linked session. A claude links on its first prompt;\n"
            L"shells (pwsh/cmd) and external codex stay observe-only here." }));
        _root.Visibility(Visibility::Visible);
        _line.Inlines().Clear();
        Run glyph{};
        glyph.Text(winrt::hstring{ L"\x25CB" }); // ○ gray — observed, but not a linked session
        glyph.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // gray
        glyph.FontWeight(FontWeights::SemiBold());
        _line.Inlines().Append(glyph);
        Run text{};
        text.Text(winrt::hstring{ std::wstring{ L" " } + kind + L"  " + kDot + L"  unlinked" });
        _line.Inlines().Append(text);
    }

    void AgentTabOverlay::_Refresh()
    {
        if (_pending)
        {
            return; // a pending badge is static — it has no registry session to refresh from
        }
        if (!_line || !_registry)
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
        const std::wstring link = bound ? (std::wstring{ kLink } + L" linked") :
                                          (s.external ? std::wstring{ L"observe" } : std::wstring{ L"unlinked" });

        std::wstring rest = L" ";
        rest += StateLabel(s.state);
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
                    me += L" ";
                    me += kDot;
                    me += L" ";
                }
                me += part;
            };
            addPart(s.model);
            addPart(s.effort);
            if (s.background)
            {
                addPart(L"bg");
            }
            if (!me.empty())
            {
                rest += L"  ";
                rest += kDot;
                rest += L"  ";
                rest += me;
            }
        }
        rest += L"  ";
        rest += kDot;
        rest += L"  ";
        rest += ModeLabel(s.autopilot.mode);
        if (pending > 0)
        {
            rest += L"  ";
            rest += kDot;
            rest += L"  ";
            rest += kHourglass;
            rest += std::to_wstring(pending);
        }
        rest += L"  ";
        rest += kDot;
        rest += L"  ";
        rest += link;

        _line.Inlines().Clear();
        Run glyph{};
        glyph.Text(winrt::hstring{ StateGlyph(s.state) });
        glyph.Foreground(SolidColorBrush{ StateColor(s.state) });
        glyph.FontWeight(FontWeights::SemiBold());
        _line.Inlines().Append(glyph);
        Run text{};
        text.Text(winrt::hstring{ rest });
        _line.Inlines().Append(text);

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
                // Row 2 is width-capped + ellipsized and shows only the leaf folder; the tooltip reveals
                // the FULL working path (+ branch) behind it (the Archive-page reveal-behind-truncation pattern).
                std::wstring tip = dir;
                if (!s.branch.empty())
                {
                    tip = tip.empty() ? s.branch : (tip + L"  " + kDot + L"  " + s.branch);
                }
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
        if (_root)
        {
            _root.Opacity(on ? 1.0 : 0.55);
        }
        if (_row3)
        {
            _row3.Visibility(on ? Visibility::Visible : Visibility::Collapsed);
        }
    }

    void AgentTabOverlay::_BuildActionsRow()
    {
        if (_row3 || !_stack)
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

        Button copyBtn = mkIconBtn(L"\xE8C8", L"Copy\x2026"); // Copy
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
        pencilBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummary();
            }
        });

        _row3 = StackPanel{};
        _row3.Orientation(Orientation::Horizontal);
        _row3.HorizontalAlignment(HorizontalAlignment::Right);
        _row3.Spacing(2);
        _row3.Margin(ThicknessHelper::FromLengths(0, 2, 0, 0));
        _row3.Visibility(Visibility::Collapsed); // hover-only
        _row3.Children().Append(folderBtn);
        _row3.Children().Append(copyBtn);
        _row3.Children().Append(pencilBtn);
        _stack.Children().Append(_row3);
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

    void AgentTabOverlay::_CopyField(int which)
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
            CopyConversationAsync(_dispatcher, codex, s.id, s.codexSessionId);
            break;
        case 6: // Summary — the FULL textual session-end.js box (everything; the panel shows a trimmed view)
        {
            const std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
            const std::wstring resume = BuildLaunchCli(s, codex); // the same REAL launch CLI as cases 3/4
            CopySummaryAsync(_dispatcher, codex, s.id, s.codexSessionId, dir, resume,
                             std::wstring{ StateGlyph(s.state) }, std::wstring{ StateLabel(s.state) });
            break;
        }
        default:
            break;
        }
    }

    void AgentTabOverlay::_BuildSummaryPanel()
    {
        if (_summaryRoot)
        {
            return; // built once (Initialize), and only for a LINKED session (never an observe badge)
        }
        // The body is a vertical StackPanel (not one TextBlock) so a section separator can be a
        // full-width Border rule that fills the panel border-to-border + re-fills on resize — a fixed
        // run of ─ chars can't do that in a wrapping block. _SetSummaryContent fills it: monospace,
        // wrapped, selectable TextBlocks for text runs, interleaved with the Border rules.
        _summaryStack = StackPanel{};
        _summaryStack.Orientation(Orientation::Vertical);

        ScrollViewer sv{};
        sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        sv.MaxHeight(480); // a long session can't run off the bottom of the pane
        sv.Content(_summaryStack);

        _summaryRoot = Border{};
        _summaryRoot.Background(Fill(0xE6, 0x20, 0x20, 0x20)); // near-opaque dark, matching the badge
        _summaryRoot.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _summaryRoot.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _summaryRoot.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        _summaryRoot.Padding(ThicknessHelper::FromLengths(8, 6, 8, 6));
        _summaryRoot.Child(sv);
        _summaryRoot.Visibility(Visibility::Collapsed); // shown only while the GLOBAL showSummaryPanel is ON
    }

    // Render the rendered-text box into the StackPanel: contiguous text lines become one monospace,
    // wrapped, selectable TextBlock; each separator sentinel line (kSepMark) becomes a full-width
    // Border rule (HorizontalAlignment::Stretch => border-to-border, re-fills as the pane resizes).
    void AgentTabOverlay::_SetSummaryContent(const std::wstring& text)
    {
        if (!_summaryStack)
        {
            return;
        }
        _summaryStack.Children().Clear();
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
            _summaryStack.Children().Append(tb);
            seg.clear();
        };
        size_t i = 0;
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
    }

    void AgentTabOverlay::SetSummaryToggleHandler(std::function<void()> handler)
    {
        _onToggleSummary = std::move(handler);
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
            _summaryRoot.Visibility(Visibility::Collapsed);
            return;
        }
        // Enabled: show + (re)load from the freshest session snapshot.
        if (_registry && !_sessionId.empty())
        {
            if (const auto info = _registry->Get(_sessionId))
            {
                _UpdateSummary(*info);
                return;
            }
        }
        _summaryRoot.Visibility(Visibility::Visible); // enabled but no session yet — _Refresh will fill it
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
        _summaryRoot.Visibility(Visibility::Visible);
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
        _LoadSummaryAsync(_summaryPath, codex, convId, std::move(cwd), std::move(liveGlyph), std::move(liveLabel), _summaryMtime);
    }

    winrt::fire_and_forget AgentTabOverlay::_LoadSummaryAsync(std::wstring transcriptPath, bool codex, std::wstring sessionId, std::wstring cwd, std::wstring liveGlyph, std::wstring liveLabel, int64_t prevMtime)
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
        int64_t mtime = prevMtime;
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
                    text = RenderCodexSummary(info, sessionId, cwd, path, L"", liveGlyph, liveLabel, /*full*/ false);
                }
                else
                {
                    const auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
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
                    text = RenderSummaryBox(a, sessionId, cwd, path, L"", liveGlyph, liveLabel, planFile, /*full*/ false);
                }
            }
        }

        // Hop back to the UI thread to publish (the StackPanel build + member writes are UI-thread only).
        if (auto disp = _dispatcher)
        {
            disp.TryEnqueue([weak = get_weak(), text, path, mtime]() {
                if (auto self = weak.get())
                {
                    if (!text.empty())
                    {
                        self->_SetSummaryContent(text); // text runs -> TextBlocks, sentinels -> full-width rules
                    }
                    self->_summaryPath = path; // cache the resolved path for the next reload
                    self->_summaryMtime = mtime;
                    self->_summaryLoading = false;
                }
            });
        }
    }
}
