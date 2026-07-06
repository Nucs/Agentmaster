// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster (TAB_OVERLAY.md / DESIGN §9.7): the ONE shared "copy a session field to the
// clipboard" action behind BOTH the per-tab link badge's copy menu (AgentTabOverlay) AND the
// Triage Board / Explorer-tree session menu's Copy submenu (AgentManagerContent). The
// implementation lives in AgentTabOverlay.cpp (where the launch-CLI / transcript / summary
// helpers already are); declaring it here lets the Manager content reuse the exact same path so
// the two copy menus can never drift apart.

#pragma once

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
    // wrapNewlines / truncate are the GLOBAL summary-panel flags (AppSettings::summaryPanelWrapNewlines /
    // summaryPanelTruncate) that govern how the Summary (case 6) renders its messages; they are
    // ignored by the other cases. tabColorMode is the GLOBAL AppSettings::tabColorMode as an int
    // (this header stays engine-include-free): case 1 (Path) resolves the session's EFFECTIVE work
    // dir through it (EffectiveWorkingDir — the INFERRED dir under InferredWorkingDirectory, else
    // the launch cwd), matching the overlay subline / board card / tree group; the default 0
    // (WorkingDirectory) reproduces the prior launch-cwd copy. A no-op for an unknown session id or
    // an empty field. The clipboard write happens on the UI thread (cases 0-4 synchronously; 5/6
    // hop back via `dispatcher`), so call this from the UI thread and pass that thread's
    // DispatcherQueue.
    void CopySessionField(::Agentmaster::SessionRegistry& registry,
                          const std::wstring& sessionId,
                          int which,
                          const winrt::Windows::System::DispatcherQueue& dispatcher,
                          bool wrapNewlines,
                          bool truncate,
                          int tabColorMode = 0);
}
