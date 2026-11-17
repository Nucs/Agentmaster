// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionTitler: the OS half of AI SESSION TITLES (doc/agentmaster/AI_TITLES.md; the pure half, with the whole
// flow, is AiSessionTitle.h). One per process, like the registry it reads: the engine starts it once (Engine.cpp), and the
// hooks bridge's sink hands it every hook message right after the registry applied it. A marked session's first real prompt
// becomes ONE job on a worker thread of the titler's own — never the bridge's pipe thread, which must keep reading hooks — that
// runs a hidden `claude -p` (ClaudeStatus.h RunClaudeCapture) and writes the answer as the session's title through the registry,
// so every window, the Manager and the Telegram topic follow (Rule #11).

#pragma once

#include <memory>

namespace Agentmaster
{
    class SessionRegistry;
    struct HookMessage;

    /// <summary>
    /// Starts the AI titles for this process: the titler keeps the registry WEAKLY (a job that outlives the engine's teardown
    /// finds nothing to rename and ends). Called once by the engine's init, before the hooks bridge starts; a second call only
    /// replaces the registry it reads (harmless, and what a test harness that builds a second engine would want).
    /// </summary>
    /// <param name="registry">The process's session registry (the one the bridge's sink applies hooks to).</param>
    void StartAiSessionTitles(const std::shared_ptr<SessionRegistry>& registry) noexcept;

    /// <summary>
    /// One hook message, AFTER the registry applied it (the bridge's sink calls the registry first, so the record a prompt
    /// belongs to is as the hook left it). Only a UserPromptSubmit with a prompt can matter: the session's AI-title mark
    /// (SessionInfo::aiTitlePendingFrom) is judged against it (AiSessionTitle.h DecideAiTitleOnPrompt) — a slash command or a shell
    /// escape waits, a renamed session drops its mark, and the first real prompt takes the mark in ONE registry update (so two
    /// prompts racing can never both name it) and queues the naming job. Cheap for every other message: one event compare, and
    /// for a prompt one lean registry read. Runs on the bridge's pipe thread and never blocks it on the model call.
    /// </summary>
    /// <param name="m">The hook message.</param>
    void NoteHookForAiSessionTitle(const HookMessage& m) noexcept;
}
