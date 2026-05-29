// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — core data model for the Manager tab (Design A / C1 / Flight Plan).
// Plain C++ (no WinRT projection) so it compiles standalone; the XAML layer (M6) wraps
// these in observable view-models. See doc/agentmaster/IMPLEMENTATION.md.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Agentmaster
{
    // Hook-driven lifecycle of a Claude Code session. Authoritative state comes from
    // Claude Code hooks (see doc/agentmaster/HOOKS.md), never from screen-scraping.
    enum class SessionState
    {
        Idle, // launched, no active turn
        Running, // mid-turn (PreToolUse/PostToolUse activity)
        WaitingForInput, // Stop hook fired: turn complete, ready for next user message
        NeedsApproval, // Notification(permission): tool-permission prompt awaiting y/n
        Error, // last turn ended in error
        Done // session ended (SessionEnd)
    };

    enum class AutopilotMode
    {
        Off,
        SemiAuto, // confirm each auto-send (recommended for the first N sends)
        Full // auto-send with no confirmation
    };

    // When a queued prompt is allowed to fire.
    enum class PromptGate
    {
        OnTurnComplete, // default: fire on the Stop hook
        AfterDelay, // fire delayMs after the gate condition is met
        Manual // only via "Send now"
    };

    enum class PromptStatus
    {
        Pending,
        Sent,
        Held, // a guard blocked auto-send (e.g., the agent asked a question)
        Skipped,
        Failed
    };

    struct QueuedPrompt
    {
        std::wstring id;
        std::wstring label; // short display name
        std::wstring text; // the prompt body (may be multi-line)
        PromptStatus status{ PromptStatus::Pending };
        PromptGate gate{ PromptGate::OnTurnComplete };
        uint32_t delayMs{ 0 };
        // Optional guard: hold auto-send unless the session output satisfies this.
        // Empty => the scheduler applies the default "not-a-question" guard.
        std::wstring guardPattern;
        std::optional<std::wstring> dependsOn; // prompt id that must be Sent first
        uint32_t attempts{ 0 };
        uint32_t maxAttempts{ 1 };
        int64_t sentAtUnixMs{ 0 };
    };

    struct ApprovalPolicy
    {
        bool pauseForHuman{ true }; // default: do not auto-approve tool permissions
        std::vector<std::wstring> autoApproveTools; // allowlist of safe tools
    };

    struct AutopilotState
    {
        AutopilotMode mode{ AutopilotMode::Off };
        uint32_t throttleMs{ 500 }; // min delay between auto-sends
        bool stopOnError{ true }; // pause the plan if a turn ends in error
        bool pauseOnHumanInput{ true }; // suspend while the human is typing
        uint32_t maxAutoSends{ 100 }; // runaway backstop
        uint32_t autoSendsThisRun{ 0 };
        ApprovalPolicy approval{};
    };

    // One Claude Code session = one claude.exe on a ConPTY connection, tracked by the
    // SessionRegistry. `id` is also injected into the child as CCMGR_SESSION_ID so hooks
    // can be correlated back to this record (see HOOKS.md).
    struct SessionInfo
    {
        std::wstring id;
        std::wstring title; // task / display name
        std::wstring workingDir; // the "M" axis: which working directory
        std::wstring branch; // git branch / worktree
        SessionState state{ SessionState::Idle };
        int64_t lastActivityUnixMs{ 0 };
        // Transient runtime flag (not persisted): set from the most recent Stop hook's
        // best-effort `lastMessageIsQuestion`. Feeds the Autopilot question-guard (M7):
        // a turn that ended on a clarifying question must NOT be auto-answered.
        bool lastMessageWasQuestion{ false };
        // Transient (not persisted): in SemiAuto, the scheduler arms the next prompt here
        // and the Flight Plan shows a one-click confirm. Empty when nothing awaits confirm.
        std::wstring pendingConfirmPromptId;
        std::vector<QueuedPrompt> queue; // the Flight Plan
        AutopilotState autopilot{};
    };
}
