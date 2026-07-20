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
// ★ TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
//   TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
//   TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
//   TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
//   TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the two SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster — Claude session lifecycle on this window (M8 lifecycle + Rules #6/#11/#12):
// spawn/launch (ConPTY wired for hooks), the process-once fleet load (startup ARCHIVES,
// never auto-launches), the archive seams (tab-X / no-tab / window-teardown), restore
// (claude --resume, transcript-gated), adopt-external (resume a foreign claude's
// conversation), session<->tab/connection reverse lookups, cross-window move eviction,
// and the one-title / one-color-per-directory identity sync.
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // GuidToPlainString (WT_SESSION match)

#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (type/hr/msg + throw stacks)
#include "AgentStatusColors.h" // AgentStatusColorFor — the shared state->color palette (tab dot)
#include "AgentTabOverlay.h" // _claudeOverlays.erase needs the complete com_ptr<AgentTabOverlay> type
#include "AgentMaster/ClaudeSpawn.h" // BuildClaudeSpawn / ClaudeConversationExists / AppendStateLog
#include "AgentMaster/CommandWatch.h" // IsSaneWatchPath — the /handover action's path re-assert (COMMANDS.md)
#include "AgentMaster/Engine.h" // SharedEngine (AM_SESSION stamp; restoreMutex barrier)
#include "AgentMaster/HooksBridge.h" // PipeName for the spawn spec
#include "AgentMaster/Persistence.h" // DeriveSessionTitle / Save-LoadSessions / LoadAppSettings / dir colors
#include "AgentMaster/ProcessInspect.h" // ProcessAlive / ProcessStartUnixMs / ResolveSessionId
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionStore.h" // SetSessionFavorite (FAVORITES.md: "Favorite & Close" dialog branch)

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
using namespace ::Microsoft::Console;
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
    // Agentmaster: per-directory tab color helpers — the engine persists colors as "#RRGGBB"
    // strings; these convert to/from the WT tab color (alpha forced opaque).
    static std::wstring ClaudeColorToHex(const winrt::Windows::UI::Color& c)
    {
        wchar_t buf[8];
        ::swprintf(buf, 8, L"#%02X%02X%02X", static_cast<unsigned>(c.R), static_cast<unsigned>(c.G), static_cast<unsigned>(c.B));
        return buf;
    }
    static std::optional<winrt::Windows::UI::Color> ClaudeHexToColor(const std::wstring& hexIn)
    {
        std::wstring s = hexIn;
        if (!s.empty() && s.front() == L'#')
        {
            s.erase(0, 1);
        }
        if (s.size() != 6)
        {
            return std::nullopt;
        }
        const auto byteAt = [&s](size_t i) {
            return static_cast<uint8_t>(::wcstoul(s.substr(i, 2).c_str(), nullptr, 16));
        };
        winrt::Windows::UI::Color c{};
        c.A = 255;
        c.R = byteAt(0);
        c.G = byteAt(2);
        c.B = byteAt(4);
        return c;
    }

    // Agentmaster (native-exe-only policy): surface the "Claude Code (native) not found" notice from a
    // FULL-WINDOW page (Archive / Sessions) or a tab/CLI entry point, where the Manager tab's rich
    // install modal (_ShowClaudeMissing) can't render. The decision is still the synchronous
    // EnsureClaudeAvailable() gate at each launch choke point; this is only the user-facing prompt shown
    // when that returns false. Idempotent: _claudeMissingPromptShowing collapses a burst — e.g. a bulk
    // Restore that calls the gate once per checked Claude session — to ONE dialog. Buttons-only (XAML-
    // Islands-safe) + agentmaster-dark, matching the pages' other dialogs. "Get Claude Code" opens the
    // setup docs; "OK" dismisses. There is no Re-check button by design — EnsureClaudeAvailable()
    // re-resolves on the NEXT attempt, so once `claude install` finishes the prompt simply stops
    // appearing and the action proceeds.
    winrt::fire_and_forget TerminalPage::_PromptClaudeMissing()
    {
        if (_claudeMissingPromptShowing)
        {
            co_return; // a prompt is already up (or queued this tick) — never stack duplicates
        }
        const auto presenter{ _dialogPresenter.get() };
        if (!presenter)
        {
            co_return; // no presenter to show with — the gate already aborted the launch, so just bail
        }
        _claudeMissingPromptShowing = true; // set BEFORE the first co_await so a synchronous bulk loop dedupes

        // Agentmaster (terminate-net): an exception escaping this fire_and_forget (a teardown-race
        // ShowDialog throw, XAML failing the dialog build) would std::terminate the app over an
        // informational prompt. Contain it — and RE-ARM the dedupe flag in the catch, else a failed
        // dialog would leave _claudeMissingPromptShowing stuck true and the not-found prompt could
        // never show again for the window's lifetime.
        const auto weak = get_weak();
        try
        {
        // Which one-click fix applies (npm-legacy => migrate via `claude install`; nothing => the official
        // claude.ai native bootstrap). Computed before the dialog so the Primary button is labeled for it.
        const auto installKind = ::Agentmaster::ClaudeInstallKind();
        const wchar_t* const installText = (installKind == ::Agentmaster::ClaudeInstallState::LegacyNpm) ?
                                               L"Run claude install" :
                                               L"Install native (PowerShell)";

        ContentDialog dialog;
        dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
        dialog.Title(winrt::box_value(L"Claude Code (native) not found"));
        dialog.Content(winrt::box_value(winrt::hstring{
            L"Agentmaster drives the native claude.exe, and none was found on PATH, in "
            L"%USERPROFILE%\\.local\\bin, or behind an npm claude.cmd. Launching, resuming, forking, and "
            L"adopting Claude sessions stay disabled until one is available \x2014 a pure-Node `claude` is "
            L"not supported.\n\nUse the button below to install it in a PowerShell window (or run  claude "
            L"install  yourself), then try again \x2014 Agentmaster re-checks automatically, so this notice "
            L"stops appearing once it's found. You can also point at an existing claude.exe in "
            L"Settings \x2192 Claude binary." }));
        dialog.PrimaryButtonText(installText); // the one-click install/migrate
        dialog.SecondaryButtonText(L"Get Claude Code"); // opens the setup docs
        dialog.CloseButtonText(L"OK");
        dialog.DefaultButton(ContentDialogButton::Close); // Enter/Esc = dismiss (never auto-launch an installer)

        const auto result = co_await presenter.ShowDialog(dialog);
        const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
        if (!strong)
        {
            co_return; // window torn down while the dialog was up — the flag dies with the object
        }
        strong->_claudeMissingPromptShowing = false; // dialog dismissed — re-arm for the next not-found gate
        if (result == ContentDialogResult::Primary)
        {
            ::Agentmaster::LaunchClaudeInstall(installKind); // open a visible PowerShell window running the install/migrate command
        }
        else if (result == ContentDialogResult::Secondary)
        {
            try
            {
                winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri{ L"https://code.claude.com/docs/en/setup" });
            }
            CATCH_LOG();
        }
        }
        catch (...)
        {
            if (const auto strong = weak.get())
            {
                strong->_claudeMissingPromptShowing = false; // re-arm (see the note above)
            }
            ::Agentmaster::AgentLogCaughtException(L"_PromptClaudeMissing");
        }
    }

    // Agentmaster: launch a fresh Claude session (the Manager's "Launch Claude"). insertPosition
    // threads tab placement: -1 (the default) keeps the end/NewTabPosition behavior; a tab
    // context-menu "New Session Here" passes clickedIndex+1 so the new tab lands next to it.
    void TerminalPage::_SpawnClaudeSession(winrt::hstring workingDir, winrt::hstring title, uint32_t insertPosition, winrt::hstring model)
    {
        // Nav audit: the user asked for a FRESH session here (Manager "Launch Claude", a tree/board/
        // Sessions-page "Open New Session Here", or a tab-menu spawn). The resulting minted id lands
        // in the downstream [spawn] line. A launch-model pick rides the line (model=<id>) so "which
        // model did that session start on" reads straight off the journey.
        ::Agentmaster::LogNav(L"open-new claude dir=" + std::wstring{ workingDir } + (model.empty() ? std::wstring{} : (L" model=" + std::wstring{ model })) + (_openClaudeTabInBackground ? L" [bg]" : L""));
        // Native-exe-only policy gate (auto-recovering). A page / tab-menu spawn that reaches here did
        // NOT pass through the Manager's rich modal, so prompt with the page dialog instead of silently
        // no-op'ing at _LaunchClaudeSession's backstop. (A Manager-tab spawn already gated upstream and
        // only reaches here when Claude IS present — a cache hit.)
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::LogNav(L"open-new claude done (blocked \x2014 no native claude.exe; install prompt shown)"); // pair the BEGIN so an unpaired begin always means a crash, never the gate
            _PromptClaudeMissing();
            return;
        }
        const auto spawnedTab = _LaunchClaudeSession(workingDir, title, std::nullopt, {}, insertPosition, std::wstring{ model });
        // Nav audit END (pairs with the open-new BEGIN above): the minted id of the fresh session. An
        // open-new with no matching done = the launch crashed/no-op'd before the tab was created.
        const std::wstring spawnedId = spawnedTab ? _ClaudeSessionForTab(spawnedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"open-new claude done " + (spawnedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(spawnedId))));
    }

    // Agentmaster (COMMANDS.md — the /handover integration): the CommandWatch's markdown await
    // resolved — the origin session ran `/handover <context-or-filepath>` and the handover markdown
    // it was instructed to Write now exists on disk. Every window's command-action sink lands here
    // (UI thread, via the engine fan-out); ONLY the window hosting the origin session's tab acts:
    // spawn the successor session — same effective working dir (the "Open New Session Here"
    // semantics), titled "<origin title> (handover)" (bumping to "(handover 2)"/… against titles
    // already in the registry, so sibling handovers from one origin never collide), inserted right
    // beside the origin tab, and FIRST-PROMPTED at the markdown via the launch commandline's
    // positional prompt (zero-race: no stdin injection, no TUI-init window; the prompt fires a real
    // UserPromptSubmit, so state/record ride the normal push path). Repeatable by design — every
    // /handover in a conversation spawns its own successor.
    void TerminalPage::_HandleCommandHandover(const std::wstring& sessionId, const std::wstring& mdPath)
    try
    {
        const auto tabIt = _claudeTabs.find(sessionId);
        if (tabIt == _claudeTabs.end())
        {
            return; // not our window (the fan-out reaches every window; the single host acts)
        }
        // Safeguard belts. The watch already vetted the path (IsSaneWatchPath at match) and the
        // file's presence (the fire gate) — re-assert BOTH here: the path feeds a log line + the
        // successor's single-line launch prompt (a control char / quote would corrupt either),
        // and the file can vanish in the fire -> UI-hop window (a deleted/moved md would spawn a
        // successor pointed at nothing — worse than not spawning: the user re-runs /handover).
        if (!_sessionRegistry || !::Agentmaster::IsSaneWatchPath(mdPath))
        {
            return;
        }
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(mdPath.c_str(), GetFileExInfoStandard, &fad) ||
                (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
                (fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover] " + ::Agentmaster::ShortId(sessionId) + L" md vanished before spawn: " + mdPath + L" (dropped)\n");
                return;
            }
        }
        const auto s = _sessionRegistry->Get(sessionId);
        if (!s || !s->live)
        {
            return; // archived/vanished between fire and hop — nothing to hand over to
        }
        ::Agentmaster::LogNav(L"handover-begin " + ::Agentmaster::ShortId(sessionId) + L" md=" + mdPath);

        // The successor spawns where the origin WORKS (EffectiveWorkingDir — the same dir every
        // "Open New Session Here" uses), falling back to the launch cwd inside that resolver.
        const std::wstring dir = ::Agentmaster::EffectiveWorkingDir(_appSettings.tabColorMode, *s);

        // "<origin title> (handover)", bumped past titles the registry already holds (live OR
        // archived — the Sessions browser lists both, and two rows named identically would be
        // ambiguous). DeriveSuffixedTitle bumps its own trailing group, so re-deriving from the
        // last candidate walks (handover) -> (handover 2) -> (handover 3) …; the cap is a
        // pathological-registry guard, not a real limit.
        std::wstring successorTitle = ::Agentmaster::DeriveSuffixedTitle(s->title, L"handover");
        {
            const auto sessions = _sessionRegistry->Snapshot();
            const auto taken = [&sessions](const std::wstring& t) {
                for (const auto& x : sessions)
                {
                    if (x.title == t)
                    {
                        return true;
                    }
                }
                return false;
            };
            for (int i = 0; i < 50 && taken(successorTitle); ++i)
            {
                successorTitle = ::Agentmaster::DeriveSuffixedTitle(successorTitle, L"handover");
            }
        }

        // Land the successor right beside the origin tab (the "New Session Here" placement).
        uint32_t insertPosition = static_cast<uint32_t>(-1);
        if (const auto originTab = tabIt->second.get())
        {
            insertPosition = originTab.TabViewIndex() + 1;
        }

        // The handover message for the new tab IS the markdown — delivered BY REFERENCE (a
        // single-line pointer prompt): the launch commandline carries one positional arg, and a
        // multi-line body would be hostage to prompt-injection quoting; the file outlives the
        // prompt and the successor Reads it as its first action.
        const std::wstring prompt = L"Read the handover document at " + mdPath + L" and continue the work it describes.";

        const auto successorTab = _LaunchClaudeSession(winrt::hstring{ dir }, winrt::hstring{ successorTitle }, std::nullopt, {}, insertPosition, {}, prompt);
        const std::wstring newId = successorTab ? _ClaudeSessionForTab(successorTab) : std::wstring{};
        ::Agentmaster::LogNav(L"handover-done " + (newId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(newId))) + L" from=" + ::Agentmaster::ShortId(sessionId));
    }
    catch (...)
    {
        // Safeguard: a spawn-path failure (a torn-down tab mid-hop, a WinRT hresult, bad_alloc)
        // must never unwind the UI thread; the nav trail's unpaired handover-begin plus this
        // forensics line pinpoint the abort.
        ::Agentmaster::AgentLogCaughtException(L"_HandleCommandHandover");
    }

    // Agentmaster: launch a claude.exe on a ConPTY in `workingDir`, wired for hooks, as a
    // normal terminal tab, and register it so its hook-driven state is tracked. Both the
    // user (keystrokes) and the orchestrator (Autorunner) write the same stdin.
    //
    // If `restored` is set, this RESUMES that conversation (claude --resume <id>) and
    // restores its Auto Testing + autorunner from persistence (DESIGN §13) — so closing and
    // reopening the app brings the session back exactly as it was. `Sent` prompts are kept
    // Sent (never replayed, Correctness Rule #4).
    TerminalApp::Tab TerminalPage::_LaunchClaudeSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromId, uint32_t insertPosition, const std::wstring& modelOverride, const std::wstring& initialPrompt)
    {
        if (!_sessionRegistry || !_hooksBridge)
        {
            return nullptr;
        }

        // Native-exe-only policy: never spawn without a resolved native claude.exe. The UI launch paths
        // already gate with the install prompt; this is the SILENT backstop for the non-UI entry points
        // (window/workspace restore) — they no-op cleanly (log only, no prompt: a reopen re-homes many
        // tabs and must never raise N modals) instead of spawning a doomed ConPTY (a fork/resume is just
        // a launch variant — all funnel through here). EnsureClaudeAvailable re-resolves first, so a
        // claude installed since startup lets a later restore succeed without a relaunch.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[launch-blocked] no native claude.exe; refusing to spawn \"" + std::wstring{ title } + L"\"\n");
            return nullptr;
        }

        std::wstring dir{ workingDir };
        if (dir.empty())
        {
            wchar_t up[MAX_PATH];
            const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
            dir = (n > 0 && n < MAX_PATH) ? std::wstring{ up, n } : std::wstring{ L"C:\\" };
        }

        std::wstring ttl{ title };
        if (ttl.empty())
        {
            // Smart tab/session name: walk past generic bin/obj/Debug/... segments to the first
            // meaningful folder, then apply the cog's configured naming technique + case/underscore
            // transforms (DeriveSessionTitle / TabTitleNaming — read fresh from settings.json).
            ttl = ::Agentmaster::DeriveSessionTitle(dir);
        }

        // Restore resumes the EXACT session this tab recorded — the WindowRecord stored its id, so we know
        // it. We do NOT redirect to a heuristic "continuation tail". That inference was purely timing-based
        // (a same-cwd session that merely started soon after) and is unsound: there is no solid on-disk
        // signal for a /clear/compact/plan successor — /compact is IN-PLACE (no new id), /clear leaves NO
        // link to its successor, and a plan-restart child references its PARENT (backward), never forward.
        // So the old "tail" was really just the next INDEPENDENT session the user started in this busy dir,
        // which merged unrelated conversations onto one tab. Resume what was recorded. (A tab hosts exactly
        // ONE session.) [Agentmaster]
        const std::wstring resumeTargetId = (restored && !restored->id.empty()) ? restored->id : std::wstring{};
        // One tab = one session (the conflict the chain creates): if this window already hosts a tab bound
        // to the conversation we'd resume, another archived ref chained to the SAME live conversation.
        // Don't spawn a second claude on it — two claude.exe appending one transcript corrupt each other —
        // the existing tab IS this session. (Reached on the multi-tab window-restore path; a lone
        // Archive-restore of an already-open session likewise no-ops. The null return tells callers to
        // count it skipped, not as a freshly homed tab.)
        if (!resumeTargetId.empty() && _claudeTabs.find(resumeTargetId) != _claudeTabs.end())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[restore-dedup] " + resumeTargetId + L" already hosted in this window — skipping duplicate resume (a tab can't be two sessions)\n");
            return nullptr;
        }
        // Resume ONLY if Claude actually has a saved conversation for the (tail) id. A session that was
        // opened but never received a prompt — or whose transcript vanished — has none, so `claude
        // --resume <id>` would fail with "No conversation found" and the tab would die (exit code 1); it
        // is re-launched FRESH instead (a new id, keeping the working dir + Auto Testing). (Correctness
        // Rule #6: restore == resume, never replay.)
        const bool wantResume = !resumeTargetId.empty() && ::Agentmaster::ClaudeConversationExists(resumeTargetId);
        const std::wstring resumeId = wantResume ? resumeTargetId : std::wstring{};
        // Agentmaster (restore a NEVER-MESSAGED fork): a fork's OWN transcript (<id>.jsonl) is written
        // only on its FIRST turn, so a fork the user created but never sent a message to has NO transcript
        // — wantResume is false and a plain restore would spawn a brand-new EMPTY conversation, silently
        // LOSING the forked branch (the "(fork)" tab returns blank, with a churned id, on EVERY restart —
        // observed live as one "(fork)" title hitting [restore-fresh] over and over). When the record
        // remembers its fork SOURCE (the PERSISTED forkParentId) and that source still has a transcript,
        // RE-FORK from it into the SAME id — re-materializing the identical branch, identity + the
        // WindowRecord tab ref preserved (BuildClaudeSpawn forks into forkIntoId rather than minting).
        // Gated on !wantResume, so a fork that LATER got its own transcript is always resumed, never
        // re-forked off its now-divergent source; if the source ALSO vanished, fall through to a fresh
        // launch (the best possible). A GENUINE in-progress fork (forkFromId) takes precedence — only a
        // RESTORE (restored set) with no explicit fork can re-fork.
        std::wstring effectiveForkFrom{ forkFromId };
        std::wstring forkIntoId; // re-fork TARGET = the fork's existing id (preserve identity); empty => mint (genuine fork)
        if (effectiveForkFrom.empty() && !wantResume && restored && !restored->forkParentId.empty() &&
            ::Agentmaster::ClaudeConversationExists(restored->forkParentId))
        {
            effectiveForkFrom = restored->forkParentId;
            forkIntoId = resumeTargetId; // == restored->id (a transcript-less fork has no continuation redirect)
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[restore->refork] " + resumeTargetId + L" has no transcript \x2014 re-forking from source " + effectiveForkFrom + L" into the same id (never-messaged fork; identity preserved)\n");
        }
        // forkFromId / a restore re-fork overrides resume/fresh: BuildClaudeSpawn forks the source
        // conversation into the target id (the source transcript is untouched). A genuine fork mints a new
        // id; a re-fork reuses the fork's existing id (forkIntoId). Launch the NATIVE claude.exe by full
        // path (resolved once at engine init; exe-only policy). CreateProcessW appends only ".exe" and
        // ignores PATHEXT, so the full path is mandatory. This is reached ONLY when ClaudeAvailable() (the
        // Manager gates launch/new/fork/resume otherwise), so claudeExePath is non-empty here; the empty
        // bare-token fallback never executes.
        // initialPrompt (COMMANDS.md — the /handover successor): rides the commandline as the trailing
        // positional prompt claude submits as the FIRST turn. FRESH launches only — a restore/resume
        // must never re-submit it (its turn already ran and lives in the transcript), and no restore
        // path passes one; the guard makes that structural rather than conventional.
        const std::wstring launchPrompt = (restored || !resumeId.empty()) ? std::wstring{} : initialPrompt;
        const auto spec = ::Agentmaster::BuildClaudeSpawn(dir, ttl, _hooksBridge->PipeName(), resumeId, ::Agentmaster::LoadAppSettings(), effectiveForkFrom, ::Agentmaster::SharedEngine().claudeExePath, forkIntoId, modelOverride, launchPrompt);

        // Build the ConPTY connection (commandline = claude + our hooks settings; child env = spec.env
        // [CCMGR_SESSION_ID + CCMGR_HOOK_PIPE + the cog's global env] plus this window's AM_SESSION
        // ownership stamp, added by the shared builder), then hand it to the normal terminal-pane path as
        // an existing connection. The default profile only supplies appearance; the process/cwd/env are
        // ours. _BuildAgentConnection is the one builder shared by launch, codex-launch, and the restart.
        //
        // Host claude INSIDE an interactive pwsh (a managed session is "claude run from a pwsh terminal"):
        // the ConPTY root is pwsh, which execs claude and -NoExit's to a live prompt when claude quits, so
        // Ctrl+C / /exit drops to `PS <cwd>>` at the working dir instead of the connection dying into a
        // "press Enter to restart" dead pane that mis-replays the launch at the wrong cwd. The observer
        // still binds it (FindDescendantByImage is descendant-OR-self, so claude.exe as a CHILD of pwsh
        // correlates like a hand-typed one); spec.env (CCMGR_* / AM_SESSION) rides pwsh and is inherited by
        // claude; --settings is inside spec.commandline. (BuildPwshHostedCommandline; pwsh resolved once at
        // engine init.)
        const std::wstring hostedCmd = ::Agentmaster::BuildPwshHostedCommandline(::Agentmaster::SharedEngine().pwshExePath, spec.commandline);
        auto connection = _BuildAgentConnection(hostedCmd, dir, ttl, spec.env);

        Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs{};
        const auto pane = _MakePane(newTerminalArgs, winrt::TerminalApp::Tab{ nullptr }, connection);
        if (!pane)
        {
            // Silent before: a null pane (ConPTY/tab build failed) produced no tab AND no trace — a launch
            // that "did nothing". Record it so a vanished spawn is diagnosable (the registry upsert + the
            // [spawn]/[fork]/[resume] success line below never ran).
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[launch-fail] " + spec.sessionId + L" \"" + ttl + L"\" cwd=" + dir + L" (pane build returned null \x2014 no tab created)\n");
            return nullptr;
        }

        // Agentmaster: suppress the profile's closeOnExit auto-close for this Claude pane.
        // The ConPTY root is now pwsh (it hosts claude), so this fires when the user EXITS pwsh
        // (claude quitting just drops to the pwsh prompt — the connection stays alive). When pwsh
        // does exit, the pane must NOT auto-close: _SweepClaudeLiveness intentionally leaves dead
        // tabs open for the user to read while archiving the session. Without this, a graceful exit
        // fires CloseRequested → Pane::Close() → Tab::Closed → _RemoveTab, bypassing the design.
        pane->WalkTree([](auto&& p) {
            if (const auto content = p->GetContent())
            {
                if (const auto term = content.try_as<winrt::TerminalApp::TerminalPaneContent>())
                {
                    if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(term))
                    {
                        impl->SuppressAutoClose();
                    }
                }
            }
        });

        // insertPosition (default -1) places the tab: -1 -> end/NewTabPosition; a tab-context-menu
        // "New Session Here" / "Fork session" passes clickedIndex+1 so the new tab lands next to it.
        const auto tab = _CreateNewTabFromPane(pane, insertPosition);
        if (tab)
        {
            // Map sessionId -> tab so the Manager can Activate (jump) / Kill it.
            _claudeTabs[spec.sessionId] = winrt::make_weak(tab);
        }

        // Register the session (restoring its queue + autorunner if resuming) and bind its
        // stdin injector. Correctness Rule #3: the injector is bound to THIS session's id.
        ::Agentmaster::SessionInfo info = restored ? *restored : ::Agentmaster::SessionInfo{};
        info.id = spec.sessionId;
        info.title = ttl;
        info.workingDir = dir;
        // Agentmaster (crash/restore state fidelity — Rule #16): a REOPENED session (window-restore, or a
        // Sessions-browser resume) keeps its persisted "needs you" state (WaitingForInput / NeedsApproval)
        // so a crash doesn't drop the "which sessions need me" triage; in-flight/ended states normalize to
        // Idle (a --resume'd claude waits, it doesn't continue the interrupted turn, and the scanner's
        // primed-cursor gate would strand a seeded Running — RestoredSessionState). A FRESH spawn (no
        // `restored`) is Idle. `info` was copied from *restored above, so info.state == restored->state
        // here. The SessionScanner refines the seed from the transcript tail; hooks own it live the instant
        // the tab is activated (claude resumes -> SessionStart -> Idle).
        info.state = restored ? ::Agentmaster::RestoredSessionState(info.state) : ::Agentmaster::SessionState::Idle;
        // Keep the persisted question-guard flag consistent with the (normalized) state: preserved for a
        // restored WaitingForInput-on-a-question (so the Autorunner queue stays held and can't auto-answer
        // it), dropped when the state is Idle — a fresh spawn (no `restored`) OR a Running-interrupted
        // record whose stale flag would otherwise falsely hold the queue. (RestoredQuestionFlag, Rule #16.)
        info.lastMessageWasQuestion = restored ? ::Agentmaster::RestoredQuestionFlag(info.state, info.lastMessageWasQuestion) : false;
        info.external = false; // we own this tab's ConPTY -> managed, not an adopted session
        info.live = true; // OPEN: has a live tab/claude now -> shows on the Triage Board (not Archived)
        info.pendingConfirmPromptId.clear();
        // Agentmaster: stamp the hosting ConPTY's WT_SESSION (== the wire tabToken hooks will report)
        // EAGERLY, not lazily on the first hook — GuidToPlainString here is byte-identical to the
        // connection's WT_SESSION env (same fn, same guid), so the first real hook's tabToken write is
        // a no-op. This is what the --fork-session source-id-echo guard keys on (SessionInfo::forkParentId
        // / SessionRegistry::OnHookEvent): a fork's FIRST SessionStart arrives under the SOURCE id
        // BEFORE this session's own first hook, so the fork must already know its tabToken for the guard
        // to recognize + drop that echo (else its tab is wrongly re-homed onto the source conversation).
        info.tabToken = ::Microsoft::Console::Utils::GuidToPlainString(connection.SessionId());
        // Remember the fork SOURCE (PERSISTED) when this launch is a (re-)fork — it drives BOTH the
        // source-id echo guard above AND restoring this fork later if it is never messaged (a fork's own
        // transcript isn't written until its first turn). Clear it otherwise so a plain resume / fresh
        // launch never carries a stale source: a fork that already has its own conversation must be
        // RESUMED, not re-forked off a now-divergent source (the registry's first-own-hook clear also
        // retires it live the moment the fork produces content).
        if (!effectiveForkFrom.empty())
        {
            info.forkParentId = effectiveForkFrom;
            // Tab color modes: a fork INHERITS its source's inferred working dir (when the source is
            // a session the registry knows and this fork has none of its own — a fresh fork always
            // has none; a re-fork may carry its previously-persisted one, which wins). A fork's own
            // transcript doesn't exist until its first turn, so without this the fork would key its
            // color on the launch cwd and wear a DIFFERENT color than the parent conversation it just
            // branched from (the reported fork-color mismatch) until the first message + the next
            // inference pass. The fork's transcript is a verbatim copy of the parent's at fork point,
            // so the parent's inference IS the fork's correct starting inference; the scan re-derives
            // it from the fork's own history once one exists. Mode-agnostic data copy (consulted
            // while the session INFERS — SessionInfersWorkingDir: the Inferred mode, or a home-dir
            // launch in any mode). A source unknown to the registry (an
            // adopt-external fork) is covered by the scan's parent-transcript fallback instead.
            if (info.inferredWorkingDir.empty() && _sessionRegistry)
            {
                if (const auto src = _sessionRegistry->Get(effectiveForkFrom); src && !src->inferredWorkingDir.empty())
                {
                    info.inferredWorkingDir = src->inferredWorkingDir;
                }
            }
        }
        else
        {
            info.forkParentId.clear();
        }
        if (!restored)
        {
            // A NEW session inherits ALL the global Autorunner defaults from the cog.
            info.autorunner.mode = _appSettings.defaultAutorunnerMode;
            info.autorunner.maxAutoSends = _appSettings.maxAutoSends;
            info.autorunner.stopOnError = _appSettings.stopOnError;
            info.autorunner.pauseOnHumanInput = _appSettings.pauseOnHumanInput;
        }
        else
        {
            // Agentmaster: an OPENED (restored / window-restored) session ALSO adopts the cog's
            // default Autorunner MODE — the product rule is "all new OR opened sessions run on the
            // default (Full)" — even though it keeps its persisted queue + backstops. This
            // intentionally OVERRIDES the session's saved per-session mode (a deliberate carve-out
            // from the old "a restored session keeps its mode"); it stays changeable afterward via
            // the Auto Testing toggle. Re-arming zeroes the per-run send counter so a reopened plan
            // isn't instantly capped by a lingering in-memory autoSendsThisRun (pendingConfirmPromptId
            // was already cleared above). Codex restores keep their own Off (see _LaunchCodexSession).
            info.autorunner.mode = _appSettings.defaultAutorunnerMode;
            info.autorunner.autoSendsThisRun = 0;
        }
        _sessionRegistry->Upsert(info);

        // The spawned id differs from the restored record's id in two cases: restore-FRESH (the
        // transcript was gone, so a new conversation id was minted) and the continuation REDIRECT above
        // (we resumed the chain tail, an id distinct from the persisted ancestor). Either way the new
        // (live) record is keyed by the spawned id and the old archived record under restored->id is now
        // stale — drop it so it doesn't linger in the Archived list as a duplicate/ancestor. A re-fork
        // (forkIntoId) deliberately does NOT differ — it forks back into restored->id — so this is a
        // no-op there and the fork keeps its identity (the whole point of re-forking into the same id).
        // Tab color modes: the new record deliberately KEEPS the old one's inferredWorkingDir (it rode
        // the *restored copy above) as a CONTINUITY seed — a restore-fresh reopens the same work in the
        // same dir, so the old inferred color is the best first guess; the inferred-workdir scan
        // replaces it with the NEW conversation's own honest inference (usually the cwd at first — no
        // file ops yet) on its first pass after the first turn. Same continuity applies to tabColorHex
        // in Individual mode (the fresh conversation keeps the tab's color).
        if (restored && !restored->id.empty() && restored->id != spec.sessionId)
        {
            _sessionRegistry->Remove(restored->id);
        }

        _sessionRegistry->SetInjector(spec.sessionId, [connection](const std::wstring& text) {
            const auto* begin = reinterpret_cast<const char16_t*>(text.data());
            connection.WriteInput(winrt::array_view<const char16_t>{ begin, begin + text.size() });
        });

        // Agentmaster: a session's title is ONE value — the Explorer-tree name, the persisted
        // SessionInfo.title, and the WT tab title are the same thing. Pin the tab to it now
        // (SetTabText) so the tab strip shows the managed name instead of floating with claude's
        // volatile OSC title; a later tab rename writes back through _SyncClaudeTitleFromTab, and an
        // Explorer-tree rename re-pins it through _RenameClaudeSession.
        if (tab)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                _SetClaudeTabTextPinned(impl, winrt::hstring{ ttl }); // pinned: registry->tab, no write-back
            }
            // Tab color (mode-aware): per-dir (default, the dir's persisted/auto color), per-session
            // (Individual), or per-inferred-dir — the one paint seam (_ApplySessionTabColor).
            _ApplySessionTabColor(tab, spec.sessionId, dir);
            // Per-tab "link badge" overlay (TAB_OVERLAY.md): top-right status HUD for this session.
            _AttachClaudeOverlay(tab, spec.sessionId);
            // Tab status dot: seed the strip dot from the (just-upserted) session's state — Idle
            // gray for a fresh launch/restore; the registry observer recolors it live from here.
            // Agentmaster (eager-init): a just-created tab's control is NotConnected, so the session is
            // DORMANT until it starts — seed the HALF-HOLLOW dot (a window-restored background tab stays
            // dormant; a focused new/resumed tab flips to the full dot the instant its control inits, via
            // _TrackSessionStarted below). The bind path never runs for a dormant tab (no claude process to
            // correlate), so this seed is the ONLY thing that marks a restored tab dormant on the strip.
            if (const auto s = _sessionRegistry->Get(spec.sessionId))
            {
                _SetTabAgentDot(tab, AgentStatusColorFor(s->state), !s->started && !s->external);
            }
            _RefreshTabFavoriteCrown(spec.sessionId); // FAVORITES.md: show the gold crown if this session is starred
            _RefreshTabTags(spec.sessionId); // bookmark tags: show the session's bookmark badges the instant its tab appears
            _TrackSessionStarted(spec.sessionId); // Agentmaster (eager-init): flip SessionInfo::started true the instant this control initializes (focused tab => no half-hollow flash)
        }

        // [refork] = a restore that re-materialized a never-messaged fork from its source into the same
        // id (forkIntoId set); [fork] = a genuine new fork; else resume/restore-fresh/spawn. The suffix
        // names the source for both fork kinds (effectiveForkFrom).
        const std::wstring tag = !forkIntoId.empty() ? L"[refork] " : (!forkFromId.empty() ? L"[fork] " : (wantResume ? L"[resume] " : (restored ? L"[restore-fresh] " : L"[spawn] ")));
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      tag + spec.sessionId + (effectiveForkFrom.empty() ? L"" : (L" (forked from " + effectiveForkFrom + L")")) + L" \"" + ttl + L"\" cwd=" + dir + L"\n");
        return tab;
    }

    // Agentmaster: on startup, load every persisted session into the registry as CLOSED
    // (live=false) — and do NOT auto-launch any of them. This is the deliberate reversal of the
    // old "close == reopen" auto-relaunch (Correctness Rule #6): the app opens to just the
    // Manager tab, the prior fleet arrives closed (resumable), and the user re-opens what they
    // want from the Sessions browser (which resumes via `claude --resume`, transcript-gated, in
    // _RestoreArchivedSession). No tabs are created here, so there is nothing to lay out / yield
    // for — the body runs synchronously. (FAVORITES.md: there is no separate "Archived" view.)
    winrt::fire_and_forget TerminalPage::_RestoreClaudeSessions()
    {
        if (!_sessionRegistry)
        {
            co_return;
        }
        // M9: loading sessions.json is a PROCESS-once action (the registry is a shared
        // singleton), but election alone isn't enough — each window restores on its OWN thread, and the
        // _RestoreWindowTabs call right after this needs the fleet ALREADY in the registry to re-home its
        // tabs. So this is a true BARRIER: the loader holds restoreMutex across the whole LoadSessions +
        // Upsert pass; a concurrent window BLOCKS here until it finishes, then sees restored==true and
        // returns. Without it, the second window would skip its (not-yet-loaded) sessions and then flush
        // an empty record over its saved workspace.
        //
        // The block below MUST stay strictly synchronous (no co_await): a fire_and_forget can resume on a
        // different thread, and releasing a std::mutex on a thread other than the one that acquired it is
        // undefined. Keep the lock_guard's scope co_await-free.
        // Agentmaster (terminate-net): a throw in the load (sessions.json IO/parse, a bad_alloc on a
        // huge fleet) escaping this fire_and_forget would std::terminate the app AT STARTUP. Contain +
        // log; the lock_guard unwinds on this same thread (no co_await inside — the rule above), and
        // eng.restored stays false on a failed load so the next window's barrier pass retries.
        try
        {
            auto& eng = ::Agentmaster::SharedEngine();
            std::lock_guard<std::mutex> restoreGuard{ eng.restoreMutex };
            if (!eng.restored)
            {
                auto saved = ::Agentmaster::LoadSessions();
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restore] " + std::to_wstring(saved.size()) + L" session(s) loaded as archived\n");

                for (auto& s : saved)
                {
                    if (s.id.empty() || s.workingDir.empty())
                    {
                        continue;
                    }
                    // Archived: present + restorable, but no live tab/claude. Reset the transient
                    // runtime fields (a fresh process has no live state) and keep the persisted queue +
                    // autorunner intact (Rule #6: restore == resume, never replay; Sent stays Sent).
                    s.live = false;
                    s.external = false;
                    // Agentmaster (crash/restore state fidelity — Rule #16): PRESERVE the persisted
                    // "needs you" triage state (WaitingForInput / NeedsApproval) instead of forcing Idle,
                    // so a crash (which persisted the LIVE state) doesn't drop "which sessions need me".
                    // In-flight/ended states normalize to Idle (RestoredSessionState). The card is not
                    // shown until re-homed (live=false here); the SessionScanner then refines it and the
                    // Waiting decay is read-gated (readUnixMs resets to 0 -> unread -> keeps waiting).
                    s.state = ::Agentmaster::RestoredSessionState(s.state);
                    // The question-guard flag (persisted too) rides ONLY a preserved needs-you state:
                    // keep it for a WaitingForInput turn that ended on a question (so the Autorunner queue
                    // stays held across the crash and can't auto-answer it), drop it when the state
                    // normalized to Idle (a stale flag from an interrupted Running turn must not hold).
                    s.lastMessageWasQuestion = ::Agentmaster::RestoredQuestionFlag(s.state, s.lastMessageWasQuestion);
                    s.pendingConfirmPromptId.clear();
                    _sessionRegistry->Upsert(std::move(s));
                }
                eng.restored = true; // publish ONLY after the registry is fully populated (still under the lock)
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_RestoreClaudeSessions");
        }
        co_return;
    }

    // Agentmaster: jump to (focus) a session's terminal tab. This is the Explorer Tree's /
    // Triage Board's "Activate" — it NEVER injects into the connection (Correctness Rule #2).
    // Local first: a session hosted in THIS window is a plain tab select. Hosted ELSEWHERE
    // (the board + the tree's GLOBAL scope show the whole fleet), fan out through the engine's
    // per-window activate sinks — the (single) window holding the tab selects it and brings
    // itself to the foreground. No host anywhere (archived / mid-bind) -> a no-op, as before.
    void TerminalPage::_ActivateClaudeSession(winrt::hstring sessionId)
    {
        const std::wstring id{ sessionId };
        // Nav audit: the user asked to jump to this session's live tab (board/tree double-click, tree
        // Enter, a "Jump to tab" menu, the Auto-Testing eye, or the Sessions page). `local` = the tab
        // is in THIS window; `fan-out` = it lives in another window, handed off via the engine sink.
        if (_FocusClaudeSessionTab(id, /*bringWindowToFront*/ false))
        {
            ::Agentmaster::LogNav(L"activate " + ::Agentmaster::ShortId(id) + L" (local)");
            return;
        }
        ::Agentmaster::LogNav(L"activate " + ::Agentmaster::ShortId(id) + L" (fan-out to other windows)");
        ::Agentmaster::ActivateSessionInOtherWindows(id, _windowId);
    }

    // Agentmaster (cross-window activate): select `sessionId`'s tab IN THIS WINDOW — the local
    // half of _ActivateClaudeSession and the receiving half of the engine's activate fan-out.
    // With `bringWindowToFront` (a remote window asked us to surface the session) also restore +
    // foreground this window's HWND: the user's click happened in ANOTHER window of THIS process,
    // so the process holds foreground and SetForegroundWindow may hand it over (the same
    // restore/foreground/SwitchToThisWindow recipe ProcessInspect uses for foreign windows).
    // Returns false when this window doesn't host the session's tab.
    bool TerminalPage::_FocusClaudeSessionTab(const std::wstring& sessionId, bool bringWindowToFront)
    {
        const auto it = _claudeTabs.find(sessionId);
        const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr;
        if (!tab)
        {
            return false;
        }
        if (const auto& item = tab.TabViewItem())
        {
            _tabView.SelectedItem(item);
        }
        if (bringWindowToFront && _hostingHwnd)
        {
            const HWND hwnd = *_hostingHwnd;
            if (::IsIconic(hwnd))
            {
                ::ShowWindow(hwnd, SW_RESTORE);
            }
            ::SetForegroundWindow(hwnd);
            if (::GetForegroundWindow() != hwnd)
            {
                ::SwitchToThisWindow(hwnd, TRUE); // belt-and-braces when the shell denies the hand-off
            }
        }
        return true;
    }

    // Agentmaster (Triage Board / Explorer-tree "Restart session"): restart a managed session's live
    // connection in place — local first (this window hosts the tab), else fan out to the hosting window.
    // Mirrors _ActivateClaudeSession's local-first-then-fan-out shape: the board's GLOBAL scope shows the
    // whole fleet, but the LIVE ConPTY pane lives in exactly ONE window, and restarting it touches that
    // window's TermControl, so it must run there.
    void TerminalPage::_RestartClaudeSession(winrt::hstring sessionId)
    {
        const std::wstring id{ sessionId };
        // Nav audit BEGIN: the user asked to RESTART this session (tab/overlay menu) — relaunch its claude
        // on the same conversation. Restart rebuilds the ConPTY (a crash-prone seam — _restartPaneConnection
        // has AV'd before), so log BEFORE the work: a restart-begin with no restart-done pinpoints a crash
        // mid-rebuild. The downstream [restart]/[restart-blocked] carry the mechanism.
        ::Agentmaster::LogNav(L"restart-begin " + ::Agentmaster::ShortId(id));
        if (_RestartClaudeSessionLocal(id))
        {
            ::Agentmaster::LogNav(L"restart-done " + ::Agentmaster::ShortId(id) + L" (local)");
            return;
        }
        // Not hosted here -> hand the restart to the window that owns the tab; that window's own
        // _RestartClaudeSessionLocal does the rebuild. So "fan-out" is the HANDOFF, not the completion —
        // the receiving window's mechanism tags carry the rest.
        ::Agentmaster::LogNav(L"restart-done " + ::Agentmaster::ShortId(id) + L" (fan-out to other windows)");
        ::Agentmaster::RestartSessionInOtherWindows(id, _windowId);
    }

    // Agentmaster (cross-window restart): restart `sessionId`'s tab IN THIS WINDOW — the local half of
    // _RestartClaudeSession and the receiving half of the engine's restart fan-out. Returns false when
    // this window doesn't host the session's tab (the caller then fans out). Reuses the SAME entry the WT
    // tab menu's "Restart session" uses (_restartPaneConnection): the NotConnected guard (never restart a
    // never-activated restored tab -> AV) + _RestartManagedSession (rebuild from the CURRENT conversation,
    // re-point the injector). Acts on the tab's ACTIVE pane, exactly like the tab menu — a managed session
    // is normally single-pane; returns true whenever the tab is found here so a hosted session never fans
    // out to other windows.
    bool TerminalPage::_RestartClaudeSessionLocal(const std::wstring& sessionId)
    {
        const auto it = _claudeTabs.find(sessionId);
        const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr;
        if (!tab)
        {
            return false; // not hosted here -> the caller fans out to the other windows
        }
        if (const auto tabImpl = _GetTabImpl(tab))
        {
            if (const auto content = tabImpl->GetActiveContent().try_as<TerminalApp::TerminalPaneContent>())
            {
                _restartPaneConnection(content, nullptr);
            }
        }
        return true;
    }

    // Agentmaster: close a session (the Manager's "Close" / tree Del / Auto-Testing "Close"). It
    // routes through the SAME tab-close seam as clicking the tab's X, so the one Close confirm +
    // archive (keep-the-record) bookkeeping (in _HandleCloseTabRequested -> _ArchiveAndCloseClaudeTab)
    // applies uniformly. The session record is KEPT (live=false) so it persists and lists in the
    // Sessions browser, resumable (FAVORITES.md: always archive, never delete).
    void TerminalPage::_ArchiveClaudeSession(winrt::hstring sessionId)
    {
        const std::wstring id{ sessionId };
        const auto it = _claudeTabs.find(id);
        if (const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr)
        {
            _HandleCloseTabRequested(tab); // -> archive confirm + bookkeeping + close
            return;
        }
        // No live tab in THIS window. The no-tab branch can't actually SHUT DOWN the claude (we don't
        // host its ConPTY), so it must NOT fake an archive on a still-running session (lifecycle gap
        // #2): setting live=false on a claude that's still alive is reverted to live=true by the next
        // Fleet Observer survey (ObserveClaude, Rule #7 — an observed claude is running), bouncing the
        // card back onto the board ~2s later. That alive-but-no-local-tab case is an OURS claude the
        // observer carded a tick before its bind completed, or one hosted by ANOTHER window (the board
        // shows the whole fleet, but per-window actions are local — like Activate). Leave such a
        // session Open: it gets archived from the window that hosts it, or once it binds here the first
        // branch above closes its tab for real. Only a session whose claude has actually EXITED archives
        // here — the legitimate "already closed" cleanup; its process is gone, so nothing revives it.
        if (_sessionRegistry)
        {
            if (const auto info = _sessionRegistry->Get(id); info && info->pid != 0 && ::Agentmaster::ProcessAlive(info->pid))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[archive] " + id + L" not archived here \x2014 claude pid=" + std::to_wstring(info->pid) + L" still alive, not hosted in this window\n");
                return;
            }
            _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(id, nullptr);
            ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        }
    }

    // Agentmaster (FAVORITES.md — INTERNAL record-drop): drop a managed record from the registry +
    // persist, clear this window's per-session maps, and strip the id from SAVED (non-live) window
    // records so a reopen can't resurrect it. The conversation .jsonl on disk is deliberately KEPT
    // (it still appears in the Sessions browser and can be reopened from there). With the "always
    // archive, never delete" model there is NO user-facing Delete — Close keeps the record (the
    // Sessions browser is the sole history). This seam survives only for the resume-fresh stale-drop
    // (a session whose transcript is gone, replaced by a fresh launch — see _RestoreArchivedSession),
    // so it deliberately does NOT auto-hide the id (hiding a vanished session is pointless, and a
    // Closed session must stay visible in Sessions).
    void TerminalPage::_RemoveSessionRecord(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        if (_sessionRegistry)
        {
            _sessionRegistry->Remove(sessionId); // erases the record + its injector (releases the connection) + notifies
            ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        }
        _claudeTabs.erase(sessionId);
        _claudeOverlays.erase(sessionId); // drop the per-tab overlay (detaches its registry observer)
        _StripSessionFromSavedWindows(sessionId);
        _RenderSessionsTable(); // refresh the Sessions page if it happens to be open (guarded no-op otherwise)
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[remove-record] " + sessionId + L"\n");
    }

    // Agentmaster: strip a removed session's tab refs from SAVED (non-live) window records, so reopening
    // such a window can't resurrect the deleted session. Only RecoverableWindows() (records minus the
    // live set) is touched: a LIVE window re-captures its OWN record on the next autosave (the removed
    // session's tab is closed, so it's naturally excluded) — rewriting its on-disk record here would
    // clobber its freshest geometry/lens. (A dangling ref left behind is already harmless —
    // _RestoreWindowTabs skips a ref whose registry record is gone — this just keeps the Archive page's
    // saved-window composition honest and the reopen tidy.) Best-effort; never throws into the caller.
    void TerminalPage::_StripSessionFromSavedWindows(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        try
        {
            for (const auto& rw : ::Agentmaster::RecoverableWindows())
            {
                ::Agentmaster::WindowRecord rec = rw.record;
                bool changed = false;
                const auto before = rec.tabs.size();
                rec.tabs.erase(std::remove_if(rec.tabs.begin(), rec.tabs.end(),
                                              [&sessionId](const ::Agentmaster::TabEntry& t) { return t.sessionId == sessionId; }),
                               rec.tabs.end());
                if (rec.tabs.size() != before)
                {
                    changed = true;
                }
                if (rec.selectedSessionId == sessionId)
                {
                    rec.selectedSessionId.clear();
                    changed = true;
                }
                if (changed)
                {
                    ::Agentmaster::SaveWindowRecord(rec);
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"remove session from window records");
        }
    }

    // Agentmaster (permanent remove — record-only): the UI seam routed from the Manager's "Delete
    // permanently" menus (which confirm first) and the Archive page's trash. If a live tab hosts the
    // session in THIS window, tear it down (the record drop releases the injector/connection, then
    // close the tab); a session still RUNNING but hosted in ANOTHER window is refused (the Fleet
    // Observer would re-create it next survey — Rule #7); an archived/exited session is simply removed.
    void TerminalPage::_DeleteClaudeSession(winrt::hstring sessionId)
    {
        const std::wstring id{ sessionId };
        if (id.empty() || !_sessionRegistry)
        {
            return;
        }
        TerminalApp::Tab liveTab{ nullptr };
        if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
        {
            liveTab = it->second.get();
        }
        if (!liveTab)
        {
            if (const auto info = _sessionRegistry->Get(id); info && info->pid != 0 && ::Agentmaster::ProcessAlive(info->pid))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[delete] " + id + L" not removed here \x2014 claude pid=" + std::to_wstring(info->pid) + L" still alive, not hosted in this window\n");
                return;
            }
        }
        _RemoveSessionRecord(id);
        if (liveTab)
        {
            liveTab.Close(); // -> Closed -> _RemoveTab (tab.Shutdown disconnects -> claude.exe exits)
        }
    }

    // Agentmaster: the shared CLOSE seam (FAVORITES.md) — confirm (unless suppressed), keep the
    // record + archive bookkeeping, then close the tab. KEEPING the registry record (live=false) is
    // what makes Close non-destructive: the session persists and lists in the Sessions browser,
    // resumable anytime. "Always archive, never delete" — there is no Delete branch here anymore;
    // the Claude transcript on disk is never touched either.
    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ArchiveAndCloseClaudeTab(TerminalApp::Tab tab, std::wstring sessionId, bool skipConfirm)
    {
        // Consequence confirm (a buttons-only ContentDialog -> XAML-Islands-safe). Gated by the
        // cog's confirmBeforeKill (relabeled "confirm before archiving"); skipConfirm is set when
        // an aggregate confirmation already ran (e.g. close-other-tabs).
        if (_appSettings.confirmBeforeKill && !skipConfirm)
        {
            if (const auto presenter{ _dialogPresenter.get() })
            {
                std::wstring title;
                if (_sessionRegistry)
                {
                    if (const auto s = _sessionRegistry->Get(sessionId))
                    {
                        title = s->title;
                    }
                }
                // FAVORITES.md: is this session ALREADY starred? Drives the Secondary button's sense
                // below (Favorite & Close vs Unfavorite & Close). The star lives in the durable
                // SessionStore, not the registry SessionInfo, so read it directly.
                const bool alreadyFavorite = ::Agentmaster::IsSessionFavorite(sessionId);
                ContentDialog dialog;
                dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
                dialog.Title(winrt::box_value(L"Close session?"));
                // FAVORITES.md: Close is the ONE verb — it shuts the session down but KEEPS the record
                // (always archived), so it stays in the Sessions browser, resumable anytime. Nothing is
                // deleted; there is no Delete option. (Star it in Sessions to keep it in your favorites.)
                dialog.Content(winrt::box_value(title.empty() ?
                                                    winrt::hstring{ L"Closing shuts the session down. It stays in Sessions — resume it anytime, and star it there to keep it in your favorites." } :
                                                    winrt::hstring{ L"“" + title + L"” — closing shuts it down. It stays in Sessions — resume it anytime, and star it there to keep it in your favorites." }));
                dialog.PrimaryButtonText(L"Close");
                // FAVORITES.md: the Secondary button FLIPS sense on alreadyFavorite — "★ Favorite & Close"
                // (star, then archive) when unstarred, vs "☆ Unfavorite & Close" when it's ALREADY a
                // favorite. The hollow ☆ is the anti-star (the SAME unfavorited glyph the Sessions page's
                // ★ column uses), so the gesture clearly reads as REMOVING the star rather than re-applying
                // it (re-favoriting an already-favorite session would be a no-op).
                dialog.SecondaryButtonText(alreadyFavorite ? L"☆ Unfavorite & Close" : L"★ Favorite & Close");
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel (the Close button)

                const auto weak = get_weak();
                const auto result = co_await presenter.ShowDialog(dialog);
                const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
                if (!strong)
                {
                    co_return;
                }
                if (result == ContentDialogResult::None)
                {
                    ::Agentmaster::LogNav(L"close-cancelled " + ::Agentmaster::ShortId(sessionId)); // asked, declined — the session stays Open
                    co_return; // Cancel / dismiss -> leave the session Open
                }
                if (result == ContentDialogResult::Secondary)
                {
                    // The Secondary gesture TOGGLES the durable star (SessionStore key) BEFORE archiving:
                    // an unfavorited session is starred ("★ Favorite & Close") so it's findable via the
                    // Sessions page's ★ column / [ ] Favorite filter; an already-favorite one is UN-starred
                    // ("☆ Unfavorite & Close"). Either way we then fall through to the archive below.
                    ::Agentmaster::SetSessionFavorite(sessionId, !alreadyFavorite);
                    // Nav audit: the dialog's star flip (otherwise invisible — the `favorite` LogNav lives
                    // on the Sessions-page toggle seam, not this direct SetSessionFavorite).
                    ::Agentmaster::LogNav(L"close-confirm " + ::Agentmaster::ShortId(sessionId) + (alreadyFavorite ? L" (unfavorite & close)" : L" (favorite & close)"));
                }
                // Primary (Close) or Secondary (Favorite/Unfavorite & Close) -> fall through to the
                // archive (keep-the-record) bookkeeping below.
            }
            // No presenter to confirm with -> close anyway (it's non-destructive; don't strand the close).
        }

        // Archive bookkeeping: KEEP the record, flip it to Archived, unbind stdin, persist, and
        // drop the sessionId -> tab mapping (so the close below is a plain teardown).
        if (_sessionRegistry)
        {
            _sessionRegistry->Update(sessionId, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(sessionId, nullptr);
            ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        }
        _claudeTabs.erase(sessionId);
        _claudeOverlays.erase(sessionId); // drop the per-tab overlay (detaches its registry observer)
        // Nav audit BEGIN (the user closed this tab) beside the [archive] mechanism line. Close always
        // archives — the record stays resumable from Sessions; nothing on disk is deleted. Logged BEFORE
        // tab.Close() tears down the ConPTY (claude.exe exits), so a close-begin with no close-done flags a
        // crash during teardown.
        ::Agentmaster::LogNav(L"close-begin " + ::Agentmaster::ShortId(sessionId) + L" (archived, resumable)");
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[archive] " + sessionId + L"\n");

        tab.Close(); // -> Closed -> _RemoveTab (tab.Shutdown disconnects -> claude.exe exits)
        ::Agentmaster::LogNav(L"close-done " + ::Agentmaster::ShortId(sessionId));
        co_return;
    }

    // Agentmaster (lifecycle gap #1): archive THIS window's live Claude sessions as the window tears
    // down. Closing a window (chrome ✕ / Alt+F4 / closeWindow action / quit) destroys its tabs but —
    // unlike the per-tab X — runs NO archive bookkeeping. Without this the sessions linger live=true
    // in the SHARED registry as phantom cards on every OTHER window's Triage Board (Activate is a
    // no-op; the claude is already gone), and worse: each session's injector lambda holds a STRONG
    // ref to its ConptyConnection, so the connection never releases and claude.exe is orphaned past
    // the window. Mirror _ArchiveAndCloseClaudeTab's bookkeeping for every hosted session — minus the
    // confirm dialog and tab.Close() (the tabs go with the window): flip live=false, clear the
    // injector (releases the connection -> claude.exe exits), drop the per-window maps, persist once.
    // Called from the deterministic close seam (CloseWindow, after the confirm) for IMMEDIATE phantom
    // clearing, and again from ~TerminalPage as the catch-all for quit / any other teardown path.
    // Idempotent: the second call finds _claudeTabs already cleared and no-ops; sessions archived
    // earlier via tab-X are already gone from _claudeTabs, so they aren't touched. The shared registry
    // outlives every window (held by SharedEngine), so this stays valid in the destructor.
    void TerminalPage::_ArchiveWindowSessionsOnTeardown()
    {
        if (!_sessionRegistry || _claudeTabs.empty())
        {
            return;
        }
        // Iterate-then-clear (never erase a node mid-iteration): `id` is a ref into the map node, used
        // only inside the loop body; the whole map is cleared AFTER the loop.
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            // "Is this session still mine?" A Claude tab/pane that moved to ANOTHER window (a cross-
            // window tear-out / move that didn't synchronously evict — or the eventual-consistency
            // backstop to the eviction hooks _DetachClaudeTab/PaneForMove) is ALIVE there. The Fleet
            // Observer re-attributes such a session to its new host window (ownerWindowId, roster-derived
            // — ProcessObserver), and the registry never clobbers a known owner with empty. So a non-empty
            // owner that isn't us means the session is hosted in a DIFFERENT window: archiving it here
            // would flip it live=false + clear the injector of a session in active use elsewhere (and,
            // since the injector lambda holds the only strong ref, could tear down its connection). Skip
            // it — leave it live + bound. (Empty ownerWindowId = ours / not-yet-correlated, e.g. a just-
            // launched session before the first observer tick -> archive normally; a session we genuinely
            // host reads ownerWindowId == _windowId by teardown.)
            if (const auto s = _sessionRegistry->Get(id); s && !s->ownerWindowId.empty() && s->ownerWindowId != _windowId)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[teardown-archive] skip " + id + L" (now hosted by window " + s->ownerWindowId + L")\n");
                continue;
            }
            _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(id, nullptr);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[teardown-archive] " + id + L"\n");
        }
        _claudeTabs.clear();
        _claudeOverlays.clear(); // releases the overlays' com_ptrs (detaches their registry observers)
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
    }

    // Agentmaster: re-launch (resume) a closed session from the Sessions browser. The record is
    // still in the registry (live=false); _LaunchClaudeSession resumes it (claude --resume <id>,
    // transcript-gated) with its Auto Testing + autorunner, and flips it back to live (Open). A
    // missing transcript yields a fresh id; _LaunchClaudeSession then drops the stale record so it
    // doesn't linger. (Name kept for churn; "archived" here just means the !live state.)
    TerminalApp::Tab TerminalPage::_RestoreArchivedSession(winrt::hstring sessionId)
    {
        if (!_sessionRegistry)
        {
            return nullptr;
        }
        const auto info = _sessionRegistry->Get(std::wstring{ sessionId });
        if (!info || info->live)
        {
            return nullptr; // unknown, or already Open
        }
        // Kind-aware restore: a Codex record resumes via `codex resume <uuid>` (gated on its rollout),
        // a Claude record via `claude --resume <id>`. Both fall back to fresh when the transcript is gone.
        // Returns the launched tab so the caller's nav-END can name the actually-opened id (a resume that
        // degraded to fresh reports the NEW id, not the requested one).
        if (info->kind == ::Agentmaster::AgentKind::Codex)
        {
            return _LaunchCodexSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
        }
        // Native-exe-only policy gate (auto-recovering) — Claude restore only (Codex above needs no
        // claude.exe). Covers the Archive page's "Restore here" / bulk Restore / double-click and the
        // Manager's restore handler; the idempotent prompt collapses a bulk loop to one dialog.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _PromptClaudeMissing();
            return nullptr;
        }
        return _LaunchClaudeSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
    }

    // Agentmaster (Codex managed-session support): launch a codex.exe on a ConPTY as a MANAGED tab,
    // on the SAME path as _LaunchClaudeSession. The divergences are all Codex-inherent:
    //   * No --session-id: OUR minted `handleId` is the durable registry/persistence/tab-map key (id's
    //     role for Claude); the REAL rollout uuid (the `codex resume` target) is carried on
    //     SessionInfo.codexSessionId and FILLED by the Fleet Observer once the rollout resolves.
    //   * No hooks / no --settings: bare `codex` (config.toml governs); state comes from the C2
    //     rollout-tail (mapped onto SessionState by _ReconcileManagedCodex each probe).
    //   * Lifecycle + state only — NO stdin injector / Autorunner (driving the Codex TUI is a later
    //     phase). The user types directly into the tab's ConPTY; Autorunner is forced Off.
    // `restored` set => RESUME that conversation (reusing its handle) with its rollout uuid, gated on
    // the rollout still existing; else a fresh codex (a new rollout the observer will resolve).
    void TerminalPage::_SpawnCodexSession(winrt::hstring workingDir, winrt::hstring title, uint32_t insertPosition)
    {
        // Nav audit: the user asked for a FRESH Codex session here (the launch bar's Claude⇄Codex
        // toggle on Codex, or an External "Open New Codex Session Here"). The minted handle id lands
        // in the downstream [codex-spawn] line; the observer fills its rollout uuid on first prompt.
        ::Agentmaster::LogNav(L"open-new codex dir=" + std::wstring{ workingDir } + (_openClaudeTabInBackground ? L" [bg]" : L""));
        const auto spawnedTab = _LaunchCodexSession(workingDir, title, std::nullopt, {}, insertPosition);
        // Nav audit END (pairs with the open-new codex BEGIN above): the minted DURABLE handle id (the
        // registry/tab key). The real rollout uuid is filled later by the observer on first prompt — so
        // the [codex-spawn] mechanism line + a later _ReconcileManagedCodex carry the rollout; this END
        // names the handle the user's tab is keyed on. A begin with no done = a crash before the tab existed.
        const std::wstring spawnedId = spawnedTab ? _ClaudeSessionForTab(spawnedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"open-new codex done " + (spawnedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(spawnedId))));
    }

    TerminalApp::Tab TerminalPage::_LaunchCodexSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromCodexUuid, uint32_t insertPosition)
    {
        if (!_sessionRegistry)
        {
            return nullptr;
        }

        std::wstring dir{ workingDir };
        if (dir.empty())
        {
            wchar_t up[MAX_PATH];
            const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
            dir = (n > 0 && n < MAX_PATH) ? std::wstring{ up, n } : std::wstring{ L"C:\\" };
        }
        std::wstring ttl{ title };
        if (ttl.empty())
        {
            ttl = ::Agentmaster::DeriveSessionTitle(dir);
        }

        // OUR durable handle = the registry / persistence / tab-map key. On restore REUSE the archived
        // handle (the same record flips back live); a fresh launch mints a new one.
        const std::wstring handleId = (restored && !restored->id.empty()) ? restored->id : ::Agentmaster::NewSessionId();

        // FORK (adopt a LIVE external safely) WINS over resume: `codex fork <uuid>` branches the
        // source rollout into a NEW rollout, so the source is never written to (no two-writers on one
        // rollout). Like resume, it is rollout-gated — a uuid whose rollout vanished drops to fresh.
        // The fork mints its OWN rollout; the observer resolves that NEW uuid onto codexSessionId, so a
        // forked session NEVER inherits the source's resume target.
        std::wstring forkUuid;
        if (!forkFromCodexUuid.empty() &&
            !::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), forkFromCodexUuid).empty())
        {
            forkUuid = forkFromCodexUuid;
        }
        // Resume ONLY if Codex still has a rollout for the persisted uuid (the resume target). No uuid
        // (a never-prompted archived codex) or the rollout vanished -> launch fresh in the dir (Codex
        // mints a new rollout; the observer re-resolves codexSessionId). Mirrors Claude's transcript-
        // gated resume (Rule #6). Skipped entirely when forking (fork wins).
        std::wstring resumeUuid;
        if (forkUuid.empty() && restored && !restored->codexSessionId.empty())
        {
            if (!::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), restored->codexSessionId).empty())
            {
                resumeUuid = restored->codexSessionId;
            }
        }
        // Launch codex BY FULL PATH (resolved once at engine init). ConPTY's CreateProcessW appends
        // only ".exe" and ignores PATHEXT, so a bare `codex` token would miss an npm codex.cmd and die
        // 0x80070002 (ERROR_FILE_NOT_FOUND). Empty launcher falls back to the bare token (surfaces the error).
        const std::wstring commandline = ::Agentmaster::BuildCodexCommandline(resumeUuid, forkUuid, ::Agentmaster::SharedEngine().codexExePath);

        // Child env: NO CCMGR_* (Codex has no hook bridge) — the cog's global env merged with this dir's
        // per-dir overrides (ResolveSessionEnv: dir-env.json over AppSettings.env, CCMGR_* dropped);
        // _BuildAgentConnection appends this window's AM_SESSION ownership stamp (same as a launched claude),
        // so the Fleet Observer attributes this codex to THIS window.
        const auto codexEnv = ::Agentmaster::ResolveSessionEnv(::Agentmaster::LoadAppSettings(), dir);
        // Host codex inside an interactive pwsh too (same as claude): quitting codex drops to a live
        // `PS <cwd>>` prompt instead of a dead "press Enter to restart" pane. The observer finds
        // codex.exe as a descendant of pwsh (FindDescendantByImage is descendant-OR-self), so the C1/C2
        // enrichment + state reconcile are unaffected.
        const std::wstring hostedCmd = ::Agentmaster::BuildPwshHostedCommandline(::Agentmaster::SharedEngine().pwshExePath, commandline);
        auto connection = _BuildAgentConnection(hostedCmd, dir, ttl, codexEnv);

        Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs{};
        const auto pane = _MakePane(newTerminalArgs, winrt::TerminalApp::Tab{ nullptr }, connection);
        if (!pane)
        {
            // Silent before: a null pane (ConPTY/tab build failed) produced no tab AND no trace (see the
            // Claude launcher) — record it so a vanished Codex spawn is diagnosable.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[launch-fail] " + handleId + L" \"" + ttl + L"\" cwd=" + dir + L" (codex pane build returned null \x2014 no tab created)\n");
            return nullptr;
        }
        // Suppress closeOnExit so a finished/crashed codex leaves its tab open to read (the liveness
        // sweep archives it), exactly like a Claude pane.
        pane->WalkTree([](auto&& p) {
            if (const auto content = p->GetContent())
            {
                if (const auto term = content.try_as<winrt::TerminalApp::TerminalPaneContent>())
                {
                    if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(term))
                    {
                        impl->SuppressAutoClose();
                    }
                }
            }
        });

        // insertPosition (default -1) places the tab: -1 -> end/NewTabPosition; a tab-context-menu
        // "New Session Here" / "Fork session" passes clickedIndex+1 so the new tab lands next to it.
        const auto tab = _CreateNewTabFromPane(pane, insertPosition);
        if (tab)
        {
            _claudeTabs[handleId] = winrt::make_weak(tab); // the tab map is agent-agnostic (Activate / Archive / capture)
        }

        // Register the managed Codex record (kind=Codex) — a live card at t=0, like Claude. No injector
        // (lifecycle + state only); Autorunner forced Off (nothing to drive). codexSessionId carries the
        // resume target (kept across restore; filled later by the observer for a fresh launch).
        ::Agentmaster::SessionInfo info = restored ? *restored : ::Agentmaster::SessionInfo{};
        info.id = handleId;
        info.kind = ::Agentmaster::AgentKind::Codex;
        info.title = ttl;
        info.workingDir = dir;
        info.codexSessionId = resumeUuid.empty() ? (restored ? restored->codexSessionId : std::wstring{}) : resumeUuid;
        // Agentmaster (crash/restore state fidelity — Rule #16): mirror _LaunchClaudeSession. A reopened
        // Codex keeps its persisted WaitingForInput (Codex has no NeedsApproval/Error — the 3-state floor),
        // so a DORMANT restored Codex tab shows the triage cue before it's activated; a fresh spawn is Idle.
        // Once activated + correlated, _ReconcileManagedCodex overwrites this UNCONDITIONALLY with the
        // rollout-tail-derived state, so this seed only governs the dormant (pre-activation) window.
        info.state = restored ? ::Agentmaster::RestoredSessionState(info.state) : ::Agentmaster::SessionState::Idle;
        info.external = false; // we own this tab's ConPTY
        info.live = true;
        info.autorunner.mode = ::Agentmaster::AutorunnerMode::Off; // no driving in this phase
        info.pendingConfirmPromptId.clear();
        _sessionRegistry->Upsert(info);

        if (tab)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                _SetClaudeTabTextPinned(impl, winrt::hstring{ ttl }); // pinned: registry->tab, no write-back
            }
            _ApplySessionTabColor(tab, handleId, dir); // mode-aware tab color (per-dir / individual / inferred)
            _AttachClaudeOverlay(tab, handleId); // the per-tab badge (status color from the record's state)
            if (const auto s = _sessionRegistry->Get(handleId))
            {
                _SetTabAgentDot(tab, AgentStatusColorFor(s->state));
            }
            _RefreshTabTags(handleId); // bookmark tags: a managed Codex tab wears its badges too
        }

        const std::wstring tag = !forkUuid.empty() ? L"[codex-fork] " : (!resumeUuid.empty() ? L"[codex-resume] " : (restored ? L"[codex-restore-fresh] " : L"[codex-spawn] "));
        const std::wstring rolloutNote = !forkUuid.empty() ? (L" (forked from " + forkUuid + L")") : (resumeUuid.empty() ? L"" : (L" (rollout " + resumeUuid + L")"));
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      tag + handleId + rolloutNote + L" \"" + ttl + L"\" cwd=" + dir + L"\n");
        return tab;
    }

    // Agentmaster (Codex): adopt an EXTERNAL (observe-only) codex from the Explorer Tree's EXTERNAL
    // scope — the Codex analog of _AdoptExternalClaude. We host no ConPTY for the foreign codex, so we
    // can't drive it (Rule #13); "adopt" brings its CONVERSATION (its rollout) under management in a
    // NEW managed tab. `fork` picks the two-writers-safe path (the user chose in the Adopt dialog):
    //   * fork == true  -> `codex fork <uuid>` branches the rollout into a NEW one; the source rollout
    //                      is untouched, so it is SAFE even though the original codex keeps running.
    //   * fork == false -> `codex resume <uuid>` continues the SAME rollout (true take-over) — the
    //                      user chose "Resume anyway" and is expected to stop the original first.
    // Resolve the rollout uuid (cwd + process-start, Rule #14). No rollout yet (never prompted) -> a
    // fresh managed codex in the same dir (both branches degrade to fresh). _LaunchCodexSession gates
    // both the resume AND the fork on the rollout still existing.
    void TerminalPage::_AdoptExternalCodex(uint32_t pid, winrt::hstring cwd, bool fork)
    {
        const std::wstring dir{ cwd };
        const int64_t start = ::Agentmaster::ProcessStartUnixMs(pid);
        const std::wstring uuid = ::Agentmaster::ResolveCodexSession(dir, start).sessionId;
        // Nav audit: the user adopted an EXTERNAL codex (rollout resolved from cwd+start). fork =
        // branch a copy into a new rollout; resume = take it over. Downstream [adopt-codex]/[codex-*]
        // carry the managed handle.
        ::Agentmaster::LogNav(L"adopt-codex pid=" + std::to_wstring(pid) + L" " + ::Agentmaster::ShortId(uuid) + (fork ? L" (fork-a-copy)" : L" (resume/take-over)") + L" cwd=" + dir);

        if (fork && !uuid.empty())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          std::wstring{ L"[adopt-codex] pid=" } + std::to_wstring(pid) + L" cwd=" + dir + L" -> fork " + uuid + L"\n");
            const auto adoptedTab = _LaunchCodexSession(cwd, winrt::hstring{}, std::nullopt, uuid); // fork: new rollout, source untouched
            // Nav audit END (pairs with the adopt-codex BEGIN above): the NEW managed handle id of the
            // forked-copy tab. A begin with no done = a crash before the managed tab was created.
            const std::wstring adoptedId = adoptedTab ? _ClaudeSessionForTab(adoptedTab) : std::wstring{};
            ::Agentmaster::LogNav(L"adopt-codex done " + (adoptedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(adoptedId))) + L" (fork of " + ::Agentmaster::ShortId(uuid) + L")");
            return;
        }

        std::optional<::Agentmaster::SessionInfo> restored;
        if (!uuid.empty())
        {
            ::Agentmaster::SessionInfo info{};
            info.kind = ::Agentmaster::AgentKind::Codex;
            info.workingDir = dir;
            info.codexSessionId = uuid; // _LaunchCodexSession gates the resume on this rollout existing
            restored = std::move(info);
        }
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      std::wstring{ L"[adopt-codex] pid=" } + std::to_wstring(pid) + L" cwd=" + dir +
                                          L" -> " + (uuid.empty() ? std::wstring{ L"(fresh \x2014 no rollout)" } : (L"resume " + uuid)) + L"\n");
        const auto adoptedTab = _LaunchCodexSession(cwd, winrt::hstring{}, restored);
        // Nav audit END (pairs with the adopt-codex BEGIN above): the NEW managed handle id of the
        // take-over (resume) tab — or a fresh handle when the external had no rollout yet.
        const std::wstring adoptedId = adoptedTab ? _ClaudeSessionForTab(adoptedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"adopt-codex done " + (adoptedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(adoptedId))) + (uuid.empty() ? std::wstring{ L" (fresh \x2014 no rollout)" } : (L" (resume of " + ::Agentmaster::ShortId(uuid) + L")")));
    }

    // Agentmaster (Fleet Observer): adopt an EXTERNAL (observe-only) claude from the Explorer Tree's
    // EXTERNAL scope. We host no ConPTY for the foreign process, so we can NEVER inject into the live
    // external (Rule #9/#13) — "adopt" instead brings its CONVERSATION under management in a NEW
    // managed, controllable tab. `fork` picks the two-writers-safe path (the user chose in the Adopt
    // dialog):
    //   * fork == true  -> claude --resume <id> --fork-session --session-id <new> branches the
    //                      conversation into a NEW transcript; the source <id>.jsonl is untouched, so
    //                      it is SAFE even though the original external keeps running (the proven
    //                      _ForkSessionFromDisk path — safe on a LIVE parent).
    //   * fork == false -> claude --resume <id> continues the SAME conversation (true take-over) — the
    //                      user chose "Resume anyway" and is expected to stop the original first to
    //                      avoid two writers on one transcript.
    // Resolve the conversation id (cwd + process-start -> ResolveSessionId, Rule #14). No transcript
    // yet (never prompted -> id "") -> a fresh managed session in the same dir (both branches degrade
    // to fresh). _LaunchClaudeSession transcript-gates both --resume and the fork, so a transcript that
    // vanished between resolve and launch also degrades to fresh rather than dying on "No conversation
    // found".
    void TerminalPage::_AdoptExternalClaude(uint32_t pid, winrt::hstring cwd, bool fork)
    {
        // Native-exe-only policy gate (auto-recovering): adopt resumes/forks the external's conversation
        // into a NEW managed claude (a launch). The Manager's external-row Adopt already gates with the
        // rich modal upstream, so when Claude is missing this is reached only via non-Manager callers —
        // prompt with the page dialog rather than no-op'ing at _LaunchClaudeSession's backstop.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _PromptClaudeMissing();
            return;
        }
        const std::wstring dir{ cwd };
        const int64_t start = ::Agentmaster::ProcessStartUnixMs(pid);
        const std::wstring id = ::Agentmaster::ResolveSessionId(dir, start);
        // Nav audit: the user adopted an EXTERNAL claude (its conversation resolved from cwd+start).
        // fork = branch a copy into a new transcript (safe on a live external); resume = take it over.
        // The downstream [adopt-external] + [fork]/[resume] lines carry the resulting managed id.
        ::Agentmaster::LogNav(L"adopt-external pid=" + std::to_wstring(pid) + L" " + ::Agentmaster::ShortId(id) + (fork ? L" (fork-a-copy)" : L" (resume/take-over)") + L" cwd=" + dir);

        if (fork && !id.empty())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          std::wstring{ L"[adopt-external] pid=" } + std::to_wstring(pid) + L" cwd=" + dir + L" -> fork " + id + L"\n");
            const auto adoptedTab = _LaunchClaudeSession(cwd, winrt::hstring{}, std::nullopt, id); // fork: new transcript, source untouched (safe on a live external)
            // Nav audit END (pairs with the adopt-external BEGIN above): the NEW managed id of the
            // forked-copy tab. A begin with no done = a crash before the managed tab was created.
            const std::wstring adoptedId = adoptedTab ? _ClaudeSessionForTab(adoptedTab) : std::wstring{};
            ::Agentmaster::LogNav(L"adopt-external done " + (adoptedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(adoptedId))) + L" (fork of " + ::Agentmaster::ShortId(id) + L")");
            return;
        }

        std::optional<::Agentmaster::SessionInfo> restored;
        if (!id.empty())
        {
            ::Agentmaster::SessionInfo info{};
            info.id = id; // _LaunchClaudeSession transcript-gates the --resume on this id
            info.workingDir = dir;
            // Adopted sessions are new to us (no persisted plan), so seed the cog's Autorunner defaults
            // as a fresh launch would — but resume the external's existing conversation.
            info.autorunner.mode = _appSettings.defaultAutorunnerMode;
            info.autorunner.maxAutoSends = _appSettings.maxAutoSends;
            info.autorunner.stopOnError = _appSettings.stopOnError;
            info.autorunner.pauseOnHumanInput = _appSettings.pauseOnHumanInput;
            restored = std::move(info);
        }
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      std::wstring{ L"[adopt-external] pid=" } + std::to_wstring(pid) + L" cwd=" + dir +
                                          L" -> " + (id.empty() ? std::wstring{ L"(fresh \x2014 no transcript)" } : (L"resume " + id)) + L"\n");
        const auto adoptedTab = _LaunchClaudeSession(cwd, winrt::hstring{}, restored);
        // Nav audit END (pairs with the adopt-external BEGIN above): the NEW managed id of the take-over
        // (resume) tab — or a fresh id when the external had no transcript yet.
        const std::wstring adoptedId = adoptedTab ? _ClaudeSessionForTab(adoptedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"adopt-external done " + (adoptedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(adoptedId))) + (id.empty() ? std::wstring{ L" (fresh \x2014 no transcript)" } : (L" (resume of " + ::Agentmaster::ShortId(id) + L")")));
    }

    // Agentmaster: which session (if any) hosts this tab? Reverse-lookup of _claudeTabs (whose
    // forward direction is sessionId -> tab). Used by the tab-close seam to recognize a Claude
    // session tab so it archives instead of plain-closing.
    std::wstring TerminalPage::_ClaudeSessionForTab(const TerminalApp::Tab& tab)
    {
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            if (const auto t = weakTab.get(); t && t == tab)
            {
                return id;
            }
        }
        return {};
    }

    // Agentmaster: which managed Claude session (if any) is BOUND to THIS connection? Match the
    // connection's WT_SESSION (== ITerminalConnection::SessionId()) against each session's tabToken (the
    // bound connection's WT_SESSION, kept current by hooks + the observer) among the sessions THIS window
    // hosts in _claudeTabs. This pinpoints the session bound to a SPECIFIC connection — NOT "a session
    // whose tab merely contains this connection" (a split tab holds several), so closing the shell
    // sibling of a Claude pane never false-matches the Claude session. Used to archive on an explicit
    // pane-close (_HandleClosePaneRequested) and to re-point the injector across a restartConnection.
    std::wstring TerminalPage::_ClaudeSessionForConnection(const TerminalConnection::ITerminalConnection& conn)
    {
        if (!_sessionRegistry || !conn)
        {
            return {};
        }
        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        const std::wstring wt = lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId()));
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            const auto info = _sessionRegistry->Get(id);
            if (info && !info->tabToken.empty() && lower(info->tabToken) == wt)
            {
                return id;
            }
        }
        return {};
    }

    // Agentmaster: build a managed-agent ConPTY connection from a ready commandline + cwd + child env.
    // Appends THIS window's AM_SESSION = "<processGuid>:<windowId>" ownership stamp LAST (so it wins over
    // any stray user `env` entry; the Fleet Observer's ClassifyRunningApp matches on the GUID prefix). The
    // SINGLE connection builder shared by _LaunchClaudeSession (spec.env carries CCMGR_* + cog env),
    // _LaunchCodexSession (cog env only — Codex has no hook bridge), and _RestartManagedSession (in-place
    // relaunch) — so all three produce the connection identically.
    TerminalConnection::ConptyConnection TerminalPage::_BuildAgentConnection(const std::wstring& commandline, const std::wstring& dir, const std::wstring& title, const std::vector<std::pair<std::wstring, std::wstring>>& env, bool inheritCursor)
    {
        auto envMap = winrt::single_threaded_map<winrt::hstring, winrt::hstring>();
        for (const auto& [k, v] : env)
        {
            envMap.Insert(winrt::hstring{ k }, winrt::hstring{ v });
        }
        if (const auto& eng = ::Agentmaster::SharedEngine(); !eng.amSession.empty() && !_windowId.empty())
        {
            envMap.Insert(winrt::hstring{ L"AM_SESSION" }, winrt::hstring{ eng.amSession + L":" + _windowId });
        }

        auto valueSet = TerminalConnection::ConptyConnection::CreateSettings(
            winrt::hstring{ commandline },
            winrt::hstring{ dir },
            winrt::hstring{ title },
            false, // reloadEnvironmentVariables
            L"", // initialEnvironment: inherit our current block
            envMap.GetView(),
            30, // rows  (the control resizes the pty to the pane on first layout)
            120, // cols
            winrt::guid{}, // WT_SESSION (auto-generated by the connection)
            winrt::guid{}); // profileGuid

        // A RESTART reuses the existing pane buffer, so inherit the cursor (no screen-clear on the swap) —
        // matching the upstream restart path. A fresh launch opens a new pane, so it leaves this off.
        if (inheritCursor)
        {
            valueSet.Insert(L"inheritCursor", winrt::Windows::Foundation::PropertyValue::CreateBoolean(true));
        }

        TerminalConnection::ConptyConnection connection{};
        connection.Initialize(valueSet);
        return connection;
    }

    // Agentmaster: which MANAGED session (Claude OR Codex) owns `conn`? Match by connection IDENTITY across
    // _claudeTabs — agent-agnostic and robust where _ClaudeSessionForConnection (keyed on tabToken ==
    // WT_SESSION) can't help: a managed Codex tab gets its tabToken only on its FIRST prompt, so a never-
    // prompted codex is unresolvable by tabToken. Walks each hosted tab's panes (a split holds several) and
    // compares the live ITerminalConnection pointer. Used by the restart path to relaunch the right agent.
    std::wstring TerminalPage::_ManagedSessionForConnection(const TerminalConnection::ITerminalConnection& conn)
    {
        if (!conn)
        {
            return {};
        }
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            const auto tab = weakTab.get();
            if (!tab)
            {
                continue;
            }
            const auto impl = _GetTabImpl(tab);
            const auto root = impl ? impl->GetRootPane() : nullptr;
            if (!root)
            {
                continue;
            }
            bool match = false;
            root->WalkTree([&](auto&& p) {
                if (const auto ctrl = p->GetTerminalControl(); ctrl && ctrl.Connection() == conn)
                {
                    match = true;
                    return true; // found — stop walking
                }
                return false;
            });
            if (match)
            {
                return id;
            }
        }
        return {};
    }

    // Agentmaster: in-place "Restart session" (WT's restartConnection) for a MANAGED Claude/Codex pane.
    // The upstream restart replays the connection's ORIGINAL launch commandline — wrong for a managed
    // agent: a fresh Claude launch's is `--session-id <id>` (NOT --resume), which after the first turn
    // collides with the now-existing transcript (claude refuses an in-use id -> the relaunch dies); a
    // fork's is `--resume <parent> --fork-session`, which would re-fork from the parent. So REBUILD the
    // connection from the CURRENT conversation state — resume <id> (transcript/rollout-gated, Rule #6),
    // else fresh reusing <id> — exactly as the launch path does, and re-point the stdin injector (Claude).
    // Returns false for a plain shell / external tab so the caller falls through to the upstream replay
    // (correct there — a shell has no conversation to resume).
    bool TerminalPage::_RestartManagedSession(const TerminalApp::TerminalPaneContent& paneContent)
    {
        if (!_sessionRegistry || !paneContent)
        {
            return false;
        }
        const auto control = paneContent.GetTermControl();
        if (!control)
        {
            return false;
        }
        const auto oldConn = control.Connection();
        std::wstring managedId = _ManagedSessionForConnection(oldConn);
        if (managedId.empty() && oldConn)
        {
            // The liveness sweep archives a session whose claude died and ERASES its _claudeTabs entry
            // while deliberately leaving the dead pane open (the exit banner is for the user to read).
            // The banner's Enter-to-restart then found no map entry, fell through to the upstream
            // PROFILE replay, and the pane came back as a plain shell — the managed session silently
            // unlinked from its own tab. Recover the identity from the registry instead: the dead
            // connection's WT_SESSION is still the record's tabToken (stamped eagerly at launch/restart
            // and by every hook). Gated on !external — an ADOPTED session's tabToken is the SHELL ConPTY
            // the user typed `claude` into, and restarting that tab must replay the shell (upstream),
            // not resurrect claude over it. Freshest-wins when one ConPTY hosted several conversations
            // over time (an in-session /resume chain leaves superseded records with the same token).
            const std::wstring wt = ::Microsoft::Console::Utils::GuidToPlainString(oldConn.SessionId());
            int64_t bestFreshness = -1;
            for (const auto& s : _sessionRegistry->Snapshot())
            {
                if (!s.external && !s.tabToken.empty() && ::Agentmaster::TabTokenEq(s.tabToken, wt))
                {
                    const int64_t freshness = std::max(s.lastActivityUnixMs, s.lastHookUnixMs);
                    if (freshness > bestFreshness)
                    {
                        bestFreshness = freshness;
                        managedId = s.id;
                    }
                }
            }
            if (!managedId.empty())
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart] " + managedId + L" resolved via tabToken " + wt + L" (tab was archived by the liveness sweep \x2014 restart revives it)\n");
                // Re-link sessionId -> tab (the sweep erased it): resolve the tab hosting this pane so
                // Activate / Close / title-sync work again right away; the observer would re-bind in ~2s
                // anyway, this just removes the gap. Overlay/status-dot re-attach ride the observer bind.
                for (const auto& t : _tabs)
                {
                    const auto impl = _GetTabImpl(t);
                    const auto root = impl ? impl->GetRootPane() : nullptr;
                    if (!root)
                    {
                        continue;
                    }
                    bool hostsPane = false;
                    root->WalkTree([&](auto&& p) {
                        if (const auto ctrl = p->GetTerminalControl(); ctrl && ctrl.Connection() == oldConn)
                        {
                            hostsPane = true;
                            return true;
                        }
                        return false;
                    });
                    if (hostsPane)
                    {
                        _claudeTabs[managedId] = winrt::make_weak(t);
                        break;
                    }
                }
            }
        }
        if (managedId.empty())
        {
            return false; // not a managed agent — let the upstream restart handle it
        }
        const auto info = _sessionRegistry->Get(managedId);
        if (!info)
        {
            // Known tab but no record (a teardown race). Don't replay the buggy launch commandline.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart] no record for " + managedId + L" \x2014 skipped\n");
            return true;
        }

        const std::wstring dir = info->workingDir;
        const std::wstring title = info->title;
        const bool isCodex = (info->kind == ::Agentmaster::AgentKind::Codex);
        bool reforked = false;

        TerminalConnection::ConptyConnection newConn{ nullptr };
        if (isCodex)
        {
            // Resume the rollout when it still exists, else fresh (rollout-gated, Rule #6). Never fork.
            std::wstring resumeUuid;
            if (!info->codexSessionId.empty() &&
                !::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), info->codexSessionId).empty())
            {
                resumeUuid = info->codexSessionId;
            }
            const std::wstring commandline = ::Agentmaster::BuildCodexCommandline(resumeUuid, {}, ::Agentmaster::SharedEngine().codexExePath);
            // Codex child env: global cog env merged with this dir's per-dir overrides (ResolveSessionEnv,
            // CCMGR_* dropped); _BuildAgentConnection stamps AM_SESSION.
            const auto codexEnv = ::Agentmaster::ResolveSessionEnv(::Agentmaster::LoadAppSettings(), dir);
            const std::wstring hostedCmd = ::Agentmaster::BuildPwshHostedCommandline(::Agentmaster::SharedEngine().pwshExePath, commandline);
            newConn = _BuildAgentConnection(hostedCmd, dir, title, codexEnv, /*inheritCursor*/ true);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart] codex " + managedId + (resumeUuid.empty() ? L" (fresh)" : (L" (resume " + resumeUuid + L")")) + L"\n");
        }
        else
        {
            // Native-exe-only policy backstop (mirrors _LaunchClaudeSession): refuse if no claude.exe.
            // Auto-recovering + silent (a restart is not a fresh user launch — log, don't prompt).
            if (!::Agentmaster::EnsureClaudeAvailable())
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart-blocked] no native claude.exe; " + managedId + L"\n");
                return true; // handled (refused) — don't fall through to the buggy replay
            }
            // Threading forkParentId lets the spec RE-FORK a never-messaged fork from its source into the
            // SAME id (the [restore->refork] recipe) instead of replacing the forked branch with an empty
            // fresh conversation — the reported "restart a fork -> loads a new claude session" loss.
            const auto spec = ::Agentmaster::BuildClaudeRestartSpec(dir, title, _hooksBridge->PipeName(), managedId, ::Agentmaster::LoadAppSettings(), ::Agentmaster::SharedEngine().claudeExePath, info->forkParentId);
            // Re-host in pwsh (as the launch path does) so the relaunched session keeps the same
            // quit-to-pwsh-prompt behavior rather than dying into a dead pane.
            const std::wstring hostedCmd = ::Agentmaster::BuildPwshHostedCommandline(::Agentmaster::SharedEngine().pwshExePath, spec.commandline);
            newConn = _BuildAgentConnection(hostedCmd, dir, title, spec.env, /*inheritCursor*/ true);
            // Log the mode straight off the built artifact (no second transcript stat / no race).
            reforked = spec.commandline.find(L"--fork-session") != std::wstring::npos;
            const bool resumed = !reforked && spec.commandline.find(L"--resume ") != std::wstring::npos;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart] claude " + managedId + (reforked ? (L" (refork from " + info->forkParentId + L")") : (resumed ? L" (resume)" : L" (fresh)")) + L"\n");
        }

        if (!newConn)
        {
            return true; // managed, but the rebuild failed — still don't replay the launch commandline
        }

        // Re-stamp the session's HOST IDENTITY before the swap. The new connection has a NEW WT_SESSION;
        // without this the record keeps the DEAD ConPTY's token and the liveness sweep — which pinpoints
        // the session's pane BY tabToken — finds no such connection in the tab and ARCHIVES the freshly
        // restarted session seconds later (clearing the injector + dropping the _claudeTabs entry; the
        // observed [restart] -> [liveness] dead -> [discover]/[adopt] churn). Stamping eagerly (the launch
        // path's idiom) also arms the --fork-session source-id echo guard for a RE-FORK, whose first
        // SessionStart echoes the PARENT id on this new ConPTY. live=true revives an archived (banner-
        // dead) record; the stale pid is dropped (the observer refills it from the new process); the
        // display state is normalized exactly like a restore (Rule #16 — a relaunched claude does not
        // continue the interrupted turn, so in-flight/ended states reset to Idle while the at-rest
        // "needs you" states survive); the persistence observer rides this Update, so it also saves.
        const std::wstring newWt = ::Microsoft::Console::Utils::GuidToPlainString(newConn.SessionId());
        _sessionRegistry->Update(managedId, [&](::Agentmaster::SessionInfo& s) {
            s.tabToken = newWt;
            s.live = true;
            s.pid = 0;
            s.state = ::Agentmaster::RestoredSessionState(s.state);
            s.lastMessageWasQuestion = ::Agentmaster::RestoredQuestionFlag(s.state, s.lastMessageWasQuestion);
            s.pendingConfirmPromptId.clear();
            s.pendingInput.clear(); // the old screen (and any unsent draft on it) is gone
            if (reforked)
            {
                s.forkEchoConsumed = false; // re-arm: the re-forked process will echo the source id once
            }
        });

        // Swap the connection — upstream's restart order. Deliberately NO explicit oldConn.Close() first:
        // control.Connection() (ControlCore::_closeConnection) revokes the output/state handlers and THEN
        // closes the old connection, so its teardown banner is raised into revoked handlers. An explicit
        // pre-swap Close() runs while the control is still attached — ConptyConnection::Close() blocks on
        // its output thread, whose exit handler (_LastConPtyClientDisconnected) reads the still-dying
        // client's exit code as STILL_ACTIVE and prints "[process exited with code 259 (0x00000103)]" +
        // "press Enter to restart" INTO the restarted pane (the reported banner). Same teardown, silent.
        control.HardResetWithoutErase();
        control.Connection(newConn);
        newConn.Start();

        // Re-point this session's stdin injector at the freshly-started connection (Claude only — Codex
        // has no injector in this phase). Correctness Rule #3 binds by sessionId; without this, Autorunner
        // / Send-now would keep writing to the replaced, dead connection. The observer won't fix it on its
        // own: it sees the tab still bound to <id> (alreadyBound) and skips re-binding.
        if (!isCodex)
        {
            const auto conn = newConn;
            _sessionRegistry->SetInjector(managedId, [conn](const std::wstring& text) {
                const auto* begin = reinterpret_cast<const char16_t*>(text.data());
                conn.WriteInput(winrt::array_view<const char16_t>{ begin, begin + text.size() });
            });
        }
        return true;
    }

    // Agentmaster: fork a MANAGED session by id — the kind-aware fork shared by the WT tab's "Fork
    // session" (_DuplicateTab) AND the Triage Board / Explorer-tree "Fork session" menu, so the two can
    // never drift. Branches the conversation into a NEW, independent session (the source's transcript is
    // untouched), registered as a normal managed session ("<title> (fork)", fresh Auto Testing), opened in
    // THIS window (it reads the shared registry — no live tab needed, so the board can fork a session
    // hosted in another window into here). A source never prompted has no transcript/rollout to fork ->
    // fresh in the same dir (Rule #6). insertPosition threads tab placement (-1 == end). Returns false
    // only when `sourceId` is not a known managed session.
    bool TerminalPage::_ForkManagedSessionById(const std::wstring& sourceId, uint32_t insertPosition, const std::wstring& modelOverride)
    {
        if (!_sessionRegistry || sourceId.empty())
        {
            return false;
        }
        const auto src = _sessionRegistry->Get(sourceId);
        if (!src)
        {
            return false; // not a known managed session
        }
        // Nav audit BEGIN: the user forked a LIVE managed session (the WT tab "Fork session" / _DuplicateTab,
        // or the board/tree "Fork session" menu). Logs the SOURCE + kind before the launch; the matching
        // fork-managed-done names the new id. A begin with no done = a crash between the two. A launch-model
        // pick rides the line (model=<id>) so "which model did that fork start on" reads off the journey.
        ::Agentmaster::LogNav(std::wstring{ L"fork-managed-begin source=" } + ::Agentmaster::ShortId(sourceId) + (modelOverride.empty() ? std::wstring{} : (L" model=" + modelOverride)) + (src->kind == ::Agentmaster::AgentKind::Codex ? L" (codex)" : L" (claude)"));
        const std::wstring dir = src->workingDir;
        const std::wstring forkBase = !src->title.empty() ? src->title : ::Agentmaster::DeriveSessionTitle(dir);
        const std::wstring ttl = ::Agentmaster::DeriveForkTitle(forkBase); // bump " (fork N)" instead of stacking
        // Codex (kind-aware): sourceId is the Codex's durable HANDLE id, which has NO Claude transcript,
        // so the Claude branch below would silently spawn a fresh claude.exe in the codex's dir (wrong
        // agent). Route to the Codex launcher — it forks via `codex fork <rolloutUuid>` and rollout-gates
        // it internally (a vanished/never-prompted rollout -> a fresh codex in the same dir), so we just
        // hand it the real rollout uuid.
        if (src->kind == ::Agentmaster::AgentKind::Codex)
        {
            const std::wstring forkFrom = src->codexSessionId; // the REAL rollout uuid (the fork source)
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-managed->codex] source=" + sourceId + (forkFrom.empty() ? L" (no rollout uuid -> fresh codex)" : L"") + L"\n");
            const auto forkedTab = _LaunchCodexSession(winrt::hstring{ dir }, winrt::hstring{ ttl }, std::nullopt, forkFrom, insertPosition);
            const std::wstring forkedId = forkedTab ? _ClaudeSessionForTab(forkedTab) : std::wstring{};
            ::Agentmaster::LogNav(L"fork-managed-done " + (forkedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(forkedId))) + L" from=" + ::Agentmaster::ShortId(sourceId) + (forkFrom.empty() ? std::wstring{ L" (codex, fresh \x2014 no rollout)" } : std::wstring{ L" (codex)" }));
            return true;
        }
        // Native-exe-only policy gate (auto-recovering): a Claude fork is a launch (--fork-session).
        // _LaunchClaudeSession's backstop is SILENT, so gate + prompt here to surface the install notice.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::LogNav(L"fork-managed-done (blocked \x2014 no native claude.exe; install prompt shown)"); // pair the BEGIN so an unpaired begin always means a crash, never the gate
            _PromptClaudeMissing();
            return true; // it IS a managed Claude session — handled (refused), never naively duplicated
        }
        const std::wstring forkFrom = ::Agentmaster::ClaudeConversationExists(sourceId) ? sourceId : std::wstring{};
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-managed->fork] source=" + sourceId + (forkFrom.empty() ? L" (no transcript -> fresh session)" : L"") + L"\n");
        const auto forkedTab = _LaunchClaudeSession(winrt::hstring{ dir }, winrt::hstring{ ttl }, std::nullopt, forkFrom, insertPosition, modelOverride);
        const std::wstring forkedId = forkedTab ? _ClaudeSessionForTab(forkedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"fork-managed-done " + (forkedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(forkedId))) + L" from=" + ::Agentmaster::ShortId(sourceId) + (forkFrom.empty() ? std::wstring{ L" (fresh \x2014 no transcript)" } : std::wstring{}));
        return true;
    }

    // Agentmaster (cross-window move): a Claude tab is being MOVED to another window — NOT closed.
    // Both the tear-out paths (_onTabDroppedOutside / a tab-strip drag onto another window, both via
    // _sendDraggedTabToWindow) and the moveTab action with a window target (_MoveTab) transfer the
    // tab's LIVE ConPTY content to a different TerminalPage in THIS same process: BuildStartupKind::
    // Content serializes only a ContentId, and the destination re-attaches the SAME ITerminalConnection
    // via the process-wide ContentManager — so the session keeps running on the same claude.exe; only
    // the hosting window changes. Those paths call _RemoveTab directly, bypassing the archive seam
    // (_HandleCloseTabRequested), so without this the moved session's entry LEAKS in this window's
    // _claudeTabs. When this window later tears down, _ArchiveWindowSessionsOnTeardown would then flip
    // that record live=false and clear its injector — severing (and possibly killing) a session that is
    // now alive in ANOTHER window.
    //
    // So evict this window's per-window binding (the _claudeTabs entry + the per-tab overlay) — but
    // deliberately leave the injector and the live flag ALONE: the injector lambda holds the strong ref
    // that keeps the ConptyConnection (hence claude.exe) alive across the move and keeps the session
    // drivable in the gap, and the session is not closing. The destination window correlates by
    // WT_SESSION (stable across the move) and RE-HOMES it on its next _ObserverProbe tick (re-points the
    // injector to its own tab + recreates the local binding + overlay — see the HasInjector branch in
    // _ObserverProbe). A non-Claude tab (a plain pwsh/cmd tab, the Manager tab) reverse-looks-up to no
    // session and is left untouched.
    void TerminalPage::_DetachClaudeTabForMove(const winrt::com_ptr<Tab>& tab)
    {
        if (!tab || _claudeTabs.empty())
        {
            return;
        }
        const auto projected = tab.try_as<winrt::TerminalApp::Tab>();
        const std::wstring id = projected ? _ClaudeSessionForTab(projected) : std::wstring{};
        if (id.empty())
        {
            return; // not a Claude session tab -> nothing to detach
        }
        // Clear the overlay element out of the moving content's slot so the destination doesn't render a
        // stale, no-longer-updating badge until adoption re-attaches a fresh one. Safe here: the content
        // is still in this tab and on this UI thread (we run before _DetachTabFromWindow). Mirrors the
        // first-terminal-pane walk in _AttachClaudeOverlay (one session per tab); clearing a pane that
        // never had an overlay is a no-op.
        if (const auto rootPane = tab->GetRootPane())
        {
            rootPane->WalkTree([](auto&& pane) {
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(term))
                        {
                            impl->SetAgentOverlay(nullptr);
                        }
                    }
                }
            });
        }
        _claudeOverlays.erase(id); // releases the overlay com_ptr -> detaches its registry observer
        _claudeTabs.erase(id); // drop the per-window binding; the injector + live flag stay untouched
        _agentNotifyLastState.erase(id); // System notifications: drop the toast track with the binding (the destination window re-tracks; a stale prev==Running here could phantom-fire if the tab later moved back)
        _agentNotifyRunningSinceMs.erase(id);
        _agentToastHeld.erase(id); // + the held/shown toast state (same rationale — the destination window owns the session's toasts now)
        _agentToastLastShownMs.erase(id);
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[move-out] " + id + L" (Claude tab leaving this window; binding kept alive for the destination)\n");
        // Nav audit (beside [move-out]): the user dragged/moved this managed tab to ANOTHER window — its
        // session changes host window, so the nav trail records that it LEFT here (the destination re-homes
        // it on its next observer probe; this window's binding is evicted, injector + live flag kept).
        ::Agentmaster::LogNav(L"move-out " + ::Agentmaster::ShortId(id) + L" (tab moved to another window)");
    }

    // Agentmaster (cross-window move, pane-level): a single PANE is being moved to another window (the
    // movePane action with a window target, _MovePane). Unlike a whole-tab move the hosting tab may
    // SURVIVE — it keeps its other panes — so we must evict the per-window binding ONLY when the LEAVING
    // pane is the one the session is bound to: the tab's first terminal pane, which is what
    // _ObserverProbe / _AttachClaudeOverlay correlate to (one session per tab). If a sibling pane (a
    // pwsh split, or a non-terminal pane) is what's moving, the session stays put and we do nothing.
    // As with _DetachClaudeTabForMove the injector + live flag are left intact — the live connection
    // moves with the pane (same claude.exe; the destination re-homes it by WT_SESSION on its next probe)
    // — only THIS window's _claudeTabs + overlay binding is dropped. The pane is still attached and on
    // this UI thread here (we run before _DetachPaneFromWindow / DetachPane).
    void TerminalPage::_DetachClaudePaneForMove(const winrt::com_ptr<Tab>& tab, const std::shared_ptr<Pane>& movingPane)
    {
        if (!tab || !movingPane || _claudeTabs.empty())
        {
            return;
        }
        const auto projected = tab.try_as<winrt::TerminalApp::Tab>();
        const std::wstring id = projected ? _ClaudeSessionForTab(projected) : std::wstring{};
        if (id.empty())
        {
            return; // this tab hosts no managed session -> nothing to detach
        }
        // The terminal connection behind a pane (null if it is not a terminal pane).
        const auto connOf = [](auto&& pane) -> TerminalConnection::ITerminalConnection {
            if (const auto content = pane->GetContent())
            {
                if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                {
                    if (const auto ctrl = term.GetTermControl())
                    {
                        return ctrl.Connection();
                    }
                }
            }
            return nullptr;
        };
        const auto movingConn = connOf(movingPane);
        if (!movingConn)
        {
            return; // moving a non-terminal pane -> the session's pane stays in this tab
        }
        // The tab's FIRST terminal pane is the one the session is bound to (the bind walks the pane tree
        // and takes the first TerminalPaneContent). A connection's SessionId is unique per pane, so
        // equality here means the moving pane IS that bound pane.
        TerminalConnection::ITerminalConnection firstConn{ nullptr };
        if (const auto rootPane = tab->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (firstConn)
                {
                    return;
                }
                if (const auto c = connOf(pane))
                {
                    firstConn = c;
                }
            });
        }
        if (!firstConn || movingConn.SessionId() != firstConn.SessionId())
        {
            return; // a sibling (non-session) terminal pane is moving -> leave the binding intact
        }
        // The session's pane is leaving this window. Clear its overlay element from the moving pane's
        // slot so the destination doesn't render a stale, no-longer-updating badge until it re-attaches.
        if (const auto content = movingPane->GetContent())
        {
            if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
            {
                if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(term))
                {
                    impl->SetAgentOverlay(nullptr);
                }
            }
        }
        _claudeOverlays.erase(id); // releases the overlay com_ptr -> detaches its registry observer
        _claudeTabs.erase(id); // drop the per-window binding; the injector + live flag stay untouched
        _agentNotifyLastState.erase(id); // System notifications: drop the toast track with the binding (the move-out twin above)
        _agentNotifyRunningSinceMs.erase(id);
        _agentToastHeld.erase(id); // + the held/shown toast state (the move-out twin above)
        _agentToastLastShownMs.erase(id);
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[move-out-pane] " + id + L" (Claude pane leaving this window; binding kept alive for the destination)\n");
    }

    // Agentmaster (Rule #11): pin a Claude tab's title from a registry-driven source — rename,
    // restore, smart-naming, bind, or the cross-window registry-observer push. Tab::SetTabText
    // synchronously raises PropertyChanged("Title") -> TerminalPage::_UpdateTitle ->
    // _SyncClaudeTitleFromTab on the SAME call stack, which would normally mirror the tab text
    // back into the registry. That reverse mirror must fire ONLY for a genuine USER tab rename,
    // never for our own programmatic pin: the _pinningClaudeTabTitle latch makes the re-entrant
    // _SyncClaudeTitleFromTab a no-op for the duration of this SetTabText. Without it, a title the
    // async registry observer captured at notify-time can go stale (the registry advanced), the
    // pin writes that stale value back, and the registry<->tab directions ping-pong forever —
    // the observed /clear symptom: the tab title swapping between the new "Agentmaster" and the
    // old conversation's "am master" while [Unknown] notify lines flooded hooks.log (a re-home
    // writes the title twice in quick succession, seeding two distinct in-flight values).
    void TerminalPage::_SetClaudeTabTextPinned(const winrt::com_ptr<Tab>& tabImpl, const winrt::hstring& title)
    {
        if (!tabImpl)
        {
            return;
        }
        _pinningClaudeTabTitle = true;
        const auto resetPin = wil::scope_exit([this]() noexcept { _pinningClaudeTabTitle = false; });
        tabImpl->SetTabText(title);
    }

    // Agentmaster: rename a Claude session from the Manager's Explorer Tree (or a Triage-Board
    // card's menu — same editor). A session's title is ONE value — the Explorer-tree name, the
    // persisted SessionInfo.title, and the WT tab title.
    // Write it to the shared registry (which persists it via the autosave-on-change observer and
    // refreshes every window's Triage Board / Explorer Tree / Auto Testing) and, when THIS window
    // hosts the session's tab, retitle the tab strip to match. A session hosted in ANOTHER window
    // is retitled there by that window's registry observer (_SyncClaudeTabTitleFromRegistry rides
    // the tab-dot push), so the rename lands on the right tab no matter which window ran it. The
    // tab's own rename path mirrors the other direction (_SyncClaudeTitleFromTab); the SetTabText
    // below re-enters it once and settles immediately (the registry title already equals the new
    // text).
    void TerminalPage::_RenameClaudeSession(winrt::hstring sessionId, winrt::hstring title)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const std::wstring id{ sessionId };
        const std::wstring name{ title };
        ::Agentmaster::LogNav(L"rename " + ::Agentmaster::ShortId(id) + L" -> \"" + name.substr(0, 80) + L"\"");
        _sessionRegistry->Update(id, [&name](::Agentmaster::SessionInfo& s) { s.title = name; });

        const auto it = _claudeTabs.find(id);
        if (const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                _SetClaudeTabTextPinned(impl, title); // pinned: don't bounce back into the tab->registry mirror
            }
        }
    }

    // Agentmaster: the reverse direction — a Claude TAB was renamed (the header double-click, the
    // right-click "Rename Tab", or the renameTab action; all funnel through Tab::SetTabText ->
    // PropertyChanged("Title") -> _UpdateTitle -> here). Mirror the tab's text into the session's
    // title so the Explorer-tree name and the persisted record track the tab. Because every Claude
    // tab is pinned to its managed name at launch, a non-empty runtime tab text that differs from
    // the registry title can only be a user rename; an emptied one (a bare renameTab / ResetTabText)
    // is re-pinned so the tab never falls back to claude's volatile OSC title and diverges.
    void TerminalPage::_SyncClaudeTitleFromTab(const TerminalApp::Tab& tab)
    {
        if (!_sessionRegistry || _pinningClaudeTabTitle)
        {
            // _pinningClaudeTabTitle: WE are pinning this tab's title from the registry side
            // (rename / restore / bind / cross-window push). Tab::SetTabText re-enters us
            // synchronously; suppressing the write-back here is what stops the registry<->tab
            // ping-pong (the /clear title-swap + [Unknown] flood). A genuine USER tab rename
            // arrives with the latch CLEAR and still mirrors into the registry below.
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // not a Claude session tab (or not yet mapped) -> nothing to sync
        }
        const auto impl = _GetTabImpl(tab);
        if (!impl)
        {
            return;
        }
        // Trim surrounding whitespace/newlines, MATCHING the Manager's _CommitRename so both rename
        // entry points (the WT tab header / renameTab action here; the Explorer-tree editor there)
        // store the same normalized title. (Internal newlines — paste only — are left as-is, exactly
        // like _CommitRename; the Manager collapses them for display via OneLine.)
        const std::wstring raw{ impl->GetTabText() };
        const auto firstNonWs = raw.find_first_not_of(L" \t\r\n");
        const std::wstring text = (firstNonWs == std::wstring::npos) ?
                                      std::wstring{} :
                                      raw.substr(firstNonWs, raw.find_last_not_of(L" \t\r\n") - firstNonWs + 1);
        const auto info = _sessionRegistry->Get(id);
        if (text.empty())
        {
            // Override cleared (ResetTabText) OR a blank / whitespace-only rename -> re-pin the managed
            // name; never let a session's title go empty (Rule #11). Pinned, so the re-entrant
            // _SyncClaudeTitleFromTab is a no-op (the latch), never a write-back.
            if (info && !info->title.empty())
            {
                _SetClaudeTabTextPinned(impl, winrt::hstring{ info->title });
            }
            return;
        }
        // Persist the trimmed title only when it actually changed (a no-op Update would still churn the
        // observer / persist / UI). A null info (no record yet) makes the Update a no-op anyway.
        if (!info || info->title != text)
        {
            // Nav audit: a genuine USER rename via the TAB STRIP (double-click header / "Rename Tab" /
            // renameTab action) — the other rename entry point beside the Explorer/Manager _RenameClaudeSession.
            // Only the real change is logged (the latch + this equality guard drop programmatic re-pins).
            ::Agentmaster::LogNav(L"rename " + ::Agentmaster::ShortId(id) + L" -> \"" + text.substr(0, 80) + L"\" (tab strip)");
            _sessionRegistry->Update(id, [&text](::Agentmaster::SessionInfo& s) { s.title = text; });
        }
        // If the user typed surrounding whitespace, normalize the tab strip to the trimmed value we
        // stored, so the tab and the Explorer/board name agree (one-title rule). Pinned -> no loop.
        if (raw != text)
        {
            _SetClaudeTabTextPinned(impl, winrt::hstring{ text });
        }
    }

    // Agentmaster (cross-window rename; Rule #11): the registry-observer reaction (bounced to this
    // window's UI thread by the engine-init observer, riding the same hop as the tab dot). When the
    // session's ONE title changes anywhere — an Explorer-tree/board-card rename in ANOTHER window,
    // a restore, the smart-naming, a bind/re-home — the window actually HOSTING the tab re-pins it
    // here. A session this window doesn't host is a cheap map-miss no-op (every window's observer
    // sees every fleet event).
    //
    // The title is read FRESH from the registry, never taken from the observer callback (which
    // captured it at notify-time and may be stale by the time this coalesced UI-thread hop runs).
    // Applying a stale value is what seeded the registry<->tab ping-pong on /clear: the re-home
    // wrote the title twice in quick succession, so two callbacks each carried a different captured
    // title and kept reverting one another. Reading fresh + pinning through _SetClaudeTabTextPinned
    // (the latch, so the synchronous _SyncClaudeTitleFromTab re-entry never writes back) makes every
    // queued push converge on the same latest value and stop.
    void TerminalPage::_SyncClaudeTabTitleFromRegistry(const std::wstring& sessionId)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const auto it = _claudeTabs.find(sessionId);
        const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr;
        if (!tab)
        {
            return; // not hosted here — every window's observer fires for every fleet event
        }
        const auto info = _sessionRegistry->Get(sessionId);
        if (!info || info->title.empty())
        {
            return; // never clear a pinned tab title from the registry side (the pin re-asserts on empty)
        }
        if (const auto impl = _GetTabImpl(tab))
        {
            if (std::wstring{ impl->GetTabText() } != info->title)
            {
                _SetClaudeTabTextPinned(impl, winrt::hstring{ info->title });
            }
        }
    }

    // Agentmaster: paint a Claude tab from its working directory's PERMANENT color — the persisted
    // color for the dir (user pick OR a previously-dealt auto color), else a fresh collision-free auto
    // color claimed from the dir's seeded probe sequence (avoiding colors other folders hold; on a full
    // palette it resets + reuses, avoiding colors open tabs are actively showing) which AssignDirAutoColor
    // then PERSISTS — so the folder keeps the same color across tabs/windows/restarts (Rule #12).
    // SetRuntimeTabColor re-enters _OnClaudeTabColorChanged once, which settles immediately (the color is
    // already the dir's persisted color, so the de-dupe no-ops), so this never loops.
    void TerminalPage::_ApplyDirColorToTab(const TerminalApp::Tab& tab, const std::wstring& dir)
    {
        auto hex = ::Agentmaster::GetDirColor(dir);
        if (!hex)
        {
            // The currently-open dirs (across all windows — the registry is process-wide) whose colors
            // an auto pick must avoid. Archived (non-live) sessions don't show, so they don't count.
            // Mode-aware key (tab color modes): an INFERRING open session (the Inferred mode, or a
            // home-dir launch in any mode — SessionInfersWorkingDir) "shows"
            // its INFERRED dir's color, so that is the key the avoid-set must carry (SessionColorKeyDir
            // == plain workingDir for the deliberately-chosen cwds of the default mode).
            std::vector<std::wstring> openDirKeys;
            if (_sessionRegistry)
            {
                for (const auto& s : _sessionRegistry->Snapshot())
                {
                    if (s.live)
                    {
                        openDirKeys.push_back(::Agentmaster::NormDirKey(::Agentmaster::SessionColorKeyDir(_appSettings.tabColorMode, s)));
                    }
                }
            }
            hex = ::Agentmaster::AssignDirAutoColor(dir, openDirKeys); // collision-free; not persisted
        }
        if (const auto color = ClaudeHexToColor(*hex))
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetRuntimeTabColor(*color);
            }
        }
    }

    // Agentmaster (tab color modes): THE mode-aware paint seam — every managed-tab launch / restore /
    // bind routes through here instead of calling _ApplyDirColorToTab directly, so the ONE global
    // AppSettings::tabColorMode decides how the tab is colored:
    //   * WorkingDirectory (default) — the classic Rule-#12 per-dir paint, verbatim.
    //   * InferredWorkingDirectory — the same per-dir machinery, but KEYED by the session's inferred
    //     working dir once one exists (SessionColorKeyDir; until then the launch cwd — identical to
    //     the default mode, so a fresh session never flashes an interim color). A session LAUNCHED
    //     in the user's home dir (%USERPROFILE% — the default launch dir) gets this inferred keying
    //     in EVERY dir-keyed mode, not just this one (the SessionInfersWorkingDir forcing: its cwd
    //     is meaningless, so it keys by where it actually works).
    //   * Individual — the session's OWN persisted color (SessionInfo::tabColorHex); a session with
    //     none yet is DEALT one collision-free against the other OPEN sessions' colors
    //     (ChooseSessionAutoColor) and the pick is persisted on the record FIRST (registry Update ->
    //     sessions.json autosave) so the synchronous _OnClaudeTabColorChanged re-entry the paint
    //     triggers sees it already stored and no-ops (the same settle-immediately contract the dir
    //     paint has vs GetDirColor).
    //   * NoColor ("Remove colors") — the tab wears NOTHING: any painted runtime color is RESET and
    //     "Change tab color" is disabled on the tab (SetColorPickerEnabled). The persisted colors
    //     (dir-colors.json / tabColorHex) are deliberately untouched — the reset's synchronous
    //     _OnClaudeTabColorChanged re-entry is swallowed by that handler's NoColor guard, so nothing
    //     is dropped and switching back to a colored mode restores exactly the prior colors.
    // Never called for the Manager tab (its color is per-window, in the WindowRecord).
    void TerminalPage::_ApplySessionTabColor(const TerminalApp::Tab& tab, const std::wstring& sessionId, const std::wstring& dir)
    {
        if (!tab)
        {
            return;
        }
        const auto mode = _appSettings.tabColorMode;
        if (const auto impl = _GetTabImpl(tab))
        {
            // The color picker follows the mode on every (re)paint — disabled under NoColor, re-armed
            // the moment a colored mode repaints the fleet (_ReapplyManagedTabColors on cog Save /
            // cross-window broadcast). Managed tabs only — shell tabs never route through here.
            impl->SetColorPickerEnabled(mode != ::Agentmaster::TabColorMode::NoColor);
            if (mode == ::Agentmaster::TabColorMode::NoColor)
            {
                // A MANAGED tab's color is mode-derived (dir-colors.json / tabColorHex — never
                // lost), so it is plain-RESET, not parked. Drop any stale PARKED color first — a
                // tab suspended as a SHELL tab that later became managed (a '+'-tab claude bound
                // mid-mode) must not resurrect that ancient park in a future persistence fold
                // (GetPersistableTabColor / BuildStartupActions prefer runtime, but a later NoColor
                // reset would empty runtime and expose it). The restore-then-reset order nets
                // colorless with the park cleared; the reset's TabColorChanged re-entry parks
                // nothing (runtime already empty when it fires).
                impl->SetTabColorSuspended(false);
                impl->ResetRuntimeTabColor(); // shed any painted color; persisted colors stay (the chokepoint guard)
                return;
            }
        }
        if (mode == ::Agentmaster::TabColorMode::Individual && _sessionRegistry)
        {
            const auto info = _sessionRegistry->Get(sessionId);
            std::wstring hex = info ? info->tabColorHex : std::wstring{};
            if (hex.empty())
            {
                // Deal this session its own color: avoid every color another OPEN session already
                // wears (both as "assigned" and as "actively shown" — for sessions the two sets
                // coincide, unlike folders, where closed dirs keep palette slots).
                std::vector<std::pair<std::wstring, std::wstring>> liveColors;
                std::unordered_set<std::wstring> activeColors;
                for (const auto& s : _sessionRegistry->Snapshot())
                {
                    if (s.live && s.id != sessionId && !s.tabColorHex.empty())
                    {
                        liveColors.emplace_back(s.id, s.tabColorHex);
                        activeColors.insert(s.tabColorHex);
                    }
                }
                hex = ::Agentmaster::ChooseSessionAutoColor(sessionId, liveColors, activeColors);
                // Persist BEFORE painting: SetRuntimeTabColor synchronously re-enters
                // _OnClaudeTabColorChanged, whose Individual branch compares against the stored
                // tabColorHex — already equal, so it settles without a spurious "user pick" write.
                _sessionRegistry->Update(sessionId, [&hex](::Agentmaster::SessionInfo& s) { s.tabColorHex = hex; });
            }
            if (const auto color = ClaudeHexToColor(hex))
            {
                if (const auto impl = _GetTabImpl(tab))
                {
                    impl->SetRuntimeTabColor(*color);
                }
            }
            return;
        }
        // Dir-keyed modes: the classic paint, keyed by the mode's color-key dir — the cwd, or the
        // inferred dir while the session INFERS (the Inferred mode, or a HOME-DIR launch in any mode,
        // the SessionInfersWorkingDir forcing), then canonicalized so a git WORKTREE keys its MAIN
        // repo's color. Route through SessionColorKeyDir — the SAME resolver the avoid-set/fan-out
        // and ResolveSessionColorHex's board/chip/pending surfaces use — so the painted tab and its
        // cards can never disagree. No registry/info (rare) falls back to the raw dir passed in.
        std::wstring keyDir = dir;
        if (_sessionRegistry)
        {
            if (const auto info = _sessionRegistry->Get(sessionId))
            {
                keyDir = ::Agentmaster::SessionColorKeyDir(mode, *info);
            }
        }
        _ApplyDirColorToTab(tab, keyDir);
    }

    // Agentmaster (tab color modes): repaint every managed tab THIS window hosts per the CURRENT
    // AppSettings::tabColorMode — the live-apply half of the cog's "Tab coloring" dropdown (called
    // from the settings Save handler + _ApplyBroadcastSettings, so a mode change recolors every
    // window's tabs immediately, the _RefreshFlashRingBrush idiom). Each paint settles: the mode's
    // color is already persisted (or is dealt+persisted first), so the synchronous
    // _OnClaudeTabColorChanged re-entries all no-op. Board cards / Sessions chips re-resolve on
    // their own next rebuild (ApplyExternalSettings ends in _Refresh()).
    // NoColor ("Remove colors") is STRIP-WIDE, so a sweep over ALL tabs runs first: "no tab is
    // colored" includes the tabs the managed loop can't see — an ex-claude pwsh tab still wearing
    // the dir paint its exited session left (archived sessions leave _claudeTabs, the tab lives
    // on), a user-colored shell tab, a restored shell tab whose actionsJson replayed a setColor,
    // and the pinned Manager tab (its per-window record color). Entering the mode SUSPENDS each
    // one's runtime color (parked on the tab — persistence still records it via
    // GetPersistableTabColor / BuildStartupActions, nothing voided) and disables the color picker
    // on every tab; leaving restores every parked color and re-arms the picker. Managed session
    // tabs are deliberately NOT parked — their color is MODE-DERIVED (dir-colors.json /
    // tabColorHex, never lost), so the mode dispatch below resets/repaints them instead.
    void TerminalPage::_ReapplyManagedTabColors()
    {
        const bool noColor = _appSettings.tabColorMode == ::Agentmaster::TabColorMode::NoColor;
        for (const auto& tab : _tabs)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetColorPickerEnabled(!noColor);
                if (_ClaudeSessionForTab(tab).empty()) // non-managed only; managed tabs repaint below
                {
                    impl->SetTabColorSuspended(noColor);
                }
            }
        }
        if (!_sessionRegistry || _claudeTabs.empty())
        {
            return;
        }
        std::vector<std::wstring> ids;
        ids.reserve(_claudeTabs.size());
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            ids.push_back(id);
        }
        for (const auto& id : ids)
        {
            const auto info = _sessionRegistry->Get(id);
            if (!info)
            {
                continue;
            }
            TerminalApp::Tab tab{ nullptr };
            if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
            {
                tab = it->second.get();
            }
            if (tab)
            {
                _ApplySessionTabColor(tab, id, info->workingDir);
            }
            // Mirror the (possibly changed) mode into the session's linked overlay too — its subline /
            // Open Path / Copy Path resolve the EFFECTIVE work dir through it, so a mode flip re-renders
            // the badge onto the same dir story the tab color just repainted to (a no-op when unchanged).
            // The model-family list rides the same broadcast (row 1's model shortener; a no-op when
            // unchanged, like the mode).
            if (const auto ovIt = _claudeOverlays.find(id); ovIt != _claudeOverlays.end() && ovIt->second)
            {
                ovIt->second->SetTabColorMode(static_cast<int>(_appSettings.tabColorMode));
                ovIt->second->SetModelFamilies(_appSettings.modelFamilies);
            }
        }
    }

    // Agentmaster: recolor every live Claude tab whose session shares `dir` (filesystem-aware match)
    // to `colorHex`, or reset them when nullopt. Fans a user's color change across the directory.
    // Mode-aware (tab color modes): a session matches by its color-KEY dir — the plain workingDir in
    // the default mode, the INFERRED dir while the session infers (the Inferred mode, or a home-dir
    // launch in any mode — SessionInfersWorkingDir) —
    // so a user pick fans out to exactly the tabs that genuinely share the picked color's key. Never
    // called in Individual mode (its _OnClaudeTabColorChanged branch has no fan-out).
    void TerminalPage::_ApplyDirColorToTabs(const std::wstring& dir, const std::optional<std::wstring>& colorHex)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        std::optional<winrt::Windows::UI::Color> color;
        if (colorHex)
        {
            color = ClaudeHexToColor(*colorHex);
        }
        const std::wstring key = ::Agentmaster::NormDirKey(dir);
        for (const auto& [sid, weakTab] : _claudeTabs)
        {
            const auto info = _sessionRegistry->Get(sid);
            if (!info || ::Agentmaster::NormDirKey(::Agentmaster::SessionColorKeyDir(_appSettings.tabColorMode, *info)) != key)
            {
                continue;
            }
            if (const auto t = weakTab.get())
            {
                if (const auto impl = _GetTabImpl(t))
                {
                    if (color)
                    {
                        impl->SetRuntimeTabColor(*color);
                    }
                    else
                    {
                        impl->ResetRuntimeTabColor();
                    }
                }
            }
        }
    }

    // Agentmaster: the user changed a Claude tab's color (color picker / setTabColor action ->
    // Tab::SetRuntimeTabColor/Reset -> TabColorChanged -> here). Color is ONE value per working
    // directory: persist it for the dir (permanently) and fan it out to every live tab in that dir.
    // The de-dupe vs the persisted color makes our OWN writes no-ops (no loop): both a user pick's
    // fan-out and our launch-time auto paint already match the persisted color (AssignDirAutoColor
    // persisted it), so only a genuine NEW user pick gets through to a re-persist + re-fan.
    void TerminalPage::_OnClaudeTabColorChanged(const TerminalApp::Tab& tab)
    {
        // Tab color modes — NoColor ("Remove colors"): NO tab wears a color — not just managed
        // ones — and this event (wired for EVERY tab) is the one chokepoint all color writes
        // funnel through. Whatever just landed on ANY tab — a restored shell tab's replayed
        // setColor action, the Manager tab's record re-tint at claim, a stray setTabColor
        // keybinding, a torn-out tab recreated in this window — is immediately SUSPENDED: the
        // visual is shed while the value stays PARKED on the tab (Tab::_suspendedTabColor), so
        // persistence — a shell tab's actionsJson via BuildStartupActions' fold, the Manager color
        // via GetPersistableTabColor — still records it and leaving the mode restores it. Nothing
        // is read from or written to dir-colors.json / tabColorHex here, so the one-color-per-key
        // maps survive the mode verbatim (the whole point — switching back restores exactly the
        // prior colors). Loop-safe: SetTabColorSuspended raises no TabColorChanged, and
        // re-suspending a colorless/parked tab is a no-op (our own ResetRuntimeTabColor repaints
        // land here with nothing left to park). The Manager tab keeps its save-on-change semantics
        // — the capture persists the PARKED value, exactly what it showed before the mode.
        if (_appSettings.tabColorMode == ::Agentmaster::TabColorMode::NoColor)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetTabColorSuspended(true);
            }
            if (_managerTab && tab == _managerTab)
            {
                _ScheduleWindowRecordSave();
            }
            return;
        }
        // Agentmaster: the pinned Manager tab persists its color PER WINDOW (in the window record), not in
        // the dir-color map — it is a per-window singleton with no working dir (Rule #12 is dir-keyed). A
        // color change on it just schedules a window-record save (the capture reads the live color); there
        // is no dir to fan the color out to. Handled before the session-tab path (which would early-return
        // anyway, since the Manager tab has no _ClaudeSessionForTab id).
        if (_managerTab && tab == _managerTab)
        {
            _ScheduleWindowRecordSave();
            return;
        }
        if (!_sessionRegistry)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // not a Claude session tab
        }
        const auto info = _sessionRegistry->Get(id);
        if (!info)
        {
            return;
        }

        std::optional<std::wstring> newHex;
        if (const auto impl = _GetTabImpl(tab))
        {
            if (const auto c = impl->GetRuntimeTabColor())
            {
                newHex = ClaudeColorToHex(*c);
            }
        }
        // Tab color modes — Individual: the pick belongs to THIS session alone. Persist it on the
        // session record (sessions.json via the registry autosave) and fan out to NOTHING; a reset
        // (nullopt) clears the stored color so the next launch deals a fresh one (the per-session
        // analog of SetDirColor's drop-on-reset). The equality guard makes our OWN paints no-ops —
        // _ApplySessionTabColor persists the deal BEFORE painting — so only a genuine user pick lands.
        if (_appSettings.tabColorMode == ::Agentmaster::TabColorMode::Individual)
        {
            const std::wstring stored = info->tabColorHex;
            const std::wstring picked = newHex ? *newHex : std::wstring{};
            if (stored == picked)
            {
                return; // already this session's persisted color (our paint / a settled pick) -> no loop
            }
            // Nav audit: a genuine user pick/reset (the equality guard above filtered our own paints).
            ::Agentmaster::LogNav(L"tab-color " + ::Agentmaster::ShortId(id) + L" -> " + (picked.empty() ? std::wstring{ L"reset" } : picked) + L" (individual)");
            _sessionRegistry->Update(id, [&picked](::Agentmaster::SessionInfo& s) { s.tabColorHex = picked; });
            _ScheduleWindowRecordSave(); // M10: the per-tab color rides in the window record
            return;
        }
        // Dir-keyed modes: color is ONE value per KEY dir — the working dir (default), or the
        // session's inferred dir while it infers (the Inferred mode, or a home-dir launch in any
        // mode — SessionColorKeyDir via SessionInfersWorkingDir) — persisted in
        // dir-colors.json and fanned out to every live tab sharing that key.
        const std::wstring dir = ::Agentmaster::SessionColorKeyDir(_appSettings.tabColorMode, *info);
        if (::Agentmaster::GetDirColor(dir) == newHex)
        {
            return; // already the dir's persisted color (our auto paint / user-pick fan-out / reset) -> no loop
        }
        // Nav audit: a genuine user pick/reset for this color-key dir (the equality guard above
        // filtered our auto paints / fan-out echoes) — it persists + recolors every same-dir tab.
        ::Agentmaster::LogNav(L"tab-color dir=" + dir + L" -> " + (newHex ? *newHex : std::wstring{ L"reset" }));
        ::Agentmaster::SetDirColor(dir, newHex); // upsert the color, or drop it on reset
        _ApplyDirColorToTabs(dir, newHex); // every live tab sharing this color key tracks the change
        _ScheduleWindowRecordSave(); // M10: the per-tab color rides in the window record
    }
}
