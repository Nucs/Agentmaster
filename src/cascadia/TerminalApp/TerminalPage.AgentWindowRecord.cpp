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
//   TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Agent Manager page (+ its Home button), _WireAgentManagerContent
//   TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
//   TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
// ★ TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
//   TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
//   TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the two SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster — per-window workspace persistence (M10; doc/agentmaster/PERSISTENCE.md):
// capture this window's WindowRecord from LIVE state (geometry via the PersistState
// recipe + ordered tab refs + the Manager lens), the debounced autosave + close-flush,
// re-home a reopened window's whole workspace (_RestoreWindowTabs: resume Claude
// sessions + replay shell tabs, in record order), the RESTORE WAKE of the re-homed tabs
// that were busy when Agentmaster went down (_BeginRestoreWake .. _LeaveRestoreWake;
// PERSISTENCE.md §14), and the "Reopen window(s)" dispatch
// (`agentmaster.exe -w -1 -s <idx>` via the execution alias BY NAME).
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // IsValidDirectory + GuidToPlainString (shell-tab cwd capture)
#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (type/hr/msg + throw stacks)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog
#include "AgentMaster/Engine.h" // RecoverableWindows (the reopen dispatch)
#include "AgentMaster/Persistence.h" // SaveWindowRecord
#include "AgentMaster/ProcessObserver.h" // Activity() table -> out-of-band shell-tab cwd for capture
#include "AgentMaster/ProfileBootstrap.h" // PackageFamilyName/IsDevPackage (the per-identity reopen alias)
#include "AgentMaster/SessionRegistry.h" // _RestoreWindowTabs reads the fleet

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
    // Agentmaster (M10): WT's LaunchMode flags -> a stable JSON token we own both ends of
    // (capture here, restore in the TerminalWindow startup seam). Mirrors WT's own token names.
    static std::wstring LaunchModeToToken(winrt::Microsoft::Terminal::Settings::Model::LaunchMode mode)
    {
        using LM = winrt::Microsoft::Terminal::Settings::Model::LaunchMode;
        if (WI_IsFlagSet(mode, LM::FullscreenMode))
        {
            return L"fullscreen";
        }
        const bool maximized = WI_IsFlagSet(mode, LM::MaximizedMode);
        const bool focus = WI_IsFlagSet(mode, LM::FocusMode);
        if (maximized && focus)
        {
            return L"maximizedFocus";
        }
        if (maximized)
        {
            return L"maximized";
        }
        if (focus)
        {
            return L"focus";
        }
        return L"default";
    }

    // Agentmaster: a shell (Other) tab's OSC-9;9 working dir (when shell integration reports one) +
    // its WT_SESSION id, walked from the tab's active terminal control. WT's BuildStartupActions
    // records WorkingDirectory() when present, else the profile's default StartingDirectory — so a tab
    // the user `cd`'d in is otherwise lost on reopen. Capture prefers the exact OSC value; when absent
    // it falls back to the Fleet Observer's Activity table, whose `cwd` carries the shell cwd read
    // OUT-OF-BAND (cmd's own PEB / a pwsh's newest native child, cached across idle) and is keyed by
    // this same WT_SESSION. The id is lowercased to match the observer's keys. (PERSISTENCE.md §13.5)
    struct ShellTabIdent
    {
        std::wstring oscCwd; // WorkingDirectory() if a valid dir, else empty
        std::wstring wtSession; // ITerminalConnection::SessionId(), lowercased
    };
    static ShellTabIdent _ShellTabIdent(const winrt::com_ptr<Tab>& tabImpl)
    {
        ShellTabIdent out;
        if (!tabImpl)
        {
            return out;
        }
        Microsoft::Terminal::Control::TermControl ctrl{ nullptr };
        tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
            if (ctrl)
            {
                return;
            }
            const auto content = pane->GetContent();
            if (!content)
            {
                return;
            }
            const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
            if (!term)
            {
                return;
            }
            if (const auto c = term.GetTermControl())
            {
                ctrl = c;
            }
        });
        if (!ctrl)
        {
            return out;
        }
        if (const auto wd = ctrl.WorkingDirectory(); ::Microsoft::Console::Utils::IsValidDirectory(wd.c_str()))
        {
            out.oscCwd = std::wstring{ wd };
        }
        if (const auto conn = ctrl.Connection())
        {
            out.wtSession = ::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId());
            for (auto& c : out.wtSession)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c + 32);
                }
            }
        }
        return out;
    }

    // Agentmaster (M10; PERSISTENCE.md §13): build this window's record from LIVE state. Geometry
    // uses the exact recipe as PersistState() (size from the tab content, position asked of the
    // window layer, mode from the focus/maximize flags) — read-only, no side effects. Tabs are
    // captured IN ORDER as REFERENCES: the pinned Manager + transient Settings tabs are skipped; a
    // Claude tab becomes its sessionId (+ runtime color); any other tab is marked Other (its full
    // ActionAndArgs restore blob is only needed by multi-window restore — Increment 3). The
    // Manager lens (rec.manager) is kept current by the content's push, so it's carried verbatim.
    ::Agentmaster::WindowRecord TerminalPage::_CaptureWindowRecord()
    {
        using namespace winrt::Microsoft::Terminal::Settings::Model;

        ::Agentmaster::WindowRecord rec = _windowRecord; // preserves windowId + the cached lens
        rec.windowId = _windowId;

        // --- geometry (same recipe as PersistState) ---
        ::Agentmaster::WindowGeometry geo;
        auto mode = LaunchMode::DefaultMode;
        WI_SetFlagIf(mode, LaunchMode::FullscreenMode, _isFullscreen);
        WI_SetFlagIf(mode, LaunchMode::FocusMode, _isInFocusMode);
        WI_SetFlagIf(mode, LaunchMode::MaximizedMode, _isMaximized);
        geo.launchMode = LaunchModeToToken(mode);

        if (_tabContent)
        {
            const auto w = static_cast<double>(_tabContent.ActualWidth());
            const auto h = static_cast<double>(_tabContent.ActualHeight());
            if (w > 0 && h > 0)
            {
                geo.hasSize = true;
                geo.width = w;
                geo.height = h;
            }
        }

        // We don't know our own position; ask the window layer (exactly as PersistState does).
        const auto launchPosRequest{ winrt::make<LaunchPositionRequest>() };
        RequestLaunchPosition.raise(*this, launchPosRequest);
        // LaunchPosition is a WinRT struct: X/Y are IReference<Int32> FIELDS, not methods.
        if (const auto pos = launchPosRequest.Position(); pos.X)
        {
            geo.hasPosition = true;
            geo.x = static_cast<double>(pos.X.Value());
            geo.y = pos.Y ? static_cast<double>(pos.Y.Value()) : 0.0;
        }
        // Agentmaster: fall back to the last-known geometry for any axis live-capture couldn't
        // read. A close/quit/teardown flush may run when _tabContent has no size (0×0) or the window
        // position request has no subscriber left, which would otherwise persist a geometry-less record
        // and lose the saved position/size. During a normal live autosave both are captured fresh, so
        // this is a no-op then. (_windowRecord still holds the prior save; rec is a copy of it.)
        if (!geo.hasSize && _windowRecord.geometry.hasSize)
        {
            geo.hasSize = true;
            geo.width = _windowRecord.geometry.width;
            geo.height = _windowRecord.geometry.height;
        }
        if (!geo.hasPosition && _windowRecord.geometry.hasPosition)
        {
            geo.hasPosition = true;
            geo.x = _windowRecord.geometry.x;
            geo.y = _windowRecord.geometry.y;
        }
        rec.geometry = geo;

        // --- ordered tab refs (+ which tab is focused) ---
        // The focused tab is persisted by STABLE IDENTITY so it survives a restart: a Claude tab by its
        // conversation id (selectedSessionId), a shell tab — which has no cross-restart id — by index
        // into rec.tabs (selectedTabIndex, the fallback). The Agent Manager page being on screen (no tab
        // focused — _GetFocusedTabIndex answers nullopt then) leaves both unset, and so does an unset
        // focus; on reopen that means "show the Manager page" (_RestoreWindowTabs). On reopen the window
        // re-selects this tab so closing/reopening preserves what was ACTIVE, not just the set of tabs.
        rec.tabs.clear();
        rec.selectedSessionId.clear();
        rec.selectedTabIndex = -1;
        TerminalApp::Tab focusedTab{ nullptr };
        if (const auto fi = _GetFocusedTabIndex(); fi && *fi < _tabs.Size())
        {
            focusedTab = _tabs.GetAt(*fi);
        }
        for (const auto& tab : _tabs)
        {
            if (_settingsTab && tab == _settingsTab)
            {
                continue; // the Settings tab is transient (the Agent Manager is a page — never in _tabs)
            }
            ::Agentmaster::TabEntry entry;
            const auto sessionId = _ClaudeSessionForTab(tab);
            if (!sessionId.empty())
            {
                // A managed session tab — Claude OR Codex (Codex-launch). A REFERENCE: the record (incl.
                // a Codex resume uuid on SessionInfo.codexSessionId) lives in sessions.json. Record the
                // KIND so _RestoreWindowTabs replays the right CLI (`claude --resume` vs `codex resume`).
                entry.kind = ::Agentmaster::TabKind::Claude;
                if (_sessionRegistry)
                {
                    if (const auto s = _sessionRegistry->Get(sessionId); s && s->kind == ::Agentmaster::AgentKind::Codex)
                    {
                        entry.kind = ::Agentmaster::TabKind::Codex;
                    }
                }
                entry.sessionId = sessionId;
                // Agentmaster: do NOT capture a per-tab color here. A Claude tab's color is ONE value per
                // working directory (Rule #12) — owned by dir-colors.json and re-applied on restore by
                // _ApplyDirColorToTab (the dir's persisted custom color, else a stable auto color hashed
                // from the dir, which reproduces exactly what the tab had). The old entry.tabColor write
                // was dead round-trip data: _RestoreWindowTabs never read it, and applying it WOULD break
                // Rule #12 (one tab keeping a custom color while its dir-siblings revert to auto).
            }
            else
            {
                // Other (pwsh / cmd / any non-Claude) tab: capture its full WT restore blob so the
                // reopened window can rebuild it with its title + color + cwd. BuildStartupActions(Persist)
                // is exactly what WT's own PersistState() serializes per tab; wrap this one tab's actions
                // in a WindowLayout and stringify via the public WindowLayout::ToJson (round-tripped on
                // restore by WindowLayout::FromJson -> ProcessStartupActions in _RestoreWindowTabs).
                // Best-effort: a tab that yields no actions stays an empty Other ref (nothing to rebuild).
                entry.kind = ::Agentmaster::TabKind::Other;
                try
                {
                    if (const auto t = _GetTabImpl(tab))
                    {
                        auto tabActions = t->BuildStartupActions(BuildStartupKind::Persist);
                        if (!tabActions.empty())
                        {
                            // Agentmaster: BuildStartupActions stamps the cwd from WorkingDirectory()
                            // (OSC 9;9) else the profile default — losing where the user `cd`'d. Recover
                            // the tab's REAL cwd and overwrite the NewTab action's StartingDirectory so the
                            // reopened tab lands there: prefer the exact OSC value, else the Fleet
                            // Observer's out-of-band reading (cmd's own PEB / a pwsh's newest native child,
                            // cached across idle gaps). Empty -> keep WT's value.
                            const auto ident = _ShellTabIdent(t);
                            std::wstring realCwd = ident.oscCwd;
                            if (realCwd.empty() && _observer && !ident.wtSession.empty())
                            {
                                for (const auto& a : _observer->Activity())
                                {
                                    if (a.wtSession == ident.wtSession && !a.cwd.empty())
                                    {
                                        realCwd = a.cwd;
                                        break;
                                    }
                                }
                            }
                            if (!realCwd.empty() && tabActions[0].Action() == ShortcutAction::NewTab)
                            {
                                if (const auto nta = tabActions[0].Args().try_as<NewTabArgs>())
                                {
                                    if (const auto term = nta.ContentArgs().try_as<NewTerminalArgs>())
                                    {
                                        term.StartingDirectory(winrt::hstring{ realCwd });
                                    }
                                }
                            }
                            WindowLayout layout;
                            layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>(std::move(tabActions)));
                            entry.actionsJson = std::wstring{ WindowLayout::ToJson(layout) };
                        }
                    }
                }
                CATCH_LOG();
            }
            if (focusedTab && tab == focusedTab)
            {
                // Prefer the stable managed-session handle (Claude conversation id, or our durable Codex
                // handle); a shell tab (no cross-restart id) falls back to its index in rec.tabs. Read
                // entry BEFORE the move below.
                if ((entry.kind == ::Agentmaster::TabKind::Claude || entry.kind == ::Agentmaster::TabKind::Codex) && !entry.sessionId.empty())
                {
                    rec.selectedSessionId = entry.sessionId;
                }
                else if (!entry.actionsJson.empty())
                {
                    // Agentmaster: only record a shell-tab focus target that can actually be RECREATED on
                    // restore. A focused Other tab whose BuildStartupActions yielded nothing (empty
                    // actionsJson) is skipped by _RestoreWindowTabs, so pointing selectedTabIndex at it
                    // would silently drop focus restoration onto a tab that never reappears. Leaving it
                    // unset falls back to the default focus (the natural choice for an un-recreatable tab).
                    rec.selectedTabIndex = static_cast<int>(rec.tabs.size()); // index this entry will occupy
                }
            }
            rec.tabs.push_back(std::move(entry));
        }

        // Agentmaster (PERSISTENCE.md §16 — HELD TAB REFS): the saved tabs this window's restore could NOT bring back go back
        // into the record, each at about its old place: the strip above holds only the tabs that exist, and a save of it alone
        // would delete the others from the workspace (a start without Claude Code would have saved every window empty). A held
        // ref whose session has a tab again — in this window (a Resume here) or in another one (live) — is let go for good: the
        // tab that exists is now the record of it. A shell ref has no session, so only the user's close of the window ends it.
        if (!_heldTabRefs.empty())
        {
            const bool heldChanged = ::Agentmaster::MergeHeldTabRefs(
                rec.tabs,
                _heldTabRefs,
                [this](const ::Agentmaster::TabEntry& t) {
                    if ((t.kind != ::Agentmaster::TabKind::Claude && t.kind != ::Agentmaster::TabKind::Codex) || t.sessionId.empty())
                    {
                        return false;
                    }
                    if (_claudeTabs.find(t.sessionId) != _claudeTabs.end())
                    {
                        return true; // this window hosts the session again
                    }
                    // Another window hosts it (live), so that window's record holds the tab. The lean read: no queue copy.
                    const auto s = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(t.sessionId) : std::nullopt;
                    return s && s->live;
                },
                rec.selectedTabIndex);
            if (heldChanged)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[rehome] window " + _windowId + L" let go of held tab ref(s) whose session has a tab again; " +
                                                                std::to_wstring(_heldTabRefs.size()) + L" still held\n");
                _PublishHeldTabRefs();
            }
        }

        // (--- The retired Manager tab's per-window color ---: nothing to capture — the Agent Manager is a
        // page behind the Home button now and wears no tab color. rec.managerTabColor is whatever the
        // claimed record carried — rec is a copy of _windowRecord — so an older record round-trips
        // unchanged instead of being wiped.)

        // --- Side panel per-window layout ---
        // Read the LIVE members only once the panel actually built this run; a capture before/without
        // the panel keeps whatever the claimed record carried (rec is a copy of _windowRecord), so a
        // build failure can never wipe a window's stored layout. The fractions are drag-band-clamped
        // at every writer, so what lands here always re-loads (the FromJson band check passes).
        if (_sidePanelBuilt)
        {
            rec.sidePanelWidthFraction = _sidePanelWidthFraction;
            rec.sidePanelSplitFraction = _sidePanelSplitFraction;
            rec.sidePanelCollapsed = _sidePanelCollapsed ? 1 : 0; // the tri-state commits once the panel exists
            rec.sidePanelSummaryHidden = _sidePanelSummaryHidden ? 1 : 0; // the "Sum" toggle — same tri-state commit
            rec.sidePanelGlobal = _sidePanelGlobal; // the card-list scope — per-window ONLY (no global default exists for it; ToJson omits a LOCAL false)
        }

        return rec;
    }

    // Agentmaster (M10): the debounced autosave trigger. No-ops until startup completes so the
    // tabs/geometry churned during _OnFirstLayout don't write a half-built record.
    // Agentmaster (flows TAB-K1): also a no-op once the window's teardown flush has written its final record
    // (_windowRecordTeardownFlushed): the window is closing, and the archive that follows that flush empties
    // _claudeTabs, so any save it would arm could only record the Claude tabs as shells. The archive's own
    // registry notifications (the lens, the side panel, the tab dots) are the triggers this stops; a save armed
    // BEFORE the latch is refused by _FlushWindowRecord itself when its timer fires.
    void TerminalPage::_ScheduleWindowRecordSave()
    {
        if (_startupState != StartupState::Initialized || _windowId.empty() || _windowRecordTeardownFlushed)
        {
            return;
        }
        if (_saveWindowRecordThrottled)
        {
            _saveWindowRecordThrottled->Run();
        }
    }

    // Agentmaster (M10): capture + persist synchronously. The throttled autosave funnels here, and
    // so does the close-flush (CloseWindow) so the last move/resize isn't lost past the debounce.
    //
    // Agentmaster (flows TAB-K1): the teardown flush is the LAST write. Every teardown seam (CloseWindow,
    // RequestQuit, _QuitForUpdate, the Manager-only self-close, ~TerminalPage's catch-all) calls this once and
    // then sets _windowRecordTeardownFlushed, and every one of them then archives the window's sessions, which
    // clears _claudeTabs. A capture after that would read every Claude tab as an anonymous shell
    // (_ClaudeSessionForTab finds no session) and save that over the good record. The autosave armed by a tab
    // switch, a resize or a lens change just before the close did exactly that when its 750 ms debounce ended
    // between the archive and the window's end — the close is only POSTED to the emperor (WM_CLOSE_TERMINAL_WINDOW),
    // so the page and its timer live a little longer: on 0.8.0 the record then held [sh,sh,] and the next start (or
    // Reopen Windows, which re-reads the record from disk) brought the sessions back as plain PowerShell tabs.
    //
    // So once the latch is set this refuses to write, and logs the refusal: that line is how a run tells that a
    // late save existed and was stopped. The latch is set only after the user's final decision (a cancelled
    // close or quit returns before it), so a window that stays open never stops saving.
    //
    // Returns: nothing. Throws: whatever the capture's XAML reads or the string building can throw
    // (winrt::hresult_error, std::bad_alloc) — the refusal path only builds its log line. A failed WRITE is not
    // an exception: SaveWindowRecord reports it as [persist-fail].
    void TerminalPage::_FlushWindowRecord()
    {
        if (_windowId.empty())
        {
            return; // engine not initialized (no Manager page) — nothing to persist
        }
        if (_windowRecordTeardownFlushed)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[window-save] " + _windowId +
                                                            L" late save refused: the window's close already saved its final record; a save now would record "
                                                            L"its archived Claude tabs as shell tabs (flows TAB-K1)\n");
            return;
        }
        // Anti-clobber (window-grouped restore): never overwrite a good record with a not-yet-started
        // window's nothing. A transient single-instance/handoff window (e.g. a quick reopen that opens
        // then closes before layout) would otherwise capture an EMPTY, geometry-less record over the
        // real one and wipe the saved workspace — breaking the very next reopen. Two cheap guards:
        //  (1) only flush once THIS window finished startup (a pre-Initialized window has no real state);
        if (_startupState != StartupState::Initialized)
        {
            return;
        }
        auto rec = _CaptureWindowRecord();
        const bool managerOnly = rec.tabs.empty(); // capture skips the Settings tab -> empty == no content tab (the Manager page alone)
        // Agentmaster (discard Manager-only windows): a window the user emptied down to just its Agent
        // Manager page must NOT persist as a restorable window (the self-close requirement). When
        // _CloseWindowIfManagerOnly latched the discard (it is the ONLY writer of this flag, and never
        // during restore — so an only-shells reopen that is briefly tab-empty is safe), DELETE the
        // on-disk record instead of saving it. Keep the in-memory copy current (empty, but its windowId +
        // lens preserved) so that if a real tab later returns the window re-persists with its lens intact.
        if (managerOnly && _managerOnlyDiscard)
        {
            ::Agentmaster::DeleteWindowRecord(_windowId);
            _windowRecord = std::move(rec);
            return;
        }
        // A real (terminal) tab is present -> this window persists normally again; clear the latch so a
        // future flush saves it (and so a stale latch from an earlier empty spell can't suppress it).
        if (!managerOnly)
        {
            _managerOnlyDiscard = false;
        }
        //  (2) a capture with NEITHER content tabs NOR geometry is an un-laid-out window — keep the
        //      record on disk (the live window's real save follows). A legitimately session-less window
        //      still has geometry, so an empty-but-positioned window (just the Manager page) still saves
        //      here UNLESS the discard latch above fired (a genuinely emptied window deletes its record).
        if (managerOnly && !rec.geometry.hasPosition && !rec.geometry.hasSize)
        {
            return;
        }
        // Save-side trace, CHANGE-GATED: log the persisted tab set only when it differs from the last save
        // (_windowRecord still holds the prior tabs until the move below). This shows the window's tab
        // COMPOSITION evolving — i.e. what will re-home next launch, the answer to "was a bad restore saved
        // wrong or loaded wrong?" — WITHOUT flooding on the geometry/lens-only autosaves that fire every
        // ~750ms during a resize. Once-per-real-change, so it's cheap and quiet.
        std::wstring newSig, oldSig;
        for (const auto& e : rec.tabs)
        {
            newSig += (e.kind == ::Agentmaster::TabKind::Other) ? std::wstring{ L"sh," } : (::Agentmaster::ShortId(e.sessionId) + L",");
        }
        for (const auto& e : _windowRecord.tabs)
        {
            oldSig += (e.kind == ::Agentmaster::TabKind::Other) ? std::wstring{ L"sh," } : (::Agentmaster::ShortId(e.sessionId) + L",");
        }
        if (newSig != oldSig)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[window-save] " + _windowId + L" tabs=" + std::to_wstring(rec.tabs.size()) + L" [" + newSig + L"]\n");
        }
        _windowRecord = std::move(rec);
        ::Agentmaster::SaveWindowRecord(_windowRecord);
    }

    // Agentmaster (discard Manager-only windows): true iff this window holds NO tab at all — just its
    // Agent Manager page (the page is never in _tabs, so an empty strip is exactly "Manager-only"; it used
    // to be "the pinned Manager tab is the sole tab"). A Settings tab (or any terminal tab) counts as real
    // content. A window WITHOUT a Manager page (the page failed to build) is never Manager-only here — an
    // empty page-less window already took upstream's close path instead (_RemoveTab /
    // _CompleteInitialization). Returns: that predicate. Never throws.
    // Agentmaster (PERSISTENCE.md §16): a window that HOLDS saved tab refs its restore could not bring back (Claude Code not
    // found at this start, ...) is NOT Manager-only, however empty its strip looks: its record still names those tabs, and
    // treating it as content-free would self-close it and delete that record — the whole workspace, when no Claude tab could
    // be made. It stays open, keeps saving its record with the held refs, and the next start brings the tabs back.
    bool TerminalPage::_IsManagerOnlyWindow() const
    {
        return static_cast<bool>(_managerPage) && _tabs.Size() == 0 && _heldTabRefs.empty();
    }

    // Agentmaster (PERSISTENCE.md §16): tell the engine which sessions this window holds saved tabs of (HeldSessionIds of
    // _heldTabRefs, replacing what it published before; an empty set clears it). The Telegram bot asks the engine before it
    // deletes a topic as "closed" (Engine::IsSessionHeldByAWindow), so a held session keeps its topic and history. Called
    // after the restore holds refs and whenever a capture lets one go. Never throws: a failure is logged (Rule #18) and the
    // engine keeps the previous set — which only keeps a topic longer, never deletes one.
    void TerminalPage::_PublishHeldTabRefs() noexcept
    {
        try
        {
            ::Agentmaster::NoteWindowHeldSessions(_windowId, ::Agentmaster::HeldSessionIds(_heldTabRefs));
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PublishHeldTabRefs");
        }
    }

    // Agentmaster (discard Manager-only windows): the debounced decision. A window that ends up holding
    // no tab — only its Agent Manager page — by closing/tearing-out its last tab, or by reopening from
    // an empty record, must not linger as a restorable, content-less window. (A FAILED restore is not
    // content-less: the refs it could not bring back are HELD — PERSISTENCE.md §16 — and a window holding
    // any is never Manager-only, see _IsManagerOnlyWindow.) Unless it is the
    // LAST Agentmaster window it self-closes SILENTLY (there is nothing to confirm or lose); the last
    // window stays open (the app needs one) but discards its record so it isn't restored as Manager-only
    // next launch. Re-validates everything itself (it is reached via a debounce, so the state may have
    // changed since it was scheduled), and gates on Initialized so a window still re-homing its tabs
    // during startup is never mistaken for empty.
    void TerminalPage::_CloseWindowIfManagerOnly()
    {
        if (_startupState != StartupState::Initialized || _windowId.empty())
        {
            return;
        }
        if (!_IsManagerOnlyWindow())
        {
            // A real tab is present -> drop any stale discard latch so this window persists normally.
            _managerOnlyDiscard = false;
            return;
        }
        // Genuinely Manager-only. Latch the discard so the record is deleted (here, and by any later
        // flush) rather than saved, then decide self-close vs keep-as-last under the engine's race-safe
        // reservation (two windows emptying at once must never both close and quit the app).
        _managerOnlyDiscard = true;
        if (::Agentmaster::ReserveManagerOnlyClose(_windowId))
        {
            // Not the last window — self-close. Mirror CloseWindow's deterministic seam MINUS the
            // confirm dialog (an empty window has nothing to warn about): discard the record, latch the
            // teardown flush so ~TerminalPage doesn't re-save, archive (a no-op — no sessions here), and
            // raise the close. UnregisterLiveWindow (in the destructor) then drops the live id + clears
            // the reservation; the deleted record means it is gone from every recover surface too.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[manager-only] self-close window " + _windowId + L"\n");
            _FlushWindowRecord();
            _windowRecordTeardownFlushed = true;
            _ArchiveWindowSessionsOnTeardown();
            CloseWindowRequested.raise(*this, nullptr);
        }
        else
        {
            // The LAST Agentmaster window: keep it open, but discard its on-disk record so the app does
            // not reopen a Manager-only window next launch (it falls back to a fresh default window). A
            // real tab returning clears the latch (above) and re-persists the window.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[manager-only] keep last window " + _windowId + L" (record discarded)\n");
            _FlushWindowRecord();
        }
    }

    // Agentmaster (discard Manager-only windows): run the debounced Manager-only check. Fed by every
    // tab add/remove (_tabs.VectorChanged) and the end of startup, so the window's SETTLED tab set is
    // evaluated once — coalescing restore churn / a tear-out / a user close into one decision. The
    // throttle fires on the UI thread; _CloseWindowIfManagerOnly re-validates and gates on Initialized.
    void TerminalPage::_ScheduleManagerOnlyCheck()
    {
        if (_managerOnlyCheckThrottled)
        {
            _managerOnlyCheckThrottled->Run();
        }
    }

    // Agentmaster (M10 window-grouped restore; PERSISTENCE.md §13 Phase C): re-home THIS window's
    // persisted tabs. A window reopened from its WindowRecord (claimed by id at engine init) comes back
    // with no tab (just its hidden Agent Manager page); this rebuilds the tabs from the record's ORDERED
    // tab refs, in place, so "close the window -> reopen it" brings the whole workspace back — not just
    // geometry + lens — and re-shows the Manager page when that is what was on screen at close (the
    // record names no focused tab then). Per ref, in record order:
    //   • Claude -> resume its session INTO this window (claude --resume, the archived record's queue +
    //     autorunner intact) via _LaunchClaudeSession — the Archived-button path, targeted here. Lazy-
    //     start safe: the tab is created through the normal pane path (no eager connection.Start()), so
    //     a background restored claude starts its conversation only when first focused (WT's lazy-tab
    //     behavior) — never the eager-Start AV (see Gotchas). A session already live (open in another
    //     window) or unknown to the registry (the process-once fleet load hasn't landed, or it was
    //     pruned) is skipped — it stays in the Archived list rather than being guessed.
    //   • Other -> replay its captured WT startup actions (WindowLayout::FromJson -> ProcessStartup-
    //     Actions) to rebuild the shell tab with its title + color + cwd.
    // Only a CLAIMED record restores (a fresh "+ new window" has nothing to re-home). v1 ordering: the
    // Claude tabs (launched synchronously) land before the Other tabs (whose actions replay on the XAML
    // dispatcher); exact left-to-right interleave of the two kinds is a deferred refinement.
    // Agentmaster (the RESTORE WAKE — PERSISTENCE.md §14): every re-homed tab stays dormant except the one the window
    // opens to, so this also picks the tabs whose session was in the middle of something when Agentmaster went down
    // (RestoreWakeWantsSession: persisted Running / NeedsApproval / Error) and hands them to _BeginRestoreWake, which
    // starts them once the tab EVERY reopened window opens to has finished launching. A window that restores nothing
    // leaves the wake barrier instead (the restoreWakeExit guard), so it never holds the other windows back.
    void TerminalPage::_RestoreWindowTabs()
    {
        // Whatever happens below — the early return of a fresh window, a throw mid-loop — this window's restore is
        // OVER when this frame ends, and the engine's "windows still restoring" count (which the Telegram bot
        // reads before it deletes a topic as "closed") must drop. The id entered the count at RegisterLiveWindow.
        auto restoreDone = wil::scope_exit([this]() noexcept {
            try
            {
                ::Agentmaster::NoteWindowRestoreDone(std::wstring{ _windowId });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_RestoreWindowTabs (NoteWindowRestoreDone)");
            }
        });
        // Agentmaster (the RESTORE WAKE): a window that does not reach the end of this function never JOINS the wake
        // barrier, so it must LEAVE it: an announced window that restores nothing (its assigned record was gone, the record
        // holds no tab) or that threw mid-loop would otherwise hold every other window's busy tabs back until the barrier's
        // 5-minute cap. The end of the function sets the flag and joins instead. A window that was never announced has no
        // key, and the leave is a silent no-op for it.
        bool restoreWakeBegun = false;
        auto restoreWakeExit = wil::scope_exit([this, &restoreWakeBegun]() noexcept {
            if (!restoreWakeBegun)
            {
                _LeaveRestoreWake(L"this window's restore re-homed no saved tabs");
            }
        });
        if (!_windowRecordClaimed || _windowRecord.tabs.empty() || !_sessionRegistry)
        {
            return;
        }
        using namespace winrt::Microsoft::Terminal::Settings::Model;
        // Snapshot the refs + the focused-tab target FIRST: _LaunchClaudeSession / ProcessStartupActions
        // re-enter capture + autosave, which rewrites _windowRecord (tabs AND the selected* fields) out
        // from under us mid-loop.
        const auto tabs = _windowRecord.tabs;
        const std::wstring selSessionId = _windowRecord.selectedSessionId; // stable Claude id (preferred)
        const int selTabIndex = _windowRecord.selectedTabIndex; // shell-tab fallback; -1 = the Manager page / none
        // Neither focus field set => the Agent Manager page was on screen at close (or nothing was
        // focused): show the page again once the tabs are back, instead of selecting any tab.
        const bool wantManager = selSessionId.empty() && selTabIndex < 0;
        size_t resumed = 0, shells = 0, skipped = 0;
        // The focused tab's ABSOLUTE index in _tabs after re-home (the Claude tabs from index 0 in
        // creation order, then the shell tabs — no pinned tab precedes them now that the Manager is a
        // page). Computed LIVE from what actually got created — the persisted identity is the stable
        // sessionId (Claude) / record index (shell), never a live position. -1 => no tab to select: the
        // Manager page is wanted (wantManager), or the selected session couldn't be restored (leave the
        // default focus — the last tab created).
        int targetAbs = -1;
        // Agentmaster (the RESTORE WAKE): the re-homed sessions that were in the middle of something when Agentmaster went
        // down, in strip order — woken once the tab every reopened window opens to has launched (_BeginRestoreWake).
        std::vector<std::wstring> wakeCandidates;

        // Restore-story trace (BEGIN): list every ref by kind + session id BEFORE re-homing, so a tab that
        // later skips or mis-resumes is pinpointable — the [rehome] end line is only counts. Pairs with the
        // [window-claim] line (same _windowId) and the per-tab [rehome] resume/skip lines below.
        {
            std::wstring refList;
            for (const auto& e : tabs)
            {
                if (e.kind == ::Agentmaster::TabKind::Claude)
                {
                    refList += L" claude:" + ::Agentmaster::ShortId(e.sessionId);
                }
                else if (e.kind == ::Agentmaster::TabKind::Codex)
                {
                    refList += L" codex:" + ::Agentmaster::ShortId(e.sessionId);
                }
                else
                {
                    refList += L" shell";
                }
            }
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[rehome-begin] window " + _windowId + L" refs=" + std::to_wstring(tabs.size()) +
                                              L" select=" + (selSessionId.empty() ? (selTabIndex >= 0 ? (L"#" + std::to_wstring(selTabIndex)) : std::wstring{ L"(manager/none)" }) : ::Agentmaster::ShortId(selSessionId)) +
                                              L" |" + refList + L"\n");
        }

        // Agentmaster (PERSISTENCE.md §16 — HELD TAB REFS): whether this start could use sessions.json. When it could not, a
        // ref to a session the registry does not know is unknown because the fleet did not load — so it is HELD below, not
        // dropped (ShouldHoldRestoreRef). Set by the one-time fleet load, which every window's restore runs after.
        const bool fleetLoadUsable = ::Agentmaster::SharedEngine().startupFleetLoadUsable.load();
        // A fresh restore starts with nothing held (the vector is this window's own, and a restore runs once per window).
        _heldTabRefs.clear();
        // Hold one ref the restore could not bring back when ShouldHoldRestoreRef says so: it then rides every save of the
        // record (_CaptureWindowRecord) until its tab exists again. Returns whether it was held, for the log line.
        const auto holdIfDue = [this, fleetLoadUsable](size_t recordIndex, const ::Agentmaster::TabEntry& entry, ::Agentmaster::RestoreRefOutcome why) {
            if (!::Agentmaster::ShouldHoldRestoreRef(why, fleetLoadUsable))
            {
                return false;
            }
            _heldTabRefs.push_back(::Agentmaster::HeldTabRef{ recordIndex, entry, why });
            return true;
        };

        // Pass 1 — managed sessions (Claude + Codex), synchronously, in record order. Indexed, so a ref the restore holds
        // keeps its place in the record (HeldTabRef::recordIndex).
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            const auto& entry = tabs[i];
            const bool isCodex = entry.kind == ::Agentmaster::TabKind::Codex;
            if ((entry.kind != ::Agentmaster::TabKind::Claude && !isCodex) || entry.sessionId.empty())
            {
                continue;
            }
            if (_claudeTabs.find(entry.sessionId) != _claudeTabs.end())
            {
                continue; // already bound in this window (re-entrant restore)
            }
            const auto info = _sessionRegistry->Get(entry.sessionId);
            if (!info || info->live)
            {
                // Unknown: held when the fleet did not load (then "unknown" says nothing about the session); a session
                // another window hosts is that window's tab — never held here.
                const bool held = holdIfDue(i, entry, info ? ::Agentmaster::RestoreRefOutcome::LiveElsewhere : ::Agentmaster::RestoreRefOutcome::UnknownSession);
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[rehome] window " + _windowId + L" skip " + ::Agentmaster::ShortId(entry.sessionId) +
                                                  (info ? std::wstring{ L" (already live elsewhere)" } : std::wstring{ L" (unknown \x2014 fleet not loaded / pruned)" }) +
                                                  (held ? std::wstring{ L" \x2014 HELD in the record: sessions.json could not be used at this start" } : std::wstring{}) + L"\n");
                ++skipped; // unknown (fleet not loaded yet / pruned) or already open elsewhere
                continue;
            }
            // Kind-aware re-home: Codex resumes via `codex resume <uuid>` (rollout-gated on
            // SessionInfo.codexSessionId), Claude via `claude --resume <id>`. _LaunchClaudeSession resumes
            // EXACTLY the id this ref recorded (no "continuation tail" redirect: /compact is in-place and
            // keeps its id, /clear leaves no link to its successor) and returns null when that
            // conversation is ALREADY hosted by an earlier ref this restore — two refs recorded for one
            // conversation (a tab can't be two sessions). A null is NOT a new tab, so it must not advance
            // the resumed/focus accounting — count it skipped.
            const auto homed = isCodex
                ? _LaunchCodexSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info)
                : _LaunchClaudeSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
            if (!homed)
            {
                // The launch made no tab. Its own line just above says why ([launch-blocked]: Claude Code not found, or a
                // preset that cannot launch; [launch-fail]: the pane could not be built; [restore-dedup]: a duplicate ref,
                // which the _claudeTabs check above already turns away). None of these is the user closing the tab, and
                // the first can pass by itself (Claude Code installed or updated) — so the ref is HELD, never dropped
                // (PERSISTENCE.md §16): without the hold, the window's next save wrote its record without the tab, and a
                // start without Claude Code wrote every window's record empty and deleted every Manager-only one.
                holdIfDue(i, entry, ::Agentmaster::RestoreRefOutcome::LaunchRefused);
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[rehome] window " + _windowId + L" skip " + ::Agentmaster::ShortId(entry.sessionId) +
                                                  L" (the launch made no tab \x2014 see the line above) \x2014 HELD in the record; the next start tries it again\n");
                ++skipped;
                continue;
            }
            if (!selSessionId.empty() && entry.sessionId == selSessionId)
            {
                targetAbs = static_cast<int>(resumed); // this managed tab's index (managed tabs lead the strip)
            }
            // Agentmaster (the RESTORE WAKE): remember a tab whose session was in the middle of something when Agentmaster
            // went down. Read AFTER the launch, whose Upsert holds the normalized state + SessionInfo::midTurnAtRestore: a
            // restore-FRESH (the transcript was gone) keeps the id but starts Idle with no turn in flight (PERSISTENCE.md
            // §16), so it reads as nothing to wake — a brand-new conversation has no turn to continue. The lean read: the
            // queue is never looked at.
            if (const auto rec = _sessionRegistry->GetWithoutQueue(entry.sessionId); rec && ::Agentmaster::RestoreWakeWantsSession(*rec))
            {
                wakeCandidates.push_back(entry.sessionId);
            }
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[rehome] window " + _windowId + L" resume " + (isCodex ? L"codex " : L"claude ") + ::Agentmaster::ShortId(entry.sessionId) + L" \"" + info->title + L"\"\n");
            ++resumed;
        }

        // Pass 2 — Other (shell) tabs: accumulate ALL their captured actions in record order and replay
        // them in ONE ProcessStartupActions call (the WT startup path appends a tab per newTab action).
        // A single call keeps the shells in order AND avoids the re-entrant virtual-cwd save/restore race
        // that per-tab calls would cause (ProcessStartupActions wraps its batch in a window cwd swap).
        std::vector<ActionAndArgs> shellActions;
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            const auto& entry = tabs[i];
            if (entry.kind != ::Agentmaster::TabKind::Other || entry.actionsJson.empty())
            {
                continue; // a shell saved with no actions (ShellEmpty) holds nothing to rebuild or keep
            }
            bool unreadable = false; // PERSISTENCE.md §16: its saved actions threw while parsing — held, not dropped
            try
            {
                const auto layout = WindowLayout::FromJson(winrt::hstring{ entry.actionsJson });
                if (layout)
                {
                    if (const auto tl = layout.TabLayout(); tl && tl.Size() > 0)
                    {
                        for (const auto& a : tl)
                        {
                            shellActions.push_back(a);
                        }
                        if (selSessionId.empty() && selTabIndex == static_cast<int>(i))
                        {
                            targetAbs = static_cast<int>(resumed) + static_cast<int>(shells); // this shell's index (after every managed tab)
                        }
                        ++shells;
                    }
                }
            }
            catch (...)
            {
                // What CATCH_LOG() did here (the wil failure log), plus the flag: the shell's saved blob is the only copy of
                // that tab (title, color, folder), so one this build cannot read is kept for a later one, not dropped.
                LOG_CAUGHT_EXCEPTION();
                unreadable = true;
            }
            if (unreadable && holdIfDue(i, entry, ::Agentmaster::RestoreRefOutcome::ShellUnreadable))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[rehome] window " + _windowId + L" skip shell #" + std::to_wstring(i) +
                                                                L" (its saved actions could not be read) \x2014 HELD in the record\n");
                ++skipped;
            }
        }

        // Re-select the focused tab. With shells (created ASYNC by ProcessStartupActions) we append a
        // SwitchToTab as the batch's LAST action so it runs AFTER the shells exist and overrides the
        // "focus the tab I just made" that each newTab does. With no shells the strip is already settled,
        // so select synchronously. targetAbs < 0 (selected session couldn't be restored) => leave default.
        // Agentmaster (the Agent Manager PAGE): a window that was on the Manager at close (wantManager)
        // re-shows the page instead of selecting any tab — after the async shells when there are some
        // (each created shell selects itself, so the batch shows the page after its LAST one, via the
        // one-shot latch it consumes in its synchronous prologue), else right now.
        if (!shellActions.empty())
        {
            if (targetAbs >= 0)
            {
                ActionAndArgs sel;
                sel.Action(ShortcutAction::SwitchToTab);
                sel.Args(SwitchToTabArgs{ static_cast<uint32_t>(targetAbs) });
                shellActions.push_back(std::move(sel));
            }
            else if (wantManager)
            {
                _showManagerPageAfterStartupBatch = true; // consumed by exactly the batch started on the next line
            }
            ProcessStartupActions(std::move(shellActions));
        }
        else if (targetAbs >= 0 && targetAbs < static_cast<int>(_tabs.Size()))
        {
            if (const auto t = _tabs.GetAt(static_cast<uint32_t>(targetAbs)))
            {
                if (const auto& item = t.TabViewItem())
                {
                    _tabView.SelectedItem(item);
                }
            }
        }
        else if (wantManager)
        {
            _ShowManagerPage(L"reopened on the Manager");
        }

        // Agentmaster (PERSISTENCE.md §16): the held refs in RECORD order (pass 1 held managed refs, pass 2 shells), which is
        // the order MergeHeldTabRefs needs to put each back at its place; then tell the engine, so the Telegram bot keeps the
        // held sessions' topics. Before the summary line, so the line's held= count is the final one.
        std::stable_sort(_heldTabRefs.begin(), _heldTabRefs.end(), [](const ::Agentmaster::HeldTabRef& a, const ::Agentmaster::HeldTabRef& b) {
            return a.recordIndex < b.recordIndex;
        });
        if (!_heldTabRefs.empty())
        {
            _PublishHeldTabRefs();
        }

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[rehome] window " + _windowId + L" resumed=" + std::to_wstring(resumed) +
                                          L" shells=" + std::to_wstring(shells) + L" skipped=" + std::to_wstring(skipped) +
                                          L" held=" + std::to_wstring(_heldTabRefs.size()) +
                                          L" select=" + (wantManager ? std::wstring{ L"manager-page" } : std::to_wstring(targetAbs)) +
                                          L" of " + std::to_wstring(tabs.size()) + L" refs\n");

        // Agentmaster (the RESTORE WAKE): join the wake barrier — even with no candidate, because the tab this window opens
        // to still holds back every OTHER window's wakes until it launched — and start the wait. The focus the gate waits
        // for is the managed session the restore selected, while this window hosts it under that id (a restore-FRESH used
        // to re-key it, and the gate then watched the current foreground instead; it keeps the id since PERSISTENCE.md
        // §16, so only a path that re-keys falls back to the foreground now); "" for a shell, the Agent Manager page, or a
        // focus that could not be restored (a selected tab whose ref was HELD included).
        const std::wstring focusSessionId = (!selSessionId.empty() && targetAbs >= 0 && _claudeTabs.find(selSessionId) != _claudeTabs.end()) ? selSessionId : std::wstring{};
        restoreWakeBegun = true;
        _BeginRestoreWake(std::move(wakeCandidates), focusSessionId);
    }

    // Agentmaster (the RESTORE WAKE — PERSISTENCE.md §14; the pure half is AgentMaster/RestoreWake.h): enter this window in
    // the process-wide wake barrier at window CREATION — SetAgentmasterWindowId, which TerminalWindow::Initialize calls for
    // a window the Emperor reopens from a saved record. The Emperor dispatches every reopened window before the first one
    // lays out, so every reopened window is a waiting entry before any restore ends: the first window to finish its
    // re-home cannot wake its busy tabs while another window is still restoring.
    //   key — the Emperor-assigned record id (empty is a no-op).
    // Never throws: a failure is logged (Rule #18), and the window then simply enters the barrier at its join.
    void TerminalPage::_AnnounceRestoreWake(const std::wstring& key) noexcept
    {
        if (key.empty())
        {
            return;
        }
        try
        {
            _restoreWake.key = key;
            ::Agentmaster::SharedRestoreWakeBarrier().Announce(key, ::GetTickCount64());
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[restore-wake] window " + key + L": announced - it restores a saved record; every reopened window's busy tabs wait until its focused tab launched\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AnnounceRestoreWake");
        }
    }

    // Agentmaster (the RESTORE WAKE): the end of this window's re-home — join the barrier and start the per-window wait.
    // The wait (_TickRestoreWake, every kRestoreWakeTickMs): the tab this window opens to finishes launching (its claude
    // fired a hook after this instant, or a terminal that hosts no claude started, or there is no terminal) → the barrier is
    // told → once EVERY window in the barrier launched (or its 5-minute cap expired) and this window's UI thread is idle,
    // the candidates that still want a wake are woken one by one, each on a slot of the process-wide RestoreWakePacer
    // (500 ms apart, at most 4 per 10 s across every window).
    //   candidates     — the re-homed sessions RestoreWakeWantsSession picked, strip order (empty is fine: the window still
    //                    holds the other windows back until its own focused tab launched, then leaves).
    //   focusSessionId — the managed session the restore focused and this window hosts, or "" (the gate then watches
    //                    whatever the window shows).
    // Never throws: a failure is logged and leaves the barrier (_EndRestoreWake), so no window waits on this one.
    void TerminalPage::_BeginRestoreWake(std::vector<std::wstring> candidates, const std::wstring& focusSessionId) noexcept
    {
        try
        {
            if (_restoreWake.key.empty())
            {
                // Not announced: the one default window that front-pops a saved record (the "No" answer to the reopen prompt,
                // or a single record restored without -s) enters the barrier here, under its own id.
                _restoreWake.key = _windowId;
            }
            const uint64_t nowTick = ::GetTickCount64();
            _restoreWake.candidates = std::move(candidates);
            _restoreWake.focusSessionId = focusSessionId;
            _restoreWake.state = {};
            // Both clocks at the same instant: the wall clock to tell a hook from the claude this restore started from an
            // older one (RestoreWakeForegroundStep), the monotonic one for every cap.
            _restoreWake.state.restoredAtUnixMs = ::Agentmaster::RestoreWakeUnixNowMs();
            _restoreWake.state.restoredAtTickMs = nowTick;
            _restoreWake.state.lastTickMs = nowTick;
            auto& barrier = ::Agentmaster::SharedRestoreWakeBarrier();
            barrier.Join(_restoreWake.key, nowTick);

            // One line names every candidate with the state it holds now (Idle = a normalized Running / Error, the
            // midTurnAtRestore flag is what made it a candidate) and where the barrier stands.
            std::wstring list;
            for (const auto& id : _restoreWake.candidates)
            {
                std::wstring state{ L"?" };
                if (const auto s = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(id) : std::nullopt)
                {
                    state = ::Agentmaster::ToString(s->state);
                }
                list += L" " + ::Agentmaster::ShortId(id) + L"(" + state + L")";
            }
            const auto counts = barrier.Counts();
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[restore-wake] window " + _restoreWake.key + L": restored; " + std::to_wstring(_restoreWake.candidates.size()) +
                                              L" busy tab(s) to wake once the tab every reopened window opens to has launched" +
                                              (list.empty() ? std::wstring{} : (L":" + list)) +
                                              L"; focus=" + (focusSessionId.empty() ? std::wstring{ L"(the current foreground)" } : ::Agentmaster::ShortId(focusSessionId)) +
                                              L"; barrier: " + std::to_wstring(counts.waiting) + L" window(s) waiting, " + std::to_wstring(counts.joined) +
                                              L" restored, " + std::to_wstring(counts.launched) + L" launched\n");

            if (!_restoreWakeTimer)
            {
                _restoreWakeTimer = WUX::DispatcherTimer{};
                _restoreWakeTimer.Interval(std::chrono::milliseconds(::Agentmaster::kRestoreWakeTickMs));
                // Weak page + the sender (never a capture of the timer itself, which would never be freed — the
                // self-capturing-handler Gotcha).
                _restoreWakeTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
                    if (auto self = weak.get())
                    {
                        self->_TickRestoreWake();
                    }
                    else if (const auto t = sender.try_as<WUX::DispatcherTimer>())
                    {
                        t.Stop(); // the page is gone: stop ticking (UI thread, safe)
                    }
                });
            }
            _restoreWakeTimer.Start();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_BeginRestoreWake");
            _EndRestoreWake(L"its wait could not start");
        }
    }

    // Agentmaster (the RESTORE WAKE): one tick of this window's wait — the punctuality streak, then the foreground gate
    // (the barrier is told exactly once, the tick the gate latches), then the wake decision. A window with no candidate
    // leaves as soon as its own foreground launched: it held the other windows back for exactly as long as it had to. In
    // the WAKING phase (after the release) the tick only drains the wake queue through the pacer.
    // Never throws: a failure is logged and ends the job (leaving the barrier).
    void TerminalPage::_TickRestoreWake() noexcept
    {
        try
        {
            const uint64_t nowTick = ::GetTickCount64();
            if (_restoreWake.waking)
            {
                _StepRestoreWakeQueue(nowTick); // released already: one paced wake per free slot
                return;
            }
            if (_restoreWake.key.empty())
            {
                _EndRestoreWake({}); // the job ended on another path (the barrier left): only stop ticking
                return;
            }
            auto& barrier = ::Agentmaster::SharedRestoreWakeBarrier();
            auto& st = _restoreWake.state;
            ::Agentmaster::RestoreWakeNoteTick(st, nowTick);
            if (!st.foregroundLaunched)
            {
                const auto why = ::Agentmaster::RestoreWakeForegroundStep(st, _ReadRestoreWakeForeground(), nowTick);
                if (!why)
                {
                    return; // the tab this window opens to is still launching
                }
                barrier.NoteForegroundLaunched(_restoreWake.key, nowTick);
                const auto counts = barrier.Counts();
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[restore-wake] window " + _restoreWake.key + L": its focused tab finished launching (" +
                                                  ::Agentmaster::RestoreWakeForegroundReasonText(*why) + L") " + std::to_wstring(nowTick - st.restoredAtTickMs) +
                                                  L" ms after its restore; " + std::to_wstring(_restoreWake.candidates.size()) + L" busy tab(s) to wake; barrier: " +
                                                  std::to_wstring(counts.waiting) + L" window(s) waiting, " + std::to_wstring(counts.joined) + L" restored, " +
                                                  std::to_wstring(counts.launched) + L" launched\n");
                if (_restoreWake.candidates.empty())
                {
                    _EndRestoreWake(L"no busy tab to wake here, and its focused tab has launched");
                    return;
                }
            }
            const bool released = barrier.IsReleased(_restoreWake.key, nowTick);
            if (const auto wake = ::Agentmaster::RestoreWakeDecideWake(st, nowTick, released))
            {
                _WakeRestoredBusyTabs(*wake);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TickRestoreWake");
            _EndRestoreWake(L"its wait failed");
        }
    }

    // Agentmaster (the RESTORE WAKE): the gate's facts about what this window opens to, right now. The restore-focused
    // session's own control while this window still hosts it under that id — with shells in the record the selection lands
    // only after the async shell replay, and a shell created meanwhile must not count for it — else whatever the window
    // shows: the Agent Manager page, no tab or a tab without a terminal (the Settings tab) read as "nothing to wait for".
    // The session's newest hook comes from the lean registry read (no queue copied).
    // Returns the facts; never throws: a failed read is logged (Rule #18) and answers "nothing to wait for", so a broken
    // read can delay no window.
    ::Agentmaster::RestoreWakeForeground TerminalPage::_ReadRestoreWakeForeground() noexcept
    {
        ::Agentmaster::RestoreWakeForeground fg;
        try
        {
            TermControl control{ nullptr };
            std::wstring sessionId;
            if (!_restoreWake.focusSessionId.empty() && _claudeTabs.find(_restoreWake.focusSessionId) != _claudeTabs.end())
            {
                sessionId = _restoreWake.focusSessionId;
                control = _ControlForSession(sessionId);
            }
            else if (!_managerPageVisible)
            {
                if (const auto tab = _GetFocusedTab())
                {
                    control = _GetActiveControl();
                    sessionId = _ClaudeSessionForTab(tab);
                }
            }
            if (!control)
            {
                return fg; // nothing launches in this window's foreground: nothing to wait for
            }
            fg.terminal = true;
            fg.started = control.ConnectionState() != TerminalConnection::ConnectionState::NotConnected;
            if (!sessionId.empty() && _sessionRegistry)
            {
                if (const auto s = _sessionRegistry->GetWithoutQueue(sessionId); s && s->kind == ::Agentmaster::AgentKind::Claude)
                {
                    fg.claudeSession = true; // a Claude tab counts as launched only once its claude is up (a hook)
                    fg.lastHookUnixMs = s->lastHookUnixMs;
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ReadRestoreWakeForeground");
            fg = {};
        }
        return fg;
    }

    // Agentmaster (the RESTORE WAKE): the barrier released this window and its UI is idle (or stayed busy too long) — wake
    // its busy tabs. Every candidate is checked again on its FRESH record (RestoreWakeWantsSession) and must still be hosted
    // here: the user may have opened it meanwhile (started), closed or moved it, or the scanner may have proved it at rest
    // (an unpinned Waiting-for-you, Done) or found it running in another process (Running). One log line names who wakes and
    // who is skipped, and why. The window then LEAVES the barrier (no other window waits on it any more) and enters the
    // WAKING phase: its timer wakes the queued tabs one by one, each on a slot of the process-wide pacer
    // (_StepRestoreWakeQueue), and the first one right now when a slot is free.
    //   reason — why now (the idle UI, or the idle wait's cap), for the log.
    // Never throws: a failure is logged and ends the job.
    void TerminalPage::_WakeRestoredBusyTabs(::Agentmaster::RestoreWakeWakeReason reason) noexcept
    {
        try
        {
            std::vector<std::wstring> wake;
            std::wstring woken;
            std::wstring skipped;
            size_t skippedCount = 0;
            for (const auto& id : _restoreWake.candidates)
            {
                const auto s = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(id) : std::nullopt;
                const bool hosted = _claudeTabs.find(id) != _claudeTabs.end();
                if (s && hosted && ::Agentmaster::RestoreWakeWantsSession(*s))
                {
                    wake.push_back(id);
                    woken += L" " + ::Agentmaster::ShortId(id) + L"(" + ::Agentmaster::ToString(s->state) + L")";
                    continue;
                }
                ++skippedCount;
                std::wstring why;
                if (!s)
                {
                    why = L"gone";
                }
                else if (!hosted)
                {
                    why = L"no longer in this window";
                }
                else if (s->started)
                {
                    why = L"running already";
                }
                else if (!s->live)
                {
                    why = L"closed";
                }
                else
                {
                    why = L"now " + ::Agentmaster::ToString(s->state);
                }
                skipped += L" " + ::Agentmaster::ShortId(id) + L"(" + why + L")";
            }
            const uint64_t nowTick = ::GetTickCount64();
            const auto release = ::Agentmaster::SharedRestoreWakeBarrier().LastRelease();
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[restore-wake] window " + _restoreWake.key + L": waking " + std::to_wstring(wake.size()) + L" busy tab(s) " +
                                              std::to_wstring(nowTick - _restoreWake.state.restoredAtTickMs) + L" ms after its restore (" +
                                              ::Agentmaster::RestoreWakeReleaseText(release) + L"; " + ::Agentmaster::RestoreWakeWakeReasonText(reason) +
                                              L"; paced across every window: " + std::to_wstring(::Agentmaster::kRestoreWakeSpacingMs) + L" ms apart, at most " +
                                              std::to_wstring(::Agentmaster::kRestoreWakeBatchSize) + L" per " + std::to_wstring(::Agentmaster::kRestoreWakeBatchPeriodMs / 1000) + L" s)" +
                                              (woken.empty() ? std::wstring{} : (L":" + woken)) +
                                              (skippedCount == 0 ? std::wstring{} : (L"; skipped " + std::to_wstring(skippedCount) + L":" + skipped)) + L"\n");
            if (wake.empty())
            {
                _EndRestoreWake({}); // nothing left to wake (all opened, closed or at rest meanwhile)
                return;
            }
            // The WAKING phase. Leave the barrier first (no window waits on this one any more), keep the timer.
            _LeaveRestoreWake({});
            _restoreWake.candidates.clear();
            _restoreWake.wakeQueue = std::move(wake);
            _restoreWake.woken = 0;
            _restoreWake.skippedLate = 0;
            _restoreWake.waking = true;
            _StepRestoreWakeQueue(nowTick); // the first wake now, when the pacer has a slot
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_WakeRestoredBusyTabs");
            _EndRestoreWake(L"its wake failed");
        }
    }

    // Agentmaster (the RESTORE WAKE): one step of the WAKING phase, on every timer tick. The queued sessions that no longer
    // want a wake by their turn (the user opened one, it closed, or the scanner proved it at rest) are dropped without a
    // pacing slot; the first one that still wants it takes a slot of the process-wide RestoreWakePacer and is woken in
    // place (_ActivateDormantSession — the "Activate Tab" path: the terminal initializes at the tab-content size, its
    // claude --resume starts, SessionInfo::started flips). At most one wake per tick (the pacer's spacing is two ticks
    // anyway); no slot → the next tick tries again. An empty queue logs the total and ends the job.
    //   nowTickMs — the monotonic clock now (the tick's).
    // Never throws: a failure is logged and ends the job.
    void TerminalPage::_StepRestoreWakeQueue(uint64_t nowTickMs) noexcept
    {
        try
        {
            auto& queue = _restoreWake.wakeQueue;
            while (!queue.empty())
            {
                const std::wstring id = queue.front();
                const auto s = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(id) : std::nullopt;
                const bool hosted = _claudeTabs.find(id) != _claudeTabs.end();
                if (!s || !hosted || !::Agentmaster::RestoreWakeWantsSession(*s))
                {
                    queue.erase(queue.begin()); // opened, closed, moved or at rest since the plan: no slot spent
                    ++_restoreWake.skippedLate;
                    continue;
                }
                if (!::Agentmaster::SharedRestoreWakePacer().TryTake(nowTickMs))
                {
                    return; // paced: another window (or this one) took the last slot — the next tick tries again
                }
                queue.erase(queue.begin());
                if (_ActivateDormantSession(id)) // logs [nav] activate-dormant <sid8> and flips SessionInfo::started
                {
                    ++_restoreWake.woken;
                }
                else
                {
                    ++_restoreWake.skippedLate; // its control could not start (not hosted any more / not dormant): the slot is spent, rarely
                }
                return; // one wake per tick
            }
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[restore-wake] window " + _windowId + L": done - woke " + std::to_wstring(_restoreWake.woken) +
                                              L" busy tab(s)" + (_restoreWake.skippedLate == 0 ? std::wstring{} : (L", " + std::to_wstring(_restoreWake.skippedLate) + L" skipped on their turn (opened, closed or at rest meanwhile)")) +
                                              L", " + std::to_wstring(nowTickMs - _restoreWake.state.restoredAtTickMs) + L" ms after its restore\n");
            _EndRestoreWake({});
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_StepRestoreWakeQueue");
            _EndRestoreWake(L"its wake failed");
        }
    }

    // Agentmaster (the RESTORE WAKE): end this window's job — stop the timer (UI-affine: this runs on the UI thread, from
    // the tick or the begin), leave the barrier when still in it, forget the candidates, the focus and the wake queue.
    //   why — logged when non-empty and the window was still in the barrier (the reason it left without waking: nothing
    //         to wake, a failure).
    // Never throws: a failed Stop is logged and the leave still runs, so the barrier never keeps a dead entry.
    void TerminalPage::_EndRestoreWake(std::wstring_view why) noexcept
    {
        try
        {
            if (_restoreWakeTimer)
            {
                _restoreWakeTimer.Stop();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_EndRestoreWake (stop the timer)");
        }
        _LeaveRestoreWake(why);
        _restoreWake.candidates.clear();
        _restoreWake.focusSessionId.clear();
        _restoreWake.wakeQueue.clear();
        _restoreWake.waking = false;
    }

    // Agentmaster (the RESTORE WAKE): leave the process-wide barrier and forget the key, so a second leave is a no-op.
    // Thread-safe and free of XAML: ~TerminalPage calls it from whatever thread the page dies on (the fire_and_forget
    // teardown class), which is why it stays bare there, like the engine detaches.
    //   why — logged when non-empty and the window was still in the barrier.
    // Never throws: a failure is logged (Rule #18) and the key is cleared anyway; the barrier's cap then covers a lost
    // leave.
    void TerminalPage::_LeaveRestoreWake(std::wstring_view why) noexcept
    {
        if (_restoreWake.key.empty())
        {
            return; // never announced, or already left
        }
        try
        {
            ::Agentmaster::SharedRestoreWakeBarrier().Leave(_restoreWake.key, ::GetTickCount64());
            if (!why.empty())
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restore-wake] window " + _restoreWake.key + L": left the wake barrier - " + std::wstring{ why } + L"\n");
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_LeaveRestoreWake");
        }
        _restoreWake.key.clear();
    }

    // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): the Manager's "Reopen Windows (N)" recover
    // button — reopen every saved window that is NOT currently open. This is the runtime analog of the
    // WindowEmperor's startup reopen loop: each recoverable record is reopened via `wt -w -1 -s <idx>`
    // (a new window, with the record's canonical sorted index as the persisted-layout index), which
    // hands off to this same Emperor process -> a new AppHost -> TerminalWindow resolves records[idx]
    // for geometry and TerminalPage claims it by id (geometry + lens agree). RecoverableWindows()
    // already pairs each not-open record with that index, so we just dispatch one wt per entry.
    // Agentmaster: what a reopen dispatch ShellExecutes. Packaged: OUR identity's execution
    // alias leaf (release = agentmaster.exe, AgentmasterDev = agentmasterdev.exe — distinct on
    // purpose, so two side-by-side installs can never reopen windows into each other; resolved
    // by NAME through the WindowsApps PATH dir + APPEXECLINK reparse). Unpackaged (portable
    // zip): the alias doesn't exist — launch the neighbor Agentmaster.exe directly; its
    // wWinMain routes through the same single-instance handoff (previously this case silently
    // no-opped on the missing alias).
    static std::wstring _AgentmasterReopenTarget()
    {
        if (!::Agentmaster::Profiles::PackageFamilyName().empty())
        {
            return ::Agentmaster::Profiles::IsDevPackage() ? L"agentmasterdev.exe" : L"agentmaster.exe";
        }
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = ::GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
        if (n > 0 && n < ARRAYSIZE(buf))
        {
            try
            {
                std::filesystem::path exe{ std::wstring{ buf, n } };
                exe.replace_filename(L"Agentmaster.exe");
                return exe.wstring();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_AgentmasterReopenTarget resolve");
            }
        }
        return L"agentmaster.exe"; // last-resort: the historical by-name launch
    }

    safe_void_coroutine TerminalPage::_ReopenSavedWindows()
    {
        // Everything before the first co_await runs on the UI thread; a throw here would escape into
        // the ContentDialog button-click handler and, because this is a safe_void_coroutine
        // (suspend_never initial-suspend), land in its unhandled_exception -> assert(false) in Debug.
        // So guard the prep explicitly and never let it throw.
        std::vector<::Agentmaster::RecoverableWindow> recoverable;
        try
        {
            // Consistent snapshot of records-minus-live (the not-currently-open windows to reopen).
            recoverable = ::Agentmaster::RecoverableWindows();
        }
        CATCH_LOG();

        // Our packages register per-IDENTITY execution aliases (release = agentmaster.exe,
        // AgentmasterDev = agentmasterdev.exe; Package-Rel/-Dev.appxmanifest); GetWtExePath() once
        // assumed a wt.exe/wtd.exe alias and returned a NON-EXISTENT <PFN>\wt.exe (so the ShellExecute
        // would silently no-op — the original bug). ShellExecute OUR alias BY NAME: the alias dir
        // (%LOCALAPPDATA%\Microsoft\WindowsApps) is on PATH, and ShellExecuteExW resolves it + follows
        // the APPEXECLINK reparse to our packaged app; the single-instance handoff then routes
        // `-s <idx>` back to the running Emperor (verified: claims the record + restores its geometry).
        // Launch by NAME, NOT the full reparse-point path — a full-path launch can bypass the alias
        // resolution and cascade a fresh, record-less window instead. The distinct per-identity names
        // also guarantee a side-by-side release+dev pair never reopens windows into EACH OTHER.
        const std::wstring exePath = _AgentmasterReopenTarget();

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[reopen] recoverable=" + std::to_wstring(recoverable.size()) + L" exe=" + exePath + L"\n");
        if (recoverable.empty())
        {
            co_return;
        }
        // Nav audit BEGIN: the user clicked the toolbar "Reopen Windows (N)" — reopen each saved-but-not-open
        // window via our alias (`-w -1 -s <idx>`). The per-dispatch [reopen] ok= lines are the granular end;
        // a reopen-windows-begin with no reopen-windows-done pinpoints a crash mid-loop.
        ::Agentmaster::LogNav(L"reopen-windows-begin count=" + std::to_wstring(recoverable.size()));

        // ShellExecuteExW may block, so dispatch from a background thread (NOTE: don't touch `this`
        // past here — everything below is local, mirroring _OpenNewWindow).
        co_await winrt::resume_background();

        for (const auto& rw : recoverable)
        {
            try
            {
                // `-w -1` forces a brand-new window; `-s <idx>` is the global persisted-layout index our
                // TerminalWindow/TerminalPage read to restore geometry + claim the record by id. The
                // single-instance handoff routes this back to the running Emperor -> the same reopen path.
                const std::wstring cmdline = L"-w -1 -s " + std::to_wstring(rw.index);

                SHELLEXECUTEINFOW seInfo{ 0 };
                seInfo.cbSize = sizeof(seInfo);
                seInfo.fMask = SEE_MASK_NOASYNC;
                seInfo.lpVerb = L"open";
                seInfo.lpFile = exePath.c_str();
                seInfo.lpParameters = cmdline.c_str();
                seInfo.nShow = SW_SHOWNORMAL;
                const auto ok = ShellExecuteExW(&seInfo);
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[reopen] dispatch -s " + std::to_wstring(rw.index) + L" ok=" + (ok ? std::wstring{ L"1" } : std::wstring{ L"0" }) + L"\n");
            }
            CATCH_LOG();
        }
        ::Agentmaster::LogNav(L"reopen-windows-done dispatched=" + std::to_wstring(recoverable.size())); // END (pairs with reopen-windows-begin); runs on the background thread — LogNav is thread-safe + touches no `this`

        co_return;
    }

    // Agentmaster (M10 window-grouped restore): reopen ONE saved window by its canonical sorted record
    // index. Mirrors a single iteration of _ReopenSavedWindows (the recover-button loop): ShellExecute
    // our execution alias BY NAME with `-w -1 -s <idx>` so the single-instance handoff routes it back to
    // the running Emperor, which resolves records[idx] for geometry + claims it by id (geometry + lens +
    // its re-homed tabs agree). Fired by the per-window "Reopen window" button in the grouped Archived
    // overlay. `index` comes from RecoverableWindows() (computed in the content), so it is already valid.
    safe_void_coroutine TerminalPage::_ReopenSavedWindow(int index)
    {
        if (index < 0)
        {
            co_return;
        }
        // ShellExecuteExW may block — dispatch off the UI thread (don't touch `this` past here).
        co_await winrt::resume_background();
        try
        {
            const std::wstring cmdline = L"-w -1 -s " + std::to_wstring(index);
            const std::wstring exePath = _AgentmasterReopenTarget(); // OUR per-identity alias, BY NAME (see _ReopenSavedWindows)
            SHELLEXECUTEINFOW seInfo{ 0 };
            seInfo.cbSize = sizeof(seInfo);
            seInfo.fMask = SEE_MASK_NOASYNC;
            seInfo.lpVerb = L"open";
            seInfo.lpFile = exePath.c_str();
            seInfo.lpParameters = cmdline.c_str();
            seInfo.nShow = SW_SHOWNORMAL;
            const auto ok = ShellExecuteExW(&seInfo);
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[reopen] dispatch one -s " + std::to_wstring(index) + L" ok=" + (ok ? std::wstring{ L"1" } : std::wstring{ L"0" }) + L"\n");
        }
        CATCH_LOG();
        co_return;
    }
}
