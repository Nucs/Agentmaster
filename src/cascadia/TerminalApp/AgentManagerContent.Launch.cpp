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
//   AgentManagerContent.Internal.h      - the ~48 shared file-local helpers: StateColor/Pill/StateDot/Text/Fill + path/sort utils (anonymous namespace, a per-TU copy)
//   AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
//   AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
//   AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
//   AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
// ★ AgentManagerContent.Launch.cpp      - the Launch bar's HOST half: _MakeLaunchBarHost, the forwarders, LaunchBoxText / SetLaunchBoxText (the bar itself: AgentLaunchBar.{h,cpp})
// ======================================================================================
//
// Agentmaster Manager tab: the LAUNCH bar's HOST half. The bar itself — the Claude/Codex agent toggle, the cwd box
// validation, launch / create / fork and the focus-triggered path-picker drop-down (recent dirs + subfolders +
// worktrees) — is the shared AgentLaunchBar (AgentLaunchBar.{h,cpp}); its code moved there VERBATIM from this file, so
// the side panel's "+" launcher card (SIDE_PANEL.md §2e) runs exactly the same code. What stays here is what only the
// Agent Manager content can answer: the host callbacks (_MakeLaunchBarHost), the three forwarders the content's other
// TUs call, and the box-text accessors the side panel's card mirrors the two boxes through. Partial TU of
// AgentManagerContent.cpp.
#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentLaunchBar.h" // AgentLaunchBar / AgentLaunchBarHost / AgentLaunchRecentDirs — the complete types the host build needs
#include "AgentMaster/SessionRegistry.h" // the registry getter hands the bar this content's shared registry

namespace winrt::TerminalApp::implementation
{
    AgentLaunchBarHost AgentManagerContent::_MakeLaunchBarHost()
    {
        AgentLaunchBarHost host;
        // The Agent Manager keeps its Claude <-> Codex toggle (the side panel's card is the Claude-only host).
        host.codexToggle = true;
        // The window's ONE recent-folders list (loaded by the constructor before _BuildLayout calls this).
        host.recentDirs = _launchRecentDirs;
        // The drop-down's parent and coordinate space: this content's root, top-left aligned in row 0 (the toolbar's
        // row) — where the drop-down block of _BuildLayout put it before the move. One row, as before.
        host.popupHost = _root;
        host.popupRowSpan = 1;
        // The "Couldn't create folder" dialog takes the forced-Dark root's island and theme (the old _root read).
        host.dialogAnchor = _root;
        host.dispatcher = _dispatcher;

        // Every callback holds the content WEAKLY (a bar never keeps its host alive — AgentLaunchBar.h, OWNERSHIP) and
        // reads the content's member when it RUNS, not now: the page wires the handlers and the registry AFTER the
        // constructor built the bar (_WireAgentManagerContent), and a cog Save replaces _appSettings. One difference
        // from the code before the move, unobservable once the page is wired: an action whose handler was still unset
        // used to skip its own folder create + recent push as well; now the bar does both and the host call does
        // nothing. The page wires every handler right after construction, before the page can show, so no click can
        // land in between.
        const auto weak = get_weak();
        host.registry = [weak]() -> std::shared_ptr<::Agentmaster::SessionRegistry> {
            if (const auto self = weak.get())
            {
                return self->_registry;
            }
            return nullptr; // the content is gone: the bar's id resolve falls back to the transcript
        };
        host.recentDirsLimit = [weak]() -> uint32_t {
            if (const auto self = weak.get())
            {
                return self->_appSettings.recentDirsLimit;
            }
            return 0u; // the bar maps 0 to the shipped 10
        };
        host.launchModels = [weak]() -> std::wstring {
            if (const auto self = weak.get())
            {
                return self->_appSettings.launchModels;
            }
            return {}; // "" => the hover menu offers Default + Specify… + the presets only
        };
        host.spawnClaude = [weak](const std::wstring& dir, const std::wstring& target) {
            const auto self = weak.get();
            if (self && self->_spawnHandler)
            {
                // (workingDir, title, model): an empty title lets the page derive one from the dir — what the old
                // _OnLaunch passed. `target` is "" (Default), a model id, or "@<presetId>" from the hover menu.
                self->_spawnHandler(winrt::hstring{ dir }, winrt::hstring{}, winrt::hstring{ target });
            }
        };
        host.resumeSession = [weak](const std::wstring& id, const std::wstring& dir, const std::wstring& title) {
            const auto self = weak.get();
            if (self && self->_resumeSessionHandler)
            {
                self->_resumeSessionHandler(winrt::hstring{ id }, winrt::hstring{ dir }, winrt::hstring{ title });
            }
        };
        host.forkSession = [weak](const std::wstring& id, const std::wstring& dir, const std::wstring& title) {
            const auto self = weak.get();
            if (self && self->_forkSessionHandler)
            {
                self->_forkSessionHandler(winrt::hstring{ id }, winrt::hstring{ dir }, winrt::hstring{ title });
            }
        };
        host.spawnCodex = [weak](const std::wstring& dir) {
            const auto self = weak.get();
            if (self && self->_codexLaunchHandler)
            {
                // pid 0 = no source external; a fresh, independent session: adopt=false, fork=false (the EXTERNAL
                // menu's "Open New Codex Session Here" shape, as the old _OnLaunch called it).
                self->_codexLaunchHandler(0, winrt::hstring{ dir }, false, false);
            }
        };
        host.claudeMissing = [weak]() {
            if (const auto self = weak.get())
            {
                self->_ShowClaudeMissing(); // the Manager's own "Claude not detected" modal (Browse… + `claude install`)
            }
        };
        // The "Specify…" opener for the hover menu. Taken once: it is itself a weak-capturing lambda that reaches the
        // content per call (_ModelSpecifyOpener), so it is the same opener the old per-hover call produced.
        host.specifyModel = _ModelSpecifyOpener();
        // PRESETS: the hover menu's "Add presets…" item opens the Manager's own help dialog. The bar decides when the
        // item shows (only while the user has no preset at all), so this is just the dialog.
        host.addPresets = [weak]() {
            if (const auto self = weak.get())
            {
                self->_ShowAddPresetsHelp();
            }
        };
        host.layoutChanged = [weak]() {
            if (const auto self = weak.get())
            {
                self->_ReflowLaunchBar(); // the toggle / Launch / Fork width changed: re-stage the toolbar row
            }
        };
        // launched / textEdited stay empty: the Agent Manager needs neither (nothing here closes after a launch, and
        // this box IS the text the side panel's card mirrors).
        return host;
    }

    void AgentManagerContent::_ClosePathPicker()
    {
        if (_launch)
        {
            _launch->ClosePathPicker();
        }
    }

    void AgentManagerContent::_QueuePathPickerRelayout()
    {
        if (_launch)
        {
            _launch->QueuePathPickerRelayout(); // coalesced to the next tick; never throws (logged, latch re-armed)
        }
    }

    void AgentManagerContent::_ValidateLaunchBox()
    {
        if (_launch)
        {
            _launch->ValidateLaunchBox();
        }
    }

    std::wstring AgentManagerContent::LaunchBoxText() const
    {
        return _launch ? _launch->BoxText() : std::wstring{};
    }

    void AgentManagerContent::SetLaunchBoxText(const std::wstring& text)
    {
        if (_launch)
        {
            // Writes only on a change (caret to the end), then re-validates explicitly: this page is usually hidden
            // while the side panel's card mirrors into it, and TextChanged is unreliable off the live tree. The box
            // is unfocused, so its TextChanged never opens the drop-down from here.
            _launch->SetBoxText(text);
        }
    }
}
