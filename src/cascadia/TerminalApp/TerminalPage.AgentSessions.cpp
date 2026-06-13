// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
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

#include "AgentStatusColors.h" // AgentStatusColorFor — the shared state->color palette (tab dot)
#include "AgentTabOverlay.h" // _claudeOverlays.erase needs the complete com_ptr<AgentTabOverlay> type
#include "AgentMaster/ClaudeSpawn.h" // BuildClaudeSpawn / ClaudeConversationExists / AppendStateLog
#include "AgentMaster/Engine.h" // SharedEngine (AM_SESSION stamp; restoreMutex barrier)
#include "AgentMaster/HooksBridge.h" // PipeName for the spawn spec
#include "AgentMaster/Persistence.h" // DeriveSessionTitle / Save-LoadSessions / LoadAppSettings / dir colors
#include "AgentMaster/ProcessInspect.h" // ProcessAlive / ProcessStartUnixMs / ResolveSessionId
#include "AgentMaster/SessionRegistry.h"

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

    // Agentmaster: launch a fresh Claude session (the Manager's "Launch session").
    void TerminalPage::_SpawnClaudeSession(winrt::hstring workingDir, winrt::hstring title)
    {
        _LaunchClaudeSession(workingDir, title, std::nullopt);
    }

    // Agentmaster: launch a claude.exe on a ConPTY in `workingDir`, wired for hooks, as a
    // normal terminal tab, and register it so its hook-driven state is tracked. Both the
    // user (keystrokes) and the orchestrator (Autopilot) write the same stdin.
    //
    // If `restored` is set, this RESUMES that conversation (claude --resume <id>) and
    // restores its Flight Plan + autopilot from persistence (DESIGN §13) — so closing and
    // reopening the app brings the session back exactly as it was. `Sent` prompts are kept
    // Sent (never replayed, Correctness Rule #4).
    TerminalApp::Tab TerminalPage::_LaunchClaudeSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored, const std::wstring& forkFromId)
    {
        if (!_sessionRegistry || !_hooksBridge)
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
            // Smart tab/session name: walk past generic bin/obj/Debug/... segments to the first
            // meaningful folder, then apply the length/case rules (see DeriveSessionTitle).
            ttl = ::Agentmaster::DeriveSessionTitle(dir);
        }

        // Resume ONLY if Claude actually has a saved conversation for this id. A session that
        // was opened but never received a prompt has no transcript, so `claude --resume <id>`
        // would fail with "No conversation found" and the tab would die (exit code 1). Such a
        // session is re-launched fresh instead — keeping its working dir + Flight Plan, with a
        // new conversation id. (Correctness Rule #6: restore == resume, never replay.)
        const bool wantResume = restored && !restored->id.empty() && ::Agentmaster::ClaudeConversationExists(restored->id);
        const std::wstring resumeId = wantResume ? restored->id : std::wstring{};
        // forkFromId set (duplicate-tab -> fork) overrides resume/fresh: BuildClaudeSpawn mints a NEW id
        // and the commandline forks the source conversation into it (the source transcript is untouched).
        const auto spec = ::Agentmaster::BuildClaudeSpawn(dir, ttl, _hooksBridge->PipeName(), resumeId, ::Agentmaster::LoadAppSettings(), forkFromId);

        // Child environment: CCMGR_SESSION_ID + CCMGR_HOOK_PIPE so hook events correlate
        // back to this session's registry record (HOOKS.md).
        auto envMap = winrt::single_threaded_map<winrt::hstring, winrt::hstring>();
        for (const auto& [k, v] : spec.env)
        {
            envMap.Insert(winrt::hstring{ k }, winrt::hstring{ v });
        }
        // Fleet Observer (§19-Q1): stamp this Launched claude with AM_SESSION = "<processGuid>:<windowId>"
        // so the observer attributes it to THIS window (the inherited process env already carries the
        // bare "<processGuid>"; this overrides it for the child with the owning window appended).
        // ClassifyRunningApp matches on the GUID prefix, so both forms still read as ours.
        if (const auto& eng = ::Agentmaster::SharedEngine(); !eng.amSession.empty() && !_windowId.empty())
        {
            envMap.Insert(winrt::hstring{ L"AM_SESSION" }, winrt::hstring{ eng.amSession + L":" + _windowId });
        }

        // Build the ConPTY connection ourselves (commandline = claude + our hooks settings),
        // then hand it to the normal terminal-pane path as an existing connection. The
        // default profile only supplies appearance; the process/cwd/env are ours.
        auto valueSet = TerminalConnection::ConptyConnection::CreateSettings(
            winrt::hstring{ spec.commandline },
            winrt::hstring{ dir },
            winrt::hstring{ ttl },
            false, // reloadEnvironmentVariables
            L"", // initialEnvironment: inherit our current block
            envMap.GetView(),
            30, // rows  (the control resizes the pty to the pane on first layout)
            120, // cols
            winrt::guid{}, // WT_SESSION (auto-generated by the connection)
            winrt::guid{}); // profileGuid

        TerminalConnection::ConptyConnection connection{};
        connection.Initialize(valueSet);

        Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs{};
        const auto pane = _MakePane(newTerminalArgs, winrt::TerminalApp::Tab{ nullptr }, connection);
        if (!pane)
        {
            return nullptr;
        }

        // Agentmaster: suppress the profile's closeOnExit auto-close for this Claude pane.
        // When claude.exe exits (Ctrl+C, natural completion, crash), the pane must NOT
        // auto-close: _SweepClaudeLiveness intentionally leaves dead tabs open for the user
        // to read while archiving the session. Without this, a graceful exit fires
        // CloseRequested → Pane::Close() → Tab::Closed → _RemoveTab, bypassing the design.
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

        const auto tab = _CreateNewTabFromPane(pane);
        if (tab)
        {
            // Map sessionId -> tab so the Manager can Activate (jump) / Kill it.
            _claudeTabs[spec.sessionId] = winrt::make_weak(tab);
        }

        // Register the session (restoring its queue + autopilot if resuming) and bind its
        // stdin injector. Correctness Rule #3: the injector is bound to THIS session's id.
        ::Agentmaster::SessionInfo info = restored ? *restored : ::Agentmaster::SessionInfo{};
        info.id = spec.sessionId;
        info.title = ttl;
        info.workingDir = dir;
        info.state = ::Agentmaster::SessionState::Idle; // hooks re-establish the real state
        info.external = false; // we own this tab's ConPTY -> managed, not an adopted session
        info.live = true; // OPEN: has a live tab/claude now -> shows on the Triage Board (not Archived)
        info.pendingConfirmPromptId.clear();
        if (!restored)
        {
            // A NEW session inherits the global Autopilot defaults from the cog; a restored one
            // keeps its persisted AutopilotState (Correctness Rule #6).
            info.autopilot.mode = _appSettings.defaultAutopilotMode;
            info.autopilot.maxAutoSends = _appSettings.maxAutoSends;
            info.autopilot.stopOnError = _appSettings.stopOnError;
            info.autopilot.pauseOnHumanInput = _appSettings.pauseOnHumanInput;
        }
        _sessionRegistry->Upsert(info);

        // Restoring an archived session whose transcript no longer exists yields a FRESH id
        // (restore-fresh). The new (live) record above replaces nothing, so drop the stale
        // archived record under the old id — otherwise it would linger in the Archived list.
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
                impl->SetTabText(winrt::hstring{ ttl });
            }
            // Per-directory tab color: the dir's persisted color, or a stable auto-assigned one.
            _ApplyDirColorToTab(tab, dir);
            // Per-tab "link badge" overlay (TAB_OVERLAY.md): top-right status HUD for this session.
            _AttachClaudeOverlay(tab, spec.sessionId);
            // Tab status dot: seed the strip dot from the (just-upserted) session's state — Idle
            // gray for a fresh launch/restore; the registry observer recolors it live from here.
            if (const auto s = _sessionRegistry->Get(spec.sessionId))
            {
                _SetTabAgentDot(tab, AgentStatusColorFor(s->state));
            }
        }

        const std::wstring tag = !forkFromId.empty() ? L"[fork] " : (wantResume ? L"[resume] " : (restored ? L"[restore-fresh] " : L"[spawn] "));
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      tag + spec.sessionId + (forkFromId.empty() ? L"" : (L" (forked from " + forkFromId + L")")) + L" \"" + ttl + L"\" cwd=" + dir + L"\n");
        return tab;
    }

    // Agentmaster: on startup, load every persisted session into the registry as ARCHIVED
    // (live=false) — and do NOT auto-launch any of them. This is the deliberate reversal of the
    // old "close == reopen" auto-relaunch (Correctness Rule #6): the app opens to just the
    // Manager tab, the prior fleet arrives Archived (restorable as a whole), and the user
    // re-opens what they want from the Manager's "Archived" button (which resumes via
    // `claude --resume`, transcript-gated, in _RestoreArchivedSession). No tabs are created
    // here, so there is nothing to lay out / yield for — the body runs synchronously.
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
                    // autopilot intact (Rule #6: restore == resume, never replay; Sent stays Sent).
                    s.live = false;
                    s.external = false;
                    s.state = ::Agentmaster::SessionState::Idle;
                    s.lastMessageWasQuestion = false;
                    s.pendingConfirmPromptId.clear();
                    _sessionRegistry->Upsert(std::move(s));
                }
                eng.restored = true; // publish ONLY after the registry is fully populated (still under the lock)
            }
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
        if (_FocusClaudeSessionTab(id, /*bringWindowToFront*/ false))
        {
            return;
        }
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

    // Agentmaster: archive a session (the Manager's Delete / Archive / tree Del / Flight-Plan
    // "Archive"). In the lifecycle model "Delete" == archive: it routes through the SAME
    // tab-close seam as clicking the tab's X, so the one consequence confirm + archive
    // bookkeeping (in _HandleCloseTabRequested -> _ArchiveAndCloseClaudeTab) applies uniformly.
    // The session record is KEPT (live=false) so it persists and lists under "Archived".
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

    // Agentmaster: the shared archive seam — confirm the consequence (unless suppressed), do the
    // archive bookkeeping, then close the tab. KEEPING the registry record (live=false) is what
    // makes archive reversible: the session persists and lists under the Manager's "Archived"
    // button for restore. (The old behavior — Remove from the registry + sessions.json — was the
    // irreversible discard the lifecycle model drops: archive is terminal, nothing is forgotten,
    // and the Claude transcript on disk is never touched either.)
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
                ContentDialog dialog;
                dialog.Title(winrt::box_value(L"Archive session?"));
                dialog.Content(winrt::box_value(title.empty() ?
                                                    winrt::hstring{ L"This shuts the session down and moves it to Archived. You can restore it (with its Flight Plan) anytime from the Manager’s “Archived” button." } :
                                                    winrt::hstring{ L"“" + title + L"” will be shut down and moved to Archived. You can restore it (with its Flight Plan) anytime from the Manager’s “Archived” button." }));
                dialog.PrimaryButtonText(L"Archive");
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel

                const auto weak = get_weak();
                const auto result = co_await presenter.ShowDialog(dialog);
                const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
                if (!strong)
                {
                    co_return;
                }
                if (result != ContentDialogResult::Primary)
                {
                    co_return; // cancelled -> leave the session Open
                }
            }
            // No presenter to confirm with -> archive anyway (it's reversible; don't strand the close).
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
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[archive] " + sessionId + L"\n");

        tab.Close(); // -> Closed -> _RemoveTab (tab.Shutdown disconnects -> claude.exe exits)
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

    // Agentmaster: re-launch (resume) an archived session from the Manager's "Archived" list.
    // The record is still in the registry (live=false); _LaunchClaudeSession resumes it
    // (claude --resume <id>, transcript-gated) with its Flight Plan + autopilot, and flips it
    // back to live (Open). A missing transcript yields a fresh id; _LaunchClaudeSession then
    // drops the stale archived record so it doesn't linger in the list.
    void TerminalPage::_RestoreArchivedSession(winrt::hstring sessionId)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const auto info = _sessionRegistry->Get(std::wstring{ sessionId });
        if (!info || info->live)
        {
            return; // unknown, or already Open
        }
        // Kind-aware restore: a Codex record resumes via `codex resume <uuid>` (gated on its rollout),
        // a Claude record via `claude --resume <id>`. Both fall back to fresh when the transcript is gone.
        if (info->kind == ::Agentmaster::AgentKind::Codex)
        {
            _LaunchCodexSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
        }
        else
        {
            _LaunchClaudeSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
        }
    }

    // Agentmaster (Codex managed-session support): launch a codex.exe on a ConPTY as a MANAGED tab,
    // on the SAME path as _LaunchClaudeSession. The divergences are all Codex-inherent:
    //   * No --session-id: OUR minted `handleId` is the durable registry/persistence/tab-map key (id's
    //     role for Claude); the REAL rollout uuid (the `codex resume` target) is carried on
    //     SessionInfo.codexSessionId and FILLED by the Fleet Observer once the rollout resolves.
    //   * No hooks / no --settings: bare `codex` (config.toml governs); state comes from the C2
    //     rollout-tail (mapped onto SessionState by _ReconcileManagedCodex each probe).
    //   * Lifecycle + state only — NO stdin injector / Autopilot (driving the Codex TUI is a later
    //     phase). The user types directly into the tab's ConPTY; Autopilot is forced Off.
    // `restored` set => RESUME that conversation (reusing its handle) with its rollout uuid, gated on
    // the rollout still existing; else a fresh codex (a new rollout the observer will resolve).
    void TerminalPage::_SpawnCodexSession(winrt::hstring workingDir, winrt::hstring title)
    {
        _LaunchCodexSession(workingDir, title, std::nullopt);
    }

    TerminalApp::Tab TerminalPage::_LaunchCodexSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored)
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

        // Resume ONLY if Codex still has a rollout for the persisted uuid (the resume target). No uuid
        // (a never-prompted archived codex) or the rollout vanished -> launch fresh in the dir (Codex
        // mints a new rollout; the observer re-resolves codexSessionId). Mirrors Claude's transcript-
        // gated resume (Rule #6).
        std::wstring resumeUuid;
        if (restored && !restored->codexSessionId.empty())
        {
            if (!::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), restored->codexSessionId).empty())
            {
                resumeUuid = restored->codexSessionId;
            }
        }
        const std::wstring commandline = ::Agentmaster::BuildCodexCommandline(resumeUuid);

        // Child env: NO CCMGR_* (Codex has no hook bridge). Stamp AM_SESSION so the Fleet Observer
        // attributes this codex to THIS window (ownership) — same as a launched claude.
        auto envMap = winrt::single_threaded_map<winrt::hstring, winrt::hstring>();
        if (const auto& eng = ::Agentmaster::SharedEngine(); !eng.amSession.empty() && !_windowId.empty())
        {
            envMap.Insert(winrt::hstring{ L"AM_SESSION" }, winrt::hstring{ eng.amSession + L":" + _windowId });
        }
        for (auto& kv : ::Agentmaster::ParseEnvAssignments(::Agentmaster::LoadAppSettings().env))
        {
            if (kv.first.rfind(L"CCMGR_", 0) == 0)
            {
                continue;
            }
            envMap.Insert(winrt::hstring{ kv.first }, winrt::hstring{ kv.second });
        }

        auto valueSet = TerminalConnection::ConptyConnection::CreateSettings(
            winrt::hstring{ commandline }, winrt::hstring{ dir }, winrt::hstring{ ttl },
            false, L"", envMap.GetView(), 30, 120, winrt::guid{}, winrt::guid{});
        TerminalConnection::ConptyConnection connection{};
        connection.Initialize(valueSet);

        Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs{};
        const auto pane = _MakePane(newTerminalArgs, winrt::TerminalApp::Tab{ nullptr }, connection);
        if (!pane)
        {
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

        const auto tab = _CreateNewTabFromPane(pane);
        if (tab)
        {
            _claudeTabs[handleId] = winrt::make_weak(tab); // the tab map is agent-agnostic (Activate / Archive / capture)
        }

        // Register the managed Codex record (kind=Codex) — a live card at t=0, like Claude. No injector
        // (lifecycle + state only); Autopilot forced Off (nothing to drive). codexSessionId carries the
        // resume target (kept across restore; filled later by the observer for a fresh launch).
        ::Agentmaster::SessionInfo info = restored ? *restored : ::Agentmaster::SessionInfo{};
        info.id = handleId;
        info.kind = ::Agentmaster::AgentKind::Codex;
        info.title = ttl;
        info.workingDir = dir;
        info.codexSessionId = resumeUuid.empty() ? (restored ? restored->codexSessionId : std::wstring{}) : resumeUuid;
        info.state = ::Agentmaster::SessionState::Idle; // the C2 rollout-tail reconcile establishes the real state
        info.external = false; // we own this tab's ConPTY
        info.live = true;
        info.autopilot.mode = ::Agentmaster::AutopilotMode::Off; // no driving in this phase
        info.pendingConfirmPromptId.clear();
        _sessionRegistry->Upsert(info);

        if (tab)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetTabText(winrt::hstring{ ttl });
            }
            _ApplyDirColorToTab(tab, dir);
            _AttachClaudeOverlay(tab, handleId); // the per-tab badge (status color from the record's state)
            if (const auto s = _sessionRegistry->Get(handleId))
            {
                _SetTabAgentDot(tab, AgentStatusColorFor(s->state));
            }
        }

        const std::wstring tag = !resumeUuid.empty() ? L"[codex-resume] " : (restored ? L"[codex-restore-fresh] " : L"[codex-spawn] ");
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      tag + handleId + (resumeUuid.empty() ? L"" : (L" (rollout " + resumeUuid + L")")) + L" \"" + ttl + L"\" cwd=" + dir + L"\n");
        return tab;
    }

    // Agentmaster (Codex): adopt an EXTERNAL (observe-only) codex from the Explorer Tree's EXTERNAL
    // scope — the Codex analog of _AdoptExternalClaude. We host no ConPTY for the foreign codex, so we
    // can't drive it (Rule #13); "adopt" resumes its CONVERSATION (its rollout) into a NEW managed tab:
    // resolve the rollout uuid (cwd + process-start, Rule #14), then `codex resume <uuid>` — leaving the
    // original codex running. No rollout yet (never prompted) -> a fresh managed codex in the same dir.
    void TerminalPage::_AdoptExternalCodex(uint32_t pid, winrt::hstring cwd)
    {
        const std::wstring dir{ cwd };
        const int64_t start = ::Agentmaster::ProcessStartUnixMs(pid);
        const std::wstring uuid = ::Agentmaster::ResolveCodexSession(dir, start).sessionId;

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
        _LaunchCodexSession(cwd, winrt::hstring{}, restored);
    }

    // Agentmaster (Fleet Observer): adopt an EXTERNAL (observe-only) claude from the Explorer Tree's
    // EXTERNAL scope. We host no ConPTY for the foreign process, so we can NEVER inject into the live
    // external (Rule #9/#13) — "adopt" instead brings its CONVERSATION under management: resolve the
    // conversation id from the transcript (cwd + process-start -> ResolveSessionId, Rule #14) and
    // resume it into a NEW managed, controllable tab (claude --resume <id>, with a Flight Plan +
    // Autopilot), reusing the proven _LaunchClaudeSession resume path. When the external has no
    // transcript yet (never prompted -> id ""), fall back to a fresh managed session in the same dir
    // (== "Open New Session Here"). The original external process is left running and untouched — we never inject
    // into or kill it; the user closes it to avoid two writers on one transcript. _LaunchClaudeSession
    // re-checks ClaudeConversationExists, so a transcript that vanished between resolve and launch also
    // degrades to fresh rather than dying on "No conversation found".
    void TerminalPage::_AdoptExternalClaude(uint32_t pid, winrt::hstring cwd)
    {
        const std::wstring dir{ cwd };
        const int64_t start = ::Agentmaster::ProcessStartUnixMs(pid);
        const std::wstring id = ::Agentmaster::ResolveSessionId(dir, start);

        std::optional<::Agentmaster::SessionInfo> restored;
        if (!id.empty())
        {
            ::Agentmaster::SessionInfo info{};
            info.id = id; // _LaunchClaudeSession transcript-gates the --resume on this id
            info.workingDir = dir;
            // Adopted sessions are new to us (no persisted plan), so seed the cog's Autopilot defaults
            // as a fresh launch would — but resume the external's existing conversation.
            info.autopilot.mode = _appSettings.defaultAutopilotMode;
            info.autopilot.maxAutoSends = _appSettings.maxAutoSends;
            info.autopilot.stopOnError = _appSettings.stopOnError;
            info.autopilot.pauseOnHumanInput = _appSettings.pauseOnHumanInput;
            restored = std::move(info);
        }
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      std::wstring{ L"[adopt-external] pid=" } + std::to_wstring(pid) + L" cwd=" + dir +
                                          L" -> " + (id.empty() ? std::wstring{ L"(fresh \x2014 no transcript)" } : (L"resume " + id)) + L"\n");
        _LaunchClaudeSession(cwd, winrt::hstring{}, restored);
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
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[move-out] " + id + L" (Claude tab leaving this window; binding kept alive for the destination)\n");
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
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[move-out-pane] " + id + L" (Claude pane leaving this window; binding kept alive for the destination)\n");
    }

    // Agentmaster: rename a Claude session from the Manager's Explorer Tree (or a Triage-Board
    // card's menu — same editor). A session's title is ONE value — the Explorer-tree name, the
    // persisted SessionInfo.title, and the WT tab title.
    // Write it to the shared registry (which persists it via the autosave-on-change observer and
    // refreshes every window's Triage Board / Explorer Tree / Flight Plan) and, when THIS window
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
        _sessionRegistry->Update(id, [&name](::Agentmaster::SessionInfo& s) { s.title = name; });

        const auto it = _claudeTabs.find(id);
        if (const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetTabText(title);
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
        if (!_sessionRegistry)
        {
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
        const std::wstring text{ impl->GetTabText() };
        const auto info = _sessionRegistry->Get(id);
        if (text.empty())
        {
            // Override cleared (ResetTabText) -> re-pin to the managed name. Re-enters once, then
            // settles (GetTabText() == title -> the equality guard below returns without writing).
            if (info && !info->title.empty())
            {
                impl->SetTabText(winrt::hstring{ info->title });
            }
            return;
        }
        if (info && info->title == text)
        {
            return; // already in sync (our own pin / an Explorer-driven rename) -> no write, no loop
        }
        _sessionRegistry->Update(id, [&text](::Agentmaster::SessionInfo& s) { s.title = text; });
    }

    // Agentmaster (cross-window rename; Rule #11): the registry-observer reaction (bounced to this
    // window's UI thread by the engine-init observer, riding the same hop as the tab dot). When the
    // session's ONE title changes anywhere — an Explorer-tree/board-card rename in ANOTHER window,
    // a restore, the smart-naming — the window actually HOSTING the tab re-pins it here. A session
    // this window doesn't host is a cheap map-miss no-op (every window's observer sees every fleet
    // event). Equality-guarded: re-pinning an already-matching tab writes nothing, and the
    // SetTabText below re-enters _SyncClaudeTitleFromTab once, which sees registry == tab and also
    // writes nothing — so the two sync directions settle instead of looping.
    void TerminalPage::_SyncClaudeTabTitleFromRegistry(const std::wstring& sessionId, const std::wstring& title)
    {
        if (title.empty())
        {
            return; // never clear a pinned tab title from the registry side (the pin re-asserts on empty)
        }
        const auto it = _claudeTabs.find(sessionId);
        const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr;
        if (!tab)
        {
            return;
        }
        if (const auto impl = _GetTabImpl(tab))
        {
            if (std::wstring{ impl->GetTabText() } != title)
            {
                impl->SetTabText(winrt::hstring{ title });
            }
        }
    }

    // Agentmaster: paint a Claude tab from its working directory's color — the persisted color for
    // the dir if any, else a stable auto-assigned one (which we persist so it survives + is shared
    // by the dir). SetRuntimeTabColor re-enters _OnClaudeTabColorChanged once, which settles
    // immediately (the persisted color now equals the tab's color), so this never loops.
    void TerminalPage::_ApplyDirColorToTab(const TerminalApp::Tab& tab, const std::wstring& dir)
    {
        auto hex = ::Agentmaster::GetDirColor(dir);
        if (!hex)
        {
            hex = ::Agentmaster::AutoDirColorHex(dir);
            ::Agentmaster::SetDirColor(dir, hex); // persist the auto color -> stable + shared by the dir
        }
        if (const auto color = ClaudeHexToColor(*hex))
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetRuntimeTabColor(*color);
            }
        }
    }

    // Agentmaster: recolor every live Claude tab whose session shares `dir` (filesystem-aware match)
    // to `colorHex`, or reset them when nullopt. Fans a user's color change across the directory.
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
            if (!info || ::Agentmaster::NormDirKey(info->workingDir) != key)
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
    // directory: persist it for the dir and fan it out to every live tab in that dir. The de-dupe
    // vs the persisted color makes our own launch/propagation writes no-ops (no loop).
    void TerminalPage::_OnClaudeTabColorChanged(const TerminalApp::Tab& tab)
    {
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
        const std::wstring dir = info->workingDir;

        std::optional<std::wstring> newHex;
        if (const auto impl = _GetTabImpl(tab))
        {
            if (const auto c = impl->GetRuntimeTabColor())
            {
                newHex = ClaudeColorToHex(*c);
            }
        }
        if (::Agentmaster::GetDirColor(dir) == newHex)
        {
            return; // already in sync (our own launch/propagation write) -> no persist, no loop
        }
        ::Agentmaster::SetDirColor(dir, newHex); // upsert the color, or drop it on reset
        _ApplyDirColorToTabs(dir, newHex); // every live tab in this dir tracks the change
        _ScheduleWindowRecordSave(); // M10: the per-tab color rides in the window record
    }
}
