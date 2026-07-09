// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster per-tab link badge / overlay (4 partial files)
// The top-right terminal HUD on every classified tab + its pencil-toggled summary panel
// (TAB_OVERLAY.md). ONE class (AgentTabOverlay) split from the former 3221-line .cpp; all four
// share AgentTabOverlay.Internal.h.
//
// Partial files in this group (★ marks THIS file):
//   AgentTabOverlay.cpp          - CORE: ctor/dtor, Initialize/_Detach, ShowActivity (the observe badge), _Refresh (the linked badge), hover/expand, opacities, Autorunner
// ★ AgentTabOverlay.Internal.h   - shared file-local helpers: StateColor/Glyph/Label, the summary-box renderers, time formatting, launch-CLI + clipboard (anonymous namespace, a per-TU copy)
//   AgentTabOverlay.Actions.cpp  - the hover action row: the folder (Open Path) button, the copy menu, and the shared CopySessionField action
//   AgentTabOverlay.Summary.cpp  - the pencil-toggled summary panel: build/render/load off-thread, the times bar, resize grips, wrap/truncate/previous, JUMP, copy-summary
// ======================================================================================
//
// Agentmaster: AgentTabOverlay file-local helpers (StateColor/Glyph/Label, the summary-box
// renderers RenderSummaryBox/RenderCodexSummary, time formatting, the launch-CLI + copy/clipboard
// helpers, ...). Factored out of AgentTabOverlay.cpp so the partial TUs (AgentTabOverlay.{cpp,
// Actions,Summary}.cpp) all share ONE copy. Kept in an ANONYMOUS namespace exactly as before
// (internal linkage, a per-TU copy) -- no behavior change. NOT standalone: include AFTER pch.h and
// the file-scope using-directives each AgentTabOverlay.*.cpp replicates (it relies on them, just as
// the original anonymous namespace relied on the using-directives above it).
#pragma once

#include "AgentClipboard.h" // RobustCopyTextToClipboard — the retry-looped Win32 writer CopyTextToClipboard routes through

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

    // Summary-panel opacity uses EXACTLY the badge's values + mechanism (the overlay panel above —
    // _root.Opacity / _SetExpanded): dim at REST, bright on HOVER. Both are now per-instance members
    // (_restOpacity / _hoverOpacity), seeded from the GLOBAL AppSettings::tabOverlayRestOpacity /
    // tabOverlayHoverOpacity by the page (SetOverlayOpacities) — so the badge and its summary panel
    // always share the same configurable pair.

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

    // Agentmaster: PointerExited is a BUBBLING routed event, so a hit-testable CHILD (an action button,
    // a selectable summary text run, the resize grip) raises its OWN exit that bubbles up to the badge/
    // panel root's PointerExited handler whenever the pointer merely crosses BETWEEN children, or off a
    // child back onto the root's own padding. A naive handler then dims the badge/panel WHILE the pointer
    // is still over it — and because the root never "re-enters" (it never actually left), it can stick dim
    // until the pointer leaves and returns. Returns true when the pointer is still inside `sender`'s OWN
    // bounds => the exit came from a child, so the handler can ignore it; a genuine leave samples at/outside
    // the edge => false. (Mirror of AgentManagerContent's PointerStillWithin — fully-qualify IInspectable
    // here to dodge the global ::IInspectable vs winrt ambiguity a free function has no member scope to break.)
    bool PointerWithin(const winrt::Windows::Foundation::IInspectable& sender, const PointerRoutedEventArgs& e)
    {
        const auto fe = sender.try_as<FrameworkElement>();
        if (!fe)
        {
            return false;
        }
        const auto p = e.GetCurrentPoint(fe).Position();
        constexpr double kEdge = 1.0; // treat the outermost ~1px as "left" so an edge-sampled real leave is never swallowed
        return p.X > kEdge && p.Y > kEdge && p.X < fe.ActualWidth() - kEdge && p.Y < fe.ActualHeight() - kEdge;
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

    const wchar_t* ModeLabel(AutorunnerMode m)
    {
        switch (m)
        {
        case AutorunnerMode::SemiAuto:
            return L"Semi";
        case AutorunnerMode::Full:
            return L"Full";
        case AutorunnerMode::Off:
        default:
            return L"Off";
        }
    }

    // Agentmaster (TAB_OVERLAY row 3): a one-line preview of a queued prompt — its FIRST line, capped
    // at `maxChars` characters. The displayed text content is at most `maxChars` chars; a trailing
    // "..." is appended when EITHER the first line is longer than the cap (so it was truncated) OR
    // there is real content after the first line (further lines), so "..." always signals "there is
    // more than what's shown". Leading blank lines / whitespace are skipped so a prompt that opens
    // with a newline still previews real text; trailing spaces on the line are trimmed. Returns ""
    // for an all-whitespace prompt (the caller then hides the row).
    std::wstring FirstLinePreview(const std::wstring& text, size_t maxChars)
    {
        const size_t start = text.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos)
        {
            return {}; // nothing but whitespace
        }
        const size_t nl = text.find_first_of(L"\r\n", start);
        std::wstring line = (nl == std::wstring::npos) ? text.substr(start) : text.substr(start, nl - start);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t'))
        {
            line.pop_back();
        }
        // Is there real (non-whitespace) content beyond the first line? If so, signal it with "..." too.
        const bool more = (nl != std::wstring::npos) && (text.find_first_not_of(L" \t\r\n", nl) != std::wstring::npos);
        if (line.size() > maxChars)
        {
            line = line.substr(0, maxChars) + L"..."; // surpassed the cap -> truncate + ellipsis
        }
        else if (more)
        {
            line += L"..."; // first line fits, but there's more below it
        }
        return line;
    }

    // A short confirmation chime for a completed row-3 action (copy / open). Async so it never blocks
    // the UI thread; SystemAsterisk is the soft Windows notification sound. Best-effort (silent if the
    // user has system sounds off). winmm is already in the link (TerminalPaneContent's WarningBell).
    void PlayActionSound()
    {
        ::PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
    }

    // Put text on the system clipboard (row 3's copy menu, incl. the shared CopySessionField behind
    // the Triage Board's Copy submenu). Routes through the robust, retry-looped Win32 writer
    // (AgentClipboard.h) rather than the WinRT Clipboard (SetContent/Flush): the latter opens the OLE
    // clipboard once with NO retry, so it silently LOST the copy whenever the FOCUSED terminal tab was
    // an active clipboard user contending for it (the "Copy Summary does not work well in a focused
    // tab" report — every copy here shared that flaw). Call on the UI thread. Plays the confirmation
    // chime only once the copy ACTUALLY lands (a genuine failure — clipboard wedged past the ~10s
    // backoff — is silent, matching reality). Best-effort.
    void CopyTextToClipboard(const std::wstring& text)
    {
        if (winrt::TerminalApp::implementation::RobustCopyTextToClipboard(text))
        {
            PlayActionSound(); // "copy is done" feedback (every row-3 copy routes through here)
        }
    }

    // Collect the user's live text selection across the summary panel (title / times line / body runs are
    // IsTextSelectionEnabled). An unselected / non-selectable TextBlock returns an empty SelectedText, so a
    // blind recursive collect picks up exactly what's selected; XAML selection can't span TextBlocks, but
    // we newline-join defensively. Recurse Panels / Borders / ContentControls so the nested layout works.
    void SummaryCollectSelectedText(const winrt::Windows::UI::Xaml::UIElement& el, std::wstring& out)
    {
        if (!el)
        {
            return;
        }
        if (const auto tb = el.try_as<TextBlock>())
        {
            const auto sel = tb.SelectedText();
            if (!sel.empty())
            {
                if (!out.empty())
                {
                    out += L"\n";
                }
                out += std::wstring{ sel };
            }
            return;
        }
        if (const auto panel = el.try_as<Panel>())
        {
            for (const auto& child : panel.Children())
            {
                SummaryCollectSelectedText(child, out);
            }
            return;
        }
        if (const auto border = el.try_as<Border>())
        {
            SummaryCollectSelectedText(border.Child(), out);
            return;
        }
        if (const auto cc = el.try_as<ContentControl>())
        {
            SummaryCollectSelectedText(cc.Content().try_as<winrt::Windows::UI::Xaml::UIElement>(), out);
            return;
        }
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
        // Agentmaster (contained): an exception escaping this fire_and_forget == winrt::terminate() ==
        // the whole app dies. ReadConversationText self-contains, but the glue (the whole-conversation
        // capture COPY into the TryEnqueue lambda can be tens of MB — a std::bad_alloc candidate on the
        // same huge transcripts that drove the v0.6.x resume crash-loop) did not. Degrade to no-copy.
        try
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
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[summary] contained CopyConversationAsync throw (no crash)\n");
        }
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
        // Collapse an embedded table in BOTH wrap modes — drop its horizontal
        // rule rows ("├────┼────┤" / "|---|---|") and de-frame its data rows ("│ Name │ Age │" ->
        // "Name · Age") — so the box-drawing noise doesn't bury the content (ProcessInspect::
        // StripSummaryTableRules). The panel is narrow (~20% pane width by default), so a wide table
        // can't render aligned there even in wrap-ON; it just wraps into noise, and de-framing is
        // strictly more readable. Wrap mode now only governs how the de-framed rows' newlines render:
        // real multi-line (wrapNewlines) vs a literal "\n" on one line. Non-table prose passes
        // through StripSummaryTableRules verbatim, so wrap-ON's multi-line prose is unaffected.
        const std::wstring stripped = ::Agentmaster::StripSummaryTableRules(m);
        std::wstring esc;
        for (const wchar_t ch : stripped)
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
    std::wstring RenderSummaryBox(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full, bool wrapNewlines, bool truncate, bool showPrevious)
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
        // Agentmaster (conversation lineage): the PREVIOUS session(s) — the pre-compaction segment(s) a
        // /compact summarized away — rendered ABOVE the recap + current Messages, OLDEST FIRST, each under
        // its own separator + a "Previous session N · <label>" header, with ITS prompts numbered 1.. (an
        // INDEPENDENT count per session). Shown only when the toggle is on (display) — the full copyable box
        // passes showPrevious=true — and only when the session actually has a previous segment.
        if (showPrevious && !a.previousSegments.empty())
        {
            int segNo = 1;
            for (const auto& seg : a.previousSegments)
            {
                sep();
                std::wstring hdr = L"Previous session " + std::to_wstring(segNo++);
                if (!seg.label.empty())
                {
                    hdr += L" · " + seg.label;
                }
                line(hdr);
                int i = 1;
                for (const auto& m : seg.userMsgs)
                {
                    line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m, wrapNewlines, truncate));
                }
            }
        }
        if (!a.awaySummary.empty())
        {
            sep();
            line(L"Recap: " + SummaryEscapeMsg(a.awaySummary, wrapNewlines, /*truncate*/ false)); // label INLINE; the recap is ALWAYS shown in FULL (never capped by the truncate toggle — only the numbered messages honor it)
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
        // Agentmaster (contained): an exception escaping this fire_and_forget == winrt::terminate() ==
        // the whole app dies. The heavy helpers (analyze / lineage / plan-file) self-contain now, but
        // this lane's own glue — the previous-segments splice, the times+box concat, the sentinel
        // rewrite, the full-box capture COPY into TryEnqueue, and the OVERLAY-LOCAL RenderSummaryBox /
        // RenderCodexSummary (independent implementations, NOT the wrapped shared renderers) — did
        // not: a std::bad_alloc on the same huge transcripts that drove the v0.6.x resume crash-loop
        // would have killed the app on a Copy-Summary click. Degrade to no-copy + a log line.
        try
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
            auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
            // Cross-file lineage: the COMPLETE copyable box carries the /clear + plan-restart parents too
            // (a one-shot click — walked fresh, no memo needed). Prepended oldest-first, like the panel.
            {
                auto lineage = ::Agentmaster::CollectConversationLineage(claudeId, cwd, 16);
                if (!lineage.empty())
                {
                    a.previousSegments.insert(a.previousSegments.begin(), lineage.begin(), lineage.end());
                }
            }
            // Agentmaster (subagent activity): fold the newest subagent side-file write into "last
            // activity" so a copy taken WHILE a Task/Agent subagent runs matches the live panel — the
            // parent transcript's a.lastTs stays quiescent then (the same fold ReadTranscriptLastActivityTail
            // does for the observer's convLastActivityUnixMs). max() only ever makes it fresher.
            int64_t copyLastActMs = IsoToUnixMs(a.lastTs);
            if (const int64_t subMs = ::Agentmaster::SubagentActivityUnixMs(path); subMs > copyLastActMs)
            {
                copyLastActMs = subMs;
            }
            times = FormatTimesLine(IsoToUnixMs(a.firstTs), IsoToUnixMs(a.lastUserTs), copyLastActMs);
            std::wstring planFile = a.planFilePath;
            if (planFile.empty() && a.hasPlanContent && !a.parentSessionId.empty())
            {
                const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(a.parentSessionId);
                if (!parentPath.empty())
                {
                    planFile = ::Agentmaster::FindPlanFileInTranscript(parentPath);
                }
            }
            text = RenderSummaryBox(a, id, cwd, path, resumeCmd, glyph, label, planFile, /*full*/ true, wrapNewlines, truncate, /*showPrevious*/ true); // the COMPLETE copyable box always carries the previous session(s)
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
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[summary] contained CopySummaryAsync throw (no crash)\n");
        }
    }
}
