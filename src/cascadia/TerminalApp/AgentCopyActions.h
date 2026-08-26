// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster (TAB_OVERLAY.md / DESIGN §9.7): the ONE shared "copy a session field to the
// clipboard" action behind BOTH the per-tab link badge's copy menu (AgentTabOverlay) AND the
// Triage Board / Explorer-tree session menu's Copy submenu (AgentManagerContent). The
// implementation lives in AgentTabOverlay.cpp (where the launch-CLI / transcript / summary
// helpers already are); declaring it here lets the Manager content reuse the exact same path so
// the two copy menus can never drift apart.

#pragma once

#include <functional> // the optional live-draft provider (case 7)
#include <string>
#include <winrt/Windows.System.h> // DispatcherQueue

namespace Agentmaster
{
    class SessionRegistry;
}

namespace winrt::TerminalApp::implementation
{
    // Copy one of a session's "fields" to the system clipboard. `which` matches the per-tab overlay
    // copy menu's codes (and the Triage Board's Copy submenu, which routes through this same path):
    //   0 = Session Id            (the resumable conversation id; Codex: its rollout uuid)
    //   1 = Working-directory Path
    //   2 = Git Branch Name
    //   3 = Claude Launch CLI     (the REAL full command line — live PEB commandline or the would-use builder)
    //   4 = Codex Launch CLI      (the REAL full command line)
    //   5 = Transcript            (the whole conversation as text — user + assistant only — read off-thread)
    //   6 = Summary               (the FULL session-end.js box — analyzed off-thread)
    //   7 = Current Prompt        (the UNSENT draft in the session's input box — PENDING_INPUT.md; Claude only)
    //   8 = Transcript Followup   (the conversation folded per turn — a "<Legend>" header, then each turn as
    //                              ❯ the user message + ● the assistant's END-of-turn reply — the paste-into-a-
    //                              follow-up-session brief; ReadConversationFollowupText, read off-thread)
    // wrapNewlines / truncate are the GLOBAL summary-panel flags (AppSettings::summaryPanelWrapNewlines /
    // summaryPanelTruncate) that govern how the Summary (case 6) renders its messages; they are
    // ignored by the other cases. tabColorMode is the GLOBAL AppSettings::tabColorMode as an int
    // (this header stays engine-include-free): case 1 (Path) resolves the session's EFFECTIVE work
    // dir through it (EffectiveWorkingDir — the INFERRED dir under InferredWorkingDirectory, else
    // the launch cwd), matching the overlay subline / board card / tree group; the default 0
    // (WorkingDirectory) reproduces the prior launch-cwd copy. A no-op for an unknown session id or
    // an empty field. The clipboard write happens on the UI thread (cases 0-4/7 synchronously; 5/6
    // hop back via `dispatcher`), so call this from the UI thread and pass that thread's
    // DispatcherQueue.
    //
    // `liveDraft` is the OPTIONAL live-read provider for case 7 (ignored by every other case): a
    // caller that can reach this session's TermControl — i.e. the window that HOSTS its tab — passes a
    // callable that reads the input box out of the terminal buffer right now
    // (TerminalPage::_ReadLiveDraftForSession → ControlCore::ReadPendingInputDraft), wrapped so any
    // failure is just "". Omitted / empty / a throwing provider all degrade to the SAME fallback: the
    // observer's recorded SessionInfo::pendingInput (one scan tick old, or the persisted memory across
    // a restart). The choice itself is the pure, unit-tested PickCurrentPromptText (PendingInput.h), so
    // every copy menu resolves it identically — the Manager board can reference a session hosted in
    // ANOTHER window, where no live read is possible and the remembered value is the whole answer.
    void CopySessionField(::Agentmaster::SessionRegistry& registry,
                          const std::wstring& sessionId,
                          int which,
                          const winrt::Windows::System::DispatcherQueue& dispatcher,
                          bool wrapNewlines,
                          bool truncate,
                          int tabColorMode = 0,
                          const std::function<std::wstring()>& liveDraft = {});

    // Agentmaster (TAB_OVERLAY.md): open a session's EFFECTIVE working directory in explorer.exe — the
    // ONE shared "open path" action behind BOTH the per-tab overlay's folder button (Open Path) AND the
    // WT tab menu's "Open In Explorer" item, so the two can never resolve a DIFFERENT folder. It resolves
    // EffectiveWorkingDir(tabColorMode, s) — the INFERRED dir while the session infers, else the launch
    // cwd (the SAME resolution as Copy Path, case 1 above, and the overlay subline) — falling back to the
    // live PEB cwd, then opens it OFF-thread (ShellExecuteEx explorer.exe). A no-op for an unknown session
    // id or an empty dir; plays the same click chime and logs [nav] open-path. Call from the UI thread.
    // Works for Claude AND Codex (both have a working directory). Implemented in AgentTabOverlay.cpp
    // beside CopySessionField, so the tab-menu handler (page) links the exact same code the overlay runs.
    void OpenSessionFolder(::Agentmaster::SessionRegistry& registry, const std::wstring& sessionId, int tabColorMode);
}
