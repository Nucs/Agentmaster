// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster (TAB_OVERLAY.md / DESIGN §9.7): the ONE shared "copy a session field to the
// clipboard" action behind BOTH the per-tab link badge's copy menu (AgentTabOverlay) AND the
// Triage Board / Explorer-tree session menu's Copy submenu (AgentManagerContent). The
// implementation lives in AgentTabOverlay.cpp (where the launch-CLI / transcript / summary
// helpers already are); declaring it here lets the Manager content reuse the exact same path so
// the two copy menus can never drift apart. It also declares the shared "copy a PROMPT" action
// (CopyPromptTextExpandingPastes — paste placeholders expanded back to their content, PENDING_INPUT.md
// §8e) behind the Auto-Testing row menu's Copy and every Copy Current Prompt.

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
    //   7 = Current Prompt        (the UNSENT draft in the session's input box — PENDING_INPUT.md; Claude only;
    //                              a "[Pasted text #N +M lines]" placeholder in it is copied as the pasted
    //                              text itself — PENDING_INPUT.md §8e, CopyPromptTextExpandingPastes below)
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
    // an empty field. The clipboard write happens on the UI thread (cases 0-4 synchronously; 5/6/8
    // read off-thread and hop back via `dispatcher`; 7 is synchronous unless the draft carries a
    // paste placeholder, which CopyPromptTextExpandingPastes — below — expands off-thread first and
    // then hops back the same way), so call this from the UI thread and pass that thread's
    // DispatcherQueue. Every call also SUPERSEDES a prompt copy whose paste expansion is still in
    // flight (see CopyPromptTextExpandingPastes), so a slow expansion can never land on top of
    // whatever the user copied after it.
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

    // Agentmaster (PENDING_INPUT.md §8e): copy a PROMPT's text to the clipboard with Claude's paste
    // PLACEHOLDERS expanded back into the text they stand for — the ONE shared "copy a prompt" path
    // behind the Auto-Testing row menu's Copy (AgentManagerContent::_MakePromptMenu) and every session
    // copy menu's Copy Current Prompt (CopySessionField code 7), so the two can never disagree about
    // what a prompt holds.
    //
    // WHY: Claude Code shows a large paste in its input box as a one-line LABEL ("[Pasted text #N +M
    // lines]", or "head[...Truncated text #N +M lines...]tail"). A draft read off that box — and so any
    // prompt the overlay's MAIL button or the compose box queued from it — carries the label, not the
    // text, and a copied label means nothing outside that one input box (pasting it back into claude
    // does NOT re-bind the content; it submits the literal label). Where the content lives: current
    // Claude Code keeps an UNSENT paste only in its own memory (its <claude home>\paste-cache file is
    // written at SUBMIT time), so the first source is the session's PASTE JOURNAL — every paste
    // Agentmaster itself passed into that session (PENDING_INPUT.md §2c) — and the paste-cache (a
    // previously submitted identical paste) the fallback; ResolvePendingDraftCarry proves the match.
    //
    // HOW: text with NO placeholder (the common case) is copied synchronously, exactly as before.
    // Otherwise the text is newline-folded first (FoldCrToLf — the Manager's compose TextBox stores a
    // newline as '\r' while the resolver works on '\n' lines, so an unfolded placeholder that sits alone
    // on its line would read as mid-line and be refused), expanded OFF the UI thread (the resolver
    // reads the WHOLE paste cache — capped at 64 MiB — which a UI-thread click must never pay), and the
    // clipboard write then hops back through `dispatcher`, so the copy lands a moment after the click.
    //
    // SAFETY — never a wrong expansion, never a lost copy. A journal match is the strong tier (the
    // session's own recent pastes, assigned by line count in paste order); a cache match must be proven
    // (a unique line-count match for a WHOLE-LINE collapsed label; head/tail content anchors + exact line
    // arithmetic for a truncated one). A MID-LINE label nothing backs is kept as written — that is a
    // document QUOTING a placeholder, not a paste — and any other unproven label (or an [Image #N])
    // refuses the WHOLE expansion. Any refusal, a throw, or a missing dispatcher degrades to copying the
    // text AS RENDERED — never to nothing — and each case is traced as a [pending] line in hooks.log.
    // The one path that copies nothing is a window closed while its expansion was still resolving (no UI
    // thread is left to write from); it is traced too.
    // ⚠ The cache tier is only as strong as "exactly one cached paste has that many lines": a placeholder
    // merely QUOTED alone on its own line, whose count happens to match a cache file, would be expanded.
    // Acceptable for a copy (visible + recoverable); never reuse this for a SEND decision.
    //
    // ORDERING: every call (and every CopySessionField call) takes a new process-wide copy generation,
    // and an off-thread expansion writes the clipboard only if no newer copy started meanwhile — so
    // copying a paste-bearing prompt and then something else can never end with the OLDER text on the
    // clipboard. Traced as "superseded" when it happens.
    //
    //   dispatcher   — the calling UI thread's DispatcherQueue; the clipboard write + the copy chime run
    //                  there. Null ⇒ nothing to hop back to, so the text is copied as rendered, synchronously.
    //   text         — the prompt as stored/rendered (any line endings). Empty ⇒ no-op, no chime.
    //   sessionId    — the owning session; used for the [pending] trace only (never to look anything up).
    //   what         — a short noun for the trace, e.g. L"queued prompt" / L"current prompt".
    //   expandPastes — false ⇒ copy `text` verbatim, synchronously. Pass false for a prompt that is
    //                  ALREADY what claude received (an Auto-Testing Typed row, recorded from the
    //                  UserPromptSubmit hook after claude expanded its own placeholders): a placeholder
    //                  in such a text is literal text the user sent, and "expanding" it would change it.
    //   registry     — the engine registry, to snapshot `sessionId`'s PASTE JOURNAL (on the calling thread,
    //                  before the off-thread expansion). Null ⇒ no journal tier: the paste-cache alone, i.e.
    //                  the historical behavior. Every shipped caller passes it.
    // Returns nothing. Never throws: every failure is logged through AgentLogCaughtException (Rule #18)
    // and degrades to the as-rendered copy. Call from the UI thread.
    void CopyPromptTextExpandingPastes(const winrt::Windows::System::DispatcherQueue& dispatcher,
                                       const std::wstring& text,
                                       const std::wstring& sessionId,
                                       const std::wstring& what,
                                       bool expandPastes = true,
                                       const ::Agentmaster::SessionRegistry* registry = nullptr);

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
