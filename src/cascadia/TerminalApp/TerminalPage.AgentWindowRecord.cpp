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

#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog
#include "AgentMaster/Engine.h" // RecoverableWindows (the reopen dispatch)
#include "AgentMaster/Persistence.h" // SaveWindowRecord
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
                entry.kind = ::Agentmaster::TabKind::Claude;
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
                // Prefer the stable Claude conversation id; a shell tab (no cross-restart id) falls back
                // to its index in rec.tabs. Read entry BEFORE the move below.
                if (entry.kind == ::Agentmaster::TabKind::Claude && !entry.sessionId.empty())
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
        //  (2) a capture with NEITHER content tabs NOR geometry is an un-laid-out window — keep the
        //      record on disk (the live window's real save follows). A legitimately session-less window
        //      still has geometry, so an empty-but-positioned window (just the Manager tab) still saves.
        if (rec.tabs.empty() && !rec.geometry.hasPosition && !rec.geometry.hasSize)
        {
            return;
        }
        _windowRecord = std::move(rec);
        ::Agentmaster::SaveWindowRecord(_windowRecord);
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

        // Pass 1 — Claude sessions, synchronously, in record order.
        for (const auto& entry : tabs)
        {
            if (entry.kind != ::Agentmaster::TabKind::Claude || entry.sessionId.empty())
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
                ++skipped; // unknown (fleet not loaded yet / pruned) or already open elsewhere
                continue;
            }
            _LaunchClaudeSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
            if (!selSessionId.empty() && entry.sessionId == selSessionId)
            {
                targetAbs = 1 + static_cast<int>(resumed); // this Claude tab's index (Manager occupies 0)
            }
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

        // Our package registers the `agentmaster.exe` execution alias (Package-Dev.appxmanifest);
        // GetWtExePath() assumes a wt.exe/wtd.exe alias and returns a NON-EXISTENT <PFN>\wt.exe (so the
        // ShellExecute would silently no-op — the original bug). ShellExecute the alias BY NAME: the
        // alias dir (%LOCALAPPDATA%\Microsoft\WindowsApps) is on PATH, and ShellExecuteExW resolves it
        // + follows the APPEXECLINK reparse to our packaged app; the single-instance handoff then routes
        // `-s <idx>` back to the running Emperor (verified: claims the record + restores its geometry).
        // Launch by NAME, NOT the full reparse-point path — a full-path launch can bypass the alias
        // resolution and cascade a fresh, record-less window instead.
        const std::wstring exePath = L"agentmaster.exe";

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[reopen] recoverable=" + std::to_wstring(recoverable.size()) + L" exe=" + exePath + L"\n");
        if (recoverable.empty())
        {
            co_return;
        }

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
            const std::wstring exePath = L"agentmaster.exe"; // launch the alias BY NAME (see _ReopenSavedWindows)
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
