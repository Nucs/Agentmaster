// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — RemoteLaunch: the one request shape a REMOTE surface (the Telegram bot, MESSAGING.md
// §5.4 / §7.3) uses to open a session into a tab — resume a closed one, fork one, start a new one in
// a folder. The engine routes it to ONE window (Engine.h RequestLaunchInWindow — the most recently
// active window), whose sink runs the same seams the Sessions page's buttons run. A tiny header of its
// own so Engine.h and Messaging.h share it without either including the other.

#pragma once

#include <string>

namespace Agentmaster
{
    struct RemoteLaunchRequest
    {
        enum class Kind
        {
            NewSession, // a fresh session in `dir` (TerminalPage::_SpawnClaudeSession)
            Resume, // `sessionId` resumed into a tab (TerminalPage::_ResumeSessionFromDisk)
            Fork, // a fork of `sessionId` (TerminalPage::_ForkSessionFromDisk)
        };
        Kind kind{ Kind::NewSession };
        std::wstring sessionId; // Resume / Fork: the session on disk
        std::wstring dir; // the working directory (NewSession; Resume / Fork: the session's)
        std::wstring title; // the tab title to pin ("" = derived)
        std::wstring model; // a per-launch `--model <id>` ("" = the settings model)
        bool pin{ false }; // MESSAGING.md §5.3: pin the opened session (never idle) once it is live
    };
}
