// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — per-window workspace persistence (M10; doc/agentmaster/PERSISTENCE.md):
// capture this window's WindowRecord from LIVE state (geometry via the PersistState
// recipe + ordered tab refs + the Manager lens), the debounced autosave + close-flush,
// re-home a reopened window's whole workspace (_RestoreWindowTabs: resume Claude
// sessions + replay shell tabs, in record order), and the "Reopen window(s)" dispatch
// (`agentmaster.exe -w -1 -s <idx>` via the execution alias BY NAME).
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // IsValidDirectory + GuidToPlainString (shell-tab cwd capture)
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

    // Agentmaster: a WT tab color -> "#RRGGBB" (alpha dropped — tab colors are opaque). Mirrors
    // AgentSessions.cpp's ClaudeColorToHex; kept TU-local like the codebase's other per-file color
    // converters (HexToColor / SessHexToColor / ClaudeColorToHex). Used to persist the Manager tab's
    // per-window color into the window record.
    static std::wstring _WindowRecordColorToHex(const winrt::Windows::UI::Color& c)
    {
        wchar_t buf[8];
        ::swprintf(buf, 8, L"#%02X%02X%02X", static_cast<unsigned>(c.R), static_cast<unsigned>(c.G), static_cast<unsigned>(c.B));
        return buf;
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
        // into rec.tabs (selectedTabIndex, the fallback). Manager / none leaves both unset; the Manager
        // tab is re-created at index 0, the natural default. On reopen the window re-selects this tab so
        // closing/reopening preserves the ACTIVE tab, not just the set of tabs.
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
            if ((_managerTab && tab == _managerTab) || (_settingsTab && tab == _settingsTab))
            {
                continue; // the pinned Manager tab is re-created; the Settings tab is transient
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

        // --- Manager tab's per-window color ---
        // The pinned Manager tab is a per-window singleton with no working dir, so its user-chosen color
        // is persisted HERE in the window record (NOT the dir-color map — Rule #12 is dir-keyed). Read it
        // LIVE like geometry; if the Manager tab is gone (a teardown flush has no live tab), keep the value
        // already copied from _windowRecord rather than wiping a good color (the geometry-fallback idiom).
        if (_managerTab)
        {
            if (const auto mgr = _GetTabImpl(_managerTab))
            {
                const auto c = mgr->GetRuntimeTabColor();
                rec.managerTabColor = c ? _WindowRecordColorToHex(*c) : std::wstring{};
            }
        }

        return rec;
    }

    // Agentmaster (M10): the debounced autosave trigger. No-ops until startup completes so the
    // tabs/geometry churned during _OnFirstLayout don't write a half-built record.
    void TerminalPage::_ScheduleWindowRecordSave()
    {
        if (_startupState != StartupState::Initialized || _windowId.empty())
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
    void TerminalPage::_FlushWindowRecord()
    {
        if (_windowId.empty())
        {
            return; // engine not initialized (no Manager tab) — nothing to persist
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
        const bool managerOnly = rec.tabs.empty(); // capture skips the Manager/Settings tabs -> empty == nothing but Manager
        // Agentmaster (discard Manager-only windows): a window the user emptied to just the pinned
        // Manager tab must NOT persist as a restorable window (the self-close requirement). When
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
        //      still has geometry, so an empty-but-positioned window (just the Manager tab) still saves
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

    // Agentmaster (discard Manager-only windows): true iff this window holds nothing but the pinned,
    // non-closable Manager tab. A Settings tab (or any terminal tab) counts as real content, so the
    // window is "Manager-only" only when the Manager tab is its sole tab. _managerTab is null on a
    // window without one (a torn-out / pre-init shell), which is never Manager-only here.
    bool TerminalPage::_IsManagerOnlyWindow() const
    {
        if (!_managerTab)
        {
            return false;
        }
        for (const auto& tab : _tabs)
        {
            if (tab != _managerTab)
            {
                return false; // a real tab (terminal / Settings) is present
            }
        }
        return true;
    }

    // Agentmaster (discard Manager-only windows): the debounced decision. A window that ends up holding
    // only the pinned Manager tab — by closing/tearing-out its last terminal tab, or by reopening from
    // an empty/failed record — must not linger as a restorable, content-less window. Unless it is the
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
    // with just the pinned Manager tab; this rebuilds the rest from the record's ORDERED tab refs, in
    // place, so "close the window -> reopen it" brings the whole workspace back — not just geometry +
    // lens. Per ref, in record order:
    //   • Claude -> resume its session INTO this window (claude --resume, the archived record's queue +
    //     autopilot intact) via _LaunchClaudeSession — the Archived-button path, targeted here. Lazy-
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
    void TerminalPage::_RestoreWindowTabs()
    {
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
        const int selTabIndex = _windowRecord.selectedTabIndex; // shell-tab fallback; -1 = Manager/none
        const bool wantManager = selSessionId.empty() && selTabIndex < 0;
        size_t resumed = 0, shells = 0, skipped = 0;
        // The focused tab's ABSOLUTE index in _tabs after re-home (Manager is index 0, then the Claude
        // tabs in creation order, then the shell tabs). Computed LIVE from what actually got created —
        // the persisted identity is the stable sessionId (Claude) / record index (shell), never a live
        // position. -1 => unresolved (the selected session couldn't be restored) => leave default focus.
        int targetAbs = wantManager ? 0 : -1;

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

        // Pass 1 — managed sessions (Claude + Codex), synchronously, in record order.
        for (const auto& entry : tabs)
        {
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
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[rehome] window " + _windowId + L" skip " + ::Agentmaster::ShortId(entry.sessionId) +
                                                  (info ? std::wstring{ L" (already live elsewhere)" } : std::wstring{ L" (unknown \x2014 fleet not loaded / pruned)" }) + L"\n");
                ++skipped; // unknown (fleet not loaded yet / pruned) or already open elsewhere
                continue;
            }
            // Kind-aware re-home: Codex resumes via `codex resume <uuid>` (rollout-gated on
            // SessionInfo.codexSessionId), Claude via `claude --resume <id>`. _LaunchClaudeSession resumes
            // the NEWEST conversation in this ref's continuation chain (Claude splits ids on /clear,
            // /compact, the plan->implement transition) and returns null when that conversation is ALREADY
            // hosted by an earlier ref this restore — two archived refs that chained to one live
            // conversation (a tab can't be two sessions). A null is NOT a new tab, so it must not advance
            // the resumed/focus accounting — count it skipped.
            const auto homed = isCodex
                ? _LaunchCodexSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info)
                : _LaunchClaudeSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
            if (!homed)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[rehome] window " + _windowId + L" skip " + ::Agentmaster::ShortId(entry.sessionId) + L" (dedup: conversation already hosted by an earlier ref)\n");
                ++skipped;
                continue;
            }
            if (!selSessionId.empty() && entry.sessionId == selSessionId)
            {
                targetAbs = 1 + static_cast<int>(resumed); // this managed tab's index (Manager occupies 0)
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
                continue;
            }
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
                            targetAbs = 1 + static_cast<int>(resumed) + static_cast<int>(shells); // this shell's index
                        }
                        ++shells;
                    }
                }
            }
            CATCH_LOG();
        }

        // Re-select the focused tab. With shells (created ASYNC by ProcessStartupActions) we append a
        // SwitchToTab as the batch's LAST action so it runs AFTER the shells exist and overrides the
        // "focus the tab I just made" that each newTab does. With no shells the strip is already settled,
        // so select synchronously. targetAbs < 0 (selected session couldn't be restored) => leave default.
        if (!shellActions.empty())
        {
            if (targetAbs >= 0)
            {
                ActionAndArgs sel;
                sel.Action(ShortcutAction::SwitchToTab);
                sel.Args(SwitchToTabArgs{ static_cast<uint32_t>(targetAbs) });
                shellActions.push_back(std::move(sel));
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

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[rehome] window " + _windowId + L" resumed=" + std::to_wstring(resumed) +
                                          L" shells=" + std::to_wstring(shells) + L" skipped=" + std::to_wstring(skipped) +
                                          L" select=" + std::to_wstring(targetAbs) +
                                          L" of " + std::to_wstring(tabs.size()) + L" refs\n");
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
    // zip): the alias doesn't exist — launch the neighbor WindowsTerminal.exe directly; its
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
                exe.replace_filename(L"WindowsTerminal.exe");
                return exe.wstring();
            }
            catch (...)
            {
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
